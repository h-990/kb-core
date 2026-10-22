/*
 * kb_segment.c — Segment map operations (§2.3).
 *
 * Manages the per-segment bitmap, kind map, and chunk summaries.
 * The bitmap uses lowest-address-first allocation (§2.4.3).
 *
 * Segment 0 has the superblock at pages 0-1, so its metadata region
 * is shifted by 2 pages. Segments > 0 use the standard layout.
 */

#include "core/kb_segment.h"
#include "core/kb_crc32c.h"
#include <stdlib.h>
#include <string.h>

/* ══════════════════ Segment-aware page offsets ══════════════════ */

/*
 * Return the page offsets for the metadata regions depending on
 * whether this is segment 0 (which has 2 superblock pages before it).
 */
static void
seg_offsets(uint32_t seg_id,
            uint32_t *out_hdr, uint32_t *out_bm,
            uint32_t *out_km,  uint32_t *out_cs,
            uint32_t *out_data)
{
    if (seg_id == 0) {
        *out_hdr  = KB_SEG0_HDR_PAGE;
        *out_bm   = KB_SEG0_BITMAP_START;
        *out_km   = KB_SEG0_KINDMAP_START;
        *out_cs   = KB_SEG0_CHUNKSUMM_START;
        *out_data = KB_SEG0_DATA_START;
    } else {
        *out_hdr  = KB_SEG_HDR_PAGE;
        *out_bm   = KB_SEG_BITMAP_START;
        *out_km   = KB_SEG_KINDMAP_START;
        *out_cs   = KB_SEG_CHUNKSUMM_START;
        *out_data = KB_SEG_DATA_START;
    }
}

/*
 * Return the data-start page for a given segment.
 */
static uint32_t
seg_data_start(uint32_t seg_id)
{
    return (seg_id == 0) ? KB_SEG0_DATA_START : KB_SEG_DATA_START;
}

/* ══════════════════ Map allocation ══════════════════ */

#define BITMAP_BYTES_4K    (KB_SEG_TOTAL_PAGES_4K / 8)     /* 65,536   */
#define KINDMAP_BYTES_4K   (KB_SEG_TOTAL_PAGES_4K)          /* 524,288  */
#define CHUNKS_BYTES_4K    (KB_CHUNKS_PER_SEG_4K * sizeof(kb_chunk_summary_t))

kb_status_t
kb_seg_alloc_maps(kb_segment_t *seg)
{
    if (seg == NULL) return KB_E_BAD_ARG;

    seg->bitmap   = (uint8_t *)calloc(1, BITMAP_BYTES_4K);
    seg->kind_map = (uint8_t *)calloc(1, KINDMAP_BYTES_4K);
    seg->chunks   = (kb_chunk_summary_t *)calloc(KB_CHUNKS_PER_SEG_4K,
                                                  sizeof(kb_chunk_summary_t));

    if (seg->bitmap == NULL || seg->kind_map == NULL || seg->chunks == NULL) {
        kb_seg_free_maps(seg);
        return KB_E_NO_RAM;
    }

    seg->bitmap_dirty   = false;
    seg->kind_map_dirty = false;
    seg->chunks_dirty   = false;
    return KB_OK;
}

void
kb_seg_free_maps(kb_segment_t *seg)
{
    if (seg == NULL) return;
    free(seg->bitmap);   seg->bitmap   = NULL;
    free(seg->kind_map); seg->kind_map = NULL;
    free(seg->chunks);   seg->chunks   = NULL;
}

/* ══════════════════ Segment formatting ══════════════════ */

