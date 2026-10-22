/*
 * test_slab.c — Slab allocator tests (§2.5).
 */

#include "test_main.h"
#include "core/kb_slab.h"
#include "core/kb_pool.h"
#include "core/kb_crc32c.h"

static const char *TEST_POOL_PATH = "/tmp/kb_test_slab.db";

static void cleanup(void) {
    (void)kb_os_delete(TEST_POOL_PATH);
}

/* ════════════════ Class table ════════════════ */

static void test_slab_class_table(void) {
    TEST_BEGIN("slab_class_table");

    /* 64-byte header → class 0. */
    TEST_EQ(kb_slab_class_for_size(64), 0);
    TEST_EQ(kb_slab_classes[0].slot_bytes, 64);
    TEST_EQ(kb_slab_classes[0].run_pages, 1);
    TEST_EQ(kb_slab_classes[0].max_slots, 63);

    /* 65 bytes → class 1 (128 B). */
    TEST_EQ(kb_slab_class_for_size(65), 1);
    TEST_EQ(kb_slab_classes[1].slot_bytes, 128);

    /* Exact 128 → class 1. */
    TEST_EQ(kb_slab_class_for_size(128), 1);

    /* 129 → class 2 (256 B). */
    TEST_EQ(kb_slab_class_for_size(129), 2);

    /* 16384 → class 8 (last). */
    TEST_EQ(kb_slab_class_for_size(16384), 8);

    /* Too large → -1. */
    TEST_EQ(kb_slab_class_for_size(16385), -1);
    TEST_EQ(kb_slab_class_for_size(100000), -1);

    /* 1-byte allocation → class 0. */
    TEST_EQ(kb_slab_class_for_size(1), 0);
}

/* ════════════════ Basic alloc/free ════════════════ */

static void test_slab_alloc_free(void) {
    TEST_BEGIN("slab_alloc_free");
    cleanup();

    kb_pool_t *pool = NULL;
    kb_pool_config_t cfg = {
        .page_size       = KB_PAGE_SIZE_DEFAULT,
        .grow_step_mb    = 1,
        .initial_size_mb = 1,
        .flags           = 0
    };
    TEST_OK(kb_pool_create(TEST_POOL_PATH, &cfg, &pool));

    kb_slab_alloc_t sa;
    TEST_OK(kb_slab_init(&sa, pool));

    /* Allocate a header slot (64 bytes). */
    kb_addr_t addr1 = KB_NULL_ADDR;
    uint32_t slot_size = 0;
    TEST_OK(kb_slab_alloc(&sa, 64, KB_PAGE_KIND_HEADER_SLAB,
                          KB_NULL_ADDR, &addr1, &slot_size));
    TEST_ASSERT(addr1 != KB_NULL_ADDR, "addr1 valid");
    TEST_EQ(slot_size, 64);
    TEST_ASSERT(addr1 % 64 == 0, "addr1 64-byte aligned");

    /* Allocate another → different address. */
    kb_addr_t addr2 = KB_NULL_ADDR;
    TEST_OK(kb_slab_alloc(&sa, 64, KB_PAGE_KIND_HEADER_SLAB,
                          KB_NULL_ADDR, &addr2, &slot_size));
    TEST_ASSERT(addr2 != KB_NULL_ADDR, "addr2 valid");
    TEST_ASSERT(addr2 != addr1, "addr2 != addr1");
    TEST_EQ(slot_size, 64);

    /* They should be in the same run (adjacent slots). */
    TEST_ASSERT(addr2 - addr1 == 64 || addr1 - addr2 == 64,
                "slots are adjacent");

    /* Free addr1. */
    TEST_OK(kb_slab_free(&sa, addr1, KB_PAGE_KIND_HEADER_SLAB));

    /* Allocate again → should reuse addr1 (lowest free). */
    kb_addr_t addr3 = KB_NULL_ADDR;
    TEST_OK(kb_slab_alloc(&sa, 64, KB_PAGE_KIND_HEADER_SLAB,
                          KB_NULL_ADDR, &addr3, &slot_size));
    TEST_EQ(addr3, addr1);  /* lowest-address reuse */

    kb_slab_destroy(&sa);
    TEST_OK(kb_pool_close(pool));
    cleanup();
}

/* ════════════════ Fill a run and overflow ════════════════ */

