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
Evidence: Repair (cycle 2): applied linting cleanup for the harness modules in scope (removed unused `RoundMetrics`/`Issue` imports in campaign.py, dead `min_round` param in issues.py, duplicate banner + unreachable `audit_findings` branch + redundant no-op in campaign.py, duplicate rsync exclude in runner.py). Re-ran the exact verification command; all 12 modules compile with exit 0, full `.factory/loop` package imports cleanly.

## Task 9: Verify full test suite passes
Title: Verify full test suite passes
Status: completed
Dependencies: none
Acceptance: Full ctest suite passes with 0 failures (hardware tests may skip with exit 77).
Verification: ./scripts/verify.sh
Runner: none
Evidence: verification exit 0 on local

## Task 10: Final documentation and specification audit
Title: Final documentation and specification audit
Status: completed
Dependencies: 1, 2, 3, 4, 5, 6, 7, 8, 9
Acceptance: The full verification suite passes and the Git tree is clean on develop. Additionally, the audit must close out the two concrete spec areas surfaced by the subsystem studies — evaluating each to a conclusion rather than passing silently:
Verification: ./scripts/verify.sh && git status --porcelain
Runner: none
Evidence: Completed. Both audit areas disposed **wired**.

§10.3 (five DBus gap workarounds, SPEC.md l.545) — **wired**. Gap #1 InterceptMode poll: `src/dbus/ip_intercept_poll.h` l.51 `IP_INTERCEPT_POLL_INTERVAL_MS 50` (DEC-002), `SDL_AddTimer` 50ms (`ip_intercept_poll.c` l.124), consumed by `src/app/overlay_service.c` l.869. Gap #2 GamepadOrder persistence/reapply: `src/dbus/ip_gamepad_order.c` (header l.2 "Task 15, gap #2") writes/loads via `cbx_assignments_save/load` (assign_persist.c l.56-84), restored on restart by `src/identify/gamepad_order_restore.c`. Gap #3 CreateCompositeDevice temp file: `src/dbus/ip_create_composite.c` l.54-75 builds `mkdir+mkstemps` template `<tmp>/controller-box-XXXXXX.yaml`. Gap #4 filesystem profile enumeration: `src/config/config_paths.c` l.28 `IP_PROFILES_SUBDIR`, `src/config/config_profile_list.c` readdir (l.86, l.195) over user+system `inputplumber/profiles/`, `devices/`, `capability_maps/`. Gap #5 (add/remove source devices) — **removed/superseded**: SPEC l.545 verdict "Not needed for v1", mirrored in SPEC §12 Out of Scope, no code path needed.

§6 Controller Identification (SPEC.md l.256-284) — **wired**. `src/identify/identity.c` `cbx_identity_extract()` implements 4-layer extraction in descending strength (BT MAC → USB serial → USB port → connection order) per §6.2; prefixed IDs `BT:`, `USB:SN`, `USB:phys:`, `ORDER:` in `cbx_identity_parse_layer`; downgrade detection `cbx_identity_is_downgrade` (identity.c) + `src/identify/identity_downgrade.c` per §6.3; assignment persistence `src/identify/assign.c`/`assign_persist.c` writes `assignments.yaml` (§7.4).

Coverage tests pass (id-45 test_identity, #47 test_assign_persist, #48 test_identity_downgrade, #14 test_assignments, #49 test_order_restore, #28 test_gamepad_order, #23 test_intercept_poll, #27 test_create_composite, #22 test_composite_calls, #16 test_profile_list).

Verification: `./scripts/verify.sh && git status --porcelain` — **exit 0**; 100% tests passed (91/91, 0 failures); the 2 skips (test_kernel_controller #3, test_backend_smoke #81) are hardware-dependent and exit 77 per AGENTS.md (kernel-uinput/gpu-compositor runners). `git status --porcelain` clean (0 changed files) on `develop`.

