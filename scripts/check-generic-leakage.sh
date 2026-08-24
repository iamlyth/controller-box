#!/usr/bin/env bash
# Controller adoption leakage gate for the committed fresh-factory surface.
#
# Product configuration is expected in this repository. This gate therefore
# scans only the hidden factory implementation, and rejects legacy orchestration
# authority or byte-for-byte copies of production assets under that surface.
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ -n "${FACTORY_VERIFIER_ROOT:-}" ]]; then
    PROJECT_ROOT=$(realpath -e -- "$FACTORY_VERIFIER_ROOT")
else
    PROJECT_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
fi
cd -- "$PROJECT_ROOT"

python3 - <<'PY'
from __future__ import annotations
import hashlib
from pathlib import Path
import re
import subprocess
import sys

root = Path.cwd()
tracked = subprocess.run(
    ["git", "ls-files", "-z"], check=True, stdout=subprocess.PIPE
).stdout.split(b"\0")
paths = [p.decode("utf-8", "strict") for p in tracked if p]

implementation_prefixes = (
    ".factory/loop/", ".factory/bin/", ".factory/smoke/",
    ".factory/prompts/", ".factory/schemas/", ".factory/tests/",
)
implementation = [p for p in paths if p.startswith(implementation_prefixes)]

# migration.py and its tests are the sole authority allowed to name retired
# paths. footprint.py may name `.ralph` only to lstat the root without walking.
legacy_allow = {
    ".factory/loop/migration.py",
    ".factory/loop/footprint.py",
    ".factory/loop/workspace_confinement.py",
    ".factory/tests/test-factory-migration.py",
    ".factory/tests/test-factory-migration.sh",
    ".factory/tests/test-factory-footprint.py",
    ".factory/tests/test-factory-footprint.sh",
    ".factory/tests/test-factory-adversarial.py",
    ".factory/tests/test-factory-adversarial.sh",
}
legacy = re.compile(
    rb"(?:\.ralph(?:/|\\)|context-summary|scripts/ralph-|pi-ralph-|"
    rb"pi-cli-shims/ralph|\.factory/ralph/)"
)
secret = re.compile(
    rb"(?i)(?:BEGIN (?:OPENSSH|RSA|EC|DSA) PRIVATE KEY|"
    rb"authorization:\s*(?:bearer|basic)\s+[A-Za-z0-9+/_.=-]{8,}|"
    rb"(?:api[_-]?key|access[_-]?token|client[_-]?secret|password)\s*[:=]\s*"
    rb"['\"]?[A-Za-z0-9+/_.=-]{16,})"
)

failed = []
hidden_hashes: dict[str, list[str]] = {}
for rel in implementation:
    path = root / rel
    if path.is_symlink() or not path.is_file():
        failed.append(f"unsafe hidden-factory inode: {rel}")
        continue
    data = path.read_bytes()
    if secret.search(data):
        failed.append(f"credential-like content in hidden factory: {rel}")
    runtime_surface = rel.startswith((
        ".factory/loop/", ".factory/bin/", ".factory/smoke/"
    ))
    if runtime_surface and rel not in legacy_allow and legacy.search(data):
        failed.append(f"legacy orchestration reference in hidden runtime: {rel}")
    hidden_hashes.setdefault(hashlib.sha256(data).hexdigest(), []).append(rel)

product_prefixes = ("src/", "data/", "packaging/")
product_paths = [
    p for p in paths
    if p.startswith(product_prefixes) or p in {"CMakeLists.txt", "config.h.in"}
]
for rel in product_paths:
    path = root / rel
    if path.is_symlink() or not path.is_file():
        continue
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    for hidden in hidden_hashes.get(digest, ()):  # exact bytes, no fuzzy claim
        failed.append(f"production blob duplicated in hidden factory: {rel} == {hidden}")

if failed:
    for item in sorted(set(failed)):
        print(f"check-generic-leakage: {item}", file=sys.stderr)
    raise SystemExit(1)
print(
    "check-generic-leakage: Controller factory surface has no credentials, "
    "legacy authority, or duplicated production blobs "
    f"({len(implementation)} hidden files scanned)"
)
PY
