#!/usr/bin/env python3
"""Mandatory round-zero production readiness authority.

This module is deliberately data-only.  Runner execution remains in the trusted
campaign coordinator; this authority validates the committed human decision and
builds the canonical, digest-bound readiness result published by that coordinator.
"""
from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path
from typing import Callable, Mapping, Sequence, Tuple

RESULT_SCHEMA = "factory-readiness-result/v1"
APPROVAL_SCHEMA = "controller-production-graphics-approval/v1"
APPROVAL_PATH = ".factory/production-graphics-approval.json"
PROTECTED_STATES = (
    "manager_editor_list", "manager_editor_sequential",
    "manager_editor_validation_error",
)
REQUIRED_CAPABILITIES = (
    "controller-production-routing", "gpu-compositor",
    "installed-licensed-diagram",
)
CORE_ROWS = {
    "controller-production-routing": frozenset(("MGR-02", "MGR-08")),
    "installed-licensed-diagram": frozenset(("OVL-10", "MGR-07", "VRF-06")),
    "gpu-compositor": frozenset(("VRF-06",)),
}
SHA1 = re.compile(r"^[0-9a-f]{40}$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")

class ReadinessError(RuntimeError): pass
class HumanApprovalBlocked(ReadinessError): pass


def canonical_sha(value: object) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def validate_human_approval(raw: bytes, *, accepted_commit: str,
                            blob_at: Callable[[str, str], bytes],
                            object_id: Callable[[str], str]) -> str:
    """Validate a committed three-state human approval; prose is never accepted."""
    try:
        data = json.loads(raw.decode("utf-8"))
    except (UnicodeError, ValueError) as exc:
        raise HumanApprovalBlocked(f"graphics approval is malformed: {exc}") from exc
    expected = {"schema", "status", "candidate_commit", "candidate_tree", "states"}
    if not isinstance(data, dict) or set(data) != expected or data.get("schema") != APPROVAL_SCHEMA:
        raise HumanApprovalBlocked("graphics approval authority has the wrong schema/fields")
    if data.get("status") != "approved":
        raise HumanApprovalBlocked("graphics approval is explicitly pending")
    candidate = data.get("candidate_commit")
    if not isinstance(candidate, str) or not SHA1.fullmatch(candidate):
        raise HumanApprovalBlocked("graphics approval has no exact candidate commit")
    # The approval is committed later than (or at) the reviewed candidate; both
    # objects are immutable.  object_id also rejects a missing candidate.
    tree = object_id(f"{candidate}^{{tree}}")
    if data.get("candidate_tree") != tree:
        raise HumanApprovalBlocked("graphics approval candidate tree is stale")
    states = data.get("states")
    if not isinstance(states, list) or len(states) != len(PROTECTED_STATES):
        raise HumanApprovalBlocked("graphics approval must cover exactly three protected editor states")
    seen = set()
    for item in states:
        fields = {"id", "capture", "capture_sha256", "renderer", "renderer_accelerated", "decision", "reviewer", "human"}
        if not isinstance(item, dict) or set(item) != fields:
            raise HumanApprovalBlocked("graphics approval state fields are malformed")
        state_id = item.get("id")
        if state_id not in PROTECTED_STATES or state_id in seen:
            raise HumanApprovalBlocked("graphics approval state set is duplicated or substituted")
        seen.add(state_id)
        capture = item.get("capture")
        if not isinstance(capture, str) or capture.startswith("/") or ".." in Path(capture).parts:
            raise HumanApprovalBlocked("graphics approval capture path is unsafe")
        try:
            capture_raw = blob_at(candidate, capture)
        except Exception as exc:
            raise HumanApprovalBlocked("graphics approval capture is not a committed candidate blob") from exc
        if hashlib.sha256(capture_raw).hexdigest() != item.get("capture_sha256"):
            raise HumanApprovalBlocked("graphics approval capture hash is stale or tampered")
        renderer = item.get("renderer")
        if (not isinstance(renderer, str) or not renderer.strip()
                or item.get("renderer_accelerated") is not True
                or re.search(r"(?i)llvmpipe|softpipe|software", renderer)):
            raise HumanApprovalBlocked("graphics approval renderer is missing or software-only")
        if item.get("decision") != "approve" or item.get("human") is not True:
            raise HumanApprovalBlocked("graphics approval decision is not explicit human approval")
        if not isinstance(item.get("reviewer"), str) or not item["reviewer"].strip():
            raise HumanApprovalBlocked("graphics approval reviewer identity is missing")
    if seen != set(PROTECTED_STATES):
        raise HumanApprovalBlocked("graphics approval omits a protected editor state")
    return hashlib.sha256(raw).hexdigest()


