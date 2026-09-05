#!/usr/bin/env python3
"""Fail-closed production round-zero readiness and human-review authority."""
from __future__ import annotations

import base64
import hashlib
import json
import os
import re
import subprocess
import tempfile
import stat
from pathlib import Path
from typing import Callable, Mapping

RESULT_SCHEMA = "factory-readiness-result/v2"
APPROVAL_SCHEMA = "controller-production-graphics-approval/v3"
TRUST_SCHEMA = "controller-human-review-trust-anchor/v2"
APPROVAL_PATH = ".factory/production-graphics-approval.json"
# This repository document is pending enrollment data only.  It is never a
# verification root. Production receives the trust anchor out-of-tree.
TRUST_PATH = ".factory/human-review-trust.json"
SIGNATURE_NAMESPACE = "controller-box-production-graphics-approval"
PROTECTED_STATES = (
    "manager_editor_list", "manager_editor_sequential",
    "manager_editor_validation_error",
)
REQUIRED_CHECKLIST = (
    "distinct-captures", "representative-licensed-models", "licensed-artwork",
    "recognizability", "sharpness", "contrast", "marker-alignment",
    "layout-and-legibility", "validation-error-state",
    "hardware-accelerated-renderer", "real-seat-display-metadata",
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
ZERO256 = "0" * 64
# Provisioned absolute verifier; never resolve a human trust primitive through
# the operator's ambient PATH.
SSH_KEYGEN = "/nix/store/c53dnjjglhynq6h3v7a96vyrpsb7zpcw-openssh-10.4p1/bin/ssh-keygen"

class ReadinessError(RuntimeError): pass
class HumanApprovalBlocked(ReadinessError): pass


def read_external_authority(path: Path, expected_sha256: str, *, expected_uid: int | None = None,
                            maximum: int = 256 * 1024) -> tuple[bytes, os.stat_result]:
    """Read an immutable offline authority without pathname/inode races.

    Every ancestor must be a root-owned real directory and must not be
    writable by group/other. The conventional root-owned sticky /tmp is
    allowed because sticky ownership prevents replacement of the exact
    operator-owned leaf. Test-only trees may use operator-owned ancestors.
    The leaf is operator-owned, single-link,
    non-writable, regular, and is compared with its opened descriptor before
    and after reading.  The coordinator-supplied digest is mandatory.
    """
    if not path.is_absolute() or not SHA256.fullmatch(expected_sha256):
        raise HumanApprovalBlocked("external authority path/digest is not exact")
    if expected_uid is None:
        expected_uid = os.getuid()
    current = Path(path.anchor)
    for part in path.parts[1:-1]:
        current /= part
        try:
            info = os.lstat(current)
        except OSError as exc:
            raise HumanApprovalBlocked(f"external authority ancestor unavailable: {current}") from exc
        sticky_root_tmp = (current == Path("/tmp") and info.st_uid == 0
                           and bool(info.st_mode & stat.S_ISVTX))
        trusted_owner = info.st_uid == 0 or (
            os.environ.get("FACTORY_TEST_AUTHORITY_ANCESTORS") == "1"
            and info.st_uid == expected_uid
        )
        if (not stat.S_ISDIR(info.st_mode) or stat.S_ISLNK(info.st_mode)
                or not trusted_owner or ((info.st_mode & 0o022) and not sticky_root_tmp)):
            raise HumanApprovalBlocked(f"external authority ancestor is unsafe: {current}")
    try:
        named = os.lstat(path)
        fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0))
    except OSError as exc:
        raise HumanApprovalBlocked("external authority is unavailable or symlinked") from exc
    try:
        opened = os.fstat(fd)
        if ((named.st_dev, named.st_ino) != (opened.st_dev, opened.st_ino)
                or not stat.S_ISREG(opened.st_mode) or opened.st_uid != expected_uid
                or opened.st_nlink != 1 or stat.S_IMODE(opened.st_mode) not in (0o400, 0o440, 0o444)):
            raise HumanApprovalBlocked("external authority ownership/mode/inode contract failed")
        chunks, total = [], 0
        while True:
            chunk = os.read(fd, 65536)
            if not chunk:
                break
            total += len(chunk)
            if total > maximum:
                raise HumanApprovalBlocked("external authority exceeds size limit")
            chunks.append(chunk)
        final = os.fstat(fd)
        if (opened.st_dev, opened.st_ino, opened.st_size, opened.st_mtime_ns) != (
                final.st_dev, final.st_ino, final.st_size, final.st_mtime_ns):
            raise HumanApprovalBlocked("external authority changed while reading")
        raw = b"".join(chunks)
        if hashlib.sha256(raw).hexdigest() != expected_sha256:
            raise HumanApprovalBlocked("external authority digest mismatch")
        return raw, opened
    finally:
        os.close(fd)


