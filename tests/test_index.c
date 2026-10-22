/*
 * test_index.c — B+Tree index tests (§3).
 */

#include "test_main.h"
#include "core/kb_index.h"
#include "core/kb_path.h"
#include "core/kb_pool.h"
#include "core/kb_crc32c.h"

/* Helper: encode a birth_num path component, returning uint32_t. */
static uint32_t enc(uint32_t birth, uint8_t *out, size_t cap) {
    int r = kb_component_encode(birth, out, cap);
    return r > 0 ? (uint32_t)r : 0;
}

static const char *TEST_POOL_PATH = "/tmp/kb_test_index.db";

static void cleanup(void) {
    (void)kb_os_delete(TEST_POOL_PATH);
}

static kb_status_t
setup(kb_pool_t **pool, kb_index_t *idx)
{
    *pool = NULL;
    kb_pool_config_t cfg = {
        .page_size       = KB_PAGE_SIZE_DEFAULT,
        .grow_step_mb    = 4,
        .initial_size_mb = 4,
        .flags           = 0
    };
    kb_status_t s = kb_pool_create(TEST_POOL_PATH, &cfg, pool);
    if (s != KB_OK) return s;
    return kb_index_create(*pool, idx);
}

static void teardown(kb_pool_t *pool) {
    kb_pool_close(pool);
    cleanup();
}

/* ════════════════ Empty index ════════════════ */

static void test_index_empty(void) {
    TEST_BEGIN("index_empty");
    cleanup();

    kb_pool_t *pool;
    kb_index_t idx;
    TEST_OK(setup(&pool, &idx));

    TEST_EQ(kb_index_count(&idx), 0);
    TEST_EQ(kb_index_height(&idx), 0);

    /* Lookup should fail. */
    uint8_t key[] = {0x00, 0x01};
    kb_addr_t val;
    TEST_EQ(kb_index_lookup(&idx, key, 2, &val), KB_E_NOT_FOUND);

    /* Delete should fail. */
    TEST_EQ(kb_index_delete(&idx, key, 2), KB_E_NOT_FOUND);

    teardown(pool);
}

/* ════════════════ Single insert/lookup ════════════════ */

static void test_index_single(void) {
    TEST_BEGIN("index_single");
    cleanup();

    kb_pool_t *pool;
    kb_index_t idx;
    TEST_OK(setup(&pool, &idx));

    uint8_t key[] = {0x00, 0x05};
    TEST_OK(kb_index_insert(&idx, key, 2, 12345));
    TEST_EQ(kb_index_count(&idx), 1);

    kb_addr_t val;
    TEST_OK(kb_index_lookup(&idx, key, 2, &val));
    TEST_EQ(val, 12345);

    /* Duplicate should fail. */
    TEST_EQ(kb_index_insert(&idx, key, 2, 99999), KB_E_EXISTS);

    teardown(pool);
}

/* ════════════════ Insert + delete ════════════════ */

static void test_index_delete(void) {
    TEST_BEGIN("index_delete");
    cleanup();

    kb_pool_t *pool;
    kb_index_t idx;
    TEST_OK(setup(&pool, &idx));

    uint8_t k1[] = {0x00, 0x01};
    uint8_t k2[] = {0x00, 0x02};
    uint8_t k3[] = {0x00, 0x03};
    TEST_OK(kb_index_insert(&idx, k1, 2, 100));
    TEST_OK(kb_index_insert(&idx, k2, 2, 200));
    TEST_OK(kb_index_insert(&idx, k3, 2, 300));
    TEST_EQ(kb_index_count(&idx), 3);

    /* Delete middle. */
    TEST_OK(kb_index_delete(&idx, k2, 2));
    TEST_EQ(kb_index_count(&idx), 2);

    /* k2 not found. */
    kb_addr_t val;
    TEST_EQ(kb_index_lookup(&idx, k2, 2, &val), KB_E_NOT_FOUND);

    /* Others still there. */
    TEST_OK(kb_index_lookup(&idx, k1, 2, &val));
    TEST_EQ(val, 100);
    TEST_OK(kb_index_lookup(&idx, k3, 2, &val));
    TEST_EQ(val, 300);

    teardown(pool);
}

/* ════════════════ Many inserts (force splits) ════════════════ */

