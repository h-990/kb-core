/*
 * kb_index.c — B+Tree index implementation (§3).
 *
 * Single-page nodes. Keys are variable-length path_uid bytes.
 * Leaf entries: key_len(u16) + key[key_len] + value(u64).
 * Internal entries: key_len(u16) + key[key_len] + child(u64).
 *
 * Split policy: when a node is full (not enough room for the new
 * entry), split it 50/50 and push the median key up.
 */

#include "core/kb_index.h"
#include "core/kb_crc32c.h"
#include <stdlib.h>
#include <string.h>

/* ══════════════════ Node buffer helpers ══════════════════ */

/* Entry layout: key_len(u16) + key[key_len] + ptr(u64). */
#define ENTRY_OVERHEAD  (2u + 8u)   /* key_len + value/child pointer */

static uint32_t
entry_total_size(uint16_t key_len)
{
    return ENTRY_OVERHEAD + key_len;
}

/* Read an entry at offset in the node buffer.
 * Returns key pointer, key_len, value/child, and the next offset. */
static void
read_entry(const uint8_t *node, uint32_t off,
           const uint8_t **key_out, uint16_t *key_len_out,
           uint64_t *ptr_out, uint32_t *next_off)
{
    uint16_t klen = kb_read_u16(node + off);
    *key_len_out = klen & KB_IDX_KEY_LEN_MASK;
    *key_out     = node + off + 2;
    *ptr_out     = kb_read_u64(node + off + 2 + *key_len_out);
    if (next_off) *next_off = off + entry_total_size(*key_len_out);
}

static void
write_entry(uint8_t *node, uint32_t off,
            const uint8_t *key, uint16_t key_len, uint64_t ptr)
{
    kb_write_u16(node + off, key_len);
    memcpy(node + off + 2, key, key_len);
    kb_write_u64(node + off + 2 + key_len, ptr);
}

/* Key comparison — standard memcmp with length tie-breaking. */
static int
key_cmp(const uint8_t *a, uint16_t alen, const uint8_t *b, uint16_t blen)
{
    uint16_t minlen = alen < blen ? alen : blen;
    int r = memcmp(a, b, minlen);
    if (r != 0) return r;
    if (alen < blen) return -1;
    if (alen > blen) return  1;
    return 0;
}

/* Node accessors. */
static uint32_t node_magic   (const uint8_t *n) { return kb_read_u32(n + 0); }
static uint16_t node_num_keys(const uint8_t *n) { return kb_read_u16(n + 4); }
static uint16_t node_data_end(const uint8_t *n) { return kb_read_u16(n + 6); }
static uint64_t node_next_or_child0(const uint8_t *n) { return kb_read_u64(n + 8); }

static void set_node_magic   (uint8_t *n, uint32_t v) { kb_write_u32(n + 0, v); }
static void set_node_num_keys(uint8_t *n, uint16_t v) { kb_write_u16(n + 4, v); }
static void set_node_data_end(uint8_t *n, uint16_t v) { kb_write_u16(n + 6, v); }
static void set_node_next_or_child0(uint8_t *n, uint64_t v) { kb_write_u64(n + 8, v); }

static bool node_is_leaf(const uint8_t *n) {
    return node_magic(n) == KB_MAGIC_IDX_LEAF;
}

/* Allocate a page for a new node. */
static kb_status_t
alloc_node(kb_index_t *idx, kb_addr_t *out)
{
    return kb_pool_alloc_pages(idx->pool, 1, KB_PAGE_KIND_CHILD_TREE, out);
}

/* Read a full node page. Caller must free. */
static kb_status_t
read_node(kb_index_t *idx, kb_addr_t addr, uint8_t **out)
{
    uint8_t *buf = (uint8_t *)malloc(idx->page_size);
    if (buf == NULL) return KB_E_NO_RAM;
    kb_status_t s = kb_os_pread(idx->pool->pool_fd, buf, idx->page_size, addr);
    if (s != KB_OK) { free(buf); return s; }
    *out = buf;
    return KB_OK;
}

/* Write a full node page. */
static kb_status_t
write_node(kb_index_t *idx, kb_addr_t addr, const uint8_t *buf)
{
    return kb_os_pwrite(idx->pool->pool_fd, buf, idx->page_size, addr);
}

