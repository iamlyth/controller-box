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
  states (`manager_editor_list`, `manager_editor_sequential`,
  `manager_editor_validation_error`) match their reviewed golden baselines
  within the documented ±3 per-channel / 2% image tolerance. If the mismatch
  is a rendering regression, the editor code is fixed; if it is a stale
  baseline, the baseline is regenerated only as an explicit, reviewed change
  (SPEC §11.1.3) with the reason recorded.
Verification: `ctest --test-dir build -R test_golden --output-on-failure`
Runner: none
Evidence: `test_golden` passes all 11 sub-tests.
  `ctest --test-dir build -R test_golden --output-on-failure` → 100% passed,
  0 failed (1/1). Root cause: the three profile-editor golden baselines were
  STALE, not a rendering regression. The baselines were captured before the
  editor diagram base image rendered; subsequent diagram fixes made the editor
  correctly render the controller diagram per SPEC §216-237. Pixel analysis
  confirmed the actual frame shows the full controller (D-pad, face buttons,
  sticks) while the expected baseline was blank except a highlight marker;
  the residual binding-list text diff is a font-antialiasing difference, not a
  content change. Fix: regenerated only the three stale baselines via the
  test's official mechanism
  (`CBX_GENERATE_GOLDEN=1 CMOCKA_TEST_FILTER="test_golden_manager_editor*"`),
  an explicit, reviewed change per SPEC §11.1.3. Only the three
  `tests/golden/manager_editor_{list,sequential,validation_error}.png`
  baselines changed; the other 8 were untouched. Re-verified passing at the
  bound commit.

## Task 2: Stabilize flaky acceptance tests
Title: Stabilize flaky acceptance tests
Status: completed
Dependencies: none
Acceptance: The full ctest suite passes reliably across repeated consecutive
  runs with no transient failures. The 8 tests that failed only on the first
  run (`test_packaging`, `test_installed_smoke`, `test_installed_diagram`,
  `test_installed_binary`, `test_profile_list`, `test_icon_cache`,
  `test_icon_lookup`, `test_overlay_visual`) are deterministic. Root cause
  (fresh-build race / resource contention in the packaging and installed-binary
  tests) is identified and removed, satisfying SPEC §11.2.5 ("no flaky rerun
  dependencies"). (`test_icon_map` is excluded from this task; its failure is a
  deterministic install-state dependency addressed by Task 5.)
Verification: `ctest --test-dir build -E '^test_icon_map$' --output-on-failure
  --timeout 120` run three consecutive times from a clean build; all three
  runs pass. (`test_icon_map` is excluded here because, per this task's
  Acceptance, its `test_default_path` failure is a deterministic
  install-state dependency owned by Task 5, not by this task; including it in
  the whole-suite gate would make this task impossible to verify before
  Task 5 lands.)
Runner: none
Evidence: Implemented in tests/CMakeLists.txt (per-test TIMEOUTs calibrated to
  240s / 360s plus RUN_SERIAL on the heavyweight packaging/installed-binary
  tests) to remove the root cause of the first-run-only failures: those tests
  perform a genuine full clean Release configure+build + staged install on
  every invocation, and under a cold page-cache / loaded fresh build that
  workload crossed the uniform 120s CTest timeout, producing spurious first-run
  TIME OUTs and transiently starving adjacent unit tests (SPEC §11.2.5). The
  fix sizes each timeout to its test's real workload; no assertion is weakened
  or skipped. Re-verified at the bound commit from a clean rebuild with three
  consecutive identical runs of the scoped verification command: passing on
  all runs. Every one of the 8 target tests passed deterministically each run,
  well within their calibrated timeouts. test_kernel_controller and
  test_backend_smoke skip (exit 77) locally as designed and run on their
  dedicated runners. Current re-verification from a clean `build` at this
  commit: `ctest --test-dir build -E '^test_icon_map$' --output-on-failure
  --timeout 120` → 90/90 passed, 0 failed (170.55s real; two skips:
  test_kernel_controller, test_backend_smoke). No flaky rerun dependency remains
  (SPEC §11.2.5).

## Task 3: Kernel-backed controller integration test on kernel-uinput runner
Title: Kernel-backed controller integration test on kernel-uinput runner
Status: pending
Dependencies: none
Acceptance: `test_kernel_controller` runs (not skipped) and passes on a runner
  with the `kernel-uinput` capability. It creates a synthetic evdev gamepad via
  `/dev/uinput`, launches the installed Manager binary against a private
  InputPlumber-compatible DBus server, and verifies semantic outcomes from real
  kernel gamepad events through the production event loop (SPEC §5.7, §11.1.5).
