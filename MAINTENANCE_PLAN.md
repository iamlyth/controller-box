---
bug_id: BUG-0001
bug_fingerprint: f54e2df5cbb1e2fac06b74e3c607f458b19321a705ccbd9c7e0686136df4c191
spec_path: docs/SPEC.md
spec_commit: 12f82db38f999986de4216dfc50a6e13452db4c9
spec_blob: 0522f7f1aa79d70b343ed6022956683a7c11695f
base_commit: 2a8fc0ae27b49e557fcd06e198e2c1de3326918e
status: active
---

# Maintenance Plan: BUG-0001 — Manager launches with an effectively blank interface

## Goal

Fix the critical defect where `controller-box --manager` opens a window that
renders only dark backgrounds, tab rectangles, and the window decoration but no
application text, labels, or usable controls. The root cause is that
`src/app/main.c` passes `NULL` as the font path to `cbx_manager_init()`, so no
font is ever loaded (`font_id` remains `-1`) and every `cbx_text_render()` call
silently returns `NULL`. The fix must ensure the production binary discovers and
loads a system font at runtime, renders visible manager labels and controls, and
produces an actionable error (not a blank UI) when no font resource is available.

## Non-goals

- Changing the product specification (`docs/SPEC.md`) — this is an implementation
  defect, not a contract change.
- Bundling or vendoring a font binary in the repository — the fix uses system
  font discovery, matching the existing test infrastructure pattern.
- Redesigning the widget system, tab bar, or rendering pipeline.
- Fixing the unrelated GTK theme warning or the non-fatal OpenGL alpha-blending
  verification warning (the latter is confirmed working correctly).
- Addressing the `CMAKE_INSTALL_PREFIX` configure-time vs install-time mismatch
  for `DATA_DIR`/`ICON_DIR` (separate concern, not part of this bug).

## Defect analysis

### Root cause

`src/app/main.c:77` calls `cbx_manager_init(&mgr, NULL)` — the production binary
hardcodes `NULL` as the font path. In `src/manager/manager.c:70-72`, the
conditional `if (font_path && font_path[0] != '\0')` is skipped entirely when
`font_path` is `NULL`, so `cbx_text_load_font()` is never called and
`mgr->font_id` stays `-1`.

### Downstream effect

All widgets (`tabbar`, `label`, `button`, `list`) receive `font_id = -1`. In
`src/ui/text.c`, `get_font()` returns `NULL` for any `font_id < 0`, and
`cbx_text_render()` returns `NULL` immediately. Every text-rendering call
silently produces nothing — no textures are created, no `SDL_RenderCopy` calls
are made. The UI draws structural rectangles (panel backgrounds, tab outlines)
but no text or control labels.

### Why tests did not catch this

The test CMakeLists.txt (`tests/CMakeLists.txt:239-253`) has a compile-time font
discovery loop that sets `CBX_FONT_PATH` to the first existing `DejaVuSans.ttf`
among four candidate paths. Tests pass `CBX_FONT_PATH` to `cbx_manager_init()`.
However, the production binary (`main.c`) has no equivalent discovery logic and
passes `NULL`. No test exercises the `main.c` → `run_manager()` production code
path to verify that a font is actually loaded.

### Additional issues

1. **No font path constant in `config.h.in`**: The config template defines
   `DATA_DIR` and `ICON_DIR` but no font-related path.
2. **Silent degradation**: The comment in `manager.c:72` says "Non-fatal: manager
   works without text rendering" — but in practice the manager is unusable
   without text. The silent degradation masks the root cause from users.
3. **NixOS font path staleness**: The test CMakeLists.txt hardcodes a Nix store
   hash (`zzs2q7lk5mn6y2rywd3snhak7098zs66`) that changes on every package
   rebuild, making that candidate unreliable.

## Task 1: Add runtime font discovery to config_paths
- Status: complete
- Dependencies: none
- Scope: `src/config/config_paths.c`, `src/config/config_paths.h`, `config.h.in`, `CMakeLists.txt`. Add a `cbx_font_path()` function that searches common system font directories at runtime for a usable TTF font (DejaVuSans.ttf prioritized). Candidate search order: `$XDG_DATA_HOME/fonts/`, `$HOME/.local/share/fonts/`, `$HOME/.fonts/`, `/usr/share/fonts/truetype/dejavu/`, `/usr/share/fonts/dejavu/`, `/usr/share/fonts/TTF/`, `/run/current-system/sw/share/X11/fonts/` (NixOS), `/nix/var/nix/profiles/default/share/X11/fonts/`. Return a `const char *` pointing to a static buffer (or `NULL` if no font is found). Add a `FONT_DIR` compile-time constant to `config.h.in` (parallel to `DATA_DIR`/`ICON_DIR`) set to `${CBX_DATA_DIR}/fonts` in CMakeLists.txt, and add a CMake install rule for a `data/fonts/` directory (created empty with a `.gitkeep` so future bundled fonts are installed automatically). Do not bundle a font binary in this task.
- Acceptance criteria: `cbx_font_path()` is declared in `config_paths.h`, defined in `config_paths.c`, returns a readable `.ttf` path on systems with DejaVuSans installed, and returns `NULL` when no font is found. `FONT_DIR` is defined in generated `config.h`. CMake configure and build succeed with no warnings.
- Verification: `nix-shell --run 'cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build'` succeeds. A new unit test `test_font_path` (added to `tests/CMakeLists.txt`, linked against `controllerbox` and cmocka, run with `SDL_VIDEODRIVER=dummy`) asserts that when `cbx_font_path()` returns non-NULL the path is `access(R_OK)`-readable, and when it returns `NULL` no crash occurs. `ctest -R test_font_path --output-on-failure` passes.
- Documentation impact: `config.h.in` gains a `FONT_DIR` comment block. `src/config/config_paths.h` gains a `cbx_font_path()` declaration with a doxygen comment. No spec changes.

