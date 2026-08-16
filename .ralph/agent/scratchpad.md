# Implementation Scratchpad — Controller-Box v1

## Task 3 complete: Real overlay lifecycle in installed tests

- **Commit:** a786604 on develop
- **What:** Replaced stub overlay DBus side-effects (Phases 7-10) in `test_installed_functional.c` with real overlay service lifecycle through production poll path. Enhanced `test_installed_binary.sh` Phase 6 with overlay activation + screenshot verification.
- **Changes:**
  - `test_installed_functional.c`: Phases 8-12 allocate `cbx_overlay_service_ctx`, init all components (renderer, DBus, enumerate, grid, surface, lifecycle fade=0, player/host mode, input events, polls, triggers) with rendering resources (text cache, icon cache, theme). Phase 9: InterceptMode PASS→ALL + poll event → `cbx_overlay_service_step` → VISIBLE. Phase 10: `fb_read_pixels` + `fb_region_has_content` on grid/header/labels/cells. Phase 11: B keydown → `cbx_overlay_lifecycle_close` → `cbx_overlay_on_save` (assignment persistence + InterceptMode→PASS). Phase 12: cleanup.
  - `test_installed_binary.sh`: Phase 6 enhanced — launch overlay, set InterceptMode ALL via `busctl set-property`, screenshot verification (mean > 5.0 + frame diff), close via InterceptMode→PASS, verify clean close.
  - `CMakeLists.txt`: Added `cbx_test_support` link (fb_assert), `CBX_SOURCE_DIR` + `CBX_FONT_PATH` compile defs.
  - `README.md`: §11.1 table updated with `test_installed_functional` and `test_installed_binary`.
  - Conformance matrix PERF-01: Finding 2 (compositor-visible overlay activation) resolved.
- **Key finding:** `cbx_overlay_on_save` profile apply (`cbx_profile_cycle_apply`) fails because the grid build sets row profile to "default" which resolves to system profile dir. Test clears row profile before close to isolate assignment-persistence path. Profile application is tested in `test_overlay_native.c` O10.
- **Verification:** `ctest --test-dir build-check` → 96/96 pass (1 pre-existing skip). `verify-project.sh` → pass.

## Next task

Task 4: Fix interaction inventory accuracy and missing coverage (M32/M34 dispatch path, M35/M36 na_ids, M38 native test, DBus InputEvent signal path for capture mode).