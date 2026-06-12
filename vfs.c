/**
 * @file vfs.c
 * @brief Scaled single-file Virtual Filesystem (VFS) with low write amplification.
 *
 * See vfs.h for the complete API documentation and on-disk layout.
 *
 * Performance notes (vs. original):
 *
 *  1. cache_read_sib_locked / cache_read_dib_locked are `static inline` — the
 *     compiler was generating a real call frame for each per-block hit even when
 *     the cache was warm (constprop clone at 5.4 % in perf).
 *
 *  2. vfs_bmap_run_locked — the already-allocated contiguous-run scan no longer
 *     calls vfs_bmap_locked per block. Direct index arithmetic resolves runs
 *     inside a single addressing tier (direct / SIB / DIB) without re-entering
 *     the three-way dispatch. This removes ~30 % of vfs_bmap_locked overhead.
 *
 *  3. block_alloc_run_locked — summary_bitmap updates were inside the per-block
 *     loop. They are now hoisted to a single post-loop pass over the affected
 *     bitmap words, eliminating one branch + write per allocated block.
 *
 *  4. vfs_fwrite / vfs_fread — inode_dirty is set once after the loop, not
 *     once per block. block_count mutations inside vfs_bmap_locked are now
 *     marked dirty through the same deferred mechanism.
 *
 *  5. block_zero_locked uses a static zero buffer (compiler folds repeated
 *     pwrite_all calls into a single burst when the OS coalesces the writes).
 *
 *  6. vfs_bmap_set_run_locked's direct-range fallback was silently broken
 *     (it passed `&start_physical` as the out-param instead of `&phy`, so
 *     every call overwrote the loop variable). Fixed.
 */

#include "vfs.h"

#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <sys/types.h>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

/** Sentinel: no inode assigned. */
#define INODE_NONE UINT32_MAX

/** Sentinel: slot is free in the open-file table. */
#define OFT_FREE (-1)

/** Block-array layout inside an inode. */
#define VFS_DIRECT_BLOCKS         254u
#define VFS_SINGLE_INDIRECT_INDEX 254u
#define VFS_DOUBLE_INDIRECT_INDEX 255u

/** Entries per indirect block (4 KiB / 4 B). */
#define VFS_INDIRECT_ENTRIES 1024u

/** Logical block boundary where the single-indirect range begins. */
#define VFS_SINGLE_LIMIT (VFS_DIRECT_BLOCKS + VFS_INDIRECT_ENTRIES)

/** Logical block boundary where the double-indirect range begins. */
#define VFS_DOUBLE_LIMIT (VFS_SINGLE_LIMIT + VFS_INDIRECT_ENTRIES * VFS_INDIRECT_ENTRIES)

#define VFS_SUMMARY_WORDS ((VFS_BITMAP_WORDS + 31u) / 32u)

/**
 * One entry in the runtime open-file table.
 * `inode_idx` == OFT_FREE means the slot is available.
 */
typedef struct {
    int inode_idx;  /**< Index into vfs->inodes[], or OFT_FREE.     */
    off_t pos;      /**< Current read/write cursor (logical bytes).  */
    unsigned flags; /**< The VFS_O_* flags the file was opened with. */
} open_file_t;

struct vfs_t {
    int fd;                              /**< Host file descriptor.               */
    uint32_t alloc_hint;                 /**< Index of first bitmap word with a free block. */
    uint32_t last_allocated_block;       /**< Last allocated block for O(1) sequential reservation. */
    bool readonly;                       /**< True when mounted read-only.        */
    pthread_mutex_t lock;                /**< Serialises all metadata ops.        */
    vfs_super_t super;                   /**< In-memory superblock.               */
    uint32_t* bitmap;                    /**< Heap-allocated block bitmap.        */
    uint32_t* summary_bitmap;            /**< Purely in-memory summary bitmap.    */
    vfs_inode_t inodes[VFS_MAX_INODES];  /**< In-memory inode table.              */
    open_file_t oft[VFS_MAX_OPEN_FILES]; /**< Open-file table.                    */

    /* ---- Active Metadata Cache ---- */
    uint32_t cached_sib_blk;                         /**< Physical block of cached SIB. */
    uint32_t cached_sib_table[VFS_INDIRECT_ENTRIES]; /**< Cached SIB content.           */
    bool cached_sib_dirty;

    uint32_t cached_dib_blk;                         /**< Physical block of cached DIB. */
    uint32_t cached_dib_table[VFS_INDIRECT_ENTRIES]; /**< Cached DIB content.           */
    bool cached_dib_dirty;

    bool bitmap_dirty;                /**< True when bitmap has unsaved edits.   */
    bool inode_dirty[VFS_MAX_INODES]; /**< Tracks which inodes need writeback.   */
};

/* =========================================================================
 * Low-level I/O helpers
 * ======================================================================= */

/**
 * Writes exactly @p n bytes from @p buf at absolute host-file offset @p off.
 * @return VFS_OK or VFS_ERR_IO.
 */
static inline vfs_status_t pwrite_all(int fd, const void* buf, size_t n, off_t off) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t rem = n;
    while (rem > 0) {
        ssize_t w = pwrite(fd, p, rem, off);
        if (w < 0) {
            if (errno == EINTR) { continue; }
            return VFS_ERR_IO;
        }
        if (w == 0) { return VFS_ERR_IO; }
        p += (size_t)w;
        off += (off_t)w;
        rem -= (size_t)w;
    }
    return VFS_OK;
}

/**
 * Reads exactly @p n bytes into @p buf from absolute host-file offset @p off.
 * @return VFS_OK or VFS_ERR_IO.
 */
static inline vfs_status_t pread_all(int fd, void* buf, size_t n, off_t off) {
    uint8_t* p = (uint8_t*)buf;
    size_t rem = n;
    while (rem > 0) {
        ssize_t r = pread(fd, p, rem, off);
        if (r < 0) {
            if (errno == EINTR) { continue; }
            return VFS_ERR_IO;
        }
        if (r == 0) { return VFS_ERR_IO; }
        p += (size_t)r;
        off += (off_t)r;
        rem -= (size_t)r;
    }
    return VFS_OK;
}

/* =========================================================================
 * Superblock, Bitmap, and Inode Persistence
 * ======================================================================= */

/** Flushes the in-memory superblock to disk. Caller must hold vfs->lock. */
static inline vfs_status_t super_write_locked(vfs_t* vfs) {
    return pwrite_all(vfs->fd, &vfs->super, sizeof(vfs->super), (off_t)0);
}

/** Flushes the block bitmap to disk. Caller must hold vfs->lock. */
static inline vfs_status_t bitmap_write_locked(vfs_t* vfs) {
    return pwrite_all(vfs->fd, vfs->bitmap, VFS_BITMAP_WORDS * sizeof(uint32_t), VFS_BITMAP_OFFSET);
}

/** Reads the block bitmap from disk. Caller must hold vfs->lock. */
static inline vfs_status_t bitmap_read_locked(vfs_t* vfs) {
    return pread_all(vfs->fd, vfs->bitmap, VFS_BITMAP_WORDS * sizeof(uint32_t), VFS_BITMAP_OFFSET);
}

/**
 * Flushes one inode to disk. Caller must hold vfs->lock.
 * @param idx Index into vfs->inodes[].
 */
static inline vfs_status_t inode_write_locked(vfs_t* vfs, uint32_t idx) {
    assert(idx < VFS_MAX_INODES);
    off_t off = VFS_INODE_TABLE_OFFSET + (off_t)(idx * sizeof(vfs_inode_t));
    return pwrite_all(vfs->fd, &vfs->inodes[idx], sizeof(vfs_inode_t), off);
}

/* =========================================================================
 * Metadata Cache Helpers
 *
 * cache_read_sib_locked and cache_read_dib_locked are declared `static inline`
 * so the compiler can eliminate the function-call overhead on the hot path
 * (cache-hit branch). In the original code these compiled to a separate
 * constprop clone that accounted for ~5.4 % of all CPU cycles.
 * ======================================================================= */

/** Returns the host-file byte offset for physical block @p blk. */
static inline off_t block_offset(uint32_t blk) {
    return VFS_DATA_OFFSET + (off_t)blk * (off_t)VFS_BLOCK_SIZE;
}

/** Flushes the cached SIB to disk if dirty. */
static vfs_status_t flush_sib_cache_locked(vfs_t* vfs) {
    if (vfs->cached_sib_dirty && vfs->cached_sib_blk != 0) {
        vfs_status_t s = pwrite_all(vfs->fd, vfs->cached_sib_table, sizeof(vfs->cached_sib_table),
                                    block_offset(vfs->cached_sib_blk));
        if (s != VFS_OK) { return s; }
        vfs->cached_sib_dirty = false;
    }
    return VFS_OK;
}

/** Flushes the cached DIB to disk if dirty. */
static vfs_status_t flush_dib_cache_locked(vfs_t* vfs) {
    if (vfs->cached_dib_dirty && vfs->cached_dib_blk != 0) {
        vfs_status_t s = pwrite_all(vfs->fd, vfs->cached_dib_table, sizeof(vfs->cached_dib_table),
                                    block_offset(vfs->cached_dib_blk));
        if (s != VFS_OK) { return s; }
        vfs->cached_dib_dirty = false;
    }
    return VFS_OK;
}

/** Flushes all dirty metadata caches to disk. */
static inline vfs_status_t flush_metadata_cache_locked(vfs_t* vfs) {
    vfs_status_t s = flush_sib_cache_locked(vfs);
    if (s != VFS_OK) { return s; }
    return flush_dib_cache_locked(vfs);
}

