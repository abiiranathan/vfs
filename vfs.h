/**
 * @file vfs.h
 * @brief High-Throughput Extent-Based Virtual Filesystem (VFS Format v3)
 *
 *  ██╗   ██╗███████╗███████╗    ██╗   ██╗██████╗
 *  ██║   ██║██╔════╝██╔════╝    ██║   ██║╚════██╗
 *  ██║   ██║█████╗  ███████╗    ██║   ██║ █████╔╝
 *  ╚██╗ ██╔╝██╔══╝  ╚════██║    ╚██╗ ██╔╝ ╚═══██╗
 *   ╚████╔╝ ██║     ███████║     ╚████╔╝ ██████╔╝
 *    ╚═══╝  ╚═╝     ╚══════╝      ╚═══╝  ╚═════╝
 *  ========================================================================
 *   HIGH-PERFORMANCE MULTITHREADED SINGLE-FILE VIRTUAL FILESYSTEM ENGINE
 *  ========================================================================
 *
 *  ========================================================================
 *  1. SYSTEM GEOMETRY & ARCHITECTURAL SPECIFICATIONS
 *  ========================================================================
 *
 *  +-----------------------------+----------------------------------------+
 *  | PARAMETER                   | VALUE / SPECIFICATION                  |
 *  +-----------------------------+----------------------------------------+
 *  | On-Disk Format Version      | Version 3 (Extent-based indexing)      |
 *  | Magic Signature             | 0x56465303 ("VFS\x03")                 |
 *  | Block Size (Payload Page)   | 4,096 Bytes (4 KiB)                    |
 *  | Total Addressable Blocks    | 16,777,216 Data Blocks                 |
 *  | Total Storage Capacity      | 64.000 GiB (68,719,476,736 Bytes)      |
 *  | Maximum Inodes (Files)      | 65,536 Distinct Files                  |
 *  | Max Concurrent File Handles | 1,024 Active Open File Descriptors     |
 *  | Inline Extents per Inode    | 32 Extents (Direct In-Inode Storage)   |
 *  | Overflow Extents per Block  | 256 Extents / 4 KiB Block (Chained)    |
 *  | Max Contiguous Extent Span  | 4,294,967,295 Blocks (~16 TiB/extent)  |
 *  | Max Individual File Size    | Up to 64 GiB (Full Device Span)        |
 *  | Path Length Limit           | 256 Bytes (NUL-Terminated UTF-8)       |
 *  | In-Memory Inode Table RAM   | ~51.5 MiB (65,536 x 824 Bytes)         |
 *  +-----------------------------+----------------------------------------+
 *
 *  ========================================================================
 *  2. ON-DISK STORAGE LAYOUT (FORMAT VERSION 3)
 *  ========================================================================
 *
 *    Offset (Hex)     Offset (Decimal)    Region Description
 *  ┌────────────────┬───────────────────┬─────────────────────────────────┐
 *  │ 0x00000000     │ 0 Bytes           │ SUPERBLOCK REGION (64 KiB)      │
 *  │                │                   │ Magic, Version, Geometry, Stats │
 *  ├────────────────┼───────────────────┼─────────────────────────────────┤
 *  │ 0x00010000     │ 65,536 Bytes      │ FREE BLOCK BITMAP (2 MiB)       │
 *  │                │                   │ 524,288 x 32-bit Words (1=Free) │
 *  ├────────────────┼───────────────────┼─────────────────────────────────┤
 *  │ 0x00210000     │ 2,162,688 Bytes   │ INODE TABLE (51.5 MiB)          │
 *  │                │                   │ 65,536 Inodes x 824 Bytes       │
 *  ├────────────────┼───────────────────┼─────────────────────────────────┤
 *  │ 0x03590000     │ 56,164,352 Bytes  │ DATA PAYLOAD AREA (Up to 64 GiB)│
 *  │                │                   │ Block 0: Permanent Sentinel     │
 *  │                │                   │ Block 1..16,777,215: User Data  │
 *  └────────────────┴───────────────────┴─────────────────────────────────┘
 *
 *  ========================================================================
 *  3. INODE RECORD STRUCTURE (824 BYTES PACKED)
 *  ========================================================================
 *
 *  +-------------------------+---------+----------------------------------+
 *  | FIELD                   | SIZE    | DESCRIPTION                      |
 *  +-------------------------+---------+----------------------------------+
 *  | char path[256]          | 256 B   | Absolute file path (0x00=Free)   |
 *  | uint64_t size           |   8 B   | Logical file size in bytes       |
 *  | uint64_t created_at     |   8 B   | Inode creation Unix timestamp    |
 *  | uint64_t modified_at    |   8 B   | Last write/truncate timestamp    |
 *  | uint32_t block_count    |   4 B   | Physical blocks allocated        |
 *  | uint32_t extent_count   |   4 B   | Total extents (inline+overflow)  |
 *  | uint32_t inline_extents |   4 B   | Extents in local inode array     |
 *  | uint32_t overflow_block |   4 B   | Physical blk of 1st overflow tab |
 *  | vfs_extent_t extents[32]| 512 B   | 32 x 16-byte Extent Descriptors  |
 *  | uint8_t _pad[16]        |  16 B   | Future proofing & alignment      |
 *  +-------------------------+---------+----------------------------------+
 *  | TOTAL PACKED SIZE       | 824 B   | sizeof(vfs_inode_t) == 824       |
 *  +-------------------------+---------+----------------------------------+
 *
 *  ========================================================================
 *  4. CONCURRENCY & DATA-PLANE ARCHITECTURE
 *  ========================================================================
 *
 *              THREAD 1 (/fileA)              THREAD 2 (/fileB)
 *                     │                              │
 *                     ▼                              ▼
 *          [ InodeLock[A] (RD) ]          [ InodeLock[B] (RD) ]
 *                     │                              │
 *          Snapshot Extent Array          Snapshot Extent Array
 *                     │                              │
 *          [ Unlock InodeLock[A] ]        [ Unlock InodeLock[B] ]
 *                     │                              │
 *                     ▼                              ▼
 *          ┌──────────────────────────────────────────────────┐
 *          │      PARALLEL UNLOCKED DIRECT HOST I/O           │
 *          │  pread(fd) / pwrite(fd) / sendfile(out_fd, fd)   │
 *          └──────────────────────────────────────────────────┘
 *
 *  - Fine-Grained Locking : Individual rwlock per inode eliminates data bottlenecks.
 *  - Lockless Host I/O    : Extents resolved in microseconds; I/O occurs unlocked.
 *  - Extent Coalescing    : Automatic O(log N) merging of contiguous physical runs.
 *  - Zero-Copy sendfile   : Direct kernel transfer between image and client sockets.
 *  - Smart Zeroing        : Overwrites skip zero-fill entirely; only boundary holes zeroed.
 */
