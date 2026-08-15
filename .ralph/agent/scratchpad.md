# Task 1 Complete — Interaction Inventory Corrected

## Outcome
- **Part (a) test_editor_seq_capture_dbus_signal**: Already passing (fixed in prior implementation cycle, commit af2de81). No action needed.
- **Part (b) interaction inventory correction**: Complete. All four defects addressed:

## Changes (commit 522e16e on develop)
1. **Stale task refs**: Updated all 58 `evidence_task` fields from "Task 7/8/9/11" to current plan numbering. Verified entries → "—", mock-only → "Task 4/5/3/6" by category.
2. **verify_status downgrades**: 33 mock-only entries downgraded from VERIFIED to UNVERIFIED (M04/M10/M12/M18/M21/M23/M30/M31/M33, O01–O11, D02–D08). 8 dialog entries changed from NOT_APPLICABLE to UNVERIFIED (M09/M15/M16/M19/M20/M24/M25/M26).
3. **pointer_path_avail**: Changed NA → AVAILABLE for 8 dialog entries (M09/M15/M16/M19/M20/M24/M25/M26) and all 8 disabled scenarios (D01–D08) per §5.1/§5.7. Updated pointer_path strings to not start with "n/a".
4. **O13 added**: Player Mode conflict detection entry. Total entries: 59 (was 58).
5. **Test file updated**: Count 59, overlay loop 1..13, replaced 3 "all verified" tests with consistency + specific-status + dialog-availability checks.

## Verification
- `nix-shell -c "ctest --test-dir build-check -R 'test_interaction_inventory|test_manager_interaction_prof' --output-on-failure"` → 2/2 passed
- Full suite: `nix-shell -c "ctest --test-dir build-check -j$(nproc)"` → 91/91 passed, 1 skipped (test_backend_smoke, §11.1.6 human-release-gated)

## Next task
Task 2: Extend native DBus test server for overlay (SetInterceptActivation, writable InterceptMode, InputEvent signal). Dependencies: none. Ready to start.