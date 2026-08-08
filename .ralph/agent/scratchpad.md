# Task 4: Functional Controllers tab with confirmed backend outcomes — COMPLETE

## Outcome
- All acceptance criteria met. Commits `0df65d3` (impl) and `601830d` (plan) on `develop`.

## Verification
- `ctest` — 81/81 pass (1 skip = backend_smoke, needs GPU).
- `test_native_target_operations` — real sd-bus: CreateTargetDevice, StopTargetDevice, GetManagedObjects, DeviceType through private dbus-daemon + forked server.
- `test_controllers_tab` — 42 tests pass including 4 new tests for type verification + error display.

## Changes
1. **Type verification in `cbx_controllers_tab_add`** (`controllers_tab.c`): After CreateTargetDevice + ObjectManager refresh, verifies the new target's DeviceType matches the selected type. Returns `-EIO` if type mismatch.
2. **Error display** (`controllers_tab.c`): `show_action_error`/`clear_action_error` helpers; `confirm_type_pick` and `on_remove_pressed` now show operation errors in the status label. `begin_type_pick` clears previous errors.
3. **Native fixture test** (`test_native_dbus.c`): Extended server with `CreateTargetDevice`, `StopTargetDevice`, `GetManagedObjects` (via `sd_bus_add_object`), and target `DeviceType` (via `sd_bus_add_fallback_vtable` with find callback). New `test_native_target_operations` test.
4. **Mock tests** (`test_controllers_tab.c`): `test_add_rejects_type_mismatch`, `test_error_display_on_failed_add`, `test_error_display_on_unconfirmed_add`, `test_error_clear_on_new_operation`.
5. **Integration test fix** (`test_manager_integration.c`): `test_add_controller` now includes DeviceType expectation.

## Key design decisions
- ObjectManager vtable with complex `a{oa{sa{sv}}}` signature fails `sd_bus_add_object_vtable` with EINVAL; used `sd_bus_add_object` with a root handler filtering for GetManagedObjects instead.
- `sd_bus_add_fallback_vtable` requires non-NULL find callback and at least one property entry.
- `ip_connection_reason_for_error` reused for DBus error messages in `show_action_error`.

## Next Task
Task 5 (Authoritative startup target topology and routability) — depends on Task 4 (now complete).