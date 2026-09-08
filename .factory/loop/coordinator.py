"""Rootless coordinator launch-authority state (BUG-0022).

The local factory harness must never require root: only remote runners may
use root.  This module creates/opens the persistent per-user coordinator
authority state under a private mode-0700 XDG state directory outside the
repository, and the installed ``factory-coordinator`` entrypoint exports only
the inherited ``FACTORY_COORDINATOR_AUTH_FD`` descriptor and execs only the
sibling installed ``factory-campaign`` with the forwarded campaign arguments.

State contract (``factory-coordinator-launch-authority/v1``):

* the state directory is ``$XDG_STATE_HOME/factory-coordinator`` (or
  ``~/.local/state/factory-coordinator``), mode 0700, owned by the current
  effective user, with no symlink component;
* the state file is a regular file owned by the current effective user with
  exact mode 0600, link count 1, opened ``O_RDWR`` with ``O_NOFOLLOW`` and
  ``O_CLOEXEC`` (the descriptor is the only thing exported to the campaign);
* a fresh state file carries a random key of at least 32 bytes and an empty
  ``entries`` map, durably fsynced;
* any malformed, unsafe, or foreign-owned existing state fails closed.

The pathname is never exported to the campaign or model environment: only
the inherited descriptor number travels as ``FACTORY_COORDINATOR_AUTH_FD``.
"""

from __future__ import annotations

import fcntl
import json
import os
import secrets
import stat
import sys
from pathlib import Path
from typing import List, Optional, Sequence, Tuple

AUTH_ENV = "FACTORY_COORDINATOR_AUTH_FD"
SCHEMA = "factory-coordinator-launch-authority/v1"
STATE_DIR_NAME = "factory-coordinator"
STATE_FILE_NAME = "launch-authority.json"
KEY_MIN_BYTES = 32
MAX_STATE_BYTES = 4 * 1024 * 1024
STATE_MODE = 0o600
DIR_MODE = 0o700


class CoordinatorError(Exception):
    """The rootless coordinator refused to create/open authority state."""


def _reject_root() -> None:
    if os.geteuid() == 0:
        raise CoordinatorError(
            "the local factory coordinator must never run as root; only "
            "remote runners may use root"
        )


def _state_dir(workspace: Optional[Path] = None) -> Path:
    base = os.environ.get("XDG_STATE_HOME")
    if not base:
        base = os.path.join(os.path.expanduser("~"), ".local", "state")
    if not base or not os.path.isabs(base):
        raise CoordinatorError("coordinator state base must be an absolute path")
    state_dir = Path(base) / STATE_DIR_NAME
    if workspace is not None:
        try:
            common = os.path.commonpath(
                (os.path.realpath(state_dir), os.path.realpath(workspace))
            )
        except ValueError:
            common = ""
        if common == os.path.realpath(workspace):
            raise CoordinatorError(
                "coordinator authority state must live outside the campaign "
                "workspace"
            )
    return state_dir


def _ensure_private_dir(state_dir: Path) -> None:
    """Create/verify the mode-0700 user-owned no-symlink state directory."""
    base = state_dir.parent
    if not base.exists():
        raise CoordinatorError(f"coordinator state base does not exist: {base}")
    try:
        base_info = os.lstat(base)
    except OSError as exc:
        raise CoordinatorError(f"cannot inspect coordinator state base: {exc}") from exc
    if stat.S_ISLNK(base_info.st_mode) or not stat.S_ISDIR(base_info.st_mode):
        raise CoordinatorError(
            f"coordinator state base is not a real directory: {base}"
        )
    try:
        os.makedirs(state_dir, mode=DIR_MODE, exist_ok=True)
    except OSError as exc:
        raise CoordinatorError(f"cannot create coordinator state directory: {exc}") from exc
    # Verify every component from the base down: no symlink, directory,
    # current-user-owned, and no group/other permission bits.
    current = base
    for part in state_dir.relative_to(base).parts:
        current = current / part
        try:
            info = os.lstat(current)
        except OSError as exc:
            raise CoordinatorError(
                f"cannot inspect coordinator state path {current}: {exc}"
            ) from exc
        if stat.S_ISLNK(info.st_mode):
            raise CoordinatorError(f"coordinator state path is a symlink: {current}")
        if not stat.S_ISDIR(info.st_mode):
            raise CoordinatorError(f"coordinator state path is not a directory: {current}")
        if info.st_uid != os.geteuid():
            raise CoordinatorError(
                f"coordinator state directory is not current-user-owned: {current}"
            )
        if stat.S_IMODE(info.st_mode) != DIR_MODE:
            raise CoordinatorError(
                f"coordinator state directory mode is not {DIR_MODE:o}: {current}"
            )


def _new_state_bytes() -> bytes:
    key = secrets.token_bytes(KEY_MIN_BYTES)
    document = {"schema": SCHEMA, "key": key.hex(), "entries": {}}
    return (json.dumps(document, sort_keys=True, separators=(",", ":")) + "\n").encode()


