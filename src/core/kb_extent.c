/*
 * kb_extent.c — Data extent management (§2.9).
 */

#include "core/kb_extent.h"
#include "core/kb_crc32c.h"
#include <stdlib.h>
#include <string.h>

/* ══════════════════ Helpers ══════════════════ */

static uint32_t
ext_repr(const kb_header_t *hdr)
{
    return (hdr->flags >> KB_FLAG_EXT_REPR_SHIFT) & 3u;
}

static void
set_ext_repr(kb_header_t *hdr, uint32_t repr)
{
    hdr->flags = (hdr->flags & ~KB_FLAG_EXT_REPR_MASK)
               | ((repr & 3u) << KB_FLAG_EXT_REPR_SHIFT);
}

static void
entry_write(uint8_t *buf, const kb_extent_entry_t *e)
{
    kb_write_u40(buf + KB_EXT_ENT_OFF_REF, e->ext_ref);
    kb_write_u24(buf + KB_EXT_ENT_OFF_CAP, e->cap_units);
}

static void
entry_read(const uint8_t *buf, kb_extent_entry_t *e)
{
    e->ext_ref   = kb_read_u40(buf + KB_EXT_ENT_OFF_REF);
    e->cap_units = kb_read_u24(buf + KB_EXT_ENT_OFF_CAP);
}

/* ══════════════════ Allocate extent ══════════════════ */

