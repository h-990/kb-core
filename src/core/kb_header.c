/*
 * kb_header.c — ObjectHeader serialization and CRUD (§2.6).
 *
 * Uses the canonical layout from kb_types.h. All field access goes
 * through the LE accessors — no struct packing.
 */

#include "core/kb_header.h"
#include "core/kb_crc32c.h"
#include <string.h>

/* ══════════════════ Serialize ══════════════════ */

kb_status_t
kb_hdr_serialize(const kb_header_t *hdr, uint8_t buf[KB_HDR_SIZE])
{
    if (hdr == NULL || buf == NULL) return KB_E_BAD_ARG;

    memset(buf, 0, KB_HDR_SIZE);

    kb_write_u8 (buf + KB_HDR_OFF_STATE_REV,      hdr->state_rev);
    kb_write_u8 (buf + KB_HDR_OFF_TYPE_VERSION,    hdr->type_version);
    kb_write_u16(buf + KB_HDR_OFF_TYPE_ID,         hdr->type_id);
    kb_write_u32(buf + KB_HDR_OFF_FLAGS,           hdr->flags);
    /* CRC field zeroed for now — filled after compute. */
    kb_write_u40(buf + KB_HDR_OFF_PARENT_REF,      hdr->parent_ref);
    kb_write_u40(buf + KB_HDR_OFF_CHILDREN_REF,    hdr->children_ref);
    kb_write_u40(buf + KB_HDR_OFF_EXT0_REF,        hdr->ext0_ref);
    kb_write_u24(buf + KB_HDR_OFF_EXT0_CAP_UNITS,  hdr->ext0_cap_units);
    kb_write_u40(buf + KB_HDR_OFF_AUX_REF,         hdr->aux_ref);
    kb_write_u24(buf + KB_HDR_OFF_AUX_LEN,         hdr->aux_len);
    kb_write_u32(buf + KB_HDR_OFF_BIRTH_NUM,       hdr->birth_num);
    kb_write_u32(buf + KB_HDR_OFF_NEXT_CHILD_CTR,  hdr->next_child_ctr);
    kb_write_u32(buf + KB_HDR_OFF_CHILD_COUNT,     hdr->child_count);
    kb_write_u32(buf + KB_HDR_OFF_CHANGE_SEQ,      hdr->change_seq);
    kb_write_u32(buf + KB_HDR_OFF_SLOT_GEN,        hdr->slot_gen);
    kb_write_u48(buf + KB_HDR_OFF_PAYLOAD_LEN,     hdr->payload_len);

    /* CRC: zero field, compute over 64 bytes. */
    kb_write_u32(buf + KB_HDR_OFF_CRC32C, 0);
    uint32_t crc = kb_crc32c_buf(buf, KB_HDR_SIZE);
    kb_write_u32(buf + KB_HDR_OFF_CRC32C, crc);

    return KB_OK;
}

/* ══════════════════ Deserialize ══════════════════ */

kb_status_t
kb_hdr_deserialize(const uint8_t buf[KB_HDR_SIZE], kb_header_t *hdr)
{
    if (buf == NULL || hdr == NULL) return KB_E_BAD_ARG;

    /* Validate CRC. */
    uint8_t tmp[KB_HDR_SIZE];
    memcpy(tmp, buf, KB_HDR_SIZE);
    uint32_t stored = kb_read_u32(tmp + KB_HDR_OFF_CRC32C);
    kb_write_u32(tmp + KB_HDR_OFF_CRC32C, 0);
    uint32_t computed = kb_crc32c_buf(tmp, KB_HDR_SIZE);
    if (computed != stored) return KB_E_CORRUPT;

    /* Parse all fields. */
    memset(hdr, 0, sizeof(*hdr));
    hdr->state_rev      = kb_read_u8 (buf + KB_HDR_OFF_STATE_REV);
    hdr->type_version   = kb_read_u8 (buf + KB_HDR_OFF_TYPE_VERSION);
    hdr->type_id        = kb_read_u16(buf + KB_HDR_OFF_TYPE_ID);
    hdr->flags          = kb_read_u32(buf + KB_HDR_OFF_FLAGS);
    hdr->crc32c         = stored;
    hdr->parent_ref     = kb_read_u40(buf + KB_HDR_OFF_PARENT_REF);
    hdr->children_ref   = kb_read_u40(buf + KB_HDR_OFF_CHILDREN_REF);
    hdr->ext0_ref       = kb_read_u40(buf + KB_HDR_OFF_EXT0_REF);
    hdr->ext0_cap_units = kb_read_u24(buf + KB_HDR_OFF_EXT0_CAP_UNITS);
    hdr->aux_ref        = kb_read_u40(buf + KB_HDR_OFF_AUX_REF);
    hdr->aux_len        = kb_read_u24(buf + KB_HDR_OFF_AUX_LEN);
    hdr->birth_num      = kb_read_u32(buf + KB_HDR_OFF_BIRTH_NUM);
    hdr->next_child_ctr = kb_read_u32(buf + KB_HDR_OFF_NEXT_CHILD_CTR);
    hdr->child_count    = kb_read_u32(buf + KB_HDR_OFF_CHILD_COUNT);
    hdr->change_seq     = kb_read_u32(buf + KB_HDR_OFF_CHANGE_SEQ);
    hdr->slot_gen       = kb_read_u32(buf + KB_HDR_OFF_SLOT_GEN);
    hdr->payload_len    = kb_read_u48(buf + KB_HDR_OFF_PAYLOAD_LEN);
    hdr->dirty          = false;

    return KB_OK;
}

