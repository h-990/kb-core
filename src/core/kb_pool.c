/*
 * kb_pool.c — Pool file manager implementation (§2.1, §2.2, §2.4).
 */

#include "core/kb_pool.h"
#include "core/kb_crc32c.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ══════════════════ Internal helpers ══════════════════ */

/*
 * Generate a 16-byte pseudo-random UUID for a new pool.
 * Uses /dev/urandom on POSIX. Falls back to time-based if unavailable.
 */
static void
generate_uuid(uint8_t uuid[16])
{
    FILE *f = fopen("/dev/urandom", "rb");
    if (f != NULL) {
        size_t r = fread(uuid, 1, 16, f);
        (void)r; /* best effort */
        fclose(f);
        return;
    }
    /* Fallback: XOR time with counter. Not cryptographic, but unique enough. */
    uint64_t t = (uint64_t)time(NULL);
    static uint64_t counter = 0;
    counter++;
    memset(uuid, 0, 16);
    memcpy(uuid, &t, 8);
    memcpy(uuid + 8, &counter, 8);
}

/*
 * Compute how many full pages fit in `bytes` at the given page size.
 */
static uint32_t
bytes_to_pages(uint64_t bytes, uint32_t page_size)
{
    return (uint32_t)(bytes / page_size);
}

/*
 * Allocate and initialize the pool handle.
 */
static kb_pool_t *
pool_alloc(void)
{
    kb_pool_t *p = (kb_pool_t *)calloc(1, sizeof(kb_pool_t));
    if (p != NULL) {
        p->pool_fd  = KB_FD_INVALID;
        p->is_open  = false;
    }
    return p;
}

/*
 * Duplicate a path string.
 */
static char *
str_dup(const char *s)
{
    if (s == NULL) return NULL;
    size_t len = strlen(s);
    char *d = (char *)malloc(len + 1);
    if (d != NULL) memcpy(d, s, len + 1);
    return d;
}

/* ══════════════════ Create ══════════════════ */

