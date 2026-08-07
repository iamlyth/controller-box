# Open Bugs

Canonical queue of defects awaiting maintenance.

Schema: `ralph-bug-ledger/v1`

```json
[
  {
    "id": "BUG-0002",
    "title": "CreateComposite XDG runtime test is order-dependent and leaks temp directories",
    "status": "triaged",
    "severity": "medium",
    "reported": "2026-08-06",
    "external": [],
    "contract_change": false,
    "reproduction": "Inside nix-shell, configure and build the project, then run ctest --test-dir build-manual -E '^test_packaging$' --output-on-failure while one or more unrelated files matching /tmp/controller-box-* exist. test_create_composite_xdg_runtime_dir_preferred may fail at tests/test_create_composite.c:272 with a nonzero count such as '3 != 0'; the same test may pass when rerun alone.",
    "expected": "The XDG runtime directory test proves that its CreateCompositeDevice call does not create a fallback file in /tmp without depending on or deleting unrelated pre-existing /tmp/controller-box-* files, and it always removes its own /tmp/cbx-xdg-* directory.",
    "actual": "The test asserts that the global count of /tmp/controller-box-* is exactly zero instead of comparing before and after. Pre-existing or concurrent files make the test fail nondeterministically, and an assertion failure bypasses rmdir(), leaving its cbx-xdg temporary directory behind.",
    "acceptance": "Seed an unrelated /tmp/controller-box-* file and verify test_create_composite still passes without modifying that file; compare relevant /tmp state before and after or otherwise identify only files created by the operation; guarantee cleanup of the test-owned XDG directory on success and failure; repeated standalone and full CTest runs pass.",
    "resolution": "",
    "verification": "",
    "closed": null
  },
  {
    "id": "BUG-0003",
    "title": "Production manager leaves every tab body uninitialized",
    "status": "in_progress",
    "severity": "critical",
    "reported": "2026-08-06",
    "external": [],
    "contract_change": false,
    "reproduction": "Build and locally install Controller-Box, then launch controller-box --manager. The Controllers, Profiles, and Settings labels appear and tab selection changes, but each tab body remains completely blank regardless of InputPlumber availability.",
    "expected": "Production manager startup attaches and initializes the Controllers, Profiles, and Settings modules so each tab displays its lists, buttons, status text, and default or degraded-state content. Profiles and Settings remain usable when InputPlumber is unavailable.",
    "actual": "src/app/main.c calls only cbx_manager_init() and cbx_manager_run(). cbx_manager_init() creates three empty panels, but production code never calls cbx_controllers_tab_init(), cbx_profiles_tab_init(), or cbx_settings_tab_init(). Integration tests manually perform the missing initialization, so they pass while the executable renders panels with zero children. This also means BUG-0001's visible-controls acceptance criterion was only partially satisfied.",
    "acceptance": "Production startup owns the complete lifecycle of all three tab modules and populates every panel; Controllers shows controls plus connected or degraded-state content, Profiles refreshes and shows controls, and Settings shows default/current settings without requiring InputPlumber; focus, event dispatch, refresh, and shutdown operate through the production path; a regression test exercises the same composition path as the executable, asserts nonempty panel children and visible rendered body content for all tabs, and the full project verifier passes.",
    "resolution": "",
    "verification": "",
    "closed": null
  }
]
```
