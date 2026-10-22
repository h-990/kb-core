/*
 * kb_os_posix.c — POSIX platform layer (macOS + Linux).
 *
 * Implements the kb_os_* API from kb_os.h.
 */

#include "core/kb_os.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/param.h>   /* F_PREALLOCATE */
#endif

/* ══════════════════ Open / create / close ══════════════════ */

kb_status_t
kb_os_open(const char *path, uint32_t flags, int mode, kb_fd_t *out)
{
    if (path == NULL || out == NULL) return KB_E_BAD_ARG;
    *out = KB_FD_INVALID;

    int oflags = 0;
    if (flags & KB_OPEN_RDONLY)  oflags |= O_RDONLY;
    if (flags & KB_OPEN_RDWR)   oflags |= O_RDWR;
    if (flags & KB_OPEN_CREATE)  oflags |= O_CREAT;
    if (flags & KB_OPEN_EXCL)    oflags |= O_EXCL;

#ifdef __APPLE__
    if (flags & KB_OPEN_DIRECT) {
        /* macOS uses fcntl F_NOCACHE after open, not O_DIRECT. */
    }
#else
    #ifdef O_DIRECT
    if (flags & KB_OPEN_DIRECT)  oflags |= O_DIRECT;
    #endif
#endif

    int fd = open(path, oflags, mode);
    if (fd < 0) {
        if (errno == EEXIST)  return KB_E_EXISTS;
        if (errno == ENOENT)  return KB_E_NOT_FOUND;
        return KB_E_IO;
    }

#ifdef __APPLE__
    if (flags & KB_OPEN_DIRECT) {
        /* Best-effort; F_NOCACHE is advisory. */
        (void)fcntl(fd, F_NOCACHE, 1);
    }
#endif

    out->fd = fd;
    return KB_OK;
}

kb_status_t
kb_os_close(kb_fd_t f)
{
    if (!kb_fd_is_valid(f)) return KB_E_BAD_ARG;
    if (close(f.fd) != 0) return KB_E_IO;
    return KB_OK;
}

/* ══════════════════ Positioned I/O ══════════════════ */

kb_status_t
kb_os_pread(kb_fd_t f, void *buf, size_t len, uint64_t off)
{
    if (!kb_fd_is_valid(f) || buf == NULL) return KB_E_BAD_ARG;
    if (len == 0) return KB_OK;

    uint8_t *p = (uint8_t *)buf;
    size_t remaining = len;
    uint64_t pos = off;

    while (remaining > 0) {
        ssize_t n = pread(f.fd, p, remaining, (off_t)pos);
        if (n < 0) {
            if (errno == EINTR) continue;
            return KB_E_IO;
        }
        if (n == 0) return KB_E_IO;  /* unexpected EOF */
        p += n;
        pos += (uint64_t)n;
        remaining -= (size_t)n;
    }
    return KB_OK;
}

kb_status_t
kb_os_pwrite(kb_fd_t f, const void *buf, size_t len, uint64_t off)
{
    if (!kb_fd_is_valid(f) || buf == NULL) return KB_E_BAD_ARG;
    if (len == 0) return KB_OK;

    const uint8_t *p = (const uint8_t *)buf;
    size_t remaining = len;
    uint64_t pos = off;

    while (remaining > 0) {
        ssize_t n = pwrite(f.fd, p, remaining, (off_t)pos);
        if (n < 0) {
            if (errno == EINTR) continue;
            return KB_E_IO;
        }
        if (n == 0) return KB_E_IO;  /* shouldn't happen */
        p += n;
        pos += (uint64_t)n;
        remaining -= (size_t)n;
    }
    return KB_OK;
}

/* ══════════════════ Sync ══════════════════ */

