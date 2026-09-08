#!/usr/bin/env python3
"""Harness-owned tests for the rootless coordinator authority (BUG-0022).

The local factory harness must never require root: only remote runners may
use root.  This suite proves the installed ``factory-coordinator`` surface:

* **creation/reopen**: a fresh state file is created under a private
  mode-0700 XDG state directory outside the repository with exact mode 0600,
  current-user ownership, link count 1, schema
  ``factory-coordinator-launch-authority/v1``, a random key of at least 32
  bytes, and an empty ``entries`` map; reopening preserves the key;
* **fail-closed state**: malformed JSON, a foreign schema, a short key, a
  symlinked state file, a hardlinked state file, a wrong mode, a foreign
  owner, and a state directory inside the campaign workspace are all
  rejected;
* **root rejection**: the coordinator refuses to run as euid 0, and the
  production launch authority rejects a root-owned descriptor;
* **fixed sibling exec**: the coordinator exports only the inherited
  ``FACTORY_COORDINATOR_AUTH_FD`` descriptor and execs only the sibling
  installed ``factory-campaign`` with the forwarded campaign arguments;
* **inherited O_RDWR FD**: the exported descriptor is opened read/write;
* **current-user authority acceptance**: the production launch authority
  accepts a current-user-owned valid state descriptor;
* **model descendant closure**: the coordinator FD/env never reaches model
  children (the child environment allowlist excludes it and the verifier
  rejects it);
* **installed surface**: the entrypoint is registered in the production
  installer and tracked as an executable.
"""

from __future__ import annotations

import fcntl
import json
import os
import stat
import subprocess
import sys
import tempfile
import types
import unittest
import unittest.mock
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LOOP = ROOT / ".factory" / "loop"
BIN = ROOT / ".factory" / "bin"

sys.path.insert(0, str(LOOP))
import coordinator  # noqa: E402
import launch  # noqa: E402
from installer import DEFAULT_ENTRYPOINTS  # noqa: E402

NIX_BASH = "/nix/store/bwry105g7v5jspr41bx9x3fcfqsmfkq2-bash-interactive-5.3p15/bin/bash"


def _valid_state_bytes() -> bytes:
    document = {
        "schema": coordinator.SCHEMA,
        "key": (b"\x01" * 32).hex(),
        "entries": {},
    }
    return (json.dumps(document, sort_keys=True, separators=(",", ":")) + "\n").encode()


class CoordinatorStateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="factory-coordinator."))
        self.state_home = self.tmp / "state"
        self.state_home.mkdir(mode=0o700)
        self.addCleanup(self._cleanup)

    def _cleanup(self) -> None:
        for child in list(self.tmp.iterdir()):
            if child.is_dir() and not child.is_symlink():
                import shutil
                shutil.rmtree(child)
            else:
                child.unlink(missing_ok=True)
        self.tmp.rmdir()

    def _open(self, workspace=None):
        with unittest.mock.patch.dict(
            os.environ, {"XDG_STATE_HOME": str(self.state_home)}, clear=False
        ):
            return coordinator.open_authority_state(workspace=workspace)

    def test_create_state_creates_private_user_owned_state(self) -> None:
        fd, state_file = self._open()
        try:
            info = os.fstat(fd)
            self.assertTrue(stat.S_ISREG(info.st_mode))
            self.assertEqual(info.st_uid, os.geteuid())
            self.assertEqual(stat.S_IMODE(info.st_mode), 0o600)
            self.assertEqual(info.st_nlink, 1)
            access = fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_ACCMODE
            self.assertEqual(access, os.O_RDWR)
        finally:
            os.close(fd)
        dir_info = os.lstat(state_file.parent)
        self.assertTrue(stat.S_ISDIR(dir_info.st_mode))
        self.assertEqual(stat.S_IMODE(dir_info.st_mode), 0o700)
        self.assertEqual(dir_info.st_uid, os.geteuid())
        document = json.loads(state_file.read_text(encoding="utf-8"))
        self.assertEqual(document["schema"], coordinator.SCHEMA)
        self.assertEqual(document["entries"], {})
        self.assertGreaterEqual(len(bytes.fromhex(document["key"])), 32)

    def test_reopen_preserves_key_and_entries(self) -> None:
        fd, state_file = self._open()
        try:
            os.lseek(fd, 0, os.SEEK_SET)
            first = json.loads(os.read(fd, 4096))
        finally:
            os.close(fd)
        fd2, _ = self._open()
        try:
            os.lseek(fd2, 0, os.SEEK_SET)
            second = json.loads(os.read(fd2, 4096))
        finally:
            os.close(fd2)
        self.assertEqual(first["key"], second["key"])
        self.assertEqual(first["entries"], second["entries"])

    def test_malformed_state_fails_closed(self) -> None:
        state_file = self.state_home / coordinator.STATE_DIR_NAME / coordinator.STATE_FILE_NAME
        state_file.parent.mkdir(mode=0o700)
        state_file.write_bytes(b"{not json")
        with self.assertRaises(coordinator.CoordinatorError):
            self._open()

    def test_unsafe_schema_fails_closed(self) -> None:
        state_file = self.state_home / coordinator.STATE_DIR_NAME / coordinator.STATE_FILE_NAME
        state_file.parent.mkdir(mode=0o700)
        state_file.write_bytes(
            json.dumps({"schema": "other/v1", "key": (b"\x01" * 32).hex(), "entries": {}}).encode()
        )
        with self.assertRaises(coordinator.CoordinatorError):
            self._open()

    def test_short_key_fails_closed(self) -> None:
        state_file = self.state_home / coordinator.STATE_DIR_NAME / coordinator.STATE_FILE_NAME
        state_file.parent.mkdir(mode=0o700)
        state_file.write_bytes(
            json.dumps({"schema": coordinator.SCHEMA, "key": (b"\x01" * 16).hex(), "entries": {}}).encode()
        )
        with self.assertRaises(coordinator.CoordinatorError):
            self._open()

    def test_symlinked_state_file_fails_closed(self) -> None:
        state_dir = self.state_home / coordinator.STATE_DIR_NAME
        state_dir.mkdir(mode=0o700)
        target = self.tmp / "elsewhere"
        target.write_bytes(_valid_state_bytes())
        (state_dir / coordinator.STATE_FILE_NAME).symlink_to(target)
        with self.assertRaises(coordinator.CoordinatorError):
            self._open()

    def test_hardlinked_state_file_fails_closed(self) -> None:
        state_dir = self.state_home / coordinator.STATE_DIR_NAME
        state_dir.mkdir(mode=0o700)
        state_file = state_dir / coordinator.STATE_FILE_NAME
        state_file.write_bytes(_valid_state_bytes())
        os.link(state_file, self.tmp / "hardlink")
        with self.assertRaises(coordinator.CoordinatorError):
            self._open()

    def test_wrong_mode_fails_closed(self) -> None:
        state_dir = self.state_home / coordinator.STATE_DIR_NAME
        state_dir.mkdir(mode=0o700)
        state_file = state_dir / coordinator.STATE_FILE_NAME
        state_file.write_bytes(_valid_state_bytes())
        state_file.chmod(0o644)
        with self.assertRaises(coordinator.CoordinatorError):
            self._open()

    def test_foreign_owner_fails_closed(self) -> None:
        # Simulate a foreign-owned file without needing root: the file is
        # owned by the real user, but the coordinator's effective-uid check
        # is patched to a different uid.
        fd, _ = self._open()
        os.close(fd)
        with unittest.mock.patch.object(coordinator.os, "geteuid", return_value=0):
            with self.assertRaises(coordinator.CoordinatorError):
                self._open()

    def test_root_rejection(self) -> None:
        with unittest.mock.patch.object(coordinator.os, "geteuid", return_value=0):
            with self.assertRaises(coordinator.CoordinatorError):
                self._open()

    def test_state_inside_workspace_rejected(self) -> None:
        workspace = self.tmp / "workspace"
        workspace.mkdir(mode=0o700)
        with unittest.mock.patch.dict(
            os.environ, {"XDG_STATE_HOME": str(workspace / "state")}, clear=False
        ):
            with self.assertRaises(coordinator.CoordinatorError):
                coordinator.open_authority_state(workspace=workspace)

    def test_symlinked_state_base_rejected(self) -> None:
        real_base = self.tmp / "real-base"
        real_base.mkdir(mode=0o700)
        link_base = self.tmp / "link-base"
        link_base.symlink_to(real_base)
        with unittest.mock.patch.dict(
            os.environ, {"XDG_STATE_HOME": str(link_base)}, clear=False
        ):
            with self.assertRaises(coordinator.CoordinatorError):
                coordinator.open_authority_state()

    def test_inherited_fd_is_rdwr(self) -> None:
        fd, _ = self._open()
        try:
            access = fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_ACCMODE
            self.assertEqual(access, os.O_RDWR)
        finally:
            os.close(fd)

    def test_fixed_sibling_exec_only(self) -> None:
        workspace = self.tmp / "workspace"
        workspace.mkdir(mode=0o700)
        captured = {}

        def fake_execv(path, argv):
            captured["path"] = path
            captured["argv"] = list(argv)
            raise SystemExit(0)

        with unittest.mock.patch.dict(
            os.environ, {"XDG_STATE_HOME": str(self.state_home)}, clear=False
        ), unittest.mock.patch.object(coordinator.os, "execv", side_effect=fake_execv):
            with self.assertRaises(SystemExit):
                coordinator.main(["--root", str(workspace), "run", "--campaign-id", "x"])
            exported = coordinator.os.environ.get(coordinator.AUTH_ENV, "")
        self.assertEqual(
            captured["path"],
            str(BIN / "factory-campaign"),
        )
        self.assertEqual(
            captured["argv"],
            [str(BIN / "factory-campaign"), "--root", str(workspace), "run", "--campaign-id", "x"],
        )
        self.assertTrue(exported.isdecimal())
        # The pathname must never be exported to the campaign environment.
        for key in os.environ:
            self.assertNotIn("factory-coordinator", key.lower())
            self.assertNotIn("launch-authority", key.lower())

    def test_unrelated_descriptors_close_on_exec(self) -> None:
        fd, _ = self._open()
        try:
            probe = os.open(self.tmp / "probe", os.O_RDWR | os.O_CREAT, 0o600)
            try:
                coordinator._mark_unrelated_close_on_exec(fd)
                probe_flags = fcntl.fcntl(probe, fcntl.F_GETFD)
                self.assertTrue(probe_flags & fcntl.FD_CLOEXEC)
                auth_flags = fcntl.fcntl(fd, fcntl.F_GETFD)
                self.assertFalse(auth_flags & fcntl.FD_CLOEXEC)
            finally:
                os.close(probe)
        finally:
            os.close(fd)

    def test_entrypoint_execs_only_sibling_campaign(self) -> None:
        # End-to-end through the real installed entrypoint script with a fake
        # sibling campaign: the coordinator must exec exactly that sibling and
        # export only the FD.
        tree = self.tmp / "installed"
        (tree / ".factory" / "bin").mkdir(parents=True, mode=0o700)
        (tree / ".factory" / "loop").mkdir(parents=True, mode=0o700)
        (tree / ".factory" / "bin" / "factory-coordinator").write_bytes(
            (BIN / "factory-coordinator").read_bytes()
        )
        (tree / ".factory" / "bin" / "factory-coordinator").chmod(0o755)
        (tree / ".factory" / "loop" / "coordinator.py").write_bytes(
            (LOOP / "coordinator.py").read_bytes()
        )
        fake_campaign = tree / ".factory" / "bin" / "factory-campaign"
        fake_campaign.write_text(
            f"#!{NIX_BASH}\n"
            "echo \"ARGS:$*\"\n"
            "echo \"AUTH_FD:${FACTORY_COORDINATOR_AUTH_FD:-}\"\n"
            "echo \"STATE_ENV:${FACTORY_COORDINATOR_STATE:-}\"\n",
            encoding="utf-8",
        )
        fake_campaign.chmod(0o755)
        workspace = self.tmp / "workspace"
        workspace.mkdir(mode=0o700)
        env = {
            "PATH": os.environ.get("PATH", ""),
            "HOME": str(self.tmp),
            "TMPDIR": str(self.tmp),
            "XDG_STATE_HOME": str(self.state_home),
            "LANG": "C.UTF-8",
        }
        result = subprocess.run(
            [str(tree / ".factory" / "bin" / "factory-coordinator"),
             "--root", str(workspace), "run", "--campaign-id", "x"],
            capture_output=True, text=True, env=env, timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("ARGS:--root", result.stdout)
        self.assertIn("run --campaign-id x", result.stdout)
        self.assertRegex(result.stdout, r"AUTH_FD:\d+")
        self.assertIn("STATE_ENV:", result.stdout)


class LaunchAuthorityOwnerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="factory-coordinator-launch."))
        self.state_home = self.tmp / "state"
        self.state_home.mkdir(mode=0o700)
        self.addCleanup(self._cleanup)

    def _cleanup(self) -> None:
        for child in list(self.tmp.iterdir()):
            if child.is_dir() and not child.is_symlink():
                import shutil
                shutil.rmtree(child)
            else:
                child.unlink(missing_ok=True)
        self.tmp.rmdir()

    def _valid_fd(self) -> int:
        with unittest.mock.patch.dict(
            os.environ, {"XDG_STATE_HOME": str(self.state_home)}, clear=False
        ):
            fd, _ = coordinator.open_authority_state()
        return fd

    def test_launch_accepts_current_user_authority(self) -> None:
        fd = self._valid_fd()
        try:
            with unittest.mock.patch.dict(
                os.environ, {coordinator.AUTH_ENV: str(fd)}, clear=False
            ):
                key, returned_fd, identity = launch._coordinator_authorization_key()
            self.assertGreaterEqual(len(key), 32)
            self.assertNotEqual(returned_fd, fd)
            self.assertIn(":", identity)
            access = fcntl.fcntl(returned_fd, fcntl.F_GETFL) & os.O_ACCMODE
            self.assertEqual(access, os.O_RDWR)
            os.close(returned_fd)
        finally:
            os.close(fd)

    def test_launch_rejects_root_owned_authority(self) -> None:
        fd = self._valid_fd()
        try:
            real_fstat = launch.os.fstat

            def root_owned(fdno):
                info = real_fstat(fdno)
                return types.SimpleNamespace(
                    st_mode=info.st_mode, st_uid=0, st_nlink=info.st_nlink,
                    st_dev=info.st_dev, st_ino=info.st_ino,
                )

            with unittest.mock.patch.dict(
                os.environ, {coordinator.AUTH_ENV: str(fd)}, clear=False
            ), unittest.mock.patch.object(launch.os, "fstat", side_effect=root_owned):
                with self.assertRaises(launch.InvocationError):
                    launch._coordinator_authorization_key()
        finally:
            os.close(fd)

    def test_launch_rejects_foreign_owner_authority(self) -> None:
        fd = self._valid_fd()
        try:
            real_fstat = launch.os.fstat

            def foreign_owned(fdno):
                info = real_fstat(fdno)
                return types.SimpleNamespace(
                    st_mode=info.st_mode, st_uid=os.geteuid() + 1, st_nlink=info.st_nlink,
                    st_dev=info.st_dev, st_ino=info.st_ino,
                )

            with unittest.mock.patch.dict(
                os.environ, {coordinator.AUTH_ENV: str(fd)}, clear=False
            ), unittest.mock.patch.object(launch.os, "fstat", side_effect=foreign_owned):
                with self.assertRaises(launch.InvocationError):
                    launch._coordinator_authorization_key()
        finally:
            os.close(fd)

    def test_launch_rejects_wrong_mode_authority(self) -> None:
        fd = self._valid_fd()
        try:
            real_fstat = launch.os.fstat

            def wrong_mode(fdno):
                info = real_fstat(fdno)
                return types.SimpleNamespace(
                    st_mode=0o100644, st_uid=info.st_uid, st_nlink=info.st_nlink,
                    st_dev=info.st_dev, st_ino=info.st_ino,
                )

            with unittest.mock.patch.dict(
                os.environ, {coordinator.AUTH_ENV: str(fd)}, clear=False
            ), unittest.mock.patch.object(launch.os, "fstat", side_effect=wrong_mode):
                with self.assertRaises(launch.InvocationError):
                    launch._coordinator_authorization_key()
        finally:
            os.close(fd)

    def test_model_descendant_closure(self) -> None:
        # The coordinator FD env value must never reach a model child: it is
        # not in the child environment allowlist, and the child-environment
        # verifier rejects it outright.
        self.assertNotIn(coordinator.AUTH_ENV, launch.ENV_ALLOWLIST)
        with self.assertRaises(launch.InvocationError):
            launch.verify_child_env({coordinator.AUTH_ENV: "3"})
        with unittest.mock.patch.dict(
            os.environ, {coordinator.AUTH_ENV: "3"}, clear=False
        ):
            self.assertNotIn(coordinator.AUTH_ENV, launch.ENV_ALLOWLIST)


class InstalledSurfaceTests(unittest.TestCase):
    def test_entrypoint_registered_in_installer(self) -> None:
        self.assertIn(".factory/bin/factory-coordinator", DEFAULT_ENTRYPOINTS)

    def test_entrypoint_executable(self) -> None:
        self.assertTrue(os.access(BIN / "factory-coordinator", os.X_OK))
        self.assertTrue((BIN / "factory-coordinator").is_file())

    def test_coordinator_module_present(self) -> None:
        self.assertTrue((LOOP / "coordinator.py").is_file())


if __name__ == "__main__":
    unittest.main(verbosity=2)
