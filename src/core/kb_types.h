/*
 * kb_types.h — Fundamental types, constants, and error codes.
 *
 * Spec references: §1.1 (conventions), §1.2 (addresses and units),
 *                  §1.3 (integer codings), §2.6 (header core layout),
 *                  §7.5 (ABI types).
 *
 * All on-disk data is little-endian. Fields are read and written through
 * memcpy-based accessors, never by casting pointers to packed structs (§1.1).
 */
#ifndef KB_TYPES_H
#define KB_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════ Static assertions ══════════════════ */

/* Verify little-endian target (§1.1). GCC/Clang define these. */
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
               "KB requires a little-endian target");
#endif

/* ══════════════════ Core scalar types ══════════════════ */

/*
 * kb_addr_t: Byte offset in pool.db.
 * Derived from a 40-bit uref (unit index) × 64 (unit size).
 * 0 is null — unit 0 is the superblock page and never holds an object (§1.2).
 */
typedef uint64_t kb_addr_t;

/* Transaction handle. 0 = invalid. */
typedef uint64_t kb_tx_t;

/* Pin lease handle. 0 = invalid. Names exactly one pin (§5.4). */
typedef uint64_t kb_lease_t;

/* Status code. Negative = error, 0 = KB_OK. */
typedef int32_t  kb_status_t;

/* ══════════════════ Constants (§1.2) ══════════════════ */

#define KB_UNIT_SIZE            64u          /* bytes; one L1 cache line        */
#define KB_UNIT_SHIFT           6u           /* log2(KB_UNIT_SIZE)              */
#define KB_PAGE_SIZE_DEFAULT    4096u        /* bytes; format-time choice       */
#define KB_PAGE_SIZE_LARGE      16384u       /* 16 KiB alternative              */
#define KB_SEGMENT_SIZE         (2ULL << 30) /* 2 GiB                           */
#define KB_CHUNK_PAGES          512u         /* pages per chunk summary entry   */
#define KB_MAX_PATH_BYTES       1024u        /* path_uid hard ceiling (§1.4.5)  */
#define KB_MAX_DEPTH            1023u        /* max containment depth           */
#define KB_UREF_BITS            40u          /* bits in a unit reference         */
#define KB_UREF_MAX             ((1ULL << KB_UREF_BITS) - 1)
#define KB_MAX_POOL_BYTES       (KB_UREF_MAX * KB_UNIT_SIZE) /* ~64 TiB        */
#define KB_NULL_ADDR            ((kb_addr_t)0)

/* System-reserved birth numbers under the root (§2.11). */
#define KB_BIRTH_TYPE_DICT      1u
#define KB_BIRTH_SETTINGS       2u
#define KB_BIRTH_RECLAIM_QUEUE  3u
#define KB_BIRTH_LOST_FOUND     4u
#define KB_BIRTH_RESERVED_MAX   15u
#define KB_BIRTH_USER_START     16u

/* Magic numbers (little-endian u32). */
#define KB_MAGIC_POOL           0x4C504B42u  /* "KBPL" as LE u32 */
#define KB_MAGIC_INDEX          0x58494B42u  /* "KBIX" as LE u32 */
#define KB_MAGIC_WAL            0x4C574B42u  /* "KBWL" as LE u32 */

/* ══════════════════ Error codes (§7.5) ══════════════════ */

enum {
    KB_OK                = 0,
    KB_E_BAD_ARG         = -1,
    KB_E_NOT_FOUND       = -2,
    KB_E_EXISTS          = -3,
    KB_E_FROZEN          = -4,
    KB_E_CYCLE           = -5,
    KB_E_DEPTH           = -6,
    KB_E_LIMIT           = -7,
    KB_E_NO_RAM          = -8,
    KB_E_NO_SPACE        = -9,
    KB_E_IO              = -10,
    KB_E_CORRUPT         = -11,
    KB_E_BUSY            = -12,
    KB_E_TX_TOO_LARGE    = -13,
    KB_E_PINNED          = -14,
    KB_E_READONLY        = -15,
    KB_E_TYPE_UNKNOWN    = -16,
    KB_E_UNSUPPORTED     = -17,
    KB_E_STALE_REF       = -18,
    KB_E_TOO_FRAGMENTED  = -19,
    KB_E_INDEX_BUSY      = -20,
    KB_E_OVERFLOW        = -21,
    KB_E_BUF_TOO_SMALL   = -22
};

