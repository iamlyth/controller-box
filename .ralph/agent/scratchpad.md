# Implementation Loop — Sanitizer Build Gate Verification

## Current State
- Task 1 (SupportedTargetDevices) and Task 2 (daemon footprint test) are complete.
- Runtime task `task-1786855836-de0e` (Add sanitizer build gate) is `in_progress`.
- Sanitizer code already committed in `b4d1a0d`: CMakeLists.txt `CBX_ENABLE_SANITIZERS` option + `scripts/verify-sanitizers.sh`.
- The sanitizer script has NOT been verified to run successfully yet.
- Plan DOD-05 still says "No sanitizer gates in suite" — needs updating.
- No remediation task appended to the plan for the sanitizer gate (operating-model step 9 requires this).
- Tasks 3–8 require undeclared runner capabilities (physical-controller, kernel-uinput, gpu-compositor, target-consumer, installed-package) — blocked.
- Task 9 (final audit) depends on Tasks 1–8.

## Work for This Iteration
1. Run `nix-shell --run './scripts/verify-sanitizers.sh'` to verify the sanitizer build gate works.
2. Fix any ASan/UBSan defects found during the run.
3. Append remediation Task 10 to `.factory/artifacts/implementation-plan.md` for the sanitizer gate.
4. Update DOD-05 evidence in conformance matrix: sanitizer gate now exists (GPU smoke still pending on Task 6).
5. Mark runtime task `task-1786855836-de0e` complete with verification evidence.
6. Commit to `develop`.

## Verification
- `nix-shell --run './scripts/verify-sanitizers.sh'` must pass (exit 0, no ASan/UBSan errors).

## Next Task
- After sanitizer gate is verified: Tasks 3–8 are all blocked on undeclared runner capabilities. Task 9 (final audit) depends on all. The loop cannot complete without runner hardware. Document this as a blocked state.