kb_status_t
kb_extent_alloc(kb_slab_alloc_t *sa, kb_pool_t *pool,
                kb_header_t *hdr, uint32_t num_pages,
                kb_addr_t *out_addr)
{
    if (sa == NULL || pool == NULL || hdr == NULL || out_addr == NULL)
        return KB_E_BAD_ARG;

    /* Allocate contiguous pages. */
    kb_addr_t addr;
    kb_status_t s = kb_pool_alloc_pages(pool, (uint16_t)num_pages,
                                        KB_PAGE_KIND_EXTENT_DATA, &addr);
    if (s != KB_OK) return s;

    uint64_t uref = addr >> KB_UNIT_SHIFT;
    *out_addr = addr;

    uint32_t repr = ext_repr(hdr);

    if (repr == KB_EXT_REPR_NONE) {
        /* No extents yet → INLINE. */
        hdr->ext0_ref       = uref;
        hdr->ext0_cap_units = num_pages * (uint32_t)(pool->sb.page_size >> KB_UNIT_SHIFT);
        set_ext_repr(hdr, KB_EXT_REPR_INLINE);
        hdr->dirty = true;
        return kb_hdr_write(pool, hdr);
    }

    if (repr == KB_EXT_REPR_INLINE) {
        /* One extent already → promote to TABLE. */
        kb_extent_entry_t old_ent = {
            .ext_ref   = hdr->ext0_ref,
            .cap_units = hdr->ext0_cap_units
        };
        kb_extent_entry_t new_ent = {
            .ext_ref   = uref,
            .cap_units = num_pages * (uint32_t)(pool->sb.page_size >> KB_UNIT_SHIFT)
        };

        uint16_t init_cap = 8;
        uint32_t payload  = (uint32_t)init_cap * KB_EXTENT_ENTRY_SIZE;
        uint32_t logical  = KB_BLOCK_HDR_SIZE + payload;
        kb_addr_t blk_addr;
        uint32_t blk_slot;
        s = kb_block_alloc(sa, payload, KB_PAGE_KIND_BLOCK_SLAB,
                           hdr->slot_addr, &blk_addr, &blk_slot);
        if (s != KB_OK) return s;

        uint8_t *buf = (uint8_t *)calloc(1, logical);
        if (buf == NULL) return KB_E_NO_RAM;

        kb_write_u32(buf + KB_BLK_OFF_MAGIC,     KB_MAGIC_EXTENT_BLOCK);
        kb_write_u16(buf + KB_BLK_OFF_ITEM_COUNT, 2);
        kb_write_u16(buf + KB_BLK_OFF_CAPACITY,   init_cap);
        kb_write_u32(buf + KB_BLK_OFF_OWNER_LO,
                     (uint32_t)(hdr->slot_addr >> KB_UNIT_SHIFT));

        uint8_t *entries = buf + KB_BLOCK_HDR_SIZE;
        entry_write(entries, &old_ent);
        entry_write(entries + KB_EXTENT_ENTRY_SIZE, &new_ent);

        s = kb_block_write(pool, blk_addr, buf, logical);
        free(buf);
        if (s != KB_OK) return s;

        /* ext0_ref now points to the table block. */
        hdr->ext0_ref       = blk_addr >> KB_UNIT_SHIFT;
        hdr->ext0_cap_units = 0; /* not used for TABLE repr */
        set_ext_repr(hdr, KB_EXT_REPR_TABLE);
        hdr->dirty = true;
        return kb_hdr_write(pool, hdr);
    }

    /* TABLE: append to the extent table. */
    kb_addr_t blk_addr = hdr->ext0_ref << KB_UNIT_SHIFT;
    kb_block_hdr_t bhdr;
    s = kb_block_read_hdr(pool, blk_addr, &bhdr);
    if (s != KB_OK) return s;

    uint32_t logical = KB_BLOCK_HDR_SIZE +
                       (uint32_t)bhdr.capacity * KB_EXTENT_ENTRY_SIZE;
    uint8_t *buf = (uint8_t *)malloc(logical);
    if (buf == NULL) return KB_E_NO_RAM;

    s = kb_os_pread(pool->pool_fd, buf, logical, blk_addr);
    if (s != KB_OK) { free(buf); return s; }

    uint16_t count = bhdr.item_count;
    uint16_t capacity = bhdr.capacity;

    if (count >= capacity) {
        /* Grow. */
        uint16_t new_cap = (uint16_t)(capacity * 2);
        uint32_t new_logical = KB_BLOCK_HDR_SIZE +
                               (uint32_t)new_cap * KB_EXTENT_ENTRY_SIZE;
        kb_addr_t new_addr;
        uint32_t new_slot;
        s = kb_block_alloc(sa, (uint32_t)new_cap * KB_EXTENT_ENTRY_SIZE,
                           KB_PAGE_KIND_BLOCK_SLAB,
                           hdr->slot_addr, &new_addr, &new_slot);
        if (s != KB_OK) { free(buf); return s; }

        uint8_t *new_buf = (uint8_t *)calloc(1, new_logical);
        if (new_buf == NULL) { free(buf); return KB_E_NO_RAM; }

        memcpy(new_buf + KB_BLOCK_HDR_SIZE, buf + KB_BLOCK_HDR_SIZE,
               (uint32_t)count * KB_EXTENT_ENTRY_SIZE);
        kb_write_u32(new_buf + KB_BLK_OFF_MAGIC, KB_MAGIC_EXTENT_BLOCK);
        kb_write_u16(new_buf + KB_BLK_OFF_CAPACITY, new_cap);
        kb_write_u32(new_buf + KB_BLK_OFF_OWNER_LO,
                     (uint32_t)(hdr->slot_addr >> KB_UNIT_SHIFT));

        kb_block_free(sa, blk_addr, KB_PAGE_KIND_BLOCK_SLAB);
        free(buf);

        buf = new_buf;
        blk_addr = new_addr;
        logical = new_logical;
        capacity = new_cap;

        hdr->ext0_ref = blk_addr >> KB_UNIT_SHIFT;
    }

    /* Append the new entry. */
    kb_extent_entry_t new_ent = {
        .ext_ref   = uref,
        .cap_units = num_pages * (uint32_t)(pool->sb.page_size >> KB_UNIT_SHIFT)
    };
    entry_write(buf + KB_BLOCK_HDR_SIZE + (uint32_t)count * KB_EXTENT_ENTRY_SIZE,
                &new_ent);
    count++;
    kb_write_u16(buf + KB_BLK_OFF_ITEM_COUNT, count);

    s = kb_block_write(pool, blk_addr, buf, logical);
    free(buf);
    if (s != KB_OK) return s;

    hdr->dirty = true;
    return kb_hdr_write(pool, hdr);
}

/* ══════════════════ Free all extents ══════════════════ */