kb_status_t
kb_seg_format(kb_fd_t pool_fd, kb_segment_t *seg,
              uint32_t seg_id, uint32_t provisioned_pgs,
              uint32_t page_size, const uint8_t pool_uuid[16])
{
    if (seg == NULL || pool_uuid == NULL) return KB_E_BAD_ARG;

    kb_status_t s = kb_seg_alloc_maps(seg);
    if (s != KB_OK) return s;

    uint32_t pg_hdr, pg_bm, pg_km, pg_cs, pg_data;
    seg_offsets(seg_id, &pg_hdr, &pg_bm, &pg_km, &pg_cs, &pg_data);

    seg->segment_id        = seg_id;
    seg->provisioned_pages = provisioned_pgs;
    seg->flags             = (provisioned_pgs < KB_SEG_TOTAL_PAGES_4K)
                             ? KB_SEG_FLAG_PARTLY_PROVISIONED : 0;
    seg->seq               = 1;
    seg->base_offset       = (uint64_t)seg_id * KB_SEGMENT_SIZE;

    /* ── Initialize bitmap ─────────────────────────────────────────────
     * Mark all metadata pages as used (bit = 1).
     * Mark unprovisioned pages as used (bit = 1).
     * Everything else is free (bit = 0, already zeroed by calloc).
     */
    for (uint32_t p = 0; p < pg_data; p++) {
        kb_seg_bitmap_set(seg, p);
    }
    for (uint32_t p = provisioned_pgs; p < KB_SEG_TOTAL_PAGES_4K; p++) {
        kb_seg_bitmap_set(seg, p);
    }

    /* ── Initialize kind map ──────────────────────────────────────────
     * Metadata pages get SEG_META; unprovisioned get UNPROVISIONED.
     */
    for (uint32_t p = 0; p < pg_data; p++) {
        seg->kind_map[p] = KB_PAGE_KIND_SEG_META;
    }
    for (uint32_t p = provisioned_pgs; p < KB_SEG_TOTAL_PAGES_4K; p++) {
        seg->kind_map[p] = KB_PAGE_KIND_UNPROVISIONED;
    }

    /* ── Compute chunk summaries ──────────────────────────────────── */
    kb_seg_recompute_all_chunks(seg);

    /* ── Write segment header page ─────────────────────────────────── */
    uint8_t *hdr_page = (uint8_t *)calloc(1, page_size);
    if (hdr_page == NULL) { kb_seg_free_maps(seg); return KB_E_NO_RAM; }

    kb_write_u32(hdr_page + KB_SEG_HDR_OFF_MAGIC,    KB_MAGIC_SEGMENT);
    kb_write_u32(hdr_page + KB_SEG_HDR_OFF_SEGMENT_ID, seg_id);
    kb_write_u32(hdr_page + KB_SEG_HDR_OFF_FLAGS,    seg->flags);
    kb_write_u32(hdr_page + KB_SEG_HDR_OFF_SEQ,      seg->seq);
    memcpy(hdr_page + KB_SEG_HDR_OFF_POOL_UUID, pool_uuid, 16);
    kb_write_u32(hdr_page + KB_SEG_HDR_OFF_PROVISIONED_PGS, provisioned_pgs);

    kb_write_u32(hdr_page + KB_SEG_HDR_OFF_CRC32C, 0);
    uint32_t crc = kb_crc32c_buf(hdr_page, KB_SEG_HDR_PAYLOAD_SIZE);
    kb_write_u32(hdr_page + KB_SEG_HDR_OFF_CRC32C, crc);

    uint64_t base = seg->base_offset;
    s = kb_os_pwrite(pool_fd, hdr_page, page_size,
                     base + (uint64_t)pg_hdr * page_size);
    free(hdr_page);
    if (s != KB_OK) { kb_seg_free_maps(seg); return s; }

    /* ── Write bitmap pages ────────────────────────────────────────── */
    uint64_t bm_off = base + (uint64_t)pg_bm * page_size;
    s = kb_os_pwrite(pool_fd, seg->bitmap, BITMAP_BYTES_4K, bm_off);
    if (s != KB_OK) { kb_seg_free_maps(seg); return s; }

    /* ── Write kind map pages ──────────────────────────────────────── */
    uint64_t km_off = base + (uint64_t)pg_km * page_size;
    s = kb_os_pwrite(pool_fd, seg->kind_map, KINDMAP_BYTES_4K, km_off);
    if (s != KB_OK) { kb_seg_free_maps(seg); return s; }

    /* ── Write chunk summary pages ─────────────────────────────────── */
    uint64_t cs_off = base + (uint64_t)pg_cs * page_size;
    s = kb_os_pwrite(pool_fd, seg->chunks, CHUNKS_BYTES_4K, cs_off);
    if (s != KB_OK) { kb_seg_free_maps(seg); return s; }

    seg->bitmap_dirty   = false;
    seg->kind_map_dirty = false;
    seg->chunks_dirty   = false;
    return KB_OK;
}

