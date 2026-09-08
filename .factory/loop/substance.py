#!/usr/bin/env python3
"""Shared meaningful-commit substance classifier.

The Git commit boundary (``.factory/tools/git-commit-guard.sh``) consults this
module so the meaningful-commit policy is enforced by one deterministic,
NUL/path-boundary-safe classifier instead of ad-hoc per-hook logic.  It
classifies a staged path set and, when the staged set contains no substantive
path, whether a canonical implementation-plan revision carries genuine
semantic planning change.

Narrow administrative paths
---------------------------
Only the following mutable control artifacts are administrative:

  * the canonical implementation plan
    (``.factory/artifacts/implementation-plan.md``),
  * the bug ledgers (``.factory/bugs/open.md``, ``.factory/bugs/closed.md``),
  * the campaign audit sidecar (``.factory/artifacts/campaign-audit.md``),
    and
  * the campaign evidence sidecar (``.factory/artifacts/blocked-facts.json``).

Every other tracked path - stable source, configuration, policies, JSON
schemas, prompts, product configuration/docs/tests, and the authenticated
conformance/capability contracts (``.factory/artifacts/conformance.json``,
``.factory/capability-contracts.json``) - is substantive.

A commit whose staged set is entirely administrative is allowed only when the
implementation plan carries a genuine semantic planning change: a task
add/remove/reorder, or a change to a task's title, priority, dependencies,
description (``Scope``), or acceptance criteria (``Acceptance criteria``).
Merely rewording ``Evidence``, flipping a completion/status marker, touching
timestamps, or editing iteration prose is not a meaningful change and the
commit is rejected, so an agent can never manufacture metadata-only progress
through the plan.

Semantic fingerprint
--------------------
The fingerprint reuses the deterministic parser in ``plan_parser`` to project
only the semantic planning surface: per task, number, title, the parsed
dependency list (never the raw ``Dependencies`` line formatting), ``Scope``,
``Acceptance criteria``, ``Priority``, and the task's ``Blocked on``
reference; plus the canonical conformance matrix rows (requirement id,
classification, and bound task references) and the interaction inventory
(boundary and text) the parser exposes as legitimate planning structures.
The projection omits status, Evidence, completion markers, Verification,
Documentation impact, front-matter lifecycle fields, the conformance
``Evidence`` cell prose, and iteration prose.  Two revisions expose a genuine
change exactly when their fingerprints differ.  A revision that no longer
parses cannot be shown to carry a genuine planning change, so it fails closed
(the loop already requires the committed plan to stay on the accepted
``factory-plan/v1`` grammar).  A plan newly introduced into a repository (the
old revision is empty because HEAD never tracked it) that parses is treated
as a genuine change: the whole task ledger is established in one revision.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import List, Optional, Tuple

# plan_parser is a self-contained stdlib-only module in the same directory;
# when this file is executed directly (as the commit guard and the focused
# unit test do) its directory is on ``sys.path`` first, so the absolute import
# resolves deterministically.  When imported as part of the installed/alias
# ``factory.loop`` package (``from . import substance``), ``plan_parser`` is
# the sibling ``factory.loop.plan_parser`` and only the relative import
# resolves; both contexts are supported so the shared classifier is the same
# authority whether it runs from a checkout, an isolated fixture, or an
# installed campaign.
try:
    from plan_parser import Plan, PlanError  # noqa: F401
except ImportError:  # package context: sibling ``factory.loop.plan_parser``
    from .plan_parser import Plan, PlanError  # type: ignore[no-redef]

IMPLEMENTATION_PLAN = ".factory/artifacts/implementation-plan.md"

# Narrow administrative mutable control artifacts.  Every other tracked path
# is substantive (stable source/config/policies/schemas/prompts, product
# config/docs/tests, and the authenticated conformance/capability contracts).
ADMIN_PATHS = frozenset({
    IMPLEMENTATION_PLAN,
    ".factory/artifacts/campaign-audit.md",
    ".factory/artifacts/blocked-facts.json",
    ".factory/bugs/open.md",
    ".factory/bugs/closed.md",
})

# Semantic planning fields projected for the fingerprint.  Description is the
# ``Scope`` field and acceptance is ``Acceptance criteria``.  ``Dependencies``
# is deliberately absent: dependencies are projected from the parsed dependency
# list (``task.dependencies``), so a pure formatting change of the raw
# ``- Dependencies:`` line (for example ``Task 1-3`` vs ``Task 1, Task 2,
# Task 3``) that expands to the same parsed list is not a semantic change.
# ``Blocked on``/``blocked_on`` is projected as a dedicated normalized field.
SEMANTIC_FIELDS = ("Scope", "Acceptance criteria", "Priority")


def classify_path_set(paths: Tuple[str, ...]) -> Tuple[List[str], List[str]]:
    """Split staged paths into ``(administrative, substantive)``.

    Membership is exact and rooted.  Callers hand over already NUL/split
    entries (``git diff --cached --name-only -z`` in the guard), so any path
    with spaces or newlines is handled exactly.  No slash is ever stripped:
    a sibling path that merely resembles an administrative path (for example
    a trailing-slash directory marker or a suffixed copy) must not match the
    administrative set by prefix or normalization.
    """
    admin: List[str] = []
    substantive: List[str] = []
    for raw in paths:
        if raw in ADMIN_PATHS:
            admin.append(raw)
        else:
            substantive.append(raw)
    return admin, substantive


def plan_fingerprint(data: bytes) -> Optional[str]:
    """Return the deterministic semantic planning fingerprint, or ``None`` when
    ``data`` is not a valid canonical plan (fail closed).

    The projection covers task add/remove/reorder (through ordered task
    numbers/titles), plus each task's title, the parsed dependency list (not
    the raw ``Dependencies`` line), Scope, Acceptance criteria, Priority, and
    the ``Blocked on`` reference; plus the canonical conformance matrix rows
    (requirement id, classification, bound task references) and the
    interaction inventory (boundary and text) that the parser exposes as
    legitimate planning structures.  It intentionally excludes status,
    Evidence, Verification, Documentation impact, the conformance ``Evidence``
    cell prose, and iteration prose.
    """
    try:
        plan = Plan.from_bytes(data)
    except PlanError:
        return None
    tasks: List[dict] = []
    for task in plan.tasks:
        tasks.append({
            "number": task.number,
            "title": task.title,
            "dependencies": list(task.dependencies),
            "blocked_on": task.blocked_on,
            "fields": {key: task.fields.get(key, "") for key in SEMANTIC_FIELDS},
        })
    matrix = [
        {
            "requirement_id": row.requirement_id,
            "classification": row.classification,
            "tasks": list(row.tasks),
        }
        for row in plan.matrix
    ]
    interactions = [
        {"boundary": entry.boundary, "text": entry.text}
        for entry in plan.interactions
    ]
    return json.dumps(
        {"tasks": tasks, "matrix": matrix, "interactions": interactions},
        sort_keys=True, separators=(",", ":"),
    )


def plan_has_semantic_change(old_data: bytes, new_data: bytes) -> Optional[bool]:
    """Return whether the plan revision is a genuine semantic planning change.

    Returns ``True``/``False``, or ``None`` when the revision cannot be
    judged (fail closed).  A newly introduced plan (the old revision is empty
    because it was not yet tracked at HEAD) that parses is a genuine change:
    the whole task ledger is established in one revision.  An otherwise
    unparseable revision cannot be proven meaningful, so it fails closed.
    """
    new_fp = plan_fingerprint(new_data)
    if new_fp is None:
        return None
    if not old_data.strip():
        return True
    old_fp = plan_fingerprint(old_data)
    if old_fp is None:
        return None
    return old_fp != new_fp


def main(argv: Optional[List[str]] = None) -> int:
    """Command-line boundary used by the Git commit guard."""
    parser = argparse.ArgumentParser(
        prog="substance",
        description="Meaningful-commit substance classifier.",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_classify = sub.add_parser(
        "classify", help="classify a staged path set (substantive | admin | empty)"
    )
    p_classify.add_argument(
        "paths", nargs="*", help="staged paths relative to the repository root"
    )

    p_plan = sub.add_parser(
        "plan-semantic",
        help="decide an implementation-plan revision (changed | unchanged | unparseable)",
    )
    p_plan.add_argument("--old", required=True, help="file with the HEAD plan bytes")
    p_plan.add_argument("--new", required=True, help="file with the staged plan bytes")

    p_admin = sub.add_parser(
        "admin",
        help="print one administrative path per line that is present in the staged set",
    )
    p_admin.add_argument("paths", nargs="*", help="staged paths")

    args = parser.parse_args(argv)

    if args.command == "admin":
        admin, _substantive = classify_path_set(tuple(args.paths))
        # Administrative paths are fixed NUL-free constants, so a
        # newline-delimited emission is unambiguous for the guard; paths are
        # deduplicated and ordered for deterministic shell comparison.
        for path in sorted(set(admin)):
            sys.stdout.write(path + "\n")
        return 0

    if args.command == "classify":
        admin, substantive = classify_path_set(tuple(args.paths))
        if substantive:
            sys.stdout.write("substantive\n")
        elif not admin and not substantive:
            sys.stdout.write("empty\n")
        else:
            sys.stdout.write(
                "admin %d\n" % (1 if IMPLEMENTATION_PLAN in args.paths else 0)
            )
        return 0

    # plan-semantic
    try:
        old_data = Path(args.old).read_bytes()
        new_data = Path(args.new).read_bytes()
    except OSError as exc:
        parser.error(f"cannot read plan file: {exc}")
        return 4

    decision = plan_has_semantic_change(old_data, new_data)
    if decision is None:
        sys.stdout.write("unparseable\n")
        return 2
    sys.stdout.write("changed\n" if decision else "unchanged\n")
    return 1 if not decision else 0


if __name__ == "__main__":
    sys.exit(main())
