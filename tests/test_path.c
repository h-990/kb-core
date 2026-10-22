/*
 * test_path.c — Path codec tests (§11.3: property-based + boundary).
 *
 * Tests the prefix-length varint encoding (§1.4) and all path operations
 * (§1.4.4). Covers:
 *   - Component encode/decode at every class boundary
 *   - Round-trip for all 10 boundary values + random values
 *   - Order-preservation property (memcmp order = numeric order)
 *   - Path construction, comparison, ancestry, depth, parent, LCA
 *   - Subtree range bounds
 *   - Validation and error cases
 *   - Iterator
 */

#include "test_main.h"
#include "core/kb_path.h"

/* ──── Simple LCG for property tests (deterministic, no deps) ──── */

static uint32_t rng_state = 0xDEADBEEF;

static uint32_t rng_next(void) {
    rng_state = rng_state * 1103515245u + 12345u;
    return rng_state;
}

/* Random birth number in [1, max]. */
static uint32_t rng_birth(uint32_t max) {
    if (max == 0) return 1;
    return (rng_next() % max) + 1;
}

/* ════════════════ Component boundary tests ════════════════ */

static void test_component_class_boundaries(void) {
    TEST_BEGIN("component_class_boundaries");

    uint8_t buf[5];
    uint32_t birth;
    int enc, dec;

    /* ── Class 1 boundaries ── */

    /* birth = 1 → 0x01 (1 byte) */
    enc = kb_component_encode(1, buf, sizeof(buf));
    TEST_EQ(enc, 1);
    TEST_EQ_HEX(buf[0], 0x01);
    dec = kb_component_decode(buf, 1, &birth);
    TEST_EQ(dec, 1);
    TEST_EQ(birth, 1);

    /* birth = 127 → 0x7F (1 byte, max class 1) */
    enc = kb_component_encode(127, buf, sizeof(buf));
    TEST_EQ(enc, 1);
    TEST_EQ_HEX(buf[0], 0x7F);
    dec = kb_component_decode(buf, 1, &birth);
    TEST_EQ(dec, 1);
    TEST_EQ(birth, 127);

    /* ── Class 2 boundaries ── */

    /* birth = 128 → 0x80 0x00 (2 bytes, min class 2) */
    enc = kb_component_encode(128, buf, sizeof(buf));
    TEST_EQ(enc, 2);
    TEST_EQ_HEX(buf[0], 0x80);
    TEST_EQ_HEX(buf[1], 0x00);
    dec = kb_component_decode(buf, 2, &birth);
    TEST_EQ(dec, 2);
    TEST_EQ(birth, 128);

    /* birth = 16511 → 0xBF 0xFF (2 bytes, max class 2) */
    enc = kb_component_encode(16511, buf, sizeof(buf));
    TEST_EQ(enc, 2);
    TEST_EQ_HEX(buf[0], 0xBF);
    TEST_EQ_HEX(buf[1], 0xFF);
    dec = kb_component_decode(buf, 2, &birth);
    TEST_EQ(dec, 2);
    TEST_EQ(birth, 16511);

    /* ── Class 3 boundaries ── */

    /* birth = 16512 → 0xC0 0x00 0x00 (3 bytes, min class 3) */
    enc = kb_component_encode(16512, buf, sizeof(buf));
    TEST_EQ(enc, 3);
    TEST_EQ_HEX(buf[0], 0xC0);
    TEST_EQ_HEX(buf[1], 0x00);
    TEST_EQ_HEX(buf[2], 0x00);
    dec = kb_component_decode(buf, 3, &birth);
    TEST_EQ(dec, 3);
    TEST_EQ(birth, 16512);

    /* birth = 2113663 → 0xDF 0xFF 0xFF (3 bytes, max class 3) */
    enc = kb_component_encode(2113663, buf, sizeof(buf));
    TEST_EQ(enc, 3);
    TEST_EQ_HEX(buf[0], 0xDF);
    TEST_EQ_HEX(buf[1], 0xFF);
    TEST_EQ_HEX(buf[2], 0xFF);
    dec = kb_component_decode(buf, 3, &birth);
    TEST_EQ(dec, 3);
    TEST_EQ(birth, 2113663);

    /* ── Class 4 boundaries ── */

    /* birth = 2113664 → 0xE0 0x00 0x00 0x00 (4 bytes, min class 4) */
    enc = kb_component_encode(2113664, buf, sizeof(buf));
    TEST_EQ(enc, 4);
    TEST_EQ_HEX(buf[0], 0xE0);
    dec = kb_component_decode(buf, 4, &birth);
    TEST_EQ(dec, 4);
    TEST_EQ(birth, 2113664);

    /* birth = 270549119 → 0xEF 0xFF 0xFF 0xFF (4 bytes, max class 4) */
    enc = kb_component_encode(270549119, buf, sizeof(buf));
    TEST_EQ(enc, 4);
    TEST_EQ_HEX(buf[0], 0xEF);
    TEST_EQ_HEX(buf[1], 0xFF);
    TEST_EQ_HEX(buf[2], 0xFF);
    TEST_EQ_HEX(buf[3], 0xFF);
    dec = kb_component_decode(buf, 4, &birth);
    TEST_EQ(dec, 4);
    TEST_EQ(birth, 270549119);

    /* ── Class 5 boundaries ── */

    /* birth = 270549120 → 0xF0 ... (5 bytes, min class 5) */
    enc = kb_component_encode(270549120, buf, sizeof(buf));
    TEST_EQ(enc, 5);
    TEST_EQ_HEX(buf[0], 0xF0);
    dec = kb_component_decode(buf, 5, &birth);
    TEST_EQ(dec, 5);
    TEST_EQ(birth, 270549120);

    /* birth = UINT32_MAX → 5 bytes, max class 5 */
    enc = kb_component_encode(UINT32_MAX, buf, sizeof(buf));
    TEST_EQ(enc, 5);
    dec = kb_component_decode(buf, 5, &birth);
    TEST_EQ(dec, 5);
    TEST_EQ(birth, UINT32_MAX);
}

