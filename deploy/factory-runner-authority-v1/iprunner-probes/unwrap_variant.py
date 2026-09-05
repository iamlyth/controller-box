#!/usr/bin/env python3
"""Strict decoders for busctl JSON replies and systemd ExecStart values.

The loose :func:`variant_value` helper is retained only for variants nested
inside a validated ObjectManager reply.  Top-level busctl replies are decoded
by signature-specific helpers which reject missing/extra fields, ambiguous
lists, and wrong result types.
"""
import json
import re
import shlex
import sys

_OBJECT_PATH = re.compile(r"^/(?:[A-Za-z0-9_]+(?:/[A-Za-z0-9_]+)*)?$")


def variant_value(prop):
    """Unwrap a property variant after its containing reply was validated."""
    if isinstance(prop, dict):
        if "body" in prop:
            return variant_value(prop["body"])
        if "data" in prop:
            return variant_value(prop["data"])
        return prop
    return prop


def _envelope(doc, signature):
    if not isinstance(doc, dict) or set(doc) != {"type", "data"}:
        raise ValueError("busctl reply must contain exactly type and data")
    if doc["type"] != signature:
        raise ValueError(f"busctl reply signature must be {signature!r}")
    return doc["data"]


def decode_method_single(doc, signature):
    data = _envelope(doc, signature)
    if not isinstance(data, list) or len(data) != 1:
        raise ValueError("method reply data must contain exactly one value")
    return data[0]


def decode_object_path(doc):
    value = decode_method_single(doc, "o")
    if not isinstance(value, str) or not _OBJECT_PATH.fullmatch(value):
        raise ValueError("method reply is not one absolute object path")
    return value


def decode_string_method(doc):
    value = decode_method_single(doc, "s")
    if not isinstance(value, str):
        raise ValueError("method reply is not one string")
    return value


def decode_property(doc, signature):
    value = _envelope(doc, signature)
    if signature == "s":
        if not isinstance(value, str):
            raise ValueError("string property payload has wrong type")
    elif signature == "as":
        if not isinstance(value, list) or not all(isinstance(v, str) for v in value):
            raise ValueError("string-array property payload has wrong type")
    else:
        raise ValueError("unsupported property signature")
    return value


def decode_object_manager(doc, *, require_nonempty=True):
    value = decode_method_single(doc, "a{oa{sa{sv}}}")
    if not isinstance(value, dict):
        raise ValueError("ObjectManager result must be exactly one dictionary")
    if require_nonempty and not value:
        raise ValueError("ObjectManager result must not be empty")
    for path, interfaces in value.items():
        if not isinstance(path, str) or not _OBJECT_PATH.fullmatch(path):
            raise ValueError(f"invalid ObjectManager object path: {path!r}")
        if not isinstance(interfaces, dict) or not all(
            isinstance(name, str) and isinstance(props, dict)
            for name, props in interfaces.items()
        ):
            raise ValueError(f"invalid ObjectManager interfaces at {path!r}")
    return value


def parse_exec_start(value):
    """Extract one absolute executable from systemctl show ExecStart output."""
    if not isinstance(value, str) or not value or "\n" in value or "\0" in value:
        raise ValueError("ExecStart is empty or malformed")
    text = value.strip()
    if text.startswith("{"):
        if not text.endswith("}") or text.count("{") != 1 or text.count("}") != 1:
            raise ValueError("ExecStart must contain exactly one command structure")
        fields = [field.strip() for field in text[1:-1].split(";") if field.strip()]
        paths = [field[5:].strip() for field in fields if field.startswith("path=")]
        if len(paths) != 1:
            raise ValueError("ExecStart command structure must contain exactly one path")
        path = paths[0]
        if any(ch.isspace() for ch in path) or path.startswith(('"', "'")):
            raise ValueError("ExecStart path is not an unambiguous executable")
    else:
        try:
            argv = shlex.split(text, posix=True)
        except ValueError as exc:
            raise ValueError("ExecStart command line is malformed") from exc
        if not argv:
            raise ValueError("ExecStart command line is empty")
        path = argv[0]
    if not path.startswith("/") or path == "/" or any(
        ch.isspace() or ch in "{};" for ch in path
    ):
        raise ValueError("ExecStart executable must be an absolute path")
    return path


def unwrap_single(prop):
    """Legacy variant helper; do not use for top-level method replies."""
    value = variant_value(prop)
    if isinstance(value, list) and len(value) == 1:
        return value[0]
    return value


def main():
    modes = [arg for arg in sys.argv[1:] if arg.startswith("--")]
    if len(modes) != 1:
        sys.stderr.write("usage: unwrap_variant.py --check|--object-manager|--object-path|--string-method|--property-s|--property-as|--exec-start\n")
        return 2
    mode = modes[0]
    try:
        if mode == "--exec-start":
            value = parse_exec_start(sys.stdin.read().rstrip("\n"))
        else:
            doc = json.load(sys.stdin)
            if mode == "--check":
                value = variant_value(doc)
            elif mode == "--object-manager":
                value = decode_object_manager(doc)
            elif mode == "--object-path":
                value = decode_object_path(doc)
            elif mode == "--string-method":
                value = decode_string_method(doc)
            elif mode == "--property-s":
                value = decode_property(doc, "s")
            elif mode == "--property-as":
                value = decode_property(doc, "as")
            else:
                raise ValueError("unknown mode")
    except (json.JSONDecodeError, ValueError) as exc:
        sys.stderr.write(f"decode error: {exc}\n")
        return 1
    sys.stdout.write(json.dumps(value) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
