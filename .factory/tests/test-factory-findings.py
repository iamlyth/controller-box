#!/usr/bin/env python3
"""Hidden findings-authority suite (Task 10; FIND-01, §16).

This test lives under the hidden ``.factory/tests/`` namespace and drives
``.factory/loop/findings.py`` — the structured tester/auditor findings
authority — plus the Task 10 §16 integration in ``campaign.py``, the
production ``InvocationBinding`` digest binding in ``launch.py``, and the
``findings-revised`` planner seam of the committed fixture driver.

The suite proves the §16 contract end to end:

* **positive flow**: verification/audit ``findings`` and ``blocked`` are
  minted by the trusted orchestrator as write-once no-replace receipts under
  the ignored ``.factory-state/`` namespace, bound to the exact campaign,
  round, phase, phase tag, phase-base commit, and the exact structured
  result bytes the orchestrator read; the next planning phase re-reads the
  receipts through the hardened no-follow bounded reader, re-validates every
  binding, and derives one deterministic ``factory-findings/v1`` payload
  that reaches only the next planner; the planner's ``findings-revised``
  seam incorporates the accepted findings into the canonical plan as
  revised/blocked tasks; only then does the next developer run;
* **no selector/evidence authority**: the deterministic selector is a pure
  function of plan + state and never reads the payload; the payload is never
  persisted, never handed to the developer/tester/auditor, and never stored
  as a runtime task ledger, memory, or context summary;
* **fail-closed classes**: malformed, oversized, symlinked, stale,
  foreign, synthetic, pass-phase, receipt-only, pre-planted, and tampered
  receipts (wrong outcome, wrong result digest, wrong phase-base commit,
  unrecorded ledger tag) each fail closed with a dedicated error class
  before the next planner launches;
* **exact result digest**: the receipt's ``result_digest`` is the SHA-256 of
  the exact structured result bytes the orchestrator consumed, recorded in
  the campaign result phase history, and re-bound at consumption.

It reuses the committed fixture workspace and helper conventions of the
sibling hidden campaign suite (``test-factory-campaign.py``) instead of
copying them.
"""

from __future__ import annotations

import dataclasses
import importlib.util
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
import unittest.mock

ROOT = Path(__file__).resolve().parents[2]
LOOP = ROOT / ".factory" / "loop"
STATE_DIR = ".factory-state"

sys.path.insert(0, str(LOOP))

# Load the sibling hidden campaign suite so the committed fixture workspace
# and helper conventions are reused, never copied.
_CAMPAIGN_SUITE = ROOT / ".factory" / "tests" / "test-factory-campaign.py"
_spec = importlib.util.spec_from_file_location(
    "factory_campaign_suite", _CAMPAIGN_SUITE)
FACTORY_CAMPAIGN = importlib.util.module_from_spec(_spec)
assert _spec.loader is not None
_spec.loader.exec_module(FACTORY_CAMPAIGN)

# The committed fixture plan generator produces valid ``factory-plan/v1``
# documents through the real parser; the defect-5 reflection tests build
# every revised-plan fixture through it (never hand-rolled plan bytes).
_FIXTURE_TOOL = ROOT / ".factory" / "tests" / "fixtures" / "fixture_plan_tool.py"
_spec_tool = importlib.util.spec_from_file_location(
    "fixture_plan_tool", _FIXTURE_TOOL)
FIXTURE_PLAN_TOOL = importlib.util.module_from_spec(_spec_tool)
assert _spec_tool.loader is not None
_spec_tool.loader.exec_module(FIXTURE_PLAN_TOOL)

import campaign as campaign_module  # noqa: E402
import findings as findings_module  # noqa: E402
import launch as launch_module  # noqa: E402
import plan_parser  # noqa: E402
import selector as selector_module  # noqa: E402
import state as state_module  # noqa: E402

FixtureWorkspace = FACTORY_CAMPAIGN.FixtureWorkspace
TASK_SPECS = FACTORY_CAMPAIGN.TASK_SPECS
SUCCESS_SCENARIO = FACTORY_CAMPAIGN.SUCCESS_SCENARIO
assert_history = FACTORY_CAMPAIGN.assert_history
assert_terminal = FACTORY_CAMPAIGN.assert_terminal
gen_plan = FACTORY_CAMPAIGN.gen_plan
sha256 = FACTORY_CAMPAIGN.sha256
_git = FACTORY_CAMPAIGN._git
TRUE_EXECUTABLE = FACTORY_CAMPAIGN.TRUE_EXECUTABLE
FALSE_EXECUTABLE = FACTORY_CAMPAIGN.FALSE_EXECUTABLE
REQUIREMENT_REGISTRY = FACTORY_CAMPAIGN.REQUIREMENT_REGISTRY
DRIVER_REL = FACTORY_CAMPAIGN.DRIVER_REL
PLAN_REL = FACTORY_CAMPAIGN.PLAN_REL
BRANCH = FACTORY_CAMPAIGN.BRANCH

EXIT_ERROR = campaign_module.EXIT_ERROR

RESULT_SCHEMA_NAME = "factory-phase-result/v1"


def _canonical_result(outcome: str, findings=None, blocked_on=None) -> bytes:
    """The exact bytes the fixture tester/auditor writes (sorted canonical)."""
    data = {"schema": RESULT_SCHEMA_NAME, "outcome": outcome}
    if findings:
        data["findings"] = findings
    if blocked_on:
        data["blocked_on"] = blocked_on
    return json.dumps(data, sort_keys=True, separators=(",", ":")).encode("utf-8")


def receipt_rel(round_no: int, phase: str) -> str:
    return (
        f"{STATE_DIR}/factory-findings-receipt-round-{round_no}-{phase}.json"
    )


# ---------------------------------------------------------------------------
# Defect 5: plan reflection is judged against the parsed `factory-plan/v1`
# task model, never raw plan bytes
# ---------------------------------------------------------------------------

FINDING = "readiness finding"
BLOCKER = "external-capability-required"


def _reflection_tasks(overrides: dict | None = None) -> list[dict]:
    """A valid three-task fixture set (final audit task last)."""
    tasks = [
        {
            "number": 1, "title": "Implement the fixture feature",
            "status": "pending", "priority": 10, "dependencies": [],
            "blocked_on": None, "scope": "initial scope statement.",
            "acceptance": "fixture acceptance gate passes.",
            "verification": "`src/work-1.md`",
            "documentation": "none.",
            "evidence": "",
        },
        {
            "number": 2, "title": "Implement the second feature",
            "status": "pending", "priority": 20, "dependencies": [],
            "blocked_on": None,
            "scope": "second scope statement.",
            "acceptance": "second acceptance gate passes.",
            "verification": "`src/work-2.md`",
            "documentation": "none.",
            "evidence": "",
        },
        {
            "number": 3, "title": "final", "status": "pending",
            "priority": 1, "dependencies": [1, 2], "blocked_on": None,
            "scope": "audit scope.",
            "acceptance": "audit gate passes.",
            "verification": "`src/work-3.md`",
            "documentation": "none.",
            "evidence": "",
        },
    ]
    for key, value in (overrides or {}).items():
        tasks[int(key) - 1].update(value)
    return tasks


def _reflection_plan(tasks=None, *, lifecycle: str = "active") -> bytes:
    """A valid ``factory-plan/v1`` document through the committed tool."""
    spec = {
        "spec_path": "docs/SPEC.md",
        "spec_commit": "a" * 40,
        "spec_blob": "b" * 40,
        "base_commit": "c" * 40,
        "lifecycle": lifecycle,
        "tasks": tasks if tasks is not None else _reflection_tasks(),
    }
    return FIXTURE_PLAN_TOOL.generate(spec, REQUIREMENT_REGISTRY).encode("utf-8")


# ---------------------------------------------------------------------------
# Unit scaffolding: a scratch root with the hardened `.factory-state/` I/O
# ---------------------------------------------------------------------------


