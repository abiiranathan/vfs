/* memfd_create(2) and MFD_CLOEXEC are glibc extensions gated behind
 * _GNU_SOURCE; define it here (before any system header is pulled in)
 * so this translation unit does not depend on the build system passing
 * -D_GNU_SOURCE on the command line. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
    #define _GNU_SOURCE
#endif

/**
 * @file vfs.c
 * @brief High-throughput single-file Virtual Filesystem (VFS), format v3.
 *
 * Design summary (see vfs.h for the full rationale):
 *   - Per-inode pthread_rwlock_t for data-plane operations; a single
 *     filesystem-wide metadata rwlock for allocation, inode table
 *     mutation, and rename/unlink. Neither lock is ever held across a
 *     host pread/pwrite/sendfile call.
 *   - In-memory sorted free-extent list layered on the durable bitmap
 *     for O(log n) allocation of large contiguous runs.
 *   - Inline sorted extent array per inode (logical, physical, length)
 *     with binary-search lookup; falls back to a chained overflow
 *     extent block for heavily fragmented files.
 *   - Dirty-flag batched metadata writeback: bitmap and inode entries
 *     are marked dirty and flushed on vfs_sync()/vfs_close(), or when
 *     the dirty-inode count crosses VFS_DIRTY_INODE_FLUSH_THRESHOLD.
 *   - Zero-fill is skipped whenever a newly allocated block is about to
 *     be fully overwritten by the caller's own write.
 */

#include "vfs.h"

#include <fcntl.h>  /* open, O_* */
#include <limits.h> /* SSIZE_MAX */
#include <stdlib.h> /* malloc, calloc, free, mkstemp */
#include <string.h> /* memset, memcpy, strncpy, strlen */
#include <unistd.h> /* pread, pwrite, close, write, unlink */

#if defined(__linux__)
    #include <sys/mman.h>     /* memfd_create, MFD_CLOEXEC */
    #include <sys/sendfile.h> /* sendfile(2) */
#endif

/** Sentinel: no inode assigned. */
#define INODE_NONE UINT32_MAX

/** Sentinel: slot is free in the open-file table. */
#define OFT_FREE (-1)

/**
 * One entry in the runtime open-file table.
 * `inode_idx` == OFT_FREE means the slot is available.
 */
typedef struct {
    int inode_idx;  /**< Index into vfs->inodes[], or OFT_FREE.      */
    off_t pos;      /**< Current read/write cursor (logical bytes).  */
    unsigned flags; /**< The VFS_O_* flags the file was opened with. */
} open_file_t;

/** In-memory (start, length) free run, kept sorted by start ascending. */
typedef struct {
    uint32_t start;
    uint32_t len;
} free_extent_t;

/**
 * Runtime overflow-extent-block cache. Overflow blocks are read/written
 * whole; only one is cached at a time per filesystem, guarded by the
 * metadata lock, mirroring the v2 SIB/DIB cache but for extents.
 */
typedef struct {
    uint32_t cached_block;                                       /**< Physical block currently cached, 0 = none. */
    bool dirty;                                                  /**< True if cached_extents needs writeback.    */
    vfs_extent_t cached_extents[VFS_EXTENTS_PER_OVERFLOW_BLOCK]; /**< Cached overflow extent array.          */
} overflow_cache_t;

struct vfs_t {
    /* ---- Locks ---- */
    pthread_rwlock_t meta_lock;                   /**< Guards bitmap, free list, inode table, superblock. */
    pthread_rwlock_t inode_locks[VFS_MAX_INODES]; /**< Per-inode data-plane lock (extents + size + cursor). */

    /* ---- Host file ---- */
    int fd; /**< Host file descriptor for the image. */

    /* ---- Superblock (protected by meta_lock) ---- */
    vfs_super_t super;

    /* ---- Inode table (protected by meta_lock for structural changes;
     *      per-inode extent/size fields protected by inode_locks[i]) ---- */
    vfs_inode_t inodes[VFS_MAX_INODES];
    bool inode_dirty[VFS_MAX_INODES];
    uint32_t dirty_inode_count;

    /* ---- Open-file table (protected by meta_lock) ---- */
    open_file_t oft[VFS_MAX_OPEN_FILES];

    /* ---- Block bitmap (protected by meta_lock) ---- */
    uint32_t* bitmap; /**< Durable free-block bitmap, 1 = free. */
    bool bitmap_dirty;

    /* ---- Free-extent allocator (protected by meta_lock) ---- */
    free_extent_t* free_extents; /**< Sorted array of free runs. */
    uint32_t free_extent_count;
    uint32_t free_extent_cap;
    uint32_t last_allocated_block; /**< For sequential-allocation fast path. */

    /* ---- Overflow extent cache (protected by meta_lock) ---- */
    overflow_cache_t overflow_cache;

    bool readonly;
};

/* =========================================================================
 * Low-level I/O helpers (no locks held; safe to call concurrently on
 * different byte ranges since all calls are pread/pwrite with explicit
 * offsets on the same fd)
 * ======================================================================= */

/** Writes exactly @p n bytes from @p buf at absolute host-file offset @p off. */
static vfs_status_t pwrite_all(int fd, const void* buf, size_t n, off_t off) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t rem = n;
    while (rem > 0) {
        ssize_t w = pwrite(fd, p, rem, off);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return VFS_ERR_IO;
        }
        if (w == 0) {
            return VFS_ERR_IO;
        }
        p += (size_t)w;
        off += (off_t)w;
        rem -= (size_t)w;
    }
    return VFS_OK;
}

/** Reads exactly @p n bytes into @p buf from absolute host-file offset @p off. */
static vfs_status_t pread_all(int fd, void* buf, size_t n, off_t off) {
    uint8_t* p = (uint8_t*)buf;
    size_t rem = n;
    while (rem > 0) {
        ssize_t r = pread(fd, p, rem, off);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return VFS_ERR_IO;
        }
        if (r == 0) {
            return VFS_ERR_IO;
        }
        p += (size_t)r;
        off += (off_t)r;
        rem -= (size_t)r;
    }
    return VFS_OK;
}

/** Writes exactly @p n bytes from @p buf to a non-seekable fd (socket/pipe). */
static vfs_status_t write_all(int fd, const void* buf, size_t n) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t rem = n;
    while (rem > 0) {
        ssize_t w = write(fd, p, rem);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return VFS_ERR_IO;
        }
        if (w == 0) {
            return VFS_ERR_IO;
        }
        p += (size_t)w;
        rem -= (size_t)w;
    }
    return VFS_OK;
}

/** Returns the host-file byte offset for physical block @p blk. */
static inline off_t block_offset(uint32_t blk) { return VFS_DATA_OFFSET + (off_t)blk * (off_t)VFS_BLOCK_SIZE; }

/** Zero-fills physical block @p blk on disk. */
static inline vfs_status_t block_zero(int fd, uint32_t blk) {
    static const uint8_t zeros[VFS_BLOCK_SIZE];
    return pwrite_all(fd, zeros, VFS_BLOCK_SIZE, block_offset(blk));
}

/* =========================================================================
 * Superblock / bitmap / inode persistence  (caller holds meta_lock)
 * ======================================================================= */

static inline vfs_status_t super_write_locked(vfs_t* vfs) {
    return pwrite_all(vfs->fd, &vfs->super, sizeof(vfs->super), (off_t)0);
}

static inline vfs_status_t bitmap_write_locked(vfs_t* vfs) {
    return pwrite_all(vfs->fd, vfs->bitmap, VFS_BITMAP_WORDS * sizeof(uint32_t), VFS_BITMAP_OFFSET);
}

static inline vfs_status_t bitmap_read_locked(vfs_t* vfs) {
    return pread_all(vfs->fd, vfs->bitmap, VFS_BITMAP_WORDS * sizeof(uint32_t), VFS_BITMAP_OFFSET);
}

static inline vfs_status_t inode_write_locked(vfs_t* vfs, uint32_t idx) {
    assert(idx < VFS_MAX_INODES);
    off_t off = VFS_INODE_TABLE_OFFSET + (off_t)idx * (off_t)VFS_INODE_ON_DISK_SIZE;
    return pwrite_all(vfs->fd, &vfs->inodes[idx], sizeof(vfs_inode_t), off);
}

/** Marks inode @p idx dirty and force-flushes the batch if the threshold is hit. */
static vfs_status_t inode_mark_dirty_locked(vfs_t* vfs, uint32_t idx) {
    if (!vfs->inode_dirty[idx]) {
        vfs->inode_dirty[idx] = true;
        vfs->dirty_inode_count++;
    }
    if (vfs->dirty_inode_count < VFS_DIRTY_INODE_FLUSH_THRESHOLD) {
        return VFS_OK;
    }

    /* Batch threshold crossed: flush all dirty inodes now, still under
     * meta_lock but this is metadata-only I/O, not on the data path. */
    vfs_status_t s = VFS_OK;
    for (uint32_t i = 0; i < VFS_MAX_INODES; i++) {
        if (vfs->inode_dirty[i]) {
            vfs_status_t ws = inode_write_locked(vfs, i);
            if (ws != VFS_OK) {
                s = ws;
                continue;
            }
            vfs->inode_dirty[i] = false;
        }
    }
    vfs->dirty_inode_count = 0;
    return s;
}

static vfs_status_t overflow_cache_flush_locked(vfs_t* vfs) {
    if (vfs->overflow_cache.dirty && vfs->overflow_cache.cached_block != 0) {
        vfs_status_t s =
            pwrite_all(vfs->fd, vfs->overflow_cache.cached_extents, sizeof(vfs->overflow_cache.cached_extents),
                       block_offset(vfs->overflow_cache.cached_block));
        if (s != VFS_OK) {
            return s;
        }
        vfs->overflow_cache.dirty = false;
    }
    return VFS_OK;
}

static vfs_status_t overflow_cache_read_locked(vfs_t* vfs, uint32_t blk, vfs_extent_t** out_table) {
    if (vfs->overflow_cache.cached_block == blk) {
        *out_table = vfs->overflow_cache.cached_extents;
        return VFS_OK;
    }
    vfs_status_t s = overflow_cache_flush_locked(vfs);
    if (s != VFS_OK) {
        return s;
    }
    s = pread_all(vfs->fd, vfs->overflow_cache.cached_extents, sizeof(vfs->overflow_cache.cached_extents),
                  block_offset(blk));
    if (s != VFS_OK) {
        return s;
    }
    vfs->overflow_cache.cached_block = blk;
    vfs->overflow_cache.dirty = false;
    *out_table = vfs->overflow_cache.cached_extents;
    return VFS_OK;
}

