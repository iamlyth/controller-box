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
Evidence: Verification passed on gpurunner (gpu-compositor capability). ctest --test-dir build -R test_backend_smoke --output-on-failure → 100%% passed. OpenGL accelerated backend detected, overlay rendering verified, alpha-blending verified, present/swap path verified. Manager test skipped on headless GPU (no CRTC display). Software renderer test also passes.

## Task 5: Fix test_icon_map default-path install-state dependency
Title: Fix test_icon_map default-path install-state dependency
Status: completed
Dependencies: none
Acceptance: `test_icon_map` passes all 42 sub-tests deterministically from a
Verification: `ctest --test-dir build -R test_icon_map --output-on-failure`
Runner: none
Evidence: test_icon_map passes all 42 sub-tests. The test_default_path assertion was fixed to use access(path, R_OK) instead of asserting a controller-box substring that only holds for the installed path. ctest --test-dir build -R test_icon_map → 100%% passed.

## Task 6: Eliminate stale dual settings copy between Settings and Controllers tabs
Title: Eliminate stale dual settings copy between Settings and Controllers tabs
Status: completed
Dependencies: 1, 2, 3, 4, 5
Acceptance: Editing a setting in the Settings tab (specifically the virtual-controller Count and per-slot Type rows) and saving must make the same values immediately visible in the Controllers tab for the remainder of the running session, without a manager restart. After the fix there must be exactly one authoritative in-memory settings object referenced by both the manager's Controllers tab (`mgr->ct.settings`) and the Settings tab (`mgr->st`), or the Settings-tab save path must reload `mgr->settings` from disk so the Controllers tab reflects the saved values. A regression test must demonstrate: (1) default settings loaded, Controllers tab shows the default count/types; (2) Settings tab edits VC_COUNT and a VC_TYPE slot then saves; (3) without restart, switching to the Controllers tab shows the updated count/types. The fix must not regress the existing settings-tab, controllers-tab, or manager visual/interaction tests.
Verification: Build and run the targeted suites: `ctest --test-dir build -R 'test_settings_tab|test_controllers_tab|test_manager' --output-on-failure`, plus the new regression test named for this scenario. Then run `./scripts/verify.sh` in a clean build tree to confirm the full gate stays green, and confirm the change is scoped to `src/manager/` with no change to the settings-save semantics on disk.
Runner: none
Evidence: New regression test `test_settings_controllers_sync` passes demonstrating the Saved-then-visible flow: default settings loaded → Controllers tab shows default count/types (ct->settings==&mgr->settings, count 4, types[0] xb360); Settings tab edits VC_COUNT 4→2 and VC_TYPE_0 xb360→ds5 then saves; without restart the Controllers tab reflects count 2 / types[0] ds5 / expected_target_count 2. The Settings-tab save path now reloads `mgr->settings` from disk (via a post-save callback registered by the manager) so the Controllers tab, which borrows `mgr->settings` as `mgr->ct.settings`, reflects the saved values immediately. Change scoped to `src/manager/` (manager.c, settings_tab.c/h) plus the new regression test in `tests/`; settings-save semantics on disk unchanged (`cbx_settings_save(&tab->settings)` still writes atomically). Targeted `ctest --test-dir build -R 'test_settings_tab|test_controllers_tab|test_manager|test_settings_controllers_sync' --output-on-failure` → 13/13 passed. Full gate in a clean build tree: `./scripts/verify.sh` → 100% tests passed, 0 failed out of 92, exit 0 (only capability-gated skips: test_kernel_controller, test_backend_smoke). No regression in settings-tab, controllers-tab, or manager visual/interaction tests.

## Task 7: Final documentation and specification audit
Title: Final documentation and specification audit
Status: pending
Dependencies: 1, 2, 3, 4, 5, 6
Acceptance: (1) The complete active-cycle task ledger is present and every task carries all required fields; there are no duplicate-scope tasks, no tasks left with `Status: pending` other than this audit, and no task whose `Verification:` command does not exercise the implementation it claims to close. (2) The full verification gate passes from scratch: `./scripts/verify.sh` reconfigures a clean build tree (the stale-build-cache condition documented in the bug study is neutralized by verify.sh's self-heal), all tests pass, and hardware-gated tests report legitimate exit-77 capability skips only where the runner lacks the declared capability (never a silent pass or fake completion). (3) `git status --porcelain` is clean apart from the plan and this audit's own changes, and the human has promoted/approved on `develop` per the git-workflow boundary. (4) A specification-conformance audit confirms every normative requirement in `docs/SPEC.md` is either `verified` with executable evidence recorded, or explicitly documented as a known limited/deferred item (§13 deferrals such as Host-Mode interior UX and theme-file format are recorded as findings, not silently claimed). (5) Previously flagged audit concerns (the plan shows Task 6 was `blocked by audit after 3 repairs` in an earlier cycle) are re-checked: no weakened/tautological assertions, no removed or silently skipped conformance checks, and real framebuffer-pixel + semantic-outcome acceptance evidence present for visual and interaction requirements. Findings become new bounded plan tasks in the next planning round, never a claim of completion.
Verification: Run `rm -rf build && ./scripts/verify.sh` (fresh reconfigure + build + full CTest) and capture the real exit code; record the per-test summary confirming passes and any exit-77 skips are only the capability-gated cases. Run `git status --porcelain` and confirm only the plan/audit artifacts are untracked. Confirm the conformance matrix in the audit shows a `verified` row (with the evidence command) for each normative requirement, or a documented finding for each exception.
Runner: none
Evidence: Output of a full `./scripts/verify.sh` run in a fresh build tree with the recorded exit code; per-test pass/skip summary showing only capability-gated exit-77 skips; clean `git status --porcelain`; a written conformance matrix mapping each normative SPEC section to a verified row or a documented finding.
