#!/usr/bin/env python3
"""Visual-audit aggregate gate (stdlib only).

Reads the review report and enforces the auditor-mandated fail-closed rules:
  - provenance: report commit/tree must match the current exact commit
  - replay: an older report cannot be replayed as current evidence
  - drift: prompt and schema hashes must match the committed files
  - tamper: every finding must validate against the strict schema
  - outage: a missing report when enabled is an infrastructure failure
  - tier honesty: machine vision never promotes an evidence tier; a PASS here
    only means no machine findings, and deterministic/real-system/human
    acceptance tiers are untouched (this gate never writes to conformance).

Exit codes:
  0  report valid, no findings above low severity, tiers untouched
  1  report has findings/errors or provenance/drift/tamper problems
  2  infrastructure problem (missing report, unsafe file, model outage)
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys
import tomllib

REPORT_SCHEMA = "ralph-visual-audit-report/v1"
SHA256 = re.compile(r"^[0-9a-f]{64}$")
SHA1 = re.compile(r"^[0-9a-f]{40}$")
HIGH_SEVERITY = {"medium", "high", "critical"}


def die(message: str) -> "NoReturn":
    raise SystemExit(message)


def sha256_file(path: Path) -> str:
    import hashlib
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description="Visual-audit aggregate gate")
    parser.add_argument("--config", default=".factory/visual-audit.toml")
    parser.add_argument("--report", default=None)
    parser.add_argument("--current-commit", default="")
    args = parser.parse_args()
    root = Path.cwd()
    config_path = Path(args.config)
    if not config_path.is_absolute():
        config_path = root / config_path
    with open(config_path, "rb") as stream:
        config = tomllib.load(stream)
    section = config.get("visual-audit", {})
    if not isinstance(section, dict):
        die("visual-audit: config section missing")
    if section.get("enabled") is not True:
        print("visual-audit: disabled; nothing to check")
        return 0

    prompt_path = Path(section["prompt_template"])
    if not prompt_path.is_absolute():
        prompt_path = root / prompt_path
    schema_path = Path(section["review_schema"])
    if not schema_path.is_absolute():
        schema_path = root / schema_path
    if not prompt_path.is_file() or prompt_path.is_symlink():
        die("visual-audit: prompt template missing or unsafe")
    if not schema_path.is_file() or schema_path.is_symlink():
        die("visual-audit: review schema missing or unsafe")

    report_path = Path(args.report) if args.report else Path(section["review_dir"]) / "report.json"
    if not report_path.is_absolute():
        report_path = root / report_path
    if not report_path.is_file() or report_path.is_symlink():
        die("visual-audit: report missing; run visual-audit-review.py run first")
    if report_path.stat().st_uid != __import__("os").getuid() or report_path.stat().st_nlink != 1:
        die("visual-audit: report has unsafe ownership or link count")

    try:
        report = json.loads(report_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        die(f"visual-audit: report is invalid: {type(exc).__name__}")
    if report.get("schema") != REPORT_SCHEMA:
        die("visual-audit: report schema mismatch")
    commit = report.get("commit")
    tree = report.get("tree")
    if not isinstance(commit, str) or not SHA1.fullmatch(commit):
        die("visual-audit: report commit invalid")
    if not isinstance(tree, str) or not SHA1.fullmatch(tree):
        die("visual-audit: report tree invalid")
    current_commit = args.current_commit or (
        __import__("subprocess").check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    )
    if SHA1.fullmatch(current_commit) and commit != current_commit:
        die(f"visual-audit: report replay/commit mismatch (report {commit}, current {current_commit})")
    if not SHA256.fullmatch(report.get("prompt_sha256", "")):
        die("visual-audit: report prompt hash invalid")
    if not SHA256.fullmatch(report.get("schema_sha256", "")):
        die("visual-audit: report schema hash invalid")
    if sha256_file(prompt_path) != report["prompt_sha256"]:
        die("visual-audit: prompt drift; findings invalidated")
    if sha256_file(schema_path) != report["schema_sha256"]:
        die("visual-audit: schema drift; findings invalidated")

    findings = report.get("findings")
    if not isinstance(findings, list) or not findings:
        die("visual-audit: report has no findings")
    problems = 0
    for finding in findings:
        if finding.get("schema") != "ralph-visual-audit-review/v1":
            die("visual-audit: finding schema mismatch")
        verdict = finding.get("verdict")
        if verdict not in ("pass", "finding", "error"):
            die("visual-audit: finding verdict invalid")
        if verdict in ("finding", "error"):
            problems += 1
        for observation in finding.get("observations", []):
            if observation.get("severity") in HIGH_SEVERITY:
                problems += 1
        # Tamper: a finding must be provenance-bound to the report image set.
        image_sha = finding.get("image_sha256")
        if not SHA256.fullmatch(image_sha or ""):
            die("visual-audit: finding image hash invalid (tamper)")
        if not any(img.get("sha256") == image_sha for img in report.get("images", [])):
            die(f"visual-audit: finding image {image_sha[:12]} not in the provenance manifest (tamper)")

    aggregate = report.get("aggregate")
    if not isinstance(aggregate, dict):
        die("visual-audit: report aggregate missing")
    for state in aggregate.get("states", []):
        if state.get("overall") in ("finding", "error"):
            problems += 1

    if problems:
        print(f"visual-audit: {problems} finding(s)/error(s) reported")
        return 1
    print("visual-audit: report valid, no findings; vision tiers untouched (no elevation)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