/** Flushes bitmap to disk if dirty. Caller holds meta_lock. */
static vfs_status_t flush_bitmap_locked(vfs_t* vfs) {
    if (vfs->bitmap_dirty) {
        vfs_status_t s = bitmap_write_locked(vfs);
        if (s == VFS_OK) {
            vfs->bitmap_dirty = false;
        }
        return s;
    }
    return VFS_OK;
}

/** Flushes every dirty inode unconditionally. Caller holds meta_lock. */
static vfs_status_t flush_all_dirty_inodes_locked(vfs_t* vfs) {
    vfs_status_t s = VFS_OK;
    for (uint32_t i = 0; i < VFS_MAX_INODES; i++) {
        if (vfs->inode_dirty[i]) {
            vfs_status_t ws = inode_write_locked(vfs, i);
            if (ws != VFS_OK) {
                s = ws;
                continue;
            }
            vfs->inode_dirty[i] = false;
        }
    }
    vfs->dirty_inode_count = 0;
    return s;
}

/* =========================================================================
 * Bitmap bit helpers (caller holds meta_lock)
 * ======================================================================= */

static inline bool bitmap_is_free(const vfs_t* vfs, uint32_t blk) {
    assert(blk < VFS_TOTAL_BLOCKS);
    return (vfs->bitmap[blk / 32u] & (UINT32_C(1) << (blk % 32u))) != 0;
}

static inline void bitmap_set_used(vfs_t* vfs, uint32_t blk) {
    assert(blk < VFS_TOTAL_BLOCKS);
    vfs->bitmap[blk / 32u] &= ~(UINT32_C(1) << (blk % 32u));
    vfs->bitmap_dirty = true;
}

static inline void bitmap_set_free(vfs_t* vfs, uint32_t blk) {
    assert(blk < VFS_TOTAL_BLOCKS);
    vfs->bitmap[blk / 32u] |= (UINT32_C(1) << (blk % 32u));
    vfs->bitmap_dirty = true;
}

static inline void bitmap_mark_range_used(vfs_t* vfs, uint32_t start, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        bitmap_set_used(vfs, start + i);
    }
}

static inline void bitmap_mark_range_free(vfs_t* vfs, uint32_t start, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        bitmap_set_free(vfs, start + i);
    }
}

/* =========================================================================
 * Free-extent allocator (caller holds meta_lock)
 *
 * A sorted array of (start, len) free runs, kept coalesced. Sequential
 * writes get large contiguous physical runs from this list instead of a
 * bit-by-bit bitmap scan, turning allocation into an O(log n) binary
 * search plus O(n) shift on insert/remove (n is small in practice: the
 * free list stays compact because adjacent frees are merged immediately).
 * The bitmap remains the durable source of truth; the free list is a
 * pure in-memory acceleration structure rebuilt from the bitmap on open.
 * ======================================================================= */

/** Ensures capacity for at least one more free-extent entry. */
static inline bool free_extents_reserve(vfs_t* vfs, uint32_t min_cap) {
    if (vfs->free_extent_cap >= min_cap) {
        return true;
    }
    uint32_t new_cap = vfs->free_extent_cap == 0 ? 64u : vfs->free_extent_cap * 2u;
    while (new_cap < min_cap) {
        new_cap *= 2u;
    }
    free_extent_t* grown = realloc(vfs->free_extents, (size_t)new_cap * sizeof(free_extent_t));
    if (grown == NULL) {
        return false;
    }
    vfs->free_extents = grown;
    vfs->free_extent_cap = new_cap;
    return true;
}

/** Binary search for the first free extent with start >= @p key. */
static inline uint32_t free_extents_lower_bound(const vfs_t* vfs, uint32_t key) {
    uint32_t lo = 0, hi = vfs->free_extent_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (vfs->free_extents[mid].start < key) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/** Inserts (start,len) into the sorted free list, coalescing with neighbors. */
static bool free_extents_insert(vfs_t* vfs, uint32_t start, uint32_t len) {
    if (len == 0) {
        return true;
    }
    uint32_t pos = free_extents_lower_bound(vfs, start);

    /* Try merging with the previous entry. */
    bool merged_left = false;
    if (pos > 0) {
        free_extent_t* prev = &vfs->free_extents[pos - 1u];
        if (prev->start + prev->len == start) {
            prev->len += len;
            merged_left = true;
            pos -= 1u;
        }
    }
    /* Try merging with the next entry into the (possibly just-extended) left one. */
    if (pos < vfs->free_extent_count) {
        free_extent_t* cur = &vfs->free_extents[pos + (merged_left ? 0u : 0u)];
        free_extent_t* target = merged_left ? &vfs->free_extents[pos] : NULL;
        uint32_t seg_start = merged_left ? target->start : start;
        uint32_t seg_len = merged_left ? target->len : len;
        (void)cur;
        if (pos < vfs->free_extent_count) {
            free_extent_t* right = &vfs->free_extents[merged_left ? pos + 1u : pos];
            if (merged_left) {
                if (pos + 1u < vfs->free_extent_count && seg_start + seg_len == right->start) {
                    target->len += right->len;
                    memmove(&vfs->free_extents[pos + 1u], &vfs->free_extents[pos + 2u],
                            (size_t)(vfs->free_extent_count - pos - 2u) * sizeof(free_extent_t));
                    vfs->free_extent_count--;
                }
                return true;
            } else {
                if (seg_start + seg_len == right->start) {
                    right->start = seg_start;
                    right->len += seg_len;
                    return true;
                }
            }
        }
    }
    if (merged_left) {
        return true;
    }

    /* No adjacency: insert a fresh entry at `pos`. */
    if (!free_extents_reserve(vfs, vfs->free_extent_count + 1u)) {
        return false;
    }

    memmove(&vfs->free_extents[pos + 1u], &vfs->free_extents[pos],
            (size_t)(vfs->free_extent_count - pos) * sizeof(free_extent_t));

    vfs->free_extents[pos].start = start;
    vfs->free_extents[pos].len = len;
    vfs->free_extent_count++;
    return true;
}

/** Removes/shrinks a run of @p len blocks starting at @p start from the free list. */
static void free_extents_remove(vfs_t* vfs, uint32_t start, uint32_t len) {
    uint32_t pos = free_extents_lower_bound(vfs, start + 1u);
    if (pos == 0) {
        return;
    }

    free_extent_t* e = &vfs->free_extents[pos - 1u];
    if (e->start > start || e->start + e->len < start + len) {
        /* Should not happen if the caller only removes from known-free
         * space, but stay defensive rather than corrupt the free list. */
        return;
    }

    uint32_t left_len = start - e->start;
    uint32_t right_start = start + len;
    uint32_t right_len = (e->start + e->len) - right_start;

    if (left_len == 0 && right_len == 0) {
        /* Whole entry consumed. */
        memmove(&vfs->free_extents[pos - 1u], &vfs->free_extents[pos],
                (size_t)(vfs->free_extent_count - pos) * sizeof(free_extent_t));
        vfs->free_extent_count--;
    } else if (left_len > 0 && right_len == 0) {
        e->len = left_len;
    } else if (left_len == 0 && right_len > 0) {
        e->start = right_start;
        e->len = right_len;
    } else {
        /* Split into two entries. */
        e->len = left_len;
        if (!free_extents_reserve(vfs, vfs->free_extent_count + 1u)) {
            /* Fall back: drop the right remainder rather than corrupt
             * state; it will be reclaimed as a leak-free but suboptimal
             * bitmap-only region until the next full rebuild on mount. */
            return;
        }

        memmove(&vfs->free_extents[pos + 1u], &vfs->free_extents[pos],
                (size_t)(vfs->free_extent_count - pos) * sizeof(free_extent_t));

        vfs->free_extents[pos].start = right_start;
        vfs->free_extents[pos].len = right_len;
        vfs->free_extent_count++;
    }
}

/** Rebuilds the free-extent list from the durable bitmap. Caller holds meta_lock. */
static bool free_extents_rebuild_locked(vfs_t* vfs) {
    vfs->free_extent_count = 0;
    uint32_t run_start = 0;
    bool in_run = false;

    for (uint32_t blk = 0; blk < VFS_TOTAL_BLOCKS; blk++) {
        bool free_bit = bitmap_is_free(vfs, blk);
        if (free_bit && !in_run) {
            run_start = blk;
            in_run = true;
        } else if (!free_bit && in_run) {
            if (!free_extents_reserve(vfs, vfs->free_extent_count + 1u)) {
                return false;
            }
            vfs->free_extents[vfs->free_extent_count].start = run_start;
            vfs->free_extents[vfs->free_extent_count].len = blk - run_start;
            vfs->free_extent_count++;
            in_run = false;
        }
    }

    if (in_run) {
        if (!free_extents_reserve(vfs, vfs->free_extent_count + 1u)) {
            return false;
        }
        vfs->free_extents[vfs->free_extent_count].start = run_start;
        vfs->free_extents[vfs->free_extent_count].len = VFS_TOTAL_BLOCKS - run_start;
        vfs->free_extent_count++;
    }
    return true;
}

/**
 * Allocates a run of up to @p preferred_len contiguous free blocks.
 * Prefers extending immediately after last_allocated_block; otherwise
 * picks the best-fit free extent (smallest one that satisfies the full
 * request, else the largest available). Caller holds meta_lock.
 *
 * @return VFS_OK with (*out_blk, *out_len), or VFS_ERR_NOSPACE.
 */
static vfs_status_t block_alloc_run_locked(vfs_t* vfs, uint32_t preferred_len, uint32_t* out_blk, uint32_t* out_len) {
    if (preferred_len == 0) {
        *out_blk = 0;
        *out_len = 0;
        return VFS_OK;
    }

    /* Fast path: does the free list contain a run starting exactly at
     * last_allocated_block + 1? Sequential writers hit this every time. */
    uint32_t want_start = vfs->last_allocated_block + 1u;
    if (want_start > 0 && want_start < VFS_TOTAL_BLOCKS) {
        uint32_t pos = free_extents_lower_bound(vfs, want_start);
        if (pos < vfs->free_extent_count && vfs->free_extents[pos].start == want_start) {
            uint32_t avail = vfs->free_extents[pos].len;
            uint32_t take = (avail < preferred_len) ? avail : preferred_len;
            free_extents_remove(vfs, want_start, take);
            bitmap_mark_range_used(vfs, want_start, take);
            vfs->last_allocated_block = want_start + take - 1u;
            if (vfs->super.free_block_count >= take) {
                vfs->super.free_block_count -= take;
            } else {
                vfs->super.free_block_count = 0;
            }
            *out_blk = want_start;
            *out_len = take;
            return VFS_OK;
        }
    }

    /* Best-fit scan: smallest extent that can satisfy the whole request;
     * fall back to the single largest extent if none is big enough. */
    uint32_t best_idx = UINT32_MAX;
    uint32_t best_len_for_fit = UINT32_MAX;
    uint32_t largest_idx = UINT32_MAX;
    uint32_t largest_len = 0;

    for (uint32_t i = 0; i < vfs->free_extent_count; i++) {
        uint32_t len = vfs->free_extents[i].len;
        if (len >= preferred_len && len < best_len_for_fit) {
            best_len_for_fit = len;
            best_idx = i;
        }
        if (len > largest_len) {
            largest_len = len;
            largest_idx = i;
        }
    }

    uint32_t chosen_idx = (best_idx != UINT32_MAX) ? best_idx : largest_idx;
    if (chosen_idx == UINT32_MAX) {
        return VFS_ERR_NOSPACE;
    }

    uint32_t start = vfs->free_extents[chosen_idx].start;
    uint32_t avail = vfs->free_extents[chosen_idx].len;
    uint32_t take = (avail < preferred_len) ? avail : preferred_len;

    free_extents_remove(vfs, start, take);
    bitmap_mark_range_used(vfs, start, take);
    vfs->last_allocated_block = start + take - 1u;
    if (vfs->super.free_block_count >= take) {
        vfs->super.free_block_count -= take;
    } else {
        vfs->super.free_block_count = 0;
    }

    *out_blk = start;
    *out_len = take;
    return VFS_OK;
}

static vfs_status_t block_alloc_one_locked(vfs_t* vfs, uint32_t* out_blk) {
    uint32_t dummy_len;
    return block_alloc_run_locked(vfs, 1u, out_blk, &dummy_len);
}

/** Frees a contiguous run of @p len physical blocks. Caller holds meta_lock. */
static void block_free_run_locked(vfs_t* vfs, uint32_t start, uint32_t len) {
    if (len == 0) {
        return;
    }
    bitmap_mark_range_free(vfs, start, len);
    vfs->super.free_block_count += len;
    (void)free_extents_insert(vfs, start, len);

    if (vfs->overflow_cache.cached_block >= start && vfs->overflow_cache.cached_block < start + len) {
        vfs->overflow_cache.cached_block = 0;
        vfs->overflow_cache.dirty = false;
    }
}

/* =========================================================================
 * Extent-map operations
 *
 * Every function here operates on one inode's extent array (inline plus
 * an optional overflow block). The caller must hold that inode's
 * per-inode write lock for mutation, or at least a read lock for
 * lookups. Allocation/free of physical blocks additionally requires
 * meta_lock, acquired internally by these functions only for the
 * duration of the allocator call -- never while doing host data I/O.
 * ======================================================================= */

/** Returns a pointer to all of an inode's extents by copying into @p out,
 * along with the total extent count. Reads the overflow block if present.
 * Takes meta_lock internally (short critical section, metadata-only). */
static vfs_status_t extents_load(vfs_t* vfs, uint32_t inode_idx, vfs_extent_t* out, uint32_t out_cap,
                                 uint32_t* out_count) {
    vfs_inode_t* in = &vfs->inodes[inode_idx];
    uint32_t inline_n = in->inline_extent_count;
    uint32_t total = in->extent_count;

    if (total > out_cap) {
        return VFS_ERR_OVERFLOW;
    }

    memcpy(out, in->extents, (size_t)inline_n * sizeof(vfs_extent_t));

    uint32_t remaining = total - inline_n;
    if (remaining > 0) {
        pthread_rwlock_rdlock(&vfs->meta_lock);
        vfs_extent_t* overflow_table = NULL;
        vfs_status_t s = overflow_cache_read_locked(vfs, in->overflow_block, &overflow_table);
        if (s == VFS_OK) {
            memcpy(out + inline_n, overflow_table, (size_t)remaining * sizeof(vfs_extent_t));
        }
        pthread_rwlock_unlock(&vfs->meta_lock);
        if (s != VFS_OK) {
            return s;
        }
    }

    *out_count = total;
    return VFS_OK;
}

/**
 * Binary search for the extent covering (or immediately following)
 * @p logical_block within a locally-loaded, sorted extent array.
 * @return Index of the extent whose [logical_block, logical_block+length)
 *         range contains @p logical_block, or the index of the first
 *         extent with a greater logical_block if none contains it, or
 *         @p count if @p logical_block is past every extent.
 */
static uint32_t extents_find(const vfs_extent_t* extents, uint32_t count, uint32_t logical_block) {
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        const vfs_extent_t* e = &extents[mid];
        if (logical_block < e->logical_block) {
            hi = mid;
        } else if (logical_block >= e->logical_block + e->length) {
            lo = mid + 1u;
        } else {
            return mid; /* Contains logical_block. */
        }
    }
    return lo;
}

