/*
 * test_superblock.c — Superblock serialization and dual-copy tests (§2.2).
 */

#include "test_main.h"
#include "core/kb_superblock.h"
#include "core/kb_os.h"

/* ════════════════ Serialize / deserialize round-trip ════════════════ */

static void test_sb_roundtrip(void) {
    TEST_BEGIN("sb_roundtrip");

    kb_crc32c_init();

    uint8_t uuid[16] = {0xDE,0xAD,0xBE,0xEF, 0x01,0x02,0x03,0x04,
                        0x05,0x06,0x07,0x08, 0x09,0x0A,0x0B,0x0C};

    kb_superblock_t sb;
    kb_sb_init_new(&sb, uuid, KB_PAGE_SIZE_DEFAULT);
    sb.root_ref          = 12345;
    sb.provisioned_bytes = 67108864;  /* 64 MiB */
    sb.segment_count     = 1;
    sb.hwm_unit          = 147 * 64;  /* past metadata */
    sb.object_count      = 42;
    sb.wal_checkpoint_lsn= 0;
    sb.feature_flags     = 0;
    sb.grow_step_mb      = 64;
    sb.seq               = 7;

    /* Serialize. */
    uint8_t page[KB_PAGE_SIZE_DEFAULT];
    TEST_OK(kb_sb_serialize(&sb, page));

    /* Verify magic at offset 0. */
    TEST_EQ_HEX(kb_read_u32(page + KB_SB_OFF_MAGIC), KB_MAGIC_POOL);

    /* Deserialize. */
    kb_superblock_t sb2;
    TEST_OK(kb_sb_deserialize(page, &sb2));

    /* Verify all fields round-trip. */
    TEST_EQ_HEX(sb2.magic,             KB_MAGIC_POOL);
    TEST_EQ(sb2.format_rev,            KB_FORMAT_REV);
    TEST_EQ(sb2.page_size,             KB_PAGE_SIZE_DEFAULT);
    TEST_EQ(sb2.unit_size,             KB_UNIT_SIZE);
    TEST_EQ(sb2.uref_bits,             KB_UREF_BITS);
    TEST_EQ(sb2.segment_size_mb,       KB_SEGMENT_SIZE >> 20);
    TEST_ASSERT(memcmp(sb2.pool_uuid, uuid, 16) == 0, "uuid matches");
    TEST_EQ(sb2.seq,                   7);
    TEST_EQ(sb2.clean_shutdown,        0);
    TEST_EQ(sb2.root_ref,             12345);
    TEST_EQ(sb2.provisioned_bytes,    67108864);
    TEST_EQ(sb2.segment_count,        1);
    TEST_EQ(sb2.hwm_unit,            (uint64_t)(147 * 64));
    TEST_EQ(sb2.object_count,        42);
    TEST_EQ(sb2.grow_step_mb,        64);
}

/* ════════════════ CRC corruption detection ════════════════ */

static void test_sb_corrupt_crc(void) {
    TEST_BEGIN("sb_corrupt_crc");

    kb_crc32c_init();

    uint8_t uuid[16] = {0};
    kb_superblock_t sb;
    kb_sb_init_new(&sb, uuid, KB_PAGE_SIZE_DEFAULT);
    sb.seq = 1;

    uint8_t page[KB_PAGE_SIZE_DEFAULT];
    TEST_OK(kb_sb_serialize(&sb, page));

    /* Flip one bit in the payload → CRC should fail. */
    page[20] ^= 0x01;
    kb_superblock_t sb2;
    TEST_EQ(kb_sb_deserialize(page, &sb2), KB_E_CORRUPT);
}

/* ════════════════ Bad magic detection ════════════════ */

static void test_sb_bad_magic(void) {
    TEST_BEGIN("sb_bad_magic");

    kb_crc32c_init();

    uint8_t page[KB_PAGE_SIZE_DEFAULT];
    memset(page, 0, sizeof(page));
    /* Write a wrong magic. */
    kb_write_u32(page, 0xDEADDEAD);

    kb_superblock_t sb;
    TEST_EQ(kb_sb_deserialize(page, &sb), KB_E_CORRUPT);
}

/* ════════════════ Dual-copy: pick higher seq ════════════════ */

static void test_sb_dual_copy(void) {
    TEST_BEGIN("sb_dual_copy");

    kb_crc32c_init();

    /* Create a temporary pool file with two superblock copies. */
    const char *path = "/tmp/kb_test_sb_dual.dat";
    kb_fd_t fd;

    /* Remove any old test file. */
    (void)kb_os_delete(path);

    kb_status_t s = kb_os_open(path, KB_OPEN_RDWR | KB_OPEN_CREATE | KB_OPEN_EXCL,
                               0644, &fd);
    TEST_OK(s);

    /* Preallocate 2 pages. */
    TEST_OK(kb_os_preallocate(fd, KB_PAGE_SIZE_DEFAULT * 2));

    /* Write copy 0 with seq=3. */
    uint8_t uuid[16] = {0x11};
    kb_superblock_t sb0;
    kb_sb_init_new(&sb0, uuid, KB_PAGE_SIZE_DEFAULT);
    sb0.seq = 3;
    sb0.root_ref = 100;

    uint8_t page[KB_PAGE_SIZE_DEFAULT];
    TEST_OK(kb_sb_serialize(&sb0, page));
    TEST_OK(kb_os_pwrite(fd, page, KB_PAGE_SIZE_DEFAULT, 0));

    /* Write copy 1 with seq=5 (higher). */
    kb_superblock_t sb1;
    kb_sb_init_new(&sb1, uuid, KB_PAGE_SIZE_DEFAULT);
    sb1.seq = 5;
    sb1.root_ref = 200;
    memset(page, 0, sizeof(page));
    TEST_OK(kb_sb_serialize(&sb1, page));
    TEST_OK(kb_os_pwrite(fd, page, KB_PAGE_SIZE_DEFAULT, KB_PAGE_SIZE_DEFAULT));

    /* Load best → should pick seq=5, root_ref=200. */
    kb_superblock_t best;
    TEST_OK(kb_sb_load_best(fd, KB_PAGE_SIZE_DEFAULT, &best));
    TEST_EQ(best.seq,      5);
    TEST_EQ(best.root_ref, 200);

    /* Now corrupt copy 1 → should fall back to copy 0. */
    uint8_t garbage[KB_PAGE_SIZE_DEFAULT];
    memset(garbage, 0xFF, sizeof(garbage));
    TEST_OK(kb_os_pwrite(fd, garbage, KB_PAGE_SIZE_DEFAULT, KB_PAGE_SIZE_DEFAULT));

    TEST_OK(kb_sb_load_best(fd, KB_PAGE_SIZE_DEFAULT, &best));
    TEST_EQ(best.seq,      3);
    TEST_EQ(best.root_ref, 100);

    kb_os_close(fd);
    kb_os_delete(path);
}

/* ════════════════ main ════════════════ */

int main(void) {
    printf("=== KB Superblock Tests ===\n\n");

    test_sb_roundtrip();
    test_sb_corrupt_crc();
    test_sb_bad_magic();
    test_sb_dual_copy();

    TEST_SUMMARY();
}