static void test_index_many(void) {
    TEST_BEGIN("index_many");
    cleanup();

    kb_pool_t *pool;
    kb_index_t idx;
    TEST_OK(setup(&pool, &idx));

    /* Insert 500 keys with path_uid encoding. */
    for (uint32_t i = 1; i <= 500; i++) {
        uint8_t key[8];
        key[0] = 0x00; /* root */
        uint32_t klen = 1;
        /* Encode birth_num as a path component. */
        uint32_t n = enc(i, key + 1, sizeof(key) - 1);
        TEST_ASSERT(n > 0, "encode ok");
        klen += n;

        TEST_OK(kb_index_insert(&idx, key, (uint16_t)klen, (kb_addr_t)(i * 64)));
    }

    TEST_EQ(kb_index_count(&idx), 500);
    TEST_ASSERT(kb_index_height(&idx) >= 1, "tree split happened");

    /* Look up all 500. */
    for (uint32_t i = 1; i <= 500; i++) {
        uint8_t key[8];
        key[0] = 0x00;
        uint32_t klen = 1 + enc(i, key + 1, sizeof(key) - 1);

        kb_addr_t val;
        TEST_OK(kb_index_lookup(&idx, key, (uint16_t)klen, &val));
        TEST_EQ(val, (kb_addr_t)(i * 64));
    }

    /* Delete every other key. */
    for (uint32_t i = 1; i <= 500; i += 2) {
        uint8_t key[8];
        key[0] = 0x00;
        uint32_t klen = 1 + enc(i, key + 1, sizeof(key) - 1);
        TEST_OK(kb_index_delete(&idx, key, (uint16_t)klen));
    }
    TEST_EQ(kb_index_count(&idx), 250);

    /* Remaining keys still findable. */
    for (uint32_t i = 2; i <= 500; i += 2) {
        uint8_t key[8];
        key[0] = 0x00;
        uint32_t klen = 1 + enc(i, key + 1, sizeof(key) - 1);

        kb_addr_t val;
        TEST_OK(kb_index_lookup(&idx, key, (uint16_t)klen, &val));
        TEST_EQ(val, (kb_addr_t)(i * 64));
    }

    teardown(pool);
}

/* ════════════════ In-order iteration ════════════════ */

typedef struct iter_ctx {
    uint32_t count;
    uint64_t last_val;
    bool     ordered;
} iter_ctx_t;

static kb_status_t
order_callback(const uint8_t *key, uint16_t key_len,
               kb_addr_t value, void *ctx)
{
    (void)key; (void)key_len;
    iter_ctx_t *ic = (iter_ctx_t *)ctx;
    if (ic->count > 0 && value <= ic->last_val) {
        ic->ordered = false;
    }
    ic->last_val = value;
    ic->count++;
    return KB_OK;
}

static void test_index_iterate(void) {
    TEST_BEGIN("index_iterate");
    cleanup();

    kb_pool_t *pool;
    kb_index_t idx;
    TEST_OK(setup(&pool, &idx));

    /* Insert 100 keys in order. Values match key order since
     * path_uid encoding is order-preserving. */
    for (uint32_t i = 1; i <= 100; i++) {
        uint8_t key[8];
        key[0] = 0x00;
        uint32_t klen = 1 + enc(i, key + 1, sizeof(key) - 1);
        TEST_OK(kb_index_insert(&idx, key, (uint16_t)klen, (kb_addr_t)i));
    }

    iter_ctx_t ic = { 0, 0, true };
    TEST_OK(kb_index_iterate(&idx, order_callback, &ic));
    TEST_EQ(ic.count, 100);
    TEST_ASSERT(ic.ordered, "iteration is in order");

    teardown(pool);
}

/* ════════════════ Prefix scan ════════════════ */

static kb_status_t
count_cb(const uint8_t *key, uint16_t key_len,
         kb_addr_t value, void *ctx)
{
    (void)key; (void)key_len; (void)value;
    uint32_t *cnt = (uint32_t *)ctx;
    (*cnt)++;
    return KB_OK;
}

