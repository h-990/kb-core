/*
 * kb_verify.c — Pool verification and repair implementation (§6).
 */

#include "core/kb_verify.h"
#include "core/kb_superblock.h"
#include "core/kb_header.h"
#include "core/kb_block.h"
#include "core/kb_child.h"
#include "core/kb_extent.h"
#include "core/kb_crc32c.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ══════════════════ Issue reporting ══════════════════ */

static void
report(kb_verify_cb cb, void *ctx, kb_verify_result_t *result,
       kb_issue_level_t level, kb_addr_t addr,
       const char *module, const char *message)
{
    switch (level) {
    case KB_ISSUE_INFO:    result->info_count++;    break;
    case KB_ISSUE_WARNING: result->warning_count++; break;
    case KB_ISSUE_ERROR:   result->error_count++;   break;
    case KB_ISSUE_FATAL:   result->fatal_count++;   break;
    }

    if (cb != NULL) {
        kb_verify_issue_t issue = {
            .level   = level,
            .addr    = addr,
            .module  = module,
            .message = message,
        };
        cb(&issue, ctx);
    }
}

/* ══════════════════ Superblock check ══════════════════ */

static void
verify_superblock(kb_pool_t *pool, const kb_verify_opts_t *opts,
                  kb_verify_cb cb, void *ctx, kb_verify_result_t *result)
{
    (void)opts;
    const kb_superblock_t *sb = &pool->sb;

    if (sb->magic != KB_MAGIC_POOL) {
        report(cb, ctx, result, KB_ISSUE_FATAL, 0,
               "superblock", "invalid magic number");
    } else {
        report(cb, ctx, result, KB_ISSUE_INFO, 0,
               "superblock", "magic OK");
    }

    if (sb->format_rev != KB_FORMAT_REV) {
        report(cb, ctx, result, KB_ISSUE_WARNING, 0,
               "superblock", "format revision mismatch");
    }

    if (sb->page_size != KB_PAGE_SIZE_DEFAULT &&
        sb->page_size != 8192 && sb->page_size != 16384) {
        report(cb, ctx, result, KB_ISSUE_ERROR, 0,
               "superblock", "unusual page size");
    }
}

/* ══════════════════ Index integrity ══════════════════ */

typedef struct idx_verify_ctx {
    kb_pool_t           *pool;
    const kb_verify_opts_t *opts;
    kb_verify_cb         cb;
    void                *user_ctx;
    kb_verify_result_t  *result;
    kb_addr_t            last_value;
    uint64_t             count;
} idx_verify_ctx_t;

static kb_status_t
idx_verify_cb_fn(const uint8_t *key, uint16_t key_len,
                  kb_addr_t value, void *ctx)
{
    idx_verify_ctx_t *vc = (idx_verify_ctx_t *)ctx;

    /* Verify key starts with root marker 0x00. */
    if (key_len == 0 || key[0] != 0x00) {
        char msg[64];
        snprintf(msg, sizeof(msg), "index entry has invalid path_uid (no root marker)");
        report(vc->cb, vc->user_ctx, vc->result, KB_ISSUE_ERROR,
               value, "index", msg);
    }

    /* Verify the value (header addr) is readable. */
    kb_header_t hdr;
    kb_status_t s = kb_hdr_read(vc->pool, value, 0, &hdr);
    if (s != KB_OK) {
        report(vc->cb, vc->user_ctx, vc->result, KB_ISSUE_ERROR,
               value, "index", "index points to unreadable header");
    } else {
        vc->result->headers_checked++;

        if (hdr.state_rev == KB_STATE_FREE) {
            report(vc->cb, vc->user_ctx, vc->result, KB_ISSUE_WARNING,
                   value, "index", "index points to FREE header slot");
        }
    }

    vc->count++;
    vc->result->index_entries_checked++;
    return KB_OK;
}