/* Initialize an empty leaf node buffer. */
static void
init_leaf(uint8_t *buf, uint32_t page_size)
{
    memset(buf, 0, page_size);
    set_node_magic(buf, KB_MAGIC_IDX_LEAF);
    set_node_num_keys(buf, 0);
    set_node_data_end(buf, KB_IDX_NODE_HDR_SIZE);
    set_node_next_or_child0(buf, 0); /* no next leaf */
}

/* Initialize an empty internal node buffer. */
static void
init_internal(uint8_t *buf, uint32_t page_size, uint16_t level, uint64_t child0)
{
    memset(buf, 0, page_size);
    set_node_magic(buf, KB_MAGIC_IDX_INTL);
    set_node_num_keys(buf, 0);
    set_node_data_end(buf, KB_IDX_NODE_HDR_SIZE);
    kb_write_u16(buf + 6, level);  /* level field */
    set_node_next_or_child0(buf, child0);
}

/* Available space in a node. */
static uint32_t
node_free_space(const uint8_t *n, uint32_t page_size)
{
    uint16_t data_end = node_data_end(n);
    return page_size - data_end;
}

/* ══════════════════ Leaf search ══════════════════ */

/*
 * Binary search in a leaf. Returns the index of the matching key,
 * or -(insertion_point + 1) if not found.
 * Also returns the byte offset of each entry via linear scan.
 */
static int32_t
leaf_search(const uint8_t *node, const uint8_t *key, uint16_t key_len,
            uint32_t *out_offset)
{
    uint16_t nkeys = node_num_keys(node);
    uint32_t off = KB_IDX_NODE_HDR_SIZE;

    for (uint16_t i = 0; i < nkeys; i++) {
        const uint8_t *ekey;
        uint16_t eklen;
        uint64_t eval;
        uint32_t next;
        read_entry(node, off, &ekey, &eklen, &eval, &next);

        int c = key_cmp(key, key_len, ekey, eklen);
        if (c == 0) {
            if (out_offset) *out_offset = off;
            return (int32_t)i;
        }
        if (c < 0) {
            if (out_offset) *out_offset = off;
            return -((int32_t)i + 1);
        }
        off = next;
    }

    if (out_offset) *out_offset = off;
    return -((int32_t)nkeys + 1);
}

/* ══════════════════ Internal node search ══════════════════ */

/*
 * Find the child pointer for a key in an internal node.
 * Returns the child address to descend into.
 */
static uint64_t
internal_search(const uint8_t *node, const uint8_t *key, uint16_t key_len)
{
    uint16_t nkeys = node_num_keys(node);
    uint64_t child = node_next_or_child0(node); /* child_0 */
    uint32_t off = KB_IDX_NODE_HDR_SIZE;

    for (uint16_t i = 0; i < nkeys; i++) {
        const uint8_t *ekey;
        uint16_t eklen;
        uint64_t echild;
        uint32_t next;
        read_entry(node, off, &ekey, &eklen, &echild, &next);

        if (key_cmp(key, key_len, ekey, eklen) < 0) {
            return child;
        }
        child = echild;
        off = next;
    }
    return child;
}

/* ══════════════════ Split ══════════════════ */

/*
 * Split a leaf into two halves. The right half gets a new page.
 * Returns the separator key (first key of right leaf).
 */
