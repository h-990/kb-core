/*
 * kb_server.c — Minimal HTTP server for the KB developer testing GUI.
 *
 * Serves the web GUI and exposes all KB Store operations as a
 * JSON API at POST /api.
 *
 * Usage: kb_server [port]      (default: 9090)
 */

#include "core/kb_store.h"
#include "core/kb_verify.h"
#include "core/kb_superblock.h"
#include "core/kb_path.h"
#include "core/kb_crc32c.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <errno.h>

/* ══════════════════ Globals ══════════════════ */

static kb_store_t g_store;
static bool g_store_open = false;
static char g_response[1024 * 64]; /* 64 KB response buffer */

/* ══════════════════ JSON helpers ══════════════════ */

/* Mini JSON string escaper. */
static void json_escape(char *dst, size_t cap, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < cap; i++) {
        if (src[i] == '"' || src[i] == '\\') dst[j++] = '\\';
        dst[j++] = src[i];
    }
    dst[j] = '\0';
}

/* Find a JSON string value by key (very basic). */
static const char *json_get_str(const char *json, const char *key,
                                 char *buf, size_t cap) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < cap) {
            if (*p == '\\' && *(p+1)) { p++; }
            buf[i++] = *p++;
        }
        buf[i] = '\0';
        return buf;
    }
    /* Number or boolean. */
    size_t i = 0;
    while (*p && *p != ',' && *p != '}' && *p != ' ' && i + 1 < cap) {
        buf[i++] = *p++;
    }
    buf[i] = '\0';
    return buf;
}

static uint64_t json_get_u64(const char *json, const char *key) {
    char buf[32];
    if (json_get_str(json, key, buf, sizeof(buf)))
        return (uint64_t)strtoull(buf, NULL, 0);
    return 0;
}

static uint32_t json_get_u32(const char *json, const char *key) {
    return (uint32_t)json_get_u64(json, key);
}

/* ══════════════════ API Handlers ══════════════════ */

static const char *api_store_create(const char *body) {
    if (g_store_open) {
        kb_store_close(&g_store);
        g_store_open = false;
    }
    char path[256];
    json_get_str(body, "path", path, sizeof(path));
    if (path[0] == '\0') strcpy(path, "/tmp/kb_dev_gui.db");

    /* Clean up old files. */
    char wal[260];
    size_t plen = strlen(path);
    if (plen >= 3 && strcmp(path + plen - 3, ".db") == 0) {
        memcpy(wal, path, plen - 3);
        memcpy(wal + plen - 3, ".wal", 5);
    } else {
        snprintf(wal, sizeof(wal), "%s.wal", path);
    }
    kb_os_delete(path);
    kb_os_delete(wal);

    kb_store_config_t cfg = {
        .page_size      = json_get_u32(body, "page_size"),
        .initial_size_mb = json_get_u32(body, "initial_size_mb"),
        .grow_step_mb   = json_get_u32(body, "grow_step_mb"),
        .buf_pool_slots = json_get_u32(body, "buf_pool_slots"),
    };

    kb_status_t s = kb_store_create(path, &cfg, &g_store);
    g_store_open = (s == KB_OK);
    snprintf(g_response, sizeof(g_response),
             "{\"status\":%d,\"open\":%s,\"path\":\"%s\"}",
             s, g_store_open ? "true" : "false", path);
    return g_response;
}

static const char *api_store_open(const char *body) {
    if (g_store_open) {
        kb_store_close(&g_store);
        g_store_open = false;
    }
    char path[256];
    json_get_str(body, "path", path, sizeof(path));
    if (path[0] == '\0') return "{\"status\":-4,\"error\":\"no path\"}";

    kb_store_config_t cfg = { .buf_pool_slots = 64 };
    kb_status_t s = kb_store_open(path, &cfg, &g_store);
    g_store_open = (s == KB_OK);
    snprintf(g_response, sizeof(g_response),
             "{\"status\":%d,\"open\":%s}", s, g_store_open ? "true" : "false");
    return g_response;
}

