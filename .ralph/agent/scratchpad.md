# Task 2: InputPlumber Readiness, Degraded UI, and Owner Recovery — COMPLETE

## Outcome
- All 6 gaps fixed and verified. Commit `c2415e4` on `develop`.

## Verification
- `ctest -R 'test_native_dbus|test_connection|test_overlay_service|test_manager_dbus_inject'` — all pass.
- Full CTest: 81/81 pass (1 skip = backend_smoke, needs GPU).

## Changes
1. **Overlay unconditional DBus process** (`overlay_service.c`): `cbx_overlay_service_step` now drains `conn.backend->process(bus)` unconditionally, independent of `input_events_ready`. Degraded-mode NameOwnerChanged recovery now works.
2. **Distinct degraded reasons** (`ip_connection.c/h`): `ip_connection_reason_for_error()` maps each `IP_ERR_*` to a specific actionable string. Threaded through `handle_name_changed`, overlay init, manager init → controllers_tab status label.
3. **Version compatibility check** (`ip_connection.c/h`): `ip_version_is_compatible()` verifies >= 0.78.0 on connect and reacquisition. Incompatible → DEGRADED with specific reason.
4. **Native fixture recovery test** (`test_native_dbus.c`): stops/restarts server child, exercises real sd-bus owner loss/reacquisition.
5. **Manager loop integration test** (`test_manager_dbus_inject.c`): proves loop drains queued NOC via `process()` and fires `cbx_manager_backend_ready`.
6. **Overlay step recovery test** (`test_overlay_service.c`): proves step drains DBus when `input_events_ready=false`.

## Key design decisions
- Mock backend enhanced with `ip_dbus_mock_queue_noc()` so `process()` dispatches queued signals (simulating real sd-bus behavior).
- Manager `cbx_manager_backend_ready`/`_degraded` exposed as non-static for testability.
- `IP_ERR_INCOMPATIBLE` (-ENOSYS) added for version-check failures.

## Next Task
Task 4 (Functional Controllers tab with confirmed backend outcomes) — depends on Tasks 1, 2 (both complete).