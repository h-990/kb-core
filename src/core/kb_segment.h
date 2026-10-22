/*
 * kb_segment.h — Segment layout, bitmap and map operations (§2.3).
 *
 * A segment is a 2 GiB region of pool.db. It contains:
 *   - 1 page:   segment header (magic, id, flags, seq, crc)
 *   - 16 pages: free bitmap   (1 bit/page, 1 = used or unprovisioned)
 *   - 128 pages: kind map     (1 byte/page, page kind enum)
 *   - 2 pages:  chunk summary (1024 chunks × 8 bytes)
 *   - 524,141 allocatable pages
 *
 * These 147 metadata pages are 588 KiB per 2 GiB segment.
 */
#ifndef KB_SEGMENT_H
#define KB_SEGMENT_H

#include "core/kb_types.h"
#include "core/kb_os.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════ Segment constants ══════════════════ */

/* Pages per 2 GiB segment at 4 KiB page size. */
#define KB_SEG_TOTAL_PAGES_4K    (KB_SEGMENT_SIZE / KB_PAGE_SIZE_DEFAULT)

/*
 * Metadata region offsets (in pages from the segment start).
 *
 * Segment 0 layout:
 *   pages 0–1:  superblock copies (owned by kb_superblock)
 *   page  2:    segment header
 *   pages 3–18: free bitmap (16 pages)
 *   pages 19–146: kind map (128 pages)
 *   pages 147–148: chunk summary (2 pages)
 *   page  149+:   allocatable
 *
 * Segments > 0 layout (no superblock):
 *   page  0:    segment header
 *   pages 1–16: free bitmap
 *   etc.
 */
#define KB_SUPERBLOCK_PAGES      2u       /* superblock copies at pages 0,1 */

/* Offsets for segments > 0 (standard layout). */
#define KB_SEG_HDR_PAGE          0u
#define KB_SEG_BITMAP_START      1u       /* 16 pages   */
#define KB_SEG_BITMAP_PAGES      16u
#define KB_SEG_KINDMAP_START     17u      /* 128 pages  */
#define KB_SEG_KINDMAP_PAGES     128u
#define KB_SEG_CHUNKSUMM_START   145u     /* 2 pages    */
#define KB_SEG_CHUNKSUMM_PAGES   2u
#define KB_SEG_DATA_START        147u     /* allocatable space begins here */

/* Offsets for segment 0 (shifted by 2 superblock pages). */
#define KB_SEG0_HDR_PAGE         2u
#define KB_SEG0_BITMAP_START     3u
#define KB_SEG0_KINDMAP_START    19u
#define KB_SEG0_CHUNKSUMM_START  147u
#define KB_SEG0_DATA_START       149u     /* 2 extra pages for superblock */

/* Chunks: 512 pages each, 1024 per segment. */
#define KB_CHUNK_PAGES_SHIFT     9u       /* log2(512)  */
#define KB_CHUNKS_PER_SEG_4K     1024u

/* ══════════════════ Segment header on-disk (page 0 of segment) ══════════════════ */

/*
 * Off │ Size │ Field
 * ────┼──────┼──────────────
 *   0 │  4   │ magic  (0x47455342  "BSEG" LE)
 *   4 │  4   │ segment_id
 *   8 │  4   │ flags  (bit 0: partly provisioned)
 *  12 │  4   │ seq    (written monotonically for torn-write detection)
 *  16 │ 16   │ pool_uuid
 *  32 │  4   │ provisioned_pages (for the last segment, may be < total)
 *  36 │ 24   │ _reserved
 *  60 │  4   │ crc32c
 */
#define KB_MAGIC_SEGMENT         0x47455342u  /* "BSEG" LE */
#define KB_SEG_HDR_PAYLOAD_SIZE  60u

#define KB_SEG_HDR_OFF_MAGIC             0
#define KB_SEG_HDR_OFF_SEGMENT_ID        4
#define KB_SEG_HDR_OFF_FLAGS             8
#define KB_SEG_HDR_OFF_SEQ              12
#define KB_SEG_HDR_OFF_POOL_UUID        16
#define KB_SEG_HDR_OFF_PROVISIONED_PGS  32
#define KB_SEG_HDR_OFF_CRC32C           60

/* Segment header flags. */
#define KB_SEG_FLAG_PARTLY_PROVISIONED   (1u << 0)

/* ══════════════════ Chunk summary entry (§2.3.2) ══════════════════ */

/*
 * 8 bytes per chunk: 4 × u16.
 *   free_pages:       pages free in this chunk
 *   longest_run:      longest contiguous free run
 *   free_at_start:    free pages at the start of the chunk
 *   free_at_end:      free pages at the end of the chunk
 */
typedef struct kb_chunk_summary {
    uint16_t free_pages;
    uint16_t longest_run;
    uint16_t free_at_start;
    uint16_t free_at_end;
} kb_chunk_summary_t;

_Static_assert(sizeof(kb_chunk_summary_t) == 8,
               "chunk summary must be 8 bytes");

/* ══════════════════ In-memory segment descriptor ══════════════════ */

/*
 * Loaded at pool open; one per segment. Holds the in-memory state of
 * the three maps. For a segment < 2 GiB, bitmap and kind map are
 * relatively small (64 KiB + 512 KiB), so they're resident.
 */
