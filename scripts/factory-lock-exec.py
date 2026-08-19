#!/usr/bin/env python3
"""Safely acquire the repository factory lock and exec one lifecycle command."""

from __future__ import annotations

import fcntl
import os
from pathlib import Path
import stat
import sys


def fail(message: str) -> None:
    raise SystemExit(f"factory-lock: {message}")


def validate_open_lock(fd: int, path: Path) -> os.stat_result:
    try:
        opened = os.fstat(fd)
        named = path.lstat()
    except OSError as exc:
        fail(f"cannot validate lock: {exc}")
    if (
        not stat.S_ISREG(opened.st_mode)
        or not stat.S_ISREG(named.st_mode)
        or stat.S_ISLNK(named.st_mode)
        or opened.st_uid != os.getuid()
        or named.st_uid != os.getuid()
        or opened.st_nlink != 1
        or named.st_nlink != 1
        or (opened.st_dev, opened.st_ino) != (named.st_dev, named.st_ino)
        or opened.st_mode & 0o022
    ):
        fail("unsafe lock file")
    return opened


def check_inherited(path: Path) -> bool:
    if os.environ.get("FACTORY_LOCK_HELD") != "1":
        return False
    raw_fd = os.environ.get("FACTORY_LOCK_FD", "")
    try:
        fd = int(raw_fd)
    except ValueError:
        fail("invalid inherited lock descriptor")
    if fd < 3:
        fail("invalid inherited lock descriptor")
    opened = validate_open_lock(fd, path)
    expected = os.environ.get("FACTORY_LOCK_ID")
    if expected != f"{opened.st_dev}:{opened.st_ino}":
        fail("inherited lock identity changed")
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        fail("inherited descriptor does not own the factory lock")
    except OSError as exc:
        fail(f"cannot verify inherited lock: {exc}")
    validate_open_lock(fd, path)
    return True


def acquire(path: Path) -> int:
    try:
        parent = path.parent.resolve(strict=True)
    except OSError as exc:
        fail(f"lock parent is unavailable: {exc}")
    if parent != path.parent.absolute():
        fail("lock parent must not contain symlinks")
    flags = os.O_WRONLY | os.O_CREAT | os.O_APPEND | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags, 0o600)
    except OSError as exc:
        fail(f"cannot safely open lock: {exc}")
    try:
        opened = validate_open_lock(fd, path)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            fail("another planner, worker, or recovery process is active")
        except OSError as exc:
            fail(f"cannot acquire lock: {exc}")
        opened = validate_open_lock(fd, path)
        os.set_inheritable(fd, True)
        target = 9
        if fd != target:
            os.dup2(fd, target, inheritable=True)
            os.close(fd)
            fd = target
        os.environ["FACTORY_LOCK_HELD"] = "1"
        os.environ["FACTORY_LOCK_FD"] = str(fd)
        os.environ["FACTORY_LOCK_ID"] = f"{opened.st_dev}:{opened.st_ino}"
        return fd
    except BaseException:
        try:
            os.close(fd)
        except OSError:
            pass
        raise


def main() -> None:
    if len(sys.argv) < 3 or sys.argv[2] != "--":
        fail("usage: factory-lock-exec.py LOCK -- COMMAND [ARG ...]")
    path = Path(sys.argv[1])
    if not path.is_absolute():
        fail("lock path must be absolute")
    command = sys.argv[3:]
    if not command:
        fail("missing command")
    if not check_inherited(path):
        acquire(path)
    try:
        os.execvpe(command[0], command, os.environ)
    except OSError as exc:
        fail(f"cannot execute lifecycle command: {exc}")


if __name__ == "__main__":
    main()