def _validate_png(raw: bytes) -> None:
    if len(raw) < 45 or not raw.startswith(b"\x89PNG\r\n\x1a\n"):
        raise HumanApprovalBlocked("graphics approval capture is not a PNG")
    offset, ihdr, idat, iend = 8, False, False, False
    while offset + 12 <= len(raw):
        size = int.from_bytes(raw[offset:offset + 4], "big")
        kind = raw[offset + 4:offset + 8]
        end = offset + 12 + size
        if end > len(raw):
            raise HumanApprovalBlocked("graphics approval PNG is truncated")
        payload = raw[offset + 8:offset + 8 + size]
        if kind == b"IHDR":
            if ihdr or size != 13 or int.from_bytes(payload[:4], "big") == 0 or int.from_bytes(payload[4:8], "big") == 0:
                raise HumanApprovalBlocked("graphics approval PNG header is invalid")
            ihdr = True
        elif kind == b"IDAT": idat = idat or bool(payload)
        elif kind == b"IEND":
            iend = size == 0 and end == len(raw)
            break
        offset = end
    if not (ihdr and idat and iend):
        raise HumanApprovalBlocked("graphics approval capture is not a complete PNG")


def canonical_bytes(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode() + b"\n"


def canonical_sha(value: object) -> str:
    return hashlib.sha256(canonical_bytes(value)).hexdigest()


def _safe_path(value: object, label: str) -> str:
    if not isinstance(value, str) or not value or value.startswith("/") or any(p in ("", ".", "..") for p in Path(value).parts):
        raise HumanApprovalBlocked(f"graphics approval {label} path is unsafe")
    return value


def validate_trust_anchor(raw: bytes) -> dict:
    try:
        trust = json.loads(raw.decode("utf-8"))
    except (UnicodeError, ValueError) as exc:
        raise HumanApprovalBlocked(f"external human-review trust is malformed: {exc}") from exc
    if (not isinstance(trust, dict) or set(trust) != {"schema", "status", "namespace", "scope", "keys"}
            or trust.get("schema") != TRUST_SCHEMA or trust.get("status") != "active"
            or trust.get("namespace") != SIGNATURE_NAMESPACE
            or trust.get("scope") != "production-human-review"):
        raise HumanApprovalBlocked("external human-review trust authority is pending or malformed")
    keys = trust.get("keys")
    if not isinstance(keys, list) or not keys:
        raise HumanApprovalBlocked("external human-review trust has no reviewer key")
    seen_ids, seen_reviewers, seen_keys = set(), set(), set()
    for item in keys:
        if not isinstance(item, dict) or set(item) != {"key_id", "reviewer", "public_key"}:
            raise HumanApprovalBlocked("external human-review key entry is malformed")
        key_id, reviewer, public = item["key_id"], item["reviewer"], item["public_key"]
        if not all(isinstance(v, str) and v and "\n" not in v for v in (key_id, reviewer, public)):
            raise HumanApprovalBlocked("external human-review key identity is invalid")
        parts = public.split()
        try:
            wire = base64.b64decode(parts[1], validate=True) if len(parts) == 2 and parts[0] == "ssh-ed25519" else b""
        except ValueError:
            wire = b""
        expected = len(b"ssh-ed25519").to_bytes(4, "big") + b"ssh-ed25519" + (32).to_bytes(4, "big")
        if not wire.startswith(expected) or len(wire) != len(expected) + 32:
            raise HumanApprovalBlocked("external human-review public key is not canonical ed25519")
        if key_id in seen_ids or reviewer in seen_reviewers or public in seen_keys:
            raise HumanApprovalBlocked("external human-review keys are ambiguous or duplicated")
        seen_ids.add(key_id); seen_reviewers.add(reviewer); seen_keys.add(public)
    return trust


def validate_human_approval(
    raw: bytes, *, accepted_commit: str, trust_raw: bytes,
    blob_at: Callable[[str, str], bytes], object_id: Callable[[str], str],
    is_ancestor: Callable[[str, str], bool], diff_paths: Callable[[str, str], list[str]],
) -> str:
    """Verify a detached allowlisted human signature over canonical approval bytes.

    The supported non-self-referential workflow is an approval-only descendant:
    captures and trust exist in ``candidate_commit``; ``accepted_commit`` may
    differ only by this approval JSON and its detached signature.
    """
    try:
        data = json.loads(raw.decode("utf-8")); trust = validate_trust_anchor(trust_raw)
    except (UnicodeError, ValueError) as exc:
        raise HumanApprovalBlocked(f"graphics approval/trust is malformed: {exc}") from exc
    fields = {"schema", "status", "candidate_commit", "candidate_tree", "accepted_relationship", "states", "provenance", "checklist", "decision", "reviewer", "signature_path"}
    if not isinstance(data, dict) or set(data) != fields or data.get("schema") != APPROVAL_SCHEMA:
        raise HumanApprovalBlocked("graphics approval authority has the wrong schema/fields")
    if data.get("status") != "approved" or data.get("decision") != "approve":
        raise HumanApprovalBlocked("graphics approval is pending or not approved")
    candidate = data.get("candidate_commit")
    if not isinstance(candidate, str) or not SHA1.fullmatch(candidate) or data.get("accepted_relationship") != "approval-only-descendant":
        raise HumanApprovalBlocked("graphics approval commit relationship is not exact")
    if object_id(f"{candidate}^{{tree}}") != data.get("candidate_tree"):
        raise HumanApprovalBlocked("graphics approval candidate tree is stale")
    signature_path = _safe_path(data.get("signature_path"), "signature")
    if candidate == accepted_commit or not is_ancestor(candidate, accepted_commit):
        raise HumanApprovalBlocked("accepted commit must strictly descend from the reviewed candidate")
    # Exactly one approval commit eliminates ancestor ambiguity and prevents a
    # post-review product delta hidden in a longer descendant chain.
    if object_id(f"{accepted_commit}^") != candidate:
        raise HumanApprovalBlocked("approval-only descendant must be the direct child of the candidate")
    if set(diff_paths(candidate, accepted_commit)) != {APPROVAL_PATH, signature_path}:
        raise HumanApprovalBlocked("accepted commit is not an approval-only descendant")
    states = data.get("states")
    if not isinstance(states, list) or len(states) != len(PROTECTED_STATES):
        raise HumanApprovalBlocked("graphics approval must cover exactly three protected states")
    seen_ids, seen_blobs, seen_digests = set(), set(), set()
    for item in states:
        item_fields = {"id", "model", "capture", "capture_blob", "capture_sha256", "assessment"}
        if not isinstance(item, dict) or set(item) != item_fields:
            raise HumanApprovalBlocked("graphics approval state fields are malformed")
        state_id = item.get("id")
        if state_id not in PROTECTED_STATES or state_id in seen_ids:
            raise HumanApprovalBlocked("graphics approval state set is duplicated or substituted")
        model = item.get("model")
        if model not in {"xb360", "xbox-series", "ds5"}:
            raise HumanApprovalBlocked("graphics approval model coverage is not representative")
        assessment = item.get("assessment")
        if not isinstance(assessment, dict) or set(assessment) != {"recognizable", "sharp", "contrast", "marker_aligned"} or not all(v is True for v in assessment.values()):
            raise HumanApprovalBlocked("graphics approval capture assessment is incomplete")
        capture = _safe_path(item.get("capture"), "capture")
        capture_blob = object_id(f"{candidate}:{capture}")
        capture_raw = blob_at(candidate, capture)
        _validate_png(capture_raw)
        digest = hashlib.sha256(capture_raw).hexdigest()
        if item.get("capture_blob") != capture_blob or item.get("capture_sha256") != digest:
            raise HumanApprovalBlocked("graphics approval capture binding is stale or tampered")
        if capture_blob in seen_blobs or digest in seen_digests:
            raise HumanApprovalBlocked("protected states reuse the same capture")
        seen_ids.add(state_id); seen_blobs.add(capture_blob); seen_digests.add(digest)
    if seen_ids != set(PROTECTED_STATES):
        raise HumanApprovalBlocked("graphics approval omits a protected state")
    if {item["model"] for item in states} != {"xb360", "xbox-series", "ds5"}:
        raise HumanApprovalBlocked("graphics approval reuses or omits representative models")
    provenance = data.get("provenance")
    if not isinstance(provenance, dict) or set(provenance) != {"renderer", "renderer_accelerated", "capture_tool", "session", "display", "seat"}:
        raise HumanApprovalBlocked("graphics approval provenance is malformed")
    renderer = provenance.get("renderer")
    if not isinstance(renderer, str) or not renderer.strip() or provenance.get("renderer_accelerated") is not True or re.search(r"(?i)llvmpipe|softpipe|software", renderer):
        raise HumanApprovalBlocked("graphics approval does not prove accelerated rendering")
    if not all(isinstance(provenance.get(k), str) and provenance[k].strip() for k in ("capture_tool", "session", "display", "seat")):
        raise HumanApprovalBlocked("graphics approval provenance is incomplete")
    if data.get("checklist") != list(REQUIRED_CHECKLIST):
        raise HumanApprovalBlocked("graphics approval checklist is incomplete or reordered")
    reviewer = data.get("reviewer")
    if not isinstance(reviewer, dict) or set(reviewer) != {"identity", "key_id"} or not all(isinstance(reviewer.get(k), str) and reviewer[k].strip() for k in reviewer):
        raise HumanApprovalBlocked("graphics approval reviewer authority is malformed")
    keys = trust.get("keys")
    matches = [k for k in keys if isinstance(k, dict) and set(k) == {"key_id", "reviewer", "public_key"} and k.get("key_id") == reviewer["key_id"] and k.get("reviewer") == reviewer["identity"]] if isinstance(keys, list) else []
    if len(matches) != 1:
        raise HumanApprovalBlocked("reviewer key is not uniquely allowlisted")
    public_key = matches[0].get("public_key")
    if not isinstance(public_key, str) or not public_key.startswith("ssh-ed25519 "):
        raise HumanApprovalBlocked("human-review public key is invalid")
    try:
        signature = blob_at(accepted_commit, signature_path)
    except Exception as exc:
        raise HumanApprovalBlocked("detached approval signature is not committed") from exc
    if not signature.startswith(b"-----BEGIN SSH SIGNATURE-----"):
        raise HumanApprovalBlocked("detached approval signature is malformed")
    # Canonical bytes, not author-controlled formatting, are signed.
    canonical = canonical_bytes(data)
    with tempfile.TemporaryDirectory() as td:
        allowed = Path(td) / "allowed_signers"
        allowed.write_text(f"{reviewer['identity']} {public_key}\n", encoding="utf-8")
        if SSH_KEYGEN is None or not Path(SSH_KEYGEN).is_absolute():
            raise HumanApprovalBlocked("human signature verifier is unavailable")
        sig = Path(td) / "approval.sig"; sig.write_bytes(signature); os.chmod(sig, 0o600)
        try:
            verified = subprocess.run(
                [SSH_KEYGEN, "-Y", "verify", "-f", str(allowed), "-I", reviewer["identity"], "-n", SIGNATURE_NAMESPACE, "-s", str(sig)],
                input=canonical, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise HumanApprovalBlocked("human signature verifier is unavailable") from exc
        if verified.returncode != 0:
            raise HumanApprovalBlocked("detached human approval signature is invalid")
    return hashlib.sha256(canonical + b"\0" + signature + b"\0" + trust_raw).hexdigest()


def validate_human_conformance(conformance_raw: bytes, *, candidate_commit: str) -> str:
    """Require the signed human decision to be the final VRF-07 authority."""
    try:
        side = json.loads(conformance_raw)
    except ValueError as exc:
        raise HumanApprovalBlocked("human conformance authority is malformed") from exc
    matches = [row for row in side.get("requirements", [])
               if isinstance(row, dict) and row.get("id") == "VRF-07"]
    if len(matches) != 1:
        raise HumanApprovalBlocked("human conformance row is absent or ambiguous")
    row = matches[0]
    if (row.get("classification") != "verified" or row.get("evidence_tier") != "human"
            or row.get("required_tier") != "human"
            or row.get("evidence_commit") != candidate_commit
            or APPROVAL_PATH not in row.get("artifacts", [])):
        raise HumanApprovalBlocked("accepted human review is not bound to final conformance")
    return hashlib.sha256(conformance_raw).hexdigest()


def validate_core_mapping(conformance_raw: bytes, policy_raw: bytes) -> str:
    try:
        side = json.loads(conformance_raw); policy = json.loads(policy_raw)
    except ValueError as exc:
        raise ReadinessError(f"core conformance mapping is malformed: {exc}") from exc
    rows = {r.get("id"): r for r in side.get("requirements", []) if isinstance(r, dict)}
    policies = {r.get("id"): r for r in policy.get("requirements", []) if isinstance(r, dict)}
    for capability, ids in CORE_ROWS.items():
        for row_id in ids:
            if row_id not in rows or row_id not in policies:
                raise ReadinessError(f"core readiness row {row_id} is absent")
            side_caps = rows[row_id].get("required_capabilities"); policy_caps = policies[row_id].get("required_capabilities")
            if not isinstance(side_caps, list) or not isinstance(policy_caps, list) or sorted(side_caps) != sorted(policy_caps) or capability not in side_caps:
                raise ReadinessError(f"core readiness row {row_id} mapping is partial or relaxed")
            if rows[row_id].get("classification") not in {"verified", "partial", "blocked"}:
                raise ReadinessError(f"core readiness row {row_id} has no actionable planning classification")
    return hashlib.sha256(conformance_raw + b"\0" + policy_raw).hexdigest()


def result_document(*, campaign_id: str, nonce: str, status: str, terminal_outcome: str, bindings: Mapping[str, object], results: Mapping[str, object]) -> dict:
    value = {"schema": RESULT_SCHEMA, "campaign_id": campaign_id, "nonce": nonce, "status": status, "terminal_outcome": terminal_outcome, "bindings": dict(bindings), "results": dict(results)}
    validate_result(value)
    return value


def validate_result(value: object, *, expected_campaign_id: str | None = None, expected_nonce: str | None = None, expected_bindings: Mapping[str, object] | None = None) -> None:
    fields = {"schema", "campaign_id", "nonce", "status", "terminal_outcome", "bindings", "results"}
    if not isinstance(value, dict) or set(value) != fields or value.get("schema") != RESULT_SCHEMA:
        raise ReadinessError("readiness result fields/schema are malformed")
    if not isinstance(value.get("campaign_id"), str) or not value["campaign_id"] or not SHA256.fullmatch(str(value.get("nonce", ""))):
        raise ReadinessError("readiness campaign/nonce is invalid")
    consistency = {"complete": "pass", "findings": "findings", "human_blocked": "blocked", "infrastructure_failure": "infrastructure_failure"}
    if value.get("status") not in consistency or value.get("terminal_outcome") != consistency[value["status"]]:
        raise ReadinessError("readiness status/outcome is inconsistent")
    binding_keys = {"accepted_commit", "tree", "environment_blob", "specification_sha256", "plan_sha256", "conformance_sha256", "policy_sha256", "contracts_sha256", "install_manifest_sha256", "command_authority_sha256", "human_authority_sha256", "trust_authority_sha256"}
    result_keys = {"aggregate_sha256", "capability_result_sha256", "core_result_sha256", "conformance_result_sha256", "human_result_sha256"}
    bindings, results = value.get("bindings"), value.get("results")
    if not isinstance(bindings, dict) or set(bindings) != binding_keys or not isinstance(results, dict) or set(results) != result_keys:
        raise ReadinessError("readiness result binding/result sets are malformed")
    for key, val in {**bindings, **results}.items():
        pattern = SHA1 if key in {"accepted_commit", "tree", "environment_blob"} else SHA256
        if not isinstance(val, str) or not pattern.fullmatch(val):
            raise ReadinessError(f"readiness result {key} is malformed")
    if expected_campaign_id is not None and value["campaign_id"] != expected_campaign_id:
        raise ReadinessError("readiness result belongs to another campaign")
    if expected_nonce is not None and value["nonce"] != expected_nonce:
        raise ReadinessError("readiness result nonce is stale or replayed")
    if expected_bindings is not None and dict(bindings) != dict(expected_bindings):
        raise ReadinessError("readiness result bindings do not exactly match expected authority")
    if value["status"] == "complete" and any(results[k] == ZERO256 for k in result_keys):
        raise ReadinessError("passing readiness requires every nonzero canonical result digest")