#ifndef VFS_H
#define VFS_H

#include <assert.h> /* static_assert                */
#include <errno.h>
#include <inttypes.h>  /* PRIu64, etc.                  */
#include <pthread.h>   /* pthread_rwlock_t, pthread_mutex_t */
#include <stdbool.h>   /* bool                          */
#include <stddef.h>    /* size_t                        */
#include <stdint.h>    /* uint8_t, uint32_t, uint64_t   */
#include <stdio.h>     /* FILE*                         */
#include <sys/types.h> /* off_t                         */
#include <time.h>      /* time_t                        */

#if defined(__cplusplus)
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Compile-time tunables
 * ---------------------------------------------------------------------- */

/** Block size in bytes. Must be a power of two. */
#define VFS_BLOCK_SIZE 4096u

/** Hard architectural ceiling for inodes (bounds RAM usage to ~54 MiB). */
#define VFS_ABSOLUTE_MAX_INODES 65536u

/** Minimum allowable inodes for a valid filesystem image. */
#define VFS_ABSOLUTE_MIN_INODES 1u

/**
 * Maximum number of inodes (== maximum number of files).
 * May be overridden at compile time via: -DVFS_MAX_INODES=<count>
 */
#ifndef VFS_MAX_INODES
    #define VFS_MAX_INODES 65536u
#endif

/* Compile-time validation of user-supplied overrides */
#if (VFS_MAX_INODES < VFS_ABSOLUTE_MIN_INODES)
    #error "VFS_MAX_INODES must be at least 1."
#endif

#if (VFS_MAX_INODES > VFS_ABSOLUTE_MAX_INODES)
    #error "VFS_MAX_INODES exceeds the maximum supported limit (65536)."
#endif

/** Maximum file-path length including the NUL terminator. */
#define VFS_MAX_PATH 256u