/* ══════════════════ Object reference (§2.6, N4) ══════════════════ */

/*
 * kb_ref_t: an (address, generation) pair. The generation guards against
 * the ABA problem where a freed header slot is reallocated to a new object.
 * A stale reference (wrong generation) fails with KB_E_STALE_REF (§2.6).
 */
typedef struct kb_ref {
    uint64_t addr;      /* byte offset of the 64-byte header; 0 = null     */
    uint32_t gen;       /* slot generation at the time the reference was made */
    uint32_t _pad;      /* padding to 16 bytes                              */
} kb_ref_t;

#define KB_NULL_REF  ((kb_ref_t){ 0, 0, 0 })

static inline bool kb_ref_is_null(kb_ref_t r) { return r.addr == 0; }

static inline bool kb_ref_eq(kb_ref_t a, kb_ref_t b) {
    return a.addr == b.addr && a.gen == b.gen;
}

/* ══════════════════ Unit reference ↔ byte address ══════════════════ */

static inline kb_addr_t kb_uref_to_addr(uint64_t uref) {
    return uref << KB_UNIT_SHIFT;
}

static inline uint64_t kb_addr_to_uref(kb_addr_t addr) {
    return addr >> KB_UNIT_SHIFT;
}

/* ══════════════════ Little-endian field accessors (§1.1) ══════════════════ */

/*
 * Read/write fixed-width LE integers from/to byte buffers via memcpy.
 * Compilers optimize these to single loads/stores on LE targets.
 * The memcpy guarantees strict-aliasing and alignment correctness per C11.
 */

static inline uint8_t  kb_read_u8 (const void *p) {
    uint8_t v; memcpy(&v, p, 1); return v;
}
static inline uint16_t kb_read_u16(const void *p) {
    uint16_t v; memcpy(&v, p, 2); return v;
}
static inline uint32_t kb_read_u32(const void *p) {
    uint32_t v; memcpy(&v, p, 4); return v;
}
static inline uint64_t kb_read_u64(const void *p) {
    uint64_t v; memcpy(&v, p, 8); return v;
}

static inline void kb_write_u8 (void *p, uint8_t  v) { memcpy(p, &v, 1); }
static inline void kb_write_u16(void *p, uint16_t v) { memcpy(p, &v, 2); }
static inline void kb_write_u32(void *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void kb_write_u64(void *p, uint64_t v) { memcpy(p, &v, 8); }

/*
 * Odd-width readers/writers for 24-bit and 40-bit fields.
 * Used for ext0_cap_units (u24), aux_len (u24), and all uref fields (u40).
 */
static inline uint32_t kb_read_u24(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16);
}
static inline void kb_write_u24(void *p, uint32_t v) {
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)(v);
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
}

static inline uint64_t kb_read_u40(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint64_t)b[0]
         | ((uint64_t)b[1] << 8)
         | ((uint64_t)b[2] << 16)
         | ((uint64_t)b[3] << 24)
         | ((uint64_t)b[4] << 32);
}
static inline void kb_write_u40(void *p, uint64_t v) {
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)(v);
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
    b[4] = (uint8_t)(v >> 32);
}

static inline uint64_t kb_read_u48(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint64_t)b[0]
         | ((uint64_t)b[1] << 8)
         | ((uint64_t)b[2] << 16)
         | ((uint64_t)b[3] << 24)
         | ((uint64_t)b[4] << 32)
         | ((uint64_t)b[5] << 40);
}
static inline void kb_write_u48(void *p, uint64_t v) {
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)(v);
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
    b[4] = (uint8_t)(v >> 32);
    b[5] = (uint8_t)(v >> 40);
}

/* ══════════════════ LEB128 codec (§1.3) ══════════════════ */

/*
 * Unsigned LEB128: used for lengths in index pages and WAL records.
 * Returns bytes written/consumed (1..10), or 0 on error.
 */