/* ════════════════ Round-trip exhaustive at boundaries ════════════════ */

static void test_component_roundtrip_boundaries(void) {
    TEST_BEGIN("component_roundtrip_boundaries");

    /* Test every value within ±2 of each class boundary. */
    static const uint32_t pivots[] = {
        1, 2, 126, 127,                       /* class 1 */
        128, 129, 16510, 16511,                /* class 2 */
        16512, 16513, 2113662, 2113663,        /* class 3 */
        2113664, 2113665, 270549118, 270549119,/* class 4 */
        270549120, 270549121, UINT32_MAX - 1, UINT32_MAX /* class 5 */
    };

    uint8_t buf[5];
    for (size_t i = 0; i < sizeof(pivots) / sizeof(pivots[0]); i++) {
        uint32_t v = pivots[i];
        int enc = kb_component_encode(v, buf, sizeof(buf));
        TEST_ASSERT(enc > 0, "encode succeeds");

        uint32_t decoded;
        int dec = kb_component_decode(buf, (size_t)enc, &decoded);
        TEST_ASSERT(dec > 0, "decode succeeds");
        TEST_EQ(dec, enc);
        TEST_EQ(decoded, v);
    }
}

/* ════════════════ Random round-trip ════════════════ */

static void test_component_roundtrip_random(void) {
    TEST_BEGIN("component_roundtrip_random");

    uint8_t buf[5];
    for (int trial = 0; trial < 10000; trial++) {
        uint32_t v = rng_next();
        if (v == 0) v = 1;

        int enc = kb_component_encode(v, buf, sizeof(buf));
        TEST_ASSERT(enc >= 1 && enc <= 5, "encode returns 1..5");

        uint32_t decoded;
        int dec = kb_component_decode(buf, (size_t)enc, &decoded);
        TEST_ASSERT(dec > 0, "decode succeeds");
        TEST_EQ(decoded, v);
    }
}

/* ════════════════ Order-preservation property (§1.4.3 prop 2) ════════════════ */