static void
verify_index(kb_pool_t *pool, kb_index_t *idx,
             const kb_verify_opts_t *opts,
             kb_verify_cb cb, void *ctx, kb_verify_result_t *result)
{
    idx_verify_ctx_t vc = {
        .pool     = pool,
        .opts     = opts,
        .cb       = cb,
        .user_ctx = ctx,
        .result   = result,
        .count    = 0,
    };

    kb_status_t s = kb_index_iterate(idx, idx_verify_cb_fn, &vc);
    if (s != KB_OK) {
        report(cb, ctx, result, KB_ISSUE_ERROR, 0,
               "index", "iteration failed (possible node corruption)");
    }

    char msg[128];
    snprintf(msg, sizeof(msg),
             "index: verified %llu entries, height=%u",
             (unsigned long long)vc.count, kb_index_height(idx));
    report(cb, ctx, result, KB_ISSUE_INFO, 0, "index", msg);
}

/* ══════════════════ Header scan ══════════════════ */

static void
verify_headers_range(kb_pool_t *pool, kb_addr_t start, kb_addr_t end,
                     const kb_verify_opts_t *opts,
                     kb_verify_cb cb, void *ctx,
                     kb_verify_result_t *result)
{
    (void)opts;
    uint8_t buf[KB_HDR_SIZE];

    for (kb_addr_t addr = start; addr < end; addr += KB_HDR_SIZE) {
        kb_status_t s = kb_os_pread(pool->pool_fd, buf, KB_HDR_SIZE, addr);
        if (s != KB_OK) continue;

        /* Check if this looks like a header (non-zero state_rev). */
        uint8_t state = buf[0];
        if (state == KB_STATE_FREE) continue;
        if (state != KB_STATE_LIVE && state != KB_STATE_DETACHED &&
            state != KB_STATE_FORWARD) continue;

        /* Try to deserialize. */
        kb_header_t hdr;
        s = kb_hdr_deserialize(buf, &hdr);
        if (s == KB_OK) {
            result->headers_checked++;
            result->pages_checked++;

            /* Check parent ref validity. */
            if (hdr.parent_ref != 0 && state == KB_STATE_LIVE) {
                kb_addr_t parent_addr = hdr.parent_ref << KB_UNIT_SHIFT;
                kb_header_t phdr;
                s = kb_hdr_read(pool, parent_addr, 0, &phdr);
                if (s != KB_OK) {
                    report(cb, ctx, result, KB_ISSUE_WARNING, addr,
                           "header", "parent_ref points to unreadable header");
                }
            }
        } else if (s == KB_E_CORRUPT) {
            report(cb, ctx, result, KB_ISSUE_ERROR, addr,
                   "header", "CRC mismatch on header slot");
        }
    }
}

/* ══════════════════ Main verify ══════════════════ */

kb_status_t
kb_verify(kb_pool_t *pool, kb_index_t *idx,
          const kb_verify_opts_t *opts,
          kb_verify_cb callback, void *ctx,
          kb_verify_result_t *result)
{
    if (pool == NULL || result == NULL) return KB_E_BAD_ARG;

    memset(result, 0, sizeof(*result));

    kb_verify_opts_t default_opts = { false, false, false };
    if (opts == NULL) opts = &default_opts;

    /* 1. Superblock. */
    verify_superblock(pool, opts, callback, ctx, result);

    /* 2. Index (if provided). */
    if (idx != NULL) {
        verify_index(pool, idx, opts, callback, ctx, result);
    }

    /* 3. Header scan over the HWM range (opt-in, slow).
     *    Only run when check_data is true, since the blind scan can hit
     *    non-header data that happens to have valid state_rev bytes. */
    if (opts->check_data) {
        uint64_t hwm_bytes = pool->sb.hwm_unit << KB_UNIT_SHIFT;
        if (hwm_bytes > 0 && hwm_bytes > (uint64_t)pool->sb.page_size * 2) {
            kb_addr_t scan_start = (kb_addr_t)pool->sb.page_size * 2;
            verify_headers_range(pool, scan_start, hwm_bytes,
                                 opts, callback, ctx, result);
        }
    }

    return KB_OK;
}
