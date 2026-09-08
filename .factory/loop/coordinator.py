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
* every component from the filesystem anchor to the state directory is
  opened by descriptor (``openat`` with ``O_DIRECTORY|O_NOFOLLOW``) and
  validated with ``fstat`` on the *held* descriptor — never lstat-then-reopen
  by full pathname — so a directory swap between inspection and use cannot
  redirect the walk; a missing ``$XDG_STATE_HOME`` base (or the private
  coordinator directory beneath it) is auto-created mode-0700 and immediately
  re-opened by descriptor so its identity is still validated on the held FD,
  while ancestors above the base are only required to be symlink-free real
  directories (a root-owned ``/``, ``/home``, or shared ``~/.local`` never
  forces root);
* the state file is a regular file owned by the current effective user with
  exact mode 0600, link count 1, opened ``O_RDWR`` with ``O_NOFOLLOW`` and
  ``O_CLOEXEC`` via the held directory descriptor (the descriptor is the only
  thing exported to the campaign);
* a fresh state file is created only when this invocation succeeds with
  ``O_CREAT|O_EXCL`` and carries a random key of at least 32 bytes and an
  empty ``entries`` map, written with a complete write loop, fsynced, and the
  containing directory fsynced after first creation;
* any malformed, unsafe, foreign-owned, or existing empty/truncated state
  fails closed — an existing zero-byte or partial file is never
  reinitialized, so a crash can fail closed but never silently resets the
  key or the consumed-scope ``entries`` map.

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
# No-follow, directory-only, close-on-exec descriptor opens used for the
# entire state walk and final file open (openat semantics: every component is
# resolved against the already-validated parent descriptor, never by lstat
# then full-pathname reopen).
_DIR_OPEN_FLAGS = (os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
                   | getattr(os, "O_NOFOLLOW", 0) | os.O_CLOEXEC)


class CoordinatorError(Exception):
    """The rootless coordinator refused to create/open authority state."""


def _reject_root() -> None:
    if os.geteuid() == 0:
        raise CoordinatorError(
            "the local factory coordinator must never run as root; only "
            "remote runners may use root"
        )


def _state_base(workspace: Optional[Path] = None) -> Path:
    base = os.environ.get("XDG_STATE_HOME")
    if not base:
        base = os.path.join(os.path.expanduser("~"), ".local", "state")
    if not base or not os.path.isabs(base):
        raise CoordinatorError("coordinator state base must be an absolute path")
    base_path = Path(base)
    state_dir = base_path / STATE_DIR_NAME
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
    return base_path


def _state_path(base: Path) -> Path:
    return base / STATE_DIR_NAME / STATE_FILE_NAME


def _ensure_private_dir(base: Path) -> int:
    """Walk/auto-create the private per-user state directory by descriptor.

    Every component from ``base.anchor`` down to
    ``<base>/factory-coordinator`` is opened with no-follow directory
    descriptor opens validated on the held FD — a component that is swapped
    for a symlink after any inspection fails the ``O_NOFOLLOW`` open or the
    ``fstat`` identity check and can never redirect the walk.  Ancestors
    above the base are only required to be symlink-free real directories
    (their ownership is not required, so a root-owned ``/home`` or a shared
    ``~/.local`` never forces root).  A missing base or coordinator directory
    is created mode-0700 and immediately re-opened by descriptor so its
    identity (current-user-owned mode-0700 real directory) is still validated
    on the held FD — auto-creation never weakens the ancestor checks that
    precede it.

    Returns an ``O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC`` descriptor of the
    coordinator directory; the caller must hold it only as long as
    initialization requires and close it (CLOEXEC keeps it out of any exec).
    """
    if not base.is_absolute():
        raise CoordinatorError("coordinator state base must be an absolute path")
    components: List[str] = list(base.parts[1:]) + [STATE_DIR_NAME]
    # Index of the base component within ``components``; components before it
    # are pre-base ancestors (no creation, no ownership requirement), the base
    # itself and the coordinator directory are private per-user state.
    base_at = len(base.parts) - 2
    fd = -1
    try:
        fd = os.open(base.anchor, _DIR_OPEN_FLAGS)
        for index, part in enumerate(components):
            try:
                child = os.open(part, _DIR_OPEN_FLAGS, dir_fd=fd)
            except FileNotFoundError:
                if index < base_at:
                    raise CoordinatorError(
                        "coordinator state ancestor is missing: "
                        f"{Path(base.anchor, *components[:index + 1])}"
                    )
                try:
                    os.mkdir(part, DIR_MODE, dir_fd=fd)
                except OSError as exc:
                    raise CoordinatorError(
                        "cannot create private coordinator state directory "
                        f"{part!r}: {exc}"
                    ) from exc
                try:
                    child = os.open(part, _DIR_OPEN_FLAGS, dir_fd=fd)
                except OSError as exc:
                    raise CoordinatorError(
                        "cannot reopen created coordinator state directory "
                        f"{part!r}: {exc}"
                    ) from exc
            except OSError as exc:
                raise CoordinatorError(
                    f"cannot open coordinator state path component {part!r}: {exc}"
                ) from exc
            os.close(fd)
            fd = child
            # Identity is validated on the held descriptor, so a swap after
            # this open cannot masquerade as our directory.
            info = os.fstat(fd)
            if not stat.S_ISDIR(info.st_mode):
                raise CoordinatorError(
                    f"coordinator state path component is not a directory: {part}"
                )
            if index >= base_at:
                if info.st_uid != os.geteuid():
                    raise CoordinatorError(
                        "coordinator state directory is not current-user-owned: "
                        f"{part}"
                    )
                # The private coordinator directory itself must be exact 0700
                # (the base may pre-exist from another tool with a looser
                # mode, but the state directory we own is strict).
                if index > base_at and stat.S_IMODE(info.st_mode) != DIR_MODE:
                    raise CoordinatorError(
                        f"coordinator state directory mode is not {DIR_MODE:o}: "
                        f"{part}"
                    )
        return fd
    except BaseException:
        if fd >= 0:
            try:
                os.close(fd)
            except OSError:
                pass
        raise


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


