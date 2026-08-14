# Planning Scratchpad

## State
- Fresh planning cycle (third). Two prior implementation cycles completed.
- 87 tests pass, verify-project.sh passes clean in nix-shell.
- 30K LoC source, 52K LoC tests across 81 test files. Mature codebase.

## Plan written: 13 tasks, all pending
1. Flatpak docs fix (§9.1 — no Flathub advertising, mark experimental)
2. Icon override settings UI (§5.5)
3. Unsaved-changes prompt on window close (§5.3)
4. Overlay performance tests (§4.9, §11 — ≤75ms p99, <10ms p99)
5. Interaction inventory gaps — save_btn/discard_btn C-path, cancel handlers, D04/D07/D08 P-path (§5.7)
6. Production-dispatch visual test fixes — validation error + sequential through dispatch (§5.6)
7. Mouse motion + visible focus/hover + resize hit-test (§5.7)
8. Installed functional test — UI-based flows + controller transport (§11.1.5)
9. Degraded overlay error rendering + service hardening (§2.4)
10. Runner evidence + BUG-0004 closure (§11.2.6)
12. Wire launch_at_boot to service install + first-run prompt (§9.1)
13. Overlay DBus InputEvent interaction tests O02–O10 (§5.7)
11. Final audit — depends on all (§11.2)

## Review fixes applied
- Added §3 System Requirements rows (SYS-001–004)
- Reclassified MGR-018, MGR-023, DBU-032, VER-007, VER-008, DOD-005 → verified
- Added SVC-004a (first-run service install) and MGR-041a (launch_at_boot wiring) as missing → Task 12
- Reclassified OVL-001–022 and INT-010 → partial (keyboard supplemental, not DBus InputEvent) → Task 13
- Updated overlay inventory O02–O10 with ✓* (supplemental) and Task 13 gap
- Updated DOD-003 dependencies to include Task 13
- Task 11 depends on all: 1-10, 12, 13
- 0 non-verified rows without task assignment