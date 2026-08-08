# Task 7 Complete — Create interaction acceptance inventory

## What was done

### Source changes

1. **`tests/interaction_inventory.h`** — New header defining:
   - `cbx_inv_category` enum (manager tabbar/ctrl/prof/settings/editor, overlay, disabled)
   - `cbx_inv_widget_type` enum (tab, list, button, picker, name_input, confirm_delete, edit_mode, binding, capture, sequential, editor, overlay_action, scenario)
   - `cbx_inv_verify_status` enum (unverified, verified, not_applicable, deferred)
   - `cbx_inv_path_availability` enum (na, available)
   - `cbx_interaction_entry` struct with: id, category, context, widget_type, controller_path, pointer_path_avail, pointer_path, semantic_outcome, dispatch_path, verify_status, evidence_task
   - API: `cbx_interaction_inventory_get()`, `cbx_interaction_inventory_count()`, `cbx_interaction_inventory_find(id)`

2. **`tests/interaction_inventory.c`** — Static array with 58 entries:
   - M01–M03: Tab bar (Controllers/Profiles/Settings tabs)
   - M04–M09: Controllers tab (device list, add, remove, change type, picker confirm/cancel)
   - M10–M20: Profiles tab (list, create, source picker, name input chars/backspace/confirm/cancel, edit, delete confirm/cancel)
   - M21–M27: Settings tab (list, toggle, edit enter/up-down/confirm/cancel, save)
   - M28–M38: Profile editor (binding list, edit, target picker, capture begin/capture, sequential begin/capture/skip/cancel, save+close, cancel editor)
   - O01–O12: Overlay actions (open, move left/right, cycle up/down, host mode enter/navigate/move/exit, close, multi-controller independence, host profile cycle deferred)
   - D01–D08: Disabled/degraded scenarios (InputPlumber unavailable, remove no device, delete no profile, save missing NES, settings cancel, DBus failure, filesystem failure, empty profile)
   - NULL terminator entry
   - Implementation of get/count/find functions

3. **`tests/test_interaction_inventory.c`** — 11 sub-tests:
   - `test_inventory_count`: 58 entries
   - `test_inventory_all_fields_populated`: all required strings non-NULL
   - `test_inventory_has_all_manager_controls`: M01–M38 all found
   - `test_inventory_has_all_overlay_actions`: O01–O12 all found
   - `test_inventory_has_all_disabled_scenarios`: D01–D08 all found
   - `test_inventory_find_returns_null_for_unknown`: M99, X01, "", NULL → NULL
   - `test_inventory_pointer_path_availability`: n/a entries have "n/a" prefix; available entries don't
   - `test_inventory_categories`: M→manager cat, O→overlay cat, D→disabled cat
   - `test_inventory_specific_entries`: spot-checks M05, M16, M37, O12, O01, D01, D08
   - `test_inventory_all_ids_unique`: no duplicate IDs
   - `test_inventory_covers_required_scenarios`: create source picker, name input cancel, capture mode, sequential mode, save+close, cancel editor all present

4. **`tests/CMakeLists.txt`** — Added `tests/interaction_inventory.c` to `cbx_test_support` static library. Added `test_interaction_inventory` executable and ctest registration.

5. **`IMPLEMENTATION_PLAN.md`** — Task 7 status → complete. REQ-023 → partial (inventory exists, interaction tests pending in Tasks 8/9). Verification command updated with full results.

## Test results
75/75 pass (1 skip: backend_smoke). No regressions. 11 new sub-tests in test_interaction_inventory.

## Commit
`c5d5db7` on `develop`

## Next task
Task 8: Manager interaction tests — Controllers and Settings tabs through production dispatch. Dependencies: Task 1, 2, 3, 7 (all complete). Ready to start.