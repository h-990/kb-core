/*
 * kb_block.h — Generic block header layout (§2.7).
 *
 * A "block" is a variable-size structure stored in a block-slab slot
 * (classes 1–8). It has a 16-byte header followed by typed content.
 * Used for: child tables, extent tables, aux data, HeaderExtBlocks.
 *
 * Block header on-disk (16 bytes):
 *
 *  Off │ Size │ Field
 *  ────┼──────┼──────────────
 *    0 │  4   │ magic          (block-type specific)
 *    4 │  2   │ item_count     (entries in the block)
 *    6 │  2   │ capacity       (max entries at current size)
 *    8 │  4   │ owner_uref_lo  (low 32 bits of owning header uref)
 *   12 │  4   │ crc32c         (CRC of entire block incl. payload)
 */
#ifndef KB_BLOCK_H
#define KB_BLOCK_H

#include "core/kb_types.h"
#include "core/kb_slab.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Block header field offsets. */
#define KB_BLK_OFF_MAGIC         0
#define KB_BLK_OFF_ITEM_COUNT    4
#define KB_BLK_OFF_CAPACITY      6
#define KB_BLK_OFF_OWNER_LO      8
#define KB_BLK_OFF_CRC32C       12

/* Block type magics. */
#define KB_MAGIC_CHILD_BLOCK     0x444C4843u  /* "CHLD" LE */
#define KB_MAGIC_EXTENT_BLOCK    0x54585845u  /* "EXXT" LE */
#define KB_MAGIC_AUX_BLOCK       0x58554142u  /* "BAUX" LE */
#define KB_MAGIC_EXT_BLOCK       0x54584548u  /* "HEXT" LE */

/* ══════════════════ In-memory block header ══════════════════ */

typedef struct kb_block_hdr {
    uint32_t magic;
    uint16_t item_count;
    uint16_t capacity;
    uint32_t owner_uref_lo;
    uint32_t crc32c;

    /* Runtime. */
    kb_addr_t addr;          /* byte address in pool.db                */
    uint32_t  total_bytes;   /* total block size (header + payload)    */
    bool      dirty;
} kb_block_hdr_t;

/* ══════════════════ Block I/O ══════════════════ */

/*
 * Allocate a block of at least `payload_bytes` + 16 bytes header.
 * Returns the address and the slab slot size.
 */
kb_status_t kb_block_alloc(kb_slab_alloc_t *sa, uint32_t payload_bytes,
                           uint8_t kind, kb_addr_t hint,
                           kb_addr_t *out_addr, uint32_t *out_total);

/*
 * Read a block header from disk. Does NOT read the payload.
 */
kb_status_t kb_block_read_hdr(kb_pool_t *pool, kb_addr_t addr,
                              kb_block_hdr_t *hdr);

/*
 * Write a block header + payload buffer to disk.
 * Computes CRC over the entire buffer.
 * `buf` must be `total_bytes` long with header at [0..15] and
 * payload at [16..total_bytes-1].
 */
kb_status_t kb_block_write(kb_pool_t *pool, kb_addr_t addr,
                           uint8_t *buf, uint32_t total_bytes);

/*
 * Free a block back to the slab allocator.
 */
kb_status_t kb_block_free(kb_slab_alloc_t *sa, kb_addr_t addr, uint8_t kind);

#ifdef __cplusplus
}
#endif

#endif /* KB_BLOCK_H */
