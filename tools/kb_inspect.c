/*
 * kb_inspect.c — CLI inspector / debug tool.
 *
 * A standalone tool that opens a pool.db and lets developers
 * inspect every layer of the storage engine interactively.
 *
 * Usage: kb_inspect <pool.db>
 *
 * Commands:
 *   info          — superblock summary
 *   stats         — pool statistics
 *   header <addr> — dump header at address
 *   index         — dump entire B+Tree index
 *   node <addr>   — dump B+Tree node at address
 *   children <addr> — list children of header
 *   extents <addr>  — list extents of header
 *   read <addr> <off> <len> — read bytes from data
 *   slab          — slab allocator state
 *   hex <addr> <len> — hex dump raw bytes
 *   quit          — exit
 */

#include "core/kb_store.h"
#include "core/kb_superblock.h"
#include "core/kb_path.h"
#include "core/kb_crc32c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static kb_store_t g_store;

/* ══════════════════ Print helpers ══════════════════ */

static void print_hex(const uint8_t *data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        if (i > 0 && (i % 16) == 0) printf("\n  ");
        printf("%02x ", data[i]);
    }
    printf("\n");
}

/* ══════════════════ Commands ══════════════════ */

static void cmd_info(void) {
    const kb_superblock_t *sb = &g_store.pool->sb;
    printf("═══ Superblock ═══\n");
    printf("  magic:          0x%08X\n", sb->magic);
    printf("  format_rev:     %u\n", sb->format_rev);
    printf("  page_size:      %u\n", sb->page_size);
    printf("  unit_size:      %u\n", sb->unit_size);
    printf("  uref_bits:      %u\n", sb->uref_bits);
    printf("  seg_size_mb:    %u\n", sb->segment_size_mb);
    printf("  segment_count:  %u\n", sb->segment_count);
    printf("  seq:            %llu\n", (unsigned long long)sb->seq);
    printf("  root_ref:       %llu (addr: 0x%llX)\n",
           (unsigned long long)sb->root_ref,
           (unsigned long long)(sb->root_ref << KB_UNIT_SHIFT));
    printf("  provisioned:    %llu bytes\n",
           (unsigned long long)sb->provisioned_bytes);
    printf("  hwm_unit:       %llu\n", (unsigned long long)sb->hwm_unit);
    printf("  object_count:   %llu\n", (unsigned long long)sb->object_count);
    printf("  wal_ckpt_lsn:   %llu\n",
           (unsigned long long)sb->wal_checkpoint_lsn);
    printf("  grow_step_mb:   %u\n", sb->grow_step_mb);
    printf("  feature_flags:  0x%08X\n", sb->feature_flags);
    printf("  clean_shutdown: %u\n", sb->clean_shutdown);
}

static void cmd_stats(void) {
    kb_store_stats_t stats;
    if (kb_store_get_stats(&g_store, &stats) != KB_OK) {
        printf("ERROR: could not get stats\n");
        return;
    }
    printf("═══ Stats ═══\n");
    printf("  total_pages:    %llu\n", (unsigned long long)stats.total_pages);
    printf("  index_height:   %u\n", stats.index_height);
    printf("  index_entries:  %u\n", stats.index_entries);
    printf("  buf_hits:       %llu\n", (unsigned long long)stats.buf_hits);
    printf("  buf_misses:     %llu\n", (unsigned long long)stats.buf_misses);
    printf("  wal_last_tx:    %u\n", stats.wal_last_tx);
}

static void cmd_header(const char *addr_str) {
    kb_addr_t addr = (kb_addr_t)strtoull(addr_str, NULL, 0);
    kb_header_t hdr;
    kb_status_t s = kb_hdr_read(g_store.pool, addr, 0, &hdr);
    if (s != KB_OK) {
        printf("ERROR: kb_hdr_read(0x%llX) → %d\n",
               (unsigned long long)addr, s);
        return;
    }
    printf("═══ Header @ 0x%llX ═══\n", (unsigned long long)addr);
    printf("  state_rev:      %u (%s)\n", hdr.state_rev,
           hdr.state_rev == KB_STATE_LIVE ? "LIVE" :
           hdr.state_rev == KB_STATE_DETACHED ? "DETACHED" :
           hdr.state_rev == KB_STATE_FREE ? "FREE" : "?");
    printf("  type_id:        %u\n", hdr.type_id);
    printf("  type_version:   %u\n", hdr.type_version);
    printf("  flags:          0x%08X\n", hdr.flags);
    printf("  slot_gen:       %u\n", hdr.slot_gen);
    printf("  birth_num:      %u\n", hdr.birth_num);
    printf("  next_child_ctr: %u\n", hdr.next_child_ctr);
    printf("  child_count:    %u\n", hdr.child_count);
    printf("  change_seq:     %u\n", hdr.change_seq);
    printf("  parent_ref:     %llu (addr: 0x%llX)\n",
           (unsigned long long)hdr.parent_ref,
           (unsigned long long)(hdr.parent_ref << KB_UNIT_SHIFT));
    printf("  children_ref:   %llu (addr: 0x%llX)\n",
           (unsigned long long)hdr.children_ref,
           (unsigned long long)(hdr.children_ref << KB_UNIT_SHIFT));
    printf("  ext0_ref:       %llu (addr: 0x%llX)\n",
           (unsigned long long)hdr.ext0_ref,
           (unsigned long long)(hdr.ext0_ref << KB_UNIT_SHIFT));
    printf("  ext0_cap_units: %u\n", hdr.ext0_cap_units);
    printf("  payload_len:    %llu\n", (unsigned long long)hdr.payload_len);
    printf("  crc32c:         0x%08X\n", hdr.crc32c);
}

