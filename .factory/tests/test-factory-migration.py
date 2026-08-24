#!/usr/bin/env python3
"""Hidden post-migration conformance suite (Task 15; MIG-01, §21).

This test lives under the hidden ``.factory/tests/`` namespace (HIDE-01, §3).
It is the deterministic verification for the post-migration authority
(``.factory/loop/migration.py``) after the Ralph Orchestrator control plane
was removed from the tracked tree — not deprecated, not frozen behind
forwarders:

* **tracked absence** — the forbidden legacy pathnames (``.ralph/**``,
  ``.factory/ralph/**``, the old role prompts and the stale context-summary
  mirror, ``scripts/ralph-*``, the Pi Ralph shim/extension, and the Ralph
  tests) are enumerated from the pinned bounded ``git ls-files -z`` listing
  and any planted tracked forbidden path fails the ``verify`` command;
* **freeze marker** — the tracked ``.factory/ralph-freeze`` marker must be a
  safe non-executable regular file: a symlink, FIFO, socket, device,
  executable, foreign-owned, hardlinked, or group/other-writable marker
  fails closed (never silently unfreezes);
* **canonical plan/spec/roles current** — the committed plan parses against
  the committed requirement-policy registry, its spec binding resolves to
  the exact committed spec blob at the snapshot head, and every canonical
  role prompt is tracked; every committed blob is exact-size-pre-checked
  against its per-kind cap before any body byte is read;
* **foreign ``.ralph/`` preservation** — the migration may ``lstat`` the
  repository's ``.ralph/`` entry for presence only; it never enumerates,
  opens, or reads anything under it.  A probe instruments every
  ``os.open``/``builtins.open``/``os.read`` in the process and fails the
  test on any ``.ralph`` open, while synthetic sentinel bytes prove the
  foreign directory's bytes, mode, and mtime are preserved exactly and
  never reach a report.

There is no context-summary authority and no launcher-compatibility
requirement: the new control plane never invokes, freezes, or names a
deprecated Ralph launcher, and the stale context-summary mirror is simply
one of the forbidden tracked pathnames whose absence is confirmed.

The suite never touches the live repository's ``.ralph/`` directory beyond
the same ``lstat``-only presence check the authority itself performs.  All
repositories are test-owned temporary Git fixtures; live-repo checks are
read-only (CLI status/freeze inspection).
"""

from __future__ import annotations

import builtins
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import stat
import subprocess
import sys
import tempfile
import time
import unittest
import unittest.mock as mock

ROOT = Path(__file__).resolve().parents[2]
LOOP = ROOT / ".factory" / "loop"
MIGRATION_SCRIPT = LOOP / "migration.py"

sys.path.insert(0, str(LOOP))
import gitutil  # noqa: E402  (PATH-pinned Git runner; also used by fixtures)
import migration  # noqa: E402
import plan_parser  # noqa: E402

# Synthetic secret markers: never real credentials.  Their presence in any
# report, log, or stdout proves a foreign ``.ralph`` byte leaked.
RALPH_SECRET = b"RALPH-FOREIGN-SENTINEL-9f4c1a"

# The canonical plan template: the committed plan-parser fixture that parses
# against the committed requirement-policy registry (76 stable IDs).  The
# fixture binds its front matter to the real spec blob at commit time.
PLAN_TEMPLATE = (
    ROOT / ".factory" / "tests" / "fixtures" / "plan-valid-base.md"
).read_text(encoding="utf-8")

FREEZE_MARKER = ".factory/ralph-freeze"
PLAN_REL = ".factory/artifacts/implementation-plan.md"
SPEC_REL = "docs/SPEC.md"


def _git(root: Path, *argv: str) -> str:
    result = gitutil.git_run(["-C", str(root), *argv], timeout=120.0)
    if result.returncode != 0:
        raise AssertionError(
            f"fixture git {argv!r} failed: {result.stderr.strip()}"
        )
    return result.stdout


def _git_bytes(root: Path, *argv: str) -> bytes:
    result = gitutil.git_bytes(["-C", str(root), *argv], timeout=120.0)
    if result.returncode != 0:
        raise AssertionError(
            f"fixture git {argv!r} failed: {result.stderr.strip()}"
        )
    return result.stdout