def validate_core_mapping(conformance_raw: bytes, policy_raw: bytes) -> str:
    """Require explicit policy/sidecar mapping for readiness core rows.

    Planning classifications may remain partial/blocked so round one can plan
    remediation; only malformed, relaxed, or absent core mappings are rejected.
    """
    try:
        side = json.loads(conformance_raw)
        policy = json.loads(policy_raw)
    except ValueError as exc:
        raise ReadinessError(f"core conformance mapping is malformed: {exc}") from exc
    rows = {r.get("id"): r for r in side.get("requirements", []) if isinstance(r, dict)}
    policies = {r.get("id"): r for r in policy.get("requirements", []) if isinstance(r, dict)}
    for capability, ids in CORE_ROWS.items():
        for row_id in ids:
            if row_id not in rows or row_id not in policies:
                raise ReadinessError(f"core readiness row {row_id} is absent")
            side_caps = rows[row_id].get("required_capabilities")
            policy_caps = policies[row_id].get("required_capabilities")
            if not isinstance(side_caps, list) or not isinstance(policy_caps, list) or sorted(side_caps) != sorted(policy_caps) or capability not in side_caps:
                raise ReadinessError(f"core readiness row {row_id} mapping is partial or relaxed")
            if rows[row_id].get("classification") not in {"verified", "partial", "blocked"}:
                raise ReadinessError(f"core readiness row {row_id} has no actionable planning classification")
    return hashlib.sha256(conformance_raw + b"\0" + policy_raw).hexdigest()


def result_document(*, campaign_id: str, nonce: str, status: str,
                    terminal_outcome: str, bindings: Mapping[str, object],
                    results: Mapping[str, object]) -> dict:
    value = {
        "schema": RESULT_SCHEMA, "campaign_id": campaign_id, "nonce": nonce,
        "status": status, "terminal_outcome": terminal_outcome,
        "bindings": dict(bindings), "results": dict(results),
    }
    validate_result(value)
    return value


def validate_result(value: object) -> None:
    if not isinstance(value, dict) or set(value) != {"schema", "campaign_id", "nonce", "status", "terminal_outcome", "bindings", "results"}:
        raise ReadinessError("readiness result fields are malformed")
    if value.get("schema") != RESULT_SCHEMA or value.get("status") not in {"complete", "findings", "human_blocked", "infrastructure_failure"}:
        raise ReadinessError("readiness result schema/status is invalid")
    if value.get("terminal_outcome") not in {"pass", "findings", "blocked", "infrastructure_failure"} or not SHA256.fullmatch(str(value.get("nonce", ""))):
        raise ReadinessError("readiness result outcome/nonce is invalid")
    bindings, results = value.get("bindings"), value.get("results")
    binding_keys = {"accepted_commit", "tree", "environment_blob", "specification_sha256", "plan_sha256", "conformance_sha256", "policy_sha256", "contracts_sha256", "install_manifest_sha256", "command_authority_sha256"}
    result_keys = {"aggregate_sha256", "evidence_result_sha256", "core_result_sha256", "human_result_sha256"}
    if not isinstance(bindings, dict) or set(bindings) != binding_keys or not isinstance(results, dict) or set(results) != result_keys:
        raise ReadinessError("readiness result binding/result sets are malformed")
    for key, val in {**bindings, **results}.items():
        pattern = SHA1 if key in {"accepted_commit", "tree", "environment_blob"} else SHA256
        if not isinstance(val, str) or not pattern.fullmatch(val):
            raise ReadinessError(f"readiness result {key} is malformed")
