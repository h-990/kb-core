/*
 * kb_path.h — Path codec and operations.
 *
 * Spec reference: §1.4 (prefix-length varint encoding, path_uid).
 *
 * A path_uid names an object by its position in the containment tree.
 * It is the byte 0x00 (root marker) followed by the component codes of
 * the birth numbers along the route from the root down to the object.
 * The root itself is the single byte 0x00.
 *
 * ═══════════════════════════════════════════════════════════════════
 * Component encoding (§1.4.2 — prefix-length varint)
 *
 * The number of leading 1-bits in the first byte gives the total byte
 * count of the component. Each class adds a base so every value has
 * exactly one minimal encoding.
 *
 *   Bytes │ Lead pattern │ Value range               │ Data bits
 *   ──────┼──────────────┼───────────────────────────┼──────────
 *     1   │ 0xxxxxxx     │ 1 .. 127                  │ 7
 *     2   │ 10xxxxxx     │ 128 .. 16,511              │ 14
 *     3   │ 110xxxxx     │ 16,512 .. 2,113,663        │ 21
 *     4   │ 1110xxxx     │ 2,113,664 .. 270,549,119   │ 28
 *     5   │ 11110xxx     │ 270,549,120 .. UINT32_MAX+  │ 35
 *
 * Lead bytes 0xF8..0xFF are reserved. Byte 0x00 is the root marker and
 * is never a valid component (property 7, §1.4.3).
 *
 * Properties verified by script (§1.4.3):
 *  1. Prefix-free
 *  2. Order-preserving (bytewise order = numeric order, across lengths)
 *  3. Pre-order (memcmp gives depth-first walk order)
 *  4. Ancestry is a byte prefix
 *  5. Subtree is one contiguous key range [P, P||0xFF)
 *  6. Unique canonical form
 *  7. 0x00 never appears after the root byte
 * ═══════════════════════════════════════════════════════════════════
 */
#ifndef KB_PATH_H
#define KB_PATH_H

#include "core/kb_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ──── Class boundary constants ──── */

#define KB_COMP_BASE_1    1u
#define KB_COMP_BASE_2    128u
#define KB_COMP_BASE_3    16512u
#define KB_COMP_BASE_4    2113664u
#define KB_COMP_BASE_5    270549120u

#define KB_COMP_MAX_1     127u
#define KB_COMP_MAX_2     16511u
#define KB_COMP_MAX_3     2113663u
#define KB_COMP_MAX_4     270549119u
/* Class 5 covers all remaining u32 values. */

/* ════════════════ Component-level operations ════════════════ */

/*
 * Encode a birth number (1 .. UINT32_MAX) into its varint representation.
 *
 * @param birth  Birth number (must be >= 1).
 * @param out    Output buffer.
 * @param cap    Capacity of the output buffer in bytes.
 * @return       Number of bytes written (1..5), or negative kb_status_t:
 *               KB_E_BAD_ARG if birth == 0,
 *               KB_E_BUF_TOO_SMALL if cap is insufficient.
 */
int kb_component_encode(uint32_t birth, uint8_t *out, size_t cap);

/*
 * Decode a varint component from a byte buffer.
 *
 * @param in         Input buffer positioned at the start of a component.
 * @param len        Remaining bytes in the buffer.
 * @param out_birth  Receives the decoded birth number (>= 1).
 * @return           Number of bytes consumed (1..5), or negative kb_status_t:
 *                   KB_E_BAD_ARG for 0x00 or reserved lead bytes (0xF8+),
 *                   KB_E_BUF_TOO_SMALL if the buffer is truncated,
 *                   KB_E_OVERFLOW if the decoded value exceeds UINT32_MAX.
 */
int kb_component_decode(const uint8_t *in, size_t len, uint32_t *out_birth);

/*
 * Return the byte length of a component given its lead byte.
 *
 * @return  1..5, or negative kb_status_t for invalid lead bytes
 *          (0x00 = root marker, 0xF8+ = reserved).
 */
int kb_component_byte_len(uint8_t lead);

/* ════════════════ Path construction ════════════════ */

/*
 * Write the root path: the single byte 0x00.
 */
kb_status_t kb_path_root(uint8_t *out, size_t cap, uint32_t *out_len);