kb_status_t
kb_extent_free_all(kb_slab_alloc_t *sa, kb_pool_t *pool, kb_header_t *hdr)
{
    if (sa == NULL || pool == NULL || hdr == NULL) return KB_E_BAD_ARG;

    uint32_t repr = ext_repr(hdr);

    if (repr == KB_EXT_REPR_NONE) return KB_OK;

    if (repr == KB_EXT_REPR_INLINE) {
        /* Free the extent pages. */
        kb_addr_t addr = hdr->ext0_ref << KB_UNIT_SHIFT;
        uint32_t pages = hdr->ext0_cap_units /
                         (uint32_t)(pool->sb.page_size >> KB_UNIT_SHIFT);
        if (pages > 0) {
            kb_pool_free_pages(pool, addr, (uint16_t)pages);
        }
        hdr->ext0_ref = 0;
        hdr->ext0_cap_units = 0;
        set_ext_repr(hdr, KB_EXT_REPR_NONE);
        hdr->dirty = true;
        return kb_hdr_write(pool, hdr);
    }

    /* TABLE: iterate entries and free each extent, then free the block. */
    kb_addr_t blk_addr = hdr->ext0_ref << KB_UNIT_SHIFT;
    kb_block_hdr_t bhdr;
    kb_status_t s = kb_block_read_hdr(pool, blk_addr, &bhdr);
    if (s != KB_OK) return s;

    uint32_t logical = KB_BLOCK_HDR_SIZE +
                       (uint32_t)bhdr.capacity * KB_EXTENT_ENTRY_SIZE;
    uint8_t *buf = (uint8_t *)malloc(logical);
    if (buf == NULL) return KB_E_NO_RAM;

    s = kb_os_pread(pool->pool_fd, buf, logical, blk_addr);
    if (s != KB_OK) { free(buf); return s; }

    uint8_t *entries = buf + KB_BLOCK_HDR_SIZE;
    for (uint16_t i = 0; i < bhdr.item_count; i++) {
        kb_extent_entry_t e;
        entry_read(entries + (uint32_t)i * KB_EXTENT_ENTRY_SIZE, &e);
        if (e.ext_ref != 0) {
            kb_addr_t ext_addr = e.ext_ref << KB_UNIT_SHIFT;
            uint32_t pages = e.cap_units /
                             (uint32_t)(pool->sb.page_size >> KB_UNIT_SHIFT);
            if (pages > 0) {
                kb_pool_free_pages(pool, ext_addr, (uint16_t)pages);
            }
        }
    }
    free(buf);

    /* Free the table block. */
    kb_block_free(sa, blk_addr, KB_PAGE_KIND_BLOCK_SLAB);

    hdr->ext0_ref = 0;
    hdr->ext0_cap_units = 0;
    set_ext_repr(hdr, KB_EXT_REPR_NONE);
    hdr->dirty = true;
    return kb_hdr_write(pool, hdr);
}

/* ══════════════════ Query ══════════════════ */

kb_status_t
kb_extent_get_inline(const kb_header_t *hdr, kb_extent_entry_t *out)
{
    if (hdr == NULL || out == NULL) return KB_E_BAD_ARG;
    if (ext_repr(hdr) != KB_EXT_REPR_INLINE) return KB_E_NOT_FOUND;

    out->ext_ref   = hdr->ext0_ref;
    out->cap_units = hdr->ext0_cap_units;
    return KB_OK;
}

uint32_t
kb_extent_count(const kb_header_t *hdr)
{
    uint32_t repr = ext_repr(hdr);
    if (repr == KB_EXT_REPR_NONE) return 0;
    if (repr == KB_EXT_REPR_INLINE) return 1;
    /* TABLE: would need to read the block header, but we don't cache count.
     * Return 2+ as a sentinel. Proper count requires disk read. */
    return 2;
}

kb_status_t
kb_extent_iterate(kb_pool_t *pool, const kb_header_t *hdr,
                  kb_extent_iter_fn callback, void *ctx)
{
    if (pool == NULL || hdr == NULL || callback == NULL) return KB_E_BAD_ARG;

    uint32_t repr = ext_repr(hdr);
    if (repr == KB_EXT_REPR_NONE) return KB_OK;

    if (repr == KB_EXT_REPR_INLINE) {
        kb_extent_entry_t e = {
            .ext_ref   = hdr->ext0_ref,
            .cap_units = hdr->ext0_cap_units
        };
        return callback(&e, ctx);
    }

    /* TABLE. */
    kb_addr_t blk_addr = hdr->ext0_ref << KB_UNIT_SHIFT;
    kb_block_hdr_t bhdr;
    kb_status_t s = kb_block_read_hdr(pool, blk_addr, &bhdr);
    if (s != KB_OK) return s;

    uint32_t logical = KB_BLOCK_HDR_SIZE +
                       (uint32_t)bhdr.capacity * KB_EXTENT_ENTRY_SIZE;
    uint8_t *buf = (uint8_t *)malloc(logical);
    if (buf == NULL) return KB_E_NO_RAM;

    s = kb_os_pread(pool->pool_fd, buf, logical, blk_addr);
    if (s != KB_OK) { free(buf); return s; }

    uint8_t *entries = buf + KB_BLOCK_HDR_SIZE;
    for (uint16_t i = 0; i < bhdr.item_count; i++) {
        kb_extent_entry_t e;
        entry_read(entries + (uint32_t)i * KB_EXTENT_ENTRY_SIZE, &e);
        s = callback(&e, ctx);
        if (s != KB_OK) { free(buf); return s; }
    }

    free(buf);
    return KB_OK;
}