kb_status_t
kb_pool_create(const char *pool_path,
               const kb_pool_config_t *cfg,
               kb_pool_t **out)
{
    if (pool_path == NULL || out == NULL) return KB_E_BAD_ARG;
    *out = NULL;

    /* Init CRC tables. */
    kb_crc32c_init();

    /* Resolve config. */
    uint16_t page_size = (cfg && cfg->page_size) ? cfg->page_size
                                                 : (uint16_t)KB_PAGE_SIZE_DEFAULT;
    uint32_t grow_mb   = (cfg && cfg->grow_step_mb) ? cfg->grow_step_mb
                                                    : KB_GROW_STEP_MB_DEFAULT;
    uint32_t init_mb   = (cfg && cfg->initial_size_mb) ? cfg->initial_size_mb
                                                       : grow_mb;
    /* Initial size must be at least one segment's metadata (588 KiB rounded up). */
    uint64_t init_bytes = (uint64_t)init_mb << 20;
    uint64_t min_bytes  = (uint64_t)(KB_SEG_DATA_START + 64) * page_size;
    if (init_bytes < min_bytes) init_bytes = min_bytes;

    /* Create the file exclusively. */
    kb_fd_t fd;
    kb_status_t s = kb_os_open(pool_path,
                               KB_OPEN_RDWR | KB_OPEN_CREATE | KB_OPEN_EXCL,
                               0644, &fd);
    if (s != KB_OK) return s;

    /* Preallocate. */
    s = kb_os_preallocate(fd, init_bytes);
    if (s != KB_OK) { kb_os_close(fd); kb_os_delete(pool_path); return s; }

    /* Allocate pool handle. */
    kb_pool_t *pool = pool_alloc();
    if (pool == NULL) { kb_os_close(fd); kb_os_delete(pool_path); return KB_E_NO_RAM; }
    pool->pool_fd  = fd;
    pool->dir_path = str_dup(pool_path);

    /* Initialize superblock. */
    uint8_t uuid[16];
    generate_uuid(uuid);
    kb_sb_init_new(&pool->sb, uuid, page_size);
    pool->sb.grow_step_mb      = grow_mb;
    pool->sb.provisioned_bytes = init_bytes;
    pool->sb.segment_count     = 1;

    /* Compute provisioned pages for the first segment. */
    uint32_t total_pages = bytes_to_pages(init_bytes, page_size);
    uint32_t seg0_pages  = total_pages;
    if (seg0_pages > KB_SEG_TOTAL_PAGES_4K) seg0_pages = KB_SEG_TOTAL_PAGES_4K;

    /* Allocate the segment array. */
    pool->seg_capacity = 4;
    pool->segments = (kb_segment_t *)calloc(pool->seg_capacity,
                                            sizeof(kb_segment_t));
    if (pool->segments == NULL) {
        kb_os_close(fd); kb_os_delete(pool_path);
        free(pool->dir_path); free(pool);
        return KB_E_NO_RAM;
    }

    /* Format segment 0. */
    s = kb_seg_format(fd, &pool->segments[0], 0, seg0_pages,
                      page_size, uuid);
    if (s != KB_OK) {
        kb_os_close(fd); kb_os_delete(pool_path);
        free(pool->segments); free(pool->dir_path); free(pool);
        return s;
    }

    /* Set the high-water mark to just past segment 0's metadata. */
    pool->sb.hwm_unit = ((uint64_t)KB_SEG0_DATA_START * page_size) >> KB_UNIT_SHIFT;

    /* Write both superblock copies. */
    uint8_t *sb_buf = (uint8_t *)calloc(1, page_size);
    if (sb_buf == NULL) {
        kb_seg_free_maps(&pool->segments[0]);
        kb_os_close(fd); kb_os_delete(pool_path);
        free(pool->segments); free(pool->dir_path); free(pool);
        return KB_E_NO_RAM;
    }

    /* Copy 0 (seq=1). */
    pool->sb.seq = 0;
    pool->sb.seq++; /* → 1 */
    s = kb_sb_serialize(&pool->sb, sb_buf);
    if (s == KB_OK) s = kb_os_pwrite(fd, sb_buf, page_size, 0);

    /* Copy 1 (seq=2). */
    if (s == KB_OK) {
        pool->sb.seq++; /* → 2 */
        memset(sb_buf, 0, page_size);
        s = kb_sb_serialize(&pool->sb, sb_buf);
        if (s == KB_OK) s = kb_os_pwrite(fd, sb_buf, page_size, page_size);
    }

    free(sb_buf);
    if (s != KB_OK) {
        kb_seg_free_maps(&pool->segments[0]);
        kb_os_close(fd); kb_os_delete(pool_path);
        free(pool->segments); free(pool->dir_path); free(pool);
        return s;
    }

    /* Sync everything to disk. */
    s = kb_os_sync(fd);
    if (s != KB_OK) {
        kb_seg_free_maps(&pool->segments[0]);
        kb_os_close(fd); kb_os_delete(pool_path);
        free(pool->segments); free(pool->dir_path); free(pool);
        return s;
    }

    pool->is_open     = true;
    pool->is_readonly = false;
    *out = pool;
    return KB_OK;
}

/* ══════════════════ Open ══════════════════ */