/* ══════════════════ Segment loading ══════════════════ */

kb_status_t
kb_seg_load(kb_fd_t pool_fd, kb_segment_t *seg,
            uint32_t seg_id, uint32_t page_size)
{
    if (seg == NULL) return KB_E_BAD_ARG;

    kb_status_t s = kb_seg_alloc_maps(seg);
    if (s != KB_OK) return s;

    uint32_t pg_hdr, pg_bm, pg_km, pg_cs, pg_data;
    seg_offsets(seg_id, &pg_hdr, &pg_bm, &pg_km, &pg_cs, &pg_data);
    (void)pg_data;

    seg->segment_id  = seg_id;
    seg->base_offset = (uint64_t)seg_id * KB_SEGMENT_SIZE;
    uint64_t base    = seg->base_offset;

    /* Read and validate segment header. */
    uint8_t *hdr_page = (uint8_t *)calloc(1, page_size);
    if (hdr_page == NULL) { kb_seg_free_maps(seg); return KB_E_NO_RAM; }

    s = kb_os_pread(pool_fd, hdr_page, page_size,
                    base + (uint64_t)pg_hdr * page_size);
    if (s != KB_OK) { free(hdr_page); kb_seg_free_maps(seg); return s; }

    uint32_t magic = kb_read_u32(hdr_page + KB_SEG_HDR_OFF_MAGIC);
    if (magic != KB_MAGIC_SEGMENT) {
        free(hdr_page);
        kb_seg_free_maps(seg);
        return KB_E_CORRUPT;
    }

    uint32_t stored_crc = kb_read_u32(hdr_page + KB_SEG_HDR_OFF_CRC32C);
    kb_write_u32(hdr_page + KB_SEG_HDR_OFF_CRC32C, 0);
    uint32_t computed_crc = kb_crc32c_buf(hdr_page, KB_SEG_HDR_PAYLOAD_SIZE);
    if (computed_crc != stored_crc) {
        free(hdr_page);
        kb_seg_free_maps(seg);
        return KB_E_CORRUPT;
    }

    seg->flags             = kb_read_u32(hdr_page + KB_SEG_HDR_OFF_FLAGS);
    seg->seq               = kb_read_u32(hdr_page + KB_SEG_HDR_OFF_SEQ);
    seg->provisioned_pages = kb_read_u32(hdr_page + KB_SEG_HDR_OFF_PROVISIONED_PGS);
    free(hdr_page);

    /* Read bitmap. */
    uint64_t bm_off = base + (uint64_t)pg_bm * page_size;
    s = kb_os_pread(pool_fd, seg->bitmap, BITMAP_BYTES_4K, bm_off);
    if (s != KB_OK) { kb_seg_free_maps(seg); return s; }

    /* Read kind map. */
    uint64_t km_off = base + (uint64_t)pg_km * page_size;
    s = kb_os_pread(pool_fd, seg->kind_map, KINDMAP_BYTES_4K, km_off);
    if (s != KB_OK) { kb_seg_free_maps(seg); return s; }

    /* Read chunk summaries. */
    uint64_t cs_off = base + (uint64_t)pg_cs * page_size;
    s = kb_os_pread(pool_fd, seg->chunks, CHUNKS_BYTES_4K, cs_off);
    if (s != KB_OK) { kb_seg_free_maps(seg); return s; }

    seg->bitmap_dirty   = false;
    seg->kind_map_dirty = false;
    seg->chunks_dirty   = false;
    return KB_OK;
}