static void test_index_prefix_scan(void) {
    TEST_BEGIN("index_prefix_scan");
    cleanup();

    kb_pool_t *pool;
    kb_index_t idx;
    TEST_OK(setup(&pool, &idx));

    /* Build a small tree:
     *   /1, /1/1, /1/2, /1/3, /2, /2/1, /3
     * Prefix scan for /1 should return /1, /1/1, /1/2, /1/3. */
    uint8_t k_1[8], k_1_1[16], k_1_2[16], k_1_3[16];
    uint8_t k_2[8], k_2_1[16], k_3[8];
    uint32_t l;

    /* /1 */
    k_1[0] = 0x00;
    l = 1 + enc(1, k_1 + 1, 7);
    TEST_OK(kb_index_insert(&idx, k_1, (uint16_t)l, 10));

    /* /1/1 */
    memcpy(k_1_1, k_1, l);
    uint32_t l2 = l + enc(1, k_1_1 + l, 8);
    TEST_OK(kb_index_insert(&idx, k_1_1, (uint16_t)l2, 11));

    /* /1/2 */
    memcpy(k_1_2, k_1, l);
    l2 = l + enc(2, k_1_2 + l, 8);
    TEST_OK(kb_index_insert(&idx, k_1_2, (uint16_t)l2, 12));

    /* /1/3 */
    memcpy(k_1_3, k_1, l);
    l2 = l + enc(3, k_1_3 + l, 8);
    TEST_OK(kb_index_insert(&idx, k_1_3, (uint16_t)l2, 13));

    /* /2 */
    k_2[0] = 0x00;
    l = 1 + enc(2, k_2 + 1, 7);
    TEST_OK(kb_index_insert(&idx, k_2, (uint16_t)l, 20));

    /* /2/1 */
    memcpy(k_2_1, k_2, l);
    l2 = l + enc(1, k_2_1 + l, 8);
    TEST_OK(kb_index_insert(&idx, k_2_1, (uint16_t)l2, 21));

    /* /3 */
    k_3[0] = 0x00;
    l = 1 + enc(3, k_3 + 1, 7);
    TEST_OK(kb_index_insert(&idx, k_3, (uint16_t)l, 30));

    TEST_EQ(kb_index_count(&idx), 7);

    /* Prefix scan for /1 → should get /1, /1/1, /1/2, /1/3. */
    uint8_t prefix[8];
    prefix[0] = 0x00;
    uint32_t plen = 1 + enc(1, prefix + 1, 7);

    uint32_t scan_count = 0;
    TEST_OK(kb_index_scan_prefix(&idx, prefix, (uint16_t)plen, count_cb, &scan_count));
    TEST_EQ(scan_count, 4);

    /* Prefix scan for /2 → /2, /2/1. */
    prefix[0] = 0x00;
    plen = 1 + enc(2, prefix + 1, 7);
    scan_count = 0;
    TEST_OK(kb_index_scan_prefix(&idx, prefix, (uint16_t)plen, count_cb, &scan_count));
    TEST_EQ(scan_count, 2);

    teardown(pool);
}

/* ════════════════ Reopen ════════════════ */

static void test_index_reopen(void) {
    TEST_BEGIN("index_reopen");
    cleanup();

    kb_addr_t root_addr;

    /* Create, insert, close. */
    {
        kb_pool_t *pool;
        kb_index_t idx;
        TEST_OK(setup(&pool, &idx));

        for (uint32_t i = 1; i <= 50; i++) {
            uint8_t key[8];
            key[0] = 0x00;
            uint32_t klen = 1 + enc(i, key + 1, 7);
            TEST_OK(kb_index_insert(&idx, key, (uint16_t)klen, (kb_addr_t)(i * 100)));
        }

        root_addr = idx.root_addr;
        kb_pool_close(pool);
    }

    /* Reopen and verify. */
    {
        kb_pool_t *pool = NULL;
        kb_pool_config_t cfg2 = {
            .page_size       = KB_PAGE_SIZE_DEFAULT,
            .grow_step_mb    = 4,
            .initial_size_mb = 4,
            .flags           = 0
        };
        TEST_OK(kb_pool_open(TEST_POOL_PATH, &cfg2, &pool));

        kb_index_t idx;
        TEST_OK(kb_index_open(pool, root_addr, &idx));

        for (uint32_t i = 1; i <= 50; i++) {
            uint8_t key[8];
            key[0] = 0x00;
            uint32_t klen = 1 + enc(i, key + 1, 7);
            kb_addr_t val;
            TEST_OK(kb_index_lookup(&idx, key, (uint16_t)klen, &val));
            TEST_EQ(val, (kb_addr_t)(i * 100));
        }

        kb_pool_close(pool);
    }

    cleanup();
}

/* ════════════════ main ════════════════ */

int main(void) {
    printf("=== KB Index Tests ===\n\n");

    kb_crc32c_init();

    test_index_empty();
    test_index_single();
    test_index_delete();
    test_index_many();
    test_index_iterate();
    test_index_prefix_scan();
    test_index_reopen();

    TEST_SUMMARY();
}
