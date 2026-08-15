# Task 6 Complete — Disabled/Degraded Scenarios with Native DBus

## Outcome
Added pointer-path tests for D01, D04, D07, D08 and updated interaction inventory D01-D08 to VERIFIED. All D01-D08 now have both controller and pointer evidence through production dispatch with native DBus.

## Changes (commit d668c02 on develop)
1. **`tests/test_installed_functional.c`**: Added `send_mouse_click` helper and `test_d01_pointer_degraded_click` — kills server, verifies Add button invisible/non-interactive, clicks its area, mode stays LIST. (4 tests total, was 3)
2. **`tests/test_manager_native_prof.c`**: Added `send_key_up`/`send_key_press` helpers and three pointer-path tests:
   - `test_d04_save_missing_nes_pointer`: create empty profile via pointer (click Create → click Empty → type name → Enter), B KEYUP save → NES validation blocks → no file
   - `test_d07_filesystem_failure_pointer`: open editor via pointer, chmod 0555, B KEYUP save → filesystem failure → editor stays open, error shown
   - `test_d08_empty_profile_create_pointer`: create empty profile via pointer, verify 0 bindings, B KEYUP save blocked
3. **`tests/interaction_inventory.c`**: D02-D08 `verify_status` changed from `CBX_VERIFY_UNVERIFIED` to `CBX_VERIFY_VERIFIED`
4. **`tests/test_interaction_inventory.c`**: `test_inventory_specific_verify_statuses` updated — D02-D08 moved from `unverified_ids` to `verified_ids`

## Existing coverage (no changes needed)
- D01 controller: `test_installed_backend_recovery` (native DBus, degraded/recovery lifecycle)
- D02/D03 controller+pointer: `test_manager_native_prof.c` (Task 5)
- D05 controller+pointer: `test_manager_native.c` M26 cancel edit tests (Task 4)
- D06 controller+pointer: `test_manager_native.c` D06 tests with `g_nip_fail_next_create` (Task 4)

## Key implementation insights
- Pointer-path create flow: click Create button (mouse) → click "Empty" in create_picker (mouse) → type name (keyboard, no mouse text input) → Enter confirms name on KEYDOWN (works without controller_event flag)
- Editor save via pointer path: `send_key_press(mgr, SDLK_b)` — B KEYDOWN is a no-op in LIST mode, B KEYUP triggers `cbx_manager_tab_cancel` → `cbx_profiles_tab_cancel` → `cbx_profiles_tab_save_editor`
- D01 pointer: degraded state makes buttons invisible (`cbx_widget_set_visible(false)`), so `cbx_manager_hit_test` won't find them — clicking their area produces no effect
- `send_key_dn(&mgr, SDLK_RETURN)` confirms name input on KEYDOWN regardless of controller_event flag (unlike SDLK_a which requires controller_event)

## Verification
- `test_manager_native_prof`: 31/31 passed (was 29, +3 pointer-path tests)
- `test_installed_functional`: 4/4 passed (was 3, +1 D01 pointer test)
- `test_interaction_inventory`: 14/14 passed (updated verify status expectations)
- Full suite: 95/95 (94 passed + 1 skipped `test_backend_smoke` §11.1.6)
- `grep -c 'ip_dbus_mock' tests/test_manager_native_prof.c` → 1 (comment only)
- `grep -c 'ip_dbus_mock' tests/test_installed_functional.c` → 0

## Next task
Task 7: Installed binary functional acceptance test. Dependencies: Task 2 (complete). Ready to start.