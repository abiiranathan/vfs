/**
 * @file vfs.c
 * @brief Scaled single-file Virtual Filesystem (VFS) with low write amplification.
 *
 * See vfs.h for the complete API documentation and on-disk layout.
 *
 * ## Implementation notes
 *
 * ### In-memory state
 * A `vfs_t` holds:
 *   - the host file descriptor,
 *   - the fully-decoded superblock (`vfs_super_t`),
 *   - the heap-allocated block allocation bitmap,
 *   - the complete inode table (`vfs_inode_t[VFS_MAX_INODES]`),
 *   - a table of open-file entries (`open_file_t[VFS_MAX_OPEN_FILES]`).
 *
 * The superblock, bitmap, and inode table are loaded entirely into memory on mount
 * and written back by vfs_sync() / vfs_close(). Individual data blocks are
 * read and written directly to/from the host file; they are never cached.
 *
 * ### Locking
 * A single `pthread_mutex_t` serialises every public API call. The helpers
 * that do the real work (suffixed `_locked`) must be called with the mutex
 * already held. Re-entrancy protections are built into iterative functions
 * (e.g. `vfs_list`) by temporarily releasing the lock around client callbacks.
 *
 * ### Block addressing & Indirect translation
 * Logical block indexes are resolved via the block map function `vfs_bmap_locked()`.
 * Files are represented as direct blocks up to logical index 253. Index 254 stores
 * a single indirect index table block, and index 255 stores a double indirect index
 * table block. This allows logical indexing up to VFS_MAX_BLOCKS_PER_FILE blocks.
 *
 * ### Block 0 reservation
 * Physical block 0 is permanently reserved (marked used in the bitmap) and is
 * never handed to any file. This makes 0 a safe "no block allocated" sentinel
 * throughout all inode block arrays, indirect tables, and the vfs_fread sparse-
 * hole detection check (`if (blk == 0) { zero-fill; }`).
 */

#include "vfs.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/** Sentinel value meaning "no inode assigned". */
#define INODE_NONE UINT32_MAX

/** Sentinel value meaning "slot is free" in the open-file table. */
#define OFT_FREE (-1)

/** Offset limits and array indices used during indirect address mapping. */
#define VFS_DIRECT_BLOCKS         254u
#define VFS_SINGLE_INDIRECT_INDEX 254u
#define VFS_DOUBLE_INDIRECT_INDEX 255u

#define VFS_SUMMARY_WORDS ((VFS_BITMAP_WORDS + 31u) / 32u)

/**
 * One entry in the runtime open-file table.
 * `inode_idx` == OFT_FREE means the slot is available.
 */
typedef struct {
    int inode_idx;  /**< Index into vfs->inodes[], or OFT_FREE.    */
    off_t pos;      /**< Current read/write cursor (logical bytes). */
    unsigned flags; /**< The VFS_O_* flags the file was opened with.*/
} open_file_t;