static kb_status_t
idx_print_cb(const uint8_t *key, uint16_t key_len,
             kb_addr_t value, void *ctx)
{
    (void)ctx;
    printf("  key[%u]: ", key_len);
    for (uint16_t i = 0; i < key_len; i++) printf("%02x", key[i]);
    printf(" → 0x%llX\n", (unsigned long long)value);
    return KB_OK;
}

static void cmd_index(void) {
    printf("═══ Index (height=%u, entries=%u) ═══\n",
           kb_index_height(&g_store.index),
           kb_index_count(&g_store.index));
    kb_index_iterate(&g_store.index, idx_print_cb, NULL);
}

static void cmd_node(const char *addr_str) {
    kb_addr_t addr = (kb_addr_t)strtoull(addr_str, NULL, 0);
    uint32_t ps = (uint32_t)g_store.pool->sb.page_size;
    uint8_t *buf = (uint8_t *)malloc(ps);
    if (buf == NULL) { printf("ERROR: OOM\n"); return; }

    if (kb_os_pread(g_store.pool->pool_fd, buf, ps, addr) != KB_OK) {
        printf("ERROR: read failed at 0x%llX\n", (unsigned long long)addr);
        free(buf); return;
    }

    uint32_t magic = kb_read_u32(buf);
    uint16_t nkeys = kb_read_u16(buf + 4);
    uint16_t extra = kb_read_u16(buf + 6);
    uint64_t ptr0  = kb_read_u64(buf + 8);

    printf("═══ Node @ 0x%llX ═══\n", (unsigned long long)addr);
    printf("  magic:    0x%08X (%s)\n", magic,
           magic == KB_MAGIC_IDX_LEAF ? "LEAF" :
           magic == KB_MAGIC_IDX_INTL ? "INTL" : "?");
    printf("  num_keys: %u\n", nkeys);
    printf("  %s:   %u\n",
           magic == KB_MAGIC_IDX_LEAF ? "data_end" : "level", extra);
    printf("  %s:      0x%llX\n",
           magic == KB_MAGIC_IDX_LEAF ? "next_leaf" : "child_0",
           (unsigned long long)ptr0);

    /* Dump entries. */
    uint32_t off = 16;
    for (uint16_t i = 0; i < nkeys && off + 2 < ps; i++) {
        uint16_t klen = kb_read_u16(buf + off);
        klen &= 0x7FFF;
        if (off + 2 + klen + 8 > ps) break;
        printf("  [%u] key[%u]=", i, klen);
        for (uint16_t j = 0; j < klen; j++)
            printf("%02x", buf[off + 2 + j]);
        uint64_t val = kb_read_u64(buf + off + 2 + klen);
        printf(" → 0x%llX\n", (unsigned long long)val);
        off += 2 + klen + 8;
    }

    free(buf);
}

static void cmd_children(const char *addr_str) {
    kb_addr_t addr = (kb_addr_t)strtoull(addr_str, NULL, 0);
    kb_header_t hdr;
    if (kb_hdr_read(g_store.pool, addr, 0, &hdr) != KB_OK) {
        printf("ERROR: cannot read header\n"); return;
    }
    printf("═══ Children of 0x%llX (count=%u) ═══\n",
           (unsigned long long)addr, hdr.child_count);

    /* Use the child iterate API. */
    kb_child_iterate(g_store.pool, &hdr,
        (kb_child_iter_fn)NULL, NULL);
    /* TODO: wire up a printer callback */
    printf("  (use 'header <addr>' on children_ref for details)\n");
}

