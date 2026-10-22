/*
 * test_child.c — Child table tests (§2.8).
 */

#include "test_main.h"
#include "core/kb_child.h"
#include "core/kb_pool.h"
#include "core/kb_crc32c.h"

static const char *TEST_POOL_PATH = "/tmp/kb_test_child.db";

static void cleanup(void) {
    (void)kb_os_delete(TEST_POOL_PATH);
}

/* Helper: create pool + slab + parent header. */
static kb_status_t
setup(kb_pool_t **pool, kb_slab_alloc_t *sa, kb_header_t *parent)
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

    memset(parent, 0, sizeof(*parent));
    parent->type_id = 1;
    return kb_hdr_create(sa, parent);
}

static void teardown(kb_pool_t *pool, kb_slab_alloc_t *sa) {
    kb_slab_destroy(sa);
    kb_pool_close(pool);
    cleanup();
}

/* ════════════════ NONE → INLINE ════════════════ */

static void test_child_inline(void) {
    TEST_BEGIN("child_inline");
    cleanup();

    kb_pool_t *pool;
    kb_slab_alloc_t sa;
    kb_header_t parent;
    TEST_OK(setup(&pool, &sa, &parent));

    /* Initially no children. */
    TEST_EQ(kb_child_count(&parent), 0);

    /* Add one child → should go INLINE. */
    uint32_t bn = 0;
    TEST_OK(kb_child_add(&sa, pool, &parent, 0xABCDE, &bn));
    TEST_EQ(bn, 0);
    TEST_EQ(kb_child_count(&parent), 1);

    /* Find it. */
    uint64_t ref = 0;
    TEST_OK(kb_child_find(pool, &parent, 0, &ref));
    TEST_EQ(ref, 0xABCDE);

    /* Iterate. */
    uint32_t iter_count = 0;
    kb_child_iterate(pool, &parent, (kb_child_iter_fn)(void (*)(void)){NULL}, NULL);
    /* Use a proper callback: */
    /* For simplicity, just verify count via child_count. */

    /* Remove it. */
    TEST_OK(kb_child_remove(pool, &parent, 0));
    TEST_EQ(kb_child_count(&parent), 0);
    TEST_EQ(kb_child_find(pool, &parent, 0, &ref), KB_E_NOT_FOUND);

    teardown(pool, &sa);
    (void)iter_count;
}

/* ════════════════ INLINE → ARRAY promotion ════════════════ */

static void test_child_promote(void) {
    TEST_BEGIN("child_promote_to_array");
    cleanup();

    kb_pool_t *pool;
    kb_slab_alloc_t sa;
    kb_header_t parent;
    TEST_OK(setup(&pool, &sa, &parent));

    /* Add first child → INLINE. */
    uint32_t bn0, bn1;
    TEST_OK(kb_child_add(&sa, pool, &parent, 100, &bn0));
    TEST_EQ(kb_child_count(&parent), 1);

    /* Add second → promotes to ARRAY. */
    TEST_OK(kb_child_add(&sa, pool, &parent, 200, &bn1));
    TEST_EQ(kb_child_count(&parent), 2);
    TEST_ASSERT(bn1 > bn0, "birth_nums monotonic");

    /* Both are findable. */
    uint64_t ref;
    TEST_OK(kb_child_find(pool, &parent, bn0, &ref));
    TEST_EQ(ref, 100);
    TEST_OK(kb_child_find(pool, &parent, bn1, &ref));
    TEST_EQ(ref, 200);

    teardown(pool, &sa);
}

/* ════════════════ Many children ════════════════ */

static void test_child_many(void) {
    TEST_BEGIN("child_many");
    cleanup();

    kb_pool_t *pool;
    kb_slab_alloc_t sa;
    kb_header_t parent;
    TEST_OK(setup(&pool, &sa, &parent));

    /* Add 50 children. */
    uint32_t bns[50];
    for (int i = 0; i < 50; i++) {
        TEST_OK(kb_child_add(&sa, pool, &parent, (uint64_t)(i + 1000), &bns[i]));
    }
    TEST_EQ(kb_child_count(&parent), 50);

    /* Find each. */
    for (int i = 0; i < 50; i++) {
        uint64_t ref;
        TEST_OK(kb_child_find(pool, &parent, bns[i], &ref));
        TEST_EQ(ref, (uint64_t)(i + 1000));
    }

    /* birth_nums are monotonic. */
    for (int i = 1; i < 50; i++) {
        TEST_ASSERT(bns[i] > bns[i-1], "birth_num monotonic");
    }

    /* Remove child 25. */
    TEST_OK(kb_child_remove(pool, &parent, bns[25]));
    TEST_EQ(kb_child_count(&parent), 49);

    /* Can't find removed child. */
    uint64_t ref;
    TEST_EQ(kb_child_find(pool, &parent, bns[25], &ref), KB_E_NOT_FOUND);

    /* Others still findable. */
    TEST_OK(kb_child_find(pool, &parent, bns[0], &ref));
    TEST_EQ(ref, 1000);
    TEST_OK(kb_child_find(pool, &parent, bns[49], &ref));
    TEST_EQ(ref, 1049);

    teardown(pool, &sa);
}

/* ════════════════ Iteration callback ════════════════ */

static kb_status_t count_callback(const kb_child_entry_t *e, void *ctx) {
    (void)e;
    uint32_t *count = (uint32_t *)ctx;
    (*count)++;
    return KB_OK;
}

static void test_child_iterate(void) {
    TEST_BEGIN("child_iterate");
    cleanup();

    kb_pool_t *pool;
    kb_slab_alloc_t sa;
    kb_header_t parent;
    TEST_OK(setup(&pool, &sa, &parent));

    /* Add 10 children. */
    for (int i = 0; i < 10; i++) {
        TEST_OK(kb_child_add(&sa, pool, &parent, (uint64_t)(i + 500), NULL));
    }

    /* Remove 2. */
    TEST_OK(kb_child_remove(pool, &parent, 3)); /* birth_num 3 */
    TEST_OK(kb_child_remove(pool, &parent, 7)); /* birth_num 7 */

    /* Iterate should visit 8 live children. */
    uint32_t count = 0;
    TEST_OK(kb_child_iterate(pool, &parent, count_callback, &count));
    TEST_EQ(count, 8);

    teardown(pool, &sa);
}

/* ════════════════ Remove nonexistent ════════════════ */

static void test_child_remove_missing(void) {
    TEST_BEGIN("child_remove_missing");
    cleanup();

    kb_pool_t *pool;
    kb_slab_alloc_t sa;
    kb_header_t parent;
    TEST_OK(setup(&pool, &sa, &parent));

    /* No children — remove should fail. */
    TEST_EQ(kb_child_remove(pool, &parent, 0), KB_E_NOT_FOUND);

    /* Add one, try to remove wrong birth_num. */
    TEST_OK(kb_child_add(&sa, pool, &parent, 100, NULL));
    TEST_EQ(kb_child_remove(pool, &parent, 999), KB_E_NOT_FOUND);

    teardown(pool, &sa);
}

/* ════════════════ main ════════════════ */

int main(void) {
    printf("=== KB Child Table Tests ===\n\n");

    kb_crc32c_init();

    test_child_inline();
    test_child_promote();
    test_child_many();
    test_child_iterate();
    test_child_remove_missing();

    TEST_SUMMARY();
}
