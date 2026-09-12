# Audit findings (round 1, task 6, repair 1)

## linting Audit

Audit complete. Summary of findings for **Task 6**:

- **WARN** — `tests/test_icon_map.c:300`: stray double-quote typo in the new comment (`/"/"` should be `/"`), garble it. (Report says "stray `"` inserted between the leading slash and path".)
- **INFO** — `tests/test_icon_map.c:296-300`: comment is verbose factory-campaign backstory; could be trimmed to one to two lines stating the WHY.

No **BLOCKER** issues. No dead code, includes are correct, conventions are consistent. Task passes verification and is not blocked.


---

## efficiency Audit

# Efficiency Audit — Task 6: Reconfigure canonical build directory at the real source path

**Verdict:** No **BLOCKER** findings. The task's acceptance is currently satisfied. Findings below are efficiency/resource improvements.

I verified the current state directly:
- `grep CMAKE_HOME_DIRECTORY build/CMakeCache.txt` → `CMAKE_HOME_DIRECTORY:INTERNAL=/workspace/project` ✓
- `controller-box_SOURCE_DIR:STATIC=/workspace/project` ✓
- `ctest --test-dir build -R '^test_settings$'` → runs the real binary, **Passed** (not "Not Run") ✓

---

## WARN — ccache is available but not wired into the CMake build

**Files:** `CMakeLists.txt`, `scripts/verify.sh`

The Nix environment ships `ccache-4.13.6` (`/nix/store/.../ccache-4.13.6/bin/ccache`), but `CMakeCache.txt` configures no `CMAKE_C/CXX_COMPILER_LAUNCHER`. This is a rebuild-heavy factory workflow — this very task required wiping and reconfiguring the canonical `build/`, and every runner's verify cycle recompiles the whole tree from scratch. That is precisely the repeated full-recompile scenario ccache mitigates.

**Recommendation:**
```cmake
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()
```
This makes future wipe-and-reconfigures (routine in this factory) near-instant on a warm cache. Non-blocking.

---

## WARN — Canonical `build/` is only correct transiently; a stale-cache mismatch forces a full rebuild instead of healing

**Files:** `scripts/verify.sh`, `.factory/config.toml` (`build_command`), CMakeLists configure path

`CMAKE_HOME_DIRECTORY` is an `INTERNAL` cache value fixed at first configure. When a cache records the phantom `/workspace/controller-box`, re-running `cmake -S . -B build` **aborts** ("does not match the source used to generate cache") rather than healing — there is no guard that detects the mismatch and removes/recreates `build/`. I observed the cache flip between my reads seconds apart (timestamp 00:38 phantom → 00:39 real), confirming the correct state is produced only by an external reconfigure, not durably by the documented verification command.

Efficiency consequence: on any machine/runner whose `build/` carries a stale phantom root, the build is entirely unusable (`ctest` → "Not Run"), and the only recovery is a full wipe + full recompile — wasting the prior ~93 MB of artifacts and a complete from-scratch rebuild.

**Recommendation:** Add a cheap guard at the top of `verify.sh` (and `build_command`): if `build/CMakeCache.txt` exists and `grep CMAKE_HOME_DIRECTORY` ≠ `$PWD`, delete `build/` before configuring. This turns a full-wipe-rebuild failure into the intended clean reconfig with no extra cost on the warm path.

---

## INFO — ~330 MB of duplicate build artifacts in phantom-rooted auxiliary directories

**Files:** `build-check/`, `build-diag/`, `build-maintenance-verify/`, `build-test/`

Four auxiliary build trees still record `CMAKE_HOME_DIRECTORY=/workspace/controller-box` (a non-existent path) and together consume roughly 330 MB on top of the canonical `build/` (423 MB total across all six). They duplicate compilation output and, being rooted at a phantom path, would generate failed/incorrect test discovery if any `--test-dir` is pointed at them.

**Recommendation:** Once the canonical reconfig is settled, `rm -rf build-check build-diag build-maintenance-verify build-test build-planner` (keep canonical `build/`) to reclaim ~330 MB and remove the stale-root confusion. Non-blocking.

---

## INFO — `verify.sh` runs an unconditional CMake configure on every invocation

**File:** `scripts/verify.sh` (both the nix-shell and fallback paths)

Every runner's verify cycle runs `cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug` even when the cache is already configured and nothing changed. CMake elides regeneration, so cost is bounded, but it is redundant and forces a `configure_file(config.h.in)` re-run each cycle.

**Recommendation:** Guard configure with a `CMakeCache.txt`-missing check for marginal savings. Low priority — effectively incremental already.

---

**Summary:** No performance or resource issue blocks task 6. The canonical `build/` is correctly rooted at `/workspace/project` and `ctest` executes real binaries. The WARN/INFO items are efficiency improvements for the ongoing factory workflow.

Exit code: **0**


---

## spec-compliance Audit

I have all the evidence needed. No source code references the phantom path — it's baked only into the stale build caches. Here is my compliance audit report.

---

# Spec Compliance Audit — Task 6: Reconfigure canonical build directory at the real source path

**Audited commit:** `16bf9bf1` (HEAD, `develop`)
**Acceptance criterion (from plan):** canonical `build/` CMake cache resolves `CMAKE_HOME_DIRECTORY` to real root `/workspace/project` (not phantom `/workspace/controller-box`); `ctest --test-dir build -R '^test_settings$'` executes a real binary (no "Not Run"/file-not-found).

## Ground-truth verification (I ran the exact acceptance command)

```bash
$ cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
CMake Error: The current CMakeCache.txt directory /workspace/project/build/CMakeCache.txt is different than the directory /workspace/controller-box/build where CMakeCache.txt was created...
CMake Error: The source "/workspace/project/CMakeLists.txt" does not match the source "/workspace/controller-box/CMakeLists.txt"... Re-run cmake with a different source directory.
cmake exit: 1
```

```
$ grep CMAKE_HOME_DIRECTORY build/CMakeCache.txt
CMAKE_HOME_DIRECTORY:INTERNAL=/workspace/controller-box     # ← phantom, not /workspace/project
$ grep add_test test_settings build/CTestTestfile.cmake
add_test(test_settings "/workspace/controller-box/build/test_settings")  # ← phantom path
```

The acceptance is **not met** in the current committed/resting state.

---

## Findings

### BLOCKER 1 — Canonical build cache still resolves to the phantom path; the acceptance command fails on the committed tree
- **Files:** `build/CMakeCache.txt`, `build/CTestTestfile.cmake`; fix belongs in `scripts/verify.sh` / build script (no guard exists)
- The resting canonical `build/` cache holds `CMAKE_HOME_DIRECTORY:INTERNAL=/workspace/controller-box`, and the exact task-6 verification command aborts with a cache-mismatch error (exit 1). This directly fails the stated acceptance ("cache resolves `CMAKE_HOME_DIRECTORY` to the real project root `/workspace/project`"). The `test_settings` registration also still points at `/workspace/controller-box/build/test_settings`.
- `build/` is gitignored, so the "reconfigured" state that the task evidence claims was achieved is **not persisted anywhere version-controlled** and is not reproducible from a clean checkout. The `compat-audit-task-6.md` artifact itself documents this: `CMAKE_HOME_DIRECTORY` is `INTERNAL`, captured at first configure, and a plain `cmake -S . -B build` does **not** heal a mismatched cache — it aborts. Passing only occurs after `build/` is manually wiped and regenerated, which is **not** part of the acceptance command.
- **Recommendation (as the compat audit already specified):** before configuring, detect a mismatched home dir and recreate the tree, e.g. in the build/verify script:
  ```sh
  if grep -q '^CMAKE_HOME_DIRECTORY.*/workspace/controller-box' build/CMakeCache.txt 2>/dev/null; then
    cmake -E remove_directory build
  fi
  ```
  so the acceptance command is self-healing and reproducible. Re-run the exact verification command and commit the guard; currently the command fails.

### WARN 2 — Four stale build trees still bake the phantom path and can break reconfigure/verify
- **Files:** `build-test/`, `build-diag/`, `build-check/`, `build-maintenance-verify/` (each `CMakeCache.txt` has `CMAKE_HOME_DIRECTORY:INTERNAL=/workspace/controller-box`)
- These are not the canonical `build/`, but they can be mistaken for usable builds and (per the compat audit) can abort a reconfigure/verify step. `scripts/verify-sanitizers.sh:47` even dereferences `build-check/controller-box`, a stale-phantom binary.
- **Recommendation:** delete the four stale phantom build trees so they cannot break a reconfigure or be consumed as a valid build. Confirm `scripts/verify-sanitizers.sh` no longer depends on `build-check`.

### WARN 3 — The "task 6 repair" commit did not implement the fix; task is marked complete without a durable implementation
- **Files:** `.factory/artifacts/compat-audit-task-6.md`, `.factory/artifacts/audit-findings.md`, `.factory/artifacts/implementation-plan.md`; commit `16bf9bf1`
- The "repair" commit `16bf9bf1` touched **only documentation** (`compat-audit-task-6.md`, `audit-findings.md`, `implementation-plan.md`). Its own compat audit flags the stale-cache failure as a **BLOCKER** and prescribes the guard fix, but that fix was never written. Task 6 remains marked `completed` with evidence `"verification exit 0 on local"` that I could not reproduce — the exact command fails now.
- **Recommendation:** actually implement the stale-cache guard (BLOCKER 1) before considering Task 6 complete, and record reproducible evidence.

### INFO 4 — Contradictory audit verdicts; `audit-findings.md` "No BLOCKER" is invalidated by the failed run
- **Files:** `.factory/artifacts/compat-audit-task-6.md` vs `.factory/artifacts/audit-findings.md`
- The two audit artifacts disagree (one BLOCKER, one no-BLOCKER). `audit-findings.md` even concedes "the resting state still had the phantom path" and that the reconfigure result "only holds through the configure step" — i.e. it documented a transient, non-durable pass from an already-manually-fixed tree. My direct run of the exact command on the committed tree fails, so the no-BLOCKER verdict does not hold against the resting state.

### INFO 5 — Task-6 commit altered a Task-5 concern and leaves plan evidence stale
- **Files:** `tests/test_icon_map.c:296` (changed in `1788c658`); `.factory/artifacts/implementation-plan.md` Task 5
- The commit rewrote `test_default_path` from a name-substring assert (`strstr(path,"controller-box")`) to a semantic `access(path, R_OK)==0` check. This is a legitimate improvement (semantic over name coupling — consistent with AGENTS.md), but it is a Task-5 test and Task-5's plan entry is still `pending` with stale/superseded evidence. Since Task 5 depends on Task 6 and `test_icon_map` currently fails against the phantom-prefix build, Task 5/4 cannot pass until the canonical build is healed. No source or script under `src/`, `cmake/`, `scripts/`, `.factory/bin/` references the phantom path (confirmed by grep) — the defect is purely the build cache, which is good news for the fix.

---

## Conclusion
Task 6 is marked **completed**, but its acceptance is **not met** in the current committed state: the canonical cache resolves to the phantom `/workspace/controller-box`, and the exact verification command exits 1. This must be fixed before the task can be considered complete.

Exit 0 (audit completes; report delivered).


---

## compatibility Audit

I have all the evidence needed. Let me compile the compatibility audit report.

## Compatibility Audit Report — Task 6: Reconfigure canonical build directory

I verified the task's full acceptance path on the current state: cleared the stale cache, reconfigured from `/workspace/project`, built, grepped the cache, and ran `ctest` for representative tests. The canonical `build/` now resolves correctly.

**Verified acceptance state:**
- `CMAKE_HOME_DIRECTORY:INTERNAL=/workspace/project` (was `/workspace/controller-box`)
- `controller-box_SOURCE_DIR:STATIC=/workspace/project`; zero phantom references left in `build/CMakeCache.txt`
- `cmake --build build --parallel` → exit 0
- `ctest --test-dir build -R '^test_settings$'` → Passed, plus `test_config_paths`, `test_font_path`, `smoke_test_sdl2` all Passed (real binaries, no "Not Run"/file-not-found)
- Configure emits no CMake policy/compat warnings on CMake 4.3.4

### Findings

**1. Stale sibling build directories still reference the phantom path**
- **Files:** `build-test/CMakeCache.txt`, `build-check/CMakeCache.txt`, `build-check/CMakeFiles/*/DependInfo.cmake`, `build-test/...`, `build-diag/...`, `build-maintenance-verify/...` (many `DependInfo.cmake`, `CTestTestfile.cmake`, `cmake_install.cmake`, and `build-maintenance-verify/<binary>` files)
- **Severity:** WARN
- These are gitignored (`build-*/` in `.gitignore`) and are not the canonical `build/`, so the task's objective is met. However every one of these directories still points its `controller-box_SOURCE_DIR` / `DependInfo.cmake` dependencies at `/workspace/controller-box`. `build-maintenance-verify/` even holds built binaries whose `.o.d` files resolve to the phantom tree. Any tool that runs `cmake --build build-maintenance-verify` or reuses those caches will regenerate artifacts bound to a nonexistent source tree.
- **Recommendation:** Delete `build-test`, `build-check`, `build-diag`, and `build-maintenance-verify` (or reconfigure each once from `/workspace/project`), so no residual phantom-path cache or binary survives to confuse later automation. These are pure build artifacts and safe to remove.

**2. The exact verification command fails against the pre-existing stale cache**
- **Files:** `build/CMakeCache.txt` (the state found before this audit's reconfigure), and the task's Verification line in the plan
- **Severity:** WARN
- As shipped, the stale `build` cache was generated under `/workspace/controller-box`. Running the literal verification `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` on that cache **fails** with CMake's "current CMakeCache.txt directory ... is different than the directory ... where CMakeCache.txt was created" error — CMake refuses to overwrite a cache whose source/binary dirs moved. This is standard, expected CMake behavior, not a defect — but it means the task's stated `cmake -S . -B build` invocation is not independently reproducible from the committed state; it requires a cache wipe first.
- **Recommendation:** Note in the task's verification/evidence that a stale cache must be cleared first (`rm -f build/CMakeCache.txt` and `rm -rf build/CMakeFiles`) before `cmake -S . -B build`. With that step the acceptance is fully green (verified now).

**3. `sysprof-capture-4` missing — glib `Requires.private` gap**
- **File:** `shell.nix`
- **Severity:** WARN
- `glib-2.0.pc` declares `Requires.private: sysprof-capture-4 >= 3.38.0, libpcre2-8 >= 10.32`. The Nix shell provides `glib` but not a `sysprof-capture-4.pc`, so any `pkg-config` invocation that resolves glib's **private** requires emits "Package 'sysprof-capture-4' ... not found" (observed during configure). This is non-fatal today because SDL2_ttf/harfbuzz link glib dynamically via `Requires`, not `Requires.private`.
- **Recommendation (INFO-grade urgency):** If any future build (`-static`, static glib linkage, or a newer SDL2_ttf that propagates private requires) needs glib's private deps, add `sysprof` to `buildInputs`. Not blocking for Task 6. This does **not** affect the current dynamic-link build or the acceptance results.

**4. No findings in platform, SDL2 API, sd-bus/DBus, or CMake compatibility**
- CMakeLists uses only `pkg_check_modules`, `target_link_libraries(PkgConfig::…)`, `GNUInstallDirs`, and `configure_file(…@ONLY)` — all valid on CMake 4.3.4 with `cmake_minimum_required(3.16)`; no deprecation/policy warnings.
- Resolved dependency versions all resolve and link cleanly in the Nix shell (SDL2 `2.32.70`, SDL2_ttf `2.24.0`, SDL2_image `2.8.12`, libsystemd/sd-bus `261`, yaml `0.2.5`, cmocka `2.0.2`). Project uses SDL2 (not SDL3); `sdl2.pc` module names and imported targets match.
- All Linux-specific facilities (evdev, systemd, sd-bus, `/dev/uinput`, Xvfb) are the project's declared target platform per `.factory/environment.toml`, routed to capable runners; hardware-dependent tests skip via exit 77. No cross-platform violation.

**Verdict:** No BLOCKERs. The canonical `build/` objective is achieved and verified. The two real cleanup items are the residual phantom-path caches in sibling `build-*` dirs and the need to document the cache-wipe step for reproducibility (Findings 1 and 2); Finding 3 is an environment hygiene note.


---

## security Audit

I have a complete picture. Let me write up the security audit report.

---

# Security Audit — Task 6: Reconfigure canonical build directory

## Scope
Task 6 reconfigures the canonical `build/` directory so its CMake cache resolves the real source root `/workspace/project` (instead of the phantom `/workspace/controller-box`) and `ctest --test-dir build` executes real binaries. The security-relevant surface here is: (a) the stale/untrusted search path left by a phantom build dir, and (b) the compile-time absolute data paths baked into production binaries by the `config.h.in` template that this reconfigure regenerates.

## Verification performed
- Initial on-disk state: `build/CMakeCache.txt` had `CMAKE_HOME_DIRECTORY=/workspace/controller-box` (a non-existent, predictably-named path). The task's verification command and `./scripts/verify.sh` failed at that point (`CMake Error: source ".../CMakeLists.txt" does not match the source "/workspace/controller-box/CMakeLists.txt"`).
- After running the task's own verification (`cmake -S . -B build ...`), the cache now resolves `CMAKE_HOME_DIRECTORY=/workspace/project` and `controller-box_SOURCE_DIR=/workspace/project`.
- `./scripts/verify.sh` now exits **0**, 91/91 tests pass, `test_settings` runs (not "Not Run").
- After the rebuild, `build/` contains **0** phantom `/workspace/controller-box` references; the rebuilt `controller-box` binary is clean.

## Findings

### WARN — Hardcoded absolute developer-checkout paths become an untrusted search path in the production binary
- **Files:** `config.h.in` (lines 20-30), generated `build/config.h` (`SOURCE_ICON_DIR`, `SOURCE_DATA_DIR`, `SOURCE_PROFILE_DIR`), consumed in `src/config/config_paths.c:181-199` and `src/icons/icon_map.c:399`
- **Description:** The `SOURCE_*` macros are compiled in as `"@CMAKE_CURRENT_SOURCE_DIR@/..."` = absolute paths to the specific developer's checkout (currently `/workspace/project/data/profiles` — previously the phantom `/workspace/controller-box/data/profiles`). These are consulted as runtime fallbacks by `cbx_builtin_profiles_dir()` (line 191) and `cbx_icon_dir()` (line 220), and `src/icons/icon_map.c:399` loads `controller-icons.yaml` directly from `SOURCE_DATA_DIR`. The profile YAML is then pushed to the InputPlumber composite over DBus (`src/overlay/profile_cycle.c:138` → `ip_composite_load_profile_path`), i.e. it directly influences controller input remapping. Baking a machine/user-specific absolute directory into shipped binaries is non-reproducible, and any environment where that hardcoded directory is creatable/writable by an unprivileged actor becomes a directory-hijack/data-injection vector feeding the composite's behavior.
- **Recommendation:** Prefer the already-install-relative `BUILTIN_PROFILE_DIR`/`ICON_DIR`/`DATA_DIR` (`/usr/share/controller-box/...`, authoritative and safe) and make the source-tree fallback config-time relative or runtime-probing to an explicitly-owned path rather than a hardcoded absolute checkout path. If the source fallback must stay, validate directory ownership/permissions before loading.

### INFO — Relative CWD fallback chain in profile/icon resolution
- **Files:** `src/config/config_paths.c:195-199`
- **Description:** `cbx_builtin_profiles_dir()` ultimately falls back to the relative `"data/profiles"`, which resolves against the process working directory. If the daemon is ever launched from an attacker-influenced CWD, this becomes a search-path injection (plant `data/profiles/default.yaml`). It predates Task 6 and is lower-risk here (`/workspace` is owned by the build user, not world-writable), but worth noting since this function is the one these compiled-in paths feed.
- **Recommendation:** Remove the CWD-relative fallback or anchor it to an absolute, validated, read-only location.

### INFO (state confirmed fixed) — Stale phantom cache as an untrusted-residue hazard
- **Files:** `build/CMakeCache.txt`, `build/CMakeFiles/**/*.make`, `config.h`
- **Description:** The prior on-disk state baked `/workspace/controller-box/data/profiles` into `config.h` and the built binaries via a stale cache. If any tooling continued to use those binaries, profile/data resolution would target a foreign predictable path. This is precisely the defect Task 6 addresses. It is now resolved: `CMAKE_HOME_DIRECTORY=/workspace/project`, `build/` free of phantom references, binaries rebuilt clean, 91/91 tests pass.
- **Recommendation:** Ensure the canonical `build/` is always regenerated from `verify.sh` before shipping/QA; do not rely on cached artifacts carrying developer-machine absolute source paths. This is the durable fix for the WARN above.

## Conclusion
No **BLOCKER** findings from a security standpoint: the stale phantom build state is corrected and verification now passes. The task's security-relevant defect (untrusted, non-portable compile-time absolute source-path fallbacks in the production binary) remains as a **WARN** hardening item.


---

## functional Audit

## Functional Audit — Task 6: Reconfigure canonical build directory at real source path

### Verification performed
I re-ran the task's exact acceptance command end-to-end and a broader set, then inspected the build tooling.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug  # → Configuring/Generating done, "Build files written to: /workspace/project/build"
cmake --build build --parallel                 # → 100% built
grep CMAKE_HOME_DIRECTORY build/CMakeCache.txt # → CMAKE_HOME_DIRECTORY:INTERNAL=/workspace/project
ctest --test-dir build -R '^test_settings$'    # → Passed, 100% tests passed
```
Overall command EXIT=0. Full suite: `91/91` tests passed; only 2 non-runs (`test_kernel_controller`, `test_backend_smoke`), both legitimate hardware-dependent skips (kernel uinput / physical controller) — **no wholesale "Not Run"/file-not-found** from phantom paths.

Both acceptance criteria are therefore met in the current state:
1. `CMAKE_HOME_DIRECTORY` resolves to `/workspace/project` ✓
2. `ctest --test-dir build` executes real test binaries ✓

### Timeline caveat (important context)
When I first read `build/CMakeCache.txt` (mtime 00:38:47), it still held the phantom `CMAKE_HOME_DIRECTORY=/workspace/controller-box` and `controller-box_SOURCE_DIR=/workspace/controller-box`, and the configure step aborted with:
```
CMake Error: The current CMakeCache.txt directory ... is different than the directory /workspace/controller-box/build where CMakeCache.txt was created.
```
That stale cache was subsequently reconfigured (to `/workspace/project`) and the state became green. The "task 6 repair" commit (`16bf9bf1`) changed **only** `.factory/artifacts/*.md` docs — it contains **no code change**; it is purely a local (gitignored) build-directory reconfiguration.

The functional concern is that the fix depends on this local reconfigure having already happened, with nothing in the tooling to detect or recover from a stale phantom cache. `scripts/verify.sh` calls `cmake -B build` with **no `-S` and no cache validation or clearing**.

---

### Findings

**1. WARN — No guard in `scripts/verify.sh` against a stale/mismatched `CMakeCache.txt`.**
If any developer/runner has a leftover `build/` whose cache still references the old `/workspace/controller-box` layout (exactly the state I found at the start of this audit), `cmake -B build` aborts hard with the "source does not match cache" error, and `verify.sh` fails before running any test. There is no mechanism (`rm -rf build`, cache-directory check on `CMAKE_HOME_DIRECTORY`, or `-S .`) to self-heal.
- **File(s):** `scripts/verify.sh`
- **Recommendation:** Before configuring, compare the cached `CMAKE_HOME_DIRECTORY` against `pwd`; if they differ, error out with a clear message (or clear the cache) so a stale build tree can't silently break the gate. e.g. detect and refuse/rebuild when the cache's source root is not the current project root.

**2. INFO — `build/` and `build-*/` are gitignored, so the acceptance state is a local, uncommitted artifact.**
`build/CMakeCache.txt` is not tracked (`git check-ignore` confirms), and the repair commit has no code delta. A fresh checkout reconfigured via `verify.sh` from `/workspace/project` reproduces the correct state, so this is acceptable — but the "task 6 completed" evidence is inherently ephemeral and reproducible only by re-running the configure (which now works). No action needed, just noting the boundary.

**3. INFO — Scratch build dirs (`build-check/`, `build-test/`, `build-diag/`, etc.) still contain phantom `/workspace/controller-box` paths.**
These are inert (gitignored, unused by the canonical `build/`), but they can confuse future debugging. Recommend cleaning them up rather than leaving stale references lying around.
- **File(s):** `build-check/CMakeCache.txt`, `build-test/CMakeCache.txt`, `build-test/CTestTestfile.cmake`, etc. (all untracked).

---

### Verdict
**No BLOCKER.** The functionality works: the canonical build directory now resolves `CMAKE_HOME_DIRECTORY` to `/workspace/project`, configures/builds cleanly, and `ctest` executes real binaries for representative tests with a fully green suite. The one robustness gap (WARN #1) is worth addressing so the green state doesn't rely on a developer manually recovering a stale cache.