/* ══════════════════ Segment flushing ══════════════════ */

kb_status_t
kb_seg_flush(kb_fd_t pool_fd, const kb_segment_t *seg, uint32_t page_size)
{
    if (seg == NULL) return KB_E_BAD_ARG;

    uint32_t pg_hdr, pg_bm, pg_km, pg_cs, pg_data;
    seg_offsets(seg->segment_id, &pg_hdr, &pg_bm, &pg_km, &pg_cs, &pg_data);
    (void)pg_hdr; (void)pg_data;

    uint64_t base = seg->base_offset;
    kb_status_t s;

    if (seg->bitmap_dirty) {
        uint64_t bm_off = base + (uint64_t)pg_bm * page_size;
        s = kb_os_pwrite(pool_fd, seg->bitmap, BITMAP_BYTES_4K, bm_off);
        if (s != KB_OK) return s;
    }

    if (seg->kind_map_dirty) {
        uint64_t km_off = base + (uint64_t)pg_km * page_size;
        s = kb_os_pwrite(pool_fd, seg->kind_map, KINDMAP_BYTES_4K, km_off);
        if (s != KB_OK) return s;
    }

    if (seg->chunks_dirty) {
        uint64_t cs_off = base + (uint64_t)pg_cs * page_size;
        s = kb_os_pwrite(pool_fd, seg->chunks, CHUNKS_BYTES_4K, cs_off);
        if (s != KB_OK) return s;
    }

    return KB_OK;
}

/* ══════════════════ Chunk summary recomputation (§2.3.2) ══════════════════ */

void
kb_seg_recompute_chunk(kb_segment_t *seg, uint32_t chunk_idx)
{
    uint32_t start = chunk_idx << KB_CHUNK_PAGES_SHIFT;
    uint32_t end   = start + KB_CHUNK_PAGES;
    if (end > KB_SEG_TOTAL_PAGES_4K) end = KB_SEG_TOTAL_PAGES_4K;

    uint16_t free_count   = 0;
    uint16_t longest_run  = 0;
    uint16_t free_at_start = 0;
    uint16_t free_at_end  = 0;
    uint16_t cur_run      = 0;
    bool     in_start     = true;

    for (uint32_t p = start; p < end; p++) {
        if (!kb_seg_bitmap_test(seg, p)) {
            free_count++;
            cur_run++;
            if (in_start) free_at_start++;
        } else {
            if (cur_run > longest_run) longest_run = cur_run;
            cur_run = 0;
            in_start = false;
        }
    }
    if (cur_run > longest_run) longest_run = cur_run;
    free_at_end = cur_run;

    seg->chunks[chunk_idx].free_pages    = free_count;
    seg->chunks[chunk_idx].longest_run   = longest_run;
    seg->chunks[chunk_idx].free_at_start = free_at_start;
    seg->chunks[chunk_idx].free_at_end   = free_at_end;
    seg->chunks_dirty = true;
}

void
kb_seg_recompute_all_chunks(kb_segment_t *seg)
{
    for (uint32_t c = 0; c < KB_CHUNKS_PER_SEG_4K; c++) {
        kb_seg_recompute_chunk(seg, c);
    }
}

/* ══════════════════ Free-run scan (§2.4.3) ══════════════════ */