def _write_all(fd: int, data: bytes) -> None:
    """Complete write loop: a short/partial write never truncates state."""
    view = memoryview(data)
    while view:
        written = os.write(fd, view)
        if written <= 0:
            raise OSError("short coordinator state write")
        view = view[written:]


def _open_state_file(dir_fd: int) -> int:
    """Open the exact-0600 user-owned state file via the held directory FD.

    Fresh state is only ever created by this invocation succeeding with
    ``O_CREAT|O_EXCL``: an existing file — including an existing zero-byte or
    truncated one — is opened again without ``O_CREAT`` and validated, never
    reinitialized.  A crash after creation therefore fails closed on the next
    run instead of silently resetting the key or consumed-scope entries.
    """
    created = False
    try:
        fd = os.open(STATE_FILE_NAME, os.O_RDWR | os.O_CREAT | os.O_EXCL
                     | os.O_CLOEXEC | getattr(os, "O_NOFOLLOW", 0),
                     STATE_MODE, dir_fd=dir_fd)
    except FileExistsError:
        try:
            fd = os.open(STATE_FILE_NAME, os.O_RDWR | os.O_CLOEXEC
                         | getattr(os, "O_NOFOLLOW", 0), dir_fd=dir_fd)
        except OSError as exc:
            raise CoordinatorError(
                f"cannot open existing coordinator state file: {exc}"
            ) from exc
    except OSError as exc:
        raise CoordinatorError(
            f"cannot create coordinator state file: {exc}"
        ) from exc
    else:
        created = True
    try:
        info = os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid()
                or stat.S_IMODE(info.st_mode) != STATE_MODE or info.st_nlink != 1):
            raise CoordinatorError(
                "coordinator state file is not a current-user-owned regular "
                "file with exact mode 0600 and link count 1"
            )
        if created:
            raw = _new_state_bytes()
            _write_all(fd, raw)
            os.fsync(fd)
            os.fsync(dir_fd)  # fresh state is a new directory entry
        else:
            if info.st_size == 0:
                raise CoordinatorError(
                    "existing coordinator authorization state is empty; an "
                    "interrupted or truncated state is never reinitialized — "
                    "delete the state file to recover"
                )
            if info.st_size > MAX_STATE_BYTES:
                raise CoordinatorError("coordinator authorization state is oversized")
            raw = os.pread(fd, MAX_STATE_BYTES + 1, 0)
            _validate_state(raw)
        return fd
    except BaseException:
        os.close(fd)
        raise


def open_authority_state(workspace: Optional[Path] = None) -> Tuple[int, Path]:
    """Create/open the rootless coordinator authority state.

    Returns ``(fd, state_file)`` where ``fd`` is the O_RDWR descriptor of the
    validated state file.  The caller exports only the descriptor number and
    must never leak the pathname into a campaign/model environment.  The
    directory descriptor is held only as long as initialization requires and
    closed before returning.
    """
    _reject_root()
    base = _state_base(workspace)
    dir_fd = _ensure_private_dir(base)
    try:
        fd = _open_state_file(dir_fd)
    finally:
        os.close(dir_fd)
    return fd, _state_path(base)


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
    """Every descriptor except stdio and the authority FD is close-on-exec.

    Fails closed when the ``/proc/self/fd`` descriptor lockdown cannot be
    performed: silently skipping would let an unrelated descriptor — a
    credential store, a lock, a stage directory — survive the exec into the
    campaign.
    """
    try:
        entries = os.listdir("/proc/self/fd")
    except OSError as exc:
        raise CoordinatorError(
            "cannot lock down descriptors for exec: /proc/self/fd is "
            f"unavailable: {exc}"
        ) from exc
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
