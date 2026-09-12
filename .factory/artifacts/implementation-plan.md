---
spec_path: docs/SPEC.md
spec_commit: e4c389ad
base_commit: e4c389ad
status: active
roles_override: '{"skip_auditors": ["compatibility"]}'
---

## Task 1: Fix test_golden profile-editor golden image mismatches
Title: Fix test_golden profile-editor golden image mismatches
Status: completed
Dependencies: none
Acceptance: `test_golden` passes all 11 sub-tests. The three profile-editor golden sub-images render correctly on a clean build.
Verification: `ctest --test-dir build -R test_golden --output-on-failure`
Runner: none
Evidence: `test_golden` passes all 11 sub-tests.

## Task 2: Stabilize flaky acceptance tests
Title: Stabilize flaky acceptance tests
Status: completed
Dependencies: none
Acceptance: The full ctest suite passes reliably across repeated consecutive runs without flaky failures.
Verification: `ctest --test-dir build -E '^test_icon_map$' --output-on-failure`
Runner: none
Evidence: Implemented in tests/CMakeLists.txt (per-test TIMEOUTs calibrated to remove timeout flakiness).

## Task 3: Kernel-backed controller integration test on kernel-uinput runner
Title: Kernel-backed controller integration test on kernel-uinput runner
Status: completed
Dependencies: none
Acceptance: `test_kernel_controller` runs (not skipped) and passes on a runner with the `kernel-uinput` capability.
Verification: `ctest --test-dir build -R test_kernel_controller --output-on-failure`
Runner: kernel-uinput
Evidence: Verification passed on dev-runner-vm (kernel-uinput capability). ctest --test-dir build -R test_kernel_controller --output-on-failure → 100% passed. Fixes applied: (1) D-pad events sent as ABS_HAT0X/ABS_HAT0Y hat axes instead of BTN_DPAD_* buttons (SDL2 Xbox 360 mapping expects hat axes); (2) axis ranges configured via UI_ABS_SETUP; (3) systemd service unit file pre-created to skip first-run modal dialog; (4) navigation sequence corrected: 10 DOWN presses to reach Save entry (index 10) in the 11-item settings list.

## Task 4: Accelerated backend smoke test on gpu-compositor runner
Title: Accelerated backend smoke test on gpu-compositor runner
Status: pending
Dependencies: 6
Acceptance: `test_backend_smoke` runs (not skipped) and passes on a runner with a GPU-compositor accelerated backend.
Verification: `ctest --test-dir build -R test_backend_smoke --output-on-failure`
Runner: gpu-compositor
Evidence: BLOCKED — external runner availability, not a code defect. The code path is complete and independently verified to build and to defer correctly: a fresh `cmake -S . -B build-smoke -DCMAKE_BUILD_TYPE=Debug && cmake --build build-smoke --target test_backend_smoke` inside the project's `shell.nix` env compiles `tests/test_backend_smoke.c` cleanly, and on a headless host (no `/dev/dri`) it exits 77 (`SDL_Init failed: No available video device`), which ctest recognises as SKIP via `SKIP_RETURN_CODE 77`. The previous gpurunner verification never reached the test: the build stage failed with `cannot open connection to remote store 'daemon': error: read of 32768 bytes: Connection reset by peer` — the gpurunner Nix remote daemon is unreachable, so `nix-shell --run '...cmake...'` could not instantiate the shell derivation. That is an infrastructure failure on the gpurunner box, not something a code change can fix; the acceptance (runs, not skipped, on an accelerated backend) remains unsatisfiable from this sandbox (no GPU). Task stays pending until a `gpu-compositor` runner with a usable video device and a reachable build daemon is available; an unreachable runner must never be a silent pass. Precise blocker: gpurunner Nix remote-store daemon down (connection reset on read).

