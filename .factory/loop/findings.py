#!/usr/bin/env python3
"""Structured tester/auditor findings authority — Task 10 (FIND-01, §16).

This module implements the §16 findings flow of ``docs/FACTORY-LOOP-SPEC.md``
on top of the committed ``factory-findings-receipt/v1`` and
``factory-findings/v1`` schemas and the hardened no-follow atomic I/O of the
``factory-state/v1`` authority (``state.py`` / ``factory_state_io.py``).

Security contract (every claim fails closed):

* **mint** — the trusted orchestrator (:class:`~campaign.Campaign`) mints one
  *receipt* under the ignored ``.factory-state/`` evidence namespace at the
  exact moment a verification or audit phase classifies ``findings`` or
  ``blocked`` through the deterministic classification functions.  The
  receipt binds the campaign id, the round/phase, the **exact commit at
  which the untrusted phase ran** (``phase_base_commit``), the phase tag
  recorded in the state digest ledger, the SHA-256 digest of the **exact
  structured phase-result bytes the orchestrator read** (``result_digest``),
  the deterministic-gate evidence, and the structured findings/blocked
  references.  Receipt publication is write-once (``no_replace=True``), so a
  pre-planted receipt at a canonical name fails the genuine mint closed.
* **consume** — at the start of the next planning phase the orchestrator
  re-reads the previous round's receipts through the hardened no-follow
  bounded reader, re-validates every receipt against the committed schema,
  and binds every receipt to (a) the current campaign id, (b) the exact
  source round, (c) the recorded phase outcome of this run
  (``phase_records``), (d) the exact recorded phase-base commit, (e) the
  reachability of that commit from the current HEAD, (f) the phase tag in
  the state digest ledger, and (g) the digest of the exact structured
  result bytes this run consumed (``record.result_digest``).  A receipt
  that is missing for a phase that recorded findings/blocked, that exists
  for a phase that recorded ``pass``, or that carries stale/foreign/forged
  bindings — a wrong campaign, round, outcome, phase-base commit, result
  digest, or an unrecorded phase tag — fails closed.
* **deterministic planner input** — the consumed receipts are projected into
  one deterministic ``factory-findings/v1`` payload whose bytes (sorted-key
  canonical JSON, no wall-clock time, no prose claims, no copies of the
  plan) are the digest-bound planner input.  The payload is delivered only
  to the next planner prompt; it is never handed to the developer, never
  read by the deterministic selector (a pure function of plan + state), and
  never stored as a runtime task queue, memory, or context
  summary.

Blocked references (``blocked_on``) remain structured findings in the
payload — the §13.4/§14 rule that a non-final ``blocked`` advances to the
next planner exactly like ``findings`` with the blocker explicit in the
plan.  The receipts and the derived payload are evidence artifacts under the
ignored ``.factory-state/`` namespace and never orchestration state.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import stat
from pathlib import Path
from typing import Dict, List, Mapping, Optional, Sequence, Tuple

try:  # package import (the hidden `.factory/loop/` package)
    from . import plan_parser
    from . import state as state_module
except ImportError:  # flat import used by the hidden `.factory/tests/` suite
    import plan_parser  # type: ignore[no-redef]
    import state as state_module  # type: ignore[no-redef]

# ---------------------------------------------------------------------------
# Constants and schema loading
# ---------------------------------------------------------------------------

PAYLOAD_SCHEMA_NAME = "factory-findings/v1"
PAYLOAD_SCHEMA_FILE = "factory-findings-v1.schema.json"
RECEIPT_SCHEMA_NAME = "factory-findings-receipt/v1"
RECEIPT_SCHEMA_FILE = "factory-findings-receipt-v1.schema.json"
PHASE_RESULT_SCHEMA_NAME = "factory-phase-result/v1"
PHASE_RESULT_SCHEMA_FILE = "factory-phase-result-v1.schema.json"

RECEIPT_NAME_TEMPLATE = "factory-findings-receipt-round-{round}-{phase}.json"
RESULT_NAME_TEMPLATE = "factory-phase-result-round-{round}-{phase}.json"
RECEIPT_NAME_RE = re.compile(
    r"^factory-findings-receipt-round-([0-9]+)-(verification|audit)\.json$"
)
RESULT_NAME_RE = re.compile(
    r"^factory-phase-result-round-([0-9]+)-(verification|audit)\.json$"
)
MAX_RECEIPT_BYTES = 256 * 1024
MAX_PAYLOAD_BYTES = 512 * 1024
MAX_LEDGER_BYTES = state_module.LEDGER_MAX
MAX_RESULT_BYTES = 256 * 1024

# BLOCKER 4: round-1 authenticated readiness-findings channel.
READINESS_FINDINGS_SCHEMA_NAME = "factory-readiness-findings/v1"
READINESS_FINDINGS_SCHEMA_FILE = "factory-readiness-findings-v1.schema.json"
READINESS_FINDINGS_CONSUMED_SCHEMA_NAME = "factory-readiness-findings-consumed/v1"
READINESS_FINDINGS_CONSUMED_SCHEMA_FILE = "factory-readiness-findings-consumed-v1.schema.json"
READINESS_FINDINGS_NAME = "factory-readiness-findings.json"
READINESS_FINDINGS_CONSUMED_NAME = "factory-readiness-findings-consumed.json"
MAX_READINESS_FINDINGS_BYTES = 512 * 1024
MAX_PLAN_BYTES = 4 * 1024 * 1024

PHASE_TAG_RE = re.compile(r"^r([0-9]+)\.([a-z]+)\.([0-9]+)\.a([0-9]+)$")
SHA40_RE = re.compile(r"^[0-9a-f]{40}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
SAFE_CAMPAIGN_ID_RE = re.compile(r"^[a-z0-9](?:[a-z0-9._-]{0,62}[a-z0-9])?$")

FINDING_PHASES = ("verification", "audit")

# The all-zero SHA-256 digest is the canonical pass-readiness sentinel: a
# complete/pass readiness carries no findings, so its expected product-findings
# digest is exactly ZERO256 and consumption returns ``None`` (no payload flows
# to the planner).  Any nonzero expected digest requires a real
# readiness-findings artifact; a missing artifact then fails closed.
ZERO256 = "0" * 64


class FindingsError(Exception):
    """Base class for every fail-closed findings-authority failure."""


class FindingsMalformedError(FindingsError):
    """A receipt/payload artifact is unsafe, oversized, or non-conforming."""


class FindingsStaleError(FindingsError):
    """A receipt is bound to a stale round, campaign, or unreachable commit."""


class FindingsSyntheticError(FindingsError):
    """A receipt exists for a phase that never produced findings (forged)."""


class FindingsForeignError(FindingsError):
    """A receipt belongs to a different campaign or unknown phase run."""


class FindingsReceiptError(FindingsError):
    """A recorded findings/blocked phase is missing its receipt (claim only)."""


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _load_schema(name: str) -> Dict[str, object]:
    here = Path(__file__).resolve().parents[1]  # .factory/
    path = here / "schemas" / name
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise FindingsError(f"cannot load the committed schema {path}: {exc}") from exc
    if len(data) > 256 * 1024:
        raise FindingsError(f"the committed schema {path} is oversized")
    try:
        schema = json.loads(data)
    except ValueError as exc:
        raise FindingsError(f"the committed schema {path} is not JSON") from exc
    if not isinstance(schema, dict):
        raise FindingsError(f"the committed schema {path} is not an object")
    return schema


def _json_type(value: object) -> str:
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, int):
        return "integer"
    if isinstance(value, float):
        return "number"
    if isinstance(value, str):
        return "string"
    if isinstance(value, list):
        return "array"
    if isinstance(value, dict):
        return "object"
    if value is None:
        return "null"
    raise FindingsError(f"value of type {type(value).__name__} is not JSON-serializable")


def _check_instance(instance: object, schema: object, path: str, context: str) -> None:
    """Validate ``instance`` against the JSON-Schema subset the committed
    schemas use (type/enum/pattern/minLength/minimum/minItems/items/properties/
    required/additionalProperties).  Any mismatch fails closed."""
    if not isinstance(schema, dict):
        return
    expected = schema.get("type")
    if expected is not None:
        types = expected if isinstance(expected, list) else [expected]
        if _json_type(instance) not in types:
            raise FindingsMalformedError(
                f"{context} violation at {path or '(root)'}: expected "
                f"{expected!r}, got {_json_type(instance)!r}"
            )
    if "enum" in schema and instance not in schema["enum"]:
        raise FindingsMalformedError(
            f"{context} violation at {path or '(root)'}: value {instance!r} "
            f"is not one of {schema['enum']!r}"
        )
    if isinstance(instance, str):
        if "minLength" in schema and len(instance) < schema["minLength"]:
            raise FindingsMalformedError(
                f"{context} violation at {path or '(root)'}: string below the "
                "minimum length"
            )
        if "pattern" in schema and re.fullmatch(schema["pattern"], instance) is None:
            raise FindingsMalformedError(
                f"{context} violation at {path or '(root)'}: pattern mismatch"
            )
    if isinstance(instance, (int, float)) and not isinstance(instance, bool):
        if "minimum" in schema and instance < schema["minimum"]:
            raise FindingsMalformedError(
                f"{context} violation at {path or '(root)'}: value below minimum"
            )
    if isinstance(instance, list):
        if "minItems" in schema and len(instance) < schema["minItems"]:
            raise FindingsMalformedError(
                f"{context} violation at {path or '(root)'}: array below the "
                "minimum item count"
            )
        if "items" in schema:
            for index, item in enumerate(instance):
                _check_instance(item, schema["items"], f"{path}[{index}]", context)
    if isinstance(instance, dict):
        if "properties" in schema:
            for key, subschema in schema["properties"].items():
                if key in instance:
                    _check_instance(
                        instance[key], subschema, f"{path}.{key}", context
                    )
        if "required" in schema:
            for key in schema["required"]:
                if key not in instance:
                    raise FindingsMalformedError(
                        f"{context} violation at {path or '(root)'}: missing "
                        f"required field {key!r}"
                    )
        if schema.get("additionalProperties") is False:
            declared = set((schema.get("properties") or {}).keys())
            for key in instance:
                if key not in declared:
                    raise FindingsMalformedError(
                        f"{context} violation at {path or '(root)'}: extra "
                        f"field {key!r}"
                    )


_PAYLOAD_SCHEMA: Optional[Dict[str, object]] = None
_RECEIPT_SCHEMA: Optional[Dict[str, object]] = None
_PHASE_RESULT_SCHEMA: Optional[Dict[str, object]] = None
_READINESS_FINDINGS_SCHEMA: Optional[Dict[str, object]] = None
_READINESS_FINDINGS_CONSUMED_SCHEMA: Optional[Dict[str, object]] = None


def _payload_schema() -> Dict[str, object]:
    global _PAYLOAD_SCHEMA
    if _PAYLOAD_SCHEMA is None:
        _PAYLOAD_SCHEMA = _load_schema(PAYLOAD_SCHEMA_FILE)
    return _PAYLOAD_SCHEMA


def _receipt_schema() -> Dict[str, object]:
    global _RECEIPT_SCHEMA
    if _RECEIPT_SCHEMA is None:
        _RECEIPT_SCHEMA = _load_schema(RECEIPT_SCHEMA_FILE)
    return _RECEIPT_SCHEMA


def _phase_result_schema() -> Dict[str, object]:
    """The committed ``factory-phase-result/v1`` schema, loaded independently.

    Task 10 review (REQ 3): the findings authority re-validates the exact
    preserved phase-result bytes at consumption with the authoritative
    committed schema — never a self-derived digest or a tolerant re-parse —
    so a tampered receipt whose ``findings``/``blocked_on`` contradict the
    exact result bytes this run consumed fails closed.
    """
    global _PHASE_RESULT_SCHEMA
    if _PHASE_RESULT_SCHEMA is None:
        _PHASE_RESULT_SCHEMA = _load_schema(PHASE_RESULT_SCHEMA_FILE)
    return _PHASE_RESULT_SCHEMA


def _readiness_findings_schema() -> Dict[str, object]:
    """The committed ``factory-readiness-findings/v1`` schema (BLOCKER 4)."""
    global _READINESS_FINDINGS_SCHEMA
    if _READINESS_FINDINGS_SCHEMA is None:
        _READINESS_FINDINGS_SCHEMA = _load_schema(READINESS_FINDINGS_SCHEMA_FILE)
    return _READINESS_FINDINGS_SCHEMA


def _readiness_findings_consumed_schema() -> Dict[str, object]:
    """The committed ``factory-readiness-findings-consumed/v1`` schema (BLOCKER 4)."""
    global _READINESS_FINDINGS_CONSUMED_SCHEMA
    if _READINESS_FINDINGS_CONSUMED_SCHEMA is None:
        _READINESS_FINDINGS_CONSUMED_SCHEMA = _load_schema(
            READINESS_FINDINGS_CONSUMED_SCHEMA_FILE
        )
    return _READINESS_FINDINGS_CONSUMED_SCHEMA


# ---------------------------------------------------------------------------
# Deterministic canonical bytes
# ---------------------------------------------------------------------------


def receipt_bytes(receipt: Mapping[str, object]) -> bytes:
    """The exact canonical receipt bytes (same bytes the atomic writer stores)."""
    return (
        json.dumps(dict(receipt), sort_keys=True, separators=(",", ":")).encode(
            "utf-8"
        )
        + b"\n"
    )


def payload_bytes(payload: Mapping[str, object]) -> bytes:
    """The deterministic planner-input bytes (sorted canonical JSON)."""
    return json.dumps(dict(payload), sort_keys=True, separators=(",", ":")).encode(
        "utf-8"
    )


# ---------------------------------------------------------------------------
# Receipt minting (trusted orchestrator only)
# ---------------------------------------------------------------------------


def receipt_name(round_number: int, phase: str) -> str:
    if (
        isinstance(round_number, bool)
        or not isinstance(round_number, int)
        or round_number < 1
    ):
        raise FindingsError("receipt round must be a positive integer")
    if phase not in FINDING_PHASES:
        raise FindingsError(f"receipt phase must be one of {FINDING_PHASES!r}")
    return RECEIPT_NAME_TEMPLATE.format(round=round_number, phase=phase)


def build_receipt(
    *,
    campaign_id: str,
    round_number: int,
    phase: str,
    phase_tag: str,
    phase_base_commit: str,
    outcome: str,
    result_digest: str,
    findings: Sequence[str] = (),
    blocked_on: Sequence[str] = (),
    gate_ran: bool = False,
    gate_exit: Optional[int] = None,
    capability_ran: bool = False,
    capability_exit: Optional[int] = None,
) -> Dict[str, object]:
    """Build one orchestrator-minted findings receipt (pure, fail-closed).

    Every binding is validated at build time so an invalid receipt can never
    be published: the campaign id, round, phase, phase tag, phase-base
    commit, outcome, and the exact result digest must all be well-formed.
    """
    if not SAFE_CAMPAIGN_ID_RE.fullmatch(campaign_id):
        raise FindingsError(f"unsafe campaign id {campaign_id!r} in a findings receipt")
    if phase not in FINDING_PHASES:
        raise FindingsError(f"unsafe findings phase {phase!r}")
    if outcome not in ("findings", "blocked"):
        raise FindingsError(f"a findings receipt requires outcome findings|blocked, got {outcome!r}")
    if not SHA40_RE.fullmatch(phase_base_commit):
        raise FindingsError("a findings receipt requires a 40-hex phase-base commit")
    if not SHA256_RE.fullmatch(result_digest):
        raise FindingsError("a findings receipt requires a 64-hex result digest")
    if not PHASE_TAG_RE.fullmatch(phase_tag):
        raise FindingsError(f"unsafe phase tag {phase_tag!r} in a findings receipt")
    if not isinstance(gate_ran, bool) or not isinstance(capability_ran, bool):
        raise FindingsError("findings receipt gate flags must be booleans")
    for value in (gate_exit, capability_exit):
        if value is not None and (
            isinstance(value, bool) or not isinstance(value, int) or value < 0
        ):
            raise FindingsError("findings receipt gate exits must be non-negative integers or null")
    findings_list = [str(item) for item in findings]
    blocked_list = [str(item) for item in blocked_on]
    receipt: Dict[str, object] = {
        "schema": RECEIPT_SCHEMA_NAME,
        "campaign_id": campaign_id,
        "round": round_number,
        "phase": phase,
        "phase_tag": phase_tag,
        "phase_base_commit": phase_base_commit,
        "outcome": outcome,
        "result_digest": result_digest,
        "gate_ran": gate_ran,
        "gate_exit": gate_exit,
        "capability_ran": capability_ran,
        "capability_exit": capability_exit,
        "findings": findings_list,
        "blocked_on": blocked_list,
    }
    validate_receipt(receipt)
    return receipt


def validate_receipt(receipt: Mapping[str, object]) -> None:
    """Fail closed unless the receipt conforms to the committed schema."""
    _check_instance(dict(receipt), _receipt_schema(), "", "factory-findings-receipt")
    tag = receipt.get("phase_tag")
    if not isinstance(tag, str) or not PHASE_TAG_RE.fullmatch(tag):
        raise FindingsMalformedError("receipt phase tag is not well-formed")
    match = PHASE_TAG_RE.fullmatch(tag)
    if match and (int(match.group(1)) != receipt.get("round") or match.group(2) != receipt.get("phase")):
        raise FindingsMalformedError(
            "receipt phase tag round/phase prefix does not match the receipt round/phase"
        )


def publish_receipt(root, receipt: Mapping[str, object]) -> str:
    """Atomically publish one findings receipt (write-only, idempotent).

    The receipt is evidence under the ignored ``.factory-state/`` namespace,
    written through the hardened no-follow atomic writer with
    ``no_replace=True``: a pre-planted receipt at the canonical name fails
    the genuine mint closed instead of being silently replaced, so a forged
    receipt cannot masquerade as the orchestrator's mint.

    Task 10 review (REQ 1): publication is **deterministically idempotent**
    across the receipt-mint crash window.  When the canonical receipt
    already exists and its hardened bytes are *exactly equal* to the
    expected canonical bytes of this mint (the previous mint of this same
    run wrote them and the state advance was interrupted), the mint accepts
    the existing receipt and the rerun never wedges.  Any other existing
    content — a tampered, foreign, stale, or replayed receipt — fails
    closed.  Returns the repository-relative receipt name.
    """
    validate_receipt(receipt)
    name = receipt_name(int(receipt["round"]), str(receipt["phase"]))
    expected = receipt_bytes(receipt)
    try:
        state_module.atomic_write_json(
            root, name, dict(receipt), no_replace=True
        )
    except state_module.StateIOError as exc:
        # Crash-window rerun: only a byte-exact re-mint of this same run is
        # accepted idempotently; anything else fails closed.
        try:
            existing = state_module.read_bytes(
                root, name, maximum=MAX_RECEIPT_BYTES, missing_ok=False
            )
        except state_module.StateIOError as read_exc:
            raise FindingsError(
                f"cannot publish the findings receipt {name}: a receipt "
                "already exists and cannot be safely re-read "
                f"({read_exc})"
            ) from read_exc
        if existing == expected:
            return name
        raise FindingsError(
            f"cannot publish the findings receipt {name}: a receipt with "
            "different bytes already exists; a pre-planted, tampered, "
            "foreign, or stale receipt fails closed (only the byte-exact "
            "re-mint of the same run is accepted as crash recovery)"
        ) from exc
    return name


# ---------------------------------------------------------------------------
# Preserved phase-result bytes (the exact content the receipt authenticates)
# ---------------------------------------------------------------------------


def result_name(round_number: int, phase: str) -> str:
    """The canonical preserved phase-result artifact name for a round/phase."""
    if (
        isinstance(round_number, bool)
        or not isinstance(round_number, int)
        or round_number < 1
    ):
        raise FindingsError("phase-result round must be a positive integer")
    if phase not in FINDING_PHASES:
        raise FindingsError(f"phase-result phase must be one of {FINDING_PHASES!r}")
    return RESULT_NAME_TEMPLATE.format(round=round_number, phase=phase)


def preserve_phase_result(
    root, round_number: int, phase: str, raw: bytes
) -> str:
    """Preserve the exact structured phase-result bytes as evidence.

    Task 10 review (REQ 3): the orchestrator preserves the exact bytes of
    the structured result a findings/blocked verification/audit phase
    produced, under the ignored ``.factory-state/`` namespace, so the
    next-round findings authority can authenticate every receipt against
    the exact trusted phase-result bytes (``result_digest`` **and** parsed
    content) instead of only a self-derived digest.  Publication is atomic
    no-replace with the same deterministic idempotency as the receipt
    mint: a byte-exact re-preserve across the crash window is accepted and
    any other pre-existing content fails closed.  Returns the
    repository-relative artifact name.
    """
    name = result_name(round_number, phase)
    if not isinstance(raw, bytes) or len(raw) > MAX_RESULT_BYTES:
        raise FindingsError(
            f"cannot preserve the phase result {name}: the bytes are not a "
            "bounded byte string"
        )
    try:
        state_module.atomic_write(root, name, raw, no_replace=True)
    except state_module.StateIOError as exc:
        try:
            existing = state_module.read_bytes(
                root, name, maximum=MAX_RESULT_BYTES, missing_ok=False
            )
        except state_module.StateIOError as read_exc:
            raise FindingsError(
                f"cannot preserve the phase result {name}: a marker already "
                f"exists and cannot be safely re-read ({read_exc})"
            ) from read_exc
        if existing != raw:
            raise FindingsError(
                f"cannot preserve the phase result {name}: a different marker "
                "already exists; a tampered or foreign phase-result artifact "
                "fails closed"
            ) from exc
    return name


def read_preserved_phase_result(
    root, round_number: int, phase: str
) -> Optional[Tuple[Dict[str, object], str]]:
    """Bounded no-follow read + strict schema validation of the preserved
    phase-result artifact; returns ``(parsed, raw_digest)`` or ``None`` when
    no artifact exists.  Any unsafe, oversized, malformed, or non-conforming
    artifact raises :class:`FindingsMalformedError`.
    """
    name = result_name(round_number, phase)
    try:
        raw = state_module.read_bytes(
            root, name, maximum=MAX_RESULT_BYTES, missing_ok=True
        )
    except state_module.StateIOError as exc:
        raise FindingsMalformedError(
            f"cannot safely read the preserved phase result {name}: {exc}"
        ) from exc
    if raw is None:
        return None
    raw_digest = sha256(raw)
    try:
        data = json.loads(raw.decode("utf-8"))
    except (ValueError, UnicodeDecodeError) as exc:
        raise FindingsMalformedError(
            f"the preserved phase result {name} is not valid JSON: {exc}"
        ) from exc
    if not isinstance(data, dict):
        raise FindingsMalformedError(
            f"the preserved phase result {name} is not an object"
        )
    _check_instance(
        data, _phase_result_schema(), "", "factory-phase-result"
    )
    return data, raw_digest


def validate_receipt_content(
    receipt: Mapping[str, object], result: Mapping[str, object], name: str
) -> None:
    """Authenticate the receipt's findings/blocked_on against the exact
    trusted phase-result bytes' parsed content (REQ 3).  A receipt whose
    structured content contradicts the exact result bytes this run consumed
    — the findings list, the blocked references, or the outcome — fails
    closed; never a self-digest-only check."""
    expected_findings = [str(item) for item in result.get("findings", [])]
    expected_blocked = [str(item) for item in result.get("blocked_on", [])]
    if [str(item) for item in receipt.get("findings", [])] != expected_findings:
        raise FindingsSyntheticError(
            f"findings receipt {name} claims findings that contradict the "
            "exact preserved phase-result bytes; a tampered receipt fails "
            "closed"
        )
    if [str(item) for item in receipt.get("blocked_on", [])] != expected_blocked:
        raise FindingsSyntheticError(
            f"findings receipt {name} claims blocked references that "
            "contradict the exact preserved phase-result bytes; a tampered "
            "receipt fails closed"
        )
    if str(result.get("outcome", "")) not in ("findings", "blocked"):
        raise FindingsSyntheticError(
            f"findings receipt {name} binds a preserved phase result whose "
            "outcome is not findings|blocked; a synthetic artifact fails "
            "closed"
        )


# ---------------------------------------------------------------------------
# Receipt consumption (next-round planner input)
# ---------------------------------------------------------------------------


def read_receipt(
    root, round_number: int, phase: str
) -> Optional[Tuple[Dict[str, object], str]]:
    """Bounded no-follow read + schema validation of one findings receipt.

    Returns ``(data, raw_digest)`` or ``None`` when no receipt exists.  Any
    unsafe, oversized, malformed, or non-conforming file raises
    :class:`FindingsMalformedError`.
    """
    name = receipt_name(round_number, phase)
    try:
        raw = state_module.read_bytes(
            root, name, maximum=MAX_RECEIPT_BYTES, missing_ok=True
        )
    except state_module.StateIOError as exc:
        raise FindingsMalformedError(f"cannot safely read the findings receipt {name}: {exc}") from exc
    if raw is None:
        return None
    raw_digest = sha256(raw)
    try:
        data = json.loads(raw.decode("utf-8"))
    except (ValueError, UnicodeDecodeError) as exc:
        raise FindingsMalformedError(f"the findings receipt {name} is not valid JSON: {exc}") from exc
    if not isinstance(data, dict):
        raise FindingsMalformedError(f"the findings receipt {name} is not an object")
    validate_receipt(data)
    return data, raw_digest


def _ledger_has_tag(root, tag: str) -> bool:
    """True when the authoritative strict state parser recorded ``tag``.

    Task 10 review (REQ 3): the ledger is read only through the hardened
    no-follow bounded reader and the authoritative strict state parser
    (:func:`state.read_phase_digest_ledger`) — never through a tolerant
    per-line re-parse — so a malformed, duplicated, or unsafe ledger line
    fails the findings authority closed instead of being silently ignored.
    """
    try:
        ledger = state_module.read_phase_digest_ledger(root)
    except state_module.StateError as exc:
        raise FindingsError(
            f"cannot read the state digest ledger through the authoritative "
            f"parser: {exc}"
        ) from exc
    return tag in ledger


def _phase_for(records: Sequence, round_number: int, phase: str):
    """The phase-history record for ``(round_number, phase)`` or ``None``."""
    for record in records:
        if getattr(record, "round", None) == round_number and getattr(
            record, "phase", None
        ) == phase:
            return record
    return None


def build_payload(
    *,
    campaign_id: str,
    source_round: int,
    entries: Sequence[Mapping[str, object]],
) -> Dict[str, object]:
    """Assemble the deterministic next-planner findings payload."""
    if not entries:
        raise FindingsError("a findings payload requires at least one entry")
    payload: Dict[str, object] = {
        "schema": PAYLOAD_SCHEMA_NAME,
        "campaign_id": campaign_id,
        "source_round": source_round,
        "entries": [dict(entry) for entry in entries],
    }
    validate_payload(payload)
    return payload


def validate_payload(payload: Mapping[str, object]) -> None:
    """Fail closed unless the payload conforms to the committed schema."""
    _check_instance(dict(payload), _payload_schema(), "", "factory-findings")
    if not isinstance(payload.get("source_round"), int) or isinstance(
        payload.get("source_round"), bool
    ):
        raise FindingsError("payload source_round must be an integer")


def consume_next_round_findings(
    root,
    *,
    campaign_id: str,
    source_round: int,
    head: str,
    is_ancestor,
    phase_records: Sequence,
) -> Optional[bytes]:
    """Derive the deterministic next-planner findings payload bytes.

    Reads the receipts of ``source_round`` for every findings phase, binds
    each to the current campaign, the exact source round, the phase the run
    recorded (``phase_records``), the recorded phase-base commit, the
    reachability of that commit from ``head``, and the state digest ledger's
    phase tag.  Returns ``None`` (no findings flowed to the next planner) or
    the canonical ``factory-findings/v1`` bytes.  Every failure class —
    malformed artifacts, stale/foreign/replayed receipts, synthetic receipts
    for phases that recorded a pass, missing receipts for phases that
    recorded findings/blocked, and receipt-only claims without the required
    structured bindings — fails closed with a dedicated error.
    """
    if (
        isinstance(source_round, bool)
        or not isinstance(source_round, int)
        or source_round < 1
    ):
        raise FindingsError("source_round must be a positive integer")
    if not SHA40_RE.fullmatch(head):
        raise FindingsError("head must be a 40-hex commit")
    entries: List[Dict[str, object]] = []
    for phase in FINDING_PHASES:
        found = read_receipt(root, source_round, phase)
        record = _phase_for(phase_records, source_round, phase)
        if found is None:
            if record is not None and record.outcome in ("findings", "blocked"):
                raise FindingsReceiptError(
                    f"phase round {source_round} {phase} recorded "
                    f"{record.outcome} but its findings receipt is missing; a "
                    "findings claim without receipt evidence fails closed"
                )
            continue
        receipt, raw_digest = found
        if record is None:
            raise FindingsForeignError(
                f"a findings receipt for round {source_round} {phase} exists "
                "but this campaign recorded no such phase; a foreign or "
                "leftover receipt fails closed"
            )
        if record.outcome not in ("findings", "blocked"):
            raise FindingsSyntheticError(
                f"a findings receipt for round {source_round} {phase} exists "
                f"but the phase recorded outcome {record.outcome!r}; a "
                "synthetic receipt for a pass phase fails closed"
            )
        if str(receipt["outcome"]) != str(record.outcome):
            raise FindingsSyntheticError(
                f"findings receipt {receipt_name(source_round, phase)} claims "
                f"outcome {receipt['outcome']!r} but the recorded phase "
                f"classified {record.outcome!r}; a forged receipt fails closed"
            )
        if str(record.head_commit) != str(receipt["phase_base_commit"]):
            raise FindingsSyntheticError(
                f"findings receipt {receipt_name(source_round, phase)} binds "
                f"phase_base_commit {receipt['phase_base_commit']} but the "
                f"recorded phase ran at {record.head_commit}; a forged receipt "
                "fails closed"
            )
        record_result_digest = getattr(record, "result_digest", "") or ""
        if record_result_digest and str(receipt["result_digest"]) != record_result_digest:
            raise FindingsSyntheticError(
                f"findings receipt {receipt_name(source_round, phase)} binds "
                f"result_digest {receipt['result_digest']} but the recorded "
                f"phase consumed the exact result bytes with digest "
                f"{record_result_digest}; a tampered receipt fails closed"
            )
        if str(receipt["campaign_id"]) != campaign_id:
            raise FindingsForeignError(
                f"findings receipt round {source_round}/{phase} belongs to "
                f"campaign {receipt['campaign_id']!r}, not {campaign_id!r}"
            )
        if int(receipt["round"]) != source_round:
            raise FindingsStaleError(
                f"findings receipt round {receipt['round']} does not match "
                f"the source round {source_round}"
            )
        if not is_ancestor(str(receipt["phase_base_commit"]), head):
            raise FindingsStaleError(
                f"findings receipt round {source_round} {phase} binds the "
                f"unreachable phase base {receipt['phase_base_commit']}; a "
                "stale receipt fails closed"
            )
        tag = str(receipt["phase_tag"])
        if not _ledger_has_tag(root, tag):
            raise FindingsSyntheticError(
                f"findings receipt round {source_round} {phase} binds a phase "
                f"tag {tag!r} that the state digest ledger never recorded; a "
                "synthetic receipt fails closed"
            )
        name = receipt_name(source_round, phase)
        # REQ 3: authenticate the receipt against the exact trusted
        # phase-result bytes this run consumed — matching ``result_digest``
        # **and** the parsed content (findings/blocked_on/outcome) — never a
        # self-digest-only check.  The preserved artifact must exist (the
        # orchestrator preserves it before minting), must be byte-exact to
        # the recorded digest, and its parsed content must equal the receipt.
        preserved = read_preserved_phase_result(root, source_round, phase)
        if preserved is None:
            raise FindingsReceiptError(
                f"findings receipt round {source_round} {phase} has no "
                "preserved phase-result artifact; a receipt whose content "
                "cannot be authenticated against the exact trusted result "
                "bytes fails closed"
            )
        result_data, result_raw_digest = preserved
        if str(receipt["result_digest"]) != result_raw_digest:
            raise FindingsSyntheticError(
                f"findings receipt {name} binds result_digest "
                f"{receipt['result_digest']} but the preserved phase-result "
                f"bytes digest to {result_raw_digest}; a tampered receipt or "
                "artifact fails closed"
            )
        if result_raw_digest != record_result_digest:
            raise FindingsSyntheticError(
                f"the preserved phase-result bytes of round {source_round} "
                f"{phase} digest to {result_raw_digest} but the recorded "
                f"phase consumed result digest {record_result_digest}; a "
                "tampered artifact fails closed"
            )
        validate_receipt_content(receipt, result_data, name)
        name = receipt_name(source_round, phase)
        entries.append(
            {
                "phase": str(receipt["phase"]),
                "phase_base_commit": str(receipt["phase_base_commit"]),
                "outcome": str(receipt["outcome"]),
                "phase_tag": tag,
                "result_digest": str(receipt["result_digest"]),
                "receipt_path": name,
                "receipt_digest": raw_digest,
                "findings": [str(item) for item in receipt.get("findings", [])],
                "blocked_on": [str(item) for item in receipt.get("blocked_on", [])],
                "gate_ran": bool(receipt.get("gate_ran", False)),
                "gate_exit": (
                    int(receipt["gate_exit"])
                    if receipt.get("gate_exit") is not None
                    else None
                ),
                "capability_ran": bool(receipt.get("capability_ran", False)),
                "capability_exit": (
                    int(receipt["capability_exit"])
                    if receipt.get("capability_exit") is not None
                    else None
                ),
            }
        )
    if not entries:
        return None
    payload = build_payload(
        campaign_id=campaign_id, source_round=source_round, entries=entries
    )
    raw = payload_bytes(payload)
    if len(raw) > MAX_PAYLOAD_BYTES:
        raise FindingsError("the next-planner findings payload exceeds the bound")
    return raw


# ---------------------------------------------------------------------------
# Round-1 readiness-findings channel (BLOCKER 4)
# ---------------------------------------------------------------------------


def readiness_findings_bytes(artifact: Mapping[str, object]) -> bytes:
    """The exact canonical readiness-findings artifact bytes (same bytes the
    atomic writer stores)."""
    return (
        json.dumps(dict(artifact), sort_keys=True, separators=(",", ":")).encode(
            "utf-8"
        )
        + b"\n"
    )


def readiness_findings_consumed_bytes(marker: Mapping[str, object]) -> bytes:
    """The exact canonical consumption-marker bytes."""
    return (
        json.dumps(dict(marker), sort_keys=True, separators=(",", ":")).encode(
            "utf-8"
        )
        + b"\n"
    )


def validate_readiness_payload(payload: Mapping[str, object]) -> None:
    """Fail closed unless the round-1 readiness-findings payload conforms to
    the deterministic planner-input shape: a ``factory-findings/v1`` payload
    whose source is the round-zero readiness phase (never a verification/audit
    round), carrying at least one structured entry with the readiness
    findings/blocked references."""
    if not isinstance(payload, dict):
        raise FindingsError("the readiness findings payload is not an object")
    if payload.get("schema") != PAYLOAD_SCHEMA_NAME:
        raise FindingsError("the readiness findings payload has the wrong schema")
    if not isinstance(payload.get("campaign_id"), str) or not SAFE_CAMPAIGN_ID_RE.fullmatch(
        payload["campaign_id"]
    ):
        raise FindingsError("the readiness findings payload has an unsafe campaign id")
    if payload.get("source_round") != 0:
        raise FindingsError("the readiness findings payload must source round zero")
    entries = payload.get("entries")
    if not isinstance(entries, list) or not entries:
        raise FindingsError("the readiness findings payload carries no entries")
    for entry in entries:
        if not isinstance(entry, dict):
            raise FindingsError("the readiness findings payload has an invalid entry")
        if entry.get("phase") != "readiness":
            raise FindingsError("the readiness findings payload entry must phase readiness")
        if entry.get("outcome") not in ("findings", "blocked"):
            raise FindingsError(
                "the readiness findings payload entry outcome must be findings|blocked"
            )
        for key in ("findings", "blocked_on"):
            values = entry.get(key)
            if not isinstance(values, list) or not all(
                isinstance(v, str) and v for v in values
            ):
                raise FindingsError(
                    f"the readiness findings payload entry {key} is malformed"
                )


def validate_readiness_findings(artifact: Mapping[str, object]) -> None:
    """Fail closed unless the readiness-findings artifact conforms to the
    committed schema and its payload is a valid round-1 planner input."""
    _check_instance(
        dict(artifact), _readiness_findings_schema(), "", "factory-readiness-findings"
    )
    payload = artifact.get("payload")
    if not isinstance(payload, dict):
        raise FindingsMalformedError(
            "the readiness-findings artifact payload is not an object"
        )
    validate_readiness_payload(payload)
    if str(artifact.get("payload_sha256", "")) != sha256(payload_bytes(payload)):
        raise FindingsMalformedError(
            "the readiness-findings artifact payload digest does not match its payload bytes"
        )


def validate_readiness_findings_consumed(marker: Mapping[str, object]) -> None:
    """Fail closed unless the consumption marker conforms to the committed schema."""
    _check_instance(
        dict(marker),
        _readiness_findings_consumed_schema(),
        "",
        "factory-readiness-findings-consumed",
    )


def mint_readiness_findings(
    root,
    *,
    campaign_id: str,
    readiness_nonce: str,
    accepted_commit: str,
    findings_payload: bytes,
) -> str:
    """Mint the write-once readiness-findings artifact (trusted orchestrator).

    The artifact binds the campaign id, the readiness nonce, the exact
    accepted commit, and the SHA-256 digest of the deterministic
    product-findings payload.  Publication is atomic no-replace with the
    same crash-window idempotency as the receipt mint: a byte-exact
    re-mint of the same run is accepted and any other pre-existing content
    fails closed.  Returns the repository-relative artifact name.
    """
    if not SAFE_CAMPAIGN_ID_RE.fullmatch(campaign_id):
        raise FindingsError(
            f"unsafe campaign id {campaign_id!r} in a readiness-findings artifact"
        )
    if not SHA256_RE.fullmatch(readiness_nonce):
        raise FindingsError(
            "a readiness-findings artifact requires a 64-hex readiness nonce"
        )
    if not SHA40_RE.fullmatch(accepted_commit):
        raise FindingsError(
            "a readiness-findings artifact requires a 40-hex accepted commit"
        )
    if not isinstance(findings_payload, bytes) or len(findings_payload) > MAX_READINESS_FINDINGS_BYTES:
        raise FindingsError("the readiness findings payload is not a bounded byte string")
    try:
        payload = json.loads(findings_payload.decode("utf-8"))
    except (ValueError, UnicodeDecodeError) as exc:
        raise FindingsError("the readiness findings payload is not valid JSON") from exc
    if not isinstance(payload, dict):
        raise FindingsError("the readiness findings payload is not an object")
    validate_readiness_payload(payload)
    artifact = {
        "schema": READINESS_FINDINGS_SCHEMA_NAME,
        "campaign_id": campaign_id,
        "readiness_nonce": readiness_nonce,
        "accepted_commit": accepted_commit,
        "payload_sha256": sha256(findings_payload),
        "payload": payload,
    }
    validate_readiness_findings(artifact)
    expected = readiness_findings_bytes(artifact)
    try:
        state_module.atomic_write_json(
            root, READINESS_FINDINGS_NAME, artifact, no_replace=True
        )
    except state_module.StateIOError as exc:
        try:
            existing = state_module.read_bytes(
                root,
                READINESS_FINDINGS_NAME,
                maximum=MAX_READINESS_FINDINGS_BYTES,
                missing_ok=False,
            )
        except state_module.StateIOError as read_exc:
            raise FindingsError(
                f"cannot publish the readiness-findings artifact "
                f"{READINESS_FINDINGS_NAME}: an artifact already exists and "
                f"cannot be safely re-read ({read_exc})"
            ) from read_exc
        if existing == expected:
            return READINESS_FINDINGS_NAME
        raise FindingsError(
            f"cannot publish the readiness-findings artifact "
            f"{READINESS_FINDINGS_NAME}: an artifact with different bytes "
            "already exists; a pre-planted, tampered, foreign, or stale "
            "artifact fails closed (only the byte-exact re-mint of the same "
            "run is accepted as crash recovery)"
        ) from exc
    return READINESS_FINDINGS_NAME


def consume_readiness_findings(
    root,
    *,
    campaign_id: str,
    readiness_nonce: str,
    accepted_commit: str,
    expected_payload_sha256: str,
) -> Optional[bytes]:
    """Consume the round-1 readiness-findings artifact (durable one-use).

    Returns the deterministic product-findings payload bytes, or ``None``
    when no readiness-findings artifact exists (a complete/pass readiness
    carries no findings).  Every binding — campaign id, readiness nonce,
    exact accepted commit, and the payload digest — is re-validated against
    the committed schema and the caller's expected authority.  Consumption
    is durably recorded by a write-once marker: a crash-window re-consume
    of the same run (byte-exact marker) is accepted idempotently, while a
    genuine replay (a different campaign, nonce, or digest) fails closed.
    """
    if not SAFE_CAMPAIGN_ID_RE.fullmatch(campaign_id):
        raise FindingsError(
            f"unsafe campaign id {campaign_id!r} in readiness-findings consumption"
        )
    if not SHA256_RE.fullmatch(readiness_nonce):
        raise FindingsError(
            "readiness-findings consumption requires a 64-hex readiness nonce"
        )
    if not SHA40_RE.fullmatch(accepted_commit):
        raise FindingsError(
            "readiness-findings consumption requires a 40-hex accepted commit"
        )
    if not SHA256_RE.fullmatch(expected_payload_sha256):
        raise FindingsError(
            "readiness-findings consumption requires a 64-hex expected payload digest"
        )
    try:
        raw = state_module.read_bytes(
            root,
            READINESS_FINDINGS_NAME,
            maximum=MAX_READINESS_FINDINGS_BYTES,
            missing_ok=True,
        )
    except state_module.StateIOError as exc:
        raise FindingsMalformedError(
            f"cannot safely read the readiness-findings artifact: {exc}"
        ) from exc
    if raw is None:
        # Fail closed: a missing artifact is only a legitimate pass-readiness
        # when the expected product-findings digest is exactly ZERO256.  Any
        # nonzero expected digest means the readiness result produced
        # findings/blocked, so a missing artifact is a claim without evidence.
        if expected_payload_sha256 != ZERO256:
            raise FindingsReceiptError(
                "the readiness-findings artifact is missing but the readiness "
                "result expected a nonzero product-findings digest; a findings "
                "claim without artifact evidence fails closed"
            )
        return None
    try:
        artifact = json.loads(raw.decode("utf-8"))
    except (ValueError, UnicodeDecodeError) as exc:
        raise FindingsMalformedError(
            "the readiness-findings artifact is not valid JSON"
        ) from exc
    if not isinstance(artifact, dict):
        raise FindingsMalformedError(
            "the readiness-findings artifact is not an object"
        )
    validate_readiness_findings(artifact)
    if str(artifact["campaign_id"]) != campaign_id:
        raise FindingsForeignError(
            f"the readiness-findings artifact belongs to campaign "
            f"{artifact['campaign_id']!r}, not {campaign_id!r}"
        )
    if str(artifact["readiness_nonce"]) != readiness_nonce:
        raise FindingsStaleError(
            "the readiness-findings artifact binds a different readiness "
            "nonce; a stale or replayed artifact fails closed"
        )
    if str(artifact["accepted_commit"]) != accepted_commit:
        raise FindingsStaleError(
            "the readiness-findings artifact binds a different accepted "
            "commit; a stale or replayed artifact fails closed"
        )
    payload = artifact["payload"]
    payload_raw = payload_bytes(payload)
    if sha256(payload_raw) != str(artifact["payload_sha256"]):
        raise FindingsSyntheticError(
            "the readiness-findings artifact's payload digest does not match "
            "its stored payload bytes; a tampered artifact fails closed"
        )
    if sha256(payload_raw) != expected_payload_sha256:
        raise FindingsSyntheticError(
            "the readiness-findings artifact's payload digest does not match "
            "the readiness result's product-findings digest; a tampered "
            "artifact fails closed"
        )
    marker = {
        "schema": READINESS_FINDINGS_CONSUMED_SCHEMA_NAME,
        "campaign_id": campaign_id,
        "readiness_nonce": readiness_nonce,
        "payload_sha256": expected_payload_sha256,
    }
    validate_readiness_findings_consumed(marker)
    marker_raw = readiness_findings_consumed_bytes(marker)
    try:
        state_module.atomic_write_json(
            root, READINESS_FINDINGS_CONSUMED_NAME, marker, no_replace=True
        )
    except state_module.StateIOError as exc:
        try:
            existing = state_module.read_bytes(
                root,
                READINESS_FINDINGS_CONSUMED_NAME,
                maximum=MAX_READINESS_FINDINGS_BYTES,
                missing_ok=False,
            )
        except state_module.StateIOError as read_exc:
            raise FindingsError(
                "cannot record the readiness-findings consumption: a marker "
                "already exists and cannot be safely re-read"
            ) from read_exc
        if existing != marker_raw:
            raise FindingsStaleError(
                "the readiness-findings artifact was already consumed by a "
                "different campaign/nonce/digest; a replay fails closed"
            ) from exc
    return payload_raw


# ---------------------------------------------------------------------------
# B5: trusted coordinator projection of the validated findings aggregate
# ---------------------------------------------------------------------------
#
# The round-1 planner must receive *actionable* product findings, never the
# generic sentence the readiness phase used to emit.  The only legitimate
# source is the strong-checker-validated signed findings aggregate
# (``factory-runner-findings-aggregate/v1``).  This pure projector re-reads
# the exact aggregate bytes, re-validates the strict committed schema and
# every campaign / readiness-nonce / commit / tree / environment /
# declaration / contract / archive binding, and maps only fixed semantic
# per-runner / per-capability result fields into bounded deterministic
# ``factory-findings/v1`` readiness entries.  Arbitrary runner prose
# (stdout/stderr/log/artifact text), result booleans, skips, simulation,
# and any unvalidated aggregate are never projected.

RUNNER_AGGREGATE_SCHEMA_NAME = "factory-runner-findings-aggregate/v1"
RUNNER_AGGREGATE_SCHEMA_FILE = "factory-runner-findings-aggregate-v1.schema.json"
MAX_AGGREGATE_BYTES = 256 * 1024
MAX_RUNNER_PROBES = 64

# The evidence namespace the coordinator (not the untrusted phases) reads:
# ``.factory-state/runner-evidence/<campaign>/<readiness-nonce>/...``.
# The aggregate artifact and every per-runner signed manifest are re-read
# through the hardened bounded no-follow reader below — the strong checker
# digest alone is never sufficient; the exact bytes this run consumed must
# re-validate.
EVIDENCE_ROOT = ".factory-state/runner-evidence"
EVIDENCE_AGGREGATE_NAME = "findings-aggregate.json"
EVIDENCE_MANIFEST_NAME = "manifest.json"
MAX_RUNNER_MANIFEST_BYTES = 256 * 1024

# The single fixed semantic code a findings record can legitimately project:
# a findings record proves every declared probe executed (no skip/simulation,
# no timeout, clean cleanup) with a non-pass verdict.  No other code is
# derivable without trusting arbitrary runner prose, so the coordinator mints
# exactly this one code; per-runner/capability/exit detail is carried in the
# structured verdict body, never in free prose.
RUNNER_FINDINGS_CODE = "runner-probe-exit-nonzero"
RUNNER_FINDINGS_CODE_RE = re.compile(
    r"^runner-probe-exit-nonzero:[a-z0-9][a-z0-9._-]*:"
    r"[a-z0-9][a-z0-9._-]*:[0-9]+$"
)

# Fixture/simulation option tokens that make a candidate probe argv
# non-actionable under the committed capability-contract policy.
_DENY_PROBE_ARGV_TOKENS = (
    "--fixture", "--fixture=", "--fixture-dir", "--fixture-facts",
)

_RUNNER_AGGREGATE_SCHEMA: Optional[Dict[str, object]] = None


def _runner_aggregate_schema() -> Dict[str, object]:
    """The committed ``factory-runner-findings-aggregate/v1`` schema, loaded
    independently so the projector revalidates the exact aggregate bytes
    against the authoritative strict schema (never a tolerant re-parse)."""
    global _RUNNER_AGGREGATE_SCHEMA
    if _RUNNER_AGGREGATE_SCHEMA is None:
        _RUNNER_AGGREGATE_SCHEMA = _load_schema(RUNNER_AGGREGATE_SCHEMA_FILE)
    return _RUNNER_AGGREGATE_SCHEMA


def _validate_hex(value: object, length: int, label: str) -> str:
    """Return ``value`` as a well-formed lowercase hex string of ``length``
    or raise :class:`FindingsError` (fail closed)."""
    if not isinstance(value, str) or len(value) != length:
        raise FindingsError(f"{label} must be a {length}-hex string")
    try:
        int(value, 16)
    except ValueError:
        raise FindingsError(f"{label} is not hex") from None
    if value.lower() != value:
        raise FindingsError(f"{label} must be lowercase hex")
    return value


def validate_runner_findings_code(code: object) -> None:
    """Fail closed unless ``code`` is the single coordinator-minted fixed
    semantic runner findings code (bounded, deterministic, prose-free)."""
    if not isinstance(code, str) or RUNNER_FINDINGS_CODE_RE.fullmatch(code) is None:
        raise FindingsError(f"untrusted runner findings code {code!r}")


def _evidence_relpath(relpath: str) -> Tuple[str, ...]:
    """Validate one runner-evidence artifact path and return its components.

    The canonical namespace is ``.factory-state/runner-evidence/`` with a
    well-formed campaign segment and a 64-hex readiness-nonce segment; the
    aggregate artifact is exactly five components and a signed manifest is
    exactly eight.  Any absolute/backslash/control-character path, any
    traversal or empty segment, and any unknown component count fails
    closed before a descriptor walk begins.
    """
    if not isinstance(relpath, str) or not relpath:
        raise FindingsMalformedError(
            "the runner evidence path must be a non-empty string"
        )
    if relpath.startswith("/") or "\\" in relpath:
        raise FindingsMalformedError(
            "the runner evidence path is absolute or contains a backslash"
        )
    if any(ord(ch) < 0x20 for ch in relpath):
        raise FindingsMalformedError(
            "the runner evidence path contains a control character"
        )
    parts = relpath.split("/")
    if (
        len(parts) not in (5, 8)
        or parts[:2] != [".factory-state", "runner-evidence"]
        or any(part in ("", ".", "..") for part in parts)
    ):
        raise FindingsMalformedError(
            f"the runner evidence path {relpath!r} escapes the canonical "
            "evidence namespace"
        )
    if not SAFE_CAMPAIGN_ID_RE.fullmatch(parts[2]):
        raise FindingsMalformedError(
            "the runner evidence path has an unsafe campaign segment"
        )
    if not SHA256_RE.fullmatch(parts[3]):
        raise FindingsMalformedError(
            "the runner evidence path has an unsafe readiness-nonce segment"
        )
    return tuple(parts)


def _open_dir_no_follow(parent_fd: int, name: str, *, what: str) -> int:
    """Open one intermediate evidence component beneath an open ``dir_fd``.

    The component must be a current-user-owned private directory and is
    re-opened with ``O_DIRECTORY|O_NOFOLLOW``; the opened descriptor's
    identity must equal the name's identity, so a component swapped while
    being opened fails closed.
    """
    try:
        info = os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
    except OSError as exc:
        raise FindingsMalformedError(
            f"{what} component {name!r} is unavailable: {exc}"
        ) from exc
    if (
        not stat.S_ISDIR(info.st_mode)
        or info.st_uid != os.getuid()
        or info.st_mode & 0o077
    ):
        raise FindingsMalformedError(
            f"{what} component {name!r} is not a private owned directory"
        )
    flags = (
        os.O_RDONLY
        | os.O_DIRECTORY
        | getattr(os, "O_NOFOLLOW", 0)
        | getattr(os, "O_CLOEXEC", 0)
    )
    try:
        descriptor = os.open(name, flags, dir_fd=parent_fd)
    except OSError as exc:
        raise FindingsMalformedError(
            f"cannot open {what} component {name!r}: {exc}"
        ) from exc
    opened = os.fstat(descriptor)
    if (opened.st_dev, opened.st_ino) != (info.st_dev, info.st_ino):
        os.close(descriptor)
        raise FindingsMalformedError(
            f"{what} component {name!r} changed while being opened"
        )
    return descriptor


def read_runner_evidence_bytes(root, relpath: str, *, maximum: int) -> bytes:
    """Bounded hardened no-follow read of one runner-evidence artifact.

    The repository-relative path is validated up front and then walked
    beneath ``.factory-state/`` through retained ``O_DIRECTORY|O_NOFOLLOW``
    descriptors: every intermediate component must be a current-user-owned
    private directory and the final file is opened with ``O_NOFOLLOW``,
    must be a regular single-link current-user-owned non-writable file no
    larger than ``maximum``, and the descriptor identity must equal the
    pathname at both ends of the bounded read.  A symlink, substitution,
    ownership, mode, link-count, or size anomaly always fails closed.
    """
    if not isinstance(maximum, int) or isinstance(maximum, bool) or maximum < 1:
        raise FindingsError("the runner evidence read bound is invalid")
    parts = _evidence_relpath(relpath)
    flags_file = (
        os.O_RDONLY
        | getattr(os, "O_NOFOLLOW", 0)
        | getattr(os, "O_CLOEXEC", 0)
    )
    flags_root = (
        os.O_RDONLY
        | os.O_DIRECTORY
        | getattr(os, "O_NOFOLLOW", 0)
        | getattr(os, "O_CLOEXEC", 0)
    )
    try:
        current = os.open(Path(root).absolute(), flags_root)
    except OSError as exc:
        raise FindingsMalformedError(
            f"cannot open the runner evidence read root: {exc}"
        ) from exc
    opened_fds = [current]
    try:
        # Walk every component beneath the root: ``.factory-state`` itself,
        # then ``runner-evidence``, the campaign/nonce (and per-runner
        # commit/nonce) directory components — never re-resolving a pathname
        # from the process root.
        for part in (".factory-state", *parts[1:-1]):
            current = _open_dir_no_follow(
                current, part, what="runner evidence"
            )
            opened_fds.append(current)
        try:
            descriptor = os.open(parts[-1], flags_file, dir_fd=current)
        except FileNotFoundError:
            raise FindingsMalformedError(
                f"the runner evidence artifact {relpath} is missing"
            ) from None
        except OSError as exc:
            raise FindingsMalformedError(
                f"cannot open the runner evidence artifact {relpath}: {exc}"
            ) from exc
        try:
            before = os.fstat(descriptor)
            named = os.stat(parts[-1], dir_fd=current, follow_symlinks=False)
            if (
                not stat.S_ISREG(before.st_mode)
                or before.st_uid != os.getuid()
                or before.st_nlink != 1
                or before.st_mode & 0o022
                or before.st_size > maximum
                or (before.st_dev, before.st_ino)
                != (named.st_dev, named.st_ino)
            ):
                raise FindingsMalformedError(
                    f"unsafe runner evidence artifact {relpath}"
                )
            chunks: List[bytes] = []
            remaining = maximum + 1
            while remaining:
                chunk = os.read(descriptor, min(65536, remaining))
                if not chunk:
                    break
                chunks.append(chunk)
                remaining -= len(chunk)
            raw = b"".join(chunks)
            after = os.fstat(descriptor)
            named_after = os.stat(
                parts[-1], dir_fd=current, follow_symlinks=False
            )
            if (
                len(raw) > maximum
                or (
                    before.st_dev, before.st_ino, before.st_size,
                    before.st_mtime_ns,
                )
                != (
                    after.st_dev, after.st_ino, after.st_size,
                    after.st_mtime_ns,
                )
                or (after.st_dev, after.st_ino)
                != (named_after.st_dev, named_after.st_ino)
            ):
                raise FindingsMalformedError(
                    f"the runner evidence artifact {relpath} changed while "
                    "reading"
                )
            return raw
        finally:
            os.close(descriptor)
    finally:
        for descriptor in opened_fds:
            os.close(descriptor)


def read_runner_findings_aggregate(
    root, *, campaign_id: str, readiness_nonce: str
) -> Tuple[bytes, Dict[str, object]]:
    """Re-read the exact validated signed findings aggregate bytes.

    The artifact is read through the hardened no-follow bounded reader at
    exactly ``.factory-state/runner-evidence/<campaign>/<nonce>/``
    ``findings-aggregate.json``; the returned bytes are the exact bytes the
    strong checker validated (their digest is bound separately by the
    caller).  Any absent, escaped, unsafe, oversized, or malformed artifact
    fails closed.
    """
    if not SAFE_CAMPAIGN_ID_RE.fullmatch(campaign_id):
        raise FindingsError(
            f"unsafe campaign id {campaign_id!r} in the aggregate read"
        )
    if not SHA256_RE.fullmatch(readiness_nonce):
        raise FindingsError(
            "the aggregate read requires a 64-hex readiness nonce"
        )
    relpath = (
        f"{EVIDENCE_ROOT}/{campaign_id}/{readiness_nonce}/"
        f"{EVIDENCE_AGGREGATE_NAME}"
    )
    raw = read_runner_evidence_bytes(
        root, relpath, maximum=MAX_AGGREGATE_BYTES
    )
    try:
        data = json.loads(raw.decode("utf-8"))
    except (ValueError, UnicodeDecodeError) as exc:
        raise FindingsMalformedError(
            "the findings aggregate is not valid JSON"
        ) from exc
    if not isinstance(data, dict):
        raise FindingsMalformedError(
            "the findings aggregate is not an object"
        )
    return raw, data


def derive_archive_bindings(
    root, aggregate: Mapping[str, object]
) -> Dict[str, Mapping[str, object]]:
    """Derive the exact per-runner archive bindings from signed manifests.

    Every aggregate record's manifest reference is re-read through the
    hardened no-follow bounded reader and its exact bytes must digest to the
    record's ``manifest_sha256`` (never a self-derived digest).  The derived
    binding carries the archive digest, the probe-authority digest, and the
    per-runner nonce from the signed receipt, plus the no-skip/clean-cleanup/
    no-timeout proof flags.  A skipped/simulated, timed-out, unproven,
    tampered, foreign, path-escaping, or malformed manifest fails closed.
    """
    records = aggregate.get("runners")
    if not isinstance(records, list):
        raise FindingsMalformedError(
            "the findings aggregate runners set is invalid"
        )
    bindings: Dict[str, Mapping[str, object]] = {}
    for record in records:
        if not isinstance(record, dict):
            raise FindingsMalformedError(
                "the findings aggregate carries an invalid runner record"
            )
        runner = record.get("name")
        manifest_rel = record.get("manifest")
        manifest_sha = record.get("manifest_sha256")
        if not all(
            isinstance(value, str)
            for value in (runner, manifest_rel, manifest_sha)
        ):
            raise FindingsMalformedError(
                "the findings aggregate record has an invalid manifest "
                "reference"
            )
        if not SHA256_RE.fullmatch(manifest_sha):
            raise FindingsMalformedError(
                f"runner {runner} manifest_sha256 is not well-formed"
            )
        raw = read_runner_evidence_bytes(
            root, manifest_rel, maximum=MAX_RUNNER_MANIFEST_BYTES
        )
        if sha256(raw) != manifest_sha:
            raise FindingsSyntheticError(
                f"runner {runner!r} manifest bytes do not digest to the "
                "aggregate record's manifest_sha256; a tampered receipt "
                "fails closed"
            )
        try:
            manifest = json.loads(raw.decode("utf-8"))
        except (ValueError, UnicodeDecodeError) as exc:
            raise FindingsMalformedError(
                f"runner {runner!r} manifest is not valid JSON"
            ) from exc
        if not isinstance(manifest, dict):
            raise FindingsMalformedError(
                f"runner {runner!r} manifest is not an object"
            )
        for key in ("archive_sha256", "authority_sha256", "nonce"):
            if not SHA256_RE.fullmatch(str(manifest.get(key, ""))):
                raise FindingsMalformedError(
                    f"runner {runner!r} manifest has an invalid {key}"
                )
        # A findings receipt proves no skip/simulation markers, clean cleanup,
        # and no timeout; a pass-only receipt class carries no markers field
        # (absent == clean), so the derived binding proves the same flags.
        skip = manifest.get("skip_marker_detected")
        if skip is not None and skip is not False:
            raise FindingsSyntheticError(
                f"runner {runner!r} manifest shows skip/simulation markers; "
                "a skipped or simulated run is never projected"
            )
        if manifest.get("cleanup") is not True:
            raise FindingsSyntheticError(
                f"runner {runner!r} manifest does not prove a clean executed "
                "cleanup; an unproven run is never projected"
            )
        if manifest.get("timed_out") is not False:
            raise FindingsSyntheticError(
                f"runner {runner!r} manifest shows a timed-out probe; a "
                "timeout is never projected as a product finding"
            )
        bindings[runner] = {
            "archive_sha256": str(manifest["archive_sha256"]),
            "authority_sha256": str(manifest["authority_sha256"]),
            "nonce": str(manifest["nonce"]),
            "skip_marker_detected": bool(skip),
            "cleanup": bool(manifest["cleanup"]),
            "timed_out": bool(manifest["timed_out"]),
        }
    if not bindings:
        raise FindingsMalformedError(
            "the findings aggregate derived no runner archive bindings"
        )
    return bindings


def _contract_for(contracts: Mapping[str, object], capability: str):
    """The committed capability-contract record for ``capability`` or ``None``."""
    capabilities = contracts.get("capabilities")
    if not isinstance(capabilities, list):
        return None
    for item in capabilities:
        if isinstance(item, dict) and item.get("name") == capability:
            return item
    return None


def _argv_has_fixture_token(argv: Sequence[object]) -> bool:
    """True when any committed probe argv token is a fixture/simulation option."""
    for arg in argv:
        if isinstance(arg, str):
            lowered = arg.lower()
            for token in _DENY_PROBE_ARGV_TOKENS:
                if token in lowered:
                    return True
    return False


def _manifest_nonce(
    manifest: str, runner: str, campaign_id: str,
    readiness_nonce: str, commit: str,
) -> str:
    """Extract and authenticate the per-runner nonce embedded in the manifest
    path: ``.factory-state/runner-evidence/<campaign>/<nonce>/<runner>/``
    ``<commit>/<nonce>/manifest.json``.  Any path that escapes the campaign /
    readiness / runner / commit namespace fails closed."""
    parts = manifest.split("/")
    if (
        len(parts) != 8
        or parts[:2] != [".factory-state", "runner-evidence"]
        or parts[7] != "manifest.json"
    ):
        raise FindingsMalformedError(
            f"runner {runner} manifest path is not a valid evidence artifact"
        )
    if (
        parts[2] != campaign_id
        or parts[3] != readiness_nonce
        or parts[4] != runner
        or parts[5] != commit
    ):
        raise FindingsSyntheticError(
            f"runner {runner} manifest path escapes its campaign/readiness/"
            "runner/commit namespace; a foreign or replayed artifact fails "
            "closed"
        )
    _validate_hex(parts[6], 64, f"runner {runner} manifest nonce")
    return parts[6]


def project_runner_findings(
    aggregate_bytes: bytes,
    *,
    aggregate_sha256: str,
    campaign_id: str,
    readiness_nonce: str,
    commit: str,
    tree: str,
    environment_blob: str,
    contracts_sha256: str,
    declarations: Mapping[str, Sequence[str]],
    contracts: Mapping[str, object],
    archive_bindings: Mapping[str, Mapping[str, object]],
) -> Tuple[Tuple[str, ...], Tuple[Dict[str, object], ...]]:
    """Project the validated signed findings aggregate into deterministic
    bounded ``factory-findings/v1`` readiness entries (pure, fail-closed).

    Accepts the exact checker-validated aggregate bytes plus the expected
    aggregate digest and the exact expected campaign / readiness-nonce /
    commit / tree / environment / declaration / contract bindings.  The
    strict committed schema is re-validated and every field is bounded:
    only ``result=findings`` records and only probes with ``exit_code != 0``
    are projected, into the single coordinator-minted semantic code
    ``runner-probe-exit-nonzero`` plus a fixed structured verdict per
    runner/capability.  Arbitrary runner prose, result booleans, skips,
    simulation markers, undeclared capabilities, mismatched runner/
    contract/declaration bindings, and any digest mismatch fail closed.

    Returns ``(code_strings, structured_verdicts)`` where ``code_strings``
    are the bounded fixed-semantic findings the round-1 planner reflects and
    ``structured_verdicts`` carry the exact binding echo and per-probe
    detail.  Both are deterministic: sorted-key JSON of the same inputs
    yields exactly the same projection.
    """
    if not isinstance(aggregate_bytes, bytes) or not aggregate_bytes:
        raise FindingsMalformedError(
            "the findings aggregate is not a non-empty byte string"
        )
    if len(aggregate_bytes) > MAX_READINESS_FINDINGS_BYTES:
        raise FindingsMalformedError(
            "the findings aggregate exceeds the bounded aggregate size"
        )
    _validate_hex(aggregate_sha256, 64, "aggregate digest")
    if sha256(aggregate_bytes) != aggregate_sha256:
        raise FindingsSyntheticError(
            "the findings aggregate digest does not match its bytes; a "
            "tampered aggregate fails closed"
        )
    if not SAFE_CAMPAIGN_ID_RE.fullmatch(campaign_id):
        raise FindingsError("unsafe campaign id in the runner findings projection")
    _validate_hex(readiness_nonce, 64, "readiness nonce")
    _validate_hex(commit, 40, "commit")
    _validate_hex(tree, 40, "tree")
    _validate_hex(environment_blob, 40, "environment blob")
    _validate_hex(contracts_sha256, 64, "contracts digest")
    try:
        aggregate = json.loads(aggregate_bytes.decode("utf-8"))
    except (ValueError, UnicodeDecodeError) as exc:
        raise FindingsMalformedError(
            "the findings aggregate is not valid JSON"
        ) from exc
    if not isinstance(aggregate, dict):
        raise FindingsMalformedError(
            "the findings aggregate is not an object"
        )
    _check_instance(
        aggregate, _runner_aggregate_schema(), "", RUNNER_AGGREGATE_SCHEMA_NAME
    )
    if str(aggregate["campaign_id"]) != campaign_id:
        raise FindingsForeignError(
            f"the findings aggregate belongs to campaign "
            f"{aggregate['campaign_id']!r}, not {campaign_id!r}"
        )
    if str(aggregate["readiness_nonce"]) != readiness_nonce:
        raise FindingsStaleError(
            "the findings aggregate binds a different readiness nonce; a "
            "stale or replayed aggregate fails closed"
        )
    if str(aggregate["commit"]) != commit:
        raise FindingsStaleError(
            "the findings aggregate binds a different commit; a stale or "
            "replayed aggregate fails closed"
        )
    if str(aggregate["tree"]) != tree:
        raise FindingsStaleError(
            "the findings aggregate binds a different tree; a stale or "
            "replayed aggregate fails closed"
        )
    if str(aggregate["environment_blob"]) != environment_blob:
        raise FindingsStaleError(
            "the findings aggregate binds a different environment blob; a "
            "stale or replayed aggregate fails closed"
        )
    records = aggregate["runners"]
    findings_records = [r for r in records if str(r["result"]) == "findings"]
    if not findings_records:
        raise FindingsSyntheticError(
            "the findings aggregate carries no result=findings record; a "
            "synthetic or pass-only aggregate cannot be projected"
        )
    if len(records) > len(declarations):
        raise FindingsSyntheticError(
            "the findings aggregate declares more runners than the committed "
            "environment declaration"
        )
    code_strings: List[str] = []
    verdicts: List[Dict[str, object]] = []
    for record in records:
        runner = str(record["name"])
        declared_caps = declarations.get(runner)
        if declared_caps is None:
            raise FindingsSyntheticError(
                f"runner {runner!r} is not declared in the committed "
                "environment declaration"
            )
        if list(record["capabilities"]) != sorted(declared_caps):
            raise FindingsSyntheticError(
                f"runner {runner!r} capability set does not match its declared "
                "environment declaration"
            )
        binding = archive_bindings.get(runner)
        if not isinstance(binding, dict):
            raise FindingsReceiptError(
                f"runner {runner!r} has no validated manifest archive binding; "
                "a findings claim without its signed receipt fails closed"
            )
        manifest_nonce = _manifest_nonce(
            str(record["manifest"]), runner, campaign_id,
            readiness_nonce, commit,
        )
        archive_sha256 = _validate_hex(
            binding.get("archive_sha256"), 64,
            f"runner {runner} archive digest",
        )
        authority_sha256 = _validate_hex(
            binding.get("authority_sha256"), 64,
            f"runner {runner} authority digest",
        )
        nonce = _validate_hex(binding.get("nonce"), 64, f"runner {runner} nonce")
        # The per-record archive binding must be bound to this exact manifest
        # path nonce (the checker re-derives it from the manifest receipt).
        if nonce != manifest_nonce:
            raise FindingsSyntheticError(
                f"runner {runner!r} archive binding nonce does not match its "
                "manifest path; a tampered or foreign receipt fails closed"
            )
        # No skip / simulation / unproven-cleanup marker survives projection.
        if binding.get("skip_marker_detected") is not False:
            raise FindingsSyntheticError(
                f"runner {runner!r} shows skip/simulation markers; a skipped "
                "or simulated run is never projected"
            )
        if binding.get("cleanup") is not True:
            raise FindingsSyntheticError(
                f"runner {runner!r} does not prove a clean executed cleanup; "
                "an unproven run is never projected"
            )
        if binding.get("timed_out") is not False:
            raise FindingsSyntheticError(
                f"runner {runner!r} shows a timed-out probe; a timeout is "
                "never projected as a product finding"
            )
        if str(record["result"]) != "findings":
            # Pass records contribute no verdict; they must only satisfy the
            # declaration binding checked above.
            continue
        probes = record["probes"]
        if not isinstance(probes, list) or not probes or len(probes) > MAX_RUNNER_PROBES:
            raise FindingsMalformedError(
                f"runner {runner!r} findings probes are not bounded"
            )
        nonzero = [p for p in probes if int(p["exit_code"]) != 0]
        if not nonzero:
            raise FindingsSyntheticError(
                f"runner {runner!r} findings record carries no non-pass probe; "
                "a synthetic pass-as-findings record fails closed"
            )
        for probe in nonzero:
            capability = str(probe["capability"])
            exit_code = int(probe["exit_code"])
            contract = _contract_for(contracts, capability)
            if contract is None:
                raise FindingsSyntheticError(
                    f"capability {capability!r} has no committed capability "
                    "contract; an undeclared probe is never projected"
                )
            if str(contract.get("status")) != "declared":
                raise FindingsSyntheticError(
                    f"capability {capability!r} is not status=declared; a "
                    "candidate capability is never projected"
                )
            if str(contract.get("runner_class")) != runner:
                raise FindingsSyntheticError(
                    f"capability {capability!r} contract runner class does not "
                    f"match runner {runner!r}"
                )
            if contract.get("must_execute") is not True:
                raise FindingsSyntheticError(
                    f"capability {capability!r} contract is not must_execute"
                )
            candidate_argv = contract.get("candidate_probe_argv")
            if isinstance(candidate_argv, list) and _argv_has_fixture_token(candidate_argv):
                raise FindingsSyntheticError(
                    f"capability {capability!r} contract carries a "
                    "fixture/simulation probe argv; a simulated probe is never "
                    "projected"
                )
            code = f"{RUNNER_FINDINGS_CODE}:{runner}:{capability}:{exit_code}"
            code_strings.append(code)
            verdicts.append(
                {
                    "code": RUNNER_FINDINGS_CODE,
                    "runner": runner,
                    "capability": capability,
                    "exit_code": exit_code,
                    "manifest": str(record["manifest"]),
                    "manifest_sha256": str(record["manifest_sha256"]),
                    "signer_principal": str(record["signer"]["principal"]),
                    "artifact_manifest_sha256": str(
                        record["artifact_manifest_sha256"]
                    ),
                    "archive_sha256": archive_sha256,
                    "authority_sha256": authority_sha256,
                    "nonce": nonce,
                    "campaign_id": campaign_id,
                    "readiness_nonce": readiness_nonce,
                    "commit": commit,
                    "tree": tree,
                    "environment_blob": environment_blob,
                    "contracts_sha256": contracts_sha256,
                    "aggregate_sha256": aggregate_sha256,
                }
            )
    if not code_strings or not verdicts:
        raise FindingsSyntheticError(
            "the findings aggregate projected no actionable finding; a "
            "synthetic or empty projection fails closed"
        )
    return tuple(code_strings), tuple(verdicts)


def build_runner_readiness_findings_payload(
    *,
    campaign_id: str,
    code_strings: Sequence[str],
    blocked_on: Sequence[str] = (),
) -> Dict[str, object]:
    """Assemble the round-zero ``factory-findings/v1`` readiness entry whose
    ``findings`` are the bounded fixed-semantic runner code strings minted by
    :func:`project_runner_findings`.  The payload is schema-conforming to the
    committed readiness-findings payload shape (phase readiness, findings /
    blocked_on) so the round-1 planner can reflect them and the plan-digest
    backstop stays string-compatible."""
    if len(code_strings) < 1:
        raise FindingsError(
            "the runner readiness entry requires at least one code string"
        )
    for code in code_strings:
        validate_runner_findings_code(code)
    entry = {
        "phase": "readiness",
        "outcome": "findings",
        "findings": [str(code) for code in code_strings],
        "blocked_on": [str(item) for item in blocked_on],
    }
    payload: Dict[str, object] = {
        "schema": PAYLOAD_SCHEMA_NAME,
        "campaign_id": campaign_id,
        "source_round": 0,
        "entries": [entry],
    }
    validate_readiness_payload(payload)
    return payload


# Structured task fields that legitimately carry readiness findings, and the
# blocked-dependency field that carries external blockers (defect 5).  A
# finding/blocker must be represented in one of these fields of a parsed
# task; occurrences in front matter, canonical-section prose, comments, or
# code fences never satisfy reflection.
FINDING_FIELDS = ("Scope", "Acceptance criteria", "Verification", "Evidence")
BLOCKED_FIELD = "Blocked on"

# Fenced code blocks, inline code spans, and HTML comments are not
# legitimate finding carriers: a planner that hides a finding inside code or
# a comment must fail closed instead of passing the raw-byte backstop.
_CODE_FENCE_RE = re.compile(r"```.*?```", re.DOTALL)
_INLINE_CODE_RE = re.compile(r"`[^`\n]*`")
_HTML_COMMENT_RE = re.compile(r"<!--.*?-->", re.DOTALL)


def _strip_non_prose(text: str) -> str:
    """Remove fenced code, inline code spans, and HTML comments from a
    structured field's logical content before reflection matching."""
    text = _CODE_FENCE_RE.sub(" ", text)
    text = _INLINE_CODE_RE.sub(" ", text)
    text = _HTML_COMMENT_RE.sub(" ", text)
    return text