static inline int kb_leb128_encode(uint64_t val, uint8_t *out, size_t cap) {
    size_t n = 0;
    do {
        if (n >= cap) return 0;
        uint8_t byte = (uint8_t)(val & 0x7Fu);
        val >>= 7;
        if (val != 0) byte |= 0x80u;
        out[n] = byte;
        n++;
    } while (val != 0);
    return (int)n;
}

static inline int kb_leb128_decode(const uint8_t *in, size_t len, uint64_t *out) {
    uint64_t result = 0;
    unsigned shift = 0;
    for (size_t i = 0; i < len; i++) {
        uint64_t byte = in[i];
        if (shift >= 64) return 0; /* overflow */
        result |= (byte & 0x7Fu) << shift;
        shift += 7;
        if ((byte & 0x80u) == 0) {
            *out = result;
            return (int)(i + 1);
        }
    }
    return 0; /* truncated */
}

/* ══════════════════ Header layout constants (§2.6) ══════════════════ */

/*
 * On-disk ObjectHeader field offsets and sizes. The total is exactly 64 bytes
 * (one L1 cache line). NEVER cast a pointer to this layout; use the accessors.
 *
 * Off │ Size │ Field              │ Type
 * ────┼──────┼────────────────────┼──────
 *   0 │  1   │ state | hdr_rev    │ u8  (low 4 = state, high 4 = rev)
 *   1 │  1   │ type_version       │ u8
 *   2 │  2   │ type_id            │ u16
 *   4 │  4   │ flags              │ u32
 *   8 │  4   │ crc32c             │ u32
 *  12 │  5   │ parent_ref         │ u40
 *  17 │  5   │ children_ref       │ u40
 *  22 │  5   │ ext0_ref           │ u40
 *  27 │  3   │ ext0_cap_units     │ u24
 *  30 │  5   │ aux_ref            │ u40
 *  35 │  3   │ aux_len            │ u24
 *  38 │  4   │ birth_num          │ u32
 *  42 │  4   │ next_child_counter │ u32
 *  46 │  4   │ child_count        │ u32
 *  50 │  4   │ change_seq         │ u32
 *  54 │  4   │ slot_gen           │ u32
 *  58 │  6   │ payload_len        │ u48
 * ────┼──────┤
 *  64        │ TOTAL
 */
#define KB_HDR_SIZE                 64u

#define KB_HDR_OFF_STATE_REV        0    /* u8                              */
#define KB_HDR_OFF_TYPE_VERSION     1    /* u8                              */
#define KB_HDR_OFF_TYPE_ID          2    /* u16                             */
#define KB_HDR_OFF_FLAGS            4    /* u32                             */
#define KB_HDR_OFF_CRC32C           8    /* u32                             */
#define KB_HDR_OFF_PARENT_REF       12   /* u40 (5 bytes)                   */
#define KB_HDR_OFF_CHILDREN_REF     17   /* u40                             */
#define KB_HDR_OFF_EXT0_REF         22   /* u40                             */
#define KB_HDR_OFF_EXT0_CAP_UNITS   27   /* u24 (3 bytes)                   */
#define KB_HDR_OFF_AUX_REF          30   /* u40                             */
#define KB_HDR_OFF_AUX_LEN          35   /* u24                             */
#define KB_HDR_OFF_BIRTH_NUM        38   /* u32                             */
#define KB_HDR_OFF_NEXT_CHILD_CTR   42   /* u32                             */
#define KB_HDR_OFF_CHILD_COUNT      46   /* u32                             */
#define KB_HDR_OFF_CHANGE_SEQ       50   /* u32                             */
#define KB_HDR_OFF_SLOT_GEN         54   /* u32                             */
#define KB_HDR_OFF_PAYLOAD_LEN      58   /* u48 (6 bytes)                   */

_Static_assert(KB_HDR_OFF_PAYLOAD_LEN + 6 == KB_HDR_SIZE,
               "Header core layout must total exactly 64 bytes");

/* Object states (§2.6). */
#define KB_STATE_FREE       0u
#define KB_STATE_LIVE       1u
#define KB_STATE_DETACHED   2u   /* removed from parent, awaiting reclaim   */
#define KB_STATE_FORWARD    3u   /* reserved for V1.1 relocation stubs      */

