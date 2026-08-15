# Task 3 Complete — Overlay Interaction Acceptance with Native DBus

## Outcome
Created `tests/test_overlay_native.c` exercising O01–O13 through `cbx_overlay_service_step` with a real sd-bus backend connected to a private InputPlumber-compatible native-signature DBus server. No `ip_dbus_mock` backend used — only shared DBus constants from `dbus_mock.h`.

## Changes (commit 6c659c4 on develop)
1. **`tests/test_overlay_native.c`**: New 18-test file covering O01–O11, O11b, O11c, O12, O13. Uses `ip_dbus_sd_backend()` + `nip_start_server` for real DBus. InputEvent signals triggered via `EmitInputEvent(ss)` method on native server. InterceptMode verified as `u` type on the wire via set/get round-trip through production sd-bus. Real SDL events via `SDL_PushEvent KEYDOWN`. Real DBus signal dispatch through `ip_input_events_subscribe` + `cbx_overlay_service_step`.
2. **`tests/CMakeLists.txt`**: Added `test_overlay_native` target linking `native_ip_server.c` with `DBUS_SESSION_CONFIG` define.

## Key implementation insights
- PersistentId must use `ORDER:n` format (not arbitrary strings) because `cbx_validate_id` only accepts `BT:`, `USB:`, `ORDER:` prefixed IDs.
- Profile cycling modifies `grid.rows[].profile` in place — tests must copy the string before dispatch to compare before/after.
- `cbx_profile_cycle_apply` verifies LoadProfilePath via GetProfilePath read-back — native server already stores profile path on LoadProfilePath call (Task 2).
- `cbx_overlay_input_build_map` queries `DbusDevices` property (as array) — native server returns composite paths as DbusDevices, InputEvent signal emitted on same path.
- `EmitInputEvent` on non-existent paths correctly fails (sd-bus returns error) — O11c test expects non-zero return.
- Test-local poll wrappers replicate production `on_intercept_activating` (updates lifecycle.composite_path) since production functions are static.

## Verification
- `test_overlay_native`: 18/18 passed
- Full suite: 93/93 passed, 1 skipped (test_backend_smoke, §11.1.6 human-release-gated)
- `grep -c 'ip_dbus_mock' tests/test_overlay_native.c` → 1 (comment only, no mock backend usage)

## Next task
Task 4: Manager Controllers + Settings interaction with native DBus (M04, M21, M23–M26, D06, MG-04, MG-15). Dependencies: Task 1 (complete). Ready to start.