class _FindingsBase(unittest.TestCase):
    """Shared helpers: one fresh scratch root per test (no Git needed)."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="factory-findings-test."))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.root = self.tmp / "root"
        self.root.mkdir()
        self.campaign_id = "campaign"
        self.head = "a" * 40
        self.tag = "r1.verification.1.a1"
        # The campaign always owns a private `.factory-state/` namespace; the
        # unit scratch root mirrors that so missing-receipt reads are clean.
        (self.root / STATE_DIR).mkdir(mode=0o700)

    def _write_marker(self, name: str, data: bytes) -> None:
        directory = self.root / STATE_DIR
        directory.mkdir(mode=0o700, exist_ok=True)
        path = directory / name
        path.write_bytes(data)
        os.chmod(path, 0o600)

    def _read_marker(self, name: str) -> bytes:
        return (self.root / STATE_DIR / name).read_bytes()

    def _write_artifact(self, name: str, data: bytes) -> None:
        directory = self.root / STATE_DIR
        directory.mkdir(mode=0o700, exist_ok=True)
        path = directory / name
        path.write_bytes(data)
        os.chmod(path, 0o600)

    def preserve(self, round_no: int, phase: str, raw: bytes) -> str:
        """Preserve the exact phase-result bytes (REQ 3) like the mint does."""
        return findings_module.preserve_phase_result(
            self.root, round_no, phase, raw
        )

    def record(
        self, round_no: int, phase: str, outcome: str,
        result_digest: str = "",
    ) -> campaign_module.PhaseRecord:
        return campaign_module.PhaseRecord(
            round=round_no, phase=phase, attempt=1, outcome=outcome,
            head_commit=self.head, plan_digest="0" * 64,
            detail="", result_digest=result_digest,
        )

    def mint(self, **overrides) -> dict:
        params = dict(
            campaign_id="campaign", round_number=1, phase="verification",
            phase_tag=self.tag, phase_base_commit=self.head,
            outcome="findings",
            result_digest=sha256(_canonical_result("findings")),
            findings=["fixture finding"], blocked_on=[],
            gate_ran=True, gate_exit=0,
            capability_ran=False, capability_exit=None,
        )
        params.update(overrides)
        return findings_module.build_receipt(**params)

    def publish(self, receipt: dict) -> str:
        return findings_module.publish_receipt(self.root, receipt)

    def ledger(self, *tags: str) -> None:
        lines = [
            json.dumps({"tag": tag, "digest": "0" * 64},
                       sort_keys=True, separators=(",", ":"))
            for tag in tags
        ]
        self._write_marker(
            state_module.DIGEST_LEDGER_NAME,
            ("\n".join(lines) + "\n").encode("utf-8"),
        )

    def consume(
        self,
        records=(),
        *,
        source_round: int = 1,
        head: str | None = None,
        campaign_id: str | None = None,
        is_ancestor=lambda a, b: True,
    ):
        return findings_module.consume_next_round_findings(
            self.root,
            campaign_id=campaign_id or self.campaign_id,
            source_round=source_round,
            head=head or self.head,
            is_ancestor=is_ancestor,
            phase_records=tuple(records),
        )

    @staticmethod
    def payload(raw: bytes) -> dict:
        return json.loads(raw.decode("utf-8"))


# ---------------------------------------------------------------------------
# Receipt minting: write-once no-replace evidence, every binding validated
# ---------------------------------------------------------------------------


class FindingsMintUnit(_FindingsBase):
    """§16 mint: the trusted orchestrator mints write-once receipts."""

    def test_build_receipt_rejects_unsafe_bindings(self) -> None:
        for kwargs, fragment in (
            ({"campaign_id": "not safe!"}, "campaign"),
            ({"campaign_id": ""}, "campaign"),
            ({"round_number": 0}, "round"),
            ({"round_number": True}, "round"),
            ({"phase": "implementation"}, "phase"),
            ({"outcome": "pass"}, "findings"),
            ({"outcome": "success"}, "findings"),
            ({"phase_base_commit": "beef"}, "commit"),
            ({"phase_base_commit": "A" * 40}, "commit"),
            ({"result_digest": "beef"}, "digest"),
            ({"phase_tag": "bogus"}, "phase tag"),
            ({"phase_tag": "r2.verification.1.a1"}, "phase tag"),
            ({"gate_ran": 1}, "gate"),
            ({"capability_ran": None}, "capability"),
            ({"gate_exit": -1}, "gate"),
            ({"gate_exit": True}, "gate"),
        ):
            with self.assertRaises(findings_module.FindingsError, msg=fragment):
                self.mint(**kwargs)

    def test_publish_is_write_once_no_replace(self) -> None:
        receipt = self.mint()
        name = self.publish(receipt)
        # The canonical name is now owned.  A crash-window re-mint of the
        # SAME run (byte-exact) is accepted idempotently (REQ 1) — the
        # previous mint wrote these exact bytes and the state advance was
        # interrupted — while any different content fails closed instead of
        # silently replacing the evidence.
        self.assertEqual(self.publish(receipt), name)
        self.assertEqual(
            self.publish(self.mint()),
            "factory-findings-receipt-round-1-verification.json",
        )
        with self.assertRaises(findings_module.FindingsError):
            self.publish(self.mint(result_digest=sha256(b"other")))

    def test_preplanted_receipt_fails_the_mint_closed(self) -> None:
        # A pre-planted receipt at the canonical name with DIFFERENT bytes
        # (forged evidence) must never be silently replaced by a genuine
        # mint: the byte-exact idempotent recovery accepts only this run's
        # own crashed mint.
        planted = self.mint(result_digest=sha256(b"preplant"))
        self._write_marker(
            "factory-findings-receipt-round-1-verification.json",
            findings_module.receipt_bytes(planted),
        )
        with self.assertRaises(findings_module.FindingsError) as cm:
            self.publish(self.mint())
        self.assertIn("different bytes", str(cm.exception).lower())

    def test_preplanted_byte_exact_remint_is_crash_recovery(self) -> None:
        # The byte-exact pre-planted receipt IS this run's own mint that
        # survived the crash window (the state advance was lost); the rerun
        # accepts it idempotently and never wedges.
        receipt = self.mint()
        self._write_marker(
            "factory-findings-receipt-round-1-verification.json",
            findings_module.receipt_bytes(receipt),
        )
        self.assertEqual(
            self.publish(receipt),
            "factory-findings-receipt-round-1-verification.json",
        )

    def test_receipt_roundtrip_and_digest(self) -> None:
        receipt = self.mint(
            findings=["first", "second"],
            blocked_on=["external-capability-required"],
        )
        name = self.publish(receipt)
        self.assertEqual(
            name, "factory-findings-receipt-round-1-verification.json")
        data, raw_digest = findings_module.read_receipt(
            self.root, 1, "verification")
        assert data is not None
        self.assertEqual(data["schema"], "factory-findings-receipt/v1")
        self.assertEqual(data["outcome"], "findings")
        self.assertEqual(data["findings"], ["first", "second"])
        self.assertEqual(data["blocked_on"], ["external-capability-required"])
        # The digest is the digest of the exact canonical bytes on disk.
        self.assertEqual(raw_digest, sha256(self._read_marker(name)))
        self.assertEqual(
            raw_digest,
            findings_module.sha256(findings_module.receipt_bytes(data)),
        )

    def test_receipt_name_and_phase_gates(self) -> None:
        self.assertEqual(
            findings_module.receipt_name(1, "verification"),
            "factory-findings-receipt-round-1-verification.json",
        )
        for bad in (0, -1, True, "1", 1.5):
            with self.assertRaises(findings_module.FindingsError):
                findings_module.receipt_name(bad, "verification")
        for phase in ("planning", "implementation", "developer"):
            with self.assertRaises(findings_module.FindingsError):
                findings_module.receipt_name(1, phase)


# ---------------------------------------------------------------------------
# Consumption: every binding fail-closed, positive flow, no authority leak
# ---------------------------------------------------------------------------


class FindingsConsumeUnit(_FindingsBase):
    """§16: the next-planner payload is derived only from bound receipts."""

    def _positive_setup(self) -> None:
        self.ledger(self.tag)
        # REQ 3: the exact structured phase-result bytes are preserved first,
        # so consumption authenticates the receipt against the exact trusted
        # content (digest AND parsed findings/blocked_on), never a
        # self-digest only.
        self.preserve(
            1, "verification",
            _canonical_result("findings", findings=["fixture finding"]),
        )
        self.publish(self.mint(
            result_digest=sha256(_canonical_result(
                "findings", findings=["fixture finding"])),
        ))

    def test_consume_derives_deterministic_payload(self) -> None:
        self._positive_setup()
        result_digest = sha256(_canonical_result(
            "findings", findings=["fixture finding"]))
        record = self.record(1, "verification", "findings",
                             result_digest=result_digest)
        raw = self.consume([record])
        assert raw is not None
        payload = self.payload(raw)
        self.assertEqual(payload["schema"], "factory-findings/v1")
        self.assertEqual(payload["campaign_id"], "campaign")
        self.assertEqual(payload["source_round"], 1)
        self.assertEqual(len(payload["entries"]), 1)
        entry = payload["entries"][0]
        self.assertEqual(entry["phase"], "verification")
        self.assertEqual(entry["outcome"], "findings")
        self.assertEqual(entry["phase_base_commit"], self.head)
        self.assertEqual(entry["phase_tag"], self.tag)
        self.assertEqual(entry["result_digest"], result_digest)
        self.assertEqual(entry["gate_ran"], True)
        self.assertEqual(entry["gate_exit"], 0)
        self.assertEqual(entry["capability_ran"], False)
        self.assertIsNone(entry["capability_exit"])
        self.assertEqual(
            entry["receipt_path"],
            "factory-findings-receipt-round-1-verification.json",
        )
        self.assertEqual(
            entry["receipt_digest"],
            findings_module.sha256(self._read_marker(
                "factory-findings-receipt-round-1-verification.json")),
        )
        self.assertEqual(entry["findings"], ["fixture finding"])
        self.assertEqual(entry["blocked_on"], [])
        # Deterministic canonical bytes: same payload bytes every time.
        again = self.consume([record])
        self.assertEqual(again, raw)
        # No wall-clock time, no prose claims, no plan copies in the payload.
        self.assertNotIn(b"timestamp", raw)
        self.assertNotIn(b"prose", raw)

    def test_consume_binds_both_verification_and_audit(self) -> None:
        audit_tag = "r1.audit.1.a1"
        self.ledger(self.tag, audit_tag)
        self.preserve(
            1, "audit",
            _canonical_result("findings", findings=["audit finding"]),
        )
        self.preserve(
            1, "verification",
            _canonical_result("findings", findings=["fixture finding"]),
        )
        self.publish(self.mint(
            phase="audit", phase_tag=audit_tag, outcome="findings",
            result_digest=sha256(_canonical_result(
                "findings", findings=["audit finding"])),
            findings=["audit finding"], gate_ran=False, gate_exit=None,
        ))
        self.publish(self.mint(
            result_digest=sha256(_canonical_result(
                "findings", findings=["fixture finding"]))))
        raw = self.consume([
            self.record(1, "verification", "findings",
                        result_digest=sha256(_canonical_result(
                            "findings", findings=["fixture finding"]))),
            self.record(1, "audit", "findings",
                        result_digest=sha256(_canonical_result(
                            "findings", findings=["audit finding"]))),
        ])
        assert raw is not None
        payload = self.payload(raw)
        self.assertEqual(
            {entry["phase"] for entry in payload["entries"]},
            {"verification", "audit"},
        )
        self.assertEqual(len(payload["entries"]), 2)

    def test_consume_external_blockers_remain_structured_findings(self) -> None:
        self.ledger(self.tag)
        result_digest = sha256(_canonical_result(
            "blocked", blocked_on=["external-capability-required"]))
        self.preserve(
            1, "verification",
            _canonical_result("blocked",
                              blocked_on=["external-capability-required"]),
        )
        self.publish(self.mint(
            outcome="blocked", result_digest=result_digest,
            findings=[], blocked_on=["external-capability-required"],
        ))
        raw = self.consume([
            self.record(1, "verification", "blocked", result_digest=result_digest),
        ])
        assert raw is not None
        entry = self.payload(raw)["entries"][0]
        self.assertEqual(entry["outcome"], "blocked")
        self.assertEqual(entry["blocked_on"], ["external-capability-required"])

    def test_consume_returns_none_when_no_findings_flowed(self) -> None:
        # No receipts exist and no phase recorded findings/blocked: no
        # payload reaches the next planner.
        self.assertIsNone(self.consume([
            self.record(1, "verification", "pass"),
            self.record(1, "audit", "pass"),
        ]))

    def test_receipt_only_claim_fails_closed(self) -> None:
        # The phase recorded findings but its receipt is missing: a claim
        # without the orchestrator-minted evidence fails closed.
        with self.assertRaises(findings_module.FindingsReceiptError):
            self.consume([
                self.record(1, "verification", "findings",
                            result_digest=sha256(_canonical_result("findings"))),
            ])

    def test_synthetic_receipt_for_pass_phase_fails_closed(self) -> None:
        self.ledger(self.tag)
        self.publish(self.mint())
        with self.assertRaises(findings_module.FindingsSyntheticError):
            self.consume([self.record(1, "verification", "pass")])

    def test_synthetic_receipt_for_unrecorded_phase_fails_closed(self) -> None:
        self.ledger(self.tag)
        self.publish(self.mint())
        # No phase record for (1, verification): a leftover/foreign receipt.
        with self.assertRaises(findings_module.FindingsForeignError):
            self.consume([self.record(1, "audit", "pass")])

    def test_foreign_campaign_receipt_fails_closed(self) -> None:
        self.ledger(self.tag)
        self.publish(self.mint(campaign_id="other-campaign"))
        with self.assertRaises(findings_module.FindingsForeignError):
            self.consume([
                self.record(1, "verification", "findings",
                            result_digest=sha256(_canonical_result("findings"))),
            ])

    def test_stale_round_receipt_fails_closed(self) -> None:
        self.ledger("r2.verification.1.a1")
        self.publish(self.mint(
            round_number=2, phase_tag="r2.verification.1.a1"))
        # Place the stale content at the canonical source-round name so the
        # parser must validate its bound round instead of merely observing a
        # missing expected receipt.
        state_dir = self.root / STATE_DIR
        (state_dir / findings_module.receipt_name(2, "verification")).rename(
            state_dir / findings_module.receipt_name(1, "verification")
        )
        with self.assertRaises(findings_module.FindingsStaleError):
            self.consume([
                self.record(1, "verification", "findings",
                            result_digest=sha256(_canonical_result("findings"))),
            ])

    def test_stale_unreachable_commit_fails_closed(self) -> None:
        self.ledger(self.tag)
        self.publish(self.mint())
        with self.assertRaises(findings_module.FindingsStaleError):
            self.consume(
                [
                    self.record(1, "verification", "findings",
                                result_digest=sha256(_canonical_result("findings"))),
                ],
                is_ancestor=lambda a, b: False,
            )

    def test_forged_outcome_fails_closed(self) -> None:
        self.ledger(self.tag)
        self.publish(self.mint(outcome="blocked"))
        with self.assertRaises(findings_module.FindingsSyntheticError):
            self.consume([
                self.record(1, "verification", "findings",
                            result_digest=sha256(_canonical_result("findings"))),
            ])

    def test_tampered_result_digest_fails_closed(self) -> None:
        self.ledger(self.tag)
        self.publish(self.mint(
            result_digest=sha256(_canonical_result(
                "findings", findings=["other"]))))
        with self.assertRaises(findings_module.FindingsSyntheticError):
            self.consume([
                self.record(1, "verification", "findings",
                            result_digest=sha256(_canonical_result("findings"))),
            ])

    def test_forged_phase_base_commit_fails_closed(self) -> None:
        self.ledger(self.tag)
        self.publish(self.mint(phase_base_commit="b" * 40))
        with self.assertRaises(findings_module.FindingsSyntheticError):
            self.consume([
                self.record(1, "verification", "findings",
                            result_digest=sha256(_canonical_result("findings"))),
            ])

    def test_unrecorded_ledger_tag_fails_closed(self) -> None:
        # A receipt whose phase tag the state digest ledger never recorded is
        # synthetic: the phase never ran under that tag.
        self.publish(self.mint())
        with self.assertRaises(findings_module.FindingsSyntheticError):
            self.consume([
                self.record(1, "verification", "findings",
                            result_digest=sha256(_canonical_result("findings"))),
            ])

    def test_malformed_json_receipt_fails_closed(self) -> None:
        self._write_marker(
            "factory-findings-receipt-round-1-verification.json",
            b"{not json",
        )
        with self.assertRaises(findings_module.FindingsMalformedError):
            self.consume([
                self.record(1, "verification", "findings",
                            result_digest=sha256(_canonical_result("findings"))),
            ])

    def test_non_object_receipt_fails_closed(self) -> None:
        self._write_marker(
            "factory-findings-receipt-round-1-verification.json",
            b'["array", 1]',
        )
        with self.assertRaises(findings_module.FindingsMalformedError):
            self.consume([self.record(1, "verification", "findings")])

    def test_schema_violating_receipt_fails_closed(self) -> None:
        name = "factory-findings-receipt-round-1-verification.json"
        record = self.record(
            1, "verification", "findings",
            result_digest=sha256(_canonical_result("findings")))
        for mutate, fragment in (
            (lambda r: r.pop("outcome"), "required"),
            (lambda r: r.__setitem__("schema", "factory-findings/v1"), "enum"),
            (lambda r: r.__setitem__("extra", 1), "extra"),
            (lambda r: r.__setitem__("round", "1"), "type"),
            (lambda r: r.__setitem__("phase", "developer"), "enum"),
            (lambda r: r.__setitem__("result_digest", "short"), "pattern"),
        ):
            receipt = self.mint()
            mutate(receipt)
            self._write_marker(name, findings_module.receipt_bytes(receipt))
            with self.assertRaises(
                findings_module.FindingsMalformedError, msg=fragment):
                self.consume([record])

    def test_oversized_receipt_fails_closed(self) -> None:
        receipt = self.mint()
        receipt["findings"] = ["x" * (findings_module.MAX_RECEIPT_BYTES + 1)]
        self._write_marker(
            "factory-findings-receipt-round-1-verification.json",
            findings_module.receipt_bytes(receipt),
        )
        with self.assertRaises(findings_module.FindingsMalformedError):
            self.consume([
                self.record(1, "verification", "findings",
                            result_digest=sha256(_canonical_result("findings"))),
            ])

    def test_symlinked_receipt_fails_closed(self) -> None:
        # A symlink at the canonical receipt name is never followed: the
        # hardened no-follow reader fails closed.
        target = self.tmp / "elsewhere.json"
        target.write_text(json.dumps(self.mint()), encoding="utf-8")
        directory = self.root / STATE_DIR
        directory.mkdir(mode=0o700, exist_ok=True)
        (directory / "factory-findings-receipt-round-1-verification.json").symlink_to(
            target)
        with self.assertRaises(findings_module.FindingsMalformedError):
            self.consume([
                self.record(1, "verification", "findings",
                            result_digest=sha256(_canonical_result("findings"))),
            ])

    # -- Task 10 REQ 3: receipt content is authenticated against the exact
    # -- preserved phase-result bytes, never a self-digest only ------------

    def _post_mint_consume(self) -> None:
        self.ledger(self.tag)
        self.preserve(
            1, "verification",
            _canonical_result("findings", findings=["fixture finding"]),
        )
        self.publish(self.mint(
            result_digest=sha256(_canonical_result(
                "findings", findings=["fixture finding"])),
        ))

    def test_post_mint_tampered_findings_vs_preserved_result_fails(self) -> None:
        # After the mint the receipt's findings list is rewritten
        # (schema-valid but contradictory); consumption authenticates the
        # receipt against the exact preserved phase-result bytes and fails
        # closed.
        self._post_mint_consume()
        receipt = json.loads(self._read_marker(
            "factory-findings-receipt-round-1-verification.json"))
        receipt["findings"] = ["tampered finding"]
        self._write_marker(
            "factory-findings-receipt-round-1-verification.json",
            findings_module.receipt_bytes(receipt),
        )
        with self.assertRaises(findings_module.FindingsSyntheticError):
            self.consume([
                self.record(1, "verification", "findings",
                            result_digest=sha256(_canonical_result(
                                "findings", findings=["fixture finding"]))),
            ])

    def test_post_mint_tampered_blocked_refs_vs_preserved_result_fails(self) -> None:
        self.ledger(self.tag)
        self.preserve(
            1, "verification",
            _canonical_result("blocked",
                              blocked_on=["external-capability-required"]),
        )
        result_digest = sha256(_canonical_result(
            "blocked", blocked_on=["external-capability-required"]))
        self.publish(self.mint(
            outcome="blocked", result_digest=result_digest,
            findings=[], blocked_on=["external-capability-required"],
        ))
        receipt = json.loads(self._read_marker(
            "factory-findings-receipt-round-1-verification.json"))
        receipt["blocked_on"] = ["another-capability"]
        self._write_marker(
            "factory-findings-receipt-round-1-verification.json",
            findings_module.receipt_bytes(receipt),
        )
        with self.assertRaises(findings_module.FindingsSyntheticError):
            self.consume([
                self.record(1, "verification", "blocked",
                            result_digest=result_digest),
            ])

    def test_post_mint_tampered_outcome_vs_preserved_result_fails(self) -> None:
        self._post_mint_consume()
        receipt = json.loads(self._read_marker(
            "factory-findings-receipt-round-1-verification.json"))
        receipt["outcome"] = "blocked"
        self._write_marker(
            "factory-findings-receipt-round-1-verification.json",
            findings_module.receipt_bytes(receipt),
        )
        with self.assertRaises(findings_module.FindingsSyntheticError):
            self.consume([
                self.record(1, "verification", "findings",
                            result_digest=sha256(_canonical_result(
                                "findings", findings=["fixture finding"]))),
            ])

    def test_tampered_preserved_result_fails_closed(self) -> None:
        # The preserved phase-result artifact is tampered after the mint:
        # consumption re-binds the receipt's result_digest to the exact
        # artifact bytes and fails closed.
        self._post_mint_consume()
        self._write_marker(
            findings_module.result_name(1, "verification"),
            _canonical_result("findings", findings=["tampered"]),
        )
        with self.assertRaises(findings_module.FindingsSyntheticError):
            self.consume([
                self.record(1, "verification", "findings",
                            result_digest=sha256(_canonical_result(
                                "findings", findings=["fixture finding"]))),
            ])

    def test_symlinked_preserved_result_fails_closed(self) -> None:
        self._post_mint_consume()
        target = self.tmp / "result-elsewhere.json"
        target.write_text(json.dumps({"schema": "factory-phase-result/v1",
                                      "outcome": "findings"}),
                          encoding="utf-8")
        directory = self.root / STATE_DIR
        name = findings_module.result_name(1, "verification")
        (directory / name).unlink()
        (directory / name).symlink_to(target)
        with self.assertRaises(findings_module.FindingsMalformedError):
            self.consume([
                self.record(1, "verification", "findings",
                            result_digest=sha256(_canonical_result(
                                "findings", findings=["fixture finding"]))),
            ])

    def test_oversized_preserved_result_fails_closed(self) -> None:
        self._post_mint_consume()
        self._write_artifact(
            findings_module.result_name(1, "verification"),
            b"{" + b"x" * (findings_module.MAX_RESULT_BYTES + 1),
        )
        with self.assertRaises(findings_module.FindingsMalformedError):
            self.consume([
                self.record(1, "verification", "findings",
                            result_digest=sha256(_canonical_result(
                                "findings", findings=["fixture finding"]))),
            ])

    def test_malformed_preserved_result_json_fails_closed(self) -> None:
        self._post_mint_consume()
        self._write_artifact(
            findings_module.result_name(1, "verification"), b"{not json",
        )
        with self.assertRaises(findings_module.FindingsMalformedError):
            self.consume([
                self.record(1, "verification", "findings",
                            result_digest=sha256(_canonical_result(
                                "findings", findings=["fixture finding"]))),
            ])

    def test_missing_preserved_result_fails_closed(self) -> None:
        # A receipt without its preserved phase-result artifact is a
        # receipt-only claim whose content cannot be authenticated.
        self.ledger(self.tag)
        self.publish(self.mint(
            result_digest=sha256(_canonical_result("findings")),
        ))
        with self.assertRaises(findings_module.FindingsReceiptError):
            self.consume([
                self.record(1, "verification", "findings",
                            result_digest=sha256(_canonical_result("findings"))),
            ])

    def test_malformed_ledger_fails_consumption_closed(self) -> None:
        # The strict state-ledger parser is the only ledger authority: a
        # malformed line fails the findings consumption closed instead of
        # being silently tolerated.
        self._post_mint_consume()
        self._write_marker(
            state_module.DIGEST_LEDGER_NAME, b"{not json\n",
        )
        with self.assertRaises(findings_module.FindingsError):
            self.consume([
                self.record(1, "verification", "findings",
                            result_digest=sha256(_canonical_result(
                                "findings", findings=["fixture finding"]))),
            ])


# ---------------------------------------------------------------------------
# Payload schema, bounds, and determinism
# ---------------------------------------------------------------------------


class FindingsPayloadUnit(_FindingsBase):
    def _entry(self, **overrides) -> dict:
        entry = {
            "phase": "verification",
            "phase_base_commit": self.head,
            "outcome": "findings",
            "phase_tag": self.tag,
            "result_digest": sha256(b"x"),
            "receipt_path": "factory-findings-receipt-round-1-verification.json",
            "receipt_digest": sha256(b"r"),
            "findings": ["f"],
            "blocked_on": [],
            "gate_ran": True,
            "gate_exit": 0,
            "capability_ran": False,
            "capability_exit": None,
        }
        entry.update(overrides)
        return entry

    def test_payload_schema_validation(self) -> None:
        payload = {
            "schema": "factory-findings/v1",
            "campaign_id": "campaign",
            "source_round": 1,
            "entries": [self._entry()],
        }
        findings_module.validate_payload(payload)
        for mutate, fragment in (
            (lambda p: p.pop("entries"), "entries"),
            (lambda p: p.__setitem__("schema", "other"), "enum"),
            (lambda p: p.__setitem__("source_round", 0), "minimum"),
            (lambda p: p.__setitem__("extra", 1), "extra"),
            (lambda p: p["entries"].pop(), "minItems"),
            (lambda p: p["entries"][0].__setitem__("outcome", "pass"), "enum"),
            (lambda p: p["entries"][0].__setitem__("phase", "planning"), "enum"),
            (lambda p: p["entries"][0].__setitem__(
                "phase_base_commit", "x"), "pattern"),
            (lambda p: p["entries"][0].__setitem__("extra", 1), "extra"),
            (lambda p: p["entries"][0].__setitem__("gate_ran", 1), "type"),
            (lambda p: p["entries"][0].__setitem__("gate_exit", -1), "minimum"),
            (lambda p: p["entries"][0].__setitem__("capability_exit", "x"), "type"),
            (lambda p: p["entries"][0].pop("gate_ran"), "required"),
        ):
            clone = json.loads(json.dumps(payload))
            mutate(clone)
            with self.assertRaises(
                findings_module.FindingsError, msg=fragment):
                findings_module.validate_payload(clone)

    def test_build_payload_requires_entries(self) -> None:
        with self.assertRaises(findings_module.FindingsError):
            findings_module.build_payload(
                campaign_id="campaign", source_round=1, entries=[])
        with self.assertRaises(findings_module.FindingsError):
            findings_module.build_payload(
                campaign_id="campaign", source_round=0,
                entries=[{"phase": "verification"}])

    def test_payload_bytes_are_canonical_and_bounded(self) -> None:
        entry = self._entry()
        payload = findings_module.build_payload(
            campaign_id="campaign", source_round=1, entries=[entry])
        raw = findings_module.payload_bytes(payload)
        self.assertLess(len(raw), findings_module.MAX_PAYLOAD_BYTES)
        # Canonical sorted-key JSON, no wall-clock time, no prose claims.
        self.assertEqual(
            raw, json.dumps(payload, sort_keys=True,
                            separators=(",", ":")).encode("utf-8"))
        self.assertNotIn(b"timestamp", raw)
        self.assertNotIn(b"prose", raw)
        # The same bytes are produced deterministically.
        self.assertEqual(
            raw,
            findings_module.payload_bytes(findings_module.build_payload(
                campaign_id="campaign", source_round=1, entries=[entry])),
        )


# ---------------------------------------------------------------------------
# BLOCKER 4: round-1 authenticated readiness-findings channel
# ---------------------------------------------------------------------------


class ReadinessFindingsUnit(_FindingsBase):
    """BLOCKER 4: the round-1 authenticated readiness-findings channel.

    The trusted orchestrator mints one write-once readiness-findings
    artifact bound to the campaign id, the readiness nonce, the exact
    accepted commit, and the digest of the deterministic product-findings
    payload.  Consumption is durable one-use: every binding is re-validated
    against the committed schema and the caller's expected authority, and a
    genuine replay (a different campaign, nonce, or digest) fails closed
    while a byte-exact crash-window re-consume is accepted idempotently.
    A complete/pass readiness carries no findings (no artifact exists), and
    the round-1 planner must reflect the findings through the canonical
    plan authority before the attempt can be ``planned``.
    """

    NONCE = "0" * 64
    READINESS_ARTIFACT = "factory-readiness-findings.json"
    READINESS_CONSUMED = "factory-readiness-findings-consumed.json"

    def _payload(self, findings=None, blocked_on=None, **overrides) -> dict:
        entry = {
            "phase": "readiness",
            "outcome": "findings",
            "findings": (["readiness finding"]
                          if findings is None else list(findings)),
            "blocked_on": ([] if blocked_on is None else list(blocked_on)),
        }
        entry.update(overrides)
        return {
            "schema": "factory-findings/v1",
            "campaign_id": self.campaign_id,
            "source_round": 0,
            "entries": [entry],
        }

    def _payload_bytes(self, **overrides) -> bytes:
        return findings_module.payload_bytes(self._payload(**overrides))

    def _artifact(self, **overrides) -> dict:
        artifact = {
            "schema": "factory-readiness-findings/v1",
            "campaign_id": self.campaign_id,
            "readiness_nonce": self.NONCE,
            "accepted_commit": self.head,
            "payload_sha256": sha256(self._payload_bytes()),
            "payload": self._payload(),
        }
        artifact.update(overrides)
        return artifact

    def _mint(self, **overrides) -> str:
        params = dict(
            campaign_id=self.campaign_id,
            readiness_nonce=self.NONCE,
            accepted_commit=self.head,
            findings_payload=self._payload_bytes(),
        )
        params.update(overrides)
        return findings_module.mint_readiness_findings(self.root, **params)

    def _consume(self, **overrides):
        params = dict(
            campaign_id=self.campaign_id,
            readiness_nonce=self.NONCE,
            accepted_commit=self.head,
            expected_payload_sha256=sha256(self._payload_bytes()),
        )
        params.update(overrides)
        return findings_module.consume_readiness_findings(self.root, **params)

    def test_mint_rejects_unsafe_bindings(self) -> None:
        for kwargs, fragment in (
            ({"campaign_id": "not safe!"}, "campaign"),
            ({"campaign_id": ""}, "campaign"),
            ({"readiness_nonce": "beef"}, "nonce"),
            ({"readiness_nonce": "A" * 64}, "nonce"),
            ({"accepted_commit": "beef"}, "commit"),
            ({"accepted_commit": "A" * 40}, "commit"),
            ({"findings_payload": b"not json"}, "json"),
            ({"findings_payload": b"[]"}, "object"),
        ):
            with self.assertRaises(
                findings_module.FindingsError, msg=fragment):
                self._mint(**kwargs)

    def test_mint_rejects_non_readiness_payload(self) -> None:
        # A payload whose source is a verification/audit round (not round
        # zero) or whose entry is not the readiness phase fails closed.
        bad = self._payload()
        bad["source_round"] = 1
        with self.assertRaises(findings_module.FindingsError):
            self._mint(findings_payload=findings_module.payload_bytes(bad))
        bad = self._payload()
        bad["entries"][0]["phase"] = "verification"
        with self.assertRaises(findings_module.FindingsError):
            self._mint(findings_payload=findings_module.payload_bytes(bad))
        bad = self._payload()
        bad["entries"][0]["outcome"] = "pass"
        with self.assertRaises(findings_module.FindingsError):
            self._mint(findings_payload=findings_module.payload_bytes(bad))

    def test_valid_round1_consumption_is_durable_one_use(self) -> None:
        name = self._mint()
        self.assertEqual(name, self.READINESS_ARTIFACT)
        # The artifact binds every authority and is schema-conforming.
        artifact = json.loads(self._read_marker(self.READINESS_ARTIFACT))
        self.assertEqual(artifact["schema"], "factory-readiness-findings/v1")
        self.assertEqual(artifact["campaign_id"], self.campaign_id)
        self.assertEqual(artifact["readiness_nonce"], self.NONCE)
        self.assertEqual(artifact["accepted_commit"], self.head)
        self.assertEqual(
            artifact["payload_sha256"], sha256(self._payload_bytes()))
        # Consumption returns the exact deterministic product-findings bytes.
        self.assertEqual(self._consume(), self._payload_bytes())
        # The durable one-use marker is recorded.
        marker = json.loads(self._read_marker(self.READINESS_CONSUMED))
        self.assertEqual(
            marker["schema"], "factory-readiness-findings-consumed/v1")
        self.assertEqual(marker["campaign_id"], self.campaign_id)
        self.assertEqual(marker["readiness_nonce"], self.NONCE)
        self.assertEqual(
            marker["payload_sha256"], sha256(self._payload_bytes()))

    def test_byte_exact_reconsume_is_crash_recovery(self) -> None:
        # A byte-exact marker pre-planted is this run's own crashed consume;
        # the rerun accepts it idempotently and never wedges.
        self._mint()
        marker = {
            "schema": "factory-readiness-findings-consumed/v1",
            "campaign_id": self.campaign_id,
            "readiness_nonce": self.NONCE,
            "payload_sha256": sha256(self._payload_bytes()),
        }
        self._write_marker(
            self.READINESS_CONSUMED,
            findings_module.readiness_findings_consumed_bytes(marker),
        )
        self.assertEqual(self._consume(), self._payload_bytes())

    def test_replay_marker_with_different_bytes_fails_closed(self) -> None:
        # A genuine replay: the same artifact is consumed again but a marker
        # from a different run (different nonce/digest) is already recorded.
        # The consume passes every artifact binding, then the write-once
        # marker with different bytes fails the replay closed.
        self._mint()
        foreign_marker = {
            "schema": "factory-readiness-findings-consumed/v1",
            "campaign_id": self.campaign_id,
            "readiness_nonce": "1" * 64,
            "payload_sha256": sha256(b"other"),
        }
        self._write_marker(
            self.READINESS_CONSUMED,
            findings_module.readiness_findings_consumed_bytes(foreign_marker),
        )
        with self.assertRaises(findings_module.FindingsStaleError):
            self._consume()

    def test_wrong_campaign_fails_closed(self) -> None:
        self._mint()
        with self.assertRaises(findings_module.FindingsForeignError):
            self._consume(campaign_id="othercampaign")

    def test_wrong_readiness_nonce_fails_closed(self) -> None:
        self._mint()
        with self.assertRaises(findings_module.FindingsStaleError):
            self._consume(readiness_nonce="1" * 64)

    def test_wrong_accepted_commit_fails_closed(self) -> None:
        self._mint()
        with self.assertRaises(findings_module.FindingsStaleError):
            self._consume(accepted_commit="b" * 40)

    def test_wrong_payload_digest_fails_closed(self) -> None:
        self._mint()
        with self.assertRaises(findings_module.FindingsSyntheticError):
            self._consume(expected_payload_sha256=sha256(b"other"))

    def test_missing_artifact_with_nonzero_digest_fails_closed(self) -> None:
        # A readiness result that expected a nonzero product-findings digest
        # but left no artifact is a findings claim without evidence: it fails
        # closed instead of silently returning ``None``.
        with self.assertRaises(findings_module.FindingsReceiptError):
            self._consume()

    def test_missing_artifact_with_zero256_is_pass_readiness(self) -> None:
        # A complete/pass readiness carries no findings: the expected digest
        # is exactly ZERO256, no artifact exists, and consumption returns
        # ``None`` (no payload flows to the planner).
        self.assertIsNone(
            self._consume(expected_payload_sha256=findings_module.ZERO256))

    def test_malformed_artifact_fails_closed(self) -> None:
        # Invalid JSON.
        self._write_marker(self.READINESS_ARTIFACT, b"not json")
        with self.assertRaises(findings_module.FindingsMalformedError):
            self._consume()
        # Not an object.
        self._write_marker(self.READINESS_ARTIFACT, b"[]")
        with self.assertRaises(findings_module.FindingsMalformedError):
            self._consume()
        # Tampered payload digest (contradicts the stored payload bytes).
        self._write_marker(
            self.READINESS_ARTIFACT,
            findings_module.readiness_findings_bytes(
                self._artifact(payload_sha256="1" * 64)),
        )
        with self.assertRaises(findings_module.FindingsMalformedError):
            self._consume()
        # A foreign campaign id inside the artifact.
        self._write_marker(
            self.READINESS_ARTIFACT,
            findings_module.readiness_findings_bytes(
                self._artifact(campaign_id="othercampaign")),
        )
        with self.assertRaises(findings_module.FindingsForeignError):
            self._consume()

    def test_plan_must_reflect_findings(self) -> None:
        payload = self._payload_bytes()
        # A revised plan that reflects the finding in a task's structured
        # Scope field passes.
        plan = _reflection_plan(_reflection_tasks({
            1: {"scope": "initial scope statement. Revised: readiness finding."}}))
        findings_module.validate_plan_reflects_findings(plan, payload)
        # A plan that omits the finding fails closed.
        with self.assertRaises(findings_module.FindingsError):
            findings_module.validate_plan_reflects_findings(
                _reflection_plan(), payload)

    def test_plan_must_reflect_blocked_references(self) -> None:
        payload = self._payload_bytes(
            findings=[], blocked_on=[BLOCKER])
        # A blocked task naming the exact reference in its Blocked on field
        # reflects the blocker.
        plan = _reflection_plan(_reflection_tasks({
            1: {"status": "blocked", "blocked_on": BLOCKER}}))
        findings_module.validate_plan_reflects_findings(plan, payload)
        # A plan without the blocker fails closed.
        with self.assertRaises(findings_module.FindingsError):
            findings_module.validate_plan_reflects_findings(
                _reflection_plan(), payload)

    def test_plan_reflection_accepts_structured_task_fields(self) -> None:
        # Every legitimate structured carrier satisfies the backstop: Scope,
        # Acceptance criteria, Verification, and Evidence for findings, and
        # the Blocked on field for blockers.
        payload = self._payload_bytes()
        for field, value in (
            ("scope", "initial scope statement. readiness finding."),
            ("acceptance", "fixture acceptance gate passes. readiness finding."),
            ("verification", "`src/work-1.md` readiness finding"),
            ("evidence", "readiness finding evidence"),
        ):
            plan = _reflection_plan(_reflection_tasks({1: {field: value}}))
            findings_module.validate_plan_reflects_findings(plan, payload)

    def test_plan_reflection_rejects_findings_in_comments(self) -> None:
        # A finding hidden inside an HTML comment in a task field is not a
        # legitimate reflection: comments never satisfy the backstop.
        payload = self._payload_bytes()
        plan = _reflection_plan(_reflection_tasks({
            1: {"scope": "initial scope statement. <!-- readiness finding -->"}}))
        with self.assertRaises(findings_module.FindingsError):
            findings_module.validate_plan_reflects_findings(plan, payload)

    def test_plan_reflection_rejects_findings_in_metadata_prose(self) -> None:
        # A finding that appears only in unrelated canonical-section prose
        # (the Goal section) or the interaction inventory is not reflected in
        # any task's structured fields and must fail closed.
        payload = self._payload_bytes()
        plan = _reflection_plan()
        text = plan.decode("utf-8")
        text = text.replace(
            "Goal: exercise the Task 9 phase/campaign orchestrator "
            "deterministically.",
            "Goal: exercise the Task 9 phase/campaign orchestrator "
            "deterministically. The readiness finding was noted.",
        )
        plan_parser.parse_plan(text)  # still a valid plan
        with self.assertRaises(findings_module.FindingsError):
            findings_module.validate_plan_reflects_findings(
                text.encode("utf-8"), payload)
        # The interaction inventory is structured but is not a task field.
        text = plan.decode("utf-8")
        text = text.replace(
            "- input boundary: every role receives only the fresh "
            "allowlisted inputs.",
            "- input boundary: every role receives only the fresh "
            "allowlisted inputs. readiness finding",
        )
        plan_parser.parse_plan(text)
        with self.assertRaises(findings_module.FindingsError):
            findings_module.validate_plan_reflects_findings(
                text.encode("utf-8"), payload)

    def test_plan_reflection_rejects_substring_occurrences(self) -> None:
        # A finding that appears only as a substring of a larger token (here
        # the plural "readiness findings" inside a task's Scope) is not a
        # representation of the exact finding and must fail closed.
        payload = self._payload_bytes()
        plan = _reflection_plan(_reflection_tasks({
            1: {"scope": "initial scope statement. readiness findings noted."}}))
        with self.assertRaises(findings_module.FindingsError):
            findings_module.validate_plan_reflects_findings(plan, payload)

    def test_plan_reflection_rejects_findings_in_code_fences(self) -> None:
        # A finding hidden inside a fenced code block in a task field is not
        # a legitimate reflection: code fences never satisfy the backstop.
        payload = self._payload_bytes()
        plan = _reflection_plan()
        text = plan.decode("utf-8")
        text = text.replace(
            "- Verification: `src/work-1.md`",
            "- Verification: ```\n  readiness finding\n  ```",
        )
        plan_parser.parse_plan(text)  # still a valid plan
        with self.assertRaises(findings_module.FindingsError):
            findings_module.validate_plan_reflects_findings(
                text.encode("utf-8"), payload)

    def test_plan_reflection_rejects_malformed_inputs(self) -> None:
        with self.assertRaises(findings_module.FindingsError):
            findings_module.validate_plan_reflects_findings(
                b"x" * (findings_module.MAX_PLAN_BYTES + 1),
                self._payload_bytes())
        with self.assertRaises(findings_module.FindingsError):
            findings_module.validate_plan_reflects_findings(
                b"plan", b"not json")
        # A plan that is not a valid factory-plan/v1 document fails closed:
        # reflection is only ever judged against the parsed task model, so a
        # raw-byte substring in arbitrary text can never satisfy it.
        with self.assertRaises(findings_module.FindingsError):
            findings_module.validate_plan_reflects_findings(
                b"revised plan: readiness finding", self._payload_bytes())


# ---------------------------------------------------------------------------
# B5: trusted coordinator projection of the validated findings aggregate
# ---------------------------------------------------------------------------


class RunnerFindingsProjectionUnit(unittest.TestCase):
    """B5: the pure coordinator projection of the validated signed findings
    aggregate into deterministic bounded ``factory-findings/v1`` readiness
    entries.

    The projector accepts the exact checker-validated aggregate bytes plus
    the expected aggregate digest and the exact expected campaign / readiness
    nonce / commit / tree / environment / declaration / contract bindings,
    re-validates the strict committed schema and every field, rejects prose /
    skips / simulation / mismatch, and returns deterministic fixed-semantic
    per-runner / per-capability payload entries.  These are pure unit tests:
    no Git, no campaign, no runner — only the projector and its bindings.
    """

    CAMPAIGN = "campaign"
    NONCE = "0" * 64
    COMMIT = "a" * 40
    TREE = "b" * 40
    ENV = "c" * 40
    CONTRACTS_SHA = "d" * 64
    RUNNER = "dev-runner-vm"
    CAPABILITY = "remote-project-gate"
    CONTRACT_RUNNER = "dev-runner-vm"

    def _contracts(self) -> dict:
        return {
            "schema": "ralph-capability-contract/v2",
            "capabilities": [
                {
                    "name": self.CAPABILITY,
                    "status": "declared",
                    "runner_class": self.CONTRACT_RUNNER,
                    "must_execute": True,
                    "candidate_probe_argv": ["./scripts/verify-project.sh"],
                }
            ],
        }

    def _declarations(self) -> dict:
        return {self.RUNNER: [self.CAPABILITY]}

    def _archive_bindings(self, **overrides) -> dict:
        binding = {
            "archive_sha256": "e" * 64,
            "authority_sha256": "f" * 64,
            "nonce": self.NONCE,
            "skip_marker_detected": False,
            "cleanup": True,
            "timed_out": False,
        }
        binding.update(overrides)
        return {self.RUNNER: binding}

    def _probe(self, exit_code: int, capability: str | None = None) -> dict:
        return {
            "capability": capability or self.CAPABILITY,
            "exit_code": exit_code,
            "timed_out": False,
        }

    def _runner(self, *, result: str = "findings",
                exit_code: int | None = 1, **overrides) -> dict:
        record = {
            "name": self.RUNNER,
            "manifest": (
                ".factory-state/runner-evidence/"
                f"{self.CAMPAIGN}/{self.NONCE}/{self.RUNNER}/"
                f"{self.COMMIT}/{self.NONCE}/manifest.json"
            ),
            "manifest_sha256": "1" * 64,
            "result": result,
            "capabilities": [self.CAPABILITY],
            "probes": [self._probe(exit_code)],
            "artifact_manifest_sha256": "2" * 64,
            "artifact_count": 1,
            "artifact_bytes": 1024,
            "signer": {
                "principal": self.RUNNER,
                "key_sha256": "3" * 64,
                "algorithm": "ssh-ed25519",
                "signature_sha256": "4" * 64,
            },
        }
        record.update(overrides)
        return record

    def _aggregate(self, runners=None, **overrides) -> dict:
        aggregate = {
            "schema": "factory-runner-findings-aggregate/v1",
            "campaign_id": self.CAMPAIGN,
            "readiness_nonce": self.NONCE,
            "commit": self.COMMIT,
            "tree": self.TREE,
            "environment_blob": self.ENV,
            "runners": runners if runners is not None else [self._runner()],
        }
        aggregate.update(overrides)
        return aggregate

    def _bytes(self, aggregate: dict) -> tuple[bytes, str]:
        raw = json.dumps(
            aggregate, sort_keys=True, separators=(",", ":")).encode("utf-8")
        return raw, findings_module.sha256(raw)

    def _project(self, aggregate: dict | None = None, **overrides):
        agg = aggregate if aggregate is not None else self._aggregate()
        raw, digest = self._bytes(agg)
        params = dict(
            aggregate_bytes=raw,
            aggregate_sha256=digest,
            campaign_id=self.CAMPAIGN,
            readiness_nonce=self.NONCE,
            commit=self.COMMIT,
            tree=self.TREE,
            environment_blob=self.ENV,
            contracts_sha256=self.CONTRACTS_SHA,
            declarations=self._declarations(),
            contracts=self._contracts(),
            archive_bindings=self._archive_bindings(),
        )
        params.update(overrides)
        return findings_module.project_runner_findings(**params)

    def test_positive_projection_is_deterministic_and_bounded(self) -> None:
        aggregate = self._aggregate([self._runner(exit_code=8)])
        raw, digest = self._bytes(aggregate)
        codes, verdicts = self._project(aggregate=aggregate)
        # Exactly one fixed-semantic code per non-pass probe.
        self.assertEqual(
            codes,
            (f"{findings_module.RUNNER_FINDINGS_CODE}:"
             f"{self.RUNNER}:{self.CAPABILITY}:8",),
        )
        self.assertEqual(len(verdicts), 1)
        verdict = verdicts[0]
        self.assertEqual(verdict["code"], findings_module.RUNNER_FINDINGS_CODE)
        self.assertEqual(verdict["runner"], self.RUNNER)
        self.assertEqual(verdict["capability"], self.CAPABILITY)
        self.assertEqual(verdict["exit_code"], 8)
        # Exact binding echo: campaign/nonce/commit/tree/env/contract/digest
        # and the per-record archive/authority/nonce + aggregate digest.
        self.assertEqual(verdict["campaign_id"], self.CAMPAIGN)
        self.assertEqual(verdict["readiness_nonce"], self.NONCE)
        self.assertEqual(verdict["commit"], self.COMMIT)
        self.assertEqual(verdict["tree"], self.TREE)
        self.assertEqual(verdict["environment_blob"], self.ENV)
        self.assertEqual(verdict["contracts_sha256"], self.CONTRACTS_SHA)
        self.assertEqual(verdict["aggregate_sha256"], digest)
        self.assertEqual(verdict["archive_sha256"], "e" * 64)
        self.assertEqual(verdict["authority_sha256"], "f" * 64)
        self.assertEqual(verdict["nonce"], self.NONCE)
        # Deterministic: projecting the same bytes yields identical output.
        codes2, verdicts2 = self._project(aggregate=aggregate)
        self.assertEqual(codes2, codes)
        self.assertEqual(verdicts2, verdicts)
        # No arbitrary prose / wall-clock time anywhere in the projection.
        blob = json.dumps(verdicts).encode("utf-8")
        self.assertNotIn(b"stdout", blob)
        self.assertNotIn(b"log", blob.lower())
        self.assertNotIn(b"timestamp", blob)

    def test_projects_only_nonpass_probes_of_findings_records(self) -> None:
        # A findings record with a non-pass and a pass probe projects exactly
        # the non-pass probe; a pass-only probe set contributes nothing.
        record = self._runner(exit_code=7)
        record["capabilities"] = [self.CAPABILITY, "systemd-user"]
        record["probes"] = [self._probe(7), self._probe(0, "systemd-user")]
        contracts = self._contracts()
        contracts["capabilities"].append({
            "name": "systemd-user", "status": "declared",
            "runner_class": self.RUNNER, "must_execute": True,
            "candidate_probe_argv": ["/usr/bin/systemd-run"],
        })
        declarations = {self.RUNNER: [self.CAPABILITY, "systemd-user"]}
        agg = self._aggregate([record])
        raw, digest = self._bytes(agg)
        codes, verdicts = self._project(
            aggregate=agg,
            declarations=declarations, contracts=contracts,
        )
        self.assertEqual(len(codes), 1)
        self.assertEqual(len(verdicts), 1)
        self.assertEqual(verdicts[0]["capability"], self.CAPABILITY)
        self.assertEqual(verdicts[0]["exit_code"], 7)

    def test_build_runner_readiness_payload_is_schema_conforming(self) -> None:
        _, verdicts = self._project()
        payload = findings_module.build_runner_readiness_findings_payload(
            campaign_id=self.CAMPAIGN,
            code_strings=[
                f"{findings_module.RUNNER_FINDINGS_CODE}:"
                f"{self.RUNNER}:{self.CAPABILITY}:8"
            ],
        )
        self.assertEqual(payload["schema"], "factory-findings/v1")
        self.assertEqual(payload["source_round"], 0)
        self.assertEqual(payload["entries"][0]["phase"], "readiness")
        self.assertEqual(payload["entries"][0]["outcome"], "findings")
        # The readiness payload validator accepts it.
        findings_module.validate_readiness_payload(payload)
        # Prose is never smuggled into the findings list.
        self.assertEqual(
            payload["entries"][0]["findings"],
            [f"{findings_module.RUNNER_FINDINGS_CODE}:"
             f"{self.RUNNER}:{self.CAPABILITY}:8"],
        )

    # -- fail-closed: digest / schema / binding mismatch --------------------

    def test_tampered_aggregate_digest_fails_closed(self) -> None:
        raw, digest = self._bytes(self._aggregate())
        with self.assertRaises(findings_module.FindingsSyntheticError):
            findings_module.project_runner_findings(
                aggregate_bytes=raw,
                aggregate_sha256=findings_module.sha256(b"other"),
                campaign_id=self.CAMPAIGN, readiness_nonce=self.NONCE,
                commit=self.COMMIT, tree=self.TREE,
                environment_blob=self.ENV,
                contracts_sha256=self.CONTRACTS_SHA,
                declarations=self._declarations(),
                contracts=self._contracts(),
                archive_bindings=self._archive_bindings(),
            )

    def test_wrong_campaign_fails_closed(self) -> None:
        with self.assertRaises(findings_module.FindingsForeignError):
            self._project(aggregate=self._aggregate(
                campaign_id="other-campaign"))

    def test_wrong_readiness_nonce_fails_closed(self) -> None:
        with self.assertRaises(findings_module.FindingsStaleError):
            self._project(aggregate=self._aggregate(
                readiness_nonce="1" * 64))

    def test_wrong_commit_fails_closed(self) -> None:
        with self.assertRaises(findings_module.FindingsStaleError):
            self._project(aggregate=self._aggregate(commit="b" * 40))

    def test_wrong_tree_fails_closed(self) -> None:
        with self.assertRaises(findings_module.FindingsStaleError):
            self._project(aggregate=self._aggregate(tree="c" * 40))

    def test_wrong_environment_blob_fails_closed(self) -> None:
        with self.assertRaises(findings_module.FindingsStaleError):
            self._project(aggregate=self._aggregate(
                environment_blob="d" * 40))

    def test_stale_archive_nonce_mismatch_fails_closed(self) -> None:
        # The archive binding's nonce must equal the nonce embedded in the
        # manifest path; a replayed/foreign receipt fails closed.
        with self.assertRaises(findings_module.FindingsSyntheticError):
            self._project(archive_bindings=self._archive_bindings(
                nonce="9" * 64))

    def test_manifest_path_escape_fails_closed(self) -> None:
        # A schema-valid manifest path whose campaign segment escapes the
        # expected namespace fails the projection closed.
        record = self._runner(exit_code=8)
        record["manifest"] = (
            ".factory-state/runner-evidence/other/"
            f"{self.NONCE}/{self.RUNNER}/{self.COMMIT}/{self.NONCE}/manifest.json"
        )
        with self.assertRaises(findings_module.FindingsSyntheticError):
            self._project(aggregate=self._aggregate([record]))

    def test_schema_violating_aggregate_fails_closed(self) -> None:
        agg = self._aggregate()
        agg["runners"][0].pop("probes")
        raw, digest = self._bytes(agg)
        params = dict(
            campaign_id=self.CAMPAIGN, readiness_nonce=self.NONCE,
            commit=self.COMMIT, tree=self.TREE, environment_blob=self.ENV,
            contracts_sha256=self.CONTRACTS_SHA,
            declarations=self._declarations(), contracts=self._contracts(),
            archive_bindings=self._archive_bindings(),
        )
        with self.assertRaises(findings_module.FindingsMalformedError):
            findings_module.project_runner_findings(
                aggregate_bytes=raw, aggregate_sha256=digest, **params)
        # Unknown extra field.
        agg = self._aggregate()
        agg["extra"] = 1
        raw, digest = self._bytes(agg)
        with self.assertRaises(findings_module.FindingsMalformedError):
            findings_module.project_runner_findings(
                aggregate_bytes=raw, aggregate_sha256=digest, **params)

    def test_non_json_or_non_object_aggregate_fails_closed(self) -> None:
        params = dict(
            campaign_id=self.CAMPAIGN, readiness_nonce=self.NONCE,
            commit=self.COMMIT, tree=self.TREE, environment_blob=self.ENV,
            contracts_sha256=self.CONTRACTS_SHA,
            declarations=self._declarations(), contracts=self._contracts(),
            archive_bindings=self._archive_bindings(),
        )
        with self.assertRaises(findings_module.FindingsMalformedError):
            findings_module.project_runner_findings(
                aggregate_bytes=b"{not json",
                aggregate_sha256=findings_module.sha256(b"{not json"),
                **params)
        with self.assertRaises(findings_module.FindingsMalformedError):
            findings_module.project_runner_findings(
                aggregate_bytes=findings_module.sha256(b"[]").encode("utf-8"),
                aggregate_sha256=findings_module.sha256(
                    findings_module.sha256(b"[]").encode("utf-8")),
                **params)

    def test_oversized_aggregate_fails_closed(self) -> None:
        raw = b"{" + b"x" * (findings_module.MAX_READINESS_FINDINGS_BYTES + 1)
        with self.assertRaises(findings_module.FindingsMalformedError):
            findings_module.project_runner_findings(
                aggregate_bytes=raw,
                aggregate_sha256=findings_module.sha256(raw),
                campaign_id=self.CAMPAIGN, readiness_nonce=self.NONCE,
                commit=self.COMMIT, tree=self.TREE,
                environment_blob=self.ENV,
                contracts_sha256=self.CONTRACTS_SHA,
                declarations=self._declarations(),
                contracts=self._contracts(),
                archive_bindings=self._archive_bindings(),
            )

    # -- fail-closed: prose / skip / simulation / mismatch ------------------

    def test_pass_only_aggregate_fails_closed(self) -> None:
        # A findings aggregate must carry at least one result=findings record.
        with self.assertRaises(findings_module.FindingsSyntheticError):
            self._project(aggregate=self._aggregate(
                [self._runner(result="pass", exit_code=0, probes=[])]))

    def test_findings_record_without_nonpass_probe_fails_closed(self) -> None:
        # A findings record whose probe set is all pass is synthetic.
        with self.assertRaises(findings_module.FindingsSyntheticError):
            self._project(aggregate=self._aggregate(
                [self._runner(result="findings", exit_code=0)]))

    def test_undeclared_runner_fails_closed(self) -> None:
        with self.assertRaises(findings_module.FindingsSyntheticError):
            self._project(
                declarations={"other-runner": [self.CAPABILITY]})

    def test_declared_capability_mismatch_fails_closed(self) -> None:
        # The aggregate record claims a capability set that differs from the
        # committed environment declaration for that runner.
        with self.assertRaises(findings_module.FindingsSyntheticError):
            self._project(declarations={self.RUNNER: ["systemd-user"]})

    def test_missing_archive_binding_fails_closed(self) -> None:
        with self.assertRaises(findings_module.FindingsReceiptError):
            self._project(archive_bindings={})

    def test_skip_marker_fails_closed(self) -> None:
        with self.assertRaises(findings_module.FindingsSyntheticError):
            self._project(archive_bindings=self._archive_bindings(
                skip_marker_detected=True))

    def test_unproven_cleanup_fails_closed(self) -> None:
        with self.assertRaises(findings_module.FindingsSyntheticError):
            self._project(archive_bindings=self._archive_bindings(
                cleanup=False))

    def test_timed_out_probe_fails_closed(self) -> None:
        with self.assertRaises(findings_module.FindingsSyntheticError):
            self._project(archive_bindings=self._archive_bindings(
                timed_out=True))

    def test_undeclared_capability_contract_fails_closed(self) -> None:
        # The projected capability has no committed capability contract.
        contracts = self._contracts()
        contracts["capabilities"] = []
        with self.assertRaises(findings_module.FindingsSyntheticError):
            self._project(contracts=contracts)

    def test_candidate_capability_contract_fails_closed(self) -> None:
        contracts = self._contracts()
        contracts["capabilities"][0]["status"] = "candidate"
        with self.assertRaises(findings_module.FindingsSyntheticError):
            self._project(contracts=contracts)

    def test_contract_runner_class_mismatch_fails_closed(self) -> None:
        contracts = self._contracts()
        contracts["capabilities"][0]["runner_class"] = "gpurunner"
        with self.assertRaises(findings_module.FindingsSyntheticError):
            self._project(contracts=contracts)

    def test_contract_not_must_execute_fails_closed(self) -> None:
        contracts = self._contracts()
        contracts["capabilities"][0]["must_execute"] = False
        with self.assertRaises(findings_module.FindingsSyntheticError):
            self._project(contracts=contracts)

    def test_fixture_probe_argv_fails_closed(self) -> None:
        contracts = self._contracts()
        contracts["capabilities"][0]["candidate_probe_argv"] = [
            "./scripts/x.sh", "--fixture-dir"]
        with self.assertRaises(findings_module.FindingsSyntheticError):
            self._project(contracts=contracts)

    def test_untrusted_code_rejected(self) -> None:
        with self.assertRaises(findings_module.FindingsError):
            findings_module.validate_runner_findings_code(
                "arbitrary prose describing a defect")
        with self.assertRaises(findings_module.FindingsError):
            findings_module.validate_runner_findings_code(
                f"{findings_module.RUNNER_FINDINGS_CODE}:bad!runner:cap:1")

    def test_build_payload_rejects_untrusted_code(self) -> None:
        with self.assertRaises(findings_module.FindingsError):
            findings_module.build_runner_readiness_findings_payload(
                campaign_id=self.CAMPAIGN, code_strings=["free prose"])

    def test_build_payload_requires_code_strings(self) -> None:
        with self.assertRaises(findings_module.FindingsError):
            findings_module.build_runner_readiness_findings_payload(
                campaign_id=self.CAMPAIGN, code_strings=[])

    def test_four_controller_capability_projection(self) -> None:
        # A realistic four-capability runner finding projects one fixed code
        # and structured verdict per failing capability, never free prose.
        capabilities = [
            "controller-production-routing", "target-consumer",
            "physical-controller", "inputplumber-system-dbus",
        ]
        record = {
            "name": "iprunner",
            "manifest": (
                ".factory-state/runner-evidence/"
                f"{self.CAMPAIGN}/{self.NONCE}/iprunner/"
                f"{self.COMMIT}/{self.NONCE}/manifest.json"
            ),
            "manifest_sha256": "1" * 64,
            "result": "findings",
            "capabilities": sorted(capabilities),
            "probes": [
                {"capability": c, "exit_code": 1, "timed_out": False}
                for c in capabilities
            ],
            "artifact_manifest_sha256": "2" * 64,
            "artifact_count": 1,
            "artifact_bytes": 1024,
            "signer": {
                "principal": "iprunner", "key_sha256": "3" * 64,
                "algorithm": "ssh-ed25519", "signature_sha256": "4" * 64,
            },
        }
        contracts = {"capabilities": [
            {"name": c, "status": "declared", "runner_class": "iprunner",
             "must_execute": True,
             "candidate_probe_argv": ["nix-shell", "--run", "x.sh"]}
            for c in capabilities
        ]}
        declarations = {"iprunner": sorted(capabilities)}
        agg = self._aggregate([record], readiness_nonce=self.NONCE)
        raw, digest = self._bytes(agg)
        archive = {"iprunner": {
            "archive_sha256": "e" * 64, "authority_sha256": "f" * 64,
            "nonce": self.NONCE, "skip_marker_detected": False,
            "cleanup": True, "timed_out": False,
        }}
        codes, verdicts = self._project(
            aggregate=agg, declarations=declarations, contracts=contracts,
            archive_bindings=archive,
        )
        self.assertEqual(len(codes), len(capabilities))
        self.assertEqual(len(verdicts), len(capabilities))
        self.assertEqual(
            {v["capability"] for v in verdicts}, set(capabilities))
        self.assertEqual({v["runner"] for v in verdicts}, {"iprunner"})
        for v in verdicts:
            self.assertEqual(v["exit_code"], 1)
            self.assertEqual(v["code"], findings_module.RUNNER_FINDINGS_CODE)
            self.assertEqual(v["aggregate_sha256"], digest)


# ---------------------------------------------------------------------------
# Launch authority: planner-only digest-bound payload, never other roles
# ---------------------------------------------------------------------------


class FindingsLaunchUnit(unittest.TestCase):
    """§16: the findings payload is a digest-bound planner-only input."""

    def _make(self, role: str = "planner", **kwargs):
        role_bytes = b"role prompt"
        agents = b"AGENTS.md"
        spec = b"spec"
        plan = b"plan"
        binding = launch_module.InvocationBinding(
            role=role,
            model="synthetic-model",
            provider="synthetic",
            backend=Path(TRUE_EXECUTABLE),
            workspace=Path("/tmp"),
            bound_commit="a" * 40,
            role_prompt_digest=sha256(role_bytes),
            prompt_set_digest=sha256(b"set"),
            plan_digest=sha256(plan),
            policy_digest=sha256(agents),
            specification_digest=sha256(spec),
            allowed_tools=("bash",),
            runtime_limit=60.0,
            inactivity_limit=30.0,
            **kwargs,
        )
        return binding, role_bytes, agents, spec, plan

    def test_planner_findings_digest_binds_exact_bytes(self) -> None:
        findings = findings_module.payload_bytes({
            "schema": "factory-findings/v1", "campaign_id": "campaign",
            "source_round": 1, "entries": [],
        })
        binding, role_bytes, agents, spec, plan = self._make(
            findings_digest=sha256(findings))
        prompt = launch_module.compose_prompt(
            binding, role_prompt=role_bytes, agents=agents, spec=spec,
            plan=plan, findings=findings,
        )
        self.assertIn(b"## Findings from the previous round", prompt)
        self.assertIn(sha256(findings).encode("ascii"), prompt)
        self.assertIn(findings, prompt)
        # The prompt is deterministic: same inputs, same bytes.
        again = launch_module.compose_prompt(
            binding, role_prompt=role_bytes, agents=agents, spec=spec,
            plan=plan, findings=findings,
        )
        self.assertEqual(again, prompt)

    def test_planner_substituted_findings_fail_closed(self) -> None:
        findings = findings_module.payload_bytes({
            "schema": "factory-findings/v1", "campaign_id": "campaign",
            "source_round": 1, "entries": [],
        })
        binding, role_bytes, agents, spec, plan = self._make(
            findings_digest=sha256(findings))
        with self.assertRaises(launch_module.InvocationError):
            launch_module.compose_prompt(
                binding, role_prompt=role_bytes, agents=agents, spec=spec,
                plan=plan, findings=b"paraphrased findings",
            )

    def test_findings_without_digest_fail_closed(self) -> None:
        binding, role_bytes, agents, spec, plan = self._make()
        with self.assertRaises(launch_module.InvocationError):
            launch_module.compose_prompt(
                binding, role_prompt=role_bytes, agents=agents, spec=spec,
                plan=plan, findings=b"payload",
            )

    def test_malformed_findings_digest_fails_closed(self) -> None:
        for digest in ("short", "Z" * 64):
            binding, _, _, _, _ = self._make(findings_digest=digest)
            with self.assertRaises(launch_module.InvocationError):
                launch_module.verify_invocation(binding)

    def test_non_planner_findings_fail_closed(self) -> None:
        findings = b"payload"
        for role in ("developer", "tester", "auditor"):
            kwargs = {}
            if role == "developer":
                kwargs.update(task_id=1, task_excerpt_digest=sha256(b"task"))
            if role == "auditor":
                kwargs.update(audit_objective_digest=sha256(b"obj"))
            binding, _, _, _, _ = self._make(
                role=role, findings_digest=sha256(findings), **kwargs)
            with self.assertRaises(launch_module.InvocationError, msg=role):
                launch_module.verify_invocation(binding)

    def test_compose_prompt_rejects_findings_for_non_planner(self) -> None:
        findings = b"payload"
        binding, role_bytes, agents, spec, plan = self._make(
            role="tester", findings_digest=sha256(findings))
        with self.assertRaises(launch_module.InvocationError):
            launch_module.compose_prompt(
                binding, role_prompt=role_bytes, agents=agents, spec=spec,
                plan=plan, findings=findings,
            )


# ---------------------------------------------------------------------------
# Production launch: exact payload digest binding (planner)
# ---------------------------------------------------------------------------


class FindingsProductionLaunch(unittest.TestCase):
    """B2: the production planner launch binds the exact payload digest."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="factory-findings-launch."))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def _production_config(self, ws):
        # The real (non-driver) launch path requires a committed absolute
        # backend; the fixture driver file is an existing committed regular
        # file, and authorize_launch is mocked so no process is spawned.
        return dataclasses.replace(
            ws.derive_config(), backend=str(ws.root / DRIVER_REL))

    def test_production_planner_binds_exact_findings_digest(self) -> None:
        ws = FixtureWorkspace(self.tmp / "ws1", scenario=SUCCESS_SCENARIO)
        ws.commit_scenario()
        config = self._production_config(ws)
        head = _git(ws.root, "rev-parse", "HEAD").stdout.strip()
        findings = findings_module.payload_bytes({
            "schema": "factory-findings/v1", "campaign_id": "campaign",
            "source_round": 1, "entries": [],
        })
        captured: dict = {}

        def _authorize(binding, **kwargs):
            captured["findings"] = kwargs.get("findings")
            captured["findings_digest"] = binding.findings_digest
            return object()

        class _Supervisor:
            def __init__(self, binding):
                self.binding = binding

            def run(self, authority):
                return type("_Result", (), {"outcome": "exited",
                                            "returncode": 0})()

        with unittest.mock.patch.object(
            campaign_module.launch_module, "authorize_launch",
            side_effect=_authorize,
        ), unittest.mock.patch.object(
            campaign_module.launch_module, "LaunchSupervision",
            side_effect=_Supervisor,
        ):
            outcome = campaign_module.launch_role_attempt(
                config, role="planner", head=head, round_number=2,
                findings_payload=findings,
            )
        self.assertEqual(outcome.exit_status, 0)
        self.assertFalse(outcome.interrupted)
        self.assertEqual(captured["findings"], findings)
        self.assertEqual(captured["findings_digest"], sha256(findings))

    def test_production_launch_refuses_findings_for_developer(self) -> None:
        ws = FixtureWorkspace(self.tmp / "ws2", scenario=SUCCESS_SCENARIO)
        ws.commit_scenario()
        config = self._production_config(ws)
        head = _git(ws.root, "rev-parse", "HEAD").stdout.strip()
        with self.assertRaises(campaign_module.CampaignPhaseError):
            campaign_module.launch_role_attempt(
                config, role="developer", head=head, task_id=1,
                findings_payload=b"payload",
            )


