/*
 * kb_crc32c.h — CRC32C (Castagnoli) checksums.
 *
 * Spec reference: §1.1 — CRC32C protects all metadata. Hardware instructions
 * are available on x86-64 (SSE4.2) and ARMv8. Phase 1 uses software tables;
 * hardware acceleration will be added in Phase 7 benchmarking.
 */
#ifndef KB_CRC32C_H
#define KB_CRC32C_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize the CRC32C lookup table. Must be called once before any other
 * kb_crc32c call. Idempotent — safe to call multiple times. The pool
 * initialization sequence calls this first.
 */
void kb_crc32c_init(void);

/*
 * Compute CRC32C incrementally.
 *
 * @param crc   Initial value (0 for a new computation, or the previous
 *              result for chaining across multiple buffers).
 * @param data  Pointer to the byte buffer.
 * @param len   Number of bytes to process.
 * @return      Updated CRC32C value.
 *
 * Example (single buffer):
 *   uint32_t c = kb_crc32c(0, buf, buf_len);
 *
 * Example (chained):
 *   uint32_t c = kb_crc32c(0, header, 8);
 *   c = kb_crc32c(c, payload, payload_len);
 *
 * For the header CRC (§2.6): zero the 4-byte crc32c field, compute over
 * all 64 bytes, then write the result back.
 */
uint32_t kb_crc32c(uint32_t crc, const void *data, size_t len);

/* Convenience: CRC32C of a complete buffer (initial = 0). */
static inline uint32_t kb_crc32c_buf(const void *data, size_t len) {
    return kb_crc32c(0, data, len);
}

#ifdef __cplusplus
}
#endif

#endif /* KB_CRC32C_H */
