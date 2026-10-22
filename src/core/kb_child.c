/*
 * kb_child.c — Child table implementation (§2.8).
 *
 * Three representations managed transparently:
 *   NONE:   child_count=0, children_ref=0.
 *   INLINE: child_count=1, children_ref = the single child uref.
 *   ARRAY:  child_count≥2, children_ref = block address of child table.
 *
 * The ARRAY block has a 16-byte block header + N×10-byte entries sorted
 * by birth_num. Binary search for lookups, linear scan for iteration.
 */

#include "core/kb_child.h"
#include "core/kb_crc32c.h"
#include <stdlib.h>
#include <string.h>

/* ══════════════════ Entry serialization ══════════════════ */

static void
entry_write(uint8_t *buf, const kb_child_entry_t *e)
{
    kb_write_u32(buf + KB_CHILD_OFF_BIRTH_NUM, e->birth_num);
    kb_write_u40(buf + KB_CHILD_OFF_CHILD_REF, e->child_ref);
    kb_write_u8 (buf + KB_CHILD_OFF_FLAGS,     e->flags);
}

static void
entry_read(const uint8_t *buf, kb_child_entry_t *e)
{
    e->birth_num = kb_read_u32(buf + KB_CHILD_OFF_BIRTH_NUM);
    e->child_ref = kb_read_u40(buf + KB_CHILD_OFF_CHILD_REF);
    e->flags     = kb_read_u8 (buf + KB_CHILD_OFF_FLAGS);
}

/* ══════════════════ Helpers ══════════════════ */

static uint32_t
child_repr(const kb_header_t *parent)
{
    return (parent->flags >> KB_FLAG_CHILD_REPR_SHIFT) & 3u;
}

static void
set_child_repr(kb_header_t *parent, uint32_t repr)
{
    parent->flags = (parent->flags & ~KB_FLAG_CHILD_REPR_MASK)
                  | ((repr & 3u) << KB_FLAG_CHILD_REPR_SHIFT);
}

/*
 * Read the full child table block into a buffer.
 * Returns the allocated buffer (caller must free) and item_count.
 */
static kb_status_t
read_child_block(kb_pool_t *pool, kb_addr_t block_addr,
                 uint8_t **out_buf, uint32_t *out_total,
                 uint16_t *out_count, uint16_t *out_capacity)
{
    /* Read the block header first. */
    kb_block_hdr_t bhdr;
    kb_status_t s = kb_block_read_hdr(pool, block_addr, &bhdr);
    if (s != KB_OK) return s;
    if (bhdr.magic != KB_MAGIC_CHILD_BLOCK) return KB_E_CORRUPT;

    uint32_t total = KB_BLOCK_HDR_SIZE +
                     (uint32_t)bhdr.capacity * KB_CHILD_ENTRY_SIZE;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (buf == NULL) return KB_E_NO_RAM;

    s = kb_os_pread(pool->pool_fd, buf, total, block_addr);
    if (s != KB_OK) { free(buf); return s; }

    /* Validate CRC. */
    uint32_t stored_crc = kb_read_u32(buf + KB_BLK_OFF_CRC32C);
    kb_write_u32(buf + KB_BLK_OFF_CRC32C, 0);
    uint32_t computed = kb_crc32c_buf(buf, total);
    if (computed != stored_crc) { free(buf); return KB_E_CORRUPT; }

    *out_buf      = buf;
    *out_total    = total;
    *out_count    = bhdr.item_count;
    *out_capacity = bhdr.capacity;
    return KB_OK;
}

/*
 * Binary search for birth_num in a sorted entry array.
 * Returns the index if found, or -(insertion_point + 1) if not found.
 */
static int32_t
bsearch_birth(const uint8_t *entries, uint16_t count, uint32_t birth_num)
{
    int32_t lo = 0, hi = (int32_t)count - 1;
    while (lo <= hi) {
        int32_t mid = lo + (hi - lo) / 2;
        uint32_t mid_bn = kb_read_u32(entries + (uint32_t)mid * KB_CHILD_ENTRY_SIZE
                                      + KB_CHILD_OFF_BIRTH_NUM);
        if (mid_bn == birth_num) return mid;
        if (mid_bn < birth_num) lo = mid + 1;
        else hi = mid - 1;
    }
    return -(lo + 1);
}

