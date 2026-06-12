/**
 * @file vfs.h
 * @brief Scaled single-file virtual filesystem (VFS) with indirect addressing.
 *
 * All logical files are stored inside one host file on disk. The
 * caller interacts with logical files through a file-descriptor-like
 * handle (vfs_fd_t) and the familiar open/read/write/close/seek/stat
 * interface.
 *
 * ## On-disk layout (Format Version 2)
 *
 * @code
 * ┌──────────────────────┐  offset 0
 * │   SuperBlock (64 KiB)│  magic, version, capacity, free-list metrics, ...
 * ├──────────────────────┤  offset VFS_BITMAP_OFFSET
 * │   Bitmap (2 MiB)     │  free-block allocation bitmap (1 = free)
 * ├──────────────────────┤  offset VFS_INODE_TABLE_OFFSET
 * │   Inode table        │  VFS_MAX_INODES × sizeof(vfs_inode_t)
 * ├──────────────────────┤  offset VFS_DATA_OFFSET
 * │   Data region        │  fixed-size blocks of VFS_BLOCK_SIZE bytes
 * │   ...                │
 * └──────────────────────┘
 * @endcode
 *
 * Each inode stores the file path (up to VFS_MAX_PATH bytes), size,
 * timestamps, and a block array containing direct, single-indirect,
 * and double-indirect physical block pointers.
 *
 * ## Thread safety
 * A single global mutex serialises all metadata operations. Concurrent
 * reads of *different* files are therefore serialised too. Applications
 * that need true parallel I/O should stripe across multiple VFS images.
 *
 * ## Limitations
 * - Maximum file size: VFS_MAX_BLOCKS_PER_FILE × VFS_BLOCK_SIZE bytes (~4.003 GiB).
 * - Maximum files:     VFS_MAX_INODES (1024).
 * - Concurrent handles: VFS_MAX_OPEN_FILES (1024).
 */

#ifndef VFS_H
#define VFS_H

#include <assert.h>    /* static_assert */
#include <errno.h>
#include <inttypes.h>  /* PRIu64, etc.                  */
#include <stdbool.h>   /* bool                          */
#include <stddef.h>    /* size_t                        */
#include <stdint.h>    /* uint8_t, uint32_t, uint64_t   */
#include <stdio.h>     /* FILE*                         */
#include <stdlib.h>
#include <sys/mman.h>  /* memfd_create */
#include <sys/types.h> /* off_t                         */
#include <time.h>      /* time_t                        */
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Compile-time tunables
 * ---------------------------------------------------------------------- */

/** Block size in bytes. Must be a power of two. */
#define VFS_BLOCK_SIZE 4096u

/** Maximum number of inodes (== maximum number of files). */
#define VFS_MAX_INODES 1024u

/** Array size in the inode for structural backward compatibility. */
#define VFS_INODE_BLOCKS_ARRAY_SIZE 256u

/** 
 * Maximum logical blocks per file.
 * 254 Direct + 1024 Single-Indirect + (1024 * 1024) Double-Indirect.
 */
#define VFS_MAX_BLOCKS_PER_FILE 1049854u

/** Maximum file-path length including the NUL terminator. */
#define VFS_MAX_PATH 256u

/** Size reserved for the superblock region on disk. */
#define VFS_SUPERBLOCK_SIZE 65536u

/** Maximum number of simultaneously open file descriptors. */
#define VFS_MAX_OPEN_FILES 1024u

/** Magic number that identifies a valid VFS format version 2 image. */
#define VFS_MAGIC UINT32_C(0x56465302)

/** Current on-disk format version. */
#define VFS_VERSION UINT32_C(2)

/* -------------------------------------------------------------------------
 * Decoupled block capacity geometry (supports up to 64 GiB image sizing)
 * ---------------------------------------------------------------------- */

/** Total addressable data blocks in the payload area. */
#define VFS_TOTAL_BLOCKS 16777216u

/** Number of 32-bit words needed to store the free-block allocation bitmap. */
#define VFS_BITMAP_WORDS (VFS_TOTAL_BLOCKS / 32u)

