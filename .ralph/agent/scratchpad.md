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