/* ══════════════════ Add child ══════════════════ */

kb_status_t
kb_child_add(kb_slab_alloc_t *sa, kb_pool_t *pool,
             kb_header_t *parent, uint64_t child_uref,
             uint32_t *out_birth_num)
{
    if (sa == NULL || pool == NULL || parent == NULL) return KB_E_BAD_ARG;

    uint32_t birth = parent->next_child_ctr;
    parent->next_child_ctr++;

    uint32_t repr = child_repr(parent);

    if (repr == KB_CHILD_REPR_NONE) {
        /* No children yet → INLINE. */
        parent->children_ref = child_uref;
        parent->child_count  = 1;
        parent->birth_num    = birth; /* store the single child's birth_num */
        set_child_repr(parent, KB_CHILD_REPR_INLINE);
        parent->dirty = true;

        if (out_birth_num) *out_birth_num = birth;
        return kb_hdr_write(pool, parent);
    }

    if (repr == KB_CHILD_REPR_INLINE) {
        /* One child → promote to ARRAY with 2 entries. */
        uint64_t old_ref = parent->children_ref;
        uint32_t old_bn  = parent->birth_num; /* the existing child's birth_num */

        /* Allocate a child block. Initial capacity = 8 entries. */
        uint16_t init_cap = 8;
        uint32_t payload = (uint32_t)init_cap * KB_CHILD_ENTRY_SIZE;
        uint32_t logical = KB_BLOCK_HDR_SIZE + payload;
        kb_addr_t blk_addr;
        uint32_t blk_slot;
        kb_status_t s = kb_block_alloc(sa, payload, KB_PAGE_KIND_BLOCK_SLAB,
                                       parent->slot_addr, &blk_addr, &blk_slot);
        if (s != KB_OK) return s;

        /* Build the block in memory (logical size). */
        uint8_t *buf = (uint8_t *)calloc(1, logical);
        if (buf == NULL) return KB_E_NO_RAM;

        kb_write_u32(buf + KB_BLK_OFF_MAGIC,      KB_MAGIC_CHILD_BLOCK);
        kb_write_u16(buf + KB_BLK_OFF_ITEM_COUNT,  2);
        kb_write_u16(buf + KB_BLK_OFF_CAPACITY,    init_cap);
        kb_write_u32(buf + KB_BLK_OFF_OWNER_LO,
                     (uint32_t)(parent->slot_addr >> KB_UNIT_SHIFT));

        /* Entries sorted by birth_num. */
        uint8_t *entries = buf + KB_BLOCK_HDR_SIZE;
        kb_child_entry_t e0 = { old_bn, old_ref, 0 };
        kb_child_entry_t e1 = { birth,  child_uref, 0 };

        /* Ensure sorted order. */
        if (e0.birth_num < e1.birth_num) {
            entry_write(entries, &e0);
            entry_write(entries + KB_CHILD_ENTRY_SIZE, &e1);
        } else {
            entry_write(entries, &e1);
            entry_write(entries + KB_CHILD_ENTRY_SIZE, &e0);
        }

        s = kb_block_write(pool, blk_addr, buf, logical);
        free(buf);
        if (s != KB_OK) return s;

        parent->children_ref = blk_addr >> KB_UNIT_SHIFT;
        parent->child_count  = 2;
        set_child_repr(parent, KB_CHILD_REPR_ARRAY);
        parent->dirty = true;

        if (out_birth_num) *out_birth_num = birth;
        return kb_hdr_write(pool, parent);
    }

    /* ARRAY representation: append to the sorted table. */
    kb_addr_t blk_addr = parent->children_ref << KB_UNIT_SHIFT;
    uint8_t *buf;
    uint32_t blk_total;
    uint16_t count, capacity;
    kb_status_t s = read_child_block(pool, blk_addr, &buf, &blk_total,
                                     &count, &capacity);
    if (s != KB_OK) return s;

    if (count >= capacity) {
        /* Need to grow — allocate a bigger block, copy, free old. */
        uint16_t new_cap = (uint16_t)(capacity * 2);
        uint32_t new_payload = (uint32_t)new_cap * KB_CHILD_ENTRY_SIZE;
        uint32_t new_logical = KB_BLOCK_HDR_SIZE + new_payload;
        kb_addr_t new_addr;
        uint32_t new_slot;
        s = kb_block_alloc(sa, new_payload, KB_PAGE_KIND_BLOCK_SLAB,
                           parent->slot_addr, &new_addr, &new_slot);
        if (s != KB_OK) { free(buf); return s; }

        uint8_t *new_buf = (uint8_t *)calloc(1, new_logical);
        if (new_buf == NULL) { free(buf); return KB_E_NO_RAM; }

        /* Copy existing entries. */
        memcpy(new_buf + KB_BLOCK_HDR_SIZE, buf + KB_BLOCK_HDR_SIZE,
               (uint32_t)count * KB_CHILD_ENTRY_SIZE);
        kb_write_u32(new_buf + KB_BLK_OFF_MAGIC, KB_MAGIC_CHILD_BLOCK);
        kb_write_u16(new_buf + KB_BLK_OFF_CAPACITY, new_cap);
        kb_write_u32(new_buf + KB_BLK_OFF_OWNER_LO,
                     (uint32_t)(parent->slot_addr >> KB_UNIT_SHIFT));

        /* Free old block. */
        kb_block_free(sa, blk_addr, KB_PAGE_KIND_BLOCK_SLAB);
        free(buf);

        buf = new_buf;
        blk_addr = new_addr;
        blk_total = new_logical;
        capacity = new_cap;

        parent->children_ref = blk_addr >> KB_UNIT_SHIFT;
    }

    /* Insert the new entry in sorted position. */
    uint8_t *entries = buf + KB_BLOCK_HDR_SIZE;
    int32_t pos = bsearch_birth(entries, count, birth);
    uint32_t insert_idx;
    if (pos >= 0) {
        /* Shouldn't happen — birth_num is monotonic. */
        free(buf);
        return KB_E_EXISTS;
    }
    insert_idx = (uint32_t)(-(pos + 1));

    /* Shift entries right. */
    if (insert_idx < count) {
        memmove(entries + (insert_idx + 1) * KB_CHILD_ENTRY_SIZE,
                entries + insert_idx * KB_CHILD_ENTRY_SIZE,
                (uint32_t)(count - insert_idx) * KB_CHILD_ENTRY_SIZE);
    }

    kb_child_entry_t new_entry = { birth, child_uref, 0 };
    entry_write(entries + insert_idx * KB_CHILD_ENTRY_SIZE, &new_entry);

    count++;
    kb_write_u16(buf + KB_BLK_OFF_ITEM_COUNT, count);

    s = kb_block_write(pool, blk_addr, buf, blk_total);
    free(buf);
    if (s != KB_OK) return s;

    parent->child_count = count;
    parent->dirty = true;

    if (out_birth_num) *out_birth_num = birth;
    return kb_hdr_write(pool, parent);
}