kb_status_t
kb_pool_open(const char *pool_path,
             const kb_pool_config_t *cfg,
             kb_pool_t **out)
{
    if (pool_path == NULL || out == NULL) return KB_E_BAD_ARG;
    *out = NULL;

    kb_crc32c_init();

    bool readonly = (cfg && (cfg->flags & KB_OPEN_RDONLY));
    uint32_t open_flags = readonly ? KB_OPEN_RDONLY : KB_OPEN_RDWR;

    kb_fd_t fd;
    kb_status_t s = kb_os_open(pool_path, open_flags, 0, &fd);
    if (s != KB_OK) return s;

    /* Load superblock — try default page size first. */
    kb_pool_t *pool = pool_alloc();
    if (pool == NULL) { kb_os_close(fd); return KB_E_NO_RAM; }
    pool->pool_fd  = fd;
    pool->dir_path = str_dup(pool_path);

    s = kb_sb_load_best(fd, KB_PAGE_SIZE_DEFAULT, &pool->sb);
    if (s != KB_OK) {
        /* Try large page size. */
        s = kb_sb_load_best(fd, KB_PAGE_SIZE_LARGE, &pool->sb);
        if (s != KB_OK) {
            kb_os_close(fd);
            free(pool->dir_path); free(pool);
            return s;
        }
    }

    /* Validate format revision. */
    if (pool->sb.format_rev != KB_FORMAT_REV) {
        kb_os_close(fd);
        free(pool->dir_path); free(pool);
        return KB_E_UNSUPPORTED;
    }

    /* Load all segments. */
    uint32_t seg_count = pool->sb.segment_count;
    pool->seg_capacity = seg_count < 4 ? 4 : seg_count;
    pool->segments = (kb_segment_t *)calloc(pool->seg_capacity,
                                            sizeof(kb_segment_t));
    if (pool->segments == NULL) {
        kb_os_close(fd);
        free(pool->dir_path); free(pool);
        return KB_E_NO_RAM;
    }

    for (uint32_t i = 0; i < seg_count; i++) {
        s = kb_seg_load(fd, &pool->segments[i], i, pool->sb.page_size);
        if (s != KB_OK) {
            /* Clean up already-loaded segments. */
            for (uint32_t j = 0; j < i; j++) {
                kb_seg_free_maps(&pool->segments[j]);
            }
            free(pool->segments);
            kb_os_close(fd);
            free(pool->dir_path); free(pool);
            return s;
        }
    }

    /* Mark as open for write (unless readonly). */
    if (!readonly) {
        pool->sb.clean_shutdown = 0;
        s = kb_sb_flush(fd, &pool->sb);
        if (s != KB_OK) {
            for (uint32_t i = 0; i < seg_count; i++) {
                kb_seg_free_maps(&pool->segments[i]);
            }
            free(pool->segments);
            kb_os_close(fd);
            free(pool->dir_path); free(pool);
            return s;
        }
    }

    pool->is_open     = true;
    pool->is_readonly = readonly;
    *out = pool;
    return KB_OK;
}

/* ══════════════════ Close ══════════════════ */

kb_status_t
kb_pool_close(kb_pool_t *pool)
{
    if (pool == NULL || !pool->is_open) return KB_E_BAD_ARG;

    kb_status_t result = KB_OK;

    if (!pool->is_readonly) {
        /* Flush dirty segment maps. */
        for (uint32_t i = 0; i < pool->sb.segment_count; i++) {
            kb_status_t s = kb_seg_flush(pool->pool_fd, &pool->segments[i],
                                         pool->sb.page_size);
            if (s != KB_OK && result == KB_OK) result = s;
        }

        /* Mark clean shutdown. */
        pool->sb.clean_shutdown = 1;
        kb_status_t s = kb_sb_flush(pool->pool_fd, &pool->sb);
        if (s != KB_OK && result == KB_OK) result = s;
    }

    /* Free segment maps. */
    for (uint32_t i = 0; i < pool->sb.segment_count; i++) {
        kb_seg_free_maps(&pool->segments[i]);
    }
    free(pool->segments);

    /* Close file. */
    kb_status_t s = kb_os_close(pool->pool_fd);
    if (s != KB_OK && result == KB_OK) result = s;

    free(pool->dir_path);
    pool->is_open = false;
    free(pool);
    return result;
}

/* ══════════════════ Stats (§2.4.7) ══════════════════ */

kb_status_t
kb_pool_get_stats(const kb_pool_t *pool, kb_pool_stats_t *out)
{
    if (pool == NULL || out == NULL || !pool->is_open) return KB_E_BAD_ARG;

    memset(out, 0, sizeof(*out));

    uint64_t file_size;
    kb_status_t s = kb_os_file_size(pool->pool_fd, &file_size);
    if (s != KB_OK) return s;

    out->file_size_bytes   = file_size;
    out->provisioned_bytes = pool->sb.provisioned_bytes;
    out->hwm_bytes         = pool->sb.hwm_unit << KB_UNIT_SHIFT;
    out->object_count      = pool->sb.object_count;
    out->segment_count     = pool->sb.segment_count;

    /* Compute free space below HWM and largest hole. */
    uint64_t free_pages    = 0;
    uint64_t largest_hole  = 0;

    for (uint32_t si = 0; si < pool->sb.segment_count; si++) {
        const kb_segment_t *seg = &pool->segments[si];
        for (uint32_t c = 0; c < KB_CHUNKS_PER_SEG_4K; c++) {
            free_pages += seg->chunks[c].free_pages;
            if (seg->chunks[c].longest_run > largest_hole) {
                largest_hole = seg->chunks[c].longest_run;
            }
        }
    }

    uint32_t ps = pool->sb.page_size;
    out->free_bytes_below_hwm = free_pages * ps;
    out->largest_hole_pages   = largest_hole;

    return KB_OK;
}

