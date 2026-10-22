/*
 * test_crc32c.c — CRC32C tests.
 *
 * Validates the software CRC32C implementation against known test vectors.
 * The canonical vector is CRC32C("123456789") = 0xE3069283.
 */

#include "test_main.h"
#include "core/kb_crc32c.h"

/* ════════════════ Known vectors ════════════════ */

static void test_crc32c_known_vectors(void) {
    TEST_BEGIN("crc32c_known_vectors");

    kb_crc32c_init();

    /* Empty input → 0 */
    TEST_EQ_HEX(kb_crc32c(0, "", 0), 0x00000000);

    /* The canonical CRC32C test vector.
     * CRC32C("123456789") = 0xE3069283. */
    const char *test_str = "123456789";
    uint32_t crc = kb_crc32c(0, test_str, 9);
    TEST_EQ_HEX(crc, 0xE3069283);

    /* Single zero byte. */
    uint8_t zero = 0;
    TEST_EQ_HEX(kb_crc32c(0, &zero, 1), 0x527D5351);

    /* Single 0xFF byte. */
    uint8_t ff = 0xFF;
    TEST_EQ_HEX(kb_crc32c(0, &ff, 1), 0xFF000000);
}

/* ════════════════ Chaining ════════════════ */

static void test_crc32c_chaining(void) {
    TEST_BEGIN("crc32c_chaining");

    kb_crc32c_init();

    /* CRC32C computed in two halves must equal the single-pass result. */
    const char *full = "123456789";
    uint32_t full_crc = kb_crc32c(0, full, 9);

    uint32_t c1 = kb_crc32c(0, full, 5);
    uint32_t c2 = kb_crc32c(c1, full + 5, 4);
    TEST_EQ_HEX(c2, full_crc);

    /* Byte-at-a-time chaining. */
    uint32_t byte_crc = 0;
    for (int i = 0; i < 9; i++) {
        byte_crc = kb_crc32c(byte_crc, full + i, 1);
    }
    TEST_EQ_HEX(byte_crc, full_crc);
}

/* ════════════════ Header CRC pattern (§2.6) ════════════════ */

static void test_crc32c_header_pattern(void) {
    TEST_BEGIN("crc32c_header_pattern");

    kb_crc32c_init();

    /*
     * Simulate the header CRC computation (§2.6):
     * Zero the 4-byte crc32c field at offset 8, compute over all 64 bytes,
     * then write the result back. Re-compute to verify.
     */
    uint8_t header[64];
    memset(header, 0xAB, sizeof(header));  /* fill with pattern */

    /* Zero the CRC field (offset 8, 4 bytes). */
    header[8] = header[9] = header[10] = header[11] = 0;

    /* Compute CRC over the full 64 bytes. */
    uint32_t crc = kb_crc32c_buf(header, 64);

    /* Write it back at offset 8. */
    header[8]  = (uint8_t)(crc);
    header[9]  = (uint8_t)(crc >> 8);
    header[10] = (uint8_t)(crc >> 16);
    header[11] = (uint8_t)(crc >> 24);

    /* Verify: zero the CRC field again and recompute. */
    uint8_t verify[64];
    memcpy(verify, header, 64);
    verify[8] = verify[9] = verify[10] = verify[11] = 0;
    uint32_t recomputed = kb_crc32c_buf(verify, 64);
    TEST_EQ_HEX(recomputed, crc);
}

/* ════════════════ Idempotent init ════════════════ */

static void test_crc32c_double_init(void) {
    TEST_BEGIN("crc32c_double_init");

    /* Calling init twice should not break anything. */
    kb_crc32c_init();
    kb_crc32c_init();

    const char *s = "123456789";
    TEST_EQ_HEX(kb_crc32c(0, s, 9), 0xE3069283);
}

/* ════════════════ main ════════════════ */

int main(void) {
    printf("=== KB CRC32C Tests ===\n\n");

    test_crc32c_known_vectors();
    test_crc32c_chaining();
    test_crc32c_header_pattern();
    test_crc32c_double_init();

    TEST_SUMMARY();
}