/**
 * Persists a (possibly grown) extent array back to the inode's inline
 * slots plus an overflow block, allocating the overflow block on first
 * need. Caller holds the inode's write lock; this function takes
 * meta_lock internally for the metadata-only writes.
 */
static vfs_status_t extents_store(vfs_t* vfs, uint32_t inode_idx, const vfs_extent_t* extents, uint32_t count) {
    vfs_inode_t* in = &vfs->inodes[inode_idx];

    uint32_t inline_n = (count < VFS_MAX_INLINE_EXTENTS) ? count : VFS_MAX_INLINE_EXTENTS;
    uint32_t overflow_n = count - inline_n;

    memcpy(in->extents, extents, (size_t)inline_n * sizeof(vfs_extent_t));
    in->inline_extent_count = inline_n;
    in->extent_count = count;

    if (overflow_n > VFS_EXTENTS_PER_OVERFLOW_BLOCK) {
        return VFS_ERR_OVERFLOW; /* Single overflow block capacity exceeded. */
    }

    pthread_rwlock_wrlock(&vfs->meta_lock);
    vfs_status_t s = VFS_OK;
    if (overflow_n > 0) {
        if (in->overflow_block == 0) {
            uint32_t blk = 0;
            s = block_alloc_one_locked(vfs, &blk);
            if (s == VFS_OK) {
                in->overflow_block = blk;
            }
        }
        if (s == VFS_OK) {
            vfs_extent_t table[VFS_EXTENTS_PER_OVERFLOW_BLOCK];
            memset(table, 0, sizeof(table));
            memcpy(table, extents + inline_n, (size_t)overflow_n * sizeof(vfs_extent_t));
            if (vfs->overflow_cache.cached_block == in->overflow_block) {
                memcpy(vfs->overflow_cache.cached_extents, table, sizeof(table));
                vfs->overflow_cache.dirty = true;
            } else {
                s = overflow_cache_flush_locked(vfs);
                if (s == VFS_OK) {
                    memcpy(vfs->overflow_cache.cached_extents, table, sizeof(table));
                    vfs->overflow_cache.cached_block = in->overflow_block;
                    vfs->overflow_cache.dirty = true;
                }
            }
        }
    } else if (in->overflow_block != 0) {
        /* No longer needed: free it. */
        block_free_run_locked(vfs, in->overflow_block, 1u);
        in->overflow_block = 0;
    }
    if (s == VFS_OK) {
        s = inode_mark_dirty_locked(vfs, inode_idx);
    }
    pthread_rwlock_unlock(&vfs->meta_lock);
    return s;
}

/**
 * Inserts a new (logical, physical, length) run into a sorted extent
 * array held in a local scratch buffer, merging with an adjacent extent
 * when the new run is physically and logically contiguous with it.
 * Overwrites any existing coverage of the same logical range (used only
 * for fresh allocations, which never overlap existing extents).
 */
static uint32_t extents_insert_local(vfs_extent_t* extents, uint32_t count, uint32_t cap, uint32_t logical,
                                     uint32_t physical, uint32_t length) {
    uint32_t pos = extents_find(extents, count, logical);

    /* Merge with previous extent if contiguous both logically and physically. */
    if (pos > 0) {
        vfs_extent_t* prev = &extents[pos - 1u];
        if (prev->logical_block + prev->length == logical && prev->physical_block != 0 &&
            prev->physical_block + prev->length == physical) {
            prev->length += length;
            /* Try merging forward into the next extent too. */
            if (pos < count) {
                vfs_extent_t* next = &extents[pos];
                if (prev->logical_block + prev->length == next->logical_block &&
                    prev->physical_block + prev->length == next->physical_block) {
                    prev->length += next->length;
                    memmove(&extents[pos], &extents[pos + 1u], (size_t)(count - pos - 1u) * sizeof(vfs_extent_t));
                    return count - 1u;
                }
            }
            return count;
        }
    }
    /* Merge with next extent only. */
    if (pos < count) {
        vfs_extent_t* next = &extents[pos];
        if (logical + length == next->logical_block && physical != 0 && physical + length == next->physical_block) {
            next->logical_block = logical;
            next->physical_block = physical;
            next->length += length;
            return count;
        }
    }

    /* No merge: insert fresh. */
    if (count >= cap) {
        return UINT32_MAX; /* Signal overflow to caller. */
    }
    memmove(&extents[pos + 1u], &extents[pos], (size_t)(count - pos) * sizeof(vfs_extent_t));
    extents[pos].logical_block = logical;
    extents[pos].physical_block = physical;
    extents[pos].length = length;
    return count + 1u;
}

/**
 * Resolves the longest contiguous physical run available starting at
 * @p logical_block, up to @p max_blocks logical blocks, from an
 * already-loaded extent array. Does not allocate.
 *
 * @param[out] physical_start  0 if the range is an unmapped hole.
 * @param[out] run_length      Length of the contiguous run found
 *                              (allocated or hole), capped at max_blocks.
 */
