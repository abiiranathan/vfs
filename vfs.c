/**
 * @file vfs.c
 * @brief Implementation of the single-file virtual filesystem (VFS).
 *
 * See vfs.h for the complete API documentation and on-disk layout.
 *
 * ## Implementation notes
 *
 * ### In-memory state
 * A `vfs_t` holds:
 *   - the host file descriptor,
 *   - the fully-decoded superblock (`vfs_super_t`),
 *   - the complete inode table (`vfs_inode_t[VFS_MAX_INODES]`),
 *   - a table of open-file entries (`open_file_t[VFS_MAX_OPEN_FILES]`).
 *
 * The superblock and inode table are loaded entirely into memory on mount
 * and written back by vfs_sync() / vfs_close().  Individual data blocks are
 * read and written directly to/from the host file; they are never cached.
 *
 * ### Locking
 * A single `pthread_mutex_t` serialises every public API call.  The helpers
 * that do the real work (suffixed `_locked`) must be called with the mutex
 * already held.
 *
 * ### Block addressing
 * Block number N occupies host-file bytes
 *   [VFS_DATA_OFFSET + N*VFS_BLOCK_SIZE, VFS_DATA_OFFSET + (N+1)*VFS_BLOCK_SIZE).
 * Block 0 is valid; the bitmap uses 1 to mean FREE and 0 to mean USED.
 */

#include "vfs.h"

#include <assert.h>    /* assert                                    */
#include <errno.h>     /* errno, EINVAL …                           */
#include <fcntl.h>     /* open, O_RDONLY, O_RDWR, O_CREAT, O_TRUNC */
#include <pthread.h>   /* pthread_mutex_t, pthread_mutex_{lock,…}   */
#include <stdarg.h>    /* (unused – kept for future debug helpers)  */
#include <stdio.h>     /* FILE, fprintf, fflush                     */
#include <stdlib.h>    /* malloc, calloc, free                      */
#include <string.h>    /* memset, memcpy, strncmp, strlen           */
#include <sys/types.h> /* off_t, ssize_t                            */
#include <time.h>      /* time                                      */
#include <unistd.h>    /* pread, pwrite, close, ftruncate           */

/* -------------------------------------------------------------------------
 * Internal constants
 * ---------------------------------------------------------------------- */

/** Sentinel value meaning "no inode assigned". */
#define INODE_NONE UINT32_MAX

/** Sentinel value meaning "slot is free" in the open-file table. */
#define OFT_FREE (-1)

/* -------------------------------------------------------------------------
 * Internal: open-file table entry
 * ---------------------------------------------------------------------- */

/**
 * One entry in the runtime open-file table.
 * `inode_idx` == OFT_FREE means the slot is available.
 */
typedef struct {
    int inode_idx;  /**< Index into vfs->inodes[], or OFT_FREE.    */
    off_t pos;      /**< Current read/write cursor (logical bytes). */
    unsigned flags; /**< The VFS_O_* flags the file was opened with.*/
} open_file_t;

/* -------------------------------------------------------------------------
 * Internal: runtime VFS handle
 * ---------------------------------------------------------------------- */

/** Runtime representation of a mounted VFS image. */
struct vfs_t {
    int fd;                              /**< Host file descriptor.              */
    bool readonly;                       /**< True when mounted read-only.       */
    pthread_mutex_t lock;                /**< Serialises all metadata ops.       */
    vfs_super_t super;                   /**< In-memory superblock.              */
    vfs_inode_t inodes[VFS_MAX_INODES];  /**< In-memory inode table.             */
    open_file_t oft[VFS_MAX_OPEN_FILES]; /**< Open-file table.                   */
};

/* =========================================================================
 * Low-level I/O helpers
 * ======================================================================= */

/**
 * Writes exactly @p n bytes from @p buf at absolute host-file offset @p off.
 *
 * @return VFS_OK or VFS_ERR_IO.
 */
static vfs_status_t pwrite_all(int fd, const void* buf, size_t n, off_t off) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t rem = n;

    while (rem > 0) {
        ssize_t w = pwrite(fd, p, rem, off);
        if (w < 0) {
            if (errno == EINTR) { continue; }
            return VFS_ERR_IO;
        }
        if (w == 0) {
            /* Unexpected: pwrite should never return 0 on a regular file. */
            return VFS_ERR_IO;
        }
        p += (size_t)w;
        off += (off_t)w;
        rem -= (size_t)w;
    }
    return VFS_OK;
}

/**
 * Reads exactly @p n bytes into @p buf from absolute host-file offset @p off.
 *
 * @return VFS_OK or VFS_ERR_IO.
 */
static vfs_status_t pread_all(int fd, void* buf, size_t n, off_t off) {
    uint8_t* p = (uint8_t*)buf;
    size_t rem = n;

    while (rem > 0) {
        ssize_t r = pread(fd, p, rem, off);
        if (r < 0) {
            if (errno == EINTR) { continue; }
            return VFS_ERR_IO;
        }
        if (r == 0) {
            /* Premature EOF — image is truncated. */
            return VFS_ERR_IO;
        }
        p += (size_t)r;
        off += (off_t)r;
        rem -= (size_t)r;
    }
    return VFS_OK;
}

/* =========================================================================
 * Superblock & inode persistence
 * ======================================================================= */

/**
 * Flushes the in-memory superblock to the host file.
 * Caller must hold vfs->lock.
 *
 * @return VFS_OK or VFS_ERR_IO.
 */