def _finding_reflected(value: str, field_text: str) -> bool:
    """True when ``value`` appears as a whole token sequence inside the
    structured field's prose content — never as a bare substring of a larger
    token, and never inside code or a comment."""
    pattern = r"(?<!\w)" + re.escape(value) + r"(?!\w)"
    return re.search(pattern, _strip_non_prose(field_text)) is not None


def _task_reflects(value: str, tasks: Sequence, *, blocked: bool) -> bool:
    """True when a parsed task's structured fields represent ``value``.

    Findings must be represented in a task's Scope, Acceptance criteria,
    Verification, or Evidence field; blocked references must be represented
    in a task's Blocked on field.  Only the parsed ``factory-plan/v1`` task
    model is consulted — never the raw plan bytes — so front matter,
    canonical-section prose, comments, and code fences can never satisfy
    reflection.
    """
    labels = (BLOCKED_FIELD,) if blocked else FINDING_FIELDS
    for task in tasks:
        for label in labels:
            content = task.fields.get(label)
            if content and _finding_reflected(value, content):
                return True
    return False


def validate_plan_reflects_findings(plan_bytes: bytes, findings_payload: bytes) -> None:
    """Canonical-plan authority propagation (plan-digest backstop).

    The round-1 planner receives the readiness findings only as a
    digest-bound prompt input; the findings reach the developer only through
    the revised canonical plan.  This backstop fails closed unless every
    structured finding and blocked reference of the readiness payload is
    represented in a legitimate task's structured field of the parsed
    ``factory-plan/v1`` document: findings in a task's Scope, Acceptance
    criteria, Verification, or Evidence field, and blocked references in a
    task's Blocked on field.  Occurrences in front matter, canonical-section
    prose, comments, code fences, or arbitrary substrings never satisfy
    reflection, so a planner that ignored its findings (or a channel that
    injected them as unauthoritative model memory) can never pass.
    """
    if not isinstance(plan_bytes, bytes) or len(plan_bytes) > MAX_PLAN_BYTES:
        raise FindingsError("the plan bytes are not a bounded byte string")
    try:
        payload = json.loads(findings_payload.decode("utf-8"))
    except (ValueError, UnicodeDecodeError) as exc:
        raise FindingsError("the readiness findings payload is not valid JSON") from exc
    if not isinstance(payload, dict):
        raise FindingsError("the readiness findings payload is not an object")
    entries = payload.get("entries")
    if not isinstance(entries, list) or not entries:
        raise FindingsError("the readiness findings payload carries no entries")
    # The canonical plan is the only reflection authority: parse it with the
    # committed ``factory-plan/v1`` parser (never a second parser or a
    # raw-byte scan).  A plan that does not parse fails closed.
    try:
        plan = plan_parser.Plan.from_bytes(plan_bytes)
    except plan_parser.PlanError as exc:
        raise FindingsError(
            f"the revised plan is not a valid factory-plan/v1 document: {exc}"
        ) from exc
    tasks = plan.tasks
    for entry in entries:
        if not isinstance(entry, dict):
            raise FindingsError("the readiness findings payload has an invalid entry")
        for key in ("findings", "blocked_on"):
            blocked = key == "blocked_on"
            for value in entry.get(key, []):
                if not isinstance(value, str) or not value:
                    raise FindingsError(
                        "the readiness findings payload has an invalid finding"
                    )
                if not _task_reflects(value, tasks, blocked=blocked):
                    raise FindingsError(
                        f"the revised plan does not reflect the readiness "
                        f"finding {value!r}; findings must be propagated "
                        "through a task's structured scope/acceptance/"
                        "verification/evidence or blocked-dependency field "
                        "of the canonical plan authority"
                    )


if __name__ == "__main__":
    # Never invoked by a model role; the findings authority is consumed only
    # by the trusted campaign orchestrator.
    raise SystemExit("factory-findings: not a standalone entrypoint")