/*
 * Build a complete path from an array of birth numbers.
 * births[0] is the first child under root, births[depth-1] is the leaf.
 * depth == 0 produces the root path [0x00].
 *
 * @return KB_E_DEPTH if the result would exceed KB_MAX_PATH_BYTES or
 *         KB_MAX_DEPTH; KB_E_BUF_TOO_SMALL if cap is insufficient.
 */
kb_status_t kb_path_build(const uint32_t *births, uint32_t depth,
                           uint8_t *out, size_t cap, uint32_t *out_len);

/*
 * Append one child component to an existing parent path.
 * `out` may alias `parent` (in-place extension) if cap is sufficient.
 *
 * @return KB_E_DEPTH if the result exceeds KB_MAX_PATH_BYTES.
 */
kb_status_t kb_path_append_child(const uint8_t *parent, uint32_t parent_len,
                                  uint32_t birth,
                                  uint8_t *out, size_t cap,
                                  uint32_t *out_len);

/* ════════════════ Path queries (§1.4.4) ════════════════ */

/*
 * Lexicographic comparison. Returns <0, 0, or >0 (like memcmp).
 * Ancestors sort before descendants (property 3).
 */
int kb_path_compare(const uint8_t *a, uint32_t a_len,
                    const uint8_t *b, uint32_t b_len);

/*
 * True iff `a` is an ancestor-or-equal of `b`.
 * Equivalent to "a is a byte prefix of b" (property 4).
 */
bool kb_path_is_ancestor(const uint8_t *a, uint32_t a_len,
                          const uint8_t *b, uint32_t b_len);

/*
 * Compute the depth (number of components after the root marker).
 * Root has depth 0; an immediate child of root has depth 1.
 */
kb_status_t kb_path_depth(const uint8_t *path, uint32_t len,
                           uint32_t *out_depth);

/*
 * Compute the parent path (drop the last component).
 * Returns KB_E_BAD_ARG for the root (no parent).
 */
kb_status_t kb_path_parent(const uint8_t *path, uint32_t len,
                            uint32_t *out_parent_len);

/*
 * Extract the last component's birth number.
 * Returns KB_E_BAD_ARG for the root (no components).
 */
kb_status_t kb_path_last_birth(const uint8_t *path, uint32_t len,
                                uint32_t *out_birth);

/*
 * Longest Common Ancestor: the longest common prefix rounded down to
 * a component boundary (§1.4.4).
 */
kb_status_t kb_path_lca(const uint8_t *a, uint32_t a_len,
                         const uint8_t *b, uint32_t b_len,
                         uint32_t *out_lca_len);

/*
 * Compute the exclusive upper bound for a subtree range scan.
 * upper = path || 0xFF. The subtree of `path` is [path, upper) (property 5).
 */
kb_status_t kb_path_subtree_upper(const uint8_t *path, uint32_t len,
                                   uint8_t *out, size_t cap,
                                   uint32_t *out_len);

/*
 * Validate a path_uid byte string. Checks:
 *  - Non-empty and starts with 0x00 (root marker).
 *  - Every component decodes successfully with a value >= 1.
 *  - No reserved lead bytes.
 *  - Total length <= KB_MAX_PATH_BYTES.
 *  - No trailing garbage bytes.
 */
kb_status_t kb_path_validate(const uint8_t *path, uint32_t len);

/* ════════════════ Path iterator ════════════════ */

/*
 * Parse components left-to-right from a path_uid.
 *
 * Usage:
 *   kb_path_iter_t it;
 *   kb_path_iter_init(&it, path, len);
 *   uint32_t birth;
 *   while (kb_path_iter_next(&it, &birth)) {
 *       // birth is the next component's value (1..UINT32_MAX)
 *   }
 */
typedef struct kb_path_iter {
    const uint8_t *data;
    uint32_t       len;
    uint32_t       pos;   /* current byte offset (starts after root marker) */
} kb_path_iter_t;

/*
 * Initialize the iterator. Advances past the 0x00 root marker.
 * Returns KB_E_BAD_ARG if the path is empty or doesn't start with 0x00.
 */
kb_status_t kb_path_iter_init(kb_path_iter_t *it,
                               const uint8_t *path, uint32_t len);

/*
 * Decode the next component. Returns true if a component was decoded
 * and *out_birth is set. Returns false when exhausted or on decode error
 * (sets *out_birth to 0 and advances pos to end on error).
 */
bool kb_path_iter_next(kb_path_iter_t *it, uint32_t *out_birth);

#ifdef __cplusplus
}
#endif

#endif /* KB_PATH_H */
