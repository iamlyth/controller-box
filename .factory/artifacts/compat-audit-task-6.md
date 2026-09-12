# Compatibility Audit — Task 6: Reconfigure canonical build directory

Audit scope: platform, dependency, API, and build-system compatibility for
Task 6 in the current tree (`HEAD 1788c658`, branch `develop`).

## Summary

The Task 6 acceptance is met *in the current working-tree state*, but the
mechanism the verification relies on is **not reproducible** and the repo
still leaves stale build trees that will fail the exact acceptance command.
One BLOCKER, one WARN.

---

## BLOCKER — `CMAKE_HOME_DIRECTORY` cannot be rewritten by reconfigure; stale phantom path breaks the acceptance command

**Files:** `build/CMakeCache.txt` (canonical build), plus stale trees
`build-test/`, `build-diag/`, `build-check/`, `build-maintenance-verify/`

`CMAKE_HOME_DIRECTORY` is an `INTERNAL` CMake cache value captured at first
configure (along with `CMAKE_CACHEFILE_DIR`). Re-invoking
`cmake -S . -B build` does **not** update it when the cache records a
different source directory — CMake **aborts** instead:

```
CMake Error: The current CMakeCache.txt directory /workspace/project/build/CMakeCache.txt
is different than the directory /workspace/controller-box/build where CMakeCache.txt was created.
CMake Error: The source "/workspace/project/CMakeLists.txt" does not match the source
"/workspace/controller-box/CMakeLists.txt" used to generate cache.
Re-run cmake with a different source directory.
```

I reproduced this deterministically twice:

1. In an isolated copy (`/tmp/stalerepro`, cache rewritten to the phantom
   `/workspace/controller-box`) — reconfigure aborted, path unchanged.
2. On the **real canonical `build/`** — I restored a faithful stale-phantom
   cache (backup preserved) and re-ran `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`
   inside `nix-shell`; it aborted with the same mismatch error and left the
   cache at `/workspace/controller-box`. (I then restored the good cache and
   re-verified the acceptance command passes.)

Consequence: the Task 6 verification command as written —
`cmake -S . -B build … && grep CMAKE_HOME_DIRECTORY build/CMakeCache.txt && ctest …`
— does **not** restore the correct path from a dirty tree. It only passes when
`build/` has already been wiped and freshly regenerated, which is not part of
the acceptance command. `build/` is gitignored, and the implementation commit
`1788c658` changed only `tests/test_icon_map.c` + the plan — it added **no
guard** that forces a clean reconfigure. Any future cycle or checkout that
reuses a stale cache will fail the gate.

Stale trees still present with the phantom path baked into their cache
(`CMAKE_HOME_DIRECTORY:INTERNAL=/workspace/controller-box`):
- `build-test/`
- `build-diag/`
- `build-check/`
- `build-maintenance-verify/`

**Recommendation:**
- Detect a mismatched `CMAKE_HOME_DIRECTORY` in the canonical build before
  configuring and recreate the tree (e.g. in the build/verify script: if
  `grep -q '^CMAKE_HOME_DIRECTORY.*/workspace/controller-box' build/CMakeCache.txt`
  then `cmake -E remove_directory build`) rather than relying on a bare
  reconfigure to heal it.
- Delete the four stale phantom build trees from the workspace so they cannot
  be mistaken for a usable build or break a reconfigure step.

---

## WARN — Full build still requires `nix-shell`; SDL2/dependency modules are not found on the host pkg-config

**Files:** `CMakeLists.txt` (lines ~44–58)

`pkg_check_modules(SDL2 REQUIRED … sdl2)` etc. fail when `cmake` is invoked
outside `nix-shell`:

```
CMake Error at .../FindPkgConfig.cmake:1093: The following required packages were not found: - sdl2
```

This is consistent with the documented build path (`AGENTS.md`: "Use Nix so
SDL2, systemd, YAML, cmocka … are consistent"), so it is expected — but any
automation/verify step that forgets the `nix-shell` wrapper will fail
configure with a misleading "sdl2 not found" error rather than a clear
"run inside nix-shell" message. (Note: the outside-nix failure in my first
probe surfaced at the sdl2 check *after* the source-match check was cleared,
which is precisely why the earlier canonical tree appeared to reconfigure
even though its cache was stale — the two failure modes mask each other.)

**Recommendation:** Keep the `nix-shell --run '…'` wrapper for all CMake
invocations; consider an early CMake-agnostic guard (e.g. verify
`$SDL2_DIR`/`PKG_CONFIG_PATH` is set) in build scripts so a bare-shell invoke
fails fast and clearly.

---

## INFO — CMake / compiler feature compatibility is fine

`cmake_minimum_required(VERSION 3.16)` is well below the installed
CMake 4.3.4 (Nix store path confirmed); no removed/renamed commands are
used. The project builds cleanly with Clang 21.1.8 in Debug. No
SDL2/SDL3 API, DBus (sd-bus vs dbus-1), or feature-test-macro conflicts
were observed in the Task-6 build path.

---

## Verified-good current state (evidence recorded)

After restoring the clean cache, the acceptance command passes end-to-end
inside `nix-shell`:

```
CMAKE_HOME_DIRECTORY:INTERNAL=/workspace/project
1/1 Test #13: test_settings .... Passed    0.04 sec
100% tests passed, 0 tests failed out of 1
```

`build/CTestTestfile.cmake:31` now registers `test_settings` at the real
path `/workspace/project/build/test_settings` and a real binary runs (no
`Not Run` / file-not-found) — satisfying the second acceptance clause.

## Verdict

Acceptance currently holds, but only because the canonical cache happens to
be freshly clean. The stale-config failure mode is a real build-system
compatibility defect that must be fixed before this is robustly "complete."
