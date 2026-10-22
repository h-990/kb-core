/*
 * kb_child.h — Child table operations (§2.8).
 *
 * A child table is a block containing an array of 10-byte entries:
 *   birth_num (u32=4) + child_ref (u40=5) + flags (u8=1) = 10 bytes.
 *
 * Entries are sorted by birth_num for binary search.
 * The table lives in a block-slab slot pointed to by the parent's
 * children_ref field.
 *
 * Child representations (from kb_types.h KB_FLAG_CHILD_REPR_*):
 *   0 = NONE:   no children
 *   1 = INLINE: up to 1 child stored in children_ref directly
 *   2 = ARRAY:  child table block (this module)
 *   3 = TREE:   B+tree of children (deferred to Phase 5)
 */
#ifndef KB_CHILD_H
#define KB_CHILD_H

#include "core/kb_types.h"
#include "core/kb_block.h"
#include "core/kb_header.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════ Child entry layout ══════════════════ */

/* 10-byte on-disk entry. */
#define KB_CHILD_OFF_BIRTH_NUM   0   /* u32 */
#define KB_CHILD_OFF_CHILD_REF   4   /* u40 */
#define KB_CHILD_OFF_FLAGS       9   /* u8  */

/* Child entry flags. */
#define KB_CHILD_FLAG_DETACHED   (1u << 0)  /* child is detached/tombstoned */

/* ══════════════════ In-memory child entry ══════════════════ */

typedef struct kb_child_entry {
    uint32_t birth_num;      /* ordinal under parent (monotonic)        */
    uint64_t child_ref;      /* u40 — child header uref                */
    uint8_t  flags;
} kb_child_entry_t;

/* ══════════════════ Child table operations ══════════════════ */

/*
 * Add a child to a parent header. Handles the transition:
 *   NONE → INLINE → ARRAY (auto-promotes).
 * Assigns birth_num from parent's next_child_ctr.
 * Updates parent's child_count and next_child_ctr.
 *
 * `child_uref`: the unit reference of the child header.
 * `out_birth_num`: the assigned birth_num (for the child's own header).
 */
kb_status_t kb_child_add(kb_slab_alloc_t *sa, kb_pool_t *pool,
                         kb_header_t *parent, uint64_t child_uref,
                         uint32_t *out_birth_num);

/*
 * Remove a child from a parent by birth_num.
 * Marks the entry as detached and decrements parent's child_count.
 * Does NOT free the child header (two-phase delete).
 */
kb_status_t kb_child_remove(kb_pool_t *pool, kb_header_t *parent,
                            uint32_t birth_num);

/*
 * Look up a child by birth_num. Returns the child_ref.
 */
kb_status_t kb_child_find(kb_pool_t *pool, const kb_header_t *parent,
                          uint32_t birth_num, uint64_t *out_child_ref);

/*
 * Iterate all live (non-detached) children. Calls `callback` for each.
 * Returns KB_OK or the first error from the callback.
 */
typedef kb_status_t (*kb_child_iter_fn)(const kb_child_entry_t *entry,
                                        void *ctx);
kb_status_t kb_child_iterate(kb_pool_t *pool, const kb_header_t *parent,
                             kb_child_iter_fn callback, void *ctx);

/*
 * Get the number of live children (from the parent header).
 */
static inline uint32_t kb_child_count(const kb_header_t *parent) {
    return parent->child_count;
}

#ifdef __cplusplus
}
#endif

#endif /* KB_CHILD_H */
