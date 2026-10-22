/*
 * test_store.c — Unified store API tests (§7).
 */

#include "test_main.h"
#include "core/kb_store.h"
#include "core/kb_crc32c.h"

static const char *TEST_PATH = "/tmp/kb_test_store.db";

static void cleanup(void) {
    (void)kb_os_delete(TEST_PATH);
    (void)kb_os_delete("/tmp/kb_test_store.wal");
}

/* ════════════════ Create / Close ════════════════ */

static void test_store_lifecycle(void) {
    TEST_BEGIN("store_lifecycle");
    cleanup();

    kb_store_t store;
    kb_store_config_t cfg = { .buf_pool_slots = 32 };
    TEST_OK(kb_store_create(TEST_PATH, &cfg, &store));
    TEST_ASSERT(store.is_open, "store is open");

    kb_store_stats_t stats;
    TEST_OK(kb_store_get_stats(&store, &stats));
    TEST_ASSERT(stats.total_pages > 0, "has pages");
    TEST_EQ(stats.index_entries, 0);

    TEST_OK(kb_store_close(&store));
    TEST_ASSERT(!store.is_open, "store closed");

    cleanup();
}

/* ════════════════ Create root + read back ════════════════ */

static void test_store_root(void) {
    TEST_BEGIN("store_root");
    cleanup();

    kb_store_t store;
    kb_store_config_t cfg = { .buf_pool_slots = 32 };
    TEST_OK(kb_store_create(TEST_PATH, &cfg, &store));

    kb_addr_t root_addr;
    TEST_OK(kb_store_create_root(&store, 1, "Root", &root_addr));
    TEST_ASSERT(root_addr != 0, "root addr valid");

    /* Read back. */
    kb_header_t hdr;
    TEST_OK(kb_store_read_header(&store, root_addr, &hdr));
    TEST_EQ(hdr.type_id, 1);

    /* Lookup by root path. */
    uint8_t root_path[1] = {0x00};
    kb_addr_t found;
    TEST_OK(kb_store_lookup(&store, root_path, 1, &found));
    TEST_EQ(found, root_addr);

    TEST_OK(kb_store_close(&store));
    cleanup();
}

/* ════════════════ Create children ════════════════ */

static void test_store_children(void) {
    TEST_BEGIN("store_children");
    cleanup();

    kb_store_t store;
    kb_store_config_t cfg = { .buf_pool_slots = 32 };
    TEST_OK(kb_store_create(TEST_PATH, &cfg, &store));

    kb_addr_t root_addr;
    TEST_OK(kb_store_create_root(&store, 1, "Root", &root_addr));

    /* Create 5 children. */
    kb_addr_t child_addrs[5];
    uint32_t births[5];
    char names[][12] = {"alpha", "beta", "gamma", "delta", "epsilon"};

    for (int i = 0; i < 5; i++) {
        TEST_OK(kb_store_create_child(&store, root_addr, 2, names[i],
                                      &child_addrs[i], &births[i]));
        TEST_ASSERT(child_addrs[i] != 0, "child addr valid");
        TEST_ASSERT(births[i] > 0, "birth assigned");
    }

    /* Read each child. */
    for (int i = 0; i < 5; i++) {
        kb_header_t hdr;
        TEST_OK(kb_store_read_header(&store, child_addrs[i], &hdr));
        TEST_EQ(hdr.type_id, 2);
    }

    /* List children of root. */
    uint32_t child_count = 0;
    kb_store_list_children(&store, root_addr,
        (kb_store_child_fn)(void (*)(void))NULL, NULL);
    /* Simple count — just verify we got the header updated. */
    kb_header_t root_hdr;
    TEST_OK(kb_store_read_header(&store, root_addr, &root_hdr));
    TEST_EQ(root_hdr.child_count, 5);
    (void)child_count;

    /* Stats. */
    kb_store_stats_t stats;
    TEST_OK(kb_store_get_stats(&store, &stats));
    TEST_ASSERT(stats.index_entries >= 6, "index has entries");

    TEST_OK(kb_store_close(&store));
    cleanup();
}

/* ════════════════ Data I/O ════════════════ */

static void test_store_data(void) {
    TEST_BEGIN("store_data");
    cleanup();

    kb_store_t store;
    kb_store_config_t cfg = { .buf_pool_slots = 32 };
    TEST_OK(kb_store_create(TEST_PATH, &cfg, &store));

    kb_addr_t root_addr;
    TEST_OK(kb_store_create_root(&store, 1, "DataObj", &root_addr));

    /* Write some data. */
    const char *payload = "Hello, Knowledge Base!";
    uint32_t plen = (uint32_t)strlen(payload);
    TEST_OK(kb_store_write_data(&store, root_addr, 0, payload, plen));

    /* Read it back. */
    char readback[64];
    memset(readback, 0, sizeof(readback));
    TEST_OK(kb_store_read_data(&store, root_addr, 0, readback, plen));
    TEST_ASSERT(memcmp(readback, payload, plen) == 0, "data matches");

    TEST_OK(kb_store_close(&store));
    cleanup();
}