class FixtureRepo:
    """A test-owned temporary Git repository with a valid committed plan.

    ``plant_forbidden`` maps a repository-relative forbidden pathname to
    content that is committed *with* the baseline, so a test can prove a
    planted tracked forbidden path fails the migration ``verify``.  The
    foreign ``.ralph/`` directory is created on disk (git-ignored) with
    synthetic sentinel bytes and is never committed.
    """

    def __init__(self, testcase: unittest.TestCase, *,
                 plant_forbidden: dict[str, str] | None = None,
                 seed_ralph: bool = True):
        tmp = tempfile.TemporaryDirectory(prefix="factory-migration-")
        testcase.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.plant_forbidden = plant_forbidden or {}
        _git(self.root, "init", "-q", "-b", "boilerplate-develop")
        self._write_baseline()
        self._commit_all("baseline harness")
        head = _git(self.root, "rev-parse", "HEAD").strip()
        self._bind_plan(head)
        self._commit_all("bind the canonical plan to the spec blob")
        if seed_ralph:
            self._seed_ralph()

    # -- construction -------------------------------------------------------
    def _write_baseline(self) -> None:
        root = self.root
        (root / "docs").mkdir(parents=True)
        (root / ".factory" / "artifacts").mkdir(parents=True)
        (root / ".factory" / "prompts").mkdir(parents=True)
        (root / ".gitignore").write_text(
            ".ralph/\n.factory-state/\n", encoding="utf-8"
        )
        (root / "docs" / "SPEC.md").write_bytes(
            b"# Fixture specification\n\nDeterministic spec bytes.\n"
        )
        (root / "product.txt").write_bytes(b"product baseline\n")
        for role in ("planner", "developer", "tester", "auditor"):
            (root / ".factory" / "prompts" / f"{role}.md").write_text(
                f"# {role} fixture prompt\n"
            )
        (root / FREEZE_MARKER).write_text("# frozen\n", encoding="utf-8")
        (root / PLAN_REL).write_text(PLAN_TEMPLATE, encoding="utf-8")
        for rel, content in self.plant_forbidden.items():
            path = root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
            # Forbidden pathnames under git-ignored namespaces (``.ralph/``)
            # are force-added so the planted path is genuinely tracked.
            _git(root, "add", "-f", "--", rel)

    def _commit_all(self, message: str) -> None:
        _git(self.root, "add", "-A")
        _git(
            self.root,
            "-c", "user.name=migration-test",
            "-c", "user.email=migration-test@example.invalid",
            "commit", "-q", "-m", message,
        )

    def _bind_plan(self, head: str) -> None:
        spec_blob = _git(self.root, "rev-parse", f"HEAD:docs/SPEC.md").strip()
        plan_path = self.root / PLAN_REL
        text = plan_path.read_text(encoding="utf-8")
        text = re.sub(
            r"^spec_commit: .*$",
            f"spec_commit: {head}",
            text, count=1, flags=re.M,
        )
        text = re.sub(
            r"^spec_blob: .*$",
            f"spec_blob: {spec_blob}",
            text, count=1, flags=re.M,
        )
        text = re.sub(
            r"^base_commit: .*$",
            f"base_commit: {head}",
            text, count=1, flags=re.M,
        )
        plan_path.write_text(text, encoding="utf-8")

    def _seed_ralph(self) -> None:
        ralph = self.root / ".ralph"
        (ralph / "agent").mkdir(parents=True, exist_ok=True)
        (ralph / "agent" / "tasks.jsonl").write_bytes(
            b"RLP-" + RALPH_SECRET + b"\n"
        )
        (ralph / "current-events").write_bytes(RALPH_SECRET + b"\n")

    # -- helpers ------------------------------------------------------------
    def git(self, *argv: str) -> str:
        return _git(self.root, *argv)

    def head(self) -> str:
        return self.git("rev-parse", "HEAD").strip()

    def plan_blob(self) -> bytes:
        blob = self.git("rev-parse", f"HEAD:{PLAN_REL}").strip()
        return self._git_bytes("cat-file", "blob", blob)

    def _git_bytes(self, *argv: str) -> bytes:
        return _git_bytes(self.root, *argv)

    def commit_forbidden(self, rel: str, content: str = "planted\n") -> None:
        """Commit one forbidden pathname on top of the clean baseline.

        Forbidden pathnames under git-ignored namespaces (``.ralph/``) are
        force-added so the planted path is genuinely tracked.
        """
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        _git(self.root, "add", "-f", "--", rel)
        self._commit_all(f"plant forbidden path {rel}")


