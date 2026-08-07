# Maintenance BUG-0003 — Scratchpad

## Bug: Production manager leaves every tab body uninitialized

**Root cause:** `cbx_manager_init()` creates three empty panels but never calls
`cbx_controllers_tab_init()`, `cbx_profiles_tab_init()`, or `cbx_settings_tab_init()`.
Integration tests manually call these, masking the bug.

## Progress

### Task 1: Extend cbx_manager to own all three tab module lifecycles — COMPLETE
- Added `ct`, `pt`, `st` tab state fields + DBus fields to `cbx_manager` struct
- Added includes for `controllers_tab.h`, `profiles_tab.h`, `settings_tab.h`, `dbus_mock.h`
- Added accessor declarations and implementations: `cbx_manager_controllers_tab()`,
  `cbx_manager_profiles_tab()`, `cbx_manager_settings_tab()`
- `cbx_manager_init()`: best-effort DBus connect, then init all three tabs + profiles refresh
- `cbx_manager_shutdown()`: tab shutdown before panel destruction, then DBus disconnect
- Declared `ip_dbus_sd_backend()` in `tests/dbus_mock.h`
- Commit: 5977459

### Task 2: Wire tab refresh into tab switching — COMPLETE
- Added refresh calls in `cbx_manager_on_tab_change()` for the newly active tab
- Controllers refresh guarded by backend NULL check (degraded mode)
- Commit: a62ca3e

### Task 3: Update existing skeleton and integration tests — COMPLETE
- Updated test_manager_tabs.c: nonempty panel assertions, focus chain > 1, DOWN navigation
- Updated test_manager_integration.c: removed manual tab init/shutdown, use accessors
- Also fixed test_controllers_tab.c, test_profiles_tab.c, test_settings_tab.c: shut down
  manager-owned tab in setup before test-specific re-init
- All 65 tests pass
- Commit: c0bad65 + fix commit

### Task 4: Add production-path regression test — COMPLETE
- New test_manager_production.c: 6 tests exercising production init/shutdown path only
- Verifies nonempty panels, visible rendered content, focus chain, tab switching, clean shutdown
- Registered in ctest with SDL_VIDEODRIVER=dummy
- Commit: dcc8cbc

### Task 5: Maintenance verification and documentation audit — COMPLETE
- Full ctest suite: 65/65 pass (excluding test_packaging)
- BUG-0003 closed with resolution and verification
- SPEC.md unchanged (git diff --exit-code)
- Stale comments cleaned up in manager.h and manager.c
- Bug ledger validates: 1 open, 2 closed
- MAINTENANCE_PLAN.md status set to complete, all tasks complete

## Build notes
- `build-manual` was stale (wrong path cache); reconfigured fresh
- `nix-shell --run 'cmake -B build-manual ...'` + `cmake --build build-manual`