struct vfs_t {
    int fd;                              /**< Host file descriptor.              */
    uint32_t alloc_hint;                 /* Index of first bitmap word that may contain a free block. */
    bool readonly;                       /**< True when mounted read-only.       */
    pthread_mutex_t lock;                /**< Serialises all metadata ops.       */
    vfs_super_t super;                   /**< In-memory superblock.              */
    uint32_t* bitmap;                    /**< Heap-allocated block bitmap.       */
    uint32_t* summary_bitmap;            /**< Purely in-memory summary bitmap.       */
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
        if (w == 0) { return VFS_ERR_IO; }
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

/**
 * Flushes the in-memory superblock structure to the host file.
 * Caller must hold vfs->lock.
 *
 * @return VFS_OK or VFS_ERR_IO.
 */
static vfs_status_t super_write_locked(vfs_t* vfs) {
    return pwrite_all(vfs->fd, &vfs->super, sizeof(vfs->super), (off_t)0);
}

/**
 * Flushes the in-memory block bitmap to the host file.
 * Caller must hold vfs->lock.
 *
 * @return VFS_OK or VFS_ERR_IO.
 */
static vfs_status_t bitmap_write_locked(vfs_t* vfs) {
    return pwrite_all(vfs->fd, vfs->bitmap, VFS_BITMAP_WORDS * sizeof(uint32_t), VFS_BITMAP_OFFSET);
}

/**
 * Flushes a single 32-bit word of the block bitmap to the host file.
 * Used to avoid writing the entire bitmap region during common metadata ops.
 * Caller must hold vfs->lock.
 *
 * @param word_idx Index of the 32-bit word in the bitmap array.
 * @return VFS_OK or VFS_ERR_IO.
 */
static vfs_status_t bitmap_write_word_locked(vfs_t* vfs, uint32_t word_idx) {
    assert(word_idx < VFS_BITMAP_WORDS);
    off_t off = VFS_BITMAP_OFFSET + (off_t)(word_idx * sizeof(uint32_t));
    return pwrite_all(vfs->fd, &vfs->bitmap[word_idx], sizeof(uint32_t), off);
}

/**
 * Reads the block bitmap from the host file into memory.
 * Caller must hold vfs->lock.
 *
 * @return VFS_OK or VFS_ERR_IO.
 */
static vfs_status_t bitmap_read_locked(vfs_t* vfs) {
    return pread_all(vfs->fd, vfs->bitmap, VFS_BITMAP_WORDS * sizeof(uint32_t), VFS_BITMAP_OFFSET);
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
 * Free-block Bitmap helpers
 * ======================================================================= */

/**
 * Tests whether block @p blk is free (bitmap bit == 1).
 * Caller must hold vfs->lock.
 */
static bool bitmap_is_free(const vfs_t* vfs, uint32_t blk) {
    assert(blk < VFS_TOTAL_BLOCKS);
    uint32_t word = blk / 32u;
    uint32_t bit = blk % 32u;
    return (vfs->bitmap[word] & (UINT32_C(1) << bit)) != 0;
}

/**
 * Marks block @p blk as used (clears the bitmap bit).
 * Caller must hold vfs->lock.
 */
static void bitmap_set_used(vfs_t* vfs, uint32_t blk) {
    assert(blk < VFS_TOTAL_BLOCKS);
    uint32_t word = blk / 32u;
    uint32_t bit = blk % 32u;
    vfs->bitmap[word] &= ~(UINT32_C(1) << bit);
}

/**
 * Marks block @p blk as free (sets the bitmap bit).
 * Caller must hold vfs->lock.
 */
static void bitmap_set_free(vfs_t* vfs, uint32_t blk) {
    assert(blk < VFS_TOTAL_BLOCKS);
    uint32_t word = blk / 32u;
    uint32_t bit = blk % 32u;
    vfs->bitmap[word] |= (UINT32_C(1) << bit);
}

/**
 * Allocates one free block and marks it used.
 * Caller must hold vfs->lock.
 *
 * Block 0 is permanently reserved as the "no block" sentinel and is
 * never returned here; the scan begins at index 1.
 *
 * @param[out] out_blk  Block number allocated.
 * @return VFS_OK or VFS_ERR_NOSPACE.
 */
static vfs_status_t block_alloc_locked(vfs_t* vfs, uint32_t* out_blk) {
    uint32_t w_start = vfs->alloc_hint;
    uint32_t sw_start = w_start / 32u;
    uint32_t sbit_start = w_start % 32u;

    uint32_t max_sw = VFS_SUMMARY_WORDS;

    for (uint32_t sw = sw_start; sw < max_sw; sw++) {
        uint32_t s_word = vfs->summary_bitmap[sw];

        /* Mask out bits below our start offset on the first word */
        if (sw == sw_start) { s_word &= (0xFFFFFFFFu << sbit_start); }

        while (s_word != 0) {
            uint32_t s_bit = (uint32_t)__builtin_ctz(s_word);
            uint32_t w = sw * 32u + s_bit;

            if (w >= VFS_BITMAP_WORDS) { break; }

            uint32_t word = vfs->bitmap[w];
            uint32_t bit = (uint32_t)__builtin_ctz(word);
            uint32_t blk = w * 32u + bit;

            /* Special case: block 0 is reserved */
            if (blk == 0) {
                word &= ~UINT32_C(1);
                if (word == 0) {
                    /* Word 0 had no other free blocks besides block 0 */
                    vfs->summary_bitmap[sw] &= ~(1u << s_bit);
                    s_word &= ~(1u << s_bit);
                    continue;
                }
                bit = (uint32_t)__builtin_ctz(word);
                blk = w * 32u + bit;
            }

            if (blk >= VFS_TOTAL_BLOCKS) { break; }

            bitmap_set_used(vfs, blk);
            if (vfs->super.free_block_count > 0) { vfs->super.free_block_count--; }
            *out_blk = blk;

            /* If the word is now fully exhausted, clear its bit in the summary */
            if (vfs->bitmap[w] == 0) {
                vfs->summary_bitmap[sw] &= ~(1u << s_bit);
                vfs->alloc_hint = w + 1;
            } else {
                vfs->alloc_hint = w;
            }

            return bitmap_write_word_locked(vfs, w);
        }
        sbit_start = 0; /* Reset bit offset for subsequent summary words */
    }

    vfs->alloc_hint = VFS_BITMAP_WORDS;
    return VFS_ERR_NOSPACE;
}

/**
 * Frees a previously-allocated block.
 * Caller must hold vfs->lock.
 *
 * @param blk  Physical block number to release.
 * @return VFS_OK or VFS_ERR_IO.
 */
static vfs_status_t block_free_locked(vfs_t* vfs, uint32_t blk) {
    assert(blk < VFS_TOTAL_BLOCKS);
    if (!bitmap_is_free(vfs, blk)) {
        bitmap_set_free(vfs, blk);
        vfs->super.free_block_count++;

        uint32_t word = blk / 32u;

        /* Update the summary bitmap */
        uint32_t sw = word / 32u;
        uint32_t sbit = word % 32u;
        vfs->summary_bitmap[sw] |= (1u << sbit);

        if (word < vfs->alloc_hint) { vfs->alloc_hint = word; }

        return bitmap_write_word_locked(vfs, word);
    }
    return VFS_OK;
}

/* =========================================================================
 * Physical addressing helpers
 * ======================================================================= */

/**
 * Returns the host-file byte offset for physical block @p blk.
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
    static const uint8_t zeros[VFS_BLOCK_SIZE];
    return pwrite_all(vfs->fd, zeros, VFS_BLOCK_SIZE, block_offset(blk));
}

/* =========================================================================
 * Multi-Level Indirect Addressing Map & Truncate
 * ======================================================================= */

/**
 * Resolves a file-relative logical block index to its physical block ID on disk.
 *
 * If @p alloc is set to true, this function will automatically allocate,
 * zero-initialize, and link intermediate tables and blocks as required.
 *
 * @param vfs            Mounted VFS handle.
 * @param inode_idx      Target file inode index.
 * @param logical_block  Target logical block number inside the file.
 * @param alloc          Set to true to allocate missing block structures.
 * @param physical_block Pointer populated with resolved physical block ID.
 * @return VFS_OK, VFS_ERR_OVERFLOW, VFS_ERR_NOSPACE or VFS_ERR_IO.
 */
static vfs_status_t vfs_bmap_locked(vfs_t* vfs, uint32_t inode_idx, uint32_t logical_block, bool alloc,
                                    uint32_t* physical_block) {
    vfs_inode_t* in = &vfs->inodes[inode_idx];
    if (logical_block >= VFS_MAX_BLOCKS_PER_FILE) { return VFS_ERR_OVERFLOW; }

    /* 1. Direct Block */
    if (logical_block < VFS_DIRECT_BLOCKS) {
        uint32_t blk = in->blocks[logical_block];
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
            in->blocks[logical_block] = blk;
            in->block_count++;
        }
        *physical_block = blk;
        return VFS_OK;
    }

    /* 2. Single-Indirect Block */
    uint32_t single_limit = VFS_DIRECT_BLOCKS + 1024u;
    if (logical_block < single_limit) {
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
            sib_created = true;
        }

        uint32_t table[1024];
        if (!sib_created) {
            vfs_status_t s = pread_all(vfs->fd, table, sizeof(table), block_offset(sib_blk));
            if (s != VFS_OK) { return s; }
        } else {
            memset(table, 0, sizeof(table));
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
            s = pwrite_all(vfs->fd, table, sizeof(table), block_offset(sib_blk));
            if (s != VFS_OK) {
                (void)block_free_locked(vfs, blk);
                return s;
            }
            in->block_count++;
        }
        *physical_block = blk;
        return VFS_OK;
    }

