/*
 * kb_store.h — Unified KB Store API (§7).
 *
 * This is the top-level facade that ties pool, slab, headers, child
 * tables, extents, WAL, index, and buffer pool into a single coherent
 * API for creating, opening, and manipulating knowledge bases.
 *
 * All public operations go through this API. Internal modules are
 * not exposed to external callers.
 */
#ifndef KB_STORE_H
#define KB_STORE_H

#include "core/kb_types.h"
#include "core/kb_pool.h"
#include "core/kb_slab.h"
#include "core/kb_header.h"
#include "core/kb_child.h"
#include "core/kb_extent.h"
#include "core/kb_wal.h"
#include "core/kb_index.h"
#include "core/kb_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════ Store handle ══════════════════ */

typedef struct kb_store {
    kb_pool_t       *pool;
    kb_slab_alloc_t  slab;
    kb_index_t       index;
    kb_wal_t         wal;
    kb_buf_pool_t    buf;
    bool             is_open;
    char             pool_path[512];
    char             wal_path[516];
} kb_store_t;

/* Store configuration. */
typedef struct kb_store_config {
    uint32_t page_size;        /* 0 = default 4096                  */
    uint32_t initial_size_mb;  /* 0 = default 2                     */
    uint32_t grow_step_mb;     /* 0 = default 2                     */
    uint32_t buf_pool_slots;   /* 0 = default 256                   */
} kb_store_config_t;

/* ══════════════════ Lifecycle ══════════════════ */

kb_status_t kb_store_create(const char *path, const kb_store_config_t *cfg,
                            kb_store_t *store);
kb_status_t kb_store_open(const char *path, const kb_store_config_t *cfg,
                          kb_store_t *store);
kb_status_t kb_store_close(kb_store_t *store);

/* ══════════════════ Object operations ══════════════════ */

/*
 * Create a new root object (no parent). Returns its header address.
 */
kb_status_t kb_store_create_root(kb_store_t *store, uint16_t type_id,
                                 const char *name,
                                 kb_addr_t *out_addr);

/*
 * Create a child object under `parent_addr`.
 * The birth number is assigned and returned via `out_birth`.
 */
kb_status_t kb_store_create_child(kb_store_t *store, kb_addr_t parent_addr,
                                  uint16_t type_id, const char *name,
                                  kb_addr_t *out_addr, uint32_t *out_birth);

/*
 * Read an object header by address.
 */
kb_status_t kb_store_read_header(kb_store_t *store, kb_addr_t addr,
                                 kb_header_t *out);

/*
 * Look up an object by path_uid.
 */
kb_status_t kb_store_lookup(kb_store_t *store,
                            const uint8_t *path_uid, uint16_t path_len,
                            kb_addr_t *out_addr);

/*
 * Delete an object (two-phase: tombstone first).
 */
kb_status_t kb_store_delete(kb_store_t *store, kb_addr_t addr);

/* ══════════════════ Child enumeration ══════════════════ */

/*
 * List children of an object.
 */
typedef kb_status_t (*kb_store_child_fn)(const kb_child_entry_t *entry,
                                          void *ctx);
kb_status_t kb_store_list_children(kb_store_t *store, kb_addr_t parent_addr,
                                   kb_store_child_fn callback, void *ctx);

/* ══════════════════ Data I/O ══════════════════ */

/*
 * Write payload data to an object. Allocates extent pages as needed.
 * `offset`: byte offset within the object's data stream.
 * `data` / `len`: bytes to write.
 */
kb_status_t kb_store_write_data(kb_store_t *store, kb_addr_t obj_addr,
                                uint64_t offset, const void *data,
                                uint32_t len);

/*
 * Read payload data from an object.
 */
kb_status_t kb_store_read_data(kb_store_t *store, kb_addr_t obj_addr,
                               uint64_t offset, void *buf, uint32_t len);

/* ══════════════════ Transaction control ══════════════════ */

kb_status_t kb_store_begin_tx(kb_store_t *store);
kb_status_t kb_store_commit_tx(kb_store_t *store);
kb_status_t kb_store_abort_tx(kb_store_t *store);

/* ══════════════════ Index operations ══════════════════ */

/*
 * Full ordered scan of all objects in the index.
 */
kb_status_t kb_store_scan_all(kb_store_t *store,
                              kb_index_iter_fn callback, void *ctx);

/*
 * Prefix scan (subtree scan).
 */
kb_status_t kb_store_scan_prefix(kb_store_t *store,
                                 const uint8_t *prefix, uint16_t prefix_len,
                                 kb_index_iter_fn callback, void *ctx);

/* ══════════════════ Maintenance ══════════════════ */

kb_status_t kb_store_checkpoint(kb_store_t *store);
kb_status_t kb_store_flush(kb_store_t *store);

/* ══════════════════ Stats / introspection ══════════════════ */

typedef struct kb_store_stats {
    uint64_t total_pages;
    uint64_t free_pages;
    uint32_t index_height;
    uint32_t index_entries;
    uint64_t buf_hits;
    uint64_t buf_misses;
    uint32_t wal_last_tx;
} kb_store_stats_t;

kb_status_t kb_store_get_stats(const kb_store_t *store, kb_store_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KB_STORE_H */