/** Physical on-disk size of the block allocation bitmap region. */
#define VFS_BITMAP_SIZE ((off_t)(VFS_BITMAP_WORDS * 4u)) /* Exactly 2 MiB */

/** Byte offset of the block allocation bitmap region inside the image. */
#define VFS_BITMAP_OFFSET ((off_t)VFS_SUPERBLOCK_SIZE)

/** Byte offset of the inode table inside the image. */
#define VFS_INODE_TABLE_OFFSET (VFS_BITMAP_OFFSET + VFS_BITMAP_SIZE)

/** Byte offset of the first data block inside the image. */
#define VFS_DATA_OFFSET (VFS_INODE_TABLE_OFFSET + ((off_t)VFS_MAX_INODES * 1312u))

#if defined(__cplusplus)
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Error codes
 * ---------------------------------------------------------------------- */

/** Return codes used throughout the API. */
typedef enum {
    VFS_OK = 0,             /**< Success.                               */
    VFS_ERR_IO = -1,        /**< Underlying host I/O error.             */
    VFS_ERR_CORRUPT = -2,   /**< Image magic / checksum mismatch.       */
    VFS_ERR_NOTFOUND = -3,  /**< No such file.                          */
    VFS_ERR_EXISTS = -4,    /**< File already exists.                   */
    VFS_ERR_NOSPACE = -5,   /**< No free inodes or data blocks.         */
    VFS_ERR_NOMEM = -6,     /**< Host malloc/calloc failure.            */
    VFS_ERR_BADFD = -7,     /**< Invalid or closed file descriptor.     */
    VFS_ERR_OVERFLOW = -8,  /**< Would exceed per-file block limit.     */
    VFS_ERR_INVAL = -9,     /**< Invalid argument (NULL, bad whence…).  */
    VFS_ERR_ISDIR = -10,    /**< Path refers to a directory (reserved). */
    VFS_ERR_READONLY = -11, /**< Write attempted on read-only mount.    */
} vfs_status_t;

/* -------------------------------------------------------------------------
 * Open flags (OR-able bit flags, like POSIX O_*)
 * ---------------------------------------------------------------------- */

#define VFS_O_RDONLY 0x00u /**< Open for reading only.                */
#define VFS_O_WRONLY 0x01u /**< Open for writing only.                */
#define VFS_O_RDWR   0x02u /**< Open for reading and writing.         */
#define VFS_O_CREAT  0x04u /**< Create file if it does not exist.     */
#define VFS_O_TRUNC  0x08u /**< Truncate to zero length on open.      */
#define VFS_O_APPEND 0x10u /**< Writes always go to end of file.      */
#define VFS_O_EXCL   0x20u /**< With O_CREAT: fail if file exists.    */

/* -------------------------------------------------------------------------
 * Seek origins (mirrors POSIX SEEK_*)
 * ---------------------------------------------------------------------- */

#define VFS_SEEK_SET 0 /**< From beginning of file.  */
#define VFS_SEEK_CUR 1 /**< From current position.   */
#define VFS_SEEK_END 2 /**< From end of file.         */

/* -------------------------------------------------------------------------
 * On-disk structures
 *
 * Every on-disk structure must be a fixed size and is written / read as a
 * raw byte sequence. Fields use explicit-width types; no padding surprises.
 * ---------------------------------------------------------------------- */

/**
 * On-disk inode. Stores metadata and block pointers.
 * A zero `path[0]` byte means the slot is free.
 */
typedef struct __attribute__((packed)) {
    char path[VFS_MAX_PATH];                      /**< Absolute virtual path, NUL-terminated. */
    uint64_t size;                                /**< Logical file size in bytes.             */
    uint64_t created_at;                          /**< Creation timestamp (Unix seconds).      */
    uint64_t modified_at;                         /**< Last-write timestamp (Unix seconds).    */
    uint32_t block_count;                         /**< Number of allocated physical blocks.    */
    uint32_t blocks[VFS_INODE_BLOCKS_ARRAY_SIZE]; /**< Physical block addresses (direct/ind).  */
    uint8_t _pad[4];                              /**< Explicit padding for alignment.         */
} vfs_inode_t;

