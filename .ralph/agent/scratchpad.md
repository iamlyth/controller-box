# Task 2: Remove test icon-path env-var bypasses (BUG-0010)

## Outcome
- Removed `getenv("CBX_ICON_DIR")` override from `cbx_icon_dir()` in `config_paths.c` — production code no longer checks env var for resource paths
- Removed all `setenv`/`unsetenv("CBX_ICON_DIR")` from `test_manager_visual.c` and `test_golden.c`
- Replaced all hardcoded `SVG_DIR`/`OVERLAY_SVG_DIR` passed to `cbx_icon_cache_init` with `cbx_icon_dir()` in 7 test files: `test_icon_cache.c`, `test_icon_lookup.c`, `test_overlay_visual.c`, `test_golden.c`, `test_installed_functional.c`, `test_backend_smoke_sw.c`, `test_backend_smoke.c`
- Removed unused `SVG_DIR`/`OVERLAY_SVG_DIR` macro definitions
- Removed `putenv("CBX_ICON_DIR=")` from `test_production_path_icon_load`
- Moved BUG-0010 to closed ledger
- Updated conformance matrix: OVL-09, MGR-06, MGR-08, DOD-04, DOD-06 → verified

## Verification
- `grep -r 'setenv.*CBX_ICON_DIR' tests/` → no matches
- `./tests/test-production-path-bypass.sh` → passes (no resource-path env-var injection found)
- `nix-shell --run 'ctest --test-dir build-check --output-on-failure'` → 98/98 pass (2 hardware skips)
- `python3 scripts/bug-ledger.py validate` → valid (0 open, 10 closed)

## Commit
- `38db6ef`: Task 2: Remove test icon-path env-var bypasses (BUG-0010)

## Next Task
- Task 3: Add re-enumeration timing test (§2.4) — test verifies re-enumeration completes ≤2s after NameOwnerChanged via production `ip_connection.c` callback path