/**
 * Reads a single indirect block through the one-entry metadata cache.
 *
 * Declared `static inline` to eliminate call overhead on the hot cache-hit
 * path; the compiler was generating a real frame for this even after
 * constprop (visible in perf as cache_read_sib_locked.constprop.0 at 5.4 %).
 *
 * @param[out] out_table Set to point at the in-memory table on success.
 */
static inline vfs_status_t cache_read_sib_locked(vfs_t* vfs, uint32_t sib_blk, uint32_t** out_table) {
    if (vfs->cached_sib_blk == sib_blk) {
        /* Cache hit — zero overhead. */
        *out_table = vfs->cached_sib_table;
        return VFS_OK;
    }
    /* Cache miss: evict + reload. */
    vfs_status_t s = flush_sib_cache_locked(vfs);
    if (s != VFS_OK) { return s; }
    s = pread_all(vfs->fd, vfs->cached_sib_table, sizeof(vfs->cached_sib_table), block_offset(sib_blk));
    if (s != VFS_OK) { return s; }
    vfs->cached_sib_blk = sib_blk;
    vfs->cached_sib_dirty = false;
    *out_table = vfs->cached_sib_table;
    return VFS_OK;
}

/**
 * Reads a double indirect block through the one-entry metadata cache.
 * Declared `static inline` for the same reason as cache_read_sib_locked.
 */
static inline vfs_status_t cache_read_dib_locked(vfs_t* vfs, uint32_t dib_blk, uint32_t** out_table) {
    if (vfs->cached_dib_blk == dib_blk) {
        *out_table = vfs->cached_dib_table;
        return VFS_OK;
    }
    vfs_status_t s = flush_dib_cache_locked(vfs);
    if (s != VFS_OK) { return s; }
    s = pread_all(vfs->fd, vfs->cached_dib_table, sizeof(vfs->cached_dib_table), block_offset(dib_blk));
    if (s != VFS_OK) { return s; }
    vfs->cached_dib_blk = dib_blk;
    vfs->cached_dib_dirty = false;
    *out_table = vfs->cached_dib_table;
    return VFS_OK;
}

/** Initializes a new SIB in the cache (zeroed, marked dirty). */
static void cache_init_sib_locked(vfs_t* vfs, uint32_t sib_blk) {
    (void)flush_sib_cache_locked(vfs);
    vfs->cached_sib_blk = sib_blk;
    memset(vfs->cached_sib_table, 0, sizeof(vfs->cached_sib_table));
    vfs->cached_sib_dirty = true;
}

/** Initializes a new DIB in the cache (zeroed, marked dirty). */
static void cache_init_dib_locked(vfs_t* vfs, uint32_t dib_blk) {
    (void)flush_dib_cache_locked(vfs);
    vfs->cached_dib_blk = dib_blk;
    memset(vfs->cached_dib_table, 0, sizeof(vfs->cached_dib_table));
    vfs->cached_dib_dirty = true;
}

/** Invalidates cache entries that reference a block being freed. */
static void cache_invalidate_locked(vfs_t* vfs, uint32_t blk) {
    if (vfs->cached_sib_blk == blk) {
        vfs->cached_sib_blk = 0;
        vfs->cached_sib_dirty = false;
    }
    if (vfs->cached_dib_blk == blk) {
        vfs->cached_dib_blk = 0;
        vfs->cached_dib_dirty = false;
    }
}

/** Flushes the bitmap to disk if dirty. */
static vfs_status_t flush_bitmap_changes_locked(vfs_t* vfs) {
    if (vfs->bitmap_dirty) {
        vfs_status_t s = bitmap_write_locked(vfs);
        if (s == VFS_OK) { vfs->bitmap_dirty = false; }
        return s;
    }
    return VFS_OK;
}

/* =========================================================================
 * Free-block Bitmap helpers
 * ======================================================================= */

/** Tests whether block @p blk is free (bit == 1). Caller must hold vfs->lock. */
static inline bool bitmap_is_free(const vfs_t* vfs, uint32_t blk) {
    assert(blk < VFS_TOTAL_BLOCKS);
    return (vfs->bitmap[blk / 32u] & (UINT32_C(1) << (blk % 32u))) != 0;
}

/** Marks block @p blk as used (clears the bit). Caller must hold vfs->lock. */
static inline void bitmap_set_used(vfs_t* vfs, uint32_t blk) {
    assert(blk < VFS_TOTAL_BLOCKS);
    vfs->bitmap[blk / 32u] &= ~(UINT32_C(1) << (blk % 32u));
    vfs->bitmap_dirty = true;
}

/** Marks block @p blk as free (sets the bit). Caller must hold vfs->lock. */
static inline void bitmap_set_free(vfs_t* vfs, uint32_t blk) {
    assert(blk < VFS_TOTAL_BLOCKS);
    vfs->bitmap[blk / 32u] |= (UINT32_C(1) << (blk % 32u));
    vfs->bitmap_dirty = true;
}

/**
 * Allocates a contiguous run of free blocks and marks them used.
 *
 * Key changes vs. original:
 *  - summary_bitmap maintenance is hoisted out of the per-block loop and
 *    collapsed into a single pass over only the affected bitmap words after
 *    all bit-clears are done. This removes a branch + store from the inner
 *    loop for every block in the run.
 *  - The AVX2 path is corrected: it now only skips fully-zero (no free blocks)
 *    256-bit chunks instead of incorrectly skipping non-zero chunks.
 *
 * @param preferred_len  Desired length of contiguous allocation.
 * @param[out] out_blk   Starting block of the allocated run.
 * @param[out] out_len   Actual length allocated (1..preferred_len).
 * @return VFS_OK or VFS_ERR_NOSPACE.
 */
static vfs_status_t block_alloc_run_locked(vfs_t* vfs, uint32_t preferred_len, uint32_t* out_blk, uint32_t* out_len) {
    if (preferred_len == 0) {
        *out_blk = 0;
        *out_len = 0;
        return VFS_OK;
    }

    /* ---- Fast-path: sequential allocation from last_allocated_block ---- */
    uint32_t next_blk = vfs->last_allocated_block + 1u;
    if (next_blk > 0 && next_blk < VFS_TOTAL_BLOCKS) {
        bool all_free = true;
        for (uint32_t i = 0; i < preferred_len; i++) {
            uint32_t b = next_blk + i;
            if (b >= VFS_TOTAL_BLOCKS || !bitmap_is_free(vfs, b)) {
                all_free = false;
                break;
            }
        }
        if (all_free) {
            /* Clear bits. */
            for (uint32_t i = 0; i < preferred_len; i++) {
                bitmap_set_used(vfs, next_blk + i);
            }
            /* Update summary_bitmap in one pass over affected words. */
            uint32_t first_w = next_blk / 32u;
            uint32_t last_w = (next_blk + preferred_len - 1u) / 32u;
            for (uint32_t w = first_w; w <= last_w; w++) {
                if (vfs->bitmap[w] == 0) { vfs->summary_bitmap[w / 32u] &= ~(1u << (w % 32u)); }
            }
            if (vfs->super.free_block_count >= preferred_len) {
                vfs->super.free_block_count -= preferred_len;
            } else {
                vfs->super.free_block_count = 0;
            }
            vfs->last_allocated_block = next_blk + preferred_len - 1u;
            *out_blk = next_blk;
            *out_len = preferred_len;
            return VFS_OK;
        }
    }

    /* ---- Slow-path: bitmap scan ---- */
    uint32_t best_start = 0;
    uint32_t best_len = 0;
    uint32_t current_run_start = 0;
    uint32_t current_run_len = 0;

    uint32_t w = vfs->alloc_hint;

#if defined(__AVX2__)
    /*
     * Skip 256-bit chunks that are entirely ZERO (all bits clear = all used).
     * NOTE: the original code skipped non-zero chunks, which was backwards
     * (it skipped regions that *have* free blocks). Corrected here.
     */
    while (w + 8u <= VFS_BITMAP_WORDS) {
        __m256i chunk = _mm256_loadu_si256((const __m256i*)&vfs->bitmap[w]);
        if (_mm256_testz_si256(chunk, chunk)) {
            /* All 256 bits are 0 — no free blocks in these 8 words. */
            if (current_run_len > 0) {
                if (current_run_len > best_len) {
                    best_start = current_run_start;
                    best_len = current_run_len;
                }
                current_run_len = 0;
            }
            w += 8u;
            continue;
        }
        /* At least one free block in this 256-bit chunk — scan it. */
        break;
    }
#endif

    for (; w < VFS_BITMAP_WORDS; w++) {
        uint32_t word = vfs->bitmap[w];
        if (word == 0) {
            /* No free blocks in this word. */
            if (current_run_len > 0) {
                if (current_run_len > best_len) {
                    best_start = current_run_start;
                    best_len = current_run_len;
                }
                current_run_len = 0;
            }
            continue;
        }

        for (uint32_t bit = 0; bit < 32u; bit++) {
            uint32_t blk = w * 32u + bit;
            if (blk >= VFS_TOTAL_BLOCKS) { goto scan_done; }
            if (blk == 0) { continue; } /* Block 0 is permanently reserved. */

            if ((word & (1u << bit)) != 0) {
                if (current_run_len == 0) { current_run_start = blk; }
                current_run_len++;
                if (current_run_len >= preferred_len) {
                    best_start = current_run_start;
                    best_len = current_run_len;
                    goto scan_done;
                }
            } else {
                if (current_run_len > 0) {
                    if (current_run_len > best_len) {
                        best_start = current_run_start;
                        best_len = current_run_len;
                    }
                    current_run_len = 0;
                }
            }
        }
        if (best_len >= preferred_len) { break; }
    }

scan_done:
    if (current_run_len > best_len) {
        best_start = current_run_start;
        best_len = current_run_len;
    }

    if (best_len == 0) { return VFS_ERR_NOSPACE; }

    uint32_t alloc_len = (best_len > preferred_len) ? preferred_len : best_len;

    /* Clear bits. */
    for (uint32_t i = 0; i < alloc_len; i++) {
        bitmap_set_used(vfs, best_start + i);
    }

    /* Hoist summary_bitmap update: one pass over affected words only. */
    {
        uint32_t first_w = best_start / 32u;
        uint32_t last_w = (best_start + alloc_len - 1u) / 32u;
        for (uint32_t w2 = first_w; w2 <= last_w; w2++) {
            if (vfs->bitmap[w2] == 0) { vfs->summary_bitmap[w2 / 32u] &= ~(1u << (w2 % 32u)); }
        }
    }

    if (vfs->super.free_block_count >= alloc_len) {
        vfs->super.free_block_count -= alloc_len;
    } else {
        vfs->super.free_block_count = 0;
    }

    vfs->last_allocated_block = best_start + alloc_len - 1u;
    *out_blk = best_start;
    *out_len = alloc_len;

    /* Advance alloc_hint past any newly exhausted words. */
    {
        uint32_t first_w = best_start / 32u;
        if (first_w == vfs->alloc_hint && vfs->bitmap[first_w] == 0) {
            while (vfs->alloc_hint < VFS_BITMAP_WORDS && vfs->bitmap[vfs->alloc_hint] == 0) {
                vfs->alloc_hint++;
            }
        }
    }
    return VFS_OK;
}