# ---------------------------------------------------------------------------
# Foreign .ralph read-isolation probe
# ---------------------------------------------------------------------------


class OpenReadProbe:
    """Records every ``os.open``/``builtins.open``/``os.read`` and asserts the
    migration never opens the foreign ``.ralph/`` namespace.

    ``dir_fd``-based paths are resolved through ``/proc/self/fd`` so any
    anchored open is reported as the real path.  Opening a ``.ralph`` path
    raises immediately — the test then also inspects the recorded trail for
    diagnostics.
    """

    def __init__(self, root: Path):
        self.root = os.path.abspath(str(root))
        self.opens: list[str] = []
        self.reads: list[tuple[int, str]] = []
        self._fd_to_path: dict[int, str] = {}
        self._patchers = []

    def _resolve(self, path: object, dir_fd: int | None = None) -> str:
        if isinstance(path, (bytes, bytearray)):
            path = os.fsdecode(bytes(path))
        elif isinstance(path, os.PathLike):
            path = os.fspath(path)
        if dir_fd is not None:
            try:
                base = os.readlink(f"/proc/self/fd/{dir_fd}")
            except OSError:
                base = f"<fd:{dir_fd}>"
            path = os.path.join(base, path)
        return os.path.abspath(path)

    def _forbidden(self, resolved: str) -> bool:
        root = self.root
        if resolved == os.path.join(root, ".ralph"):
            return True
        return resolved.startswith(os.path.join(root, ".ralph") + os.sep)

    def install(self) -> "OpenReadProbe":
        probe = self
        real_open = os.open
        real_builtin_open = builtins.open
        real_read = os.read

        def fake_open(path, *args, **kwargs):
            resolved = probe._resolve(path, kwargs.get("dir_fd"))
            probe.opens.append(resolved)
            if probe._forbidden(resolved):
                raise AssertionError(f"forbidden .ralph open/read: {resolved}")
            fd = real_open(path, *args, **kwargs)
            flags = args[1] if len(args) >= 2 else kwargs.get("flags", 0)
            if not (flags & (os.O_DIRECTORY | os.O_PATH)):
                probe._fd_to_path[fd] = resolved
            return fd

        def fake_builtin_open(path, *args, **kwargs):
            resolved = probe._resolve(path)
            probe.opens.append(resolved)
            if probe._forbidden(resolved):
                raise AssertionError(f"forbidden .ralph open/read: {resolved}")
            handle = real_builtin_open(path, *args, **kwargs)
            probe._fd_to_path[handle.fileno()] = resolved
            return handle

        def fake_read(fd, n):
            probe.reads.append((fd, probe._fd_to_path.get(fd, "<unknown>")))
            return real_read(fd, n)

        # Patching ``os.open`` moves it out of the cached ``os.supports_dir_fd``
        # frozenset; re-register the patched open in a copied set so any
        # dir_fd-capable authority still sees the primitive available.
        supported = frozenset(os.supports_dir_fd) | {fake_open}
        self._patchers = [
            mock.patch.object(os, "open", fake_open),
            mock.patch.object(os, "supports_dir_fd", supported),
            mock.patch.object(builtins, "open", fake_builtin_open),
            mock.patch.object(os, "read", fake_read),
        ]
        for patcher in self._patchers:
            patcher.start()
        return self

    def restore(self) -> None:
        for patcher in reversed(self._patchers):
            patcher.stop()

    def assert_no_ralph_access(self, testcase: unittest.TestCase) -> None:
        forbidden = [path for path in self.opens if self._forbidden(path)]
        testcase.assertEqual(
            forbidden, [], f"foreign .ralph opened: {forbidden}"
        )
        testcase.assertFalse(
            any(self._forbidden(path) for _, path in self.reads),
            "foreign .ralph read through a descriptor",
        )


# ---------------------------------------------------------------------------
# Clean-fixture verification
# ---------------------------------------------------------------------------