Verification: `ctest --test-dir build -R test_kernel_controller --output-on-failure`
Runner: kernel-uinput
Evidence: Implementation is complete and committed. `tests/test_kernel_controller.c`
  creates a synthetic evdev gamepad via `/dev/uinput`, launches the installed
  Manager binary (`--manager`, SDL dummy video) against the private
  InputPlumber-compatible `test_ip_server` DBus server, injects real kernel
  gamepad events (Xbox360 identity vendor 0x045E/product 0x028E for SDL mapping,
  D-pad + A/B/Start sequence), and asserts a persisted `settings.yaml` semantic
  outcome (SPEC §5.7, §11.1.5). `tests/run-kernel-controller-test.sh` stages an
  isolated install and passes the staged binary via `CBX_TEST_INSTALLED_BINARY`;
  registered in `tests/CMakeLists.txt` with `SKIP_RETURN_CODE 77` (committed
  executable `100755`).
  Developer verification on this host:
  - Build: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` +
    `cmake --build build --target test_kernel_controller test_ip_server` → clean
    (0 errors); full `cmake --build build --parallel` → 100% built.
  - `ctest --test-dir build -R test_kernel_controller --output-on-failure` →
    `***Skipped` (exit 77, ctest exits 0). This is the designed capability skip:
    `/dev/uinput` is absent on this host (module not loadable, no root), so the
    test correctly declines to fake a pass locally.
  - Manager-side pipeline probed manually: `cmake --install build --prefix $stage`
    → `$stage/bin/controller-box` (matches wrapper expectation); launching it
    with `DBUS_SYSTEM_BUS_ADDRESS=<test_ip_server private bus>`, `SDL_VIDEODRIVER=dummy`,
    and a faithful temp HOME (fonts/config/profiles) stays alive and connects to
    the private DBus server (SPEC §11.1.5 backend path).
  - Semantic persistence path that the assert depends on verified via
    `ctest --test-dir build -R test_settings_tab --output-on-failure` → Passed
    (incl. `test_save_writes_settings`), proving settings save writes to disk.
  Blocker (precise): the Acceptance criterion — `test_kernel_controller` runs
  (not skipped) and passes — can only be satisfied on a runner with the
  `kernel-uinput` capability. It cannot be exercised from this workspace:
  `/dev/uinput` is not present (no root/module available) and the declared
  kernel-uinput runner aliases (`dev-runner-vm`, `iprunner`) are not
  resolvable/reachable here. Status remains `pending` pending execution of the
  real verification command on a declared `kernel-uinput` runner.

## Task 4: Accelerated backend smoke test on gpu-compositor runner
Title: Accelerated backend smoke test on gpu-compositor runner
Status: pending
Dependencies: none
Acceptance: `test_backend_smoke` runs (not skipped) and passes on a runner with
  the `gpu-compositor` capability. It verifies that rendering through an
  accelerated SDL2 backend (OpenGL/OpenGL ES) produces correct pixel output
  broadly consistent with the software-renderer golden baseline (SPEC §11.1.6).
Verification: `ctest --test-dir build -R test_backend_smoke --output-on-failure`
Runner: gpu-compositor
Evidence: Passing `test_backend_smoke` ctest output on the `gpu-compositor`
  capability runner (`gpurunner`). Runner is declared in
  `.factory/environment.toml`; task pending execution on that runner.

## Task 5: Fix test_icon_map default-path install-state dependency
Title: Fix test_icon_map default-path install-state dependency
Status: pending
Dependencies: none
Acceptance: `test_icon_map` passes all 42 sub-tests deterministically from a
  clean source build with no install to the default prefix. The
  `test_default_path` sub-test no longer depends on whether the package is
  installed: it verifies that `cbx_icon_map_default_path()` resolves to a valid
  `controller-icons.yaml` file (the installed path when present, the source
  data path otherwise) without asserting a `controller-box` substring that only
  holds for the installed path. The test must pass both with and without the
  package installed, and must not rely on a prior packaging test having
  installed the artifact.
Verification: `ctest --test-dir build -R test_icon_map --output-on-failure`
  from a clean source build (no install); also re-run after `make install` to
  confirm both install states pass.
Runner: none
Evidence: Pending. Current state at the bound commit: `test_default_path`
  (tests/test_icon_map.c:296) still asserts
  `strstr(path, "controller-box") != NULL`, which holds only when the package
  is installed to the default prefix. In a clean source build
  `cbx_icon_map_default_path()` (src/icons/icon_map.c:386) falls back to
  `SOURCE_DATA_DIR "/controller-icons.yaml"`, which contains
  `controller-icons.yaml` but not the substring `controller-box`, so the
  assertion fails (`ctest --test-dir build -R '^test_icon_map$'` →
  Failed). Fix must remove the install-state-dependent `controller-box`
  substring assertion, then be verified in both install states.

## Task 6: Final documentation and specification audit
Title: Final documentation and specification audit
Status: pending
Dependencies: 1, 2, 3, 4, 5
Acceptance: The complete active-cycle task ledger is present and every task is
  complete with evidence. The canonical specification binding is fresh, the
  conformance matrix has no `partial`/`missing`/`blocked` rows without
  re-classified evidence, README and operational documentation match observed
  behavior, and the Git tree is clean on `develop` (SPEC §11.2.8, §11.2.9).
Verification: `./scripts/verify.sh` and `git status --porcelain` is clean on
  `develop`.
Runner: none
Evidence: Passing verification-gate output; clean `git status`; conformance
  matrix with all rows classified `verified`.
