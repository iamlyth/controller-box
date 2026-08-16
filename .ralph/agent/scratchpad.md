# Implementation Loop — Current Handoff

## Outcome
Task 2 (Move DBus interface definitions to production header) is complete. This was the last software-fixable task.

## What changed
- Created `src/dbus/dbus_interface.h` — production header with all DBus constants, `ip_bus_handle`, `ip_signal_cb`, signal payload structs, `ip_prop_type` enum, `ip_dbus_backend` vtable, and `ip_dbus_sd_backend()` declaration.
- Updated all 24 `src/` files: `#include "dbus_mock.h"` → `#include "dbus_interface.h"` (or `"dbus/dbus_interface.h"` for non-dbus dirs).
- Updated `tests/dbus_mock.h`: includes `dbus/dbus_interface.h`, retains only mock-specific code (IP_MOCK_*, ip_mock_*, ip_dbus_mock, mock lifecycle functions).
- Updated 6 test files that only use constants (`native_ip_server.c`, `test_native_dbus.c`, `test_manager_native.c`, `test_manager_native_prof.c`, `test_overlay_native.c`, `test_installed_functional.c`) to include `dbus_interface.h` directly.
- Updated `src/dbus/dbus_client.h`: includes `dbus_interface.h`, removed redundant `ip_dbus_sd_backend()` declaration.
- `CMakeLists.txt`: removed `tests/` from `controllerbox` PUBLIC include path.
- `tests/CMakeLists.txt`: added `src/` to `cbx_test_support` PUBLIC and `test_ip_server` PRIVATE include dirs.
- `.factory/artifacts/implementation-plan.md`: Task 2 marked complete; DBUS-04 and DOD-02 conformance rows upgraded to verified.

## Verification
- `grep -rn '#include.*"dbus_mock.h"' src/` — no matches (acceptance criterion met)
- `nix-shell --run 'cmake --build build-check --parallel 2 && ctest --test-dir build-check --output-on-failure'` — 98/98 pass, 2 skipped (hardware-blocked).
- Sanitizer build compiles clean. 0 warnings.

## Commit
889d9b1 — refactor: move DBus interface definitions to production header (Task 2, DBUS-04)

## Next task
Tasks 3-6 are hardware-blocked (need /dev/uinput, GPU compositor, Pi 4 target hardware). Task 7 depends on 3-6. Task 8 (final audit) depends on all. No more software-fixable tasks remain in the plan. The next iteration should verify the hardware-blocked task status and determine if any remediation or audit prep work can proceed, or if the cycle is blocked pending hardware access.