/**
 * On-disk superblock. Always lives at offset 0 of the image file.
 * Fixed at VFS_SUPERBLOCK_SIZE bytes; the tail is unused/zeroed.
 */
typedef struct {
    uint32_t magic;            /**< Must equal VFS_MAGIC.            */
    uint32_t version;          /**< Must equal VFS_VERSION.          */
    uint32_t block_size;       /**< Must equal VFS_BLOCK_SIZE.       */
    uint32_t max_inodes;       /**< Must equal VFS_MAX_INODES.       */
    uint32_t total_blocks;     /**< Must equal VFS_TOTAL_BLOCKS.     */
    uint32_t free_block_count; /**< Informational; not authoritative.*/
    uint32_t free_inode_count; /**< Informational; not authoritative.*/
    uint32_t bitmap_words;     /**< Number of words in the bitmap.   */
    uint8_t _reserved[32];     /**< Future use/padding.              */
} vfs_super_t;

/* -------------------------------------------------------------------------
 * Runtime handles (opaque to callers)
 * ---------------------------------------------------------------------- */

/** Opaque handle for a mounted VFS image. Obtain via vfs_open(). */
typedef struct vfs_t vfs_t;

/**
 * File descriptor returned by vfs_fopen().
 * Negative values indicate an error (cast to vfs_status_t).
 */
typedef int vfs_fd_t;

/* -------------------------------------------------------------------------
 * File stat
 * ---------------------------------------------------------------------- */

/** File metadata returned by vfs_stat(). */
typedef struct {
    char path[VFS_MAX_PATH]; /**< Virtual path of the file.            */
    uint64_t size;           /**< Logical file size in bytes.           */
    uint32_t block_count;    /**< Number of data blocks allocated.      */
    time_t created_at;       /**< Creation time.                        */
    time_t modified_at;      /**< Last modification time.               */
} vfs_stat_t;

/* -------------------------------------------------------------------------
 * Filesystem lifecycle
 * ---------------------------------------------------------------------- */

/**
 * Creates a new VFS image on disk at @p image_path and mounts it.
 *
 * If a file already exists at @p image_path it is overwritten.
 *
 * @param image_path  Host filesystem path for the image file.
 * @param out_vfs     On success, set to the mounted VFS handle. Never NULL.
 * @return VFS_OK on success, or a negative vfs_status_t on failure.
 * @note Caller must eventually call vfs_close() on the returned handle.
 */
vfs_status_t vfs_create(const char* image_path, vfs_t** out_vfs);

/**
 * Opens (mounts) an existing VFS image.
 *
 * @param image_path  Host filesystem path of the image to open.
 * @param readonly    If true the image is opened read-only.
 * @param out_vfs     On success, set to the mounted VFS handle. Never NULL.
 * @return VFS_OK, VFS_ERR_CORRUPT if the magic/version is wrong, or another
 *         negative vfs_status_t on I/O failure.
 * @note Caller must eventually call vfs_close() on the returned handle.
 */
vfs_status_t vfs_open(const char* image_path, bool readonly, vfs_t** out_vfs);

/**
 * Flushes all pending writes and closes the VFS handle.
 *
 * @p vfs is freed and must not be used after this call. NULL is a no-op.
 *
 * @param vfs  Handle returned by vfs_create() or vfs_open().
 */
void vfs_close(vfs_t* vfs);

/**
 * Flushes in-memory superblock, bitmap, and inode changes to the host file without
 * closing the VFS.
 *
 * @param vfs  Mounted VFS handle.
 * @return VFS_OK or a negative vfs_status_t.
 */
vfs_status_t vfs_sync(vfs_t* vfs);

/* -------------------------------------------------------------------------
 * File operations
 * ---------------------------------------------------------------------- */

/**
 * Opens a logical file inside the VFS, optionally creating it.
 *
 * @param vfs    Mounted VFS handle.
 * @param path   Absolute virtual path, e.g. "/data/image.png".
 * @param flags  Bitwise OR of VFS_O_* flags.
 * @return A non-negative vfs_fd_t on success, or a negative vfs_status_t.
 */
vfs_fd_t vfs_fopen(vfs_t* vfs, const char* path, unsigned int flags);

