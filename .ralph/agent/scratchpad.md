# Task 1 Complete: Interaction Inventory M39 + Stale Documentation Fix

## Outcome
- Added M39 (first-run service install) to `interaction_inventory.c` with correct category, widget type, both paths, and VERIFIED status
- Updated `test_interaction_inventory.c`: manager count 38→39, loop range, verified_ids, dialog-paths arrays, specific-entry and scenario assertions
- Fixed stale docs: README.md (kernel-uinput capability IS declared, inventory counts 52/60 verified, 7 NA, 1 deferred), OPERATIONS.md (runner reachable, /dev/uinput provisioned, correct counts), CMakeLists.txt comments

## Verification
- `ctest --test-dir build-check`: 98/98 (96 pass, 2 skip — test_kernel_controller, test_backend_smoke)
- `test_interaction_inventory`: PASS
- `check-docs-sync.sh`: PASS
- No stale `M01–M38`, `52/59`, `6 NOT_APPLICABLE`, or `no kernel-uinput` references remain

## Commit
- `769b42e`: Task 1: Complete interaction inventory (M39) and fix stale documentation

## Next Task
- Task 2: Add controller-transport evidence for profile editor interactions (M28–M38 via ctrl_press)
- Task 3: Attempt aarch64 cross-compile and document hardware-deferred capabilities
- Task 4: Final documentation and specification audit (depends on Tasks 1–3)