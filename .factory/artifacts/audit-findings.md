# Audit findings (round 1, task 6, repair 2)

## security Audit

Now `test_icon_map` runs and passes (the earlier "No tests found" was transient ctest state; the binaries are real and all 91 tests are registered). The full picture is consistent: `CMAKE_HOME_DIRECTORY=/workspace/project`, targeted tests run real binaries and pass.

I have completed the security audit of task 6. Here is my report.

---

# Security Audit — Task 6: Reconfigure canonical build directory at the real source path

## Scope reviewed
- `tests/test_icon_map.c` — the `test_default_path` assertion change (the only production-facing code touched by this task).
- `src/icons/icon_map.c` — `cbx_icon_map_default_path()` (the function under test, changed behavior is observable through it).
- `src/icons/icon_map.h`, `config.h.in` and the generated `build/config.h` / `build-planner/config.h`.
- Build state: `build/CMakeCache.txt` (`CMAKE_HOME_DIRECTORY`), CTest registration/executability.

## Security findings

No BLOCKER or WARN findings.

The task is security-neutral in its core change (a CMake reconfiguration of the canonical build directory). The path data feeding the touched code is compile-time constants, not attacker input, and the buffer/symlink handling is sound:

- `cbx_icon_map_default_path()` builds paths from the compile-time macros `DATA_DIR` / `SOURCE_DATA_DIR` (`config.h.in`). There is **no user-controlled input**, so no path-traversal or injection surface.
- Both `snprintf` calls check the return against the buffer size and return `-ENAMETOOLONG` on truncation — no unbounded write, no off-by-one in `%s` handling with a null-terminated bounded buffer.
- The function explicitly rejects symlinks via `lstat()` + `!S_ISLNK()` before selecting a data file — a legitimate defensive measure against symlink substitution in the data dir.
- `build/CMakeCache.txt` resolves `CMAKE_HOME_DIRECTORY=/workspace/project` (not the phantom `/workspace/controller-box`), test binaries are real and executable, and `test_settings` + `test_icon_map` execute and pass — the task's acceptance criteria hold.

## INFO observations (non-blocking)

1. **TOCTOU on data-path selection** — `src/icons/icon_map.c:394-401`
   The function `lstat()`-validates a non-symlink regular file, returns the path, and the *caller* opens it separately (`overlay_service.c:1473`, `profile_editor_list.c:283`). A local actor able to write to `SOURCE_DATA_DIR`/`DATA_DIR` between the check and the open could race in a symlink. This is not exploitable in the stated threat model (install prefix `/usr/share/controller-box` and the source tree are not attacker-writable; the consumer only parses read-only YAML icon data, no privilege boundary is crossed), so I rate it INFO. If the data locations ever become user-writable or the file is ever loaded with elevated privileges, re-open via `open(... O_NOFOLLOW)`.

2. **`access(path, R_OK)` under root — `tests/test_icon_map.c` (test change)**
   `access()` uses real credentials and reports success for euid 0 regardless of permission bits, so under a root test runner the "readable" assertion is weaker than under a normal user. This is a test-observation nuance, not a vulnerability — the function under test already established `S_ISREG` and non-symlink. If stricter semantics are ever desired, use `faccessat(AT_EACCESS, ...)`. No action required for security.

3. **Compile-time absolute `SOURCE_DATA_DIR "/workspace/project/data"`** — `config.h.in:21`
   Baked at configure time. This is a deployment/correctness consideration (a differently-located install must be reconfigured), not a security issue.

## Conclusion
No security vulnerabilities or attack-surface regressions introduced by this task. The changed test assertion is safe and correctly exercises the resolved default path. Task 6 may be considered complete from a security standpoint.


---

## linting Audit

## Linting Audit — Task 6: Reconfigure canonical build directory

Scope: the code changes introduced by this task — `scripts/verify.sh` (self-heal block) and `tests/test_icon_map.c` (`test_default_path` + comment). I reviewed the diff (`1788c658`, `16bf9bf1`, `6dd7a095`) and the current file state.

**No BLOCKER findings.** The changed code is internally consistent, properly included, and free of dead code.

---

### WARN

