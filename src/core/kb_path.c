/*
 * kb_path.c — Path codec implementation.
 *
 * Implements the prefix-length varint encoding from spec §1.4.
 *
 * Encoding scheme (§1.4.2):
 *   The number of leading 1-bits in the first byte gives the byte count.
 *   Each class subtracts a base and writes the remainder big-endian.
 *
 *   Class │ Bytes │ Lead     │ Base        │ Max
 *   ──────┼───────┼──────────┼─────────────┼─────────────
 *     1   │  1    │ 0xxxxxxx │ 1           │ 127
 *     2   │  2    │ 10xxxxxx │ 128         │ 16,511
 *     3   │  3    │ 110xxxxx │ 16,512      │ 2,113,663
 *     4   │  4    │ 1110xxxx │ 2,113,664   │ 270,549,119
 *     5   │  5    │ 11110xxx │ 270,549,120 │ all of u32
 */

#include "core/kb_path.h"
#include <string.h>

/* ═══════════════════ Component codec ═══════════════════ */

int
kb_component_encode(uint32_t birth, uint8_t *out, size_t cap)
{
    if (birth == 0 || out == NULL)
        return KB_E_BAD_ARG;

    if (birth <= KB_COMP_MAX_1) {
        /* Class 1: 0xxxxxxx.  birth ∈ [1, 127]. */
        if (cap < 1) return KB_E_BUF_TOO_SMALL;
        out[0] = (uint8_t)birth;
        return 1;
    }

    if (birth <= KB_COMP_MAX_2) {
        /* Class 2: 10xxxxxx + 1 byte.  birth ∈ [128, 16511]. */
        if (cap < 2) return KB_E_BUF_TOO_SMALL;
        uint32_t r = birth - KB_COMP_BASE_2;
        out[0] = (uint8_t)(0x80u | ((r >> 8) & 0x3Fu));
        out[1] = (uint8_t)(r & 0xFFu);
        return 2;
    }

    if (birth <= KB_COMP_MAX_3) {
        /* Class 3: 110xxxxx + 2 bytes.  birth ∈ [16512, 2113663]. */
        if (cap < 3) return KB_E_BUF_TOO_SMALL;
        uint32_t r = birth - KB_COMP_BASE_3;
        out[0] = (uint8_t)(0xC0u | ((r >> 16) & 0x1Fu));
        out[1] = (uint8_t)((r >> 8) & 0xFFu);
        out[2] = (uint8_t)(r & 0xFFu);
        return 3;
    }

    if (birth <= KB_COMP_MAX_4) {
        /* Class 4: 1110xxxx + 3 bytes.  birth ∈ [2113664, 270549119]. */
        if (cap < 4) return KB_E_BUF_TOO_SMALL;
        uint32_t r = birth - KB_COMP_BASE_4;
        out[0] = (uint8_t)(0xE0u | ((r >> 24) & 0x0Fu));
        out[1] = (uint8_t)((r >> 16) & 0xFFu);
        out[2] = (uint8_t)((r >> 8) & 0xFFu);
        out[3] = (uint8_t)(r & 0xFFu);
        return 4;
    }

    /* Class 5: 11110xxx + 4 bytes.  birth ∈ [270549120, UINT32_MAX]. */
    {
        if (cap < 5) return KB_E_BUF_TOO_SMALL;
        uint64_t r = (uint64_t)birth - (uint64_t)KB_COMP_BASE_5;
        out[0] = (uint8_t)(0xF0u | (uint8_t)((r >> 32) & 0x07u));
        out[1] = (uint8_t)((r >> 24) & 0xFFu);
        out[2] = (uint8_t)((r >> 16) & 0xFFu);
        out[3] = (uint8_t)((r >> 8)  & 0xFFu);
        out[4] = (uint8_t)(r & 0xFFu);
        return 5;
    }
}

