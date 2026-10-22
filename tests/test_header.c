/*
 * test_header.c — ObjectHeader tests (§2.6).
 *
 * Uses the canonical layout from kb_types.h.
 */

#include "test_main.h"
#include "core/kb_header.h"
#include "core/kb_pool.h"
#include "core/kb_crc32c.h"

static const char *TEST_POOL_PATH = "/tmp/kb_test_header.db";

static void cleanup(void) {
    (void)kb_os_delete(TEST_POOL_PATH);
}

/* ════════════════ Round-trip serialize/deserialize ════════════════ */

static void test_hdr_roundtrip(void) {
    TEST_BEGIN("hdr_roundtrip");

    kb_crc32c_init();

    kb_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.state_rev      = KB_STATE_LIVE;
    hdr.type_version   = 3;
    hdr.type_id        = 42;
    hdr.flags          = KB_FLAG_PATH_FROZEN | KB_FLAG_HAS_AUX;
    hdr.parent_ref     = 12345;
    hdr.children_ref   = 67890;
    hdr.ext0_ref       = 100;
    hdr.ext0_cap_units = 256;
    hdr.aux_ref        = 200;
    hdr.aux_len        = 1024;
    hdr.birth_num      = 7;
    hdr.next_child_ctr = 10;
    hdr.child_count    = 3;
    hdr.change_seq     = 55;
    hdr.slot_gen       = 1;
    hdr.payload_len    = 0x123456789ULL; /* 48-bit value */

    /* Serialize. */
    uint8_t buf[KB_HDR_SIZE];
    TEST_OK(kb_hdr_serialize(&hdr, buf));

    /* Deserialize. */
    kb_header_t hdr2;
    TEST_OK(kb_hdr_deserialize(buf, &hdr2));

    TEST_EQ(hdr2.state_rev,      KB_STATE_LIVE);
    TEST_EQ(hdr2.type_version,   3);
    TEST_EQ(hdr2.type_id,        42);
    TEST_EQ_HEX(hdr2.flags,     KB_FLAG_PATH_FROZEN | KB_FLAG_HAS_AUX);
    TEST_EQ(hdr2.parent_ref,     12345);
    TEST_EQ(hdr2.children_ref,   67890);
    TEST_EQ(hdr2.ext0_ref,       100);
    TEST_EQ(hdr2.ext0_cap_units, 256);
    TEST_EQ(hdr2.aux_ref,        200);
    TEST_EQ(hdr2.aux_len,        1024);
    TEST_EQ(hdr2.birth_num,      7);
    TEST_EQ(hdr2.next_child_ctr, 10);
    TEST_EQ(hdr2.child_count,    3);
    TEST_EQ(hdr2.change_seq,     55);
    TEST_EQ(hdr2.slot_gen,       1);
    TEST_EQ(hdr2.payload_len,    0x123456789ULL);
}

/* ════════════════ CRC corruption detection ════════════════ */

static void test_hdr_corrupt(void) {
    TEST_BEGIN("hdr_corrupt");

    kb_crc32c_init();

    kb_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.state_rev = KB_STATE_LIVE;
    hdr.type_id = 1;
    hdr.slot_gen = 1;

    uint8_t buf[KB_HDR_SIZE];
    TEST_OK(kb_hdr_serialize(&hdr, buf));

    /* Flip a bit → CRC should fail. */
    buf[20] ^= 0x01;
    kb_header_t bad;
    TEST_EQ(kb_hdr_deserialize(buf, &bad), KB_E_CORRUPT);
}

/* ════════════════ Create and read via slab ════════════════ */

