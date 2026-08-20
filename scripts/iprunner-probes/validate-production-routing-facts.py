#!/usr/bin/env python3
"""Validate the committed controller-production-routing fact bundle.

Non-skipping validator for `controller-production-routing` (Controller-Box
infrastructure). In fixture mode the probe passes it a committed JSON fact
bundle (scripts/probe-controller-production-routing.sh --fixture DIR) that
records, with artifact hashes, the topology, observer, and cleanup facts of a
recorded production-routing run. This validator rejects:

  * an unsafe (symlinked) or unparsable facts.json,
  * a bundle whose schema is not iprunner-controller-production-routing-facts/v1,
  * a bundle that declares non-isolated HOME or a non-4-xb360 default topology,
  * a bundle whose referenced artifact files are missing,
  * a bundle whose artifact sha256 hashes do not match the files on disk.

The committed contract probe_argv never passes --fixture, so live runs
collect and validate facts directly rather than through a bundle.

Usage:
  validate-production-routing-facts.py --facts FACTS.json --fixture-dir DIR
Exit codes: 0 = bundle internally consistent, 1 = invalid (any reason).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

SCHEMA = "iprunner-controller-production-routing-facts/v1"


def fail(message: str) -> None:
    sys.stderr.write(f"production-routing-probe: {message}\n")
    raise SystemExit(1)


def load_json(path: Path, label: str) -> dict:
    if path.is_symlink() or not path.is_file():
        fail(f"{label} file is missing or unsafe: {path}")
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        fail(f"{label} file is invalid: {path}: {exc}")
    if not isinstance(data, dict):
        fail(f"{label} file must be an object")
    return data


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate(facts: dict, fixture_dir: Path) -> None:
    if facts.get("schema") != SCHEMA:
        fail(f"facts schema is not {SCHEMA}")

    # HOME isolation + the default 4-xb360 topology are contract invariants
    # for this probe. A bundle that deviates is not a real recorded run.
    home = facts.get("home", {})
    require(home.get("isolated") is True, "HOME must be isolated")
    vc = home.get("virtual_controllers", {})
    require(vc.get("count") == 4, "virtual controller count must be 4")
    types = vc.get("types", [])
    require(
        len(types) == 4 and all(t == "xb360" for t in types),
        "default topology must request exactly 4 xb360 controllers",
    )

    # Binary placement facts must be present and boolean.
    binary = facts.get("binary", {})
    require(isinstance(binary.get("realpath_inside_prefix"), bool),
            "binary realpath_inside_prefix must be boolean")
    require(isinstance(binary.get("assets_inside_prefix"), bool),
            "binary assets_inside_prefix must be boolean")

    # Topology cardinality facts must be integers; exactness is decided by
    # the probe from these.
    topology = facts.get("topology", {})
    require(isinstance(topology.get("expected"), int), "expected targets must be int")
    require(isinstance(topology.get("observed"), int), "observed targets must be int")
    require(isinstance(topology.get("target_paths"), int), "target_paths must be int")
    require(isinstance(topology.get("kernel_nodes"), int), "kernel_nodes must be int")

    # Artifact hash verification: every referenced artifact must exist in the
    # fixture directory and its sha256 must match the committed hash. A
    # missing file or mismatched hash fails the probe (missing screenshot /
    # log / fact hash).
    artifacts = facts.get("artifacts", {})
    require(isinstance(artifacts, dict) and len(artifacts) > 0,
            "fact bundle must reference at least one artifact")
    for label, ref in artifacts.items():
        if not isinstance(ref, dict):
            fail(f"artifact {label} reference is not an object")
        rel = ref.get("path")
        expected = ref.get("sha256")
        if not isinstance(rel, str) or not rel:
            fail(f"artifact {label} has no path")
        if not isinstance(expected, str) or len(expected) != 64:
            fail(f"artifact {label} has no valid sha256 hash")
        artifact_path = fixture_dir / rel
        if artifact_path.is_symlink() or not artifact_path.is_file():
            fail(f"artifact {label} file is missing or unsafe: {artifact_path}")
        actual = sha256_of(artifact_path)
        if actual != expected:
            fail(f"artifact {label} sha256 mismatch: expected {expected}, got {actual}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--facts", required=True)
    parser.add_argument("--fixture-dir", required=True)
    args = parser.parse_args()
    facts_path = Path(args.facts)
    fixture_dir = Path(args.fixture_dir)
    if fixture_dir.is_symlink() or not fixture_dir.is_dir():
        fail(f"fixture directory is missing or unsafe: {fixture_dir}")
    facts = load_json(facts_path, "facts")
    validate(facts, fixture_dir)
    print("production-routing-probe: fact bundle verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