static const char *api_store_close(const char *body) {
    (void)body;
    if (!g_store_open) return "{\"status\":-4,\"error\":\"not open\"}";
    kb_status_t s = kb_store_close(&g_store);
    g_store_open = false;
    snprintf(g_response, sizeof(g_response), "{\"status\":%d}", s);
    return g_response;
}

static const char *api_info(const char *body) {
    (void)body;
    if (!g_store_open) return "{\"status\":-4,\"error\":\"not open\"}";
    const kb_superblock_t *sb = &g_store.pool->sb;
    snprintf(g_response, sizeof(g_response),
        "{\"status\":0,\"magic\":\"0x%08X\",\"format_rev\":%u,"
        "\"page_size\":%u,\"unit_size\":%u,\"uref_bits\":%u,"
        "\"segment_size_mb\":%u,\"segment_count\":%u,"
        "\"seq\":%llu,\"root_ref\":%llu,\"root_addr\":\"0x%llX\","
        "\"provisioned_bytes\":%llu,\"hwm_unit\":%llu,"
        "\"object_count\":%llu,\"grow_step_mb\":%u,"
        "\"clean_shutdown\":%u,\"feature_flags\":\"0x%08X\"}",
        sb->magic, sb->format_rev,
        sb->page_size, sb->unit_size, sb->uref_bits,
        sb->segment_size_mb, sb->segment_count,
        (unsigned long long)sb->seq,
        (unsigned long long)sb->root_ref,
        (unsigned long long)(sb->root_ref << KB_UNIT_SHIFT),
        (unsigned long long)sb->provisioned_bytes,
        (unsigned long long)sb->hwm_unit,
        (unsigned long long)sb->object_count,
        sb->grow_step_mb, sb->clean_shutdown, sb->feature_flags);
    return g_response;
}

static const char *api_stats(const char *body) {
    (void)body;
    if (!g_store_open) return "{\"status\":-4,\"error\":\"not open\"}";
    kb_store_stats_t stats;
    kb_store_get_stats(&g_store, &stats);
    snprintf(g_response, sizeof(g_response),
        "{\"status\":0,\"total_pages\":%llu,\"free_pages\":%llu,"
        "\"index_height\":%u,\"index_entries\":%u,"
        "\"buf_hits\":%llu,\"buf_misses\":%llu,"
        "\"wal_last_tx\":%u}",
        (unsigned long long)stats.total_pages,
        (unsigned long long)stats.free_pages,
        stats.index_height, stats.index_entries,
        (unsigned long long)stats.buf_hits,
        (unsigned long long)stats.buf_misses,
        stats.wal_last_tx);
    return g_response;
}

static const char *api_create_root(const char *body) {
    if (!g_store_open) return "{\"status\":-4,\"error\":\"not open\"}";
    char name[64] = "";
    json_get_str(body, "name", name, sizeof(name));
    uint16_t type_id = (uint16_t)json_get_u32(body, "type_id");

    kb_addr_t addr;
    kb_status_t s = kb_store_create_root(&g_store, type_id, name, &addr);
    snprintf(g_response, sizeof(g_response),
        "{\"status\":%d,\"addr\":%llu,\"addr_hex\":\"0x%llX\"}",
        s, (unsigned long long)addr, (unsigned long long)addr);
    return g_response;
}

static const char *api_create_child(const char *body) {
    if (!g_store_open) return "{\"status\":-4,\"error\":\"not open\"}";
    kb_addr_t parent = json_get_u64(body, "parent_addr");
    uint16_t type_id = (uint16_t)json_get_u32(body, "type_id");
    char name[64] = "";
    json_get_str(body, "name", name, sizeof(name));

    kb_addr_t addr;
    uint32_t birth;
    kb_status_t s = kb_store_create_child(&g_store, parent, type_id,
                                           name, &addr, &birth);
    snprintf(g_response, sizeof(g_response),
        "{\"status\":%d,\"addr\":%llu,\"addr_hex\":\"0x%llX\","
        "\"birth_num\":%u}",
        s, (unsigned long long)addr, (unsigned long long)addr, birth);
    return g_response;
}

