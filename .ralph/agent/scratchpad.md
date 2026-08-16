# Implementation Scratchpad — Controller-Box v1

## Task 5 complete: Host mode visual rendering in overlay grid

- **Commit:** 7c85706 on develop
- **What:** Wired `cbx_host_mode_row_state()` into `cbx_select_grid_render()` via new `cbx_grid_render_ctx.hm` field. HOST/SELECTED/FROZEN rows now render with distinct visuals. Also fixed conflict_red to use `theme->conflict`, wired up `render_ctx.conflicts` and `render_ctx.hm` in overlay_service.c, and added real-time conflict detection after slot changes and grid rebuilds.
- **Changes:**
  - `host_mode.h`: Changed anonymous struct typedef to named `struct cbx_host_mode` for forward declaration.
  - `grid_render.h`: Added `typedef struct cbx_host_mode cbx_host_mode;` forward decl and `const cbx_host_mode *hm` field to `cbx_grid_render_ctx`.
  - `grid_render.c`: Added host mode color setup (success, text_accent, text_secondary, text_disabled, panel_bg from theme). Per-row: get `cbx_host_mode_row_state()`, apply HOST (green cell + 4px green bar), SELECTED (blue cell + 2px accent row border), FROZEN (dim bg, secondary text, disabled indicators/borders). Fixed `conflict_red` to use `theme->conflict`.
  - `overlay_service.c`: Wired `render_ctx.conflicts = &svc->conflicts` and `render_ctx.hm = &svc->hm`. Added `cbx_conflict_detect` after initial grid build, hotplug rebuilds, and `cbx_overlay_on_slot_change` for real-time conflict display.
  - `test_overlay_visual.c`: Added `test_host_mode_row_states` — uses actual `cbx_host_mode` state machine, verifies green in HOST cell, blue in SELECTED cell, no blue in FROZEN cell, frame differs from player mode.
  - `test_golden.c`: Updated `test_golden_overlay_host_mode` to use actual host mode state machine. Regenerated `overlay_host_mode.png` and `overlay_conflict.png` baselines.
- **Verification:** `ctest --test-dir build-check` → 96/96 pass (1 pre-existing skip). `verify-project.sh` → pass.
- **Conformance:** OV-04 partial→verified, OV-10 partial→verified.

## Next task

Task 6: Fix documentation inaccuracies — `docs/OPERATIONS.md` inventory description (50 verified, 8 NOT_APPLICABLE, 1 DEFERRED), `README.md` §11.1 verification table (add test_installed_functional and test_installed_binary). Run `check-docs-sync.sh` and `verify-boilerplate.sh`.