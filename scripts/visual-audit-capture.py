#!/usr/bin/env python3
"""Serial/isolated visual capture runner (stdlib only).

Holds the dedicated visual-audit lease (flock, O_CLOEXEC) in THIS process
across every capture-driver invocation, so:
  - captures are strictly serial; a concurrent capture is refused (no
    shared-session race)
  - the untrusted capture driver and the vision model never inherit the
    lease fd (CLOEXEC), and the campaign factory lock is never touched
Then binds every image to the exact commit/tree via the provenance manifest.
"""

from __future__ import annotations

import fcntl
import json
import os
from pathlib import Path
import subprocess
import sys

def die(message: str) -> "NoReturn":
    raise SystemExit(f"visual-audit-capture: {message}")

def main() -> int:
    root = Path.cwd()
    config_path = Path(os.environ.get("VISUAL_AUDIT_CONFIG", root / ".factory/visual-audit.toml"))
    if not config_path.is_absolute():
        config_path = root / config_path
    import tomllib
    with open(config_path, "rb") as stream:
        section = tomllib.load(stream).get("visual-audit", {})
    if section.get("enabled") is not True:
        die("visual audit is disabled")
    driver = section.get("capture_driver", "")
    capture_dir = section.get("capture_dir", "")
    lease_file = section.get("lease_file", "")
    inventory = section.get("inventory", "")
    if not all((driver, capture_dir, lease_file, inventory)):
        die("capture_driver, capture_dir, lease_file, inventory must be set")
    for field, value in (("capture_driver", driver), ("capture_dir", capture_dir),
                         ("lease_file", lease_file), ("inventory", inventory)):
        path = Path(value)
        if not path.is_absolute():
            path = root / path
        if field == "inventory" and (not path.is_file() or path.is_symlink()):
            die("inventory is missing or unsafe")
        if field == "capture_driver" and (not path.is_file() or path.is_symlink() or not os.access(path, os.X_OK)):
            die("capture driver is missing or not executable")

    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    tree = subprocess.check_output(["git", "write-tree"], cwd=root, text=True).strip()
    environment_blob = os.environ.get("FACTORY_ENVIRONMENT_BLOB", "")

    lease_path = Path(lease_file)
    if not lease_path.is_absolute():
        lease_path = root / lease_path
    lease_path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    lease_fd = os.open(lease_path, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC, 0o600)
    try:
        try:
            fcntl.flock(lease_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            die("another capture holds the visual-audit lease (race refused)")
        os.ftruncate(lease_fd, 0)
        os.write(lease_fd, f"{os.getpid()}|capture".encode())
        os.fsync(lease_fd)

        out_dir = Path(capture_dir)
        if not out_dir.is_absolute():
            out_dir = root / out_dir
        out_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
        if out_dir.stat().st_mode & 0o077:
            die("capture dir must not be group/world accessible")

        inventory_path = Path(inventory)
        if not inventory_path.is_absolute():
            inventory_path = root / inventory_path
        inv = json.loads(inventory_path.read_text(encoding="utf-8"))
        if inv.get("schema") != "ralph-visual-audit-inventory/v1":
            die("inventory schema invalid")
        states = inv.get("states", [])
        if not isinstance(states, list) or not states:
            die("inventory has no states")

        for state in states:
            if not isinstance(state, dict) or "id" not in state or "risk" not in state:
                die("inventory state schema invalid")
            if state.get("risk") not in {"critical", "high", "medium", "low"}:
                die(f"invalid risk for {state.get('id')}")
            state_id = state["id"]
            output = out_dir / f"{state_id}.png"
            if output.exists() and not output.is_symlink() and output.stat().st_size > 0:
                print(f"visual-audit-capture: state {state_id} already captured; skipping (deterministic)")
                continue
            driver_path = Path(driver)
            if not driver_path.is_absolute():
                driver_path = root / driver_path
            print(f"visual-audit-capture: capturing state {state_id} (risk {state['risk']})", flush=True)
            result = subprocess.run(
                [str(driver_path), state_id, str(output), commit],
                cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            if result.stdout:
                sys.stdout.write(result.stdout.decode("utf-8", errors="replace"))
            if result.returncode != 0:
                die(f"driver failed for state {state_id} (rc={result.returncode})")
            if not output.is_file() or output.is_symlink() or output.stat().st_size == 0:
                die(f"driver produced no image for state {state_id}")
    finally:
        try:
            fcntl.flock(lease_fd, fcntl.LOCK_UN)
        except OSError:
            pass
        os.close(lease_fd)

    # Provenance manifest, bound to the exact commit/tree.
    scripts = root / "scripts"
    subprocess.run(
        [sys.executable, str(scripts / "visual-audit-provenance.py"), "manifest",
         "--out", str(out_dir), "--commit", commit, "--tree", tree,
         "--environment", environment_blob],
        cwd=root, check=True, stdout=subprocess.DEVNULL,
    )
    subprocess.run(
        [sys.executable, str(scripts / "visual-audit-provenance.py"), "verify",
         "--out", str(out_dir)],
        cwd=root, check=True,
    )
    count = len(list(out_dir.glob("*.png")))
    print(f"visual-audit-capture: captured {count} state images at {commit[:12]}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