# ---------------------------------------------------------------------------
# Positive end-to-end: findings -> receipt -> payload -> revised plan -> dev
# ---------------------------------------------------------------------------


class FindingsWorkspace(FixtureWorkspace):
    """Fixture workspace that also commits the findings-revised templates."""

    FINDING_MARKER = "fixture finding addressed"
    BLOCKER = "external-capability-required"

    def _generate_plans(self, common: dict) -> None:
        super()._generate_plans(common)
        ws = self.root
        # Round N (>= 2) findings-revised templates: tasks before N stay
        # complete, the runnable task N carries the accepted finding as a
        # scope revision, and the final audit task stays pending.
        for round_no in (2, 3):
            revised = [
                {
                    **t,
                    "scope": (
                        t.get("scope", "fixture-scoped work only.")
                        + (f" Revised after findings: {self.FINDING_MARKER}."
                           if t["number"] == round_no else "")
                    ),
                    "status": "complete" if t["number"] < round_no else t["status"],
                }
                for t in TASK_SPECS
            ]
            gen_plan(
                ws, common,
                f"fixture/templates/planner-findings-revised-{round_no}.md",
                revised,
            )
        # External-blocker variant: the planner keeps the unavailable
        # capability explicit as a blocked task with the exact reference
        # while the next runnable task carries the accepted finding.
        for round_no in (2, 3):
            blocked_revised = [
                {
                    **t,
                    "scope": (
                        t.get("scope", "fixture-scoped work only.")
                        + (f" | Revised after findings: {self.FINDING_MARKER}."
                           if t["number"] == round_no else "")
                    ),
                    "status": (
                        "blocked" if t["number"] == 1
                        else "complete" if t["number"] < round_no
                        else t["status"]
                    ),
                    "blocked_on": (
                        self.BLOCKER if t["number"] == 1
                        else t.get("blocked_on")
                    ),
                }
                for t in TASK_SPECS
            ]
            gen_plan(
                ws, common,
                f"fixture/templates/planner-findings-blocked-revised-{round_no}.md",
                blocked_revised,
            )


