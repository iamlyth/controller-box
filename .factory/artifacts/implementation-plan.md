---
schema: factory-plan/v1
spec_path: docs/SPEC.md
status: active
---

# Controller-Box Implementation Plan

## Verified baseline

Baseline captured on branch `develop` from a clean build:

```bash
rm -rf build && cmake -B build -DCMAKE_BUILD_TYPE=Debug \
  && cmake --build build -j$(nproc) \
  && ctest --test-dir build --output-on-failure --timeout 120
```

**Build:** succeeds cleanly (Debug, SDL2 + nanosvg + cmocka + systemd all found).

**Test suite:** 91 tests registered. 89 pass, 1 fails, 2 skip.

### Genuine failure (reproducible)

- **`test_golden`** — 3 of 11 sub-tests fail: `manager_editor_list`,
  `manager_editor_sequential`, `manager_editor_validation_error`. The captured
  profile-editor framebuffer differs from the reviewed golden baseline in the
  editor region (diagram + binding list + status label). ~30,784 pixels exceed
  the ±3 per-channel tolerance; the 2% image threshold is 18,432 pixels, so the
  comparison fails. RMSE ≈ 0.06. The other 8 golden sub-tests (overlay states,
  manager controllers/profiles/settings) pass. Root cause is under
  investigation; the diff is consistent with a font-antialiasing difference
  between the environment that generated the baselines and the current
  environment, but a rendering regression in the editor must be ruled out
  before regenerating baselines.

### Flaky (transient first-run failures, pass on re-run)

The following 9 tests failed on the first full run but pass when re-run
individually and when re-run together: `test_packaging`,
`test_installed_smoke`, `test_installed_diagram`, `test_installed_binary`,
`test_profile_list`, `test_icon_map`, `test_icon_cache`, `test_icon_lookup`,
`test_overlay_visual`. The failures are attributed to a fresh-build race /
resource contention in the heavy packaging and installed-binary tests. This
violates SPEC §11.2.5 ("no flaky rerun dependencies") and must be stabilized.

### Skipped (need runner hardware)

- **`test_kernel_controller`** — skips (exit 77): `/dev/uinput` is not
  available. Requires a runner with the `kernel-uinput` capability
  (`dev-runner-vm` or `iprunner`).
- **`test_backend_smoke`** — skips (exit 77): no accelerated OpenGL/OpenGL ES
  video device in headless CI. Requires a runner with the `gpu-compositor`
  capability (`gpurunner`). Software-renderer partial evidence is provided by
  `test_backend_smoke_sw`, which passes.

---

## Task 1: Fix test_golden profile-editor golden image mismatches

Title: Fix test_golden profile-editor golden image mismatches
Status: pending
Dependencies: none
Acceptance: `test_golden` passes all 11 sub-tests. The three profile-editor
  states (`manager_editor_list`, `manager_editor_sequential`,
  `manager_editor_validation_error`) match their reviewed golden baselines
  within the documented ±3 per-channel / 2% image tolerance. If the mismatch
  is a rendering regression, the editor code is fixed; if it is a stale
  baseline, the baseline is regenerated only as an explicit, reviewed change
  (SPEC §11.1.3) and the reason recorded.
Verification: `ctest --test-dir build -R test_golden --output-on-failure`
Runner: none
Evidence: Passing `test_golden` ctest output; resolution of the
  `tests/golden-fail/` artifacts (either a code fix or a reviewed baseline
  regeneration with the diff documented).

## Task 2: Stabilize flaky acceptance tests

Title: Stabilize flaky acceptance tests
Status: pending
Dependencies: none
Acceptance: The full ctest suite passes reliably across repeated consecutive
  runs with no transient failures. The 9 tests that failed only on the first
  run (`test_packaging`, `test_installed_smoke`, `test_installed_diagram`,
  `test_installed_binary`, `test_profile_list`, `test_icon_map`,
  `test_icon_cache`, `test_icon_lookup`, `test_overlay_visual`) are
  deterministic. Root cause (fresh-build race / resource contention in the
  packaging and installed-binary tests) is identified and removed, satisfying
  SPEC §11.2.5 ("no flaky rerun dependencies").
Verification: `ctest --test-dir build --output-on-failure --timeout 120`
  run three consecutive times from a clean build; all three runs pass.
Runner: none
Evidence: Three consecutive clean full-suite ctest runs; root-cause note for
  the transient failures.

## Task 3: Kernel-backed controller integration test on kernel-uinput runner

Title: Kernel-backed controller integration test on kernel-uinput runner
Status: blocked
Dependencies: none
Acceptance: `test_kernel_controller` runs (not skipped) and passes on a runner
  with the `kernel-uinput` capability. It creates a synthetic evdev gamepad via
  `/dev/uinput`, launches the installed Manager binary against a private
  InputPlumber-compatible DBus server, and verifies semantic outcomes from real
  kernel gamepad events through the production event loop (SPEC §5.7, §11.1.5).
Verification: `ctest --test-dir build -R test_kernel_controller --output-on-failure`
Runner: kernel-uinput
Evidence: Passing `test_kernel_controller` ctest output on the
  `kernel-uinput` runner (`dev-runner-vm` or `iprunner`).

## Task 4: Accelerated backend smoke test on gpu-compositor runner

Title: Accelerated backend smoke test on gpu-compositor runner
Status: blocked
Dependencies: none
Acceptance: `test_backend_smoke` runs (not skipped) and passes on a runner with
  the `gpu-compositor` capability. It verifies that rendering through an
  accelerated SDL2 backend (OpenGL/OpenGL ES) produces correct pixel output
  broadly consistent with the software-renderer golden baseline (SPEC §11.1.6).
Verification: `ctest --test-dir build -R test_backend_smoke --output-on-failure`
Runner: gpu-compositor
Evidence: Passing `test_backend_smoke` ctest output on the `gpu-compositor`
  runner (`gpurunner`).

## Task 5: Final documentation and specification audit

Title: Final documentation and specification audit
Status: pending
Dependencies: 1, 2, 3, 4
Acceptance: The complete active-cycle task ledger is present and every task is
  complete with evidence. The canonical specification binding is fresh, the
  conformance matrix has no `partial`/`missing`/`blocked` rows without
  re-classified evidence, README and operational documentation match observed
  behavior, and the Git tree is clean on `develop` (SPEC §11.2.8, §11.2.9).
Verification: `./scripts/verify.sh` and `git status --porcelain` is
  clean on `develop`.
Runner: none
Evidence: Passing verification-gate output; clean `git status`; conformance
  matrix with all rows classified `verified`.