static void test_component_order_preservation(void) {
    TEST_BEGIN("component_order_preservation");

    /*
     * Property: if a < b then encode(a) < encode(b) under memcmp.
     * Test across and within class boundaries.
     */
    static const uint32_t values[] = {
        1, 2, 50, 126, 127, 128, 129, 255, 1000, 16510, 16511,
        16512, 16513, 100000, 2113662, 2113663, 2113664, 2113665,
        270549118, 270549119, 270549120, 270549121, UINT32_MAX
    };
    size_t nv = sizeof(values) / sizeof(values[0]);

    uint8_t bufs[23][5]; /* same count as values[] */
    int lens[23];

    for (size_t i = 0; i < nv; i++) {
        lens[i] = kb_component_encode(values[i], bufs[i], 5);
        TEST_ASSERT(lens[i] > 0, "encode ok");
    }

    for (size_t i = 0; i + 1 < nv; i++) {
        /* values[i] < values[i+1], so encode(i) must sort before encode(i+1). */
        int min_len = lens[i] < lens[i + 1] ? lens[i] : lens[i + 1];
        int cmp = memcmp(bufs[i], bufs[i + 1], (size_t)min_len);
        if (cmp == 0) {
            /* Shorter must be the smaller value. */
            TEST_ASSERT(lens[i] < lens[i + 1],
                        "prefix tie: shorter encodes smaller value");
        } else {
            TEST_ASSERT(cmp < 0,
                        "bytewise order matches numeric order");
        }
    }
}

/* ════════════════ Component error cases ════════════════ */

static void test_component_errors(void) {
    TEST_BEGIN("component_errors");

    uint8_t buf[5];
    uint32_t birth;

    /* birth = 0 → BAD_ARG */
    TEST_EQ(kb_component_encode(0, buf, sizeof(buf)), KB_E_BAD_ARG);

    /* NULL output → BAD_ARG */
    TEST_EQ(kb_component_encode(1, NULL, 5), KB_E_BAD_ARG);

    /* Buffer too small for each class */
    TEST_EQ(kb_component_encode(1, buf, 0), KB_E_BUF_TOO_SMALL);
    TEST_EQ(kb_component_encode(128, buf, 1), KB_E_BUF_TOO_SMALL);
    TEST_EQ(kb_component_encode(16512, buf, 2), KB_E_BUF_TOO_SMALL);
    TEST_EQ(kb_component_encode(2113664, buf, 3), KB_E_BUF_TOO_SMALL);
    TEST_EQ(kb_component_encode(270549120, buf, 4), KB_E_BUF_TOO_SMALL);

    /* Decode: 0x00 lead byte → BAD_ARG (root marker, not a component) */
    buf[0] = 0x00;
    TEST_EQ(kb_component_decode(buf, 1, &birth), KB_E_BAD_ARG);

    /* Decode: reserved lead byte 0xF8 → BAD_ARG */
    buf[0] = 0xF8;
    TEST_EQ(kb_component_decode(buf, 1, &birth), KB_E_BAD_ARG);

    /* Decode: reserved lead byte 0xFF → BAD_ARG */
    buf[0] = 0xFF;
    TEST_EQ(kb_component_decode(buf, 1, &birth), KB_E_BAD_ARG);

    /* Decode: truncated class 2 (only 1 byte for 10xxxxxx) → BUF_TOO_SMALL */
    buf[0] = 0x80;
    TEST_EQ(kb_component_decode(buf, 1, &birth), KB_E_BUF_TOO_SMALL);

    /* Decode: NULL input → BAD_ARG */
    TEST_EQ(kb_component_decode(NULL, 1, &birth), KB_E_BAD_ARG);

    /* byte_len for valid leads */
    TEST_EQ(kb_component_byte_len(0x01), 1);
    TEST_EQ(kb_component_byte_len(0x7F), 1);
    TEST_EQ(kb_component_byte_len(0x80), 2);
    TEST_EQ(kb_component_byte_len(0xBF), 2);
    TEST_EQ(kb_component_byte_len(0xC0), 3);
    TEST_EQ(kb_component_byte_len(0xDF), 3);
    TEST_EQ(kb_component_byte_len(0xE0), 4);
    TEST_EQ(kb_component_byte_len(0xEF), 4);
    TEST_EQ(kb_component_byte_len(0xF0), 5);
    TEST_EQ(kb_component_byte_len(0xF7), 5);

    /* byte_len for invalid leads */
    TEST_EQ(kb_component_byte_len(0x00), KB_E_BAD_ARG);
    TEST_EQ(kb_component_byte_len(0xF8), KB_E_BAD_ARG);
    TEST_EQ(kb_component_byte_len(0xFF), KB_E_BAD_ARG);
}