int
kb_component_decode(const uint8_t *in, size_t len, uint32_t *out_birth)
{
    if (in == NULL || out_birth == NULL || len == 0)
        return KB_E_BAD_ARG;

    uint8_t b = in[0];

    if (b == 0x00) {
        /* 0x00 is the root marker, not a valid component (property 7). */
        return KB_E_BAD_ARG;
    }

    if (b < 0x80u) {
        /* Class 1: value = b directly.  b ∈ [0x01, 0x7F] → [1, 127]. */
        *out_birth = (uint32_t)b;
        return 1;
    }

    if (b < 0xC0u) {
        /* Class 2: 10xxxxxx + 1 byte. */
        if (len < 2) return KB_E_BUF_TOO_SMALL;
        uint32_t r = ((uint32_t)(b & 0x3Fu) << 8) | (uint32_t)in[1];
        *out_birth = r + KB_COMP_BASE_2;
        return 2;
    }

    if (b < 0xE0u) {
        /* Class 3: 110xxxxx + 2 bytes. */
        if (len < 3) return KB_E_BUF_TOO_SMALL;
        uint32_t r = ((uint32_t)(b & 0x1Fu) << 16)
                   | ((uint32_t)in[1] << 8)
                   | (uint32_t)in[2];
        *out_birth = r + KB_COMP_BASE_3;
        return 3;
    }

    if (b < 0xF0u) {
        /* Class 4: 1110xxxx + 3 bytes. */
        if (len < 4) return KB_E_BUF_TOO_SMALL;
        uint32_t r = ((uint32_t)(b & 0x0Fu) << 24)
                   | ((uint32_t)in[1] << 16)
                   | ((uint32_t)in[2] << 8)
                   | (uint32_t)in[3];
        *out_birth = r + KB_COMP_BASE_4;
        return 4;
    }

    if (b < 0xF8u) {
        /* Class 5: 11110xxx + 4 bytes. */
        if (len < 5) return KB_E_BUF_TOO_SMALL;
        uint64_t r = ((uint64_t)(b & 0x07u) << 32)
                   | ((uint64_t)in[1] << 24)
                   | ((uint64_t)in[2] << 16)
                   | ((uint64_t)in[3] << 8)
                   | (uint64_t)in[4];
        uint64_t val = r + (uint64_t)KB_COMP_BASE_5;
        if (val > UINT32_MAX) return KB_E_OVERFLOW;
        *out_birth = (uint32_t)val;
        return 5;
    }

    /* Reserved lead byte (0xF8..0xFF). */
    return KB_E_BAD_ARG;
}

int
kb_component_byte_len(uint8_t lead)
{
    if (lead == 0x00) return KB_E_BAD_ARG;   /* root marker       */
    if (lead <  0x80u) return 1;
    if (lead <  0xC0u) return 2;
    if (lead <  0xE0u) return 3;
    if (lead <  0xF0u) return 4;
    if (lead <  0xF8u) return 5;
    return KB_E_BAD_ARG;                      /* reserved 0xF8+    */
}


/* ═══════════════════ Path construction ═══════════════════ */

kb_status_t
kb_path_root(uint8_t *out, size_t cap, uint32_t *out_len)
{
    if (out == NULL || out_len == NULL) return KB_E_BAD_ARG;
    if (cap < 1) return KB_E_BUF_TOO_SMALL;
    out[0] = 0x00;
    *out_len = 1;
    return KB_OK;
}

kb_status_t
kb_path_build(const uint32_t *births, uint32_t depth,
              uint8_t *out, size_t cap, uint32_t *out_len)
{
    if (out == NULL || out_len == NULL) return KB_E_BAD_ARG;
    if (depth > 0 && births == NULL) return KB_E_BAD_ARG;
    if (depth > KB_MAX_DEPTH) return KB_E_DEPTH;
    if (cap < 1) return KB_E_BUF_TOO_SMALL;

    out[0] = 0x00;
    uint32_t pos = 1;

    for (uint32_t i = 0; i < depth; i++) {
        size_t remaining = (pos < (uint32_t)cap) ? cap - (size_t)pos : 0;
        int n = kb_component_encode(births[i], out + pos, remaining);
        if (n < 0) return (kb_status_t)n;
        pos += (uint32_t)n;
        if (pos > KB_MAX_PATH_BYTES) return KB_E_DEPTH;
    }

    *out_len = pos;
    return KB_OK;
}

