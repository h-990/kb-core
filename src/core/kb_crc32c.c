/*
 * kb_crc32c.c — CRC32C (Castagnoli) software implementation.
 *
 * Polynomial: 0x1EDC6F41 (bit-reversed: 0x82F63B78).
 * Known test vector: CRC32C("123456789") = 0xE3069283.
 *
 * Phase 1: software table-based (byte-at-a-time).
 * Phase 7: hardware acceleration via SSE4.2 / ARMv8 CRC intrinsics.
 */

#include "core/kb_crc32c.h"

#define CRC32C_POLY 0x82F63B78u  /* bit-reversed Castagnoli polynomial */

static uint32_t crc32c_table[256];
static int crc32c_ready = 0;

void
kb_crc32c_init(void)
{
    if (crc32c_ready) return;

    for (unsigned i = 0; i < 256; i++) {
        uint32_t crc = (uint32_t)i;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ CRC32C_POLY) : (crc >> 1);
        }
        crc32c_table[i] = crc;
    }
    crc32c_ready = 1;
}

uint32_t
kb_crc32c(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc = crc32c_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}
