#!/usr/bin/env python3
"""Snapshot and validate Ralph event reception for one supervised attempt."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat

from factory_state_io import (
    StateIOError,
    atomic_write_json,
    read_bytes,
    read_json,
    read_text,
    remove,
)

ROOT = Path(__file__).resolve().parent.parent
RALPH = ROOT / ".ralph"
TOKENS = {
    "PLAN_COMPLETE",
    "LOOP_COMPLETE",
    "AUDIT_COMPLETE",
    "MAINTENANCE_PLAN_COMPLETE",
    "MAINTENANCE_COMPLETE",
}
MODES = {"planning", "implementation", "campaign-audit", "maintenance-planning", "maintenance"}
START_TOPICS = {
    "planning": "factory.plan",
    "implementation": "factory.implement",
    "campaign-audit": "factory.audit",
    "maintenance-planning": "factory.maintenance.plan",
    "maintenance": "factory.maintenance.implement",
}
EVENT_NAME = re.compile(r"^events(?:-[0-9]{8}-[0-9]{6})?\.jsonl$")
LOOP_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")


class BoundaryError(RuntimeError):
    pass


def _ralph_fd() -> int:
    descriptor = os.open(RALPH, os.O_RDONLY | os.O_DIRECTORY | getattr(os, "O_NOFOLLOW", 0))
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
            raise BoundaryError("unsafe .ralph directory")
        return descriptor
    except BaseException:
        os.close(descriptor)
        raise


def _read_marker(directory_fd: int, name: str, *, missing_ok: bool = False) -> str | None:
    try:
        descriptor = os.open(name, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0), dir_fd=directory_fd)
    except FileNotFoundError:
        if missing_ok:
            return None
        raise BoundaryError(f"missing Ralph marker {name}")
    try:
        info = os.fstat(descriptor)
        named = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != os.getuid()
            or info.st_nlink != 1
            or info.st_size > 4096
            or (info.st_dev, info.st_ino) != (named.st_dev, named.st_ino)
        ):
            raise BoundaryError(f"unsafe Ralph marker {name}")
        raw = os.read(descriptor, 4097)
        after = os.fstat(descriptor)
        if len(raw) > 4096 or (info.st_size, info.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise BoundaryError(f"Ralph marker changed while reading: {name}")
        return raw.decode("utf-8").strip()
    except UnicodeError as exc:
        raise BoundaryError(f"Ralph marker is not UTF-8: {name}") from exc
    finally:
        os.close(descriptor)


def _event_files(directory_fd: int) -> dict[str, dict[str, int]]:
    result: dict[str, dict[str, int]] = {}
    for name in os.listdir(directory_fd):
        if not EVENT_NAME.fullmatch(name):
            continue
        info = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_nlink != 1:
            raise BoundaryError(f"unsafe Ralph event stream: {name}")
        if info.st_size > 64 * 1024 * 1024:
            raise BoundaryError(f"Ralph event stream exceeds limit: {name}")
        result[name] = {
            "dev": info.st_dev,
            "ino": info.st_ino,
            "size": info.st_size,
            "mtime_ns": info.st_mtime_ns,
        }
    return result


def _snapshot_name(mode: str) -> str:
    return f"ralph-attempt-{mode}.json"


def _campaign_binding(mode: str) -> tuple[str | None, str | None, str | None]:
    round_number = os.environ.get("FACTORY_CAMPAIGN_ROUND")
    phase = os.environ.get("FACTORY_CAMPAIGN_PHASE")
    if round_number is None and phase is None:
        return None, None, None
    expected_phase = "audit" if mode == "campaign-audit" else mode
    if (
        phase != expected_phase
        or round_number is None
        or not re.fullmatch(r"[1-9][0-9]*", round_number)
    ):
        raise BoundaryError("invalid campaign launch binding")
    raw = read_bytes(ROOT, "ralph-campaign.json", maximum=1024 * 1024)
    assert raw is not None
    return round_number, phase, hashlib.sha256(raw).hexdigest()


def begin(mode: str) -> None:
    attempt = os.environ.get("FACTORY_RALPH_ATTEMPT_ID", "")
    cycle = os.environ.get("FACTORY_RALPH_CYCLE_ID", "")
    if not HEX64.fullmatch(attempt) or not HEX64.fullmatch(cycle):
        raise BoundaryError("missing attempt or cycle binding")
    campaign_round, campaign_phase, campaign_digest = _campaign_binding(mode)
    directory_fd = _ralph_fd()
    try:
        data = {
            "schema": "ralph-event-snapshot/v1",
            "mode": mode,
            "attempt_id": attempt,
            "cycle_id": cycle,
            "campaign_round": campaign_round,
            "campaign_phase": campaign_phase,
            "campaign_state_sha256": campaign_digest,
            "files": _event_files(directory_fd),
            "current_events": _read_marker(directory_fd, "current-events", missing_ok=True),
            "current_loop_id": _read_marker(directory_fd, "current-loop-id", missing_ok=True),
        }
    finally:
        os.close(directory_fd)
    atomic_write_json(ROOT, _snapshot_name(mode), data)


def _contains_token(value: object) -> bool:
    if isinstance(value, str):
        return any(token in value for token in TOKENS)
    if isinstance(value, list):
        return any(_contains_token(item) for item in value)
    if isinstance(value, dict):
        return any(_contains_token(key) or _contains_token(item) for key, item in value.items())
    return False


def _trusted_start(record: object, mode: str) -> bool:
    return (
        isinstance(record, dict)
        and set(record) == {"ts", "iteration", "hat", "topic", "triggered", "payload"}
        and record.get("iteration") == 0
        and record.get("hat") == "loop"
        and record.get("topic") == START_TOPICS[mode]
        and isinstance(record.get("ts"), str)
        and isinstance(record.get("triggered"), str)
        and isinstance(record.get("payload"), str)
    )


def _read_delta(directory_fd: int, name: str, offset: int) -> tuple[list[object], os.stat_result]:
    descriptor = os.open(name, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0), dir_fd=directory_fd)
    try:
        info = os.fstat(descriptor)
        named = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != os.getuid()
            or info.st_nlink != 1
            or (info.st_dev, info.st_ino) != (named.st_dev, named.st_ino)
            or info.st_size < offset
        ):
            raise BoundaryError(f"event stream changed unsafely: {name}")
        length = info.st_size - offset
        if length > 8 * 1024 * 1024:
            raise BoundaryError("attempt event delta exceeds 8 MiB")
        raw = os.pread(descriptor, length, offset)
        after = os.fstat(descriptor)
        if (info.st_size, info.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise BoundaryError(f"event stream changed while validating: {name}")
    finally:
        os.close(descriptor)
    if raw and not raw.endswith(b"\n"):
        raise BoundaryError(f"event stream lacks a complete final record: {name}")
    records: list[object] = []
    for line in raw.splitlines():
        if not line:
            continue
        try:
            record = json.loads(line.decode("utf-8"))
        except (UnicodeError, json.JSONDecodeError) as exc:
            raise BoundaryError(f"invalid event record in {name}: {exc}") from exc
        if not isinstance(record, dict):
            raise BoundaryError(f"event record is not an object: {name}")
        records.append(record)
    return records, info


def finish(mode: str) -> bool:
    snapshot = read_json(ROOT, _snapshot_name(mode), maximum=1024 * 1024)
    if not isinstance(snapshot, dict) or set(snapshot) != {
        "schema", "mode", "attempt_id", "cycle_id", "campaign_round",
        "campaign_phase", "campaign_state_sha256", "files", "current_events",
        "current_loop_id",
    }:
        raise BoundaryError("attempt event snapshot has an invalid schema")
    attempt = os.environ.get("FACTORY_RALPH_ATTEMPT_ID", "")
    cycle = os.environ.get("FACTORY_RALPH_CYCLE_ID", "")
    campaign_round, campaign_phase, campaign_digest = _campaign_binding(mode)
    if (
        snapshot["schema"] != "ralph-event-snapshot/v1"
        or snapshot["mode"] != mode
        or snapshot["attempt_id"] != attempt
        or snapshot["cycle_id"] != cycle
        or snapshot["campaign_round"] != campaign_round
        or snapshot["campaign_phase"] != campaign_phase
        or snapshot["campaign_state_sha256"] != campaign_digest
    ):
        raise BoundaryError("attempt event snapshot binding changed")
    before_files = snapshot["files"]
    if not isinstance(before_files, dict):
        raise BoundaryError("attempt event snapshot files are invalid")

    directory_fd = _ralph_fd()
    changed: dict[str, os.stat_result] = {}
    try:
        current_events = _read_marker(directory_fd, "current-events", missing_ok=True)
        current_loop = _read_marker(directory_fd, "current-loop-id", missing_ok=True)
        current_event_name = (
            current_events.removeprefix(".ralph/")
            if isinstance(current_events, str) and current_events.startswith(".ralph/")
            else None
        )
        after_files = _event_files(directory_fd)
        for name, current in after_files.items():
            prior = before_files.get(name)
            offset = 0
            if isinstance(prior, dict) and prior.get("dev") == current["dev"] and prior.get("ino") == current["ino"]:
                old_size = prior.get("size")
                if not isinstance(old_size, int) or current["size"] < old_size:
                    raise BoundaryError(f"event stream shrank during attempt: {name}")
                offset = old_size
            if current["size"] == offset:
                continue
            records, info = _read_delta(directory_fd, name, offset)
            for index, record in enumerate(records):
                # Exactly the first receiver record in Ralph's selected stream
                # may be the trusted starting prompt, which names the protocol
                # token. A later record cannot spoof iteration zero to bypass
                # contamination detection.
                trusted_start = (
                    name == current_event_name
                    and index == 0
                    and _trusted_start(record, mode)
                )
                if not trusted_start and _contains_token(record):
                    raise BoundaryError(
                        f"reserved lifecycle token contaminated received event stream {name}"
                    )
            changed[name] = info
    finally:
        os.close(directory_fd)

    if not changed:
        return False
    if not isinstance(current_events, str) or not current_events.startswith(".ralph/"):
        raise BoundaryError("current-events marker is missing or non-canonical")
    event_name = current_events.removeprefix(".ralph/")
    if event_name not in changed or not EVENT_NAME.fullmatch(event_name):
        raise BoundaryError("current-events does not identify this attempt's changed stream")
    if not isinstance(current_loop, str) or not LOOP_ID.fullmatch(current_loop):
        raise BoundaryError("current-loop-id is missing or invalid")
    recorded_mode = read_text(ROOT, "loop-mode", maximum=128)
    if recorded_mode is None or recorded_mode.strip() != mode:
        raise BoundaryError("loop-mode does not match the received event attempt")
    event_info = changed[event_name]
    handshake = {
        "schema": "ralph-launch-handshake/v1",
        "mode": mode,
        "cycle_id": cycle,
        "attempt_id": attempt,
        "campaign_round": campaign_round,
        "campaign_phase": campaign_phase,
        "campaign_state_sha256": campaign_digest,
        "loop_id": current_loop,
        "events": current_events,
        "events_dev": event_info.st_dev,
        "events_ino": event_info.st_ino,
        "events_size": event_info.st_size,
    }
    atomic_write_json(ROOT, f"ralph-launch-handshake-{mode}.json", handshake)
    remove(ROOT, _snapshot_name(mode))
    return True


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("operation", choices=("begin", "finish"))
    parser.add_argument("mode", choices=sorted(MODES))
    args = parser.parse_args()
    try:
        if args.operation == "begin":
            begin(args.mode)
        else:
            raise SystemExit(0 if finish(args.mode) else 3)
    except (OSError, StateIOError, BoundaryError) as exc:
        raise SystemExit(f"ralph-event-boundary: {exc}") from exc


if __name__ == "__main__":
    main()
