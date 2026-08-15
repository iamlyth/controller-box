# Task 4 Complete — Manager Controllers + Settings Interaction with Native DBus

## Outcome
Created `tests/test_manager_native.c` with 17 tests covering M04, M09, M21, M23–M26, MG-04, MG-15, D06 through `cbx_manager_handle_event` with real sd-bus backend connected to a private native-signature InputPlumber-compatible DBus server. Controller path via `SDL_JoystickSetVirtualButton`; pointer path via `SDL_MOUSEBUTTONDOWN/UP`. No `ip_dbus_mock` backend used.

## Changes (commit 6578044 on develop)
1. **`tests/test_manager_native.c`**: New 17-test file. Controller path uses virtual gamepad (`SDL_JoystickSetVirtualButton` → `SDL_CONTROLLERBUTTONDOWN` → `cbx_manager_controller_to_key`). Pointer path uses mouse events (`SDL_MOUSEBUTTONDOWN/UP` → `cbx_manager_handle_mouse_event`). Tests: M04 (list select controller+pointer), M09 (type picker cancel), M21 (settings list select controller+pointer), M23+M24+M25 (opacity, VC count, VC type, trigger combo edit flow — controller+pointer), M26 (cancel edit with revert — controller+pointer), MG-04 (topology failure: expected 4, actual 0, error visible), MG-15 (post-resize hit testing: resize 800×600, click at new widget center), D06 (CreateTargetDevice failure — controller+pointer).
2. **`tests/native_ip_server.h/.c`**: Added `g_nip_fail_next_create` flag. When set, server returns DBus error on next `CreateTargetDevice` and auto-resets. Reset in `nip_reset_server_state`.
3. **`tests/CMakeLists.txt`**: Added `test_manager_native` target linking `native_ip_server.c` with `DBUS_SESSION_CONFIG` define.

## Key implementation insights
- List widget KEYDOWN DOWN changes `lst->selected` but does NOT call `on_select` or sync `tab->selected`. Sync happens on KEYUP A via `on_setting_selected` callback. Controller-path tests must check `cbx_list_get_selected` after navigation, not `cbx_settings_tab_selected`.
- `pump_manager` after `cbx_manager_init` processes SDL_WINDOWEVENT (RESIZED/SHOWN) which may change focus from tabbar to panel child. Controller-path navigation must use `ctrl_press` (virtual gamepad through SDL event queue) not `send_key_dn` (synthetic keyboard event directly to `cbx_manager_handle_event`), because focus state after init's window events differs between mock and native DBus setups.
- D06 testing: `g_nip_fail_next_create` set before `nip_fork_server` causes first `CreateTargetDevice` to fail. Server auto-resets flag after one failure. D06 tests use `mn_setup_fail` setup function.
- MG-04 topology failure: default settings expect 4 VCs, server starts with 0 targets → `check_orphan_columns` shows "Topology incomplete: 0 of 4".
- MG-15 resize hit testing: `send_window_resize` dispatches SDL_WINDOWEVENT_RESIZED through `cbx_manager_handle_event` → `cbx_manager_layout` + `cbx_manager_rebuild_focus`. Clicking at new widget center activates correct widget.

## Verification
- `test_manager_native`: 17/17 passed
- Full suite: 94/94 (93 passed + 1 skipped `test_backend_smoke` §11.1.6 human-release-gated)
- `grep -c 'ip_dbus_mock' tests/test_manager_native.c` → 1 (comment only, no mock backend usage)

## Next task
Task 5: Manager Profiles + Editor interaction with native DBus (M10, M12–M14, M16, M18, M20, M30–M36, D02–D05, D07–D08). Dependencies: Task 1 (complete). Ready to start.