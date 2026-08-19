#!/usr/bin/env python3
"""Adversarial checks for the stable factory lifecycle lock."""

from __future__ import annotations

import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

SOURCE = Path(__file__).resolve().parent.parent


def run(command: list[str], root: Path, *, check: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, cwd=root, text=True, capture_output=True)
    if check and result.returncode:
        raise AssertionError((command, result.returncode, result.stdout, result.stderr))
    return result


def make_repo() -> Path:
    root = Path(tempfile.mkdtemp(prefix="controller-box-lock-test."))
    (root / "scripts").mkdir()
    for name in ("factory-lock-exec.py", "factory_lock.py"):
        shutil.copy2(SOURCE / "scripts" / name, root / "scripts" / name)
    (root / ".gitignore").write_text(".factory-lock\n", encoding="utf-8")
    (root / "tracked").write_text("test\n", encoding="utf-8")
    run(["git", "init", "-q", "-b", "develop"], root)
    run(["git", "config", "user.name", "test"], root)
    run(["git", "config", "user.email", "test@example.invalid"], root)
    run(["git", "add", "."], root)
    run(["git", "commit", "-qm", "base"], root)
    return root


def test_retention_drop_unlock_and_legacy_migration() -> None:
    root = make_repo()
    try:
        legacy = root / ".factory-lock"
        legacy.write_text("legacy\n", encoding="utf-8")
        legacy.chmod(0o600)
        inspect = root / "inspect.py"
        inspect.write_text(
            """import os, pathlib, subprocess, sys
root=pathlib.Path(sys.argv[1])
keys=('FACTORY_LOCK_HELD','FACTORY_LOCK_FD','FACTORY_LOCK_ID','FACTORY_LOCK_PATH')
assert all(key not in os.environ for key in keys)
lock=(root/'.git/controller-box-factory/lifecycle.lock').stat()
for item in pathlib.Path('/proc/self/fd').iterdir():
    try:
        info=os.stat(item)
    except OSError:
        continue
    assert (info.st_dev,info.st_ino)!=(lock.st_dev,lock.st_ino)
check=subprocess.run([str(root/'scripts/factory-lock-exec.py'),str(root),'--check'],capture_output=True)
assert check.returncode != 0
print('untrusted-clean')
""",
            encoding="utf-8",
        )
        probe = root / "probe.sh"
        probe.write_text(
            """#!/usr/bin/env bash
set -euo pipefail
root=$1
helper=$root/scripts/factory-lock-exec.py
python3 "$helper" "$root" --check
fd=${FACTORY_LOCK_FD:?}
[[ $fd =~ ^[0-9]+$ ]] && (( fd >= 3 ))
[[ -e /proc/self/fd/$fd ]]
[[ ${FACTORY_LOCK_PATH:?} == "$root/.git/controller-box-factory/lifecycle.lock" ]]
python3 "$helper" "$root" --drop -- python3 "$root/inspect.py" "$root"
python3 "$helper" "$root" --check
if env -u FACTORY_LOCK_HELD -u FACTORY_LOCK_FD -u FACTORY_LOCK_ID -u FACTORY_LOCK_PATH \
    python3 "$helper" "$root" -- true; then
    exit 41
fi
""",
            encoding="utf-8",
        )
        probe.chmod(0o755)
        result = run(
            [str(root / "scripts/factory-lock-exec.py"), str(root), "--", str(probe), str(root)],
            root,
        )
        assert "untrusted-clean" in result.stdout
        canonical = root / ".git/controller-box-factory/lifecycle.lock"
        assert canonical.is_file() and canonical.stat().st_nlink == 1
        assert not legacy.exists(), "legacy pathname was not removed after dual-lock migration"

        # The pathname is retained after unlock, and a later owner reuses it.
        identity = (canonical.stat().st_dev, canonical.stat().st_ino)
        run([str(root / "scripts/factory-lock-exec.py"), str(root), "--", "true"], root)
        assert canonical.is_file()
        assert (canonical.stat().st_dev, canonical.stat().st_ino) == identity
    finally:
        shutil.rmtree(root)


def test_legacy_holder_and_unsafe_legacy_fail_closed() -> None:
    root = make_repo()
    try:
        helper = root / "scripts/factory-lock-exec.py"
        legacy = root / ".factory-lock"
        legacy.write_text("held\n", encoding="utf-8")
        legacy.chmod(0o600)
        with legacy.open("a", encoding="utf-8") as stream:
            fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            rejected = run([str(helper), str(root), "--", "true"], root, check=False)
            assert rejected.returncode != 0 and "legacy factory lock is still held" in rejected.stderr
            assert legacy.is_file()

        legacy.unlink()
        external = root / "external"
        external.write_text("unchanged\n", encoding="utf-8")
        legacy.symlink_to(external)
        rejected = run([str(helper), str(root), "--", "true"], root, check=False)
        assert rejected.returncode != 0
        assert external.read_text(encoding="utf-8") == "unchanged\n"
        legacy.unlink()

        os.link(external, legacy)
        rejected = run([str(helper), str(root), "--", "true"], root, check=False)
        assert rejected.returncode != 0
        assert external.read_text(encoding="utf-8") == "unchanged\n"
    finally:
        shutil.rmtree(root)


def main() -> None:
    test_retention_drop_unlock_and_legacy_migration()
    test_legacy_holder_and_unsafe_legacy_fail_closed()
    print("test: stable factory lock checks passed")


if __name__ == "__main__":
    main()