static const char *api_read_header(const char *body) {
    if (!g_store_open) return "{\"status\":-4,\"error\":\"not open\"}";
    kb_addr_t addr = json_get_u64(body, "addr");
    kb_header_t hdr;
    kb_status_t s = kb_store_read_header(&g_store, addr, &hdr);
    if (s != KB_OK) {
        snprintf(g_response, sizeof(g_response), "{\"status\":%d}", s);
        return g_response;
    }
    const char *state_name =
        hdr.state_rev == KB_STATE_LIVE ? "LIVE" :
        hdr.state_rev == KB_STATE_DETACHED ? "DETACHED" :
        hdr.state_rev == KB_STATE_FREE ? "FREE" : "UNKNOWN";

    snprintf(g_response, sizeof(g_response),
        "{\"status\":0,\"state\":\"%s\",\"state_rev\":%u,"
        "\"type_id\":%u,\"type_version\":%u,"
        "\"flags\":\"0x%08X\",\"slot_gen\":%u,"
        "\"birth_num\":%u,\"next_child_ctr\":%u,"
        "\"child_count\":%u,\"change_seq\":%u,"
        "\"parent_ref\":%llu,\"parent_addr\":\"0x%llX\","
        "\"children_ref\":%llu,\"children_addr\":\"0x%llX\","
        "\"ext0_ref\":%llu,\"ext0_addr\":\"0x%llX\","
        "\"ext0_cap_units\":%u,\"payload_len\":%llu,"
        "\"crc32c\":\"0x%08X\"}",
        state_name, hdr.state_rev,
        hdr.type_id, hdr.type_version,
        hdr.flags, hdr.slot_gen,
        hdr.birth_num, hdr.next_child_ctr,
        hdr.child_count, hdr.change_seq,
        (unsigned long long)hdr.parent_ref,
        (unsigned long long)(hdr.parent_ref << KB_UNIT_SHIFT),
        (unsigned long long)hdr.children_ref,
        (unsigned long long)(hdr.children_ref << KB_UNIT_SHIFT),
        (unsigned long long)hdr.ext0_ref,
        (unsigned long long)(hdr.ext0_ref << KB_UNIT_SHIFT),
        hdr.ext0_cap_units, (unsigned long long)hdr.payload_len,
        hdr.crc32c);
    return g_response;
}

static const char *api_delete_object(const char *body) {
    if (!g_store_open) return "{\"status\":-4,\"error\":\"not open\"}";
    kb_addr_t addr = json_get_u64(body, "addr");
    kb_status_t s = kb_store_delete(&g_store, addr);
    snprintf(g_response, sizeof(g_response), "{\"status\":%d}", s);
    return g_response;
}

static const char *api_write_data(const char *body) {
    if (!g_store_open) return "{\"status\":-4,\"error\":\"not open\"}";
    kb_addr_t addr = json_get_u64(body, "addr");
    uint64_t offset = json_get_u64(body, "offset");
    char data[1024] = "";
    json_get_str(body, "data", data, sizeof(data));
    uint32_t len = (uint32_t)strlen(data);

    kb_status_t s = kb_store_write_data(&g_store, addr, offset, data, len);
    snprintf(g_response, sizeof(g_response),
             "{\"status\":%d,\"bytes_written\":%u}", s, len);
    return g_response;
}

