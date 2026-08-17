# Task 1: Fix icon cache SVG path mismatch (BUG-0008, BUG-0009)

## Outcome
- Fixed `icon_cache.c:99` to build `{icon_dir}/svg/{name}.svg` matching CMake install layout
- Added `SOURCE_ICON_DIR` fallback in `cbx_icon_dir()` (config_paths.c) for build-tree operation
- Updated all test `SVG_DIR`/`OVERLAY_SVG_DIR` macros from `…/data/icons/svg/` to `…/data/icons`
- Added `test_production_path_icon_load` exercising `cbx_icon_dir()` → `cbx_icon_cache_init()` → `cbx_icon_cache_load()`
- Moved BUG-0008/0009 to closed ledger
- Updated conformance matrix: OVL-08, ICO-01 → verified

## Verification
- `nix-shell --run 'ctest --test-dir build-check --output-on-failure'` → 98/98 pass (2 hardware skips)
- `test_production_path_icon_load` passes — verifies production path with no env-var injection
- Remaining `test-production-path-bypass.sh` violations (6) are in `test_golden.c` and `test_manager_visual.c` — Task 2 scope

## Commit
- `f375ec4`: Task 1: Fix icon cache SVG path mismatch (BUG-0008, BUG-0009)

## Next Task
- Task 2: Remove test icon-path env-var bypasses (BUG-0010) — remove `setenv("CBX_ICON_DIR",…)` from `test_manager_visual.c` and `test_golden.c`, make all tests use `cbx_icon_dir()` production path