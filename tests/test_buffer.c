/*
 * test_buffer.c — Buffer pool tests (§5).
 */

#include "test_main.h"
#include "core/kb_buffer.h"
#include "core/kb_pool.h"
#include "core/kb_crc32c.h"

static const char *TEST_POOL_PATH = "/tmp/kb_test_buffer.db";

static void cleanup(void) {
    (void)kb_os_delete(TEST_POOL_PATH);
}

static kb_status_t
setup(kb_pool_t **pool)
{
    *pool = NULL;
    kb_pool_config_t cfg = {
        .page_size       = KB_PAGE_SIZE_DEFAULT,
        .grow_step_mb    = 2,
        .initial_size_mb = 2,
        .flags           = 0
    };
    return kb_pool_create(TEST_POOL_PATH, &cfg, pool);
}

/* ════════════════ Init / Destroy ════════════════ */

static void test_buf_init(void) {
    TEST_BEGIN("buf_init_destroy");
    cleanup();

    kb_pool_t *pool;
    TEST_OK(setup(&pool));

    kb_buf_pool_t bp;
    TEST_OK(kb_buf_init(&bp, pool, 16));
    TEST_EQ(bp.num_buffers, 16);
    TEST_EQ(bp.total_pins, 0);

    TEST_OK(kb_buf_destroy(&bp));
    kb_pool_close(pool);
    cleanup();
}

/* ════════════════ Pin / Unpin ════════════════ */

static void test_buf_pin_unpin(void) {
    TEST_BEGIN("buf_pin_unpin");
    cleanup();

    kb_pool_t *pool;
    TEST_OK(setup(&pool));

    /* Allocate a page to have a valid address. */
    kb_addr_t addr;
    TEST_OK(kb_pool_alloc_pages(pool, 1, KB_PAGE_KIND_EXTENT_DATA, &addr));

    /* Write known data at that address. */
    uint8_t pattern[4096];
    memset(pattern, 0xAB, sizeof(pattern));
    TEST_OK(kb_os_pwrite(pool->pool_fd, pattern, 4096, addr));

    kb_buf_pool_t bp;
    TEST_OK(kb_buf_init(&bp, pool, 8));

    /* Pin for read. */
    uint8_t *buf = NULL;
    TEST_OK(kb_buf_pin(&bp, addr, false, &buf));
    TEST_ASSERT(buf != NULL, "buf not null");
    TEST_EQ(buf[0], 0xAB);
    TEST_EQ(buf[4095], 0xAB);
    TEST_EQ(bp.total_pins, 1);
    TEST_EQ(bp.miss_count, 1);

    /* Pin again (cache hit). */
    uint8_t *buf2 = NULL;
    TEST_OK(kb_buf_pin(&bp, addr, false, &buf2));
    TEST_ASSERT(buf == buf2, "same buffer");
    TEST_EQ(bp.total_pins, 2);
    TEST_EQ(bp.hit_count, 1);

    /* Unpin both. */
    TEST_OK(kb_buf_unpin(&bp, addr, false));
    TEST_OK(kb_buf_unpin(&bp, addr, false));
    TEST_EQ(bp.total_pins, 0);

    TEST_OK(kb_buf_destroy(&bp));
    kb_pool_close(pool);
    cleanup();
}

/* ════════════════ Dirty writeback ════════════════ */

static void test_buf_dirty(void) {
    TEST_BEGIN("buf_dirty_writeback");
    cleanup();

    kb_pool_t *pool;
    TEST_OK(setup(&pool));

    kb_addr_t addr;
    TEST_OK(kb_pool_alloc_pages(pool, 1, KB_PAGE_KIND_EXTENT_DATA, &addr));

    kb_buf_pool_t bp;
    TEST_OK(kb_buf_init(&bp, pool, 8));

    /* Pin for write, modify. */
    uint8_t *buf = NULL;
    TEST_OK(kb_buf_pin(&bp, addr, true, &buf));
    memset(buf, 0xCD, 4096);
    TEST_OK(kb_buf_unpin(&bp, addr, true));

    /* Flush. */
    TEST_OK(kb_buf_flush_all(&bp));

    /* Verify on disk. */
    uint8_t verify[4096];
    TEST_OK(kb_os_pread(pool->pool_fd, verify, 4096, addr));
    TEST_EQ(verify[0], 0xCD);
    TEST_EQ(verify[2048], 0xCD);
    TEST_EQ(verify[4095], 0xCD);

    TEST_OK(kb_buf_destroy(&bp));
    kb_pool_close(pool);
    cleanup();
}