/* ════════════════ Path construction ════════════════ */

static void test_path_root(void) {
    TEST_BEGIN("path_root");

    uint8_t buf[16];
    uint32_t len;

    TEST_OK(kb_path_root(buf, sizeof(buf), &len));
    TEST_EQ(len, 1);
    TEST_EQ_HEX(buf[0], 0x00);

    /* Validate the root path. */
    TEST_OK(kb_path_validate(buf, len));

    /* Root depth is 0. */
    uint32_t depth;
    TEST_OK(kb_path_depth(buf, len, &depth));
    TEST_EQ(depth, 0);

    /* Root has no parent. */
    uint32_t parent_len;
    TEST_EQ(kb_path_parent(buf, len, &parent_len), KB_E_BAD_ARG);

    /* Root has no last birth. */
    uint32_t birth;
    TEST_EQ(kb_path_last_birth(buf, len, &birth), KB_E_BAD_ARG);
}

static void test_path_build(void) {
    TEST_BEGIN("path_build");

    uint8_t buf[64];
    uint32_t len;

    /* depth 0 → root */
    TEST_OK(kb_path_build(NULL, 0, buf, sizeof(buf), &len));
    TEST_EQ(len, 1);
    TEST_EQ_HEX(buf[0], 0x00);

    /* depth 1: child 5 under root → [0x00, 0x05] */
    uint32_t births1[] = {5};
    TEST_OK(kb_path_build(births1, 1, buf, sizeof(buf), &len));
    TEST_EQ(len, 2);
    TEST_EQ_HEX(buf[0], 0x00);
    TEST_EQ_HEX(buf[1], 0x05);
    TEST_OK(kb_path_validate(buf, len));

    /* depth 3: root/5/128/1 */
    uint32_t births3[] = {5, 128, 1};
    TEST_OK(kb_path_build(births3, 3, buf, sizeof(buf), &len));
    /* Expected: 0x00, 0x05, 0x80 0x00, 0x01 → 5 bytes */
    TEST_EQ(len, 5);
    TEST_EQ_HEX(buf[0], 0x00);
    TEST_EQ_HEX(buf[1], 0x05);
    TEST_EQ_HEX(buf[2], 0x80);
    TEST_EQ_HEX(buf[3], 0x00);
    TEST_EQ_HEX(buf[4], 0x01);
    TEST_OK(kb_path_validate(buf, len));

    /* Verify depth. */
    uint32_t depth;
    TEST_OK(kb_path_depth(buf, len, &depth));
    TEST_EQ(depth, 3);
}

static void test_path_append_child(void) {
    TEST_BEGIN("path_append_child");

    uint8_t buf[64];
    uint32_t len;

    /* Start with root. */
    TEST_OK(kb_path_root(buf, sizeof(buf), &len));

    /* Append child 42 → [0x00, 0x2A] */
    TEST_OK(kb_path_append_child(buf, len, 42, buf, sizeof(buf), &len));
    TEST_EQ(len, 2);
    TEST_EQ_HEX(buf[0], 0x00);
    TEST_EQ_HEX(buf[1], 0x2A);

    /* Append child 200 (class 2) → [0x00, 0x2A, 0x80 0x48] */
    /* 200 - 128 = 72 = 0x48.  Lead = 0x80 | 0x00 = 0x80 */
    TEST_OK(kb_path_append_child(buf, len, 200, buf, sizeof(buf), &len));
    TEST_EQ(len, 4);
    TEST_EQ_HEX(buf[2], 0x80);
    TEST_EQ_HEX(buf[3], 0x48);
    TEST_OK(kb_path_validate(buf, len));

    /* Verify last birth. */
    uint32_t birth;
    TEST_OK(kb_path_last_birth(buf, len, &birth));
    TEST_EQ(birth, 200);
}

/* ════════════════ Path comparison & ancestry ════════════════ */

