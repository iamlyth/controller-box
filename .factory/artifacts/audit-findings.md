# Audit findings (round 1, task 6)

## linting Audit

# Linting Audit — Task 6: Reconfigure canonical build directory at the real source path

**Scope reviewed:** commit `1788c658` (task 6 implementation) — `.factory/artifacts/implementation-plan.md` and `tests/test_icon_map.c`.

## Context

The task itself is an operational CMake reconfiguration (build/ rebuild at the real source root). There is no source code in the reconfiguration itself to lint. The only code change introduced by this task is a single assertion modification in `tests/test_icon_map.c`, which is the legitimate fix for the stale-path-hidden defect (the test previously required the literal `"controller-box"` substring, only true under the phantom build path). The implementation-plan.md changes are canonical mutable loop state per `AGENTS.md`, so no findings there.

Overall the change is sound: the old substring assertion was replaced with a correct semantic outcome check (`access(path, R_OK) == 0`), `<unistd.h>` is already included so `access()` needs no new include, and the surrounding test functions remain consistent with the file's established `/* short kebab description */` + `assert_*` style.

## Findings

### WARN — Verbose comment with iteration-history content and a typo
- **Files:** `tests/test_icon_map.c:296-301`
- **Issue:** The new comment block justifying the semantic assertion is six lines long and recounts iteration history ("The prior literal 'controller-box' check only passed because a stale build rooted at the phantom ... happened to embed that string") rather than concisely recording *why* the constraint matters. This directly contradicts the project's own rule in `AGENTS.md`: "Documentation records why a constraint or test matters, not iteration history." Additionally there is a typo: an extraneous `/` appears before the quoted path — `/"workspace/controller-box"` should be `"/workspace/controller-box"`.
- **Recommendation:** Condense to a brief "why" comment, e.g.:
  ```c
  /* Resolve the default path to a real, readable file regardless of how
     the source tree is named (the path must be semantic, not substring-coupled). */
  assert_true(access(path, R_OK) == 0);
  ```
  This keeps the WHY (semantic, not literal-substring assertion) without the stale-build backstory or the malformed literal.

### INFO — Assertion provides no diagnostic on failure
- **Files:** `tests/test_icon_map.c:301`
- **Issue:** `assert_true(access(path, R_OK) == 0)` fails silently if the default path ever resolves to a missing/unreadable file; a future maintainer would not immediately know *which* path failed. This is a minor ergonomic point, not a correctness problem.
- **Recommendation:** Optional. Use cmocka's `fail_msg()`/non-macro branch, or print the resolved `path` on failure, so a regression reports the offending path directly. Not blocking.

## Summary

No **BLOCKER** findings. The code change is correct, minimal, and consistent with the surrounding file. One recommendable cleanup: shorten the comment to its "why" and fix the `/"workspace` typo (**WARN**). One optional diagnostic nicety (**INFO**).

Exit status: **0** (no blocking issues).


---

## efficiency Audit

Audit complete. The verification confirms the canonical build is correctly reconfigured: `CMAKE_HOME_DIRECTORY:INTERNAL=/workspace/project`, `ctest -R '^test_settings$'` runs a real binary (Passed, 0.04s), and 90 real test binaries are present in `build/`. The task's acceptance is satisfied — no blockers.

# Efficiency Audit Report — Task 6

**Verdict:** No **BLOCKER** findings. Task 6 is complete and its acceptance is met.

---

## WARN

### 1. ccache is available but not wired into the build
- **File:** `CMakeLists.txt`, `scripts/verify.sh`, `shell.nix`
- The Nix environment ships ccache (`/nix/store/.../ccache-4.13.6`), but the CMake build does not configure `CMAKE_CXX_COMPILER_LAUNCHER`/`CMAKE_C_COMPILER_LAUNCHER`. The factory workflow is rebuild-heavy — this very task required wiping and reconfiguring the canonical `build/`, and each verify/runner invocation recompiles all translation units from scratch. That is exactly the repeated full-recompile scenario ccache is built for.
- **Recommendation:** In `CMakeLists.txt` add:
  ```cmake
  find_program(CCACHE_PROGRAM ccache)
  if(CCACHE_PROGRAM)
      set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
      set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
  endif()
  ```
  This makes any future `build/` wipe-and-reconfigure (the operation this task performs routinely) near-instant on the warm cache. Non-blocking; build correctness is unaffected.

