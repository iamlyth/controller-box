#!/usr/bin/env python3
"""Strictly decode a busctl GetManagedObjects reply and list new targets.

The input must be the real ``busctl --json=short`` method envelope
``{"type":"a{oa{sa{sv}}}","data":[{...}]}``.  Naked maps and malformed,
ambiguous, empty, extra-field, or wrong-signature envelopes fail closed.

Usage:
  extract_om_targets.py [--all-paths] OM_JSON BASELINE IFACE
"""
import json
import sys

from unwrap_variant import decode_object_manager, variant_value


def fail(message):
    print(f"FAIL: {message}", file=sys.stderr)
    return 1


def main():
    args = sys.argv[1:]
    all_paths = bool(args and args[0] == "--all-paths")
    if all_paths:
        args = args[1:]
    if len(args) != 3:
        return fail("usage: extract_om_targets.py [--all-paths] OM_JSON BASELINE IFACE")
    source, baseline, iface = args
    try:
        try:
            doc = json.loads(source)
        except json.JSONDecodeError:
            with open(source, encoding="utf-8") as stream:
                doc = json.load(stream)
        om = decode_object_manager(doc, require_nonempty=True)
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as exc:
        return fail(f"invalid ObjectManager reply: {exc}")

    base = set()
    try:
        with open(baseline, encoding="utf-8") as stream:
            for line in stream:
                line = line.strip()
                if line:
                    base.add(line)
    except FileNotFoundError:
        pass

    new_paths = [path for path in sorted(om) if iface in om[path] and path not in base]
    if all_paths:
        for path in new_paths:
            print(path)
        return 0

    bad = False
    for path in new_paths:
        props = om[path][iface]
        device_type = variant_value(props.get("DeviceType"))
        name = variant_value(props.get("Name"))
        if device_type != "xb360":
            print(f"FAIL: new target {path} DeviceType={device_type!r} is not xb360", file=sys.stderr)
            bad = True
        if not isinstance(name, str) or not name:
            print(f"FAIL: new target {path} has no Name", file=sys.stderr)
            bad = True
    if bad:
        return 1

    for path in new_paths:
        name = variant_value(om[path][iface].get("Name"))
        print(f"{path}\t{name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
