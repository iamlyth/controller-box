# Implementation Loop — Stale Build Cache Fixed (Iteration 47)

## Outcome
- Discovered stale `build-maintenance-verify` directory with `CMAKE_HOME_DIRECTORY:INTERNAL=/workspace/controller-box` (old workspace path). `rm -rf` from outside nix-shell failed silently; deleting from within nix-shell (`nix-shell --run 'rm -rf build-maintenance-verify'`) and rebuilding fixed 10 test failures (icon, golden, profile, installed tests).
- **Final gate now PASSES**: `final-gate: implementation, specification, tests, and documentation accepted`
- 98/98 tests pass (0 failed, 2 hardware skips: test_kernel_controller, test_backend_smoke)
- Hardware tasks 3–9 remain blocked (no env change since iter 28)
- Completion token NOT emitted — non-final tasks 3–9 remain (blocked/pending)

## Verification (this iteration)
- check-plan-freshness.sh: OK
- check-scratchpad.sh: OK
- validate-implementation-plan.py complete: OK (status: blocked accepted)
- bug-ledger.py validate: OK (0 open, 6 closed)
- check-docs-sync.sh: OK
- verify-boilerplate.sh: OK
- verify-project.sh: PASS (98/98, 0 failures, 2 skips, packaging, installed-smoke)
- check-installed-functional-evidence.sh: PASS at 11a46e6, zero skips
- final-gate.sh --implementation: PASS — accepted
- Git tree: clean on develop at 11a46e6

## Root Cause of Prior False Positives
- Previous iterations reported "98/98 pass" based on the `build` directory (correct path /workspace/project)
- The `build-maintenance-verify` directory used by verify-project.sh had stale CBX_SOURCE_DIR=/workspace/controller-box
- Tests compiled with stale path couldn't find data/icons/golden/profiles → 10 failures
- Fix: `nix-shell --run 'rm -rf build-maintenance-verify && cmake -S . -B build-maintenance-verify && cmake --build build-maintenance-verify'`

## Environment State (unchanged since iter 28)
- No /dev/uinput, no /dev/dri, no /dev/input — zero capabilities (CapEff=0, CapBnd=0)
- mknod /dev/uinput: Operation not permitted
- SSH runner dev-runner-vm: hostname unresolvable, no ~/.ssh/factory-ssh symlink

## Recovery Handoff (when hardware available)
1. Expose /dev/uinput → Task 3
2. Run test_kernel_controller.c (exit 0) → Task 4
3. Run test_installed_functional.c with kernel controller → Task 5
4. Expose /dev/dri/ for GPU → Task 6
5. Provision Pi-4 hardware → Task 7 (perf) → Task 8 (human sign-off)
6. Run final audit gate → Task 9
7. Then emit the completion token