static vfs_status_t super_write_locked(vfs_t* vfs) {
    /* Write the entire VFS_SUPERBLOCK_SIZE region; the tail beyond
     * sizeof(vfs_super_t) was zeroed at creation time and we never
     * dirty it, so this is safe. */
    return pwrite_all(vfs->fd, &vfs->super, sizeof(vfs->super), (off_t)0);
}

/**
 * Flushes all inodes to the host file.
 * Caller must hold vfs->lock.
 *
 * @return VFS_OK or VFS_ERR_IO.
 */
static vfs_status_t inodes_write_locked(vfs_t* vfs) {
    return pwrite_all(vfs->fd, vfs->inodes, sizeof(vfs->inodes), VFS_INODE_TABLE_OFFSET);
}

/**
 * Flushes a single inode to the host file.
 * Caller must hold vfs->lock.
 *
 * @param idx  Index into vfs->inodes[].
 * @return VFS_OK or VFS_ERR_IO.
 */
static vfs_status_t inode_write_locked(vfs_t* vfs, uint32_t idx) {
    assert(idx < VFS_MAX_INODES);
    off_t off = VFS_INODE_TABLE_OFFSET + (off_t)(idx * sizeof(vfs_inode_t));
    return pwrite_all(vfs->fd, &vfs->inodes[idx], sizeof(vfs_inode_t), off);
}

/* =========================================================================
 * Free-block bitmap helpers
 * ======================================================================= */

/**
 * Tests whether block @p blk is free (bitmap bit == 1).
 * Caller must hold vfs->lock.
 */
static bool bitmap_is_free(const vfs_t* vfs, uint32_t blk) {
    assert(blk < VFS_TOTAL_BLOCKS);
    uint32_t word = blk / 32u;
    uint32_t bit = blk % 32u;
    return (vfs->super.bitmap[word] & (UINT32_C(1) << bit)) != 0;
}

/**
 * Marks block @p blk as used (clears the bitmap bit).
 * Caller must hold vfs->lock.
 */
static void bitmap_set_used(vfs_t* vfs, uint32_t blk) {
    assert(blk < VFS_TOTAL_BLOCKS);
    uint32_t word = blk / 32u;
    uint32_t bit = blk % 32u;
    vfs->super.bitmap[word] &= ~(UINT32_C(1) << bit);
}

/**
 * Marks block @p blk as free (sets the bitmap bit).
 * Caller must hold vfs->lock.
 */
static void bitmap_set_free(vfs_t* vfs, uint32_t blk) {
    assert(blk < VFS_TOTAL_BLOCKS);
    uint32_t word = blk / 32u;
    uint32_t bit = blk % 32u;
    vfs->super.bitmap[word] |= (UINT32_C(1) << bit);
}

/**
 * Allocates one free block and marks it used.
 * Caller must hold vfs->lock.
 *
 * @param[out] out_blk  Block number allocated.
 * @return VFS_OK or VFS_ERR_NOSPACE.
 */
static vfs_status_t block_alloc_locked(vfs_t* vfs, uint32_t* out_blk) {
    for (uint32_t i = 0; i < VFS_TOTAL_BLOCKS; i++) {
        if (bitmap_is_free(vfs, i)) {
            bitmap_set_used(vfs, i);
            if (vfs->super.free_block_count > 0) { vfs->super.free_block_count--; }
            *out_blk = i;
            return VFS_OK;
        }
    }
    return VFS_ERR_NOSPACE;
}

/**
 * Frees a previously-allocated block.
 * Caller must hold vfs->lock.
 */
static void block_free_locked(vfs_t* vfs, uint32_t blk) {
    assert(blk < VFS_TOTAL_BLOCKS);
    if (!bitmap_is_free(vfs, blk)) {
        bitmap_set_free(vfs, blk);
        vfs->super.free_block_count++;
    }
}

/* =========================================================================
 * Inode helpers
 * ======================================================================= */

/**
 * Finds the inode index for @p path.
 * Caller must hold vfs->lock.
 *
 * @return Index on success, INODE_NONE if not found.
 */
static uint32_t inode_find_locked(const vfs_t* vfs, const char* path) {
    for (uint32_t i = 0; i < VFS_MAX_INODES; i++) {
        /* An inode slot is occupied when path[0] is non-zero. */
        if (vfs->inodes[i].path[0] != '\0' && strncmp(vfs->inodes[i].path, path, VFS_MAX_PATH - 1u) == 0) { return i; }
    }
    return INODE_NONE;
}

/**
 * Finds a free inode slot.
 * Caller must hold vfs->lock.
 *
 * @return Index on success, INODE_NONE if the table is full.
 */
static uint32_t inode_alloc_slot_locked(const vfs_t* vfs) {
    for (uint32_t i = 0; i < VFS_MAX_INODES; i++) {
        if (vfs->inodes[i].path[0] == '\0') { return i; }
    }
    return INODE_NONE;
}

/**
 * Frees all data blocks owned by inode @p idx and zeroes the inode.
 * Caller must hold vfs->lock.
 *
 * @return VFS_OK or VFS_ERR_IO (from inode_write_locked).
 */
static vfs_status_t inode_free_locked(vfs_t* vfs, uint32_t idx) {
    assert(idx < VFS_MAX_INODES);
    vfs_inode_t* in = &vfs->inodes[idx];

    for (uint32_t b = 0; b < in->block_count; b++) {
        block_free_locked(vfs, in->blocks[b]);
    }
    vfs->super.free_inode_count++;

    memset(in, 0, sizeof(*in));
    return inode_write_locked(vfs, idx);
}