static kb_status_t
split_leaf(kb_index_t *idx, uint8_t *left_buf, kb_addr_t left_addr,
           uint8_t **sep_key, uint16_t *sep_key_len,
           kb_addr_t *right_addr)
{
    uint16_t nkeys = node_num_keys(left_buf);
    uint16_t split_at = nkeys / 2;

    /* Allocate right leaf. */
    kb_addr_t raddr;
    kb_status_t s = alloc_node(idx, &raddr);
    if (s != KB_OK) return s;

    uint8_t *right_buf = (uint8_t *)malloc(idx->page_size);
    if (right_buf == NULL) return KB_E_NO_RAM;

    /* Right leaf's next = left's old next. */
    init_leaf(right_buf, idx->page_size);
    set_node_next_or_child0(right_buf, node_next_or_child0(left_buf));

    /* Copy entries [split_at..nkeys) to right. */
    uint32_t left_off = KB_IDX_NODE_HDR_SIZE;
    uint32_t right_off = KB_IDX_NODE_HDR_SIZE;
    uint16_t right_count = 0;

    for (uint16_t i = 0; i < nkeys; i++) {
        const uint8_t *ekey;
        uint16_t eklen;
        uint64_t eval;
        uint32_t next;
        read_entry(left_buf, left_off, &ekey, &eklen, &eval, &next);

        if (i >= split_at) {
            if (i == split_at) {
                /* Copy separator key. */
                *sep_key_len = eklen;
                *sep_key = (uint8_t *)malloc(eklen);
                if (*sep_key == NULL) { free(right_buf); return KB_E_NO_RAM; }
                memcpy(*sep_key, ekey, eklen);
            }
            write_entry(right_buf, right_off, ekey, eklen, eval);
            right_off += entry_total_size(eklen);
            right_count++;
        }
        left_off = next;
    }

    set_node_num_keys(right_buf, right_count);
    set_node_data_end(right_buf, (uint16_t)right_off);

    /* Truncate left to [0..split_at). */
    left_off = KB_IDX_NODE_HDR_SIZE;
    for (uint16_t i = 0; i < split_at; i++) {
        const uint8_t *ekey;
        uint16_t eklen;
        uint64_t eval;
        uint32_t next;
        read_entry(left_buf, left_off, &ekey, &eklen, &eval, &next);
        left_off = next;
    }
    set_node_num_keys(left_buf, split_at);
    set_node_data_end(left_buf, (uint16_t)left_off);
    set_node_next_or_child0(left_buf, raddr); /* left->next = right */

    /* Write both. */
    s = write_node(idx, left_addr, left_buf);
    if (s != KB_OK) { free(right_buf); return s; }
    s = write_node(idx, raddr, right_buf);
    free(right_buf);
    if (s != KB_OK) return s;

    *right_addr = raddr;
    return KB_OK;
}

/*
 * Split an internal node.
 */
static kb_status_t
split_internal(kb_index_t *idx, uint8_t *left_buf, kb_addr_t left_addr,
               uint16_t level,
               uint8_t **sep_key, uint16_t *sep_key_len,
               kb_addr_t *right_addr)
{
    uint16_t nkeys = node_num_keys(left_buf);
    uint16_t split_at = nkeys / 2;

    kb_addr_t raddr;
    kb_status_t s = alloc_node(idx, &raddr);
    if (s != KB_OK) return s;

    uint8_t *right_buf = (uint8_t *)malloc(idx->page_size);
    if (right_buf == NULL) return KB_E_NO_RAM;

    /* Find the median entry. The median key goes up; entries after
     * median go to the right node. */
    uint32_t left_off = KB_IDX_NODE_HDR_SIZE;
    uint32_t right_off = KB_IDX_NODE_HDR_SIZE;
    uint16_t right_count = 0;
    uint64_t right_child0 = 0;

    for (uint16_t i = 0; i < nkeys; i++) {
        const uint8_t *ekey;
        uint16_t eklen;
        uint64_t echild;
        uint32_t next;
        read_entry(left_buf, left_off, &ekey, &eklen, &echild, &next);

        if (i == split_at) {
            /* This key goes up as the separator. */
            *sep_key_len = eklen;
            *sep_key = (uint8_t *)malloc(eklen);
            if (*sep_key == NULL) { free(right_buf); return KB_E_NO_RAM; }
            memcpy(*sep_key, ekey, eklen);
            right_child0 = echild; /* right node's child_0 */
        } else if (i > split_at) {
            write_entry(right_buf, right_off, ekey, eklen, echild);
            right_off += entry_total_size(eklen);
            right_count++;
        }
        left_off = next;
    }

    init_internal(right_buf, idx->page_size, level, right_child0);
    set_node_num_keys(right_buf, right_count);
    set_node_data_end(right_buf, (uint16_t)right_off);

    /* Truncate left. */
    left_off = KB_IDX_NODE_HDR_SIZE;
    for (uint16_t i = 0; i < split_at; i++) {
        const uint8_t *ekey;
        uint16_t eklen;
        uint64_t echild;
        uint32_t next;
        read_entry(left_buf, left_off, &ekey, &eklen, &echild, &next);
        left_off = next;
    }
    set_node_num_keys(left_buf, split_at);
    set_node_data_end(left_buf, (uint16_t)left_off);

    s = write_node(idx, left_addr, left_buf);
    if (s != KB_OK) { free(right_buf); return s; }
    s = write_node(idx, raddr, right_buf);
    free(right_buf);

    *right_addr = raddr;
    return s;
}

