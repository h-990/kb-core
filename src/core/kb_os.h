/*
 * kb_os.h — Platform abstraction layer (§4.7).
 *
 * All OS-specific I/O and sync calls sit behind these functions.
 * The implementation is in kb_os_posix.c (macOS + Linux) and
 * kb_os_win.c (Windows, deferred to V1.1).
 *
 * Key contract:
 *   - kb_os_sync()   guarantees write-through to stable media
 *     (F_FULLFSYNC on macOS, fdatasync on Linux).
 *   - I/O is always positioned (pread/pwrite) — no seek state.
 *   - The pool is never mmap'd writable (§5, pool → explicit I/O).
 */
#ifndef KB_OS_H
#define KB_OS_H

#include "core/kb_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════ File handle ══════════════════ */

/*
 * Opaque file descriptor wrapper. On POSIX this is just an int fd;
 * on Windows it would be a HANDLE. -1 / INVALID_HANDLE_VALUE means closed.
 */
typedef struct kb_fd {
    int fd;
} kb_fd_t;

#define KB_FD_INVALID  ((kb_fd_t){ -1 })

static inline bool kb_fd_is_valid(kb_fd_t f) { return f.fd >= 0; }

/* ══════════════════ Open / create / close ══════════════════ */

/*
 * Open flags — combined with bitwise OR.
 */
#define KB_OPEN_RDONLY   (1u << 0)
#define KB_OPEN_RDWR     (1u << 1)
#define KB_OPEN_CREATE   (1u << 2)   /* create if absent   */
#define KB_OPEN_EXCL     (1u << 3)   /* fail if exists     */
#define KB_OPEN_DIRECT   (1u << 4)   /* try uncached I/O (§4.7 io_mode=direct) */

/*
 * Open or create a file. mode is a POSIX permission mode (ignored on Windows).
 * Returns KB_OK and sets *out, or an error code (KB_E_IO, KB_E_EXISTS, …).
 */
kb_status_t kb_os_open(const char *path, uint32_t flags, int mode, kb_fd_t *out);

/*
 * Close a file. Errors are returned but cannot be meaningfully retried.
 */
kb_status_t kb_os_close(kb_fd_t f);

/* ══════════════════ Positioned I/O ══════════════════ */

/*
 * Read exactly `len` bytes from file offset `off` into `buf`.
 * Short reads are retried; a short final read returns KB_E_IO.
 * Thread-safe (pread does not modify the file descriptor offset).
 */
kb_status_t kb_os_pread(kb_fd_t f, void *buf, size_t len, uint64_t off);

/*
 * Write exactly `len` bytes from `buf` to file offset `off`.
 * Short writes are retried. Thread-safe (pwrite).
 */
kb_status_t kb_os_pwrite(kb_fd_t f, const void *buf, size_t len, uint64_t off);

/* ══════════════════ Sync ══════════════════ */

/*
 * Force all pending writes to stable storage (§4.7).
 *   macOS:  fcntl(F_FULLFSYNC) — plain fsync does NOT force drive cache.
 *   Linux:  fdatasync.
 */
kb_status_t kb_os_sync(kb_fd_t f);

/* ══════════════════ File size ══════════════════ */

/*
 * Get the current file size in bytes.
 */
kb_status_t kb_os_file_size(kb_fd_t f, uint64_t *out_size);

/* ══════════════════ Preallocate / truncate (§2.4.6) ══════════════════ */

/*
 * Extend the file to at least `new_size` bytes, preallocating disk blocks.
 * The file must not shrink; if new_size ≤ current size this is a no-op.
 *   macOS:  F_PREALLOCATE + ftruncate.
 *   Linux:  fallocate(0, 0, new_size) or posix_fallocate.
 */
kb_status_t kb_os_preallocate(kb_fd_t f, uint64_t new_size);

/*
 * Truncate the file to exactly `new_size` bytes (§2.4.6 shrink).
 */
kb_status_t kb_os_truncate(kb_fd_t f, uint64_t new_size);

/* ══════════════════ Aligned allocation ══════════════════ */

/*
 * Allocate `size` bytes aligned to `alignment` (must be a power of 2).
 * Required for O_DIRECT / F_NOCACHE buffers.
 */
void *kb_os_aligned_alloc(size_t alignment, size_t size);

/*
 * Free a buffer from kb_os_aligned_alloc. NULL is safe.
 */
void kb_os_aligned_free(void *ptr);

/* ══════════════════ Delete ══════════════════ */

/*
 * Delete a file by path.
 */
kb_status_t kb_os_delete(const char *path);

/* ══════════════════ File exists ══════════════════ */

/*
 * Check if a file exists. Sets *out to true if it does.
 */
kb_status_t kb_os_file_exists(const char *path, bool *out);

#ifdef __cplusplus
}
#endif

#endif /* KB_OS_H */