/* =========================================================================
 * Open-file table helpers
 * ======================================================================= */

/**
 * Validates that @p fd is a live entry in vfs->oft[].
 * Caller must hold vfs->lock.
 *
 * @return Pointer to the open_file_t, or NULL on bad fd.
 */
static open_file_t* oft_get(vfs_t* vfs, vfs_fd_t fd) {
    if (fd < 0 || (unsigned int)fd >= VFS_MAX_OPEN_FILES) { return NULL; }
    open_file_t* of = &vfs->oft[(unsigned int)fd];
    if (of->inode_idx == OFT_FREE) { return NULL; }
    return of;
}

/* =========================================================================
 * Data-block I/O
 * ======================================================================= */

/**
 * Returns the host-file byte offset for logical block @p blk.
 */
static off_t block_offset(uint32_t blk) {
    return VFS_DATA_OFFSET + (off_t)blk * (off_t)VFS_BLOCK_SIZE;
}

/**
 * Zero-fills block @p blk on disk.
 * Caller must hold vfs->lock.
 *
 * @return VFS_OK or VFS_ERR_IO.
 */
static vfs_status_t block_zero_locked(vfs_t* vfs, uint32_t blk) {
    static const uint8_t zeros[VFS_BLOCK_SIZE]; /* BSS – zero-initialised */
    return pwrite_all(vfs->fd, zeros, VFS_BLOCK_SIZE, block_offset(blk));
}

/* =========================================================================
 * Mount helpers
 * ======================================================================= */

/**
 * Initialises the open-file table to all-free.
 */
static void oft_init(vfs_t* vfs) {
    for (uint32_t i = 0; i < VFS_MAX_OPEN_FILES; i++) {
        vfs->oft[i].inode_idx = OFT_FREE;
        vfs->oft[i].pos = 0;
        vfs->oft[i].flags = 0;
    }
}

/**
 * Allocates and partially initialises a vfs_t.
 *
 * @return Pointer on success, NULL on allocation failure.
 */
static vfs_t* vfs_alloc(void) {
    vfs_t* v = calloc(1, sizeof(*v));
    if (v == NULL) { return NULL; }
    v->fd = -1;

    if (pthread_mutex_init(&v->lock, NULL) != 0) {
        free(v);
        return NULL;
    }
    oft_init(v);
    return v;
}

/* =========================================================================
 * Public API – filesystem lifecycle
 * ======================================================================= */

vfs_status_t vfs_create(const char* image_path, vfs_t** out_vfs) {
    if (image_path == NULL || out_vfs == NULL) { return VFS_ERR_INVAL; }

    vfs_t* vfs = vfs_alloc();
    if (vfs == NULL) { return VFS_ERR_NOMEM; }

    /* Open (or create/truncate) the host file. */
    vfs->fd = open(image_path, O_RDWR | O_CREAT | O_TRUNC, (mode_t)0600);
    if (vfs->fd < 0) {
        pthread_mutex_destroy(&vfs->lock);
        free(vfs);
        return VFS_ERR_IO;
    }
    vfs->readonly = false;

    /* ---- Build the superblock ---- */
    vfs->super = (vfs_super_t){
        .magic = VFS_MAGIC,
        .version = VFS_VERSION,
        .block_size = VFS_BLOCK_SIZE,
        .max_inodes = VFS_MAX_INODES,
        .total_blocks = VFS_TOTAL_BLOCKS,
        .free_block_count = VFS_TOTAL_BLOCKS,
        .free_inode_count = VFS_MAX_INODES,
    };

    /* All bitmap bits set to 1 == all blocks free. */
    memset(vfs->super.bitmap, 0xFF, sizeof(vfs->super.bitmap));

    /* ---- Write superblock region (full VFS_SUPERBLOCK_SIZE bytes) ---- */
    {
        /* Write the struct itself. */
        vfs_status_t s = pwrite_all(vfs->fd, &vfs->super, sizeof(vfs->super), (off_t)0);
        if (s != VFS_OK) { goto io_error; }

        /* Zero the remaining padding so the region is fully initialised. */
        size_t pad = VFS_SUPERBLOCK_SIZE - sizeof(vfs->super);
        if (pad > 0) {
            uint8_t* zeros = calloc(1, pad);
            if (zeros == NULL) {
                vfs_close(vfs);
                return VFS_ERR_NOMEM;
            }
            s = pwrite_all(vfs->fd, zeros, pad, (off_t)sizeof(vfs->super));
            free(zeros);
            if (s != VFS_OK) { goto io_error; }
        }
    }

    /* ---- Write inode table (all zeroes — no files yet) ---- */
    {
        vfs_status_t s = pwrite_all(vfs->fd, vfs->inodes, sizeof(vfs->inodes), VFS_INODE_TABLE_OFFSET);
        if (s != VFS_OK) { goto io_error; }
    }

    *out_vfs = vfs;
    return VFS_OK;

io_error:
    vfs_close(vfs);
    return VFS_ERR_IO;
}