---

## INFO

### 2. `verify.sh` runs an unconditional CMake configure step on every invocation
- **File:** `scripts/verify.sh`
- Both the `nix-shell` path and the fallback path run `cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug` before every build, on every runner, even when the cache is already fully configured and no sources changed. Cost is bounded because CMake elides regeneration when nothing changed, so this is not expensive — but it is redundant and forces a `configure_file(config.h.in)` re-run on each verify cycle.
- **Recommendation:** It is already effectively incremental; consider a quick `cmake -S . -B build >/dev/null &&` guard only when `CMakeCache.txt` is missing for marginal savings. Low priority.

### 3. ~450 MB of stale legacy build directories remain in the tree
- **File:** `build-check/`, `build-diag/`, `build-maintenance-verify/`, `build-planner/`, `build-test/`
- These prior diagnostic build dirs (~90 MB each) are gitignored and do not affect correctness or the canonical `build/`, but they duplicate the compilation output and consume disk. This task's purpose is a single canonical build; the leftover trees are pure waste.
- **Recommendation:** Optional cleanup (`rm -rf build-check build-diag build-maintenance-verify build-planner build-test`) to reclaim ~450 MB. Non-blocking.

---

**Summary:** No performance or resource issues block task 6. The canonical `build/` is correctly rooted at `/workspace/project` and executes real test binaries. All findings above are optional efficiency improvements for the ongoing factory workflow, not defects in this task's completion.

Exit code: 0 (No findings that block completion; report issued with WARN/INFO items).


---

## security Audit

## Security Audit Report — Task 6: Reconfigure canonical build directory

**Scope:** Build-directory reconfiguration (task commit `1788c658` only touched `tests/test_icon_map.c` plus the implementation plan; production sources unchanged) and the canonical `build/` cache state. I focused on the security-relevant surface exercised by the task: the icon-map default-path resolution and file load (`src/icons/icon_map.c`) and the build-cache path integrity.

### Verification of acceptance state
- Canonical `build/CMakeCache.txt` → `CMAKE_HOME_DIRECTORY:INTERNAL=/workspace/project` (real root). ✔
- `build/CTestTestfile.cmake` → all test paths reference `/workspace/project`; no `workspace/controller-box` string in the canonical build products. ✔
- `ctest --test-dir build -R '^test_settings$'` executed a real binary (`/workspace/project/build/test_settings`) and **Passed** 0.04s. ✔

No **BLOCKER** issues.

---

### WARN — Stale auxiliary build caches rooted at the phantom source path
**Files:** `build-check/CMakeCache.txt`, `build-diag/CMakeCache.txt`, `build-test/CMakeCache.txt`, `build-maintenance-verify/CMakeCache.txt`

Four auxiliary build directories still have `CMAKE_HOME_DIRECTORY = /workspace/controller-box` (a non-existent path). Any `ctest --test-dir <aux>` or targeted build against these stale caches will regenerate against / point tests at a phantom tree, producing failed/incorrect test discovery rather than a clean result. Security impact is low (the path does not exist, so nothing can be read or written there and no attacker controls it without root on `/workspace`), but these stale roots are exactly the class of "canonical vs. phantom" confusion this task set out to eliminate.

**Recommendation:** Wipe the aux build dirs (`build-check`, `build-diag`, `build-test`, `build-maintenance-verify`) once the canonical reconfig landed, or add a script check that fails if `CMAKE_HOME_DIRECTORY` does not equal the real project root. Do not ship these as authoritative.

---

### INFO — Test assertion uses `access()` whose semantics differ from production opens
**File:** `tests/test_icon_map.c` (updated line `assert_true(access(path, R_OK) == 0)`)

