# Handoff: BUG-0018 device-mapped diagram resolution implemented (task-1787452487-2cb0)

## Outcome this iteration
Implemented runtime task `task-1787452487-2cb0` (Fix BUG-0018, device-mapped redirect).
Replaced the profile editor's hardcoded `cbx_icon_dir()/svg/generic-gamepad.svg` path
build with production icon-utility resolution and added device-mapped marker-layout
infrastructure. Tree was clean @ d5848c7 on develop before this work.

## What changed
- `src/manager/profile_editor_list.c/.h`: diagram base + marker layout now resolved via
  `cbx_icon_map` + `cbx_icon_cache` + `cbx_icon_lookup` keyed by device type
  (`cbx_profile_editor_set_device`), with a geometry guard so unregistered devices keep
  the generic-gamepad asset whose controls match the marker table. Editor owns the icon
  cache (512 raster) + map; shutdown cleans the cache.
- `src/manager/profile_diagram.c/.h`: `cbx_profile_diagram_set_base_image` (borrowed
  cache texture), `set_device` (active layout from device-layout registry),
  `device_geometry_known`, `active_button_pos`; renderer anchors markers through the
  active table. Aspect/512-raster/content-box anchoring (prior BUG-0018 machinery)
  unchanged.
- Tests: `tests/test_profile_diagram.c` device-mapped resolution/pixelation/stretch/
  marker-alignment/lookup tests; `tests/test_editor_list_mode.c` production-cache
  resolution + set_device re-resolve. Real installed `test_installed_diagram.sh` passes.
- Plan: appended "BUG-0018 device-mapped diagram resolution" section (validates clean).

## Verification
- Full `ctest --test-dir build-check` 99/100 pass; only failure is pre-existing
  `test_golden` three-editor-baseline mismatch (goldens protected, out-of-band regen, NOT
  regenerated). 2 hardware skips. `test_manager_visual`, `test_installed_diagram/smoke/
  binary` pass. `./scripts/verify-boilerplate.sh` exit 0. Build clean under Debug -Werror.
- `python3 scripts/validate-implementation-plan.py planning ...` exit 0; plan fresh.

## Constraints honoured
No golden regenerated/closed, no conformance evidence tier elevated, BUG-0015/BUG-0018
left open. Per-device marker calibration for licensed Controllercons SVGs (control
geometry not machine-verifiable) stays out-of-band (visual/human). Out-of-band human
review required before any acceptance claim.

## Next action
Commit this checkpoint to develop. Do NOT emit the completion token: Task 4 final audit,
FACT-002..007, and golden re-approval remain open. Later iterations should await the
human/out-of-band device-calibration and acceptance review; no further software work is
queued on this task.