vfs_status_t vfs_open(const char* image_path, bool readonly, vfs_t** out_vfs) {
    if (image_path == NULL || out_vfs == NULL) { return VFS_ERR_INVAL; }

    vfs_t* vfs = vfs_alloc();
    if (vfs == NULL) { return VFS_ERR_NOMEM; }

    int oflags = readonly ? O_RDONLY : O_RDWR;
    vfs->fd = open(image_path, oflags, (mode_t)0);
    if (vfs->fd < 0) {
        pthread_mutex_destroy(&vfs->lock);
        free(vfs);
        return VFS_ERR_IO;
    }
    vfs->readonly = readonly;

    /* ---- Read superblock ---- */
    {
        vfs_status_t s = pread_all(vfs->fd, &vfs->super, sizeof(vfs->super), (off_t)0);
        if (s != VFS_OK) { goto io_error; }
    }

    /* ---- Validate magic and version ---- */
    if (vfs->super.magic != VFS_MAGIC || vfs->super.version != VFS_VERSION || vfs->super.block_size != VFS_BLOCK_SIZE ||
        vfs->super.max_inodes != VFS_MAX_INODES || vfs->super.total_blocks != VFS_TOTAL_BLOCKS) {
        pthread_mutex_destroy(&vfs->lock);
        close(vfs->fd);
        free(vfs);
        return VFS_ERR_CORRUPT;
    }

    /* ---- Read inode table ---- */
    {
        vfs_status_t s = pread_all(vfs->fd, vfs->inodes, sizeof(vfs->inodes), VFS_INODE_TABLE_OFFSET);
        if (s != VFS_OK) { goto io_error; }
    }

    *out_vfs = vfs;
    return VFS_OK;

io_error:
    pthread_mutex_destroy(&vfs->lock);
    close(vfs->fd);
    free(vfs);
    return VFS_ERR_IO;
}

void vfs_close(vfs_t* vfs) {
    if (vfs == NULL) { return; }

    /* Best-effort flush; errors are swallowed on close. */
    if (!vfs->readonly && vfs->fd >= 0) {
        pthread_mutex_lock(&vfs->lock);
        (void)super_write_locked(vfs);
        (void)inodes_write_locked(vfs);
        pthread_mutex_unlock(&vfs->lock);
    }

    if (vfs->fd >= 0) {
        (void)close(vfs->fd);
        vfs->fd = -1;
    }

    pthread_mutex_destroy(&vfs->lock);
    free(vfs);
}

vfs_status_t vfs_sync(vfs_t* vfs) {
    if (vfs == NULL) { return VFS_ERR_INVAL; }
    if (vfs->readonly) { return VFS_OK; /* Nothing to flush. */ }

    pthread_mutex_lock(&vfs->lock);

    vfs_status_t s = super_write_locked(vfs);
    if (s == VFS_OK) { s = inodes_write_locked(vfs); }

    pthread_mutex_unlock(&vfs->lock);
    return s;
}

/* =========================================================================
 * Public API – file operations
 * ======================================================================= */

vfs_fd_t vfs_fopen(vfs_t* vfs, const char* path, unsigned int flags) {
    if (vfs == NULL || path == NULL || path[0] == '\0') { return (vfs_fd_t)VFS_ERR_INVAL; }
    if (vfs->readonly && (flags & (VFS_O_WRONLY | VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC))) {
        return (vfs_fd_t)VFS_ERR_READONLY;
    }

    pthread_mutex_lock(&vfs->lock);

    vfs_status_t rc;
    uint32_t inode_idx = inode_find_locked(vfs, path);

    /* ---- Handle VFS_O_CREAT / VFS_O_EXCL ---- */
    if (flags & VFS_O_CREAT) {
        if (inode_idx != INODE_NONE) {
            /* File already exists. */
            if (flags & VFS_O_EXCL) {
                rc = VFS_ERR_EXISTS;
                goto out;
            }
            /* Fall through – open the existing file. */
        } else {
            /* Allocate a new inode. */
            inode_idx = inode_alloc_slot_locked(vfs);
            if (inode_idx == INODE_NONE) {
                rc = VFS_ERR_NOSPACE;
                goto out;
            }

            vfs_inode_t* in = &vfs->inodes[inode_idx];
            memset(in, 0, sizeof(*in));
            strncpy(in->path, path, VFS_MAX_PATH - 1u);
            in->path[VFS_MAX_PATH - 1u] = '\0';
            in->created_at = (uint64_t)time(NULL);
            in->modified_at = in->created_at;
            in->size = 0;
            in->block_count = 0;

            if (vfs->super.free_inode_count > 0) { vfs->super.free_inode_count--; }

            rc = inode_write_locked(vfs, inode_idx);
            if (rc != VFS_OK) {
                /* Roll back the inode slot. */
                memset(in, 0, sizeof(*in));
                goto out;
            }
        }
    } else {
        /* No O_CREAT – the file must exist. */
        if (inode_idx == INODE_NONE) {
            rc = VFS_ERR_NOTFOUND;
            goto out;
        }
    }

    /* ---- Find a free slot in the open-file table ---- */
    vfs_fd_t fd = -1;
    for (uint32_t i = 0; i < VFS_MAX_OPEN_FILES; i++) {
        if (vfs->oft[i].inode_idx == OFT_FREE) {
            fd = (vfs_fd_t)i;
            break;
        }
    }
    if (fd < 0) {
        rc = VFS_ERR_NOSPACE;
        goto out;
    }

    /* ---- Handle VFS_O_TRUNC ---- */
    if ((flags & VFS_O_TRUNC) && (flags & (VFS_O_WRONLY | VFS_O_RDWR))) {
        vfs_inode_t* in = &vfs->inodes[inode_idx];
        for (uint32_t b = 0; b < in->block_count; b++) {
            block_free_locked(vfs, in->blocks[b]);
        }
        in->block_count = 0;
        in->size = 0;
        in->modified_at = (uint64_t)time(NULL);
        rc = inode_write_locked(vfs, inode_idx);
        if (rc != VFS_OK) { goto out; }
    }

    /* ---- Fill in the OFT entry ---- */
    vfs->oft[(unsigned int)fd].inode_idx = (int)inode_idx;
    vfs->oft[(unsigned int)fd].flags = flags;

    /* For append mode, cursor starts at EOF; otherwise at 0. */
    vfs->oft[(unsigned int)fd].pos = (flags & VFS_O_APPEND) ? (off_t)vfs->inodes[inode_idx].size : (off_t)0;

    rc = (vfs_status_t)fd; /* Returning fd value >= 0 signals success. */

out:
    pthread_mutex_unlock(&vfs->lock);
    return (vfs_fd_t)rc;
}

