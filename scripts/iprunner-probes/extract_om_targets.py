#!/usr/bin/env python3
"""Extract new org.shadowblip.Input.Target object paths from a
GetManagedObjects JSON snapshot, given a baseline set.

Requires each new target to have DeviceType==xb360 and a non-empty Name
(variant values unwrapped defensively: `data`, `body`, dict/list/scalar
forms). Prints "path<TAB>Name" per new conforming target. Fails if any new
target is non-xb360 or nameless, so a wrong target name/type cannot slip into
a topology claim.

Usage:
  python3 extract_om_targets.py OM_JSON BASELINE IFACE
Exit codes: 0 = OK (path<TAB>Name lines on stdout), 1 = non-conforming target.
"""
import json
import sys

from unwrap_variant import variant_value


def main() -> int:
    try:
        om = json.loads(sys.argv[1])
    except json.JSONDecodeError:
        # argv[1] may be a file path to the GetManagedObjects JSON snapshot.
        with open(sys.argv[1], encoding="utf-8") as stream:
            om = json.load(stream)
    iface = sys.argv[3]
    base = set()
    try:
        for line in open(sys.argv[2], encoding="utf-8"):
            line = line.strip()
            if line:
                base.add(line)
    except FileNotFoundError:
        pass

    new_paths = [p for p in sorted(om) if iface in om[p] and p not in base]

    bad = 0
    for p in new_paths:
        props = om[p].get(iface, {})
        dt = variant_value(props.get("DeviceType"))
        name = variant_value(props.get("Name"))
        if dt != "xb360":
            print(f"FAIL: new target {p} DeviceType={dt!r} is not xb360",
                  file=sys.stderr)
            bad = 1
        if not isinstance(name, str) or not name:
            print(f"FAIL: new target {p} has no Name", file=sys.stderr)
            bad = 1
    if bad:
        sys.exit(1)

    for p in new_paths:
        name = variant_value(om[p].get(iface, {}).get("Name"))
        print(f"{p}\t{name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