static void test_path_compare_and_ancestry(void) {
    TEST_BEGIN("path_compare_and_ancestry");

    uint8_t root[1], child5[2], child10[2], grandchild[5];
    uint32_t root_len, c5_len, c10_len, gc_len;

    TEST_OK(kb_path_root(root, sizeof(root), &root_len));

    uint32_t b5[] = {5};
    TEST_OK(kb_path_build(b5, 1, child5, sizeof(child5), &c5_len));

    uint32_t b10[] = {10};
    TEST_OK(kb_path_build(b10, 1, child10, sizeof(child10), &c10_len));

    uint32_t b5_3[] = {5, 3};
    TEST_OK(kb_path_build(b5_3, 2, grandchild, sizeof(grandchild), &gc_len));

    /* Root < child5 < child10 (§1.4.3 property 3) */
    TEST_ASSERT(kb_path_compare(root, root_len, child5, c5_len) < 0,
                "root < child5");
    TEST_ASSERT(kb_path_compare(child5, c5_len, child10, c10_len) < 0,
                "child5 < child10");

    /* Ancestor < descendant (§1.4.3 property 3) */
    TEST_ASSERT(kb_path_compare(child5, c5_len, grandchild, gc_len) < 0,
                "child5 < grandchild(5,3)");

    /* Grandchild sorts between child5 and child10 */
    TEST_ASSERT(kb_path_compare(grandchild, gc_len, child10, c10_len) < 0,
                "grandchild(5,3) < child10");

    /* Self-comparison = 0 */
    TEST_EQ(kb_path_compare(child5, c5_len, child5, c5_len), 0);

    /* Ancestry (§1.4.3 property 4) */
    TEST_ASSERT(kb_path_is_ancestor(root, root_len, child5, c5_len),
                "root is ancestor of child5");
    TEST_ASSERT(kb_path_is_ancestor(root, root_len, grandchild, gc_len),
                "root is ancestor of grandchild");
    TEST_ASSERT(kb_path_is_ancestor(child5, c5_len, grandchild, gc_len),
                "child5 is ancestor of grandchild");

    /* Not ancestor */
    TEST_ASSERT(!kb_path_is_ancestor(child10, c10_len, grandchild, gc_len),
                "child10 is NOT ancestor of grandchild(5,3)");
    TEST_ASSERT(!kb_path_is_ancestor(grandchild, gc_len, child5, c5_len),
                "grandchild is NOT ancestor of child5");

    /* Self is ancestor-or-equal */
    TEST_ASSERT(kb_path_is_ancestor(child5, c5_len, child5, c5_len),
                "child5 is ancestor-or-equal of itself");
}

/* ════════════════ Parent, LCA, subtree bounds ════════════════ */

static void test_path_parent_and_lca(void) {
    TEST_BEGIN("path_parent_and_lca");

    uint8_t pa[16], pb[16];
    uint32_t pa_len, pb_len;

    /* Build root/5/3 */
    uint32_t ba[] = {5, 3};
    TEST_OK(kb_path_build(ba, 2, pa, sizeof(pa), &pa_len));

    /* Parent of root/5/3 → root/5 (length 2) */
    uint32_t parent_len;
    TEST_OK(kb_path_parent(pa, pa_len, &parent_len));
    TEST_EQ(parent_len, 2);
    /* Verify it's [0x00, 0x05] */
    TEST_EQ_HEX(pa[0], 0x00);
    TEST_EQ_HEX(pa[1], 0x05);

    /* Parent of root/5 → root (length 1) */
    TEST_OK(kb_path_parent(pa, 2, &parent_len));
    TEST_EQ(parent_len, 1);

    /* Build root/5/7 */
    uint32_t bb[] = {5, 7};
    TEST_OK(kb_path_build(bb, 2, pb, sizeof(pb), &pb_len));

    /* LCA(root/5/3, root/5/7) → root/5 (length 2) */
    uint32_t lca_len;
    TEST_OK(kb_path_lca(pa, pa_len, pb, pb_len, &lca_len));
    TEST_EQ(lca_len, 2);

    /* LCA(root/5/3, root/10) → root (length 1) */
    uint8_t pc[2];
    uint32_t pc_len;
    uint32_t bc[] = {10};
    TEST_OK(kb_path_build(bc, 1, pc, sizeof(pc), &pc_len));
    TEST_OK(kb_path_lca(pa, pa_len, pc, pc_len, &lca_len));
    TEST_EQ(lca_len, 1);

    /* LCA of identical paths = the path itself */
    TEST_OK(kb_path_lca(pa, pa_len, pa, pa_len, &lca_len));
    TEST_EQ(lca_len, pa_len);
}