static void test_slab_fill_run(void) {
    TEST_BEGIN("slab_fill_run");
    cleanup();

    kb_pool_t *pool = NULL;
    kb_pool_config_t cfg = {
        .page_size       = KB_PAGE_SIZE_DEFAULT,
        .grow_step_mb    = 1,
        .initial_size_mb = 1,
        .flags           = 0
    };
    TEST_OK(kb_pool_create(TEST_POOL_PATH, &cfg, &pool));

    kb_slab_alloc_t sa;
    TEST_OK(kb_slab_init(&sa, pool));

    /* Class 0 has 63 slots per run. Fill all 63. */
    kb_addr_t addrs[63];
    for (int i = 0; i < 63; i++) {
        TEST_OK(kb_slab_alloc(&sa, 64, KB_PAGE_KIND_HEADER_SLAB,
                              KB_NULL_ADDR, &addrs[i], NULL));
        TEST_ASSERT(addrs[i] != KB_NULL_ADDR, "slot allocated");
    }

    /* All 63 should be unique. */
    for (int i = 0; i < 63; i++) {
        for (int j = i + 1; j < 63; j++) {
            TEST_ASSERT(addrs[i] != addrs[j], "all slots unique");
        }
    }

    /* 64th allocation should create a second run. */
    kb_addr_t addr64 = KB_NULL_ADDR;
    TEST_OK(kb_slab_alloc(&sa, 64, KB_PAGE_KIND_HEADER_SLAB,
                          KB_NULL_ADDR, &addr64, NULL));
    TEST_ASSERT(addr64 != KB_NULL_ADDR, "64th slot ok");

    /* It should be in a different page (new run). */
    kb_addr_t first_run_base = addrs[0] - 64; /* run header is slot 0 */
    TEST_ASSERT(addr64 < first_run_base ||
                addr64 >= first_run_base + KB_PAGE_SIZE_DEFAULT,
                "64th slot in a new run");

    kb_slab_destroy(&sa);
    TEST_OK(kb_pool_close(pool));
    cleanup();
}

/* ════════════════ Larger class ════════════════ */

static void test_slab_larger_class(void) {
    TEST_BEGIN("slab_larger_class");
    cleanup();

    kb_pool_t *pool = NULL;
    kb_pool_config_t cfg = {
        .page_size       = KB_PAGE_SIZE_DEFAULT,
        .grow_step_mb    = 1,
        .initial_size_mb = 1,
        .flags           = 0
    };
    TEST_OK(kb_pool_create(TEST_POOL_PATH, &cfg, &pool));

    kb_slab_alloc_t sa;
    TEST_OK(kb_slab_init(&sa, pool));

    /* Allocate a 256-byte block slab slot. */
    kb_addr_t addr = KB_NULL_ADDR;
    uint32_t slot_size = 0;
    TEST_OK(kb_slab_alloc(&sa, 200, KB_PAGE_KIND_BLOCK_SLAB,
                          KB_NULL_ADDR, &addr, &slot_size));
    TEST_ASSERT(addr != KB_NULL_ADDR, "256-class ok");
    TEST_EQ(slot_size, 256);

    /* 1024-byte class. */
    kb_addr_t addr2 = KB_NULL_ADDR;
    TEST_OK(kb_slab_alloc(&sa, 1000, KB_PAGE_KIND_BLOCK_SLAB,
                          KB_NULL_ADDR, &addr2, &slot_size));
    TEST_EQ(slot_size, 1024);

    kb_slab_destroy(&sa);
    TEST_OK(kb_pool_close(pool));
    cleanup();
}

/* ════════════════ Empty run release ════════════════ */

static void test_slab_empty_run_release(void) {
    TEST_BEGIN("slab_empty_run_release");
    cleanup();

    kb_pool_t *pool = NULL;
    kb_pool_config_t cfg = {
        .page_size       = KB_PAGE_SIZE_DEFAULT,
        .grow_step_mb    = 1,
        .initial_size_mb = 1,
        .flags           = 0
    };
    TEST_OK(kb_pool_create(TEST_POOL_PATH, &cfg, &pool));

    kb_slab_alloc_t sa;
    TEST_OK(kb_slab_init(&sa, pool));

    /* Allocate one slot, then free it → run should be released. */
    kb_addr_t addr = KB_NULL_ADDR;
    TEST_OK(kb_slab_alloc(&sa, 64, KB_PAGE_KIND_HEADER_SLAB,
                          KB_NULL_ADDR, &addr, NULL));

    /* There should be 1 run in class 0. */
    TEST_EQ(sa.classes[0].run_count, 1);

    /* Free it. */
    TEST_OK(kb_slab_free(&sa, addr, KB_PAGE_KIND_HEADER_SLAB));

    /* Run should have been released (0 runs). */
    TEST_EQ(sa.classes[0].run_count, 0);

    /* non_empty_mask bit 0 should be clear. */
    TEST_ASSERT((sa.non_empty_mask & 1u) == 0,
                "non_empty_mask cleared");

    kb_slab_destroy(&sa);
    TEST_OK(kb_pool_close(pool));
    cleanup();
}

/* ════════════════ Persist across reopen ════════════════ */

