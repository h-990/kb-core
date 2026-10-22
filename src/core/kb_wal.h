/*
 * kb_wal.h — Write-Ahead Log (§4.1–§4.5).
 *
 * The WAL provides crash-safe atomic transactions. All mutations to
 * pool.db are first written to the WAL file (wal.db) as log records,
 * then applied to pool.db on commit.
 *
 * WAL record layout (variable-length):
 *
 *  Off │ Size │ Field
 *  ────┼──────┼──────────
 *    0 │  4   │ magic     (0x4C415742 = "BWAL" LE)
 *    4 │  4   │ tx_seq    (transaction sequence number)
 *    8 │  4   │ rec_type  (WRITE=1, COMMIT=2, CHECKPOINT=3)
 *   12 │  8   │ target_offset  (byte offset in pool.db)
 *   20 │  4   │ data_len       (payload length in bytes)
 *   24 │  4   │ crc32c         (CRC over entire record)
 *   28 │  N   │ data[]         (payload bytes, only for WRITE records)
 *
 * WAL file header (first 64 bytes of wal.db):
 *
 *  Off │ Size │ Field
 *  ────┼──────┼──────────
 *    0 │  4   │ magic     (0x48444C57 = "WLDH" LE)
 *    4 │  4   │ version   (1)
 *    8 │  4   │ page_size
 *   12 │  8   │ write_offset  (next write position)
 *   20 │  4   │ last_committed_tx
 *   24 │  4   │ crc32c
 *   28 │ 36   │ _reserved
 *
 * Recovery (§4.6):
 *   On open, scan the WAL and replay all committed transactions
 *   that haven't been checkpointed yet. Uncommitted records are
 *   discarded (torn-write safe via CRC).
 */
#ifndef KB_WAL_H
#define KB_WAL_H

#include "core/kb_types.h"
#include "core/kb_os.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════ Constants ══════════════════ */

#define KB_MAGIC_WAL_HDR     0x48444C57u  /* "WLDH" LE */
#define KB_MAGIC_WAL_REC     0x4C415742u  /* "BWAL" LE */

#define KB_WAL_VERSION       1u

#define KB_WAL_HDR_SIZE      64u
#define KB_WAL_REC_HDR_SIZE  28u

/* Record types. */
#define KB_WAL_REC_WRITE      1u
#define KB_WAL_REC_COMMIT     2u
#define KB_WAL_REC_CHECKPOINT 3u

/* WAL header field offsets. */
#define KB_WAL_HDR_OFF_MAGIC          0
#define KB_WAL_HDR_OFF_VERSION        4
#define KB_WAL_HDR_OFF_PAGE_SIZE      8
#define KB_WAL_HDR_OFF_WRITE_OFF     12
#define KB_WAL_HDR_OFF_LAST_TX       20
#define KB_WAL_HDR_OFF_CRC32C        24

/* Record header field offsets. */
#define KB_WAL_REC_OFF_MAGIC          0
#define KB_WAL_REC_OFF_TX_SEQ         4
#define KB_WAL_REC_OFF_TYPE           8
#define KB_WAL_REC_OFF_TARGET        12
#define KB_WAL_REC_OFF_DATA_LEN      20
#define KB_WAL_REC_OFF_CRC32C        24

/* ══════════════════ WAL state ══════════════════ */

typedef struct kb_wal {
    kb_fd_t   wal_fd;            /* file descriptor for wal.db           */
    kb_fd_t   pool_fd;           /* file descriptor for pool.db          */
    uint32_t  page_size;
    uint64_t  write_offset;      /* next write position in wal.db        */
    uint32_t  last_committed_tx; /* highest committed tx_seq             */
    uint32_t  current_tx;        /* tx_seq of the active transaction     */
    bool      tx_active;         /* true if a transaction is in progress */
} kb_wal_t;

/* ══════════════════ Lifecycle ══════════════════ */

/*
 * Create a new WAL file alongside the pool.
 * `wal_path`: path to wal.db.
 */
kb_status_t kb_wal_create(const char *wal_path, uint32_t page_size,
                          kb_fd_t pool_fd, kb_wal_t *wal);

/*
 * Open an existing WAL file. Runs recovery if needed.
 */
kb_status_t kb_wal_open(const char *wal_path, kb_fd_t pool_fd,
                        kb_wal_t *wal);

/*
 * Close the WAL, flushing any pending state.
 */
kb_status_t kb_wal_close(kb_wal_t *wal);

/* ══════════════════ Transaction API ══════════════════ */

/*
 * Begin a new transaction. Returns KB_E_BUSY if one is already active.
 */
kb_status_t kb_wal_begin(kb_wal_t *wal);

/*
 * Log a write operation. The data will be written to the WAL
 * and applied to pool.db on commit.
 * `pool_offset`: byte offset in pool.db where data should be written.
 * `data`: the bytes to write.
 * `len`: number of bytes.
 */
kb_status_t kb_wal_log_write(kb_wal_t *wal, uint64_t pool_offset,
                             const void *data, uint32_t len);

/*
 * Commit the current transaction. Writes a COMMIT record and
 * applies all WRITE records to pool.db, then syncs.
 */
kb_status_t kb_wal_commit(kb_wal_t *wal);

/*
 * Abort the current transaction. Discards any logged writes.
 */
kb_status_t kb_wal_abort(kb_wal_t *wal);

/*
 * Checkpoint: truncate the WAL after all committed transactions
 * have been applied. Resets write_offset to the header.
 */
kb_status_t kb_wal_checkpoint(kb_wal_t *wal);

/* ══════════════════ Recovery ══════════════════ */

/*
 * Replay committed transactions from the WAL to pool.db.
 * Called during kb_wal_open if the WAL has unfinished business.
 */
kb_status_t kb_wal_recover(kb_wal_t *wal);

#ifdef __cplusplus
}
#endif

#endif /* KB_WAL_H */