static void test_hdr_create_read(void) {
    TEST_BEGIN("hdr_create_read");
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

    /* Create a header. */
    kb_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.type_id       = 7;
    hdr.parent_ref    = 999;
    hdr.child_count   = 0;
    hdr.ext0_ref      = 42;
    hdr.ext0_cap_units= 64;
    hdr.payload_len   = 8192;

    TEST_OK(kb_hdr_create(&sa, &hdr));
    TEST_ASSERT(hdr.slot_addr != KB_NULL_ADDR, "slot allocated");
    TEST_EQ(hdr.slot_gen, 1);
    TEST_EQ(hdr.state_rev, KB_STATE_LIVE);

    /* Read it back. */
    kb_header_t hdr2;
    TEST_OK(kb_hdr_read(pool, hdr.slot_addr, 1, &hdr2));
    TEST_EQ(hdr2.state_rev,      KB_STATE_LIVE);
    TEST_EQ(hdr2.type_id,        7);
    TEST_EQ(hdr2.parent_ref,     999);
    TEST_EQ(hdr2.ext0_ref,       42);
    TEST_EQ(hdr2.ext0_cap_units, 64);
    TEST_EQ(hdr2.payload_len,    8192);

    kb_slab_destroy(&sa);
    TEST_OK(kb_pool_close(pool));
    cleanup();
}

/* ════════════════ ABA slot_gen guard (§2.6.3) ════════════════ */

static void test_hdr_aba_guard(void) {
    TEST_BEGIN("hdr_aba_guard");
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

    /* Create a header. */
    kb_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.type_id = 1;
    TEST_OK(kb_hdr_create(&sa, &hdr));

    kb_addr_t addr = hdr.slot_addr;
    TEST_EQ(hdr.slot_gen, 1);

    /* Detach it (gen increments). */
    TEST_OK(kb_hdr_detach(pool, &hdr));
    TEST_EQ(hdr.slot_gen, 2);

    /* Reading with old gen should fail with KB_E_STALE_REF. */
    kb_header_t stale;
    TEST_EQ(kb_hdr_read(pool, addr, 1, &stale), KB_E_STALE_REF);

    /* Reading with current gen succeeds. */
    kb_header_t ok;
    TEST_OK(kb_hdr_read(pool, addr, 2, &ok));
    TEST_EQ(ok.state_rev, KB_STATE_DETACHED);

    /* Reading with gen=0 skips the check. */
    kb_header_t any;
    TEST_OK(kb_hdr_read(pool, addr, 0, &any));

    kb_slab_destroy(&sa);
    TEST_OK(kb_pool_close(pool));
    cleanup();
}

/* ════════════════ Detach clears fields ════════════════ */

static void test_hdr_detach_clears(void) {
    TEST_BEGIN("hdr_detach_clears");
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

    kb_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.type_id       = 99;
    hdr.parent_ref    = 555;
    hdr.child_count   = 10;
    hdr.ext0_ref      = 100;
    hdr.ext0_cap_units= 50;
    hdr.payload_len   = 50000;
    TEST_OK(kb_hdr_create(&sa, &hdr));

    TEST_OK(kb_hdr_detach(pool, &hdr));

    /* Read back — content fields should be cleared. */
    kb_header_t hdr2;
    TEST_OK(kb_hdr_read(pool, hdr.slot_addr, 0, &hdr2));
    TEST_EQ(hdr2.state_rev,      KB_STATE_DETACHED);
    TEST_EQ(hdr2.parent_ref,     0);
    TEST_EQ(hdr2.children_ref,   0);
    TEST_EQ(hdr2.child_count,    0);
    TEST_EQ(hdr2.ext0_ref,       0);
    TEST_EQ(hdr2.payload_len,    0);
    /* type_id preserved so the tombstone knows the module. */
    TEST_EQ(hdr2.type_id,        99);

    kb_slab_destroy(&sa);
    TEST_OK(kb_pool_close(pool));
    cleanup();
}

/* ════════════════ Multiple headers in same pool ════════════════ */

