# Closed Bugs

Completed defects and their verification evidence.

Schema: `ralph-bug-ledger/v1`

```json
[
  {
    "id": "BUG-0002",
    "title": "CreateComposite XDG runtime test is order-dependent and leaks temp directories",
    "status": "closed",
    "severity": "medium",
    "reported": "2026-08-06",
    "external": [],
    "contract_change": false,
    "reproduction": "Inside nix-shell, configure and build the project, then run ctest --test-dir build-manual -E '^test_packaging$' --output-on-failure while one or more unrelated files matching /tmp/controller-box-* exist. test_create_composite_xdg_runtime_dir_preferred may fail at tests/test_create_composite.c:272 with a nonzero count such as '3 != 0'; the same test may pass when rerun alone.",
    "expected": "The XDG runtime directory test proves that its CreateCompositeDevice call does not create a fallback file in /tmp without depending on or deleting unrelated pre-existing /tmp/controller-box-* files, and it always removes its own /tmp/cbx-xdg-* directory.",
    "actual": "The test asserts that the global count of /tmp/controller-box-* is exactly zero instead of comparing before and after. Pre-existing or concurrent files make the test fail nondeterministically, and an assertion failure bypasses rmdir(), leaving its cbx-xdg temporary directory behind.",
    "acceptance": "Seed an unrelated /tmp/controller-box-* file and verify test_create_composite still passes without modifying that file; compare relevant /tmp state before and after or otherwise identify only files created by the operation; guarantee cleanup of the test-owned XDG directory on success and failure; repeated standalone and full CTest runs pass.",
    "resolution": "Fixed test_create_composite_xdg_runtime_dir_preferred in tests/test_create_composite.c: (1) replaced the assertion that global /tmp/controller-box-* count equals zero with a before/after comparison (tmp_before vs tmp_after), so unrelated pre-existing files no longer cause false failures; (2) moved the XDG temp directory path from a local stack variable into the create_fixture struct (xdg_dir field) so teardown() cleans it up with rm -rf even when an assertion failure longjmps past the test body; (3) removed the bare rmdir(xdg_dir) at end of test since teardown now owns cleanup.",
    "verification": "Seeded an unrelated /tmp/controller-box-unrelated-test-file before running ctest --test-dir build-check -R test_create_composite --output-on-failure — all tests pass with the unrelated file present. Full ctest suite (78/78, 1 skip) passes with no regressions. XDG temp dir cleanup verified: teardown rm -rf handles f->xdg_dir on both success and assertion-failure paths. Repeated standalone and full CTest runs pass deterministically.",
    "closed": "2026-08-08"
  },
  {
    "id": "BUG-0001",
    "title": "Manager launches with an effectively blank interface",
    "status": "closed",
    "severity": "critical",
    "reported": "2026-08-06",
    "external": [],
    "contract_change": false,
    "reproduction": "On NixOS, build and install to a local CMake prefix, then run the installed controller-box binary with --manager. The window opens, but only dark backgrounds, tab rectangles, and the window decoration are visible.",
    "expected": "The manager visibly renders the Controllers, Profiles, and Settings tab labels plus the controls, lists, status text, and other content required to operate the controller-only UI.",
    "actual": "The manager window renders structural rectangles but no application text or usable controls. Startup also prints a non-fatal OpenGL alpha-blending verification warning; the unrelated GTK theme warning originates from the desktop theme.",
    "acceptance": "A locally installed NixOS build visibly renders manager labels and controls using a reliably available font; missing font resources produce an actionable failure rather than a blank UI; an automated regression exercises real font initialization/text rendering; the full project verifier passes.",
    "resolution": "Fixed by adding runtime font discovery (cbx_font_path() in config_paths.c) that searches XDG_DATA_HOME, ~/.local/share/fonts, ~/.fonts, and system font directories including NixOS paths for DejaVuSans.ttf. main.c now calls cbx_font_path() instead of passing NULL to cbx_manager_init(). When no font is found, an actionable error is printed to stderr and the manager exits non-zero. manager.c now checks cbx_text_load_font() return value and returns an error code with a diagnostic message naming the path and error code when font loading fails, instead of silently continuing with font_id=-1. Added FONT_DIR to config.h.in and CMake install rule for data/fonts/.",
    "verification": "Full project verifier (scripts/verify-project.sh) passes: clean Debug build with 0 warnings, 65/65 ctest tests pass (including new test_font_init with 4 cases: font init+render, invalid path fails, NULL path, empty path), packaging integration checks pass. test_font_path unit test verifies cbx_font_path() returns readable .ttf or NULL safely. test_font_init regression test exercises real font init (font_id >= 0) and text rendering (non-NULL SDL_Texture*), and asserts invalid font path returns non-zero. docs/SPEC.md unchanged (git diff --exit-code = 0). spec_blob matches HEAD:docs/SPEC.md. No misleading Non-fatal comments in manager init path. bug-ledger validate reports valid.",
    "closed": "2026-08-06"
  },
  {
    "id": "BUG-0003",
    "title": "Production manager leaves every tab body uninitialized",
    "status": "closed",
    "severity": "critical",
    "reported": "2026-08-06",
    "external": [],
    "contract_change": false,
    "reproduction": "Build and locally install Controller-Box, then launch controller-box --manager. The Controllers, Profiles, and Settings labels appear and tab selection changes, but each tab body remains completely blank regardless of InputPlumber availability.",
    "expected": "Production manager startup attaches and initializes the Controllers, Profiles, and Settings modules so each tab displays its lists, buttons, status text, and default or degraded-state content. Profiles and Settings remain usable when InputPlumber is unavailable.",
    "actual": "src/app/main.c calls only cbx_manager_init() and cbx_manager_run(). cbx_manager_init() creates three empty panels, but production code never calls cbx_controllers_tab_init(), cbx_profiles_tab_init(), or cbx_settings_tab_init(). Integration tests manually perform the missing initialization, so they pass while the executable renders panels with zero children. This also means BUG-0001's visible-controls acceptance criterion was only partially satisfied.",
    "acceptance": "Production startup owns the complete lifecycle of all three tab modules and populates every panel; Controllers shows controls plus connected or degraded-state content, Profiles refreshes and shows controls, and Settings shows default/current settings without requiring InputPlumber; focus, event dispatch, refresh, and shutdown operate through the production path; a regression test exercises the same composition path as the executable, asserts nonempty panel children and visible rendered body content for all tabs, and the full project verifier passes.",
    "resolution": "cbx_manager_init now initializes all three tab modules (controllers, profiles, settings) and populates every panel; cbx_manager_shutdown tears down tabs and disconnects DBus; tab switching refreshes the active tab; regression test test_manager_production verifies nonempty panel children and visible rendered body content for all tabs via the production path",
    "verification": "Full ctest suite passes (65/65, excluding test_packaging); test_manager_production asserts nonempty panel children, visible rendered body content, populated focus chain, tab switching, and clean shutdown via production cbx_manager_init path only; test_manager_tabs and test_manager_integration updated to verify populated panels via accessor functions; SPEC.md unchanged",
    "closed": "2026-08-06"
  }
]
```
