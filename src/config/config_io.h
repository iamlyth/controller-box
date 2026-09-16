/*
 * config_io.h — Shared bounded file reads, atomic writes and the per-user
 *               config transaction lock (SPEC §7.1–§7.4).
 *
 * The GUI config layer (settings.yaml, assignments.yaml) must never publish
 * a half-written file, never block on a FIFO placed where a config file is
 * expected, and never let two processes interleave a read-modify-write and
 * erase each other's updates.  These helpers centralise those guarantees so
 * settings and assignments share one implementation.
 */
#ifndef CBX_CONFIG_IO_H
#define CBX_CONFIG_IO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Read a bounded, regular file into a freshly malloc'd NUL-terminated buffer.
 *
 * The file is opened O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC so a symlink
 * target or a FIFO at `path` can never redirect or block the read.  The
 * descriptor is fstat(2)ed and must be a regular file no larger than
 * `max_size`; growth beyond the snapshot size is detected and rejected.
 *
 * On success `*out_buf` points at a NUL-terminated buffer and `*out_len`
 * holds the byte length (excluding the terminator).  The caller owns the
 * buffer (free(3)).
 *
 * @return 0 on success;
 *         -ENOENT if the file does not exist;
 *         -EINVAL if `path` is not a regular file (directory/FIFO/device);
 *         -EFBIG if the file exceeds `max_size` (including growth races);
 *         negative errno on open/read failure.
 */
int cbx_io_read_regular(const char *path, size_t max_size,
                        char **out_buf, size_t *out_len);

/*
 * Atomically replace `path` with `len` bytes from `data`.
 *
 * A uniquely named temp file is created in the same directory (mode 0600),
 * written, fsync(2)ed, close(2)d and rename(2)d over the destination.  The
 * directory is fsync(2)ed after the rename for durability.  Any failure
 * before the rename unlinks the owned temp file and leaves the original in
 * place; write/flush/fsync/close/rename failures are propagated.
 *
 * @return 0 on success; negative errno on failure.
 */
int cbx_io_write_atomic(const char *path, const void *data, size_t len);

/*
 * Acquire the exclusive cross-process lock protecting user config writes
 * (~/.config/controller-box/.controller-box.lock, created 0600).
 *
 * The lock file itself is never renamed, so it is stable across the atomic
 * replace of settings.yaml/assignments.yaml.  Returns a lock descriptor
 * >= 0 on success (pass it to cbx_io_unlock) or a negative errno on failure.
 */
int cbx_io_lock(void);

/* Release a lock descriptor from cbx_io_lock.  Safe on negative values. */
void cbx_io_unlock(int lock_fd);

/*
 * Acquire the exclusive cross-process lock, waiting at most `timeout_ms`
 * milliseconds (0 = fail immediately, < 0 = wait indefinitely).
 *
 * Interactive callers on a latency-critical event loop (the resident overlay
 * service) use a bounded wait so an unrelated writer holding the lock across
 * slow work cannot freeze input dispatch; on timeout they receive -ETIMEDOUT
 * and report the persistence failure rather than blocking.
 *
 * @return 0 or a positive lock descriptor on success; -ETIMEDOUT on timeout;
 *         other negative errno on open/lock failure.
 */
int cbx_io_lock_timeout(int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* CBX_CONFIG_IO_H */
