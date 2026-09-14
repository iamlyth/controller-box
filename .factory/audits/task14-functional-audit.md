# Functional Audit — Task 14: Clean reproducible verification baseline

**Scope**: `scripts/verify.sh` (stale-cache detection), `tests/test_profile_diagram.c`
(borrowed-theme fix). Task 14 changed no production module code; the deliverable is a
reproducible build/test/packaging baseline plus two infrastructure repairs.

## Independent verification performed

| Check | Command | Result |
|---|---|---|
| Clean build | `rm -rf build && cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j$(nproc)` | exit 0, **0 warnings, 0 errors** (full log scanned, not tail) |
| Full CTest | `ctest --test-dir build --output-on-failure --timeout 120` | exit 0, **91/91 passed, 0 failed**, 173.4 s |
| Stale-cache recovery (end-to-end) | re-pinned `build/CMakeCache.txt` to a dead root, ran `./scripts/verify.sh` | stale branch fired, tree regenerated, re-pinned to `/workspace/project`, exit 0, 91/91 |
| Idempotency | second `./scripts/verify.sh` on canonical cache | exit 0, **no false-positive removal**, 91/91, 169.9 s |
| Skip legitimacy | ran both skipped binaries directly | `test_kernel_controller` exit 77 (`/dev/uinput` absent); `test_backend_smoke` exit 77 (`SDL_Init: No available video device`); `test_backend_smoke_sw` exit 0 |
| Packaging/installed | from CTest log | `test_flatpak_manifest`, `test_packaging` (77.1 s), `test_installed_smoke` (17.0 s), `test_installed_diagram` (13.1 s), `test_installed_binary`, `test_installed_functional`, `test_service_install` — all Passed |
| Stale-cache probe logic | 7 scratch A/B cases (exact root, symlink→root, nonexistent, empty, different root, no variable, relative) | RETAIN×2 / REMOVE×5 — all correct; ambiguous cases fail conservative (rebuild) |
| ASan/UBSan repair validation | fresh `-DCBX_ENABLE_SANITIZERS=ON` build, `ASAN_OPTIONS=detect_stack_use_after_return=1 ./test_profile_diagram` | **37/37 passed**, no sanitizer reports |
| Stale-root hardcodes | grep tracked source/scripts/cmake/nix for `lalobied/repos`, `/workspace/controller-box` | none |

## Findings

No **BLOCKER** or **WARN** findings. The recorded baseline is independently reproducible,
and both infrastructure repairs are functionally correct:

- The `tests/test_profile_diagram.c:757` fix correctly borrows the fixture-owned theme
  (`&f->theme`, initialized via `cbx_theme_default` in setup — semantics identical to the
  old stack local), matching the documented borrowed-theme contract in
  `src/manager/profile_diagram.h:104`. All other `&theme` uses in that file are
  function-scoped with the diagram shut down before return, so no remaining instance of
  the dangling pattern exists in tests; production (`profiles_tab.c:1284`) borrows the
  manager-owned theme for the app lifetime — safe.
- The `scripts/verify.sh:32-44` probe resolves both roots physically (`pwd -P`), so a
  symlinked checkout is retained while genuinely moved/dead roots are rebuilt. Quoting is
  safe throughout; unresolvable/empty/multi-line cache values fail conservative (rebuild),
  which can only cost time, never correctness.

### INFO-1: Canonical build cache at audit time was pinned to a dead root
**Files**: `build/CMakeCache.txt` (untracked artifact, since regenerated)

Before this audit, `build/CMakeCache.txt` recorded
`CMAKE_HOME_DIRECTORY=/workspace/controller-box`, a path that does not exist
(`/workspace` contains only `project`). Task 14's final evidence recorded the cache
pinned to `/workspace/project`, so something reconfigured the canonical tree from a
`/workspace/controller-box` path (most likely a since-removed symlink) after the task's
last run. Not a code defect — I verified end-to-end that `verify.sh` correctly detects
this exact state as stale, removes the tree, and regenerates it (exit 0, 91/91). Noted
only because it shows the recorded post-task environment did not persist to audit time.

### INFO-2: Stale-cache self-heal intentionally covers only the canonical `build/` tree
**Files**: `scripts/verify.sh:32` (`[ "$BUILD_DIR" = "build" ]` guard)

With `CBX_BUILD_DIR` set to a non-canonical directory, a moved checkout's stale cache is
not self-healed and `cmake` will abort with a source/binary-dir mismatch. This matches the
script's documented "canonical build/ tree" scope and the AGENTS.md guidance that custom
prefix/build trees are for isolated testing, so no change is requested — just an
observation for anyone relying on `CBX_BUILD_DIR` across checkouts.

## Conclusion

Task 14's acceptance is met with real, independently reproduced evidence: clean build
(0 warnings/0 errors), 91/91 CTest passes with exactly the two recorded exit-77 hardware
skips, all packaging/installed checks passing, stale-cache detection working in both
directions (fires on stale, silent on canonical), and the ASan stack-use-after-return
repair validated under sanitizers. **No blocking findings.**