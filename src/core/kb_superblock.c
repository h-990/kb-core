/*
 * kb_superblock.c — Superblock serialization and dual-copy logic (§2.2).
 */

#include "core/kb_superblock.h"
#include "core/kb_os.h"
#include <string.h>
#include <time.h>

/* ══════════════════ Serialize ══════════════════ */

kb_status_t
kb_sb_serialize(const kb_superblock_t *sb, uint8_t *page_buf)
{
    if (sb == NULL || page_buf == NULL) return KB_E_BAD_ARG;
    if (sb->page_size < KB_SB_PAYLOAD_SIZE + 4) return KB_E_BAD_ARG;

    /* Zero the entire page. */
    memset(page_buf, 0, sb->page_size);

    /* Write fields using LE accessors. */
    kb_write_u32(page_buf + KB_SB_OFF_MAGIC,          sb->magic);
    kb_write_u16(page_buf + KB_SB_OFF_FORMAT_REV,     sb->format_rev);
    kb_write_u16(page_buf + KB_SB_OFF_PAGE_SIZE,      sb->page_size);
    kb_write_u16(page_buf + KB_SB_OFF_UNIT_SIZE,      sb->unit_size);
    kb_write_u8 (page_buf + KB_SB_OFF_UREF_BITS,      sb->uref_bits);
    kb_write_u32(page_buf + KB_SB_OFF_SEGMENT_SIZE_MB, sb->segment_size_mb);
    memcpy(page_buf + KB_SB_OFF_POOL_UUID, sb->pool_uuid, 16);
    kb_write_u64(page_buf + KB_SB_OFF_CREATED_UNIX,    sb->created_unix);
    kb_write_u64(page_buf + KB_SB_OFF_SEQ,             sb->seq);
    kb_write_u8 (page_buf + KB_SB_OFF_CLEAN_SHUTDOWN,  sb->clean_shutdown);
    kb_write_u40(page_buf + KB_SB_OFF_ROOT_REF,        sb->root_ref);
    kb_write_u64(page_buf + KB_SB_OFF_PROVISIONED,     sb->provisioned_bytes);
    kb_write_u32(page_buf + KB_SB_OFF_SEGMENT_COUNT,   sb->segment_count);
    kb_write_u64(page_buf + KB_SB_OFF_HWM_UNIT,        sb->hwm_unit);
    kb_write_u64(page_buf + KB_SB_OFF_OBJECT_COUNT,    sb->object_count);
    kb_write_u64(page_buf + KB_SB_OFF_WAL_CKPT_LSN,   sb->wal_checkpoint_lsn);
    kb_write_u32(page_buf + KB_SB_OFF_SLAB_LADDER_REV, sb->slab_ladder_rev);
    kb_write_u32(page_buf + KB_SB_OFF_FEATURE_FLAGS,   sb->feature_flags);
    kb_write_u32(page_buf + KB_SB_OFF_GROW_STEP_MB,    sb->grow_step_mb);

    /* Zero the CRC field, compute CRC over the first 252 bytes. */
    kb_write_u32(page_buf + KB_SB_OFF_CRC32C, 0);
    uint32_t crc = kb_crc32c_buf(page_buf, KB_SB_PAYLOAD_SIZE);
    kb_write_u32(page_buf + KB_SB_OFF_CRC32C, crc);

    return KB_OK;
}

/* ══════════════════ Deserialize ══════════════════ */

kb_status_t
kb_sb_deserialize(const uint8_t *page_buf, kb_superblock_t *sb)
{
    if (page_buf == NULL || sb == NULL) return KB_E_BAD_ARG;

    /* Check magic first. */
    uint32_t magic = kb_read_u32(page_buf + KB_SB_OFF_MAGIC);
    if (magic != KB_MAGIC_POOL) return KB_E_CORRUPT;

    /* Validate CRC: zero the crc field in a temp copy. */
    uint8_t tmp[KB_SB_PAYLOAD_SIZE];
    memcpy(tmp, page_buf, KB_SB_PAYLOAD_SIZE);
    kb_write_u32(tmp + KB_SB_OFF_CRC32C, 0);
    uint32_t computed = kb_crc32c_buf(tmp, KB_SB_PAYLOAD_SIZE);
    uint32_t stored = kb_read_u32(page_buf + KB_SB_OFF_CRC32C);
    if (computed != stored) return KB_E_CORRUPT;

    /* Parse all fields. */
    sb->magic             = magic;
    sb->format_rev        = kb_read_u16(page_buf + KB_SB_OFF_FORMAT_REV);
    sb->page_size         = kb_read_u16(page_buf + KB_SB_OFF_PAGE_SIZE);
    sb->unit_size         = kb_read_u16(page_buf + KB_SB_OFF_UNIT_SIZE);
    sb->uref_bits         = kb_read_u8 (page_buf + KB_SB_OFF_UREF_BITS);
    sb->segment_size_mb   = kb_read_u32(page_buf + KB_SB_OFF_SEGMENT_SIZE_MB);
    memcpy(sb->pool_uuid, page_buf + KB_SB_OFF_POOL_UUID, 16);
    sb->created_unix      = kb_read_u64(page_buf + KB_SB_OFF_CREATED_UNIX);
    sb->seq               = kb_read_u64(page_buf + KB_SB_OFF_SEQ);
    sb->clean_shutdown    = kb_read_u8 (page_buf + KB_SB_OFF_CLEAN_SHUTDOWN);
    sb->root_ref          = kb_read_u40(page_buf + KB_SB_OFF_ROOT_REF);
    sb->provisioned_bytes = kb_read_u64(page_buf + KB_SB_OFF_PROVISIONED);
    sb->segment_count     = kb_read_u32(page_buf + KB_SB_OFF_SEGMENT_COUNT);
    sb->hwm_unit          = kb_read_u64(page_buf + KB_SB_OFF_HWM_UNIT);
    sb->object_count      = kb_read_u64(page_buf + KB_SB_OFF_OBJECT_COUNT);
    sb->wal_checkpoint_lsn= kb_read_u64(page_buf + KB_SB_OFF_WAL_CKPT_LSN);
    sb->slab_ladder_rev   = kb_read_u32(page_buf + KB_SB_OFF_SLAB_LADDER_REV);
    sb->feature_flags     = kb_read_u32(page_buf + KB_SB_OFF_FEATURE_FLAGS);
    sb->grow_step_mb      = kb_read_u32(page_buf + KB_SB_OFF_GROW_STEP_MB);
    sb->crc32c            = stored;

    return KB_OK;
}