/**
 * Closes an open file descriptor, flushing any pending metadata.
 *
 * @param vfs  Mounted VFS handle.
 * @param fd   Descriptor returned by vfs_fopen().
 * @return VFS_OK or VFS_ERR_BADFD.
 */
vfs_status_t vfs_fclose(vfs_t* vfs, vfs_fd_t fd);

/**
 * Reads up to @p count bytes from @p fd into @p buf.
 *
 * Advances the file position by the number of bytes read.
 *
 * @param vfs    Mounted VFS handle.
 * @param fd     Open file descriptor.
 * @param buf    Destination buffer. Must be at least @p count bytes.
 * @param count  Maximum bytes to read.
 * @param[out] bytes_read  Bytes actually read (0 at EOF). Never NULL.
 * @return VFS_OK or a negative vfs_status_t.
 */
vfs_status_t vfs_fread(vfs_t* vfs, vfs_fd_t fd, void* buf, size_t count, size_t* bytes_read);

/**
 * Writes @p count bytes from @p buf into @p fd.
 *
 * Allocates direct, single-indirect, or double-indirect blocks as needed.
 * In append mode the cursor is advanced to EOF before each write.
 *
 * @param vfs    Mounted VFS handle.
 * @param fd     Open file descriptor (must have write permission).
 * @param buf    Source buffer.
 * @param count  Number of bytes to write.
 * @param[out] bytes_written  Bytes actually written. Never NULL.
 * @return VFS_OK or a negative vfs_status_t (VFS_ERR_NOSPACE, VFS_ERR_OVERFLOW…).
 */
vfs_status_t vfs_fwrite(vfs_t* vfs, vfs_fd_t fd, const void* buf, size_t count, size_t* bytes_written);

/**
 * Repositions the read/write cursor for @p fd.
 *
 * @param vfs     Mounted VFS handle.
 * @param fd      Open file descriptor.
 * @param offset  Byte offset relative to @p whence.
 * @param whence  VFS_SEEK_SET, VFS_SEEK_CUR, or VFS_SEEK_END.
 * @param[out] new_offset  Resulting absolute offset. May be NULL.
 * @return VFS_OK or a negative vfs_status_t.
 */
vfs_status_t vfs_fseek(vfs_t* vfs, vfs_fd_t fd, off_t offset, int whence, off_t* new_offset);

/**
 * Returns the current cursor position of @p fd.
 *
 * @param vfs  Mounted VFS handle.
 * @param fd   Open file descriptor.
 * @param[out] pos  Current byte offset from the start of the file. Never NULL.
 * @return VFS_OK or a negative vfs_status_t.
 */
vfs_status_t vfs_ftell(vfs_t* vfs, vfs_fd_t fd, off_t* pos);

/**
 * Truncates @p path to exactly @p length bytes.
 *
 * Extends with zero-mapped blocks if @p length > current size. Blocks beyond the
 * new end are recursively freed.
 *
 * @param vfs     Mounted VFS handle.
 * @param path    Virtual path of the file to truncate.
 * @param length  New file size in bytes.
 * @return VFS_OK or a negative vfs_status_t.
 */
vfs_status_t vfs_truncate(vfs_t* vfs, const char* path, uint64_t length);

/**
 * Retrieves metadata for @p path.
 *
 * @param vfs   Mounted VFS handle.
 * @param path  Virtual path of the file.
 * @param[out] st  Populated on success. Never NULL.
 * @return VFS_OK or VFS_ERR_NOTFOUND.
 */
vfs_status_t vfs_stat(vfs_t* vfs, const char* path, vfs_stat_t* st);

/**
 * Deletes a file from the VFS, freeing its metadata structures and data blocks.
 *
 * @param vfs   Mounted VFS handle.
 * @param path  Virtual path of the file to remove.
 * @return VFS_OK or VFS_ERR_NOTFOUND.
 */
vfs_status_t vfs_unlink(vfs_t* vfs, const char* path);

/**
 * Tests whether a file exists in the VFS.
 *
 * @param vfs   Mounted VFS handle.
 * @param path  Virtual path to test.
 * @return true if the file exists, false otherwise.
 */
