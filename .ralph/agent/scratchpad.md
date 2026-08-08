# Task 6 Complete — Overlay DBus InputEvent signal handling for multi-controller input

## What was done

### Source changes

1. **`tests/dbus_mock.h`** — Added `int (*process)(ip_bus_handle bus)` to `ip_dbus_backend` vtable. Production calls `sd_bus_process()` to dispatch pending signals; mock returns 0 (no-op since signals are injected via `inject_signal`).

2. **`tests/dbus_mock.c`** — Added `mock_process()` (no-op returning 0) and wired it into the mock backend vtable.

3. **`src/dbus/dbus_client.c`** — Added `sd_process()` that calls `sd_bus_process(w->bus, NULL)` (process one pending message, non-blocking). Returns >0 if message processed, 0 if none pending. Wired into production vtable.

4. **`src/dbus/ip_input_signal.h`** — Added `int ip_input_events_process(ip_input_events *ie)` declaration. Drains pending DBus messages via backend's `process` function, which triggers `ip_input_events_handle` for each InputEvent signal.

5. **`src/dbus/ip_input_signal.c`** — Implemented `ip_input_events_process()`: loops calling `ie->backend->process(ie->bus)` until no more messages.

6. **`src/app/overlay_service.h`** — Added:
   - `CBX_MAX_DBUS_DEVICES` constant (64)
   - `cbx_overlay_input_ctx` struct (pm, hm, grid, lifecycle, device_paths array, row_indices, path_count)
   - `cbx_overlay_input_add_mapping()` — adds a single device_path→row entry
   - `cbx_overlay_input_build_map()` — queries each composite's DbusDevices property, parses CSV, adds mappings
   - `cbx_overlay_input_find_row()` — linear scan for device path
   - `cbx_ip_input_to_pm()` / `cbx_ip_input_to_hm()` — maps ip_input_id to cbx_pm_input/cbx_hm_input
   - `cbx_overlay_input_cb()` — the ip_input_event_cb callback

7. **`src/app/overlay_service.c`** — Added to `overlay_ctx`: `input_ctx`, `input_events`, `expected_sender[128]`, `input_events_ready`. Implemented all overlay input functions. In `run_overlay_service()`:
   - Step 10b: Build device_path→row mapping via `cbx_overlay_input_build_map()`, initialize `ip_input_events` with `ip_connection_get_unique_name()` as expected_sender (fail-closed if unavailable), subscribe to InputEvent signals
   - Poll loop: call `ip_input_events_process()` after SDL event processing to drain pending DBus signals
   - `cbx_overlay_input_cb` callback: maps device_path→row (unknown paths dropped), only processes button press events (value==1.0), dispatches to `cbx_player_mode_handle` or `cbx_host_mode_handle` with correct row index (not hardcoded row 0). Host mode uses `host_row` (the host's row), player mode uses the mapped row.

### Test changes

- **`tests/test_overlay_service.c`** — Added `overlay_input_fixture` with mock DBus, 2-composite grid, player/host mode, lifecycle, and overlay_input_ctx. Added 4 new sub-tests:
  - `test_multi_controller_independent_rows`: Injects "Right" from DEV_PATH_0 → row 0 moves, row 1 unchanged. Injects "Right" from DEV_PATH_1 → row 1 moves, row 0 unchanged. Injects "Left" from DEV_PATH_0 → row 0 moves back. Verifies slot_change callback fires for correct row.
  - `test_unknown_device_path_dropped`: Signal from unknown device path → no row movement.
  - `test_wrong_sender_dropped`: Signal from spoofed sender ":1.99" → rejected (fail-closed security).
  - `test_ip_input_events_process_mock_noop`: `ip_input_events_process()` returns 0 on mock backend.

### Plan update
- `IMPLEMENTATION_PLAN.md`: Task 6 status → complete. Conformance matrix: REQ-009 → verified, REQ-010 updated (DBus input now dispatched to host mode with host_row, not row 0).

## Test results
74/74 pass (1 skip: backend_smoke). No regressions. 8 sub-tests in test_overlay_service (4 original + 4 new).

## Commit
`0f8bf0d` on `develop`

## Next task
Task 7: Create interaction acceptance inventory. Dependencies: Task 1, 2, 3 (all complete). Ready to start.