# Task 2: InputPlumber Readiness, Degraded UI, and Owner Recovery

## Outcome
- Delegated Task 2 implementation to Factory Worker via `factory.implement`.
- Runtime task `task-1786210489-7df7` (key `spec:task-2`) started.

## Key Gaps Identified (from codebase exploration)
1. **Overlay DBus processing not unconditional**: `cbx_overlay_service_step` only drains DBus via `ip_input_events_process`, gated on `input_events_ready`. If `ip_input_events_subscribe` failed during init, no DBus is processed in degraded mode → NameOwnerChanged never fires → no recovery. Fix: add unconditional `svc->conn.backend->process(svc->conn.bus)` in step.
2. **Error messages not distinct in UI**: All failures collapse to "InputPlumber unavailable — waiting for recovery". `IP_ERR_ACCESS_DENIED`/`IP_ERR_NO_REPLY`/`IP_ERR_INVALID_ARGS` codes exist at transport but aren't threaded to `cbx_controllers_tab_set_available` reason strings. Need distinct actionable messages for unavailable/denied/incompatible/enumeration.
3. **No version compatibility check**: `ip_connection_handle_name_changed` reacquisition path conflates "incompatible" with "unavailable" in one degraded reason string. Need to distinguish.
4. **No native-fixture recovery tests**: `test_native_dbus.c` only tests property round-trip. Need tests that stop/restart the server child to exercise owner loss/reacquisition through real sd-bus.
5. **No Manager loop integration test**: Current tests inject signals directly, bypassing the run loop. Need test proving the loop drains NameOwnerChanged and fires `cbx_manager_backend_ready`/`_degraded`.
6. **No framebuffer degraded-to-ready recovery test**: Need visual evidence of state transition.

## Verification Target
Native fixture scenarios + connection tests + Manager production + overlay service + framebuffer degraded/recovery tests. Clean build + full CTest must pass.

## Next Task
Task 4 (Functional Controllers tab with confirmed backend outcomes) — depends on Tasks 1, 2.