    /* 3. Double-Indirect Block */
    uint32_t offset = logical_block - single_limit;
    uint32_t dib_idx = offset / 1024u;
    uint32_t sib_idx = offset % 1024u;

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
        dib_created = true;
    }

    uint32_t dib_table[1024];
    if (!dib_created) {
        vfs_status_t s = pread_all(vfs->fd, dib_table, sizeof(dib_table), block_offset(dib_blk));
        if (s != VFS_OK) { return s; }
    } else {
        memset(dib_table, 0, sizeof(dib_table));
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
        s = pwrite_all(vfs->fd, dib_table, sizeof(dib_table), block_offset(dib_blk));
        if (s != VFS_OK) {
            (void)block_free_locked(vfs, sib_blk);
            return s;
        }
        sib_created = true;
    }

    uint32_t sib_table[1024];
    if (!sib_created) {
        vfs_status_t s = pread_all(vfs->fd, sib_table, sizeof(sib_table), block_offset(sib_blk));
        if (s != VFS_OK) { return s; }
    } else {
        memset(sib_table, 0, sizeof(sib_table));
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
        s = pwrite_all(vfs->fd, sib_table, sizeof(sib_table), block_offset(sib_blk));
        if (s != VFS_OK) {
            (void)block_free_locked(vfs, blk);
            return s;
        }
        in->block_count++;
    }
    *physical_block = blk;
    return VFS_OK;
}

