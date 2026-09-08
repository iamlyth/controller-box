#!/usr/bin/env python3
"""Focused unit coverage for the meaningful-commit substance classifier.

Exercises the two decisions the Git commit boundary delegates to
``.factory/loop/substance.py``:

* ``classify_path_set`` splits a staged path set into the narrow
  administrative set (implementation plan, bug ledgers, campaign audit and
  evidence sidecars) versus substantive paths.  Stable source/config/policies/
  schemas/prompts, product config/docs/tests, and the authenticated
  conformance/capability contracts fall on the substantive side.

* ``plan_has_semantic_change`` decides whether a canonical-plan revision
  carries genuine semantic planning change: a task add/remove/reorder, or a
  change to a task's title, priority, dependencies, description (``Scope``),
  or acceptance criteria (``Acceptance criteria``).  Evidence prose,
  completion/status markers, Verification/Documentation-impact text, and
  timestamps must not count as a change; an unparseable revision fails closed.

The semantic mutations are built by manipulating the parsed ``plan_parser``
raw blocks and re-serializing, so the suite stays robust against the committed
implementation plan evolving across phases (mutations are keyed by task number
and field label, which the ``factory-plan/v1`` grammar guarantees every task
carries).
"""

from __future__ import annotations

import json
import re
import sys
import unittest
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(PROJECT_ROOT / ".factory" / "loop"))

from plan_parser import Plan  # noqa: E402
from substance import (  # noqa: E402
    ADMIN_PATHS,
    IMPLEMENTATION_PLAN,
    SEMANTIC_FIELDS,
    classify_path_set,
    plan_fingerprint,
    plan_has_semantic_change,
)

PLAN_PATH = PROJECT_ROOT / ".factory" / "artifacts" / "implementation-plan.md"


def find_task_block(plan: Plan, number: int):
    """Return the raw ``Block`` backing the given task number, or None."""
    for block in plan._blocks:
        heading = (block.heading or "").strip()
        match = re.match(r"^## Task\s+(\d+):", heading)
        if match and int(match.group(1)) == number:
            return block
    return None


def field_index(block, field: str) -> int:
    """Return the index of a task field's first line within a block, or -1."""
    for index, line in enumerate(block.lines):
        if line.strip().startswith(f"- {field}:"):
            return index
    return -1


class ClassifyPathSetTest(unittest.TestCase):
    """Path-set classification and the narrow administrative boundary."""

    def test_plan_is_administrative(self):
        admin, substantive = classify_path_set((IMPLEMENTATION_PLAN,))
        self.assertEqual(substantive, [])
        self.assertEqual(admin, [IMPLEMENTATION_PLAN])

    def test_bug_ledgers_and_campaign_sidecars_are_administrative(self):
        for path in (
            ".factory/bugs/open.md",
            ".factory/bugs/closed.md",
            ".factory/artifacts/campaign-audit.md",
            ".factory/artifacts/blocked-facts.json",
        ):
            with self.subTest(path=path):
                self.assertIn(path, ADMIN_PATHS)
                admin, substantive = classify_path_set((path,))
                self.assertEqual(admin, [path])
                self.assertEqual(substantive, [])

    def test_stable_subsystem_paths_are_substantive(self):
        for path in (
            "src/controller.c",
            ".factory/loop/plan_parser.py",
            ".factory/config.toml",
            ".factory/schemas/conformance.schema.json",
            ".factory/prompts/developer.md",
            ".factory/artifacts/conformance.json",
            ".factory/capability-contracts.json",
            "docs/SPEC.md",
            "README.md",
            "tests/CMakeLists.txt",
            ".factory/tests/test-git-commit-guard.sh",
        ):
            with self.subTest(path=path):
                admin, substantive = classify_path_set((path,))
                self.assertEqual(substantive, [path])
                self.assertEqual(admin, [])

    def test_mixed_set_reports_both(self):
        admin, substantive = classify_path_set(
            (IMPLEMENTATION_PLAN, "src/controller.c")
        )
        self.assertEqual(admin, [IMPLEMENTATION_PLAN])
        self.assertEqual(substantive, ["src/controller.c"])

    def test_empty_set(self):
        self.assertEqual(classify_path_set(()), ([], []))

    def test_exact_membership_survives_paths_with_odd_bytes(self):
        # Membership is exact; a sibling file must not match the plan by prefix.
        _, substantive = classify_path_set(
            (".factory/artifacts/implementation-plan.md.bak",)
        )
        self.assertIn(".factory/artifacts/implementation-plan.md.bak", substantive)


