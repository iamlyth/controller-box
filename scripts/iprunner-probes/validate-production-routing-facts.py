#!/usr/bin/env python3
"""Validate the committed controller-production-routing fact bundle.

Non-skipping validator for `controller-production-routing` (Controller-Box
infrastructure). In fixture mode the probe passes it a committed JSON fact
bundle (scripts/probe-controller-production-routing.sh --fixture DIR) that
records, with artifact hashes, the topology, observer, bus-identity, and
cleanup facts of a recorded production-routing run. This validator rejects:

  * an unsafe (symlinked) or unparsable facts.json,
  * a bundle whose schema is not iprunner-controller-production-routing-facts/v1,
  * a bundle that declares non-isolated HOME or a non-4-xb360 default topology,
  * a bundle with boolean-typed counts (a bool is not an int),
  * a bundle whose bus-identity facts are not the pinned real system bus,
  * a bundle whose observer/cleanup fields are malformed,
  * an artifact or event-stream path that is absolute, contains "..", or
    escapes the fixture directory (including via symlink),
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
PINNED_INPUTPLUMBER = "/usr/bin/inputplumber"


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


def confined_path(rel: str, fixture_dir: Path, label: str) -> Path:
    """Return a path inside fixture_dir for a relative ref, or fail on any
    absolute / '..' / symlink escape. The returned path is the un-resolved
    candidate; callers additionally reject symlinked final targets."""
    p = Path(rel)
    if p.is_absolute():
        fail(f"{label} path must be relative: {rel}")
    if ".." in p.parts:
        fail(f"{label} path must not traverse '..': {rel}")
    base = fixture_dir.resolve()
    candidate = fixture_dir / rel
    try:
        resolved = candidate.resolve()
        resolved.relative_to(base)
    except ValueError:
        fail(f"{label} path escapes the fixture directory: {rel}")
    return candidate


def validate(facts: dict, fixture_dir: Path) -> None:
    if facts.get("schema") != SCHEMA:
        fail(f"facts schema is not {SCHEMA}")

    # HOME isolation + the default 4-xb360 topology are contract invariants
    # for this probe. A bundle that deviates is not a real recorded run.
    home = facts.get("home", {})
    require(home.get("isolated") is True, "HOME must be isolated")
    vc = home.get("virtual_controllers", {})
    require(type(vc.get("count")) is int, "virtual controller count must be int")
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
    require(isinstance(binary.get("launch_cwd_isolated"), bool),
            "binary launch_cwd_isolated must be boolean")

    # Topology cardinality facts must be ints (bool is a subclass of int and
    # must be rejected); exactness is decided by the probe from these.
    topology = facts.get("topology", {})
    require(type(topology.get("expected")) is int, "expected targets must be int")
    require(type(topology.get("observed")) is int, "observed targets must be int")
    require(type(topology.get("target_paths")) is int, "target_paths must be int")
    require(type(topology.get("kernel_nodes")) is int, "kernel_nodes must be int")
    require(isinstance(topology.get("identities_match"), bool),
            "topology identities_match must be boolean")

    # Real bus identity: the probe must only claim routing evidence when the
    # system bus is the real root-owned socket and the InputPlumber owner
    # process is the pinned /usr/bin/inputplumber (or dpkg-owned pinned).
    bus = facts.get("bus", {})
    require(bus.get("system_socket") == "/run/dbus/system_bus_socket",
            "bus system_socket must be /run/dbus/system_bus_socket")
    require(bus.get("socket_root_owned") is True, "system bus socket must be root-owned")
    require(type(bus.get("owner_pid")) is int, "bus owner_pid must be int")
    require(isinstance(bus.get("owner_exe"), str) and bus.get("owner_exe"),
            "bus owner_exe must be a non-empty string")
    require(bus.get("owner_exe_pinned") is True, "bus owner executable must be pinned")
    require(bus.get("owner_exe") == PINNED_INPUTPLUMBER,
            f"bus owner executable must be the pinned {PINNED_INPUTPLUMBER}")

    # Observer fields: event stream is a confined path, injection flag is a
    # bool, and the physical/target EVIOCGNAME identities are strings.
    observer = facts.get("observer", {})
    require(isinstance(observer.get("event_stream"), str) and observer.get("event_stream"),
            "observer event_stream must be a non-empty path")
    require(type(observer.get("direct_injection")) is bool,
            "observer direct_injection must be boolean")
    require(isinstance(observer.get("physical_name"), str),
            "observer physical_name must be a string")
    require(isinstance(observer.get("target_name"), str),
            "observer target_name must be a string")
    stream_path = confined_path(observer["event_stream"], fixture_dir, "event stream")
    if stream_path.is_symlink() or not stream_path.is_file():
        fail(f"event stream file is missing or unsafe: {observer['event_stream']}")

    # Cleanup fields must be well-formed.
    cleanup = facts.get("cleanup", {})
    require(cleanup.get("termination") in ("ok", "fail"),
            "cleanup termination must be 'ok' or 'fail'")
    require(cleanup.get("target_cleanup") in ("ok", "fail"),
            "cleanup target_cleanup must be 'ok' or 'fail'")
    require(type(cleanup.get("targets_absent")) is bool,
            "cleanup targets_absent must be boolean")
    require(type(cleanup.get("kernel_nodes_absent")) is bool,
            "cleanup kernel_nodes_absent must be boolean")

    # Artifact hash verification: every referenced artifact must exist in the
    # fixture directory, must be confined there, and its sha256 must match the
    # committed hash. A missing file, symlink escape, or mismatched hash fails
    # the probe (missing screenshot / log / fact hash).
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
        artifact_path = confined_path(rel, fixture_dir, f"artifact {label}")
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