class CleanFixtureVerifyTest(unittest.TestCase):
    def test_verify_clean_fixture_passes(self) -> None:
        fx = FixtureRepo(self)
        report = migration.verify_migration(fx.root)
        self.assertTrue(report["ok"], json.dumps(report, indent=2))
        self.assertEqual(report["schema"], "factory-migration/v1")
        self.assertEqual(report["tracked_forbidden"], [])
        self.assertTrue(report["freeze_marker"]["safe"])
        self.assertTrue(report["plan"]["tracked"])
        self.assertTrue(report["plan"]["parses"])
        self.assertTrue(report["plan"]["spec_current"])
        self.assertTrue(all(report["roles"].values()))
        self.assertTrue(report["ralph_presence"]["present"])

    def test_verify_cli_exit_zero_on_clean_fixture(self) -> None:
        fx = FixtureRepo(self)
        result = subprocess.run(
            [sys.executable, str(MIGRATION_SCRIPT), "--root", str(fx.root),
             "verify"],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertTrue(payload["ok"])

    def test_status_cli_reports_tracked_absence(self) -> None:
        fx = FixtureRepo(self)
        result = subprocess.run(
            [sys.executable, str(MIGRATION_SCRIPT), "--root", str(fx.root),
             "status"],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["schema"], "factory-migration/v1")
        self.assertEqual(payload["tracked_forbidden"], [])
        self.assertTrue(payload["freeze_marker"]["safe"])
        self.assertTrue(payload["ralph_presence"]["present"])

    def test_plan_spec_roles_current(self) -> None:
        fx = FixtureRepo(self)
        report = migration.verify_migration(fx.root)
        plan = report["plan"]
        self.assertEqual(plan["path"], PLAN_REL)
        self.assertEqual(plan["spec_path"], SPEC_REL)
        self.assertTrue(re.fullmatch(r"[0-9a-f]{40}", plan["spec_commit"]))
        self.assertTrue(re.fullmatch(r"[0-9a-f]{40}", plan["spec_blob"]))
        self.assertTrue(re.fullmatch(r"[0-9a-f]{64}", plan["digest"]))
        # The digest is the exact committed plan bytes.
        self.assertEqual(
            plan["digest"], hashlib.sha256(fx.plan_blob()).hexdigest()
        )
        self.assertEqual(
            set(report["roles"]),
            {"planner", "developer", "tester", "auditor"},
        )

    def test_verify_never_opens_ralph(self) -> None:
        fx = FixtureRepo(self)
        probe = OpenReadProbe(fx.root).install()
        self.addCleanup(probe.restore)
        report = migration.verify_migration(fx.root)
        self.assertTrue(report["ok"])
        probe.assert_no_ralph_access(self)

    def test_report_contains_no_foreign_ralph_bytes(self) -> None:
        fx = FixtureRepo(self)
        report = migration.verify_migration(fx.root)
        raw = json.dumps(report, sort_keys=True).encode("utf-8")
        self.assertNotIn(RALPH_SECRET, raw)
        result = subprocess.run(
            [sys.executable, str(MIGRATION_SCRIPT), "--root", str(fx.root),
             "status"],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(RALPH_SECRET, result.stdout.encode("utf-8"))
        self.assertNotIn(RALPH_SECRET, result.stderr.encode("utf-8"))


# ---------------------------------------------------------------------------
# Planted tracked forbidden pathnames fail the verify
# ---------------------------------------------------------------------------


class ForbiddenTrackedTest(unittest.TestCase):
    FORBIDDEN_CASES = (
        (".ralph/agent/tasks.jsonl", "legacy runtime namespace"),
        (".factory/ralph/plan.yml", "legacy plan YAML"),
        (".factory/prompts/plan.md", "old role prompt"),
        (".factory/prompts/audit.md", "old role prompt"),
        (".factory/artifacts/context-summary.md", "stale context-summary"),
        ("scripts/ralph-run.sh", "deprecated launcher"),
        ("scripts/ralph-campaign.sh", "deprecated launcher"),
        ("scripts/pi-cli-shims/ralph", "Pi Ralph shim"),
        ("scripts/pi-ralph-emit-extension.mjs", "Pi Ralph emit extension"),
        ("tests/test-ralph-campaign.sh", "Ralph test"),
        ("tests/test-context-summary.sh", "context-summary test"),
    )

    def test_planted_tracked_forbidden_path_rejects(self) -> None:
        for rel, label in self.FORBIDDEN_CASES:
            with self.subTest(rel=rel):
                fx = FixtureRepo(self)
                fx.commit_forbidden(rel)
                report = migration.verify_migration(fx.root)
                self.assertFalse(report["ok"], f"{label} must fail verify")
                self.assertIn(rel, report["tracked_forbidden"])
                result = subprocess.run(
                    [sys.executable, str(MIGRATION_SCRIPT),
                     "--root", str(fx.root), "verify"],
                    capture_output=True, text=True,
                )
                self.assertEqual(result.returncode, 2,
                                 f"{label} must fail the verify CLI")
                self.assertIn(rel, result.stdout)

    def test_planted_forbidden_committed_with_baseline_rejects(self) -> None:
        # A forbidden path committed with the baseline (not added later) is
        # rejected the same way.
        fx = FixtureRepo(
            self, plant_forbidden={".ralph/agent/tasks.jsonl": "[]"}
        )
        report = migration.verify_migration(fx.root)
        self.assertFalse(report["ok"])
        self.assertIn(".ralph/agent/tasks.jsonl", report["tracked_forbidden"])

    def test_clean_fixture_has_no_forbidden_tracked(self) -> None:
        fx = FixtureRepo(self)
        self.assertEqual(migration.forbidden_tracked(fx.root), ())


# ---------------------------------------------------------------------------
# Freeze-marker safety: symlink/FIFO/socket/device/executable/owner/mode/link
# ---------------------------------------------------------------------------


class FreezeMarkerSafetyTest(unittest.TestCase):
    def _marker_root(self, kind: str):
        """A test-owned root whose freeze marker is ``kind`` (or missing)."""
        tmp = tempfile.TemporaryDirectory(prefix="factory-migration-marker-")
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        (root / ".factory").mkdir(parents=True)
        marker = root / ".factory" / "ralph-freeze"
        if kind == "missing":
            return root
        if kind == "regular":
            marker.write_text("# frozen\n", encoding="utf-8")
            return root
        if kind == "executable":
            marker.write_text("#!/bin/sh\n", encoding="utf-8")
            marker.chmod(0o755)
            return root
        if kind == "group-writable":
            marker.write_text("# frozen\n", encoding="utf-8")
            marker.chmod(0o664)
            return root
        if kind == "hardlink":
            marker.write_text("# frozen\n", encoding="utf-8")
            os.link(marker, root / ".factory" / "marker-link")
            return root
        if kind == "symlink":
            marker.symlink_to(root / "docs")
        elif kind == "fifo":
            os.mkfifo(marker)
        elif kind == "socket":
            sock = socket.socket(socket.AF_UNIX)
            self.addCleanup(sock.close)
            sock.bind(str(marker))
        elif kind == "device":
            os.mknod(str(marker), 0o600 | stat.S_IFCHR, os.makedev(1, 3))
        else:
            raise AssertionError(f"unknown marker kind {kind!r}")
        return root

    def test_regular_marker_is_frozen(self) -> None:
        self.assertTrue(migration.is_ralph_frozen(self._marker_root("regular")))

    def test_missing_marker_is_not_frozen(self) -> None:
        self.assertFalse(migration.is_ralph_frozen(self._marker_root("missing")))

    def test_symlink_marker_fails_closed(self) -> None:
        with self.assertRaises(migration.MigrationUnavailableError):
            migration.is_ralph_frozen(self._marker_root("symlink"))

    def test_fifo_marker_fails_closed(self) -> None:
        with self.assertRaises(migration.MigrationUnavailableError):
            migration.is_ralph_frozen(self._marker_root("fifo"))

    def test_socket_marker_fails_closed(self) -> None:
        with self.assertRaises(migration.MigrationUnavailableError):
            migration.is_ralph_frozen(self._marker_root("socket"))

    def test_device_marker_fails_closed_or_declares_unavailability(self) -> None:
        try:
            root = self._marker_root("device")
        except OSError as exc:
            # mknod of a device node needs root or a device-capable
            # filesystem; declare the exact unavailability and never claim
            # device-node coverage.
            self.assertIn(exc.errno, (errno.EPERM, errno.EACCES, errno.EINVAL))
            return
        with self.assertRaises(migration.MigrationUnavailableError):
            migration.is_ralph_frozen(root)

    def test_executable_marker_fails_closed(self) -> None:
        with self.assertRaises(migration.MigrationUnavailableError) as caught:
            migration.is_ralph_frozen(self._marker_root("executable"))
        self.assertIn("executable", str(caught.exception))

    def test_group_writable_marker_fails_closed(self) -> None:
        with self.assertRaises(migration.MigrationUnavailableError) as caught:
            migration.is_ralph_frozen(self._marker_root("group-writable"))
        self.assertIn("group/other-writable", str(caught.exception))

    def test_hardlinked_marker_fails_closed(self) -> None:
        with self.assertRaises(migration.MigrationUnavailableError) as caught:
            migration.is_ralph_frozen(self._marker_root("hardlink"))
        self.assertIn("single-link", str(caught.exception))

    def test_wrong_owner_marker_fails_closed_deterministically(self) -> None:
        """The owner-rejection branch runs without chown (private hook)."""
        root = self._marker_root("regular")
        marker = root / ".factory" / "ralph-freeze"
        info = os.lstat(marker)
        with self.assertRaises(migration.MigrationUnavailableError) as caught:
            migration._validate_marker_info(
                os.stat_result(
                    (info.st_mode, info.st_ino, info.st_dev, info.st_nlink,
                     os.getuid() + 1, info.st_gid, info.st_size,
                     info.st_atime, info.st_mtime, info.st_ctime)
                ),
                marker,
            )
        self.assertIn("not owned", str(caught.exception))

    def test_unsafe_marker_fails_verify(self) -> None:
        for kind in ("symlink", "fifo", "executable", "group-writable"):
            with self.subTest(kind=kind):
                fx = FixtureRepo(self)
                marker = fx.root / FREEZE_MARKER
                marker.unlink()
                if kind == "symlink":
                    marker.symlink_to(fx.root / "docs")
                elif kind == "fifo":
                    os.mkfifo(marker)
                elif kind == "executable":
                    marker.write_text("#!/bin/sh\n", encoding="utf-8")
                    marker.chmod(0o755)
                elif kind == "group-writable":
                    marker.write_text("# frozen\n", encoding="utf-8")
                    marker.chmod(0o664)
                report = migration.verify_migration(fx.root)
                self.assertFalse(report["ok"])
                self.assertFalse(report["freeze_marker"]["safe"])

    def test_missing_marker_fails_verify(self) -> None:
        fx = FixtureRepo(self)
        (fx.root / FREEZE_MARKER).unlink()
        report = migration.verify_migration(fx.root)
        self.assertFalse(report["ok"])
        self.assertFalse(report["freeze_marker"]["safe"])
        self.assertFalse(report["freeze_marker"]["present"])

    def test_freeze_guard_exit_table(self) -> None:
        """0 frozen / 1 not frozen / 2 unsafe, through the retained authority."""
        for kind, expected in (("missing", 1), ("regular", 0),
                               ("symlink", 2), ("fifo", 2)):
            with self.subTest(kind=kind):
                root = self._marker_root(kind)
                result = subprocess.run(
                    [sys.executable, str(MIGRATION_SCRIPT), "--root",
                     str(root), "freeze", "--guard"],
                    capture_output=True, text=True,
                )
                self.assertEqual(result.returncode, expected,
                                 f"{kind} marker guard exit")

    def test_freeze_cli_fails_closed_on_unsafe_marker(self) -> None:
        root = self._marker_root("fifo")
        result = subprocess.run(
            [sys.executable, str(MIGRATION_SCRIPT), "--root", str(root),
             "freeze"],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("regular file", result.stderr)

    def test_live_repo_marker_is_safe(self) -> None:
        self.assertTrue(migration.is_ralph_frozen(ROOT))
        marker = ROOT / FREEZE_MARKER
        info = marker.lstat()
        self.assertTrue(stat.S_ISREG(info.st_mode))
        self.assertFalse(stat.S_ISLNK(info.st_mode))
        self.assertFalse(info.st_mode & 0o111)


# ---------------------------------------------------------------------------
# Foreign .ralph preservation: sentinel bytes/mode/mtime untouched
# ---------------------------------------------------------------------------


class RalphPreservationTest(unittest.TestCase):
    def test_ralph_sentinel_bytes_mode_mtime_preserved(self) -> None:
        fx = FixtureRepo(self)
        sentinel = fx.root / ".ralph" / "agent" / "tasks.jsonl"
        before_bytes = sentinel.read_bytes()
        before_stat = os.lstat(sentinel)
        before = (
            before_bytes,
            before_stat.st_mode,
            before_stat.st_mtime_ns,
            before_stat.st_uid,
            before_stat.st_nlink,
        )
        report = migration.verify_migration(fx.root)
        self.assertTrue(report["ok"])
        result = subprocess.run(
            [sys.executable, str(MIGRATION_SCRIPT), "--root", str(fx.root),
             "status"],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        after_stat = os.lstat(sentinel)
        after = (
            sentinel.read_bytes(),
            after_stat.st_mode,
            after_stat.st_mtime_ns,
            after_stat.st_uid,
            after_stat.st_nlink,
        )
        self.assertEqual(after, before,
                         "the foreign .ralph sentinel must be preserved exactly")

    def test_ralph_presence_reported_without_enumeration(self) -> None:
        fx = FixtureRepo(self)
        report = migration.verify_migration(fx.root)
        self.assertTrue(report["ralph_presence"]["present"])
        # The report carries presence only — never a path, size, or byte
        # under .ralph/.
        raw = json.dumps(report, sort_keys=True)
        self.assertNotIn(".ralph/agent", raw)
        self.assertNotIn("tasks.jsonl", raw)

    def test_absent_ralph_reported_absent(self) -> None:
        fx = FixtureRepo(self, seed_ralph=False)
        report = migration.verify_migration(fx.root)
        self.assertTrue(report["ok"])
        self.assertFalse(report["ralph_presence"]["present"])


# ---------------------------------------------------------------------------
# Committed-authority hardening: bounded reads, plan/spec/roles currency
# ---------------------------------------------------------------------------


class CommittedAuthorityHardeningTest(unittest.TestCase):
    def test_oversized_plan_fails_before_body_read(self) -> None:
        fx = FixtureRepo(self)
        plan_path = fx.root / PLAN_REL
        plan_path.write_bytes(
            PLAN_TEMPLATE.encode("utf-8")
            + b"#" * (migration.PLAN_BLOB_MAX + 1)
        )
        fx._commit_all("oversized plan")
        recorded: list = []
        real = migration._git_bytes

        def recording(root, argv, *, maximum=None):
            recorded.append(list(argv))
            return real(root, argv, maximum=maximum)

        with mock.patch.object(
            migration, "_git_bytes", side_effect=recording
        ):
            report = migration.verify_migration(fx.root)
        # The oversized plan fails the verify closed (never accepted), and
        # the failure is reported as a plan error, not a silent pass.
        self.assertFalse(report["ok"])
        self.assertFalse(report["plan"]["parses"])
        self.assertIn("per-kind cap", report["plan"]["error"])
        plan_blob = fx.git("rev-parse", f"HEAD:{PLAN_REL}").strip()
        self.assertTrue(re.fullmatch(r"[0-9a-f]{40}", plan_blob))
        self.assertNotIn(
            ["cat-file", "blob", plan_blob], recorded,
            "the plan body was read before the exact-size per-kind pre-check",
        )

    def test_plan_missing_fails_verify(self) -> None:
        fx = FixtureRepo(self)
        (fx.root / PLAN_REL).unlink()
        fx._commit_all("remove the plan")
        report = migration.verify_migration(fx.root)
        self.assertFalse(report["ok"])
        self.assertFalse(report["plan"]["tracked"])

    def test_unparseable_plan_fails_verify(self) -> None:
        fx = FixtureRepo(self)
        (fx.root / PLAN_REL).write_text(
            "---\nspec_path: docs/SPEC.md\n---\n# broken\n",
            encoding="utf-8",
        )
        fx._commit_all("broken plan")
        report = migration.verify_migration(fx.root)
        self.assertFalse(report["ok"])
        self.assertFalse(report["plan"]["parses"])
        self.assertIsNotNone(report["plan"]["error"])

    def test_spec_mismatch_fails_verify(self) -> None:
        # The plan binds a spec blob that is not the committed spec at the
        # snapshot head: the binding is stale and the verify fails.
        fx = FixtureRepo(self)
        plan_path = fx.root / PLAN_REL
        text = plan_path.read_text(encoding="utf-8")
        text = re.sub(
            r"^spec_blob: .*$",
            "spec_blob: " + "a" * 40,
            text, count=1, flags=re.M,
        )
        plan_path.write_text(text, encoding="utf-8")
        fx._commit_all("stale spec binding")
        report = migration.verify_migration(fx.root)
        self.assertFalse(report["ok"])
        self.assertFalse(report["plan"]["spec_current"])

    def test_role_prompt_missing_fails_verify(self) -> None:
        for role in ("planner", "developer", "tester", "auditor"):
            with self.subTest(role=role):
                fx = FixtureRepo(self)
                (fx.root / ".factory" / "prompts" / f"{role}.md").unlink()
                fx._commit_all(f"remove {role} prompt")
                report = migration.verify_migration(fx.root)
                self.assertFalse(report["ok"])
                self.assertFalse(report["roles"][role])

    def test_verify_cli_exit_two_on_regression(self) -> None:
        fx = FixtureRepo(self)
        fx.commit_forbidden("scripts/ralph-run.sh")
        result = subprocess.run(
            [sys.executable, str(MIGRATION_SCRIPT), "--root", str(fx.root),
             "verify"],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("scripts/ralph-run.sh", result.stdout)


# ---------------------------------------------------------------------------
# Live-repo smoke: the status/freeze CLIs run against the real repository
# ---------------------------------------------------------------------------


class LiveRepoSmokeTest(unittest.TestCase):
    def test_live_repo_migration_checks(self) -> None:
        report = migration.verify_migration(ROOT)
        # The Ralph removal is complete: no forbidden pathname is tracked and
        # the freeze marker is a safe non-executable regular file.
        self.assertEqual(report["tracked_forbidden"], [])
        self.assertTrue(report["freeze_marker"]["safe"])
        self.assertTrue(report["freeze_marker"]["tracked"])
        self.assertTrue(report["freeze_marker"]["present"])
        self.assertFalse(report["freeze_marker"]["executable"])
        # The foreign .ralph/ entry is reported by presence only.
        self.assertIn("ralph_presence", report)
        self.assertIsInstance(report["ralph_presence"]["present"], bool)

    def test_live_repo_status_cli_is_read_only(self) -> None:
        ralph = ROOT / ".ralph"
        ralph_before = None
        if ralph.exists():
            sentinel = ralph / "agent" / "tasks.jsonl"
            if sentinel.exists():
                info = os.lstat(sentinel)
                ralph_before = (
                    sentinel.read_bytes(),
                    info.st_mode,
                    info.st_mtime_ns,
                )
        result = subprocess.run(
            [sys.executable, str(MIGRATION_SCRIPT), "--root", str(ROOT),
             "status"],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["schema"], "factory-migration/v1")
        self.assertEqual(payload["tracked_forbidden"], [])
        self.assertTrue(payload["freeze_marker"]["safe"])
        if ralph_before is not None:
            info = os.lstat(sentinel)
            self.assertEqual(
                (sentinel.read_bytes(), info.st_mode, info.st_mtime_ns),
                ralph_before,
                "the live foreign .ralph sentinel must be preserved",
            )

    def test_live_repo_status_never_opens_ralph(self) -> None:
        probe = OpenReadProbe(ROOT).install()
        self.addCleanup(probe.restore)
        report = migration.verify_migration(ROOT)
        self.assertEqual(report["tracked_forbidden"], [])
        probe.assert_no_ralph_access(self)

    def test_live_repo_freeze_cli(self) -> None:
        result = subprocess.run(
            [sys.executable, str(MIGRATION_SCRIPT), "--root", str(ROOT),
             "freeze"],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "frozen")


if __name__ == "__main__":
    unittest.main(verbosity=2)