/* ══════════════════ Recursive insert ══════════════════ */

/* Return value for split propagation. */
typedef struct insert_result {
    bool     did_split;
    uint8_t  *sep_key;       /* allocated — caller must free */
    uint16_t sep_key_len;
    kb_addr_t right_addr;
} insert_result_t;

static kb_status_t
insert_recursive(kb_index_t *idx, kb_addr_t node_addr,
                 const uint8_t *key, uint16_t key_len, kb_addr_t value,
                 insert_result_t *result)
{
    uint8_t *buf;
    kb_status_t s = read_node(idx, node_addr, &buf);
    if (s != KB_OK) return s;

    result->did_split = false;
    result->sep_key   = NULL;

    if (node_is_leaf(buf)) {
        /* Leaf insert. */
        uint32_t ins_off;
        int32_t pos = leaf_search(buf, key, key_len, &ins_off);

        if (pos >= 0) {
            /* Key exists. */
            free(buf);
            return KB_E_EXISTS;
        }

        uint32_t needed = entry_total_size(key_len);
        uint32_t avail  = node_free_space(buf, idx->page_size);

        if (needed <= avail) {
            /* Room — insert in place by shifting. */
            uint16_t nkeys   = node_num_keys(buf);
            uint16_t data_end = node_data_end(buf);

            /* Shift everything from ins_off onward right by `needed` bytes. */
            if (ins_off < data_end) {
                memmove(buf + ins_off + needed,
                        buf + ins_off,
                        data_end - ins_off);
            }
            write_entry(buf, ins_off, key, key_len, value);
            set_node_num_keys(buf, (uint16_t)(nkeys + 1));
            set_node_data_end(buf, (uint16_t)(data_end + needed));

            s = write_node(idx, node_addr, buf);
            free(buf);
            if (s != KB_OK) return s;
            idx->entry_count++;
            return KB_OK;
        }

        /* Need to split. First insert into the buffer (may temporarily
         * exceed page — we use a temp allocation). */
        uint16_t nkeys   = node_num_keys(buf);
        uint16_t data_end = node_data_end(buf);
        uint32_t new_size = data_end + needed;

        uint8_t *tmp = (uint8_t *)malloc(new_size + idx->page_size);
        if (tmp == NULL) { free(buf); return KB_E_NO_RAM; }
        memcpy(tmp, buf, data_end);
        /* Insert at ins_off. */
        if (ins_off < data_end) {
            memmove(tmp + ins_off + needed,
                    tmp + ins_off,
                    data_end - ins_off);
        }
        write_entry(tmp, ins_off, key, key_len, value);
        set_node_num_keys(tmp, (uint16_t)(nkeys + 1));
        set_node_data_end(tmp, (uint16_t)new_size);
        /* Preserve next_leaf from original. */
        set_node_next_or_child0(tmp, node_next_or_child0(buf));
        free(buf);

        /* Now split tmp into left (node_addr) and right (new page). */
        /* Rebuild left buf. */
        buf = (uint8_t *)calloc(1, idx->page_size);
        if (buf == NULL) { free(tmp); return KB_E_NO_RAM; }
        memcpy(buf, tmp, idx->page_size < new_size ? idx->page_size : new_size);

        s = split_leaf(idx, buf, node_addr,
                       &result->sep_key, &result->sep_key_len,
                       &result->right_addr);
        free(buf);
        free(tmp);
        if (s != KB_OK) return s;

        result->did_split = true;
        idx->entry_count++;
        return KB_OK;
    }

    /* Internal node — descend. */
    uint64_t child_addr = internal_search(buf, key, key_len);

    insert_result_t child_result;
    s = insert_recursive(idx, child_addr, key, key_len, value, &child_result);
    if (s != KB_OK) { free(buf); return s; }

    if (!child_result.did_split) {
        free(buf);
        return KB_OK;
    }

    /* Child split — insert separator into this internal node. */
    uint32_t needed = entry_total_size(child_result.sep_key_len);
    uint32_t avail  = node_free_space(buf, idx->page_size);

    if (needed <= avail) {
        /* Find insertion point. */
        uint16_t nkeys = node_num_keys(buf);
        uint32_t off = KB_IDX_NODE_HDR_SIZE;
        uint32_t ins_off = KB_IDX_NODE_HDR_SIZE;

        for (uint16_t i = 0; i < nkeys; i++) {
            const uint8_t *ekey;
            uint16_t eklen;
            uint64_t echild;
            uint32_t next;
            read_entry(buf, off, &ekey, &eklen, &echild, &next);
            if (key_cmp(child_result.sep_key, child_result.sep_key_len,
                        ekey, eklen) < 0) {
                break;
            }
            ins_off = next;
            off = next;
        }

        uint16_t data_end = node_data_end(buf);
        if (ins_off < data_end) {
            memmove(buf + ins_off + needed,
                    buf + ins_off,
                    data_end - ins_off);
        }
        write_entry(buf, ins_off, child_result.sep_key,
                    child_result.sep_key_len, child_result.right_addr);
        set_node_num_keys(buf, (uint16_t)(nkeys + 1));
        set_node_data_end(buf, (uint16_t)(data_end + needed));

        s = write_node(idx, node_addr, buf);
        free(buf);
        free(child_result.sep_key);
        return s;
    }

    /* Internal node needs to split too. */
    /* Insert into temp buffer first. */
    uint16_t nkeys = node_num_keys(buf);
    uint16_t data_end = node_data_end(buf);
    uint32_t new_size = data_end + needed;

    uint8_t *tmp = (uint8_t *)malloc(new_size + idx->page_size);
    if (tmp == NULL) { free(buf); free(child_result.sep_key); return KB_E_NO_RAM; }
    memcpy(tmp, buf, data_end);

    /* Find insertion point again. */
    uint32_t off = KB_IDX_NODE_HDR_SIZE;
    uint32_t ins_off = KB_IDX_NODE_HDR_SIZE;
    for (uint16_t i = 0; i < nkeys; i++) {
        const uint8_t *ekey;
        uint16_t eklen;
        uint64_t echild;
        uint32_t next;
        read_entry(tmp, off, &ekey, &eklen, &echild, &next);
        if (key_cmp(child_result.sep_key, child_result.sep_key_len,
                    ekey, eklen) < 0) break;
        ins_off = next;
        off = next;
    }
    if (ins_off < data_end) {
        memmove(tmp + ins_off + needed,
                tmp + ins_off,
                data_end - ins_off);
    }
    write_entry(tmp, ins_off, child_result.sep_key,
                child_result.sep_key_len, child_result.right_addr);
    set_node_num_keys(tmp, (uint16_t)(nkeys + 1));
    set_node_data_end(tmp, (uint16_t)new_size);
    set_node_next_or_child0(tmp, node_next_or_child0(buf));
    free(buf);

    uint16_t level = node_data_end(tmp); /* abuse: read level from offset 6 */
    level = kb_read_u16(tmp + 6); /* level field at offset 6 for INTL */

    buf = (uint8_t *)calloc(1, idx->page_size);
    if (buf == NULL) { free(tmp); free(child_result.sep_key); return KB_E_NO_RAM; }
    memcpy(buf, tmp, idx->page_size < new_size ? idx->page_size : new_size);

    s = split_internal(idx, buf, node_addr, level,
                       &result->sep_key, &result->sep_key_len,
                       &result->right_addr);
    free(buf);
    free(tmp);
    free(child_result.sep_key);
    if (s != KB_OK) return s;

    result->did_split = true;
    return KB_OK;
}