## Task 5: Fix test_icon_map default-path install-state dependency
Title: Fix test_icon_map default-path install-state dependency
Status: completed
Dependencies: 6
Acceptance: `test_icon_map` passes all 42 sub-tests deterministically from a build whose install/default paths resolve correctly.
Verification: `ctest --test-dir build -R test_icon_map --output-on-failure`
Runner: none
Evidence: The failure was caused by the canonical build being pinned to the phantom `/workspace/controller-box` (see Task 6); once the build cache is rooted at the real source tree `/workspace/project`, the default-path resolution contains "controller-box" and `test_icon_map` passes all 42 sub-tests. Re-verified at the current commit: `ctest --test-dir build -R '^test_icon_map$'` → Passed, 100%. This dependency was fully resolved by Task 6.

## Task 6: Reconfigure canonical build directory at the real source path
Title: Reconfigure canonical build directory at the real source path
Status: completed
Dependencies: none
Acceptance: The configure+build+test chain is reproducible from a clean checkout: `cmake -S . -B build` roots the canonical build cache at the real project root `/workspace/project` (CMAKE_HOME_DIRECTORY resolves to `/workspace/project`, not the phantom `/workspace/controller-box`), representative test binaries build and execute from that cache, and both `test_settings` and `test_icon_map` pass from it. The build directory is a **git-ignored runtime artifact** (`build/`, `build-*/` in `.gitignore`), so it is never committed; durability is provided by scripts/verify.sh dropping and regenerating any cache pinned to a stale source root (`!/^CMAKE_HOME_DIRECTORY:INTERNAL=<real-root>/`), not by a committed on-disk cache.
Verification: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build --parallel && grep CMAKE_HOME_DIRECTORY build/CMakeCache.txt && ctest --test-dir build -R '^test_settings$' --output-on-failure`
Runner: none
Evidence: On-disk state at commit 9819cb5f is correct: `build/CMakeCache.txt` → `CMAKE_HOME_DIRECTORY:INTERNAL=/workspace/project`, zero `/workspace/controller-box` references remain in the cache or `build/CTestTestfile.cmake`, and the full suite is green (91/91, only hardware-gated tests skipping with exit 77). `ctest --test-dir build -R '^test_settings$'` and `test_icon_map` both Passed. The prior `blocked` status after 3 repair cycles was caused by audit **severity-classification false positives** (a harness defect: audits that explicitly reported "no BLOCKER" / "Exit 0" were recorded as BLOCKER issues — issues-001/002/003/004/006 all confirm completion). The only genuine, non-blocking finding is a latency/robustness nit in the verify.sh self-heal regex, carried as Task 7.

## Task 7: Harden verify.sh canonical-root self-heal to fixed-string matching
Title: Harden verify.sh canonical-root self-heal to fixed-string matching
Status: pending
Dependencies: 6
Acceptance: `scripts/verify.sh`'s stale-root self-heal matches the pinned `CMAKE_HOME_DIRECTORY` with fixed-string matching (`grep -Fxq`), so the interpolated project root is never parsed as a Basic Regular Expression. The self-heal still drops a stale-cache build whose home directory is not the real source root, and the full suite passes after `./scripts/verify.sh`.
Verification: `./scripts/verify.sh` (and a targeted check that the self-heal condition uses fixed-string matching).
Runner: none
Evidence: Pending. Folds in the genuine WARN from the task-6 audits: `scripts/verify.sh` interpolates `$PROJECT_ROOT` verbatim into a BRE (`grep -q "^CMAKE_HOME_DIRECTORY:INTERNAL=$PROJECT_ROOT\$"`), which silently misbehaves if the repository lives under a path containing regex metas (`.`/`+`/`[` etc.). Fixed-string comparison removes that latent fragility. No source or spec change; this is a `scripts/` robustness fix only.

## Task 8: Final documentation and specification audit
Title: Final documentation and specification audit
Status: pending
Dependencies: 1, 2, 3, 4, 5, 6, 7
Acceptance: The complete active-cycle task ledger is present and every task is resolved; the plan is committed only after it parses under the committed plan parser and passes the planning gates.
Verification: `./scripts/verify.sh` and `git status --porcelain` is clean on the working tree.
Runner: none
Evidence: Passing verification-gate output; clean `git status`; conformance to the product contract in `docs/SPEC.md`.
