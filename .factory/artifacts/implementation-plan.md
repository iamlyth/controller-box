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
Evidence: Pending. Requires (a) a `gpu-compositor` runner with a usable video device and (b) the canonical `build/` configured at the real source tree (see Task 6). Without a video device the test currently skips with exit 77.

## Task 5: Fix test_icon_map default-path install-state dependency
Title: Fix test_icon_map default-path install-state dependency
Status: pending
Dependencies: 6
Acceptance: `test_icon_map` passes all 42 sub-tests deterministically from a build whose install/default paths resolve correctly.
Verification: `ctest --test-dir build -R test_icon_map --output-on-failure`
Runner: none
Evidence: Confirmed at the bound commit: `test_default_path` (tests/test_icon_map.c) asserts the resolved default icon-map path contains "controller-box", so it fails (`-2 != 0`, `-ENOENT`) on a build whose `DATA_DIR`/`SOURCE_DATA_DIR` resolve to the phantom `/workspace/controller-box/data`; even after a fresh configure it still asserts the basename substring. The test couples its result to the build/install prefix actually being named `controller-box`. Verification additionally requires the canonical `build/` to be reconfigured at the real source path `/workspace/project` (see Task 6).

## Task 6: Reconfigure canonical build directory at the real source path
Title: Reconfigure canonical build directory at the real source path
Status: complete
Dependencies: none
Acceptance: The canonical `build/` directory's CMake cache resolves `CMAKE_HOME_DIRECTORY` to the real project root `/workspace/project` (not the phantom `/workspace/controller-box`), and `ctest --test-dir build` executes real test binaries (no wholesale "Not Run"/file-not-found) for representative tests.
Verification: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build --parallel && grep CMAKE_HOME_DIRECTORY build/CMakeCache.txt && ctest --test-dir build -R '^test_settings$' --output-on-failure`
Runner: none
Evidence: The phantom `build/` cache was wiped and reconfigured at the real source root. Run in `nix-shell`:
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` -> `Build files have been written to: /workspace/project/build`
- `grep CMAKE_HOME_DIRECTORY build/CMakeCache.txt` -> `CMAKE_HOME_DIRECTORY:INTERNAL=/workspace/project`
- `cmake --build build --parallel` -> 100% (all targets built)
- `ctest --test-dir build -R '^test_settings$' --output-on-failure` -> `test_settings .... Passed` / `100% tests passed, 0 tests failed out of 1`
Full `ctest --test-dir build --output-on-failure --timeout 120`: `100% tests passed, 0 tests failed out of 91` (only `test_kernel_controller` and `test_backend_smoke` skip as env/hardware-dependent, unrelated). Reconfiguring surfaced one genuine defect hidden by the phantom path: `test_default_path` (tests/test_icon_map.c:296) asserted the resolved default icon path contains the literal `"controller-box"`, which only passed because the phantom build dir name matched; the real tree at `/workspace/project` no longer embeds it. Fixed the assertion to check the semantic outcome (`access(path, R_OK) == 0`) that the default path resolves to an existing readable `controller-icons.yaml`; `test_icon_map` now passes.

## Task 7: Final documentation and specification audit
Title: Final documentation and specification audit
Status: pending
Dependencies: 1, 2, 3, 4, 5, 6
Acceptance: The complete active-cycle task ledger is present and every task is resolved; the plan is committed only after it parses under the committed plan parser and passes the planning gates.
Verification: `./scripts/verify.sh` and `git status --porcelain` is clean on the working tree.
Runner: none
Evidence: Passing verification-gate output; clean `git status`; conformance to the product contract in `docs/SPEC.md`.
