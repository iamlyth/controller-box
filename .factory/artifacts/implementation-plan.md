---
spec_path: docs/SPEC.md
spec_commit: HEAD
base_commit: HEAD
status: active
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

## Task 6: Update README to reflect plan-once architecture
Title: Update README to reflect plan-once architecture
Status: completed
Dependencies: none
Acceptance: README.md accurately describes the campaign as planning once then looping implementation. No mention of per-round planning.
Verification: grep -q "plan once" README.md && grep -q "implementation loop" README.md
Runner: none
Evidence: verification exit 0 on local

## Task 7: Update FACTORY-LOOP-SPEC requirement registry
Title: Update FACTORY-LOOP-SPEC requirement registry
Status: completed
Dependencies: none
Acceptance: The requirement registry in §16 of FACTORY-LOOP-SPEC.md has no entries referencing per-round planning or roles_override per round.
Verification: ! grep -q "per round" docs/FACTORY-LOOP-SPEC.md
Runner: none
Evidence: verification exit 0 on local

## Task 8: Verify all harness modules compile
Title: Verify all harness modules compile
Status: completed
Dependencies: none
Acceptance: All Python modules in .factory/loop/ compile without errors.
Verification: python3 -c "import py_compile; [py_compile.compile(f'.factory/loop/{m}', doraise=True) for m in ['campaign.py','parallel.py','state.py','selector.py','plan_parser.py','metrics.py','issues.py','runner.py','preflight.py','gitutil.py','lock.py','__init__.py']]"
Runner: none
Evidence: verification exit 0 on local

## Task 9: Verify full test suite passes
Title: Verify full test suite passes
Status: pending
Dependencies: none
Acceptance: Full ctest suite passes with 0 failures (hardware tests may skip with exit 77).
Verification: ./scripts/verify.sh
Runner: none
Evidence: Pending.

## Task 10: Final documentation and specification audit
Title: Final documentation and specification audit
Status: pending
Dependencies: 1, 2, 3, 4, 5, 6, 7, 8, 9
Acceptance: The full verification suite passes and the Git tree is clean on develop. Additionally, the audit must close out the two concrete spec areas surfaced by the subsystem studies — evaluating each to a conclusion rather than passing silently:
Verification: ./scripts/verify.sh && git status --porcelain
Runner: none
Evidence: Pending. Auditor records the §10.3/#§6 dispositions (wired/superseded/removed) with exact line/reference facts, plus the verification exit code and clean-tree confirmation.

