/*
 * kb_slab.h — Slab allocator for fixed-size slots (§2.5).
 *
 * Small structures (header cores, blocks, small payloads) come from slab
 * runs. A run is 1–64 pages with a 64-byte run header (free mask, class,
 * slot count, CRC32C) followed by equal-sized slots.
 *
 * The free mask is one u64, so a run has at most 63 slots (bit 0 represents
 * the run header itself).
 *
 * Slab classes (§2.5 ladder):
 *   Multiples of 64 B up to 512 B, then 4 classes per doubling up to 16,384 B.
 *   Neighbouring classes differ by at most 25%, so rounding waste ≤ 20%.
 *
 * The size-class bucketed hole index (cherry-picked v2.0) provides O(1)
 * best-fit selection via a 32-bit non_empty_mask.
 */
#ifndef KB_SLAB_H
#define KB_SLAB_H

#include "core/kb_types.h"
#include "core/kb_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════ Slab class table (§2.5) ══════════════════ */

/*
 * Each class has:
 *   slot_bytes:   slot size in bytes
 *   run_pages:    number of pages per run
 *   max_slots:    number of usable slots (excluding the run header slot)
 *
 * Class index 0 = 64 bytes (headers); higher indices = larger slots.
 */
typedef struct kb_slab_class {
    uint32_t slot_bytes;    /* slot size                                */
    uint16_t run_pages;     /* pages per run                            */
    uint16_t max_slots;     /* usable slots per run (≤ 63)              */
} kb_slab_class_t;

/* Number of defined slab classes. */
#define KB_SLAB_NUM_CLASSES  9u

/* The slab class table (defined in kb_slab.c). */
extern const kb_slab_class_t kb_slab_classes[KB_SLAB_NUM_CLASSES];

/*
 * Find the smallest slab class that fits `size` bytes.
 * Returns the class index, or -1 if too large for any slab class.
 */
int kb_slab_class_for_size(uint32_t size);

/* ══════════════════ Run header on-disk (64 bytes) ══════════════════ */

/*
 * Off │ Size │ Field
 * ────┼──────┼──────────────
 *   0 │  4   │ magic  (0x42414C53  "SLAB" LE)
 *   4 │  1   │ class_idx
 *   5 │  1   │ flags  (bit 0: header-slab vs payload-slab)
 *   6 │  2   │ slot_count  (max usable slots)
 *   8 │  8   │ free_mask   (u64, bit per slot; 1 = free)
 *  16 │  4   │ used_count  (how many slots are occupied)
 *  20 │  4   │ crc32c
 *  24 │ 16   │ type_summary (for header slabs: up to 8 type_ids + overflow)
 *  40 │ 24   │ _reserved
 */
#define KB_MAGIC_SLAB       0x42414C53u   /* "SLAB" LE */

#define KB_SLAB_RUN_HDR_SIZE       64u
#define KB_SLAB_RUN_OFF_MAGIC       0
#define KB_SLAB_RUN_OFF_CLASS       4
#define KB_SLAB_RUN_OFF_FLAGS       5
#define KB_SLAB_RUN_OFF_SLOT_COUNT  6
#define KB_SLAB_RUN_OFF_FREE_MASK   8
#define KB_SLAB_RUN_OFF_USED_COUNT 16
#define KB_SLAB_RUN_OFF_CRC32C     20
#define KB_SLAB_RUN_OFF_TYPE_SUMM  24

#define KB_SLAB_FLAG_HEADER_SLAB   (1u << 0)

/* ══════════════════ In-memory run descriptor ══════════════════ */

typedef struct kb_slab_run {
    kb_addr_t    addr;       /* byte offset of run start in pool.db     */
    uint64_t     free_mask;  /* 1 = free slot                           */
    uint8_t      class_idx;  /* index into kb_slab_classes              */
    uint8_t      flags;
    uint16_t     slot_count; /* usable slots                            */
    uint16_t     used_count; /* occupied slots                          */
    bool         dirty;      /* needs write-back                        */
} kb_slab_run_t;

/* ══════════════════ Slab allocator state ══════════════════ */

/*
 * Per-class run list: a dynamic array of runs ordered by address
 * (lowest-address-first, §2.4.3). Each class has a separate list.
 *
 * v2.0 cherry-pick: the non_empty_mask tracks which classes have
 * at least one run with free slots, enabling O(1) best-fit selection.
 */
typedef struct kb_slab_class_state {
    kb_slab_run_t *runs;       /* dynamic array                         */
    uint32_t       run_count;  /* used entries                           */
    uint32_t       run_capacity;
} kb_slab_class_state_t;

typedef struct kb_slab_alloc {
    kb_pool_t              *pool;        /* owning pool                  */
    kb_slab_class_state_t   classes[KB_SLAB_NUM_CLASSES];
    uint32_t                non_empty_mask; /* v2.0: bit i = class i has free slots */
} kb_slab_alloc_t;

/* ══════════════════ Lifecycle ══════════════════ */

/*
 * Initialize the slab allocator for a pool.
 * Scans existing header-slab and block-slab pages to rebuild run lists.
 */
kb_status_t kb_slab_init(kb_slab_alloc_t *sa, kb_pool_t *pool);

/*
 * Flush dirty run headers and free resources.
 */
void kb_slab_destroy(kb_slab_alloc_t *sa);

/* ══════════════════ Slot allocation ══════════════════ */

/*
 * Allocate a slot of at least `size` bytes from a slab run.
 * Returns the byte offset in pool.db and the actual slot size.
 *
 * `kind` determines whether this is a header-slab or block/payload slab
 * (KB_PAGE_KIND_HEADER_SLAB vs KB_PAGE_KIND_BLOCK_SLAB).
 *
 * `hint_addr`: preferred locality (e.g., near parent). 0 = no preference.
 */
kb_status_t kb_slab_alloc(kb_slab_alloc_t *sa, uint32_t size,
                          uint8_t kind, kb_addr_t hint_addr,
                          kb_addr_t *out_addr, uint32_t *out_slot_size);

/*
 * Free a slot at the given address. The address must be the start of
 * a previously allocated slot.
 */
kb_status_t kb_slab_free(kb_slab_alloc_t *sa, kb_addr_t addr, uint8_t kind);

/* ══════════════════ Query ══════════════════ */

/*
 * Given a slot address, find which run it belongs to and which slot index.
 * Returns KB_OK and fills out_run and out_slot_idx, or KB_E_NOT_FOUND.
 */
kb_status_t kb_slab_find_run(const kb_slab_alloc_t *sa, kb_addr_t addr,
                             uint8_t kind,
                             const kb_slab_run_t **out_run,
                             uint32_t *out_slot_idx);

#ifdef __cplusplus
}
#endif

#endif /* KB_SLAB_H */