vfs_status_t vfs_fclose(vfs_t* vfs, vfs_fd_t fd) {
    if (vfs == NULL) { return VFS_ERR_INVAL; }

    pthread_mutex_lock(&vfs->lock);

    open_file_t* of = oft_get(vfs, fd);
    if (of == NULL) {
        pthread_mutex_unlock(&vfs->lock);
        return VFS_ERR_BADFD;
    }

    /* Mark the slot free. */
    of->inode_idx = OFT_FREE;
    of->pos = 0;
    of->flags = 0;

    pthread_mutex_unlock(&vfs->lock);
    return VFS_OK;
}

vfs_status_t vfs_fread(vfs_t* vfs, vfs_fd_t fd, void* buf, size_t count, size_t* bytes_read) {
    if (vfs == NULL || buf == NULL || bytes_read == NULL) { return VFS_ERR_INVAL; }
    *bytes_read = 0;

    if (count == 0) { return VFS_OK; }

    pthread_mutex_lock(&vfs->lock);

    open_file_t* of = oft_get(vfs, fd);
    if (of == NULL) {
        pthread_mutex_unlock(&vfs->lock);
        return VFS_ERR_BADFD;
    }

    /* A write-only descriptor cannot be read. */
    if ((of->flags & VFS_O_WRONLY) && !(of->flags & VFS_O_RDWR)) {
        pthread_mutex_unlock(&vfs->lock);
        return VFS_ERR_INVAL;
    }

    vfs_inode_t* in = &vfs->inodes[(uint32_t)of->inode_idx];
    uint64_t fsize = in->size;

    /* Clamp to available bytes. */
    if ((uint64_t)of->pos >= fsize) {
        /* At or past EOF. */
        pthread_mutex_unlock(&vfs->lock);
        return VFS_OK;
    }
    uint64_t avail = fsize - (uint64_t)of->pos;
    if ((uint64_t)count > avail) { count = (size_t)avail; }

    uint8_t* dst = (uint8_t*)buf;
    size_t remaining = count;
    off_t cur_pos = of->pos;

    while (remaining > 0) {
        uint32_t block_idx = (uint32_t)((uint64_t)cur_pos / VFS_BLOCK_SIZE);
        uint32_t block_off = (uint32_t)((uint64_t)cur_pos % VFS_BLOCK_SIZE);
        size_t can_read = VFS_BLOCK_SIZE - block_off;
        if (can_read > remaining) { can_read = remaining; }

        if (block_idx >= in->block_count) {
            /* Sparse region: return zeroes. */
            memset(dst, 0, can_read);
        } else {
            uint32_t blk = in->blocks[block_idx];
            off_t off = block_offset(blk) + (off_t)block_off;
            vfs_status_t s = pread_all(vfs->fd, dst, can_read, off);
            if (s != VFS_OK) {
                pthread_mutex_unlock(&vfs->lock);
                return s;
            }
        }

        dst += can_read;
        cur_pos += (off_t)can_read;
        remaining -= can_read;
    }

    of->pos = cur_pos;
    *bytes_read = count;

    pthread_mutex_unlock(&vfs->lock);
    return VFS_OK;
}

