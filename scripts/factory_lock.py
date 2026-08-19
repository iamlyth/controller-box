#!/usr/bin/env python3
"""Stable git-common factory lock acquisition and inherited-FD validation."""

from __future__ import annotations

from contextlib import contextmanager
import fcntl
import os
from pathlib import Path
import stat
import subprocess
from typing import Iterator

PRIVATE_DIRECTORY = "controller-box-factory"
LOCK_NAME = "lifecycle.lock"
LEGACY_NAME = ".factory-lock"


class FactoryLockError(RuntimeError):
    pass


def _git(root: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", *args], cwd=root, text=True, capture_output=True,
        env={**os.environ, "GIT_NO_REPLACE_OBJECTS": "1"},
    )
    if result.returncode:
        raise FactoryLockError(f"cannot resolve Git lock state: {' '.join(args)}")
    return result.stdout.strip()


def repository_root(root: Path) -> Path:
    root = root.absolute()
    try:
        opened = root.resolve(strict=True)
    except OSError as exc:
        raise FactoryLockError(f"repository root is unavailable: {exc}") from exc
    if opened != root:
        raise FactoryLockError("repository root must not contain symlinks")
    top = Path(_git(root, "rev-parse", "--path-format=absolute", "--show-toplevel"))
    if top.absolute() != root:
        raise FactoryLockError("factory lock root is not the Git top level")
    return root


def git_common_directory(root: Path) -> Path:
    root = repository_root(root)
    common = Path(_git(root, "rev-parse", "--path-format=absolute", "--git-common-dir")).absolute()
    git_dir = Path(_git(root, "rev-parse", "--path-format=absolute", "--git-dir")).absolute()
    try:
        common_resolved = common.resolve(strict=True)
        git_resolved = git_dir.resolve(strict=True)
    except OSError as exc:
        raise FactoryLockError(f"Git directory is unavailable: {exc}") from exc
    if common_resolved != common or git_resolved != git_dir:
        raise FactoryLockError("Git lock directory must not contain symlinks")
    if common != git_dir:
        raise FactoryLockError("Git worktrees are forbidden for the factory lock")
    info = common.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
        raise FactoryLockError("unsafe Git common directory")
    return common


def _validate_private_directory(info: os.stat_result) -> None:
    if (
        not stat.S_ISDIR(info.st_mode)
        or info.st_uid != os.getuid()
        or info.st_mode & 0o077
    ):
        raise FactoryLockError("unsafe factory private-state directory")


def _validate_lock(info: os.stat_result) -> None:
    if (
        not stat.S_ISREG(info.st_mode)
        or info.st_uid != os.getuid()
        or info.st_nlink != 1
        or info.st_mode & 0o022
    ):
        raise FactoryLockError("unsafe factory lock file")


def prepare_lock_directory(root: Path) -> tuple[int, Path]:
    common = git_common_directory(root)
    flags = os.O_RDONLY | os.O_DIRECTORY | getattr(os, "O_NOFOLLOW", 0)
    common_fd = os.open(common, flags)
    private_fd: int | None = None
    try:
        try:
            named = os.stat(PRIVATE_DIRECTORY, dir_fd=common_fd, follow_symlinks=False)
        except FileNotFoundError:
            os.mkdir(PRIVATE_DIRECTORY, 0o700, dir_fd=common_fd)
            os.fsync(common_fd)
            named = os.stat(PRIVATE_DIRECTORY, dir_fd=common_fd, follow_symlinks=False)
        _validate_private_directory(named)
        private_fd = os.open(PRIVATE_DIRECTORY, flags, dir_fd=common_fd)
        opened = os.fstat(private_fd)
        _validate_private_directory(opened)
        if (opened.st_dev, opened.st_ino) != (named.st_dev, named.st_ino):
            raise FactoryLockError("factory private-state directory was replaced")
        return private_fd, common / PRIVATE_DIRECTORY / LOCK_NAME
    except BaseException:
        if private_fd is not None:
            os.close(private_fd)
        raise
    finally:
        os.close(common_fd)