/** Size reserved for the superblock region on disk. */
#define VFS_SUPERBLOCK_SIZE 65536u

/** Maximum number of simultaneously open file descriptors. */
#define VFS_MAX_OPEN_FILES 1024u

/** Magic number that identifies a valid VFS format version 3 image. */
#define VFS_MAGIC UINT32_C(0x56465303)

/** Current on-disk format version. */
#define VFS_VERSION UINT32_C(3)

/** Total addressable data blocks in the payload area. */
#define VFS_TOTAL_BLOCKS 16777216u

/** Number of 32-bit words needed to store the free-block allocation bitmap. */
#define VFS_BITMAP_WORDS (VFS_TOTAL_BLOCKS / 32u)

/** Physical on-disk size of the block allocation bitmap region. */
#define VFS_BITMAP_SIZE ((off_t)(VFS_BITMAP_WORDS * 4u))

/** Byte offset of the block allocation bitmap region inside the image. */
#define VFS_BITMAP_OFFSET ((off_t)VFS_SUPERBLOCK_SIZE)

/**
 * Number of extent records stored inline in each inode. Chosen so that
 * a typical file (contiguous or lightly fragmented) never needs an
 * overflow block: 32 extents at up to ~1 GiB each (256K blocks) covers
 * very large files without fragmentation.
 */
#define VFS_MAX_INLINE_EXTENTS 32u

/** Extent records per overflow block (4 KiB / 16 B). */
#define VFS_EXTENTS_PER_OVERFLOW_BLOCK 256u

/** Byte offset of the inode table inside the image. */
#define VFS_INODE_TABLE_OFFSET (VFS_BITMAP_OFFSET + VFS_BITMAP_SIZE)

/*
 * Exact packed size of vfs_inode_t: path(256) + size(8) + created_at(8) +
 * modified_at(8) + block_count(4) + extent_count(4) + inline_extent_count(4)
 * + overflow_block(4) + extents(32*16=512) + _pad(16) = 824 bytes.
 * Verified below via static_assert against sizeof(vfs_inode_t).
 */
#define VFS_INODE_ON_DISK_SIZE 824u

/** Byte offset of the first data block inside the image. */
#define VFS_DATA_OFFSET (VFS_INODE_TABLE_OFFSET + ((off_t)VFS_MAX_INODES * VFS_INODE_ON_DISK_SIZE))

/**
 * Metadata writeback is batched. A flush is forced when the number of
 * dirty inodes reaches this threshold, bounding the amount of data that
 * could be lost if the process is killed without a clean vfs_close().
 */
#define VFS_DIRTY_INODE_FLUSH_THRESHOLD 64u

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
    VFS_ERR_OVERFLOW = -8,  /**< Would exceed per-file extent capacity. */
    VFS_ERR_INVAL = -9,     /**< Invalid argument (NULL, bad whence...).*/
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

#define VFS_SEEK_SET 0 /**< From beginning of file. */
#define VFS_SEEK_CUR 1 /**< From current position.  */
#define VFS_SEEK_END 2 /**< From end of file.       */

/* -------------------------------------------------------------------------
 * On-disk structures
 * ---------------------------------------------------------------------- */

/**
 * One (logical, physical, length) mapping. A run of @ref length
 * contiguous logical blocks starting at @ref logical_block maps to a
 * run of the same length of contiguous physical blocks starting at
 * @ref physical_block.
 */
typedef struct __attribute__((packed)) {
    uint32_t logical_block;  /**< First logical block number covered.       */
    uint32_t physical_block; /**< First physical block number, or 0 = hole. */
    uint32_t length;         /**< Number of contiguous blocks in this run.  */
    uint32_t _pad;           /**< Reserved; keeps the record 16 bytes.      */
} vfs_extent_t;

/**
 * On-disk inode. Stores metadata and an inline sorted extent array.
 * A zero `path[0]` byte means the slot is free.
 */