kb_status_t
kb_path_append_child(const uint8_t *parent, uint32_t parent_len,
                     uint32_t birth,
                     uint8_t *out, size_t cap, uint32_t *out_len)
{
    if (parent == NULL || out == NULL || out_len == NULL) return KB_E_BAD_ARG;
    if (parent_len == 0 || parent[0] != 0x00) return KB_E_BAD_ARG;

    /* Encode the component into a temporary to measure its length
     * without writing past the caller's buffer. */
    uint8_t tmp[5];
    int comp_len = kb_component_encode(birth, tmp, sizeof(tmp));
    if (comp_len < 0) return (kb_status_t)comp_len;

    uint32_t new_len = parent_len + (uint32_t)comp_len;
    if (new_len > KB_MAX_PATH_BYTES) return KB_E_DEPTH;
    if ((size_t)new_len > cap) return KB_E_BUF_TOO_SMALL;

    /* Copy parent into output (memmove handles overlap / aliasing). */
    if (out != parent) {
        memmove(out, parent, parent_len);
    }
    /* Append the encoded component. */
    memcpy(out + parent_len, tmp, (size_t)comp_len);
    *out_len = new_len;
    return KB_OK;
}


/* ═══════════════════ Path queries ═══════════════════ */

int
kb_path_compare(const uint8_t *a, uint32_t a_len,
                const uint8_t *b, uint32_t b_len)
{
    uint32_t min_len = a_len < b_len ? a_len : b_len;
    int cmp = 0;
    if (min_len > 0) {
        cmp = memcmp(a, b, min_len);
    }
    if (cmp != 0) return cmp;
    if (a_len < b_len) return -1;
    if (a_len > b_len) return  1;
    return 0;
}

bool
kb_path_is_ancestor(const uint8_t *a, uint32_t a_len,
                    const uint8_t *b, uint32_t b_len)
{
    if (a_len > b_len) return false;
    return memcmp(a, b, a_len) == 0;
}

kb_status_t
kb_path_depth(const uint8_t *path, uint32_t len, uint32_t *out_depth)
{
    if (path == NULL || out_depth == NULL) return KB_E_BAD_ARG;
    if (len == 0 || path[0] != 0x00) return KB_E_BAD_ARG;

    uint32_t depth = 0;
    uint32_t pos = 1; /* skip root marker */

    while (pos < len) {
        int n = kb_component_byte_len(path[pos]);
        if (n < 0) return KB_E_CORRUPT;
        if (pos + (uint32_t)n > len) return KB_E_CORRUPT;
        pos += (uint32_t)n;
        depth++;
    }

    *out_depth = depth;
    return KB_OK;
}

kb_status_t
kb_path_parent(const uint8_t *path, uint32_t len, uint32_t *out_parent_len)
{
    if (path == NULL || out_parent_len == NULL) return KB_E_BAD_ARG;
    if (len == 0 || path[0] != 0x00) return KB_E_BAD_ARG;
    if (len == 1) return KB_E_BAD_ARG; /* root has no parent */

    /* Parse forward, tracking the start of the most recent component. */
    uint32_t pos = 1;
    uint32_t last_start = 1;

    while (pos < len) {
        last_start = pos;
        int n = kb_component_byte_len(path[pos]);
        if (n < 0) return KB_E_CORRUPT;
        if (pos + (uint32_t)n > len) return KB_E_CORRUPT;
        pos += (uint32_t)n;
    }

    *out_parent_len = last_start;
    return KB_OK;
}

kb_status_t
kb_path_last_birth(const uint8_t *path, uint32_t len, uint32_t *out_birth)
{
    if (path == NULL || out_birth == NULL) return KB_E_BAD_ARG;
    if (len <= 1 || path[0] != 0x00) return KB_E_BAD_ARG;

    /* Find the last component by parsing forward. */
    uint32_t pos = 1;
    uint32_t last_pos = 1;

    while (pos < len) {
        last_pos = pos;
        int n = kb_component_byte_len(path[pos]);
        if (n < 0) return KB_E_CORRUPT;
        if (pos + (uint32_t)n > len) return KB_E_CORRUPT;
        pos += (uint32_t)n;
    }

    /* Decode the last component. */
    int r = kb_component_decode(path + last_pos, len - last_pos, out_birth);
    if (r < 0) return (kb_status_t)r;
    return KB_OK;
}