/* ══════════════════ CRUD ══════════════════ */

kb_status_t
kb_hdr_create(kb_slab_alloc_t *sa, kb_header_t *hdr)
{
    if (sa == NULL || hdr == NULL) return KB_E_BAD_ARG;

    /* Default the state to LIVE. */
    hdr->state_rev = KB_STATE_LIVE;

    /* Allocate a class-0 (64-byte) slot from the header slab. */
    kb_addr_t addr;
    uint32_t slot_size;
    kb_status_t s = kb_slab_alloc(sa, KB_HDR_SIZE,
                                  KB_PAGE_KIND_HEADER_SLAB,
                                  KB_NULL_ADDR, &addr, &slot_size);
    if (s != KB_OK) return s;

    hdr->slot_addr = addr;
    hdr->slot_gen  = 1;
    hdr->change_seq = 1;
    hdr->dirty     = true;

    /* Serialize and write to the slot. */
    uint8_t buf[KB_HDR_SIZE];
    s = kb_hdr_serialize(hdr, buf);
    if (s != KB_OK) return s;

    s = kb_os_pwrite(sa->pool->pool_fd, buf, KB_HDR_SIZE, addr);
    if (s != KB_OK) return s;

    hdr->dirty = false;
    return KB_OK;
}

kb_status_t
kb_hdr_read(kb_pool_t *pool, kb_addr_t slot_addr,
            uint32_t expected_gen, kb_header_t *hdr)
{
    if (pool == NULL || hdr == NULL) return KB_E_BAD_ARG;

    uint8_t buf[KB_HDR_SIZE];
    kb_status_t s = kb_os_pread(pool->pool_fd, buf, KB_HDR_SIZE, slot_addr);
    if (s != KB_OK) return s;

    s = kb_hdr_deserialize(buf, hdr);
    if (s != KB_OK) return s;

    hdr->slot_addr = slot_addr;

    /* ABA check (§2.6.3). */
    if (expected_gen != 0 && hdr->slot_gen != expected_gen) {
        return KB_E_STALE_REF;
    }

    return KB_OK;
}

kb_status_t
kb_hdr_write(kb_pool_t *pool, kb_header_t *hdr)
{
    if (pool == NULL || hdr == NULL) return KB_E_BAD_ARG;
    if (!hdr->dirty) return KB_OK;

    hdr->change_seq++;

    uint8_t buf[KB_HDR_SIZE];
    kb_status_t s = kb_hdr_serialize(hdr, buf);
    if (s != KB_OK) return s;

    s = kb_os_pwrite(pool->pool_fd, buf, KB_HDR_SIZE, hdr->slot_addr);
    if (s != KB_OK) return s;

    hdr->dirty = false;
    return KB_OK;
}

kb_status_t
kb_hdr_detach(kb_pool_t *pool, kb_header_t *hdr)
{
    if (pool == NULL || hdr == NULL) return KB_E_BAD_ARG;

    hdr->state_rev    = KB_STATE_DETACHED;
    hdr->parent_ref   = 0;
    hdr->children_ref = 0;
    hdr->child_count  = 0;
    hdr->ext0_ref     = 0;
    hdr->ext0_cap_units = 0;
    hdr->aux_ref      = 0;
    hdr->aux_len      = 0;
    hdr->payload_len  = 0;

    /* Increment slot_gen so stale references detect the reuse (§2.6.3). */
    hdr->slot_gen++;
    hdr->dirty = true;

    return kb_hdr_write(pool, hdr);
}
