#!/usr/bin/env python3
"""Validate that a set of DBus Target.Name values collectively matches a set
of evdev EVIOCGNAME node names: exactly `expected` of each, identical
multisets, no extra nodes. Used by create-xb360-targets.sh in both live and
fixture modes to empirically validate the DBus Target.Name <-> EVIOCGNAME
identity/cardinality assumption.

Usage:
  python3 validate_name_sets.py --expected N --target-names FILE --node-names FILE
Exit: 0 = exact collective match, 1 = mismatch (any reason).
"""
import sys


def optval(flag: str):
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == flag and i + 1 < len(args):
            return args[i + 1]
    return None


def read_names(path: str):
    names = []
    with open(path, encoding="utf-8") as stream:
        for line in stream:
            line = line.strip()
            if line:
                names.append(line)
    return names


def main() -> int:
    if "--expected" not in sys.argv[1:]:
        sys.stderr.write("usage: validate_name_sets.py --expected N "
                         "--target-names FILE --node-names FILE\n")
        return 2
    expected = int(optval("--expected"))
    target_path = optval("--target-names")
    node_path = optval("--node-names")
    targets = read_names(target_path)
    nodes = read_names(node_path)

    if len(targets) != expected:
        sys.stderr.write(f"FAIL: expected {expected} target names, got {len(targets)}\n")
        return 1
    if len(nodes) != expected:
        sys.stderr.write(f"FAIL: expected {expected} node names, got {len(nodes)} "
                         "(extra or missing evdev node)\n")
        return 1
    if sorted(targets) != sorted(nodes):
        sys.stderr.write("FAIL: DBus Target.Name set does not match the evdev "
                         "EVIOCGNAME set (identity/cardinality mismatch)\n")
        return 1
    print(f"OK: {expected} target names collectively match {expected} evdev node names")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