/* ══════════════════ Growth (§2.4.6) ══════════════════ */

kb_status_t
kb_pool_ensure_space(kb_pool_t *pool, uint32_t needed_pages)
{
    if (pool == NULL || !pool->is_open || pool->is_readonly) return KB_E_BAD_ARG;

    /* Check existing free space across all segments. */
    uint64_t total_free = 0;
    for (uint32_t si = 0; si < pool->sb.segment_count; si++) {
        const kb_segment_t *seg = &pool->segments[si];
        for (uint32_t c = 0; c < KB_CHUNKS_PER_SEG_4K; c++) {
            total_free += seg->chunks[c].free_pages;
        }
    }

    if (total_free >= needed_pages) return KB_OK;

    /* Need to grow. Compute how much to add (§2.4.6). */
    uint32_t deficit = needed_pages - (uint32_t)total_free;
    uint64_t grow_bytes = (uint64_t)pool->sb.grow_step_mb << 20;
    uint64_t deficit_bytes = (uint64_t)deficit * pool->sb.page_size;
    if (deficit_bytes > grow_bytes) grow_bytes = deficit_bytes;

    /* Round up to page boundary. */
    uint32_t ps = pool->sb.page_size;
    grow_bytes = ((grow_bytes + ps - 1) / ps) * ps;

    uint64_t new_size = pool->sb.provisioned_bytes + grow_bytes;

    /* Preallocate the space. */
    kb_status_t s = kb_os_preallocate(pool->pool_fd, new_size);
    if (s != KB_OK) return s;

    /*
     * Add the new pages to the last segment (if it has room) or
     * add a new segment. For simplicity in Phase 2, we extend the
     * last segment up to the 2 GiB boundary, then add new segments.
     */
    uint32_t last_seg = pool->sb.segment_count - 1;
    kb_segment_t *seg = &pool->segments[last_seg];

    uint32_t added_pages = (uint32_t)(grow_bytes / ps);
    uint32_t seg_room = KB_SEG_TOTAL_PAGES_4K - seg->provisioned_pages;

    if (added_pages <= seg_room) {
        /* Fits in the existing last segment. */
        for (uint32_t p = seg->provisioned_pages;
             p < seg->provisioned_pages + added_pages; p++) {
            kb_seg_bitmap_clear(seg, p);
            seg->kind_map[p] = KB_PAGE_KIND_FREE;
        }
        seg->provisioned_pages += added_pages;
        seg->kind_map_dirty = true;

        /* Recompute affected chunks. */
        uint32_t first_chunk = (seg->provisioned_pages - added_pages)
                               >> KB_CHUNK_PAGES_SHIFT;
        uint32_t last_chunk  = (seg->provisioned_pages - 1)
                               >> KB_CHUNK_PAGES_SHIFT;
        for (uint32_t c = first_chunk;
             c <= last_chunk && c < KB_CHUNKS_PER_SEG_4K; c++) {
            kb_seg_recompute_chunk(seg, c);
        }

        /* Update flags. */
        seg->flags = (seg->provisioned_pages < KB_SEG_TOTAL_PAGES_4K)
                     ? KB_SEG_FLAG_PARTLY_PROVISIONED : 0;
    } else {
        /* Need a new segment — for now, extend the last segment to full
         * and allocate a new one with the remainder.
         * (Full multi-segment growth is deferred to Phase 3.) */

        /* Fill the last segment. */
        if (seg_room > 0) {
            for (uint32_t p = seg->provisioned_pages;
                 p < KB_SEG_TOTAL_PAGES_4K; p++) {
                kb_seg_bitmap_clear(seg, p);
                seg->kind_map[p] = KB_PAGE_KIND_FREE;
            }
            seg->provisioned_pages = KB_SEG_TOTAL_PAGES_4K;
            seg->flags = 0;
            seg->kind_map_dirty = true;
            kb_seg_recompute_all_chunks(seg);
        }

        /* Allocate a new segment descriptor if needed. */
        uint32_t new_seg_id = pool->sb.segment_count;
        if (new_seg_id >= pool->seg_capacity) {
            uint32_t new_cap = pool->seg_capacity * 2;
            kb_segment_t *tmp = (kb_segment_t *)realloc(pool->segments,
                                    new_cap * sizeof(kb_segment_t));
            if (tmp == NULL) return KB_E_NO_RAM;
            pool->segments = tmp;
            memset(&pool->segments[pool->seg_capacity], 0,
                   (new_cap - pool->seg_capacity) * sizeof(kb_segment_t));
            pool->seg_capacity = new_cap;
        }

        uint32_t remaining = added_pages - seg_room;
        uint32_t new_prov = remaining;
        if (new_prov > KB_SEG_TOTAL_PAGES_4K) new_prov = KB_SEG_TOTAL_PAGES_4K;

        s = kb_seg_format(pool->pool_fd, &pool->segments[new_seg_id],
                          new_seg_id, new_prov, ps, pool->sb.pool_uuid);
        if (s != KB_OK) return s;

        pool->sb.segment_count++;
    }

    pool->sb.provisioned_bytes = new_size;
    return KB_OK;
}

