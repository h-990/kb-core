/*
 * kb_verify.h — Pool verification and repair (§6).
 *
 * Walks every on-disk structure and validates integrity:
 *   - Superblock CRC and magic
 *   - Segment bitmap consistency
 *   - All header CRCs
 *   - All block CRCs
 *   - Index leaf chain integrity
 *   - Parent/child ref consistency
 *   - Extent ref validity
 *
 * Reports issues via a callback. Optional repair mode
 * fixes recoverable problems (CRC recompute, orphan cleanup).
 */
#ifndef KB_VERIFY_H
#define KB_VERIFY_H

#include "core/kb_types.h"
#include "core/kb_pool.h"
#include "core/kb_index.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Issue severity. */
typedef enum kb_issue_level {
    KB_ISSUE_INFO    = 0,
    KB_ISSUE_WARNING = 1,
    KB_ISSUE_ERROR   = 2,
    KB_ISSUE_FATAL   = 3
} kb_issue_level_t;

/* Issue report. */
typedef struct kb_verify_issue {
    kb_issue_level_t level;
    kb_addr_t        addr;      /* address of problematic structure */
    const char      *module;    /* "superblock", "header", "index", etc. */
    const char      *message;
} kb_verify_issue_t;

/* Callback for each issue found. */
typedef void (*kb_verify_cb)(const kb_verify_issue_t *issue, void *ctx);

/* Verify options. */
typedef struct kb_verify_opts {
    bool repair;        /* attempt to fix recoverable issues */
    bool verbose;       /* report INFO-level items */
    bool check_data;    /* verify data page checksums (slow) */
} kb_verify_opts_t;

/* Verify results summary. */
typedef struct kb_verify_result {
    uint32_t info_count;
    uint32_t warning_count;
    uint32_t error_count;
    uint32_t fatal_count;
    uint32_t repaired_count;
    uint64_t headers_checked;
    uint64_t index_entries_checked;
    uint64_t pages_checked;
} kb_verify_result_t;

/*
 * Run a full verification pass on the pool.
 */
kb_status_t kb_verify(kb_pool_t *pool, kb_index_t *idx,
                      const kb_verify_opts_t *opts,
                      kb_verify_cb callback, void *ctx,
                      kb_verify_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* KB_VERIFY_H */
