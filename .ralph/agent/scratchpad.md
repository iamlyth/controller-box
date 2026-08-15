# Task 5 Complete — Manager Profiles + Editor Interaction with Native DBus

## Outcome
Created `tests/test_manager_native_prof.c` with 29 tests covering M10, M12–M14, M16, M18, M20, M30–M36, D02–D04, D07–D08 through `cbx_manager_handle_event` with real sd-bus backend connected to a private native-signature InputPlumber-compatible DBus server. Controller path via `SDL_JoystickSetVirtualButton`; pointer path via `SDL_MOUSEBUTTONDOWN/UP`. M32/M34 InputEvent capture via `cbx_profile_editor_on_input_event` (production callback). No `ip_dbus_mock` backend used.

## Changes (commit 714cef2 on develop)
1. **`tests/test_manager_native_prof.c`**: New 29-test file. Controller path uses virtual gamepad (`SDL_JoystickSetVirtualButton` → `SDL_CONTROLLERBUTTONDOWN` → `cbx_manager_controller_to_key`). Pointer path uses mouse events (`SDL_MOUSEBUTTONDOWN/UP`). Name input letters use keyboard events (`send_key_dn` with SDL_Keycode). Tests cover M10 (list select), M12 (create source picker), M13 (name input chars), M14 (backspace), M16 (cancel), M18 (delete open), M20 (delete cancel), M30 (target pick confirm), M31 (capture begin), M32 (capture event), M33 (sequential begin), M34 (sequential capture), M35 (sequential skip), M36 (sequential cancel), D02 (no device), D03 (no profile), D04 (NES validation), D07 (filesystem failure), D08 (empty profile creation).
2. **`tests/CMakeLists.txt`**: Added `test_manager_native_prof` target linking `native_ip_server.c` with `DBUS_SESSION_CONFIG` define.

## Key implementation insights
- Used `cbx_profiles_tab_set_test_dirs` after `cbx_manager_init` to override profile directories for test isolation (same pattern as mock tests, works with native backend).
- Controller path navigation uses `ctrl_press` (virtual gamepad through SDL event queue), not `send_key_dn` (synthetic keyboard), because focus state after init window events differs between mock and native DBus setups.
- Name input letters use keyboard `send_key_dn` (no `CBX_CONTROLLER_EVENT_WINDOW_ID`) — same pattern as `test_installed_functional.c` Phase 7. Letters 'a' and 'b' trigger confirm/cancel, so use c-z letters for name input tests.
- M32/M34 (InputEvent capture) use `cbx_profile_editor_on_input_event` directly — this is the production callback wired to DBus InputEvent signal handler, not a mock.
- D02 (no device): server starts with 0 targets, Remove button activation has no effect.
- D03 (no profile): uses `mnp_setup_empty` (no profile files), Delete activation has no effect.
- D07 (filesystem failure): `chmod(user_dir, 0555)` makes save fail; restore in test before teardown.
- D04/D08: empty profile has 0 bindings, NES validation blocks save, editor stays open, no file written.

## Verification
- `test_manager_native_prof`: 29/29 passed
- Full suite: 95/95 (94 passed + 1 skipped `test_backend_smoke` §11.1.6 human-release-gated)
- `grep -c 'ip_dbus_mock' tests/test_manager_native_prof.c` → 1 (comment only, no mock backend usage)

## Next task
Task 6: Disabled/degraded scenarios with native DBus (D01–D08, both controller+pointer paths). Dependencies: Task 1 (complete). Some overlap with Task 5 (D02–D04, D07–D08 already covered in test_manager_native_prof.c). Ready to start.