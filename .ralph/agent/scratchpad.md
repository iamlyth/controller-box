# Task 5: Add overlay DBus InputEvent navigation tests (O02–O09)

## Outcome
- Added 8 new DBus InputEvent test functions to `tests/test_overlay_native.c`:
  - `test_o02_move_left_dbus` — emit "Left" via `emit_input_event` → grid col 1→0
  - `test_o03_move_right_dbus` — emit "Right" → grid col 0→1
  - `test_o04_cycle_profile_up_dbus` — emit "Up" → profile changes + LoadProfilePath DBus verified on wire
  - `test_o05_cycle_profile_down_dbus` — emit "Down" → profile changes + LoadProfilePath verified
  - `test_o06_enter_host_mode_dbus` — emit "R3" → host mode active, host_row=0
  - `test_o07_host_navigate_rows_dbus` — emit "Down"/"Up" → selected_row changes
  - `test_o08_host_move_slot_dbus` — emit "Right"/"Left" → slot column changes
  - `test_o09_exit_host_mode_dbus` — emit "R3" → host mode inactive
- Each test uses `emit_input_event` → native DBus `EmitInputEvent` → `InputEvent(sd)` signal → `ip_input_events_process` → `cbx_overlay_input_cb` → production player_mode/host_mode handler
- Existing keyboard tests (push_keydown) remain as supplemental evidence
- Updated conformance matrix: OVL-10 partial→verified, DOD-03 gap reduced (O02–O09 resolved)

## Verification
- `nix-shell --run 'ctest --test-dir build-check -R "overlay_native" --output-on-failure'` → 1/1 pass (2.86s)
- `nix-shell --run 'ctest --test-dir build-check --output-on-failure'` → 98/98 pass (2 hardware skips)

## Commit
- `79e5b99`: Task 5: Add overlay DBus InputEvent navigation tests (O02–O09)

## Next Task
- Task 6: Fix golden comparison silent skip in backend smoke SW (`tests/test_backend_smoke_sw.c` lines ~429, ~646)