/* ══════════════════ Remove child ══════════════════ */

kb_status_t
kb_child_remove(kb_pool_t *pool, kb_header_t *parent, uint32_t birth_num)
{
    if (pool == NULL || parent == NULL) return KB_E_BAD_ARG;

    uint32_t repr = child_repr(parent);

    if (repr == KB_CHILD_REPR_NONE) return KB_E_NOT_FOUND;

    if (repr == KB_CHILD_REPR_INLINE) {
        if (parent->birth_num != birth_num) return KB_E_NOT_FOUND;
        parent->children_ref = 0;
        parent->child_count  = 0;
        set_child_repr(parent, KB_CHILD_REPR_NONE);
        parent->dirty = true;
        return kb_hdr_write(pool, parent);
    }

    /* ARRAY. */
    kb_addr_t blk_addr = parent->children_ref << KB_UNIT_SHIFT;
    uint8_t *buf;
    uint32_t blk_total;
    uint16_t count, capacity;
    kb_status_t s = read_child_block(pool, blk_addr, &buf, &blk_total,
                                     &count, &capacity);
    if (s != KB_OK) return s;

    uint8_t *entries = buf + KB_BLOCK_HDR_SIZE;
    int32_t pos = bsearch_birth(entries, count, birth_num);
    if (pos < 0) { free(buf); return KB_E_NOT_FOUND; }

    /* Mark entry as detached. */
    uint8_t *ent = entries + (uint32_t)pos * KB_CHILD_ENTRY_SIZE;
    kb_write_u8(ent + KB_CHILD_OFF_FLAGS,
                kb_read_u8(ent + KB_CHILD_OFF_FLAGS) | KB_CHILD_FLAG_DETACHED);

    s = kb_block_write(pool, blk_addr, buf, blk_total);
    free(buf);
    if (s != KB_OK) return s;

    parent->child_count--;
    parent->dirty = true;
    return kb_hdr_write(pool, parent);
}

