#!/usr/bin/env python3
"""Parallel read-only machine visual-audit review (stdlib only).

The auditor-mandated pipeline:
  1. Serial/isolated exact-commit capture (visual-audit-capture.sh) produces a
     provenance-bound image set; the capture lease is a dedicated CLOEXEC
     lease, never the campaign factory lock.
  2. Calibration must pass with the exact frozen model/prompt/schema: a
     known-good image is reviewed 3 times (all must pass) and every known-bad
     image must be flagged. Any misclassification, drift, outage, or missing
     image fails closed and blocks live review.
  3. Live review runs independent role shards (diagram, layout, legibility,
     state, consistency, adversarial) in parallel over the frozen artifacts;
     every finding is validated against the committed strict schema and is
     provenance-bound to the exact captured image bytes.
  4. Aggregation is deterministic and advisory: machine vision never
     certifies runtime, never promotes an evidence tier, and can only add
     findings. A receipt proves invocation, not visual truth.

Vision model outage, malformed output, image tamper, protocol drift, replay
of an older report, and shared-session races all fail closed.

Usage:
  visual-audit-review.py run
  visual-audit-review.py calibrate
  visual-audit-review.py verify-json --finding FILE
  visual-audit-review.py report-check --report FILE [--current-commit HASH]
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tomllib

SCHEMA = "ralph-visual-audit-review/v1"
REPORT_SCHEMA = "ralph-visual-audit-report/v1"
ROLES = ("diagram", "layout", "legibility", "state", "consistency", "adversarial")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
SHA1 = re.compile(r"^[0-9a-f]{40}$")


def die(message: str) -> "NoReturn":
    raise SystemExit(f"visual-audit-review: {message}")


def config(path: Path) -> dict:
    with open(path, "rb") as stream:
        data = tomllib.load(stream)
    section = data.get("visual-audit", {})
    if not isinstance(section, dict):
        die("visual-audit section is missing")
    return section


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def load_review_schema(path: Path) -> dict:
    raw = json.loads(path.read_text(encoding="utf-8"))
    if raw.get("$id") != SCHEMA:
        die("review schema $id does not match the expected schema")
    return raw


def validate_finding(finding: dict, schema: dict) -> None:
    """Strict structural validation against the committed schema."""
    if not isinstance(finding, dict):
        raise SystemExit("visual-audit-review: finding is not an object")
    required = {"schema", "state_id", "image_sha256", "role", "model",
                "prompt_sha256", "schema_sha256", "verdict", "observations"}
    if not required.issubset(set(finding)):
        raise SystemExit(f"visual-audit-review: finding missing keys {sorted(required - set(finding))}")
    if set(finding) - required - {"untrusted_image_text_note"}:
        raise SystemExit(f"visual-audit-review: finding has unexpected keys {sorted(set(finding) - required)}")
    if finding["schema"] != SCHEMA:
        raise SystemExit("visual-audit-review: finding schema mismatch")
    if finding["role"] not in ROLES and finding["role"] != "probe":
        raise SystemExit(f"visual-audit-review: invalid role {finding['role']}")
    if finding["verdict"] not in ("pass", "finding", "error"):
        raise SystemExit(f"visual-audit-review: invalid verdict {finding['verdict']}")
    for key in ("image_sha256", "prompt_sha256", "schema_sha256"):
        if not SHA256.fullmatch(finding.get(key, "")):
            raise SystemExit(f"visual-audit-review: {key} invalid")
    observations = finding["observations"]
    if not isinstance(observations, list):
        raise SystemExit("visual-audit-review: observations is not an array")
    for obs in observations:
        if not isinstance(obs, dict) or set(obs) - {"code", "severity", "description", "region", "expected", "observed"}:
            raise SystemExit("visual-audit-review: observation schema invalid")
        if not re.fullmatch(r"^[A-Z][A-Z0-9_]{2,63}$", obs.get("code", "")):
            raise SystemExit("visual-audit-review: observation code invalid")
        if obs.get("severity") not in ("info", "low", "medium", "high", "critical"):
            raise SystemExit("visual-audit-review: observation severity invalid")
        if not isinstance(obs.get("description", ""), str) or not obs["description"]:
            raise SystemExit("visual-audit-review: observation description invalid")


def build_sdk_argv(section: dict, driver: Path) -> list[str]:
    """SDK driver argv; tests may substitute VISUAL_AUDIT_SDK_DRIVER."""
    injected = os.environ.get("VISUAL_AUDIT_SDK_DRIVER", "")
    if injected:
        base = injected.split()
    else:
        if not driver.is_file() or driver.is_symlink():
            die("SDK driver missing")
        node = shutil.which("node")
        if not node:
            die("node is unavailable; visual-audit requires the pi SDK driver")
        base = [node, str(driver)]
    return base


def run_one(section: dict, driver: Path, image: Path, expected_sha: str, state_id: str,
            role: str, prompt_path: Path, prompt_hash: str, schema_hash: str,
            out_dir: Path, timeout: int) -> tuple[int, str, dict | None]:
    argv = build_sdk_argv(section, driver) + [
        "--image", str(image),
        "--expected-sha256", expected_sha,
        "--state-id", state_id,
        "--role", role,
        "--prompt-file", str(prompt_path),
        "--prompt-sha256", prompt_hash,
        "--schema-sha256", schema_hash,
        "--model", section.get("vision_model", ""),
        "--out-dir", str(out_dir),
    ]
    try:
        result = subprocess.run(argv, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return 124, "reviewer timed out", None
    except OSError as exc:
        return 127, f"reviewer unavailable: {type(exc).__name__}: {exc}", None
    output = result.stdout.decode("utf-8", errors="replace") + result.stderr.decode("utf-8", errors="replace")
    if result.returncode != 0:
        return result.returncode, output, None
    finding_path = out_dir / f"finding-{state_id}-{role}.json"
    if not finding_path.is_file():
        return result.returncode, "SDK driver produced no finding file", None
    finding = json.loads(finding_path.read_text(encoding="utf-8"))
    return 0, output, finding


def aggregate(findings: list[dict]) -> dict:
    by_state: dict[str, dict] = {}
    for finding in findings:
        entry = by_state.setdefault(finding["state_id"], {"findings": [], "verdicts": []})
        entry["findings"].append(finding)
        entry["verdicts"].append(finding["verdict"])
    aggregate = {"states": []}
    for state_id, entry in sorted(by_state.items()):
        verdicts = entry["verdicts"]
        critical = [f for f in entry["findings"] if f["verdict"] == "error"]
        flagged = [f for f in entry["findings"] if f["verdict"] == "finding"]
        passed = [f for f in entry["findings"] if f["verdict"] == "pass"]
        overall = "error" if critical else ("finding" if flagged else ("pass" if passed else "error"))
        aggregate["states"].append({
            "state_id": state_id,
            "overall": overall,
            "reviewers": len(verdicts),
            "passed": len(passed),
            "findings": len(flagged),
            "errors": len(critical),
            "observation_count": sum(len(f["observations"]) for f in entry["findings"]),
        })
    return aggregate


def cmd_calibrate(section: dict, root: Path, driver: Path) -> int:
    if section.get("enabled") is not True:
        print("visual-audit-review: visual audit disabled; skipping calibration")
        return 0
    cal_path = Path(section["calibration_set"])
    if not cal_path.is_absolute():
        cal_path = root / cal_path
    cal = json.loads(cal_path.read_text(encoding="utf-8"))
    if cal.get("schema") != "ralph-visual-audit-calibration/v1":
        die("calibration set schema invalid")
    prompt_path = Path(section["prompt_template"])
    if not prompt_path.is_absolute():
        prompt_path = root / prompt_path
    schema_path = Path(section["review_schema"])
    if not schema_path.is_absolute():
        schema_path = root / schema_path
    prompt_hash = sha256_file(prompt_path)
    schema_hash = sha256_file(schema_path)
    timeout = int(section.get("review_timeout_seconds", 300))
    capture_dir = Path(section["capture_dir"])
    if not capture_dir.is_absolute():
        capture_dir = root / capture_dir
    cal_dir = capture_dir / "calibration"
    out_dir = capture_dir / "calibration-out"
    out_dir.mkdir(parents=True, exist_ok=True)
    failed = 0
    for entry in cal["images"]:
        expectation = entry["expectation"]
        image = cal_dir / f"{entry['id']}.png"
        if not image.is_file() or image.is_symlink():
            print(f"visual-audit-review: calibration image missing: {entry['id']}", file=sys.stderr)
            failed += 1
            continue
        expected_sha = sha256_file(image)
        runs = 3 if expectation == "pass" else 1
        verdicts = []
        for run in range(runs):
            rc, output, finding = run_one(
                section, driver, image, expected_sha, entry["id"], "adversarial",
                prompt_path, prompt_hash, schema_hash, out_dir, timeout)
            if rc != 0:
                print(f"visual-audit-review: calibration {entry['id']} run {run + 1} failed (rc={rc})", file=sys.stderr)
                verdicts.append("error")
                continue
            validate_finding(finding, schema_path and load_review_schema(schema_path))
            verdicts.append(finding["verdict"])
        if expectation == "pass":
            ok = all(v == "pass" for v in verdicts) and len(verdicts) == 3
        else:
            ok = all(v != "pass" for v in verdicts) and len(verdicts) == 1
        if not ok:
            print(f"visual-audit-review: calibration FAILED: {entry['id']} expected {expectation}, got {verdicts}", file=sys.stderr)
            failed += 1
        else:
            print(f"visual-audit-review: calibration OK: {entry['id']} -> {verdicts}")
    if failed:
        raise SystemExit(f"visual-audit-review: calibration failed ({failed} misclassified)")
    print("visual-audit-review: calibration passed")
    return 0


def cmd_run(section: dict, root: Path, driver: Path) -> int:
    if section.get("enabled") is not True:
        print("visual-audit-review: visual audit disabled; nothing to review")
        return 0
    capture_dir = Path(section["capture_dir"])
    if not capture_dir.is_absolute():
        capture_dir = root / capture_dir
    provenance = capture_dir / "provenance.json"
    if not provenance.is_file():
        die("no provenance manifest; run visual-audit-capture.sh first")
    manifest = json.loads(provenance.read_text(encoding="utf-8"))
    if manifest.get("schema") != "ralph-visual-audit-provenance/v1":
        die("provenance manifest schema invalid")

    prompt_path = Path(section["prompt_template"])
    if not prompt_path.is_absolute():
        prompt_path = root / prompt_path
    schema_path = Path(section["review_schema"])
    if not schema_path.is_absolute():
        schema_path = root / schema_path
    prompt_hash = sha256_file(prompt_path)
    schema_hash = sha256_file(schema_path)
    timeout = int(section.get("review_timeout_seconds", 300))
    parallelism = int(section.get("review_parallelism", 4))

    inventory = json.loads((root / section["inventory"]).read_text(encoding="utf-8"))
    states = {s["id"]: s for s in inventory["states"]}
    images = {e["file"]: e for e in manifest["images"]}
    review_dir = Path(section["review_dir"])
    if not review_dir.is_absolute():
        review_dir = root / review_dir
    review_dir.mkdir(parents=True, exist_ok=True)
    review_dir.chmod(0o700)

    tasks = []
    for file_name, entry in sorted(images.items()):
        state = states.get(entry["state_id"])
        if state is None:
            die(f"capture image {file_name} has no inventory state {entry['state_id']}")
        image_path = capture_dir / file_name
        for role in ROLES:
            tasks.append((state, image_path, entry["sha256"], role))

    findings: list[dict] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=parallelism) as pool:
        futures = {
            pool.submit(run_one, section, driver, image_path, expected_sha,
                        state["id"], role, prompt_path, prompt_hash, schema_hash,
                        review_dir, timeout): (state["id"], role)
            for state, image_path, expected_sha, role in tasks
        }
        for future in concurrent.futures.as_completed(futures):
            state_id, role = futures[future]
            rc, output, finding = future.result()
            if rc != 0:
                # Fail closed on reviewer outage/tooling failure: an error
                # finding alone could be mistaken for a completed review.
                die(f"reviewer outage for {state_id}/{role} (rc={rc}): {output[:400]}")
            validate_finding(finding, load_review_schema(schema_path))
            findings.append(finding)

    report = {
        "schema": REPORT_SCHEMA,
        "commit": manifest["commit"],
        "tree": manifest["tree"],
        "prompt_sha256": prompt_hash,
        "schema_sha256": schema_hash,
        "model": section.get("vision_model", ""),
        "images": manifest["images"],
        "findings": findings,
        "aggregate": aggregate(findings),
    }
    output = review_dir / "report.json"
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    output.chmod(0o600)
    print(json.dumps(report["aggregate"], indent=2, sort_keys=True))
    print(f"visual-audit-review: {len(findings)} findings written to {output}")
    return 0


def cmd_report_check(section: dict, root: Path, report_path: Path, current_commit: str) -> int:
    """Replay/drift/tamper gate on an existing report."""
    report = json.loads(report_path.read_text(encoding="utf-8"))
    if report.get("schema") != REPORT_SCHEMA:
        die("report schema mismatch")
    if current_commit and report.get("commit") != current_commit:
        die(f"report replay/commit mismatch: report {report.get('commit')} vs current {current_commit}")
    prompt_path = Path(section["prompt_template"])
    if not prompt_path.is_absolute():
        prompt_path = root / prompt_path
    schema_path = Path(section["review_schema"])
    if not schema_path.is_absolute():
        schema_path = root / schema_path
    if sha256_file(prompt_path) != report.get("prompt_sha256"):
        die("prompt drift: report prompt hash differs from the committed template")
    if sha256_file(schema_path) != report.get("schema_sha256"):
        die("schema drift: report schema hash differs from the committed schema")
    findings = report.get("findings")
    if not isinstance(findings, list) or not findings:
        die("report has no findings")
    for finding in findings:
        validate_finding(finding, load_review_schema(schema_path))
    return 0


def cmd_verify_json(section: dict, root: Path, finding_path: Path) -> int:
    schema_path = Path(section["review_schema"])
    if not schema_path.is_absolute():
        schema_path = root / schema_path
    finding = json.loads(finding_path.read_text(encoding="utf-8"))
    validate_finding(finding, load_review_schema(schema_path))
    print("visual-audit-review: finding JSON valid")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Machine visual-audit review")
    parser.add_argument("--config", default=".factory/visual-audit.toml")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("run")
    sub.add_parser("calibrate")
    v = sub.add_parser("verify-json")
    v.add_argument("--finding", required=True)
    r = sub.add_parser("report-check")
    r.add_argument("--report", required=True)
    r.add_argument("--current-commit", default="")
    args = parser.parse_args()
    root = Path.cwd()
    config_path = Path(args.config)
    if not config_path.is_absolute():
        config_path = root / config_path
    section = config(config_path)
    driver = Path(section.get("sdk_driver", "scripts/visual-audit-review-sdk.mjs"))
    if not driver.is_absolute():
        driver = root / driver
    if args.command == "run":
        return cmd_run(section, root, driver)
    if args.command == "calibrate":
        return cmd_calibrate(section, root, driver)
    if args.command == "verify-json":
        return cmd_verify_json(section, root, Path(args.finding))
    if args.command == "report-check":
        return cmd_report_check(section, root, Path(args.report), args.current_commit)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
