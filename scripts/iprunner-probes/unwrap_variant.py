#!/usr/bin/env python3
"""Defensive unwrapping of busctl property variants.

busctl emits property values in two shapes, depending on how the value was
obtained:
  * --json=short get-property / method call replies, e.g.
        {"type":"s","data":"xb360"}
        {"type":"v","data":{"type":"s","data":"xb360"}}
  * GetManagedObjects / org.freedesktop.DBus.Properties.Get: the property is a
    variant whose payload is under `body`, e.g.
        {"type":"v","body":{"type":"s","data":"Xbox 360 Controller"}}

variant_value() unwraps any of these — `data` and `body`, nested variants,
dict/list/scalar payloads — down to the plain value. It is used by the
controller-production-routing probe's ObjectManager parsing and by the
create-xb360-targets live helper, so a single defensively correct
implementation is unit-tested here.

--check reads one JSON document on stdin and prints the unwrapped value as
JSON for deterministic unit tests.

Usage:
  python3 unwrap_variant.py --check    # JSON on stdin -> unwrapped value
Exit codes: 0 = unwrapped, 2 = usage.
"""
import json
import sys


def variant_value(prop):
    """Return the plain value of a busctl property variant.

    Handles dicts with `data`, dicts with a dict/list `body`, nested
    variants, and passthrough of dict/list/scalar values.
    """
    if isinstance(prop, dict):
        # GetManagedObjects / Properties.Get variant shape.
        if "body" in prop:
            return variant_value(prop["body"])
        # get-property / method-call reply shape.
        if "data" in prop:
            return variant_value(prop["data"])
        # A plain dict value (e.g. a struct) with no variant wrapper.
        return prop
    # List, str, int, float, bool, None pass through as-is.
    return prop


def unwrap_single(prop):
    """Unwrap a variant and, if the result is a one-element list (a shape
    some method-call replies use), return that single element."""
    value = variant_value(prop)
    if isinstance(value, list) and len(value) == 1:
        return value[0]
    return value


def main() -> int:
    if "--check" not in sys.argv[1:]:
        sys.stderr.write("usage: unwrap_variant.py --check\n")
        return 2
    doc = sys.stdin.read()
    value = variant_value(json.loads(doc))
    sys.stdout.write(json.dumps(value))
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
