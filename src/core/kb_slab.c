/*
 * kb_slab.c — Slab allocator implementation (§2.5).
 *
 * Each slab class manages a list of "runs" — groups of pages containing
 * equal-sized slots. The 64-byte run header carries a free mask (u64,
 * up to 63 slots) and is the first slot in the run.
 *
 * Allocation strategy (§2.4.4):
 *   1. If a hint address is given, prefer a run near that address.
 *   2. Otherwise take the lowest-address run with free slots.
 *   3. Within a run, take the lowest-index free slot (CTZ on free_mask).
 *   4. If no run has space, allocate a new run via the page allocator.
 *
 * v2.0 cherry-pick: non_empty_mask tracks which classes have free slots.
 */

#include "core/kb_slab.h"
#include "core/kb_crc32c.h"
#include <stdlib.h>
#include <string.h>

/* ══════════════════ Class table (§2.5) ══════════════════ */

/*
 * | Class (bytes) | Run pages | Slots | Run waste |
 * |   64          |  1        | 63    | 0.0%      |
 * |  128          |  1        | 31    | 1.6%      |
 * |  256          |  1        | 15    | 4.7%      |
 * |  512          |  2        | 15    | 5.5%      |
 * | 1024          |  4        | 15    | 5.9%      |
 * | 2048          |  8        | 15    | 6.1%      |
 * | 4096          | 16        | 15    | 6.2%      |
 * | 8192          | 32        | 15    | 6.2%      |
 * |16384          | 64        | 15    | 6.2%      |
 */
const kb_slab_class_t kb_slab_classes[KB_SLAB_NUM_CLASSES] = {
    {    64,  1, 63 },
    {   128,  1, 31 },
    {   256,  1, 15 },
    {   512,  2, 15 },
    {  1024,  4, 15 },
    {  2048,  8, 15 },
    {  4096, 16, 15 },
    {  8192, 32, 15 },
    { 16384, 64, 15 },
};

int
kb_slab_class_for_size(uint32_t size)
{
    for (uint32_t i = 0; i < KB_SLAB_NUM_CLASSES; i++) {
        if (kb_slab_classes[i].slot_bytes >= size) {
            return (int)i;
        }
    }
    return -1;  /* too large for slab */
}

/* ══════════════════ Internal helpers ══════════════════ */

/*
 * Count trailing zeros. Used to find the lowest free slot.
 */
static inline uint32_t
ctz64(uint64_t v)
{
    if (v == 0) return 64;
#if defined(__GNUC__) || defined(__clang__)
    return (uint32_t)__builtin_ctzll(v);
#else
    uint32_t n = 0;
    while ((v & 1) == 0) { v >>= 1; n++; }
    return n;
#endif
}

/*
 * Serialize a run header to the first 64 bytes of a page buffer.
 */
static void
run_hdr_write(uint8_t *buf, const kb_slab_run_t *run)
{
    memset(buf, 0, KB_SLAB_RUN_HDR_SIZE);
    kb_write_u32(buf + KB_SLAB_RUN_OFF_MAGIC,      KB_MAGIC_SLAB);
    kb_write_u8 (buf + KB_SLAB_RUN_OFF_CLASS,       run->class_idx);
    kb_write_u8 (buf + KB_SLAB_RUN_OFF_FLAGS,       run->flags);
    kb_write_u16(buf + KB_SLAB_RUN_OFF_SLOT_COUNT,  run->slot_count);
    kb_write_u64(buf + KB_SLAB_RUN_OFF_FREE_MASK,   run->free_mask);
    kb_write_u16(buf + KB_SLAB_RUN_OFF_USED_COUNT,  run->used_count);

    /* CRC over first 20 bytes with CRC zeroed. */
    kb_write_u32(buf + KB_SLAB_RUN_OFF_CRC32C, 0);
    uint32_t crc = kb_crc32c_buf(buf, KB_SLAB_RUN_HDR_SIZE);
    kb_write_u32(buf + KB_SLAB_RUN_OFF_CRC32C, crc);
}

/*
 * Deserialize a run header from the first 64 bytes of a page buffer.
 * Validates magic and CRC.
 */