The replacement assertion is a reasonable functional check (the path resolves to something readable and, combined with `cbx_icon_map_default_path`'s `lstat` + `S_ISREG` guard, rejects symlinks). However `access(2)` follows symlinks and is itself TOCTOU-prone, so it would pass on a symlink pointing to a readable file that the production loader (`open(…, O_RDONLY | O_NOFOLLOW)`) would reject — a minor test-vs-production semantic mismatch.

**Recommendation:** Optionally assert the same regular-file/no-symlink semantics the loader enforces (`lstat(...)->st_mode` regular, not a link), so the test mirrors the real open path.

---

### INFO — TOCTOU between path selection and load
**Files:** `src/icons/icon_map.c` (`cbx_icon_map_default_path` lstat check → `cbx_icon_map_load` open)

A local actor able to write the install data directory could swap the resolved regular file between the `lstat` check and the subsequent `open`. This is mitigated by `O_NOFOLLOW` (blocks symlink swaps) and by the fact that write access to the data dir already implies the ability to replace the binary itself — no privilege escalation. Observation only.

---

### Review of the security-relevant code (no findings)
The icon-map path/data handling is well-defended and introduces no attack surface:
- `snprintf` output is length-bounded and checked for truncation; no `sprintf`/unchecked `strcpy`.
- `lstat` + `S_ISREG` rejects symlinks; `open(…, O_NOFOLLOW)` confirms it at load time.
- File size capped at `MAX_DOC_SIZE` (1 MB) before allocation; `malloc(fsize+1)` with `ftell`/`fread` validated — no integer overflow or overflow read.
- YAML parser uses bounded `safe_copy`, guards `map->count < CBX_ICON_MAP_MAX_ENTRIES` on entry insert, enforces a depth cap, and rejects YAML type/anchor tags (`check_event_tags`) — limits libyaml attack surface.
- `cbx_icon_map_default_path` validates `!out_path || path_size == 0` → `-EINVAL`, and `DATA_DIR` is a compile-time constant, not runtime/network attacker input.

### Conclusion
No **BLOCKER**. Two **WARN/INFO**-level observations (stale auxiliary build caches, minor test-semantics note) plus one TOCTOU **INFO**, none of which represent a vulnerability in this task's changes. The canonical build directory is correctly rooted at `/workspace/project` and executes real test binaries, satisfying the task acceptance.


---

## compatibility Audit

Audit complete. Report written to `.factory/artifacts/compat-audit-task-6.md`.

## Findings

### BLOCKER — `CMAKE_HOME_DIRECTORY` cannot be rewritten by reconfigure; stale phantom path breaks the acceptance command

`CMAKE_HOME_DIRECTORY` is an `INTERNAL` CMake cache value fixed at first configure. When the cache records a different source dir (the phantom `/workspace/controller-box`), re-running `cmake -S . -B build` does **not** heal it — CMake **aborts**:

```
CMake Error: The source "/workspace/project/CMakeLists.txt" does not match the
source "/workspace/controller-box/CMakeLists.txt" used to generate cache.
Re-run cmake with a different source directory.
```

I reproduced this deterministically **twice** — in an isolated copy and on the real canonical `build/` after faithfully restoring a stale-phantom cache (backup preserved, state restored afterward).

Consequences:
- The Task 6 verification command (`cmake -S . -B build … && grep CMAKE_HOME_DIRECTORY … && ctest …`) does not restore the correct path from a dirty tree — it only passes because the current cache happens to be freshly clean (ephemeral: `build/` is gitignored, and commit `1788c658` added no reconfigure guard).
- Four stale build trees still carry the phantom path: `build-test/`, `build-diag/`, `build-check/`, `build-maintenance-verify/`.

**Fix:** detect a mismatched `CMAKE_HOME_DIRECTORY` and remove/recreate `build/` before configuring, and delete the stale phantom trees.

### WARN — build must run inside `nix-shell`

`pkg_check_modules(SDL2 REQUIRED sdl2)` fails when CMake is invoked outside nix-shell ("No package 'sdl2' found"). Expected per AGENTS.md, but any verify step that forgets the wrapper fails misleadingly.

### INFO — no other API/ABI/dependency issues

CMake 4.3.4 vs `cmake_minimum_required(3.16)` is fine; Clang 21.1.8 Debug build clean; no SDL2/SDL3, sd-bus/dbus-1, or feature-macro conflicts in the task-6 path.

## Verdict

Acceptance holds **now** (verified: `CMAKE_HOME_DIRECTORY=/workspace/project`, `test_settings` Passed, real binary runs), but only because the canonical cache is momentarily clean. The stale-config failure mode is a genuine build-system compatibility defect and should be fixed before task 6 is considered robustly complete.


---

## functional Audit

**Exit 0.** Task 6 is functionally complete — the canonical `build/` directory now resolves `CMAKE_HOME_DIRECTORY` to the real `/workspace/project`, builds cleanly, and `ctest --test-dir build` runs real test binaries (91/91 pass with 2 legitimate environment skips). Only two non-blocking INFO observations, no BLOCKER or WARN findings.


---

## spec-compliance Audit

I have completed the audit. Here is my report.

---

# Spec Compliance Audit — Task 6: Reconfigure canonical build directory

## Scope
Task 6 acceptance: the canonical `build/` CMake cache resolves `CMAKE_HOME_DIRECTORY` to the real root `/workspace/project`, and `ctest --test-dir build` executes real test binaries.

## What I verified (ran the actual verification sequence)
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` → `Build files have been written to: /workspace/project/build` ✓
- `cmake --build build --parallel` → builds to completion (no `-Werror`/`bool` errors) ✓
- `grep CMAKE_HOME_DIRECTORY build/CMakeCache.txt` → `CMAKE_HOME_DIRECTORY:INTERNAL=/workspace/project` ✓
  - Also `SOURCE_DATA_DIR` now resolves to `/workspace/project/data` (was phantom `/workspace/controller-box/data`)
- `ctest --test-dir build -R '^test_settings$'` → `Passed`, executes the real binary (no file-not-found) ✓
- Dependent `test_icon_map` (Task 5's test, which Task 6 unblocks) now passes — the phantom-path coupling is gone ✓

The acceptance criteria are satisfied when the verification is run; the verification exits 0.

## Findings

**1. WARN — The canonical build cache was not left durably reconfigured; the "reconfigure" only holds through the configure step.**
When I first inspected `build/CMakeCache.txt` at audit time (before re-running the configure step), it still read:
```
CMAKE_CACHEFILE_DIR:INTERNAL=/workspace/controller-box/build
CMAKE_HOME_DIRECTORY:INTERNAL=/workspace/controller-box
```
i.e. the resting state still had the phantom path. The committed task-6 evidence in `.factory/artifacts/implementation-plan.md` claims "the phantom `build/` cache was wiped and reconfigured," but that state was not present on disk when I checked — only re-running the configure command produced `/workspace/project`. Because `build/` is gitignored (`build/` in `.gitignore`), no durable artifact of the reconfigure is version-controlled; the task-6 commit (`1788c658`) contains only a doc change plus the `test_icon_map.c` fix.
Recommendation: confirm no process/invocation reconfigures `build/` against the phantom path, and document that the acceptance is realized through, and only through, the documented configure step (it cannot be committed). No code path in `CMakeLists.txt`, `cmake/`, or `scripts/` references the phantom path, so a fresh checkout always resolves correctly.

**2. INFO — Task-6 commit touches `test_icon_map.c`, a Task-5 concern; Task 5's plan entry is now stale.**
`git show 1788c658` changes `tests/test_icon_map.c:296` — `test_default_path` had asserted `strstr(path, "controller-box") != NULL` (coupled to the dir name), changed to a semantic `access(path, R_OK) == 0` check. This is the defect Task 5 still lists as "pending/unconfirmed," and the change is a genuine improvement (semantic outcome over name-substring coupling, consistent with AGENTS.md "assert semantic outcomes"). Not a blocker, but `.factory/artifacts/implementation-plan.md` Task 5's evidence is now contradicted/superseded and should be reconciled.

**3. INFO — `test_installed_binary` failed once in a broad concurrent ctest run but passes in isolation (26 s).**
Outside task-6's scope (an installed-package packaging test, not a test-binary-resolution check), it is relevant to Task 7's "no flaky failures" gate. Not a task-6 blocker.

## Recommendation note on the test change
The task-6 commit legitimately fixed a test that was only passing because the phantom build-dir name happened to embed `"controller-box"`; changing it to a semantic `access` check is correct and not a weakened assertion.

---

**Conclusion:** No **BLOCKER** findings. Task 6's defined acceptance (verification command exits 0; cache resolves to `/workspace/project`; representative tests execute) is met. The items above are WARN/INFO for the orchestrator's awareness.

Exiting 0.

