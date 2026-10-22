/*
 * kb_index.h — B+Tree path→object index (§3).
 *
 * Maps path_uid byte strings to object header addresses (kb_addr_t).
 * Each B+Tree node occupies one pool page (default 4096 bytes).
 *
 * Node on-disk layout:
 *
 *  LEAF NODE:
 *  Off │ Size │ Field
 *  ────┼──────┼──────────────
 *    0 │  4   │ magic       (KB_MAGIC_IDX_LEAF)
 *    4 │  2   │ num_keys
 *    6 │  2   │ data_end    (offset of last byte of data from node start)
 *    8 │  8   │ next_leaf   (address of right sibling leaf, 0 if none)
 *   16 │  N   │ entries[]   (key_len:u16 + key_data[key_len] + value:u64)
 *
 *  INTERNAL NODE:
 *  Off │ Size │ Field
 *  ────┼──────┼──────────────
 *    0 │  4   │ magic       (KB_MAGIC_IDX_INTL)
 *    4 │  2   │ num_keys
 *    6 │  2   │ level       (leaves=0, above=1+)
 *    8 │  8   │ child_0     (leftmost child address)
 *   16 │  N   │ entries[]   (key_len:u16 + key_data[key_len] + child:u64)
 *
 * v2.0 Cherry-pick — Frozen Prefix Compression:
 *   Keys for objects below a frozen ancestor (KB_FLAG_PATH_FROZEN)
 *   store (frozen_addr:u40, suffix[]) instead of the full path_uid.
 *   The full key is reconstructed by looking up the frozen ancestor's
 *   path and appending the suffix. This is signaled by a reserved
 *   high bit in key_len (bit 15 = compressed flag).
 */
#ifndef KB_INDEX_H
#define KB_INDEX_H

#include "core/kb_types.h"
#include "core/kb_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════ Constants ══════════════════ */

#define KB_MAGIC_IDX_LEAF    0x4641454Cu  /* "LEAF" LE */
#define KB_MAGIC_IDX_INTL    0x4C544E49u  /* "INTL" LE */

#define KB_IDX_NODE_HDR_SIZE 16u

/* Key_len high bit for frozen prefix compression (v2.0). */
#define KB_IDX_KEY_COMPRESSED  (1u << 15)
#define KB_IDX_KEY_LEN_MASK    0x7FFFu

/* ══════════════════ Index state ══════════════════ */

typedef struct kb_index {
    kb_pool_t   *pool;
    kb_addr_t    root_addr;   /* address of root node page               */
    uint32_t     page_size;   /* bytes per node (= pool page size)       */
    uint32_t     height;      /* 0 = single leaf, 1 = root + leaves, …   */
    uint32_t     entry_count; /* total number of key-value pairs         */
} kb_index_t;

/* ══════════════════ Lifecycle ══════════════════ */

/*
 * Create a new, empty index. Allocates the root leaf page.
 */
kb_status_t kb_index_create(kb_pool_t *pool, kb_index_t *idx);

/*
 * Open an existing index by its root address.
 */
kb_status_t kb_index_open(kb_pool_t *pool, kb_addr_t root_addr,
                          kb_index_t *idx);

/* ══════════════════ Operations ══════════════════ */

/*
 * Insert a key→value pair.
 * key: path_uid bytes, key_len: byte count.
 * value: header address (kb_addr_t).
 * Returns KB_E_EXISTS if the key already exists.
 */
kb_status_t kb_index_insert(kb_index_t *idx,
                            const uint8_t *key, uint16_t key_len,
                            kb_addr_t value);

/*
 * Look up a key. Returns the value in *out_value.
 * Returns KB_E_NOT_FOUND if absent.
 */
kb_status_t kb_index_lookup(kb_index_t *idx,
                            const uint8_t *key, uint16_t key_len,
                            kb_addr_t *out_value);

/*
 * Delete a key. Returns KB_E_NOT_FOUND if absent.
 */
kb_status_t kb_index_delete(kb_index_t *idx,
                            const uint8_t *key, uint16_t key_len);

/*
 * Iterate all entries in key order. Calls `callback` for each.
 * Walking leaf next-pointers (no tree descent per entry).
 */
typedef kb_status_t (*kb_index_iter_fn)(const uint8_t *key, uint16_t key_len,
                                        kb_addr_t value, void *ctx);
kb_status_t kb_index_iterate(kb_index_t *idx,
                             kb_index_iter_fn callback, void *ctx);

/*
 * Prefix scan: iterate all keys that start with the given prefix.
 * Uses property 4 (ancestry = byte prefix) and property 5 (subtree
 * is one contiguous key range) from §1.4.3.
 */
kb_status_t kb_index_scan_prefix(kb_index_t *idx,
                                 const uint8_t *prefix, uint16_t prefix_len,
                                 kb_index_iter_fn callback, void *ctx);

/* ══════════════════ Statistics ══════════════════ */

static inline uint32_t kb_index_count(const kb_index_t *idx) {
    return idx->entry_count;
}

static inline uint32_t kb_index_height(const kb_index_t *idx) {
    return idx->height;
}

#ifdef __cplusplus
}
#endif

#endif /* KB_INDEX_H */