static void extents_resolve_read(const vfs_extent_t* extents, uint32_t count, uint32_t logical_block,
                                 uint32_t max_blocks, uint32_t* physical_start, uint32_t* run_length) {
    uint32_t idx = extents_find(extents, count, logical_block);

    if (idx < count && extents[idx].logical_block <= logical_block &&
        logical_block < extents[idx].logical_block + extents[idx].length) {
        const vfs_extent_t* e = &extents[idx];
        uint32_t skip = logical_block - e->logical_block;
        uint32_t avail = e->length - skip;
        uint32_t take = (avail < max_blocks) ? avail : max_blocks;
        *physical_start = (e->physical_block == 0) ? 0 : (e->physical_block + skip);
        *run_length = take;
        return;
    }

    /* Hole: extends until the next extent's start, or max_blocks. */
    uint32_t next_start = (idx < count) ? extents[idx].logical_block : (logical_block + max_blocks);
    uint32_t hole_len = next_start - logical_block;
    *physical_start = 0;
    *run_length = (hole_len < max_blocks) ? hole_len : max_blocks;
    if (*run_length == 0) {
        *run_length = 1; /* Always make forward progress. */
    }
}

/* =========================================================================
 * Inode helpers (caller holds meta_lock)
 * ======================================================================= */

static uint32_t inode_find_locked(const vfs_t* vfs, const char* path) {
    for (uint32_t i = 0; i < VFS_MAX_INODES; i++) {
        if (vfs->inodes[i].path[0] != '\0' && strncmp(vfs->inodes[i].path, path, VFS_MAX_PATH - 1u) == 0) {
            return i;
        }
    }
    return INODE_NONE;
}

static uint32_t inode_alloc_slot_locked(const vfs_t* vfs) {
    for (uint32_t i = 0; i < VFS_MAX_INODES; i++) {
        if (vfs->inodes[i].path[0] == '\0') {
            return i;
        }
    }
    return INODE_NONE;
}

/**
 * Frees every physical block (data + overflow extent block) owned by
 * inode @p idx and zeroes the inode entry. Caller holds meta_lock AND
 * the inode's write lock (via the public API's locking order).
 */
static vfs_status_t inode_free_all_blocks_locked(vfs_t* vfs, uint32_t idx) {
    vfs_inode_t* in = &vfs->inodes[idx];

    for (uint32_t i = 0; i < in->inline_extent_count; i++) {
        if (in->extents[i].physical_block != 0) {
            block_free_run_locked(vfs, in->extents[i].physical_block, in->extents[i].length);
        }
    }

    if (in->overflow_block != 0) {
        vfs_extent_t* table = NULL;
        vfs_status_t s = overflow_cache_read_locked(vfs, in->overflow_block, &table);
        if (s == VFS_OK) {
            uint32_t overflow_n = in->extent_count - in->inline_extent_count;
            for (uint32_t i = 0; i < overflow_n; i++) {
                if (table[i].physical_block != 0) {
                    block_free_run_locked(vfs, table[i].physical_block, table[i].length);
                }
            }
        }
        block_free_run_locked(vfs, in->overflow_block, 1u);
    }

    vfs->super.free_inode_count++;
    memset(in, 0, sizeof(*in));
    vfs->inode_dirty[idx] = false;
    return inode_write_locked(vfs, idx);
}

/* =========================================================================
 * Open-file table helpers (caller holds meta_lock)
 * ======================================================================= */

static open_file_t* oft_get_locked(vfs_t* vfs, vfs_fd_t fd) {
    if (fd < 0 || (unsigned int)fd >= VFS_MAX_OPEN_FILES) {
        return NULL;
    }
    open_file_t* of = &vfs->oft[(unsigned int)fd];
    if (of->inode_idx == OFT_FREE) {
        return NULL;
    }
    return of;
}

static void oft_init(vfs_t* vfs) {
    for (uint32_t i = 0; i < VFS_MAX_OPEN_FILES; i++) {
        vfs->oft[i].inode_idx = OFT_FREE;
        vfs->oft[i].pos = 0;
        vfs->oft[i].flags = 0;
    }
}

/* =========================================================================
 * Mount helpers
 * ======================================================================= */

static vfs_t* vfs_alloc(void) {
    vfs_t* v = calloc(1, sizeof(*v));
    if (v == NULL) {
        return NULL;
    }
    v->fd = -1;

    v->bitmap = calloc(VFS_BITMAP_WORDS, sizeof(uint32_t));
    if (v->bitmap == NULL) {
        free(v);
        return NULL;
    }

    if (pthread_rwlock_init(&v->meta_lock, NULL) != 0) {
        free(v->bitmap);
        free(v);
        return NULL;
    }
    for (uint32_t i = 0; i < VFS_MAX_INODES; i++) {
        if (pthread_rwlock_init(&v->inode_locks[i], NULL) != 0) {
            for (uint32_t j = 0; j < i; j++) {
                pthread_rwlock_destroy(&v->inode_locks[j]);
            }
            pthread_rwlock_destroy(&v->meta_lock);
            free(v->bitmap);
            free(v);
            return NULL;
        }
    }

    oft_init(v);
    return v;
}

static void vfs_free_all(vfs_t* v) {
    if (v == NULL) {
        return;
    }
    for (uint32_t i = 0; i < VFS_MAX_INODES; i++) {
        pthread_rwlock_destroy(&v->inode_locks[i]);
    }
    pthread_rwlock_destroy(&v->meta_lock);
    free(v->bitmap);
    free(v->free_extents);
    free(v);
}

/* =========================================================================
 * Public API - filesystem lifecycle
 * ======================================================================= */

vfs_status_t vfs_create(const char* image_path, vfs_t** out_vfs) {
    if (image_path == NULL || out_vfs == NULL) {
        return VFS_ERR_INVAL;
    }

    vfs_t* vfs = vfs_alloc();
    if (vfs == NULL) {
        return VFS_ERR_NOMEM;
    }

    vfs->fd = open(image_path, O_RDWR | O_CREAT | O_TRUNC, (mode_t)0600);
    if (vfs->fd < 0) {
        vfs_free_all(vfs);
        return VFS_ERR_IO;
    }
    vfs->readonly = false;

    memset(vfs->bitmap, 0xFF, VFS_BITMAP_WORDS * sizeof(uint32_t));
    bitmap_set_used(vfs, 0u); /* Block 0 is the permanent "no block" sentinel. */
    vfs->bitmap_dirty = false;

    if (!free_extents_reserve(vfs, 2u)) {
        vfs_close(vfs);
        return VFS_ERR_NOMEM;
    }
    vfs->free_extents[0].start = 1u;
    vfs->free_extents[0].len = VFS_TOTAL_BLOCKS - 1u;
    vfs->free_extent_count = 1;

    vfs->super = (vfs_super_t){
        .magic = VFS_MAGIC,
        .version = VFS_VERSION,
        .block_size = VFS_BLOCK_SIZE,
        .max_inodes = VFS_MAX_INODES,
        .total_blocks = VFS_TOTAL_BLOCKS,
        .free_block_count = VFS_TOTAL_BLOCKS - 1u,
        .free_inode_count = VFS_MAX_INODES,
        .bitmap_words = VFS_BITMAP_WORDS,
    };

    vfs_status_t s = pwrite_all(vfs->fd, &vfs->super, sizeof(vfs->super), (off_t)0);
    if (s != VFS_OK) {
        goto io_error;
    }
    {
        size_t pad = VFS_SUPERBLOCK_SIZE - sizeof(vfs->super);
        if (pad > 0) {
            uint8_t* zeros = calloc(1, pad);
            if (zeros == NULL) {
                vfs_close(vfs);
                return VFS_ERR_NOMEM;
            }
            s = pwrite_all(vfs->fd, zeros, pad, (off_t)sizeof(vfs->super));
            free(zeros);
            if (s != VFS_OK) {
                goto io_error;
            }
        }
    }

    s = bitmap_write_locked(vfs);
    if (s != VFS_OK) {
        goto io_error;
    }

    s = pwrite_all(vfs->fd, vfs->inodes, sizeof(vfs->inodes), VFS_INODE_TABLE_OFFSET);
    if (s != VFS_OK) {
        goto io_error;
    }

    *out_vfs = vfs;
    return VFS_OK;

io_error:
    vfs_close(vfs);
    return VFS_ERR_IO;
}

vfs_status_t vfs_open(const char* image_path, bool readonly, vfs_t** out_vfs) {
    if (image_path == NULL || out_vfs == NULL) {
        return VFS_ERR_INVAL;
    }

    vfs_t* vfs = vfs_alloc();
    if (vfs == NULL) {
        return VFS_ERR_NOMEM;
    }

    int oflags = readonly ? O_RDONLY : O_RDWR;
    vfs->fd = open(image_path, oflags, (mode_t)0);
    if (vfs->fd < 0) {
        vfs_free_all(vfs);
        return VFS_ERR_IO;
    }
    vfs->readonly = readonly;

    vfs_status_t s = pread_all(vfs->fd, &vfs->super, sizeof(vfs->super), (off_t)0);
    if (s != VFS_OK) {
        goto io_error;
    }

    struct {
        uint32_t disk;
        uint32_t expected;
        const char* field;
    } checks[] = {
        {vfs->super.magic, VFS_MAGIC, "magic"},
        {vfs->super.version, VFS_VERSION, "version"},
        {vfs->super.block_size, VFS_BLOCK_SIZE, "block_size"},
        {vfs->super.max_inodes, VFS_MAX_INODES, "max_inodes"},
        {vfs->super.total_blocks, VFS_TOTAL_BLOCKS, "total_blocks"},
        {vfs->super.bitmap_words, VFS_BITMAP_WORDS, "bitmap_words"},
    };
    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
        if (checks[i].disk != checks[i].expected) {
            fprintf(stderr, "VFS corrupt: field '%s' on disk is 0x%08X, expected 0x%08X\n", checks[i].field,
                    checks[i].disk, checks[i].expected);
            close(vfs->fd);
            vfs->fd = -1;
            vfs_free_all(vfs);
            return VFS_ERR_CORRUPT;
        }
    }

    s = bitmap_read_locked(vfs);
    if (s != VFS_OK) {
        goto io_error;
    }
    vfs->bitmap_dirty = false;

    if (!free_extents_rebuild_locked(vfs)) {
        close(vfs->fd);
        vfs->fd = -1;
        vfs_free_all(vfs);
        return VFS_ERR_NOMEM;
    }

    s = pread_all(vfs->fd, vfs->inodes, sizeof(vfs->inodes), VFS_INODE_TABLE_OFFSET);
    if (s != VFS_OK) {
        goto io_error;
    }

    *out_vfs = vfs;
    return VFS_OK;

