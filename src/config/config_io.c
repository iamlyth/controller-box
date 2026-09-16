/*
 * config_io.c — Shared bounded file reads, atomic writes and the per-user
 *               config transaction lock (SPEC §7.1–§7.4).
 */
#include "config_io.h"

#include "config_paths.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Temp-file name within the destination directory.  Owned by this module. */
#define CBX_IO_TMP_PREFIX ".cbx-tmp-"

/* Cross-process lock file within the config directory. */
#define CBX_IO_LOCK_NAME ".controller-box.lock"

/* --- Bounded regular-file read ------------------------------------------- */

int cbx_io_read_regular(const char *path, size_t max_size,
                        char **out_buf, size_t *out_len)
{
    if (!path || !out_buf || !out_len || max_size == 0)
        return -EINVAL;

    *out_buf = NULL;
    *out_len = 0;

    /* O_NONBLOCK keeps a FIFO from blocking the open; fstat rejects it.
     * O_NOFOLLOW rejects a symlink at the config path. */
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -errno;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        int e = errno;
        close(fd);
        return -e;
    }

    if (!S_ISREG(st.st_mode)) {
        close(fd);
        return -EINVAL;
    }

    if (st.st_size < 0 || (uintmax_t)st.st_size > (uintmax_t)max_size) {
        close(fd);
        return -EFBIG;
    }

    /* Allocate the snapshot size + 1 so a growth race can be detected with
     * one extra probe byte rather than an unbounded read. */
    size_t cap = (size_t)st.st_size;
    if (cap > max_size)
        cap = max_size;

    char *buf = malloc(cap + 1);
    if (!buf) {
        close(fd);
        return -ENOMEM;
    }

    size_t total = 0;
    while (total < cap) {
        ssize_t n = read(fd, buf + total, cap - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            int e = errno;
            free(buf);
            close(fd);
            return -e;
        }
        if (n == 0)
            break;
        total += (size_t)n;
    }

    if (total == cap) {
        /* Probe for growth past the accepted size. */
        char probe;
        ssize_t n;
        do {
            n = read(fd, &probe, 1);
        } while (n < 0 && errno == EINTR);
        if (n > 0) {
            free(buf);
            close(fd);
            return -EFBIG;
        }
    }

    buf[total] = '\0';
    close(fd);

    *out_buf = buf;
    *out_len = total;
    return 0;
}

/* --- Atomic write -------------------------------------------------------- */

/* Write all bytes or fail.  Returns 0 or negative errno. */
static int write_all(int fd, const void *data, size_t len)
{
    const unsigned char *p = data;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (n == 0)
            return -EIO;
        off += (size_t)n;
    }
    return 0;
}

/* fsync the directory containing `path` so the rename is durable.  Some
 * filesystems do not support directory fsync; EINVAL/ENOTSUP are tolerated,
 * every other failure is propagated. */
static int fsync_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return -errno;

    int rc = 0;
    if (fsync(fd) != 0 && errno != EINVAL && errno != ENOTSUP)
        rc = -errno;
    close(fd);
    return rc;
}

int cbx_io_write_atomic(const char *path, const void *data, size_t len)
{
    if (!path || (!data && len > 0))
        return -EINVAL;

    /* Split path into directory + basename. */
    const char *slash = strrchr(path, '/');
    if (!slash)
        return -EINVAL;

    size_t dir_len = (size_t)(slash - path);
    if (dir_len == 0)
        dir_len = 1; /* root directory "/" */

    char dir[PATH_MAX];
    if (dir_len >= sizeof(dir))
        return -ENAMETOOLONG;
    memcpy(dir, path, dir_len);
    dir[dir_len] = '\0';

    char tmpl[PATH_MAX];
    int n = snprintf(tmpl, sizeof(tmpl), "%s/%sXXXXXX", dir,
                     CBX_IO_TMP_PREFIX);
    if (n < 0 || (size_t)n >= sizeof(tmpl))
        return -ENAMETOOLONG;

    int fd = mkstemp(tmpl);
    if (fd < 0)
        return -errno;

    /* Restrict permissions immediately (mkstemp uses 0600, but be explicit
     * and independent of umask). */
    if (fchmod(fd, 0600) != 0) {
        int e = errno;
        close(fd);
        unlink(tmpl);
        return -e;
    }

    int rc = write_all(fd, data, len);
    if (rc != 0) {
        close(fd);
        unlink(tmpl);
        return rc;
    }

    /* Propagate fsync failure — a write that cannot be made durable must
     * not be reported as persisted. */
    if (fsync(fd) != 0) {
        int e = errno;
        close(fd);
        unlink(tmpl);
        return -e;
    }

    if (close(fd) != 0) {
        int e = errno;
        unlink(tmpl);
        return -e;
    }

    if (rename(tmpl, path) != 0) {
        int e = errno;
        unlink(tmpl);
        return -e;
    }

    return fsync_dir(dir);
}

/* --- Cross-process lock -------------------------------------------------- */

int cbx_io_lock(void)
{
    char dir[PATH_MAX];
    int rc = cbx_config_dir(dir, sizeof(dir));
    if (rc < 0)
        return rc;

    char lock_path[PATH_MAX + sizeof(CBX_IO_LOCK_NAME) + 2];
    int n = snprintf(lock_path, sizeof(lock_path), "%s/%s", dir,
                     CBX_IO_LOCK_NAME);
    if (n < 0 || (size_t)n >= sizeof(lock_path))
        return -ENAMETOOLONG;

    int fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                  0600);
    if (fd < 0)
        return -errno;

    (void)fchmod(fd, 0600);

    while (flock(fd, LOCK_EX) != 0) {
        if (errno == EINTR)
            continue;
        int e = errno;
        close(fd);
        return -e;
    }

    return fd;
}

void cbx_io_unlock(int lock_fd)
{
    if (lock_fd < 0)
        return;
    (void)flock(lock_fd, LOCK_UN);
    close(lock_fd);
}