class PlanSemanticChangeTest(unittest.TestCase):
    """Semantic canonical-plan revision decision."""

    @classmethod
    def setUpClass(cls):
        cls.base = PLAN_PATH.read_bytes()
        cls.base_text = cls.base.decode("utf-8")
        cls.plan = Plan.from_bytes(cls.base)
        cls.fingerprint = plan_fingerprint(cls.base)
        assert cls.fingerprint is not None, "committed plan must parse"

    def _mutate_field_line(self, number, field, transform):
        """Re-serialize the plan with ``transform`` applied to a task's first
        ``<field>:`` line."""
        plan = Plan.from_text(self.base_text)
        block = find_task_block(plan, number)
        assert block is not None, f"task {number} block missing"
        index = field_index(block, field)
        assert index != -1, f"task {number} missing field {field}"
        block.lines[index] = transform(block.lines[index])
        return plan.serialize().encode("utf-8")

    def _append_continuation(self, number, field):
        """Re-serialize the plan with an extra continuation line on a field."""
        plan = Plan.from_text(self.base_text)
        block = find_task_block(plan, number)
        assert block is not None
        index = field_index(block, field)
        assert index != -1
        block.lines.insert(index + 1, "  appended continuation note")
        return plan.serialize().encode("utf-8")

    def test_title_change_is_semantic(self):
        # A title change is a genuine planning change (the heading is the first
        # line of the task block).
        plan = Plan.from_text(self.base_text)
        block = find_task_block(plan, 1)
        assert block is not None
        block.lines[0] = block.lines[0].rstrip() + " (revised scope)"
        new = plan.serialize().encode("utf-8")
        self.assertIsNotNone(plan_fingerprint(new))
        self.assertTrue(plan_has_semantic_change(self.base, new))

    def test_priority_change_is_semantic(self):
        def bump_priority(line):
            match = re.match(r"^(-\s*Priority:\s*)(\d+)$", line.strip())
            if not match:
                raise AssertionError(line)
            return line.replace(match.group(2), str(int(match.group(2)) + 1), 1)
        new = self._mutate_field_line(1, "Priority", bump_priority)
        self.assertIsNotNone(plan_fingerprint(new))
        self.assertTrue(plan_has_semantic_change(self.base, new))

    def test_scope_change_is_semantic(self):
        new = self._mutate_field_line(1, "Scope", lambda line: line + " [revised]")
        self.assertIsNotNone(plan_fingerprint(new))
        self.assertTrue(plan_has_semantic_change(self.base, new))

    def test_acceptance_change_is_semantic(self):
        new = self._mutate_field_line(
            1, "Acceptance criteria", lambda line: line + " [revised]"
        )
        self.assertIsNotNone(plan_fingerprint(new))
        self.assertTrue(plan_has_semantic_change(self.base, new))

    def test_dependency_change_is_semantic(self):
        # Reduce a non-final task's dependency list by one; removing a
        # dependency cannot create a self/later/cycle reference so the plan
        # stays on the accepted grammar.
        tasks = self.plan.to_dict()["tasks"]
        last = tasks[-1]["number"]
        target = targ_deps = None
        for task in tasks:
            if task["number"] != last and len(task["dependencies"]) >= 2:
                target, targ_deps = task["number"], task["dependencies"]
                break
        if target is None:  # defensive: should never happen for this ledger
            self.skipTest("no non-final task with >=2 dependencies found")
        reduced = targ_deps[:-1]
        new_dep_line = "- Dependencies: " + ", ".join(
            "Task %d" % number for number in reduced
        )
        plan = Plan.from_text(self.base_text)
        block = find_task_block(plan, target)
        assert block is not None
        index = field_index(block, "Dependencies")
        assert index != -1
        block.lines[index] = new_dep_line
        new = plan.serialize().encode("utf-8")
        self.assertIsNotNone(Plan.from_bytes(new) or plan_fingerprint(new))
        self.assertIsNotNone(plan_fingerprint(new))
        self.assertTrue(plan_has_semantic_change(self.base, new))

    def test_evidence_prose_is_not_semantic(self):
        # Append Evidence prose (a task with an Evidence field guarantees a
        # non-semantic field exists to exercise the "prose is not a change"
        # rule).
        target = None
        for task in self.plan.to_dict()["tasks"]:
            if "Evidence" in task["fields"]:
                target = task["number"]
                break
        if target is None:
            self.skipTest("no task with an Evidence field found")
        new = self._append_continuation(target, "Evidence")
        self.assertIsNotNone(plan_fingerprint(new))
        self.assertFalse(plan_has_semantic_change(self.base, new))
        self.assertEqual(plan_fingerprint(new), self.fingerprint)

    def test_verification_prose_is_not_semantic(self):
        new = self._append_continuation(1, "Verification")
        self.assertIsNotNone(plan_fingerprint(new))
        self.assertFalse(plan_has_semantic_change(self.base, new))
        self.assertEqual(plan_fingerprint(new), self.fingerprint)

    def test_identical_bytes_are_not_a_change(self):
        self.assertFalse(
            plan_has_semantic_change(self.base, self.base)
        )

    def test_newly_introduced_plan_is_a_change(self):
        # HEAD never tracked the plan, so the old revision is empty; a valid
        # new plan establishes the whole ledger and is a genuine change.
        self.assertTrue(plan_has_semantic_change(b"", self.base))
        self.assertTrue(plan_has_semantic_change(b"\n", self.base))

    def test_unparseable_revision_fails_closed(self):
        self.assertIsNone(plan_has_semantic_change(self.base, b"not a canonical plan"))
        self.assertIsNone(plan_has_semantic_change(b"not a canonical plan", self.base))
        self.assertIsNone(
            plan_has_semantic_change(b"\xef\xbb\xbfgarbage", self.base)
        )
        self.assertIsNone(plan_fingerprint(b"garbage"))

    def test_fingerprint_orders_tasks_and_excludes_status_prose(self):
        projection = json.loads(self.fingerprint)
        numbers = [task["number"] for task in projection["tasks"]]
        self.assertEqual(numbers, list(range(1, len(numbers) + 1)))
        for task in projection["tasks"]:
            self.assertEqual(
                set(task.keys()), {"number", "title", "dependencies", "fields"}
            )
            self.assertEqual(set(task["fields"].keys()), set(SEMANTIC_FIELDS))

    def test_fingerprint_is_deterministic(self):
        self.assertEqual(plan_fingerprint(self.base), self.fingerprint)


if __name__ == "__main__":
    unittest.main(verbosity=2)