typedef struct __attribute__((packed)) {
    char path[VFS_MAX_PATH];                      /**< Absolute virtual path, NUL-terminated. */
    uint64_t size;                                /**< Logical file size in bytes.            */
    uint64_t created_at;                          /**< Creation timestamp (Unix seconds).     */
    uint64_t modified_at;                         /**< Last-write timestamp (Unix seconds).   */
    uint32_t block_count;                         /**< Number of allocated physical blocks.   */
    uint32_t extent_count;                        /**< Number of extents in use, inline + overflow. */
    uint32_t inline_extent_count;                 /**< Number of extents stored inline here.  */
    uint32_t overflow_block;                      /**< First overflow extent block, or 0 = none. */
    vfs_extent_t extents[VFS_MAX_INLINE_EXTENTS]; /**< Sorted by logical_block, ascending.    */
    uint8_t _pad[16];                             /**< Reserved for future fields.            */
} vfs_inode_t;

/**
 * On-disk superblock. Always lives at offset 0 of the image file.
 * Fixed at VFS_SUPERBLOCK_SIZE bytes; the tail is unused/zeroed.
 */
typedef struct {
    uint32_t magic;            /**< Must equal VFS_MAGIC.             */
    uint32_t version;          /**< Must equal VFS_VERSION.           */
    uint32_t block_size;       /**< Must equal VFS_BLOCK_SIZE.        */
    uint32_t max_inodes;       /**< Must equal VFS_MAX_INODES.        */
    uint32_t total_blocks;     /**< Must equal VFS_TOTAL_BLOCKS.      */
    uint32_t free_block_count; /**< Informational; not authoritative. */
    uint32_t free_inode_count; /**< Informational; not authoritative. */
    uint32_t bitmap_words;     /**< Number of words in the bitmap.    */
    uint8_t _reserved[32];     /**< Future use/padding.               */
} vfs_super_t;

/* -------------------------------------------------------------------------
 * Runtime handles (opaque to callers)
 * ---------------------------------------------------------------------- */

/** Opaque handle for a mounted VFS image. Obtain via vfs_open()/vfs_create(). */
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
    char path[VFS_MAX_PATH]; /**< Virtual path of the file.       */
    uint64_t size;           /**< Logical file size in bytes.     */
    uint32_t block_count;    /**< Number of data blocks allocated.*/
    time_t created_at;       /**< Creation time.                  */
    time_t modified_at;      /**< Last modification time.         */
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
 * Flushes in-memory superblock, bitmap, and dirty inode entries to the
 * host file without closing the VFS.
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
 * Closes an open file descriptor.
 *
 * Does not force a metadata flush; dirty inode/bitmap state is left
 * batched for vfs_sync() or vfs_close() unless the dirty-count
 * threshold has been crossed. Call vfs_sync() explicitly if durability
 * across a crash is required immediately after close.
 *
 * @param vfs  Mounted VFS handle.
 * @param fd   Descriptor returned by vfs_fopen().
 * @return VFS_OK or VFS_ERR_BADFD.
 */
vfs_status_t vfs_fclose(vfs_t* vfs, vfs_fd_t fd);

/**
 * Reads up to @p count bytes from @p fd into @p buf.
 *
 * Advances the file position by the number of bytes read. Host I/O for
 * this call is never performed while holding any filesystem-wide lock;
 * only this file's own per-inode lock may be held, and only around
 * extent-map lookups, not around the pread(2) itself.
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
 * Allocates extents as needed, preferring large contiguous physical
 * runs. New blocks are zero-filled only when correctness requires it
 * (a partial block at the write boundary); full-block overwrites are
 * never zeroed first. In append mode the cursor is advanced to EOF
 * before the write.
 *
 * @param vfs    Mounted VFS handle.
 * @param fd     Open file descriptor (must have write permission).
 * @param buf    Source buffer.
 * @param count  Number of bytes to write.
 * @param[out] bytes_written  Bytes actually written. Never NULL.
 * @return VFS_OK or a negative vfs_status_t (VFS_ERR_NOSPACE, VFS_ERR_OVERFLOW...).
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
 * Extends with zero-mapped blocks if @p length > current size. Extents
 * beyond the new end are freed and coalesced back into the free-extent
 * list.
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
 * Transfers @p count bytes from a VFS file to a host file descriptor,
 * mirroring the Linux sendfile(2) interface.
 *
 * If @p offset is non-NULL it is used as the read position and is
 * updated to reflect the next unread byte on return; the file's
 * internal cursor is not modified (Linux sendfile semantics). If
 * @p offset is NULL the file's current cursor is used and advanced.
 *
 * The physical run(s) backing the transferred range are resolved under
 * the file's per-inode lock, then the lock is released before any data
 * moves. On Linux, allocated runs are transferred with the kernel
 * sendfile(2) syscall directly between the two host descriptors,
 * avoiding a userspace bounce buffer. On other POSIX platforms this
 * falls back to pread(2) + write(2) with a stack buffer sized to one
 * VFS block. Sparse holes are materialised as zero bytes on @p out_fd.
 *
 * @param vfs         Mounted VFS handle.
 * @param out_fd      Destination host file descriptor (socket, pipe, file).
 * @param in_fd       Source VFS file descriptor; must be open for reading.
 * @param offset      If non-NULL: read position in bytes (updated on return).
 *                    If NULL: use and advance the file's cursor.
 * @param count       Maximum number of bytes to transfer.
 * @param bytes_sent  Set to the number of bytes successfully written to
 *                    @p out_fd. Never NULL.
 * @return VFS_OK on success, or a VFS_ERR_* code on failure.
 *         A short transfer (bytes_sent < count) paired with VFS_OK means EOF
 *         was reached before @p count bytes were consumed.
 * @note Concurrent vfs_sendfile/vfs_fread/vfs_fwrite calls on the same
 *       @p in_fd from different threads are serialised by the file's
 *       per-inode lock; calls on different files proceed in parallel.
 */
