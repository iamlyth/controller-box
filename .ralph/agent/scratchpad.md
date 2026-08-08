# Task 9: Visible reusable overlay and runtime reconciliation — COMPLETE

## Outcome
- All acceptance criteria met. Commit `5b5696e` on `develop`.

## Verification
- `ctest` — 82/82 pass (1 skip = backend_smoke, needs GPU).
- `test_overlay_reconcile` — 7 tests:
  - `test_per_composite_activation_close_sets_pass_on_activating` — comp 1 triggers activation, lifecycle.composite_path updates to comp 1, close sets PASS on comp 1
  - `test_poll_rearm_after_close` — activate→close→deactivation+rearm in one step→re-activation works
  - `test_hotplug_target_add_rebuilds_columns` — 2→3 targets, grid 3→4 columns
  - `test_hotplug_target_remove_clamps_positions` — 2→1 target, grid 3→2, row at col 2 clamped to Unassigned
  - `test_window_visibility_tracks_lifecycle` — IDLE→activate→VISIBLE→close→IDLE
  - `test_surface_reuse_across_cycles` — surface stays built across 2 open/close cycles
  - `test_primary_composite_activation_close_sets_pass_on_comp0` — comp 0 activation, composite_path stays comp 0

## Changes
1. **Per-activating-composite lifecycle** (`overlay_service.c/h`): `cbx_poll_activation_ctx` struct per poll holds lifecycle pointer + composite path. `on_intercept_activating` updates `lifecycle.composite_path` before activating. Close sets PASS on the activating composite, not the primary.
2. **Poll re-arm** (`overlay_service.c`): Step function re-arms IDLE polls when lifecycle is IDLE and backend ready. After close, poll detects PASS→IDLE, then immediately re-arms to PASS_WAIT in the same step.
3. **Hotplug integration** (`overlay_service.c/h`, `ip_hotplug.c/h`): `ip_hotplug` wired into overlay service. `model_changed` flag set when hotplug handler modifies device model. Step function checks flag and calls `cbx_overlay_reconcile_hotplug` which rebuilds grid (dynamic columns), input map, triggers, and polls.
4. **`cbx_overlay_rearm_polls`** (`overlay_service.c/h`): Factored out poll initialization for reuse in startup, backend recovery, and hotplug paths.
5. **`ip_hotplug` model_changed** (`ip_hotplug.c/h`): `bool model_changed` field, set after successful `cbx_device_model_add_*/remove_*` calls.

## Key design decisions
- Re-arm only when lifecycle is IDLE and backend is ready — prevents re-arming during close/fade-out.
- Deactivation + re-arm can happen in one step (ACTIVE→IDLE→PASS_WAIT) — minimal latency.
- Hotplug reconcile uses device model as-is (already updated by ip_hotplug) without full re-enumeration — avoids redundant GetManagedObjects call.
- Mock DBus deduplicates by (iface, member) — tests set expectations in stages between activation and close steps.

## Next Task
Task 10 (Mandatory installed functional acceptance gate) — depends on Tasks 3, 5, 7, 9 (all complete).