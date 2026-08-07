# Scratchpad — BUG-0003 Maintenance Planning

## Iteration 1 — Planning

### Bug summary
BUG-0003: Production manager leaves every tab body uninitialized.
- Status: triaged, severity: critical
- contract_change: false (no spec edit needed)
- Fingerprint: 4725af4ffd58afffcac5784a897f4e9f640e4b7333ec829d27dc364a7e1a23cf

### Root cause
`cbx_manager_init()` in `src/manager/manager.c` creates three empty `cbx_panel`
containers and a tab bar but never calls `cbx_controllers_tab_init()`,
`cbx_profiles_tab_init()`, or `cbx_settings_tab_init()`. Integration tests
manually init the tabs, so they pass while the executable shows blank panels.

### Key findings from code inspection
- `cbx_controllers_tab_init()` handles NULL backend/bus gracefully (creates
  widgets, skips data load). Good for degraded mode.
- `cbx_profiles_tab_init()` does NOT auto-refresh — caller must call
  `cbx_profiles_tab_refresh()` after init.
- `cbx_settings_tab_init()` auto-refreshes (calls refresh at end of init).
- `ip_dbus_sd_backend()` is defined in `src/dbus/dbus_client.c` but declared in
  no header. Need to add declaration to `tests/dbus_mock.h`.
- `test_manager_tabs.c` asserts `fc->count == 1` (empty panels) — will break
  after fix, needs updating.
- `test_manager_integration.c` manually inits all three tabs after
  `cbx_manager_init()` — will double-init after fix, needs migrating to use
  manager-owned tabs via accessors.
- `sd_bus_open_system()` fails fast when no system bus; InputPlumber DBus calls
  return `ServiceUnknown`/`NameHasNoOwner` immediately when IP not running.
  No timeout risk in test environment.

### Plan structure (5 tasks)
1. Extend cbx_manager struct + init/shutdown to own tab lifecycle + DBus
2. Wire tab refresh into tab switching
3. Update existing tests (test_manager_tabs, test_manager_integration)
4. Add production-path regression test (test_manager_production)
5. Maintenance verification and documentation audit

### Front matter values
- bug_id: BUG-0003
- bug_fingerprint: 4725af4ffd58afffcac5784a897f4e9f640e4b7333ec829d27dc364a7e1a23cf
- spec_path: docs/SPEC.md
- spec_commit: 12f82db38f999986de4216dfc50a6e13452db4c9
- spec_blob: 0522f7f1aa79d70b343ed6022956683a7c11695f
- base_commit: f5701e816bb8df97d75df7c8426c6b82043959da
- status: active

### Decision: no spec change required
contract_change is false. The fix is purely implementation — wiring existing
tab init/shutdown/refresh calls into the manager lifecycle. No product
behavior change, no API contract change.

Plan is coherent, fresh, and limited to an ordinary defect. Ready for
MAINTENANCE_PLAN_COMPLETE.