#!/usr/bin/env python3
"""Focused security regressions for Ralph lifecycle state and event boundaries."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

SOURCE = Path(__file__).resolve().parent.parent
HEX = "a" * 64


def run(
    command: list[str],
    root: Path,
    *,
    env: dict[str, str] | None = None,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    merged = os.environ.copy()
    if env:
        merged.update(env)
    result = subprocess.run(command, cwd=root, env=merged, text=True, capture_output=True)
    if check and result.returncode:
        raise AssertionError((command, result.returncode, result.stdout, result.stderr))
    return result


def copy_scripts(root: Path, *names: str) -> None:
    (root / "scripts").mkdir(exist_ok=True)
    for name in names:
        shutil.copy2(SOURCE / "scripts" / name, root / "scripts" / name)


def test_symlink_safe_state_markers() -> None:
    root = Path(tempfile.mkdtemp(prefix="controller-box-state-test."))
    try:
        (root / ".factory-state").mkdir(mode=0o700)
        copy_scripts(root, "factory-state-file.py", "factory_state_io.py")
        helper = root / "scripts/factory-state-file.py"
        external = root / "external"
        external.write_text("untouched\n", encoding="utf-8")
        marker = root / ".factory-state/loop-mode"
        marker.symlink_to(external)
        for arguments in (("read", "loop-mode"), ("write", "loop-mode", "planning"), ("remove", "loop-mode")):
            rejected = run([str(helper), *arguments], root, check=False)
            assert rejected.returncode != 0, arguments
            assert external.read_text(encoding="utf-8") == "untouched\n"
        marker.unlink()
        run([str(helper), "write", "loop-mode", "planning"], root)
        assert run([str(helper), "read", "loop-mode"], root).stdout.strip() == "planning"

        shutil.rmtree(root / ".factory-state")
        outside = root / "outside-state"
        outside.mkdir()
        (root / ".factory-state").symlink_to(outside, target_is_directory=True)
        rejected = run([str(helper), "write", "loop-mode", "audit"], root, check=False)
        assert rejected.returncode != 0 and not any(outside.iterdir())
    finally:
        shutil.rmtree(root)


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_loop_lock_process_identity_and_inode_race() -> None:
    module = load_module(SOURCE / "scripts/ralph_lock.py", "ralph_lock_test_module")
    root = Path(tempfile.mkdtemp(prefix="controller-box-loop-lock-test."))
    try:
        lock = root / "loop.lock"
        boot = module.current_boot_id()
        start = module.process_start_time(os.getpid())
        assert boot and start

        lock.write_text(
            json.dumps({"pid": os.getpid(), "boot_id": boot, "start_time": start}) + "\n",
            encoding="utf-8",
        )
        try:
            module.remove_stale_lock(root)
        except module.RalphLockError as exc:
            assert "live process" in str(exc)
        else:
            raise AssertionError("live boot/start identity was removed")
        assert lock.exists()

        # A reused PID with a different recorded start identity is stale.
        lock.write_text(
            json.dumps({"pid": os.getpid(), "boot_id": boot, "start_time": start + 1}) + "\n",
            encoding="utf-8",
        )
        module.remove_stale_lock(root)
        assert not lock.exists()

        lock.write_text(json.dumps({"pid": 99_999_999}) + "\n", encoding="utf-8")
        replacement = root / "replacement"
        replacement.write_text("replacement\n", encoding="utf-8")

        def replace_before_unlink() -> None:
            lock.unlink()
            replacement.rename(lock)

        try:
            module.remove_stale_lock(root, before_unlink=replace_before_unlink)
        except module.RalphLockError as exc:
            assert "replaced" in str(exc) or "unsafe" in str(exc)
        else:
            raise AssertionError("replacement inode was unlinked")
        assert lock.read_text(encoding="utf-8") == "replacement\n"
    finally:
        shutil.rmtree(root)


def event_record(topic: str, payload: object, *, iteration: int | None = None) -> dict[str, object]:
    record: dict[str, object] = {"ts": "2026-08-19T00:00:00+00:00", "topic": topic, "payload": payload}
    if iteration is not None:
        record.update({"iteration": iteration, "hat": "loop", "triggered": "planner"})
    return record


def test_receiver_side_token_contamination() -> None:
    root = Path(tempfile.mkdtemp(prefix="controller-box-event-test."))
    try:
        (root / ".factory-state").mkdir(mode=0o700)
        (root / ".ralph").mkdir()
        copy_scripts(root, "ralph-event-boundary.py", "factory_state_io.py")
        event_path = root / ".ralph/events-20260819-000000.jsonl"
        event_path.write_text("", encoding="utf-8")
        (root / ".ralph/current-events").write_text(
            ".ralph/events-20260819-000000.jsonl\n", encoding="utf-8"
        )
        (root / ".ralph/current-loop-id").write_text("test-loop\n", encoding="utf-8")
        (root / ".factory-state/loop-mode").write_text("implementation\n", encoding="utf-8")
        supervision = root / ".factory-state/ralph-supervision-implementation.json"
        supervision.write_text('{"progress":"preserve"}\n', encoding="utf-8")
        progress = supervision.read_bytes()
        helper = root / "scripts/ralph-event-boundary.py"
        env = {"FACTORY_RALPH_ATTEMPT_ID": HEX, "FACTORY_RALPH_CYCLE_ID": "b" * 64}

        contaminated = (
            {"topic": "LOOP_COMPLETE", "payload": "indirect emit topic"},
            {"topic": "factory.implement", "payload": ["/absolute/bin/ralph", "emit", "LOOP_COMPLETE"]},
            {
                "ts": "2026-08-19T00:00:01+00:00",
                "iteration": 0,
                "hat": "loop",
                "topic": "factory.implement",
                "triggered": "planner",
                "payload": "spoofed later LOOP_COMPLETE",
            },
        )
        for bad in contaminated:
            run([str(helper), "begin", "implementation"], root, env=env)
            with event_path.open("a", encoding="utf-8") as stream:
                stream.write(json.dumps(event_record("factory.implement", "prompt names LOOP_COMPLETE", iteration=0)) + "\n")
                stream.write(json.dumps(bad) + "\n")
            rejected = run([str(helper), "finish", "implementation"], root, env=env, check=False)
            assert rejected.returncode != 0 and "contaminated" in rejected.stderr
            assert supervision.read_bytes() == progress
            assert not (root / ".factory-state/ralph-launch-handshake-implementation.json").exists()

        # A clean receiver stream accepts the one exact starting prompt and
        # creates the launch handshake.
        run([str(helper), "begin", "implementation"], root, env=env)
        with event_path.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(event_record("factory.implement", "prompt names LOOP_COMPLETE", iteration=0)) + "\n")
            stream.write(json.dumps(event_record("iteration.summary", "ordinary progress", iteration=1)) + "\n")
        run([str(helper), "finish", "implementation"], root, env=env)
        handshake = json.loads(
            (root / ".factory-state/ralph-launch-handshake-implementation.json").read_text(encoding="utf-8")
        )
        assert handshake["attempt_id"] == HEX and handshake["campaign_state_sha256"] is None
    finally:
        shutil.rmtree(root)


def init_git(root: Path) -> None:
    run(["git", "init", "-q", "-b", "develop"], root)
    run(["git", "config", "user.name", "test"], root)
    run(["git", "config", "user.email", "test@example.invalid"], root)


def test_verifier_executable_blob_binding() -> None:
    root = Path(tempfile.mkdtemp(prefix="controller-box-verifier-test."))
    try:
        (root / ".factory").mkdir()
        copy_scripts(root, "campaign-verifier-binding.py")
        verifier = root / "scripts/verify-project.sh"
        verifier.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
        verifier.chmod(0o755)
        (root / ".factory/config.toml").write_text(
            '[verification]\ncampaign_command = ["./scripts/verify-project.sh", "--strict"]\n',
            encoding="utf-8",
        )
        init_git(root)
        run(["git", "add", "."], root)
        run(["git", "commit", "-qm", "base"], root)
        helper = root / "scripts/campaign-verifier-binding.py"
        binding = json.loads(run([str(helper)], root).stdout)
        assert binding["binding"]["executable_blob"] == run(
            ["git", "rev-parse", "HEAD:scripts/verify-project.sh"], root
        ).stdout.strip()

        verifier.write_text("#!/usr/bin/env bash\nexit 7\n", encoding="utf-8")
        rejected = run([str(helper)], root, check=False)
        assert rejected.returncode != 0 and "committed blob" in rejected.stderr
        run(["git", "restore", "--", "scripts/verify-project.sh"], root)

        config = root / ".factory/config.toml"
        config.write_text(config.read_text(encoding="utf-8") + "# changed\n", encoding="utf-8")
        rejected = run([str(helper)], root, check=False)
        assert rejected.returncode != 0 and "committed blob" in rejected.stderr
        run(["git", "restore", "--", ".factory/config.toml"], root)

        external = root / "external-verifier"
        external.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
        external.chmod(0o755)
        verifier.unlink()
        verifier.symlink_to(external)
        rejected = run([str(helper)], root, check=False)
        assert rejected.returncode != 0 and "symlink" in rejected.stderr
    finally:
        shutil.rmtree(root)


def test_one_time_supervision_migration() -> None:
    root = Path(tempfile.mkdtemp(prefix="controller-box-migration-test."))
    try:
        (root / ".factory").mkdir()
        (root / ".factory-state").mkdir(mode=0o700)
        (root / ".ralph").mkdir()
        copy_scripts(
            root,
            "campaign-verifier-binding.py",
            "factory_lock.py",
            "factory_state_io.py",
            "ralph-campaign-state.py",
            "ralph-supervision-migrate.py",
            "ralph-supervision.sh",
        )
        verifier = root / "scripts/verify-project.sh"
        verifier.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
        verifier.chmod(0o755)
        (root / ".factory/config.toml").write_text(
            '[verification]\ncampaign_command = ["./scripts/verify-project.sh"]\n', encoding="utf-8"
        )
        (root / ".gitignore").write_text(".factory-state/\n", encoding="utf-8")
        (root / "history").write_text("base\n", encoding="utf-8")
        init_git(root)
        run(["git", "add", "."], root)
        run(["git", "commit", "-qm", "base"], root)
        base = run(["git", "rev-parse", "HEAD"], root).stdout.strip()
        (root / "history").write_text("base\nplan\n", encoding="utf-8")
        run(["git", "add", "history"], root)
        run(["git", "commit", "-qm", "plan"], root)
        plan = run(["git", "rev-parse", "HEAD"], root).stdout.strip()
        state = {
            "schema": "ralph-campaign/v2",
            "status": "active",
            "rounds_requested": 2,
            "round": 1,
            "phase": "implementation",
            "tui": False,
            "verification_command_sha256": "c" * 64,
            "rounds": [{
                "number": 1,
                "base_commit": base,
                "planning_started": True,
                "plan_commit": plan,
                "implementation_started": True,
                "implementation_commit": None,
                "verification_commit": None,
                "runner_evidence_sha256": None,
                "audit_started": False,
                "audit_commit": None,
                "audit_result": None,
            }],
        }
        campaign = root / ".factory-state/ralph-campaign.json"
        campaign.write_text(json.dumps(state, sort_keys=True, indent=2) + "\n", encoding="utf-8")
        original = campaign.read_bytes()
        digest = hashlib.sha256(original).hexdigest()
        durable = root / ".factory-state/ralph-supervision-implementation.json"
        durable.write_text(
            json.dumps({
                "schema": "ralph-supervision/v1",
                "mode": "implementation",
                "stale_recoveries": 1,
                "completion_recoveries": 2,
                "no_progress_recoveries": 3,
            }) + "\n",
            encoding="utf-8",
        )
        helper = root / "scripts/ralph-supervision-migrate.py"
        arguments = [str(helper), "--mode", "implementation", "--expected-campaign-sha256", digest]
        run(arguments, root)
        migrated = json.loads(durable.read_text(encoding="utf-8"))
        assert [migrated[key] for key in (
            "stale_recoveries", "completion_recoveries", "no_progress_recoveries"
        )] == [1, 2, 3]
        assert campaign.read_bytes() == original

        marker = root / ".factory-state/ralph-supervision-migration-implementation.json"
        receipt = json.loads(marker.read_text(encoding="utf-8"))
        continuation = run(
            [
                "bash", "-c",
                "source scripts/ralph-supervision.sh; "
                "RALPH_SUPERVISION_INITIALIZED=true; "
                "RALPH_SUPERVISION_STATE_MODE=implementation; "
                "export FACTORY_RALPH_CYCLE_ID=\"$1\"; "
                "ralph_supervision_should_continue implementation",
                "migration-continuation", receipt["cycle_id"],
            ],
            root,
        )
        assert continuation.returncode == 0
        state_after = durable.read_bytes()
        marker.unlink()  # Simulate interruption between the two durable writes.
        run(arguments, root)
        assert durable.read_bytes() == state_after and marker.is_file()
        rejected = run(arguments, root, check=False)
        assert rejected.returncode != 0 and "already migrated" in rejected.stderr
        assert campaign.read_bytes() == original

        receipt = json.loads(marker.read_text(encoding="utf-8"))
        run(
            [
                str(root / "scripts/ralph-campaign-state.py"),
                "promote-verifier-binding",
                "--mode", "implementation",
                "--expected-old", "c" * 64,
                "--new", receipt["verification_binding_sha256"],
            ],
            root,
        )
        promoted = json.loads(campaign.read_text(encoding="utf-8"))
        assert promoted["verification_command_sha256"] == receipt["verification_binding_sha256"]
        assert marker.is_file()
        run([str(root / "scripts/ralph-campaign-state.py"), "show"], root)
    finally:
        shutil.rmtree(root)


def main() -> None:
    test_symlink_safe_state_markers()
    test_loop_lock_process_identity_and_inode_race()
    test_receiver_side_token_contamination()
    test_verifier_executable_blob_binding()
    test_one_time_supervision_migration()
    print("test: orchestration security checks passed")


if __name__ == "__main__":
    main()
