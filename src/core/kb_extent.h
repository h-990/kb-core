/*
 * kb_extent.h — Data extent management (§2.9).
 *
 * An extent is a contiguous run of pages holding object payload data.
 * Small objects (≤1 extent) use the inline ext0_ref/ext0_cap_units
 * fields in the object header. Larger objects use an extent table
 * (a block containing an array of extent entries).
 *
 * Extent representations (from KB_FLAG_EXT_REPR_*):
 *   0 = NONE:    no payload
 *   1 = INLINE:  single extent in ext0_ref/ext0_cap_units
 *   2 = TABLE:   extent table block
 *
 * Extent entry on-disk (8 bytes):
 *   ext_ref (u40=5) + cap_units (u24=3) = 8 bytes.
 *
 * Fragment tag (§2.9, 8 bytes): sits at the start of each extent page
 * to identify ownership during crash recovery.
 */
#ifndef KB_EXTENT_H
#define KB_EXTENT_H

#include "core/kb_types.h"
#include "core/kb_block.h"
#include "core/kb_header.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════ Extent entry layout ══════════════════ */

#define KB_EXTENT_ENTRY_SIZE     8u
#define KB_EXT_ENT_OFF_REF       0   /* u40 — page address / 64 */
#define KB_EXT_ENT_OFF_CAP       5   /* u24 — capacity in units */

/* ══════════════════ In-memory extent entry ══════════════════ */

typedef struct kb_extent_entry {
    uint64_t ext_ref;        /* u40 — extent start address / 64        */
    uint32_t cap_units;      /* u24 — extent capacity in allocation units */
} kb_extent_entry_t;

/* ══════════════════ Extent operations ══════════════════ */

/*
 * Allocate a new extent of `num_pages` contiguous pages for an object.
 * Handles the transition NONE → INLINE → TABLE (auto-promotes).
 * Updates the header's ext0_ref, ext0_cap_units, and EXT_REPR flag.
 *
 * Returns the byte address of the allocated extent.
 */
kb_status_t kb_extent_alloc(kb_slab_alloc_t *sa, kb_pool_t *pool,
                            kb_header_t *hdr, uint32_t num_pages,
                            kb_addr_t *out_addr);

/*
 * Free all extents owned by this header. Releases pages back to the
 * page allocator. Clears ext0 and any extent table.
 */
kb_status_t kb_extent_free_all(kb_slab_alloc_t *sa, kb_pool_t *pool,
                               kb_header_t *hdr);

/*
 * Get the inline extent (ext0). Returns KB_E_NOT_FOUND if none.
 */
kb_status_t kb_extent_get_inline(const kb_header_t *hdr,
                                 kb_extent_entry_t *out);

/*
 * Get the number of extents.
 */
uint32_t kb_extent_count(const kb_header_t *hdr);

/*
 * Iterate all extents. Calls `callback` for each.
 */
typedef kb_status_t (*kb_extent_iter_fn)(const kb_extent_entry_t *entry,
                                         void *ctx);
kb_status_t kb_extent_iterate(kb_pool_t *pool, const kb_header_t *hdr,
                              kb_extent_iter_fn callback, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* KB_EXTENT_H */
