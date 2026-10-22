/*
 * test_pool.c — Pool create/open/close and page allocation tests (§2.1-§2.4).
 */

#include "test_main.h"
#include "core/kb_pool.h"
#include "core/kb_crc32c.h"

static const char *TEST_POOL_PATH = "/tmp/kb_test_pool.db";

/* ════════════════ Helper: clean up test files ════════════════ */

static void cleanup(void) {
    (void)kb_os_delete(TEST_POOL_PATH);
}

/* ════════════════ Create and close ════════════════ */

static void test_pool_create_close(void) {
    TEST_BEGIN("pool_create_close");
    cleanup();

    kb_pool_t *pool = NULL;
    kb_pool_config_t cfg = {
        .page_size       = KB_PAGE_SIZE_DEFAULT,
        .grow_step_mb    = 1,   /* small for test speed */
        .initial_size_mb = 1,
        .flags           = 0
    };

    TEST_OK(kb_pool_create(TEST_POOL_PATH, &cfg, &pool));
    TEST_ASSERT(pool != NULL, "pool is not NULL");
    TEST_ASSERT(pool->is_open, "pool is open");
    TEST_EQ_HEX(pool->sb.magic, KB_MAGIC_POOL);
    TEST_EQ(pool->sb.format_rev, KB_FORMAT_REV);
    TEST_EQ(pool->sb.page_size, KB_PAGE_SIZE_DEFAULT);
    TEST_EQ(pool->sb.segment_count, 1);

    /* File should exist. */
    bool exists = false;
    TEST_OK(kb_os_file_exists(TEST_POOL_PATH, &exists));
    TEST_ASSERT(exists, "pool file exists");

    /* Close. */
    TEST_OK(kb_pool_close(pool));

    cleanup();
}

/* ════════════════ Create, close, reopen ════════════════ */

static void test_pool_reopen(void) {
    TEST_BEGIN("pool_reopen");
    cleanup();

    kb_pool_t *pool = NULL;
    kb_pool_config_t cfg = {
        .page_size       = KB_PAGE_SIZE_DEFAULT,
        .grow_step_mb    = 1,
        .initial_size_mb = 1,
        .flags           = 0
    };

    /* Create. */
    TEST_OK(kb_pool_create(TEST_POOL_PATH, &cfg, &pool));
    uint8_t saved_uuid[16];
    memcpy(saved_uuid, pool->sb.pool_uuid, 16);
    uint64_t saved_seq = pool->sb.seq;
    TEST_OK(kb_pool_close(pool));

    /* Reopen. */
    pool = NULL;
    TEST_OK(kb_pool_open(TEST_POOL_PATH, &cfg, &pool));
    TEST_ASSERT(pool != NULL, "pool reopened");
    TEST_ASSERT(pool->is_open, "pool is open");

    /* UUID should match. */
    TEST_ASSERT(memcmp(pool->sb.pool_uuid, saved_uuid, 16) == 0,
                "uuid preserved across close/open");

    /* seq should have advanced (close writes, open writes). */
    TEST_ASSERT(pool->sb.seq > saved_seq, "seq advanced");

    /* Clean shutdown should have been set by close, then cleared by open. */
    TEST_EQ(pool->sb.clean_shutdown, 0);

    TEST_OK(kb_pool_close(pool));
    cleanup();
}

/* ════════════════ Duplicate create fails ════════════════ */

static void test_pool_create_exists(void) {
    TEST_BEGIN("pool_create_exists");
    cleanup();

    kb_pool_t *pool = NULL;
    kb_pool_config_t cfg = {
        .page_size       = KB_PAGE_SIZE_DEFAULT,
        .grow_step_mb    = 1,
        .initial_size_mb = 1,
        .flags           = 0
    };

    TEST_OK(kb_pool_create(TEST_POOL_PATH, &cfg, &pool));
    TEST_OK(kb_pool_close(pool));

    /* Second create should fail because the file already exists. */
    pool = NULL;
    TEST_EQ(kb_pool_create(TEST_POOL_PATH, &cfg, &pool), KB_E_EXISTS);
    TEST_ASSERT(pool == NULL, "pool is NULL on failure");

    cleanup();
}

/* ════════════════ Stats ════════════════ */

static void test_pool_stats(void) {
    TEST_BEGIN("pool_stats");
    cleanup();

    kb_pool_t *pool = NULL;
    kb_pool_config_t cfg = {
        .page_size       = KB_PAGE_SIZE_DEFAULT,
        .grow_step_mb    = 1,
        .initial_size_mb = 1,
        .flags           = 0
    };

    TEST_OK(kb_pool_create(TEST_POOL_PATH, &cfg, &pool));

    kb_pool_stats_t stats;
    TEST_OK(kb_pool_get_stats(pool, &stats));

    TEST_ASSERT(stats.file_size_bytes >= (1u << 20), "file >= 1 MiB");
    TEST_EQ(stats.segment_count, 1);
    TEST_ASSERT(stats.free_bytes_below_hwm >= 0, "free bytes reported");
    TEST_ASSERT(stats.provisioned_bytes > 0, "provisioned > 0");

    TEST_OK(kb_pool_close(pool));
    cleanup();
}