static void cmd_extents(const char *addr_str) {
    kb_addr_t addr = (kb_addr_t)strtoull(addr_str, NULL, 0);
    kb_header_t hdr;
    if (kb_hdr_read(g_store.pool, addr, 0, &hdr) != KB_OK) {
        printf("ERROR: cannot read header\n"); return;
    }
    printf("═══ Extents of 0x%llX ═══\n", (unsigned long long)addr);
    uint32_t ec = kb_extent_count(&hdr);
    printf("  count: %u\n", ec);
    if (ec > 0) {
        kb_extent_entry_t ext;
        if (kb_extent_get_inline(&hdr, &ext) == KB_OK) {
            printf("  [inline] ref=%llu (addr=0x%llX) cap_units=%u\n",
                   (unsigned long long)ext.ext_ref,
                   (unsigned long long)(ext.ext_ref << KB_UNIT_SHIFT),
                   ext.cap_units);
        }
    }
}

static void cmd_read_data(const char *args) {
    kb_addr_t addr;
    uint64_t off;
    uint32_t len;
    if (sscanf(args, "%llx %llu %u",
               (unsigned long long *)&addr,
               (unsigned long long *)&off, &len) != 3) {
        printf("Usage: read <addr-hex> <offset> <length>\n");
        return;
    }
    if (len > 4096) len = 4096;
    uint8_t *buf = (uint8_t *)malloc(len);
    if (buf == NULL) { printf("ERROR: OOM\n"); return; }

    kb_status_t s = kb_store_read_data(&g_store, addr, off, buf, len);
    if (s != KB_OK) {
        printf("ERROR: read_data → %d\n", s);
        free(buf); return;
    }
    printf("═══ Data @ obj=0x%llX offset=%llu len=%u ═══\n  ",
           (unsigned long long)addr, (unsigned long long)off, len);
    print_hex(buf, len);
    free(buf);
}

static void cmd_hex(const char *args) {
    kb_addr_t addr;
    uint32_t len;
    if (sscanf(args, "%llx %u",
               (unsigned long long *)&addr, &len) != 2) {
        printf("Usage: hex <addr-hex> <length>\n");
        return;
    }
    if (len > 4096) len = 4096;
    uint8_t *buf = (uint8_t *)malloc(len);
    if (buf == NULL) { printf("ERROR: OOM\n"); return; }

    if (kb_os_pread(g_store.pool->pool_fd, buf, len, addr) != KB_OK) {
        printf("ERROR: read failed\n");
        free(buf); return;
    }
    printf("═══ Hex @ 0x%llX (%u bytes) ═══\n  ",
           (unsigned long long)addr, len);
    print_hex(buf, len);
    free(buf);
}

/* ══════════════════ REPL ══════════════════ */

static void print_help(void) {
    printf("\n═══ KB Inspector Commands ═══\n");
    printf("  info               — superblock summary\n");
    printf("  stats              — store statistics\n");
    printf("  header <addr>      — dump header at address (hex)\n");
    printf("  index              — dump entire B+Tree index\n");
    printf("  node <addr>        — dump B+Tree node (hex)\n");
    printf("  children <addr>    — list children of header\n");
    printf("  extents <addr>     — list extents of header\n");
    printf("  read <addr> <off> <len> — read object data\n");
    printf("  hex <addr> <len>   — raw hex dump at address\n");
    printf("  help               — show this help\n");
    printf("  quit               — exit\n\n");
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <pool.db>\n", argv[0]);
        return 1;
    }

    kb_crc32c_init();

    kb_store_config_t cfg = { .buf_pool_slots = 64 };
    kb_status_t s = kb_store_open(argv[1], &cfg, &g_store);
    if (s != KB_OK) {
        fprintf(stderr, "ERROR: kb_store_open(%s) → %d\n", argv[1], s);
        return 1;
    }

    printf("KB Inspector — opened %s\n", argv[1]);
    print_help();

    char line[256];
    while (1) {
        printf("kb> ");
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL) break;

        /* Trim newline. */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';
        if (len == 0) continue;

        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) break;
        else if (strcmp(line, "info") == 0) cmd_info();
        else if (strcmp(line, "stats") == 0) cmd_stats();
        else if (strcmp(line, "index") == 0) cmd_index();
        else if (strcmp(line, "help") == 0) print_help();
        else if (strncmp(line, "header ", 7) == 0) cmd_header(line + 7);
        else if (strncmp(line, "node ", 5) == 0) cmd_node(line + 5);
        else if (strncmp(line, "children ", 9) == 0) cmd_children(line + 9);
        else if (strncmp(line, "extents ", 8) == 0) cmd_extents(line + 8);
        else if (strncmp(line, "read ", 5) == 0) cmd_read_data(line + 5);
        else if (strncmp(line, "hex ", 4) == 0) cmd_hex(line + 4);
        else printf("Unknown command. Type 'help' for commands.\n");
    }

    kb_store_close(&g_store);
    printf("Closed.\n");
    return 0;
}
