# Implementation Scratchpad — Controller-Box v1

## Task 4 complete: Interaction inventory accuracy and missing coverage

- **Commit:** 284f2a8 on develop
- **What:** Fixed M32/M34 dispatch_path descriptions in interaction_inventory.c, added M35/M36 to na_ids test guard, added DBus InputEvent signal path test for capture mode and M38 controller-path discard test in test_manager_native_prof.c.
- **Changes:**
  - `interaction_inventory.c`: M32 dispatch_path → "direct callback: cbx_profile_editor_on_input_event (supplemental; DBus signal path tested in test_manager_native_prof)"; M34 → "direct callback: cbx_profile_editor_on_input_event → cbx_profile_editor_seq_on_input (supplemental; DBus signal path tested in test_manager_native_prof)".
  - `test_interaction_inventory.c`: na_ids array now includes M35 and M36 alongside M13, M14, M32, M34, M37, M38.
  - `test_manager_native_prof.c`: Added `drain_bus` and `emit_input_event` helpers. Added `test_m32_capture_dbus_signal` — emits InputEvent via native server's EmitInputEvent method at CompositeDevice0 path, drains manager bus via sd_bus_process, verifies full signal dispatch chain (sd_input_event_callback → input_event_signal_cb → ip_input_events_handle with sender verification → cbx_profile_editor_on_input_event → capture ends). Added `test_m38_discard_ctrl` — Start button (virtual gamepad button 6) from editor LIST mode → controller event dispatch → cbx_profiles_tab_close_editor, verifies mode returns to LIST and profile file mtime unchanged. Both registered in test runner. File header updated to M30–M38.
  - Conformance matrix MGR-07: partial → verified.
- **Key finding:** The native server's `EmitInputEvent(ss)` method emits a real `InputEvent(sd)` signal via `sd_bus_emit_signal`. The signal is received on the manager's bus (separate connection from the fixture's bus). `drain_bus` on `mgr.dbus_backend/mgr.dbus_bus` processes it through the full chain including sender verification (expected_sender resolved via GetNameOwner during editor open).
- **Verification:** `ctest --test-dir build-check` → 96/96 pass (1 pre-existing skip). `verify-project.sh` → pass.

## Next task

Task 5: Fix host mode visual rendering in overlay grid — `src/overlay/grid_render.c` consume `cbx_host_mode_row_state()`, render distinct visuals for SELECTED/HOST/FROZEN rows, update visual and golden tests.