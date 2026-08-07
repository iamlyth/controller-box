---
bug_id: BUG-0003
bug_fingerprint: 4725af4ffd58afffcac5784a897f4e9f640e4b7333ec829d27dc364a7e1a23cf
spec_path: docs/SPEC.md
spec_commit: 12f82db38f999986de4216dfc50a6e13452db4c9
spec_blob: 0522f7f1aa79d70b343ed6022956683a7c11695f
base_commit: f5701e816bb8df97d75df7c8426c6b82043959da
status: active
---

# Maintenance Plan: BUG-0003 — Production manager leaves every tab body uninitialized

## Goal

Make `cbx_manager_init()` own the complete lifecycle of all three tab modules
(Controllers, Profiles, Settings) so that launching `controller-box --manager`
produces a fully populated UI — not three empty panels with only a tab bar.

## Non-goals

- Do not change `docs/SPEC.md` or any product-facing contract.
- Do not implement new tab features, new DBus calls, or new UI widgets.
- Do not fix BUG-0002 (separate maintenance cycle).
- Do not restructure the tab module APIs (`controllers_tab_init`,
  `profiles_tab_init`, `settings_tab_init` signatures stay the same).
- Do not add a Hotkeys tab or change the tab count.

## Defect analysis

### Root cause

`src/app/main.c::run_manager()` calls `cbx_manager_init()` then
`cbx_manager_run()`. `cbx_manager_init()` (in `src/manager/manager.c`) creates
three `cbx_panel` containers and a tab bar, but **never calls**
`cbx_controllers_tab_init()`, `cbx_profiles_tab_init()`, or
`cbx_settings_tab_init()`. Each panel has `child_count == 0`. The tab bar
works (labels render, Left/Right switches), but every tab body is blank.

### Why tests pass

`tests/test_manager_integration.c` manually calls all three `*_tab_init()`
functions after `cbx_manager_init()`, injecting a mock DBus backend. This
exercises the tab modules but **not the production composition path**.
`tests/test_manager_tabs.c` tests the skeleton and explicitly asserts empty
panels (`fc->count == 1`, "tabbar only"). Both test suites pass while the
executable is broken.

### What the fix must do

1. **`cbx_manager` struct** must hold the three tab state structs and a DBus
   backend/bus handle so their lifetime is tied to the manager.
2. **`cbx_manager_init()`** must, after creating panels, connect to the system
   DBus (via `ip_dbus_sd_backend()` → `backend->connect(&bus)`), then call all
   three tab init functions. If DBus connect fails (InputPlumber unavailable,
   no system bus), pass `NULL`/`NULL` to `cbx_controllers_tab_init()` — it
   already handles this gracefully (creates widgets, skips data load).
   `cbx_profiles_tab_init()` does not auto-refresh, so `cbx_manager_init()` must
   call `cbx_profiles_tab_refresh()` afterward.
3. **`cbx_manager_shutdown()`** must call all three `*_tab_shutdown()` functions
   (which remove their widgets from the panel and destroy them) **before**
   destroying the panels, then disconnect DBus.
4. **`cbx_manager_on_tab_change()`** should refresh the newly active tab so
   stale data is replaced on switch.
5. **Accessor functions** are needed so tests can reach the manager-owned tab
   instances without manual init.
6. **`ip_dbus_sd_backend()`** is defined in `src/dbus/dbus_client.c` but
   declared in no header. A declaration must be added to `tests/dbus_mock.h`
   (the shared vtable header already included by production code).

### DBus degraded mode

When InputPlumber is not running, `sd_bus_open_system()` may succeed (system
bus exists) but InputPlumber DBus calls return `ServiceUnknown` /
`NameHasNoOwner` immediately (no timeout). The controllers tab will show an
empty device list with functional Add/Remove/Change Type buttons — this is the
degraded state. Profiles and Settings tabs do not use DBus and work
independently.

## Task 1: Extend cbx_manager to own all three tab module lifecycles

