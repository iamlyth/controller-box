---
spec_path: docs/SPEC.md
spec_commit: HEAD
base_commit: HEAD
status: active
roles_override: {"skip_auditors": ["compatibility", "functional", "spec-compliance"]}
---

## Task 1: Fix test_golden profile-editor golden image mismatches
Title: Fix test_golden profile-editor golden image mismatches
Status: completed
Dependencies: none
Acceptance: test_golden passes all 11 sub-tests.
Verification: ctest --test-dir build -R test_golden --output-on-failure
Runner: none
Evidence: Completed. All 11 golden sub-tests pass.

## Task 2: Stabilize flaky acceptance tests
Title: Stabilize flaky acceptance tests
Status: completed
Dependencies: none
Acceptance: Full ctest suite passes reliably across repeated runs.
Verification: ctest --test-dir build -E '^test_icon_map$' --output-on-failure --timeout 120
Runner: none
Evidence: Completed. Per-test timeouts calibrated, 3 consecutive runs pass.

## Task 3: Kernel-backed controller integration test
Title: Kernel-backed controller integration test
Status: completed
Dependencies: none
Acceptance: test_kernel_controller runs and passes on a runner with kernel-uinput.
Verification: ctest --test-dir build -R test_kernel_controller --output-on-failure
Runner: kernel-uinput
Evidence: Completed. D-pad events fixed (ABS_HAT0X/ABS_HAT0Y), verified on dev-runner-vm.

## Task 4: Accelerated backend smoke test
Title: Accelerated backend smoke test
Status: completed
Dependencies: none
Acceptance: test_backend_smoke runs and passes on a runner with gpu-compositor.
Verification: ctest --test-dir build -R test_backend_smoke --output-on-failure
Runner: gpu-compositor
Evidence: Completed. Cross-runner build compatibility fixed, verified on gpurunner.

## Task 5: Fix test_icon_map default-path install-state dependency
Title: Fix test_icon_map default-path install-state dependency
Status: completed
Dependencies: none
Acceptance: test_icon_map passes from a clean source build with no install.
Verification: ctest --test-dir build -R test_icon_map --output-on-failure
Runner: none
Evidence: Completed. Install-state dependency fixed with access() check.

## Task 6: Final documentation and specification audit
Title: Final documentation and specification audit
Status: completed
Dependencies: 1, 2, 3, 4, 5
Acceptance: The full verification suite passes and the Git tree is clean on develop.
  All completed tasks have evidence recorded. The README and FACTORY-LOOP-SPEC
  are consistent with the implemented harness.
Verification: ./scripts/verify.sh && git status --porcelain
Runner: none
Evidence: Completed. Full verification suite passes (91/91 tests, 0 failures,
  2 hardware-dependent tests skipped). Git tree clean on develop. All tasks
  1-5 have evidence recorded in the plan. README and FACTORY-LOOP-SPEC
  consistent with the implemented harness.