/* ══════════════════ Dual-copy load (§2.2) ══════════════════ */

kb_status_t
kb_sb_load_best(kb_fd_t pool_fd, uint32_t page_size, kb_superblock_t *out)
{
    if (out == NULL || page_size < KB_SB_PAYLOAD_SIZE + 4) return KB_E_BAD_ARG;

    uint8_t buf[KB_PAGE_SIZE_LARGE]; /* max page size we support */
    if (page_size > sizeof(buf)) return KB_E_BAD_ARG;

    kb_superblock_t sb0, sb1;
    bool valid0 = false, valid1 = false;

    /* Copy 0 at offset 0. */
    if (kb_os_pread(pool_fd, buf, page_size, 0) == KB_OK) {
        if (kb_sb_deserialize(buf, &sb0) == KB_OK) {
            valid0 = true;
        }
    }

    /* Copy 1 at offset page_size. */
    if (kb_os_pread(pool_fd, buf, page_size, page_size) == KB_OK) {
        if (kb_sb_deserialize(buf, &sb1) == KB_OK) {
            valid1 = true;
        }
    }

    if (!valid0 && !valid1) return KB_E_CORRUPT;

    if (valid0 && valid1) {
        /* Pick the one with the higher seq. */
        *out = (sb0.seq >= sb1.seq) ? sb0 : sb1;
    } else if (valid0) {
        *out = sb0;
    } else {
        *out = sb1;
    }
    return KB_OK;
}

/* ══════════════════ Flush (§2.2) ══════════════════ */

kb_status_t
kb_sb_flush(kb_fd_t pool_fd, kb_superblock_t *sb)
{
    if (sb == NULL) return KB_E_BAD_ARG;

    uint8_t buf[KB_PAGE_SIZE_LARGE];
    if (sb->page_size > sizeof(buf)) return KB_E_BAD_ARG;

    /*
     * Load both copies to find which has the lower seq.
     * The lower-seq copy is the one we overwrite.
     */
    kb_superblock_t sb0, sb1;
    bool valid0 = false, valid1 = false;
    uint32_t ps = sb->page_size;

    if (kb_os_pread(pool_fd, buf, ps, 0) == KB_OK) {
        if (kb_sb_deserialize(buf, &sb0) == KB_OK) valid0 = true;
    }
    if (kb_os_pread(pool_fd, buf, ps, ps) == KB_OK) {
        if (kb_sb_deserialize(buf, &sb1) == KB_OK) valid1 = true;
    }

    /* Determine which copy to overwrite. */
    uint64_t offset;
    if (!valid0 && !valid1) {
        /* Fresh pool: write copy 0 first. */
        offset = 0;
    } else if (!valid0) {
        offset = 0; /* copy 0 is invalid → overwrite it */
    } else if (!valid1) {
        offset = ps; /* copy 1 is invalid → overwrite it */
    } else {
        /* Both valid: overwrite the one with the lower seq. */
        offset = (sb0.seq <= sb1.seq) ? 0 : ps;
    }

    /* Increment seq and serialize. */
    sb->seq++;
    kb_status_t s = kb_sb_serialize(sb, buf);
    if (s != KB_OK) return s;

    /* Write and sync. */
    s = kb_os_pwrite(pool_fd, buf, ps, offset);
    if (s != KB_OK) return s;

    return kb_os_sync(pool_fd);
}

/* ══════════════════ Initialization ══════════════════ */

void
kb_sb_init_new(kb_superblock_t *sb, const uint8_t uuid[16],
               uint16_t page_size)
{
    memset(sb, 0, sizeof(*sb));
    sb->magic             = KB_MAGIC_POOL;
    sb->format_rev        = KB_FORMAT_REV;
    sb->page_size         = page_size;
    sb->unit_size         = (uint16_t)KB_UNIT_SIZE;
    sb->uref_bits         = (uint8_t)KB_UREF_BITS;
    sb->segment_size_mb   = (uint32_t)(KB_SEGMENT_SIZE >> 20);
    memcpy(sb->pool_uuid, uuid, 16);
    sb->created_unix      = (uint64_t)time(NULL);
    sb->seq               = 0;
    sb->clean_shutdown    = 0;
    sb->root_ref          = 0;  /* set after root header allocation */
    sb->provisioned_bytes = 0;  /* set after initial provisioning   */
    sb->segment_count     = 0;
    sb->hwm_unit          = 0;
    sb->object_count      = 0;
    sb->wal_checkpoint_lsn= 0;
    sb->slab_ladder_rev   = 1;
    sb->feature_flags     = 0;
    sb->grow_step_mb      = KB_GROW_STEP_MB_DEFAULT;
}