class _FindingsCampaign(unittest.TestCase):
    """Shared campaign-flow helpers (one fresh workspace per test)."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="factory-findings-campaign."))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self._workspace_count = 0

    def make(self, scenario: dict, **kwargs) -> FindingsWorkspace:
        self._workspace_count += 1
        ws = FindingsWorkspace(
            self.tmp / f"ws{self._workspace_count}",
            scenario=scenario, **kwargs,
        )
        ws.commit_scenario()
        return ws

    @staticmethod
    def _phase_record(data: dict, round_no: int, phase: str) -> dict:
        return next(
            r for r in data["phase_history"]
            if r["round"] == round_no and r["phase"] == phase
        )

    def _receipt(self, ws, round_no: int, phase: str) -> dict:
        raw = (ws.root / STATE_DIR /
               f"factory-findings-receipt-round-{round_no}-{phase}.json").read_bytes()
        return json.loads(raw.decode("utf-8"))

    def _ledger_tags(self, ws) -> list[str]:
        raw = (ws.root / STATE_DIR / state_module.DIGEST_LEDGER_NAME).read_text()
        return [
            json.loads(line)["tag"]
            for line in raw.splitlines() if line.strip()
        ]

    def _plan_at_commit(self, ws, subject: str) -> bytes:
        lines = _git(
            ws.root, "log", "--format=%H %s", "--all").stdout.strip().splitlines()
        for line in lines:
            commit, sep, rest = line.partition(" ")
            if sep and rest == subject:
                return _git(
                    ws.root, "show",
                    f"{commit}:{PLAN_REL}").stdout.encode("utf-8")
        self.fail(f"no commit with subject {subject!r}")

    def _write_marker(self, ws, name: str, data: bytes) -> None:
        directory = ws.root / STATE_DIR
        directory.mkdir(mode=0o700, exist_ok=True)
        path = directory / name
        path.write_bytes(data)
        os.chmod(path, 0o600)


class FindingsCampaignFlow(_FindingsCampaign):
    """§16 positive end-to-end flows."""

    def test_tester_findings_flow_to_next_planner_revision(self) -> None:
        # Round 1 tester findings -> orchestrator receipt -> round 2 planner
        # receives the deterministic payload and revises the canonical plan
        # -> only then the developer runs (the revised task).
        ws = self.make({
            "planner": {"behavior": {"1": "planned", "2": "findings-revised",
                                     "default": "planned"}},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": {"1": "findings", "2": "pass",
                                    "default": "pass"}},
            "auditor": {"behavior": "pass"},
        }, rounds=2)
        rc, data = ws.run_cli()
        self.assertEqual(rc, 0)
        assert_terminal(self, data, terminal_phase="success",
                        terminal_outcome="pass", exit_code=0,
                        rounds_completed=2)
        assert_history(self, data, [
            (1, "planning", "planned"),
            (1, "implementation", "task_completed"),
            (1, "verification", "findings"),
            (1, "audit", "pass"),
            (2, "planning", "planned"),
            (2, "implementation", "task_completed"),
            (2, "verification", "pass"),
            (2, "audit", "pass"),
        ])
        # Exactly one receipt: round 1 verification (audit passed).
        self.assertTrue((ws.root / receipt_rel(1, "verification")).exists())
        self.assertFalse((ws.root / receipt_rel(1, "audit")).exists())
        receipt = self._receipt(ws, 1, "verification")
        self.assertEqual(receipt["campaign_id"], "campaign")
        self.assertEqual(receipt["round"], 1)
        self.assertEqual(receipt["phase"], "verification")
        self.assertEqual(receipt["outcome"], "findings")
        record = self._phase_record(data, 1, "verification")
        # Exact commit binding: the receipt's phase base equals the recorded
        # head of the phase it ran at, and the tag is in the digest ledger.
        self.assertEqual(receipt["phase_base_commit"], record["head_commit"])
        self.assertIn(receipt["phase_tag"], self._ledger_tags(ws))
        # Exact result binding: the receipt's result digest equals the digest
        # of the exact structured bytes the tester wrote and equals the
        # recorded phase result digest.
        expected = _canonical_result("findings", findings=["fixture finding"])
        self.assertEqual(receipt["result_digest"], sha256(expected))
        self.assertEqual(record["result_digest"], receipt["result_digest"])
        self.assertEqual(receipt["findings"], ["fixture finding"])
        self.assertEqual(receipt["gate_ran"], True)
        self.assertEqual(receipt["gate_exit"], 0)
        # The revised round-2 plan incorporates the finding as a revised task
        # before the developer worked task 2.
        revised_plan = self._plan_at_commit(
            ws, "factory-campaign: planning round 2")
        plan = plan_parser.Plan.from_bytes(revised_plan)
        task2 = next(t for t in plan.tasks if t.number == 2)
        self.assertEqual(task2.status, "pending")
        self.assertIn(FindingsWorkspace.FINDING_MARKER, task2.fields["Scope"])
        # The developer only ran after the revision: in newest-first git log
        # the task-2 completion commit appears before the round-2 planning
        # commit (the planning commit is older).
        log = _git(
            ws.root, "log", "--format=%s", "--all").stdout.strip().splitlines()
        self.assertLess(
            log.index("factory-campaign: task 2 complete"),
            log.index("factory-campaign: planning round 2"),
        )
        # The payload is evidence-only: no runtime payload file is ever
        # persisted (it flows in-memory to the planner prompt).
        names = [p.name for p in (ws.root / STATE_DIR).iterdir()]
        self.assertFalse(any(n.startswith("factory-findings-payload") for n in names))

    def test_auditor_findings_flow(self) -> None:
        ws = self.make({
            "planner": {"behavior": {"1": "planned", "2": "findings-revised",
                                     "default": "planned"}},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": {"1": "findings", "2": "pass",
                                     "default": "pass"}},
        }, rounds=2)
        rc, data = ws.run_cli()
        self.assertEqual(rc, 0)
        assert_terminal(self, data, terminal_phase="success",
                        terminal_outcome="pass", exit_code=0,
                        rounds_completed=2)
        self.assertTrue((ws.root / receipt_rel(1, "audit")).exists())
        self.assertFalse((ws.root / receipt_rel(1, "verification")).exists())
        receipt = self._receipt(ws, 1, "audit")
        self.assertEqual(receipt["outcome"], "findings")
        self.assertEqual(receipt["findings"], ["fixture audit finding"])
        record = self._phase_record(data, 1, "audit")
        self.assertEqual(receipt["phase_base_commit"], record["head_commit"])
        self.assertEqual(
            receipt["result_digest"],
            sha256(_canonical_result(
                "findings", findings=["fixture audit finding"])),
        )
        self.assertEqual(receipt["result_digest"], record["result_digest"])
        self.assertIn(receipt["phase_tag"], self._ledger_tags(ws))

    def test_verification_and_audit_findings_both_flow(self) -> None:
        ws = self.make({
            "planner": {"behavior": {"1": "planned", "2": "findings-revised",
                                     "default": "planned"}},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": {"1": "findings", "2": "pass",
                                    "default": "pass"}},
            "auditor": {"behavior": {"1": "findings", "2": "pass",
                                     "default": "pass"}},
        }, rounds=2)
        rc, data = ws.run_cli()
        self.assertEqual(rc, 0)
        assert_terminal(self, data, terminal_phase="success",
                        terminal_outcome="pass", exit_code=0,
                        rounds_completed=2)
        for phase in ("verification", "audit"):
            self.assertTrue((ws.root / receipt_rel(1, phase)).exists())
        # Both entries reached the round-2 planner (its driver validated the
        # payload) and the revised plan carries the finding.
        revised_plan = self._plan_at_commit(
            ws, "factory-campaign: planning round 2")
        plan = plan_parser.Plan.from_bytes(revised_plan)
        task2 = next(t for t in plan.tasks if t.number == 2)
        self.assertIn(FindingsWorkspace.FINDING_MARKER, task2.fields["Scope"])

    def test_external_blockers_flow_as_structured_findings(self) -> None:
        # Round 1 verification blocked (unavailable declared capability):
        # the receipt carries the exact external references and the round-2
        # planner keeps the blocker explicit in the plan.
        ws = self.make({
            "planner": {"behavior": {"1": "planned", "2": "findings-revised",
                                     "default": "planned"}},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": {"1": "blocked", "2": "pass",
                                    "default": "pass"}},
            "auditor": {"behavior": "pass"},
        }, rounds=2)
        rc, data = ws.run_cli(
            extra=["--capability-command", str(FALSE_EXECUTABLE)])
        self.assertEqual(rc, 0)
        receipt = self._receipt(ws, 1, "verification")
        self.assertEqual(receipt["outcome"], "blocked")
        self.assertEqual(receipt["blocked_on"], ["external-capability-required"])
        record = self._phase_record(data, 1, "verification")
        self.assertEqual(record["outcome"], "blocked")
        self.assertEqual(receipt["result_digest"], record["result_digest"])
        # The blocked-revised template keeps the blocker explicit as a
        # blocked task with the exact reference while the next task carries
        # the accepted finding.
        revised_plan = self._plan_at_commit(
            ws, "factory-campaign: planning round 2")
        plan = plan_parser.Plan.from_bytes(revised_plan)
        task1 = next(t for t in plan.tasks if t.number == 1)
        self.assertEqual(task1.status, "blocked")
        self.assertEqual(task1.blocked_on, FindingsWorkspace.BLOCKER)
        task2 = next(t for t in plan.tasks if t.number == 2)
        self.assertIn(FindingsWorkspace.FINDING_MARKER, task2.fields["Scope"])

    def test_selector_is_not_a_findings_authority(self) -> None:
        # The deterministic selector is a pure function of plan + state; the
        # receipt payload is never consulted and never stored.
        ws = self.make({
            "planner": {"behavior": {"1": "planned", "2": "findings-revised",
                                     "default": "planned"}},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": {"1": "findings", "2": "pass",
                                    "default": "pass"}},
            "auditor": {"behavior": "pass"},
        }, rounds=2)
        rc, data = ws.run_cli()
        self.assertEqual(rc, 0)
        revised_plan = self._plan_at_commit(
            ws, "factory-campaign: planning round 2")
        plan = plan_parser.Plan.from_bytes(revised_plan)
        selection = selector_module.select_task(plan)
        # The revised plan deterministically selects the revised task (2).
        self.assertTrue(selection.selected)
        self.assertEqual(selection.task_id, 2)
        # The selector API surface has no findings channel and the payload
        # is never persisted (the evidence namespace holds receipts only).
        names = [p.name for p in (ws.root / STATE_DIR).iterdir()]
        self.assertFalse(any(
            n.startswith("factory-findings") and "receipt" not in n
            for n in names))


class FindingsCrashWindow(_FindingsCampaign):
    """Task 10 REQ 1: the receipt-mint crash window reconciles deterministically.

    A crash between the trusted publish (preserved phase-result + write-once
    receipt) and the state-advance write leaves the state file recording the
    verification/audit phase at the round's planning base while HEAD already
    advanced through the round's own implementation commits.  The rerun
    re-validates the committed scope and completes the transition from the
    already-published trusted artifacts **without re-running the untrusted
    role** — re-execution could only produce a changed result that the
    byte-exact no-replace preserve would then reject (a wedge).  Torn or
    tampered mint artifacts fail closed for operator inspection.
    """

    def _crash_verification_advance(self, ws) -> dict:
        """Run the campaign, losing the verification -> audit state advance.

        The tester produced findings; the orchestrator preserved the exact
        result bytes and published the write-once receipt before the state
        advance write was lost.  Returns the pre-run config.
        """
        original = state_module.write_state
        crashed = {"raised": False}

        def crashing_write(root, state):
            if not crashed["raised"] and state.current_phase == "audit":
                crashed["raised"] = True
                raise state_module.StateError(
                    "simulated crash: verification state advance lost")
            return original(root, state)

        config = ws.derive_config()
        with unittest.mock.patch.object(
            campaign_module.state_module, "write_state",
            side_effect=crashing_write,
        ):
            with self.assertRaises(state_module.StateError):
                campaign_module.Campaign(config).run()
        self.assertTrue(crashed["raised"])
        # The receipt and preserved phase-result were already published, and
        # the state file still records the verification phase at the round
        # base (HEAD advanced through the implementation commits).
        self.assertTrue((ws.root / receipt_rel(1, "verification")).exists())
        self.assertTrue(
            (ws.root / STATE_DIR /
             findings_module.result_name(1, "verification")).exists())
        state = ws.load_state()
        self.assertEqual(state.current_phase, "verification")
        self.assertNotEqual(
            _git(ws.root, "rev-parse", "HEAD").stdout.strip(),
            state.phase_base_commit,
        )
        return config

    def _count_role_runs(self, config: dict):
        calls: list[str] = []
        original = campaign_module.Campaign._run_role

        def counting(self, role, state, head, **kwargs):
            calls.append(role)
            return original(self, role, state, head, **kwargs)

        with unittest.mock.patch.object(
            campaign_module.Campaign, "_run_role",
            autospec=True, side_effect=counting,
        ):
            result = campaign_module.Campaign(config).run()
        return result, calls

    def test_verification_crash_window_completes_from_published_receipt(
        self,
    ) -> None:
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "findings"},
            "auditor": {"behavior": "pass"},
        }, rounds=1)
        config = self._crash_verification_advance(ws)
        result, calls = self._count_role_runs(config)
        self.assertEqual(result.terminal_phase, "success")
        self.assertEqual(result.terminal_outcome, "pass")
        self.assertEqual(result.rounds_completed, 1)
        # The verification transition was reconciled from the published
        # receipt: the untrusted tester never re-ran (only the audit ran).
        self.assertEqual(calls, ["auditor"])
        self.assertEqual(len(result.phase_history), 2)
        recovered = result.phase_history[0]
        self.assertEqual((recovered.round, recovered.phase),
                         (1, "verification"))
        self.assertEqual(recovered.outcome, "findings")
        self.assertEqual(
            recovered.detail, "reconciled from the published findings receipt")
        self.assertEqual(recovered.result_digest,
                         sha256(_canonical_result(
                             "findings", findings=["fixture finding"])))
        self.assertEqual(result.phase_history[1].phase, "audit")
        self.assertEqual(result.phase_history[1].outcome, "pass")
        # The receipt evidence survives intact with every binding.
        receipt = self._receipt(ws, 1, "verification")
        self.assertEqual(receipt["outcome"], "findings")
        self.assertEqual(
            receipt["phase_base_commit"],
            _git(ws.root, "rev-parse", "HEAD").stdout.strip(),
        )
        self.assertIn(receipt["phase_tag"], self._ledger_tags(ws))
        preserved = (ws.root / STATE_DIR /
                     findings_module.result_name(1, "verification")).read_bytes()
        self.assertEqual(receipt["result_digest"], sha256(preserved))

    def test_audit_crash_window_completes_terminal_from_published_receipt(
        self,
    ) -> None:
        # Final-round audit findings: the crash loses the terminal state
        # advance after the receipt was published; the rerun completes the
        # audit directly into the ``findings`` terminal from the receipt.
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "findings"},
        }, rounds=1)
        original = state_module.write_state
        crashed = {"raised": False}

        def crashing_write(root, state):
            if not crashed["raised"] and state.current_phase == "findings":
                crashed["raised"] = True
                raise state_module.StateError(
                    "simulated crash: audit state advance lost")
            return original(root, state)

        config = ws.derive_config()
        with unittest.mock.patch.object(
            campaign_module.state_module, "write_state",
            side_effect=crashing_write,
        ):
            with self.assertRaises(state_module.StateError):
                campaign_module.Campaign(config).run()
        self.assertTrue(crashed["raised"])
        self.assertTrue((ws.root / receipt_rel(1, "audit")).exists())
        self.assertEqual(ws.load_state().current_phase, "audit")
        result, calls = self._count_role_runs(config)
        self.assertEqual(result.terminal_phase, "findings")
        self.assertEqual(result.terminal_outcome, "findings")
        self.assertEqual(result.rounds_completed, 1)
        # No role ran at all: the terminal was completed from the receipt.
        self.assertEqual(calls, [])
        self.assertEqual(len(result.phase_history), 1)
        recovered = result.phase_history[0]
        self.assertEqual((recovered.round, recovered.phase),
                         (1, "audit"))
        self.assertEqual(recovered.outcome, "findings")
        self.assertEqual(
            recovered.detail, "reconciled from the published findings receipt")

    def test_verification_crash_window_before_mint_reruns_phase_idempotently(
        self,
    ) -> None:
        # A crash DURING the untrusted verification phase (before any
        # preserve/mint) leaves no receipt and no preserved artifact; the
        # rerun re-validates the committed scope, re-runs the phase normally
        # at HEAD, and the fresh preserve/mint of the reproduced result
        # complete without wedging (REQ 1).
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "findings"},
            "auditor": {"behavior": "pass"},
        }, rounds=1)
        config = ws.derive_config()
        original_run_role = campaign_module.Campaign._run_role
        crashed = {"raised": False}

        def crashing_run_role(self, role, state, head, **kwargs):
            if role == "tester" and not crashed["raised"]:
                crashed["raised"] = True
                raise state_module.StateError(
                    "simulated crash: the verifier died before the mint")
            return original_run_role(self, role, state, head, **kwargs)

        with unittest.mock.patch.object(
            campaign_module.Campaign, "_run_role",
            autospec=True, side_effect=crashing_run_role,
        ):
            with self.assertRaises(state_module.StateError):
                campaign_module.Campaign(config).run()
        self.assertTrue(crashed["raised"])
        # No mint artifacts exist: the crash predates the preserve/mint.
        self.assertFalse((ws.root / receipt_rel(1, "verification")).exists())
        self.assertFalse(
            (ws.root / STATE_DIR /
             findings_module.result_name(1, "verification")).exists())
        self.assertEqual(ws.load_state().current_phase, "verification")
        # The rerun re-runs the campaign from the verification phase; the
        # tester runs again and the phase completes normally.
        result, calls = self._count_role_runs(config)
        self.assertEqual(result.terminal_phase, "success")
        self.assertEqual(result.terminal_outcome, "pass")
        self.assertEqual(calls, ["tester", "auditor"])
        self.assertTrue((ws.root / receipt_rel(1, "verification")).exists())

    def test_torn_preserved_without_receipt_fails_closed_during_recovery(
        self,
    ) -> None:
        # A crash between the preserve and the mint leaves a preserved
        # phase-result with no receipt: recovery fails closed for operator
        # inspection instead of guessing or re-running over the evidence.
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "findings"},
            "auditor": {"behavior": "pass"},
        }, rounds=1)
        config = self._crash_verification_advance(ws)
        (ws.root / receipt_rel(1, "verification")).unlink()
        with self.assertRaises(campaign_module.CampaignRecoveryError) as cm:
            campaign_module.Campaign(config).run()
        self.assertIn("without its findings receipt", str(cm.exception))

    def test_torn_receipt_without_preserved_fails_closed_during_recovery(
        self,
    ) -> None:
        # A receipt whose preserved phase-result artifact is missing is a
        # receipt-only claim whose content cannot be authenticated; recovery
        # fails closed instead of trusting the self-digest.
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "findings"},
            "auditor": {"behavior": "pass"},
        }, rounds=1)
        config = self._crash_verification_advance(ws)
        (ws.root / STATE_DIR /
         findings_module.result_name(1, "verification")).unlink()
        with self.assertRaises(campaign_module.CampaignRecoveryError) as cm:
            campaign_module.Campaign(config).run()
        self.assertIn("no preserved phase-result artifact", str(cm.exception))

    def test_tampered_receipt_fails_closed_during_recovery(self) -> None:
        # A schema-valid but contradictory receipt (findings rewritten after
        # the mint) is authenticated against the exact preserved phase-result
        # bytes during recovery and fails closed.
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "findings"},
            "auditor": {"behavior": "pass"},
        }, rounds=1)
        config = self._crash_verification_advance(ws)
        receipt = json.loads((ws.root / receipt_rel(1, "verification")).read_text())
        receipt["findings"] = ["forged finding"]
        self._write_marker(
            ws, "factory-findings-receipt-round-1-verification.json",
            findings_module.receipt_bytes(receipt),
        )
        with self.assertRaises(campaign_module.CampaignRecoveryError) as cm:
            campaign_module.Campaign(config).run()
        self.assertIn("contradict", str(cm.exception))


class FindingsCampaignFailClosed(_FindingsCampaign):
    """Pre-planted/malformed/oversized/symlinked receipts fail the campaign."""

    def test_preplanted_receipt_fails_the_mint_closed(self) -> None:
        # A forged receipt pre-planted at the canonical name of a phase that
        # will produce findings: the genuine mint cannot publish over it.
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "findings"},
            "auditor": {"behavior": "pass"},
        }, rounds=1)
        planted = findings_module.build_receipt(
            campaign_id="campaign", round_number=1, phase="verification",
            phase_tag="r1.verification.1.a1", phase_base_commit="0" * 40,
            outcome="findings", result_digest=sha256(b"preplant"),
        )
        self._write_marker(
            ws, "factory-findings-receipt-round-1-verification.json",
            findings_module.receipt_bytes(planted))
        rc, data = ws.run_cli()
        self.assertEqual(rc, campaign_module.EXIT_ERROR)
        self.assertIsNone(data)

    def test_preplanted_synthetic_receipt_fails_consumption_closed(self) -> None:
        # A forged receipt for a phase that will record a pass: round 1 runs
        # clean, round 2 planning consumption rejects the synthetic receipt.
        ws = self.make(SUCCESS_SCENARIO, rounds=2)
        planted = findings_module.build_receipt(
            campaign_id="campaign", round_number=1, phase="verification",
            phase_tag="r1.verification.1.a1", phase_base_commit="0" * 40,
            outcome="findings", result_digest=sha256(b"preplant"),
        )
        self._write_marker(
            ws, "factory-findings-receipt-round-1-verification.json",
            findings_module.receipt_bytes(planted))
        rc, data = ws.run_cli()
        self.assertEqual(rc, campaign_module.EXIT_ERROR)
        self.assertIsNone(data)

    def test_malformed_receipt_fails_consumption_closed(self) -> None:
        ws = self.make(SUCCESS_SCENARIO, rounds=2)
        self._write_marker(
            ws, "factory-findings-receipt-round-1-verification.json",
            b"{not valid json")
        rc, data = ws.run_cli()
        self.assertEqual(rc, campaign_module.EXIT_ERROR)
        self.assertIsNone(data)

    def test_oversized_receipt_fails_closed(self) -> None:
        ws = self.make(SUCCESS_SCENARIO, rounds=2)
        planted = findings_module.build_receipt(
            campaign_id="campaign", round_number=1, phase="verification",
            phase_tag="r1.verification.1.a1", phase_base_commit="0" * 40,
            outcome="findings", result_digest=sha256(b"x"),
            findings=["x" * (findings_module.MAX_RECEIPT_BYTES + 1)],
        )
        self._write_marker(
            ws, "factory-findings-receipt-round-1-verification.json",
            findings_module.receipt_bytes(planted))
        rc, data = ws.run_cli()
        self.assertEqual(rc, campaign_module.EXIT_ERROR)
        self.assertIsNone(data)

    def test_symlinked_receipt_fails_closed(self) -> None:
        ws = self.make(SUCCESS_SCENARIO, rounds=2)
        directory = ws.root / STATE_DIR
        directory.mkdir(mode=0o700, exist_ok=True)
        target = directory / "elsewhere.json"
        target.write_text(json.dumps({
            "schema": "factory-findings-receipt/v1",
        }), encoding="utf-8")
        (directory / "factory-findings-receipt-round-1-verification.json").symlink_to(
            target)
        rc, data = ws.run_cli()
        self.assertEqual(rc, campaign_module.EXIT_ERROR)
        self.assertIsNone(data)


class RunnerEvidenceReadUnit(_FindingsBase):
    """B5: the hardened bounded no-follow runner-evidence reader and the
    derive-archive-bindings derivation the coordinator feeds the projector.

    The exact evidence bytes are re-read beneath ``.factory-state/`` with
    per-component ``O_DIRECTORY|O_NOFOLLOW`` descriptors; any absent,
    symlinked, path-escaping, oversized, ownership/mode-anomalous, or
    digest-mismatched artifact fails closed.
    """

    NONCE = "ab" * 32
    RUNNER = "dev-runner-vm"

    def _evidence_dir(self) -> Path:
        path = (
            self.root / ".factory-state" / "runner-evidence"
            / "campaign" / self.NONCE
        )
        self._private_dirs(path)
        return path

    def _private_dirs(self, path: Path) -> None:
        """Create the evidence ancestry as private owned directories (the
        umask must never leave group/other bits on the evidence tree)."""
        current = self.root
        for part in path.relative_to(self.root).parts[:-1]:
            current = current / part
            current.mkdir(mode=0o700, exist_ok=True)
            current.chmod(0o700)
        path.mkdir(mode=0o700, exist_ok=True)
        path.chmod(0o700)

    def _write_evidence(self, relpath: str, data: bytes) -> Path:
        path = self.root / relpath
        self._private_dirs(path.parent)
        path.write_bytes(data)
        os.chmod(path, 0o600)
        return path

    def _manifest(self, **overrides) -> dict:
        manifest = {
            "schema": "factory-runner-findings-receipt/v1",
            "result": "findings",
            "runner": self.RUNNER,
            "commit": "a" * 40,
            "tree": "b" * 40,
            "environment_blob": "c" * 40,
            "archive_sha256": "d" * 64,
            "capabilities": ["remote-project-gate"],
            "campaign_id": "campaign",
            "readiness_nonce": self.NONCE,
            "authority_sha256": "e" * 64,
            "nonce": self.NONCE,
            "exit_code": 8,
            "timed_out": False,
            "cleanup": True,
            "skip_marker_detected": False,
        }
        manifest.update(overrides)
        return manifest

    def _manifest_rel(self, commit: str = "a" * 40) -> str:
        return (
            f".factory-state/runner-evidence/campaign/{self.NONCE}/"
            f"{self.RUNNER}/{commit}/{self.NONCE}/manifest.json"
        )

    def _aggregate(self, *, manifest: dict, commit: str = "a" * 40) -> dict:
        manifest_raw = json.dumps(
            manifest, sort_keys=True, separators=(",", ":")).encode()
        return {
            "schema": "factory-runner-findings-aggregate/v1",
            "campaign_id": "campaign",
            "readiness_nonce": self.NONCE,
            "commit": commit,
            "tree": "b" * 40,
            "environment_blob": "c" * 40,
            "runners": [{
                "name": self.RUNNER,
                "manifest": self._manifest_rel(commit),
                "manifest_sha256": findings_module.sha256(manifest_raw),
                "result": "findings",
                "capabilities": ["remote-project-gate"],
                "probes": [{
                    "capability": "remote-project-gate",
                    "exit_code": 8,
                    "timed_out": False,
                }],
                "artifact_manifest_sha256": "f" * 64,
                "artifact_count": 1,
                "artifact_bytes": 1024,
                "signer": {
                    "principal": self.RUNNER,
                    "key_sha256": "7" * 64,
                    "algorithm": "ssh-ed25519",
                    "signature_sha256": "8" * 64,
                },
            }],
        }

    def test_aggregate_read_returns_exact_bytes_and_digest(self) -> None:
        aggregate = self._aggregate(manifest=self._manifest())
        raw = (
            json.dumps(
                aggregate, sort_keys=True, separators=(",", ":")).encode()
            + b"\n"
        )
        self._write_evidence(
            ".factory-state/runner-evidence/campaign/"
            f"{self.NONCE}/findings-aggregate.json", raw)
        got, data = findings_module.read_runner_findings_aggregate(
            self.root, campaign_id="campaign",
            readiness_nonce=self.NONCE,
        )
        self.assertEqual(got, raw)
        self.assertEqual(findings_module.sha256(got), findings_module.sha256(raw))
        self.assertEqual(data["schema"], "factory-runner-findings-aggregate/v1")
        self.assertEqual(data["campaign_id"], "campaign")

    def test_missing_aggregate_fails_closed(self) -> None:
        with self.assertRaises(findings_module.FindingsMalformedError):
            findings_module.read_runner_findings_aggregate(
                self.root, campaign_id="campaign",
                readiness_nonce=self.NONCE,
            )

    def test_aggregate_over_owned_writable_component_fails_closed(self) -> None:
        raw = (
            json.dumps(self._aggregate(manifest=self._manifest()),
                       sort_keys=True, separators=(",", ":")).encode()
            + b"\n"
        )
        path = self._write_evidence(
            ".factory-state/runner-evidence/campaign/"
            f"{self.NONCE}/findings-aggregate.json", raw)
        os.chmod(path, 0o620)  # group-writable artifact
        with self.assertRaises(findings_module.FindingsMalformedError):
            findings_module.read_runner_findings_aggregate(
                self.root, campaign_id="campaign",
                readiness_nonce=self.NONCE,
            )

    def test_symlinked_final_artifact_fails_closed(self) -> None:
        raw = (
            json.dumps(self._aggregate(manifest=self._manifest()),
                       sort_keys=True, separators=(",", ":")).encode()
            + b"\n"
        )
        self._write_evidence(
            ".factory-state/runner-evidence/campaign/"
            f"{self.NONCE}/findings-aggregate.json", raw)
        directory = self._evidence_dir()
        target = self.root / "elsewhere.json"
        target.write_bytes(b"{}")
        (directory / "findings-aggregate.json").unlink()
        (directory / "findings-aggregate.json").symlink_to(target)
        with self.assertRaises(findings_module.FindingsMalformedError):
            findings_module.read_runner_findings_aggregate(
                self.root, campaign_id="campaign",
                readiness_nonce=self.NONCE,
            )

    def test_path_escape_and_foreign_namespace_fail_closed(self) -> None:
        for relpath in (
            "../secret.json",
            ".factory-state/other/findings-aggregate.json",
            ".factory-state/runner-evidence/campaign/findings-aggregate.json",
            "/abs/findings-aggregate.json",
        ):
            with self.subTest(path=relpath):
                with self.assertRaises(
                    findings_module.FindingsMalformedError
                ):
                    findings_module.read_runner_evidence_bytes(
                        self.root, relpath, maximum=1024)

    def test_derive_archive_bindings_positive(self) -> None:
        manifest = self._manifest()
        manifest_raw = json.dumps(
            manifest, sort_keys=True, separators=(",", ":")).encode()
        self._write_evidence(self._manifest_rel(), manifest_raw)
        aggregate = self._aggregate(manifest=manifest)
        bindings = findings_module.derive_archive_bindings(
            self.root, aggregate)
        binding = bindings[self.RUNNER]
        self.assertEqual(binding["archive_sha256"], "d" * 64)
        self.assertEqual(binding["authority_sha256"], "e" * 64)
        self.assertEqual(binding["nonce"], self.NONCE)
        self.assertIs(binding["skip_marker_detected"], False)
        self.assertIs(binding["cleanup"], True)
        self.assertIs(binding["timed_out"], False)

    def test_derive_archive_bindings_manifest_digest_mismatch_fails(self) -> None:
        manifest = self._manifest()
        manifest_raw = json.dumps(
            manifest, sort_keys=True, separators=(",", ":")).encode()
        self._write_evidence(self._manifest_rel(), manifest_raw)
        aggregate = self._aggregate(manifest=manifest)
        aggregate["runners"][0]["manifest_sha256"] = "9" * 64
        with self.assertRaises(findings_module.FindingsSyntheticError):
            findings_module.derive_archive_bindings(self.root, aggregate)

    def test_derive_archive_bindings_skip_marker_fails_closed(self) -> None:
        manifest = self._manifest(skip_marker_detected=True)
        manifest_raw = json.dumps(
            manifest, sort_keys=True, separators=(",", ":")).encode()
        self._write_evidence(self._manifest_rel(), manifest_raw)
        aggregate = self._aggregate(manifest=manifest)
        with self.assertRaises(findings_module.FindingsSyntheticError):
            findings_module.derive_archive_bindings(self.root, aggregate)

    def test_derive_archive_bindings_timed_out_fails_closed(self) -> None:
        manifest = self._manifest(timed_out=True)
        manifest_raw = json.dumps(
            manifest, sort_keys=True, separators=(",", ":")).encode()
        self._write_evidence(self._manifest_rel(), manifest_raw)
        aggregate = self._aggregate(manifest=manifest)
        with self.assertRaises(findings_module.FindingsSyntheticError):
            findings_module.derive_archive_bindings(self.root, aggregate)

    def test_pass_manifest_without_marker_field_binds_clean(self) -> None:
        # A pass-only receipt class carries no skip-marker field; the derived
        # binding still proves clean cleanup and no timeout.
        manifest = {
            "schema": "factory-runner-receipt/v3",
            "result": "pass",
            "runner": self.RUNNER,
            "commit": "a" * 40,
            "tree": "b" * 40,
            "environment_blob": "c" * 40,
            "archive_sha256": "d" * 64,
            "capabilities": ["remote-project-gate"],
            "campaign_id": "campaign",
            "readiness_nonce": self.NONCE,
            "authority_sha256": "e" * 64,
            "nonce": self.NONCE,
            "exit_code": 0,
            "timed_out": False,
            "cleanup": True,
        }
        manifest_raw = json.dumps(
            manifest, sort_keys=True, separators=(",", ":")).encode()
        self._write_evidence(self._manifest_rel(), manifest_raw)
        aggregate = self._aggregate(manifest=manifest)
        aggregate["runners"][0]["result"] = "pass"
        binding = findings_module.derive_archive_bindings(
            self.root, aggregate)[self.RUNNER]
        self.assertIs(binding["skip_marker_detected"], False)
        self.assertIs(binding["cleanup"], True)
        self.assertIs(binding["timed_out"], False)


if __name__ == "__main__":
    unittest.main()