bool vfs_exists(vfs_t* vfs, const char* path);

/* -------------------------------------------------------------------------
 * Directory-like listing
 * ---------------------------------------------------------------------- */

/**
 * Callback invoked once per file by vfs_list().
 *
 * @param path      Virtual path of the file.
 * @param st        Metadata for the file.
 * @param userdata  Pointer passed through from vfs_list().
 * @return Return true to continue iteration, false to stop early.
 */
typedef bool (*vfs_list_cb_t)(const char* path, const vfs_stat_t* st, void* userdata);

/**
 * Iterates over every file whose virtual path starts with @p prefix.
 *
 * Pass "/" or "" to list all files.
 *
 * @param vfs       Mounted VFS handle.
 * @param prefix    Path prefix to filter by.
 * @param callback  Called once per matching file.
 * @param userdata  Forwarded opaquely to @p callback.
 */
void vfs_list(vfs_t* vfs, const char* prefix, vfs_list_cb_t callback, void* userdata);

/* -------------------------------------------------------------------------
 * Utility
 * ---------------------------------------------------------------------- */

/**
 * Returns a human-readable string for @p status.
 *
 * @param status  A vfs_status_t value.
 * @return Pointer to a static string. Never NULL.
 */
const char* vfs_strerror(vfs_status_t status);

/**
 * Writes a diagnostic dump of the superblock and inode table to @p out.
 *
 * @param vfs  Mounted VFS handle.
 * @param out  Destination FILE* (e.g. stdout).
 */
void vfs_dump(const vfs_t* vfs, FILE* out);

/**
 * Renames (moves) the file at @p oldpath to @p newpath.
 *
 * If @p newpath already names an existing file it is replaced,
 * consistent with POSIX rename(2) semantics. No data blocks are moved;
 * only the inode's path field is updated.
 *
 * @param vfs      Mounted VFS handle.
 * @param oldpath  Current virtual path of the file.
 * @param newpath  Desired virtual path.
 * @return VFS_OK on success, or:
 *   - VFS_ERR_INVAL    if either path is NULL/empty or newpath is too long.
 *   - VFS_ERR_NOTFOUND if oldpath does not exist.
 *   - VFS_ERR_READONLY if the filesystem is mounted read-only.
 *   - VFS_ERR_IO       on disk write failure.
 */
vfs_status_t vfs_rename(vfs_t* vfs, const char* oldpath, const char* newpath);

/**
 * @brief Writes a contiguous memory buffer to a virtual path, creating or truncating the file.
 *
 * This utility encapsulates the open, write, truncate, and close lifecycle. If the target file
 * does not exist, it is created. If it already exists, its length is truncated to zero
 * before the content is written.
 *
 * @param vfs     Mounted VFS handle.
 * @param path    Absolute virtual path of the destination file.
 * @param content Source memory buffer containing payload bytes.
 * @param len     Total number of bytes to write from @p content.
 * @return VFS_OK on success, or a negative vfs_status_t error code.
 *         Specifically returns VFS_ERR_IO if the total bytes written does not match @p len.
 */
static inline vfs_status_t write_vfs_file(vfs_t* vfs, const char* path, const void* content, size_t len) {
    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    if (fd < 0) { return (vfs_status_t)fd; }

    size_t written = 0;
    vfs_status_t status = vfs_fwrite(vfs, fd, content, len, &written);
    vfs_fclose(vfs, fd);

    if (status != VFS_OK) { return status; }
    return (written == len) ? VFS_OK : VFS_ERR_IO;
}

/**
 * @brief Appends a contiguous memory buffer to a virtual path, creating the file if missing.
 *
 * This utility encapsulates the open, append, and close lifecycle. If the target file
 * does not exist, it is created and initialized to zero length prior to appending. If it exists,
 * the write cursor is redirected to EOF before the payload is appended.
 *
 * @param vfs  Mounted VFS handle.
 * @param path Absolute virtual path of the destination file.
 * @param data Source memory buffer containing append payload bytes.
 * @param len  Total number of bytes to write from @p data.
 * @return VFS_OK on success, or a negative vfs_status_t error code.
 *         Specifically returns VFS_ERR_IO if the total bytes written does not match @p len.
 */