static const char *api_read_data(const char *body) {
    if (!g_store_open) return "{\"status\":-4,\"error\":\"not open\"}";
    kb_addr_t addr = json_get_u64(body, "addr");
    uint64_t offset = json_get_u64(body, "offset");
    uint32_t len = json_get_u32(body, "length");
    if (len > 4096) len = 4096;

    char buf[4096];
    memset(buf, 0, sizeof(buf));
    kb_status_t s = kb_store_read_data(&g_store, addr, offset, buf, len);
    if (s != KB_OK) {
        snprintf(g_response, sizeof(g_response), "{\"status\":%d}", s);
        return g_response;
    }

    /* Build hex string. */
    char hex[8200];
    size_t hi = 0;
    for (uint32_t i = 0; i < len && hi + 3 < sizeof(hex); i++) {
        hi += (size_t)snprintf(hex + hi, sizeof(hex) - hi,
                               "%02x", (uint8_t)buf[i]);
    }
    hex[hi] = '\0';

    /* Escape the raw text. */
    char escaped[4200];
    json_escape(escaped, sizeof(escaped), buf);

    snprintf(g_response, sizeof(g_response),
        "{\"status\":0,\"hex\":\"%s\",\"text\":\"%s\",\"length\":%u}",
        hex, escaped, len);
    return g_response;
}

/* Index scan callback. */
static char *g_idx_ptr;
static size_t g_idx_cap;
static uint32_t g_idx_count;

static kb_status_t idx_scan_cb(const uint8_t *key, uint16_t key_len,
                                kb_addr_t value, void *ctx) {
    (void)ctx;
    if (g_idx_count > 0 && g_idx_ptr < g_response + g_idx_cap - 2)
        *g_idx_ptr++ = ',';

    size_t remain = g_idx_cap - (size_t)(g_idx_ptr - g_response);
    int wrote = snprintf(g_idx_ptr, remain,
        "{\"key\":\"");
    g_idx_ptr += wrote;
    remain = g_idx_cap - (size_t)(g_idx_ptr - g_response);
    for (uint16_t i = 0; i < key_len && remain > 3; i++) {
        int w = snprintf(g_idx_ptr, remain, "%02x", key[i]);
        g_idx_ptr += w;
        remain -= (size_t)w;
    }
    wrote = snprintf(g_idx_ptr, remain,
        "\",\"key_len\":%u,\"value\":%llu,\"value_hex\":\"0x%llX\"}",
        key_len, (unsigned long long)value, (unsigned long long)value);
    g_idx_ptr += wrote;
    g_idx_count++;
    if (g_idx_count >= 500) return KB_E_BUSY; /* limit output */
    return KB_OK;
}

static const char *api_index_scan(const char *body) {
    (void)body;
    if (!g_store_open) return "{\"status\":-4,\"error\":\"not open\"}";

    g_idx_ptr = g_response;
    g_idx_cap = sizeof(g_response) - 100;
    g_idx_count = 0;

    int n = snprintf(g_idx_ptr, g_idx_cap,
        "{\"status\":0,\"height\":%u,\"count\":%u,\"entries\":[",
        kb_index_height(&g_store.index),
        kb_index_count(&g_store.index));
    g_idx_ptr += n;

    kb_index_iterate(&g_store.index, idx_scan_cb, NULL);

    size_t remain = sizeof(g_response) - (size_t)(g_idx_ptr - g_response);
    snprintf(g_idx_ptr, remain, "]}");
    return g_response;
}

static const char *api_tx_begin(const char *body) {
    (void)body;
    if (!g_store_open) return "{\"status\":-4,\"error\":\"not open\"}";
    kb_status_t s = kb_store_begin_tx(&g_store);
    snprintf(g_response, sizeof(g_response), "{\"status\":%d}", s);
    return g_response;
}

static const char *api_tx_commit(const char *body) {
    (void)body;
    if (!g_store_open) return "{\"status\":-4,\"error\":\"not open\"}";
    kb_status_t s = kb_store_commit_tx(&g_store);
    snprintf(g_response, sizeof(g_response), "{\"status\":%d}", s);
    return g_response;
}

static const char *api_tx_abort(const char *body) {
    (void)body;
    if (!g_store_open) return "{\"status\":-4,\"error\":\"not open\"}";
    kb_status_t s = kb_store_abort_tx(&g_store);
    snprintf(g_response, sizeof(g_response), "{\"status\":%d}", s);
    return g_response;
}

static const char *api_checkpoint(const char *body) {
    (void)body;
    if (!g_store_open) return "{\"status\":-4,\"error\":\"not open\"}";
    kb_status_t s = kb_store_checkpoint(&g_store);
    snprintf(g_response, sizeof(g_response), "{\"status\":%d}", s);
    return g_response;
}

