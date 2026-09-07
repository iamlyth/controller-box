---
spec_path: docs/FACTORY-LOOP-SPEC.md
spec_commit: 1111111111111111111111111111111111111111
spec_blob: 2222222222222222222222222222222222222222
base_commit: 3333333333333333333333333333333333333333
status: active
---

# Implementation Plan

## Goal and non-goals

Goal: exercise the deterministic §8 selector resume rule.

## Architecture and constraints

Plan: Python 3.11 standard library only; selection is a pure function of the
parsed plan and never consults a runtime task ledger.

## Specification conformance matrix

| ID | Spec § | Classification | Evidence | Task |
|----|--------|--------------|----------|------|
| ARCH-01 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| ARCH-02 | §5, §9 | missing | fixture exercises selector determinism | Task 1 |
| ARCH-03 | §5, §18 | missing | fixture exercises selector determinism | Task 1 |
| ARCH-04 | §6 | missing | fixture exercises selector determinism | Task 1 |
| ARCH-05 | §7 | missing | fixture exercises selector determinism | Task 1 |
| SYS-01 | §7, §8 | missing | fixture exercises selector determinism | Task 1 |
| SYS-02 | §9, §20 | missing | fixture exercises selector determinism | Task 1 |
| SYS-03 | §10 | missing | fixture exercises selector determinism | Task 1 |
| SYS-04 | §10 | missing | fixture exercises selector determinism | Task 1 |
| SYS-05 | §11, §17 | missing | fixture exercises selector determinism | Task 1 |
| SYS-06 | §12 | missing | fixture exercises selector determinism | Task 1 |
| OVL-01 | §9, §12, §17 | missing | fixture exercises selector determinism | Task 1 |
| OVL-02 | §12, §17 | missing | fixture exercises selector determinism | Task 1 |
| OVL-03 | §13, §14 | missing | fixture exercises selector determinism | Task 1 |
| OVL-04 | §15 | missing | fixture exercises selector determinism | Task 1 |
| OVL-05 | §16 | missing | fixture exercises selector determinism | Task 1 |
| OVL-06 | §18 | missing | fixture exercises selector determinism | Task 1 |
| OVL-07 | §19 | missing | fixture exercises selector determinism | Task 1 |
| OVL-08 | §19 | missing | fixture exercises selector determinism | Task 1 |
| OVL-09 | §19 | missing | fixture exercises selector determinism | Task 1 |
| OVL-10 | §3 | missing | fixture exercises selector determinism | Task 1 |
| MGR-01 | §21 | missing | fixture exercises selector determinism | Task 1 |
| MGR-02 | §22 | missing | fixture exercises selector determinism | Task 1 |
| MGR-03 | §23 | missing | fixture exercises selector determinism | Task 1 |
| MGR-04 | §23 | missing | fixture exercises selector determinism | Task 1 |
| MGR-05 | §23 | missing | fixture exercises selector determinism | Task 1 |
| MGR-06 | §23 | missing | fixture exercises selector determinism | Task 1 |
| MGR-07 | §23 | missing | fixture exercises selector determinism | Task 1 |
| MGR-08 | §23 | missing | fixture exercises selector determinism | Task 1 |
| ID-01 | §23 | missing | fixture exercises selector determinism | Task 1 |
| ID-02 | §23 | missing | fixture exercises selector determinism | Task 1 |
| ID-03 | §23 | missing | fixture exercises selector determinism | Task 1 |
| CFG-01 | §23 | missing | fixture exercises selector determinism | Task 1 |
| CFG-02 | §23 | missing | fixture exercises selector determinism | Task 1 |
| CFG-03 | §23 | missing | fixture exercises selector determinism | Task 1 |
| CFG-04 | §23 | missing | fixture exercises selector determinism | Task 1 |
| CFG-05 | §23 | missing | fixture exercises selector determinism | Task 1 |
| CFG-06 | §23 | missing | fixture exercises selector determinism | Task 1 |
| ICN-01 | §23 | missing | fixture exercises selector determinism | Task 1 |
| ICN-02 | §23 | missing | fixture exercises selector determinism | Task 1 |
| ICN-03 | §23 | missing | fixture exercises selector determinism | Task 1 |
| ICN-04 | §23 | missing | fixture exercises selector determinism | Task 1 |
| ICN-05 | §23 | missing | fixture exercises selector determinism | Task 1 |
| PKG-01 | §23 | missing | fixture exercises selector determinism | Task 1 |
| PKG-02 | §23 | missing | fixture exercises selector determinism | Task 1 |
| PKG-03 | §23 | missing | fixture exercises selector determinism | Task 1 |
| PKG-04 | §23 | missing | fixture exercises selector determinism | Task 1 |
| PKG-05 | §23 | missing | fixture exercises selector determinism | Task 1 |
| DBUS-01 | §23 | missing | fixture exercises selector determinism | Task 1 |
| DBUS-02 | §23 | missing | fixture exercises selector determinism | Task 1 |
| DBUS-03 | §23 | missing | fixture exercises selector determinism | Task 1 |
| DBUS-04 | §23 | missing | fixture exercises selector determinism | Task 1 |
| DBUS-05 | §23 | missing | fixture exercises selector determinism | Task 1 |
| DBUS-06 | §23 | missing | fixture exercises selector determinism | Task 1 |
| DBUS-07 | §23 | missing | fixture exercises selector determinism | Task 1 |
| PERF-01 | §23 | missing | fixture exercises selector determinism | Task 1 |
| PERF-02 | §23 | missing | fixture exercises selector determinism | Task 1 |
| PERF-03 | §23 | missing | fixture exercises selector determinism | Task 1 |
| PERF-04 | §23 | missing | fixture exercises selector determinism | Task 1 |
| PERF-05 | §23 | missing | fixture exercises selector determinism | Task 1 |
| VRF-01 | §23 | missing | fixture exercises selector determinism | Task 1 |
| VRF-02 | §23 | missing | fixture exercises selector determinism | Task 1 |
| VRF-03 | §23 | missing | fixture exercises selector determinism | Task 1 |
| VRF-04 | §23 | missing | fixture exercises selector determinism | Task 1 |
| VRF-05 | §23 | missing | fixture exercises selector determinism | Task 1 |
| VRF-06 | §23 | missing | fixture exercises selector determinism | Task 1 |
| VRF-07 | §23 | missing | fixture exercises selector determinism | Task 1 |
| DOD-01 | §23 | missing | fixture exercises selector determinism | Task 1 |
| DOD-02 | §23 | missing | fixture exercises selector determinism | Task 1 |
| DOD-03 | §23 | missing | fixture exercises selector determinism | Task 1 |
| DOD-04 | §23 | missing | fixture exercises selector determinism | Task 1 |
| DOD-05 | §23 | missing | fixture exercises selector determinism | Task 1 |
| DOD-06 | §23 | missing | fixture exercises selector determinism | Task 1 |
| DOD-07 | §23 | missing | fixture exercises selector determinism | Task 1 |
| DOD-08 | §23 | missing | fixture exercises selector determinism | Task 1 |
| DOD-09 | §23 | missing | fixture exercises selector determinism | Task 1 |
| CRED-01 | §18 | missing | fixture exercises selector determinism | Task 1 |
| GIT-01 | §12, §17 | missing | fixture exercises selector determinism | Task 1 |

## Interaction acceptance inventory

- input boundary: only the committed plan model reaches the selector.
- semantic boundary: no memory or runtime ledger is read or written; controller and pointer interactions assert semantic outcomes.
- production boundary: the selector never writes product code.
- evidence boundary: selection is a deterministic function of plan and bytes.

## Task 1: In-progress task to resume

- Status: in_progress
- Dependencies: None
- Priority: 5
- Scope: the sole in-progress task must be resumed regardless of priority.
- Acceptance criteria: the selector resumes Task 1.
- Verification: the hidden selector fixture suite.
- Documentation impact: none.

## Task 2: Final documentation and specification audit

- Status: pending
- Dependencies: Task 1
- Priority: 2
- Scope: fixture audit task for the acceptance plan; the definition of done
  covers the conformance matrix, the interaction inventory, open findings,
  the final review, and a clean tree.
- Acceptance criteria: the fixture remains parseable and the selector never
  selects the audit while Task 1 is in progress.
- Verification: the fixture suite.
- Documentation impact: none.