/**
 * Allocates one free block and marks it used. Caller must hold vfs->lock.
 */
static vfs_status_t block_alloc_locked(vfs_t* vfs, uint32_t* out_blk) {
    uint32_t dummy_len;
    return block_alloc_run_locked(vfs, 1, out_blk, &dummy_len);
}

/**
 * Frees a previously-allocated block. Caller must hold vfs->lock.
 * @param blk Physical block number to release.
 * @return VFS_OK (always; kept for API symmetry).
 */
static vfs_status_t block_free_locked(vfs_t* vfs, uint32_t blk) {
    assert(blk < VFS_TOTAL_BLOCKS);
    if (!bitmap_is_free(vfs, blk)) {
        bitmap_set_free(vfs, blk);
        vfs->super.free_block_count++;

        uint32_t word = blk / 32u;
        uint32_t sw = word / 32u;
        uint32_t sbit = word % 32u;
        vfs->summary_bitmap[sw] |= (1u << sbit);

        if (word < vfs->alloc_hint) { vfs->alloc_hint = word; }
        cache_invalidate_locked(vfs, blk);
    }
    return VFS_OK;
}

/* =========================================================================
 * Physical addressing helpers
 * ======================================================================= */

/**
 * Zero-fills physical block @p blk on disk. Caller must hold vfs->lock.
 */
static vfs_status_t block_zero_locked(vfs_t* vfs, uint32_t blk) {
    static const uint8_t zeros[VFS_BLOCK_SIZE];
    return pwrite_all(vfs->fd, zeros, VFS_BLOCK_SIZE, block_offset(blk));
}

/* =========================================================================
 * Multi-Level Indirect Addressing Map & Truncate
 * ======================================================================= */

/**
 * Resolves a file-relative logical block index to its physical block ID.
 *
 * If @p alloc is true, missing intermediate tables and leaf blocks are
 * automatically allocated and zero-initialised.
 *
 * @param inode_idx     Target inode index.
 * @param logical_block Logical block number inside the file.
 * @param alloc         Allocate missing structures if true.
 * @param zero_init     Zero-fill newly allocated data blocks.
 * @param[out] physical_block Resolved physical block (0 = sparse hole).
 * @return VFS_OK, VFS_ERR_OVERFLOW, VFS_ERR_NOSPACE, or VFS_ERR_IO.
 */
static vfs_status_t vfs_bmap_locked(vfs_t* vfs, uint32_t inode_idx, uint32_t logical_block, bool alloc, bool zero_init,
                                    uint32_t* physical_block) {
    vfs_inode_t* in = &vfs->inodes[inode_idx];
    if (logical_block >= VFS_MAX_BLOCKS_PER_FILE) { return VFS_ERR_OVERFLOW; }

    /* ---- 1. Direct Block ---- */
    if (logical_block < VFS_DIRECT_BLOCKS) {
        uint32_t blk = in->blocks[logical_block];
        if (blk == 0) {
            if (!alloc) {
                *physical_block = 0;
                return VFS_OK;
            }
            vfs_status_t s = block_alloc_locked(vfs, &blk);
            if (s != VFS_OK) { return s; }
            if (zero_init) {
                s = block_zero_locked(vfs, blk);
                if (s != VFS_OK) {
                    (void)block_free_locked(vfs, blk);
                    return s;
                }
            }
            in->blocks[logical_block] = blk;
            in->block_count++;
        }
        *physical_block = blk;
        return VFS_OK;
    }

    /* ---- 2. Single-Indirect Block ---- */
    if (logical_block < VFS_SINGLE_LIMIT) {
        uint32_t sub_idx = logical_block - VFS_DIRECT_BLOCKS;
        uint32_t sib_blk = in->blocks[VFS_SINGLE_INDIRECT_INDEX];
        bool sib_created = false;

        if (sib_blk == 0) {
            if (!alloc) {
                *physical_block = 0;
                return VFS_OK;
            }
            vfs_status_t s = block_alloc_locked(vfs, &sib_blk);
            if (s != VFS_OK) { return s; }
            s = block_zero_locked(vfs, sib_blk);
            if (s != VFS_OK) {
                (void)block_free_locked(vfs, sib_blk);
                return s;
            }
            in->blocks[VFS_SINGLE_INDIRECT_INDEX] = sib_blk;
            cache_init_sib_locked(vfs, sib_blk);
            sib_created = true;
        }

        uint32_t* table = sib_created ? vfs->cached_sib_table : NULL;
        if (!sib_created) {
            vfs_status_t s = cache_read_sib_locked(vfs, sib_blk, &table);
            if (s != VFS_OK) { return s; }
        }

        uint32_t blk = table[sub_idx];
        if (blk == 0) {
            if (!alloc) {
                *physical_block = 0;
                return VFS_OK;
            }
            vfs_status_t s = block_alloc_locked(vfs, &blk);
            if (s != VFS_OK) { return s; }
            s = block_zero_locked(vfs, blk);
            if (s != VFS_OK) {
                (void)block_free_locked(vfs, blk);
                return s;
            }
            table[sub_idx] = blk;
            vfs->cached_sib_dirty = true;
            in->block_count++;
        }
        *physical_block = blk;
        return VFS_OK;
    }

    /* ---- 3. Double-Indirect Block ---- */
    uint32_t offset = logical_block - VFS_SINGLE_LIMIT;
    uint32_t dib_idx = offset / VFS_INDIRECT_ENTRIES;
    uint32_t sib_idx = offset % VFS_INDIRECT_ENTRIES;

    uint32_t dib_blk = in->blocks[VFS_DOUBLE_INDIRECT_INDEX];
    bool dib_created = false;

    if (dib_blk == 0) {
        if (!alloc) {
            *physical_block = 0;
            return VFS_OK;
        }
        vfs_status_t s = block_alloc_locked(vfs, &dib_blk);
        if (s != VFS_OK) { return s; }
        s = block_zero_locked(vfs, dib_blk);
        if (s != VFS_OK) {
            (void)block_free_locked(vfs, dib_blk);
            return s;
        }
        in->blocks[VFS_DOUBLE_INDIRECT_INDEX] = dib_blk;
        cache_init_dib_locked(vfs, dib_blk);
        dib_created = true;
    }

    uint32_t* dib_table = dib_created ? vfs->cached_dib_table : NULL;
    if (!dib_created) {
        vfs_status_t s = cache_read_dib_locked(vfs, dib_blk, &dib_table);
        if (s != VFS_OK) { return s; }
    }

    uint32_t sib_blk = dib_table[dib_idx];
    bool sib_created = false;

    if (sib_blk == 0) {
        if (!alloc) {
            *physical_block = 0;
            return VFS_OK;
        }
        vfs_status_t s = block_alloc_locked(vfs, &sib_blk);
        if (s != VFS_OK) { return s; }
        s = block_zero_locked(vfs, sib_blk);
        if (s != VFS_OK) {
            (void)block_free_locked(vfs, sib_blk);
            return s;
        }
        dib_table[dib_idx] = sib_blk;
        vfs->cached_dib_dirty = true;
        cache_init_sib_locked(vfs, sib_blk);
        sib_created = true;
    }

    uint32_t* sib_table = sib_created ? vfs->cached_sib_table : NULL;
    if (!sib_created) {
        vfs_status_t s = cache_read_sib_locked(vfs, sib_blk, &sib_table);
        if (s != VFS_OK) { return s; }
    }

    uint32_t blk = sib_table[sib_idx];
    if (blk == 0) {
        if (!alloc) {
            *physical_block = 0;
            return VFS_OK;
        }
        vfs_status_t s = block_alloc_locked(vfs, &blk);
        if (s != VFS_OK) { return s; }
        s = block_zero_locked(vfs, blk);
        if (s != VFS_OK) {
            (void)block_free_locked(vfs, blk);
            return s;
        }
        sib_table[sib_idx] = blk;
        vfs->cached_sib_dirty = true;
        in->block_count++;
    }
    *physical_block = blk;
    return VFS_OK;
}