static kb_status_t
run_hdr_read(const uint8_t *buf, kb_slab_run_t *run, kb_addr_t addr)
{
    uint32_t magic = kb_read_u32(buf + KB_SLAB_RUN_OFF_MAGIC);
    if (magic != KB_MAGIC_SLAB) return KB_E_CORRUPT;

    /* Validate CRC. */
    uint8_t tmp[KB_SLAB_RUN_HDR_SIZE];
    memcpy(tmp, buf, KB_SLAB_RUN_HDR_SIZE);
    uint32_t stored = kb_read_u32(tmp + KB_SLAB_RUN_OFF_CRC32C);
    kb_write_u32(tmp + KB_SLAB_RUN_OFF_CRC32C, 0);
    uint32_t computed = kb_crc32c_buf(tmp, KB_SLAB_RUN_HDR_SIZE);
    if (computed != stored) return KB_E_CORRUPT;

    run->addr       = addr;
    run->class_idx  = kb_read_u8(buf + KB_SLAB_RUN_OFF_CLASS);
    run->flags      = kb_read_u8(buf + KB_SLAB_RUN_OFF_FLAGS);
    run->slot_count = kb_read_u16(buf + KB_SLAB_RUN_OFF_SLOT_COUNT);
    run->free_mask  = kb_read_u64(buf + KB_SLAB_RUN_OFF_FREE_MASK);
    run->used_count = kb_read_u16(buf + KB_SLAB_RUN_OFF_USED_COUNT);
    run->dirty      = false;

    return KB_OK;
}

/*
 * Add a run to a class's run list, maintaining address order.
 */
static kb_status_t
class_add_run(kb_slab_class_state_t *cls, const kb_slab_run_t *run)
{
    if (cls->run_count >= cls->run_capacity) {
        uint32_t new_cap = cls->run_capacity == 0 ? 8 : cls->run_capacity * 2;
        kb_slab_run_t *tmp = (kb_slab_run_t *)realloc(cls->runs,
                                                       new_cap * sizeof(kb_slab_run_t));
        if (tmp == NULL) return KB_E_NO_RAM;
        cls->runs = tmp;
        cls->run_capacity = new_cap;
    }

    /* Insert in address order. */
    uint32_t i = cls->run_count;
    while (i > 0 && cls->runs[i - 1].addr > run->addr) {
        cls->runs[i] = cls->runs[i - 1];
        i--;
    }
    cls->runs[i] = *run;
    cls->run_count++;
    return KB_OK;
}

/*
 * Update the non_empty_mask bit for a class.
 */
static void
update_non_empty(kb_slab_alloc_t *sa, uint32_t class_idx)
{
    const kb_slab_class_state_t *cls = &sa->classes[class_idx];
    bool has_free = false;
    for (uint32_t i = 0; i < cls->run_count; i++) {
        if (cls->runs[i].free_mask != 0) {
            has_free = true;
            break;
        }
    }
    if (has_free) {
        sa->non_empty_mask |= (1u << class_idx);
    } else {
        sa->non_empty_mask &= ~(1u << class_idx);
    }
}

/*
 * Flush a dirty run header to disk.
 */
static kb_status_t
flush_run(kb_pool_t *pool, kb_slab_run_t *run)
{
    if (!run->dirty) return KB_OK;

    uint8_t buf[KB_SLAB_RUN_HDR_SIZE];
    run_hdr_write(buf, run);

    kb_status_t s = kb_os_pwrite(pool->pool_fd, buf, KB_SLAB_RUN_HDR_SIZE,
                                 run->addr);
    if (s != KB_OK) return s;
    run->dirty = false;
    return KB_OK;
}

/* ══════════════════ Lifecycle ══════════════════ */

kb_status_t
kb_slab_init(kb_slab_alloc_t *sa, kb_pool_t *pool)
{
    if (sa == NULL || pool == NULL) return KB_E_BAD_ARG;

    memset(sa, 0, sizeof(*sa));
    sa->pool = pool;
    sa->non_empty_mask = 0;

    kb_crc32c_init();

    /*
     * Scan the kind map of each segment for HEADER_SLAB and BLOCK_SLAB pages.
     * For each slab run start, read the run header and add it to the class list.
     */
    for (uint32_t si = 0; si < pool->sb.segment_count; si++) {
        const kb_segment_t *seg = &pool->segments[si];
        uint32_t data_start = (si == 0) ? KB_SEG0_DATA_START : KB_SEG_DATA_START;

        for (uint32_t p = data_start; p < seg->provisioned_pages; p++) {
            uint8_t kind = seg->kind_map[p];
            if (kind != KB_PAGE_KIND_HEADER_SLAB &&
                kind != KB_PAGE_KIND_BLOCK_SLAB) {
                continue;
            }

            /* Read the first 64 bytes to see if this is a run header. */
            kb_addr_t page_addr = seg->base_offset +
                                  (uint64_t)p * pool->sb.page_size;
            uint8_t buf[KB_SLAB_RUN_HDR_SIZE];
            kb_status_t s = kb_os_pread(pool->pool_fd, buf,
                                        KB_SLAB_RUN_HDR_SIZE, page_addr);
            if (s != KB_OK) continue;

            /* Check magic. */
            uint32_t magic = kb_read_u32(buf + KB_SLAB_RUN_OFF_MAGIC);
            if (magic != KB_MAGIC_SLAB) continue;

            kb_slab_run_t run;
            s = run_hdr_read(buf, &run, page_addr);
            if (s != KB_OK) continue;

            if (run.class_idx >= KB_SLAB_NUM_CLASSES) continue;

            s = class_add_run(&sa->classes[run.class_idx], &run);
            if (s != KB_OK) continue;

            /* Skip the remaining pages of this run. */
            uint16_t run_pages = kb_slab_classes[run.class_idx].run_pages;
            p += (uint32_t)(run_pages - 1);
        }
    }

    /* Build non_empty_mask. */
    for (uint32_t c = 0; c < KB_SLAB_NUM_CLASSES; c++) {
        update_non_empty(sa, c);
    }

    return KB_OK;
}