- Status: pending
- Dependencies: none
- Scope: bounded files and behavior
  - `src/manager/manager.h`: Add `cbx_controllers_tab ct`, `cbx_profiles_tab
    pt`, `cbx_settings_tab st` fields to `cbx_manager`. Add `const
    ip_dbus_backend *dbus_backend`, `ip_bus_handle dbus_bus`, `bool
    dbus_connected` fields. Add includes for the three tab headers. Add
    accessor declarations: `cbx_controllers_tab
    *cbx_manager_controllers_tab(cbx_manager*)`, `cbx_profiles_tab
    *cbx_manager_profiles_tab(cbx_manager*)`, `cbx_settings_tab
    *cbx_manager_settings_tab(cbx_manager*)`.
  - `src/manager/manager.c`: In `cbx_manager_init()`, after panel creation and
    before layout/focus setup: (a) obtain `ip_dbus_sd_backend()`, call
    `backend->connect(&bus)`; on failure set `bus=NULL`, `backend=NULL`;
    (b) call `cbx_controllers_tab_init(&mgr->ct, &mgr->panels[0], backend,
    bus, &mgr->text_cache, &mgr->theme, mgr->font_id)`; (c) call
    `cbx_profiles_tab_init(&mgr->pt, &mgr->panels[1], &mgr->text_cache,
    &mgr->theme, mgr->font_id)` then `cbx_profiles_tab_refresh(&mgr->pt)`;
    (d) call `cbx_settings_tab_init(&mgr->st, &mgr->panels[2],
    &mgr->text_cache, &mgr->theme, mgr->font_id)` (auto-refreshes). Store
    `dbus_backend`, `dbus_bus`, `dbus_connected`. In `cbx_manager_shutdown()`,
    call `cbx_controllers_tab_shutdown()`, `cbx_profiles_tab_shutdown()`,
    `cbx_settings_tab_shutdown()` before the existing panel destruction loop,
    then disconnect DBus via `backend->disconnect(bus)` if connected. Implement
    the three accessor functions.
  - `tests/dbus_mock.h`: Add `const ip_dbus_backend *ip_dbus_sd_backend(void);`
    declaration so `manager.c` can call it.
- Acceptance criteria: After `cbx_manager_init()`, all three panels have
  `child_count > 0`. After `cbx_manager_shutdown()`, all tab widgets are
  destroyed and DBus is disconnected. The three accessor functions return
  non-NULL pointers to the manager-owned tab structs.
- Verification: Build with `nix-shell --run 'cmake --build build-manual'`.
  Run `ctest --test-dir build-manual -R 'test_manager_tabs' --output-on-failure`
  (existing tests will need updating in Task 3, but the build must succeed).
  A unit test added in Task 4 will verify nonempty panels. Manual code
  inspection confirms tab init/shutdown calls are in the correct order.
- Documentation impact: none (no SPEC change; manager.h comments updated to
  reflect tab ownership).

## Task 2: Wire tab refresh into tab switching

- Status: pending
- Dependencies: Task 1
- Scope: bounded files and behavior
  - `src/manager/manager.c`: In `cbx_manager_on_tab_change()`, after updating
    `active_tab` and panel visibility, call the refresh function for the newly
    active tab: `cbx_controllers_tab_refresh(&mgr->ct)` for Controllers (guard
    with `if (mgr->ct.backend)`), `cbx_profiles_tab_refresh(&mgr->pt)` for
    Profiles, `cbx_settings_tab_refresh(&mgr->st)` for Settings. This ensures
    stale data is replaced when the user navigates to a tab.
- Acceptance criteria: Switching to each tab calls the corresponding refresh
  function. The refresh calls do not crash when DBus is unavailable
  (controllers refresh guarded by backend NULL check).
- Verification: Build and run
  `ctest --test-dir build-manual -R 'test_manager_tabs' --output-on-failure`.
  The tab-switching tests should still pass with populated panels. A test in
  Task 4 will verify refresh is called on switch by checking updated list
  content.
- Documentation impact: none.

## Task 3: Update existing skeleton and integration tests for populated panels

- Status: pending
- Dependencies: Task 1
- Scope: bounded files and behavior
  - `tests/test_manager_tabs.c`: Update assertions that assume empty panels:
    (a) `test_manager_up_down_focus_navigation` — `fc->count` is no longer 1
    (tabbar + panel children); change to `fc->count > 1` and verify DOWN from
    tabbar navigates to a panel child; (b) `test_manager_panels_visibility` —
    add assertions that active panel has `child_count > 0`; (c)
    `test_manager_init_basic` — assert all three panels have children after
    init. All other tests (tab switching, render, null safety, shutdown)
    should pass unchanged or with minor assertion updates.
  - `tests/test_manager_integration.c`: Remove the manual
    `cbx_controllers_tab_init`, `cbx_profiles_tab_init`, and
    `cbx_settings_tab_init` calls from `setup()`. Instead, after
    `cbx_manager_init()`, obtain the tab instances via the new accessor
    functions (`cbx_manager_controllers_tab`, `cbx_manager_profiles_tab`,
    `cbx_manager_settings_tab`). For the controllers tab, set its `backend`
    and `bus` fields to the mock DBus backend/bus, then call
    `cbx_controllers_tab_load_supported_types()` and
    `cbx_controllers_tab_refresh()` to populate with mock data. For the
    profiles tab, call `cbx_profiles_tab_set_test_dirs()` then
    `cbx_profiles_tab_refresh()`. Update the `teardown()` to remove the
    manual `*_tab_shutdown()` calls (manager shutdown now handles them).
    Update all test functions that reference `f->ct`, `f->pt`, `f->st` to use
    the accessor-derived pointers.
