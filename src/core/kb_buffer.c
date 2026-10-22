/*
 * kb_buffer.c — Buffer pool implementation (§5).
 *
 * Clock-sweep eviction:
 *   - Each buffer has a ref_bit set on access.
 *   - On eviction: sweep from clock_hand, clear ref_bits.
 *   - First unpinned buffer with ref_bit=0 is evicted.
 *   - If dirty, write back before eviction.
 */

#include "core/kb_buffer.h"
#include <stdlib.h>
#include <string.h>

/* ══════════════════ Lifecycle ══════════════════ */

kb_status_t
kb_buf_init(kb_buf_pool_t *bp, kb_pool_t *pool, uint32_t num_buffers)
{
    if (bp == NULL || pool == NULL || num_buffers == 0)
        return KB_E_BAD_ARG;

    memset(bp, 0, sizeof(*bp));
    bp->pool        = pool;
    bp->page_size   = pool->sb.page_size;
    bp->num_buffers = num_buffers;
    bp->clock_hand  = 0;

    bp->descriptors = (kb_buf_desc_t *)calloc(num_buffers, sizeof(kb_buf_desc_t));
    if (bp->descriptors == NULL) return KB_E_NO_RAM;

    for (uint32_t i = 0; i < num_buffers; i++) {
        bp->descriptors[i].page_buf = (uint8_t *)malloc(bp->page_size);
        if (bp->descriptors[i].page_buf == NULL) {
            /* Clean up already allocated. */
            for (uint32_t j = 0; j < i; j++) {
                free(bp->descriptors[j].page_buf);
            }
            free(bp->descriptors);
            bp->descriptors = NULL;
            return KB_E_NO_RAM;
        }
        bp->descriptors[i].addr = 0; /* empty */
    }

    return KB_OK;
}

kb_status_t
kb_buf_destroy(kb_buf_pool_t *bp)
{
    if (bp == NULL) return KB_E_BAD_ARG;

    /* Flush all dirty pages. */
    kb_status_t s = kb_buf_flush_all(bp);

    if (bp->descriptors != NULL) {
        for (uint32_t i = 0; i < bp->num_buffers; i++) {
            free(bp->descriptors[i].page_buf);
        }
        free(bp->descriptors);
        bp->descriptors = NULL;
    }

    return s;
}

/* ══════════════════ Internal: find / evict ══════════════════ */

/* Find a descriptor for `addr`. Returns index or -1. */
static int32_t
find_slot(const kb_buf_pool_t *bp, kb_addr_t addr)
{
    for (uint32_t i = 0; i < bp->num_buffers; i++) {
        if (bp->descriptors[i].addr == addr && addr != 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

/* Find an empty slot. Returns index or -1. */
static int32_t
find_empty(const kb_buf_pool_t *bp)
{
    for (uint32_t i = 0; i < bp->num_buffers; i++) {
        if (bp->descriptors[i].addr == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

/*
 * Clock-sweep eviction: find an unpinned victim.
 * Makes at most 2 full passes (one to clear ref bits, one to find victim).
 */
static int32_t
clock_evict(kb_buf_pool_t *bp)
{
    uint32_t n = bp->num_buffers;
    uint32_t scanned = 0;

    while (scanned < 2 * n) {
        kb_buf_desc_t *d = &bp->descriptors[bp->clock_hand];

        if (d->addr != 0 && d->pin_count == 0) {
            if (!d->ref_bit) {
                return (int32_t)bp->clock_hand;
            }
            d->ref_bit = false; /* give it one more chance */
        }

        bp->clock_hand = (bp->clock_hand + 1) % n;
        scanned++;
    }

    return -1; /* all pinned — cannot evict */
}

/* Write back a dirty page. */
static kb_status_t
writeback(kb_buf_pool_t *bp, kb_buf_desc_t *d)
{
    if (!d->dirty || d->addr == 0) return KB_OK;

    kb_status_t s = kb_os_pwrite(bp->pool->pool_fd,
                                  d->page_buf, bp->page_size, d->addr);
    if (s != KB_OK) return s;

    d->dirty = false;
    return KB_OK;
}

/* ══════════════════ Pin / Unpin ══════════════════ */

kb_status_t
kb_buf_pin(kb_buf_pool_t *bp, kb_addr_t addr,
           bool for_write, uint8_t **out_buf)
{
    if (bp == NULL || out_buf == NULL || addr == 0)
        return KB_E_BAD_ARG;

    /* Check if already cached. */
    int32_t slot = find_slot(bp, addr);
    if (slot >= 0) {
        kb_buf_desc_t *d = &bp->descriptors[slot];
        d->pin_count++;
        d->ref_bit = true;
        if (for_write) d->dirty = true;
        bp->total_pins++;
        bp->hit_count++;
        *out_buf = d->page_buf;
        return KB_OK;
    }

    /* Cache miss — find a slot. */
    bp->miss_count++;

    slot = find_empty(bp);
    if (slot < 0) {
        /* Need to evict. */
        slot = clock_evict(bp);
        if (slot < 0) return KB_E_NO_RAM; /* all pinned */

        kb_buf_desc_t *victim = &bp->descriptors[slot];
        kb_status_t s = writeback(bp, victim);
        if (s != KB_OK) return s;

        victim->addr = 0;
        bp->clock_hand = ((uint32_t)slot + 1) % bp->num_buffers;
    }

    /* Load the page from disk. */
    kb_buf_desc_t *d = &bp->descriptors[slot];
    kb_status_t s = kb_os_pread(bp->pool->pool_fd,
                                 d->page_buf, bp->page_size, addr);
    if (s != KB_OK) return s;

    d->addr      = addr;
    d->pin_count = 1;
    d->dirty     = for_write;
    d->ref_bit   = true;
    bp->total_pins++;
    *out_buf = d->page_buf;
    return KB_OK;
}

kb_status_t
kb_buf_unpin(kb_buf_pool_t *bp, kb_addr_t addr, bool dirty)
{
    if (bp == NULL || addr == 0) return KB_E_BAD_ARG;

    int32_t slot = find_slot(bp, addr);
    if (slot < 0) return KB_E_NOT_FOUND;

    kb_buf_desc_t *d = &bp->descriptors[slot];
    if (d->pin_count == 0) return KB_E_BAD_ARG;

    d->pin_count--;
    if (dirty) d->dirty = true;
    if (bp->total_pins > 0) bp->total_pins--;

    return KB_OK;
}

/* ══════════════════ Flush ══════════════════ */

kb_status_t
kb_buf_flush_page(kb_buf_pool_t *bp, kb_addr_t addr)
{
    if (bp == NULL) return KB_E_BAD_ARG;

    int32_t slot = find_slot(bp, addr);
    if (slot < 0) return KB_OK; /* not cached, nothing to flush */

    return writeback(bp, &bp->descriptors[slot]);
}

kb_status_t
kb_buf_flush_all(kb_buf_pool_t *bp)
{
    if (bp == NULL) return KB_E_BAD_ARG;

    for (uint32_t i = 0; i < bp->num_buffers; i++) {
        kb_buf_desc_t *d = &bp->descriptors[i];
        if (d->addr != 0 && d->dirty) {
            kb_status_t s = writeback(bp, d);
            if (s != KB_OK) return s;
        }
    }

    return kb_os_sync(bp->pool->pool_fd);
}