vfs_status_t vfs_sendfile(vfs_t* vfs, int out_fd, vfs_fd_t in_fd, off_t* offset, size_t count, size_t* bytes_sent);

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
 * @brief Writes a contiguous memory buffer to a virtual path, creating or truncating the file.
 *
 * Encapsulates the open, write, truncate, and close lifecycle. If the
 * target file does not exist, it is created. If it already exists, its
 * length is truncated to zero before the content is written.
 *
 * @param vfs     Mounted VFS handle.
 * @param path    Absolute virtual path of the destination file.
 * @param content Source memory buffer containing payload bytes.
 * @param len     Total number of bytes to write from @p content.
 * @return VFS_OK on success, or a negative vfs_status_t error code.
 *         Specifically returns VFS_ERR_IO if the total bytes written does not match @p len.
 */
vfs_status_t vfs_write_file(vfs_t* vfs, const char* path, const void* content, size_t len);

/**
 * @brief Appends a contiguous memory buffer to a virtual path, creating the file if missing.
 *
 * @param vfs  Mounted VFS handle.
 * @param path Absolute virtual path of the destination file.
 * @param data Source memory buffer containing append payload bytes.
 * @param len  Total number of bytes to write from @p data.
 * @return VFS_OK on success, or a negative vfs_status_t error code.
 *         Specifically returns VFS_ERR_IO if the total bytes written does not match @p len.
 */
vfs_status_t vfs_append_file(vfs_t* vfs, const char* path, const void* data, size_t len);

/**
 * @brief Reads the entire contents of the file at @p path.
 *
 * @param vfs       Mounted VFS handle.
 * @param path      Absolute virtual path of the file to read.
 * @param[out] out_size  Set to the number of bytes read. Never NULL.
 * @return Heap-allocated buffer the caller must free(), or NULL on any
 *         failure (not found, I/O error, or allocation failure).
 */
void* vfs_read_file(vfs_t* vfs, const char* path, size_t* out_size);

/**
 * @brief Mounts a read-only or read-write VFS directly from a static memory array.
 *
 * @param embed_data Pointer to the embedded static byte array (e.g., asset_vfs_bytes).
 * @param embed_size Total size of the embedded byte array.
 * @param readonly   Mount the filesystem as read-only.
 * @param out_vfs    Pointer populated with the resulting initialized vfs_t handle.
 * @return VFS_OK, VFS_ERR_IO, or VFS_ERR_INVAL.
 */
vfs_status_t vfs_open_embedded(const void* embed_data, size_t embed_size, bool readonly, vfs_t** out_vfs);

static_assert(sizeof(vfs_extent_t) == 16, "vfs_extent_t size must be exactly 16 bytes");
static_assert(sizeof(vfs_inode_t) == VFS_INODE_ON_DISK_SIZE, "vfs_inode_t size mismatch");
static_assert(sizeof(vfs_super_t) <= VFS_SUPERBLOCK_SIZE, "vfs_super_t exceeds VFS_SUPERBLOCK_SIZE");

#if defined(__cplusplus)
}
#endif

#endif /* VFS_H */