kb_status_t
kb_os_sync(kb_fd_t f)
{
    if (!kb_fd_is_valid(f)) return KB_E_BAD_ARG;

#ifdef __APPLE__
    /*
     * macOS: fcntl(F_FULLFSYNC) forces the drive's write cache to flush.
     * Plain fsync() on macOS only sends the data to the drive's buffer,
     * which is NOT crash-safe with write-back caching enabled (§4.7).
     */
    if (fcntl(f.fd, F_FULLFSYNC) != 0) return KB_E_IO;
#else
    /*
     * Linux: fdatasync is sufficient — we don't need to flush metadata
     * changes like atime, since we only care about data integrity.
     */
    if (fdatasync(f.fd) != 0) return KB_E_IO;
#endif

    return KB_OK;
}

/* ══════════════════ File size ══════════════════ */

kb_status_t
kb_os_file_size(kb_fd_t f, uint64_t *out_size)
{
    if (!kb_fd_is_valid(f) || out_size == NULL) return KB_E_BAD_ARG;
    struct stat st;
    if (fstat(f.fd, &st) != 0) return KB_E_IO;
    *out_size = (uint64_t)st.st_size;
    return KB_OK;
}

/* ══════════════════ Preallocate / truncate ══════════════════ */

kb_status_t
kb_os_preallocate(kb_fd_t f, uint64_t new_size)
{
    if (!kb_fd_is_valid(f)) return KB_E_BAD_ARG;

    uint64_t cur;
    kb_status_t s = kb_os_file_size(f, &cur);
    if (s != KB_OK) return s;
    if (new_size <= cur) return KB_OK;  /* already large enough */

#ifdef __APPLE__
    /*
     * macOS: F_PREALLOCATE reserves contiguous blocks, then ftruncate
     * extends the logical file size (§4.7).
     */
    fstore_t fst = {
        .fst_flags   = F_ALLOCATECONTIG,
        .fst_posmode = F_PEOFPOSMODE,
        .fst_offset  = 0,
        .fst_length  = (off_t)(new_size - cur),
    };
    if (fcntl(f.fd, F_PREALLOCATE, &fst) == -1) {
        /* Fall back to non-contiguous. */
        fst.fst_flags = F_ALLOCATEALL;
        if (fcntl(f.fd, F_PREALLOCATE, &fst) == -1) {
            return KB_E_NO_SPACE;
        }
    }
    if (ftruncate(f.fd, (off_t)new_size) != 0) return KB_E_IO;
#else
    /* Linux: posix_fallocate is always available and extends the file. */
    int rc = posix_fallocate(f.fd, 0, (off_t)new_size);
    if (rc != 0) {
        return (rc == ENOSPC || rc == EDQUOT) ? KB_E_NO_SPACE : KB_E_IO;
    }
#endif

    return KB_OK;
}

kb_status_t
kb_os_truncate(kb_fd_t f, uint64_t new_size)
{
    if (!kb_fd_is_valid(f)) return KB_E_BAD_ARG;
    if (ftruncate(f.fd, (off_t)new_size) != 0) return KB_E_IO;
    return KB_OK;
}

/* ══════════════════ Aligned allocation ══════════════════ */

void *
kb_os_aligned_alloc(size_t alignment, size_t size)
{
    void *ptr = NULL;
    if (posix_memalign(&ptr, alignment, size) != 0) return NULL;
    return ptr;
}

void
kb_os_aligned_free(void *ptr)
{
    free(ptr);  /* POSIX posix_memalign returns free-able pointers */
}

/* ══════════════════ Delete ══════════════════ */

kb_status_t
kb_os_delete(const char *path)
{
    if (path == NULL) return KB_E_BAD_ARG;
    if (unlink(path) != 0) {
        if (errno == ENOENT) return KB_E_NOT_FOUND;
        return KB_E_IO;
    }
    return KB_OK;
}

/* ══════════════════ File exists ══════════════════ */

kb_status_t
kb_os_file_exists(const char *path, bool *out)
{
    if (path == NULL || out == NULL) return KB_E_BAD_ARG;
    struct stat st;
    if (stat(path, &st) == 0) {
        *out = true;
    } else if (errno == ENOENT) {
        *out = false;
    } else {
        return KB_E_IO;
    }
    return KB_OK;
}
