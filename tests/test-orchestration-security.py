#!/usr/bin/env python3
"""Focused security regressions for retained state and verifier boundaries."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

SOURCE = Path(__file__).resolve().parent.parent
HEX = "a" * 64


def run(
    command: list[str],
    root: Path,
    *,
    env: dict[str, str] | None = None,
    check: bool = True,
    pass_fds: tuple[int, ...] = (),
) -> subprocess.CompletedProcess[str]:
    merged = os.environ.copy()
    if env:
        merged.update(env)
    result = subprocess.run(
        command, cwd=root, env=merged, text=True, capture_output=True, pass_fds=pass_fds
    )
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


def test_state_removal_quarantines_post_check_substitution() -> None:
    module = load_module(SOURCE / "scripts/factory_state_io.py", "factory_state_io_race_module")
    root = Path(tempfile.mkdtemp(prefix="controller-box-state-race-test."))
    try:
        state = root / ".factory-state"
        state.mkdir(mode=0o700)
        marker = state / "completion-rejected.json"
        replacement = state / "replacement"

        def install_original() -> None:
            marker.write_text('{"value":"original"}\n', encoding="utf-8")
            marker.chmod(0o600)
            replacement.write_text('{"value":"replacement"}\n', encoding="utf-8")
            replacement.chmod(0o600)

        def substitute() -> None:
            marker.unlink()
            replacement.rename(marker)

        install_original()
        try:
            module.remove(root, "completion-rejected.json", after_final_check=substitute)
        except module.StateIOError as exc:
            assert "substituted at quarantine" in str(exc)
        else:
            raise AssertionError("post-check marker replacement was removed")
        quarantines = list(state.glob(".completion-rejected.json.quarantine-*"))
        assert len(quarantines) == 1
        assert json.loads(quarantines[0].read_text(encoding="utf-8"))["value"] == "replacement"
        quarantines[0].unlink()

        install_original()
        try:
            module.consume_json(
                root,
                "completion-rejected.json",
                lambda data: data["value"],
                after_final_check=substitute,
            )
        except module.StateIOError as exc:
            assert "substituted at quarantine" in str(exc)
        else:
            raise AssertionError("post-validation rejection marker replacement was consumed")
        quarantines = list(state.glob(".completion-rejected.json.quarantine-*"))
        assert len(quarantines) == 1
        assert json.loads(quarantines[0].read_text(encoding="utf-8"))["value"] == "replacement"
    finally:
        shutil.rmtree(root)


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


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
        (root / ".factory/verifier-acceptance.json").write_text(
            json.dumps({"schema": "ralph-verifier-acceptance/v1", "gates": [{"name": "test-one.sh", "args": []}]}),
            encoding="utf-8",
        )
        init_git(root)
        run(["git", "add", "."], root)
        run(["git", "commit", "-qm", "base"], root)
        helper = root / "scripts/campaign-verifier-binding.py"
        binding = json.loads(run([str(helper)], root).stdout)
        digest = binding["sha256"]
        assert binding["helper"] is None  # direct pathname invocation performs no self-binding
        run([str(helper), "--expected-digest", digest, "--exec"], root)
        assert binding["binding"]["executable_blob"] == run(
            ["git", "rev-parse", "HEAD:scripts/verify-project.sh"], root
        ).stdout.strip()

        verifier.write_text("#!/usr/bin/env bash\nexit 7\n", encoding="utf-8")
        rejected = run([str(helper), "--expected-digest", digest, "--exec"], root, check=False)
        assert rejected.returncode != 0 and (
            "committed blob" in rejected.stderr or "binding changed" in rejected.stderr
        )
        run(["git", "restore", "--", "scripts/verify-project.sh"], root)

        verifier.chmod(0o775)
        rejected = run([str(helper)], root, check=False)
        assert rejected.returncode != 0 and "unsafe file" in rejected.stderr
        verifier.chmod(0o755)

        config = root / ".factory/config.toml"
        config.write_text(config.read_text(encoding="utf-8") + "# changed\n", encoding="utf-8")
        rejected = run([str(helper), "--expected-digest", digest, "--exec"], root, check=False)
        assert rejected.returncode != 0 and (
            "committed blob" in rejected.stderr or "binding changed" in rejected.stderr
        )
        run(["git", "restore", "--", ".factory/config.toml"], root)
        config.chmod(0o664)
        rejected = run([str(helper)], root, check=False)
        assert rejected.returncode != 0 and "unsafe file" in rejected.stderr
        config.chmod(0o644)

        external = root / "external-verifier"
        external.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
        external.chmod(0o755)
        verifier.unlink()
        verifier.symlink_to(external)
        rejected = run([str(helper)], root, check=False)
        assert rejected.returncode != 0 and "symlink" in rejected.stderr
    finally:
        shutil.rmtree(root)


def test_retained_helper_fd_immune_to_pathname_swap() -> None:
    root = Path(tempfile.mkdtemp(prefix="controller-box-helper-race."))
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
        (root / ".factory/verifier-acceptance.json").write_text(
            json.dumps({"schema": "ralph-verifier-acceptance/v1", "gates": [{"name": "test-one.sh", "args": []}]}),
            encoding="utf-8",
        )
        init_git(root)
        run(["git", "add", "."], root)
        run(["git", "commit", "-qm", "base"], root)
        helper = root / "scripts/campaign-verifier-binding.py"
        original_bytes = helper.read_bytes()
        original_blob = run(
            ["git", "rev-parse", "HEAD:scripts/campaign-verifier-binding.py"], root
        ).stdout.strip()
        fd = os.open(helper, os.O_RDONLY)
        try:
            os.set_inheritable(fd, True)
            # Replace the helper pathname before invocation. The retained
            # descriptor still refers to the exact committed inode opened at
            # campaign startup, so the original helper runs and self-binds;
            # the substitute never runs.
            helper.rename(root / "scripts/campaign-verifier-binding.py.original")
            substitute = root / "scripts/campaign-verifier-binding.py"
            substitute.write_text(
                "#!/usr/bin/env python3\nprint('SUBSTITUTE-HELPER-RAN')\n", encoding="utf-8"
            )
            substitute.chmod(0o755)
            result = run([f"/proc/self/fd/{fd}"], root, pass_fds=(fd,))
            # A second invocation through the same retained descriptor must
            # still bind: the shared open-file description position is rewound
            # before each self-binding read.
            repeated = run([f"/proc/self/fd/{fd}"], root, pass_fds=(fd,))
            assert json.loads(repeated.stdout)["sha256"] == json.loads(result.stdout)["sha256"]
        finally:
            os.close(fd)
        assert "SUBSTITUTE-HELPER-RAN" not in result.stdout
        binding = json.loads(result.stdout)
        assert binding["binding"]["schema"] == "campaign-verifier-binding/v1"
        assert binding["helper"] == {
            "path": "scripts/campaign-verifier-binding.py",
            "sha256": hashlib.sha256(original_bytes).hexdigest(),
            "blob": original_blob,
            "mode": "0755",
        }
        assert binding["sha256"]
    finally:
        shutil.rmtree(root)


def test_verifier_swap_in_final_exec_race_runs_original() -> None:
    root = Path(tempfile.mkdtemp(prefix="controller-box-verifier-race."))
    try:
        (root / ".factory").mkdir()
        copy_scripts(root, "campaign-verifier-binding.py")
        verifier = root / "scripts/verify-project.sh"
        verifier.write_text(
            "#!/usr/bin/env bash\necho ORIGINAL-VERIFIER-RAN\nexit 0\n", encoding="utf-8"
        )
        verifier.chmod(0o755)
        (root / ".factory/config.toml").write_text(
            '[verification]\ncampaign_command = ["./scripts/verify-project.sh", "--strict"]\n',
            encoding="utf-8",
        )
        (root / ".factory/verifier-acceptance.json").write_text(
            json.dumps({"schema": "ralph-verifier-acceptance/v1", "gates": [{"name": "test-one.sh", "args": []}]}),
            encoding="utf-8",
        )
        init_git(root)
        run(["git", "add", "."], root)
        run(["git", "commit", "-qm", "base"], root)
        driver = root / "scripts/race-driver.py"
        driver.write_text(
            """import importlib.util, os, sys
