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
- Build succeeds; `test_manager_up_down_focus_navigation` fails as expected (panels now populated)
- Commit: 5977459

### Task 2: Wire tab refresh into tab switching — PENDING
- Add refresh calls in `cbx_manager_on_tab_change()` for the newly active tab

### Task 3: Update existing skeleton and integration tests — PENDING
- Update `test_manager_tabs.c` assertions for populated panels
- Update `test_manager_integration.c` to use accessors instead of manual tab init

### Task 4: Add production-path regression test — PENDING
- New `test_manager_production.c` exercising production init/shutdown path only

### Task 5: Maintenance verification and documentation audit — PENDING
- Full ctest, close BUG-0003, verify SPEC.md unchanged

## Build notes
- `build-manual` was stale (wrong path cache); reconfigured fresh
- `nix-shell --run 'cmake -B build-manual ...'` + `cmake --build build-manual`