# Task 8: Apply assignments and profiles to live InputPlumber — COMPLETE

## Outcome
- All acceptance criteria met. Commit `259b280` + plan commit `5d4a2e4` on `develop`.

## Verification
- `ctest` — 81/81 pass (1 skip = backend_smoke, needs GPU).
- `test_native_assignment_application` — real sd-bus: create 2 targets, attach to composite, LoadProfilePath, verify ProfilePath + ProfileName match, SetGamepadOrder + read back, simulate restart (clear order), restore + verify, PersistentId stability, second profile load + verify. 11 steps.
- `test_profile_change_load_failure` — mock: LoadProfilePath error → assignment NOT updated.
- `test_profile_change_verify_mismatch` — mock: ProfilePath read-back mismatch → assignment NOT updated.
- `test_composite_calls` — ProfileName/ProfilePath mock tests (success + null args).

## Changes
1. **`cbx_overlay_on_save`** (`overlay_service.c`): Refactored from sync-first to apply-to-engine-first. Phase 1: LoadProfilePath + ProfilePath verification + AttachTargetDevice per row. Phase 2: SetGamepadOrder. Phase 3: sync grid→assignments + save to disk. In-memory and disk only updated after all engine state verified.
2. **`cbx_profile_cycle_apply`** (`profile_cycle.c`): Added ProfilePath read-back verification after LoadProfilePath. Returns -EIO if engine ProfilePath doesn't match requested path. Assignment update only happens after verification succeeds.
3. **`overlay_backend_ready`** (`overlay_service.c`): After rebuild, re-initialises profile_cycle with current backend/bus and calls `cbx_overlay_on_save` to restore profiles, attachments, and GamepadOrder after InputPlumber restart.
4. **`ip_composite_get_profile_name` / `ip_composite_get_profile_path`** (`ip_composite.c/.h`): New wrappers for ProfileName/ProfilePath properties.
5. **Native test server** (`test_native_dbus.c`): GamepadOrder writable property (get/set) on Manager, LoadProfilePath method on CompositeDevice, ProfileName/ProfilePath/PersistentId property getters on CompositeDevice.
6. **Mock tests**: ProfileName/ProfilePath tests in test_composite_calls, LoadProfile failure + ProfilePath mismatch tests in test_overlay_integration, ProfilePath mock expectations added to test_profile_cycle, device model + AttachTargetDevice/GamepadOrder expectations in test_overlay_interaction fixture.

## Key design decisions
- Apply-to-engine-first: in-memory assignments and disk persistence happen only after every LoadProfilePath, AttachTargetDevice, and SetGamepadOrder succeeds. LoadProfile failures do not appear saved.
- Profile application is skipped when no profiles list is available (degraded/test mode); slot assignment and GamepadOrder still apply.
- ProfilePath verification ensures the engine actually loaded the requested profile, not just that LoadProfilePath returned 0.

## Next Task
Task 9 (Visible reusable overlay and runtime reconciliation) — depends on Tasks 1, 2, 8 (all complete).