static const char *api_flush(const char *body) {
    (void)body;
    if (!g_store_open) return "{\"status\":-4,\"error\":\"not open\"}";
    kb_status_t s = kb_store_flush(&g_store);
    snprintf(g_response, sizeof(g_response), "{\"status\":%d}", s);
    return g_response;
}

/* Verify callback. */
static char *g_vfy_ptr;
static size_t g_vfy_cap;
static uint32_t g_vfy_count;

static void vfy_cb(const kb_verify_issue_t *issue, void *ctx) {
    (void)ctx;
    if (g_vfy_count > 0 && g_vfy_ptr < g_response + g_vfy_cap - 2)
        *g_vfy_ptr++ = ',';

    const char *lvl[] = {"info","warning","error","fatal"};
    char escaped_msg[256];
    json_escape(escaped_msg, sizeof(escaped_msg), issue->message);

    size_t remain = g_vfy_cap - (size_t)(g_vfy_ptr - g_response);
    int wrote = snprintf(g_vfy_ptr, remain,
        "{\"level\":\"%s\",\"module\":\"%s\","
        "\"addr\":%llu,\"message\":\"%s\"}",
        lvl[issue->level], issue->module,
        (unsigned long long)issue->addr, escaped_msg);
    g_vfy_ptr += wrote;
    g_vfy_count++;
}

static const char *api_verify(const char *body) {
    (void)body;
    if (!g_store_open) return "{\"status\":-4,\"error\":\"not open\"}";

    g_vfy_ptr = g_response;
    g_vfy_cap = sizeof(g_response) - 200;
    g_vfy_count = 0;

    int n = snprintf(g_vfy_ptr, g_vfy_cap,
                     "{\"status\":0,\"issues\":[");
    g_vfy_ptr += n;

    kb_verify_result_t result;
    kb_verify(g_store.pool, &g_store.index, NULL, vfy_cb, NULL, &result);

    size_t remain = sizeof(g_response) - (size_t)(g_vfy_ptr - g_response);
    snprintf(g_vfy_ptr, remain,
        "],\"info\":%u,\"warnings\":%u,\"errors\":%u,\"fatal\":%u,"
        "\"headers_checked\":%llu,\"index_checked\":%llu}",
        result.info_count, result.warning_count,
        result.error_count, result.fatal_count,
        (unsigned long long)result.headers_checked,
        (unsigned long long)result.index_entries_checked);
    return g_response;
}

static const char *api_hex_dump(const char *body) {
    if (!g_store_open) return "{\"status\":-4,\"error\":\"not open\"}";
    kb_addr_t addr = json_get_u64(body, "addr");
    uint32_t len = json_get_u32(body, "length");
    if (len > 4096) len = 4096;
    if (len == 0) len = 64;

    uint8_t buf[4096];
    kb_status_t s = kb_os_pread(g_store.pool->pool_fd, buf, len, addr);
    if (s != KB_OK) {
        snprintf(g_response, sizeof(g_response), "{\"status\":%d}", s);
        return g_response;
    }

    char *p = g_response;
    size_t cap = sizeof(g_response);
    int n = snprintf(p, cap, "{\"status\":0,\"addr\":%llu,\"length\":%u,\"hex\":\"",
                     (unsigned long long)addr, len);
    p += n; cap -= (size_t)n;

    for (uint32_t i = 0; i < len && cap > 4; i++) {
        int w = snprintf(p, cap, "%02x", buf[i]);
        p += w; cap -= (size_t)w;
    }
    snprintf(p, cap, "\"}");
    return g_response;
}

static const char *api_path_encode(const char *body) {
    uint32_t birth = json_get_u32(body, "birth_num");
    uint8_t out[8];
    int n = kb_component_encode(birth, out, sizeof(out));
    if (n <= 0) {
        snprintf(g_response, sizeof(g_response),
                 "{\"status\":-4,\"error\":\"encode failed\"}");
        return g_response;
    }
    char hex[20]; size_t hi = 0;
    for (int i = 0; i < n; i++)
        hi += (size_t)snprintf(hex + hi, sizeof(hex) - hi, "%02x", out[i]);
    snprintf(g_response, sizeof(g_response),
             "{\"status\":0,\"bytes\":\"%s\",\"length\":%d}", hex, n);
    return g_response;
}

