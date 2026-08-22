# Handoff: Task 14 (BUG-0015 software portion) implemented, verified, and recorded

## Outcome this iteration
- Completed plan **Task 14** (Fix virtual-controller type-change topology preservation, BUG-0015 software portion, runtime task `task-1787407706-cd8d`). The production fix was already on `develop` at HEAD `69ccf9e`; this iteration re-verified it at the current HEAD and recorded the plan Result.
- `cbx_controllers_tab_change_type` sends only the selected slot's new type via `ip_composite_set_target_devices` (SPEC §5.2) instead of assembling a CSV of every model target's type (which corrupted the other slots).
- Mock DBus records most-recent `call_method` string args (`ip_dbus_mock_last_call`); `test_change_type_mixed`/`test_change_type_success` assert the exact `SetTargetDevices` CSV; Add path asserts exact `AttachTargetDevice` target→composite paths.

## Exact verification (no tree change)
- Build: `cmake --build build-check --parallel` clean (Debug).
- `ctest --test-dir build-check -R 'test_controllers_tab|test_manager_calls'` → 2/2 pass.
- Full suite: 100% pass (98 pass / 2 pre-existing hardware skips `test_kernel_controller`, `test_backend_smoke`).
- Regression real: temporarily reverted to buggy CSV assembly → `test_change_type_mixed` FAILS; restored fix → passes.
- `validate-conformance.py planning` valid (76 reqs); `validate-implementation-plan.py planning` exit 0; `verify-boilerplate.sh` passed.

## Commit
- HEAD `69ccf9e` already carried the code+tests; this iteration recorded plan Task 14 Result. Commit boundary: plan path (`.factory/artifacts/...`) is outside `.ralph/`, so the commit is substantive and guard-allowed.

## Next
- The next software task is BUG-0018 diagram geometry (runtime task `task-1787407706-bacd`): installed mapping-editor diagram is pixelated/stretched, mapped-button markers misaligned; requires repairing the installed exact-commit capture driver + semantic installed-window tests (machine-vision findings-only, no golden regen). After that the final audit (Task 4) is the sole gate, still blocked on external hardware/capability/signer facts (FACT-002..007). Do not emit the completion token; ledger stays open; plan stays `active`.
