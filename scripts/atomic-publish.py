#!/usr/bin/env python3
"""atomic-publish.py — atomic no-replace publish + fail-closed fsync helper.

Owned primitive used by scripts/visual-capture-driver.sh so the driver never
uses an ordinary overwriting `mv` (which is TOCTOU-prone and would silently
replace an unowned destination that appears after an existence check).

Subcommands:

  publish <src> <dst>
      Atomically rename <src> to <dst> ONLY if <dst> does not already exist
      (as a regular file, symlink, dangling symlink, hardlink, or any other
      directory entry). Uses renameat2(RENAME_NOREPLACE) on Linux; falls back
      to link()+unlink() where the filesystem does not support RENAME_NOREPLACE.
      Both paths must be on the same filesystem (the driver guarantees this by
      writing its owned temps in the same directory as the destination).
      Exit: 0 success; 2 destination already exists (never overwritten);
            3 any other error.

  fsync <path>
      fsync the given file or directory and propagate failure via a nonzero
      exit. This is the driver's fail-closed durability primitive: a fsync
      error is never swallowed with `|| true`, so the driver cannot claim a
      crash/power-durable publish unless every file and directory entry it
      depends on was actually flushed to stable storage.
      Exit: 0 success; 3 on any open/fsync error.
"""

import ctypes
import errno
import os
import sys

AT_FDCWD = -100
RENAME_NOREPLACE = 1


def _renameat2_noreplace(src: str, dst: str):
    """Return 0 on success, 2 if dst exists, or None to fall back to link()."""
    try:
        libc = ctypes.CDLL(None, use_errno=True)
        renameat2 = getattr(libc, "renameat2", None)
    except OSError:
        renameat2 = None
    if renameat2 is None:
        return None
    renameat2.argtypes = [
        ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint,
    ]
    renameat2.restype = ctypes.c_int
    rc = renameat2(AT_FDCWD, os.fsencode(src), AT_FDCWD, os.fsencode(dst),
                   RENAME_NOREPLACE)
    if rc == 0:
        return 0
    err = ctypes.get_errno()
    if err == errno.EEXIST:
        return 2
    # EINVAL/ENOSYS/ENOTSUP/EPERM/other: fall back to the universal link().
    return None


def _publish_link(src: str, dst: str):
    """Atomic no-replace publish via link()+unlink(); 0 ok, 2 exists, 3 error."""
    try:
        os.link(src, dst)
    except FileExistsError:
        return 2
    except OSError as e:
        if e.errno == errno.EEXIST:
            return 2
        print(f"atomic-publish: link {src} -> {dst}: {e}", file=sys.stderr)
        return 3
    # The destination is published; remove the source temp. A failure here is
    # non-fatal (the published entry already exists).
    try:
        os.unlink(src)
    except OSError:
        pass
    return 0


def cmd_publish(src: str, dst: str) -> int:
    if os.path.exists(dst) or os.path.islink(dst):
        # Fast path for a clear pre-existing/unowned destination; the syscall
        # below is still the authoritative gate (it refuses atomically), so a
        # destination that appears after this check is still caught.
        return 2
    rc = _renameat2_noreplace(src, dst)
    if rc is not None:
        return rc
    return _publish_link(src, dst)


def cmd_fsync(path: str) -> int:
    try:
        fd = os.open(path, os.O_RDONLY)
    except OSError as e:
        print(f"atomic-publish: open {path} for fsync: {e}", file=sys.stderr)
        return 3
    try:
        os.fsync(fd)
    except OSError as e:
        print(f"atomic-publish: fsync {path}: {e}", file=sys.stderr)
        return 3
    finally:
        os.close(fd)
    return 0


def main(argv) -> int:
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 3
    sub = argv[1]
    if sub == "publish" and len(argv) == 4:
        return cmd_publish(argv[2], argv[3])
    if sub == "fsync" and len(argv) == 3:
        return cmd_fsync(argv[2])
    print(f"atomic-publish: usage: {argv[0]} publish <src> <dst> | fsync <path>",
          file=sys.stderr)
    return 3


if __name__ == "__main__":
    sys.exit(main(sys.argv))