/* ══════════════════ Public API ══════════════════ */

kb_status_t
kb_index_create(kb_pool_t *pool, kb_index_t *idx)
{
    if (pool == NULL || idx == NULL) return KB_E_BAD_ARG;

    idx->pool       = pool;
    idx->page_size  = pool->sb.page_size;
    idx->height     = 0;
    idx->entry_count = 0;

    /* Allocate root leaf. */
    kb_status_t s = alloc_node(idx, &idx->root_addr);
    if (s != KB_OK) return s;

    uint8_t *buf = (uint8_t *)calloc(1, idx->page_size);
    if (buf == NULL) return KB_E_NO_RAM;

    init_leaf(buf, idx->page_size);
    s = write_node(idx, idx->root_addr, buf);
    free(buf);
    return s;
}

kb_status_t
kb_index_open(kb_pool_t *pool, kb_addr_t root_addr, kb_index_t *idx)
{
    if (pool == NULL || idx == NULL) return KB_E_BAD_ARG;

    idx->pool      = pool;
    idx->page_size = pool->sb.page_size;
    idx->root_addr = root_addr;

    /* Read root to determine height. */
    uint8_t *buf;
    kb_status_t s = read_node(idx, root_addr, &buf);
    if (s != KB_OK) return s;

    if (node_is_leaf(buf)) {
        idx->height = 0;
    } else {
        idx->height = kb_read_u16(buf + 6); /* level */
    }

    /* Count entries by walking leaves (for now, simple). */
    idx->entry_count = 0;
    /* We'll skip full counting on open for performance. */

    free(buf);
    return KB_OK;
}

