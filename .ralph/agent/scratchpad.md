# Task 6: Controller-transport acceptance and manager-UI backend recovery

## Outcome
Tasks 1-5 complete (commits through d405b7d). Task 6 in_progress.

## What Task 6 requires (3 deliverables)

### 1. Controller acceptance via production gamepad transport (IA-03, IA-17, M49)
Extend `test_installed_functional.c` to drive representative controls across all 3 tabs using `SDL_JoystickSetVirtualButton`→`SDL_PollEvent`→`cbx_manager_handle_event`:
- **Controllers tab**: A button (b0) to open Add, confirm type, Remove, Change Type
- **Profiles tab**: A button to Create (Default copy), Edit, Save, Delete
- **Settings tab**: A button to toggle a setting, navigate to Save, save
- **Editor**: D-pad + A to navigate bindings, B to cancel/back
- Assert semantic outcomes (device count changes, DBus calls observed, files written, mode transitions)
- Currently only D-pad (buttons 13/14) is used for tab navigation. Need A (b0) and B (b1).

### 2. Keyboard tests relabeled as supplemental (§5.7)
In `test_manager_interaction_ctrl.c`, add clear comments/test-name annotations that keyboard-dispatched `*_controller_path` tests are "supplemental accessibility evidence" per §5.7, not controller acceptance. The genuine controller-transport evidence comes from `test_installed_functional.c`.

### 3. Manager-UI backend recovery through production dispatch (IA-14, M50)
New test that:
- Initializes manager with real private sd-bus backend (reuse `test_installed_functional.c` infrastructure or `test_native_dbus.c` patterns)
- Simulates IP owner loss (kill server) → NameOwnerChanged → `cbx_manager_backend_degraded` callback
- Verifies controls are disabled: `ct.add_btn.base.interactive == false`, status label visible with reason, `ct.backend == NULL`
- Simulates IP owner reacquisition (restart server) → NameOwnerChanged → `cbx_manager_backend_ready` callback
- Verifies controls re-enable: `ct.add_btn.base.interactive == true`, `ct.backend != NULL`, device model re-enumerated, no restart needed
- **Key**: must use the manager's own callbacks wired in `cbx_manager_init_with_dbus()`, not standalone test callbacks

## Key infrastructure facts
- `test_installed_functional.c` already has: private dbus-daemon fork, InputPlumber server fork, SDL virtual gamepad (6 axes, 15 buttons, mapping a:b0,b:b1,start:b6,dpup:b11..dpright:b14), `ctrl_press()` helper, `pump_manager()` helper
- Manager's `cbx_manager_controller_to_key` maps: A→SDLK_a, B→SDLK_b, START→SDLK_TAB, D-pad→arrows
- Recovery path: `ip_connection_handle_name_changed` → `reenumerate_cb`/`degraded_cb` → `cbx_manager_backend_ready`/`cbx_manager_backend_degraded`
- `cbx_controllers_tab_set_available(false, reason)` sets `add_btn/remove_btn/change_type_btn.base.interactive = false`, hides buttons, shows status_lbl
- `cbx_controllers_tab_set_available(true, NULL)` re-enables buttons, hides status_lbl
- Manager main loop drains DBus via `backend->process(bus)` up to 64x per frame
- `test_native_dbus.c` has `test_native_owner_loss_and_reacquisition` but at ip_connection layer only — no manager UI

## Verification
`nix-shell --run "ctest --test-dir build-check -R 'test_installed_functional|test_manager_production|test_native_dbus|test_manager_interaction_ctrl' --output-on-failure"`; full gate.

## Next task
Task 7 (overlay dynamic columns hotplug + visual skip hardening) — no deps, can start after Task 6.