vfs_status_t vfs_fwrite(vfs_t* vfs, vfs_fd_t fd, const void* buf, size_t count, size_t* bytes_written) {
    if (vfs == NULL || buf == NULL || bytes_written == NULL) { return VFS_ERR_INVAL; }
    *bytes_written = 0;

    if (count == 0) { return VFS_OK; }

    if (vfs->readonly) { return VFS_ERR_READONLY; }

    pthread_mutex_lock(&vfs->lock);

    open_file_t* of = oft_get(vfs, fd);
    if (of == NULL) {
        pthread_mutex_unlock(&vfs->lock);
        return VFS_ERR_BADFD;
    }

    /* Must be opened with write permission. */
    if (!(of->flags & (VFS_O_WRONLY | VFS_O_RDWR))) {
        pthread_mutex_unlock(&vfs->lock);
        return VFS_ERR_INVAL;
    }

    /* In append mode, always write at the current end of file. */
    if (of->flags & VFS_O_APPEND) { of->pos = (off_t)vfs->inodes[(uint32_t)of->inode_idx].size; }

    vfs_inode_t* in = &vfs->inodes[(uint32_t)of->inode_idx];
    const uint8_t* src = (const uint8_t*)buf;
    size_t remaining = count;
    off_t cur_pos = of->pos;
    vfs_status_t rc = VFS_OK;

    while (remaining > 0) {
        uint32_t block_idx = (uint32_t)((uint64_t)cur_pos / VFS_BLOCK_SIZE);
        uint32_t block_off = (uint32_t)((uint64_t)cur_pos % VFS_BLOCK_SIZE);
        size_t can_write = VFS_BLOCK_SIZE - block_off;
        if (can_write > remaining) { can_write = remaining; }

        /* Allocate a new block if needed. */
        if (block_idx >= in->block_count) {
            if (in->block_count >= VFS_MAX_BLOCKS_PER_FILE) {
                rc = VFS_ERR_OVERFLOW;
                break;
            }
            uint32_t new_blk;
            rc = block_alloc_locked(vfs, &new_blk);
            if (rc != VFS_OK) { break; }
            /* Zero out the freshly-allocated block on disk. */
            rc = block_zero_locked(vfs, new_blk);
            if (rc != VFS_OK) {
                block_free_locked(vfs, new_blk);
                break;
            }
            in->blocks[in->block_count++] = new_blk;
        }

        uint32_t blk = in->blocks[block_idx];
        off_t off = block_offset(blk) + (off_t)block_off;
        rc = pwrite_all(vfs->fd, src, can_write, off);
        if (rc != VFS_OK) { break; }

        src += can_write;
        cur_pos += (off_t)can_write;
        remaining -= can_write;
    }

    size_t written = count - remaining;
    *bytes_written = written;

    /* Extend the logical file size if we wrote beyond the old end. */
    if ((uint64_t)cur_pos > in->size) { in->size = (uint64_t)cur_pos; }
    in->modified_at = (uint64_t)time(NULL);
    of->pos = cur_pos;

    /* Persist the updated inode; report I/O errors over earlier errors. */
    vfs_status_t ws = inode_write_locked(vfs, (uint32_t)of->inode_idx);
    if (ws != VFS_OK && rc == VFS_OK) { rc = ws; }

    pthread_mutex_unlock(&vfs->lock);
    return rc;
}

vfs_status_t vfs_fseek(vfs_t* vfs, vfs_fd_t fd, off_t offset, int whence, off_t* new_offset) {
    if (vfs == NULL) { return VFS_ERR_INVAL; }

    pthread_mutex_lock(&vfs->lock);

    open_file_t* of = oft_get(vfs, fd);
    if (of == NULL) {
        pthread_mutex_unlock(&vfs->lock);
        return VFS_ERR_BADFD;
    }

    vfs_inode_t* in = &vfs->inodes[(uint32_t)of->inode_idx];
    off_t base;

    switch (whence) {
        case VFS_SEEK_SET:
            base = (off_t)0;
            break;
        case VFS_SEEK_CUR:
            base = of->pos;
            break;
        case VFS_SEEK_END:
            base = (off_t)in->size;
            break;
        default:
            pthread_mutex_unlock(&vfs->lock);
            return VFS_ERR_INVAL;
    }

    off_t result = base + offset;
    if (result < 0) {
        pthread_mutex_unlock(&vfs->lock);
        return VFS_ERR_INVAL;
    }

    of->pos = result;
    if (new_offset != NULL) { *new_offset = result; }

    pthread_mutex_unlock(&vfs->lock);
    return VFS_OK;
}

vfs_status_t vfs_ftell(vfs_t* vfs, vfs_fd_t fd, off_t* pos) {
    if (vfs == NULL || pos == NULL) { return VFS_ERR_INVAL; }

    pthread_mutex_lock(&vfs->lock);

    open_file_t* of = oft_get(vfs, fd);
    if (of == NULL) {
        pthread_mutex_unlock(&vfs->lock);
        return VFS_ERR_BADFD;
    }

    *pos = of->pos;

    pthread_mutex_unlock(&vfs->lock);
    return VFS_OK;
}

vfs_status_t vfs_truncate(vfs_t* vfs, const char* path, uint64_t length) {
    if (vfs == NULL || path == NULL) { return VFS_ERR_INVAL; }
    if (vfs->readonly) { return VFS_ERR_READONLY; }

    pthread_mutex_lock(&vfs->lock);

    uint32_t idx = inode_find_locked(vfs, path);
    if (idx == INODE_NONE) {
        pthread_mutex_unlock(&vfs->lock);
        return VFS_ERR_NOTFOUND;
    }

    vfs_inode_t* in = &vfs->inodes[idx];
    vfs_status_t rc = VFS_OK;

    if (length < in->size) {
        /* ---- Shrink: free blocks beyond the new size ---- */
        uint32_t new_block_count = (uint32_t)((length + VFS_BLOCK_SIZE - 1u) / VFS_BLOCK_SIZE);

        for (uint32_t b = new_block_count; b < in->block_count; b++) {
            block_free_locked(vfs, in->blocks[b]);
            in->blocks[b] = 0;
        }
        in->block_count = new_block_count;
        in->size = length;

        /* If the new size falls in the middle of a block, zero the tail
         * of that last block so reads past the new EOF return zeroes. */
        if (new_block_count > 0 && (length % VFS_BLOCK_SIZE) != 0) {
            uint32_t last_blk = in->blocks[new_block_count - 1u];
            uint32_t tail_off = (uint32_t)(length % VFS_BLOCK_SIZE);
            uint32_t tail_len = VFS_BLOCK_SIZE - tail_off;
            off_t write_off = block_offset(last_blk) + (off_t)tail_off;
            uint8_t* zeros = calloc(1, tail_len);
            if (zeros == NULL) {
                rc = VFS_ERR_NOMEM;
                goto out;
            }
            rc = pwrite_all(vfs->fd, zeros, tail_len, write_off);
            free(zeros);
            if (rc != VFS_OK) { goto out; }
        }
    } else if (length > in->size) {
        /* ---- Extend: allocate blocks and zero-fill ---- */
        uint32_t new_block_count = (uint32_t)((length + VFS_BLOCK_SIZE - 1u) / VFS_BLOCK_SIZE);

        if (new_block_count > VFS_MAX_BLOCKS_PER_FILE) {
            rc = VFS_ERR_OVERFLOW;
            goto out;
        }

        /* Allocate any new blocks needed. */
        for (uint32_t b = in->block_count; b < new_block_count; b++) {
            uint32_t new_blk;
            rc = block_alloc_locked(vfs, &new_blk);
            if (rc != VFS_OK) { goto out; }
            rc = block_zero_locked(vfs, new_blk);
            if (rc != VFS_OK) {
                block_free_locked(vfs, new_blk);
                goto out;
            }
            in->blocks[b] = new_blk;
            in->block_count++;
        }
        in->size = length;
    }
    /* length == in->size: no-op. */

    in->modified_at = (uint64_t)time(NULL);
    rc = inode_write_locked(vfs, idx);

out:
    pthread_mutex_unlock(&vfs->lock);
    return rc;
}

