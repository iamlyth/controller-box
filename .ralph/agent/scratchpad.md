# Scratchpad — Framebuffer Visual Acceptance Implementation

## Iteration: Task 1 — Framebuffer test infrastructure

### What was done
- Created `tests/fb_assert.h` — API for pixel readback, region assertions, golden compare, PNG save
- Created `tests/fb_assert.c` — implementation with integer-only comparisons (no floats)
  - `fb_read_pixels` — wraps `SDL_RenderReadPixels` with `SDL_PIXELFORMAT_ABGR8888`
  - `fb_region_has_content` — checks if any pixel differs from bg_color beyond tolerance
  - `fb_region_has_color` — checks if any pixel matches target_rgb within tolerance
  - `fb_frames_differ` — integer percentage comparison (differ*100 > total*threshold)
  - `fb_golden_compare` — loads PNG via IMG_Load, converts to ABGR8888, per-pixel compare
  - `fb_save_png` — creates SDL_Surface from RGBA buffer, IMG_SavePNG
  - `fb_save_diff` — highlights differing pixels in red, matches dimmed to 50%
- Created `tests/test_fb_assert.c` — 9 cmocka sub-tests covering all functions
- Updated `tests/CMakeLists.txt`:
  - Added `fb_assert.c` to `cbx_test_support` static library
  - Added `PkgConfig::SDL2IMG` as PUBLIC dep on `cbx_test_support`
  - Registered `test_fb_assert` as ctest with `SDL_VIDEODRIVER=dummy`
- Fixed deprecated `assert_in_range` → `assert_int_in_range` (cmocka 2.0.2)

### Verification
- `nix-shell --run "cmake --build build-check --target test_fb_assert && ctest --test-dir build-check -R test_fb_assert --output-on-failure"` → PASS
- Full suite: 67/67 tests pass (66 existing + 1 new)

### Key decisions
- Used `SDL_PIXELFORMAT_ABGR8888` for pixel readback: on little-endian, bytes in memory are R,G,B,A — simplest direct access
- `threshold_pct` and `image_tol_pct` are `int` (0-100) to comply with "no floating-point" requirement
- `fb_assert.c` added to `cbx_test_support` so all future visual tests (Tasks 5, 7, 8, 10) can use it transitively
- `SDL2IMG` added as PUBLIC to `cbx_test_support` so all tests linking it get IMG_Load/IMG_SavePNG

### Next task
- Task 2 (Conflict red rendering) — depends on Task 1 (now complete)
- Task 3 (Overlay service init) — no dependencies, can proceed in parallel
- Task 6 (Manager DBus backend injection) — no dependencies, can proceed in parallel
## Iteration: Task 6 — Manager DBus backend injection point

### What was done
- Added `cbx_manager_init_with_dbus(mgr, font_path, backend, bus)` to `manager.h`/`manager.c`
  - When `backend` is non-NULL: uses it directly, sets `dbus_connected=true`, no `connect()` call
  - When `backend` is NULL: falls back to `ip_dbus_sd_backend()` + real connect (existing behavior)
- Refactored `cbx_manager_init` to a thin wrapper: `return cbx_manager_init_with_dbus(mgr, font_path, NULL, NULL)`
- Created `tests/test_manager_dbus_inject.c` with 4 sub-tests:
  1. `test_injected_backend_wired` — mock backend wired into `mgr->ct->backend` and `mgr->dbus_backend`
  2. `test_null_backend_falls_back` — NULL backend uses production path (degraded mode in CI)
  3. `test_wrapper_delegates` — `cbx_manager_init` wrapper works correctly
  4. `test_null_mgr_returns_einval` — NULL manager returns -EINVAL
- Registered `test_manager_dbus_inject` in `tests/CMakeLists.txt`

### Verification
- 3/3 targeted tests PASS (test_manager_dbus_inject, test_manager_production, test_manager_tabs)
- Full suite: 68/68 tests pass (67 existing + 1 new)

### Key decisions
- Used `const ip_dbus_backend *backend` (not `ip_dbus_backend *`) to match the struct field type
- Used `ip_bus_handle bus` (value, not pointer) to match struct field — mock bus is pre-connected
- No documentation impact (internal API extension per plan)

### Next task
- Task 3 (Overlay service init) — still ready, no dependencies
- Task 2 (Conflict red rendering) — depends on Task 1 (complete), should be ready
- Task 4 (Overlay poll loop) — depends on Task 3

## Iteration: Task 3 — Overlay service initialization

### What was done
- Created `src/app/overlay_service.h` — declares `int run_overlay_service(int dry_run);`
- Created `src/app/overlay_service.c` — full implementation:
  1. SDL video init + hidden renderer (cbx_renderer_init)
  2. DBus connect via ip_connection; "InputPlumber not found" error on failure
  3. Device enumeration via cbx_objectmanager_enumerate
  4. Settings/assignments loading (best-effort, defaults OK)
  5. Text cache, theme, icon cache/map setup
  6. Grid build (cbx_select_grid_build) + overlay surface init + pre-render
  7. Trigger registration (cbx_trigger_register_all) + InterceptMode=PASS
  8. Lifecycle init (cbx_overlay_lifecycle_init)
  9. Minimal poll loop skeleton (Task 4 fills in InterceptMode polling, signals)
  10. Clean shutdown (force_close, destroy surface, cleanup caches, disconnect)
- Modified `src/app/main.c` — removed inline stub, includes overlay_service.h
- Added `src/app/overlay_service.c` to controllerbox library in CMakeLists.txt
- Created `tests/test_overlay_service.c` — 2 cmocka sub-tests:
  (a) SDL_VIDEODRIVER=nonexistent → run_overlay_service(0) returns non-zero
  (b) dry-run → run_overlay_service(1) returns 0
- Registered test_overlay_service in tests/CMakeLists.txt
- Updated docs/OPERATIONS.md with overlay service startup sequence, prerequisites, failure modes

### Verification
- `./build-check/controller-box --overlay-service --dry-run` → exit 0 ✓
- `SDL_VIDEODRIVER=nonexistent ./build-check/controller-box --overlay-service` → exit 1 ✓
- `./build-check/controller-box --manager --dry-run` → exit 0 ✓ (unaffected)
- 69/69 ctest pass (67 existing + 2 new; 2 pre-existing packaging failures unchanged)

### Key decisions
- Extracted run_overlay_service into separate overlay_service.c (not main.c) so cmocka tests can link against it via the controllerbox library without pulling in main()
- Used `dbus_mock.h` for ip_dbus_backend/ip_dbus_sd_backend (this is where the production vtable is declared, not a separate ip_dbus.h)
- Best-effort loading for settings, assignments, icon map, icon cache — service runs with defaults if files are missing
- font_id = -1 when no font available; text_cache set to NULL in render_ctx to skip text rendering gracefully
- Poll loop is a minimal SDL event loop with 50ms delay (IP_INTERCEPT_POLL_INTERVAL_MS) — Task 4 adds InterceptMode polling, signal handling, input processing

### Next task
- Task 4 (Overlay poll loop) — now unblocked (depends on Task 3, which is complete)
- Task 2 (Conflict red rendering) — depends on Task 1 (complete), should be ready
- Task 10 (Backend smoke coverage) — depends on Task 1 (complete), should be ready