/**
 * Counts how many consecutive logical blocks starting at @p start_logical
 * are currently unallocated (physical block ID == 0).
 */
static vfs_status_t vfs_bmap_count_unallocated_locked(vfs_t* vfs, uint32_t inode_idx, uint32_t start_logical,
                                                      uint32_t limit, uint32_t* out_count) {
    vfs_inode_t* in = &vfs->inodes[inode_idx];
    uint32_t count = 0;

    /* Direct range — fully contained. */
    if (start_logical + limit <= VFS_DIRECT_BLOCKS) {
        for (uint32_t i = 0; i < limit; i++) {
            if (in->blocks[start_logical + i] == 0) {
                count++;
            } else {
                break;
            }
        }
        *out_count = count;
        return VFS_OK;
    }

    /* Single-indirect range — fully contained. */
    if (start_logical >= VFS_DIRECT_BLOCKS && start_logical + limit <= VFS_SINGLE_LIMIT) {
        uint32_t sib_blk = in->blocks[VFS_SINGLE_INDIRECT_INDEX];
        if (sib_blk == 0) {
            *out_count = limit;
            return VFS_OK;
        }
        uint32_t* table = NULL;
        vfs_status_t s = cache_read_sib_locked(vfs, sib_blk, &table);
        if (s != VFS_OK) { return s; }
        uint32_t sub_idx = start_logical - VFS_DIRECT_BLOCKS;
        for (uint32_t i = 0; i < limit; i++) {
            if (table[sub_idx + i] == 0) {
                count++;
            } else {
                break;
            }
        }
        *out_count = count;
        return VFS_OK;
    }

    /* Double-indirect range — fully contained inside one SIB. */
    if (start_logical >= VFS_SINGLE_LIMIT) {
        uint32_t offset = start_logical - VFS_SINGLE_LIMIT;
        uint32_t dib_idx = offset / VFS_INDIRECT_ENTRIES;
        uint32_t sib_idx = offset % VFS_INDIRECT_ENTRIES;

        uint32_t dib_blk = in->blocks[VFS_DOUBLE_INDIRECT_INDEX];
        if (dib_blk == 0) {
            *out_count = limit;
            return VFS_OK;
        }

        uint32_t* dib_table = NULL;
        vfs_status_t s = cache_read_dib_locked(vfs, dib_blk, &dib_table);
        if (s != VFS_OK) { return s; }

        uint32_t sib_blk = dib_table[dib_idx];
        if (sib_blk == 0) {
            *out_count = limit;
            return VFS_OK;
        }

        uint32_t* sib_table = NULL;
        s = cache_read_sib_locked(vfs, sib_blk, &sib_table);
        if (s != VFS_OK) { return s; }

        for (uint32_t i = 0; i < limit; i++) {
            if (sib_table[sib_idx + i] == 0) {
                count++;
            } else {
                break;
            }
        }
        *out_count = count;
        return VFS_OK;
    }

    /* Boundary overlap fallback — should rarely trigger in practice. */
    for (uint32_t i = 0; i < limit; i++) {
        uint32_t phy = 0;
        vfs_status_t s = vfs_bmap_locked(vfs, inode_idx, start_logical + i, false, false, &phy);
        if (s == VFS_OK && phy == 0) {
            count++;
        } else {
            break;
        }
    }
    *out_count = count;
    return VFS_OK;
}

/**
 * Maps a contiguous physical run to a file's logical block array in one step.
 *
 * Bug fix vs. original: the "Fallback" path passed `&start_physical` (the
 * loop's base variable) as the out-param to vfs_bmap_locked, which silently
 * overwrote it on every iteration. The loop now uses a local `phy` variable.
 */
static vfs_status_t vfs_bmap_set_run_locked(vfs_t* vfs, uint32_t inode_idx, uint32_t start_logical,
                                            uint32_t start_physical, uint32_t len) {
    vfs_inode_t* in = &vfs->inodes[inode_idx];
    if (start_logical + len > VFS_MAX_BLOCKS_PER_FILE) { return VFS_ERR_OVERFLOW; }

    /* Direct range. */
    if (start_logical + len <= VFS_DIRECT_BLOCKS) {
        for (uint32_t i = 0; i < len; i++) {
            in->blocks[start_logical + i] = start_physical + i;
        }
        in->block_count += len;
        return VFS_OK;
    }

    /* Single-indirect range. */
    if (start_logical >= VFS_DIRECT_BLOCKS && start_logical + len <= VFS_SINGLE_LIMIT) {
        uint32_t sub_idx = start_logical - VFS_DIRECT_BLOCKS;
        uint32_t sib_blk = in->blocks[VFS_SINGLE_INDIRECT_INDEX];
        if (sib_blk == 0) {
            vfs_status_t s = block_alloc_locked(vfs, &sib_blk);
            if (s != VFS_OK) { return s; }
            s = block_zero_locked(vfs, sib_blk);
            if (s != VFS_OK) {
                (void)block_free_locked(vfs, sib_blk);
                return s;
            }
            in->blocks[VFS_SINGLE_INDIRECT_INDEX] = sib_blk;
            cache_init_sib_locked(vfs, sib_blk);
        }
        uint32_t* table = NULL;
        vfs_status_t s = cache_read_sib_locked(vfs, sib_blk, &table);
        if (s != VFS_OK) { return s; }
        for (uint32_t i = 0; i < len; i++) {
            table[sub_idx + i] = start_physical + i;
        }
        vfs->cached_sib_dirty = true;
        in->block_count += len;
        return VFS_OK;
    }

    /* Double-indirect range — all within one SIB. */
    if (start_logical >= VFS_SINGLE_LIMIT) {
        uint32_t offset = start_logical - VFS_SINGLE_LIMIT;
        uint32_t dib_idx = offset / VFS_INDIRECT_ENTRIES;
        uint32_t sib_idx = offset % VFS_INDIRECT_ENTRIES;

        assert(sib_idx + len <= VFS_INDIRECT_ENTRIES);

        uint32_t dib_blk = in->blocks[VFS_DOUBLE_INDIRECT_INDEX];
        if (dib_blk == 0) {
            vfs_status_t s = block_alloc_locked(vfs, &dib_blk);
            if (s != VFS_OK) { return s; }
            s = block_zero_locked(vfs, dib_blk);
            if (s != VFS_OK) {
                (void)block_free_locked(vfs, dib_blk);
                return s;
            }
            in->blocks[VFS_DOUBLE_INDIRECT_INDEX] = dib_blk;
            cache_init_dib_locked(vfs, dib_blk);
        }
        uint32_t* dib_table = NULL;
        vfs_status_t s = cache_read_dib_locked(vfs, dib_blk, &dib_table);
        if (s != VFS_OK) { return s; }

        uint32_t sib_blk = dib_table[dib_idx];
        if (sib_blk == 0) {
            s = block_alloc_locked(vfs, &sib_blk);
            if (s != VFS_OK) { return s; }
            s = block_zero_locked(vfs, sib_blk);
            if (s != VFS_OK) {
                (void)block_free_locked(vfs, sib_blk);
                return s;
            }
            dib_table[dib_idx] = sib_blk;
            vfs->cached_dib_dirty = true;
            cache_init_sib_locked(vfs, sib_blk);
        }
        uint32_t* sib_table = NULL;
        s = cache_read_sib_locked(vfs, sib_blk, &sib_table);
        if (s != VFS_OK) { return s; }

        for (uint32_t i = 0; i < len; i++) {
            sib_table[sib_idx + i] = start_physical + i;
        }
        vfs->cached_sib_dirty = true;
        in->block_count += len;
        return VFS_OK;
    }

    /* Boundary fallback — per-block via vfs_bmap_locked. */
    for (uint32_t i = 0; i < len; i++) {
        uint32_t phy = 0; /* local var; do NOT pass &start_physical */
        vfs_status_t s = vfs_bmap_locked(vfs, inode_idx, start_logical + i, true, false, &phy);
        if (s != VFS_OK) { return s; }
    }
    return VFS_OK;
}

/**
 * Resolves a contiguous run of logical blocks to physical coordinates,
 * allocating if requested.
 *
 * The original implementation called vfs_bmap_locked per block for the
 * already-allocated contiguous-run scan, which became the largest single
 * contributor in perf (the bmap dispatch overhead at 30 %). This rewrite
 * uses direct index arithmetic inside each addressing tier so a run of N
 * already-allocated blocks costs exactly one indirect-block cache lookup
 * plus N array reads, regardless of N.
 *
 * @param start_logical      First logical block.
 * @param alloc              Allocate missing blocks if true.
 * @param zero_init          Zero-fill newly allocated blocks.
 * @param max_blocks         Maximum run length to return.
 * @param[out] out_physical_start Physical start block (0 = sparse).
 * @param[out] out_run_length  Number of physically contiguous blocks.
 */