## Task 2: Wire font discovery into manager launch and make failure actionable
- Status: pending
- Dependencies: Task 1
- Scope: `src/app/main.c`, `src/manager/manager.c`. Update `run_manager()` in `main.c` to call `cbx_font_path()` and pass the result to `cbx_manager_init()` instead of `NULL`. When `cbx_font_path()` returns `NULL`, print an actionable error to stderr (e.g., `"controller-box: no system font found; install DejaVuSans or a TTF font in standard font directories"`) and return `1` without launching the manager. In `manager.c`, update `cbx_manager_init()`: when `font_path` is non-NULL but `cbx_text_load_font()` fails (returns negative), print a clear error to stderr naming the font path and the error code, and return the error code rather than silently continuing with `font_id = -1`. Remove or update the misleading "Non-fatal" comment.
- Acceptance criteria: `controller-box --manager` with a font available produces a manager with `font_id >= 0` and visibly rendered tab labels and controls. `controller-box --manager` with no font available prints an actionable error message to stderr and exits non-zero instead of showing a blank window. `cbx_manager_init()` with a non-NULL but invalid font path returns a non-zero error code and prints the path and error.
- Verification: `nix-shell --run 'cmake --build build'` succeeds. `nix-shell --run 'SDL_VIDEODRIVER=dummy ./build/controller-box --dry-run --manager'` exits 0 (dry-run path unaffected). Manual or scripted check: `SDL_VIDEODRIVER=dummy CBX_FONT_PATH_OVERRIDE=/nonexistent ./build/controller-box --manager` prints a font error and exits non-zero (may require a test harness or environment override). Existing tests `test_manager_tabs`, `test_manager_integration` still pass: `nix-shell --run 'ctest -R "test_manager" --output-on-failure'`.
- Documentation impact: No spec changes. The misleading code comment in `manager.c` is corrected. `main.c` `run_manager()` gains a brief comment explaining font discovery.

## Task 3: Automated regression test for font initialization and text rendering
- Status: pending
- Dependencies: Task 2
- Scope: `tests/test_font_init.c`, `tests/CMakeLists.txt`. Add a regression test that exercises the full production font path: call `cbx_font_path()` to discover a font (or use `CBX_FONT_PATH` compile-time fallback as tests already do), call `cbx_manager_init()` with the discovered path, and assert `mgr.font_id >= 0`. Then call `cbx_text_render()` on the manager's text cache with the loaded `font_id` and assert the returned `SDL_Texture *` is non-NULL. Also test the failure path: call `cbx_manager_init()` with a non-NULL invalid path (e.g., `"/nonexistent/font.ttf"`) and assert the return code is non-zero. The test must be headless-safe (`SDL_VIDEODRIVER=dummy` environment set via `set_tests_properties`). If no system font is available in the test environment, the font-dependent assertions use `skip()` (matching the existing `test_manager_tabs` pattern at line 40), but the invalid-path failure assertion always runs.
- Acceptance criteria: A `test_font_init` ctest exists, is registered in `tests/CMakeLists.txt`, runs headless with `SDL_VIDEODRIVER=dummy`, and asserts (a) discovered font → `font_id >= 0` → `cbx_text_render()` returns non-NULL, and (b) invalid font path → `cbx_manager_init()` returns non-zero. The test passes in the nix-shell environment.
- Verification: `nix-shell --run 'cmake -B build && cmake --build build'` succeeds. `nix-shell --run 'ctest -R test_font_init --output-on-failure'` passes. `nix-shell --run 'ctest --output-on-failure'` (full suite) still passes with no regressions.
- Documentation impact: None. Test-only addition.

## Task 4: Maintenance verification and documentation audit
- Status: pending
- Dependencies: Task 1, Task 2, Task 3
- Scope: Run the full project verifier, confirm all acceptance criteria from the bug record are met, close BUG-0001 in the ledger with non-empty resolution and verification strings, and audit documentation for accuracy. Verify that `docs/SPEC.md` was not modified. Confirm the plan's `spec_blob` still matches `git rev-parse HEAD:docs/SPEC.md`. Update `src/config/config_paths.h` doxygen if the public API surface changed. Check that no misleading "Non-fatal" comments remain in the manager initialization path.
- Acceptance criteria: Full test suite passes (`ctest --output-on-failure`) with zero failures. Build succeeds in Debug mode with no new warnings. `python3 scripts/bug-ledger.py validate` reports valid. BUG-0001 is closed via `python3 scripts/bug-ledger.py close BUG-0001 --resolution "<summary>" --verification "<evidence>"` with non-empty resolution and verification. `docs/SPEC.md` is unchanged (`git diff --exit-code docs/SPEC.md`). The bug's acceptance criteria are all met: (1) manager visibly renders labels and controls with a system font, (2) missing font produces actionable failure, (3) automated regression test exercises real font init and text rendering, (4) full project verifier passes.
- Verification: `nix-shell --run 'cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ctest --output-on-failure'` — all tests pass. `python3 scripts/bug-ledger.py validate` — valid. `python3 scripts/bug-ledger.py show BUG-0001` — status is `closed`, resolution and verification are non-empty. `git diff --exit-code docs/SPEC.md` — no changes. `git diff --exit-code MAINTENANCE_PLAN.md` — only the plan file and scratchpad were modified beyond source/test changes.
- Documentation impact: Bug ledger updated with resolution and verification. No spec changes. Source comments corrected where misleading.

MAINTENANCE_PLAN_COMPLETE
<!-- Strict maintenance-plan schema validated after draft checkpoint recovery. -->
