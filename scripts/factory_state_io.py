#!/usr/bin/env python3
"""Dirfd-bound, no-follow I/O for private .factory-state lifecycle files."""

from __future__ import annotations

from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
import secrets
import stat
from typing import Iterator

SAFE_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")


class StateIOError(RuntimeError):
    pass


def _name(name: str) -> str:
    if not SAFE_NAME.fullmatch(name) or name in {".", ".."}:
        raise StateIOError(f"unsafe lifecycle marker name: {name!r}")
    return name


def _validate_directory(info: os.stat_result) -> None:
    if (
        not stat.S_ISDIR(info.st_mode)
        or info.st_uid != os.getuid()
        or info.st_mode & 0o077
    ):
        raise StateIOError(".factory-state must be a private owned directory")


def _validate_file(info: os.stat_result, *, maximum: int) -> None:
    if (
        not stat.S_ISREG(info.st_mode)
        or info.st_uid != os.getuid()
        or info.st_nlink != 1
        or info.st_mode & 0o022
        or info.st_size > maximum
    ):
        raise StateIOError("unsafe lifecycle marker")


@contextmanager
def state_dir(root: Path, *, create: bool = False) -> Iterator[int]:
    root = root.absolute()
    flags = os.O_RDONLY | os.O_DIRECTORY | getattr(os, "O_NOFOLLOW", 0)
    root_fd = os.open(root, flags)
    directory_fd: int | None = None
    try:
        try:
            info = os.stat(".factory-state", dir_fd=root_fd, follow_symlinks=False)
        except FileNotFoundError:
            if not create:
                raise StateIOError(".factory-state is missing")
            os.mkdir(".factory-state", 0o700, dir_fd=root_fd)
            os.fsync(root_fd)
            info = os.stat(".factory-state", dir_fd=root_fd, follow_symlinks=False)
        _validate_directory(info)
        directory_fd = os.open(".factory-state", flags, dir_fd=root_fd)
        opened = os.fstat(directory_fd)
        _validate_directory(opened)
        if (opened.st_dev, opened.st_ino) != (info.st_dev, info.st_ino):
            raise StateIOError(".factory-state changed while being opened")
        yield directory_fd
    finally:
        if directory_fd is not None:
            os.close(directory_fd)
        os.close(root_fd)


def read_bytes(
    root: Path,
    name: str,
    *,
    maximum: int = 1024 * 1024,
    missing_ok: bool = False,
) -> bytes | None:
    name = _name(name)
    with state_dir(root) as directory_fd:
        try:
            descriptor = os.open(
                name,
                os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0),
                dir_fd=directory_fd,
            )
        except FileNotFoundError:
            if missing_ok:
                return None
            raise StateIOError(f"lifecycle marker is missing: {name}")
        except OSError as exc:
            raise StateIOError(f"cannot safely open lifecycle marker {name}: {exc}") from exc
        try:
            before = os.fstat(descriptor)
            _validate_file(before, maximum=maximum)
            named = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
            if (before.st_dev, before.st_ino) != (named.st_dev, named.st_ino):
                raise StateIOError(f"lifecycle marker changed while opening: {name}")
            chunks: list[bytes] = []
            remaining = maximum + 1
            while remaining:
                chunk = os.read(descriptor, min(65536, remaining))
                if not chunk:
                    break
                chunks.append(chunk)
                remaining -= len(chunk)
            data = b"".join(chunks)
            after = os.fstat(descriptor)
            named_after = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
            if (
                len(data) > maximum
                or (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns)
                != (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns)
                or (after.st_dev, after.st_ino) != (named_after.st_dev, named_after.st_ino)
            ):
                raise StateIOError(f"lifecycle marker changed while reading: {name}")
            return data
        finally:
            os.close(descriptor)


def read_text(
    root: Path,
    name: str,
    *,
    maximum: int = 1024 * 1024,
    missing_ok: bool = False,
) -> str | None:
    raw = read_bytes(root, name, maximum=maximum, missing_ok=missing_ok)
    if raw is None:
        return None
    try:
        return raw.decode("utf-8")
    except UnicodeError as exc:
        raise StateIOError(f"lifecycle marker is not UTF-8: {name}") from exc


def read_json(
    root: Path,
    name: str,
    *,
    maximum: int = 1024 * 1024,
    missing_ok: bool = False,
) -> object | None:
    text = read_text(root, name, maximum=maximum, missing_ok=missing_ok)
    if text is None:
        return None
    try:
        return json.loads(text)
    except json.JSONDecodeError as exc:
        raise StateIOError(f"invalid JSON lifecycle marker {name}: {exc}") from exc


def atomic_write(root: Path, name: str, data: bytes, *, create_directory: bool = True) -> None:
    name = _name(name)
    if len(data) > 1024 * 1024:
        raise StateIOError("lifecycle marker exceeds 1 MiB")
    with state_dir(root, create=create_directory) as directory_fd:
        try:
            existing = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
        except FileNotFoundError:
            existing = None
        if existing is not None:
            _validate_file(existing, maximum=1024 * 1024)
        temporary = f".{name}.{secrets.token_hex(16)}"
        descriptor = os.open(
            temporary,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0),
            0o600,
            dir_fd=directory_fd,
        )
        renamed = False
        try:
            view = memoryview(data)
            while view:
                written = os.write(descriptor, view)
                if written <= 0:
                    raise StateIOError("short lifecycle marker write")
                view = view[written:]
            os.fsync(descriptor)
            os.close(descriptor)
            descriptor = -1
            if existing is not None:
                current = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
                if (current.st_dev, current.st_ino) != (existing.st_dev, existing.st_ino):
                    raise StateIOError(f"lifecycle marker replaced before update: {name}")
            os.rename(temporary, name, src_dir_fd=directory_fd, dst_dir_fd=directory_fd)
            renamed = True
            os.fsync(directory_fd)
        finally:
            if descriptor >= 0:
                os.close(descriptor)
            if not renamed:
                try:
                    os.unlink(temporary, dir_fd=directory_fd)
                except FileNotFoundError:
                    pass


def atomic_write_text(root: Path, name: str, value: str) -> None:
    atomic_write(root, name, value.encode("utf-8"))


def atomic_write_json(root: Path, name: str, value: object, *, indent: int | None = None) -> None:
    raw = json.dumps(value, sort_keys=True, indent=indent, separators=None if indent else (",", ":"))
    atomic_write_text(root, name, raw + "\n")


def remove(root: Path, name: str, *, missing_ok: bool = True) -> None:
    name = _name(name)
    with state_dir(root) as directory_fd:
        try:
            before = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
        except FileNotFoundError:
            if missing_ok:
                return
            raise StateIOError(f"lifecycle marker is missing: {name}")
        _validate_file(before, maximum=1024 * 1024)
        current = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
        if (current.st_dev, current.st_ino) != (before.st_dev, before.st_ino):
            raise StateIOError(f"lifecycle marker replaced before removal: {name}")
        os.unlink(name, dir_fd=directory_fd)
        os.fsync(directory_fd)
