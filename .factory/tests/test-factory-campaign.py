#!/usr/bin/env python3
"""Hidden campaign suite for the trusted phase/campaign orchestrator
(PHASE-01, COMPLETE-01, GIT-01; FACTORY-LOOP-SPEC §11-§15, §17; Task 9).

This test lives under the hidden ``.factory/tests/`` namespace because the
specification (HIDE-01, §3) keeps harness-only tests out of the adopting
product's visible test tree.  It drives ``.factory/loop/campaign.py`` — the
trusted phase/campaign control plane — through the explicit deterministic
embedded role seam (``.factory/tests/fixtures/campaign_driver.py``) on
committed synthetic fixture repositories:

* **finite terminal outcomes (§13/§14)**: fixture campaigns for every
  terminal — ``success``, ``findings``, ``blocked`` (with exact
  unavailable-evidence references), ``failed`` (planning-attempt
  exhaustion), ``interrupted`` (planning interruption and dirty
  implementation-attempt exhaustion), and ``infrastructure_failure``
  (untrusted verifier/auditor) — each terminating within the configured
  round/attempt bounds with the exact §14 exit code and round count;
* **empty work does not spin (§13.2/§14)**: ``work_exhausted`` and
  ``blocked`` selections still reach verification and audit, and a
  multi-task campaign terminates after the last runnable task;
* **crash reconciliation (§17)**: a trusted commit that landed before its
  transition was recorded is reconciled deterministically from Git + plan +
  state on the next run (planning and implementation), an ambiguous
  recovery fails closed, and dirty work is preserved — never reset,
  discarded, or silently overwritten;
* **descriptor-anchored Git authority (GIT-01, §12)**: every campaign
  commit is created by the orchestrator identity, the committed scope of
  every campaign commit is exactly the allowlisted role work, and the
  model/fixture role never runs Git;
* **one trusted lifecycle surface (§11)**: the campaign touches exactly the
  control-state file, the append-only digest ledger, and the published
  campaign result under ``.factory-state/`` — no runtime task ledger,
  memory store, or event stream is ever created;
* **fail-closed control-plane errors**: write-once campaign bindings, a
  terminal control state, malformed phase-result files, unbound plans,
  scope violations, and unknown configurations all fail closed;
* **schema and CLI**: the published campaign result and the structured
  phase results conform to their committed schemas; the CLI help, exit
  codes, and config derivation are exercised end to end.

The suite reuses the established fixture patterns of the other
``test-factory-*.py`` modules: real committed Git repositories, the pinned
absolute Git executable, and the committed role-driver seam (never evidence
of real model acceptance or real confinement).
"""

from __future__ import annotations

import contextlib
import dataclasses
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
import unittest.mock

ROOT = Path(__file__).resolve().parents[2]
LOOP = ROOT / ".factory" / "loop"
FIXTURES = ROOT / ".factory" / "tests" / "fixtures"
SCHEMAS = ROOT / ".factory" / "schemas"
STATE_DIR = ".factory-state"

sys.path.insert(0, str(LOOP))
import audit_objectives as audit_objectives_module  # noqa: E402
import campaign as campaign_module  # noqa: E402
import findings as findings_module  # noqa: E402
import gitutil  # noqa: E402
import launch as launch_module  # noqa: E402
import plan_parser  # noqa: E402
import pre_round as pre_round_module  # noqa: E402
import readiness as readiness_module  # noqa: E402
import state as state_module  # noqa: E402

GIT = gitutil.GIT_EXECUTABLE

# Deterministic gate executables for the fixture campaigns.  ``/bin/false``
# and ``/bin/true`` do not exist under the Nix store layout, so the tests
# bind ``--verification-command`` / ``--capability-command`` to the store
# ``bin/true`` / ``bin/false`` entrypoints following the established pattern
# of the hidden suite.  The paths are intentionally *not* resolved through
# symlinks: the store ``bin/true`` is a symlink to the multi-call
# ``coreutils`` binary, which dispatches on argv[0], so the symlink path must
# be exec'd verbatim.  Task 9 review MED: verification requires an explicit
# deterministic verification command — the tester JSON alone never gates —
# so every fixture campaign configures the ``true`` gate and an absent gate
# fails the campaign closed (``infrastructure_failure``).
TRUE_EXECUTABLE = Path(shutil.which("true"))
FALSE_EXECUTABLE = Path(shutil.which("false"))

REQUIREMENT_REGISTRY = SCHEMAS / "factory-plan-v1.requirements.json"
PLAN_TOOL = FIXTURES / "fixture_plan_tool.py"
DRIVER_REL = ".factory/tests/fixtures/campaign_driver.py"
PLAN_REL = ".factory/artifacts/implementation-plan.md"
BRANCH = "fixture-main"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def run(
    command: list[str],
    root: Path | None = None,
    *,
    check: bool = True,
    env: dict | None = None,
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command, cwd=root, text=True, capture_output=True, env=env
    )
    if check and result.returncode:
        raise AssertionError(
            (command, result.returncode, result.stdout[-2000:], result.stderr[-2000:])
        )
    return result