def validate_open_lock(descriptor: int, root: Path) -> os.stat_result:
    private_fd, canonical = prepare_lock_directory(root)
    try:
        opened = os.fstat(descriptor)
        _validate_lock(opened)
        named = os.stat(LOCK_NAME, dir_fd=private_fd, follow_symlinks=False)
        _validate_lock(named)
        if (opened.st_dev, opened.st_ino) != (named.st_dev, named.st_ino):
            raise FactoryLockError("factory lock path/inode was replaced")
        expected_path = os.environ.get("FACTORY_LOCK_PATH")
        if expected_path and Path(expected_path).absolute() != canonical.absolute():
            raise FactoryLockError("inherited factory lock path changed")
        return opened
    except OSError as exc:
        raise FactoryLockError(f"cannot validate factory lock: {exc}") from exc
    finally:
        os.close(private_fd)


def inherited_descriptor(root: Path) -> int | None:
    if os.environ.get("FACTORY_LOCK_HELD") != "1":
        return None
    raw = os.environ.get("FACTORY_LOCK_FD", "")
    try:
        descriptor = int(raw)
    except ValueError as exc:
        raise FactoryLockError("invalid inherited factory lock descriptor") from exc
    if descriptor < 3:
        raise FactoryLockError("invalid inherited factory lock descriptor")
    opened = validate_open_lock(descriptor, root)
    if os.environ.get("FACTORY_LOCK_ID") != f"{opened.st_dev}:{opened.st_ino}":
        raise FactoryLockError("inherited factory lock identity changed")
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as exc:
        raise FactoryLockError("inherited descriptor does not own the factory lock") from exc
    except OSError as exc:
        raise FactoryLockError(f"cannot verify inherited factory lock: {exc}") from exc
    validate_open_lock(descriptor, root)
    return descriptor


def _open_legacy(root: Path) -> tuple[int, os.stat_result] | None:
    root_fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | getattr(os, "O_NOFOLLOW", 0))
    try:
        try:
            descriptor = os.open(
                LEGACY_NAME,
                os.O_WRONLY | os.O_APPEND | getattr(os, "O_NOFOLLOW", 0),
                dir_fd=root_fd,
            )
        except FileNotFoundError:
            return None
        except OSError as exc:
            raise FactoryLockError(f"cannot safely migrate legacy factory lock: {exc}") from exc
        try:
            opened = os.fstat(descriptor)
            named = os.stat(LEGACY_NAME, dir_fd=root_fd, follow_symlinks=False)
            _validate_lock(opened)
            _validate_lock(named)
            if (opened.st_dev, opened.st_ino) != (named.st_dev, named.st_ino):
                raise FactoryLockError("legacy factory lock was replaced")
            try:
                fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as exc:
                raise FactoryLockError("legacy factory lock is still held") from exc
            return descriptor, opened
        except BaseException:
            os.close(descriptor)
            raise
    finally:
        os.close(root_fd)


def _remove_legacy(root: Path, descriptor: int, expected: os.stat_result) -> None:
    root_fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | getattr(os, "O_NOFOLLOW", 0))
    try:
        opened = os.fstat(descriptor)
        named = os.stat(LEGACY_NAME, dir_fd=root_fd, follow_symlinks=False)
        _validate_lock(opened)
        _validate_lock(named)
        if (
            (opened.st_dev, opened.st_ino) != (expected.st_dev, expected.st_ino)
            or (named.st_dev, named.st_ino) != (expected.st_dev, expected.st_ino)
        ):
            raise FactoryLockError("legacy factory lock changed during migration")
        os.unlink(LEGACY_NAME, dir_fd=root_fd)
        os.fsync(root_fd)
    finally:
        os.close(root_fd)