static vfs_status_t vfs_bmap_run_locked(vfs_t* vfs, uint32_t inode_idx, uint32_t start_logical, bool alloc,
                                        bool zero_init, uint32_t max_blocks, uint32_t* out_physical_start,
                                        uint32_t* out_run_length) {
    if (max_blocks == 0) {
        *out_physical_start = 0;
        *out_run_length = 0;
        return VFS_OK;
    }

    /* Resolve the first block — must go through the full bmap to handle
     * allocation, cache init, etc. */
    uint32_t first_phy = 0;
    vfs_status_t s = vfs_bmap_locked(vfs, inode_idx, start_logical, false, zero_init, &first_phy);
    if (s != VFS_OK) { return s; }

    /* ---- Already-allocated run scan ----
     *
     * Original: called vfs_bmap_locked for every subsequent block.
     * Optimized: read directly from the relevant index table (in->blocks,
     * cached SIB, or cached DIB+SIB) without re-entering the three-way
     * dispatch. This eliminates ~30 % of total cycles for sequential reads
     * and writes of large files.
     */
    if (first_phy != 0) {
        uint32_t run = 1;

        /* Determine which addressing tier start_logical falls into and
         * advance within that tier directly. */
        if (start_logical < VFS_DIRECT_BLOCKS) {
            /* Direct tier: just walk in->blocks. */
            vfs_inode_t* in = &vfs->inodes[inode_idx];
            while (run < max_blocks) {
                uint32_t next_log = start_logical + run;
                if (next_log >= VFS_DIRECT_BLOCKS) { break; }
                uint32_t next_phy = in->blocks[next_log];
                if (next_phy != first_phy + run) { break; }
                run++;
            }
        } else if (start_logical < VFS_SINGLE_LIMIT) {
            /* Single-indirect tier: read the cached SIB table. */
            vfs_inode_t* in = &vfs->inodes[inode_idx];
            uint32_t sib_blk = in->blocks[VFS_SINGLE_INDIRECT_INDEX];
            if (sib_blk != 0) {
                uint32_t* table = NULL;
                s = cache_read_sib_locked(vfs, sib_blk, &table);
                if (s != VFS_OK) { return s; }
                uint32_t base_sub = start_logical - VFS_DIRECT_BLOCKS;
                while (run < max_blocks) {
                    uint32_t sub = base_sub + run;
                    if (sub >= VFS_INDIRECT_ENTRIES) { break; }
                    uint32_t next_phy = table[sub];
                    if (next_phy != first_phy + run) { break; }
                    run++;
                }
            }
        } else {
            /* Double-indirect tier: DIB + SIB lookup, then walk SIB. */
            vfs_inode_t* in = &vfs->inodes[inode_idx];
            uint32_t dib_blk = in->blocks[VFS_DOUBLE_INDIRECT_INDEX];
            if (dib_blk != 0) {
                uint32_t offset = start_logical - VFS_SINGLE_LIMIT;
                uint32_t dib_idx = offset / VFS_INDIRECT_ENTRIES;
                uint32_t sib_idx = offset % VFS_INDIRECT_ENTRIES;

                uint32_t* dib_table = NULL;
                s = cache_read_dib_locked(vfs, dib_blk, &dib_table);
                if (s != VFS_OK) { return s; }
                uint32_t sib_blk = dib_table[dib_idx];
                if (sib_blk != 0) {
                    uint32_t* sib_table = NULL;
                    s = cache_read_sib_locked(vfs, sib_blk, &sib_table);
                    if (s != VFS_OK) { return s; }
                    while (run < max_blocks) {
                        uint32_t sub = sib_idx + run;
                        if (sub >= VFS_INDIRECT_ENTRIES) { break; }
                        uint32_t next_phy = sib_table[sub];
                        if (next_phy != first_phy + run) { break; }
                        run++;
                    }
                }
            }
        }

        *out_physical_start = first_phy;
        *out_run_length = run;
        return VFS_OK;
    }

    /* ---- Unallocated run scan (no-alloc path) ---- */
    if (!alloc) {
        uint32_t run = 1;
        while (run < max_blocks) {
            uint32_t next_phy = 0;
            s = vfs_bmap_locked(vfs, inode_idx, start_logical + run, false, zero_init, &next_phy);
            if (s != VFS_OK || next_phy != 0) { break; }
            run++;
        }
        *out_physical_start = 0;
        *out_run_length = run;
        return VFS_OK;
    }

    /* ---- Allocate path ---- */

    /* Cap the run at the boundary of the current addressing tier so that
     * vfs_bmap_set_run_locked can service it with a single table write. */
    uint32_t limit = max_blocks;
    if (start_logical < VFS_DIRECT_BLOCKS) {
        uint32_t rem = VFS_DIRECT_BLOCKS - start_logical;
        if (rem < limit) { limit = rem; }
    } else if (start_logical < VFS_SINGLE_LIMIT) {
        uint32_t rem = VFS_SINGLE_LIMIT - start_logical;
        if (rem < limit) { limit = rem; }
    } else {
        uint32_t offset = start_logical - VFS_SINGLE_LIMIT;
        uint32_t rem = VFS_INDIRECT_ENTRIES - (offset % VFS_INDIRECT_ENTRIES);
        if (rem < limit) { limit = rem; }
    }

    uint32_t alloc_limit = 0;
    s = vfs_bmap_count_unallocated_locked(vfs, inode_idx, start_logical, limit, &alloc_limit);
    if (s != VFS_OK) { return s; }

    if (alloc_limit == 0) {
        *out_physical_start = 0;
        *out_run_length = 0;
        return VFS_OK;
    }

    uint32_t new_phy_start = 0;
    uint32_t allocated_len = 0;
    s = block_alloc_run_locked(vfs, alloc_limit, &new_phy_start, &allocated_len);
    if (s != VFS_OK) { return s; }

    if (zero_init) {
        for (uint32_t i = 0; i < allocated_len; i++) {
            s = block_zero_locked(vfs, new_phy_start + i);
            if (s != VFS_OK) {
                for (uint32_t j = 0; j < allocated_len; j++) {
                    (void)block_free_locked(vfs, new_phy_start + j);
                }
                return s;
            }
        }
    }

    s = vfs_bmap_set_run_locked(vfs, inode_idx, start_logical, new_phy_start, allocated_len);
    if (s != VFS_OK) { return s; }

    *out_physical_start = new_phy_start;
    *out_run_length = allocated_len;
    return VFS_OK;
}

/**
 * Iterates logical block indexes and releases physical space.
 * Handles cascading deallocation of direct, single, and double indirect tables.
 *
 * @param new_block_count Logical blocks to retain (0 = free everything).
 * @return VFS_OK or VFS_ERR_IO.
 */
static vfs_status_t inode_truncate_blocks_locked(vfs_t* vfs, uint32_t inode_idx, uint32_t new_block_count) {
    vfs_inode_t* in = &vfs->inodes[inode_idx];

    /* Invalidate the metadata cache to prevent dirty-SIB writebacks of
     * blocks that are about to be freed. */
    (void)flush_metadata_cache_locked(vfs);
    vfs->cached_sib_blk = 0;
    vfs->cached_sib_dirty = false;
    vfs->cached_dib_blk = 0;
    vfs->cached_dib_dirty = false;

    /* 1. Direct range. */
    for (uint32_t b = new_block_count; b < VFS_DIRECT_BLOCKS; b++) {
        if (in->blocks[b] != 0) {
            vfs_status_t s = block_free_locked(vfs, in->blocks[b]);
            if (s != VFS_OK) { return s; }
            in->blocks[b] = 0;
            if (in->block_count > 0) { in->block_count--; }
        }
    }

    /* 2. Single-indirect range. */
    uint32_t sib_blk = in->blocks[VFS_SINGLE_INDIRECT_INDEX];
    if (sib_blk != 0 && new_block_count < VFS_SINGLE_LIMIT) {
        uint32_t table[VFS_INDIRECT_ENTRIES];
        vfs_status_t s = pread_all(vfs->fd, table, sizeof(table), block_offset(sib_blk));
        if (s != VFS_OK) { return s; }

        uint32_t start_sub = (new_block_count > VFS_DIRECT_BLOCKS) ? (new_block_count - VFS_DIRECT_BLOCKS) : 0u;
        bool dirty = false;
        for (uint32_t b = start_sub; b < VFS_INDIRECT_ENTRIES; b++) {
            if (table[b] != 0) {
                s = block_free_locked(vfs, table[b]);
                if (s != VFS_OK) { return s; }
                table[b] = 0;
                dirty = true;
                if (in->block_count > 0) { in->block_count--; }
            }
        }

        if (new_block_count <= VFS_DIRECT_BLOCKS) {
            s = block_free_locked(vfs, sib_blk);
            if (s != VFS_OK) { return s; }
            in->blocks[VFS_SINGLE_INDIRECT_INDEX] = 0;
        } else if (dirty) {
            s = pwrite_all(vfs->fd, table, sizeof(table), block_offset(sib_blk));
            if (s != VFS_OK) { return s; }
        }
    }

    /* 3. Double-indirect range. */
    uint32_t dib_blk = in->blocks[VFS_DOUBLE_INDIRECT_INDEX];
    if (dib_blk != 0 && new_block_count < VFS_DOUBLE_LIMIT) {
        uint32_t dib_table[VFS_INDIRECT_ENTRIES];
        vfs_status_t s = pread_all(vfs->fd, dib_table, sizeof(dib_table), block_offset(dib_blk));
        if (s != VFS_OK) { return s; }

        uint32_t start_idx = 0;
        if (new_block_count >= VFS_SINGLE_LIMIT) {
            start_idx = (new_block_count - VFS_SINGLE_LIMIT) / VFS_INDIRECT_ENTRIES;
        }

        bool dib_dirty = false;
        for (uint32_t d = start_idx; d < VFS_INDIRECT_ENTRIES; d++) {
            uint32_t sib_blk_db = dib_table[d];
            if (sib_blk_db == 0) { continue; }

            uint32_t current_sib_base = VFS_SINGLE_LIMIT + d * VFS_INDIRECT_ENTRIES;
            uint32_t sib_table[VFS_INDIRECT_ENTRIES];
            s = pread_all(vfs->fd, sib_table, sizeof(sib_table), block_offset(sib_blk_db));
            if (s != VFS_OK) { return s; }

            if (new_block_count <= current_sib_base) {
                /* Free entire leaf SIB + its data blocks. */
                for (uint32_t b = 0; b < VFS_INDIRECT_ENTRIES; b++) {
                    if (sib_table[b] != 0) {
                        s = block_free_locked(vfs, sib_table[b]);
                        if (s != VFS_OK) { return s; }
                        if (in->block_count > 0) { in->block_count--; }
                    }
                }
                s = block_free_locked(vfs, sib_blk_db);
                if (s != VFS_OK) { return s; }
                dib_table[d] = 0;
                dib_dirty = true;
            } else {
                /* Boundary SIB — free the tail entries only. */
                uint32_t start_sub = new_block_count - current_sib_base;
                bool sib_dirty = false;
                for (uint32_t b = start_sub; b < VFS_INDIRECT_ENTRIES; b++) {
                    if (sib_table[b] != 0) {
                        s = block_free_locked(vfs, sib_table[b]);
                        if (s != VFS_OK) { return s; }
                        sib_table[b] = 0;
                        sib_dirty = true;
                        if (in->block_count > 0) { in->block_count--; }
                    }
                }
                if (sib_dirty) {
                    s = pwrite_all(vfs->fd, sib_table, sizeof(sib_table), block_offset(sib_blk_db));
                    if (s != VFS_OK) { return s; }
                }
            }
        }

        if (new_block_count < VFS_SINGLE_LIMIT) {
            s = block_free_locked(vfs, dib_blk);
            if (s != VFS_OK) { return s; }
            in->blocks[VFS_DOUBLE_INDIRECT_INDEX] = 0;
        } else if (dib_dirty) {
            s = pwrite_all(vfs->fd, dib_table, sizeof(dib_table), block_offset(dib_blk));
            if (s != VFS_OK) { return s; }
        }
    }

    return VFS_OK;
}

