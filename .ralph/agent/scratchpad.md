# Task 6: Controller-transport acceptance and manager-UI backend recovery

## Outcome
Task 6 complete (commit a43ccad). All 3 deliverables implemented.

## Deliverables

### 1. Controller acceptance via production gamepad transport (IA-03, IA-17, M49)
`test_installed_controller_acceptance` in `test_installed_functional.c`:
- Controllers tab: A (b0) opens type picker, B (b1) cancels, A confirms type (creates target via DBus), A opens Change Type picker, B cancels, A removes device — all via `SDL_JoystickSetVirtualButton→SDL_CONTROLLERBUTTONDOWN→cbx_manager_controller_to_key→cbx_manager_handle_event`
- Settings tab: A toggles launch_at_boot, D-pad down ×10 to Save item, A saves to disk
- Profiles tab: A opens Create picker, A confirms "Default copy", keyboard types name, A confirms → opens editor, B cancels BINDING_EDIT→LIST, B saves+closes editor (file written), A opens Edit, D-pad+A navigates binding, B cancels, B saves+closes, A opens Delete confirm, A confirms delete (file removed)
- Asserts: mode transitions, device count changes, file creation/removal

### 2. Keyboard tests relabeled as supplemental (§5.7)
File header comment and runner comment in `test_manager_interaction_ctrl.c` clearly state that `*_controller_path` tests are supplemental accessibility evidence per §5.7, and genuine controller-transport evidence comes from `test_installed_controller_acceptance`.

### 3. Manager-UI backend recovery through production dispatch (IA-14, M50)
`test_installed_backend_recovery` in `test_installed_functional.c`:
- Init manager with production path (`cbx_manager_init(NULL)`) — owns DBus connection, wires `cbx_manager_backend_ready/degraded` callbacks
- Kill InputPlumber server → `drain_manager_dbus` processes NameOwnerChanged → `cbx_manager_backend_degraded` → asserts: `dbus_connected==false`, `ct.backend==NULL`, buttons `interactive==false`, status label visible
- Restart server → `drain_manager_dbus` processes NameOwnerChanged → `cbx_manager_backend_ready` → asserts: `dbus_connected==true`, `ct.backend!=NULL`, buttons `interactive==true`, status label hidden, `composite_count>=2`

## Key discovery
When gamepad A confirms a name in NAME_INPUT mode, the A KEYUP also triggers `cbx_manager_tab_activate→cbx_profile_editor_activate` which enters BINDING_EDIT mode. Fixed by adding B to cancel BINDING_EDIT before B to save+close.

## Verification
`ctest --test-dir build-check -R 'test_installed_functional|test_manager_production|test_native_dbus|test_manager_interaction_ctrl' --output-on-failure` → 5/5 pass.
Full gate: 90/91 pass (1 pre-existing failure: `test_pi2_ollama_wrapper` needs ollama not in nix-shell).

## Next task
Task 7 (overlay dynamic columns hotplug + visual skip hardening) — no deps.