def acquire(root: Path, *, inheritable: bool) -> int:
    root = repository_root(root)
    legacy = _open_legacy(root)
    private_fd: int | None = None
    descriptor: int | None = None
    try:
        private_fd, canonical = prepare_lock_directory(root)
        descriptor = os.open(
            LOCK_NAME,
            os.O_WRONLY | os.O_CREAT | os.O_APPEND | getattr(os, "O_NOFOLLOW", 0),
            0o600,
            dir_fd=private_fd,
        )
        opened = os.fstat(descriptor)
        named = os.stat(LOCK_NAME, dir_fd=private_fd, follow_symlinks=False)
        _validate_lock(opened)
        _validate_lock(named)
        if (opened.st_dev, opened.st_ino) != (named.st_dev, named.st_ino):
            raise FactoryLockError("factory lock changed while opening")
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise FactoryLockError("another planner, worker, or recovery process is active") from exc
        validate_named = os.stat(LOCK_NAME, dir_fd=private_fd, follow_symlinks=False)
        if (opened.st_dev, opened.st_ino) != (validate_named.st_dev, validate_named.st_ino):
            raise FactoryLockError("factory lock path/inode was replaced")
        if legacy is not None:
            legacy_fd, legacy_info = legacy
            _remove_legacy(root, legacy_fd, legacy_info)
            os.close(legacy_fd)
            legacy = None
        os.set_inheritable(descriptor, inheritable)
        os.environ["FACTORY_LOCK_HELD"] = "1"
        os.environ["FACTORY_LOCK_FD"] = str(descriptor)
        os.environ["FACTORY_LOCK_ID"] = f"{opened.st_dev}:{opened.st_ino}"
        os.environ["FACTORY_LOCK_PATH"] = str(canonical)
        return descriptor
    except OSError as exc:
        if descriptor is not None:
            os.close(descriptor)
        raise FactoryLockError(f"cannot safely acquire factory lock: {exc}") from exc
    except BaseException:
        if descriptor is not None:
            os.close(descriptor)
        raise
    finally:
        if private_fd is not None:
            os.close(private_fd)
        if legacy is not None:
            os.close(legacy[0])


def close_inherited_copies(root: Path) -> None:
    descriptor = inherited_descriptor(root)
    if descriptor is None:
        for key in ("FACTORY_LOCK_HELD", "FACTORY_LOCK_FD", "FACTORY_LOCK_ID", "FACTORY_LOCK_PATH"):
            os.environ.pop(key, None)
        return
    opened = os.fstat(descriptor)
    identity = (opened.st_dev, opened.st_ino)
    candidates: set[int] = {descriptor}
    proc_fds = Path("/proc/self/fd")
    if not proc_fds.is_dir():
        raise FactoryLockError("cannot enumerate inherited descriptors before untrusted exec")
    for item in proc_fds.iterdir():
        try:
            candidate = int(item.name)
            info = os.fstat(candidate)
        except (ValueError, OSError):
            continue
        if candidate >= 3 and (info.st_dev, info.st_ino) == identity:
            candidates.add(candidate)
    for candidate in sorted(candidates, reverse=True):
        try:
            os.close(candidate)
        except OSError:
            pass
    for key in ("FACTORY_LOCK_HELD", "FACTORY_LOCK_FD", "FACTORY_LOCK_ID", "FACTORY_LOCK_PATH"):
        os.environ.pop(key, None)


@contextmanager
def locked(root: Path) -> Iterator[int]:
    descriptor = inherited_descriptor(root)
    close = descriptor is None
    if descriptor is None:
        descriptor = acquire(root, inheritable=False)
    try:
        validate_open_lock(descriptor, root)
        yield descriptor
        validate_open_lock(descriptor, root)
    finally:
        if close:
            os.close(descriptor)
            for key in ("FACTORY_LOCK_HELD", "FACTORY_LOCK_FD", "FACTORY_LOCK_ID", "FACTORY_LOCK_PATH"):
                os.environ.pop(key, None)