io_error:
    close(vfs->fd);
    vfs->fd = -1;
    vfs_free_all(vfs);
    return VFS_ERR_IO;
}

void vfs_close(vfs_t* vfs) {
    if (vfs == NULL) {
        return;
    }

    if (!vfs->readonly && vfs->fd >= 0) {
        pthread_rwlock_wrlock(&vfs->meta_lock);
        (void)overflow_cache_flush_locked(vfs);
        (void)super_write_locked(vfs);
        (void)flush_bitmap_locked(vfs);
        (void)flush_all_dirty_inodes_locked(vfs);
        pthread_rwlock_unlock(&vfs->meta_lock);
    }

    if (vfs->fd >= 0) {
        (void)close(vfs->fd);
        vfs->fd = -1;
    }

    vfs_free_all(vfs);
}

vfs_status_t vfs_sync(vfs_t* vfs) {
    if (vfs == NULL) {
        return VFS_ERR_INVAL;
    }
    if (vfs->readonly) {
        return VFS_OK;
    }

    pthread_rwlock_wrlock(&vfs->meta_lock);
    vfs_status_t s = overflow_cache_flush_locked(vfs);
    if (s == VFS_OK) {
        s = super_write_locked(vfs);
    }
    if (s == VFS_OK) {
        s = flush_bitmap_locked(vfs);
    }
    if (s == VFS_OK) {
        s = flush_all_dirty_inodes_locked(vfs);
    }
    pthread_rwlock_unlock(&vfs->meta_lock);
    return s;
}

/* =========================================================================
 * Public API - file operations
 * ======================================================================= */

vfs_fd_t vfs_fopen(vfs_t* vfs, const char* path, unsigned int flags) {
    if (vfs == NULL || path == NULL || path[0] == '\0') {
        return (vfs_fd_t)VFS_ERR_INVAL;
    }
    if (vfs->readonly && (flags & (VFS_O_WRONLY | VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC))) {
        return (vfs_fd_t)VFS_ERR_READONLY;
    }

    pthread_rwlock_wrlock(&vfs->meta_lock);

    vfs_status_t rc;
    uint32_t inode_idx = inode_find_locked(vfs, path);

    if (flags & VFS_O_CREAT) {
        if (inode_idx != INODE_NONE) {
            if (flags & VFS_O_EXCL) {
                rc = VFS_ERR_EXISTS;
                goto out;
            }
        } else {
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

            if (vfs->super.free_inode_count > 0) {
                vfs->super.free_inode_count--;
            }

            rc = inode_write_locked(vfs, inode_idx);
            if (rc != VFS_OK) {
                memset(in, 0, sizeof(*in));
                goto out;
            }
        }
    } else if (inode_idx == INODE_NONE) {
        rc = VFS_ERR_NOTFOUND;
        goto out;
    }

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

    if ((flags & VFS_O_TRUNC) && (flags & (VFS_O_WRONLY | VFS_O_RDWR))) {
        pthread_rwlock_wrlock(&vfs->inode_locks[inode_idx]);
        vfs_status_t s = inode_free_all_blocks_locked(vfs, inode_idx);
        pthread_rwlock_unlock(&vfs->inode_locks[inode_idx]);
        if (s != VFS_OK) {
            rc = s;
            goto out;
        }
        /* inode_free_all_blocks_locked zeroed the whole inode; restore path. */
        vfs_inode_t* in = &vfs->inodes[inode_idx];
        strncpy(in->path, path, VFS_MAX_PATH - 1u);
        in->path[VFS_MAX_PATH - 1u] = '\0';
        in->created_at = (uint64_t)time(NULL);
        in->modified_at = in->created_at;
        if (vfs->super.free_inode_count > 0) {
            vfs->super.free_inode_count--;
        }
        rc = inode_write_locked(vfs, inode_idx);
        if (rc != VFS_OK) {
            goto out;
        }
    }

    vfs->oft[(unsigned int)fd].inode_idx = (int)inode_idx;
    vfs->oft[(unsigned int)fd].flags = flags;
    vfs->oft[(unsigned int)fd].pos = (flags & VFS_O_APPEND) ? (off_t)vfs->inodes[inode_idx].size : (off_t)0;

    rc = (vfs_status_t)fd;

out:
    pthread_rwlock_unlock(&vfs->meta_lock);
    return (vfs_fd_t)rc;
}

vfs_status_t vfs_fclose(vfs_t* vfs, vfs_fd_t fd) {
    if (vfs == NULL) {
        return VFS_ERR_INVAL;
    }

    pthread_rwlock_wrlock(&vfs->meta_lock);
    open_file_t* of = oft_get_locked(vfs, fd);
    if (of == NULL) {
        pthread_rwlock_unlock(&vfs->meta_lock);
        return VFS_ERR_BADFD;
    }
    of->inode_idx = OFT_FREE;
    of->pos = 0;
    of->flags = 0;
    /* Deliberately not flushing metadata here: writeback is batched via
     * inode_mark_dirty_locked's threshold and vfs_sync()/vfs_close(). */
    pthread_rwlock_unlock(&vfs->meta_lock);
    return VFS_OK;
}

/**
 * Shared implementation for the read-side resolve step: takes the
 * inode's read lock, loads its extent array, releases the lock, and
 * hands back a snapshot the caller can safely use for host I/O without
 * holding any VFS lock.
 */
static vfs_status_t snapshot_extents_for_read(vfs_t* vfs, uint32_t inode_idx, vfs_extent_t* scratch,
                                              uint32_t scratch_cap, uint32_t* out_count, uint64_t* out_size) {
    pthread_rwlock_rdlock(&vfs->inode_locks[inode_idx]);
    *out_size = vfs->inodes[inode_idx].size;
    vfs_status_t s = extents_load(vfs, inode_idx, scratch, scratch_cap, out_count);
    pthread_rwlock_unlock(&vfs->inode_locks[inode_idx]);
    return s;
}

vfs_status_t vfs_fread(vfs_t* vfs, vfs_fd_t fd, void* buf, size_t count, size_t* bytes_read) {
    if (vfs == NULL || buf == NULL || bytes_read == NULL) {
        return VFS_ERR_INVAL;
    }
    *bytes_read = 0;
    if (count == 0) {
        return VFS_OK;
    }

    pthread_rwlock_rdlock(&vfs->meta_lock);
    open_file_t* of = oft_get_locked(vfs, fd);
    if (of == NULL) {
        pthread_rwlock_unlock(&vfs->meta_lock);
        return VFS_ERR_BADFD;
    }
    if ((of->flags & VFS_O_WRONLY) && !(of->flags & VFS_O_RDWR)) {
        pthread_rwlock_unlock(&vfs->meta_lock);
        return VFS_ERR_INVAL;
    }
    uint32_t inode_idx = (uint32_t)of->inode_idx;
    off_t start_pos = of->pos;
    pthread_rwlock_unlock(&vfs->meta_lock);

    /* Snapshot extents under the inode's read lock only; no lock is held
     * during the host pread(2) calls below. */
    vfs_extent_t scratch[VFS_MAX_INLINE_EXTENTS + VFS_EXTENTS_PER_OVERFLOW_BLOCK];
    uint32_t extent_count = 0;
    uint64_t fsize = 0;
    vfs_status_t s =
        snapshot_extents_for_read(vfs, inode_idx, scratch, sizeof(scratch) / sizeof(scratch[0]), &extent_count, &fsize);
    if (s != VFS_OK) {
        return s;
    }

    if ((uint64_t)start_pos >= fsize) {
        return VFS_OK; /* EOF. */
    }
    uint64_t avail = fsize - (uint64_t)start_pos;
    if ((uint64_t)count > avail) {
        count = (size_t)avail;
    }

    uint8_t* dst = (uint8_t*)buf;
    size_t remaining = count;
    off_t cur_pos = start_pos;

    while (remaining > 0) {
        uint32_t block_idx = (uint32_t)((uint64_t)cur_pos / VFS_BLOCK_SIZE);
        uint32_t block_off = (uint32_t)((uint64_t)cur_pos % VFS_BLOCK_SIZE);
        uint32_t max_logical = (uint32_t)((remaining + block_off + VFS_BLOCK_SIZE - 1u) / VFS_BLOCK_SIZE);

        uint32_t physical_start = 0, run_blocks = 0;
        extents_resolve_read(scratch, extent_count, block_idx, max_logical, &physical_start, &run_blocks);

        size_t run_bytes = ((size_t)run_blocks * VFS_BLOCK_SIZE) - block_off;
        if (run_bytes > remaining) {
            run_bytes = remaining;
        }

        if (physical_start == 0) {
            memset(dst, 0, run_bytes); /* Sparse hole: zeros, no I/O. */
        } else {
            off_t off = block_offset(physical_start) + (off_t)block_off;
            s = pread_all(vfs->fd, dst, run_bytes, off);
            if (s != VFS_OK) {
                return s;
            }
        }

        dst += run_bytes;
        cur_pos += (off_t)run_bytes;
        remaining -= run_bytes;
    }

    /* Re-acquire briefly just to update the cursor. */
    pthread_rwlock_wrlock(&vfs->meta_lock);
    of = oft_get_locked(vfs, fd);
    if (of != NULL) {
        of->pos = cur_pos;
    }
    pthread_rwlock_unlock(&vfs->meta_lock);

    *bytes_read = count;
    return VFS_OK;
}

/**
 * Extends an inode's extent array (in a local scratch copy) to cover a
 * new allocation, growing the file's block_count and marking the inode
 * dirty. Takes the inode's write lock for the duration of the mutation
 * (not for the I/O that follows in the caller).
 */