/* ════════════════ Delete (tombstone) ════════════════ */

static void test_store_delete(void) {
    TEST_BEGIN("store_delete");
    cleanup();

    kb_store_t store;
    kb_store_config_t cfg = { .buf_pool_slots = 32 };
    TEST_OK(kb_store_create(TEST_PATH, &cfg, &store));

    kb_addr_t addr;
    TEST_OK(kb_store_create_root(&store, 1, "ToDelete", &addr));

    /* Delete it. */
    TEST_OK(kb_store_delete(&store, addr));

    /* Read back — should be tombstoned. */
    kb_header_t hdr;
    TEST_OK(kb_store_read_header(&store, addr, &hdr));
    TEST_ASSERT(hdr.state_rev == KB_STATE_DETACHED, "is detached");

    TEST_OK(kb_store_close(&store));
    cleanup();
}

/* ════════════════ Transactions ════════════════ */

static void test_store_transactions(void) {
    TEST_BEGIN("store_transactions");
    cleanup();

    kb_store_t store;
    kb_store_config_t cfg = { .buf_pool_slots = 32 };
    TEST_OK(kb_store_create(TEST_PATH, &cfg, &store));

    TEST_OK(kb_store_begin_tx(&store));
    TEST_OK(kb_store_commit_tx(&store));

    /* Begin and abort. */
    TEST_OK(kb_store_begin_tx(&store));
    TEST_OK(kb_store_abort_tx(&store));

    /* Double begin should fail. */
    TEST_OK(kb_store_begin_tx(&store));
    TEST_EQ(kb_store_begin_tx(&store), KB_E_BUSY);
    TEST_OK(kb_store_abort_tx(&store));

    TEST_OK(kb_store_close(&store));
    cleanup();
}

/* ════════════════ Reopen ════════════════ */

static void test_store_reopen(void) {
    TEST_BEGIN("store_reopen");
    cleanup();

    kb_addr_t root_addr;

    /* Create, add data, close. */
    {
        kb_store_t store;
        kb_store_config_t cfg = { .buf_pool_slots = 32 };
        TEST_OK(kb_store_create(TEST_PATH, &cfg, &store));

        TEST_OK(kb_store_create_root(&store, 42, "Persist", &root_addr));

        const char *data = "persistent data";
        TEST_OK(kb_store_write_data(&store, root_addr, 0,
                                    data, (uint32_t)strlen(data)));

        TEST_OK(kb_store_close(&store));
    }

    /* Reopen and verify. */
    {
        kb_store_t store;
        kb_store_config_t cfg = { .buf_pool_slots = 32 };
        TEST_OK(kb_store_open(TEST_PATH, &cfg, &store));

        kb_header_t hdr;
        TEST_OK(kb_store_read_header(&store, root_addr, &hdr));
        TEST_EQ(hdr.type_id, 42);

        char readback[32];
        memset(readback, 0, sizeof(readback));
        TEST_OK(kb_store_read_data(&store, root_addr, 0, readback, 15));
        TEST_ASSERT(memcmp(readback, "persistent data", 15) == 0,
                    "data persisted");

        TEST_OK(kb_store_close(&store));
    }

    cleanup();
}

/* ════════════════ Scan all ════════════════ */

static kb_status_t
scan_count_cb(const uint8_t *key, uint16_t key_len,
              kb_addr_t value, void *ctx)
{
    (void)key; (void)key_len; (void)value;
    uint32_t *cnt = (uint32_t *)ctx;
    (*cnt)++;
    return KB_OK;
}

static void test_store_scan(void) {
    TEST_BEGIN("store_scan");
    cleanup();

    kb_store_t store;
    kb_store_config_t cfg = { .buf_pool_slots = 32 };
    TEST_OK(kb_store_create(TEST_PATH, &cfg, &store));

    /* Create root + 10 children. */
    kb_addr_t root;
    TEST_OK(kb_store_create_root(&store, 1, "R", &root));

    for (int i = 0; i < 10; i++) {
        kb_addr_t ca;
        uint32_t b;
        TEST_OK(kb_store_create_child(&store, root, 2, "c",
                                      &ca, &b));
    }

    /* Scan all — should be 11 (root + 10 children). */
    uint32_t count = 0;
    TEST_OK(kb_store_scan_all(&store, scan_count_cb, &count));
    TEST_EQ(count, 11);

    TEST_OK(kb_store_close(&store));
    cleanup();
}

/* ════════════════ main ════════════════ */

int main(void) {
    printf("=== KB Store Tests ===\n\n");

    kb_crc32c_init();

    test_store_lifecycle();
    test_store_root();
    test_store_children();
    test_store_data();
    test_store_delete();
    test_store_transactions();
    test_store_reopen();
    test_store_scan();

    TEST_SUMMARY();
}
