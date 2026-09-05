#!/usr/bin/env python3
"""Check a campaign audit report against its per-round falsification objective.

`.factory/campaign-objectives.json` maps every audit round (round r uses
objectives[(r-1) mod N]) to a product-neutral falsification objective with
objective-specific receipt categories. An audit round cannot pass without
machine receipts (or accepted runner manifests) covering every category of
that round's objective: replaying the same generic suite cannot satisfy all
rounds.

Authorization (`.factory/campaign-receipt-policy.json`):
- every objective category must exist in the tracked receipt policy, which
  binds the category to its allowlisted argv (exact argv arrays) and required
  evidence tier;
- a `[receipt: <path>]` reference satisfies a category only when the receipt's
  `tag` equals the category exactly, its recorded `argv` equals one of the
  category's allowlisted argv arrays, its `evidence_commit` equals the active
  campaign audit base, and its `coordinator_round` equals the active round
  (arbitrary commands such as a bare `true`, tag/path spoofing, stale round
  receipts, and receipts reused across rounds are rejected);
- a `[manifest: <path>]` reference satisfies a category only when the
  manifest is an exact signed record in the runner-evidence aggregate bound to
  the audit base (validated by `check-factory-runner-evidence.py`), the
  category allows manifests, and the manifest's recorded capabilities contain
  the exact capability whose committed contract `probe_argv` equals one of
  the category's allowlisted argv arrays (resolved through
  `.factory/capability-contracts.json`); manifest matching is exact capability
  evidence, never a path-substring proxy — the manifest path is ignored.
  Standalone/minimal, unsigned, fabricated, and path-category-only manifests
  are rejected;
- the protected `.factory-state/audit-coordinator.json` state is read and
  revalidated: the audit round/base must equal it exactly and every receipt's
  coordinator nonce must match it (matching `check-audit-receipts.py`);
- BLOCKED evidence lines carry no receipt/manifest and satisfy nothing;
- every required category of the round's objective must be covered by at
  least one executable-evidence line in `## Evidence reviewed`.

Usage:
  scripts/check-campaign-objectives.py [--round N] [--base COMMIT] [--report PATH] [--root ROOT]
  (the audit round and audit base come from --round/--base, the environment, or
   the protected audit coordinator state)
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OBJECTIVES_DEFAULT = ROOT / ".factory/campaign-objectives.json"
POLICY_DEFAULT = ROOT / ".factory/campaign-receipt-policy.json"
REPORT_DEFAULT = ROOT / ".factory/artifacts/campaign-audit.md"
COORDINATOR_FILE = ".factory-state/audit-coordinator.json"
RECEIPT = re.compile(r"\[receipt:\s*([^\]]+)\]")
MANIFEST = re.compile(r"\[manifest:\s*([^\]]+)\]")
SHA1 = re.compile(r"^[0-9a-f]{40}$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
TIERS = ["unit", "simulated", "private_integration", "installed", "real_system", "human"]


def fail(message: str) -> None:
    raise SystemExit(f"campaign-objectives: {message}")


def load_json(path: Path, schema: str, maximum: int = 2 * 1024 * 1024) -> dict:
    if path.is_symlink() or not path.is_file():
        fail(f"required tracked file is missing or unsafe: {path}")
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        fail(f"cannot parse {path}: {exc}")
    if not isinstance(data, dict) or data.get("schema") != schema:
        fail(f"{path} schema must be {schema}")
    return data


def load_objectives(root: Path) -> dict:
    path = root / ".factory/campaign-objectives.json"
    data = load_json(path, "ralph-campaign-objectives/v1")
    objectives = data.get("objectives")
    if not isinstance(objectives, list) or not objectives:
        fail("campaign objectives map must declare at least one objective")
    keys: list[str] = []
    for objective in objectives:
        if not isinstance(objective, dict):
            fail("every campaign objective must be an object")
        key = objective.get("key")
        if not isinstance(key, str) or not key or not re.fullmatch(r"[a-z0-9][a-z0-9-]*", key):
            fail(f"campaign objective has an invalid key: {key!r}")
        keys.append(key)
        categories = objective.get("receipt_categories")
        if not isinstance(categories, list) or not categories or not all(
            isinstance(item, str) and item for item in categories
        ):
            fail(f"objective {key} requires non-empty receipt_categories")
        if len(categories) != len(set(categories)):
            fail(f"objective {key} receipt_categories must be unique")
    if len(keys) != len(set(keys)):
        fail("campaign objective keys must be unique")
    return data


def load_policy(root: Path) -> dict[str, dict]:
    path = root / ".factory/campaign-receipt-policy.json"
    data = load_json(path, "ralph-receipt-policy/v1")
    categories = data.get("categories")
    if not isinstance(categories, list) or not categories:
        fail("receipt policy must declare at least one category")
    by_name: dict[str, dict] = {}
    for category in categories:
        if not isinstance(category, dict):
            fail("every receipt-policy category must be an object")
        name = category.get("name")
        if not isinstance(name, str) or not name or not re.fullmatch(r"[a-z0-9][a-z0-9._-]*", name):
            fail(f"receipt policy has an invalid category name: {name!r}")
        if name in by_name:
            fail(f"receipt policy has duplicate category {name}")
        tier = category.get("tier")
        if tier not in TIERS:
            fail(f"receipt policy category {name} has invalid evidence tier {tier!r}")
        if category.get("allow_manifest") is not True:
            category["allow_manifest"] = False
        argv = category.get("argv")
        if not isinstance(argv, list) or not argv:
            fail(f"receipt policy category {name} must declare allowlisted argv arrays")
        for entry in argv:
            if (
                not isinstance(entry, list) or not entry
                or not all(isinstance(item, str) and item for item in entry)
            ):
                fail(f"receipt policy category {name} has an invalid argv allowlist entry")
        by_name[name] = {
            "name": name,
            "tier": tier,
            "allow_manifest": category["allow_manifest"],
            "argv": [list(entry) for entry in argv],
        }
    return by_name


def resolve(root: Path, reference: str) -> Path:
    path = Path(reference)
    if path.is_absolute() or ".." in path.parts:
        fail(f"objective evidence reference escapes the repository: {reference}")
    resolved = (root / reference).resolve()
    try:
        resolved.relative_to(root.resolve())
    except ValueError:
        fail(f"objective evidence reference escapes the repository: {reference}")
    return resolved


def load_evidence_authority(root: Path):
    """Load the canonical hidden receipt authority from this repository.

    Runtime receipts are never parsed independently here: the shared authority
    owns hardened coordinator, no-follow record/transcript, digest, and exact
    round/base/nonce validation.  Runner manifests remain on their distinct
    signed aggregate path below.
    """
    loop_dir = root / ".factory" / "loop"
    source = loop_dir / "evidence.py"
    if source.is_symlink() or not source.is_file():
        fail("canonical runtime evidence authority is missing or unsafe")
    module_name = "_factory_campaign_objective_evidence"
    spec = importlib.util.spec_from_file_location(module_name, source)
    if spec is None or spec.loader is None:
        fail("cannot load the canonical runtime evidence authority")
    module = importlib.util.module_from_spec(spec)
    previous_path = list(sys.path)
    sys.modules[module_name] = module
    try:
        sys.path.insert(0, str(loop_dir))
        spec.loader.exec_module(module)
    except Exception as exc:
        fail(f"cannot load the canonical runtime evidence authority: {exc}")
    finally:
        sys.path[:] = previous_path
    for api in ("active_coordinator", "validate_receipt"):
        if not callable(getattr(module, api, None)):
            fail(f"canonical runtime evidence authority lacks {api}")
    return module


def manifest_capabilities(root: Path, reference: str) -> list[str]:
    """Return the recorded capabilities of an accepted exact-commit manifest.

    The reference has already passed the strict runner-evidence validation
    (signature, commit/tree/environment/archive/probe-authority bindings), so the file is
    the exact signed record; its `capabilities` array is the runner's exact
    capability evidence for category matching.
    """
    path = resolve(root, reference)
    if path.is_symlink() or not path.is_file():
        fail(f"objective manifest is missing or unsafe: {reference}")
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        fail(f"invalid objective manifest {reference}: {exc}")
    if not isinstance(data, dict) or data.get("schema") != "factory-runner-receipt/v3":
        fail(f"objective manifest schema is invalid: {reference}")
    capabilities = data.get("capabilities")
    if not isinstance(capabilities, list) or not all(
        isinstance(item, str) and item for item in capabilities
    ):
        fail(f"objective manifest has invalid capabilities: {reference}")
    return list(capabilities)


def capability_evidence(root: Path, policy: dict[str, dict]) -> dict[str, list[str]]:
    """Map each receipt-policy category to the capabilities that evidence it.

    A capability evidences a category exactly when its committed contract
    `probe_argv` (from `.factory/capability-contracts.json`) equals one of the
    category's allowlisted argv arrays. This is what makes manifest category
    matching exact capability evidence rather than a path-substring proxy: a
    runner manifest covers a category only through the capability that runs
    exactly the category's fixed probe command.
    """
    contracts_path = root / ".factory/capability-contracts.json"
    contracts: list[object] = []
    if contracts_path.is_symlink() or not contracts_path.is_file():
        fail("capability-contracts is missing or unsafe; manifest evidence cannot be category-matched")
    try:
        data = json.loads(contracts_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        fail(f"cannot parse capability-contracts: {exc}")
    if not isinstance(data, dict) or data.get("schema") != "ralph-capability-contract/v1":
        fail("capability-contracts schema is invalid; manifest evidence cannot be category-matched")
    contracts = data.get("capabilities", [])
    if not isinstance(contracts, list):
        fail("capability-contracts has no capabilities array")
    by_argv: dict[str, str] = {}
    for contract in contracts:
        if not isinstance(contract, dict):
            continue
        name = contract.get("name")
        argv = contract.get("probe_argv")
        if not isinstance(name, str) or not name or not isinstance(argv, list) or not argv or not all(
            isinstance(item, str) for item in argv
        ):
            continue
        by_argv.setdefault(json.dumps(argv, separators=(",", ":")), name)
    mapping: dict[str, list[str]] = {}
    for name, category in policy.items():
        capabilities = sorted(
            {
                by_argv[digest]
                for digest in (
                    json.dumps(entry, separators=(",", ":")) for entry in category["argv"]
                )
                if digest in by_argv
            }
        )
        mapping[name] = capabilities
    return mapping


def audit_binding(args: argparse.Namespace, root: Path) -> tuple[int, str, str, object]:
    """Return the mandatory canonical coordinator binding and authority.

    Runtime receipt validation has no coordinator-optional mode.  CLI/env
    claims may only repeat the hardened active state; they can never replace
    it or supply a local nonce when the state is absent/unsafe.
    """
    evidence = load_evidence_authority(root)
    try:
        state = evidence.active_coordinator(root)
    except Exception as exc:
        fail(f"runtime receipt objectives require the active coordinator: {exc}")
    env_round = os.environ.get("FACTORY_CAMPAIGN_AUDIT_ROUND", "")
    env_base = os.environ.get("FACTORY_CAMPAIGN_AUDIT_BASE", "")
    env_nonce = os.environ.get("FACTORY_CAMPAIGN_AUDIT_NONCE", "")
    claimed_round = args.round
    if claimed_round is None and env_round:
        if not env_round.isdigit() or int(env_round) < 1:
            fail("audit round environment binding is malformed")
        claimed_round = int(env_round)
    claimed_base = args.base
    if claimed_base is None and env_base:
        if not SHA1.fullmatch(env_base):
            fail("audit base environment binding is malformed")
        claimed_base = env_base
    if claimed_round is not None and claimed_round != state["round"]:
        fail("audit round does not match the protected coordinator state")
    if claimed_base is not None and claimed_base != state["base_commit"]:
        fail("audit base does not match the protected coordinator state")
    if env_nonce and env_nonce != state["nonce"]:
        fail("audit nonce does not match the protected coordinator state")
    return state["round"], state["base_commit"], state["nonce"], evidence


def strict_manifest(root: Path, reference: str, base: str) -> None:
    """Require an exact signed aggregate record bound to the audit base.

    A standalone/minimal manifest (a bare schema/result/exit JSON) is never
    accepted: the reference must be an exact record in the runner-evidence
    aggregate and pass the same signer/commit/tree/environment/archive/argv
    validation as `check-factory-runner-evidence.py`, anchored at the campaign
    audit base. Unsigned, fabricated, and path-category-only manifests fail.
    """
    helper = root / "scripts/check-factory-runner-evidence.py"
    if helper.is_symlink() or not helper.is_file():
        fail(f"strict runner-evidence helper is missing: {helper}")
    result = subprocess.run(
        [sys.executable, str(helper), "--verify-manifest", reference,
         "--expected-commit", base],
        cwd=root, text=True, capture_output=True,
    )
    if result.returncode:
        detail = (result.stderr or result.stdout).strip()
        fail(
            f"runner manifest is not an accepted exact-commit runner receipt: "
            f"{reference} ({detail or 'strict runner-evidence validation failed'})"
        )


def covered_categories(root: Path, lines: list[str], required: set[str],
                       round_number: int, base: str, nonce: str,
                       policy: dict[str, dict], evidence: object) -> set[str]:
    covered: set[str] = set()
    capability_evidence_by_category = capability_evidence(root, policy)
    for line in lines:
        for receipt_ref in RECEIPT.findall(line):
            try:
                data = evidence.validate_receipt(
                    root,
                    receipt_ref,
                    expected_round=round_number,
                    expected_base=base,
                    expected_nonce=nonce,
                )
            except Exception as exc:
                fail(f"objective runtime receipt is invalid: {receipt_ref} ({exc})")
            tag = data.get("tag")
            if not isinstance(tag, str) or tag not in required:
                continue
            category = policy.get(tag)
            if category is None:
                fail(f"receipt tag {tag!r} has no tracked receipt-policy category")
            argv = data.get("argv")
            if argv not in category["argv"]:
                fail(
                    f"receipt {receipt_ref} argv {argv!r} is not allowlisted for category "
                    f"{tag!r} (tag/path spoofing or arbitrary command rejected)"
                )
            if data.get("exit_code") != 0:
                fail(
                    f"receipt {receipt_ref} exited {data.get('exit_code')}; "
                    "nonzero runtime evidence cannot satisfy an objective category"
                )
            covered.add(tag)
        for manifest_ref in MANIFEST.findall(line):
            strict_manifest(root, manifest_ref, base)
            capabilities = manifest_capabilities(root, manifest_ref)
            for category_name in sorted(required - covered):
                category = policy[category_name]
                evidencing = capability_evidence_by_category.get(category_name, [])
                if not evidencing or not any(
                    capability in capabilities for capability in evidencing
                ):
                    continue
                if not category["allow_manifest"]:
                    fail(
                        f"category {category_name!r} does not allow runner-manifest coverage; "
                        f"a machine receipt with the allowlisted argv is required"
                    )
                covered.add(category_name)
    return covered


def parse_evidence_lines(report: Path) -> list[str]:
    text = report.read_text(encoding="utf-8")
    match = re.search(r"^## Evidence reviewed\s*\n(.*?)(?=^## |\Z)", text, re.M | re.S)
    if not match:
        fail("audit report has no Evidence reviewed section")
    lines = []
    for line in match.group(1).splitlines():
        stripped = line.strip()
        if stripped.startswith("- Executable evidence:"):
            lines.append(stripped)
    return lines


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--round", type=int, default=None)
    parser.add_argument("--base", default=None)
    parser.add_argument("--report", default=str(REPORT_DEFAULT))
    parser.add_argument("--root", default=str(ROOT))
    args = parser.parse_args()
    root = Path(args.root).resolve()
    round_number, base, nonce, evidence = audit_binding(args, root)
    data = load_objectives(root)
    objectives = data["objectives"]
    objective = objectives[(round_number - 1) % len(objectives)]
    required = set(objective["receipt_categories"])
    policy = load_policy(root)
    missing_policy = sorted(required - set(policy))
    if missing_policy:
        fail(
            f"objective `{objective['key']}` requires receipt categories without a "
            f"tracked receipt-policy binding: {missing_policy}"
        )
    report_path = root / args.report if not Path(args.report).is_absolute() else Path(args.report)
    if report_path.is_symlink() or not report_path.is_file():
        fail(f"audit report is missing: {report_path}")
    lines = parse_evidence_lines(report_path)
    covered = covered_categories(
        root, lines, required, round_number, base, nonce, policy, evidence
    )
    missing = sorted(required - covered)
    if missing:
        fail(
            f"round {round_number} objective `{objective['key']}` requires objective-specific "
            f"receipt categories not covered by the audit evidence: {missing}"
        )
    print(
        f"campaign-objectives: round {round_number} objective `{objective['key']}` "
        f"covered all {len(required)} required receipt categories at base {base[:12]}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