static void test_hdr_multiple(void) {
    TEST_BEGIN("hdr_multiple");
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

    /* Create 100 headers. */
    kb_addr_t addrs[100];
    for (int i = 0; i < 100; i++) {
        kb_header_t h;
        memset(&h, 0, sizeof(h));
        h.type_id    = (uint16_t)i;
        h.birth_num  = (uint32_t)i;
        h.payload_len = (uint64_t)i * 100;
        TEST_OK(kb_hdr_create(&sa, &h));
        addrs[i] = h.slot_addr;
    }

    /* All addresses are unique. */
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT(addrs[i] != KB_NULL_ADDR, "addr valid");
        for (int j = i + 1; j < 100; j++) {
            TEST_ASSERT(addrs[i] != addrs[j], "addrs unique");
        }
    }

    /* Read back a sample. */
    kb_header_t h50;
    TEST_OK(kb_hdr_read(pool, addrs[50], 0, &h50));
    TEST_EQ(h50.type_id,    50);
    TEST_EQ(h50.birth_num,  50);
    TEST_EQ(h50.payload_len, 5000);

    kb_slab_destroy(&sa);
    TEST_OK(kb_pool_close(pool));
    cleanup();
}

/* ════════════════ 48-bit payload_len boundary ════════════════ */

static void test_hdr_payload_len_48bit(void) {
    TEST_BEGIN("hdr_payload_len_48bit");

    kb_crc32c_init();

    kb_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.state_rev = KB_STATE_LIVE;
    hdr.slot_gen  = 1;

    /* Max 48-bit value: 2^48 - 1 = 281,474,976,710,655. */
    hdr.payload_len = 0xFFFFFFFFFFFFULL;

    uint8_t buf[KB_HDR_SIZE];
    TEST_OK(kb_hdr_serialize(&hdr, buf));

    kb_header_t hdr2;
    TEST_OK(kb_hdr_deserialize(buf, &hdr2));
    TEST_EQ(hdr2.payload_len, 0xFFFFFFFFFFFFULL);

    /* Zero. */
    hdr.payload_len = 0;
    TEST_OK(kb_hdr_serialize(&hdr, buf));
    TEST_OK(kb_hdr_deserialize(buf, &hdr2));
    TEST_EQ(hdr2.payload_len, 0);

    /* Midpoint. */
    hdr.payload_len = 0x800000000000ULL >> 1;
    TEST_OK(kb_hdr_serialize(&hdr, buf));
    TEST_OK(kb_hdr_deserialize(buf, &hdr2));
    TEST_EQ(hdr2.payload_len, 0x400000000000ULL);
}

/* ════════════════ Write-back increments change_seq ════════════════ */

static void test_hdr_write_change_seq(void) {
    TEST_BEGIN("hdr_write_change_seq");
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

    kb_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.type_id = 1;
    TEST_OK(kb_hdr_create(&sa, &hdr));
    uint32_t seq0 = hdr.change_seq;

    /* Modify and write back. */
    hdr.child_count = 5;
    hdr.dirty = true;
    TEST_OK(kb_hdr_write(pool, &hdr));
    TEST_EQ(hdr.change_seq, seq0 + 1);

    /* Read back. */
    kb_header_t hdr2;
    TEST_OK(kb_hdr_read(pool, hdr.slot_addr, 0, &hdr2));
    TEST_EQ(hdr2.change_seq, seq0 + 1);
    TEST_EQ(hdr2.child_count, 5);

    kb_slab_destroy(&sa);
    TEST_OK(kb_pool_close(pool));
    cleanup();
}

/* ════════════════ main ════════════════ */

int main(void) {
    printf("=== KB Header Tests ===\n\n");

    kb_crc32c_init();

    test_hdr_roundtrip();
    test_hdr_corrupt();
    test_hdr_create_read();
    test_hdr_aba_guard();
    test_hdr_detach_clears();
    test_hdr_multiple();
    test_hdr_payload_len_48bit();
    test_hdr_write_change_seq();

    TEST_SUMMARY();
}
