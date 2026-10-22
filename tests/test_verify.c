/*
 * test_verify.c — Verify/repair tests (§6).
 */

#include "test_main.h"
#include "core/kb_verify.h"
#include "core/kb_store.h"
#include "core/kb_crc32c.h"

static const char *TEST_PATH = "/tmp/kb_test_verify.db";

static void cleanup(void) {
    (void)kb_os_delete(TEST_PATH);
    (void)kb_os_delete("/tmp/kb_test_verify.wal");
}

/* Issue collector. */
typedef struct issue_log {
    uint32_t count;
    kb_issue_level_t max_level;
} issue_log_t;

static void
issue_cb(const kb_verify_issue_t *issue, void *ctx)
{
    issue_log_t *log = (issue_log_t *)ctx;
    log->count++;
    if (issue->level > log->max_level) {
        log->max_level = issue->level;
    }
}

/* ════════════════ Clean pool ════════════════ */

static void test_verify_clean(void) {
    TEST_BEGIN("verify_clean_pool");
    cleanup();

    kb_store_t store;
    kb_store_config_t cfg = { .buf_pool_slots = 32 };
    TEST_OK(kb_store_create(TEST_PATH, &cfg, &store));

    /* Create some objects. */
    kb_addr_t root;
    TEST_OK(kb_store_create_root(&store, 1, "Root", &root));
    for (int i = 0; i < 5; i++) {
        kb_addr_t ca;
        uint32_t b;
        TEST_OK(kb_store_create_child(&store, root, 2, "c", &ca, &b));
    }

    /* Verify — should have no errors. */
    issue_log_t log = { 0, KB_ISSUE_INFO };
    kb_verify_result_t result;
    TEST_OK(kb_verify(store.pool, &store.index, NULL, issue_cb, &log, &result));

    TEST_ASSERT(result.fatal_count == 0, "no fatal issues");
    TEST_ASSERT(result.error_count == 0, "no error issues");
    TEST_ASSERT(result.index_entries_checked >= 6, "index entries checked");

    TEST_OK(kb_store_close(&store));
    cleanup();
}

/* ════════════════ Verbose mode ════════════════ */

static void test_verify_verbose(void) {
    TEST_BEGIN("verify_verbose");
    cleanup();

    kb_store_t store;
    kb_store_config_t cfg = { .buf_pool_slots = 32 };
    TEST_OK(kb_store_create(TEST_PATH, &cfg, &store));

    kb_addr_t root;
    TEST_OK(kb_store_create_root(&store, 1, "V", &root));

    kb_verify_opts_t opts = { .verbose = true };
    issue_log_t log = { 0, KB_ISSUE_INFO };
    kb_verify_result_t result;
    TEST_OK(kb_verify(store.pool, &store.index, &opts, issue_cb, &log, &result));

    /* Verbose mode reports INFO items. */
    TEST_ASSERT(result.info_count > 0, "info items reported");
    TEST_ASSERT(result.fatal_count == 0, "no fatal");

    TEST_OK(kb_store_close(&store));
    cleanup();
}

/* ════════════════ After delete ════════════════ */

static void test_verify_after_delete(void) {
    TEST_BEGIN("verify_after_delete");
    cleanup();

    kb_store_t store;
    kb_store_config_t cfg = { .buf_pool_slots = 32 };
    TEST_OK(kb_store_create(TEST_PATH, &cfg, &store));

    kb_addr_t root;
    TEST_OK(kb_store_create_root(&store, 1, "D", &root));
    kb_addr_t child;
    uint32_t birth;
    TEST_OK(kb_store_create_child(&store, root, 2, "c", &child, &birth));

    /* Delete the child. */
    TEST_OK(kb_store_delete(&store, child));

    /* Verify — detached headers are not errors. */
    issue_log_t log = { 0, KB_ISSUE_INFO };
    kb_verify_result_t result;
    TEST_OK(kb_verify(store.pool, &store.index, NULL, issue_cb, &log, &result));
    TEST_ASSERT(result.fatal_count == 0, "no fatal");

    TEST_OK(kb_store_close(&store));
    cleanup();
}

/* ════════════════ main ════════════════ */

int main(void) {
    printf("=== KB Verify Tests ===\n\n");

    kb_crc32c_init();

    test_verify_clean();
    test_verify_verbose();
    test_verify_after_delete();

    TEST_SUMMARY();
}
