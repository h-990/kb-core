/*
 * kb_superblock.h — Superblock layout and operations (§2.2).
 *
 * Two copies at pool pages 0 and 1 (byte offsets 0 and page_size).
 * An update writes the copy with the lower seq number and increments it;
 * open picks the valid copy with the higher seq. A torn write always
 * leaves one good copy.
 *
 * The superblock is always page_size bytes but only the first
 * KB_SB_PAYLOAD_SIZE (252) bytes carry fields; the rest is zero-padded.
 * CRC32C covers the first 252 bytes (with the crc field zeroed).
 */
#ifndef KB_SUPERBLOCK_H
#define KB_SUPERBLOCK_H

#include "core/kb_types.h"
#include "core/kb_crc32c.h"
#include "core/kb_os.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════ On-disk field offsets ══════════════════ */

/*
 * Off │ Size │ Field                │ Type
 * ────┼──────┼──────────────────────┼──────
 *   0 │  4   │ magic                │ u32 (KBPL = 0x4C504B42)
 *   4 │  2   │ format_rev           │ u16
 *   6 │  2   │ page_size            │ u16
 *   8 │  2   │ unit_size            │ u16
 *  10 │  1   │ uref_bits            │ u8
 *  11 │  1   │ _pad0                │ u8 (reserved)
 *  12 │  4   │ segment_size_mb      │ u32 (segment size in MiB)
 *  16 │ 16   │ pool_uuid            │ 16 bytes
 *  32 │  8   │ created_unix         │ u64 (seconds since epoch)
 *  40 │  8   │ seq                  │ u64 (copy sequence number)
 *  48 │  1   │ clean_shutdown       │ u8 (0=open for write, 1=orderly close)
 *  49 │  3   │ _pad1                │ reserved
 *  52 │  5   │ root_ref             │ u40 (uref of the root header)
 *  57 │  3   │ _pad2                │ reserved
 *  60 │  8   │ provisioned_bytes    │ u64
 *  68 │  4   │ segment_count        │ u32
 *  72 │  4   │ _pad3                │ reserved
 *  76 │  8   │ hwm_unit             │ u64 (high-water mark in units)
 *  84 │  8   │ object_count         │ u64 (cached, recomputable)
 *  92 │  8   │ wal_checkpoint_lsn   │ u64
 * 100 │  4   │ slab_ladder_rev      │ u32
 * 104 │  4   │ feature_flags        │ u32
 * 108 │  4   │ grow_step_mb         │ u32 (grow step in MiB, default 64)
 * 112 │ 136  │ _reserved            │ zero
 * 248 │  4   │ crc32c               │ u32
 *                                     252 bytes payload
 */

#define KB_SB_PAYLOAD_SIZE       252u   /* CRC covers this many bytes */

/* Field offsets */
#define KB_SB_OFF_MAGIC              0
#define KB_SB_OFF_FORMAT_REV         4
#define KB_SB_OFF_PAGE_SIZE          6
#define KB_SB_OFF_UNIT_SIZE          8
#define KB_SB_OFF_UREF_BITS        10
#define KB_SB_OFF_SEGMENT_SIZE_MB   12
#define KB_SB_OFF_POOL_UUID         16
#define KB_SB_OFF_CREATED_UNIX      32
#define KB_SB_OFF_SEQ               40
#define KB_SB_OFF_CLEAN_SHUTDOWN    48
#define KB_SB_OFF_ROOT_REF          52
#define KB_SB_OFF_PROVISIONED       60
#define KB_SB_OFF_SEGMENT_COUNT     68
#define KB_SB_OFF_HWM_UNIT          76
#define KB_SB_OFF_OBJECT_COUNT      84
#define KB_SB_OFF_WAL_CKPT_LSN     92
#define KB_SB_OFF_SLAB_LADDER_REV  100
#define KB_SB_OFF_FEATURE_FLAGS    104
#define KB_SB_OFF_GROW_STEP_MB     108
#define KB_SB_OFF_CRC32C           248

/* Current format revision. */
#define KB_FORMAT_REV            1u

/* Default grow step: 64 MiB. */
#define KB_GROW_STEP_MB_DEFAULT  64u

/* ══════════════════ In-memory superblock ══════════════════ */

/*
 * Parsed superblock for in-memory use. Loaded at open, flushed at
 * checkpoint and close. The on-disk format uses the LE accessors;
 * this struct lives in host byte order.
 */
typedef struct kb_superblock {
    uint32_t magic;
    uint16_t format_rev;
    uint16_t page_size;
    uint16_t unit_size;
    uint8_t  uref_bits;
    uint32_t segment_size_mb;
    uint8_t  pool_uuid[16];
    uint64_t created_unix;
    uint64_t seq;
    uint8_t  clean_shutdown;
    uint64_t root_ref;           /* uref of root header                    */
    uint64_t provisioned_bytes;
    uint32_t segment_count;
    uint64_t hwm_unit;
    uint64_t object_count;
    uint64_t wal_checkpoint_lsn;
    uint32_t slab_ladder_rev;
    uint32_t feature_flags;
    uint32_t grow_step_mb;
    uint32_t crc32c;
} kb_superblock_t;

/* ══════════════════ Serialization ══════════════════ */

/*
 * Serialize a superblock into a page-sized buffer.
 * `page_buf` must be at least `sb->page_size` bytes, zeroed.
 * Sets the CRC field. Returns KB_OK or KB_E_BAD_ARG.
 */
kb_status_t kb_sb_serialize(const kb_superblock_t *sb, uint8_t *page_buf);

/*
 * Deserialize a superblock from a page-sized buffer.
 * Validates magic and CRC. Returns KB_OK or KB_E_CORRUPT.
 */
kb_status_t kb_sb_deserialize(const uint8_t *page_buf, kb_superblock_t *sb);

/* ══════════════════ Dual-copy logic (§2.2) ══════════════════ */

/*
 * Read both superblock copies from the pool file, validate each,
 * and return the valid one with the higher seq number.
 *
 * page_size: the page size to try (KB_PAGE_SIZE_DEFAULT first).
 * Returns KB_E_CORRUPT if neither copy is valid.
 */
kb_status_t kb_sb_load_best(kb_fd_t pool_fd, uint32_t page_size,
                            kb_superblock_t *out);

/*
 * Write the superblock to the copy with the lower seq (§2.2).
 * Increments sb->seq before writing. Syncs the file.
 */
kb_status_t kb_sb_flush(kb_fd_t pool_fd, kb_superblock_t *sb);

/* ══════════════════ Initialization ══════════════════ */

/*
 * Fill a superblock with defaults for a new pool.
 * `uuid` is 16 bytes of random identity.
 */
void kb_sb_init_new(kb_superblock_t *sb, const uint8_t uuid[16],
                    uint16_t page_size);

#ifdef __cplusplus
}
#endif

#endif /* KB_SUPERBLOCK_H */