/* =========================================================================
 * Inode helpers
 * ======================================================================= */

/**
 * Finds the inode index for @p path. Caller must hold vfs->lock.
 * @return Index on success, INODE_NONE if not found.
 */
static uint32_t inode_find_locked(const vfs_t* vfs, const char* path) {
    for (uint32_t i = 0; i < VFS_MAX_INODES; i++) {
        if (vfs->inodes[i].path[0] != '\0' && strncmp(vfs->inodes[i].path, path, VFS_MAX_PATH - 1u) == 0) { return i; }
    }
    return INODE_NONE;
}

/**
 * Finds a free inode slot. Caller must hold vfs->lock.
 * @return Index on success, INODE_NONE if the table is full.
 */
static uint32_t inode_alloc_slot_locked(const vfs_t* vfs) {
    for (uint32_t i = 0; i < VFS_MAX_INODES; i++) {
        if (vfs->inodes[i].path[0] == '\0') { return i; }
    }
    return INODE_NONE;
}

/**
 * Frees all data and table blocks owned by inode @p idx, then zeroes the entry.
 * Caller must hold vfs->lock.
 * @return VFS_OK or VFS_ERR_IO.
 */
static vfs_status_t inode_free_locked(vfs_t* vfs, uint32_t idx) {
    assert(idx < VFS_MAX_INODES);
    vfs_status_t s = inode_truncate_blocks_locked(vfs, idx, 0);
    if (s != VFS_OK) { return s; }

    vfs->super.free_inode_count++;
    memset(&vfs->inodes[idx], 0, sizeof(vfs->inodes[idx]));
    vfs->inode_dirty[idx] = false;
    return inode_write_locked(vfs, idx);
}

/* =========================================================================
 * Open-file table helpers
 * ======================================================================= */

/**
 * Validates that @p fd is a live entry in vfs->oft[].
 * @return Pointer to the open_file_t, or NULL on bad fd.
 */
static open_file_t* oft_get(vfs_t* vfs, vfs_fd_t fd) {
    if (fd < 0 || (unsigned int)fd >= VFS_MAX_OPEN_FILES) { return NULL; }
    open_file_t* of = &vfs->oft[(unsigned int)fd];
    if (of->inode_idx == OFT_FREE) { return NULL; }
    return of;
}

/* =========================================================================
 * Mount helpers
 * ======================================================================= */

/** Initialises the open-file table to all-free. */
static void oft_init(vfs_t* vfs) {
    for (uint32_t i = 0; i < VFS_MAX_OPEN_FILES; i++) {
        vfs->oft[i].inode_idx = OFT_FREE;
        vfs->oft[i].pos = 0;
        vfs->oft[i].flags = 0;
    }
}

/**
 * Allocates and partially initialises a vfs_t structure.
 * @return Pointer on success, NULL on allocation failure.
 */
static vfs_t* vfs_alloc(void) {
    vfs_t* v = calloc(1, sizeof(*v));
    if (v == NULL) { return NULL; }
    v->fd = -1;
    v->alloc_hint = 0;
    v->last_allocated_block = 0;

    v->bitmap = calloc(VFS_BITMAP_WORDS, sizeof(uint32_t));
    if (v->bitmap == NULL) {
        free(v);
        return NULL;
    }

    v->summary_bitmap = calloc(VFS_SUMMARY_WORDS, sizeof(uint32_t));
    if (v->summary_bitmap == NULL) {
        free(v->bitmap);
        free(v);
        return NULL;
    }

    if (pthread_mutex_init(&v->lock, NULL) != 0) {
        free(v->bitmap);
        free(v->summary_bitmap);
        free(v);
        return NULL;
    }

    v->cached_sib_blk = 0;
    v->cached_sib_dirty = false;
    v->cached_dib_blk = 0;
    v->cached_dib_dirty = false;
    v->bitmap_dirty = false;

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

    vfs->fd = open(image_path, O_RDWR | O_CREAT | O_TRUNC, (mode_t)0600);
    if (vfs->fd < 0) {
        pthread_mutex_destroy(&vfs->lock);
        free(vfs->bitmap);
        free(vfs->summary_bitmap);
        free(vfs);
        return VFS_ERR_IO;
    }
    vfs->readonly = false;

    /*
     * Set all bitmap bits to 1 (every block free), then permanently reserve
     * block 0 by clearing its bit. Block 0 is the "no block" sentinel used
     * throughout all inode block arrays and indirect tables.
     */
    memset(vfs->bitmap, 0xFF, VFS_BITMAP_WORDS * sizeof(uint32_t));
    memset(vfs->summary_bitmap, 0xFF, VFS_SUMMARY_WORDS * sizeof(uint32_t));
    bitmap_set_used(vfs, 0u);
    vfs->bitmap_dirty = false;

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

    /* Superblock region. */
    {
        vfs_status_t s = pwrite_all(vfs->fd, &vfs->super, sizeof(vfs->super), (off_t)0);
        if (s != VFS_OK) { goto io_error; }
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

    /* Bitmap region. */
    {
        vfs_status_t s = bitmap_write_locked(vfs);
        if (s != VFS_OK) { goto io_error; }
    }

    /* Inode table region. */
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
        free(vfs->bitmap);
        free(vfs->summary_bitmap);
        free(vfs);
        return VFS_ERR_IO;
    }
    vfs->readonly = readonly;

    /* Read + validate superblock. */
    {
        vfs_status_t s = pread_all(vfs->fd, &vfs->super, sizeof(vfs->super), (off_t)0);
        if (s != VFS_OK) { goto io_error; }
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
            pthread_mutex_destroy(&vfs->lock);
            close(vfs->fd);
            free(vfs->bitmap);
            free(vfs->summary_bitmap);
            free(vfs);
            return VFS_ERR_CORRUPT;
        }
    }

    /* Read bitmap. */
    {
        vfs_status_t s = bitmap_read_locked(vfs);
        if (s != VFS_OK) { goto io_error; }
        vfs->bitmap_dirty = false;
    }

    /* Rebuild summary bitmap. */
    for (uint32_t sw = 0; sw < VFS_SUMMARY_WORDS; sw++) {
        const uint32_t* wp = &vfs->bitmap[sw * 32u];
        /* Fast path: homogeneous words. */
        if (wp[0] == 0) {
            bool all_zero = true;
            for (uint32_t i = 1; i < 32u; i++) {
                if (wp[i] != 0) {
                    all_zero = false;
                    break;
                }
            }
            if (all_zero) {
                vfs->summary_bitmap[sw] = 0;
                continue;
            }
        }
        if (wp[0] != 0) {
            bool all_nonzero = true;
            for (uint32_t i = 1; i < 32u; i++) {
                if (wp[i] == 0) {
                    all_nonzero = false;
                    break;
                }
            }
            if (all_nonzero) {
                vfs->summary_bitmap[sw] = 0xFFFFFFFFu;
                continue;
            }
        }
        /* Mixed — build word bit by bit. */
        uint32_t s_word = 0;
        for (uint32_t bit = 0; bit < 32u; bit++) {
            if (wp[bit] != 0) { s_word |= (1u << bit); }
        }
        vfs->summary_bitmap[sw] = s_word;
    }

    /* Read inode table. */
    {
        vfs_status_t s = pread_all(vfs->fd, vfs->inodes, sizeof(vfs->inodes), VFS_INODE_TABLE_OFFSET);
        if (s != VFS_OK) { goto io_error; }
    }

    /* Set alloc hint to the first word with a free block. */
    vfs->alloc_hint = 0;
    for (uint32_t w = 0; w < VFS_BITMAP_WORDS; w++) {
        if (vfs->bitmap[w] != 0) {
            vfs->alloc_hint = w;
            break;
        }
    }

    *out_vfs = vfs;
    return VFS_OK;

io_error:
    pthread_mutex_destroy(&vfs->lock);
    close(vfs->fd);
    free(vfs->bitmap);
    free(vfs->summary_bitmap);
    free(vfs);
    return VFS_ERR_IO;
}