from pathlib import Path
root = Path(sys.argv[1])
spec = importlib.util.spec_from_file_location('cvb', root / 'scripts/campaign-verifier-binding.py')
cvb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cvb)
verifier = root / 'scripts/verify-project.sh'
real_execve = os.execve
def racing_execve(path, argv, env):
    # The final validation->exec race: swap the verifier pathname now.
    verifier.rename(root / 'scripts/verify-project.sh.original')
    substitute = root / 'scripts/verify-project.sh'
    substitute.write_text('#!/usr/bin/env bash\\necho SUBSTITUTE-VERIFIER-RAN\\nexit 7\\n')
    substitute.chmod(0o755)
    return real_execve(path, argv, env)
os.execve = racing_execve
_binding, digest, command, executable, executable_bytes = cvb.binding()
cvb.execute_verified(command, executable, executable_bytes)
""",
            encoding="utf-8",
        )
        result = run([sys.executable, str(driver), str(root)], root, check=False)
        assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
        assert "ORIGINAL-VERIFIER-RAN" in result.stdout
        assert "SUBSTITUTE-VERIFIER-RAN" not in result.stdout
    finally:
        shutil.rmtree(root)


def main() -> None:
    test_symlink_safe_state_markers()
    test_state_removal_quarantines_post_check_substitution()
    test_verifier_executable_blob_binding()
    test_retained_helper_fd_immune_to_pathname_swap()
    test_verifier_swap_in_final_exec_race_runs_original()
    print("test: orchestration security checks passed")


if __name__ == "__main__":
    main()