/* ════════════════ Eviction ════════════════ */

static void test_buf_eviction(void) {
    TEST_BEGIN("buf_eviction");
    cleanup();

    kb_pool_t *pool;
    TEST_OK(setup(&pool));

    /* Allocate 10 pages but use a 4-slot buffer pool. */
    kb_addr_t addrs[10];
    for (int i = 0; i < 10; i++) {
        TEST_OK(kb_pool_alloc_pages(pool, 1, KB_PAGE_KIND_EXTENT_DATA, &addrs[i]));
        /* Write page number as first byte. */
        uint8_t data[4096];
        memset(data, 0, sizeof(data));
        data[0] = (uint8_t)i;
        TEST_OK(kb_os_pwrite(pool->pool_fd, data, 4096, addrs[i]));
    }

    kb_buf_pool_t bp;
    TEST_OK(kb_buf_init(&bp, pool, 4));

    /* Pin and unpin pages 0..9 — this forces evictions. */
    for (int i = 0; i < 10; i++) {
        uint8_t *buf = NULL;
        TEST_OK(kb_buf_pin(&bp, addrs[i], false, &buf));
        TEST_EQ(buf[0], (uint8_t)i);
        TEST_OK(kb_buf_unpin(&bp, addrs[i], false));
    }

    /* We should have had cache misses. */
    TEST_ASSERT(bp.miss_count >= 5, "evictions happened");

    /* The last 4 pages should be cached (or some subset). */
    /* Re-pin the very last page — should be a hit. */
    uint8_t *buf = NULL;
    TEST_OK(kb_buf_pin(&bp, addrs[9], false, &buf));
    TEST_EQ(buf[0], 9);
    TEST_OK(kb_buf_unpin(&bp, addrs[9], false));

    TEST_OK(kb_buf_destroy(&bp));
    kb_pool_close(pool);
    cleanup();
}

/* ════════════════ Flush specific page ════════════════ */

static void test_buf_flush_page(void) {
    TEST_BEGIN("buf_flush_page");
    cleanup();

    kb_pool_t *pool;
    TEST_OK(setup(&pool));

    kb_addr_t addr;
    TEST_OK(kb_pool_alloc_pages(pool, 1, KB_PAGE_KIND_EXTENT_DATA, &addr));

    kb_buf_pool_t bp;
    TEST_OK(kb_buf_init(&bp, pool, 4));

    uint8_t *buf = NULL;
    TEST_OK(kb_buf_pin(&bp, addr, true, &buf));
    memset(buf, 0xEF, 4096);
    TEST_OK(kb_buf_unpin(&bp, addr, true));

    /* Flush just this page. */
    TEST_OK(kb_buf_flush_page(&bp, addr));

    /* Verify. */
    uint8_t verify[4096];
    TEST_OK(kb_os_pread(pool->pool_fd, verify, 4096, addr));
    TEST_EQ(verify[0], 0xEF);

    TEST_OK(kb_buf_destroy(&bp));
    kb_pool_close(pool);
    cleanup();
}

/* ════════════════ main ════════════════ */

int main(void) {
    printf("=== KB Buffer Pool Tests ===\n\n");

    kb_crc32c_init();

    test_buf_init();
    test_buf_pin_unpin();
    test_buf_dirty();
    test_buf_eviction();
    test_buf_flush_page();

    TEST_SUMMARY();
}
