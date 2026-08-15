# Task 2 Complete — Native IP Server Extended for Overlay

## Outcome
Extracted the private InputPlumber-compatible DBus server from `test_native_dbus.c` and `test_installed_functional.c` into a shared `tests/native_ip_server.h`/`.c`. Added overlay interaction capabilities: SetInterceptActivation, writable InterceptMode, InputEvent signal emission.

## Changes (commit 3c7155a on develop)
1. **`tests/native_ip_server.h`**: Reusable header declaring server config, handle, global state (`g_nip_*`), and API: `nip_start_server`, `nip_stop_server`, `nip_fork_server`, `nip_start_private_bus`, `nip_reset_server_state`.
2. **`tests/native_ip_server.c`**: Full server implementation with Manager + CompositeDevice + Target + ObjectManager + DBusDevice vtables. Configurable composite count and version string. InputEvent signal emitted via `sd_bus_emit_signal` with native `sd` signature. Test-only `EmitInputEvent(ss)` method triggers real signal emission (uses `ss` because production `call_method` vtable supports `s`/`as` only; server converts to `double` and emits `sd`).
3. **`tests/test_native_dbus.c`**: Refactored to use shared server. 9 tests (6 existing + 3 new): `test_native_set_intercept_activation`, `test_native_intercept_mode_writable`, `test_native_input_event_signal`. Signal test verifies `ip_input_events_init`/`subscribe`/callback receives correct `ip_input_id`, category, value, raw event, device path.
4. **`tests/test_installed_functional.c`**: Refactored to use shared server. Removed ~540 lines of duplicated server code. All 3 tests pass (functional, controller acceptance, backend recovery).
5. **`tests/CMakeLists.txt`**: Both targets compile `native_ip_server.c`.

## Verification
- `test_native_dbus`: 9/9 passed
- `test_installed_functional`: 3/3 passed
- Full suite: 92/92 passed, 1 skipped (test_backend_smoke, §11.1.6 human-release-gated)

## Next task
Task 3: Overlay interaction acceptance with native DBus backend (O01–O13 through `cbx_overlay_service_step` with extended native server). Dependencies: Task 2 (complete). Ready to start.