# Maintenance BUG-0001 — Scratchpad

## Iteration 1: Task 1 complete

- Transitioned BUG-0001 from `planned` to `in_progress` via `bug-ledger.py`.
- Created runtime tasks for all 4 maintenance plan tasks.
- Implemented Task 1: Add runtime font discovery to config_paths.
  - Added `cbx_font_path()` and `cbx_font_dir()` to `src/config/config_paths.{c,h}`.
  - `cbx_font_path()` searches XDG_DATA_HOME, $HOME/.local/share/fonts, $HOME/.fonts,
    and system font dirs (including NixOS paths) for DejaVuSans.ttf. Returns const char*
    to static buffer or NULL.
  - Added `FONT_DIR` to `config.h.in` and `CBX_FONT_DIR` to `CMakeLists.txt`.
  - Added install rule for `data/fonts/` with `.gitkeep`.
  - Added `test_font_path.c` unit test (5 test cases). All pass.
  - Build succeeds in Debug mode with no warnings.
  - Full suite: 64/64 tests pass (was 63, +1 new test).

### Format-truncation lesson (reinforces existing memory)
GCC Debug `-Werror=format-truncation` flags `snprintf` into a buffer when the source
variable has the same or larger declared size. Fix: make destination buffers
progressively larger than source buffers by at least the suffix length. Used
PATH_MAX+16, PATH_MAX+32, PATH_MAX+64, PATH_MAX+128 in the chain.

### Next iteration
- Task 2: Wire font discovery into manager launch (main.c, manager.c).
  Depends on Task 1 (now complete). Runtime task task-1786062044-7006 is open.
## Iteration 2: Task 2 complete

- Implemented Task 2: Wire font discovery into manager launch and make failure actionable.
  - `src/app/main.c`: `run_manager()` now calls `cbx_font_path()` instead of passing NULL.
    When `cbx_font_path()` returns NULL, prints actionable error to stderr and returns 1
    without launching the manager. Added `#include "config/config_paths.h"`.
  - `src/manager/manager.c`: `cbx_manager_init()` now checks `cbx_text_load_font()` return.
    When font_path is non-NULL but loading fails (returns <0), prints error naming the
    path and error code, cleans up text cache + renderer, and returns the error code.
    Removed misleading "Non-fatal: manager works without text rendering" comment.
- Verification:
  - Build succeeds in Debug mode with no warnings.
  - All 64/64 tests pass (no regressions).
  - `--dry-run --manager` exits 0 (dry-run path unaffected).
  - `test_manager_tabs`, `test_manager_integration` pass.
  - Invalid font path in cbx_manager_init returns non-zero (code path verified; formal
    regression test is Task 3's scope).
- Committed as checkpoint.

### Next iteration
- Task 3: Automated regression test for font initialization and text rendering.
  Runtime task task-1786062047-5ca0 is open. Depends on Task 2 (now complete).

## Iteration 3: Task 3 complete

- Implemented Task 3: Automated regression test for font initialization and text rendering.
  - Created `tests/test_font_init.c` with 4 test cases:
    1. `test_font_init_and_render`: Discovers font via `cbx_font_path()` or
       `CBX_FONT_PATH` compile-time fallback, calls `cbx_manager_init()`, asserts
       `font_id >= 0`, then calls `cbx_text_render()` and asserts non-NULL
       `SDL_Texture*`. Skips if no font available.
    2. `test_invalid_font_path_fails`: `cbx_manager_init()` with nonexistent path
       (`/nonexistent/font.ttf`) returns non-zero. Always runs.
    3. `test_null_font_path_succeeds`: NULL path skips font loading, init succeeds
       with `font_id = -1`.
    4. `test_empty_font_path_succeeds`: Empty string treated like NULL.
  - Registered in `tests/CMakeLists.txt` after the `CBX_FONT_PATH` discovery loop
    (important: must be after `set(CBX_FONT_PATH ...)` or the compile definition
    will be empty).
  - Headless-safe via `set_tests_properties(... ENVIRONMENT "SDL_VIDEODRIVER=dummy")`.
- Verification:
  - Build succeeds in Debug mode with no warnings (clean build).
  - `ctest -R test_font_init --output-on-failure` passes.
  - Full suite: 65/65 tests pass in nix-shell (was 64, +1 new test).
  - The 2 failures outside nix-shell (test_flatpak_manifest, test_packaging) are
    pre-existing environment issues (PyYAML not installed, clean configure can't
    find sdl2 without nix-shell) — not regressions.
- Committed as c252627.

### CMakeLists.txt ordering lesson
The `CBX_FONT_PATH` variable is set by a `foreach` loop at line ~258 in
`tests/CMakeLists.txt`. Test targets that use `target_compile_definitions(...
CBX_FONT_PATH="${CBX_FONT_PATH}")` MUST be registered AFTER that loop, not
before it, or the compile definition will be empty.

### Next iteration
- Task 4: Maintenance verification and documentation audit.
  Runtime task task-1786062047-6a0c is open. Depends on Tasks 1-3 (all complete).

## Iteration 4: Task 4 complete — MAINTENANCE COMPLETE

- Implemented Task 4: Maintenance verification and documentation audit.
  - Ran `scripts/check-maintenance-freshness.sh` — passed (BUG-0001, spec matches).
  - Full build in Debug mode: 0 warnings, clean build.
  - Full test suite: 65/65 tests pass (nix-shell, ctest --output-on-failure).
  - Full project verifier (`scripts/verify-project.sh`): passes — build, tests,
    packaging integration all pass.
  - Fixed pre-existing typo in `scripts/verify-project.sh`: called
    `tests/test-packaging.sh` (hyphen) instead of `tests/test_packaging.sh`
    (underscore). This was blocking the maintenance completion gate.
  - `docs/SPEC.md` unchanged (`git diff --exit-code` = 0).
  - `spec_blob` in plan matches `git rev-parse HEAD:docs/SPEC.md`.
  - No misleading "Non-fatal" comments in manager init path (manager.c, main.c).
  - `config_paths.h` doxygen comments are accurate for `cbx_font_path()` and
    `cbx_font_dir()`.
  - `python3 scripts/bug-ledger.py validate` — valid (0 open, 1 closed).
  - Closed BUG-0001 via `bug-ledger.py close` with non-empty resolution and
    verification strings.
  - Updated MAINTENANCE_PLAN.md: Task 4 status → complete, front-matter
    status → complete.
- All 4 maintenance plan tasks complete. BUG-0001 closed.