/* ══════════════════ Router ══════════════════ */

typedef const char *(*api_handler_fn)(const char *body);

static struct { const char *method; api_handler_fn fn; } g_routes[] = {
    {"store.create",     api_store_create},
    {"store.open",       api_store_open},
    {"store.close",      api_store_close},
    {"store.info",       api_info},
    {"store.stats",      api_stats},
    {"object.createRoot", api_create_root},
    {"object.createChild", api_create_child},
    {"object.readHeader", api_read_header},
    {"object.delete",    api_delete_object},
    {"data.write",       api_write_data},
    {"data.read",        api_read_data},
    {"index.scan",       api_index_scan},
    {"tx.begin",         api_tx_begin},
    {"tx.commit",        api_tx_commit},
    {"tx.abort",         api_tx_abort},
    {"maintenance.checkpoint", api_checkpoint},
    {"maintenance.flush", api_flush},
    {"maintenance.verify", api_verify},
    {"debug.hexDump",    api_hex_dump},
    {"path.encode",      api_path_encode},
    {NULL, NULL}
};

static const char *route_api(const char *body) {
    char method[64];
    if (!json_get_str(body, "method", method, sizeof(method)))
        return "{\"error\":\"missing method\"}";

    for (int i = 0; g_routes[i].method; i++) {
        if (strcmp(g_routes[i].method, method) == 0)
            return g_routes[i].fn(body);
    }
    snprintf(g_response, sizeof(g_response),
             "{\"error\":\"unknown method: %s\"}", method);
    return g_response;
}

/* ══════════════════ HTTP Server ══════════════════ */

/* Embedded HTML GUI (extern, linked from generated .c) */
extern const char kb_gui_html[];
extern const unsigned int kb_gui_html_len;

static void send_response(int fd, const char *status,
                           const char *content_type,
                           const char *body, size_t body_len) {
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, content_type, body_len);
    (void)write(fd, header, (size_t)hlen);
    if (body_len > 0)
        (void)write(fd, body, body_len);
}

static void handle_request(int client_fd) {
    char buf[65536];
    ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = '\0';

    /* Parse method and path. */
    char method[8], path[256];
    sscanf(buf, "%7s %255s", method, path);

    /* CORS preflight. */
    if (strcmp(method, "OPTIONS") == 0) {
        send_response(client_fd, "204 No Content",
                      "text/plain", "", 0);
        return;
    }

    /* API endpoint. */
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api") == 0) {
        const char *body = strstr(buf, "\r\n\r\n");
        if (body) body += 4; else body = "{}";
        const char *resp = route_api(body);
        send_response(client_fd, "200 OK", "application/json",
                      resp, strlen(resp));
        return;
    }

    /* Serve the GUI. */
    if (strcmp(method, "GET") == 0 &&
        (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
        send_response(client_fd, "200 OK", "text/html",
                      kb_gui_html, kb_gui_html_len);
        return;
    }

    send_response(client_fd, "404 Not Found", "text/plain",
                  "Not Found", 9);
}

int main(int argc, char *argv[]) {
    int port = 9090;
    if (argc > 1) port = atoi(argv[1]);

    signal(SIGPIPE, SIG_IGN);
    kb_crc32c_init();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(server_fd); return 1;
    }

    if (listen(server_fd, 16) < 0) {
        perror("listen"); close(server_fd); return 1;
    }

    printf("KB Dev GUI running at http://localhost:%d\n", port);
    printf("Press Ctrl+C to stop.\n\n");

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept"); break;
        }
        handle_request(client_fd);
        close(client_fd);
    }

    if (g_store_open) kb_store_close(&g_store);
    close(server_fd);
    return 0;
}