static void test_path_subtree_bounds(void) {
    TEST_BEGIN("path_subtree_bounds");

    uint8_t child5[2], upper[3], desc[16];
    uint32_t c5_len, upper_len, desc_len;

    /* child5 = [0x00, 0x05] */
    uint32_t b5[] = {5};
    TEST_OK(kb_path_build(b5, 1, child5, sizeof(child5), &c5_len));

    /* Upper bound = child5 || 0xFF → [0x00, 0x05, 0xFF] */
    TEST_OK(kb_path_subtree_upper(child5, c5_len, upper, sizeof(upper), &upper_len));
    TEST_EQ(upper_len, 3);
    TEST_EQ_HEX(upper[0], 0x00);
    TEST_EQ_HEX(upper[1], 0x05);
    TEST_EQ_HEX(upper[2], 0xFF);

    /* Descendants are in [child5, upper):
     * root/5/1, root/5/127, root/5/128 should all be < upper */
    uint32_t test_births[] = {1, 50, 127, 128, 16511, 16512};
    for (size_t i = 0; i < sizeof(test_births) / sizeof(test_births[0]); i++) {
        uint32_t bd[] = {5, test_births[i]};
        TEST_OK(kb_path_build(bd, 2, desc, sizeof(desc), &desc_len));

        /* desc >= child5 */
        TEST_ASSERT(kb_path_compare(desc, desc_len, child5, c5_len) > 0,
                    "descendant > parent");
        /* desc < upper */
        TEST_ASSERT(kb_path_compare(desc, desc_len, upper, upper_len) < 0,
                    "descendant < upper bound");
    }

    /* A non-descendant (root/6) should be >= upper */
    uint8_t child6[2];
    uint32_t c6_len;
    uint32_t b6[] = {6};
    TEST_OK(kb_path_build(b6, 1, child6, sizeof(child6), &c6_len));
    TEST_ASSERT(kb_path_compare(child6, c6_len, upper, upper_len) >= 0,
                "non-descendant >= upper bound");
}

/* ════════════════ Path iterator ════════════════ */

static void test_path_iterator(void) {
    TEST_BEGIN("path_iterator");

    /* Build root/5/128/42 */
    uint32_t births[] = {5, 128, 42};
    uint8_t path[16];
    uint32_t path_len;
    TEST_OK(kb_path_build(births, 3, path, sizeof(path), &path_len));

    kb_path_iter_t it;
    TEST_OK(kb_path_iter_init(&it, path, path_len));

    uint32_t birth;
    TEST_ASSERT(kb_path_iter_next(&it, &birth), "iter yields 1st");
    TEST_EQ(birth, 5);

    TEST_ASSERT(kb_path_iter_next(&it, &birth), "iter yields 2nd");
    TEST_EQ(birth, 128);

    TEST_ASSERT(kb_path_iter_next(&it, &birth), "iter yields 3rd");
    TEST_EQ(birth, 42);

    TEST_ASSERT(!kb_path_iter_next(&it, &birth), "iter exhausted");

    /* Iterator on root path yields nothing. */
    uint8_t root[1] = {0x00};
    TEST_OK(kb_path_iter_init(&it, root, 1));
    TEST_ASSERT(!kb_path_iter_next(&it, &birth), "root iter is empty");
}

/* ════════════════ Validation ════════════════ */

static void test_path_validation(void) {
    TEST_BEGIN("path_validation");

    /* Valid paths */
    uint8_t p1[] = {0x00};
    TEST_OK(kb_path_validate(p1, 1));

    uint8_t p2[] = {0x00, 0x01};
    TEST_OK(kb_path_validate(p2, 2));

    uint8_t p3[] = {0x00, 0x7F, 0x80, 0x00};
    TEST_OK(kb_path_validate(p3, 4));

    /* Invalid: doesn't start with 0x00 */
    uint8_t bad1[] = {0x01};
    TEST_EQ(kb_path_validate(bad1, 1), KB_E_CORRUPT);

    /* Invalid: reserved lead byte in component */
    uint8_t bad2[] = {0x00, 0xF8};
    TEST_EQ(kb_path_validate(bad2, 2), KB_E_CORRUPT);

    /* Invalid: truncated class 2 component */
    uint8_t bad3[] = {0x00, 0x80};
    TEST_EQ(kb_path_validate(bad3, 2), KB_E_CORRUPT);

    /* Invalid: 0x00 in component position (root marker repeated) */
    uint8_t bad4[] = {0x00, 0x05, 0x00};
    TEST_EQ(kb_path_validate(bad4, 3), KB_E_CORRUPT);

    /* Invalid: empty */
    TEST_EQ(kb_path_validate(NULL, 0), KB_E_BAD_ARG);
}