**1. Unescaped path interpolated into a grep regex**
- **File:** `scripts/verify.sh:30`
- The self-heal condition greps `"^CMAKE_HOME_DIRECTORY:INTERNAL=$PROJECT_ROOT\$"`. `$PROJECT_ROOT` is spliced into a Basic Regular Expression without escaping. For the current path (`/workspace/project`) every character is a literal, so it works today — but a checkout located under any directory whose path contains BRE metacharacters (`.`, `+`, `[`, `(`, `*`, `^`, `|`, `{`, etc.) would silently mismatch, either nuking a healthy `build/` tree on every run or failing to self-heal a stale one. This is a generic CI script meant to run identically on all runners, so it should be path-hostile regardless of where the repo lands.
- **Recommendation:** match by exact string comparison instead of regex, e.g.:
  ```bash
  home_dir=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD_DIR/CMakeCache.txt")
  if [ "$BUILD_DIR" = "build" ] && [ -f "$BUILD_DIR/CMakeCache.txt" ] && \
     [ "$home_dir" != "$PROJECT_ROOT" ]; then
      ...
  fi
  ```

**2. Line exceeds 100 columns**
- **File:** `scripts/verify.sh:30` (107 chars)
- The grep invocation spills past the file's otherwise disciplined width.
- **Recommendation:** split the condition into multiple `&&`-chained lines (as the self-heal block already does elsewhere), or fold the check into a small helper function with the `sed` approach above.

---

### INFO

**1. Redundant `cd -- "$PWD"`**
- **File:** `scripts/verify.sh:17`
- `PROJECT_ROOT=$(cd -- "$PWD" && pwd -P)` runs immediately after `cd -- "$(dirname -- "$0")/.."`, so `$PWD` is already the project root. `PROJECT_ROOT=$(pwd -P)` expresses the same intent in one command. Harmless, but the extra command reads as accidental.

**2. Verbose self-heal comment (borderline acceptable)**
- **File:** `scripts/verify.sh:19-27`
- An 8-line comment precedes the self-heal. It genuinely explains *why* (a non-obvious CMake cache quirk: `CMAKE_HOME_DIRECTORY` is pinned at first configure and a source/binary mismatch aborts reconfiguration) rather than restating the code, so I would not cut it outright. It could be trimmed to ~4 lines without losing the rationale. The more useful addition is a one-line note explaining *why custom build dirs are intentionally skipped* — the guard `[ "$BUILD_DIR" = "build" ]` is non-obvious yet currently unexplained (an important design choice, since the self-heal nukes the directory).

**3. `test_icon_map.c` change is clean**
- **File:** `tests/test_icon_map.c:296-299`
- The old comment (with a stray `/"` typo and stale `controller-box` literal) was replaced with a concise, correct comment, and the brittle `strstr(path, "controller-box")` check became a semantic `access(path, R_OK) == 0`. `unistd.h` (for `access`) and `string.h` (for `strstr`) are already included; no new includes needed, no dead code left behind. Good.

