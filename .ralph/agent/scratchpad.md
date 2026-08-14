# Implementation Handoff

## Outcome
Task 2 (Controllers tab topology reconciliation and auto-Unassign) complete. Tasks 1-2 closed; 8 plan tasks remain.

## What was done
- **CT-02 (auto-Unassign):** `controllers_tab.c:cbx_controllers_tab_remove` now loads `assignments.yaml`, removes the entry whose slot matches the removed device, and shifts higher slots down. Tests: `test_remove_auto_unassign`, `test_remove_shifts_higher_slots`, `test_ctrl_remove_auto_unassign_controller_path` (prod dispatch).
- **CT-03 (orphan columns):** Added `expected_target_count` field + `cbx_controllers_tab_set_expected_count()`. `check_orphan_columns()` shows "Topology incomplete: N of M" error when actual < expected. Manager wires it from `settings.virtual_controllers.count`. Tests: `test_orphan_columns_shows_error`, `test_ctrl_orphan_columns_visible_controller_path`.
- **CT-04 (startup reconciliation):** Added `test_native_startup_reconciliation_prod_path` in `test_native_dbus.c` — uses real sd-bus + `cbx_reconcile_startup_targets` to verify grow (0→3), shrink (3→1), type correction, and routability via `TargetDevices`.
- **CT-05 (add routability):** `cbx_controllers_tab_add` now checks `TargetDevices` on the composite; calls `AttachTargetDevice` if not attached; fails if attach fails. Tests: `test_add_attaches_target_if_not_routable`, `test_add_skips_attach_when_already_routable`, `test_add_fails_when_attach_fails`.
- Updated existing add tests to mock `TargetDevices` + `AttachTargetDevice`.
- Updated `docs/OPERATIONS.md` with topology reconciliation step, auto-Unassign, and orphan-columns docs.
- Updated conformance matrix: CT-02–CT-05 all `verified`.

## Verification
- `ctest -R 'test_controllers_tab|test_native_dbus|test_manager_interaction_ctrl'` → all passed.
- Full suite: 90/90 passed, 1 pre-existing skip (test_backend_smoke).
- No regressions.

## Commit
- dbe8f5c on `develop`.

## Next task
Task 3: Editor unsaved-close prompt, sequential production capture, clone, determinism (pending, no deps).
Tasks 4, 7, 8 also pending with no deps. Task 5 depends on 4. Task 6 depends on 2,3,4,5. Task 9 depends on 7. Task 10 (final audit) depends on all.