uint32_t
kb_seg_find_free_run_in_chunk(const kb_segment_t *seg,
                              uint32_t chunk_idx, uint32_t n)
{
    uint32_t start = chunk_idx << KB_CHUNK_PAGES_SHIFT;
    uint32_t end   = start + KB_CHUNK_PAGES;
    if (end > KB_SEG_TOTAL_PAGES_4K) end = KB_SEG_TOTAL_PAGES_4K;

    uint32_t run_start = start;
    uint32_t run_len   = 0;

    for (uint32_t p = start; p < end; p++) {
        if (!kb_seg_bitmap_test(seg, p)) {
            if (run_len == 0) run_start = p;
            run_len++;
            if (run_len >= n) return run_start;
        } else {
            run_len = 0;
        }
    }
    return UINT32_MAX;
}

uint32_t
kb_seg_find_free_run(const kb_segment_t *seg, uint32_t n)
{
    if (n == 0) return UINT32_MAX;

    if (n <= KB_CHUNK_PAGES) {
        for (uint32_t c = 0; c < KB_CHUNKS_PER_SEG_4K; c++) {
            if (seg->chunks[c].longest_run >= n) {
                uint32_t result = kb_seg_find_free_run_in_chunk(seg, c, n);
                if (result != UINT32_MAX) return result;
            }
        }

        /* Check cross-chunk boundaries. */
        for (uint32_t c = 0; c + 1 < KB_CHUNKS_PER_SEG_4K; c++) {
            uint32_t cross = (uint32_t)seg->chunks[c].free_at_end
                           + (uint32_t)seg->chunks[c + 1].free_at_start;
            if (cross >= n) {
                uint32_t boundary = ((c + 1) << KB_CHUNK_PAGES_SHIFT);
                uint32_t run_start = boundary - (uint32_t)seg->chunks[c].free_at_end;
                return run_start;
            }
        }
    } else {
        /* Large run: brute-force scan starting past metadata. */
        uint32_t data_start = seg_data_start(seg->segment_id);
        uint32_t run_start = 0;
        uint32_t run_len   = 0;
        for (uint32_t p = data_start; p < KB_SEG_TOTAL_PAGES_4K; p++) {
            if (!kb_seg_bitmap_test(seg, p)) {
                if (run_len == 0) run_start = p;
                run_len++;
                if (run_len >= n) return run_start;
            } else {
                run_len = 0;
            }
        }
    }

    return UINT32_MAX;
}

/* ══════════════════ Page allocation (§2.4.3) ══════════════════ */

uint32_t
kb_seg_alloc_pages(kb_segment_t *seg, uint32_t n, uint8_t kind)
{
    uint32_t start = kb_seg_find_free_run(seg, n);
    if (start == UINT32_MAX) return UINT32_MAX;

    for (uint32_t p = start; p < start + n; p++) {
        kb_seg_bitmap_set(seg, p);
        seg->kind_map[p] = kind;
    }
    seg->kind_map_dirty = true;

    uint32_t first_chunk = start >> KB_CHUNK_PAGES_SHIFT;
    uint32_t last_chunk  = (start + n - 1) >> KB_CHUNK_PAGES_SHIFT;
    for (uint32_t c = first_chunk; c <= last_chunk && c < KB_CHUNKS_PER_SEG_4K; c++) {
        kb_seg_recompute_chunk(seg, c);
    }

    return start;
}

void
kb_seg_free_pages(kb_segment_t *seg, uint32_t start_page, uint32_t n)
{
    for (uint32_t p = start_page; p < start_page + n; p++) {
        kb_seg_bitmap_clear(seg, p);
        seg->kind_map[p] = KB_PAGE_KIND_FREE;
    }
    seg->kind_map_dirty = true;

    uint32_t first_chunk = start_page >> KB_CHUNK_PAGES_SHIFT;
    uint32_t last_chunk  = (start_page + n - 1) >> KB_CHUNK_PAGES_SHIFT;
    for (uint32_t c = first_chunk; c <= last_chunk && c < KB_CHUNKS_PER_SEG_4K; c++) {
        kb_seg_recompute_chunk(seg, c);
    }
}
