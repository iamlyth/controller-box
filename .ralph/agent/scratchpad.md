# Implementation Loop — Task 1 Complete

## Outcome
- Task 1 (Add SupportedTargetDevices to native test server) is complete.
- Added `SupportedTargetDevices:as` property to `native_ip_server.c` manager vtable with human-readable names matching `SupportedTargetDeviceIds` values ("Xbox 360 Controller", "DualSense", "Generic Gamepad").
- Added native round-trip test assertions in `test_native_dbus.c` (Test 1 via `backend->get_property`, Test 3 via `ip_manager_get_supported_target_devices` wrapper).
- DBUS-07 reclassified from `partial` to `verified` in conformance matrix.

## Verification
- `nix-shell --run "ctest --test-dir build-check -R 'test_native_dbus' --output-on-failure"` — 10/10 tests passed (5.25s)
- `test_manager_calls` (mock) also passes — no regressions

## Commit
- `96bc4f7` on `develop`

## Next Task
- Task 2: Add daemon memory footprint test (no deps, code change). Measure RSS/heap of overlay service after init and 100 idle steps. Assert <50MB. PERF-04 -> verified.
- Tasks 3–8 require declaring runner capabilities (physical-controller, kernel-uinput, gpu-compositor, target-consumer, installed-package) which are NOT available in the current environment. These cannot be completed without hardware/runner access.
- Task 9 (final audit) depends on all other tasks.