static void test_slab_persist(void) {
    TEST_BEGIN("slab_persist");
    cleanup();

    kb_pool_t *pool = NULL;
    kb_pool_config_t cfg = {
        .page_size       = KB_PAGE_SIZE_DEFAULT,
        .grow_step_mb    = 1,
        .initial_size_mb = 1,
        .flags           = 0
    };
    TEST_OK(kb_pool_create(TEST_POOL_PATH, &cfg, &pool));

    kb_slab_alloc_t sa;
    TEST_OK(kb_slab_init(&sa, pool));

    /* Allocate 5 header slots. */
    kb_addr_t addrs[5];
    for (int i = 0; i < 5; i++) {
        TEST_OK(kb_slab_alloc(&sa, 64, KB_PAGE_KIND_HEADER_SLAB,
                              KB_NULL_ADDR, &addrs[i], NULL));
    }
    /* Free slot 2. */
    TEST_OK(kb_slab_free(&sa, addrs[2], KB_PAGE_KIND_HEADER_SLAB));

    kb_slab_destroy(&sa);
    TEST_OK(kb_pool_close(pool));

    /* Reopen and reinit. */
    pool = NULL;
    TEST_OK(kb_pool_open(TEST_POOL_PATH, &cfg, &pool));

    kb_slab_alloc_t sa2;
    TEST_OK(kb_slab_init(&sa2, pool));

    /* The run should exist with the correct free mask. */
    TEST_EQ(sa2.classes[0].run_count, 1);

    const kb_slab_run_t *run = &sa2.classes[0].runs[0];
    TEST_EQ(run->used_count, 4); /* 5 allocated - 1 freed */

    /* Allocating should reuse slot 2's address. */
    kb_addr_t reused = KB_NULL_ADDR;
    TEST_OK(kb_slab_alloc(&sa2, 64, KB_PAGE_KIND_HEADER_SLAB,
                          KB_NULL_ADDR, &reused, NULL));
    /* The lowest free slot should be slot 2's position. */
    /* (Slot indices: 1=addr[0], 2=addr[1], 3=addr[2](freed), 4=addr[3], 5=addr[4]) */
    /* After free of slot 3 (addrs[2]), realloc should pick slot 3 → addrs[2]. */
    TEST_EQ(reused, addrs[2]);

    kb_slab_destroy(&sa2);
    TEST_OK(kb_pool_close(pool));
    cleanup();
}

/* ════════════════ non_empty_mask (v2.0 cherry-pick) ════════════════ */

static void test_slab_non_empty_mask(void) {
    TEST_BEGIN("slab_non_empty_mask");
    cleanup();

    kb_pool_t *pool = NULL;
    kb_pool_config_t cfg = {
        .page_size       = KB_PAGE_SIZE_DEFAULT,
        .grow_step_mb    = 1,
        .initial_size_mb = 1,
        .flags           = 0
    };
    TEST_OK(kb_pool_create(TEST_POOL_PATH, &cfg, &pool));

    kb_slab_alloc_t sa;
    TEST_OK(kb_slab_init(&sa, pool));

    /* Initially no runs → mask = 0. */
    TEST_EQ(sa.non_empty_mask, 0);

    /* Allocate in class 0 → bit 0 set. */
    kb_addr_t addr = KB_NULL_ADDR;
    TEST_OK(kb_slab_alloc(&sa, 64, KB_PAGE_KIND_HEADER_SLAB,
                          KB_NULL_ADDR, &addr, NULL));
    TEST_ASSERT((sa.non_empty_mask & (1u << 0)) != 0, "bit 0 set");

    /* Allocate in class 2 (256 B) → bit 2 set. */
    kb_addr_t addr2 = KB_NULL_ADDR;
    TEST_OK(kb_slab_alloc(&sa, 200, KB_PAGE_KIND_BLOCK_SLAB,
                          KB_NULL_ADDR, &addr2, NULL));
    TEST_ASSERT((sa.non_empty_mask & (1u << 2)) != 0, "bit 2 set");

    /* Fill all 63 slots of class 0's run → bit 0 cleared. */
    kb_addr_t fill[62]; /* already have 1 */
    for (int i = 0; i < 62; i++) {
        TEST_OK(kb_slab_alloc(&sa, 64, KB_PAGE_KIND_HEADER_SLAB,
                              KB_NULL_ADDR, &fill[i], NULL));
    }
    TEST_ASSERT((sa.non_empty_mask & (1u << 0)) == 0,
                "bit 0 cleared when run full");

    /* Allocating one more creates a new run → bit 0 set again. */
    kb_addr_t overflow = KB_NULL_ADDR;
    TEST_OK(kb_slab_alloc(&sa, 64, KB_PAGE_KIND_HEADER_SLAB,
                          KB_NULL_ADDR, &overflow, NULL));
    TEST_ASSERT((sa.non_empty_mask & (1u << 0)) != 0,
                "bit 0 set after new run");

    kb_slab_destroy(&sa);
    TEST_OK(kb_pool_close(pool));
    cleanup();
}

/* ════════════════ main ════════════════ */

int main(void) {
    printf("=== KB Slab Allocator Tests ===\n\n");

    kb_crc32c_init();

    test_slab_class_table();
    test_slab_alloc_free();
    test_slab_fill_run();
    test_slab_larger_class();
    test_slab_empty_run_release();
    test_slab_persist();
    test_slab_non_empty_mask();

    TEST_SUMMARY();
}
