---
spec_path: docs/SPEC.md
spec_commit: e4c389ad
base_commit: e4c389ad
status: active
---

## Task 1: Fix test_golden profile-editor golden image mismatches
Title: Fix test_golden profile-editor golden image mismatches
Status: completed
Dependencies: none
Acceptance: `test_golden` passes all 11 sub-tests. The three profile-editor
Verification: `ctest --test-dir build -R test_golden --output-on-failure`
Runner: none
Evidence: `test_golden` passes all 11 sub-tests.

## Task 2: Stabilize flaky acceptance tests
Title: Stabilize flaky acceptance tests
Status: completed
Dependencies: none
Acceptance: The full ctest suite passes reliably across repeated consecutive
Verification: `ctest --test-dir build -E '^test_icon_map$' --output-on-failure
Runner: none
Evidence: Implemented in tests/CMakeLists.txt (per-test TIMEOUTs calibrated to

## Task 3: Kernel-backed controller integration test on kernel-uinput runner
Title: Kernel-backed controller integration test on kernel-uinput runner
Status: completed
Dependencies: none
Acceptance: `test_kernel_controller` runs (not skipped) and passes on a runner
Verification: `ctest --test-dir build -R test_kernel_controller --output-on-failure`
Runner: kernel-uinput
Evidence: Verification passed on dev-runner-vm (kernel-uinput capability). ctest --test-dir build -R test_kernel_controller --output-on-failure → 100% passed. Fixes applied: (1) D-pad events sent as ABS_HAT0X/ABS_HAT0Y hat axes instead of BTN_DPAD_* buttons (SDL2 Xbox 360 mapping expects hat axes); (2) axis ranges configured via UI_ABS_SETUP; (3) systemd service unit file pre-created to skip first-run modal dialog; (4) navigation sequence corrected: 10 DOWN presses to reach Save entry (index 10) in the 11-item settings list.

## Task 4: Accelerated backend smoke test on gpu-compositor runner
Title: Accelerated backend smoke test on gpu-compositor runner
Status: pending
Dependencies: none
Acceptance: `test_backend_smoke` runs (not skipped) and passes on a runner with
Verification: `ctest --test-dir build -R test_backend_smoke --output-on-failure`
Runner: gpu-compositor
Evidence: Passing `test_backend_smoke` ctest output on the `gpu-compositor`

## Task 5: Fix test_icon_map default-path install-state dependency
Title: Fix test_icon_map default-path install-state dependency
Status: pending
Dependencies: none
Acceptance: `test_icon_map` passes all 42 sub-tests deterministically from a
Verification: `ctest --test-dir build -R test_icon_map --output-on-failure`
Runner: none
Evidence: Pending. Current state at the bound commit: `test_default_path`

## Task 6: Final documentation and specification audit
Title: Final documentation and specification audit
Status: pending
Dependencies: 1, 2, 3, 4, 5
Acceptance: The complete active-cycle task ledger is present and every task is
Verification: `./scripts/verify.sh` and `git status --porcelain` is clean on
Runner: none
Evidence: Passing verification-gate output; clean `git status`; conformance

