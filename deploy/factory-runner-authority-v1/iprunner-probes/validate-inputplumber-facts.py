#!/usr/bin/env python3
"""Validate collected inputplumber system-bus facts against the committed pins.

Non-skipping capability probe validator for `inputplumber-system-dbus`. The
collector (probe-inputplumber-system-dbus.sh) gathers live facts from the real
system; this validator compares every fact against the committed expectations
(scripts/iprunner-probes/inputplumber-expectations.json). Any mismatch —
wrong package/binary/service, wrong version, wrong interface signatures,
non-writable GamepadOrder, missing ObjectManager objects, a private or
non-default bus address — fails the probe. There is no skip path.

Usage: validate-inputplumber-facts.py --facts FACTS.json [--expectations JSON]
Exit codes: 0 = all pins match, 1 = mismatch (any reason).
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

UNIQUE_NAME = re.compile(r"^:[0-9]+\.[0-9]+$")


def fail(message: str) -> None:
    sys.stderr.write(f"inputplumber-probe: {message}\n")
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


def validate(facts: dict, expectations: dict) -> None:
    if facts.get("schema") != "iprunner-inputplumber-facts/v1":
        fail("facts schema is not iprunner-inputplumber-facts/v1")
    if expectations.get("schema") != "iprunner-inputplumber-expectations/v1":
        fail("expectations schema is not iprunner-inputplumber-expectations/v1")

    # Bus: the probe must run against the real system bus. A private bus is
    # rejected both by the collector (env override) and here (effective
    # address must be the system socket).
    bus_expectations = expectations["bus"]
    require(
        facts.get("dbus_system_bus_address_env") is None,
        "DBUS_SYSTEM_BUS_ADDRESS must not be set (private-bus override rejected)",
    )
    require(
        facts.get("bus_address_effective") == "unix:path=" + bus_expectations["system_socket"],
        "effective system bus address is not the default system socket",
    )
    require(
        facts.get("name_owner", {}).get("has_owner") is True,
        "org.shadowblip.InputPlumber has no unique bus owner",
    )
    owner = facts["name_owner"].get("unique_owner", "")
    require(
        isinstance(owner, str) and UNIQUE_NAME.fullmatch(owner),
        f"name owner is not a real unique bus name: {owner!r}",
    )

    # Package: exact pinned version.
    package = facts.get("package", {})
    require(package.get("installed") is True, "inputplumber package is not installed")
    require(
        package.get("name") == expectations["package"]["name"],
        f"package name mismatch: {package.get('name')!r}",
    )
    require(
        package.get("version") == expectations["package"]["version"],
        f"package version mismatch: {package.get('version')!r}",
    )

    # Binary: the package ships an executable binary.
    binary = facts.get("binary", {})
    require(binary.get("path"), "no inputplumber binary discovered")
    require(binary.get("exists") is True, "inputplumber binary does not exist")
    require(binary.get("executable") is True, "inputplumber binary is not executable")
    require(
        binary.get("owned_by_package") is True,
        "inputplumber binary is not owned by the inputplumber package",
    )

    # Service: active and Type=dbus.
    service = facts.get("service", {})
    require(
        service.get("unit") == expectations["service"]["unit"],
        f"service unit mismatch: {service.get('unit')!r}",
    )
    require(service.get("active") is True, "inputplumber.service is not active")
    require(
        service.get("type") == expectations["service"]["type"],
        f"service type mismatch: {service.get('type')!r}",
    )
    require(
        service.get("exec_start_binary") == binary.get("path"),
        "service ExecStart does not run the inputplumber package binary",
    )

    # Manager version: exact.
    manager = facts.get("manager", {})
    expected_manager = expectations["manager"]
    require(
        manager.get("path") == expected_manager["path"],
        "manager object path mismatch",
    )
    require(
        manager.get("interface") == expected_manager["interface"],
        "manager interface name mismatch",
    )
    require(
        manager.get("version") == expected_manager["version"],
        f"manager version mismatch: {manager.get('version')!r}",
    )

    # Manager interface signatures: exact method set.
    signatures = manager.get("methods", {})
    for method, signature in expected_manager["methods"].items():
        require(
            signatures.get(method) == signature,
            f"manager method {method} signature mismatch (expected {signature}, got {signatures.get(method)!r})",
        )
    # No extra manager methods beyond the pinned set would be a future API
    # change worth re-pinning; the exact set must match.
    require(
        set(signatures) == set(expected_manager["methods"]),
        "manager method set does not match the pinned set exactly",
    )

    # GamepadOrder must be writable with signature 'as'.
    require(
        manager.get("gamepad_order_writable") is True,
        "Manager.GamepadOrder is not writable",
    )
    require(
        manager.get("gamepad_order_signature") == expected_manager["gamepad_order"]["signature"],
        "Manager.GamepadOrder signature is not 'as'",
    )

    # Supported target device IDs must include xb360.
    ids = manager.get("supported_target_device_ids", [])
    for required_id in expected_manager["supported_target_device_ids_required"]:
        require(
            required_id in ids,
            f"SupportedTargetDeviceIds does not include {required_id}",
        )

    # ObjectManager must be real and non-empty.
    object_manager = facts.get("object_manager", {})
    require(
        object_manager.get("path") == expectations["object_manager"]["path"],
        "ObjectManager path mismatch",
    )
    require(
        object_manager.get("interface") == expectations["object_manager"]["interface"],
        "ObjectManager interface mismatch",
    )
    require(
        isinstance(object_manager.get("objects"), int)
        and object_manager["objects"] >= expectations["object_manager"]["minimum_objects"],
        "ObjectManager returned no managed objects",
    )

    # The interface names the production client binds to must be present in
    # the manager introspection (target/composite/source interfaces).
    present = set(facts.get("interfaces_present", []))
    for interface in expectations["interface_names"].values():
        require(
            interface in present,
            f"required interface {interface} is not present in introspection",
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--facts", required=True)
    parser.add_argument(
        "--expectations",
        default=str(Path(__file__).resolve().parent / "inputplumber-expectations.json"),
    )
    args = parser.parse_args()
    facts = load_json(Path(args.facts), "facts")
    expectations = load_json(Path(args.expectations), "expectations")
    validate(facts, expectations)
    print("inputplumber-system-dbus-probe: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