/**
 * Iterates through logical block indexes and recursively releases physical space on disk.
 * Handles cascading deallocations of direct, single, and double indirect structures.
 *
 * @param vfs             Mounted VFS handle.
 * @param inode_idx       Target file inode index.
 * @param new_block_count Remaining block count threshold.
 * @return VFS_OK or VFS_ERR_IO.
 */
static vfs_status_t inode_truncate_blocks_locked(vfs_t* vfs, uint32_t inode_idx, uint32_t new_block_count) {
    vfs_inode_t* in = &vfs->inodes[inode_idx];

    /* 1. Direct Block Range */
    for (uint32_t b = new_block_count; b < VFS_DIRECT_BLOCKS; b++) {
        if (in->blocks[b] != 0) {
            vfs_status_t s = block_free_locked(vfs, in->blocks[b]);
            if (s != VFS_OK) { return s; }
            in->blocks[b] = 0;
            if (in->block_count > 0) { in->block_count--; }
        }
    }

    /* 2. Single-Indirect Range */
    uint32_t sib_blk = in->blocks[VFS_SINGLE_INDIRECT_INDEX];
    if (sib_blk != 0) {
        if (new_block_count < VFS_DIRECT_BLOCKS + 1024u) {
            uint32_t table[1024];
            vfs_status_t s = pread_all(vfs->fd, table, sizeof(table), block_offset(sib_blk));
            if (s != VFS_OK) { return s; }

            uint32_t start_sub = (new_block_count > VFS_DIRECT_BLOCKS) ? (new_block_count - VFS_DIRECT_BLOCKS) : 0u;
            bool dirty = false;
            for (uint32_t b = start_sub; b < 1024u; b++) {
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
    }

    /* 3. Double-Indirect Range */
    uint32_t dib_blk = in->blocks[VFS_DOUBLE_INDIRECT_INDEX];
    if (dib_blk != 0) {
        uint32_t single_limit = VFS_DIRECT_BLOCKS + 1024u;
        if (new_block_count < single_limit + 1024u * 1024u) {
            uint32_t dib_table[1024];
            vfs_status_t s = pread_all(vfs->fd, dib_table, sizeof(dib_table), block_offset(dib_blk));
            if (s != VFS_OK) { return s; }

            uint32_t start_idx = 0;
            if (new_block_count >= single_limit) { start_idx = (new_block_count - single_limit) / 1024u; }

            bool dib_dirty = false;
            for (uint32_t d = start_idx; d < 1024u; d++) {
                uint32_t sib_blk_db = dib_table[d];
                if (sib_blk_db != 0) {
                    uint32_t current_sib_base = single_limit + d * 1024u;
                    if (new_block_count <= current_sib_base) {
                        /* Free entire child single indirect block + all data leaves */
                        uint32_t sib_table[1024];
                        s = pread_all(vfs->fd, sib_table, sizeof(sib_table), block_offset(sib_blk_db));
                        if (s != VFS_OK) { return s; }

                        for (uint32_t b = 0; b < 1024u; b++) {
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
                        /* Boundary single-indirect block inside double indirect array */
                        uint32_t sib_table[1024];
                        s = pread_all(vfs->fd, sib_table, sizeof(sib_table), block_offset(sib_blk_db));
                        if (s != VFS_OK) { return s; }

                        uint32_t start_sub = new_block_count - current_sib_base;
                        bool sib_dirty = false;
                        for (uint32_t b = start_sub; b < 1024u; b++) {
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
            }

            if (new_block_count < single_limit) {
                s = block_free_locked(vfs, dib_blk);
                if (s != VFS_OK) { return s; }
                in->blocks[VFS_DOUBLE_INDIRECT_INDEX] = 0;
            } else if (dib_dirty) {
                s = pwrite_all(vfs->fd, dib_table, sizeof(dib_table), block_offset(dib_blk));
                if (s != VFS_OK) { return s; }
            }
        }
    }

    return VFS_OK;
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
        if (vfs->inodes[i].path[0] != '\0' && strncmp(vfs->inodes[i].path, path, VFS_MAX_PATH - 1u) == 0) { return i; }
    }
    return INODE_NONE;
}

/**
 * Finds a free inode slot inside the in-memory cache.
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
 * Frees all data and table blocks owned by inode @p idx and zeroes the entry.
 * Caller must hold vfs->lock.
 *
 * @return VFS_OK or VFS_ERR_IO.
 */
static vfs_status_t inode_free_locked(vfs_t* vfs, uint32_t idx) {
    assert(idx < VFS_MAX_INODES);
    vfs_inode_t* in = &vfs->inodes[idx];

    vfs_status_t s = inode_truncate_blocks_locked(vfs, idx, 0);
    if (s != VFS_OK) { return s; }

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
 * Allocates and partially initialises a vfs_t structure in-memory.
 * Allocates the heap buffer for the block allocation bitmap.
 *
 * @return Pointer on success, NULL on allocation failure.
 */
static vfs_t* vfs_alloc(void) {
    vfs_t* v = calloc(1, sizeof(*v));
    if (v == NULL) { return NULL; }
    v->fd = -1;
    v->alloc_hint = 0;

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

    vfs->fd = open(image_path, O_RDWR | O_CREAT | O_TRUNC, (mode_t)0600);
    if (vfs->fd < 0) {
        pthread_mutex_destroy(&vfs->lock);
        free(vfs->bitmap);
        free(vfs);
        return VFS_ERR_IO;
    }
    vfs->readonly = false;

    /*
     * Initialise the bitmap with all bits set (all blocks free), then
     * immediately clear bit 0 to permanently reserve physical block 0.
     * Block 0 is never handed to any file; this ensures that 0 remains a
     * safe "no block allocated" sentinel throughout all inode block arrays,
     * indirect tables, and the sparse-hole detection in vfs_fread.
     */
    memset(vfs->bitmap, 0xFF, VFS_BITMAP_WORDS * sizeof(uint32_t));
    /* After setting vfs->bitmap with 0xFF... */
    memset(vfs->summary_bitmap, 0xFF, VFS_SUMMARY_WORDS * sizeof(uint32_t));
    bitmap_set_used(vfs, 0u);

    vfs->super = (vfs_super_t){
        .magic = VFS_MAGIC,
        .version = VFS_VERSION,
        .block_size = VFS_BLOCK_SIZE,
        .max_inodes = VFS_MAX_INODES,
        .total_blocks = VFS_TOTAL_BLOCKS,
        .free_block_count = VFS_TOTAL_BLOCKS - 1u, /* block 0 is reserved */
        .free_inode_count = VFS_MAX_INODES,
        .bitmap_words = VFS_BITMAP_WORDS,
    };

    /* Superblock block region persist */
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

    /* Bitmap block region persist */
    {
        vfs_status_t s = bitmap_write_locked(vfs);
        if (s != VFS_OK) { goto io_error; }
    }

    /* Inodes table block region persist */
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
        free(vfs);
        return VFS_ERR_IO;
    }
    vfs->readonly = readonly;

    /* Validate Superblock */
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
            free(vfs);
            return VFS_ERR_CORRUPT;
        }
    }

    /* Read Bitmap */
    {
        vfs_status_t s = bitmap_read_locked(vfs);
        if (s != VFS_OK) { goto io_error; }
    }

    /* Populate summary bitmap */
    memset(vfs->summary_bitmap, 0, VFS_SUMMARY_WORDS * sizeof(uint32_t));
    for (uint32_t w = 0; w < VFS_BITMAP_WORDS; w++) {
        if (vfs->bitmap[w] != 0) {
            uint32_t sw = w / 32u;
            uint32_t sbit = w % 32u;
            vfs->summary_bitmap[sw] |= (1u << sbit);
        }
    }

    /* Read Inodes Table */
    {
        vfs_status_t s = pread_all(vfs->fd, vfs->inodes, sizeof(vfs->inodes), VFS_INODE_TABLE_OFFSET);
        if (s != VFS_OK) { goto io_error; }
    }

    /* Set alloc hints */
    {
        vfs->alloc_hint = 0;
        for (uint32_t w = 0; w < VFS_BITMAP_WORDS; w++) {
            if (vfs->bitmap[w] != 0) {
                vfs->alloc_hint = w;
                break;
            }
        }
    }

    *out_vfs = vfs;
    return VFS_OK;

io_error:
    pthread_mutex_destroy(&vfs->lock);
    close(vfs->fd);
    free(vfs->bitmap);
    free(vfs);
    return VFS_ERR_IO;
}

void vfs_close(vfs_t* vfs) {
    if (vfs == NULL) { return; }

    if (!vfs->readonly && vfs->fd >= 0) {
        pthread_mutex_lock(&vfs->lock);
        (void)super_write_locked(vfs);
        (void)bitmap_write_locked(vfs);
        (void)inodes_write_locked(vfs);
        pthread_mutex_unlock(&vfs->lock);
    }

    if (vfs->fd >= 0) {
        (void)close(vfs->fd);
        vfs->fd = -1;
    }

    pthread_mutex_destroy(&vfs->lock);
    if (vfs->bitmap) { free(vfs->bitmap); }
    if (vfs->summary_bitmap) { free(vfs->summary_bitmap); }
    free(vfs);
}

vfs_status_t vfs_sync(vfs_t* vfs) {
    if (vfs == NULL) { return VFS_ERR_INVAL; }
    if (vfs->readonly) { return VFS_OK; }

    pthread_mutex_lock(&vfs->lock);

    vfs_status_t s = super_write_locked(vfs);
    if (s == VFS_OK) { s = bitmap_write_locked(vfs); }
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
            in->size = 0;
            in->block_count = 0;

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
        size_t can_read = VFS_BLOCK_SIZE - block_off;
        if (can_read > remaining) { can_read = remaining; }

        uint32_t blk = 0;
        vfs_status_t s = vfs_bmap_locked(vfs, (uint32_t)of->inode_idx, block_idx, false, &blk);
        if (s != VFS_OK) {
            pthread_mutex_unlock(&vfs->lock);
            return s;
        }

        if (blk == 0) {
            /* Sparse hole: physical block was never allocated; return zeroes. */
            memset(dst, 0, can_read);
        } else {
            off_t off = block_offset(blk) + (off_t)block_off;
            s = pread_all(vfs->fd, dst, can_read, off);
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
        size_t can_write = VFS_BLOCK_SIZE - block_off;
        if (can_write > remaining) { can_write = remaining; }

        uint32_t blk = 0;
        rc = vfs_bmap_locked(vfs, (uint32_t)of->inode_idx, block_idx, true, &blk);
        if (rc != VFS_OK) { break; }

        off_t off = block_offset(blk) + (off_t)block_off;
        rc = pwrite_all(vfs->fd, src, can_write, off);
        if (rc != VFS_OK) { break; }

        src += can_write;
        cur_pos += (off_t)can_write;
        remaining -= can_write;
    }

    size_t written = count - remaining;
    *bytes_written = written;

    if ((uint64_t)cur_pos > in->size) { in->size = (uint64_t)cur_pos; }
    in->modified_at = (uint64_t)time(NULL);
    of->pos = cur_pos;

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
        uint32_t new_block_count = (uint32_t)((length + VFS_BLOCK_SIZE - 1u) / VFS_BLOCK_SIZE);

        rc = inode_truncate_blocks_locked(vfs, idx, new_block_count);
        if (rc != VFS_OK) { goto out; }
        in->size = length;

        if (new_block_count > 0 && (length % VFS_BLOCK_SIZE) != 0) {
            uint32_t last_blk = 0;
            rc = vfs_bmap_locked(vfs, idx, new_block_count - 1u, false, &last_blk);
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
            rc = vfs_bmap_locked(vfs, idx, b, true, &new_blk);
            if (rc != VFS_OK) { goto out; }
        }
        in->size = length;
    }

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

    rc = inode_write_locked(vfs, src_idx);
    if (rc != VFS_OK) { goto out; }

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