def _git(workspace: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return run([GIT, "-C", str(workspace), *args], check=True)


def gen_plan(ws: Path, common: dict, out_rel: str, tasks: list[dict]) -> None:
    """Generate one committed-plan template through the committed tool."""
    spec_path = ws / "fixture-spec.json"
    spec_path.write_text(json.dumps({**common, "tasks": tasks}), encoding="utf-8")
    run(
        [
            sys.executable, str(PLAN_TOOL),
            "--spec", str(spec_path),
            "--registry", str(REQUIREMENT_REGISTRY),
            "--out", str(ws / out_rel),
        ],
        root=ws,
    )


# The standard fixture task set: two implementation tasks plus the final
# audit task (which must be last and depend on every other task).
TASK_SPECS: list[dict] = [
    {
        "number": 1, "title": "Implement the fixture feature",
        "status": "pending", "priority": 10, "dependencies": [],
        "blocked_on": None, "scope": "initial scope statement.",
        "verification": "`src/work-1.md`",
    },
    {
        "number": 2, "title": "Implement the second feature",
        "status": "pending", "priority": 20, "dependencies": [],
        "blocked_on": None,
        "verification": "`src/work-2.md`",
    },
    {
        "number": 3, "title": "final", "status": "pending",
        "priority": 1, "dependencies": [1, 2], "blocked_on": None,
        "verification": "`src/work-3.md`",
    },
]


class FixtureWorkspace:
    """One committed synthetic repository for campaign scenarios.

    A real Git repository whose committed blobs bind every authoritative byte
    the campaign reads (spec, plan, prompts, registry, driver), so the
    lock/binding authority and the embedded role driver re-derive exactly
    what the campaign config declared.  The role driver is copied from the
    committed fixture module so the worktree copy equals the committed blob
    the campaign verifies.
    """

    def __init__(
        self,
        tmp: Path,
        *,
        scenario: dict,
        rounds: int = 1,
        planning_attempts: int = 3,
        implementation_attempts: int = 3,
        phase_result: str = f"{STATE_DIR}/phase-result.json",
        audit_result: str = f"{STATE_DIR}/audit-result.json",
    ) -> None:
        self.root = tmp / "workspace"
        self.scenario = scenario
        self.rounds = rounds
        self.planning_attempts = planning_attempts
        self.implementation_attempts = implementation_attempts
        self.phase_result = phase_result
        self.audit_result = audit_result
        self.scenario_commit: str | None = None
        self.build()

    # -- fixture construction -------------------------------------------------

    def build(self) -> None:
        ws = self.root
        ws.mkdir(parents=True)
        for rel in (
            "docs",
            "scripts",
            ".factory/tools/pi-cli-shims",
            ".factory/loop",
            ".factory/prompts",
            ".factory/audit-objectives",
            ".factory/artifacts",
            ".factory/bugs",
            ".factory/tests",
            ".factory/tests/fixtures",
            "fixture/templates",
            "src",
        ):
            (ws / rel).mkdir(parents=True)
        # Task 11: every fixture repository commits the exact credential
        # guard — deterministic gate output is redacted through the exact
        # committed guard before it can enter a result, log, receipt, or
        # repository state.
        shutil.copy2(
            ROOT / ".factory/tools" / "credential-guard.py",
            ws / ".factory" / "tools" / "credential-guard.py",
        )
        # Task 11: every fixture repository commits the exact model-side Pi
        # guard extension — the launch authority always loads it through
        # ``--extension`` in the child argv.
        shutil.copy2(
            ROOT / ".factory/tools" / "pi-factory-guard-extension.mjs",
            ws / ".factory" / "tools" / "pi-factory-guard-extension.mjs",
        )
        shutil.copy2(
            ROOT / ".factory" / "tools" / "pi-cli-shims" / "git",
            ws / ".factory" / "tools" / "pi-cli-shims" / "git",
        )
        shutil.copy2(
            ROOT / ".factory/tools" / "pi2-secure-exec.py",
            ws / ".factory" / "tools" / "pi2-secure-exec.py",
        )
        for module in (
            "usage.py", "usage_fetch.py", "pre_round.py", "campaign.py", "state.py",
            "lock.py", "gitutil.py",
        ):
            shutil.copy2(
                ROOT / ".factory" / "loop" / module,
                ws / ".factory" / "loop" / module,
            )
        (ws / "AGENTS.md").write_text(
            "AGENTS.md operational policy\n", encoding="utf-8")
        (ws / "docs" / "SPEC.md").write_text(
            "PRODUCT SPEC FIXTURE\n", encoding="utf-8")
        for role in ("planner", "developer", "tester", "auditor"):
            (ws / ".factory" / "prompts" / f"{role}.md").write_text(
                f"# {role} role prompt\n", encoding="utf-8")
        shutil.copy2(
            ROOT / ".factory" / "audit-objectives" / "registry.json",
            ws / ".factory" / "audit-objectives" / "registry.json",
        )
        shutil.copy2(
            ROOT / ".factory" / "pre-round-hooks.json",
            ws / ".factory" / "pre-round-hooks.json",
        )
        _git(ws, "init", "-q", "-b", BRANCH)
        _git(ws, "config", "user.email", "fixture@test")
        _git(ws, "config", "user.name", "fixture")
        _git(ws, "add", "-A")
        _git(ws, "commit", "-qm", "fixture base")
        base_head = _git(ws, "rev-parse", "HEAD").stdout.strip()
        spec_blob = _git(ws, "rev-parse", "HEAD:docs/SPEC.md").stdout.strip()
        common = {
            "spec_path": "docs/SPEC.md",
            "spec_commit": base_head,
            "spec_blob": spec_blob,
            "base_commit": base_head,
            "lifecycle": "active",
        }
        self._generate_plans(common)
        driver_dst = ws / DRIVER_REL
        shutil.copy2(FIXTURES / "campaign_driver.py", driver_dst)
        os.chmod(driver_dst, 0o755)
        _git(ws, "add", "-A")
        _git(ws, "commit", "-qm", "fixture plan and driver")

    def _generate_plans(self, common: dict) -> None:
        ws = self.root
        gen_plan(ws, common, ".factory/artifacts/implementation-plan.md",
                 TASK_SPECS)
        # The planner template of round N preserves the tasks the previous
        # rounds already completed (a real planner revises the plan from the
        # committed plan state), so a multi-round campaign works through
        # every runnable task instead of re-selecting completed work.
        for round_no in range(1, self.rounds + 1):
            revised = [
                {
                    **t,
                    "scope": (t.get("scope", "fixture-scoped work only.")
                              + f" revised {round_no}."),
                    "status": "complete" if t["number"] < round_no else t["status"],
                }
                for t in TASK_SPECS
            ]
            if round_no > len(TASK_SPECS):
                revised[-1] = {
                    **revised[-1], "status": "blocked",
                    "blocked_on": "synthetic-five-round-extension",
                }
            gen_plan(ws, common, f"fixture/templates/planner-{round_no}.md",
                     revised)
        # The work-exhausted planner output must be a *genuine semantic*
        # planning revision (a scope edit, not just status flips), because the
        # meaningful-substance boundary commits a planner revision only when it
        # carries a real planning change; ``planned-complete`` therefore edits
        # each task's Scope while marking it complete so the committed
        # complete-plan drives implementation to ``work_exhausted``.
        complete = [
            {
                **dict(t),
                "status": "complete",
                "scope": (t.get("scope", "fixture-scoped work only.")
                          + " complete."),
            }
            for t in TASK_SPECS
        ]
        gen_plan(ws, {**common, "lifecycle": "complete"},
                 "fixture/templates/planner-complete.md", complete)
        blocked = [
            {**dict(TASK_SPECS[0]), "status": "blocked",
             "blocked_on": "external-capability-required"},
            {**dict(TASK_SPECS[1]), "dependencies": [1]},
            TASK_SPECS[2],
        ]
        gen_plan(ws, common, "fixture/templates/planner-blocked.md", blocked)
        # An unbound template: every binding matches except the cycle base.
        gen_plan(ws, {**common, "base_commit": "1" * 40},
                 "fixture/templates/planner-unbound.md", TASK_SPECS)
        for task in TASK_SPECS:
            number = task["number"]
            # The developer's completion template of task N keeps every
            # earlier task complete (it revises the already-committed plan
            # state), never regressing completed work; the final task's plan
            # therefore carries every task complete and a `complete`
            # lifecycle (the parser rejects an `active` plan whose
            # non-verified matrix rows reference only completed tasks).
            complete_tasks = [
                {**dict(t),
                 "status": "complete" if t["number"] <= number else t["status"]}
                for t in TASK_SPECS
            ]
            dev_common = (
                {**common, "lifecycle": "complete"}
                if all(t["status"] == "complete" for t in complete_tasks)
                else common
            )
            gen_plan(ws, dev_common, f"fixture/templates/dev-{number}.md",
                     complete_tasks)
            # A valid plan may reach ``in_progress`` only when every
            # dependency is complete (§8), so progress templates exist for
            # tasks without incomplete dependencies (the fixture's task 1).
            if not task["dependencies"]:
                progress_tasks = [
                    {**dict(t),
                     "status": "in_progress" if t["number"] == number else t["status"]}
                    for t in TASK_SPECS
                ]
                gen_plan(ws, common,
                         f"fixture/templates/dev-{number}-progress.md",
                         progress_tasks)

    def commit_scenario(self, scenario: dict | None = None) -> None:
        """Write and commit the scenario JSON (never dirty role work)."""
        if scenario is not None:
            self.scenario = scenario
        (self.root / "scenario.json").write_text(
            json.dumps(self.scenario), encoding="utf-8")
        _git(self.root, "add", "scenario.json")
        _git(self.root, "commit", "-qm", "scenario fixture")
        self.scenario_commit = _git(
            self.root, "rev-parse", "HEAD").stdout.strip()

    # -- invocation ------------------------------------------------------------

    def config_kwargs(self) -> dict:
        return {
            "campaign_id": "campaign",
            "rounds": self.rounds,
            "branch": BRANCH,
            "plan_path": PLAN_REL,
            "planning_attempts": self.planning_attempts,
            "implementation_attempts": self.implementation_attempts,
            "provider": "synthetic",
            "model": "fixture-model",
            "backend": "",
            "role_driver": DRIVER_REL,
            "scenario_path": "scenario.json",
            "acceptance_command": [],
            # Task 9 review MED: every fixture campaign configures an
            # explicit deterministic verification command; the tester's
            # structured result alone is never a gate.
            "verification_command": [str(TRUE_EXECUTABLE)],
            "capability_command": [],
            "phase_result_path": self.phase_result,
            "audit_result_path": self.audit_result,
            "role_timeout": 60.0,
            "gate_timeout": 60.0,
        }

    def run_cli(
        self,
        campaign_id: str = "campaign",
        extra: list[str] | None = None,
    ) -> tuple[int, dict | None]:
        """Run the campaign CLI in a subprocess; parse the JSON result.

        Every fixture campaign configures the deterministic verification
        gate (Task 9 review MED); a test that overrides it passes its own
        ``--verification-command`` in ``extra`` and the default is not
        added a second time.
        """
        argv = [
            sys.executable, str(LOOP / "campaign.py"),
            "--root", str(self.root),
            "run",
            "--campaign-id", campaign_id,
            "--rounds", str(self.rounds),
            "--branch", BRANCH,
            "--role-driver", DRIVER_REL,
            "--scenario", "scenario.json",
            "--phase-result", self.phase_result,
            "--audit-result", self.audit_result,
            "--planning-attempts", str(self.planning_attempts),
            "--implementation-attempts", str(self.implementation_attempts),
        ]
        if extra:
            argv.extend(extra)
        if not any(token.startswith("--verification-command")
                   for token in argv):
            argv += ["--verification-command", str(TRUE_EXECUTABLE)]
        result = run(argv, root=ROOT, check=False)
        data = None
        try:
            data = json.loads(result.stdout)
        except ValueError:
            pass
        return result.returncode, data

    def derive_config(self, campaign_id: str = "campaign"):
        return campaign_module.derive_campaign_config(
            self.root, **{**self.config_kwargs(), "campaign_id": campaign_id}
        )

    # -- state helpers ----------------------------------------------------------

    def state_file(self) -> Path:
        return self.root / STATE_DIR / state_module.STATE_FILE_NAME

    def ledger_file(self) -> Path:
        return self.root / STATE_DIR / state_module.DIGEST_LEDGER_NAME

    def result_file(self, campaign_id: str = "campaign") -> Path:
        return self.root / STATE_DIR / f"campaign-result-{campaign_id}.json"

    def load_state(self):
        return state_module.load_state(
            self.root,
            expected_branch=BRANCH,
            expected_campaign_id="campaign",
            expected_rounds_requested=self.rounds,
        )


SUCCESS_SCENARIO = {
    "planner": {"behavior": "planned"},
    "developer": {"behavior": "complete"},
    "tester": {"behavior": "pass"},
    "auditor": {"behavior": "pass"},
}


class _CampaignBase(unittest.TestCase):
    """Shared helpers: one fresh fixture workspace per test."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="factory-campaign-test."))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self._workspace_count = 0

    def make(self, scenario: dict, **kwargs) -> FixtureWorkspace:
        # A test may build more than one workspace (e.g. a second scenario
        # in the same test); every workspace gets its own directory.
        self._workspace_count += 1
        ws = FixtureWorkspace(
            self.tmp / f"ws{self._workspace_count}",
            scenario=scenario, **kwargs,
        )
        ws.commit_scenario()
        return ws

    def _crash_at_plan(
        self,
        ws: FixtureWorkspace,
        when: callable,
    ):
        """Run the campaign with a state-write that simulates a crash.

        Returns the pre-run config.  The trusted transition commit lands but
        the state write is lost, leaving the state file at the previous
        trusted write — the §17 recovery window.
        """
        original = state_module.write_state
        crashed = {"raised": False}

        def crashing_write(root, state):
            if not crashed["raised"] and when(state):
                crashed["raised"] = True
                raise state_module.StateError(
                    "simulated crash: state write lost")
            return original(root, state)

        config = ws.derive_config()
        with unittest.mock.patch.object(
            campaign_module.state_module, "write_state",
            side_effect=crashing_write,
        ):
            with self.assertRaises(state_module.StateError):
                campaign_module.Campaign(config).run()
        self.assertTrue(crashed["raised"])
        return config


def assert_terminal(
    test: unittest.TestCase,
    data: dict,
    *,
    terminal_phase: str,
    terminal_outcome: str,
    exit_code: int,
    rounds_completed: int,
) -> None:
    test.assertEqual(data["terminal_phase"], terminal_phase)
    test.assertEqual(data["terminal_outcome"], terminal_outcome)
    test.assertEqual(data["exit_code"], exit_code)
    test.assertEqual(data["rounds_completed"], rounds_completed)
    test.assertEqual(data["schema"], "factory-campaign-result/v1")
    test.assertEqual(data["campaign_id"], "campaign")
    test.assertEqual(len(data["head_commit"]), 40)


def assert_history(
    test: unittest.TestCase,
    data: dict,
    expected: list[tuple[int, str, str]],
) -> None:
    """Assert the phase-history ``(round, phase, outcome)`` sequence exactly."""
    got = [
        (record["round"], record["phase"], record["outcome"])
        for record in data["phase_history"]
    ]
    test.assertEqual(got, expected)


class CampaignTerminals(_CampaignBase):
    """§14 finite terminal classification fixtures."""

    def test_success_campaign(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        rc, data = ws.run_cli()
        self.assertEqual(rc, 0)
        assert_terminal(self, data, terminal_phase="success",
                        terminal_outcome="pass", exit_code=0,
                        rounds_completed=1)
        assert_history(self, data, [
            (1, "planning", "planned"),
            (1, "implementation", "task_completed"),
            (1, "verification", "pass"),
            (1, "audit", "pass"),
        ])
        state = ws.load_state()
        self.assertEqual(state.current_phase, "success")
        self.assertEqual(state.last_outcome, "success")

    def test_missing_tester_handoff_retries_once_before_gates(self) -> None:
        scenario = json.loads(json.dumps(SUCCESS_SCENARIO))
        scenario["tester"] = {
            "behavior": {"1.1": "no-result", "1.2": "pass"}
        }
        ws = self.make(scenario)
        rc, data = ws.run_cli()
        self.assertEqual(rc, 0)
        assert_terminal(self, data, terminal_phase="success",
                        terminal_outcome="pass", exit_code=0,
                        rounds_completed=1)
        verification = next(
            record for record in data["phase_history"]
            if record["phase"] == "verification"
        )
        self.assertEqual(verification["attempt"], 2)
        self.assertNotEqual(verification["result_digest"], "0" * 64)

    def test_malformed_tester_handoff_retries_once_before_gates(self) -> None:
        scenario = json.loads(json.dumps(SUCCESS_SCENARIO))
        scenario["tester"] = {
            "behavior": {"1.1": "malformed-result", "1.2": "pass"}
        }
        ws = self.make(scenario)
        rc, data = ws.run_cli()
        self.assertEqual(rc, 0)
        verification = next(
            record for record in data["phase_history"]
            if record["phase"] == "verification"
        )
        self.assertEqual(verification["attempt"], 2)
        self.assertNotEqual(verification["result_digest"], "0" * 64)

    def test_transient_auditor_interruption_retries_once(self) -> None:
        scenario = json.loads(json.dumps(SUCCESS_SCENARIO))
        scenario["auditor"] = {
            "behavior": {"1.1": "crash", "1.2": "pass"}
        }
        ws = self.make(scenario)
        rc, data = ws.run_cli()
        self.assertEqual(rc, 0)
        audit = next(
            record for record in data["phase_history"]
            if record["phase"] == "audit"
        )
        self.assertEqual(audit["attempt"], 2)
        self.assertEqual(audit["outcome"], "pass")

    def test_final_findings(self) -> None:
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "findings"},
            "auditor": {"behavior": "findings"},
        })
        rc, data = ws.run_cli()
        self.assertEqual(rc, 1)
        assert_terminal(self, data, terminal_phase="findings",
                        terminal_outcome="findings", exit_code=1,
                        rounds_completed=1)
        assert_history(self, data, [
            (1, "planning", "planned"),
            (1, "implementation", "task_completed"),
            (1, "verification", "findings"),
            (1, "audit", "findings"),
        ])

    def test_final_audit_blocked_exact_references(self) -> None:
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "blocked"},
        })
        rc, data = ws.run_cli()
        self.assertEqual(rc, 2)
        assert_terminal(self, data, terminal_phase="blocked",
                        terminal_outcome="blocked", exit_code=2,
                        rounds_completed=1)

    def test_audit_findings_take_precedence_over_blocked(self) -> None:
        # §14: when both categories exist, findings take precedence.  The
        # auditor writes findings *and* exact blocked references.
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "pass"},
            "auditor": {
                "behavior": "findings",
                "blocked_on": ["external-human-authority"],
            },
        })
        rc, data = ws.run_cli()
        self.assertEqual(rc, 1)
        assert_terminal(self, data, terminal_phase="findings",
                        terminal_outcome="findings", exit_code=1,
                        rounds_completed=1)

    def test_planning_attempt_exhaustion_terminates_failed(self) -> None:
        ws = self.make({
            "planner": {
                "behavior": {"default": "invalid"},
            },
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "pass"},
        })
        rc, data = ws.run_cli()
        self.assertEqual(rc, 3)
        assert_terminal(self, data, terminal_phase="failed",
                        terminal_outcome="failed", exit_code=3,
                        rounds_completed=0)
        history = data["phase_history"]
        self.assertEqual(len(history), 3)
        self.assertTrue(all(
            record["phase"] == "planning" and record["outcome"] == "failed"
            for record in history))

    def test_invalid_planner_retry_commits_from_restored_plan(self) -> None:
        # A deterministic planner failure leaves its unparsable plan restored
        # to the committed canonical bytes; a later valid ``planned`` attempt
        # then commits a genuine semantic revision without any parse residual.
        ws = self.make({
            "planner": {"behavior": {
                "1.1": "invalid", "1.2": "planned", "default": "planned",
            }},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "pass"},
        })
        rc, data = ws.run_cli()
        self.assertEqual(rc, 0)
        history = data["phase_history"]
        self.assertEqual(history[0]["outcome"], "failed")
        self.assertIn("does not parse", history[0]["detail"])
        self.assertEqual(history[1]["outcome"], "planned")
        self.assertNotIn("does not parse", history[1]["detail"])
        # The planner committed the restored canonical plan; the tree is clean.
        self.assertEqual(
            _git(ws.root, "status", "--short", "--", PLAN_REL).stdout, ""
        )

    def test_nonzero_planner_fails_despite_valid_changed_plan(self) -> None:
        ws = self.make({
            "planner": {"behavior": "planned-exit1"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "pass"},
        })
        rc, data = ws.run_cli()
        self.assertEqual(rc, 3)
        self.assertTrue(all(
            record["phase"] == "planning" and record["outcome"] == "failed"
            for record in data["phase_history"]
        ))

    def test_planning_interruption_terminates_interrupted(self) -> None:
        ws = self.make({
            "planner": {"behavior": "crash"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "pass"},
        })
        rc, data = ws.run_cli()
        self.assertEqual(rc, 4)
        assert_terminal(self, data, terminal_phase="interrupted",
                        terminal_outcome="interrupted", exit_code=4,
                        rounds_completed=0)

    def test_dirty_implementation_exhaustion_terminates_interrupted(self) -> None:
        # §13.2: budget expiry with dirty work terminates interrupted;
        # verification does not run.
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "crash"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "pass"},
        })
        rc, data = ws.run_cli()
        self.assertEqual(rc, 4)
        assert_terminal(self, data, terminal_phase="interrupted",
                        terminal_outcome="interrupted", exit_code=4,
                        rounds_completed=0)
        self.assertTrue(any(r["phase"] == "implementation"
                            for r in data["phase_history"]))
        self.assertTrue(all(r["phase"] != "verification"
                            for r in data["phase_history"]))

    def test_clean_implementation_timeout_exhaustion_reaches_verification(self) -> None:
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "clean-crash"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "pass"},
        })
        rc, data = ws.run_cli()
        self.assertEqual(rc, 0)
        self.assertEqual(data["rounds_completed"], 1)
        self.assertTrue(any(
            r["phase"] == "implementation" and r["outcome"] == "task_failed"
            for r in data["phase_history"]
        ))
        self.assertTrue(any(
            r["phase"] == "verification" for r in data["phase_history"]
        ))

    def test_untrusted_verifier_terminates_infrastructure_failure(self) -> None:
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "dirty"},
            "auditor": {"behavior": "pass"},
        })
        rc, data = ws.run_cli()
        self.assertEqual(rc, 5)
        assert_terminal(self, data, terminal_phase="infrastructure_failure",
                        terminal_outcome="infrastructure_failure",
                        exit_code=5, rounds_completed=0)

    def test_untrusted_auditor_terminates_infrastructure_failure(self) -> None:
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "dirty"},
        })
        rc, data = ws.run_cli()
        self.assertEqual(rc, 5)
        assert_terminal(self, data, terminal_phase="infrastructure_failure",
                        terminal_outcome="infrastructure_failure",
                        exit_code=5, rounds_completed=0)

    def test_nonzero_tester_fails_despite_valid_pass_result(self) -> None:
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "pass-exit1"},
            "auditor": {"behavior": "pass"},
        })
        rc, data = ws.run_cli()
        self.assertEqual(rc, 5)
        self.assertEqual(data["phase_history"][-1]["outcome"],
                         "infrastructure_failure")

    def test_nonzero_auditor_fails_despite_valid_pass_result(self) -> None:
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "pass-exit1"},
        })
        rc, data = ws.run_cli()
        self.assertEqual(rc, 5)
        self.assertEqual(data["phase_history"][-1]["outcome"],
                         "infrastructure_failure")

    def test_interrupted_audit_terminates_interrupted(self) -> None:
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "crash"},
        })
        rc, data = ws.run_cli()
        self.assertEqual(rc, 4)
        assert_terminal(self, data, terminal_phase="interrupted",
                        terminal_outcome="interrupted", exit_code=4,
                        rounds_completed=0)
        assert_history(self, data, [
            (1, "planning", "planned"),
            (1, "implementation", "task_completed"),
            (1, "verification", "pass"),
            (1, "audit", "interrupted"),
        ])

    def test_work_exhausted_reaches_verification_and_audit(self) -> None:
        # §13.2/§14: empty runnable work reaches verification and audit
        # instead of spinning.
        ws = self.make({
            "planner": {"behavior": "planned-complete"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "pass"},
        })
        rc, data = ws.run_cli()
        self.assertEqual(rc, 0)
        assert_terminal(self, data, terminal_phase="success",
                        terminal_outcome="pass", exit_code=0,
                        rounds_completed=1)
        assert_history(self, data, [
            (1, "planning", "planned"),
            (1, "implementation", "work_exhausted"),
            (1, "verification", "pass"),
            (1, "audit", "pass"),
        ])

    def test_blocked_plan_reaches_verification_and_audit(self) -> None:
        # §8/§13.2: a plan whose unfinished tasks are all explicitly blocked
        # on unavailable references classifies implementation as blocked and
        # still runs verification/audit.
        ws = self.make({
            "planner": {"behavior": "planned-blocked"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "blocked"},
        })
        rc, data = ws.run_cli()
        self.assertEqual(rc, 2)
        assert_terminal(self, data, terminal_phase="blocked",
                        terminal_outcome="blocked", exit_code=2,
                        rounds_completed=1)
        assert_history(self, data, [
            (1, "planning", "planned"),
            (1, "implementation", "blocked"),
            (1, "verification", "pass"),
            (1, "audit", "blocked"),
        ])


class CampaignRecovery(_CampaignBase):
    """§17 crash reconciliation derived from Git + plan + state."""

    def test_planning_commit_before_transition_is_reconciled(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        config = self._crash_at_plan(
            ws,
            lambda state: (
                state.current_phase == "implementation"
                and state.last_outcome == "planned"
            ),
        )
        # The planning commit landed; the state file still records the
        # planning phase at the old base — the recovery window.
        self.assertNotEqual(
            _git(ws.root, "rev-parse", "HEAD").stdout.strip(),
            ws.load_state().phase_base_commit,
        )
        self.assertEqual(ws.load_state().current_phase, "planning")
        # A fresh run reconciles the committed plan and continues to success.
        result = campaign_module.Campaign(config).run()
        self.assertEqual(result.terminal_phase, "success")
        self.assertEqual(result.terminal_outcome, "pass")

    def test_completion_commit_before_transition_is_reconciled(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        config = self._crash_at_plan(
            ws,
            lambda state: (
                state.current_phase == "verification"
                and state.last_outcome == "task_completed"
            ),
        )
        # The developer's completion commit landed; the state file still says
        # implementation with the selected task.
        state = state_module.load_state(ws.root, expected_branch=BRANCH)
        self.assertEqual(state.current_phase, "implementation")
        self.assertEqual(state.selected_task_id, 1)
        # Re-run: recovery verifies the committed completion and advances.
        result = campaign_module.Campaign(config).run()
        self.assertEqual(result.terminal_phase, "success")
        outcomes = [r.outcome for r in result.phase_history]
        self.assertIn("task_completed", outcomes)

    def test_failed_hook_crash_before_atomic_terminal_never_runs_planner(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        config = ws.derive_config()
        real_write = state_module.write_state

        def crash_before_terminal(root, candidate):
            if candidate.current_phase == "infrastructure_failure":
                raise RuntimeError("synthetic terminal publication crash")
            return real_write(root, candidate)

        with unittest.mock.patch.object(
            campaign_module.lock_module.RootLock, "validate_live_branch",
            side_effect=campaign_module.lock_module.RootLockError(
                "synthetic branch failure"
            ),
        ), unittest.mock.patch.object(
            state_module, "write_state", side_effect=crash_before_terminal
        ), self.assertRaisesRegex(RuntimeError, "publication crash"):
            campaign_module.Campaign(config).run()
        claimed = ws.load_state()
        self.assertEqual(claimed.pre_round_hook_started_round, 1)
        self.assertEqual(claimed.pre_round_hook_completed_round, 0)
        recovered = campaign_module.Campaign(config).run()
        self.assertEqual(recovered.terminal_phase, "infrastructure_failure")
        self.assertNotIn("planned", [r.outcome for r in recovered.phase_history])

    def test_programmatic_forged_empty_hook_registry_fails_under_lock(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        config = ws.derive_config()
        forged = pre_round_module.Registry((), "0" * 64)
        forged_digest = pre_round_module.configuration_digest(
            forged, {}, bound_commit=config.phase_base_commit
        )
        forged_config = dataclasses.replace(
            config,
            pre_round_registry=forged,
            pre_round_implementation_digests={},
            pre_round_hook_configuration_digest=forged_digest,
        )
        with self.assertRaisesRegex(
            campaign_module.CampaignBindingError,
            "differs from the locked exact commit",
        ):
            campaign_module.Campaign(forged_config).run()
        # Binding failure released the sole writer lock; the exact config can run.
        self.assertEqual(campaign_module.Campaign(config).run().terminal_phase, "success")

    def test_ambiguous_recovery_fails_closed(self) -> None:
        # A HEAD advanced past the phase base with a foreign commit during
        # planning fails closed instead of guessing.
        ws = self.make(SUCCESS_SCENARIO)
        config = self._crash_at_plan(
            ws,
            lambda state: (
                state.current_phase == "implementation"
                and state.last_outcome == "planned"
            ),
        )
        (ws.root / "foreign.txt").write_text("not role work\n", encoding="utf-8")
        _git(ws.root, "add", "foreign.txt")
        _git(ws.root, "commit", "-qm", "foreign scope")
        with self.assertRaises(campaign_module.CampaignRecoveryError):
            campaign_module.Campaign(config).run()

    def test_non_semantic_plan_only_recovered_commit_fails_closed(self) -> None:
        # A plan-only recovery commit that is not a genuine semantic planning
        # change from the committed plan at the planning base must never
        # advance the phase as ``planned``.  The planning commit below (prose-
        # only, semantically identical to the base plan) would otherwise be a
        # foreign/metadata-only recovery; the reconciler re-checks
        # ``plan_has_semantic_change`` and fails closed for inspection.
        ws = self.make(SUCCESS_SCENARIO)
        config = self._crash_at_plan(
            ws,
            lambda state: (
                state.current_phase == "implementation"
                and state.last_outcome == "planned"
            ),
        )
        base = ws.load_state().phase_base_commit
        # Rewind to the phase base: the state file (untracked under
        # ``.factory-state``) still records the planning phase at ``base``,
        # while HEAD is now advanced only by a plan-only prose commit.
        _git(ws.root, "reset", "--hard", base)
        plan_rel = ws.root / PLAN_REL
        plan = plan_parser.Plan.from_file(plan_rel)
        block = next(
            blk for blk in plan._blocks
            if (blk.heading or "").strip().startswith("## Task 1:")
        )
        target = next(
            i for i, line in enumerate(block.lines)
            if line.strip() == "- Verification:"
            or line.strip().startswith("- Verification: ")
        )
        block.lines.insert(target + 1, "  appended non-semantic prose")
        plan_rel.write_text(plan.serialize(), encoding="utf-8")
        _git(ws.root, "add", PLAN_REL)
        _git(ws.root, "commit", "-qm", "non-semantic plan-only recovery")
        changed = _git(ws.root, "diff", "--name-only", base, "HEAD").stdout.split()
        self.assertEqual(changed, [PLAN_REL])
        with self.assertRaises(campaign_module.CampaignRecoveryError):
            campaign_module.Campaign(config).run()


class EmptyWorkAndFindings(_CampaignBase):
    """§14/§16: findings and empty work flow through verification/audit."""

    def test_multi_task_campaign_completes_every_runnable_task(self) -> None:
        # §14: each round runs planning -> implementation -> verification ->
        # audit, and the planner revision of round N preserves the tasks the
        # earlier rounds completed.  The campaign therefore works through
        # every runnable task — one per round — and the final round's last
        # selection classifies as work_exhausted before verification/audit.
        ws = self.make(SUCCESS_SCENARIO, rounds=3)
        rc, data = ws.run_cli()
        self.assertEqual(rc, 0)
        self.assertEqual(data["rounds_completed"], 3)
        assert_history(self, data, [
            (1, "planning", "planned"),
            (1, "implementation", "task_completed"),
            (1, "verification", "pass"),
            (1, "audit", "pass"),
            (2, "planning", "planned"),
            (2, "implementation", "task_completed"),
            (2, "verification", "pass"),
            (2, "audit", "pass"),
            (3, "planning", "planned"),
            (3, "implementation", "task_completed"),
            (3, "verification", "pass"),
            (3, "audit", "pass"),
        ])

    def test_five_round_campaign_runs_one_pre_round_sequence_per_round(self) -> None:
        ws = self.make(SUCCESS_SCENARIO, rounds=5)
        rc, data = ws.run_cli()
        self.assertEqual(rc, 0)
        self.assertEqual(data["rounds_completed"], 5)
        self.assertEqual(
            [r["round"] for r in data["phase_history"] if r["phase"] == "planning"],
            [1, 2, 3, 4, 5],
        )
        state = ws.load_state()
        self.assertEqual(state.pre_round_hook_started_round, 5)
        self.assertEqual(state.pre_round_hook_completed_round, 5)
        self.assertNotEqual(state.pre_round_hook_results_digest, "0" * 64)

    def test_five_round_hook_order_and_execution_count_are_exact(self) -> None:
        ws = self.make(SUCCESS_SCENARIO, rounds=5)
        observed = []
        original = pre_round_module.run_hooks

        def track(registry, *, implementation_digests, execute):
            executed = []
            def tracked_execute(hook):
                executed.append(hook.hook_id)
                return execute(hook)
            outcome = original(
                registry,
                implementation_digests=implementation_digests,
                execute=tracked_execute,
            )
            observed.append((tuple(h.hook_id for h in registry.hooks), tuple(executed)))
            return outcome

        with unittest.mock.patch.object(pre_round_module, "run_hooks", side_effect=track):
            result = campaign_module.Campaign(ws.derive_config()).run()
        self.assertEqual(result.terminal_phase, "success")
        self.assertEqual(len(observed), 5)
        self.assertTrue(all(
            registry == ("branch-guard",)
            and executed == ("branch-guard",)
            for registry, executed in observed
        ))

    def test_planner_retries_do_not_rerun_pre_round_hooks(self) -> None:
        ws = self.make({
            "planner": {"behavior": {"default": "invalid"}},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "pass"},
        }, planning_attempts=3)
        calls = 0
        original = pre_round_module.run_hooks
        def track(*args, **kwargs):
            nonlocal calls
            calls += 1
            return original(*args, **kwargs)
        with unittest.mock.patch.object(pre_round_module, "run_hooks", side_effect=track):
            result = campaign_module.Campaign(ws.derive_config()).run()
        self.assertEqual(result.terminal_phase, "failed")
        self.assertEqual(calls, 1)

    def test_findings_reach_next_round_via_revised_plan(self) -> None:
        # Round 1 verification+audit findings; round 2's planner revises the
        # plan and the campaign completes.
        ws = self.make({
            "planner": {"behavior": "planned"},
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
        rounds = [r["round"] for r in data["phase_history"]]
        self.assertEqual(rounds, [1, 1, 1, 1, 2, 2, 2, 2])

    def test_secret_findings_never_persist_or_prompt(self) -> None:
        """Task 23 (F): raw credential-shaped values in free-text findings/
        blocked references are redacted through the exact-commit credential
        guard BEFORE any durable storage and before the next planner receives
        them.  The raw secret candidate must never appear in any
        ``.factory-state`` artifact (preserved result, receipt, control
        state, payload) and never in the campaign output."""
        secret_finding = "api_token=super-secret-value-123 leak in fixture"
        secret_blocker = "GITHUB_TOKEN=ghp_secret_blocker external-capability"
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": {"1": "secret-findings", "2": "pass",
                                     "default": "pass"}},
            "auditor": {"behavior": {"1": "secret-blocked", "2": "pass",
                                     "default": "pass"}},
        }, rounds=2)
        rc, data = ws.run_cli()
        self.assertEqual(rc, 0, data)
        assert_terminal(self, data, terminal_phase="success",
                        terminal_outcome="pass", exit_code=0,
                        rounds_completed=2)
        # The raw secret candidates never persist anywhere under the ignored
        # runtime namespace and never reach the next planner's payload.
        state_dir = ws.root / STATE_DIR
        for path in sorted(state_dir.rglob("*")):
            if path.is_symlink() or not path.is_file():
                continue
            text = path.read_bytes().decode("utf-8", "replace")
            self.assertNotIn(
                "super-secret-value-123", text,
                f"raw secret persisted in {path.relative_to(state_dir)}",
            )
            self.assertNotIn(
                "ghp_secret_blocker", text,
                f"raw blocked reference persisted in "
                f"{path.relative_to(state_dir)}",
            )
        # The preserved structured results and the receipts carry the
        # semantic redaction markers instead of the raw values.
        preserved_verification = (
            state_dir / "factory-phase-result-round-1-verification.json"
        )
        preserved_audit = state_dir / "factory-phase-result-round-1-audit.json"
        for artifact in (preserved_verification, preserved_audit):
            text = artifact.read_text(encoding="utf-8")
            self.assertIn("[REDACTED]", text,
                          f"the preserved result must carry a redaction marker: "
                          f"{artifact.name}")
        receipt_verification = (
            state_dir / "factory-findings-receipt-round-1-verification.json"
        )
        receipt_audit = state_dir / "factory-findings-receipt-round-1-audit.json"
        verification_receipt = json.loads(
            receipt_verification.read_text(encoding="utf-8"))
        audit_receipt = json.loads(
            receipt_audit.read_text(encoding="utf-8"))
        self.assertEqual(
            verification_receipt["findings"],
            ["api_token=[REDACTED] leak in fixture"],
            "the findings receipt must bind the redacted content",
        )
        self.assertEqual(
            audit_receipt["blocked_on"],
            ["GITHUB_TOKEN=[REDACTED] external-capability"],
            "the blocked reference in the receipt must be redacted",
        )
        for artifact in (receipt_verification, receipt_audit):
            text = artifact.read_text(encoding="utf-8")
            self.assertIn("[REDACTED]", text,
                          f"the findings receipt must carry a redaction "
                          f"marker: {artifact.name}")
        # The receipts authenticate the exact preserved (redacted) content:
        # the next-round consumption path re-validates the receipt against
        # the preserved bytes, so the redaction is part of the trusted chain
        # and the next planner can only ever receive the redacted payload.
        verification_result = json.loads(
            preserved_verification.read_text(encoding="utf-8"))
        audit_result = json.loads(
            preserved_audit.read_text(encoding="utf-8"))
        self.assertEqual(verification_result["findings"],
                         verification_receipt["findings"])
        self.assertEqual(audit_result["blocked_on"],
                         audit_receipt["blocked_on"])
        # The campaign result output itself carries no raw secret.
        self.assertNotIn("super-secret-value-123", str(data))
        self.assertNotIn("ghp_secret_blocker", str(data))

    def test_planning_retries_within_budget_then_succeeds(self) -> None:
        # A planner that fails once (exit 1) then produces a plan on the
        # second attempt stays within the configured budget.
        ws = self.make({
            "planner": {"behavior": {"1.1": "exit1", "1.2": "planned",
                                     "default": "planned"}},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "pass"},
        })
        rc, data = ws.run_cli()
        self.assertEqual(rc, 0)
        assert_history(self, data, [
            (1, "planning", "failed"),
            (1, "planning", "planned"),
            (1, "implementation", "task_completed"),
            (1, "verification", "pass"),
            (1, "audit", "pass"),
        ])

    def test_clean_task_failure_reaches_verification(self) -> None:
        # A reproducible deterministic developer failure (nonzero exit with no
        # work) exhausts the attempt budget cleanly and then proceeds to
        # verification/audit at the last coherent commit (§13.3).
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "exit1"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "pass"},
        })
        rc, data = ws.run_cli()
        self.assertEqual(rc, 0)
        assert_history(self, data, [
            (1, "planning", "planned"),
            (1, "implementation", "task_failed"),
            (1, "implementation", "task_failed"),
            (1, "implementation", "task_failed"),
            (1, "verification", "pass"),
            (1, "audit", "pass"),
        ])

    def test_invalid_developer_plan_fails_closed(self) -> None:
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "invalid"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "pass"},
        })
        rc, data = ws.run_cli()
        self.assertEqual(rc, 0)
        self.assertEqual(
            [r["outcome"] for r in data["phase_history"][1:4]],
            ["task_failed", "task_failed", "task_failed"],
        )

    def test_acceptance_gate_rejects_missing_verification_file(self) -> None:
        # The default acceptance gate requires every verification reference
        # of the committed task to exist; a missing file fails the task.
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "complete-no-file"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "pass"},
        })
        rc, data = ws.run_cli()
        self.assertEqual(rc, 0)
        self.assertEqual(data["phase_history"][1]["outcome"], "task_failed")

    def test_shell_command_verification_prose_is_not_treated_as_path(self) -> None:
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "pass"},
        })
        command = "`nix-shell --run 'ctest -R fixture'`"
        for relative in (
            "fixture/templates/planner-1.md",
            "fixture/templates/dev-1.md",
        ):
            path = ws.root / relative
            text = path.read_text(encoding="utf-8")
            self.assertIn("`src/work-1.md`", text)
            path.write_text(
                text.replace("`src/work-1.md`", command), encoding="utf-8"
            )
        _git(ws.root, "add", "fixture/templates/planner-1.md",
             "fixture/templates/dev-1.md")
        _git(ws.root, "commit", "-qm", "command verification prose fixture")
        rc, data = ws.run_cli()
        self.assertEqual(rc, 0)
        self.assertEqual(data["phase_history"][1]["outcome"], "task_completed")

    def test_task_progress_retries_then_completes(self) -> None:
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": {"1.1": "progress", "1.2": "complete",
                                       "default": "complete"}},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "pass"},
        })
        rc, data = ws.run_cli()
        self.assertEqual(rc, 0)
        assert_history(self, data, [
            (1, "planning", "planned"),
            (1, "implementation", "task_progress"),
            (1, "implementation", "task_completed"),
            (1, "verification", "pass"),
            (1, "audit", "pass"),
        ])

    def test_verification_gate_failure_is_findings(self) -> None:
        ws = self.make({
            "planner": {"behavior": "planned-complete"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "findings"},
        })
        rc, data = ws.run_cli(
            extra=["--verification-command", str(FALSE_EXECUTABLE)])
        self.assertEqual(rc, 1)
        self.assertEqual(data["phase_history"][2]["phase"], "verification")
        self.assertEqual(data["phase_history"][2]["outcome"], "findings")

    def test_campaign_deadline_is_finite_positive_and_bounded(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        config = ws.derive_config()
        with self.assertRaises(campaign_module.CampaignConfigError):
            dataclasses.replace(config, campaign_timeout=0)
        with self.assertRaises(campaign_module.CampaignConfigError):
            dataclasses.replace(
                config,
                campaign_timeout=campaign_module.MAX_CAMPAIGN_TIMEOUT + 1,
            )

    def test_absent_verification_command_fails_closed(self) -> None:
        # Verification argv is a construction/preflight requirement.  Even a
        # fixture cannot create a campaign contract without its explicit safe
        # verifier, so no planner/tester role can run first.
        ws = self.make(SUCCESS_SCENARIO)
        with self.assertRaises(campaign_module.CampaignConfigError):
            dataclasses.replace(ws.derive_config(), verification_command=())
        self.assertFalse(
            (ws.root / STATE_DIR / state_module.STATE_FILE_NAME).exists()
        )

    def test_fresh_campaign_namespace_never_reads_or_overwrites_foreign_state(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        foreign_dir = ws.root / ".factory-state"
        foreign_dir.mkdir(mode=0o700)
        foreign = foreign_dir / "foreign-lifecycle.bin"
        foreign.write_bytes(b"foreign\x00bytes\xff")
        foreign.chmod(0o640)
        os.utime(foreign, ns=(1_700_000_000_000_000_000,
                              1_700_000_001_000_000_000))
        foreign_before = foreign.lstat()
        with unittest.mock.patch.object(
            campaign_module.os, "listdir",
            side_effect=AssertionError("campaign reservation enumerated state"),
        ), unittest.mock.patch.object(
            campaign_module.os, "scandir",
            side_effect=AssertionError("campaign reservation enumerated state"),
        ):
            rel = campaign_module._reserve_campaign_namespace(
                ws.root, "fresh-production-id"
            )
        namespace = ws.root / rel
        self.assertTrue(namespace.is_dir())
        self.assertEqual(stat.S_IMODE(namespace.stat().st_mode), 0o700)
        self.assertEqual(rel, ".factory-state/campaigns/fresh-production-id")
        self.assertEqual(foreign.read_bytes(), b"foreign\x00bytes\xff")
        foreign_after = foreign.lstat()
        self.assertEqual(stat.S_IMODE(foreign_after.st_mode),
                         stat.S_IMODE(foreign_before.st_mode))
        self.assertEqual(foreign_after.st_mtime_ns, foreign_before.st_mtime_ns)
        planted = namespace / "planted"
        planted.write_bytes(b"untouched")
        planted.chmod(0o600)
        with self.assertRaises(campaign_module.CampaignConfigError):
            campaign_module._reserve_campaign_namespace(
                ws.root, "fresh-production-id"
            )
        self.assertEqual(planted.read_bytes(), b"untouched")
        self.assertEqual(foreign.read_bytes(), b"foreign\x00bytes\xff")

        active_rel = campaign_module._reserve_campaign_namespace(
            ws.root, "fixture-namespaced-run"
        )
        config = dataclasses.replace(
            ws.derive_config(campaign_id="fixture-namespaced-run"),
            state_namespace=active_rel,
            phase_result_path=f"{active_rel}/phase-result.json",
            audit_result_path=f"{active_rel}/audit-result.json",
        )
        result = campaign_module.Campaign(config).run()
        self.assertEqual(result.terminal_phase, "success")
        active = ws.root / active_rel
        self.assertTrue((active / state_module.STATE_FILE_NAME).is_file())
        self.assertTrue(
            (active / "campaign-result-fixture-namespaced-run.json").is_file()
        )
        self.assertEqual(foreign.read_bytes(), b"foreign\x00bytes\xff")

    def test_namespaced_verifier_evidence_override_preserves_foreign_root_bytes(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        state_root = ws.root / STATE_DIR
        state_root.mkdir(mode=0o700)
        foreign = state_root / "installed-functional-evidence.env"
        foreign.write_bytes(b"foreign-root-evidence\x00bytes\xff")
        foreign.chmod(0o640)
        active_rel = campaign_module._reserve_campaign_namespace(
            ws.root, "evidence-override"
        )
        code = (
            "import os,pathlib; "
            "p=pathlib.Path(os.environ['FACTORY_INSTALLED_FUNCTIONAL_EVIDENCE_PATH']); "
            "assert p.as_posix().endswith('/.factory-state/campaigns/evidence-override/installed-functional-evidence.env'); "
            "p.write_bytes(b'campaign-owned-evidence')"
        )
        config = dataclasses.replace(
            ws.derive_config(campaign_id="evidence-override"),
            state_namespace=active_rel,
            phase_result_path=f"{active_rel}/phase-result.json",
            audit_result_path=f"{active_rel}/audit-result.json",
            verification_command=(sys.executable, "-c", code),
        )
        result = campaign_module.Campaign(config).run()
        self.assertEqual(result.terminal_phase, "success")
        self.assertEqual(foreign.read_bytes(), b"foreign-root-evidence\x00bytes\xff")
        self.assertEqual(
            (ws.root / active_rel / "installed-functional-evidence.env").read_bytes(),
            b"campaign-owned-evidence",
        )

    def test_campaign_namespace_requires_safe_exact_state_components(self) -> None:
        absent = self.make(SUCCESS_SCENARIO)
        with self.assertRaises(campaign_module.CampaignConfigError):
            campaign_module._reserve_campaign_namespace(absent.root, "absent-state")
        self.assertFalse((absent.root / ".factory-state").exists())

        unsafe_root = self.make(SUCCESS_SCENARIO)
        state_root = unsafe_root.root / ".factory-state"
        state_root.mkdir(mode=0o700)
        state_root.chmod(0o750)
        with self.assertRaises(campaign_module.CampaignConfigError):
            campaign_module._reserve_campaign_namespace(unsafe_root.root, "bad-mode")
        self.assertFalse((state_root / "campaigns").exists())
        self.assertEqual(stat.S_IMODE(state_root.lstat().st_mode), 0o750)

        unsafe_parent = self.make(SUCCESS_SCENARIO)
        state_root = unsafe_parent.root / ".factory-state"
        state_root.mkdir(mode=0o700)
        outside = unsafe_parent.root / "foreign-target"
        outside.mkdir(mode=0o700)
        marker = outside / "marker"
        marker.write_bytes(b"foreign-parent-bytes")
        (state_root / "campaigns").symlink_to(outside, target_is_directory=True)
        with self.assertRaises(campaign_module.CampaignConfigError):
            campaign_module._reserve_campaign_namespace(
                unsafe_parent.root, "symlink-parent"
            )
        self.assertEqual(marker.read_bytes(), b"foreign-parent-bytes")
        self.assertEqual(os.readlink(state_root / "campaigns"), str(outside))

    def test_verification_blocked_requires_declared_capability(self) -> None:
        # A tester blocked result with exact references becomes a genuine
        # verification blocked only when the declared capability probe fails;
        # without a capability command the blocked claim is a finding.
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "blocked"},
            "auditor": {"behavior": "blocked"},
        })
        rc, data = ws.run_cli(
            extra=["--capability-command", str(FALSE_EXECUTABLE)])
        self.assertEqual(rc, 2)
        self.assertEqual(data["phase_history"][2]["outcome"], "blocked")
        self.assertEqual(data["phase_history"][3]["outcome"], "blocked")

        ws2 = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "blocked"},
            "auditor": {"behavior": "findings"},
        })
        rc2, data2 = ws2.run_cli()
        self.assertEqual(rc2, 1)
        self.assertEqual(data2["phase_history"][2]["outcome"], "findings")


class SubstanceBoundaryCampaigns(_CampaignBase):
    """Meaningful-commit substance boundary end to end (BUG/TASK evidence).

    These fixtures drive the shared ``substance`` classifier through the real
    campaign authority: a planner result that only rewords non-semantic plan
    prose is still a valid ``planned`` phase, but it is never committed (no
    metadata-only revision commit, no HEAD advance, plan restored), and a
    developer that revises only the plan (complete, progress, or a crash that
    leaves only a plan revision) produces no commit and fails/retries with a
    clean restoration instead of manufacturing resume progress.  Each scenario
    asserts the exact orchestrator-authored commit set so no harness-metadata
    commit can be hidden inside substantive-looking progress.
    """

    def _committed_plan_digest(self, ws, commit: str) -> str:
        blob = _git(ws.root, "show", f"{commit}:{PLAN_REL}").stdout.encode()
        return campaign_module.plan_sha256(blob)

    def test_planner_prose_revision_plans_without_commit_or_head_advance(self) -> None:
        # A planner result that only rewords prose (non-semantic plan surface)
        # is a genuine ``planned`` phase outcome: the meaningful-substance
        # boundary records the phase WITHOUT committing a metadata-only
        # revision, leaves HEAD on the committed plan, and restores the plan
        # so the developer works the exact committed bytes.  Only the
        # developer's later substantive completion commit advances history.
        ws = self.make({
            "planner": {"behavior": "planned-prose"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "pass"},
        })
        start_head = ws.scenario_commit
        start_digest = self._committed_plan_digest(ws, start_head)
        rc, data = ws.run_cli()
        self.assertEqual(rc, 0)
        assert_terminal(self, data, terminal_phase="success",
                        terminal_outcome="pass", exit_code=0,
                        rounds_completed=1)
        assert_history(self, data, [
            (1, "planning", "planned"),
            (1, "implementation", "task_completed"),
            (1, "verification", "pass"),
            (1, "audit", "pass"),
        ])
        # The planning ``planned`` record advanced the phase without a commit
        # or HEAD move and reused the committed plan digest; a genuine semantic
        # planner revision would update both fields.
        planning = data["phase_history"][0]
        self.assertEqual(planning["outcome"], "planned")
        self.assertEqual(planning["head_commit"], start_head)
        self.assertEqual(planning["plan_digest"], start_digest)
        # No ``planning round`` commit ever landed; only the developer's one
        # substantive completion commit advanced history.
        messages = _git(ws.root, "log", f"{start_head}..HEAD",
                        "--format=%s").stdout.splitlines()
        self.assertEqual(messages, ["factory-campaign: task 1 complete"])
        # The restored plan left no worktree residue: the tree is clean.
        self.assertEqual(
            _git(ws.root, "status", "--short", "--", PLAN_REL).stdout, ""
        )

    def test_developer_plan_only_complete_fails_without_commit(self) -> None:
        # A developer that revises only the plan (marks the task complete, a
        # ``complete-no-file`` fixture) and produces no substantive
        # source/tests/evidence is not progressive: the completion claim is
        # rejected (``task_failed``), the plan-only revision is restored, and
        # nothing beyond the planner's semantic revision is ever committed.
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "complete-no-file"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "pass"},
        })
        start_head = ws.scenario_commit
        rc, data = ws.run_cli()
        self.assertEqual(rc, 0)
        self.assertEqual(
            [r["outcome"] for r in data["phase_history"][1:4]],
            ["task_failed", "task_failed", "task_failed"],
        )
        # Only the planner's genuine semantic revision was committed; no
        # implementation commit (complete/progress/resume) ever landed.
        messages = _git(ws.root, "log", f"{start_head}..HEAD",
                        "--format=%s").stdout.splitlines()
        self.assertEqual(messages, ["factory-campaign: planning round 1"])
        # The plan-only revision was restored, never committed: tree is clean.
        self.assertEqual(
            _git(ws.root, "status", "--short", "--", PLAN_REL).stdout, ""
        )

    def test_developer_plan_only_progress_fails_without_commit(self) -> None:
        # A plan-only *progress* revision (``progress-no-file``: the task is
        # marked ``in_progress`` in the plan with no substantive work) is
        # metadata, never progressive work: ``task_failed`` and restored.
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "progress-no-file"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "pass"},
        })
        start_head = ws.scenario_commit
        rc, data = ws.run_cli()
        self.assertEqual(rc, 0)
        # progress-no-file marks task 1 in_progress (not complete), so every
        # attempt is a rejected ``task_failed`` with no committed progress.
        self.assertEqual(
            [r["outcome"] for r in data["phase_history"][1:4]],
            ["task_failed", "task_failed", "task_failed"],
        )
        messages = _git(ws.root, "log", f"{start_head}..HEAD",
                        "--format=%s").stdout.splitlines()
        self.assertEqual(messages, ["factory-campaign: planning round 1"])
        self.assertEqual(
            _git(ws.root, "status", "--short", "--", PLAN_REL).stdout, ""
        )

    def test_developer_plan_only_crash_retries_clean_without_commit(self) -> None:
        # A crashed attempt that leaves only a plan revision (``plan-crash``)
        # is regenerable harness metadata, never preservable progress: the
        # orchestrator restores the plan so the retry starts clean at the
        # committed plan, never manufactures a ``resume`` commit, and records
        # a retry (``interrupted``) then deterministic ``task_failed`` on the
        # later plan-only completion attempts.
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": {
                "1.1": "plan-crash",
                "default": "complete-no-file",
            }},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "pass"},
        })
        start_head = ws.scenario_commit
        rc, data = ws.run_cli()
        self.assertEqual(rc, 0)
        self.assertEqual(
            [r["outcome"] for r in data["phase_history"][1:4]],
            ["interrupted", "task_failed", "task_failed"],
        )
        # No resume/complete/progress commit was manufactured from the
        # plan-only crash; only the planner's genuine semantic revision.
        messages = _git(ws.root, "log", f"{start_head}..HEAD",
                        "--format=%s").stdout.splitlines()
        self.assertEqual(messages, ["factory-campaign: planning round 1"])
        self.assertNotIn(
            "resume", [m for m in messages], "plan-only crash manufactured a resume commit"
        )
        self.assertEqual(
            _git(ws.root, "status", "--short", "--", PLAN_REL).stdout, ""
        )


class ScopeAndGit(_CampaignBase):
    """§12 scope authority, commit boundary, and dirty preservation."""


    def test_planner_scope_violation_terminates_failed(self) -> None:
        ws = self.make({
            "planner": {"behavior": "scope"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "pass"},
        })
        rc, data = ws.run_cli()
        self.assertEqual(rc, 3)
        self.assertEqual(data["terminal_phase"], "failed")
        # The foreign dirty file is preserved, never reset.
        self.assertTrue((ws.root / "src" / "planner-touched.py").exists())

    def test_developer_scope_violation_dirty_exhaustion_interrupts(self) -> None:
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "scope"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "pass"},
        })
        rc, data = ws.run_cli()
        self.assertEqual(rc, 4)
        self.assertEqual(data["terminal_phase"], "interrupted")
        # The harness-state violation is preserved, never removed.
        self.assertTrue((ws.root / ".factory" / "config.toml").exists())

    def test_developer_cannot_commit_trusted_policy_surface(self) -> None:
        # Task 9 review HIGH: an untrusted developer that tampers with the
        # trusted policy/harness surface (operational policy, harness docs,
        # legacy security scripts) is a deterministic scope violation: the
        # dirty work is preserved but the orchestrator never commits any of
        # it, and the campaign fails closed on budget exhaustion.
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "scope-policy"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "pass"},
        })
        rc, data = ws.run_cli()
        self.assertEqual(rc, 4)
        self.assertEqual(data["terminal_phase"], "interrupted")
        # Every tampered policy/harness path stays dirty and preserved.
        for rel in ("AGENTS.md", "docs/FACTORY.md", "scripts/guard.sh"):
            self.assertTrue((ws.root / rel).exists())
        self.assertIn(
            "dirty fixture work",
            (ws.root / "AGENTS.md").read_text(encoding="utf-8"),
        )
        # No campaign commit after the scenario touches the policy surface.
        self.assertIsNotNone(ws.scenario_commit)
        heads = _git(ws.root, "log", f"{ws.scenario_commit}..HEAD",
                     "--format=%H").stdout.splitlines()
        for entry in heads:
            files = _git(ws.root, "diff-tree", "--no-commit-id",
                         "--name-only", "-r", entry).stdout.splitlines()
            for path in files:
                self.assertNotIn(
                    path,
                    ("AGENTS.md", "docs/FACTORY.md", "scripts/guard.sh"),
                    f"campaign commit touched trusted policy path {path!r}",
                )

    def test_dirty_work_is_preserved(self) -> None:
        # §17: dirty work is never reset, discarded, or silently overwritten.
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "crash"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "pass"},
        })
        rc, data = ws.run_cli()
        self.assertEqual(rc, 4)
        work = ws.root / "src" / "work-1.md"
        self.assertTrue(work.exists())
        # The fixture's crash behavior appends one crash-attempt marker per
        # developer attempt; the dirty work must survive byte-identically.
        self.assertIn("crash-attempt-1\n", work.read_text(encoding="utf-8"))

    def test_every_campaign_commit_is_orchestrator_authored(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        rc, _ = ws.run_cli()
        self.assertEqual(rc, 0)
        self.assertIsNotNone(ws.scenario_commit)
        # Only the commits the orchestrator created during the campaign are
        # in scope; the fixture's own setup commits carry the fixture
        # identity and predate the scenario fixture.
        log = _git(ws.root, "log", f"{ws.scenario_commit}..HEAD",
                   "--format=%an <%ae>").stdout.splitlines()
        self.assertTrue(log)
        for entry in log:
            self.assertEqual(
                entry, "factory-campaign <factory-campaign@localhost>",
                msg=f"unexpected commit author {entry!r}")

    def test_committed_scope_is_exactly_the_allowed_paths(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        rc, _ = ws.run_cli()
        self.assertEqual(rc, 0)
        self.assertIsNotNone(ws.scenario_commit)
        heads = _git(ws.root, "log", f"{ws.scenario_commit}..HEAD",
                     "--format=%H").stdout.splitlines()
        self.assertTrue(heads)
        for entry in heads:
            files = _git(ws.root, "diff-tree", "--no-commit-id",
                         "--name-only", "-r", entry).stdout.splitlines()
            for path in files:
                self.assertFalse(
                    path.startswith(".git/"),
                    f"campaign commit touched {path!r}")
                self.assertTrue(
                    path == PLAN_REL or not path.startswith(".factory/"),
                    f"campaign commit touched harness path {path!r}")

    def test_no_foreign_commit_during_campaign(self) -> None:
        # The only commits after the scenario fixture are orchestrator
        # commits; the fixture role never runs Git (the driver performs no
        # git operation at all).
        ws = self.make(SUCCESS_SCENARIO)
        rc, _ = ws.run_cli()
        self.assertEqual(rc, 0)
        messages = _git(ws.root, "log", "--format=%s").stdout.splitlines()
        for message in messages:
            self.assertTrue(
                message.startswith("factory-campaign: ")
                or message in ("fixture base", "fixture plan and driver",
                               "scenario fixture"),
                msg=f"unexpected commit {message!r}")


class LifecycleAndCli(_CampaignBase):
    """§11 one-lifecycle surface, committed schema, and CLI behavior."""

    def test_no_runtime_ledger_is_created(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        rc, _ = ws.run_cli()
        self.assertEqual(rc, 0)
        names = sorted(path.name for path in (ws.root / STATE_DIR).iterdir())
        self.assertEqual(names, [
            "campaign-result-campaign.json",
            "factory-loop.json",
            "state-digest-ledger.jsonl",
            "state-floor.json",
        ])

    def test_published_result_conforms_to_committed_schema(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        rc, data = ws.run_cli()
        self.assertEqual(rc, 0)
        published = json.loads(
            ws.result_file().read_text(encoding="utf-8"))
        campaign_module.validate_campaign_result(published)
        # The published bytes equal the CLI's printed result (single writer).
        self.assertEqual(published, data)

    def test_control_state_is_exactly_the_section11_field_set(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        rc, _ = ws.run_cli()
        self.assertEqual(rc, 0)
        state = ws.load_state()
        keys = sorted(state.to_dict())
        self.assertEqual(keys, sorted(state_module.FIELD_NAMES))

    def test_write_once_campaign_binding_fails_closed(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        rc, _ = ws.run_cli()
        self.assertEqual(rc, 0)
        # A second campaign with a different id must not clobber the binding.
        rc2, data2 = ws.run_cli(campaign_id="other")
        self.assertEqual(rc2, 6)
        self.assertIsNone(data2)

    def test_terminal_state_refuses_to_rerun(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        config = ws.derive_config()
        rc, _ = ws.run_cli()
        self.assertEqual(rc, 0)
        with self.assertRaises(campaign_module.CampaignPhaseError):
            campaign_module.Campaign(config).run()

    def test_cli_rejects_unknown_provider(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        result = run(
            [sys.executable, str(LOOP / "campaign.py"),
             "--root", str(ws.root), "run",
             "--campaign-id", "x", "--rounds", "1",
             "--branch", BRANCH, "--provider", "bogus"],
            root=ROOT, check=False)
        self.assertEqual(result.returncode, 6)
        self.assertIn("factory-campaign:", result.stderr)

    def test_production_cli_requires_all_commands_and_deadline(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        result = run(
            [sys.executable, str(LOOP / "campaign.py"),
             "--root", str(ws.root), "run", "--campaign-id", "x",
             "--rounds", "5", "--branch", BRANCH,
             "--verification-command", str(TRUE_EXECUTABLE)],
            root=ROOT, check=False,
        )
        self.assertEqual(result.returncode, 6)
        self.assertIn("--capability-command", result.stderr)
        self.assertIn("--acceptance-command", result.stderr)
        self.assertIn("--runner-command", result.stderr)
        self.assertFalse((ws.root / STATE_DIR / state_module.STATE_FILE_NAME).exists())

        result = run(
            [sys.executable, str(LOOP / "campaign.py"),
             "--root", str(ws.root), "run", "--campaign-id", "x",
             "--rounds", "5", "--branch", BRANCH,
             "--verification-command", "./scripts/verify.sh",
             "--capability-command", "./scripts/capability.sh",
             "--runner-command", "./.factory/runner/run-factory-runners.py",
             "--acceptance-command", "./scripts/acceptance.sh"],
            root=ROOT, check=False,
        )
        self.assertEqual(result.returncode, 6)
        self.assertIn("--campaign-timeout", result.stderr)

    def test_production_cli_requires_explicit_provider_model_backend(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        result = run(
            [sys.executable, str(LOOP / "campaign.py"),
             "--root", str(ws.root), "run", "--campaign-id", "x",
             "--rounds", "5", "--branch", BRANCH,
             "--campaign-timeout", "60",
             "--verification-command", "./scripts/verify.sh",
             "--capability-command", "./scripts/capability.sh",
             "--runner-command", "./.factory/runner/run-factory-runners.py",
             "--acceptance-command", "./scripts/acceptance.sh"],
            root=ROOT, check=False,
        )
        self.assertEqual(result.returncode, 6)
        self.assertIn("explicit --provider, --model, --backend", result.stderr)

    def test_cli_main_catches_git_boundary_error(self) -> None:
        # Task 9 review MED: a pinned-Git failure (timeout, missing binary,
        # broken pipe) during config derivation is a clean fail-closed
        # control-plane error, never an unhandled GitBoundaryError traceback.
        ws = self.make(SUCCESS_SCENARIO)
        import contextlib
        import io
        with unittest.mock.patch.object(
            campaign_module.gitutil, "git_run",
            side_effect=campaign_module.gitutil.GitBoundaryError(
                "pinned Git hung"),
        ):
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                rc = campaign_module.main([
                    "--root", str(ws.root), "run",
                    "--campaign-id", "x", "--rounds", "1",
                    "--branch", BRANCH,
                    "--provider", "synthetic", "--model", "test-model",
                    "--role-driver", DRIVER_REL, "--scenario", "scenario.json",
                    "--verification-command", str(TRUE_EXECUTABLE),
                ])
        self.assertEqual(rc, 6)
        self.assertIn("pinned Git hung", stderr.getvalue())

    def test_cli_rejects_role_driver_with_ollama_provider(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        result = run(
            [sys.executable, str(LOOP / "campaign.py"),
             "--root", str(ws.root), "run",
             "--campaign-id", "x", "--rounds", "1",
             "--branch", BRANCH, "--provider", "ollama",
             "--role-driver", DRIVER_REL, "--scenario", "scenario.json"],
            root=ROOT, check=False)
        self.assertEqual(result.returncode, 6)
        self.assertIn("fixture surface", result.stderr)

    def test_cli_help_omits_foreign_state_show(self) -> None:
        result = run(
            [sys.executable, str(LOOP / "campaign.py"), "--help"],
            root=ROOT, check=False)
        self.assertEqual(result.returncode, 0)
        self.assertIn("factory-campaign", result.stdout)
        self.assertIn("{run}", result.stdout)
        self.assertNotIn("\n    show ", result.stdout)
        result = run(
            [sys.executable, str(LOOP / "campaign.py"), "show"],
            root=ROOT, check=False)
        self.assertEqual(result.returncode, 2)
        self.assertIn("invalid choice", result.stderr)

    def test_malformed_phase_result_fails_closed(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        bad = ws.root / STATE_DIR / "bad-result.json"
        bad.parent.mkdir(parents=True, exist_ok=True)
        bad.write_text(
            '{"schema": "factory-phase-result/v1", "outcome": "bogus"}',
            encoding="utf-8")
        with self.assertRaises(campaign_module.CampaignResultError):
            campaign_module.read_phase_result(
                ws.root, f"{STATE_DIR}/bad-result.json", "fixture")

    def test_pass_phase_result_rejects_findings_and_blockers(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        state = ws.root / STATE_DIR
        state.mkdir(parents=True, exist_ok=True)
        for name, field in (("findings", "findings"), ("blocked", "blocked_on")):
            path = state / f"pass-with-{name}.json"
            path.write_text(json.dumps({
                "schema": "factory-phase-result/v1", "outcome": "pass",
                field: ["must not accompany pass"],
            }), encoding="utf-8")
            with self.assertRaises(campaign_module.CampaignResultError):
                campaign_module.read_phase_result(
                    ws.root, f"{STATE_DIR}/{path.name}", "fixture"
                )
            self.assertFalse(path.exists())

    def test_malformed_secret_result_leaves_no_bytes_or_path(self) -> None:
        """Task 23: the transient raw result is secure-unlinked in a finally
        on parse/schema/oversize/error paths — a malformed secret-laden
        result leaves no bytes and no path; a substituted pathname (the
        role rewriting the file between the read and the cleanup) is never
        deleted."""
        ws = self.make(SUCCESS_SCENARIO)
        state = ws.root / STATE_DIR
        state.mkdir(parents=True, exist_ok=True)
        # Schema-invalid content carrying a raw secret candidate is removed.
        bad = state / "secret-schema.json"
        bad.write_text(
            '{"schema": "factory-phase-result/v1", "outcome": "bogus", '
            '"detail": "api_token=super-secret-malformed-123"}',
            encoding="utf-8")
        with self.assertRaises(campaign_module.CampaignResultError):
            campaign_module.read_phase_result(
                ws.root, f"{STATE_DIR}/secret-schema.json", "fixture")
        self.assertFalse(bad.exists(),
                         "a schema-invalid secret result must leave no path")
        # Non-JSON content carrying a raw secret candidate is removed.
        not_json = state / "secret-not-json.json"
        not_json.write_text(
            "GITHUB_TOKEN=ghp_secret_not_json !!! ", encoding="utf-8")
        with self.assertRaises(campaign_module.CampaignResultError):
            campaign_module.read_phase_result(
                ws.root, f"{STATE_DIR}/secret-not-json.json", "fixture")
        self.assertFalse(not_json.exists(),
                         "a non-JSON secret result must leave no path")
        # An oversized result is removed too.
        oversized = state / "secret-oversize.json"
        oversized.write_text(
            '{"schema": "factory-phase-result/v1", "detail": "'
            + "api_token=oversize-secret " + "x" * (campaign_module.MAX_RESULT_FILE + 64)
            + '"}',
            encoding="utf-8")
        with self.assertRaises(campaign_module.CampaignResultError):
            campaign_module.read_phase_result(
                ws.root, f"{STATE_DIR}/secret-oversize.json", "fixture")
        self.assertFalse(oversized.exists(),
                         "an oversized secret result must leave no path")
        # A symlinked handoff is never followed (O_NOFOLLOW) and never
        # deleted by the secure cleanup: the open fails closed and the
        # symlink and its target stay untouched.
        link = state / "symlinked-result.json"
        target = state / "symlink-target.json"
        target.write_text("{}", encoding="utf-8")
        os.symlink(target, link)
        with self.assertRaises(campaign_module.CampaignResultError):
            campaign_module.read_phase_result(
                ws.root, f"{STATE_DIR}/symlinked-result.json", "fixture")
        self.assertTrue(link.is_symlink(),
                        "a symlinked handoff must never be deleted")
        self.assertTrue(target.exists(), "the symlink target must never be deleted")
        # A successful read still removes the consumed handoff.
        ok = state / "ok-result.json"
        ok.write_text(
            '{"schema": "factory-phase-result/v1", "outcome": "pass"}',
            encoding="utf-8")
        data, digest, raw = campaign_module.read_phase_result(
            ws.root, f"{STATE_DIR}/ok-result.json", "fixture")
        self.assertEqual(data["outcome"], "pass")
        self.assertEqual(len(digest), 64)
        self.assertFalse(ok.exists(), "a consumed handoff must be removed")

    def test_result_model_validation_fails_closed(self) -> None:
        result = campaign_module.CampaignResult(
            campaign_id="campaign", rounds_requested=1, rounds_completed=0,
            terminal_phase="success", terminal_outcome="pass",
            head_commit="0" * 40,
            phase_history=(
                campaign_module.PhaseRecord(
                    round=1, phase="audit", attempt=1, outcome="bogus",
                    head_commit="0" * 40, plan_digest="0" * 64,
                ),
            ),
        )
        with self.assertRaises(campaign_module.CampaignResultError):
            result.validate()

    def test_config_validation_fails_closed(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        config = ws.derive_config()
        with self.assertRaises(campaign_module.CampaignConfigError):
            dataclasses.replace(config, rounds_requested=0)
        with self.assertRaises(campaign_module.CampaignConfigError):
            dataclasses.replace(config, planning_attempts=0)
        with self.assertRaises(campaign_module.CampaignConfigError):
            dataclasses.replace(config, provider="bogus")

    def test_unbound_plan_is_rejected_at_planning(self) -> None:
        # A planner revision that changes the bound base commit is unbound and
        # fails the planning phase (PLAN-01 acceptance boundary).
        ws = self.make({
            "planner": {"behavior": "planned-unbound"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "pass"},
        })
        rc, data = ws.run_cli()
        self.assertEqual(rc, 3)
        self.assertEqual(data["terminal_phase"], "failed")


class DeveloperEvidenceBoundary(_CampaignBase):
    """The developer-evidence artifact is the private evidence-smoke seam.

    ``developer_evidence_path`` may be bound only to exactly the one
    designated evidence-smoke artifact, and only by a config that is the
    designated evidence-smoke lane (synthetic committed role driver and an
    ``evidence-smoke-`` campaign-id seam label).  A production campaign
    (``role_driver is None``), an arbitrary ``.factory/artifacts`` path, a
    non-synthetic provider, or an ordinary/production campaign id is rejected
    at construction, so no source-methodology evidence can ever be created
    outside the exact smoke seam.
    """

    def test_exact_smoke_seam_is_accepted(self) -> None:
        # The designated synthetic committed-driver evidence-smoke seam
        # validates at construction and keeps the exact artifact binding.
        ws = self.make(SUCCESS_SCENARIO)
        config = ws.derive_config(campaign_id="evidence-smoke-deadbeef")
        bound = dataclasses.replace(
            config,
            developer_evidence_path=campaign_module.EVIDENCE_SMOKE_EVIDENCE_REL,
        )
        self.assertEqual(
            bound.developer_evidence_path,
            campaign_module.EVIDENCE_SMOKE_EVIDENCE_REL,
        )
        self.assertEqual(bound.provider, "synthetic")
        self.assertTrue(bound.role_driver)
        self.assertTrue(
            bound.campaign_id.startswith("evidence-smoke-"), bound.campaign_id
        )

    def test_rejects_arbitrary_evidence_artifact_paths(self) -> None:
        # Only the one designated artifact is ever accepted; no alternative
        # or normalized ``.factory/artifacts`` path (sibling, suffix, or
        # directory marker) can reach the smoke exception.
        ws = self.make(SUCCESS_SCENARIO)
        config = ws.derive_config(campaign_id="evidence-smoke-deadbeef")
        for path in (
            ".factory/artifacts/other-evidence.json",
            ".factory/artifacts/campaign-smoke-evidence.json.bak",
            ".factory/artifacts/",
            ".factory/artifacts/campaign-smoke-evidence.json/",
            ".factory/artifacts",
        ):
            with self.subTest(path=path):
                with self.assertRaises(campaign_module.CampaignConfigError):
                    dataclasses.replace(
                        config, developer_evidence_path=path
                    )

    def test_rejects_production_without_committed_driver(self) -> None:
        # A production campaign (``role_driver is None``) can never bind a
        # developer evidence artifact, even when it names the designated path.
        ws = self.make(SUCCESS_SCENARIO)
        config = ws.derive_config(campaign_id="evidence-smoke-deadbeef")
        with self.assertRaises(campaign_module.CampaignConfigError):
            dataclasses.replace(
                config,
                developer_evidence_path=campaign_module.EVIDENCE_SMOKE_EVIDENCE_REL,
                role_driver=None,
            )

    def test_rejects_ordinary_campaign_id(self) -> None:
        # The ``evidence-smoke-`` seam label is mandatory; an ordinary or
        # production campaign id can never bind a developer evidence artifact.
        ws = self.make(SUCCESS_SCENARIO)
        config = ws.derive_config()  # default campaign id: "campaign"
        with self.assertRaises(campaign_module.CampaignConfigError):
            dataclasses.replace(
                config,
                developer_evidence_path=campaign_module.EVIDENCE_SMOKE_EVIDENCE_REL,
            )

    def test_rejects_non_synthetic_provider(self) -> None:
        # The evidence-smoke seam is synthetic only; an external model never
        # publishes smoke evidence.
        ws = self.make(SUCCESS_SCENARIO)
        config = ws.derive_config(campaign_id="evidence-smoke-deadbeef")
        with self.assertRaises(campaign_module.CampaignConfigError):
            dataclasses.replace(
                config,
                developer_evidence_path=campaign_module.EVIDENCE_SMOKE_EVIDENCE_REL,
                provider="anthropic",
            )


class ClassificationUnits(_CampaignBase):
    """§13 classification is a pure function of trusted inputs."""

    def test_planning_classification(self) -> None:
        planned = campaign_module.RoleOutcome("planner", 0)
        # A valid, scope-clean, exit-0 planner result is ``planned`` whether
        # or not its plan bytes changed; whether it also commits is decided
        # separately from the semantic-plan fingerprint in the phase step.
        self.assertEqual(campaign_module.classify_planning(
            role=planned, plan_valid=True, scope_ok=True),
            "planned")
        self.assertEqual(campaign_module.classify_planning(
            role=planned, plan_valid=False, scope_ok=True),
            "failed")
        self.assertEqual(campaign_module.classify_planning(
            role=planned, plan_valid=True, scope_ok=False),
            "failed")
        self.assertEqual(campaign_module.classify_planning(
            role=campaign_module.RoleOutcome("planner", 7),
            plan_valid=True, scope_ok=True), "failed")
        # BLOCKER 4: a round-1 planner that received a readiness findings
        # payload reflects it in the plan before the attempt can be planned.
        self.assertEqual(campaign_module.classify_planning(
            role=planned, plan_valid=True, scope_ok=True,
            findings_reflected=False), "failed")
        interrupted = campaign_module.RoleOutcome(
            "planner", -9, interrupted=True, signal="SIGKILL")
        self.assertEqual(campaign_module.classify_planning(
            role=interrupted, plan_valid=True, scope_ok=True), "interrupted")

    def test_implementation_classification(self) -> None:
        ok = campaign_module.RoleOutcome("developer", 0)
        base = dict(role=ok, plan_valid=True, task_complete=False,
                    acceptance_pass=False, had_substantive=True, scope_ok=True)
        self.assertEqual(campaign_module.classify_implementation(
            **{**base, "task_complete": True, "acceptance_pass": True,
               "had_substantive": True}), "task_completed")
        self.assertEqual(campaign_module.classify_implementation(
            **{**base, "task_complete": True, "acceptance_pass": False,
               "had_substantive": True}), "task_failed")
        self.assertEqual(campaign_module.classify_implementation(**base),
                         "task_progress")
        # No-substance (plan/bug/audit/evidence-sidecar-only) work is never
        # progressive: it can neither complete a task nor record progress.
        self.assertEqual(campaign_module.classify_implementation(
            **{**base, "had_substantive": False}), "task_failed")
        self.assertEqual(campaign_module.classify_implementation(
            **{**base, "scope_ok": False}), "task_failed")
        self.assertEqual(campaign_module.classify_implementation(
            **{**base, "role": campaign_module.RoleOutcome("developer", -9)}),
            "interrupted")

    def test_verification_classification(self) -> None:
        ok = campaign_module.RoleOutcome("tester", 0)
        base = dict(role=ok, scope_ok=True, gate_ran=True, gate_exit=0,
                    tester_result_valid=True, tester_result_outcome="pass",
                    findings=(), blocked_refs=(), capability_available=True)
        self.assertEqual(campaign_module.classify_verification(**base), "pass")
        self.assertEqual(campaign_module.classify_verification(
            **{**base, "gate_exit": 1}), "findings")
        for command_failure in (-1, 126, 127):
            with self.subTest(gate_command_failure=command_failure):
                self.assertEqual(campaign_module.classify_verification(
                    **{**base, "gate_exit": command_failure}),
                    "infrastructure_failure")
        self.assertEqual(campaign_module.classify_verification(
            **{**base, "capability_ran": True, "capability_exit": 126,
               "capability_available": False,
               "tester_result_outcome": "blocked", "blocked_refs": ["ext"]}),
            "infrastructure_failure")
        self.assertEqual(campaign_module.classify_verification(
            **{**base, "capability_ran": True, "capability_exit": 1,
               "capability_available": False,
               "tester_result_outcome": "blocked", "blocked_refs": ["ext"]}),
            "blocked")
        self.assertEqual(campaign_module.classify_verification(
            **{**base, "gate_skipped": True}), "findings")
        self.assertEqual(campaign_module.classify_verification(
            **{**base, "gate_ran": False, "gate_skipped": True}),
            "infrastructure_failure")
        self.assertEqual(campaign_module.classify_verification(
            **{**base, "gate_skipped": True, "tester_result_valid": False}),
            "infrastructure_failure")
        self.assertEqual(campaign_module.classify_verification(
            **{**base, "capability_skipped": True,
               "capability_available": False}), "findings")
        self.assertEqual(campaign_module.classify_verification(
            **{**base, "capability_skipped": True,
               "capability_available": False,
               "tester_result_outcome": "blocked", "blocked_refs": ["ext"]}),
            "blocked")
        self.assertEqual(campaign_module.classify_verification(
            **{**base, "tester_result_outcome": "findings"}), "findings")
        # Task 9 review MED: an absent deterministic verification command is
        # an unrun gate — the tester JSON alone never gates.
        self.assertEqual(campaign_module.classify_verification(
            **{**base, "gate_ran": False}), "infrastructure_failure")
        self.assertEqual(campaign_module.classify_verification(
            **{**base, "gate_ran": False,
               "tester_result_outcome": "findings"}),
            "infrastructure_failure")
        self.assertEqual(campaign_module.classify_verification(
            **{**base, "tester_result_outcome": "blocked",
               "blocked_refs": ["ext"], "capability_available": False}),
            "blocked")
        self.assertEqual(campaign_module.classify_verification(
            **{**base, "tester_result_outcome": "blocked",
               "blocked_refs": ["ext"], "capability_available": True}),
            "findings")
        self.assertEqual(campaign_module.classify_verification(
            **{**base, "tester_result_valid": False}),
            "infrastructure_failure")
        self.assertEqual(campaign_module.classify_verification(
            **{**base, "role": campaign_module.RoleOutcome("tester", 9)}),
            "infrastructure_failure")
        self.assertEqual(campaign_module.classify_verification(
            **{**base, "tester_result_outcome": "blocked",
               "findings": ["higher precedence"], "blocked_refs": ["ext"],
               "capability_available": False}), "findings")

    def test_audit_classification(self) -> None:
        ok = campaign_module.RoleOutcome("auditor", 0)
        self.assertEqual(campaign_module.classify_audit(
            role=ok, scope_ok=True, result_valid=True, outcome="pass",
            findings=(), blocked_refs=()), "pass")
        self.assertEqual(campaign_module.classify_audit(
            role=ok, scope_ok=True, result_valid=True, outcome="pass",
            findings=("f",), blocked_refs=()), "findings")
        self.assertEqual(campaign_module.classify_audit(
            role=ok, scope_ok=True, result_valid=True, outcome="pass",
            findings=(), blocked_refs=("b",)), "blocked")
        self.assertEqual(campaign_module.classify_audit(
            role=ok, scope_ok=True, result_valid=True, outcome="pass",
            findings=("f",), blocked_refs=("b",)), "findings")
        interrupted = campaign_module.RoleOutcome(
            "auditor", -9, interrupted=True, signal="SIGKILL")
        self.assertEqual(campaign_module.classify_audit(
            role=interrupted, scope_ok=True, result_valid=True,
            outcome=None, findings=(), blocked_refs=()), "interrupted")
        self.assertEqual(campaign_module.classify_audit(
            role=campaign_module.RoleOutcome("auditor", 4), scope_ok=True,
            result_valid=True, outcome="pass", findings=(), blocked_refs=()),
            "infrastructure_failure")

    # -- Phase 3: conformance sidecar consultation --------------------------

    def _write_conformance(self, rows):
        import json as _json
        path = Path(tempfile.mkdtemp(prefix="conformance.")) / "conformance.json"
        path.write_text(_json.dumps({
            "schema": "ralph-conformance/v1",
            "requirements": rows,
        }), encoding="utf-8")
        return path

    def test_conformance_partial_rows_produce_findings(self) -> None:
        # When conformance.json carries partial rows, the conformance
        # consultation yields explicit findings even when the deterministic
        # verify-project.sh gate passed and the tester claimed pass. The
        # campaign therefore classifies verification as ``findings`` instead
        # of silently accepting partial conformance.
        path = self._write_conformance([
            {"id": "ARCH-01", "classification": "verified", "reason": ""},
            {"id": "MGR-02", "classification": "partial",
             "reason": "Real InputPlumber system-bus acceptance pending."},
            {"id": "DOD-05", "classification": "partial",
             "reason": "Licensed diagram selection unverified."},
        ])
        findings = campaign_module._read_conformance_findings(path)
        self.assertEqual(len(findings), 2)
        self.assertTrue(
            all(f.startswith("conformance ") and "status=partial" in f
                for f in findings),
            findings,
        )
        self.assertTrue(
            any("conformance MGR-02 status=partial" in f for f in findings),
            findings,
        )
        # Fed into the pure classifier with a passing gate and an
        # optimistic tester pass, conformance findings must yield findings.
        ok = campaign_module.RoleOutcome("tester", 0)
        self.assertEqual(
            campaign_module.classify_verification(
                role=ok, scope_ok=True, gate_ran=True, gate_exit=0,
                tester_result_valid=True, tester_result_outcome="pass",
                findings=findings, blocked_refs=[],
                capability_available=True,
            ),
            "findings",
        )

    def test_conformance_all_verified_allows_pass(self) -> None:
        # When every conformance row is verified, the consultation adds no
        # findings, so a passing gate + tester pass still classifies pass.
        path = self._write_conformance([
            {"id": "ARCH-01", "classification": "verified", "reason": ""},
            {"id": "MGR-02", "classification": "verified", "reason": ""},
            {"id": "DOD-05", "classification": "verified", "reason": ""},
        ])
        self.assertEqual(campaign_module._read_conformance_findings(path), [])
        ok = campaign_module.RoleOutcome("tester", 0)
        self.assertEqual(
            campaign_module.classify_verification(
                role=ok, scope_ok=True, gate_ran=True, gate_exit=0,
                tester_result_valid=True, tester_result_outcome="pass",
                findings=[], blocked_refs=[], capability_available=True,
            ),
            "pass",
        )

    def test_missing_conformance_json_fails_closed(self) -> None:
        missing = Path(tempfile.mkdtemp(prefix="no-conformance.")) / "missing.json"
        self.assertFalse(missing.exists())
        with self.assertRaises(campaign_module.ConformanceParseError):
            campaign_module._read_conformance_findings(missing)

    def test_malformed_conformance_json_is_infrastructure_failure(self) -> None:
        # A malformed conformance sidecar means the verifier itself is
        # broken: the helper raises ConformanceParseError and the campaign
        # wiring maps that to ``infrastructure_failure`` (never a silent
        # acceptance and never a product finding).
        import json as _json
        bad = Path(tempfile.mkdtemp(prefix="bad-conformance.")) / "conformance.json"
        bad.write_text("{ this is not valid json ", encoding="utf-8")
        with self.assertRaises(campaign_module.ConformanceParseError):
            campaign_module._read_conformance_findings(bad)
        # A document missing the requirements array is also malformed.
        bad2 = Path(tempfile.mkdtemp(prefix="bad2-conformance.")) / "conformance.json"
        bad2.write_text(_json.dumps({"schema": "ralph-conformance/v1"}),
                        encoding="utf-8")
        with self.assertRaises(campaign_module.ConformanceParseError):
            campaign_module._read_conformance_findings(bad2)
        for payload in (
            {"schema": "wrong", "requirements": [{"id": "A-01", "classification": "verified"}]},
            {"schema": "ralph-conformance/v1", "requirements": []},
            {"schema": "ralph-conformance/v1", "requirements": [{"id": "A-01", "status": "verified"}]},
            {"schema": "ralph-conformance/v1", "requirements": [
                {"id": "A-01", "classification": "verified"},
                {"id": "A-01", "classification": "partial"},
            ]},
        ):
            path = self._write_conformance(payload.get("requirements", []))
            path.write_text(_json.dumps(payload), encoding="utf-8")
            with self.assertRaises(campaign_module.ConformanceParseError):
                campaign_module._read_conformance_findings(path)


class ReviewHardening(_CampaignBase):
    """Task 9 review remediation adversarial tests (B1/B2/M1/M2/L1/L2/L3/L4)."""

    # -- B1: persisted audit terminal + rerun refusal -------------------------

    def test_interrupted_audit_is_persisted_and_refuses_rerun(self) -> None:
        # An interrupted audit ends the campaign and is persisted in the
        # authoritative control state as the terminal `interrupted` phase
        # (the round never advances); a later run refuses to re-execute it.
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "crash"},
        })
        config = ws.derive_config()
        rc, data = ws.run_cli()
        self.assertEqual(rc, 4)
        self.assertEqual(data["terminal_phase"], "interrupted")
        state = ws.load_state()
        self.assertEqual(state.current_phase, "interrupted")
        self.assertEqual(state.last_outcome, "interrupted")
        self.assertEqual(state.current_round, 1)
        head_before = _git(ws.root, "rev-parse", "HEAD").stdout.strip()
        with self.assertRaises(campaign_module.CampaignPhaseError) as cm:
            campaign_module.Campaign(config).run()
        self.assertIn("already terminal", str(cm.exception))
        # The refused rerun creates no commit and never re-executes the audit.
        self.assertEqual(
            _git(ws.root, "rev-parse", "HEAD").stdout.strip(), head_before)
        rc2, data2 = ws.run_cli()
        self.assertEqual(rc2, 6)
        self.assertIsNone(data2)

    def test_untrusted_audit_is_persisted_and_refuses_rerun(self) -> None:
        # An untrusted audit (infrastructure_failure) is likewise a terminal
        # fail-closed close persisted in the control state.
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "complete"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "dirty"},
        })
        config = ws.derive_config()
        rc, data = ws.run_cli()
        self.assertEqual(rc, 5)
        self.assertEqual(data["terminal_phase"], "infrastructure_failure")
        state = ws.load_state()
        self.assertEqual(state.current_phase, "infrastructure_failure")
        self.assertEqual(state.last_outcome, "infrastructure_failure")
        self.assertEqual(state.current_round, 1)
        with self.assertRaises(campaign_module.CampaignPhaseError) as cm:
            campaign_module.Campaign(config).run()
        self.assertIn("already terminal", str(cm.exception))

    # -- B2: production (non-driver) launch re-derives exact bytes -----------

    def _production_config(self, ws: FixtureWorkspace):
        # The real (non-driver) launch path requires a committed absolute
        # backend; the fixture driver file is an existing committed regular
        # file, and authorize_launch is mocked so no process is spawned.
        return dataclasses.replace(
            ws.derive_config(), backend=str(ws.root / DRIVER_REL))

    def _full_production_config(self, ws: FixtureWorkspace):
        """A real production (non-driver) config that passes validation.

        ``_bind_command_closure`` is reached only on the production path
        (``role_driver is None``), so this helper builds the exact committed
        verification authority, canonical capability/acceptance/runner
        commands, a fresh private namespace, an exact accepted commit, and an
        absolute install manifest that ``CampaignConfig.validate`` requires.
        """
        (ws.root / ".factory/config.toml").write_text(
            '[verification]\ncampaign_command = ["' + str(TRUE_EXECUTABLE) + '"]\n',
            encoding="utf-8",
        )
        _git(ws.root, "add", ".factory/config.toml")
        _git(ws.root, "commit", "-qm", "add committed verification authority")
        head = _git(ws.root, "rev-parse", "HEAD").stdout.strip()
        return dataclasses.replace(
            ws.derive_config(),
            provider="ollama",
            model="fixture-real-model",
            rounds_requested=5,
            backend=str(ws.root / DRIVER_REL),
            role_driver=None,
            acceptance_command=campaign_module.CANONICAL_FINAL_ACCEPTANCE_COMMAND,
            capability_command=campaign_module.CANONICAL_CAPABILITY_COMMAND,
            runner_command=campaign_module.RUNNER_COMMAND,
            state_namespace=".factory-state/campaigns/campaign",
            accepted_commit=head,
            install_manifest=str(ws.root / "fixture-install-manifest.json"),
            phase_result_path=".factory-state/campaigns/campaign/phase-result.json",
            audit_result_path=".factory-state/campaigns/campaign/audit-result.json",
        )

    def test_bind_command_closure_production_path_imports_subprocess(self) -> None:
        """Regression (BUG-0025): the real ``_bind_command_closure`` runs.

        The installed production coordinator reached
        ``campaign._bind_command_closure``, which calls ``subprocess.run`` to
        import the accepted commit into the staged closure, but ``subprocess``
        was not imported, raising ``NameError`` before any runner launch.  This
        test drives the real ``_bind_command_closure`` code path with a real
        Git authority and a real committed closure tree (never a mocked-away
        path), so a missing ``subprocess`` import fails the test.
        """
        ws = self.make(SUCCESS_SCENARIO)
        config = self._full_production_config(ws)
        campaign = campaign_module.Campaign(config)
        # The private campaign namespace must already exist (the production
        # preflight reserves it before acquisition); create it exactly as the
        # coordinator does so ``_bind_command_closure`` can stage its closure.
        namespace = ws.root / config.state_namespace
        namespace.mkdir(mode=0o700, parents=True, exist_ok=True)
        # Set up the real lock + Git authority exactly as ``_acquire`` does,
        # then invoke the real ``_bind_command_closure`` (not a mock).
        spec = campaign_module.lock_module.SpecBinding(
            config.spec_path, config.spec_commit, config.spec_blob)
        spec.validate()
        head = campaign_module._live_head(ws.root)
        plan = campaign_module.lock_module.PlanBinding(
            config.plan_path, head,
            campaign_module.plan_sha256(
                campaign_module._blob_at(ws.root, config.plan_path)))
        plan.validate()
        campaign._lock = campaign_module.lock_module.RootLock(
            ws.root,
            expected_identity=state_module.repository_identity(ws.root),
            expected_branch=config.branch,
            spec=spec,
            plan=plan,
        )
        campaign._git = campaign_module.TrustedGit(campaign._lock, config.plan_path)
        try:
            campaign._bind_command_closure()
            self.assertTrue(campaign._command_closure)
            self.assertIsNotNone(campaign._command_closure_root)
            self.assertTrue(campaign._command_closure_root.is_dir())
            # The closure must contain the committed plan and the committed
            # verification authority, proving the real tree was staged.
            self.assertIn(config.plan_path, campaign._command_closure)
            self.assertIn(".factory/config.toml", campaign._command_closure)
        finally:
            campaign._lock.release()

    def test_production_developer_launch_derives_exact_committed_task_bytes(
        self,
    ) -> None:
        # B2: when the hidden suite does not supply the excerpt, the
        # production launch re-derives the developer's task bytes from the
        # committed plan blob at the bound commit and binds their exact
        # digest — never a self-claimed or operator-supplied byte set.
        ws = self.make(SUCCESS_SCENARIO)
        config = self._production_config(ws)
        head = _git(ws.root, "rev-parse", "HEAD").stdout.strip()
        plan_blob = campaign_module._blob_at(ws.root, config.plan_path)
        expected_excerpt, expected_digest = launch_module.derive_task_excerpt(
            plan_blob, 1)
        captured: dict = {}

        def _authorize(binding, **kwargs):
            captured["task_excerpt"] = kwargs["task_excerpt"]
            captured["task_excerpt_digest"] = binding.task_excerpt_digest
            captured["plan_bytes"] = kwargs["plan"]
            captured["plan_digest"] = binding.plan_digest
            captured["bound_commit"] = binding.bound_commit
            return object()

        class _Supervisor:
            def __init__(self, binding):
                self.binding = binding

            def run(self, authority):
                return type("_Result", (), {
                    "outcome": "exited", "returncode": 0,
                })()

        with unittest.mock.patch.object(
            campaign_module.launch_module, "authorize_launch",
            side_effect=_authorize,
        ), unittest.mock.patch.object(
            campaign_module.launch_module, "LaunchSupervision",
            side_effect=_Supervisor,
        ):
            outcome = campaign_module.launch_role_attempt(
                config, role="developer", head=head, task_id=1,
            )
        self.assertEqual(outcome.exit_status, 0)
        self.assertFalse(outcome.interrupted)
        self.assertEqual(captured["task_excerpt"], expected_excerpt)
        self.assertEqual(captured["task_excerpt_digest"], expected_digest)
        self.assertEqual(captured["task_excerpt_digest"], sha256(expected_excerpt))
        self.assertEqual(captured["plan_bytes"], plan_blob)
        self.assertEqual(captured["plan_digest"], sha256(plan_blob))
        self.assertEqual(captured["bound_commit"], head)

    def test_nonzero_role_preserves_only_bounded_redacted_diagnostic(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        config = self._production_config(ws)
        head = _git(ws.root, "rev-parse", "HEAD").stdout.strip()

        class _Supervisor:
            def __init__(self, binding):
                self.binding = binding

            def run(self, authority):
                stream = type("_Stream", (), {"tail": "  Provider not\n configured  "})()
                return type("_Result", (), {
                    "outcome": "completed", "returncode": 1, "reason": None,
                    "signal": None, "stderr": stream,
                    "stdout": type("_Stream", (), {"tail": "ignored"})(),
                })()

        with unittest.mock.patch.object(
            campaign_module.launch_module, "authorize_launch", return_value=object(),
        ), unittest.mock.patch.object(
            campaign_module.launch_module, "LaunchSupervision", side_effect=_Supervisor,
        ):
            outcome = campaign_module.launch_role_attempt(
                config, role="developer", head=head, task_id=1,
            )
        self.assertEqual(outcome.exit_status, 1)
        self.assertEqual(outcome.diagnostic, "Provider not configured")

    def test_production_auditor_derives_exact_committed_objective_bytes(
        self,
    ) -> None:
        # B2: the auditor objective is re-derived from the committed
        # audit-objective registry with the deterministic §6.4 selection and
        # the exact deterministic JSON encoding the registry CLI prints.
        ws = self.make(SUCCESS_SCENARIO)
        config = self._production_config(ws)
        head = _git(ws.root, "rev-parse", "HEAD").stdout.strip()
        registry = campaign_module._blob_at(
            ws.root, ".factory/audit-objectives/registry.json")
        document = audit_objectives_module.parse_registry(registry)
        objective = audit_objectives_module.select_audit_objective(2, document)
        expected = json.dumps(
            dict(objective), sort_keys=True, separators=(",", ":")
        ).encode("utf-8")
        captured: dict = {}

        def _authorize(binding, **kwargs):
            captured["audit_objective"] = kwargs["audit_objective"]
            captured["audit_objective_digest"] = binding.audit_objective_digest
            return object()

        class _Supervisor:
            def __init__(self, binding):
                self.binding = binding

            def run(self, authority):
                return type("_Result", (), {
                    "outcome": "exited", "returncode": 0,
                })()

        with unittest.mock.patch.object(
            campaign_module.launch_module, "authorize_launch",
            side_effect=_authorize,
        ), unittest.mock.patch.object(
            campaign_module.launch_module, "LaunchSupervision",
            side_effect=_Supervisor,
        ):
            outcome = campaign_module.launch_role_attempt(
                config, role="auditor", head=head, round_number=2,
            )
        self.assertEqual(outcome.exit_status, 0)
        self.assertEqual(captured["audit_objective"], expected)
        self.assertEqual(captured["audit_objective_digest"], sha256(expected))

    def test_real_ollama_tester_flow_writes_structured_result_from_prompt(self) -> None:
        """The non-driver Ollama role receives and fills the exact safe channel.

        Network quota transport alone is mocked; authorization, prompt
        composition, sealed memfd transport, Landlock, wrapper/backend exec,
        result-file write, and schema consumption are all real.
        """
        ws = self.make(SUCCESS_SCENARIO)
        # The minimal campaign fixture omits the child-only launcher because
        # driver tests do not need it; this regression exercises the real
        # production launch and therefore commits the exact launcher too.
        shutil.copy2(
            ROOT / ".factory" / "loop" / "confine_launcher.py",
            ws.root / ".factory" / "loop" / "confine_launcher.py",
        )
        backend = ws.root / "result-backend.py"
        backend.write_text(
            "#!/usr/bin/env python3\n"
            "import json, pathlib, sys\n"
            "prompt = sys.stdin.read()\n"
            "marker = 'Write the final machine result to this exact UTF-8 path: '\n"
            "lines = [line for line in prompt.splitlines() if line.startswith(marker)]\n"
            "if len(lines) != 1 or 'factory-phase-result/v1' not in prompt:\n"
            "    raise SystemExit(31)\n"
            "path = pathlib.Path(lines[0][len(marker):])\n"
            "path.write_text(json.dumps({'schema':'factory-phase-result/v1',"
            "'outcome':'pass'}) + '\\n', encoding='utf-8')\n",
            encoding="utf-8",
        )
        backend.chmod(0o755)
        (ws.root / ".factory/config.toml").write_text(
            '[verification]\ncampaign_command = ["' + str(TRUE_EXECUTABLE) + '"]\n', encoding="utf-8"
        )
        _git(
            ws.root, "add", "result-backend.py",
            ".factory/loop/confine_launcher.py", ".factory/config.toml",
        )
        _git(ws.root, "commit", "-qm", "add real result backend")
        head = _git(ws.root, "rev-parse", "HEAD").stdout.strip()
        config = dataclasses.replace(
            ws.derive_config(),
            provider="ollama",
            model="fixture-real-model",
            rounds_requested=5,
            backend=str(backend),
            role_driver=None,
            acceptance_command=campaign_module.CANONICAL_FINAL_ACCEPTANCE_COMMAND,
            capability_command=campaign_module.CANONICAL_CAPABILITY_COMMAND,
            runner_command=campaign_module.RUNNER_COMMAND,
            state_namespace=".factory-state/campaigns/campaign",
            accepted_commit=head,
            install_manifest=str(ws.root / "fixture-install-manifest.json"),
            phase_result_path=".factory-state/campaigns/campaign/phase-result.json",
            audit_result_path=".factory-state/campaigns/campaign/audit-result.json",
        )
        result_path = ws.root / config.phase_result_path
        result_path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        descriptor = os.open(result_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        os.close(descriptor)
        # Standalone/programmatic real-provider invocation has no published
        # descriptor-bound readiness token and must stop before a model.
        with self.assertRaisesRegex(
            campaign_module.CampaignPhaseError, "readiness authorization"
        ):
            campaign_module.launch_role_attempt(
                config, role="tester", head=head, round_number=1,
            )
        self.assertFalse(result_path.read_text() if result_path.exists() else "")

    def test_production_launch_invocation_error_is_clean_campaign_error(
        self,
    ) -> None:
        # B2: every InvocationError of the production launch (a refused
        # launch) is a clean, documented CampaignPhaseError — never an
        # unhandled traceback.
        ws = self.make(SUCCESS_SCENARIO)
        config = self._production_config(ws)
        head = _git(ws.root, "rev-parse", "HEAD").stdout.strip()
        with unittest.mock.patch.object(
            campaign_module.launch_module, "authorize_launch",
            side_effect=launch_module.InvocationError("guard refusal"),
        ):
            with self.assertRaises(campaign_module.CampaignPhaseError) as cm:
                campaign_module.launch_role_attempt(
                    config, role="developer", head=head, task_id=1,
                )
        self.assertIn("guard refusal", str(cm.exception))
        # A malformed invocation binding is a clean campaign error too.
        with unittest.mock.patch.object(
            campaign_module.launch_module, "verify_invocation",
            side_effect=launch_module.InvocationError("unbound bytes"),
        ):
            with self.assertRaises(campaign_module.CampaignPhaseError) as cm2:
                campaign_module.launch_role_attempt(
                    config, role="developer", head=head, task_id=1,
                )
        self.assertIn("unbound bytes", str(cm2.exception))
        # A task id absent from the committed plan refuses the launch cleanly.
        with self.assertRaises(campaign_module.CampaignPhaseError) as cm3:
            campaign_module.launch_role_attempt(
                config, role="developer", head=head, task_id=999,
            )
        self.assertIn("no Task 999", str(cm3.exception))

    # -- M1: the acceptance gate validates the exact newly validated plan ----

    def test_acceptance_gate_validates_the_exact_new_plan_not_the_stale_head(
        self,
    ) -> None:
        # M1: the default gate operates on the exact newly validated (worktree)
        # plan argument, never the stale pre-commit head plan.  A newly
        # validated plan whose verification reference differs from the
        # committed plan must be honored — a reference missing from the new
        # plan fails the gate even though the committed plan's reference
        # exists.
        ws = self.make(SUCCESS_SCENARIO)
        campaign = campaign_module.Campaign(ws.derive_config())
        text = (ws.root / PLAN_REL).read_text(encoding="utf-8")
        committed = campaign_module.plan_parser.Plan.from_bytes(text.encode())
        committed_task = next(t for t in committed.tasks if t.number == 1)
        self.assertIn("src/work-1.md", committed_task.fields["Verification"])
        (ws.root / "src" / "work-1.md").write_text("work", encoding="utf-8")
        ok, _ = campaign._acceptance_gate(1, committed)
        self.assertTrue(ok)
        # The exact newly validated plan changes the reference to a file the
        # worktree does not have: the gate must consult the new plan and fail,
        # even though the committed plan's reference exists.
        revised = text.replace(
            "- Verification: `src/work-1.md`",
            "- Verification: `src/work-missing.md`",
            1,
        )
        work_plan = campaign_module.plan_parser.Plan.from_bytes(
            revised.encode())
        ok, detail = campaign._acceptance_gate(1, work_plan)
        self.assertFalse(ok)
        self.assertIn("missing verification references", detail)
        self.assertIn("src/work-missing.md", detail)
        # The committed (stale head) plan still passes on its own, proving the
        # failure came from validating the exact newly validated plan.
        ok, _ = campaign._acceptance_gate(1, committed)
        self.assertTrue(ok)

    # -- M2: the selector is bound to the authoritative plan base ------------

    def test_selector_is_bound_to_the_authoritative_plan_base(self) -> None:
        # M2: a committed plan whose front-matter base_commit drifted from
        # the authoritative base anchored at the state's phase base fails
        # closed at implementation selection/recovery instead of selecting a
        # task from a stale plan.
        ws = self.make(SUCCESS_SCENARIO)
        config = self._crash_at_plan(
            ws,
            lambda state: (
                state.current_phase == "verification"
                and state.last_outcome == "task_completed"
            ),
        )
        plan_path = ws.root / PLAN_REL
        text = plan_path.read_text(encoding="utf-8")
        tampered = re.sub(
            r"^base_commit: [0-9a-f]{40}$",
            "base_commit: " + "1" * 40,
            text, count=1, flags=re.M,
        )
        plan_path.write_text(tampered, encoding="utf-8")
        _git(ws.root, "add", PLAN_REL)
        _git(ws.root, "commit", "-qm", "tampered stale plan")
        with self.assertRaises(campaign_module.CampaignRecoveryError) as cm:
            campaign_module.Campaign(config).run()
        self.assertIn("stale", str(cm.exception))

    def test_selector_call_uses_the_authoritative_base_binding(self) -> None:
        # M2 wiring: _step_implementation passes the authoritative base derived
        # from the state's phase base (never the plan's self-declared base) to
        # the selector, and a stale/ambiguous selection fails the step closed.
        ws = self.make(SUCCESS_SCENARIO)
        config = ws.derive_config()
        campaign = campaign_module.Campaign(config)
        campaign._acquire()
        try:
            head = campaign._git.head()
            plan_blob = campaign._git.blob_at(head, config.plan_path)
            state = state_module.init_state(
                ws.root,
                campaign_id=config.campaign_id,
                rounds_requested=config.rounds_requested,
                specification_digest=config.specification_digest,
                plan_digest=config.plan_digest,
                role_prompt_digests=dict(config.role_prompt_digests),
                audit_objectives_digest=config.audit_objectives_digest,
                phase_base_commit=config.phase_base_commit,
                branch=config.branch,
            )
            state = state_module.advance(
                state, "planned",
                plan_digest=campaign_module.plan_sha256(plan_blob),
                phase_base_commit=head,
            )
            self.assertEqual(state.current_phase, "implementation")
            authoritative = campaign._authoritative_plan_base(state)
            recorded: dict = {}

            def _fake_select(plan, *, bound_base_commit=None):
                recorded["bound"] = bound_base_commit
                recorded["plan_base"] = plan.base_commit
                raise campaign_module.selector_module.SelectorError(
                    "stale plan")

            with unittest.mock.patch.object(
                campaign_module.selector_module, "select_task",
                side_effect=_fake_select,
            ):
                with self.assertRaises(campaign_module.CampaignRecoveryError) as cm:
                    campaign._step_implementation(state)
            self.assertIn("stale or ambiguous", str(cm.exception))
        finally:
            campaign._lock.release()
        self.assertEqual(recorded["bound"], authoritative)
        self.assertEqual(recorded["bound"], recorded["plan_base"])

    # -- L1: status entries never guess how to stage a move/gitlink ---------

    def test_status_rejects_rename_and_copy_records(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        campaign = campaign_module.Campaign(ws.derive_config())
        (ws.root / "src" / "tracked.txt").write_text("x", encoding="utf-8")
        _git(ws.root, "add", "src/tracked.txt")
        _git(ws.root, "commit", "-qm", "tracked fixture file")
        campaign._acquire()
        try:
            git = campaign._git
            _git(ws.root, "mv", "src/tracked.txt", "src/renamed.txt")
            with self.assertRaises(campaign_module.CampaignGitError) as cm:
                git.status_entries()
            self.assertIn("rename/copy", str(cm.exception))
            _git(ws.root, "reset", "--hard", "-q")
            # A copy record is rejected the same way (crafted porcelain bytes;
            # with `-z` a real rename/copy is `XY old\0new\0`).
            class _Bytes:
                def __init__(self, payload):
                    self.stdout = payload
                    self.returncode = 0

            with unittest.mock.patch.object(
                type(git), "_bytes",
                return_value=_Bytes(b"C  src/old\x00src/new\x00"),
            ):
                with self.assertRaises(campaign_module.CampaignGitError) as cm2:
                    git.status_entries()
            self.assertIn("rename/copy", str(cm2.exception))
        finally:
            campaign._lock.release()

    def test_status_rejects_directory_marker_and_gitlink(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        campaign = campaign_module.Campaign(ws.derive_config())
        campaign._acquire()
        try:
            git = campaign._git
            # A whole-directory untracked marker (untracked nested repository)
            # is never an explicit file path.
            (ws.root / "nested").mkdir()
            _git(ws.root, "init", "-q", str(ws.root / "nested"))
            with self.assertRaises(campaign_module.CampaignGitError) as cm:
                git.status_entries()
            self.assertIn("whole-directory", str(cm.exception))
            shutil.rmtree(ws.root / "nested")
            # A tracked gitlink (submodule pointer, mode 160000) among the
            # dirty paths is rejected: the orchestrator never stages a
            # submodule pointer.
            head = _git(ws.root, "rev-parse", "HEAD").stdout.strip()
            _git(ws.root, "update-index", "--add", "--cacheinfo",
                 "160000,%s,sub" % head)
            _git(ws.root, "commit", "-qm", "add gitlink")
            _git(ws.root, "update-index", "--cacheinfo",
                 "160000,%s,sub" % ws.scenario_commit)
            with self.assertRaises(campaign_module.CampaignGitError) as cm2:
                git.status_entries()
            self.assertIn("gitlink", str(cm2.exception))
        finally:
            campaign._lock.release()

    def test_status_rejects_quoted_and_malformed_entries(self) -> None:
        # Defensive parser contract: porcelain bytes that would be ambiguous
        # (a quoted path, a malformed record) are rejected deterministically.
        ws = self.make(SUCCESS_SCENARIO)
        campaign = campaign_module.Campaign(ws.derive_config())
        campaign._acquire()
        try:
            git = campaign._git

            class _Bytes:
                def __init__(self, payload):
                    self.stdout = payload
                    self.returncode = 0

            for payload in (
                b'?? "quoted path"\x00',
                b"??\x00",
                b"?? \x00",
            ):
                with self.assertRaises(campaign_module.CampaignGitError):
                    with unittest.mock.patch.object(
                        type(git), "_bytes", return_value=_Bytes(payload),
                    ):
                        git.status_entries()
        finally:
            campaign._lock.release()

    # -- L2: verification references never escape the repository ------------

    def test_unsafe_verification_references_are_rejected(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        campaign = campaign_module.Campaign(ws.derive_config())
        plan = campaign_module.plan_parser.Plan.from_bytes(
            (ws.root / PLAN_REL).read_bytes())
        task = next(t for t in plan.tasks if t.number == 1)
        (ws.root / "src" / "work-1.md").write_text("x", encoding="utf-8")
        task.fields["Verification"] = "`src/work-1.md`"
        ok, _ = campaign._acceptance_gate(1, plan)
        self.assertTrue(ok)
        for unsafe in (
            "`/etc/passwd`",
            "`../escape.md`",
            "`src/../escape.md`",
            "`src//work.md`",
            "`src/./work.md`",
        ):
            task.fields["Verification"] = unsafe
            ok, detail = campaign._acceptance_gate(1, plan)
            self.assertFalse(ok)
            self.assertIn("unsafe verification reference", detail)
        # Verification is shell command prose by contract. Whitespace-bearing
        # argv is owned by the trusted verifier and is never rejected as a
        # purported pathname.
        task.fields["Verification"] = "`nix-shell --run 'ctest -R task-1'`"
        ok, detail = campaign._acceptance_gate(1, plan)
        self.assertTrue(ok, detail)
        for unsafe in (
            "/etc/passwd", "../escape.md", "a/../b", "a//b", "a/./b",
            "a\\b", "a\x00b", "",
        ):
            self.assertIsNotNone(
                campaign_module._unsafe_repo_relative(unsafe)[0], unsafe)
        self.assertEqual(
            campaign_module._unsafe_repo_relative("src/work-1.md"),
            (None, None))

    # -- HIGH: the untrusted developer never writes/commits trusted policy --

    def test_scope_violation_denies_trusted_policy_surface(self) -> None:
        # The orchestrator scope authority shares the confinement's trusted
        # policy/harness deny set: AGENTS.md, the hidden CI/forge tooling,
        # shell.nix, the factory configs, the harness docs, and the legacy
        # scripts/ security surface are never writable by any phase, while
        # genuine product entries stay writable.
        denied = [
            "AGENTS.md", ".gitignore", ".github/workflows/x.yml",
            ".forgejo/ISSUE_TEMPLATE/bug_report.md", "shell.nix",
            ".factory/config.toml", ".factory/environment.toml",
            "docs/FACTORY.md", "docs/OPERATIONS.md",
            "docs/FACTORY-LOOP-SPEC.md", "scripts/verify-project.sh",
            ".factory/tools/git-commit-guard.sh", ".factory/tools/pi2-secure-exec.py",
        ]
        allowed = [
            "src/main.py", "tests/test-main.py", "data/fixture.bin",
            "README.md", "docs/product-notes.md", "LICENSE",
        ]
        for phase in ("planning", "implementation", "verification", "audit"):
            for path in denied:
                with self.subTest(phase=phase, path=path):
                    violation = campaign_module.scope_violation(
                        [path], phase=phase,
                        plan_path=PLAN_REL,
                        spec_path="docs/SPEC.md",
                        allow_paths=[".factory-state/phase-result.json"],
                    )
                    self.assertIsNotNone(violation)
                    self.assertIn("trusted policy/harness surface", violation)
        for path in allowed:
            self.assertIsNone(campaign_module.scope_violation(
                [path], phase="implementation",
                plan_path=PLAN_REL, spec_path="docs/SPEC.md",
            ))
        # The canonical specification is still denied by the spec rule (a
        # genuine product entry, not the policy surface).
        violation = campaign_module.scope_violation(
            ["docs/SPEC.md"], phase="implementation",
            plan_path=PLAN_REL, spec_path="docs/SPEC.md",
        )
        self.assertIsNotNone(violation)
        self.assertIn("canonical specification", violation)

    def test_missing_role_digest_is_clean_campaign_error(self) -> None:
        # LOW: a committed role-prompt digest absent from the config is a
        # clean CampaignPhaseError (fail closed), never an unhandled KeyError.
        ws = self.make(SUCCESS_SCENARIO)
        config = self._production_config(ws)
        config = dataclasses.replace(
            config,
            role_prompt_digests={
                role: digest
                for role, digest in config.role_prompt_digests.items()
                if role != "tester"
            },
        )
        head = _git(ws.root, "rev-parse", "HEAD").stdout.strip()
        with unittest.mock.patch.object(
            campaign_module.launch_module, "authorize_launch",
            side_effect=launch_module.InvocationError("unreachable"),
        ):
            with self.assertRaises(campaign_module.CampaignPhaseError) as cm:
                campaign_module.launch_role_attempt(
                    config, role="tester", head=head,
                )
        self.assertIn("no digest for 'tester'", str(cm.exception))

    # -- L3: classify_implementation honors the §13 exit status -------------- 

    def test_implementation_classification_honors_exit_status(self) -> None:
        ok = campaign_module.RoleOutcome("developer", 0)
        base = dict(role=ok, plan_valid=True, task_complete=True,
                    acceptance_pass=True, had_substantive=True, scope_ok=True)
        self.assertEqual(campaign_module.classify_implementation(**base),
                         "task_completed")
        failed = campaign_module.RoleOutcome("developer", 1)
        self.assertEqual(campaign_module.classify_implementation(
            **{**base, "role": failed}), "task_failed")
        self.assertEqual(campaign_module.classify_implementation(
            **{**base, "role": failed, "task_complete": False,
               "acceptance_pass": False, "had_substantive": True}), "task_failed")
        doc = campaign_module.classify_implementation.__doc__ or ""
        self.assertIn("nonzero machine-readable", doc)

    def test_complete_work_with_nonzero_exit_retries_then_completes(self) -> None:
        # L3 integration: a developer whose complete work is accompanied by a
        # nonzero exit status is a deterministic task_failed on that attempt;
        # the coherent work is preserved and the retry commits it as the
        # completion.
        ws = self.make({
            "planner": {"behavior": "planned"},
            "developer": {"behavior": "complete-exit1"},
            "tester": {"behavior": "pass"},
            "auditor": {"behavior": "pass"},
        })
        rc, data = ws.run_cli()
        self.assertEqual(rc, 0)
        assert_history(self, data, [
            (1, "planning", "planned"),
            (1, "implementation", "task_failed"),
            (1, "implementation", "task_completed"),
            (1, "verification", "pass"),
            (1, "audit", "pass"),
        ])
        # The coherent work survives both attempts.
        work = ws.root / "src" / "work-1.md"
        self.assertTrue(work.exists())

    # -- L4: every trusted Git call is finite-bounded ------------------------

    def test_every_trusted_git_call_is_finite_bounded(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        campaign = campaign_module.Campaign(ws.derive_config())
        campaign._acquire()
        try:
            real_run = campaign._lock._git_run
            real_bytes = campaign._lock._git_bytes
            recorded: list = []

            def recording_run(argv, *, timeout=None, env=None):
                recorded.append((list(argv), timeout))
                return real_run(argv, timeout=timeout, env=env)

            def recording_bytes(argv, *, timeout=None, env=None):
                recorded.append((list(argv), timeout))
                return real_bytes(argv, timeout=timeout, env=env)

            with unittest.mock.patch.object(
                campaign._lock, "_git_run", side_effect=recording_run,
            ), unittest.mock.patch.object(
                campaign._lock, "_git_bytes", side_effect=recording_bytes,
            ):
                git = campaign._git
                git.head()
                git.is_ancestor("HEAD", "HEAD")
                git.status_entries()
                git.diff_paths("HEAD")
                git.history(3)
                git.commit_count()
                git.plan_at(git.head())
            self.assertTrue(recorded)
            for argv, timeout in recorded:
                self.assertIsNotNone(timeout, f"unbounded Git call {argv}")
                self.assertGreater(timeout, 0)
                self.assertLessEqual(timeout, campaign_module.GIT_TIMEOUT)
        finally:
            campaign._lock.release()
        # The module-level trusted Git reads are bounded the same way.
        recorded2: list = []
        real_git_run = campaign_module.gitutil.git_run
        real_git_bytes = campaign_module.gitutil.git_bytes

        def recording_git_run(argv, *, timeout=None, **kwargs):
            recorded2.append(timeout)
            return real_git_run(argv, timeout=timeout, **kwargs)

        def recording_git_bytes(argv, *, timeout=None, **kwargs):
            recorded2.append(timeout)
            return real_git_bytes(argv, timeout=timeout, **kwargs)

        with unittest.mock.patch.object(
            campaign_module.gitutil, "git_run",
            side_effect=recording_git_run,
        ), unittest.mock.patch.object(
            campaign_module.gitutil, "git_bytes",
            side_effect=recording_git_bytes,
        ):
            campaign_module._live_head(ws.root)
            campaign_module._blob_at(ws.root, PLAN_REL)
            campaign_module._audit_objective_bytes(ws.root, 1)
        self.assertTrue(recorded2)
        for timeout in recorded2:
            self.assertIsNotNone(timeout)
            self.assertGreater(timeout, 0)
            self.assertLessEqual(timeout, campaign_module.GIT_TIMEOUT)


class RunnerAcquisitionLifecycleTests(_CampaignBase):
    """Coordinator-owned exact-HEAD runner acquisition regressions."""

    def _authority(self, *, timeout: float = 2.0):
        ws = self.make(SUCCESS_SCENARIO)
        (ws.root / ".factory" / "environment.toml").write_text(
            "schema_version = 1\n", encoding="utf-8",
        )
        runner = ws.root / ".factory" / "runner" / "run-factory-runners.py"
        runner.parent.mkdir(exist_ok=True)
        runner.write_text(
            "#!/usr/bin/env python3\n"
            "import hashlib,json,pathlib,subprocess,sys,time\n"
            "root=pathlib.Path(__file__).resolve().parent.parent.parent\n"
            "state=root/'.factory-state'; state.mkdir(mode=0o700,exist_ok=True)\n"
            "mode=(state/'runner-mode').read_text().strip() if (state/'runner-mode').exists() else 'pass'\n"
            "if mode=='timeout': time.sleep(5)\n"
            "if mode=='transport': raise SystemExit(20)\n"
            "if mode=='integrity': raise SystemExit(22)\n"
            "head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()\n"
            "if mode=='findings':\n"
            "    (state/'findings-aggregate.json').write_bytes((json.dumps({'commit':head,'runners':[{'result':'findings'}]},sort_keys=True)+'\\n').encode())\n"
            "    raise SystemExit(21)\n"
            "counter=state/'runner-count'; n=int(counter.read_text())+1 if counter.exists() else 1\n"
            "counter.write_text(str(n))\n"
            "raw=(json.dumps({'commit':head,'attempt':n},sort_keys=True)+'\\n').encode()\n"
            "tmp=state/'.runner-evidence.tmp'; tmp.write_bytes(raw); tmp.replace(state/'runner-evidence.json')\n",
            encoding="utf-8",
        )
        checker = ws.root / ".factory" / "tools" / "check-factory-runner-evidence.py"
        checker.write_text(
            "#!/usr/bin/env python3\n"
            "import argparse,hashlib,json,pathlib,subprocess,sys\n"
            "p=argparse.ArgumentParser(); p.add_argument('--expected-commit',required=True); p.add_argument('--expected-campaign-id',required=True); p.add_argument('--expected-readiness-nonce',required=True); p.add_argument('--verify-findings',action='store_true'); p.add_argument('--print-digest',action='store_true'); a=p.parse_args()\n"
            "root=pathlib.Path(__file__).resolve().parent.parent.parent; name='findings-aggregate.json' if a.verify_findings else 'runner-evidence.json'; path=root/'.factory-state'/name\n"
            "if path.is_symlink() or not path.is_file(): raise SystemExit(22)\n"
            "raw=path.read_bytes()\n"
            "try: data=json.loads(raw)\n"
            "except Exception: raise SystemExit(22)\n"
            "head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()\n"
            "if data.get('commit')!=a.expected_commit or head!=a.expected_commit: raise SystemExit(22)\n"
            "if a.verify_findings and not any((r.get('result')=='findings') for r in data.get('runners',[]) if isinstance(r,dict)): raise SystemExit(22)\n"
            "print(hashlib.sha256(raw).hexdigest())\n",
            encoding="utf-8",
        )
        runner.chmod(0o755); checker.chmod(0o755)
        _git(ws.root, "add", ".factory/environment.toml", ".factory/runner/run-factory-runners.py",
             ".factory/tools/check-factory-runner-evidence.py")
        _git(ws.root, "commit", "-qm", "add declared runner acquisition fixtures")
        config = dataclasses.replace(
            ws.derive_config(), runner_command=campaign_module.RUNNER_COMMAND,
            runner_timeout=timeout,
        )
        authority = campaign_module.Campaign(config)
        authority._acquire()
        return ws, authority

    def _close(self, authority) -> None:
        for name in (
            "_held_verifier", "_held_capability", "_held_core_acceptance",
            "_held_conformance_validator", "_held_acceptance", "_held_runner",
            "_held_runner_checker", "_held_driver",
        ):
            held = getattr(authority, name, None)
            if held is not None:
                held.close()
                setattr(authority, name, None)
        if authority._lock is not None:
            authority._lock.release()
            authority._lock = None

    def test_commit_refreshes_and_unchanged_valid_aggregate_reuses(self) -> None:
        ws, authority = self._authority()
        try:
            acquired = authority._ensure_runner_evidence()
            self.assertEqual(acquired[1], 0, acquired)
            self.assertEqual((ws.root / ".factory-state/runner-count").read_text(), "1")
            self.assertIn("reused", authority._ensure_runner_evidence()[2])
            self.assertEqual((ws.root / ".factory-state/runner-count").read_text(), "1")
            for role in ("planner", "developer", "audit"):
                path = ws.root / "src" / f"{role}-commit.txt"
                path.write_text(role, encoding="utf-8")
                _git(ws.root, "add", str(path.relative_to(ws.root)))
                _git(ws.root, "commit", "-qm", f"{role} authored commit")
                self.assertEqual(authority._ensure_runner_evidence()[1], 0)
            self.assertEqual((ws.root / ".factory-state/runner-count").read_text(), "4")
            metadata = json.loads(
                (ws.root / ".factory-state/runner-acquisition.json").read_text()
            )
            self.assertEqual(metadata["head"], _git(ws.root, "rev-parse", "HEAD").stdout.strip())
            self.assertEqual(metadata["status"], "complete")
            self.assertEqual(metadata["attempt"], 4)
        finally:
            self._close(authority)

    def test_current_head_forged_partial_or_symlinked_aggregate_is_integrity_failure(self) -> None:
        for kind in ("forged", "partial", "symlink"):
            with self.subTest(kind=kind):
                ws, authority = self._authority()
                try:
                    acquired = authority._ensure_runner_evidence()
                    self.assertEqual(acquired[1], 0, acquired)
                    aggregate = ws.root / ".factory-state/runner-evidence.json"
                    if kind == "forged":
                        aggregate.write_text('{"commit":"' + ('0' * 40) + '"}\n')
                    elif kind == "partial":
                        aggregate.write_text('{"commit":')
                    else:
                        aggregate.unlink()
                        aggregate.symlink_to("runner-count")
                    ran, code, detail = authority._ensure_runner_evidence()
                    self.assertTrue(ran)
                    self.assertEqual(code, -1)
                    self.assertIn("integrity", detail)
                    self.assertEqual((ws.root / ".factory-state/runner-count").read_text(), "1")
                    # The durable integrity marker is never reused as success;
                    # the next same-HEAD boundary reacquires and strongly
                    # checks a replacement aggregate.
                    self.assertEqual(authority._ensure_runner_evidence()[1], 0)
                    metadata = json.loads(
                        (ws.root / ".factory-state/runner-acquisition.json").read_text()
                    )
                    self.assertEqual(metadata["status"], "complete")
                    self.assertEqual(metadata["attempt"], 2)
                finally:
                    self._close(authority)

    def test_transport_timeout_and_crash_recovery_are_finite_and_honest(self) -> None:
        for mode in ("transport", "findings", "timeout"):
            with self.subTest(mode=mode):
                ws, authority = self._authority(timeout=0.1)
                try:
                    (ws.root / ".factory-state").mkdir(mode=0o700, exist_ok=True)
                    (ws.root / ".factory-state/runner-mode").write_text(mode)
                    ran, code, detail = authority._ensure_runner_evidence()
                    self.assertTrue(ran)
                    expected = (
                        campaign_module.RUNNER_FINDINGS_EXIT
                        if mode == "findings"
                        else campaign_module.RUNNER_TRANSPORT_EXIT
                    )
                    self.assertEqual(code, expected)
                    self.assertIn(
                        "verification" if mode == "findings" else "transport",
                        detail,
                    )
                    metadata = json.loads(
                        (ws.root / ".factory-state/runner-acquisition.json").read_text()
                    )
                    self.assertEqual(
                        metadata["status"],
                        "findings" if mode == "findings" else "transport_failure",
                    )
                    (ws.root / ".factory-state/runner-mode").unlink()
                    self.assertEqual(authority._ensure_runner_evidence()[1], 0)
                    recovered = json.loads(
                        (ws.root / ".factory-state/runner-acquisition.json").read_text()
                    )
                    self.assertEqual(recovered["attempt"], 2)
                    self.assertEqual(recovered["status"], "complete")
                finally:
                    self._close(authority)
        ws, authority = self._authority()
        try:
            head, tree, environment_blob = authority._runner_bindings()
            authority._write_runner_acquisition(
                attempt=1, status="acquiring", head=head, tree=tree,
                environment_blob=environment_blob,
                diagnostic="runner acquisition started",
            )
            self.assertEqual(authority._ensure_runner_evidence()[1], 0)
            metadata = json.loads(
                (ws.root / ".factory-state/runner-acquisition.json").read_text()
            )
            self.assertEqual(metadata["attempt"], 2)
            self.assertEqual(metadata["status"], "complete")
        finally:
            self._close(authority)

    def test_transport_unavailable_is_terminal_infrastructure_failure(self) -> None:
        # Phase 5: a runner transport failure (exit 20, endpoint unreachable)
        # is terminal infrastructure, never a product finding and never a
        # blockable external capability. The campaign must stop attempting
        # model rounds to "fix" product code when the real problem is an
        # unreachable runner endpoint.
        outcome = campaign_module.classify_verification(
            role=campaign_module.RoleOutcome("tester", 0),
            scope_ok=True, gate_ran=True, gate_exit=0,
            tester_result_valid=True, tester_result_outcome="pass",
            findings=[], blocked_refs=[], capability_available=False,
            capability_ran=True,
            capability_exit=campaign_module.RUNNER_TRANSPORT_EXIT,
        )
        self.assertEqual(outcome, "infrastructure_failure")
        blocked = campaign_module.classify_verification(
            role=campaign_module.RoleOutcome("tester", 0),
            scope_ok=True, gate_ran=True, gate_exit=0,
            tester_result_valid=True, tester_result_outcome="blocked",
            findings=[], blocked_refs=["FACT-007"], capability_available=False,
            capability_ran=True,
            capability_exit=campaign_module.RUNNER_TRANSPORT_EXIT,
        )
        self.assertEqual(blocked, "infrastructure_failure")

    def test_integrity_exit_is_infrastructure_failure(self) -> None:
        # Phase 5: a runner integrity failure (exit 22, invalid signed
        # evidence) is terminal infrastructure, never a product finding.
        for tester_outcome in ("pass", "blocked", "findings"):
            with self.subTest(tester_outcome=tester_outcome):
                self.assertEqual(
                    campaign_module.classify_verification(
                        role=campaign_module.RoleOutcome("tester", 0),
                        scope_ok=True, gate_ran=True, gate_exit=0,
                        tester_result_valid=True,
                        tester_result_outcome=tester_outcome,
                        findings=[], blocked_refs=["FACT-007"],
                        capability_available=False,
                        capability_ran=True,
                        capability_exit=campaign_module.RUNNER_INTEGRITY_EXIT,
                    ),
                    "infrastructure_failure",
                )

    def test_findings_exit_remains_findings(self) -> None:
        # Phase 5: exit 21 (runner executed but reported product findings) is
        # a genuine product finding, NOT infrastructure. Only transport (20)
        # and integrity (22) are terminal infrastructure.
        self.assertEqual(
            campaign_module.classify_verification(
                role=campaign_module.RoleOutcome("tester", 0),
                scope_ok=True, gate_ran=True, gate_exit=0,
                tester_result_valid=True, tester_result_outcome="pass",
                findings=[], blocked_refs=[], capability_available=False,
                capability_ran=True,
                capability_exit=campaign_module.RUNNER_FINDINGS_EXIT,
            ),
            "findings",
        )

    def test_runner_argv_refuses_arguments_shell_and_substitution(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        base = ws.derive_config()
        for command in (
            ("./.factory/runner/run-factory-runners.py", "--candidate"),
            ("sh", "-c", "./.factory/runner/run-factory-runners.py"),
            ("./scripts/model-owned-runner.py",),
            ("./.factory/runner/run-factory-runners.py;touch",),
        ):
            with self.subTest(command=command):
                with self.assertRaises(campaign_module.CampaignConfigError):
                    dataclasses.replace(
                        base, role_driver=None, backend="/trusted/backend",
                        acceptance_command=("./.factory/tools/credential-guard.py",),
                        capability_command=("./.factory/tools/credential-guard.py",),
                        runner_command=command,
                        state_namespace=".factory-state/campaigns/campaign",
                        accepted_commit=base.phase_base_commit,
                        install_manifest="/trusted/manifest.json",
                    )


class _FinalGateHarness:
    """Stand-in exposing the exact collaborator surface ``_final_gate`` reads.

    ``Campaign._final_gate`` is exercised as an unbound method on this object so
    the strict exact-HEAD pass/findings/infrastructure decision is behaviorally
    tested without a full untrusted auditor round or real runner acquisition.
    ``calls`` records ``(label, current_product)`` for every ``_run_gate`` so a
    test can assert every final gate executes against the current product tree.
    """

    def __init__(self, *, acquisition, gates, config, git):
        self.acquisition = acquisition  # (ran, exit, detail) from the runner
        self.gates = gates              # label -> (ran, exit, detail, skipped)
        self.calls: list[tuple[str, bool]] = []
        self._config = config
        self._git = git

    def _ensure_runner_evidence(self):
        return self.acquisition

    def _run_gate(self, command, label, *, current_product=False):
        self.calls.append((label, current_product))
        return self.gates[label]


class FinalGateDirect(_CampaignBase):
    """Direct unit tests of the strict exact-HEAD ``_final_gate`` seam.

    The success transition is never granted on auditor JSON alone: every
    exact-commit-bound command must execute without skips, the committed
    readiness conformance mapping must derive, and the independent human
    graphics-approval / VRF-07 authority must revalidate at the current
    exact HEAD.  Deterministic failures become findings and can never reach
    success; missing/substituted/unexecuted gates and signed-runner
    transport/integrity failures are control-plane infrastructure.
    """

    APPROVAL = json.dumps({
        "candidate_commit": "a" * 40,
        "schema": "human-approval/v1",
        "signature": "sig",
    }).encode()

    def _config(self):
        cfg = unittest.mock.Mock()
        cfg.capability_command = ("./scripts/check-capability-evidence.py",)
        cfg.acceptance_command = ("./scripts/final-gate.sh",)
        cfg.human_trust_anchor = "/trusted/anchor.json"
        cfg.human_trust_anchor_sha256 = "0" * 64
        return cfg

    def _config_absent_anchor(self):
        cfg = self._config()
        cfg.human_trust_anchor = ""
        cfg.human_trust_anchor_sha256 = ""
        return cfg

    def _config_partial_anchor(self):
        cfg = self._config()
        cfg.human_trust_anchor = "/trusted/anchor.json"
        cfg.human_trust_anchor_sha256 = ""
        return cfg

    def _git(self):
        g = unittest.mock.Mock()
        g.head.return_value = "c" * 40
        g.blob_at.side_effect = lambda commit, relpath: (
            self.APPROVAL
            if relpath == campaign_module.readiness_module.APPROVAL_PATH
            else b"{}"
        )
        return g

    def _harness(self, *, acquisition=(True, 0, ""), gates=None):
        default = {
            "final capability": (True, 0, "", False),
            "final core acceptance": (True, 0, "", False),
            "final acceptance": (True, 0, "", False),
            "final conformance validator": (True, 0, "", False),
        }
        default.update(gates or {})
        return _FinalGateHarness(
            acquisition=acquisition, gates=default,
            config=self._config(), git=self._git(),
        )

    @contextlib.contextmanager
    def _readiness(self, *, mapping_ok=True, human_ok=True):
        if mapping_ok:
            core_mapping = unittest.mock.Mock(return_value="ok")
        else:
            core_mapping = unittest.mock.Mock(
                side_effect=campaign_module.readiness_module.ReadinessError(
                    "mapping"))
        if human_ok:
            human_approval = unittest.mock.Mock(return_value="ok")
        else:
            human_approval = unittest.mock.Mock(
                side_effect=(
                    campaign_module.readiness_module.HumanApprovalBlocked(
                        "blocked")))
        with unittest.mock.patch.multiple(
            campaign_module.readiness_module,
            validate_core_mapping=core_mapping,
            read_external_authority=unittest.mock.Mock(
                return_value=(b"trust-raw", b"")),
            validate_trust_anchor=unittest.mock.Mock(
                return_value={"keys": []}),
            validate_human_approval=human_approval,
            validate_human_conformance=unittest.mock.Mock(return_value="ok"),
        ):
            yield

    def _audit_role(self):
        return campaign_module.RoleOutcome(
            role="auditor", exit_status=0, diagnostic="")

    def test_all_authorities_pass_preserves_pass(self) -> None:
        role = self._audit_role()
        result = {"schema": "audit-result/v1", "outcome": "pass",
                  "findings": []}
        harness = self._harness()
        with self._readiness():
            out_role, out_result, detail = (
                campaign_module.Campaign._final_gate(
                    harness, role, dict(result)))
        # A fully passing strict gate leaves the outcome untouched: the pass
        # auditor may transition to success.
        self.assertEqual(out_role, role)
        self.assertEqual(out_role.exit_status, 0)
        self.assertEqual(out_result["outcome"], "pass")
        self.assertEqual(out_result["findings"], [])
        self.assertEqual(detail, "")
        # Every final gate executes against the current exact product tree.
        labels = [label for label, _ in harness.calls]
        self.assertEqual(labels, [
            "final capability", "final core acceptance",
            "final acceptance", "final conformance validator",
        ])
        for _, current_product in harness.calls:
            self.assertTrue(current_product)

    def test_runner_transport_and_integrity_failure_are_infrastructure(self) -> None:
        for exit_code, kind in (
            (campaign_module.RUNNER_TRANSPORT_EXIT, "transport"),
            (campaign_module.RUNNER_INTEGRITY_EXIT, "integrity"),
        ):
            with self.subTest(kind=kind):
                role = self._audit_role()
                result = {"schema": "audit-result/v1", "outcome": "pass",
                          "findings": []}
                harness = self._harness(
                    acquisition=(False, exit_code, f"{kind} detail"))
                with self._readiness():
                    out_role, out_result, detail = (
                        campaign_module.Campaign._final_gate(
                            harness, role, dict(result)))
                # A signed-runner transport/integrity authority failure is
                # control-plane infrastructure and can never be satisfied by
                # fixing the product.
                self.assertEqual(out_role.exit_status, -1)
                self.assertEqual(
                    out_role.diagnostic, "final acceptance authority failure")
                # The capability gate inherits the acquisition failure, so the
                # reclassified result also carries that recorded finding.
                self.assertEqual(out_result["outcome"], "findings")
                self.assertIn(
                    "final capability/evidence command did not pass without "
                    "skips", out_result["findings"])

    def test_skipped_or_unavailable_gates_fail_infrastructure(self) -> None:
        for label, gates, human_ok in (
            ("final capability",
             {"final capability": (True, 0, "", True)}, True),
            ("final core acceptance",
             {"final core acceptance": (True, 0, "", True)}, True),
            ("final acceptance",
             {"final acceptance": (True, 0, "", True)}, True),
            ("final conformance validator",
             {"final conformance validator": (True, 0, "", True)}, True),
            ("final capability unavailable",
             {"final capability": (False, 0, "", False)}, True),
            ("final core acceptance not run (unavailable)",
             {"final core acceptance": (False, 0, "", False)}, True),
        ):
            with self.subTest(gate=label):
                role = self._audit_role()
                result = {"schema": "audit-result/v1", "outcome": "pass",
                          "findings": []}
                harness = self._harness(gates=gates)
                with self._readiness(human_ok=human_ok):
                    out_role, out_result, _detail = (
                        campaign_module.Campaign._final_gate(
                            harness, role, dict(result)))
                # A skipped or unexecuted authority means the exact tree was
                # never fully certified: fail closed as infrastructure.
                self.assertEqual(out_role.exit_status, -1)
                self.assertEqual(
                    out_role.diagnostic, "final acceptance authority failure")

    def test_deterministic_conformance_human_product_nonpass_yields_findings(
        self) -> None:
        # Deterministic non-passes (a conformance/acceptance command whose
        # probe ran and returned findings, or a human approval that fails to
        # revalidate at the current exact HEAD) are legitimate product
        # findings: the role is NOT reclassified to infrastructure, yet the
        # campaign can never reach success.
        scenarios = [
            ("conformance findings",
             {"final conformance validator": (True, 1, "partial rows", False)},
             True),
            ("acceptance findings",
             {"final acceptance": (True, 1, "test failed", False)}, True),
            ("capability findings",
             {"final capability": (True, 1, "probe failed", False)}, True),
            ("human approval/VRF-07 revalidation", {}, False),
        ]
        for name, gates, human_ok in scenarios:
            with self.subTest(case=name):
                role = self._audit_role()
                result = {"schema": "audit-result/v1", "outcome": "pass",
                          "findings": []}
                harness = self._harness(gates=gates)
                with self._readiness(human_ok=human_ok):
                    out_role, out_result, detail = (
                        campaign_module.Campaign._final_gate(
                            harness, role, dict(result)))
                # The product-level authority failure stays a finding, never
                # control-plane infrastructure.
                self.assertEqual(out_role.exit_status, 0)
                self.assertEqual(out_result["outcome"], "findings")
                self.assertTrue(out_result["findings"])
                self.assertTrue(detail)
                # The real classifier confirms the campaign cannot succeed: a
                # deterministic non-pass is at most ``findings``.
                outcome = campaign_module.classify_audit(
                    role=out_role, scope_ok=True, result_valid=True,
                    outcome=out_result["outcome"],
                    findings=out_result["findings"], blocked_refs=[])
                self.assertEqual(outcome, "findings")
                self.assertNotEqual(outcome, "pass")

    def test_absent_anchor_is_product_findings_not_infrastructure(self) -> None:
        # The canonical absent authority (omitted path+digest, bound as
        # ZERO256) is an authenticated product finding: the role is NOT
        # reclassified to infrastructure, yet the campaign can never reach
        # success.
        role = self._audit_role()
        result = {"schema": "audit-result/v1", "outcome": "pass",
                  "findings": []}
        harness = self._harness()
        harness._config = self._config_absent_anchor()
        with self._readiness():
            out_role, out_result, detail = (
                campaign_module.Campaign._final_gate(
                    harness, role, dict(result)))
        self.assertEqual(out_role.exit_status, 0)
        self.assertEqual(out_result["outcome"], "findings")
        self.assertIn(
            "final human graphics approval/VRF-07 did not revalidate "
            "at current exact HEAD", out_result["findings"])
        self.assertTrue(detail)
        outcome = campaign_module.classify_audit(
            role=out_role, scope_ok=True, result_valid=True,
            outcome=out_result["outcome"],
            findings=out_result["findings"], blocked_refs=[])
        self.assertEqual(outcome, "findings")
        self.assertNotEqual(outcome, "infrastructure_failure")
        self.assertNotEqual(outcome, "pass")

    def test_partial_anchor_is_infrastructure(self) -> None:
        # A path without its exact digest (or vice versa) is a provided-but-
        # invalid anchor: infrastructure, never an ordinary product finding.
        role = self._audit_role()
        result = {"schema": "audit-result/v1", "outcome": "pass",
                  "findings": []}
        harness = self._harness()
        harness._config = self._config_partial_anchor()
        with self._readiness():
            out_role, out_result, _detail = (
                campaign_module.Campaign._final_gate(
                    harness, role, dict(result)))
        self.assertEqual(out_role.exit_status, -1)
        self.assertEqual(
            out_role.diagnostic, "final acceptance authority failure")

    def test_mapping_or_authority_unavailability_fails_infrastructure(self) -> None:
        # A conformance mapping that cannot derive from the committed policy
        # (ReadinessError) or an unavailable external human trust anchor means
        # the authority cannot run: infrastructure, not a product finding.
        role = self._audit_role()
        result = {"schema": "audit-result/v1", "outcome": "pass",
                  "findings": []}
        harness = self._harness()
        with self._readiness(mapping_ok=False):
            out_role, out_result, _detail = (
                campaign_module.Campaign._final_gate(
                    harness, role, dict(result)))
        self.assertEqual(out_role.exit_status, -1)
        self.assertEqual(
            out_role.diagnostic, "final acceptance authority failure")


class _RunnerFindingsReadinessHarness(campaign_module.Campaign):
    """Direct round-zero seam overrides for the B5 readiness tests.

    Only the untrusted authority entry points (runner acquisition, the
    readiness binding seed, and the gate results) are stubbed; every real B5
    path — the hardened no-follow aggregate/manifest re-reads, the archive
    binding derivation, the projection, the merged payload mint, and the
    published terminal state — executes the production code.
    """

    def __init__(self, config, *, acquisition, gates, binding):
        super().__init__(config)
        self._test_acquisition = acquisition
        self._test_gates = gates
        self._test_binding = binding
        self.gate_calls = []

    def _initial_readiness_binding(self):
        return dict(self._test_binding)

    def _ensure_runner_evidence(self):
        return (
            True,
            campaign_module.RUNNER_FINDINGS_EXIT,
            "declared runner verification did not pass",
        )

    def _read_runner_acquisition(self):
        return (
            dict(self._test_acquisition)
            if self._test_acquisition is not None else None
        )

    def _run_gate(self, command, label, *, current_product=False):
        self.gate_calls.append((label, current_product))
        return self._test_gates[label]

    def _persist_state(
        self, value: campaign_module.state_module.FactoryState,
    ) -> campaign_module.state_module.FactoryState:
        """The real publish/reopen/digest-verify transition, with the
        readiness-required expectation pinned to the production policy this
        direct suite drives (the fixture's role-driver seam would otherwise
        expect a non-readiness state)."""
        state_module.write_state(self._root, value)
        reread = state_module.load_state(
            self._root, expected_branch=self._config.branch,
            expected_campaign_id=self._config.campaign_id,
            expected_rounds_requested=self._config.rounds_requested,
            expected_readiness_required=True,
        )
        if (
            reread != value
            or state_module.state_digest(reread)
            != state_module.state_digest(value)
        ):
            raise campaign_module.CampaignRecoveryError(
                "persisted state bytes differ from the recovered transition"
            )
        return reread


class RunnerFindingsReadinessTests(_CampaignBase):
    """B5 direct campaign tests: round-zero readiness projects the validated
    signed findings aggregate into fixed-semantic structured findings.

    A real fixture workspace provides the trusted Git/config surface; the
    untrusted authority seams (runner acquisition, gates, human trust,
    conformance mapping) are stubbed exactly like the other direct seam
    suites while the aggregate re-read, the signed-manifest archive binding
    derivation, the projection, the merged payload mint, and the published
    infrastructure-ready state exercise the real implementation paths.  Any
    absent, path-escaping, malformed, digest-mismatched, binding-mismatched,
    skipped/simulated, or prose artifact fails the campaign closed as an
    infrastructure failure — never a plannable claim.
    """

    RUNNER = "dev-runner-vm"
    CAPABILITY = "remote-project-gate"
    NONCE = "ab" * 32

    def expected_code(self, exit_code: int = 8) -> str:
        return (
            f"{findings_module.RUNNER_FINDINGS_CODE}:"
            f"{self.RUNNER}:{self.CAPABILITY}:{exit_code}"
        )

    def _policy_raw(self) -> bytes:
        return (ROOT / ".factory" / "readiness-policy.json").read_bytes()

    def _environment_raw(self) -> bytes:
        return (
            "schema_version = 1\n\n"
            "[[runners]]\n"
            f"name = \"{self.RUNNER}\"\n"
            "transport = \"ssh\"\n"
            'verify_argv = ["./scripts/verify-project.sh"]\n'
            f"capabilities = [\"{self.CAPABILITY}\"]\n"
        ).encode()

    def _contracts_raw(self) -> bytes:
        return json.dumps({
            "schema": "ralph-capability-contract/v2",
            "description": "fixture",
            "capabilities": [{
                "name": self.CAPABILITY,
                "status": "declared",
                "runner_class": self.RUNNER,
                "must_execute": True,
                "candidate_probe_argv": ["./scripts/verify-project.sh"],
            }],
        }, sort_keys=True, separators=(",", ":")).encode()

    def _manifest(
        self, *, commit: str, tree: str, env_blob: str, nonce: str,
        **overrides,
    ) -> dict:
        manifest = {
            "schema": "factory-runner-findings-receipt/v1",
            "result": "findings",
            "runner": self.RUNNER,
            "commit": commit,
            "tree": tree,
            "environment_blob": env_blob,
            "archive_sha256": "4" * 64,
            "capabilities": [self.CAPABILITY],
            "campaign_id": "campaign",
            "readiness_nonce": nonce,
            "authority_sha256": "5" * 64,
            "nonce": nonce,
            "exit_code": 8,
            "timed_out": False,
            "cleanup": True,
            "skip_marker_detected": False,
        }
        manifest.update(overrides)
        return manifest

    def _record(
        self, *, commit: str, nonce: str, manifest_raw: bytes, **overrides,
    ) -> dict:
        record = {
            "name": self.RUNNER,
            "manifest": (
                ".factory-state/runner-evidence/campaign/"
                f"{nonce}/{self.RUNNER}/{commit}/{nonce}/manifest.json"
            ),
            "manifest_sha256": sha256(manifest_raw),
            "result": "findings",
            "capabilities": [self.CAPABILITY],
            "probes": [{
                "capability": self.CAPABILITY,
                "exit_code": 8,
                "timed_out": False,
            }],
            "artifact_manifest_sha256": "6" * 64,
            "artifact_count": 1,
            "artifact_bytes": 1024,
            "signer": {
                "principal": self.RUNNER,
                "key_sha256": "7" * 64,
                "algorithm": "ssh-ed25519",
                "signature_sha256": "8" * 64,
            },
        }
        record.update(overrides)
        return record

    @contextlib.contextmanager
    def _readiness_authorities(self, *, human_blocked: bool = True):
        if human_blocked:
            human = unittest.mock.Mock(
                side_effect=(
                    campaign_module.readiness_module.HumanApprovalBlocked(
                        "blocked")))
        else:
            human = unittest.mock.Mock(return_value="2" * 64)
        with unittest.mock.patch.multiple(
            campaign_module.readiness_module,
            validate_core_mapping=unittest.mock.Mock(
                return_value="1" * 64),
            read_external_authority=unittest.mock.Mock(
                return_value=(b"trust-raw", b"")),
            validate_trust_anchor=unittest.mock.Mock(
                return_value={"keys": []}),
            validate_human_approval=human,
            validate_human_conformance=unittest.mock.Mock(
                return_value="c" * 64),
        ):
            yield

    def _build(
        self,
        *,
        gate_results=None,
        human_blocked=True,
        acquisition_digest=None,
        aggregate=None,
        aggregate_overrides=None,
        aggregate_bytes=None,
        write_aggregate=True,
        manifest_overrides=None,
        records=None,
    ):
        ws = self.make(SUCCESS_SCENARIO)
        config = ws.derive_config()
        # The direct readiness suite drives a provided (valid) external
        # human trust anchor so the human gate is evaluated through the
        # stubbed approval authority rather than short-circuited by the
        # canonical absent-anchor product finding.
        config = dataclasses.replace(
            config,
            human_trust_anchor="/trusted/anchor.json",
            human_trust_anchor_sha256="0" * 64,
        )
        nonce = self.NONCE
        commit = _git(ws.root, "rev-parse", "HEAD").stdout.strip()
        tree = _git(ws.root, "rev-parse", "HEAD^{tree}").stdout.strip()
        env_blob = "c" * 40
        policy_raw = self._policy_raw()
        environment_raw = self._environment_raw()
        contracts_raw = self._contracts_raw()
        git = unittest.mock.Mock()
        git.role_dirty_paths.return_value = False
        git.head.return_value = commit
        git.object_id.side_effect = lambda rev: {
            f"{commit}^{{tree}}": tree,
            f"{commit}:.factory/environment.toml": env_blob,
        }[rev]
        git.blob_at.side_effect = lambda c, relpath: {
            campaign_module.readiness_module.READINESS_POLICY_PATH: policy_raw,
            campaign_module.DEFAULT_CONFORMANCE_PATH: b"{}",
            ".factory/requirement-policy.json": b"{}",
            campaign_module.readiness_module.APPROVAL_PATH: (
                b'{"candidate_commit": "' + commit.encode() + b'"}'),
            ".factory/environment.toml": environment_raw,
            ".factory/capability-contracts.json": contracts_raw,
        }.get(relpath, b"")
        binding = state_module.empty_readiness(required=True)
        binding.update({
            "nonce": nonce,
            "accepted_commit": commit,
            "tree": tree,
            "environment_blob": env_blob,
            "specification_sha256": config.specification_digest,
            "plan_sha256": config.plan_digest,
            "conformance_sha256": "1" * 64,
            "policy_sha256": "2" * 64,
            "readiness_policy_sha256": campaign_module.plan_sha256(
                policy_raw),
            "contracts_sha256": campaign_module.plan_sha256(
                contracts_raw),
            "install_manifest_sha256": "3" * 64,
            "command_authority_sha256": "9" * 64,
            "human_authority_sha256": "a" * 64,
            "trust_authority_sha256": "b" * 64,
        })
        state = state_module.init_state(
            ws.root,
            campaign_id=config.campaign_id,
            rounds_requested=config.rounds_requested,
            specification_digest=config.specification_digest,
            plan_digest=config.plan_digest,
            role_prompt_digests=dict(config.role_prompt_digests),
            audit_objectives_digest=config.audit_objectives_digest,
            pre_round_hook_configuration_digest=(
                config.pre_round_hook_configuration_digest),
            pre_round_hook_commit=config.pre_round_hook_commit,
            phase_base_commit=config.phase_base_commit,
            readiness_required=True,
            readiness_binding=binding,
            branch=config.branch,
        )
        # The mutable worktree policy must be byte-bound to the committed
        # policy digest the readiness state binds.
        (ws.root / ".factory" / "readiness-policy.json").write_bytes(
            policy_raw)
        if records is None:
            manifest = self._manifest(
                commit=commit, tree=tree, env_blob=env_blob, nonce=nonce,
                **(manifest_overrides or {}))
            manifest_raw = json.dumps(
                manifest, sort_keys=True, separators=(",", ":")).encode()
            record = self._record(
                commit=commit, nonce=nonce, manifest_raw=manifest_raw)
            records = [record]
        else:
            manifest_raw = None
        if aggregate_bytes is None:
            if aggregate is None:
                aggregate = {
                    "schema": "factory-runner-findings-aggregate/v1",
                    "campaign_id": "campaign",
                    "readiness_nonce": nonce,
                    "commit": commit,
                    "tree": tree,
                    "environment_blob": env_blob,
                    "runners": records,
                }
            if aggregate_overrides:
                aggregate.update(aggregate_overrides)
            aggregate_bytes = (
                json.dumps(
                    aggregate, sort_keys=True, separators=(",", ":")).encode()
                + b"\n"
            )
        digest = acquisition_digest or sha256(aggregate_bytes)
        if write_aggregate:
            evidence_dir = (
                ws.root / ".factory-state" / "runner-evidence"
                / "campaign" / nonce
            )
            evidence_dir.mkdir(mode=0o700, parents=True)
            for candidate in (
                ws.root / ".factory-state" / "runner-evidence",
                ws.root / ".factory-state" / "runner-evidence" / "campaign",
                evidence_dir,
            ):
                candidate.chmod(0o700)
            aggregate_path = evidence_dir / "findings-aggregate.json"
            aggregate_path.write_bytes(aggregate_bytes)
            os.chmod(aggregate_path, 0o600)
        if manifest_raw is not None:
            runner_dir = (
                ws.root / ".factory-state" / "runner-evidence"
                / "campaign" / nonce / self.RUNNER / commit / nonce
            )
            runner_dir.mkdir(mode=0o700, parents=True)
            for candidate in (
                ws.root / ".factory-state" / "runner-evidence"
                / "campaign" / nonce / self.RUNNER,
                ws.root / ".factory-state" / "runner-evidence"
                / "campaign" / nonce / self.RUNNER / commit,
                runner_dir,
            ):
                candidate.chmod(0o700)
            manifest_path = runner_dir / "manifest.json"
            manifest_path.write_bytes(manifest_raw)
            os.chmod(manifest_path, 0o600)
        gates = {
            "capability": (True, 0, "", False),
            "core acceptance": (True, 0, "", False),
            "conformance validator": (True, 0, "", False),
        }
        gates.update(gate_results or {})
        acquisition = {
            "status": "findings",
            "aggregate_sha256": digest,
            "head": commit,
            "tree": tree,
            "environment_blob": env_blob,
        }
        harness = _RunnerFindingsReadinessHarness(
            config, acquisition=acquisition, gates=gates, binding=binding)
        harness._git = git
        return ws, harness, state, {
            "nonce": nonce, "commit": commit, "tree": tree,
            "env_blob": env_blob, "aggregate_bytes": aggregate_bytes,
            "digest": digest, "manifest_raw": manifest_raw,
            "records": records,
        }

    def _readiness(self, ws):
        return state_module.load_state(
            ws.root,
            expected_branch=BRANCH,
            expected_campaign_id="campaign",
            expected_rounds_requested=1,
            expected_readiness_required=True,
        ).readiness

    # -- positive projection ------------------------------------------------

    def test_runner_findings_projection_publishes_digest_bound_plannable(
        self,
    ) -> None:
        ws, harness, state, ctx = self._build()
        with self._readiness_authorities(human_blocked=False):
            final_state, terminal = harness._run_readiness(state)
        self.assertIsNone(terminal)
        readiness = self._readiness(ws)
        self.assertEqual(readiness["status"], "infrastructure_ready")
        self.assertEqual(readiness["terminal_outcome"], "plannable")
        self.assertEqual(
            readiness["findings_aggregate_sha256"], ctx["digest"])
        # The canonical payload digest is the one the readiness artifact/state
        # binds — never a self-derived or free-text digest.
        artifact = json.loads(
            (ws.root / ".factory-state"
             / findings_module.READINESS_FINDINGS_NAME).read_bytes())
        self.assertEqual(artifact["campaign_id"], "campaign")
        self.assertEqual(
            artifact["readiness_nonce"], ctx["nonce"])
        self.assertEqual(
            artifact["accepted_commit"], ctx["commit"])
        payload = artifact["payload"]
        self.assertEqual(payload["schema"], "factory-findings/v1")
        self.assertEqual(payload["source_round"], 0)
        self.assertEqual(len(payload["entries"]), 1)
        entry = payload["entries"][0]
        self.assertEqual(entry["phase"], "readiness")
        self.assertEqual(entry["outcome"], "findings")
        self.assertEqual(entry["findings"], [self.expected_code()])
        self.assertEqual(entry["blocked_on"], [])
        # The structured payload is prose-free and its digest binds exactly.
        self.assertEqual(
            artifact["payload_sha256"],
            findings_module.sha256(findings_module.payload_bytes(payload)))
        self.assertEqual(
            readiness["product_findings_sha256"],
            campaign_module.plan_sha256(
                findings_module.payload_bytes(payload)))
        self.assertEqual(
            readiness["product_findings_sha256"],
            artifact["payload_sha256"],
        )
        # The three infrastructure/product authorities executed.
        self.assertEqual(
            [label for label, _ in harness.gate_calls],
            ["capability", "core acceptance", "conformance validator"])
        # The published state advances the round-zero terminal to planning.
        self.assertEqual(final_state.current_phase, "planning")
        self.assertEqual(final_state.current_round, 1)

    def test_local_product_gate_findings_merge_into_structured_entries(
        self,
    ) -> None:
        # The fixed local product-gate findings merge after the structured
        # runner entry into one canonical deterministic payload; the runner
        # prose sentence never appears.
        ws, harness, state, ctx = self._build(
            gate_results={
                "core acceptance": (True, 1, "core probe failed", False),
                "conformance validator": (True, 1, "partial rows", False),
            },
        )
        with self._readiness_authorities(human_blocked=True):
            final_state, terminal = harness._run_readiness(state)
        self.assertIsNone(terminal)
        readiness = self._readiness(ws)
        self.assertEqual(readiness["terminal_outcome"], "plannable")
        artifact = json.loads(
            (ws.root / ".factory-state"
             / findings_module.READINESS_FINDINGS_NAME).read_bytes())
        entries = artifact["payload"]["entries"]
        self.assertEqual(len(entries), 4)
        self.assertEqual(entries[0]["findings"], [self.expected_code()])
        self.assertEqual(
            entries[1]["findings"],
            ["core acceptance reported product findings"])
        self.assertEqual(
            entries[2]["findings"],
            ["conformance evaluation reported product findings"])
        self.assertEqual(
            entries[3]["findings"],
            ["human graphics acceptance remains blocked"])
        blob = json.dumps(artifact["payload"]).encode()
        self.assertNotIn(
            b"signed runner probes reported product findings", blob)
        self.assertEqual(
            readiness["product_findings_sha256"],
            artifact["payload_sha256"],
        )

    # -- fail-closed: absent / digest / path / binding / skip --------------

    def test_missing_findings_aggregate_is_infrastructure_failure(self) -> None:
        ws, harness, state, ctx = self._build(write_aggregate=False)
        with self._readiness_authorities():
            with self.assertRaises(campaign_module.CampaignBindingError):
                harness._run_readiness(state)

    def test_re_read_aggregate_digest_mismatch_fails_closed(self) -> None:
        ws, harness, state, ctx = self._build(
            acquisition_digest=findings_module.sha256(b"other bytes"))
        with self._readiness_authorities():
            with self.assertRaises(campaign_module.CampaignBindingError):
                harness._run_readiness(state)

    def test_symlinked_aggregate_fails_closed(self) -> None:
        ws, harness, state, ctx = self._build()
        evidence_dir = (
            ws.root / ".factory-state" / "runner-evidence"
            / "campaign" / self.NONCE
        )
        aggregate_path = evidence_dir / "findings-aggregate.json"
        target = ws.root / "aggregate-target.json"
        target.write_bytes(b"{}")
        aggregate_path.unlink()
        os.symlink(target, aggregate_path)
        with self._readiness_authorities():
            with self.assertRaises(campaign_module.CampaignBindingError):
                harness._run_readiness(state)

    def test_aggregate_git_binding_mismatch_fails_closed(self) -> None:
        # The aggregate binds a commit that disagrees with the trusted
        # acquisition/Git identity: a stale or replayed artifact fails closed.
        ws, harness, state, ctx = self._build(
            aggregate_overrides={"commit": "b" * 40})
        with self._readiness_authorities():
            with self.assertRaises(campaign_module.CampaignBindingError):
                harness._run_readiness(state)

    def test_skipped_or_simulated_manifest_fails_closed(self) -> None:
        # A signed findings receipt showing skip/simulation markers can never
        # be projected into a plannable product finding.
        ws, harness, state, ctx = self._build(
            manifest_overrides={"skip_marker_detected": True})
        with self._readiness_authorities():
            with self.assertRaises(campaign_module.CampaignBindingError):
                harness._run_readiness(state)

    def test_declaration_mismatch_fails_closed(self) -> None:
        # The aggregate record's capability set must exactly match the
        # committed environment declaration.  A fresh aggregate byte stream is
        # written with a runner whose capability set disagrees with the
        # committed declaration while every signed evidence file stays exact.
        ws, harness, state, ctx = self._build()
        agg = self._aggregate_doc(ctx, capabilities=["systemd-user"])
        agg_bytes = (
            json.dumps(agg, sort_keys=True, separators=(",", ":")).encode()
            + b"\n"
        )
        evidence_dir = (
            ws.root / ".factory-state" / "runner-evidence"
            / "campaign" / self.NONCE
        )
        aggregate_path = evidence_dir / "findings-aggregate.json"
        aggregate_path.write_bytes(agg_bytes)
        os.chmod(aggregate_path, 0o600)
        harness._test_acquisition["aggregate_sha256"] = sha256(agg_bytes)
        with self._readiness_authorities():
            with self.assertRaises(campaign_module.CampaignBindingError):
                harness._run_readiness(state)

    def _aggregate_doc(self, ctx: dict, *, capabilities) -> dict:
        manifest_raw = ctx["manifest_raw"]
        manifest = json.loads(manifest_raw)
        record = self._record(
            commit=ctx["commit"], nonce=self.NONCE,
            manifest_raw=json.dumps(
                manifest, sort_keys=True, separators=(",", ":")).encode())
        record["capabilities"] = list(capabilities)
        return {
            "schema": "factory-runner-findings-aggregate/v1",
            "campaign_id": "campaign",
            "readiness_nonce": self.NONCE,
            "commit": ctx["commit"],
            "tree": ctx["tree"],
            "environment_blob": ctx["env_blob"],
            "runners": [record],
        }


class BoundedReadinessReasonTests(_CampaignBase):
    """BUG-0026: bounded readiness failure reasons.

    The runner stderr classifier matches only anchored first-party markers
    from the canonical runner command; raw output, hostnames, paths, remote
    bytes, nonces, and unknown prose never influence the result and fail
    closed to ``generic_integrity_failure``.  Every terminal path publishes a
    valid closed-enum reason (``none`` when not applicable), and the public
    readiness/campaign result JSON carries it.
    """

    def test_classifier_maps_first_party_markers_to_categories(self) -> None:
        cases = [
            ("factory-runner: runner gpu transport failed: connection reset",
             readiness_module.TERMINAL_REASON_TRANSPORT),
            ("factory-runner: runner gpu broker refused nonce issuance",
             readiness_module.TERMINAL_REASON_ENROLLMENT),
            ("factory-runner: runner gpu returned malformed protocol output",
             readiness_module.TERMINAL_REASON_PROTOCOL),
            ("factory-runner: canonical signature checker cannot be loaded",
             readiness_module.TERMINAL_REASON_SIGNATURE),
            ("factory-runner: runner evidence publication collision",
             readiness_module.TERMINAL_REASON_MANIFEST),
            ("factory-runner: runner evidence records carry no exact classified result",
             readiness_module.TERMINAL_REASON_AGGREGATE),
            ("factory-runner: runner gpu root semantics failed before publication",
             readiness_module.TERMINAL_REASON_CAPABILITY),
            ("factory-runner: committed factory environment is invalid TOML",
             readiness_module.TERMINAL_REASON_ENROLLMENT),
        ]
        for stderr, expected in cases:
            with self.subTest(stderr=stderr):
                self.assertEqual(
                    campaign_module.classify_runner_failure(
                        campaign_module.RUNNER_INTEGRITY_EXIT, stderr),
                    expected,
                )

    def test_classifier_unknown_and_adversarial_stderr_collapse_to_generic(self) -> None:
        # Credentials, hostnames, absolute paths, remote bytes, and unknown
        # prose must never surface in a bounded reason and never match a
        # category: they fail closed to generic_integrity_failure.
        adversarial = [
            "GITHUB_TOKEN=ghp_super_secret_12345 leaked from runner",
            "ssh://root@10.0.0.5:22/var/run/runner.sock refused",
            "/home/operator/.ssh/id_ed25519 permission denied",
            "nonce=deadbeefcafebabe transport bytes 0x7f 0x45 0x4c 0x46",
        ]
        for stderr in adversarial:
            with self.subTest(stderr=stderr):
                self.assertEqual(
                    campaign_module.classify_runner_failure(
                        campaign_module.RUNNER_INTEGRITY_EXIT, stderr),
                    readiness_module.TERMINAL_REASON_GENERIC,
                )

    def test_classifier_exit_code_fallbacks(self) -> None:
        # Success and executed product findings are not infrastructure
        # failures; a bare transport exit maps to transport; any other
        # unknown exit fails closed to generic.
        self.assertEqual(
            campaign_module.classify_runner_failure(0, ""),
            readiness_module.TERMINAL_REASON_NONE,
        )
        self.assertEqual(
            campaign_module.classify_runner_failure(
                campaign_module.RUNNER_FINDINGS_EXIT, ""),
            readiness_module.TERMINAL_REASON_NONE,
        )
        self.assertEqual(
            campaign_module.classify_runner_failure(
                campaign_module.RUNNER_TRANSPORT_EXIT, ""),
            readiness_module.TERMINAL_REASON_TRANSPORT,
        )
        self.assertEqual(
            campaign_module.classify_runner_failure(99, ""),
            readiness_module.TERMINAL_REASON_GENERIC,
        )

    def test_runner_acquisition_persists_bounded_reason(self) -> None:
        ws = self.make(SUCCESS_SCENARIO)
        config = ws.derive_config()
        campaign = campaign_module.Campaign(config)
        campaign._acquire()
        try:
            campaign._write_runner_acquisition(
                attempt=1, status="integrity_failure",
                head="0" * 40, tree="0" * 40,
                environment_blob="0" * 40,
                terminal_reason=readiness_module.TERMINAL_REASON_PROTOCOL,
                diagnostic="fixture",
            )
            persisted = campaign._read_runner_acquisition()
            self.assertIsNotNone(persisted)
            self.assertEqual(
                persisted["terminal_reason"],
                readiness_module.TERMINAL_REASON_PROTOCOL,
            )
            # An unbound reason is rejected before any write.
            with self.assertRaises(campaign_module.CampaignBindingError):
                campaign._write_runner_acquisition(
                    attempt=1, status="integrity_failure",
                    head="0" * 40, tree="0" * 40,
                    environment_blob="0" * 40,
                    terminal_reason="not-a-bounded-reason",
                    diagnostic="fixture",
                )
        finally:
            campaign._lock.release()

    def test_readiness_result_schema_and_digest_carry_reason(self) -> None:
        bindings = {"accepted_commit": "a" * 40, "tree": "b" * 40,
                    "environment_blob": "c" * 40,
                    **{k: "d" * 64 for k in (
                        "specification_sha256", "plan_sha256",
                        "conformance_sha256", "policy_sha256",
                        "readiness_policy_sha256", "contracts_sha256",
                        "install_manifest_sha256", "command_authority_sha256",
                        "human_authority_sha256", "trust_authority_sha256")}}
        results = {k: "e" * 64 for k in (
            "aggregate_sha256", "capability_result_sha256",
            "core_result_sha256", "conformance_result_sha256",
            "human_result_sha256")}
        results.update({"findings_aggregate_sha256": "0" * 64,
                        "product_findings_sha256": "0" * 64})
        value = readiness_module.result_document(
            campaign_id="campaign-a", nonce="f" * 64, status="complete",
            terminal_outcome="pass",
            terminal_reason=readiness_module.TERMINAL_REASON_NONE,
            bindings=bindings, results=results)
        self.assertEqual(value["terminal_reason"], "none")
        readiness_module.validate_result(value)
        # An unbound reason is rejected by the readiness validator.
        bad = dict(value)
        bad["terminal_reason"] = "not-a-bounded-reason"
        with self.assertRaises(readiness_module.ReadinessError):
            readiness_module.validate_result(bad)

    def test_campaign_result_public_json_carries_reason(self) -> None:
        result = campaign_module.CampaignResult(
            campaign_id="campaign", rounds_requested=1, rounds_completed=1,
            terminal_phase="success", terminal_outcome="success",
            head_commit="0" * 40,
            phase_history=(
                campaign_module.PhaseRecord(
                    round=1, phase="audit", attempt=1, outcome="pass",
                    head_commit="0" * 40, plan_digest="0" * 64,
                ),
            ),
            terminal_reason=readiness_module.TERMINAL_REASON_NONE,
        )
        result.validate()
        campaign_module.validate_campaign_result(result)
        self.assertEqual(result.to_dict()["terminal_reason"], "none")
        # An unbound reason fails the model and the committed schema.
        bad = dataclasses.replace(
            result, terminal_reason="not-a-bounded-reason")
        with self.assertRaises(campaign_module.CampaignResultError):
            bad.validate()
        with self.assertRaises(campaign_module.CampaignResultError):
            campaign_module.validate_campaign_result(bad)

    def test_readiness_only_result_reason_is_none(self) -> None:
        # A readiness-only completion is not an infrastructure failure, so
        # its terminal reason must be ``none`` and the result must validate
        # against the committed schema.
        result = campaign_module.CampaignResult(
            campaign_id="campaign", rounds_requested=1, rounds_completed=0,
            terminal_phase="readiness_complete",
            terminal_outcome="readiness_complete",
            head_commit="0" * 40,
            terminal_reason=readiness_module.TERMINAL_REASON_NONE,
        )
        result.validate()
        campaign_module.validate_campaign_result(result)
        self.assertEqual(result.to_dict()["terminal_reason"], "none")
        # A readiness-only result must never impersonate a completed campaign.
        bad = dataclasses.replace(
            result, terminal_reason=readiness_module.TERMINAL_REASON_GENERIC)
        self.assertEqual(bad.terminal_reason, "generic_integrity_failure")
        # An unbound reason is rejected by the model and the committed schema.
        invalid = dataclasses.replace(result, terminal_reason="not-a-reason")
        with self.assertRaises(campaign_module.CampaignResultError):
            invalid.validate()
        with self.assertRaises(campaign_module.CampaignResultError):
            campaign_module.validate_campaign_result(invalid)


if __name__ == "__main__":
    unittest.main()
