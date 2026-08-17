# Task 3: Add re-enumeration timing test (§2.4)

## Outcome
- Added `test_native_reenumeration_timing` to `tests/test_native_dbus.c`
- Test uses split `nip_start_private_bus` + `nip_fork_server` pattern with real dbus-daemon
- Kills server → verifies degraded → restarts server → measures elapsed via `clock_gettime(CLOCK_MONOTONIC)` from restart to `reenumerate_cb` firing → asserts ≤2000ms
- Production path exercised: NameOwnerChanged → `noc_signal_callback` → `ip_connection_handle_name_changed` → `get_property(Version)` → `ip_version_is_compatible` → `reenumerate_cb`
- Added `#include <time.h>` for `clock_gettime`
- Updated conformance matrix: ARCH-01 partial → verified

## Verification
- `nix-shell --run 'ctest --test-dir build-check -R "connection|native_dbus" --output-on-failure'` → both pass (0.01s + 7.58s)
- `nix-shell --run 'ctest --test-dir build-check --output-on-failure'` → 98/98 pass (2 hardware skips)

## Commit
- `a75f883`: Task 3: Add re-enumeration timing test (§2.4)

## Next Task
- Task 4: Complete missing interaction test paths — M09 (type picker cancel pointer path), M16 (name input cancel pointer path), VC types slots 1–3 cycling tests