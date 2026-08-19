# Task 2 Complete: Controller-Transport Evidence for M28–M38

## Outcome
- Added 3 new `ctrl_press`-based tests to `test_manager_native_prof.c`:
  - `test_m28_binding_nav_ctrl`: D-pad Down/Up via real gamepad transport; asserts selected index AND diagram highlight change (A→B→Up→B)
  - `test_m29_binding_edit_ctrl`: A on binding via ctrl_press → BINDING_EDIT mode, target_list visible with 3 options
  - `test_m37_save_ctrl`: B (button 1) via ctrl_press in LIST → save to disk, editor closes, file exists
- Existing ctrl_press tests for M30, M31, M33, M38 already present and retained
- Relabeled all `_controller` → `_keyboard` in `test_manager_interaction_prof.c`; updated header comment with §5.7 supplemental annotation
- Updated `interaction_inventory.c` dispatch_path fields for M28–M31, M33, M37–M38 to reference `test_manager_native_prof.c` (ctrl_press) as primary controller-transport evidence
- Updated implementation plan: MGR-08 → verified, DOD-03 → verified, inventory status 52/60, Task 2 → complete

## Verification
- `ctest --test-dir build-check`: 98/98 (96 pass, 2 skip — test_kernel_controller, test_backend_smoke)
- `test_manager_native_prof`: PASS (includes new M28/M29/M37 tests)
- `test_manager_interaction_prof`: PASS (relabeled functions)
- `test_interaction_inventory`: PASS (updated dispatch_path fields)
- `check-docs-sync.sh`: PASS
- No `_controller` function names remain in `test_manager_interaction_prof.c`

## Commit
- `4b5f15b`: Task 2: Add controller-transport evidence for profile editor (M28–M38)

## Next Task
- Task 3: Attempt aarch64 cross-compile and document hardware-deferred capabilities
- Task 4: Final documentation and specification audit (depends on Tasks 1–3)