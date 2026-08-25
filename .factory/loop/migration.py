#!/usr/bin/env python3
"""Post-migration authority (MIG-01, §21): confirms the Ralph control plane is removed.

This module is the compact post-migration authority of the hidden
``.factory/loop/`` package.  The Ralph Orchestrator control plane has been
removed from the tracked tree — not deprecated, not frozen behind forwarders:
the deprecated launchers, lifecycle tools, Pi shim/extension, context-summary
authority, and Ralph tests are gone.  The migration authority therefore
verifies, with metadata and trusted Git only, that the removal is complete
and stays complete:

* **tracked absence** — the forbidden legacy pathnames are enumerated from
  the pinned, bounded ``git ls-files -z`` listing (never from foreign file
  contents): ``.ralph/**``, ``.factory/ralph/**``, the old role prompts and
  the stale context-summary mirror, ``scripts/ralph-*``, the Pi Ralph
  shim/extension, and the Ralph tests.  Any tracked forbidden pathname is a
  migration regression and fails the ``verify`` command;
* **freeze marker** — the tracked ``.factory/ralph-freeze`` marker must be a
  safe non-executable regular file: present, tracked, a regular non-symlink
  file owned by the invoking user, single-link, with no group/other write
  bits and no execute bits.  A symlink, FIFO, socket, device, executable,
  foreign-owned, hardlinked, or group/other-writable marker fails closed;
* **canonical plan/spec/roles current** — the committed plan parses against
  the committed requirement-policy registry, its spec binding
  (``spec_path``/``spec_commit``/``spec_blob``) resolves to the exact
  committed spec blob at the snapshot head, and every canonical role prompt
  (``planner``/``developer``/``tester``/``auditor``) is tracked.  Every
  committed blob is exact-size-pre-checked against its per-kind cap before
  any body byte is read (bounded, no-follow, no-replace Git reads);
* **foreign ``.ralph/`` presence only** — the migration authority may
  ``lstat`` the repository's ``.ralph/`` entry to report presence, but it
  never enumerates, opens, or reads anything under it.  The foreign
  directory (and its sentinel bytes, mode, and mtime) is preserved exactly;
  no legacy byte can ever enter a report, log, or receipt.

There is no context-summary authority and no launcher-compatibility
requirement: the new control plane never invokes, freezes, or names a
deprecated Ralph launcher, and the stale context-summary mirror is simply one
of the forbidden tracked pathnames whose absence is confirmed.

The module uses only the Python standard library plus the committed
hidden-loop modules (``gitutil``, ``plan_parser``) — never Ralph, never a
subprocess beyond the pinned Git boundary, never a wall-clock timestamp.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys
from typing import Dict, List, Optional, Sequence, Tuple

try:  # package import (the hidden `.factory/loop/` package)
    from . import gitutil
    from . import plan_parser
except ImportError:  # flat import used by the hidden `.factory/tests/` suite
    import gitutil  # type: ignore[no-redef]
    import plan_parser  # type: ignore[no-redef]

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

MIGRATION_SCHEMA = "factory-migration/v1"

# The tracked freeze marker: a safe non-executable regular file that records
# the Ralph control plane is frozen/removed.  It is trusted only as a
# regular non-executable file; any other shape fails closed.
FREEZE_MARKER_RELPATH = ".factory/ralph-freeze"

DEFAULT_PLAN_PATH = ".factory/artifacts/implementation-plan.md"

# The canonical role prompts the new control plane requires.
ROLE_PROMPTS: Tuple[str, ...] = ("planner", "developer", "tester", "auditor")

# Forbidden tracked pathnames (post-removal).  The migration enumerates
# tracked names through the pinned bounded Git boundary and rejects any of
# these exact paths/prefixes/globs — it never inspects foreign file
# contents to decide.
FORBIDDEN_TRACKED_PREFIXES: Tuple[str, ...] = (
    ".ralph/",  # legacy runtime namespace (tracked content is forbidden)
    ".factory/ralph/",  # legacy plan/audit/maintenance YAML
)
FORBIDDEN_TRACKED_EXACT: Tuple[str, ...] = (
    ".factory/artifacts/context-summary.md",  # stale context-summary mirror
    "scripts/pi-cli-shims/ralph",  # Pi Ralph shim
    "scripts/pi-ralph-emit-extension.mjs",  # retired Pi Ralph emit extension
    "scripts/pi2-ollama.sh",  # legacy Ralph/Pi wrapper
    "tests/test-pi2-ollama-wrapper.sh",  # legacy wrapper tests
)
# The generic model-side Pi guard extension
# (``scripts/pi-factory-guard-extension.mjs``) is the required replacement
# for the retired emit extension: it is *not* forbidden here and is bound
# by the boilerplate verifier and the launch authority.
# Old role prompts replaced by the canonical planner/developer/tester/auditor
# set.
FORBIDDEN_OLD_PROMPTS: Tuple[str, ...] = (
    ".factory/prompts/audit.md",
    ".factory/prompts/implementation.md",
    ".factory/prompts/maintenance-plan.md",
    ".factory/prompts/maintenance.md",
    ".factory/prompts/plan.md",
)
# Deprecated Ralph launchers/lifecycle tools and Ralph tests.
FORBIDDEN_TRACKED_GLOBS: Tuple[str, ...] = (
    "scripts/ralph-*",
    "tests/test-ralph-*",
    "tests/test-context-summary.sh",
)

# Bounds for every read the migration performs (finite, fail closed on
# overflow — never unbounded).  The tracked-file listing is a hard-bounded
# capture; the per-kind Git blob caps exactly match the launch prompt-input
# limits (``launch.PROMPT_INPUT_MAX``, 1 MiB per input kind), so the
# migration can never accept a blob that a real launch could not consume.
# Every migration blob is size-pre-checked with the pinned ``git cat-file
# -s`` (finite bounded timeout, replace refs disabled) against its exact
# per-kind cap *before* any blob byte is read, and the subsequent read is a
# hard-bounded capture of at most the same cap.
TRACKED_LIST_MAX = 4 * 1024 * 1024
PLAN_BLOB_MAX = 1024 * 1024
SPEC_BLOB_MAX = 1024 * 1024
ROLE_PROMPT_MAX = 1024 * 1024
# ``git cat-file -s`` prints a bare decimal byte count and a newline; a
# fixed tiny bound keeps even the size query a bounded capture, and the
# same bound covers the ``rev-parse`` 40-hex blob-ID outputs.
CAT_FILE_SIZE_MAX = 256
BLOB_ID_MAX = 256

SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
SHA40_RE = re.compile(r"^[0-9a-f]{40}$")


class MigrationError(Exception):
    """Base class for every fail-closed migration failure."""


class MigrationUnavailableError(MigrationError):
    """The migration authority cannot operate (no pinned Git, no root)."""


# ---------------------------------------------------------------------------
# Safe detection primitives (metadata only)
# ---------------------------------------------------------------------------


def _as_root(root) -> Path:
    """Validate the canonical repository root (no-follow, real directory)."""
    root = Path(root).absolute()
    try:
        info = os.stat(root, follow_symlinks=False)
    except OSError as exc:
        raise MigrationError(f"cannot stat repository root {root}: {exc}") from exc
    if not stat.S_ISDIR(info.st_mode):
        raise MigrationError(f"repository root is not a directory: {root}")
    return root


def is_ralph_frozen(root) -> bool:
    """True when the tracked freeze marker is a safe non-executable regular file.

    A missing marker means *not frozen* (the removal is not yet recorded).
    Any *present* marker that is not an ordinary non-executable regular
    file — a symlink, FIFO, socket, device, directory, an executable file, a
    foreign-owned, hardlinked, or group/other-writable file — fails closed
    with :class:`MigrationUnavailableError`: an unsafe marker must neither
    freeze nor silently unfreeze a launch.
    """
    root = _as_root(root)
    marker = root / FREEZE_MARKER_RELPATH
    try:
        info = os.lstat(marker)
    except FileNotFoundError:
        return False
    except OSError as exc:
        raise MigrationUnavailableError(
            f"cannot inspect the freeze marker {marker}: {exc}"
        ) from exc
    _validate_marker_info(info, marker)
    return True


def _validate_marker_info(info: os.stat_result, marker: Path) -> None:
    """Fail-closed owner/mode/link identity of the freeze marker.

    The marker is trusted only as a regular non-symlink single-link file
    owned by the invoking user, with no group/other write bits and no
    execute bits.  A symlink, FIFO, socket, device, directory, executable,
    foreign-owned, hardlinked, or group/other-writable marker fails closed.
    """
    if not stat.S_ISREG(info.st_mode) or stat.S_ISLNK(info.st_mode):
        kind = "a symlink" if stat.S_ISLNK(info.st_mode) else "not a regular file"
        raise MigrationUnavailableError(
            f"the freeze marker {marker} is {kind}; it must be a regular file"
        )
    if info.st_uid != os.getuid():
        raise MigrationUnavailableError(
            f"the freeze marker {marker} is not owned by the invoking user"
        )
    if info.st_nlink != 1:
        raise MigrationUnavailableError(
            f"the freeze marker {marker} is hardlinked; it must be single-link"
        )
    if info.st_mode & 0o022:
        raise MigrationUnavailableError(
            f"the freeze marker {marker} is group/other-writable; it must be "
            "a safe non-executable regular file"
        )
    if info.st_mode & 0o111:
        raise MigrationUnavailableError(
            f"the freeze marker {marker} is executable; it must be a safe "
            "non-executable regular file"
        )


def _lexists(root: Path, relpath: str) -> bool:
    try:
        os.lstat(root / relpath)
        return True
    except FileNotFoundError:
        return False
    except OSError as exc:
        raise MigrationUnavailableError(
            f"cannot inspect {relpath!r}: {exc}"
        ) from exc


def ralph_presence(root) -> Dict[str, object]:
    """Presence-only report of the foreign ``.ralph/`` entry (lstat only).

    The migration may ``lstat`` the repository's ``.ralph/`` entry to report
    whether the foreign directory remains, but it never enumerates, opens,
    or reads anything under it — the foreign bytes, mode, and mtime are
    preserved exactly and can never enter a report.
    """
    root = _as_root(root)
    return {"present": _lexists(root, ".ralph")}


# ---------------------------------------------------------------------------
# Trusted read-only Git derivation
# ---------------------------------------------------------------------------


def _git(root: Path, argv: Sequence[str]) -> subprocess.CompletedProcess[str]:
    try:
        return gitutil.git_run(
            ["-C", str(root), *argv], timeout=gitutil.GIT_TIMEOUT
        )
    except gitutil.GitBoundaryError as exc:
        raise MigrationUnavailableError(
            f"the pinned Git boundary failed during migration verification: {exc}"
        ) from exc


def _git_bytes(
    root: Path, argv: Sequence[str], *, maximum: Optional[int] = None
) -> bytes:
    """Byte-preserving pinned Git output; raises on boundary failure.

    ``maximum`` turns the capture into a hard-bounded pipe read (never an
    unbounded capture); the default keeps the plain byte read, used only for
    structurally tiny outputs (``rev-parse``, ``cat-file -s``) that callers
    bound explicitly where required.
    """
    try:
        if maximum is None:
            result = gitutil.git_bytes(
                ["-C", str(root), *argv], timeout=gitutil.GIT_TIMEOUT
            )
        else:
            result = gitutil.git_bytes_bounded(
                ["-C", str(root), *argv],
                maximum=maximum,
                timeout=gitutil.GIT_TIMEOUT,
            )
    except gitutil.GitBoundaryError as exc:
        raise MigrationUnavailableError(
            f"the pinned Git boundary failed during migration verification: {exc}"
        ) from exc
    if result.returncode != 0:
        raise MigrationUnavailableError(
            f"pinned Git command failed: {' '.join(argv)}"
        )
    return result.stdout


def _git_blob_bounded(
    root: Path, blob_id: str, maximum: int, what: str
) -> bytes:
    """Exact pre-checked bounded read of one committed migration blob.

    The blob's *exact* size is first queried with the pinned
    ``git cat-file -s`` (finite bounded timeout; replace refs are disabled by
    the pinned Git boundary) and must satisfy the per-kind cap *before* any
    blob byte is read; only then is the blob content read with a hard-bounded
    capture (``git cat-file blob``, at most ``maximum`` bytes), and the
    captured length must equal the pre-checked exact size.  An oversized,
    malformed, or size-mutating blob fails closed before its bytes are ever
    returned — never an unbounded capture.
    """
    if not SHA40_RE.fullmatch(blob_id):
        raise MigrationUnavailableError(
            f"the {what} blob is not a 40-hex Git object ID: {blob_id!r}"
        )
    size_out = _git_bytes(
        root, ["cat-file", "-s", blob_id], maximum=CAT_FILE_SIZE_MAX
    )
    size_text = size_out.decode("utf-8", "replace").strip()
    if not re.fullmatch(r"[0-9]{1,12}", size_text):
        raise MigrationUnavailableError(
            f"cannot parse the exact size of the {what} blob "
            f"{blob_id}: {size_text!r}"
        )
    size = int(size_text)
    if size > maximum:
        raise MigrationUnavailableError(
            f"the {what} blob {blob_id} is {size} bytes, exceeding the "
            f"{maximum}-byte per-kind cap"
        )
    raw = _git_bytes(root, ["cat-file", "blob", blob_id], maximum=maximum)
    if len(raw) != size:
        raise MigrationUnavailableError(
            f"the {what} blob {blob_id} changed size between the exact size "
            f"pre-check and the bounded read ({size} != {len(raw)})"
        )
    return raw


def _head_commit(root: Path) -> str:
    result = _git(root, ["rev-parse", "--verify", "HEAD"])
    if result.returncode != 0:
        raise MigrationUnavailableError("cannot resolve HEAD of the repository")
    head = result.stdout.strip()
    if not SHA40_RE.fullmatch(head):
        raise MigrationUnavailableError("resolved HEAD is not a 40-hex commit")
    return head


def _committed_blob_id(root: Path, head: str, relpath: str, what: str) -> str:
    """The exact 40-hex blob ID of ``head:<relpath>`` (strict SHA, no refs).

    The blob ID is resolved with the pinned no-replace Git boundary and must
    be a strict 40-hex Git object ID before any byte is read: a ref, a
    non-hex string, or a replace-ref object fails closed here, so the
    per-kind bounded blob reads below can never be handed an ambiguous or
    redirectable object name.
    """
    try:
        raw = _git_bytes(
            root,
            ["rev-parse", f"{head}:{relpath}"],
            maximum=BLOB_ID_MAX,
        )
        blob = raw.decode("ascii").strip()
    except (MigrationUnavailableError, UnicodeDecodeError) as exc:
        raise MigrationUnavailableError(
            f"{what} {relpath!r} is not committed at the snapshot head"
        ) from exc
    if not SHA40_RE.fullmatch(blob):
        raise MigrationUnavailableError(
            f"{what} at {relpath!r} does not resolve to a strict 40-hex Git "
            f"object ID: {blob!r}"
        )
    return blob


def _tracked_files(root: Path) -> Tuple[str, ...]:
    """Bounded trusted-Git enumeration of every tracked name.

    The tracked names are read from the pinned ``git ls-files -z`` listing
    (hard-bounded capture, replace refs disabled) — never from foreign file
    contents.  A malformed or oversized listing fails closed.
    """
    raw = _git_bytes(
        root, ["ls-files", "-z"], maximum=TRACKED_LIST_MAX
    )
    if len(raw) > TRACKED_LIST_MAX:
        raise MigrationUnavailableError("the tracked-file listing is oversized")
    return tuple(
        path for path in raw.decode("utf-8", "replace").split("\0") if path
    )


def _is_forbidden_tracked(relpath: str) -> bool:
    """True when one tracked name is a forbidden post-removal pathname."""
    if relpath in FORBIDDEN_TRACKED_EXACT:
        return True
    if relpath in FORBIDDEN_OLD_PROMPTS:
        return True
    if any(relpath.startswith(prefix) for prefix in FORBIDDEN_TRACKED_PREFIXES):
        return True
    for glob in FORBIDDEN_TRACKED_GLOBS:
        prefix, _, suffix = glob.partition("*")
        if relpath.startswith(prefix) and relpath.endswith(suffix):
            return True
    return False


def forbidden_tracked(root) -> Tuple[str, ...]:
    """Every forbidden legacy pathname that is still tracked (sorted)."""
    return tuple(
        sorted(path for path in _tracked_files(root) if _is_forbidden_tracked(path))
    )


# ---------------------------------------------------------------------------
# Freeze-marker status
# ---------------------------------------------------------------------------


def freeze_marker_status(root) -> Dict[str, object]:
    """Machine-readable status of the tracked freeze marker.

    ``safe`` is true exactly when the marker is tracked, present, a regular
    non-symlink single-link file owned by the invoking user, with no
    group/other write bits and no execute bits.  An unsafe marker is
    reported (never silently blessed); the ``verify`` command fails on it.
    """
    root = _as_root(root)
    tracked = FREEZE_MARKER_RELPATH in _tracked_files(root)
    marker = root / FREEZE_MARKER_RELPATH
    try:
        info = os.lstat(marker)
    except FileNotFoundError:
        return {
            "tracked": tracked,
            "present": False,
            "regular": False,
            "executable": False,
            "safe": False,
        }
    except OSError as exc:
        raise MigrationUnavailableError(
            f"cannot inspect the freeze marker {marker}: {exc}"
        ) from exc
    regular = stat.S_ISREG(info.st_mode) and not stat.S_ISLNK(info.st_mode)
    executable = bool(info.st_mode & 0o111)
    safe = (
        regular
        and not executable
        and info.st_uid == os.getuid()
        and info.st_nlink == 1
        and not (info.st_mode & 0o022)
    )
    return {
        "tracked": tracked,
        "present": True,
        "regular": regular,
        "executable": executable,
        "safe": safe,
    }


# ---------------------------------------------------------------------------
# Canonical plan/spec/roles status
# ---------------------------------------------------------------------------


def _plan_status(root: Path) -> Dict[str, object]:
    """Status of the canonical plan, its spec binding, and the role prompts.

    The committed plan must parse against the committed requirement-policy
    registry; its spec binding (``spec_path``/``spec_commit``/``spec_blob``)
    must resolve to the exact committed spec blob at the snapshot head; and
    every canonical role prompt must be tracked.  Every blob read is
    exact-size-pre-checked and hard-bounded (never an unbounded capture).
    """
    root = _as_root(root)
    head = _head_commit(root)
    tracked = _tracked_files(root)
    plan_tracked = DEFAULT_PLAN_PATH in tracked
    roles: Dict[str, bool] = {}
    for role in ROLE_PROMPTS:
        roles[role] = f".factory/prompts/{role}.md" in tracked
    plan: Dict[str, object] = {
        "path": DEFAULT_PLAN_PATH,
        "tracked": plan_tracked,
        "parses": False,
        "digest": None,
        "spec_path": None,
        "spec_commit": None,
        "spec_blob": None,
        "base_commit": None,
        "spec_current": False,
        "error": None,
    }
    if plan_tracked:
        try:
            plan_blob = _committed_blob_id(
                root, head, DEFAULT_PLAN_PATH, "plan"
            )
            raw = _git_blob_bounded(root, plan_blob, PLAN_BLOB_MAX, "plan")
            parsed = plan_parser.Plan.from_bytes(raw)
        except (MigrationUnavailableError, plan_parser.PlanError) as exc:
            plan["error"] = str(exc)
            return {"plan": plan, "roles": roles}
        plan["parses"] = True
        plan["digest"] = hashlib.sha256(raw).hexdigest()
        plan["spec_path"] = parsed.spec_path
        plan["spec_commit"] = parsed.spec_commit
        plan["spec_blob"] = parsed.spec_blob
        plan["base_commit"] = parsed.base_commit
        # The spec binding is current when the committed spec blob at the
        # snapshot head is exactly the blob the plan binds.
        try:
            spec_blob = _committed_blob_id(
                root, head, parsed.spec_path, "specification"
            )
            plan["spec_current"] = spec_blob == parsed.spec_blob
        except MigrationUnavailableError:
            plan["spec_current"] = False
    return {
        "plan": plan,
        "roles": roles,
    }


# ---------------------------------------------------------------------------
# Aggregate verification
# ---------------------------------------------------------------------------


def verify_migration(root) -> Dict[str, object]:
    """The deterministic post-migration verification report.

    The report confirms (1) tracked absence of every forbidden legacy
    pathname, (2) a safe non-executable regular freeze marker, (3) a current
    canonical plan/spec/roles binding, and (4) the presence-only status of
    the foreign ``.ralph/`` entry (lstat only, never enumerated/opened/read).
    ``ok`` is true exactly when every check passes.
    """
    root = _as_root(root)
    forbidden = forbidden_tracked(root)
    marker = freeze_marker_status(root)
    plan_status = _plan_status(root)
    plan = plan_status["plan"]
    roles = plan_status["roles"]
    ok = (
        not forbidden
        and bool(marker["safe"])
        and bool(plan["tracked"])
        and bool(plan["parses"])
        and bool(plan["spec_current"])
        and all(roles.values())
    )
    return {
        "schema": MIGRATION_SCHEMA,
        "command": "verify",
        "ok": ok,
        "head_commit": _head_commit(root),
        "tracked_forbidden": list(forbidden),
        "freeze_marker": marker,
        "plan": plan,
        "roles": roles,
        "ralph_presence": ralph_presence(root),
    }


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="factory-migration",
        description=(
            "Post-migration authority (FACTORY-LOOP-SPEC §21; MIG-01). "
            "Confirms the Ralph control plane is removed: tracked absence of "
            "the forbidden legacy pathnames, a safe non-executable regular "
            "freeze marker, and a current canonical plan/spec/roles binding. "
            "The foreign .ralph/ entry is lstat'ed for presence only and "
            "never opened, enumerated, or read."
        ),
    )
    parser.add_argument(
        "--root",
        metavar="ROOT",
        default=str(Path(__file__).resolve().parent.parent.parent),
        help="canonical repository root (default: this repository)",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_status = sub.add_parser(
        "status", help="print the post-migration verification report"
    )

    p_verify = sub.add_parser(
        "verify",
        help="exit 0 when the migration is complete and current, else 2",
    )

    p_freeze = sub.add_parser(
        "freeze", help="print whether the freeze marker is a safe regular file"
    )
    p_freeze.add_argument(
        "--guard",
        action="store_true",
        help=(
            "exit-code freeze guard: exit 0 when the marker is a safe "
            "non-executable regular file, exit 1 when it is missing, exit 2 "
            "when the marker is unsafe or the authority is unavailable "
            "(fail closed)"
        ),
    )

    args = parser.parse_args(argv)
    root = Path(args.root)
    try:
        if args.command == "status":
            print(
                json.dumps(verify_migration(root), sort_keys=True, indent=2)
            )
            return 0
        if args.command == "verify":
            report = verify_migration(root)
            print(
                json.dumps(report, sort_keys=True, indent=2)
            )
            return 0 if report["ok"] else 2
        if args.command == "freeze":
            # The decision is a single no-follow re-stat in this process
            # (never a cached ``[ -f ]`` shell check): safe regular marker ->
            # 0, missing marker -> 1, unsafe/unavailable -> 2 (fail closed).
            if args.guard:
                if not is_ralph_frozen(root):
                    return 1
                return 0
            print("frozen" if is_ralph_frozen(root) else "not-frozen")
            return 0
    except MigrationError as exc:
        print(f"factory-migration: {exc}", file=sys.stderr)
        return 2
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