/* ══════════════════ Cross-segment page allocation (§2.4.3) ══════════════════ */

kb_status_t
kb_pool_alloc_pages(kb_pool_t *pool, uint32_t n, uint8_t kind,
                    kb_addr_t *out_addr)
{
    if (pool == NULL || out_addr == NULL || n == 0) return KB_E_BAD_ARG;
    *out_addr = KB_NULL_ADDR;

    /* Try each segment in order (lowest address first). */
    for (uint32_t si = 0; si < pool->sb.segment_count; si++) {
        kb_segment_t *seg = &pool->segments[si];
        uint32_t pg = kb_seg_alloc_pages(seg, n, kind);
        if (pg != UINT32_MAX) {
            /* Compute absolute byte offset. */
            *out_addr = seg->base_offset + (uint64_t)pg * pool->sb.page_size;

            /* Update HWM if needed. */
            uint64_t top_unit = (*out_addr + (uint64_t)n * pool->sb.page_size)
                                >> KB_UNIT_SHIFT;
            if (top_unit > pool->sb.hwm_unit) {
                pool->sb.hwm_unit = top_unit;
            }
            return KB_OK;
        }
    }

    /* No space. Try growing. */
    kb_status_t s = kb_pool_ensure_space(pool, n);
    if (s != KB_OK) return s;

    /* Retry on the (possibly new) last segment. */
    uint32_t last_si = pool->sb.segment_count - 1;
    kb_segment_t *seg = &pool->segments[last_si];
    uint32_t pg = kb_seg_alloc_pages(seg, n, kind);
    if (pg == UINT32_MAX) return KB_E_NO_SPACE;

    *out_addr = seg->base_offset + (uint64_t)pg * pool->sb.page_size;
    uint64_t top_unit = (*out_addr + (uint64_t)n * pool->sb.page_size)
                        >> KB_UNIT_SHIFT;
    if (top_unit > pool->sb.hwm_unit) {
        pool->sb.hwm_unit = top_unit;
    }
    return KB_OK;
}

kb_status_t
kb_pool_free_pages(kb_pool_t *pool, kb_addr_t addr, uint32_t n)
{
    if (pool == NULL || n == 0) return KB_E_BAD_ARG;

    /* Find which segment this address belongs to. */
    uint32_t si = (uint32_t)(addr / KB_SEGMENT_SIZE);
    if (si >= pool->sb.segment_count) return KB_E_BAD_ARG;

    kb_segment_t *seg = &pool->segments[si];
    uint32_t pg = (uint32_t)((addr - seg->base_offset) / pool->sb.page_size);

    kb_seg_free_pages(seg, pg, n);
    return KB_OK;
}
