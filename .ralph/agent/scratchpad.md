# Implementation Scratchpad — Controller-Box v1

## Task 6 complete: Fix documentation inaccuracies

- **Commit:** 728e06f on develop
- **What:** Fixed three documentation/boilerplate issues:
  1. `docs/OPERATIONS.md` line 1032: inventory description corrected from "59 entries, all verified" to "59 entries: 50 verified, 8 NOT_APPLICABLE, 1 DEFERRED"
  2. `README.md`: added individual run command for `test_installed_functional` and `test_installed_binary` in the "Or run individual test groups" section (table entries 5a/5b already present from Task 3)
  3. `tests/test_installed_binary.sh`: fixed two SC2181 shellcheck style warnings (`if [ $? -eq 0 ]` → `if cmd; then`) at lines 563 and 634 that caused `verify-boilerplate.sh` to fail
- **Verification:** `./scripts/check-docs-sync.sh` → pass. `nix-shell --run './scripts/verify-boilerplate.sh'` → pass ("verify: boilerplate checks passed").
- **Plan:** Task 6 marked complete with evidence in implementation-plan.md.

## Next task

Task 7: Add kernel-backed controller test or document environment limitation. Create `tests/test_kernel_controller.c` using `uinput` for synthetic evdev controller. If `/dev/uinput` unavailable, skip with exit 77. Register in CMakeLists.txt with `SKIP_RETURN_CODE 77`. If test cannot run in declared environment, document PERF-01/MGR-07 as `partial` with explicit rationale.