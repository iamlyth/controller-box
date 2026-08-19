#!/usr/bin/env python3
"""Bind campaign verification to tracked config and executable identity/content."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import subprocess
import tomllib

ROOT = Path(__file__).resolve().parent.parent
CONFIG = ROOT / ".factory/config.toml"


def fail(message: str) -> None:
    raise SystemExit(f"campaign-verifier-binding: {message}")


def git(*args: str) -> str:
    result = subprocess.run(
        ["git", *args], cwd=ROOT, text=True, capture_output=True,
        env={**os.environ, "GIT_NO_REPLACE_OBJECTS": "1"},
    )
    if result.returncode:
        fail(f"Git binding failed for {' '.join(args)}")
    return result.stdout.strip()


def secure_read(path: Path, maximum: int) -> tuple[bytes, os.stat_result]:
    absolute = path.absolute()
    try:
        if absolute.resolve(strict=True) != absolute:
            fail(f"path contains a symlink: {path.relative_to(ROOT)}")
    except OSError as exc:
        fail(f"path is unavailable: {exc}")
    descriptor = os.open(absolute, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    try:
        before = os.fstat(descriptor)
        named = absolute.lstat()
        if (
            not stat.S_ISREG(before.st_mode)
            or before.st_uid != os.getuid()
            or before.st_nlink != 1
            or (before.st_dev, before.st_ino) != (named.st_dev, named.st_ino)
            or before.st_size > maximum
        ):
            fail(f"unsafe file: {path.relative_to(ROOT)}")
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
        named_after = absolute.lstat()
        if (
            len(data) > maximum
            or (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns)
            != (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns)
            or (after.st_dev, after.st_ino) != (named_after.st_dev, named_after.st_ino)
        ):
            fail(f"file changed while binding: {path.relative_to(ROOT)}")
        return data, before
    finally:
        os.close(descriptor)


def blob_id(data: bytes) -> str:
    object_format = git("rev-parse", "--show-object-format")
    if object_format not in {"sha1", "sha256"}:
        fail("unsupported Git object format")
    payload = f"blob {len(data)}\0".encode("ascii") + data
    return hashlib.new(object_format, payload).hexdigest()


def tracked_blob(path: str, data: bytes, expected_mode: str) -> str:
    entry = git("ls-files", "-s", "--", path).split()
    if len(entry) != 4 or entry[0] != expected_mode or entry[3] != path:
        fail(f"tracked file has the wrong mode or identity: {path}")
    committed = git("rev-parse", f"HEAD:{path}")
    if blob_id(data) != committed or entry[1] != committed:
        fail(f"working file does not match its committed blob: {path}")
    return committed


def main() -> None:
    config_bytes, _ = secure_read(CONFIG, 1024 * 1024)
    try:
        config = tomllib.loads(config_bytes.decode("utf-8"))
    except (UnicodeError, tomllib.TOMLDecodeError) as exc:
        fail(f"invalid factory config: {exc}")
    command = config.get("verification", {}).get("campaign_command")
    if (
        not isinstance(command, list)
        or not command
        or not all(isinstance(arg, str) and arg and "\x00" not in arg for arg in command)
    ):
        fail("verification.campaign_command must be a non-empty argv array")
    executable_arg = command[0]
    if not executable_arg.startswith("./"):
        fail("campaign verifier executable must be canonical repository-relative ./path")
    relative = PurePosixPath(executable_arg[2:])
    if relative.is_absolute() or not relative.parts or any(part in {"", ".", ".."} for part in relative.parts):
        fail("campaign verifier executable path is not canonical")
    canonical_arg = "./" + relative.as_posix()
    if canonical_arg != executable_arg:
        fail("campaign verifier executable path is not canonical")
    executable = ROOT.joinpath(*relative.parts)
    executable_bytes, executable_info = secure_read(executable, 16 * 1024 * 1024)
    if not executable_info.st_mode & 0o111:
        fail("campaign verifier is not executable")
    relative_text = relative.as_posix()
    executable_blob = tracked_blob(relative_text, executable_bytes, "100755")
    config_blob = tracked_blob(".factory/config.toml", config_bytes, "100644")
    binding = {
        "schema": "campaign-verifier-binding/v1",
        "argv": command,
        "executable": relative_text,
        "executable_sha256": hashlib.sha256(executable_bytes).hexdigest(),
        "executable_blob": executable_blob,
        "executable_mode": format(stat.S_IMODE(executable_info.st_mode), "04o"),
        "config_sha256": hashlib.sha256(config_bytes).hexdigest(),
        "config_blob": config_blob,
    }
    digest = hashlib.sha256(
        json.dumps(binding, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()
    print(json.dumps({"binding": binding, "sha256": digest}, sort_keys=True, separators=(",", ":")))


if __name__ == "__main__":
    main()