static vfs_status_t inode_apply_new_extent(vfs_t* vfs, uint32_t inode_idx, uint32_t logical, uint32_t physical,
                                           uint32_t length) {
    pthread_rwlock_wrlock(&vfs->inode_locks[inode_idx]);

    vfs_extent_t scratch[VFS_MAX_INLINE_EXTENTS + VFS_EXTENTS_PER_OVERFLOW_BLOCK];
    uint32_t count = 0;
    vfs_status_t s = extents_load(vfs, inode_idx, scratch, sizeof(scratch) / sizeof(scratch[0]), &count);
    if (s != VFS_OK) {
        pthread_rwlock_unlock(&vfs->inode_locks[inode_idx]);
        return s;
    }

    uint32_t new_count =
        extents_insert_local(scratch, count, sizeof(scratch) / sizeof(scratch[0]), logical, physical, length);
    if (new_count == UINT32_MAX) {
        pthread_rwlock_unlock(&vfs->inode_locks[inode_idx]);
        return VFS_ERR_OVERFLOW;
    }

    s = extents_store(vfs, inode_idx, scratch, new_count);
    if (s == VFS_OK) {
        pthread_rwlock_wrlock(&vfs->meta_lock);
        vfs->inodes[inode_idx].block_count += length;
        pthread_rwlock_unlock(&vfs->meta_lock);
    }

    pthread_rwlock_unlock(&vfs->inode_locks[inode_idx]);
    return s;
}

vfs_status_t vfs_fwrite(vfs_t* vfs, vfs_fd_t fd, const void* buf, size_t count, size_t* bytes_written) {
    if (vfs == NULL || buf == NULL || bytes_written == NULL) {
        return VFS_ERR_INVAL;
    }
    *bytes_written = 0;
    if (count == 0) {
        return VFS_OK;
    }
    if (vfs->readonly) {
        return VFS_ERR_READONLY;
    }

    pthread_rwlock_wrlock(&vfs->meta_lock);
    open_file_t* of = oft_get_locked(vfs, fd);
    if (of == NULL) {
        pthread_rwlock_unlock(&vfs->meta_lock);
        return VFS_ERR_BADFD;
    }
    if (!(of->flags & (VFS_O_WRONLY | VFS_O_RDWR))) {
        pthread_rwlock_unlock(&vfs->meta_lock);
        return VFS_ERR_INVAL;
    }
    uint32_t inode_idx = (uint32_t)of->inode_idx;
    if (of->flags & VFS_O_APPEND) {
        of->pos = (off_t)vfs->inodes[inode_idx].size;
    }
    off_t cur_pos = of->pos;
    pthread_rwlock_unlock(&vfs->meta_lock);

    const uint8_t* src = (const uint8_t*)buf;
    size_t remaining = count;
    vfs_status_t rc = VFS_OK;

    while (remaining > 0) {
        uint32_t block_idx = (uint32_t)((uint64_t)cur_pos / VFS_BLOCK_SIZE);
        uint32_t block_off = (uint32_t)((uint64_t)cur_pos % VFS_BLOCK_SIZE);
        bool full_block_overwrite = (block_off == 0) && (remaining >= VFS_BLOCK_SIZE);

        /* Snapshot the current mapping for this logical block (read lock
         * only, released before any I/O). */
        vfs_extent_t scratch[VFS_MAX_INLINE_EXTENTS + VFS_EXTENTS_PER_OVERFLOW_BLOCK];
        uint32_t extent_count = 0;
        pthread_rwlock_rdlock(&vfs->inode_locks[inode_idx]);
        rc = extents_load(vfs, inode_idx, scratch, sizeof(scratch) / sizeof(scratch[0]), &extent_count);
        pthread_rwlock_unlock(&vfs->inode_locks[inode_idx]);
        if (rc != VFS_OK) {
            break;
        }

        uint32_t existing_phys = 0, existing_run = 0;
        uint32_t max_logical = (uint32_t)((remaining + block_off + VFS_BLOCK_SIZE - 1u) / VFS_BLOCK_SIZE);
        extents_resolve_read(scratch, extent_count, block_idx, max_logical, &existing_phys, &existing_run);

        size_t run_bytes;
        uint32_t phys_start;

        if (existing_phys != 0) {
            /* Already allocated: overwrite in place, no zeroing, no
             * metadata mutation needed. */
            run_bytes = ((size_t)existing_run * VFS_BLOCK_SIZE) - block_off;
            if (run_bytes > remaining) {
                run_bytes = remaining;
            }
            phys_start = existing_phys;
        } else {
            /* Hole: allocate a new contiguous physical run sized to the
             * unallocated span, capped by the allocator's own limits. */
            uint32_t want_len = existing_run; /* Hole length in blocks, from extents_resolve_read. */
            pthread_rwlock_wrlock(&vfs->meta_lock);
            uint32_t new_phys = 0, alloc_len = 0;
            rc = block_alloc_run_locked(vfs, want_len, &new_phys, &alloc_len);
            pthread_rwlock_unlock(&vfs->meta_lock);
            if (rc != VFS_OK) {
                break;
            }

            run_bytes = ((size_t)alloc_len * VFS_BLOCK_SIZE) - block_off;
            if (run_bytes > remaining) {
                run_bytes = remaining;
            }

            /*
             * Zero-fill only the parts of the newly allocated run that
             * this write will not fully overwrite: a partial first block
             * (block_off != 0) or a partial trailing block (this run ends
             * before its last allocated block is fully covered). A full,
             * whole-block run entirely covered by `remaining` needs no
             * zeroing at all -- the caller's data covers every byte.
             */
            bool covers_whole_run = full_block_overwrite && (run_bytes == (size_t)alloc_len * VFS_BLOCK_SIZE);
            if (!covers_whole_run) {
                for (uint32_t i = 0; i < alloc_len; i++) {
                    uint32_t blk_logical_off = i * VFS_BLOCK_SIZE;
                    /* Skip zeroing a block that this write will cover completely. */
                    size_t blk_start_in_run = (i == 0) ? 0 : (size_t)blk_logical_off - block_off;
                    bool fully_covered = (i == 0) ? (block_off == 0 && run_bytes >= VFS_BLOCK_SIZE)
                                                  : (blk_start_in_run + VFS_BLOCK_SIZE <= run_bytes);
                    if (!fully_covered) {
                        rc = block_zero(vfs->fd, new_phys + i);
                        if (rc != VFS_OK) {
                            pthread_rwlock_wrlock(&vfs->meta_lock);
                            block_free_run_locked(vfs, new_phys, alloc_len);
                            pthread_rwlock_unlock(&vfs->meta_lock);
                            goto write_done;
                        }
                    }
                }
            }

            rc = inode_apply_new_extent(vfs, inode_idx, block_idx, new_phys, alloc_len);
            if (rc != VFS_OK) {
                pthread_rwlock_wrlock(&vfs->meta_lock);
                block_free_run_locked(vfs, new_phys, alloc_len);
                pthread_rwlock_unlock(&vfs->meta_lock);
                break;
            }
            phys_start = new_phys;
        }

        off_t off = block_offset(phys_start) + (off_t)block_off;
        rc = pwrite_all(vfs->fd, src, run_bytes, off);
        if (rc != VFS_OK) {
            break;
        }

        src += run_bytes;
        cur_pos += (off_t)run_bytes;
        remaining -= run_bytes;
    }

write_done:
    *bytes_written = count - remaining;

    pthread_rwlock_wrlock(&vfs->meta_lock);
    of = oft_get_locked(vfs, fd);
    if (of != NULL) {
        vfs_inode_t* in = &vfs->inodes[inode_idx];
        if ((uint64_t)cur_pos > in->size) {
            in->size = (uint64_t)cur_pos;
        }
        in->modified_at = (uint64_t)time(NULL);
        of->pos = cur_pos;
        (void)inode_mark_dirty_locked(vfs, inode_idx);
    }
    pthread_rwlock_unlock(&vfs->meta_lock);

    return rc;
}

vfs_status_t vfs_fseek(vfs_t* vfs, vfs_fd_t fd, off_t offset, int whence, off_t* new_offset) {
    if (vfs == NULL) {
        return VFS_ERR_INVAL;
    }

    pthread_rwlock_wrlock(&vfs->meta_lock);
    open_file_t* of = oft_get_locked(vfs, fd);
    if (of == NULL) {
        pthread_rwlock_unlock(&vfs->meta_lock);
        return VFS_ERR_BADFD;
    }

    const vfs_inode_t* in = &vfs->inodes[(uint32_t)of->inode_idx];
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
            pthread_rwlock_unlock(&vfs->meta_lock);
            return VFS_ERR_INVAL;
    }

    off_t result = base + offset;
    if (result < 0) {
        pthread_rwlock_unlock(&vfs->meta_lock);
        return VFS_ERR_INVAL;
    }

    of->pos = result;
    if (new_offset != NULL) {
        *new_offset = result;
    }

    pthread_rwlock_unlock(&vfs->meta_lock);
    return VFS_OK;
}

vfs_status_t vfs_ftell(vfs_t* vfs, vfs_fd_t fd, off_t* pos) {
    if (vfs == NULL || pos == NULL) {
        return VFS_ERR_INVAL;
    }

    pthread_rwlock_rdlock(&vfs->meta_lock);
    open_file_t* of = oft_get_locked(vfs, fd);
    if (of == NULL) {
        pthread_rwlock_unlock(&vfs->meta_lock);
        return VFS_ERR_BADFD;
    }
    *pos = of->pos;
    pthread_rwlock_unlock(&vfs->meta_lock);
    return VFS_OK;
}