def _validate_state(raw: bytes) -> None:
    try:
        document = json.loads(raw)
        key = bytes.fromhex(document["key"])
    except (ValueError, TypeError, KeyError) as exc:
        raise CoordinatorError("coordinator authorization state is malformed") from exc
    if (set(document) != {"schema", "key", "entries"}
            or document.get("schema") != SCHEMA
            or not isinstance(document.get("entries"), dict)
            or len(key) < KEY_MIN_BYTES):
        raise CoordinatorError("coordinator authorization state is malformed")


def _open_state_file(state_dir: Path) -> int:
    """Open (creating when absent) the exact-0600 user-owned state file."""
    state_file = state_dir / STATE_FILE_NAME
    flags = os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(state_file, flags, STATE_MODE)
    except OSError as exc:
        raise CoordinatorError(f"cannot open coordinator state file: {exc}") from exc
    try:
        info = os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid()
                or stat.S_IMODE(info.st_mode) != STATE_MODE or info.st_nlink != 1):
            raise CoordinatorError(
                "coordinator state file is not a current-user-owned regular "
                "file with exact mode 0600 and link count 1"
            )
        if info.st_size == 0:
            raw = _new_state_bytes()
            os.write(fd, raw)
            os.fsync(fd)
        else:
            if info.st_size > MAX_STATE_BYTES:
                raise CoordinatorError("coordinator authorization state is oversized")
            os.lseek(fd, 0, os.SEEK_SET)
            raw = os.read(fd, MAX_STATE_BYTES + 1)
            _validate_state(raw)
        return fd
    except BaseException:
        os.close(fd)
        raise


def open_authority_state(workspace: Optional[Path] = None) -> Tuple[int, Path]:
    """Create/open the rootless coordinator authority state.

    Returns ``(fd, state_file)`` where ``fd`` is the O_RDWR descriptor of the
    validated state file.  The caller exports only the descriptor number and
    must never leak the pathname into a campaign/model environment.
    """
    _reject_root()
    state_dir = _state_dir(workspace)
    _ensure_private_dir(state_dir)
    fd = _open_state_file(state_dir)
    return fd, state_dir / STATE_FILE_NAME


def _extract_root(argv: Sequence[str]) -> Optional[Path]:
    root: Optional[str] = None
    for index, arg in enumerate(argv):
        if arg == "--root" and index + 1 < len(argv):
            root = argv[index + 1]
        elif arg.startswith("--root="):
            root = arg.split("=", 1)[1]
    if root is None:
        return None
    return Path(root).absolute()


def _mark_unrelated_close_on_exec(auth_fd: int) -> None:
    """Every descriptor except stdio and the authority FD is close-on-exec."""
    try:
        entries = os.listdir("/proc/self/fd")
    except OSError:
        return
    for name in entries:
        try:
            candidate = int(name)
        except ValueError:
            continue
        if candidate in (0, 1, 2, auth_fd):
            continue
        try:
            flags = fcntl.fcntl(candidate, fcntl.F_GETFD)
            fcntl.fcntl(candidate, fcntl.F_SETFD, flags | fcntl.FD_CLOEXEC)
        except OSError:
            continue
    # The authority descriptor must survive the exec into the campaign.
    try:
        flags = fcntl.fcntl(auth_fd, fcntl.F_GETFD)
        fcntl.fcntl(auth_fd, fcntl.F_SETFD, flags & ~fcntl.FD_CLOEXEC)
    except OSError as exc:
        raise CoordinatorError(f"cannot clear close-on-exec on authority FD: {exc}") from exc


def _sibling_campaign() -> Path:
    sibling = Path(__file__).resolve().parent.parent / "bin" / "factory-campaign"
    try:
        info = os.lstat(sibling)
    except OSError as exc:
        raise CoordinatorError(f"sibling factory-campaign is unavailable: {exc}") from exc
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
        raise CoordinatorError("sibling factory-campaign is not a regular file")
    return sibling


def main(argv: Optional[Sequence[str]] = None) -> int:
    """Rootless coordinator entrypoint: state then fixed sibling exec.

    Exports only ``FACTORY_COORDINATOR_AUTH_FD=<fd>`` and execs only the
    sibling installed ``factory-campaign`` with the forwarded campaign
    arguments — never an arbitrary command.
    """
    if argv is None:
        argv = sys.argv[1:]
    _reject_root()
    workspace = _extract_root(argv)
    fd, _ = open_authority_state(workspace=workspace)
    os.environ[AUTH_ENV] = str(fd)
    _mark_unrelated_close_on_exec(fd)
    campaign = _sibling_campaign()
    os.execv(str(campaign), [str(campaign), *argv])
    return 1  # pragma: no cover - execv never returns


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except CoordinatorError as exc:
        print(f"factory-coordinator: {exc}", file=sys.stderr)
        raise SystemExit(1)
