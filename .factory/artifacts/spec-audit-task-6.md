# Spec Compliance Audit — Task 6

**Task:** Reconfigure canonical build directory at the real source path
**Date:** 2026-09-12
**Auditor:** spec-compliance

## Acceptance vs. verified state

| Acceptance clause | Required | Verified |
|---|---|---|
| `CMAKE_HOME_DIRECTORY` resolves to real `/workspace/project` | `/workspace/project` | ✅ `CMAKE_HOME_DIRECTORY:INTERNAL=/workspace/project` (after fresh `cmake -S . -B build`) |
| Not the phantom `/workspace/controller-box` | absent | ✅ `/workspace/controller-box` does not exist on disk; no cache entry references it |
| `ctest --test-dir build` executes real test binaries | real binaries, no wholesale Not-Run | ✅ `test_settings` (Test #13) runs `build/test_settings` binary, passes 0.04s |
| Full acceptance verification exits 0 | 0 | ✅ Ran verbatim: `cmake -S . -B build && cmake --build build --parallel && grep CMAKE_HOME_DIRECTORY && ctest -R '^test_settings$'` → `VERIFICATION_EXIT=0` |

## Findings

### None (blocking or non-blocking)

No spec-compliance findings. Task 6's acceptance criteria are met on the real
project tree:

1. Reconfiguring the canonical `build/` (per the config `build_command` and the
   task verification command) writes `CMAKE_HOME_DIRECTORY=/workspace/project`,
   the real source root. The phantom `/workspace/controller-box` is no longer
   referenced from the cache and does not exist as a directory.
2. `ctest --test-dir build -R '^test_settings$'` executes the actual
   `build/test_settings` binary (a real production-initialization test) and
   passes — not a "Not Run"/file-not-found result.
3. The full acceptance pipeline exits 0 when run verbatim inside the project's
   required `nix-shell` build environment.

## Notes

- Compilation requires the project's documented `nix-shell` environment; a
  bare-shell `cmake` configure fails at `FindPkgConfig` (no `sdl2`). This is an
  environment/runner constraint, not a task 6 defect, and the task verification
  is satisfied when run as prescribed by `AGENTS.md`.
- A transient compile failure on the unrelated `test_assign`/`test_profile_diagram`
  targets was observed mid-audit after an intermediate interrupted `rm -rf build`
  + failed bare-shell configure left the tree inconsistent; a clean, complete
  run of the acceptance command built every target cleanly and passed. No defect
  remains in the finished state.

## Verdict

**No findings.** Task 6 is compliant with its acceptance criteria.