vfs_status_t vfs_truncate(vfs_t* vfs, const char* path, uint64_t length) {
    if (vfs == NULL || path == NULL) {
        return VFS_ERR_INVAL;
    }
    if (vfs->readonly) {
        return VFS_ERR_READONLY;
    }

    if (length > (uint64_t)VFS_TOTAL_BLOCKS * (uint64_t)VFS_BLOCK_SIZE) {
        return VFS_ERR_OVERFLOW;
    }

    pthread_rwlock_wrlock(&vfs->meta_lock);
    uint32_t idx = inode_find_locked(vfs, path);
    if (idx == INODE_NONE) {
        pthread_rwlock_unlock(&vfs->meta_lock);
        return VFS_ERR_NOTFOUND;
    }
    pthread_rwlock_unlock(&vfs->meta_lock);

    pthread_rwlock_wrlock(&vfs->inode_locks[idx]);

    vfs_extent_t scratch[VFS_MAX_INLINE_EXTENTS + VFS_EXTENTS_PER_OVERFLOW_BLOCK];
    uint32_t count = 0;
    vfs_status_t rc = extents_load(vfs, idx, scratch, sizeof(scratch) / sizeof(scratch[0]), &count);
    if (rc != VFS_OK) {
        pthread_rwlock_unlock(&vfs->inode_locks[idx]);
        return rc;
    }

    uint64_t old_size = vfs->inodes[idx].size;

    if (length < old_size) {
        uint32_t new_last_block = (length == 0) ? 0 : (uint32_t)((length - 1u) / VFS_BLOCK_SIZE);
        uint32_t new_block_count = (length == 0) ? 0 : new_last_block + 1u;

        /* Split/trim extents at new_block_count, freeing everything past it. */
        uint32_t kept = 0;
        pthread_rwlock_wrlock(&vfs->meta_lock);
        for (uint32_t i = 0; i < count; i++) {
            vfs_extent_t* e = &scratch[i];
            uint32_t e_end = e->logical_block + e->length;
            if (e_end <= new_block_count) {
                scratch[kept++] = *e;
            } else if (e->logical_block >= new_block_count) {
                if (e->physical_block != 0) {
                    block_free_run_locked(vfs, e->physical_block, e->length);
                }
            } else {
                /* Straddles the boundary: keep the front part, free the tail. */
                uint32_t keep_len = new_block_count - e->logical_block;
                uint32_t free_len = e->length - keep_len;
                if (e->physical_block != 0 && free_len > 0) {
                    block_free_run_locked(vfs, e->physical_block + keep_len, free_len);
                }
                e->length = keep_len;
                scratch[kept++] = *e;
            }
        }
        pthread_rwlock_unlock(&vfs->meta_lock);
        count = kept;

        rc = extents_store(vfs, idx, scratch, count);
        if (rc != VFS_OK) {
            pthread_rwlock_unlock(&vfs->inode_locks[idx]);
            return rc;
        }

        pthread_rwlock_wrlock(&vfs->meta_lock);
        vfs->inodes[idx].size = length;
        vfs->inodes[idx].modified_at = (uint64_t)time(NULL);
        /* Recompute block_count from the surviving extents. */
        uint32_t total_blocks = 0;
        for (uint32_t i = 0; i < count; i++) {
            if (scratch[i].physical_block != 0) {
                total_blocks += scratch[i].length;
            }
        }
        vfs->inodes[idx].block_count = total_blocks;
        rc = inode_write_locked(vfs, idx);
        pthread_rwlock_unlock(&vfs->meta_lock);

        /* Zero the partial tail block, if any, so reads past new EOF see zeros. */
        if (rc == VFS_OK && new_block_count > 0 && (length % VFS_BLOCK_SIZE) != 0) {
            uint32_t last_phys = 0, dummy_run = 0;
            extents_resolve_read(scratch, count, new_block_count - 1u, 1u, &last_phys, &dummy_run);
            if (last_phys != 0) {
                uint32_t tail_off = (uint32_t)(length % VFS_BLOCK_SIZE);
                uint32_t tail_len = VFS_BLOCK_SIZE - tail_off;
                uint8_t* zeros = calloc(1, tail_len);
                if (zeros == NULL) {
                    rc = VFS_ERR_NOMEM;
                } else {
                    rc = pwrite_all(vfs->fd, zeros, tail_len, block_offset(last_phys) + (off_t)tail_off);
                    free(zeros);
                }
            }
        }
    } else if (length > old_size) {
        /* Extend: reserve a hole; no physical allocation happens until a
         * write touches it, except we must ensure reads of the extended
         * region return zeros, which the hole (no extent) already gives
         * us for free. Only update size. */
        pthread_rwlock_wrlock(&vfs->meta_lock);
        vfs->inodes[idx].size = length;
        vfs->inodes[idx].modified_at = (uint64_t)time(NULL);
        rc = inode_mark_dirty_locked(vfs, idx);
        pthread_rwlock_unlock(&vfs->meta_lock);
    }

    pthread_rwlock_unlock(&vfs->inode_locks[idx]);
    return rc;
}

vfs_status_t vfs_stat(vfs_t* vfs, const char* path, vfs_stat_t* st) {
    if (vfs == NULL || path == NULL || st == NULL) {
        return VFS_ERR_INVAL;
    }

    pthread_rwlock_rdlock(&vfs->meta_lock);
    uint32_t idx = inode_find_locked(vfs, path);
    if (idx == INODE_NONE) {
        pthread_rwlock_unlock(&vfs->meta_lock);
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
    pthread_rwlock_unlock(&vfs->meta_lock);
    return VFS_OK;
}

vfs_status_t vfs_unlink(vfs_t* vfs, const char* path) {
    if (vfs == NULL || path == NULL) {
        return VFS_ERR_INVAL;
    }
    if (vfs->readonly) {
        return VFS_ERR_READONLY;
    }

    pthread_rwlock_wrlock(&vfs->meta_lock);
    uint32_t idx = inode_find_locked(vfs, path);
    if (idx == INODE_NONE) {
        pthread_rwlock_unlock(&vfs->meta_lock);
        return VFS_ERR_NOTFOUND;
    }

    for (uint32_t i = 0; i < VFS_MAX_OPEN_FILES; i++) {
        if (vfs->oft[i].inode_idx == (int)idx) {
            vfs->oft[i].inode_idx = OFT_FREE;
            vfs->oft[i].pos = 0;
            vfs->oft[i].flags = 0;
        }
    }
    pthread_rwlock_unlock(&vfs->meta_lock);

    pthread_rwlock_wrlock(&vfs->inode_locks[idx]);
    pthread_rwlock_wrlock(&vfs->meta_lock);
    vfs_status_t rc = inode_free_all_blocks_locked(vfs, idx);
    if (rc == VFS_OK) {
        rc = flush_bitmap_locked(vfs);
    }
    pthread_rwlock_unlock(&vfs->meta_lock);
    pthread_rwlock_unlock(&vfs->inode_locks[idx]);
    return rc;
}

bool vfs_exists(vfs_t* vfs, const char* path) {
    if (vfs == NULL || path == NULL) {
        return false;
    }
    pthread_rwlock_rdlock(&vfs->meta_lock);
    bool found = (inode_find_locked(vfs, path) != INODE_NONE);
    pthread_rwlock_unlock(&vfs->meta_lock);
    return found;
}

/* =========================================================================
 * Public API - directory-like listing
 * ======================================================================= */

void vfs_list(vfs_t* vfs, const char* prefix, vfs_list_cb_t callback, void* userdata) {
    if (vfs == NULL || callback == NULL) {
        return;
    }

    bool match_all = (prefix == NULL || prefix[0] == '\0' || (prefix[0] == '/' && prefix[1] == '\0'));
    size_t prefix_len = match_all ? 0u : strlen(prefix);

    pthread_rwlock_rdlock(&vfs->meta_lock);
    for (uint32_t i = 0; i < VFS_MAX_INODES; i++) {
        const vfs_inode_t* in = &vfs->inodes[i];
        if (in->path[0] == '\0') {
            continue;
        }
        if (!match_all && strncmp(in->path, prefix, prefix_len) != 0) {
            continue;
        }

        vfs_stat_t st = {
            .size = in->size,
            .block_count = in->block_count,
            .created_at = (time_t)in->created_at,
            .modified_at = (time_t)in->modified_at,
        };
        strncpy(st.path, in->path, VFS_MAX_PATH - 1u);
        st.path[VFS_MAX_PATH - 1u] = '\0';

        pthread_rwlock_unlock(&vfs->meta_lock);
        bool cont = callback(in->path, &st, userdata);
        pthread_rwlock_rdlock(&vfs->meta_lock);

        if (!cont) {
            break;
        }
    }
    pthread_rwlock_unlock(&vfs->meta_lock);
}

vfs_status_t vfs_rename(vfs_t* vfs, const char* oldpath, const char* newpath) {
    if (vfs == NULL || oldpath == NULL || newpath == NULL) {
        return VFS_ERR_INVAL;
    }
    if (oldpath[0] == '\0' || newpath[0] == '\0') {
        return VFS_ERR_INVAL;
    }
    if (vfs->readonly) {
        return VFS_ERR_READONLY;
    }

    size_t new_len = strlen(newpath);
    if (new_len >= VFS_MAX_PATH) {
        return VFS_ERR_INVAL;
    }
    if (strncmp(oldpath, newpath, VFS_MAX_PATH) == 0) {
        return VFS_OK;
    }

    pthread_rwlock_wrlock(&vfs->meta_lock);

    vfs_status_t rc = VFS_OK;
    uint32_t src_idx = inode_find_locked(vfs, oldpath);
    if (src_idx == INODE_NONE) {
        rc = VFS_ERR_NOTFOUND;
        goto out;
    }

    uint32_t dst_idx = inode_find_locked(vfs, newpath);
    if (dst_idx != INODE_NONE && dst_idx != src_idx) {
        for (uint32_t i = 0; i < VFS_MAX_OPEN_FILES; i++) {
            if (vfs->oft[i].inode_idx == (int)dst_idx) {
                vfs->oft[i].inode_idx = OFT_FREE;
                vfs->oft[i].pos = 0;
                vfs->oft[i].flags = 0;
            }
        }
        rc = inode_free_all_blocks_locked(vfs, dst_idx);
        if (rc != VFS_OK) {
            goto out;
        }
    }

    vfs_inode_t* src = &vfs->inodes[src_idx];
    memset(src->path, 0, VFS_MAX_PATH);
    memcpy(src->path, newpath, new_len);
    src->modified_at = (uint64_t)time(NULL);
    rc = inode_mark_dirty_locked(vfs, src_idx);
    /* Rename does not need to touch the superblock; only the inode changed. */

out:
    (void)flush_bitmap_locked(vfs);
    pthread_rwlock_unlock(&vfs->meta_lock);
    return rc;
}

/* =========================================================================
 * Public API - sendfile
 * ======================================================================= */

vfs_status_t vfs_sendfile(vfs_t* vfs, int out_fd, vfs_fd_t in_fd, off_t* offset, size_t count, size_t* bytes_sent) {
    if (vfs == NULL || out_fd < 0 || bytes_sent == NULL) {
        return VFS_ERR_INVAL;
    }
    *bytes_sent = 0;
    if (count == 0) {
        return VFS_OK;
    }

    pthread_rwlock_rdlock(&vfs->meta_lock);
    open_file_t* of = oft_get_locked(vfs, in_fd);
    if (of == NULL) {
        pthread_rwlock_unlock(&vfs->meta_lock);
        return VFS_ERR_BADFD;
    }
    if ((of->flags & VFS_O_WRONLY) && !(of->flags & VFS_O_RDWR)) {
        pthread_rwlock_unlock(&vfs->meta_lock);
        return VFS_ERR_INVAL;
    }
    uint32_t inode_idx = (uint32_t)of->inode_idx;
    off_t start_pos = (offset != NULL) ? *offset : of->pos;
    pthread_rwlock_unlock(&vfs->meta_lock);

    vfs_extent_t scratch[VFS_MAX_INLINE_EXTENTS + VFS_EXTENTS_PER_OVERFLOW_BLOCK];
    uint32_t extent_count = 0;
    uint64_t fsize = 0;
    vfs_status_t rc =
        snapshot_extents_for_read(vfs, inode_idx, scratch, sizeof(scratch) / sizeof(scratch[0]), &extent_count, &fsize);
    if (rc != VFS_OK) {
        return rc;
    }

    if (start_pos < 0 || (uint64_t)start_pos >= fsize) {
        return VFS_OK; /* EOF; not an error. */
    }
    uint64_t avail = fsize - (uint64_t)start_pos;
    if ((uint64_t)count > avail) {
        count = (size_t)avail;
    }

    off_t cur_pos = start_pos;
    size_t remaining = count;

    while (remaining > 0) {
        uint32_t block_idx = (uint32_t)((uint64_t)cur_pos / VFS_BLOCK_SIZE);
        uint32_t block_off = (uint32_t)((uint64_t)cur_pos % VFS_BLOCK_SIZE);
        uint32_t max_logical = (uint32_t)((remaining + block_off + VFS_BLOCK_SIZE - 1u) / VFS_BLOCK_SIZE);

        uint32_t phys_start = 0, run_blocks = 0;
        extents_resolve_read(scratch, extent_count, block_idx, max_logical, &phys_start, &run_blocks);

        size_t run_bytes = (size_t)run_blocks * VFS_BLOCK_SIZE - block_off;
        if (run_bytes > remaining) {
            run_bytes = remaining;
        }

        if (phys_start == 0) {
            static const uint8_t zero_block[VFS_BLOCK_SIZE];
            size_t hole_rem = run_bytes;
            while (hole_rem > 0) {
                size_t chunk = hole_rem < VFS_BLOCK_SIZE ? hole_rem : VFS_BLOCK_SIZE;
                rc = write_all(out_fd, zero_block, chunk);
                if (rc != VFS_OK) {
                    goto done;
                }
                hole_rem -= chunk;
            }
        } else {
            off_t host_off = block_offset(phys_start) + (off_t)block_off;
#if defined(__linux__)
            size_t sf_rem = run_bytes;
            while (sf_rem > 0) {
                size_t want = sf_rem < (size_t)SSIZE_MAX ? sf_rem : (size_t)SSIZE_MAX;
                ssize_t sent = sendfile(out_fd, vfs->fd, &host_off, want);
                if (sent < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno == EINVAL || errno == ENOSYS) {
                        uint8_t buf[VFS_BLOCK_SIZE];
                        size_t pb_rem = sf_rem;
                        off_t pb_off = host_off;
                        while (pb_rem > 0) {
                            size_t chunk = pb_rem < VFS_BLOCK_SIZE ? pb_rem : VFS_BLOCK_SIZE;
                            rc = pread_all(vfs->fd, buf, chunk, pb_off);
                            if (rc != VFS_OK) {
                                goto done;
                            }
                            rc = write_all(out_fd, buf, chunk);
                            if (rc != VFS_OK) {
                                goto done;
                            }
                            pb_off += (off_t)chunk;
                            pb_rem -= chunk;
                        }
                        sf_rem = 0;
                        break;
                    }
                    rc = VFS_ERR_IO;
                    goto done;
                }
                if (sent == 0) {
                    rc = VFS_ERR_IO;
                    goto done;
                }
                sf_rem -= (size_t)sent;
            }
#else
            uint8_t buf[VFS_BLOCK_SIZE];
            size_t pb_rem = run_bytes;
            off_t pb_off = host_off;
            while (pb_rem > 0) {
                size_t chunk = pb_rem < VFS_BLOCK_SIZE ? pb_rem : VFS_BLOCK_SIZE;
                rc = pread_all(vfs->fd, buf, chunk, pb_off);
                if (rc != VFS_OK) {
                    goto done;
                }
                rc = write_all(out_fd, buf, chunk);
                if (rc != VFS_OK) {
                    goto done;
                }
                pb_off += (off_t)chunk;
                pb_rem -= chunk;
            }
#endif
        }

        cur_pos += (off_t)run_bytes;
        remaining -= run_bytes;
    }

done:
    *bytes_sent = count - remaining;

    pthread_rwlock_wrlock(&vfs->meta_lock);
    of = oft_get_locked(vfs, in_fd);
    if (of != NULL) {
        if (offset != NULL) {
            *offset = cur_pos;
        } else {
            of->pos = cur_pos;
        }
    }
    pthread_rwlock_unlock(&vfs->meta_lock);

    return rc;
}