- Acceptance criteria: All existing tests in `test_manager_tabs` and
  `test_manager_integration` pass with populated panels. No test manually
  calls `*_tab_init()` — tabs are initialized by `cbx_manager_init()` and
  accessed via accessors.
- Verification:
  `nix-shell --run 'cmake --build build-manual && ctest --test-dir build-manual -R "test_manager_tabs|test_manager_integration" --output-on-failure'`.
- Documentation impact: none.

## Task 4: Add production-path regression test

- Status: pending
- Dependencies: Task 1, Task 2
- Scope: bounded files and behavior
  - `tests/test_manager_production.c` (new): A regression test that exercises
    the same composition path as the executable — calls only
    `cbx_manager_init()` and `cbx_manager_shutdown()`, with no manual tab
    initialization. Tests:
    (a) **Nonempty panels**: after `cbx_manager_init()`, assert
    `cbx_manager_panel(&mgr, i)->child_count > 0` for all three tabs.
    (b) **Visible rendered body content**: for each tab, set `active_tab`,
    call `cbx_manager_render()`, and assert at least one child widget has
    non-zero width and height and `cbx_widget_is_visible()` returns true.
    (c) **Focus chain populated**: assert `cbx_manager_focus(&mgr)->count > 1`
    (tabbar + at least one panel child).
    (d) **Tab switching**: send SDLK_RIGHT/LEFT events and verify
    `active_tab` changes and the newly active panel has children.
    (e) **Shutdown clean**: `cbx_manager_shutdown()` does not crash; struct is
    zeroed; can re-init.
    Set up an isolated `$HOME` with a profiles directory in `setup()` so
    profiles tab refresh succeeds. Use SDL2 dummy driver. Use per-test
    `setup`/`teardown` via `cmocka_unit_test_setup_teardown`.
  - `tests/CMakeLists.txt`: Add `test_manager_production` target, link
    against `controllerbox` and `PkgConfig::CMOCKA`, set
    `CBX_FONT_PATH` compile definition, register with `add_test()`, and set
    `ENVIRONMENT SDL_VIDEODRIVER=dummy` like the other manager tests.
- Acceptance criteria: The new test passes and asserts nonempty panel children
  and visible rendered body content for all three tabs through the production
  `cbx_manager_init()` path only. The test is registered in ctest.
- Verification:
  `nix-shell --run 'cmake --build build-manual && ctest --test-dir build-manual -R test_manager_production --output-on-failure'`.
- Documentation impact: none.

## Task 5: Maintenance verification and documentation audit

- Status: pending
- Dependencies: Task 1, Task 2, Task 3, Task 4
- Scope: bounded files and behavior
  - Run the full project verifier: `nix-shell --run 'cmake --build build-manual
    && ctest --test-dir build-manual -E "^test_packaging$" --output-on-failure'`.
  - Verify BUG-0003 acceptance criteria: production startup owns the complete
    lifecycle of all three tab modules; all panels populated; Controllers shows
    controls (degraded-state content when InputPlumber unavailable); Profiles
    refreshes and shows controls; Settings shows default/current settings;
    focus, event dispatch, refresh, and shutdown operate through the production
    path; regression test exercises the same composition path as the
    executable.
  - Close BUG-0003 via `scripts/bug-ledger.py`: set non-empty `resolution`
    (e.g., "cbx_manager_init now initializes all three tab modules and
    populates every panel; cbx_manager_shutdown tears down tabs and disconnects
    DBus; regression test verifies nonempty panels via production path") and
    non-empty `verification` (e.g., "Full ctest suite passes; new
    test_manager_production asserts nonempty panel children and visible
    rendered body content for all tabs").
  - Confirm `docs/SPEC.md` was not modified (`git diff --exit-code docs/SPEC.md`).
  - Review `src/manager/manager.h` and `src/manager/manager.c` comments for
    accuracy (remove "Subsequent tasks populate panel contents" stale comments;
    update to reflect tab ownership).
- Acceptance criteria: Full ctest suite passes (excluding `test_packaging`).
  BUG-0003 is closed with non-empty resolution and verification. SPEC.md is
  unchanged. No stale comments about empty panels remain.
- Verification:
  `nix-shell --run 'cmake --build build-manual && ctest --test-dir build-manual -E "^test_packaging$" --output-on-failure'`;
  `python3 scripts/bug-ledger.py validate`;
  `python3 scripts/bug-ledger.py show BUG-0003` (status=closed, resolution non-empty);
  `git diff --exit-code docs/SPEC.md`.
- Documentation impact: `src/manager/manager.h` and `src/manager/manager.c`
  comment updates only; no SPEC or external doc changes.