**4. Build-state observation (outside lint scope)**
- The current `build/` tree is a partial artifact (its `CMakeCache.txt`/`config.h` consistently resolve `CMAKE_HOME_DIRECTORY` and `SOURCE_DATA_DIR` to `/workspace/project`, which satisfies this task's acceptance — but several test executables are absent, so `ctest -N` currently reports "Could not find executable" for them). This looks like an interrupted background rebuild rather than a defect in the changed code; it belongs to evidence verification, so I have not treated it as a lint finding.

---

### Summary
No mandatory fixes. Two recommendations (regex escaping — the only robustness risk — and line width) and four observations. The `verify.sh` self-heal logic is functionally sound for the canonical path and the test change is a genuine readability/assertion improvement.


---

## efficiency Audit

I have enough evidence. Here is my efficiency audit.

---

# Efficiency Audit — Task 6: Reconfigure canonical build directory at the real source path

**Verdict: No BLOCKER findings.** The task's core acceptance is met in the current state:
- `CMAKE_HOME_DIRECTORY:INTERNAL=/workspace/project` and `controller-box_SOURCE_DIR:STATIC=/workspace/project` ✓ (not the phantom `/workspace/controller-box`)
- `ctest --test-dir build -R '^test_settings$'` built the real binary and **Passed** (0.04s), not "Not Run"/file-not-found ✓
- The prior round's key WARN (no stale-cache guard) is **resolved**: `scripts/verify.sh` now self-heals by dropping a cache pinned to a stale root (`cmake -E remove_directory`), and the phantom auxiliary trees `build-check/ build-diag/ build-maintenance-verify/ build-test/` have been cleaned up.

Remaining efficiency/resource findings:

---

## WARN — ccache is still not wired into the CMake build despite a rebuild-heavy factory

**Files:** `CMakeLists.txt`, `scripts/verify.sh`

`ccache 4.13.6` ships in the Nix environment but `build/CMakeCache.txt` sets no `CMAKE_C_COMPILER_LAUNCHER` (grep count 0). The workflow's self-heal does a full wipe-and-recompile (`cmake -E remove_directory build/`), and every runner's verify cycle recompiles the whole tree. This is the exact repeated-full-compile scenario ccache is for — a fresh cold rebuild (the 5,000+ files compiled here) would be near-instant on a warm ccache.

**Recommendation:**
```cmake
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()
```
Non-blocking, but it directly amortizes the cost of task 6's wipe-and-reconfigure recovery path.

---

## WARN — The forced cold rebuild is brittle: full `cmake --build build --parallel` currently exits nonzero on pre-existing `-Werror` gates

**Files:** `tests/test_assign.c` (lines 125, 367), `tests/test_profile_diagram.c` (lines 692, 697), `scripts/verify.sh` (self-heal)

The self-heal discards the existing healthy binary tree and recompiles from scratch with the *current* toolchain (this Nix shell's **Clang 21.1.8**). That fresh build now fails on:
- `-Werror -Wuninitialized-const-pointer` in `test_assign.c:125, :367`
- `-Werror -Wunused-but-set-variable` in `test_profile_diagram.c:692, :697`

The inherited `build/` (which had a complete artifact set when I started) had masked these. The pre-wipe tree was compiled under a toolchain that did not flag them; the wipe rebuild surfaces them, so the task's literal verification step `... && cmake --build build --parallel && ...` currently returns nonzero.

This is not task-6's own diff, but it directly impacts the efficiency/robustness of the recovery path task 6 depends on, and it can trip the "verification exit 0" gate. The representative acceptance target (`test_settings`) builds and runs cleanly, so acceptance criterion #2 is satisfiable today.

**Recommendation:** Either (a) scope the task's build step to the representative target (`cmake --build build --target test_settings`) since that is what `ctest -R '^test_settings$'` actually needs, or (b) get the two `-Werror` violations in `test_assign.c`/`test_profile_diagram.c` fixed under the current compiler so a full parallel build succeeds. The `-Wunused-but-set-variable` `cnt`/`bcnt` are genuinely dead counters in a pixel-scan loop and can be removed.

---

## INFO — Auxiliary phantom-rooted build tree `build-planner/` still present (~93 MB)

**File:** `build-planner/` (build artifact, not source)

The other phantom trees were cleaned, but `build-planner/CMakeCache.txt` still pins `CMAKE_HOME_DIRECTORY=/workspace/controller-box` and consumes ~93 MB of duplicated compilation output rooted at a nonexistent path. `rm -rf build-planner` reclaims the disk and removes a stale-root trap if any `--test-dir` is ever pointed at it. Non-blocking.

---

## INFO — `verify.sh` runs an unconditional CMake configure on every invoke

**File:** `scripts/verify.sh` (nix-shell and fallback paths)

`cmake -B "$BUILD_DIR"` runs each cycle even when the cache is valid/warm; CMake elides regeneration so cost is bounded and effectively incremental. Could be gated on a missing `CMakeCache.txt`, but the savings are marginal. Low priority.

---

**Summary:** No efficiency or resource issue blocks task 6 — the canonical cache correctly resolves to `/workspace/project` and representative tests execute real binaries. The actionable improvement, given the wipe-and-reconfigure workflow this task formalizes, is wiring in ccache (WARN 1) and resolving the two `-Werror` gates so the cold-rebuild recovery path actually completes (WARN 2).

Exit code: **0**


---

## spec-compliance Audit

## Audit Complete: Task 6 — No findings

I ran the task's exact acceptance verification against the real project tree and confirmed it exits 0 end-to-end.

### Evidence verified

| Acceptance clause | Verified result |
|---|---|
| `CMAKE_HOME_DIRECTORY` → real `/workspace/project` | ✅ Fresh `cmake -S . -B build` writes `CMAKE_HOME_DIRECTORY:INTERNAL=/workspace/project` |
| Phantom `/workspace/controller-box` gone | ✅ Directory doesn't exist on disk; no cache entry references it |
| `ctest` runs real binaries | ✅ `test_settings` (Test #13) executes `build/test_settings` and passes in 0.04s — not a "Not Run"/file-not-found |
| Full verification exits 0 | ✅ Ran verbatim: `VERIFICATION_EXIT=0` |

### Key details

- **Build environment.** The task verification only passes inside the project's documented `nix-shell`; a bare-shell `cmake` fails at `FindPkgConfig` (no `sdl2`). This is an environment constraint (per `AGENTS.md`), not a task 6 defect.
- **Transient compile error.** Mid-audit I observed failures on the unrelated `test_assign`/`test_profile_diagram` targets after an interrupted `rm -rf build` + bare-shell configure left the tree inconsistent. After a clean, complete run of the acceptance command, **every target built cleanly** and ctest passed — no residual defect.

### Verdict

**No findings.** Task 6 is compliant with its acceptance criteria. Report written to `.factory/artifacts/spec-audit-task-6.md`.


---

## compatibility Audit

I have completed the compatibility audit for task 6. Let me summarize findings.

## Compatibility Audit — Task 6

### Acceptance verification

I ran the exact task verification sequence (within the mandated `nix-shell` environment):

- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` → exit 0, and `build/CMakeCache.txt` now resolves `CMAKE_HOME_DIRECTORY:INTERNAL=/workspace/project` (the real project root). ✅
- `cmake --build build --parallel` → exit 0. ✅
- `ctest --test-dir build -R "^test_settings$" --output-on-failure` → **Passed**, executing a real binary. ✅
- Representative set (`test_settings_tab`, `test_sample`, `test_config_paths`, `test_font_path`, `test_icon_map`) → all **Passed** with real binaries, no "Not Run"/file-not-found. ✅
- Full suite single run: 100% passed (89/91, 2 hardware skips: `test_kernel_controller`, `test_backend_smoke`). The only failure I observed was my own artifact (two concurrent ctest runs colliding on Xvfb `:98`), not a code defect.

The phantom `/workspace/controller-box` is gone from the build tree; compile definitions and include paths all resolve to `/workspace/project`. Task 6 acceptance criteria are met.

---

### Findings

**Info** — `.factory/ssh/ssh_config` (lines 6–7, 23–24, 38–39, 46–47)
- **Issue:** All four runner hosts (`dev-runner-vm`, `gpu-runner-vm`, `iprunner`, `gpurunner`) still hardcode `IdentityFile`/`UserKnownHostsFile` under the phantom `/workspace/controller-box/.factory/ssh/...`. That directory does **not** exist on this host (confirmed via `ls`). The keys actually live at the real `.factory/ssh/` under `/workspace/project`.
- **Recommendation:** Update the paths to `/workspace/project/.factory/ssh/factory_runner_ed25519` and `/workspace/project/.factory/ssh/known_hosts`. This is not required for task 6's build-directory acceptance, but if the factory invokes SSH through this config, `BatchMode yes` will fail to load the identity file and runners will be unreachable. Outside task 6's scope; safe to defer but should be tracked.

**Info** — Task verification command vs. build environment
- **Issue:** The task's listed verification command (`cmake -S . -B build ...`) succeeds for `configure` outside `nix-shell`, but `cmake --build` fails without it (`systemd/sd-bus.h`, `cmocka.h`, `SDL2/SDL.h`: No such file or directory). The build and ctest only work inside `nix-shell`.
- **Recommendation:** No blocker — `AGENTS.md` already mandates building via `nix-shell --run 'cmake …'`. Noting so a developer does not run the literal one-line command bare and report a false negative.

**No dependency/API compatibility issues found:**
- `cmake_minimum_required(VERSION 3.16)` vs. installed CMake 4.3.4 — compatible; build succeeds.
- SDL2/sdl2-compat 2.32.70, SDL2_image, SDL2_ttf, systemd (sd-bus), cmocka, libyaml all resolve correctly from the Nix store and compile with `-Werror`.
- No deprecated/changed APIs flagged; no test assumes hardware it disclaims (hardware-dependent tests skip cleanly with exit 77).

No BLOCKER or WARN findings that block task 6 completion. The task's acceptance is satisfied.


---

## functional Audit

**Auditor failed (exit 124)**

subagent auditor-functional timed out after 600s
