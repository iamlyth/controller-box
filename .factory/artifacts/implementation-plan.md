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
Status: completed
Dependencies: none
Acceptance: `test_backend_smoke` runs (not skipped) and passes on a runner with
Verification: `ctest --test-dir build -R test_backend_smoke --output-on-failure`
Runner: gpu-compositor
Evidence: Verification passed on gpurunner. 100% tests passed.

## Task 5: Fix test_icon_map default-path install-state dependency
Title: Fix test_icon_map default-path install-state dependency
Status: completed
Dependencies: none
Acceptance: `test_icon_map` passes all 42 sub-tests deterministically from a
Verification: `ctest --test-dir build -R test_icon_map --output-on-failure`
Runner: none
Evidence: test_icon_map passes all 42 sub-tests. Fix applied.

## Task 6: Resolve orphaned study-report files to keep `git status` clean
Title: Resolve orphaned study-report files to keep `git status` clean
Status: completed
Dependencies: none
Acceptance: The repo root contains no untracked files. The two study artifacts
  currently at the repo root (`architecture-study.md`, `subsystem-ui-report.md`)
  are either relocated under `.factory/artifacts/` (per the AGENTS.md artifact
  convention), committed deliberately, or removed. `./scripts/verify.sh` completes
  and `git status --porcelain` reports a clean tree, satisfying the final audit's
  clean-tree gate. Do not silently delete valuable evidence; preserve the analysis
  in `.factory/artifacts/` by moving, not discarding.
Verification: `git status --porcelain` shows no untracked files after the change;
  `./scripts/verify.sh` passes.
Runner: none
Evidence: (unassigned) tester records clean `git status --porcelain` plus passing `./scripts/verify.sh`.

## Task 7: Create missing docs/OPERATIONS.md and fix referenced documentation
Title: Create missing docs/OPERATIONS.md and fix referenced documentation
Status: pending
Dependencies: 6
Acceptance: `docs/OPERATIONS.md` exists and is referenced as the authoritative
  operational guide. README.md (lines ~28, ~256, ~352) and REVIEW.md finding D-2
  reference it and must no longer point at a missing file. The document records:
  service architecture (overlay user service, InputPlumber system service,
  ordered-with-session, bounded restart backoff, NameOwnerChanged recovery ~2s),
  the golden-image workflow and tolerance values referenced by README, the
  systemd management/troubleshooting commands, and the actual icon naming
  convention (custom icons use the `cc-` prefix and the cache strips it before
  file lookup — correcting the D-2 discrepancy). Also address README/D-1 by
  documenting the `-h`/`--help` flag. Any `docs/OPERATIONS.md` link in README and
  REVIEW verifies (file exists, target content present, no stale path).
Verification: `test -f docs/OPERATIONS.md && grep -q 'OPERATIONS.md' README.md &&
  grep -qE '--help|-h ' README.md`; `./scripts/verify.sh` still passes.
Runner: none
Evidence: (unassigned) tester records the file present and the doc-link checks passing.

## Task 8: Fix config overlay_opacity serialization precision drift
Title: Fix config overlay_opacity serialization precision drift
Status: pending
Dependencies: 6
Acceptance: `overlay_opacity` round-trips without precision loss. The serializer
  in `src/config/config_settings.c` (`emit_settings_yaml`, currently `%.2f`) must
  preserve the parsed value (e.g. 0.855 stays 0.855, not drifting to 0.86) while
  still writing well-formed YAML. A regression test asserts the round-trip for
  non-multiples of 0.01 (e.g. 0.855, 0.333). The in-memory default and validation
  bounds (0.0..1.0) are unchanged. No behavior change for values that are exact
  multiples of 0.01.
Verification: `ctest --test-dir build -R test_settings --output-on-failure`
  including a new round-trip precision case; `./scripts/verify.sh` passes.
Runner: none
Evidence: (unassigned) tester records `test_settings` pass plus the new precision case.
  Optional: `git diff` of the serializer confirms the format change.

## Task 9: Wire identify connect-time orchestration into production connect path
Title: Wire identify connect-time orchestration into production connect path
Status: pending
Dependencies: 6
Acceptance: The controller-connect pipeline that SPEC §6.2–6.3 and §10.3 describe is
  invoked from production code, not only from tests. Verify whether
  `cbx_identity_extract`, `cbx_identity_downgrade_*`/`cbx_downgrade_resolve`,
  `cbx_assign_persist_auto_assign`, and `cbx_gamepad_order_restore` are genuinely
  unreferenced in production (the subsystem study reports they have no production
  call sites) and, if that is confirmed, wire them into the overlay/manager
  connect/on_save/recovery flow so that a reconnect (same controller, possibly a
  weaker identity) resolves its assignment via the identity ladder and gamepad
  order is restored per §10.3 gap #2. If the gap is intentional and the flow is
  instead handled positionally elsewhere, record the exact routing (file/function)
  in the plan evidence and do NOT add dead code; the acceptance is then the
  documented proof that assignment is restored on connect. No weakened assertions,
  no test-only bypass: any new wiring must be exercised by a production-path
  integration test (real init/DBus) that survives under a mock/native backend.
Verification: `ctest --test-dir build -R 'test_identity|test_assign|test_order_restore|test_overlay_native' --output-on-failure`;
  `./scripts/verify.sh` passes. A targeted note in plan evidence states whether the
  wiring was added or proven positionally-routed, with concrete file/function refs.
Runner: none
Evidence: (unassigned) tester records the targeted and full test results plus the
  routing note.

## Task 10: Final documentation and specification audit
Title: Final documentation and specification audit
Status: pending
Dependencies: 1, 2, 3, 4, 5, 6, 7, 8, 9
Acceptance: The complete active-cycle task ledger is present and every task is
Verification: ./scripts/verify.sh
Runner: none
Evidence: Passing verification-gate output; clean `git status`; conformance