/* ════════════════ Order preservation property (paths, §1.4.3) ════════════════ */

static void test_path_order_property(void) {
    TEST_BEGIN("path_order_property");

    /*
     * Property test (§11.3): for random paths, if the birth tuple of A
     * is lexicographically less than B's, then memcmp(encode(A), encode(B)) < 0.
     */
    for (int trial = 0; trial < 5000; trial++) {
        uint32_t depth_a = (rng_next() % 5) + 1;
        uint32_t depth_b = (rng_next() % 5) + 1;

        uint32_t births_a[5], births_b[5];
        for (uint32_t i = 0; i < depth_a; i++) births_a[i] = rng_birth(200);
        for (uint32_t i = 0; i < depth_b; i++) births_b[i] = rng_birth(200);

        uint8_t pa[64], pb[64];
        uint32_t la, lb;
        if (kb_path_build(births_a, depth_a, pa, sizeof(pa), &la) != KB_OK) continue;
        if (kb_path_build(births_b, depth_b, pb, sizeof(pb), &lb) != KB_OK) continue;

        /* Determine the expected tuple order. */
        int tuple_cmp = 0;
        uint32_t min_d = depth_a < depth_b ? depth_a : depth_b;
        for (uint32_t i = 0; i < min_d; i++) {
            if (births_a[i] < births_b[i]) { tuple_cmp = -1; break; }
            if (births_a[i] > births_b[i]) { tuple_cmp =  1; break; }
        }
        if (tuple_cmp == 0) {
            if (depth_a < depth_b) tuple_cmp = -1;
            else if (depth_a > depth_b) tuple_cmp = 1;
        }

        int path_cmp = kb_path_compare(pa, la, pb, lb);

        /* Signs must agree. */
        if (tuple_cmp < 0) {
            TEST_ASSERT(path_cmp < 0, "tuple < ⟹ path <");
        } else if (tuple_cmp > 0) {
            TEST_ASSERT(path_cmp > 0, "tuple > ⟹ path >");
        } else {
            TEST_ASSERT(path_cmp == 0, "tuple == ⟹ path ==");
        }
    }
}

/* ════════════════ Ancestry property (paths, §1.4.3 prop 4) ════════════════ */

static void test_path_ancestry_property(void) {
    TEST_BEGIN("path_ancestry_property");

    /* If A's births are a prefix of B's births, then is_ancestor(A, B) is true. */
    for (int trial = 0; trial < 2000; trial++) {
        uint32_t depth = (rng_next() % 6) + 1;
        uint32_t births[6];
        for (uint32_t i = 0; i < depth; i++) births[i] = rng_birth(500);

        /* Pick a prefix depth. */
        uint32_t prefix_depth = rng_next() % (depth + 1);

        uint8_t full_path[64], prefix_path[64];
        uint32_t full_len, prefix_len;
        if (kb_path_build(births, depth, full_path, sizeof(full_path), &full_len) != KB_OK) continue;
        if (kb_path_build(births, prefix_depth, prefix_path, sizeof(prefix_path), &prefix_len) != KB_OK) continue;

        TEST_ASSERT(kb_path_is_ancestor(prefix_path, prefix_len, full_path, full_len),
                    "birth prefix ⟹ byte prefix (ancestry)");
    }
}

/* ════════════════ main ════════════════ */

int main(void) {
    printf("=== KB Path Codec Tests ===\n\n");

    test_component_class_boundaries();
    test_component_roundtrip_boundaries();
    test_component_roundtrip_random();
    test_component_order_preservation();
    test_component_errors();

    test_path_root();
    test_path_build();
    test_path_append_child();
    test_path_compare_and_ancestry();
    test_path_parent_and_lca();
    test_path_subtree_bounds();
    test_path_iterator();
    test_path_validation();

    test_path_order_property();
    test_path_ancestry_property();

    TEST_SUMMARY();
}
