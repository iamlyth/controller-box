# Task 7 Complete — Installed Binary Functional Acceptance Test

## Outcome
Task 7 verified complete. The existing `tests/test_installed_binary.sh` already implemented the full acceptance test. Verified it passes and updated the plan.

## Verification
- `nix-shell --run "ctest --test-dir build-check -R 'installed_binary' --output-on-failure"` → 1/1 Passed (22.92s)
- All 6 phases pass: manager launch+render (mean=31.0), tab nav+settings persistence (178 bytes), target creation via DBus GetManagedObjects, profile load/save, persistence after restart, overlay service idle poll+InterceptMode query
- Full suite: 96/96 (95 passed + 1 skipped `test_backend_smoke` §11.1.6)
- `nix-shell --run './scripts/verify-project.sh'` → PASS
- Test does NOT link libcontrollerbox (shell script + test_ip_server helper links only libsystemd)

## Changes (commit d57830c on develop)
- `.factory/artifacts/implementation-plan.md`: VS-01 conformance matrix row updated from `partial` to `verified`; Task 7 status changed from `pending` to `complete` with detailed evidence

## Key facts
- `/dev/uinput` not available in Nix-shell → kernel-backed virtual gamepad not possible
- SDL virtual joysticks are process-local for event delivery → can't send controller events cross-process
- Test uses xdotool keyboard/mouse events through manager's `cbx_manager_handle_event` dispatch path (same code path as SDL_CONTROLLERBUTTONDOWN)
- Virtual SDL gamepad + controller button event coverage provided by `test_installed_functional.c` (in-process, links libcontrollerbox)
- `test_ip_server` binary: standalone native DBus server helper, links only libsystemd, NOT libcontrollerbox

## Next task
Task 8: Final documentation and specification audit. All dependencies (Tasks 1-7) are complete. Ready to start.