/* ════════════════ Page allocation ════════════════ */

static void test_pool_alloc_pages(void) {
    TEST_BEGIN("pool_alloc_pages");
    cleanup();

    kb_pool_t *pool = NULL;
    kb_pool_config_t cfg = {
        .page_size       = KB_PAGE_SIZE_DEFAULT,
        .grow_step_mb    = 1,
        .initial_size_mb = 1,
        .flags           = 0
    };

    TEST_OK(kb_pool_create(TEST_POOL_PATH, &cfg, &pool));

    /* Allocate a single header slab page. */
    kb_addr_t addr1 = KB_NULL_ADDR;
    TEST_OK(kb_pool_alloc_pages(pool, 1, KB_PAGE_KIND_HEADER_SLAB, &addr1));
    TEST_ASSERT(addr1 != KB_NULL_ADDR, "addr1 is valid");
    TEST_ASSERT(addr1 % KB_PAGE_SIZE_DEFAULT == 0, "addr1 is page-aligned");

    /* Allocate a 4-page extent. */
    kb_addr_t addr2 = KB_NULL_ADDR;
    TEST_OK(kb_pool_alloc_pages(pool, 4, KB_PAGE_KIND_EXTENT_DATA, &addr2));
    TEST_ASSERT(addr2 != KB_NULL_ADDR, "addr2 is valid");
    TEST_ASSERT(addr2 > addr1, "addr2 > addr1 (lowest-address-first)");

    /* Allocate another single page → should be at the lowest free address. */
    kb_addr_t addr3 = KB_NULL_ADDR;
    TEST_OK(kb_pool_alloc_pages(pool, 1, KB_PAGE_KIND_BLOCK_SLAB, &addr3));
    TEST_ASSERT(addr3 > addr2, "addr3 follows addr2");

    /* Free addr1 and allocate again → should reuse addr1's space. */
    TEST_OK(kb_pool_free_pages(pool, addr1, 1));

    kb_addr_t addr4 = KB_NULL_ADDR;
    TEST_OK(kb_pool_alloc_pages(pool, 1, KB_PAGE_KIND_HEADER_SLAB, &addr4));
    TEST_EQ(addr4, addr1);  /* lowest-address-first reuse */

    TEST_OK(kb_pool_close(pool));
    cleanup();
}

/* ════════════════ Allocation survives reopen ════════════════ */

static void test_pool_alloc_persist(void) {
    TEST_BEGIN("pool_alloc_persist");
    cleanup();

    kb_pool_t *pool = NULL;
    kb_pool_config_t cfg = {
        .page_size       = KB_PAGE_SIZE_DEFAULT,
        .grow_step_mb    = 1,
        .initial_size_mb = 1,
        .flags           = 0
    };

    TEST_OK(kb_pool_create(TEST_POOL_PATH, &cfg, &pool));

    /* Allocate 10 pages. */
    kb_addr_t addr = KB_NULL_ADDR;
    TEST_OK(kb_pool_alloc_pages(pool, 10, KB_PAGE_KIND_EXTENT_DATA, &addr));
    TEST_ASSERT(addr != KB_NULL_ADDR, "alloc ok");

    /* Write recognizable data to the first allocated page. */
    uint8_t marker[KB_PAGE_SIZE_DEFAULT];
    memset(marker, 0xAB, sizeof(marker));
    TEST_OK(kb_os_pwrite(pool->pool_fd, marker, sizeof(marker), addr));

    TEST_OK(kb_pool_close(pool));

    /* Reopen. */
    pool = NULL;
    TEST_OK(kb_pool_open(TEST_POOL_PATH, &cfg, &pool));

    /* Verify the marker data survived. */
    uint8_t verify[KB_PAGE_SIZE_DEFAULT];
    TEST_OK(kb_os_pread(pool->pool_fd, verify, sizeof(verify), addr));
    TEST_ASSERT(memcmp(verify, marker, sizeof(marker)) == 0,
                "data persisted across close/open");

    /* Verify the page is still marked as used in the bitmap. */
    uint32_t seg_page = (uint32_t)(addr / KB_PAGE_SIZE_DEFAULT);
    TEST_ASSERT(kb_seg_bitmap_test(&pool->segments[0], seg_page),
                "page still marked used after reopen");

    TEST_OK(kb_pool_close(pool));
    cleanup();
}

/* ════════════════ main ════════════════ */

int main(void) {
    printf("=== KB Pool Tests ===\n\n");

    kb_crc32c_init();

    test_pool_create_close();
    test_pool_reopen();
    test_pool_create_exists();
    test_pool_stats();
    test_pool_alloc_pages();
    test_pool_alloc_persist();

    TEST_SUMMARY();
}
