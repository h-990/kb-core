/*
 * kb_header.h — 64-byte ObjectHeader operations (§2.6, §2.7).
 *
 * Uses the canonical on-disk layout defined in kb_types.h:
 *
 *  Off │ Size │ Field             │ Type
 *  ────┼──────┼───────────────────┼──────
 *    0 │  1   │ state_rev         │ u8
 *    1 │  1   │ type_version      │ u8
 *    2 │  2   │ type_id           │ u16
 *    4 │  4   │ flags             │ u32
 *    8 │  4   │ crc32c            │ u32
 *   12 │  5   │ parent_ref        │ u40
 *   17 │  5   │ children_ref      │ u40
 *   22 │  5   │ ext0_ref          │ u40
 *   27 │  3   │ ext0_cap_units    │ u24
 *   30 │  5   │ aux_ref           │ u40
 *   35 │  3   │ aux_len           │ u24
 *   38 │  4   │ birth_num         │ u32
 *   42 │  4   │ next_child_ctr    │ u32
 *   46 │  4   │ child_count       │ u32
 *   50 │  4   │ change_seq        │ u32
 *   54 │  4   │ slot_gen          │ u32
 *   58 │  6   │ payload_len       │ u48
 *  ────┼──────┤
 *   64        │ TOTAL
 *
 * All offsets, flags, and magic come from kb_types.h. This file provides
 * the in-memory struct and serialization/CRUD operations.
 */
#ifndef KB_HEADER_H
#define KB_HEADER_H

#include "core/kb_types.h"
#include "core/kb_slab.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════ In-memory header ══════════════════ */

typedef struct kb_header {
    /* On-disk fields (host byte order). */
    uint8_t  state_rev;       /* KB_STATE_FREE / _LIVE / _DETACHED / _FORWARD */
    uint8_t  type_version;
    uint16_t type_id;
    uint32_t flags;           /* KB_FLAG_* from kb_types.h                     */
    uint32_t crc32c;

    uint64_t parent_ref;      /* u40 — parent header uref                      */
    uint64_t children_ref;    /* u40 — children block uref                     */
    uint64_t ext0_ref;        /* u40 — first extent block uref                 */
    uint32_t ext0_cap_units;  /* u24 — first extent capacity in units          */
    uint64_t aux_ref;         /* u40 — auxiliary block uref                    */
    uint32_t aux_len;         /* u24 — aux block length in bytes               */

    uint32_t birth_num;       /* child's ordinal under parent (§2.8)           */
    uint32_t next_child_ctr;  /* monotonic counter for next child birth_num    */
    uint32_t child_count;     /* number of live children                       */
    uint32_t change_seq;      /* incremented on any mutation                   */
    uint32_t slot_gen;        /* ABA guard (§2.6.3)                            */
    uint64_t payload_len;     /* u48 — total payload size in bytes             */

    /* Runtime (not on-disk). */
    kb_addr_t slot_addr;      /* byte address of the 64-byte slot in pool.db   */
    bool      dirty;
} kb_header_t;

/* ══════════════════ Serialization ══════════════════ */

/*
 * Serialize a header to a 64-byte buffer (LE, CRC computed).
 */
kb_status_t kb_hdr_serialize(const kb_header_t *hdr, uint8_t buf[KB_HDR_SIZE]);

/*
 * Deserialize a header from a 64-byte buffer.
 * Validates CRC. Returns KB_OK or KB_E_CORRUPT.
 */
kb_status_t kb_hdr_deserialize(const uint8_t buf[KB_HDR_SIZE], kb_header_t *hdr);

/* ══════════════════ CRUD via slab ══════════════════ */

/*
 * Allocate a new header slot and write the initial header.
 * Sets hdr->slot_addr and hdr->slot_gen = 1.
 */
kb_status_t kb_hdr_create(kb_slab_alloc_t *sa, kb_header_t *hdr);

/*
 * Read a header from a slot address.
 * If expected_gen != 0, validates against slot_gen (ABA guard §2.6.3);
 * returns KB_E_STALE_REF on mismatch.
 */
kb_status_t kb_hdr_read(kb_pool_t *pool, kb_addr_t slot_addr,
                        uint32_t expected_gen, kb_header_t *hdr);

/*
 * Write a dirty header back to its slot.
 * Increments change_seq and recomputes CRC.
 */
kb_status_t kb_hdr_write(kb_pool_t *pool, kb_header_t *hdr);

/*
 * Mark a header as DETACHED (two-phase delete §2.8).
 * Clears parent/children/extents, increments slot_gen.
 */
kb_status_t kb_hdr_detach(kb_pool_t *pool, kb_header_t *hdr);

#ifdef __cplusplus
}
#endif

#endif /* KB_HEADER_H */
