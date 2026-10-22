/*
 * kb_buffer.h — Buffer pool and page cache (§5).
 *
 * Manages a fixed-size pool of page-sized buffers in RAM. Pages are
 * loaded from pool.db on demand, and evicted via a clock/LRU policy
 * when the pool is full.
 *
 * Features:
 *   - Pin/unpin API for safe concurrent access.
 *   - Dirty tracking — dirty pages are written back on eviction or sync.
 *   - Clock-sweep eviction (approximation of LRU with low overhead).
 *
 * Buffer descriptor layout (in-memory only):
 *
 *   addr      — pool.db byte address of the cached page
 *   page_buf  — pointer to the page data
 *   pin_count — number of active pins (0 = evictable)
 *   dirty     — true if modified since last write-back
 *   ref_bit   — used by clock sweep (set on access, cleared on sweep)
 */
#ifndef KB_BUFFER_H
#define KB_BUFFER_H

#include "core/kb_types.h"
#include "core/kb_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════ Buffer descriptor ══════════════════ */

typedef struct kb_buf_desc {
    kb_addr_t  addr;        /* pool.db byte offset, 0 = empty slot     */
    uint8_t   *page_buf;    /* page_size bytes                         */
    uint32_t   pin_count;   /* >0 means in use                         */
    bool       dirty;       /* has been modified since load/flush       */
    bool       ref_bit;     /* clock sweep reference bit                */
} kb_buf_desc_t;

/* ══════════════════ Buffer pool ══════════════════ */

typedef struct kb_buf_pool {
    kb_pool_t      *pool;         /* underlying pool                     */
    kb_buf_desc_t  *descriptors;  /* array of num_buffers descriptors    */
    uint32_t        num_buffers;  /* total number of buffer slots        */
    uint32_t        page_size;
    uint32_t        clock_hand;   /* clock sweep position                */
    uint32_t        total_pins;   /* current count of pinned pages       */
    uint64_t        hit_count;    /* cache hits (stats)                  */
    uint64_t        miss_count;   /* cache misses (stats)                */
} kb_buf_pool_t;

/* ══════════════════ Lifecycle ══════════════════ */

/*
 * Initialize the buffer pool with `num_buffers` page-sized slots.
 * Typical: 256–4096 buffers (1 MB – 16 MB for 4 KB pages).
 */
kb_status_t kb_buf_init(kb_buf_pool_t *bp, kb_pool_t *pool,
                        uint32_t num_buffers);

/*
 * Destroy the buffer pool. Flushes all dirty pages first.
 */
kb_status_t kb_buf_destroy(kb_buf_pool_t *bp);

/* ══════════════════ Pin / Unpin ══════════════════ */

/*
 * Pin a page. If not in the cache, load it from disk.
 * Returns a pointer to the page data via *out_buf.
 * The page remains pinned until kb_buf_unpin is called.
 * Multiple concurrent pins on the same page are allowed.
 *
 * `for_write`: if true, the page is marked dirty immediately
 * (caller intends to modify it).
 */
kb_status_t kb_buf_pin(kb_buf_pool_t *bp, kb_addr_t addr,
                       bool for_write, uint8_t **out_buf);

/*
 * Unpin a page. Decrements pin_count. If dirty, marks for write-back.
 */
kb_status_t kb_buf_unpin(kb_buf_pool_t *bp, kb_addr_t addr, bool dirty);

/* ══════════════════ Flush ══════════════════ */

/*
 * Flush a specific page to disk (if dirty and in cache).
 */
kb_status_t kb_buf_flush_page(kb_buf_pool_t *bp, kb_addr_t addr);

/*
 * Flush all dirty pages to disk.
 */
kb_status_t kb_buf_flush_all(kb_buf_pool_t *bp);

/* ══════════════════ Stats ══════════════════ */

static inline uint64_t kb_buf_hits(const kb_buf_pool_t *bp) {
    return bp->hit_count;
}

static inline uint64_t kb_buf_misses(const kb_buf_pool_t *bp) {
    return bp->miss_count;
}

#ifdef __cplusplus
}
#endif

#endif /* KB_BUFFER_H */
