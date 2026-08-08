# Task 5: Authoritative startup target topology and routability — COMPLETE

## Outcome
- All acceptance criteria met. Commit `1975aef` on `develop`.

## Verification
- `ctest` — 81/81 pass (1 skip = backend_smoke, needs GPU).
- `test_native_topology_reconciliation` — real sd-bus: create ordered topology (3 targets: xb360/ds5/gamepad), verify DeviceType per slot, attach to CompositeDevice0, verify routability via TargetDevices property, remove one slot preserving others, type correction via reverse-order stop+create, re-attach after correction.
- `test_reconcile_grow_and_attach` — mock: grow from 0→1, verify attach.
- `test_reconcile_create_fails` — mock: CreateTargetDevice fails, returns error.
- `test_reconcile_enumerate_fails` — mock: create succeeds but ObjectManager doesn't confirm → rollback stops created target.
- `test_reconcile_shrink` — mock: shrink from 2→1, verify remaining target.

## Changes
1. **`cbx_reconcile_startup_targets`** (`overlay_service.c`): Made non-static, exposed in header. Enhanced from 2-phase (grow/shrink) to 4-phase (grow, shrink, per-slot type correction via reverse-order stop+create, attach targets to composites for routability). Rollback on any failure stops targets created during this reconcile that were not in the original set. Every create/stop confirmed via ObjectManager re-enumeration.
2. **`overlay_service.h`**: Added `cbx_reconcile_startup_targets` declaration with documentation of all 4 phases.
3. **Native test server** (`test_native_dbus.c`): Added `AttachTargetDevice` method handler (validates target exists, records target→composite attachment), `TargetDevices` property on CompositeDevice (returns all attached target paths as `as`), direct object vtable for CompositeDevice0.
4. **`test_native_topology_reconciliation`**: 4 scenarios — clean startup with ordered topology + routability verification, remove preserving others, type correction via reverse-order stop+create, re-attach after correction.
5. **Mock tests** (`test_overlay_service.c`): 4 tests covering grow+attach, create failure, enumerate failure with rollback, and shrink paths.

## Key design decisions
- Type correction uses reverse-order stop+create to preserve array ordering: stopping the last target causes no shift, and the new target is appended to the same position.
- Rollback only stops targets NOT in the original set (preserving the last confirmed topology); targets stopped for type correction that can't be recreated are accepted as topology reduction.
- Attach phase maps target[i] → composite[i] for routability, matching the slot model used by `cbx_overlay_on_save`.

## Next Task
Task 8 (Apply assignments and profiles to live InputPlumber) — depends on Tasks 1 (complete), 5 (now complete), 6 (complete).