/* Header flags (§2.6). */
#define KB_FLAG_PATH_FROZEN         (1u << 0)
#define KB_FLAG_HAS_FROZEN_BELOW    (1u << 1)
#define KB_FLAG_OBJ_IMMUTABLE       (1u << 2)
#define KB_FLAG_PAYLOAD_ENCRYPTED   (1u << 3)
#define KB_FLAG_HAS_AUX             (1u << 4)
#define KB_FLAG_HAS_PAYLOAD_CRC     (1u << 5)
/* Bits 6–7: CHILD_REPR (0=none, 1=inline, 2=array, 3=tree). */
#define KB_FLAG_CHILD_REPR_SHIFT    6u
#define KB_FLAG_CHILD_REPR_MASK     (3u << KB_FLAG_CHILD_REPR_SHIFT)
/* Bits 8–9: EXT_REPR (0=none, 1=inline, 2=table). */
#define KB_FLAG_EXT_REPR_SHIFT      8u
#define KB_FLAG_EXT_REPR_MASK       (3u << KB_FLAG_EXT_REPR_SHIFT)
#define KB_FLAG_SYSTEM              (1u << 10)
/* Bits 11–15: reserved. Bits 16–31: module-defined, preserved by kernel. */

#define KB_CHILD_REPR_NONE          0u
#define KB_CHILD_REPR_INLINE        1u
#define KB_CHILD_REPR_ARRAY         2u
#define KB_CHILD_REPR_TREE          3u

#define KB_EXT_REPR_NONE            0u
#define KB_EXT_REPR_INLINE          1u
#define KB_EXT_REPR_TABLE           2u

/* ══════════════════ Block layout constants ══════════════════ */

/* Block header (§2.7, §2.8): 16 bytes. */
#define KB_BLOCK_HDR_SIZE           16u

/* Child table entry (§2.8): 10 bytes packed.
 * birth_num(u32=4) + child_ref(u40=5) + flags(u8=1) = 10 bytes. */
#define KB_CHILD_ENTRY_SIZE         10u

/* Fragment tag (§2.9): 8 bytes.
 * owner_ref(u40=5) + extent_idx(u16=2) + crc8(u8=1) = 8 bytes. */
#define KB_FRAGMENT_TAG_SIZE        8u

/* ══════════════════ Slab constants (§2.5) ══════════════════ */

#define KB_SLAB_MAX_SLOTS           63u  /* u64 free mask; slot 0 = run hdr */

/* ══════════════════ Segment layout (§2.3) ══════════════════ */

/*
 * With the default 4 KiB page size, a 2 GiB segment has 524,288 pages:
 *     1 page    segment header
 *    16 pages   free bitmap  (524,288 bits = 65,536 bytes)
 *   128 pages   kind map     (524,288 bytes)
 *     2 pages   chunk summary (1024 chunks × 8 bytes = 8,192 bytes)
 *   ─────────
 *   147 pages   metadata (588 KiB, 0.028 % of the segment)
 *   524,141     allocatable pages
 */
#define KB_SEG_META_PAGES_4K        147u
#define KB_SEG_ALLOC_PAGES_4K       524141u

/* Page kind values (§2.3.2). */
#define KB_PAGE_KIND_FREE           0u
#define KB_PAGE_KIND_UNPROVISIONED  1u
#define KB_PAGE_KIND_SEG_META       2u
#define KB_PAGE_KIND_HEADER_SLAB    3u
#define KB_PAGE_KIND_BLOCK_SLAB     4u
#define KB_PAGE_KIND_EXTENT_DATA    5u
#define KB_PAGE_KIND_BLOCK_RUN      6u
#define KB_PAGE_KIND_CHILD_TREE     7u

/* ══════════════════ Aux block layout (§2.10) ══════════════════ */

/* Aux directory: 24 bytes. */
#define KB_AUX_DIR_SIZE             24u
#define KB_AUX_MAX_SIZE             (16u * 1024 * 1024)  /* 16 MiB */

/* Hook record: 24-byte fixed part + locator (padded to 8). */
#define KB_HOOK_FIXED_SIZE          24u
#define KB_HOOK_LOCATOR_MAX         255u

#ifdef __cplusplus
}
#endif

#endif /* KB_TYPES_H */