kb_status_t
kb_index_insert(kb_index_t *idx,
                const uint8_t *key, uint16_t key_len,
                kb_addr_t value)
{
    if (idx == NULL || key == NULL) return KB_E_BAD_ARG;

    insert_result_t result;
    kb_status_t s = insert_recursive(idx, idx->root_addr,
                                     key, key_len, value, &result);
    if (s != KB_OK) return s;

    if (result.did_split) {
        /* Root split — create a new root. */
        kb_addr_t old_root = idx->root_addr;
        kb_addr_t new_root;
        s = alloc_node(idx, &new_root);
        if (s != KB_OK) { free(result.sep_key); return s; }

        uint8_t *buf = (uint8_t *)calloc(1, idx->page_size);
        if (buf == NULL) { free(result.sep_key); return KB_E_NO_RAM; }

        idx->height++;
        init_internal(buf, idx->page_size, (uint16_t)idx->height, old_root);
        write_entry(buf, KB_IDX_NODE_HDR_SIZE,
                    result.sep_key, result.sep_key_len, result.right_addr);
        set_node_num_keys(buf, 1);
        set_node_data_end(buf, (uint16_t)(KB_IDX_NODE_HDR_SIZE +
                          entry_total_size(result.sep_key_len)));

        s = write_node(idx, new_root, buf);
        free(buf);
        free(result.sep_key);
        if (s != KB_OK) return s;

        idx->root_addr = new_root;
    }

    return KB_OK;
}

kb_status_t
kb_index_lookup(kb_index_t *idx,
                const uint8_t *key, uint16_t key_len,
                kb_addr_t *out_value)
{
    if (idx == NULL || key == NULL || out_value == NULL) return KB_E_BAD_ARG;

    kb_addr_t addr = idx->root_addr;

    /* Descend to leaf. */
    for (uint32_t level = idx->height; level > 0; level--) {
        uint8_t *buf;
        kb_status_t s = read_node(idx, addr, &buf);
        if (s != KB_OK) return s;

        addr = internal_search(buf, key, key_len);
        free(buf);
    }

    /* Search leaf. */
    uint8_t *buf;
    kb_status_t s = read_node(idx, addr, &buf);
    if (s != KB_OK) return s;

    uint32_t off;
    int32_t pos = leaf_search(buf, key, key_len, &off);
    if (pos < 0) {
        free(buf);
        return KB_E_NOT_FOUND;
    }

    /* Read the value. */
    const uint8_t *ekey;
    uint16_t eklen;
    uint64_t eval;
    read_entry(buf, off, &ekey, &eklen, &eval, NULL);
    *out_value = eval;
    free(buf);
    return KB_OK;
}