typedef struct kb_segment {
    uint32_t segment_id;
    uint32_t flags;
    uint32_t seq;
    uint32_t provisioned_pages; /* for last segment, may be < total */

    /*
     * Bitmap: 1 bit per page. Bit = 1 means used/unprovisioned.
     * 524,288 bits = 65,536 bytes = 16 pages.
     */
    uint8_t  *bitmap;       /* 65,536 bytes, allocated */

    /*
     * Kind map: 1 byte per page. Values from KB_PAGE_KIND_*.
     * 524,288 bytes = 128 pages.
     */
    uint8_t  *kind_map;     /* 524,288 bytes, allocated */

    /*
     * Chunk summaries: 1024 entries.
     */
    kb_chunk_summary_t *chunks;  /* 1024 × 8 = 8,192 bytes */

    /*
     * Base byte offset of this segment in pool.db.
     */
    uint64_t base_offset;

    /*
     * Dirty flags — which maps need flushing.
     */
    bool bitmap_dirty;
    bool kind_map_dirty;
    bool chunks_dirty;
} kb_segment_t;

/* ══════════════════ Bitmap operations ══════════════════ */

/*
 * Set page `page_idx` as used (bit = 1) in the bitmap.
 */
static inline void kb_seg_bitmap_set(kb_segment_t *seg, uint32_t page_idx)
{
    uint32_t byte_idx = page_idx >> 3;
    uint8_t  bit      = (uint8_t)(1u << (page_idx & 7u));
    seg->bitmap[byte_idx] |= bit;
    seg->bitmap_dirty = true;
}

/*
 * Clear page `page_idx` (free, bit = 0) in the bitmap.
 */
static inline void kb_seg_bitmap_clear(kb_segment_t *seg, uint32_t page_idx)
{
    uint32_t byte_idx = page_idx >> 3;
    uint8_t  bit      = (uint8_t)(1u << (page_idx & 7u));
    seg->bitmap[byte_idx] &= (uint8_t)~bit;
    seg->bitmap_dirty = true;
}

/*
 * Test whether page `page_idx` is used (bit = 1).
 */
static inline bool kb_seg_bitmap_test(const kb_segment_t *seg, uint32_t page_idx)
{
    uint32_t byte_idx = page_idx >> 3;
    uint8_t  bit      = (uint8_t)(1u << (page_idx & 7u));
    return (seg->bitmap[byte_idx] & bit) != 0;
}

/* ══════════════════ Segment lifecycle ══════════════════ */

/*
 * Allocate the in-memory maps for a segment descriptor.
 */
kb_status_t kb_seg_alloc_maps(kb_segment_t *seg);

/*
 * Free the in-memory maps.
 */
void kb_seg_free_maps(kb_segment_t *seg);

/*
 * Initialize a new segment on disk: write the segment header, empty bitmap
 * (all free or unprovisioned), kind map (metadata pages marked), and zeroed
 * chunk summaries. The in-memory descriptor is also populated.
 *
 * `seg_id`:          segment index (0, 1, 2, …)
 * `provisioned_pgs`: number of pages provisioned (may be < total for last seg)
 * `page_size`:       from the superblock
 * `pool_uuid`:       16-byte pool identity
 */
kb_status_t kb_seg_format(kb_fd_t pool_fd, kb_segment_t *seg,
                          uint32_t seg_id, uint32_t provisioned_pgs,
                          uint32_t page_size, const uint8_t pool_uuid[16]);

/*
 * Load a segment's maps from disk into the in-memory descriptor.
 */
kb_status_t kb_seg_load(kb_fd_t pool_fd, kb_segment_t *seg,
                        uint32_t seg_id, uint32_t page_size);

/*
 * Flush dirty maps to disk.
 */
kb_status_t kb_seg_flush(kb_fd_t pool_fd, const kb_segment_t *seg,
                         uint32_t page_size);

/* ══════════════════ Chunk summary recomputation ══════════════════ */

/*
 * Recompute the chunk summary for one chunk from the bitmap.
 */
void kb_seg_recompute_chunk(kb_segment_t *seg, uint32_t chunk_idx);

/*
 * Recompute all chunk summaries from the bitmap.
 */
void kb_seg_recompute_all_chunks(kb_segment_t *seg);

/* ══════════════════ Free-run scan (§2.4.3 step 3) ══════════════════ */

/*
 * Find the lowest free run of `n` contiguous pages in a chunk.
 * Returns the page index (relative to the segment) or UINT32_MAX if not found.
 */
uint32_t kb_seg_find_free_run_in_chunk(const kb_segment_t *seg,
                                       uint32_t chunk_idx, uint32_t n);

/*
 * Find the lowest free run of `n` contiguous pages in the entire segment.
 * Uses chunk summaries for fast skip. Returns segment-relative page index
 * or UINT32_MAX if not found.
 */
uint32_t kb_seg_find_free_run(const kb_segment_t *seg, uint32_t n);

/*
 * Allocate `n` contiguous pages in the segment, marking them used in the
 * bitmap and setting the kind map. Returns the segment-relative page index
 * or UINT32_MAX if no run found.
 */
uint32_t kb_seg_alloc_pages(kb_segment_t *seg, uint32_t n, uint8_t kind);

/*
 * Free `n` contiguous pages starting at `start_page`.
 */
void kb_seg_free_pages(kb_segment_t *seg, uint32_t start_page, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* KB_SEGMENT_H */