static inline vfs_status_t append_vfs_file(vfs_t* vfs, const char* path, const void* data, size_t len) {
    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_APPEND);
    if (fd < 0) { return (vfs_status_t)fd; }

    size_t written = 0;
    vfs_status_t status = vfs_fwrite(vfs, fd, data, len, &written);
    vfs_fclose(vfs, fd);

    if (status != VFS_OK) { return status; }
    return (written == len) ? VFS_OK : VFS_ERR_IO;
}

/**
 * @brief Read the entire content of file at path.
 *
 * @param vfs     Mounted VFS handle.
 * @param path    Absolute virtual path of the file o read.
 * @param out_size Total number of bytes read or file size are written here.
 * @return VFS_OK on success, or a negative vfs_status_t error code.
 */
static inline void* vfs_read_file(vfs_t* fs, const char* path, size_t* out_size) {
    vfs_fd_t fd = vfs_fopen(fs, path, VFS_O_RDONLY);
    if (fd < 0) return NULL;

    vfs_stat_t st;
    if (vfs_stat(fs, path, &st) != VFS_OK) return NULL;
    if (st.size == 0) {
        *out_size = 0;
        return malloc(1);
    }

    void* data = malloc(st.size);
    if (!data) return NULL;

    size_t bytes_read = 0;
    if (vfs_fread(fs, fd, data, st.size, &bytes_read) != VFS_OK) {
        free(data);
        return NULL;
    }
    *out_size = bytes_read;
    return data;
}

// =================== LOADING IMAGES FROM MEMORY =====================
/**
 * @brief Mounts a read-only or read-write VFS directly from a static memory array.
 * 
 * This helper instantiates an anonymous, RAM-backed file, writes the embedded
 * filesystem payload into it, and mounts it using the standard VFS engine.
 *
 * @param embed_data Pointer to the embedded static byte array (e.g., asset_vfs_bytes).
 * @param embed_size Total size of the embedded byte array.
 * @param readonly   Mount the filesystem as read-only.
 * @param out_vfs    Pointer populated with the resulting initialized vfs_t handle.
 * @return VFS_OK, VFS_ERR_IO, or VFS_ERR_INVAL.
 */
static inline vfs_status_t vfs_open_embedded(const void* embed_data, size_t embed_size, bool readonly,
                                             vfs_t** out_vfs) {
    if (embed_data == NULL || embed_size == 0 || out_vfs == NULL) { return VFS_ERR_INVAL; }

    /* 1. Create a purely RAM-backed, anonymous host file descriptor */
    int mem_fd = memfd_create("vfs_embedded_image", MFD_CLOEXEC);
    if (mem_fd < 0) { return VFS_ERR_IO; }

    /* 2. Write the static binary array payload into the anonymous file */
    const uint8_t* src = (const uint8_t*)embed_data;
    size_t remaining = embed_size;
    while (remaining > 0) {
        ssize_t written = write(mem_fd, src, remaining);
        if (written < 0) {
            if (errno == EINTR) { continue; }
            close(mem_fd);
            return VFS_ERR_IO;
        }
        src += written;
        remaining -= (size_t)written;
    }

    /* 3. Reference the open descriptor using procfs namespace */
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", mem_fd);

    /* 4. Mount through existing path-based API */
    vfs_status_t status = vfs_open(proc_path, readonly, out_vfs);

    /* 5. Close local descriptor. The VFS internally duplicates or holds 
          open the file reference, keeping the RAM file alive until vfs_close. */
    close(mem_fd);

    return status;
}

static_assert(sizeof(vfs_inode_t) == 1312, "vfs_inode_t size must be exactly 1312 bytes");
static_assert(offsetof(vfs_inode_t, size) == 256, "layout changed");
static_assert(offsetof(vfs_inode_t, block_count) == 280, "layout changed");
static_assert(sizeof(vfs_super_t) <= VFS_SUPERBLOCK_SIZE, "vfs_super_t exceeds VFS_SUPERBLOCK_SIZE");

#if defined(__cplusplus)
}
#endif

#endif /* VFS_H */