void
kb_slab_destroy(kb_slab_alloc_t *sa)
{
    if (sa == NULL) return;

    /* Flush dirty run headers. */
    for (uint32_t c = 0; c < KB_SLAB_NUM_CLASSES; c++) {
        kb_slab_class_state_t *cls = &sa->classes[c];
        for (uint32_t i = 0; i < cls->run_count; i++) {
            (void)flush_run(sa->pool, &cls->runs[i]);
        }
        free(cls->runs);
        cls->runs = NULL;
        cls->run_count = 0;
        cls->run_capacity = 0;
    }
    sa->non_empty_mask = 0;
}

/* ══════════════════ Slot allocation (§2.4.4) ══════════════════ */

/*
 * Create a new slab run by allocating pages and writing the run header.
 */
static kb_status_t
create_new_run(kb_slab_alloc_t *sa, uint32_t class_idx, uint8_t kind,
               kb_slab_run_t *out_run)
{
    const kb_slab_class_t *cls = &kb_slab_classes[class_idx];

    /* Allocate pages. */
    kb_addr_t addr;
    kb_status_t s = kb_pool_alloc_pages(sa->pool, cls->run_pages, kind, &addr);
    if (s != KB_OK) return s;

    /* Initialize the run descriptor. */
    out_run->addr       = addr;
    out_run->class_idx  = (uint8_t)class_idx;
    out_run->flags      = (kind == KB_PAGE_KIND_HEADER_SLAB)
                          ? KB_SLAB_FLAG_HEADER_SLAB : 0;
    out_run->slot_count = cls->max_slots;
    out_run->used_count = 0;
    out_run->dirty      = true;

    /* Build free mask: bits 1 through slot_count are free (bit 0 = run header). */
    uint64_t mask = 0;
    for (uint16_t i = 1; i <= cls->max_slots; i++) {
        mask |= (1ULL << i);
    }
    out_run->free_mask = mask;

    /* Write the run header to disk. */
    s = flush_run(sa->pool, out_run);
    if (s != KB_OK) {
        /* Free the pages on failure. */
        kb_pool_free_pages(sa->pool, addr, cls->run_pages);
        return s;
    }

    return KB_OK;
}

kb_status_t
kb_slab_alloc(kb_slab_alloc_t *sa, uint32_t size,
              uint8_t kind, kb_addr_t hint_addr,
              kb_addr_t *out_addr, uint32_t *out_slot_size)
{
    if (sa == NULL || out_addr == NULL) return KB_E_BAD_ARG;
    *out_addr = KB_NULL_ADDR;

    int ci = kb_slab_class_for_size(size);
    if (ci < 0) return KB_E_BAD_ARG;
    uint32_t class_idx = (uint32_t)ci;

    const kb_slab_class_t *cls_info = &kb_slab_classes[class_idx];
    kb_slab_class_state_t *cls = &sa->classes[class_idx];

    /* Find a run with free slots. */
    kb_slab_run_t *best_run = NULL;

    if (hint_addr != KB_NULL_ADDR) {
        /* Prefer a run near the hint (locality for siblings, §2.4.4). */
        int64_t best_dist = INT64_MAX;
        for (uint32_t i = 0; i < cls->run_count; i++) {
            if (cls->runs[i].free_mask == 0) continue;
            int64_t dist = (int64_t)cls->runs[i].addr - (int64_t)hint_addr;
            if (dist < 0) dist = -dist;
            if (dist < best_dist) {
                best_dist = dist;
                best_run = &cls->runs[i];
            }
        }
    }

    if (best_run == NULL) {
        /* Lowest-address run with free slots. */
        for (uint32_t i = 0; i < cls->run_count; i++) {
            if (cls->runs[i].free_mask != 0) {
                best_run = &cls->runs[i];
                break;
            }
        }
    }

    if (best_run == NULL) {
        /* No run with space. Create a new one. */
        kb_slab_run_t new_run;
        kb_status_t s = create_new_run(sa, class_idx, kind, &new_run);
        if (s != KB_OK) return s;

        s = class_add_run(cls, &new_run);
        if (s != KB_OK) return s;

        /* The new run is the last one added or in its sorted position. */
        /* Find it. */
        for (uint32_t i = 0; i < cls->run_count; i++) {
            if (cls->runs[i].addr == new_run.addr) {
                best_run = &cls->runs[i];
                break;
            }
        }
        if (best_run == NULL) return KB_E_CORRUPT; /* shouldn't happen */
    }

    /* Allocate the lowest free slot (CTZ). */
    uint32_t slot_idx = ctz64(best_run->free_mask);
    if (slot_idx >= 64) return KB_E_CORRUPT;

    /* Clear the bit. */
    best_run->free_mask &= ~(1ULL << slot_idx);
    best_run->used_count++;
    best_run->dirty = true;

    /* Compute the slot address. */
    *out_addr = best_run->addr + (uint64_t)slot_idx * cls_info->slot_bytes;
    if (out_slot_size) *out_slot_size = cls_info->slot_bytes;

    /* Write-back the updated run header. */
    kb_status_t s = flush_run(sa->pool, best_run);
    if (s != KB_OK) return s;

    update_non_empty(sa, class_idx);
    return KB_OK;
}

