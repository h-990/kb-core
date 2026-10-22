/*
 * test_extent.c — Extent management tests (§2.9).
 */

#include "test_main.h"
#include "core/kb_extent.h"
#include "core/kb_pool.h"
#include "core/kb_crc32c.h"

static const char *TEST_POOL_PATH = "/tmp/kb_test_extent.db";

static void cleanup(void) {
    (void)kb_os_delete(TEST_POOL_PATH);
}

static kb_status_t
setup(kb_pool_t **pool, kb_slab_alloc_t *sa, kb_header_t *hdr)
{
    *pool = NULL;
    kb_pool_config_t cfg = {
        .page_size       = KB_PAGE_SIZE_DEFAULT,
        .grow_step_mb    = 2,
        .initial_size_mb = 2,
        .flags           = 0
    };
    kb_status_t s = kb_pool_create(TEST_POOL_PATH, &cfg, pool);
    if (s != KB_OK) return s;
    s = kb_slab_init(sa, *pool);
    if (s != KB_OK) return s;

    memset(hdr, 0, sizeof(*hdr));
    hdr->type_id = 1;
    return kb_hdr_create(sa, hdr);
}

static void teardown(kb_pool_t *pool, kb_slab_alloc_t *sa) {
    kb_slab_destroy(sa);
    kb_pool_close(pool);
    cleanup();
}

/* ════════════════ Single extent (INLINE) ════════════════ */

static void test_extent_inline(void) {
    TEST_BEGIN("extent_inline");
    cleanup();

    kb_pool_t *pool;
    kb_slab_alloc_t sa;
    kb_header_t hdr;
    TEST_OK(setup(&pool, &sa, &hdr));

    TEST_EQ(kb_extent_count(&hdr), 0);

    /* Allocate a 4-page extent. */
    kb_addr_t addr = 0;
    TEST_OK(kb_extent_alloc(&sa, pool, &hdr, 4, &addr));
    TEST_ASSERT(addr != 0, "extent addr valid");
    TEST_EQ(kb_extent_count(&hdr), 1);

    /* Get inline. */
    kb_extent_entry_t e;
    TEST_OK(kb_extent_get_inline(&hdr, &e));
    TEST_ASSERT(e.ext_ref != 0, "ext_ref set");
    TEST_ASSERT(e.cap_units > 0, "cap_units set");

    /* Free all. */
    TEST_OK(kb_extent_free_all(&sa, pool, &hdr));
    TEST_EQ(kb_extent_count(&hdr), 0);
    TEST_EQ(kb_extent_get_inline(&hdr, &e), KB_E_NOT_FOUND);

    teardown(pool, &sa);
}

/* ════════════════ Two extents (TABLE promotion) ════════════════ */

static void test_extent_table(void) {
    TEST_BEGIN("extent_table_promotion");
    cleanup();

    kb_pool_t *pool;
    kb_slab_alloc_t sa;
    kb_header_t hdr;
    TEST_OK(setup(&pool, &sa, &hdr));

    /* First extent → INLINE. */
    kb_addr_t addr1 = 0;
    TEST_OK(kb_extent_alloc(&sa, pool, &hdr, 2, &addr1));
    TEST_EQ(kb_extent_count(&hdr), 1);

    /* Second extent → TABLE. */
    kb_addr_t addr2 = 0;
    TEST_OK(kb_extent_alloc(&sa, pool, &hdr, 3, &addr2));
    TEST_ASSERT(kb_extent_count(&hdr) >= 2, "table has 2+");
    TEST_ASSERT(addr1 != addr2, "different extents");

    /* Free all. */
    TEST_OK(kb_extent_free_all(&sa, pool, &hdr));
    TEST_EQ(kb_extent_count(&hdr), 0);

    teardown(pool, &sa);
}

/* ════════════════ Many extents ════════════════ */

static kb_status_t count_extent_cb(const kb_extent_entry_t *e, void *ctx) {
    (void)e;
    uint32_t *cnt = (uint32_t *)ctx;
    (*cnt)++;
    return KB_OK;
}

static void test_extent_many(void) {
    TEST_BEGIN("extent_many");
    cleanup();

    kb_pool_t *pool;
    kb_slab_alloc_t sa;
    kb_header_t hdr;
    TEST_OK(setup(&pool, &sa, &hdr));

    /* Allocate 20 extents. */
    kb_addr_t addrs[20];
    for (int i = 0; i < 20; i++) {
        TEST_OK(kb_extent_alloc(&sa, pool, &hdr, 1, &addrs[i]));
        TEST_ASSERT(addrs[i] != 0, "addr valid");
    }

    /* All unique. */
    for (int i = 0; i < 20; i++) {
        for (int j = i + 1; j < 20; j++) {
            TEST_ASSERT(addrs[i] != addrs[j], "addrs unique");
        }
    }

    /* Iterate should visit 20. */
    uint32_t count = 0;
    TEST_OK(kb_extent_iterate(pool, &hdr, count_extent_cb, &count));
    TEST_EQ(count, 20);

    /* Free all. */
    TEST_OK(kb_extent_free_all(&sa, pool, &hdr));
    TEST_EQ(kb_extent_count(&hdr), 0);

    teardown(pool, &sa);
}

/* ════════════════ main ════════════════ */

int main(void) {
    printf("=== KB Extent Tests ===\n\n");

    kb_crc32c_init();

    test_extent_inline();
    test_extent_table();
    test_extent_many();

    TEST_SUMMARY();
}