vfs_status_t vfs_stat(vfs_t* vfs, const char* path, vfs_stat_t* st) {
    if (vfs == NULL || path == NULL || st == NULL) { return VFS_ERR_INVAL; }

    pthread_mutex_lock(&vfs->lock);

    uint32_t idx = inode_find_locked(vfs, path);
    if (idx == INODE_NONE) {
        pthread_mutex_unlock(&vfs->lock);
        return VFS_ERR_NOTFOUND;
    }

    const vfs_inode_t* in = &vfs->inodes[idx];
    *st = (vfs_stat_t){
        .size = in->size,
        .block_count = in->block_count,
        .created_at = (time_t)in->created_at,
        .modified_at = (time_t)in->modified_at,
    };
    strncpy(st->path, in->path, VFS_MAX_PATH - 1u);
    st->path[VFS_MAX_PATH - 1u] = '\0';

    pthread_mutex_unlock(&vfs->lock);
    return VFS_OK;
}

vfs_status_t vfs_unlink(vfs_t* vfs, const char* path) {
    if (vfs == NULL || path == NULL) { return VFS_ERR_INVAL; }
    if (vfs->readonly) { return VFS_ERR_READONLY; }

    pthread_mutex_lock(&vfs->lock);

    uint32_t idx = inode_find_locked(vfs, path);
    if (idx == INODE_NONE) {
        pthread_mutex_unlock(&vfs->lock);
        return VFS_ERR_NOTFOUND;
    }

    /* Close any open descriptors pointing at this inode. */
    for (uint32_t i = 0; i < VFS_MAX_OPEN_FILES; i++) {
        if (vfs->oft[i].inode_idx == (int)idx) {
            vfs->oft[i].inode_idx = OFT_FREE;
            vfs->oft[i].pos = 0;
            vfs->oft[i].flags = 0;
        }
    }

    vfs_status_t rc = inode_free_locked(vfs, idx);

    pthread_mutex_unlock(&vfs->lock);
    return rc;
}

bool vfs_exists(vfs_t* vfs, const char* path) {
    if (vfs == NULL || path == NULL) { return false; }

    pthread_mutex_lock(&vfs->lock);
    bool found = (inode_find_locked(vfs, path) != INODE_NONE);
    pthread_mutex_unlock(&vfs->lock);

    return found;
}

/* =========================================================================
 * Public API – directory-like listing
 * ======================================================================= */

void vfs_list(vfs_t* vfs, const char* prefix, vfs_list_cb_t callback, void* userdata) {
    if (vfs == NULL || callback == NULL) { return; }

    /* Treat NULL or empty prefix as "match everything". */
    bool match_all = (prefix == NULL || prefix[0] == '\0' || (prefix[0] == '/' && prefix[1] == '\0'));
    size_t prefix_len = match_all ? 0u : strlen(prefix);

    pthread_mutex_lock(&vfs->lock);

    for (uint32_t i = 0; i < VFS_MAX_INODES; i++) {
        const vfs_inode_t* in = &vfs->inodes[i];
        if (in->path[0] == '\0') { continue; /* Free slot. */ }

        if (!match_all && strncmp(in->path, prefix, prefix_len) != 0) { continue; }

        vfs_stat_t st = {
            .size = in->size,
            .block_count = in->block_count,
            .created_at = (time_t)in->created_at,
            .modified_at = (time_t)in->modified_at,
        };
        strncpy(st.path, in->path, VFS_MAX_PATH - 1u);
        st.path[VFS_MAX_PATH - 1u] = '\0';

        /* Release the lock around the callback to avoid re-entrancy
         * deadlocks if the caller calls back into the VFS. */
        pthread_mutex_unlock(&vfs->lock);
        bool cont = callback(in->path, &st, userdata);
        pthread_mutex_lock(&vfs->lock);

        if (!cont) { break; }
    }

    pthread_mutex_unlock(&vfs->lock);
}

