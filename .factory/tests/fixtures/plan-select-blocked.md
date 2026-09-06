---
spec_path: docs/FACTORY-LOOP-SPEC.md
spec_commit: 1111111111111111111111111111111111111111
spec_blob: 2222222222222222222222222222222222222222
base_commit: 3333333333333333333333333333333333333333
status: active
---

# Implementation Plan

## Goal and non-goals

Goal: exercise the deterministic §8 task selector deterministically.

## Architecture and constraints

Plan: Python 3.11 standard library only; selection is a pure function of the
parsed plan and never consults a runtime task ledger.

## Specification conformance matrix

| ID | Spec § | Classification | Evidence | Task |
|----|--------|--------------|----------|------|
| ARCH-01 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| ARCH-02 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| ARCH-03 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| ARCH-04 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| ARCH-05 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| SYS-01 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| SYS-02 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| SYS-03 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| SYS-04 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| SYS-05 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| SYS-06 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| OVL-01 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| OVL-02 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| OVL-03 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| OVL-04 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| OVL-05 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| OVL-06 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| OVL-07 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| OVL-08 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| OVL-09 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| OVL-10 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| MGR-01 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| MGR-02 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| MGR-03 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| MGR-04 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| MGR-05 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| MGR-06 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| MGR-07 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| MGR-08 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| ID-01 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| ID-02 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| ID-03 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| CFG-01 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| CFG-02 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| CFG-03 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| CFG-04 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| CFG-05 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| CFG-06 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| ICN-01 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| ICN-02 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| ICN-03 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| ICN-04 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| ICN-05 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| PKG-01 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| PKG-02 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| PKG-03 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| PKG-04 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| PKG-05 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| DBUS-01 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| DBUS-02 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| DBUS-03 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| DBUS-04 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| DBUS-05 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| DBUS-06 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| DBUS-07 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| PERF-01 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| PERF-02 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| PERF-03 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| PERF-04 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| PERF-05 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| VRF-01 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| VRF-02 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| VRF-03 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| VRF-04 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| VRF-05 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| VRF-06 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| VRF-07 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| DOD-01 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| DOD-02 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| DOD-03 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| DOD-04 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| DOD-05 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| DOD-06 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| DOD-07 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| DOD-08 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| DOD-09 | §5, §7 | missing | fixture exercises selector determinism | Task 1 |
| CRED-01 | §18 | missing | fixture exercises selector determinism | Task 1 |
| GIT-01 | §12, §17 | missing | fixture exercises selector determinism | Task 1 |

## Interaction acceptance inventory

- input boundary: only the committed plan model reaches the selector.
- semantic boundary: no memory or runtime ledger is read or written; controller and pointer interactions assert semantic outcomes.
- production boundary: the selector never writes product code.
- evidence boundary: selection is a deterministic function of plan and bytes.
## Task 1: Blocked fixture

- Status: blocked
- Dependencies: None
- Blocked on: an exact unavailable external fixture fact (selector test)
- Scope: fixture task.
- Acceptance criteria: the fixture proves deterministic selection.
- Verification: the hidden selector fixture suite.
- Documentation impact: none.

## Task 2: Final documentation and specification audit

- Status: pending
- Dependencies: Task 1
- Scope: fixture audit task for the acceptance plan; the definition of done
  covers the conformance matrix, the interaction inventory, open findings,
  the final review, and a clean tree.
- Acceptance criteria: the fixture remains parseable.
- Verification: the fixture suite.
- Documentation impact: none.