kb_status_t
kb_path_lca(const uint8_t *a, uint32_t a_len,
            const uint8_t *b, uint32_t b_len,
            uint32_t *out_lca_len)
{
    if (a == NULL || b == NULL || out_lca_len == NULL) return KB_E_BAD_ARG;
    if (a_len == 0 || b_len == 0) return KB_E_BAD_ARG;
    if (a[0] != 0x00 || b[0] != 0x00) return KB_E_BAD_ARG;

    /* Find the longest byte-level common prefix. */
    uint32_t min_len = a_len < b_len ? a_len : b_len;
    uint32_t common = 0;
    while (common < min_len && a[common] == b[common]) {
        common++;
    }

    /* Round down to a component boundary.  Parse from position 1
     * (after the root marker), advancing while complete components
     * fit within the common prefix. */
    uint32_t pos = 1;
    uint32_t last_boundary = 1; /* the root path is always shared */

    while (pos < common) {
        int n = kb_component_byte_len(a[pos]);
        if (n < 0) break;
        if (pos + (uint32_t)n > common) break; /* component split by divergence */
        pos += (uint32_t)n;
        last_boundary = pos;
    }

    *out_lca_len = last_boundary;
    return KB_OK;
}

kb_status_t
kb_path_subtree_upper(const uint8_t *path, uint32_t len,
                      uint8_t *out, size_t cap, uint32_t *out_len)
{
    if (path == NULL || out == NULL || out_len == NULL) return KB_E_BAD_ARG;
    if (len == 0) return KB_E_BAD_ARG;

    uint32_t new_len = len + 1;
    if ((size_t)new_len > cap) return KB_E_BUF_TOO_SMALL;

    /* Copy path to output (memmove handles overlap). */
    if (out != path) {
        memmove(out, path, len);
    }
    /* Append 0xFF — the sentinel. Since all valid component lead bytes are
     * below 0xF8 (§1.4.3 property 5), every descendant sorts before this. */
    out[len] = 0xFF;
    *out_len = new_len;
    return KB_OK;
}

kb_status_t
kb_path_validate(const uint8_t *path, uint32_t len)
{
    if (path == NULL || len == 0) return KB_E_BAD_ARG;
    if (len > KB_MAX_PATH_BYTES) return KB_E_DEPTH;
    if (path[0] != 0x00) return KB_E_CORRUPT;

    uint32_t pos = 1;
    uint32_t depth = 0;

    while (pos < len) {
        uint32_t birth;
        int n = kb_component_decode(path + pos, (size_t)(len - pos), &birth);
        if (n < 0) return KB_E_CORRUPT;
        pos += (uint32_t)n;
        depth++;
        if (depth > KB_MAX_DEPTH) return KB_E_DEPTH;
    }

    /* All bytes consumed with no remainder → well-formed. */
    if (pos != len) return KB_E_CORRUPT;
    return KB_OK;
}


/* ═══════════════════ Path iterator ═══════════════════ */

kb_status_t
kb_path_iter_init(kb_path_iter_t *it, const uint8_t *path, uint32_t len)
{
    if (it == NULL || path == NULL || len == 0) return KB_E_BAD_ARG;
    if (path[0] != 0x00) return KB_E_BAD_ARG;
    it->data = path;
    it->len  = len;
    it->pos  = 1;  /* advance past root marker */
    return KB_OK;
}

bool
kb_path_iter_next(kb_path_iter_t *it, uint32_t *out_birth)
{
    if (it->pos >= it->len) return false;

    uint32_t dummy;
    uint32_t *target = out_birth ? out_birth : &dummy;

    int n = kb_component_decode(it->data + it->pos,
                                (size_t)(it->len - it->pos),
                                target);
    if (n < 0) {
        /* Corrupt data: stop iteration. */
        if (out_birth) *out_birth = 0;
        it->pos = it->len;
        return false;
    }
    it->pos += (uint32_t)n;
    return true;
}