vfs_status_t vfs_rename(vfs_t* vfs, const char* oldpath, const char* newpath) {
    if (vfs == NULL || oldpath == NULL || newpath == NULL) { return VFS_ERR_INVAL; }
    if (oldpath[0] == '\0' || newpath[0] == '\0') { return VFS_ERR_INVAL; }
    if (vfs->readonly) { return VFS_ERR_READONLY; }

    /* newpath must fit in the inode's fixed path buffer. */
    size_t new_len = strlen(newpath);
    if (new_len >= VFS_MAX_PATH) { return VFS_ERR_INVAL; }

    /* No-op short circuit — compare before acquiring the lock. */
    if (strncmp(oldpath, newpath, VFS_MAX_PATH) == 0) { return VFS_OK; }

    pthread_mutex_lock(&vfs->lock);

    vfs_status_t rc = VFS_OK;

    uint32_t src_idx = inode_find_locked(vfs, oldpath);
    if (src_idx == INODE_NONE) {
        rc = VFS_ERR_NOTFOUND;
        goto out;
    }

    /* ---- Handle destination ---- */
    uint32_t dst_idx = inode_find_locked(vfs, newpath);
    if (dst_idx != INODE_NONE && dst_idx != src_idx) {
        /*
         * Destination exists and is a different inode.  Evict any open
         * file descriptors pointing at it, free its blocks, and zero its
         * slot — identical to vfs_unlink, minus the lock acquire.
         */
        for (uint32_t i = 0; i < VFS_MAX_OPEN_FILES; i++) {
            if (vfs->oft[i].inode_idx == (int)dst_idx) {
                vfs->oft[i].inode_idx = OFT_FREE;
                vfs->oft[i].pos = 0;
                vfs->oft[i].flags = 0;
            }
        }

        /* Step A: persist the zeroed victim inode before touching the source. */
        rc = inode_free_locked(vfs, dst_idx);
        if (rc != VFS_OK) { goto out; }
    }

    /* ---- Step B: update the source inode's path ---- */
    vfs_inode_t* src = &vfs->inodes[src_idx];
    memset(src->path, 0, VFS_MAX_PATH);
    memcpy(src->path, newpath, new_len); /* memcpy: length already validated */
    src->modified_at = (uint64_t)time(NULL);

    rc = inode_write_locked(vfs, src_idx);
    if (rc != VFS_OK) { goto out; }

    /* ---- Step C: persist updated superblock free-counts ---- */
    rc = super_write_locked(vfs);

out:
    pthread_mutex_unlock(&vfs->lock);
    return rc;
}

/* =========================================================================
 * Public API – utility
 * ======================================================================= */

const char* vfs_strerror(vfs_status_t status) {
    switch (status) {
        case VFS_OK:
            return "success";
        case VFS_ERR_IO:
            return "host I/O error";
        case VFS_ERR_CORRUPT:
            return "image corrupted or invalid magic";
        case VFS_ERR_NOTFOUND:
            return "no such file";
        case VFS_ERR_EXISTS:
            return "file already exists";
        case VFS_ERR_NOSPACE:
            return "no free inodes or data blocks";
        case VFS_ERR_NOMEM:
            return "memory allocation failure";
        case VFS_ERR_BADFD:
            return "invalid or closed file descriptor";
        case VFS_ERR_OVERFLOW:
            return "would exceed per-file block limit";
        case VFS_ERR_INVAL:
            return "invalid argument";
        case VFS_ERR_ISDIR:
            return "path is a directory";
        case VFS_ERR_READONLY:
            return "filesystem is read-only";
        default:
            return "unknown error";
    }
}

void vfs_dump(const vfs_t* vfs, FILE* out) {
    if (vfs == NULL || out == NULL) { return; }

    /* Cast away const for the mutex — the lock is logically const here. */
    vfs_t* v = (vfs_t*)(uintptr_t)vfs;
    pthread_mutex_lock(&v->lock);

    const vfs_super_t* sb = &vfs->super;

    fprintf(out, "=== VFS Superblock ===\n");
    fprintf(out, "  magic            : 0x%08X\n", sb->magic);
    fprintf(out, "  version          : %u\n", sb->version);
    fprintf(out, "  block_size       : %u\n", sb->block_size);
    fprintf(out, "  max_inodes       : %u\n", sb->max_inodes);
    fprintf(out, "  total_blocks     : %u\n", sb->total_blocks);
    fprintf(out, "  free_block_count : %u\n", sb->free_block_count);
    fprintf(out, "  free_inode_count : %u\n", sb->free_inode_count);

    /* Count actually-used inodes for a sanity cross-check. */
    uint32_t used_inodes = 0;
    for (uint32_t i = 0; i < VFS_MAX_INODES; i++) {
        if (vfs->inodes[i].path[0] != '\0') { used_inodes++; }
    }
    fprintf(out, "  used_inodes      : %u (counted)\n", used_inodes);

    fprintf(out, "\n=== Inode Table ===\n");
    for (uint32_t i = 0; i < VFS_MAX_INODES; i++) {
        const vfs_inode_t* in = &vfs->inodes[i];
        if (in->path[0] == '\0') { continue; }
        fprintf(out, "  [%4u] path=%-32s  size=%-10" PRIu64 "  blocks=%-4u  mtime=%" PRIu64 "\n", i, in->path, in->size,
                in->block_count, in->modified_at);
    }

    fprintf(out, "\n=== Open-file Table ===\n");
    for (uint32_t i = 0; i < VFS_MAX_OPEN_FILES; i++) {
        const open_file_t* of = &vfs->oft[i];
        if (of->inode_idx == OFT_FREE) { continue; }
        fprintf(out, "  fd=%-3u  inode=%-4d  pos=%-10" PRId64 "  flags=0x%02X\n", i, of->inode_idx, (int64_t)of->pos,
                of->flags);
    }

    fflush(out);
    pthread_mutex_unlock(&v->lock);
}