kb_status_t
kb_index_delete(kb_index_t *idx,
                const uint8_t *key, uint16_t key_len)
{
    if (idx == NULL || key == NULL) return KB_E_BAD_ARG;

    /* Find the leaf. */
    kb_addr_t addr = idx->root_addr;
    for (uint32_t level = idx->height; level > 0; level--) {
        uint8_t *buf;
        kb_status_t s = read_node(idx, addr, &buf);
        if (s != KB_OK) return s;
        addr = internal_search(buf, key, key_len);
        free(buf);
    }

    /* Search and remove from leaf. */
    uint8_t *buf;
    kb_status_t s = read_node(idx, addr, &buf);
    if (s != KB_OK) return s;

    uint32_t del_off;
    int32_t pos = leaf_search(buf, key, key_len, &del_off);
    if (pos < 0) {
        free(buf);
        return KB_E_NOT_FOUND;
    }

    /* Remove by shifting left. */
    const uint8_t *ekey;
    uint16_t eklen;
    uint64_t eval;
    uint32_t next_off;
    read_entry(buf, del_off, &ekey, &eklen, &eval, &next_off);

    uint16_t data_end = node_data_end(buf);
    uint32_t entry_size = entry_total_size(eklen);

    if (next_off < data_end) {
        memmove(buf + del_off, buf + next_off, data_end - next_off);
    }
    set_node_num_keys(buf, (uint16_t)(node_num_keys(buf) - 1));
    set_node_data_end(buf, (uint16_t)(data_end - entry_size));

    s = write_node(idx, addr, buf);
    free(buf);
    if (s == KB_OK) idx->entry_count--;
    return s;
}

kb_status_t
kb_index_iterate(kb_index_t *idx, kb_index_iter_fn callback, void *ctx)
{
    if (idx == NULL || callback == NULL) return KB_E_BAD_ARG;

    /* Find leftmost leaf. */
    kb_addr_t addr = idx->root_addr;
    for (uint32_t level = idx->height; level > 0; level--) {
        uint8_t *buf;
        kb_status_t s = read_node(idx, addr, &buf);
        if (s != KB_OK) return s;
        addr = node_next_or_child0(buf); /* child_0 = leftmost */
        free(buf);
    }

    /* Walk the leaf chain. */
    while (addr != 0) {
        uint8_t *buf;
        kb_status_t s = read_node(idx, addr, &buf);
        if (s != KB_OK) return s;

        uint16_t nkeys = node_num_keys(buf);
        uint32_t off = KB_IDX_NODE_HDR_SIZE;
        for (uint16_t i = 0; i < nkeys; i++) {
            const uint8_t *ekey;
            uint16_t eklen;
            uint64_t eval;
            uint32_t next;
            read_entry(buf, off, &ekey, &eklen, &eval, &next);
            s = callback(ekey, eklen, eval, ctx);
            if (s != KB_OK) { free(buf); return s; }
            off = next;
        }

        addr = node_next_or_child0(buf); /* next_leaf */
        free(buf);
    }
    return KB_OK;
}

kb_status_t
kb_index_scan_prefix(kb_index_t *idx,
                     const uint8_t *prefix, uint16_t prefix_len,
                     kb_index_iter_fn callback, void *ctx)
{
    if (idx == NULL || prefix == NULL || callback == NULL)
        return KB_E_BAD_ARG;

    /* Find the leaf containing the prefix. */
    kb_addr_t addr = idx->root_addr;
    for (uint32_t level = idx->height; level > 0; level--) {
        uint8_t *buf;
        kb_status_t s = read_node(idx, addr, &buf);
        if (s != KB_OK) return s;
        addr = internal_search(buf, prefix, prefix_len);
        free(buf);
    }

    /* Scan leaves while keys start with the prefix.
     * Property 5 (§1.4.3): subtree is one contiguous key range. */
    while (addr != 0) {
        uint8_t *buf;
        kb_status_t s = read_node(idx, addr, &buf);
        if (s != KB_OK) return s;

        uint16_t nkeys = node_num_keys(buf);
        uint32_t off = KB_IDX_NODE_HDR_SIZE;
        bool found_any = false;

        for (uint16_t i = 0; i < nkeys; i++) {
            const uint8_t *ekey;
            uint16_t eklen;
            uint64_t eval;
            uint32_t next;
            read_entry(buf, off, &ekey, &eklen, &eval, &next);

            /* Check prefix match. */
            if (eklen >= prefix_len &&
                memcmp(ekey, prefix, prefix_len) == 0) {
                found_any = true;
                s = callback(ekey, eklen, eval, ctx);
                if (s != KB_OK) { free(buf); return s; }
            } else if (found_any) {
                /* Past the prefix range — done. */
                free(buf);
                return KB_OK;
            }
            off = next;
        }

        addr = node_next_or_child0(buf);
        free(buf);

        /* If we didn't find any matches in this leaf and we're past,
         * the prefix range is exhausted. */
        if (!found_any && nkeys > 0) break;
    }
    return KB_OK;
}