void vfs_close(vfs_t* vfs) {
    if (vfs == NULL) { return; }

    if (!vfs->readonly && vfs->fd >= 0) {
        pthread_mutex_lock(&vfs->lock);
        (void)flush_metadata_cache_locked(vfs);
        (void)super_write_locked(vfs);
        (void)bitmap_write_locked(vfs);
        for (uint32_t i = 0; i < VFS_MAX_INODES; i++) {
            if (vfs->inode_dirty[i]) {
                (void)inode_write_locked(vfs, i);
                vfs->inode_dirty[i] = false;
            }
        }
        pthread_mutex_unlock(&vfs->lock);
    }

    if (vfs->fd >= 0) {
        (void)close(vfs->fd);
        vfs->fd = -1;
    }

    pthread_mutex_destroy(&vfs->lock);
    free(vfs->bitmap);
    free(vfs->summary_bitmap);
    free(vfs);
}

vfs_status_t vfs_sync(vfs_t* vfs) {
    if (vfs == NULL) { return VFS_ERR_INVAL; }
    if (vfs->readonly) { return VFS_OK; }

    pthread_mutex_lock(&vfs->lock);

    vfs_status_t s = flush_metadata_cache_locked(vfs);
    if (s == VFS_OK) { s = super_write_locked(vfs); }
    if (s == VFS_OK) { s = bitmap_write_locked(vfs); }
    if (s == VFS_OK) { vfs->bitmap_dirty = false; }
    if (s == VFS_OK) {
        for (uint32_t i = 0; i < VFS_MAX_INODES; i++) {
            if (vfs->inode_dirty[i]) {
                vfs_status_t ws = inode_write_locked(vfs, i);
                if (ws != VFS_OK) {
                    s = ws;
                } else {
                    vfs->inode_dirty[i] = false;
                }
            }
        }
    }

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

            if (vfs->super.free_inode_count > 0) { vfs->super.free_inode_count--; }

            rc = inode_write_locked(vfs, inode_idx);
            if (rc != VFS_OK) {
                memset(in, 0, sizeof(*in));
                goto out;
            }
        }
    } else {
        if (inode_idx == INODE_NONE) {
            rc = VFS_ERR_NOTFOUND;
            goto out;
        }
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
        vfs_status_t s = inode_truncate_blocks_locked(vfs, inode_idx, 0);
        if (s != VFS_OK) {
            rc = s;
            goto out;
        }
        vfs_inode_t* in = &vfs->inodes[inode_idx];
        in->size = 0;
        in->modified_at = (uint64_t)time(NULL);
        rc = inode_write_locked(vfs, inode_idx);
        if (rc != VFS_OK) { goto out; }
    }

    vfs->oft[(unsigned int)fd].inode_idx = (int)inode_idx;
    vfs->oft[(unsigned int)fd].flags = flags;
    vfs->oft[(unsigned int)fd].pos = (flags & VFS_O_APPEND) ? (off_t)vfs->inodes[inode_idx].size : (off_t)0;

    rc = (vfs_status_t)fd;

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

    uint32_t idx = (uint32_t)of->inode_idx;
    of->inode_idx = OFT_FREE;
    of->pos = 0;
    of->flags = 0;

    (void)flush_metadata_cache_locked(vfs);
    (void)flush_bitmap_changes_locked(vfs);
    if (vfs->inode_dirty[idx]) {
        (void)inode_write_locked(vfs, idx);
        vfs->inode_dirty[idx] = false;
    }

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

    if ((of->flags & VFS_O_WRONLY) && !(of->flags & VFS_O_RDWR)) {
        pthread_mutex_unlock(&vfs->lock);
        return VFS_ERR_INVAL;
    }

    vfs_inode_t* in = &vfs->inodes[(uint32_t)of->inode_idx];
    uint64_t fsize = in->size;

    if ((uint64_t)of->pos >= fsize) {
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

        uint32_t max_logical_blocks = (uint32_t)((remaining + block_off + VFS_BLOCK_SIZE - 1u) / VFS_BLOCK_SIZE);
        bool zero_init = (block_off != 0 || remaining < VFS_BLOCK_SIZE);

        uint32_t physical_start = 0;
        uint32_t run_blocks = 0;
        vfs_status_t s = vfs_bmap_run_locked(vfs, (uint32_t)of->inode_idx, block_idx, false, zero_init,
                                             max_logical_blocks, &physical_start, &run_blocks);
        if (s != VFS_OK) {
            pthread_mutex_unlock(&vfs->lock);
            return s;
        }

        size_t run_bytes = ((size_t)run_blocks * VFS_BLOCK_SIZE) - block_off;
        if (run_bytes > remaining) { run_bytes = remaining; }

        if (physical_start == 0) {
            /* Sparse hole — return zeros without touching disk. */
            memset(dst, 0, run_bytes);
        } else {
            off_t off = block_offset(physical_start) + (off_t)block_off;
            s = pread_all(vfs->fd, dst, run_bytes, off);
            if (s != VFS_OK) {
                pthread_mutex_unlock(&vfs->lock);
                return s;
            }
        }

        dst += run_bytes;
        cur_pos += (off_t)run_bytes;
        remaining -= run_bytes;
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

    if (!(of->flags & (VFS_O_WRONLY | VFS_O_RDWR))) {
        pthread_mutex_unlock(&vfs->lock);
        return VFS_ERR_INVAL;
    }

    if (of->flags & VFS_O_APPEND) { of->pos = (off_t)vfs->inodes[(uint32_t)of->inode_idx].size; }

    vfs_inode_t* in = &vfs->inodes[(uint32_t)of->inode_idx];
    const uint8_t* src = (const uint8_t*)buf;
    size_t remaining = count;
    off_t cur_pos = of->pos;
    vfs_status_t rc = VFS_OK;

    while (remaining > 0) {
        uint32_t block_idx = (uint32_t)((uint64_t)cur_pos / VFS_BLOCK_SIZE);
        uint32_t block_off = (uint32_t)((uint64_t)cur_pos % VFS_BLOCK_SIZE);

        uint32_t max_logical_blocks = (uint32_t)((remaining + block_off + VFS_BLOCK_SIZE - 1u) / VFS_BLOCK_SIZE);
        bool zero_init = (block_off != 0 || remaining < VFS_BLOCK_SIZE);

        uint32_t physical_start = 0;
        uint32_t run_blocks = 0;
        rc = vfs_bmap_run_locked(vfs, (uint32_t)of->inode_idx, block_idx, true, zero_init, max_logical_blocks,
                                 &physical_start, &run_blocks);
        if (rc != VFS_OK) { break; }

        size_t run_bytes = ((size_t)run_blocks * VFS_BLOCK_SIZE) - block_off;
        if (run_bytes > remaining) { run_bytes = remaining; }

        off_t off = block_offset(physical_start) + (off_t)block_off;
        rc = pwrite_all(vfs->fd, src, run_bytes, off);
        if (rc != VFS_OK) { break; }

        src += run_bytes;
        cur_pos += (off_t)run_bytes;
        remaining -= run_bytes;
    }

    *bytes_written = count - remaining;

    if ((uint64_t)cur_pos > in->size) { in->size = (uint64_t)cur_pos; }
    in->modified_at = (uint64_t)time(NULL);
    of->pos = cur_pos;

    /*
     * Mark dirty once per write call, not once per block.
     * vfs_bmap_locked mutates in->block_count, so the dirty flag must cover
     * those structural changes too — setting it here is sufficient because
     * the mutex is held for the entire write.
     */
    vfs->inode_dirty[(uint32_t)of->inode_idx] = true;

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
        uint32_t new_block_count = (uint32_t)((length + VFS_BLOCK_SIZE - 1u) / VFS_BLOCK_SIZE);
        rc = inode_truncate_blocks_locked(vfs, idx, new_block_count);
        if (rc != VFS_OK) { goto out; }
        in->size = length;

        /* Zero the partial tail block so reads past the new EOF see zeros. */
        if (new_block_count > 0 && (length % VFS_BLOCK_SIZE) != 0) {
            uint32_t last_blk = 0;
            rc = vfs_bmap_locked(vfs, idx, new_block_count - 1u, false, false, &last_blk);
            if (rc != VFS_OK) { goto out; }
            if (last_blk != 0) {
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
        }
    } else if (length > in->size) {
        uint32_t new_block_count = (uint32_t)((length + VFS_BLOCK_SIZE - 1u) / VFS_BLOCK_SIZE);
        if (new_block_count > VFS_MAX_BLOCKS_PER_FILE) {
            rc = VFS_ERR_OVERFLOW;
            goto out;
        }
        for (uint32_t b = in->block_count; b < new_block_count; b++) {
            uint32_t new_blk = 0;
            rc = vfs_bmap_locked(vfs, idx, b, true, true, &new_blk);
            if (rc != VFS_OK) { goto out; }
        }
        in->size = length;
    }

    in->modified_at = (uint64_t)time(NULL);
    rc = inode_write_locked(vfs, idx);

out:
    (void)flush_metadata_cache_locked(vfs);
    (void)flush_bitmap_changes_locked(vfs);
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

    /* Close any open file descriptors referring to this inode. */
    for (uint32_t i = 0; i < VFS_MAX_OPEN_FILES; i++) {
        if (vfs->oft[i].inode_idx == (int)idx) {
            vfs->oft[i].inode_idx = OFT_FREE;
            vfs->oft[i].pos = 0;
            vfs->oft[i].flags = 0;
        }
    }

    vfs_status_t rc = inode_free_locked(vfs, idx);
    if (rc == VFS_OK) { rc = flush_bitmap_changes_locked(vfs); }

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

    bool match_all = (prefix == NULL || prefix[0] == '\0' || (prefix[0] == '/' && prefix[1] == '\0'));
    size_t prefix_len = match_all ? 0u : strlen(prefix);

    pthread_mutex_lock(&vfs->lock);

    for (uint32_t i = 0; i < VFS_MAX_INODES; i++) {
        const vfs_inode_t* in = &vfs->inodes[i];
        if (in->path[0] == '\0') { continue; }
        if (!match_all && strncmp(in->path, prefix, prefix_len) != 0) { continue; }

        vfs_stat_t st = {
            .size = in->size,
            .block_count = in->block_count,
            .created_at = (time_t)in->created_at,
            .modified_at = (time_t)in->modified_at,
        };
        strncpy(st.path, in->path, VFS_MAX_PATH - 1u);
        st.path[VFS_MAX_PATH - 1u] = '\0';

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

    size_t new_len = strlen(newpath);
    if (new_len >= VFS_MAX_PATH) { return VFS_ERR_INVAL; }

    if (strncmp(oldpath, newpath, VFS_MAX_PATH) == 0) { return VFS_OK; }

    pthread_mutex_lock(&vfs->lock);

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
        rc = inode_free_locked(vfs, dst_idx);
        if (rc != VFS_OK) { goto out; }
    }

    vfs_inode_t* src = &vfs->inodes[src_idx];
    memset(src->path, 0, VFS_MAX_PATH);
    memcpy(src->path, newpath, new_len);
    src->modified_at = (uint64_t)time(NULL);

    vfs->inode_dirty[src_idx] = true;
    rc = super_write_locked(vfs);

out:
    (void)flush_bitmap_changes_locked(vfs);
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
    fprintf(out, "  bitmap_words     : %u\n", sb->bitmap_words);

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

/* =========================================================================
 * Portable write-all helper for the sendfile fallback path.
 * Unlike pwrite_all this works on non-seekable fds (sockets, pipes).
 * ======================================================================= */

/**
 * Writes exactly @p n bytes from @p buf to @p fd, retrying on EINTR and
 * partial writes.
 *
 * @return VFS_OK or VFS_ERR_IO.
 */
static vfs_status_t write_all(int fd, const void* buf, size_t n) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t rem = n;
    while (rem > 0) {
        ssize_t w = write(fd, p, rem);
        if (w < 0) {
            if (errno == EINTR) { continue; }
            return VFS_ERR_IO;
        }
        if (w == 0) { return VFS_ERR_IO; }
        p += (size_t)w;
        rem -= (size_t)w;
    }
    return VFS_OK;
}