/* =========================================================================
 * Public API - utility
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
            return "would exceed per-file extent capacity";
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
    if (vfs == NULL || out == NULL) {
        return;
    }
    vfs_t* v = (vfs_t*)(uintptr_t)vfs;
    pthread_rwlock_rdlock(&v->meta_lock);

    const vfs_super_t* sb = &vfs->super;
    fprintf(out, "=== VFS Superblock (v3) ===\n");
    fprintf(out, "  magic            : 0x%08X\n", sb->magic);
    fprintf(out, "  version          : %u\n", sb->version);
    fprintf(out, "  block_size       : %u\n", sb->block_size);
    fprintf(out, "  max_inodes       : %u\n", sb->max_inodes);
    fprintf(out, "  total_blocks     : %u\n", sb->total_blocks);
    fprintf(out, "  free_block_count : %u\n", sb->free_block_count);
    fprintf(out, "  free_inode_count : %u\n", sb->free_inode_count);
    fprintf(out, "  free_extents     : %u\n", vfs->free_extent_count);

    uint32_t used_inodes = 0;
    for (uint32_t i = 0; i < VFS_MAX_INODES; i++) {
        if (vfs->inodes[i].path[0] != '\0') {
            used_inodes++;
        }
    }
    fprintf(out, "  used_inodes      : %u (counted)\n", used_inodes);

    fprintf(out, "\n=== Inode Table ===\n");
    for (uint32_t i = 0; i < VFS_MAX_INODES; i++) {
        const vfs_inode_t* in = &vfs->inodes[i];
        if (in->path[0] == '\0') {
            continue;
        }
        fprintf(out, "  [%4u] path=%-32s size=%-10" PRIu64 " blocks=%-4u extents=%-3u mtime=%" PRIu64 "\n", i, in->path,
                in->size, in->block_count, in->extent_count, in->modified_at);
    }

    fprintf(out, "\n=== Open-file Table ===\n");
    for (uint32_t i = 0; i < VFS_MAX_OPEN_FILES; i++) {
        const open_file_t* of = &vfs->oft[i];
        if (of->inode_idx == OFT_FREE) {
            continue;
        }
        fprintf(out, "  fd=%-3u inode=%-4d pos=%-10" PRId64 " flags=0x%02X\n", i, of->inode_idx, (int64_t)of->pos,
                of->flags);
    }

    fflush(out);
    pthread_rwlock_unlock(&v->meta_lock);
}

vfs_status_t vfs_write_file(vfs_t* vfs, const char* path, const void* content, size_t len) {
    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC);
    if (fd < 0) {
        return (vfs_status_t)fd;
    }
    size_t written = 0;
    vfs_status_t status = vfs_fwrite(vfs, fd, content, len, &written);
    vfs_fclose(vfs, fd);
    if (status != VFS_OK) {
        return status;
    }
    return (written == len) ? VFS_OK : VFS_ERR_IO;
}

vfs_status_t vfs_append_file(vfs_t* vfs, const char* path, const void* data, size_t len) {
    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_APPEND);
    if (fd < 0) {
        return (vfs_status_t)fd;
    }
    size_t written = 0;
    vfs_status_t status = vfs_fwrite(vfs, fd, data, len, &written);
    vfs_fclose(vfs, fd);
    if (status != VFS_OK) {
        return status;
    }
    return (written == len) ? VFS_OK : VFS_ERR_IO;
}

void* vfs_read_file(vfs_t* vfs, const char* path, size_t* out_size) {
    if (vfs == NULL || path == NULL || out_size == NULL) {
        return NULL;
    }

    vfs_fd_t fd = vfs_fopen(vfs, path, VFS_O_RDONLY);
    if (fd < 0) {
        return NULL;
    }

    vfs_stat_t st;
    if (vfs_stat(vfs, path, &st) != VFS_OK) {
        vfs_fclose(vfs, fd);
        return NULL;
    }

    if (st.size == 0) {
        vfs_fclose(vfs, fd);
        *out_size = 0;
        return malloc(1);
    }

    void* data = malloc(st.size);
    if (data == NULL) {
        vfs_fclose(vfs, fd);
        return NULL;
    }

    size_t bytes_read = 0;
    vfs_status_t s = vfs_fread(vfs, fd, data, st.size, &bytes_read);
    vfs_fclose(vfs, fd);
    if (s != VFS_OK) {
        free(data);
        return NULL;
    }

    *out_size = bytes_read;
    return data;
}

vfs_status_t vfs_open_embedded(const void* embed_data, size_t embed_size, bool readonly, vfs_t** out_vfs) {
    if (embed_data == NULL || embed_size == 0 || out_vfs == NULL) {
        return VFS_ERR_INVAL;
    }

#if defined(__linux__)
    int mem_fd = memfd_create("vfs_embedded_image", MFD_CLOEXEC);
#else
    char tmpl[] = "/tmp/vfs_embedded_XXXXXX";
    int mem_fd = mkstemp(tmpl);
    if (mem_fd >= 0) {
        unlink(tmpl); /* Anonymous once unlinked, like memfd. */
    }
#endif
    if (mem_fd < 0) {
        return VFS_ERR_IO;
    }

    const uint8_t* src = (const uint8_t*)embed_data;
    size_t remaining = embed_size;
    while (remaining > 0) {
        ssize_t written = write(mem_fd, src, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(mem_fd);
            return VFS_ERR_IO;
        }
        src += written;
        remaining -= (size_t)written;
    }

    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", mem_fd);

    vfs_status_t status = vfs_open(proc_path, readonly, out_vfs);

    close(mem_fd);
    return status;
}
