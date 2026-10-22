/*
 * kb_block.c — Generic block I/O (§2.7).
 */

#include "core/kb_block.h"
#include "core/kb_crc32c.h"
#include <string.h>

kb_status_t
kb_block_alloc(kb_slab_alloc_t *sa, uint32_t payload_bytes,
               uint8_t kind, kb_addr_t hint,
               kb_addr_t *out_addr, uint32_t *out_total)
{
    if (sa == NULL || out_addr == NULL) return KB_E_BAD_ARG;

    uint32_t total = KB_BLOCK_HDR_SIZE + payload_bytes;

    kb_addr_t addr;
    uint32_t slot_size;
    kb_status_t s = kb_slab_alloc(sa, total, kind, hint, &addr, &slot_size);
    if (s != KB_OK) return s;

    *out_addr = addr;
    if (out_total) *out_total = slot_size;
    return KB_OK;
}

kb_status_t
kb_block_read_hdr(kb_pool_t *pool, kb_addr_t addr, kb_block_hdr_t *hdr)
{
    if (pool == NULL || hdr == NULL) return KB_E_BAD_ARG;

    uint8_t buf[KB_BLOCK_HDR_SIZE];
    kb_status_t s = kb_os_pread(pool->pool_fd, buf, KB_BLOCK_HDR_SIZE, addr);
    if (s != KB_OK) return s;

    hdr->magic        = kb_read_u32(buf + KB_BLK_OFF_MAGIC);
    hdr->item_count   = kb_read_u16(buf + KB_BLK_OFF_ITEM_COUNT);
    hdr->capacity     = kb_read_u16(buf + KB_BLK_OFF_CAPACITY);
    hdr->owner_uref_lo= kb_read_u32(buf + KB_BLK_OFF_OWNER_LO);
    hdr->crc32c       = kb_read_u32(buf + KB_BLK_OFF_CRC32C);
    hdr->addr         = addr;
    hdr->dirty        = false;

    return KB_OK;
}

kb_status_t
kb_block_write(kb_pool_t *pool, kb_addr_t addr,
               uint8_t *buf, uint32_t total_bytes)
{
    if (pool == NULL || buf == NULL || total_bytes < KB_BLOCK_HDR_SIZE)
        return KB_E_BAD_ARG;

    /* Compute CRC over the entire block with CRC field zeroed. */
    kb_write_u32(buf + KB_BLK_OFF_CRC32C, 0);
    uint32_t crc = kb_crc32c_buf(buf, total_bytes);
    kb_write_u32(buf + KB_BLK_OFF_CRC32C, crc);

    return kb_os_pwrite(pool->pool_fd, buf, total_bytes, addr);
}

kb_status_t
kb_block_free(kb_slab_alloc_t *sa, kb_addr_t addr, uint8_t kind)
{
    return kb_slab_free(sa, addr, kind);
}