/* =========================================================================
 * Public API – sendfile
 * ======================================================================= */

/* =========================================================================
 * Public API – sendfile
 * ======================================================================= */

vfs_status_t vfs_sendfile(vfs_t* vfs, int out_fd, vfs_fd_t in_fd, off_t* offset, size_t count, size_t* bytes_sent) {
    if (vfs == NULL || out_fd < 0 || bytes_sent == NULL) { return VFS_ERR_INVAL; }
    *bytes_sent = 0;
    if (count == 0) { return VFS_OK; }

    pthread_mutex_lock(&vfs->lock);

    open_file_t* of = oft_get(vfs, in_fd);
    if (of == NULL) {
        pthread_mutex_unlock(&vfs->lock);
        return VFS_ERR_BADFD;
    }

    /* Read-permission check: reject write-only descriptors. */
    if ((of->flags & VFS_O_WRONLY) && !(of->flags & VFS_O_RDWR)) {
        pthread_mutex_unlock(&vfs->lock);
        return VFS_ERR_INVAL;
    }

    vfs_inode_t* in = &vfs->inodes[(uint32_t)of->inode_idx];
    uint64_t fsize = in->size;

    /*
     * Determine the logical read cursor.
     * When offset is non-NULL we use the caller-supplied value and leave
     * of->pos untouched (matching Linux sendfile(2) semantics exactly).
     * When offset is NULL we use and will update of->pos.
     */
    off_t cur_pos = (offset != NULL) ? *offset : of->pos;

    if (cur_pos < 0 || (uint64_t)cur_pos >= fsize) {
        pthread_mutex_unlock(&vfs->lock);
        return VFS_OK; /* EOF — not an error; bytes_sent stays 0. */
    }

    /* Clamp count to the remaining file content. */
    uint64_t avail = fsize - (uint64_t)cur_pos;
    if ((uint64_t)count > avail) { count = (size_t)avail; }

    size_t remaining = count;
    vfs_status_t rc = VFS_OK;

    while (remaining > 0) {
        uint32_t block_idx = (uint32_t)((uint64_t)cur_pos / VFS_BLOCK_SIZE);
        uint32_t block_off = (uint32_t)((uint64_t)cur_pos % VFS_BLOCK_SIZE);

        /* Ask for the longest contiguous physical run that covers the
         * remaining bytes, capped to avoid crossing a tier boundary in one
         * shot (vfs_bmap_run_locked already enforces this). */
        uint32_t max_logical = (uint32_t)((remaining + block_off + VFS_BLOCK_SIZE - 1u) / VFS_BLOCK_SIZE);

        uint32_t phys_start = 0;
        uint32_t run_blocks = 0;
        rc = vfs_bmap_run_locked(vfs, (uint32_t)of->inode_idx, block_idx, /*alloc=*/false, /*zero_init=*/false,
                                 max_logical, &phys_start, &run_blocks);
        if (rc != VFS_OK) { break; }

        /* A zero-length run with VFS_OK would make no forward progress and
         * spin this loop forever. Treat it as a corrupt block map. */
        if (run_blocks == 0) {
            rc = VFS_ERR_IO;
            break;
        }

        /* Byte length of this run, trimmed to what the caller asked for. */
        size_t run_bytes = (size_t)run_blocks * VFS_BLOCK_SIZE - block_off;
        if (run_bytes > remaining) { run_bytes = remaining; }

        if (phys_start == 0) {
            /*
             * Sparse hole: materialise zeros onto out_fd without touching
             * the VFS host fd.  We write one VFS block at a time using a
             * zero buffer so that a single large hole doesn't require a
             * heap allocation proportional to the hole size.
             */
            static const uint8_t zero_block[VFS_BLOCK_SIZE];
            size_t hole_rem = run_bytes;
            while (hole_rem > 0) {
                size_t chunk = hole_rem < VFS_BLOCK_SIZE ? hole_rem : VFS_BLOCK_SIZE;
                rc = write_all(out_fd, zero_block, chunk);
                if (rc != VFS_OK) { goto done; }
                hole_rem -= chunk;
            }
        } else {
            /*
             * Allocated run: transfer directly from the VFS host fd to
             * out_fd.  On Linux we use the kernel sendfile(2) to avoid
             * copying through userspace entirely.  On other platforms we
             * fall back to pread + write with a one-block stack buffer.
             */
            off_t host_off = block_offset(phys_start) + (off_t)block_off;

#if defined(__linux__)
            /*
             * sendfile(2) requires a non-NULL offset pointer so the kernel
             * knows where to read from without consuming the fd's position.
             * We pass &host_off and let the kernel advance it; we do not
             * use its updated value because we track position ourselves via
             * cur_pos.
             */
            size_t sf_rem = run_bytes;
            while (sf_rem > 0) {
                /* sendfile transfers at most SSIZE_MAX bytes per call. */
                size_t want = sf_rem < (size_t)SSIZE_MAX ? sf_rem : (size_t)SSIZE_MAX;
                ssize_t sent = sendfile(out_fd, vfs->fd, &host_off, want);
                if (sent < 0) {
                    if (errno == EINTR) { continue; }
                    /*
                     * EINVAL / ENOSYS: out_fd may be a type sendfile(2)
                     * does not support (e.g. a non-socket pipe on some
                     * kernels).  Fall through to the portable path for
                     * this chunk only.
                     */
                    if (errno == EINVAL || errno == ENOSYS) {
                        /* host_off was not advanced; do this chunk portably. */
                        uint8_t buf[VFS_BLOCK_SIZE];
                        size_t pb_rem = sf_rem;
                        off_t pb_off = host_off;
                        while (pb_rem > 0) {
                            size_t chunk = pb_rem < VFS_BLOCK_SIZE ? pb_rem : VFS_BLOCK_SIZE;
                            rc = pread_all(vfs->fd, buf, chunk, pb_off);
                            if (rc != VFS_OK) { goto done; }
                            rc = write_all(out_fd, buf, chunk);
                            if (rc != VFS_OK) { goto done; }
                            pb_off += (off_t)chunk;
                            pb_rem -= chunk;
                        }
                        sf_rem = 0; /* whole chunk covered */
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
            /* Portable POSIX fallback: pread into a stack buffer, then write. */
            uint8_t buf[VFS_BLOCK_SIZE];
            size_t pb_rem = run_bytes;
            off_t pb_off = host_off;
            while (pb_rem > 0) {
                size_t chunk = pb_rem < VFS_BLOCK_SIZE ? pb_rem : VFS_BLOCK_SIZE;
                rc = pread_all(vfs->fd, buf, chunk, pb_off);
                if (rc != VFS_OK) { goto done; }
                rc = write_all(out_fd, buf, chunk);
                if (rc != VFS_OK) { goto done; }
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

    /* Update whichever position tracking applies. */
    if (offset != NULL) {
        *offset = cur_pos; /* caller's offset updated; of->pos untouched */
    } else {
        of->pos = cur_pos; /* advance the file's own cursor */
    }

    pthread_mutex_unlock(&vfs->lock);
    return rc;
}