/* ══════════════════ Find child ══════════════════ */

kb_status_t
kb_child_find(kb_pool_t *pool, const kb_header_t *parent,
              uint32_t birth_num, uint64_t *out_child_ref)
{
    if (pool == NULL || parent == NULL || out_child_ref == NULL)
        return KB_E_BAD_ARG;

    uint32_t repr = child_repr(parent);

    if (repr == KB_CHILD_REPR_NONE) return KB_E_NOT_FOUND;

    if (repr == KB_CHILD_REPR_INLINE) {
        if (parent->birth_num != birth_num) return KB_E_NOT_FOUND;
        *out_child_ref = parent->children_ref;
        return KB_OK;
    }

    /* ARRAY. */
    kb_addr_t blk_addr = parent->children_ref << KB_UNIT_SHIFT;
    uint8_t *buf;
    uint32_t blk_total;
    uint16_t count, capacity;
    kb_status_t s = read_child_block(pool, blk_addr, &buf, &blk_total,
                                     &count, &capacity);
    if (s != KB_OK) return s;

    uint8_t *entries = buf + KB_BLOCK_HDR_SIZE;
    int32_t pos = bsearch_birth(entries, count, birth_num);
    if (pos < 0) { free(buf); return KB_E_NOT_FOUND; }

    /* Check not detached. */
    uint8_t flags = kb_read_u8(entries + (uint32_t)pos * KB_CHILD_ENTRY_SIZE
                               + KB_CHILD_OFF_FLAGS);
    if (flags & KB_CHILD_FLAG_DETACHED) { free(buf); return KB_E_NOT_FOUND; }

    *out_child_ref = kb_read_u40(entries + (uint32_t)pos * KB_CHILD_ENTRY_SIZE
                                 + KB_CHILD_OFF_CHILD_REF);
    free(buf);
    return KB_OK;
}

/* ══════════════════ Iterate children ══════════════════ */

kb_status_t
kb_child_iterate(kb_pool_t *pool, const kb_header_t *parent,
                 kb_child_iter_fn callback, void *ctx)
{
    if (pool == NULL || parent == NULL || callback == NULL)
        return KB_E_BAD_ARG;

    uint32_t repr = child_repr(parent);

    if (repr == KB_CHILD_REPR_NONE) return KB_OK;

    if (repr == KB_CHILD_REPR_INLINE) {
        kb_child_entry_t e = {
            .birth_num = parent->birth_num,
            .child_ref = parent->children_ref,
            .flags     = 0
        };
        return callback(&e, ctx);
    }

    /* ARRAY. */
    kb_addr_t blk_addr = parent->children_ref << KB_UNIT_SHIFT;
    uint8_t *buf;
    uint32_t blk_total;
    uint16_t count, capacity;
    kb_status_t s = read_child_block(pool, blk_addr, &buf, &blk_total,
                                     &count, &capacity);
    if (s != KB_OK) return s;

    uint8_t *entries = buf + KB_BLOCK_HDR_SIZE;
    for (uint16_t i = 0; i < count; i++) {
        kb_child_entry_t e;
        entry_read(entries + (uint32_t)i * KB_CHILD_ENTRY_SIZE, &e);
        if (e.flags & KB_CHILD_FLAG_DETACHED) continue;
        s = callback(&e, ctx);
        if (s != KB_OK) { free(buf); return s; }
    }

    free(buf);
    return KB_OK;
}
