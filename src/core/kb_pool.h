/*
 * kb_pool.h — Pool file manager (§2.1, §2.2, §2.4).
 *
 * Owns the file set (pool.db, index.db, wal.db), the superblock,
 * and the segment array. Provides create, open, close, and stats.
 *
 * This is the "engine core" entry point. Higher layers (transactions,
 * headers, the index) are built on top of the pool.
 */
#ifndef KB_POOL_H
#define KB_POOL_H

#include "core/kb_types.h"
#include "core/kb_os.h"
#include "core/kb_superblock.h"
#include "core/kb_segment.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════ Pool configuration ══════════════════ */

typedef struct kb_pool_config {
    uint16_t page_size;       /* KB_PAGE_SIZE_DEFAULT or KB_PAGE_SIZE_LARGE */
    uint32_t grow_step_mb;    /* 0 → default (64 MiB)                     */
    uint32_t initial_size_mb; /* initial provisioned size; 0 → grow_step   */
    uint32_t flags;           /* KB_OPEN_* flags from kb_os.h              */
} kb_pool_config_t;

/* ══════════════════ Pool statistics (§2.4.7) ══════════════════ */

typedef struct kb_pool_stats {
    uint64_t file_size_bytes;       /* physical file size                  */
    uint64_t provisioned_bytes;     /* bytes provisioned (may < file)      */
    uint64_t hwm_bytes;             /* high-water mark                     */
    uint64_t free_bytes_below_hwm;  /* free space in holes                 */
    uint64_t largest_hole_pages;    /* largest contiguous free run (pages) */
    uint64_t object_count;          /* cached, recomputable                */
    uint32_t segment_count;
} kb_pool_stats_t;

/* ══════════════════ Pool handle ══════════════════ */

/*
 * In-memory pool state. One per open pool.db.
 * Allocated internally by kb_pool_create / kb_pool_open.
 */
typedef struct kb_pool {
    /* File handles. */
    kb_fd_t pool_fd;

    /* Superblock. */
    kb_superblock_t sb;

    /* Segment array. */
    kb_segment_t *segments;     /* sb.segment_count entries              */
    uint32_t      seg_capacity; /* allocated entries                     */

    /* Pool directory path (for resolving index/wal paths). */
    char *dir_path;

    /* Open state. */
    bool is_open;
    bool is_readonly;
} kb_pool_t;

/* ══════════════════ Lifecycle ══════════════════ */

/*
 * Create a new pool at `pool_path`. The file must not exist.
 * Creates pool.db with the superblock and the first segment.
 * Returns an opened pool handle.
 */
kb_status_t kb_pool_create(const char *pool_path,
                           const kb_pool_config_t *cfg,
                           kb_pool_t **out);

/*
 * Open an existing pool at `pool_path`.
 * Loads the superblock and all segment maps.
 */
kb_status_t kb_pool_open(const char *pool_path,
                         const kb_pool_config_t *cfg,
                         kb_pool_t **out);

/*
 * Close the pool: flush dirty maps, write a clean superblock, close files.
 * Frees the pool handle.
 */
kb_status_t kb_pool_close(kb_pool_t *pool);

/* ══════════════════ Stats ══════════════════ */

/*
 * Fill in pool statistics (§2.4.7).
 */
kb_status_t kb_pool_get_stats(const kb_pool_t *pool, kb_pool_stats_t *out);

/* ══════════════════ Growth ══════════════════ */

/*
 * Ensure the pool has at least `needed_pages` free pages.
 * Grows by grow_step if necessary (§2.4.6).
 */
kb_status_t kb_pool_ensure_space(kb_pool_t *pool, uint32_t needed_pages);

/* ══════════════════ Page allocation (cross-segment) ══════════════════ */

/*
 * Allocate `n` contiguous pages anywhere in the pool.
 * Returns the absolute byte offset in pool.db, or KB_NULL_ADDR on failure.
 * Uses lowest-address-first across segments (§2.4.3).
 */
kb_status_t kb_pool_alloc_pages(kb_pool_t *pool, uint32_t n, uint8_t kind,
                                kb_addr_t *out_addr);

/*
 * Free `n` contiguous pages at absolute byte offset `addr`.
 */
kb_status_t kb_pool_free_pages(kb_pool_t *pool, kb_addr_t addr, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* KB_POOL_H */