/* ══════════════════ Slot free ══════════════════ */

kb_status_t
kb_slab_free(kb_slab_alloc_t *sa, kb_addr_t addr, uint8_t kind)
{
    if (sa == NULL) return KB_E_BAD_ARG;
    (void)kind; /* used for validation in a full impl */

    /* Find which run this slot belongs to. */
    for (uint32_t c = 0; c < KB_SLAB_NUM_CLASSES; c++) {
        kb_slab_class_state_t *cls = &sa->classes[c];
        const kb_slab_class_t *info = &kb_slab_classes[c];
        uint64_t run_bytes = (uint64_t)info->run_pages * sa->pool->sb.page_size;

        for (uint32_t i = 0; i < cls->run_count; i++) {
            kb_slab_run_t *run = &cls->runs[i];
            if (addr >= run->addr && addr < run->addr + run_bytes) {
                /* Found the run. Compute slot index. */
                uint64_t offset = addr - run->addr;
                if (offset % info->slot_bytes != 0) return KB_E_BAD_ARG;
                uint32_t slot_idx = (uint32_t)(offset / info->slot_bytes);
                if (slot_idx == 0) return KB_E_BAD_ARG; /* can't free run header */
                if (slot_idx > info->max_slots) return KB_E_BAD_ARG;

                /* Check it's actually allocated. */
                uint64_t bit = 1ULL << slot_idx;
                if (run->free_mask & bit) return KB_E_BAD_ARG; /* already free */

                run->free_mask |= bit;
                run->used_count--;
                run->dirty = true;

                kb_status_t s = flush_run(sa->pool, run);
                if (s != KB_OK) return s;

                update_non_empty(sa, c);

                /* If the run is now completely empty, release pages back. */
                if (run->used_count == 0) {
                    kb_pool_free_pages(sa->pool, run->addr, info->run_pages);
                    /* Remove from run list. */
                    for (uint32_t j = i; j + 1 < cls->run_count; j++) {
                        cls->runs[j] = cls->runs[j + 1];
                    }
                    cls->run_count--;
                    update_non_empty(sa, c);
                }

                return KB_OK;
            }
        }
    }

    return KB_E_NOT_FOUND;
}

/* ══════════════════ Query ══════════════════ */

kb_status_t
kb_slab_find_run(const kb_slab_alloc_t *sa, kb_addr_t addr,
                 uint8_t kind,
                 const kb_slab_run_t **out_run,
                 uint32_t *out_slot_idx)
{
    if (sa == NULL || out_run == NULL) return KB_E_BAD_ARG;
    (void)kind;

    for (uint32_t c = 0; c < KB_SLAB_NUM_CLASSES; c++) {
        const kb_slab_class_state_t *cls = &sa->classes[c];
        const kb_slab_class_t *info = &kb_slab_classes[c];
        uint64_t run_bytes = (uint64_t)info->run_pages * sa->pool->sb.page_size;

        for (uint32_t i = 0; i < cls->run_count; i++) {
            const kb_slab_run_t *run = &cls->runs[i];
            if (addr >= run->addr && addr < run->addr + run_bytes) {
                uint64_t offset = addr - run->addr;
                if (offset % info->slot_bytes != 0) return KB_E_BAD_ARG;
                *out_run = run;
                if (out_slot_idx) {
                    *out_slot_idx = (uint32_t)(offset / info->slot_bytes);
                }
                return KB_OK;
            }
        }
    }

    return KB_E_NOT_FOUND;
}
