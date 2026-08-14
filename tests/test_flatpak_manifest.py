#!/usr/bin/env python3
"""tests/test_flatpak_manifest.py — validate Flatpak manifest.

Validates packaging/org.shadowblip.ControllerBox.yaml against the acceptance
criteria from .factory/artifacts/implementation-plan.md Task 42 and SPEC §9.1.

Run: python3 tests/test_flatpak_manifest.py
Exit: 0 on success, 1 on failure.
"""

import sys
import os
import re

try:
    import yaml
except ImportError:
    print("FAIL: PyYAML not available — install with: pip install pyyaml", file=sys.stderr)
    sys.exit(1)

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(PROJECT_ROOT, "packaging", "org.shadowblip.ControllerBox.yaml")
MANIFEST = os.path.normpath(MANIFEST)

errors = 0


def ok(msg):
    print(f"OK:  {msg}")


def err(msg):
    global errors
    print(f"FAIL: {msg}", file=sys.stderr)
    errors += 1


# --- Manifest file exists ---
if not os.path.isfile(MANIFEST):
    print(f"FAIL: manifest file not found at {MANIFEST}", file=sys.stderr)
    sys.exit(1)
ok("manifest file exists")

# --- Parse YAML ---
with open(MANIFEST) as f:
    raw_content = f.read()
try:
    data = yaml.safe_load(raw_content)
except yaml.YAMLError as e:
    err(f"manifest is not valid YAML: {e}")
    sys.exit(1)
ok("manifest is valid YAML")

# --- Check app-id ---
app_id = data.get("app-id", "")
if app_id == "org.shadowblip.ControllerBox":
    ok(f"app-id is {app_id}")
else:
    err(f"app-id is {app_id!r}, expected 'org.shadowblip.ControllerBox'")

# --- Check runtime and SDK ---
rt = data.get("runtime", "")
rtv = data.get("runtime-version", "")
sdk = data.get("sdk", "")
if "org.freedesktop.Platform" in rt:
    ok(f"runtime is {rt} {rtv}")
else:
    err(f"runtime is {rt!r}")

if "org.freedesktop.Sdk" in sdk:
    ok(f"sdk is {sdk}")
else:
    err(f"sdk is {sdk!r}")

# --- Check command ---
cmd = data.get("command", "")
if cmd == "controller-box":
    ok(f"command is {cmd}")
else:
    err(f"command is {cmd!r}, expected 'controller-box'")

# --- Check modules ---
modules = [m.get("name", "") for m in data.get("modules", [])]
required_modules = ["SDL2", "SDL2_ttf", "SDL2_image", "controller-box"]
for r in required_modules:
    if r in modules:
        ok(f"module '{r}' present")
    else:
        err(f"module '{r}' missing")
ok(f"all {len(modules)} modules present: {modules}")

# --- Check that nanosvg is NOT a separate module (vendored in source tree) ---
if "nanosvg" not in modules:
    ok("nanosvg not a separate module (vendored in source tree)")
else:
    err("nanosvg should not be a separate module")

# --- Check required permissions (SPEC §9.1 + plan) ---
args = data.get("finish-args", [])
required_perms = [
    "--system-talk-name=org.shadowblip.InputPlumber",
    "--filesystem=~/.local/share/inputplumber/profiles",
    "--filesystem=/usr/share/inputplumber:ro",
    "--filesystem=~/.config/controller-box",
    "--filesystem=~/.config/systemd/user",
]
for perm in required_perms:
    if perm in args:
        ok(f"permission {perm}")
    else:
        err(f"missing permission {perm}")

# --- Check display permissions ---
display_perms = ["--socket=wayland", "--socket=fallback-x11", "--device=dri"]
for perm in display_perms:
    if perm in args:
        ok(f"display permission {perm}")
    else:
        err(f"missing display permission {perm}")

# --- Check flatpak-spawn permission ---
if "--talk-name=org.freedesktop.Flatpak" in args:
    ok("flatpak-spawn permission present")
else:
    err("missing --talk-name=org.freedesktop.Flatpak for flatpak-spawn --host")

# --- Check no overly broad permissions ---
forbidden = [
    "--filesystem=host",
    "--filesystem=home",
    "--socket=system-bus",
    "--device=all",
]
for f in forbidden:
    if f in args:
        err(f"overly broad permission {f}")
if not any(f in args for f in forbidden):
    ok("no overly broad permissions")

# --- Check each permission has a rationale comment in the raw file ---
lines = raw_content.split("\n")
in_finish = False
perm_count = 0
commented_count = 0
for i, line in enumerate(lines):
    stripped = line.strip()
    if stripped == "finish-args:":
        in_finish = True
        continue
    if in_finish:
        if stripped.startswith("- --"):
            perm_count += 1
            has_comment = False
            # Check inline comment (after the permission, on same line)
            if "#" in line and not line.strip().startswith("#"):
                has_comment = True
            # Check preceding comment lines
            if not has_comment:
                j = i - 1
                while j >= 0 and lines[j].strip().startswith("#"):
                    has_comment = True
                    break
            if has_comment:
                commented_count += 1
        elif stripped == "modules:":
            break

if commented_count >= 8:
    ok(f"{commented_count}/{perm_count} permissions have rationale comments")
else:
    err(f"only {commented_count}/{perm_count} permissions have rationale comments (need >= 8)")

# --- Check SDL2 modules use cmake buildsystem ---
for m in data.get("modules", []):
    name = m.get("name", "")
    bs = m.get("buildsystem", "")
    if name in ("SDL2", "SDL2_ttf", "SDL2_image", "controller-box"):
        if "cmake" in bs:
            ok(f"{name} uses {bs}")
        else:
            err(f"{name} uses {bs!r}, expected cmake-based")

# --- Check controller-box module has git source ---
for m in data.get("modules", []):
    if m.get("name") == "controller-box":
        sources = m.get("sources", [])
        if sources and sources[0].get("type") == "git":
            url = sources[0].get("url", "")
            if "controller-box" in url:
                ok(f"controller-box module uses git source ({url})")
            else:
                err(f"controller-box git url does not contain 'controller-box': {url}")
        else:
            err("controller-box module missing git source")
        # Check post-install exists (desktop file fix)
        if m.get("post-install"):
            ok(f"controller-box has post-install ({len(m['post-install'])} commands)")
        else:
            err("controller-box missing post-install (desktop file fix)")

# --- Check SDL2 source URLs are valid ---
for m in data.get("modules", []):
    name = m.get("name", "")
    if name in ("SDL2", "SDL2_ttf", "SDL2_image"):
        sources = m.get("sources", [])
        if sources:
            src = sources[0]
            if src.get("type") == "git" and src.get("url"):
                ok(f"{name} has git source: {src['url']}")
            else:
                err(f"{name} missing valid source")

# --- Check libyaml module ---
libyaml_found = False
for m in data.get("modules", []):
    if m.get("name") == "libyaml":
        libyaml_found = True
        sources = m.get("sources", [])
        if sources:
            src = sources[0]
            if src.get("type") in ("git", "archive") and src.get("url"):
                ok(f"libyaml has source: {src['url']}")
            else:
                err("libyaml missing valid source")
if libyaml_found:
    ok("libyaml module present (ensures YAML lib availability)")
else:
    err("libyaml module missing")

# --- Summary ---
print()
if errors == 0:
    print("=== All flatpak manifest checks passed ===")
    sys.exit(0)
else:
    print(f"=== {errors} check(s) failed ===", file=sys.stderr)
    sys.exit(1)