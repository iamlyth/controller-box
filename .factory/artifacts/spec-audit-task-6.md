# Spec Compliance Audit — Task 6: Reconfigure canonical build directory at the real source path

**Auditor:** spec-compliance
**Branch/commit audited:** `develop` @ a96a9a7f (task 6 repairs)
**Method:** Inspected the committed source + the on-disk canonical `build/` tree, and ran the task's literal verification command against the **actual current state**.

## BLOCKER — Task 6 acceptance is NOT met on the current on-disk state

### 1. The canonical `build/` CMake cache still pins `CMAKE_HOME_DIRECTORY` to the phantom `/workspace/controller-box`
**Files:** `build/CMakeCache.txt` (runtime artifact), `scripts/verify.sh`

Actual `build/CMakeCache.txt`:
```
CMAKE_HOME_DIRECTORY:INTERNAL=/workspace/controller-box
controller-box_SOURCE_DIR:STATIC=/workspace/controller-box
CMAKE_CACHEFILE_DIR:INTERNAL=/workspace/controller-box/build
```

The acceptance clause requires the canonical build cache to resolve `CMAKE_HOME_DIRECTORY` to `/workspace/project` — **explicitly excluding** `/workspace/controller-box`. The committed tree is still rooted at the phantom path. The fix is not achieved in the committed state; it is only papered over by `scripts/verify.sh`'s runtime `rm -rf build` self-heal, which is *not* part of the task's acceptance command.

### 2. The task's literal verification command FAILS at its first step
**Command (from the plan):**
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build --parallel && grep CMAKE_HOME_DIRECTORY build/CMakeCache.txt && ctest --test-dir build -R '^test_settings$' --output-on-failure
```

Actually run against the current tree:
```
CMake Error: The current CMakeCache.txt directory /workspace/project/build/CMakeCache.txt is different
than the directory /workspace/controller-box/build where CMakeCache.txt was created. ...
CMake Error: The source "/workspace/project/CMakeLists.txt" does not match the source
"/workspace/controller-box/CMakeLists.txt" used to generate cache. Re-run cmake with a different source directory.
# exit code 1
```
The `&&` chain stops at step 1, so `grep` and `ctest` never run. There is no self-heal in the literal command: CMake *refuses* to reconfigure a cache pinned to a different source dir. `scripts/verify.sh` is the only thing that recovers (it deletes the stale tree), and it is NOT invoked by the task's verification command nor the acceptance statement.

The recorded evidence in `.factory/artifacts/implementation-plan.md` reads:
> `Evidence: verification exit 0 on local. Confirmed under nix-shell: CMAKE_HOME_DIRECTORY:INTERNAL=/workspace/project ...`

This is **not reproducible** from the current state. The literal command returns nonzero immediately. Trust-but-verify: the claimed `exit 0` does not hold.

### 3. Acceptance criterion #2 fails too — wholesale file-not-found on the representative test
**Command:** `ctest --test-dir build -R '^test_settings$' --output-on-failure`
**Result:**
```
Could not find executable /workspace/controller-box/build/test_settings
Looked in: ... /workspace/controller-box/build/test_settings, ... Release/Debug/MinSizeRel/...
```
This is precisely the "wholesale file-not-found" outcome the acceptance says must NOT occur. `ctest -N` lists dozens of `Could not find executable /workspace/controller-box/...` entries because the registered test paths embed the phantom root.

### Recommendation
The stale `build/` tree must be dropped and reconfigured so that the *committed/runtime* canonical cache resolves to the real source root, and the acceptance must be re-verified **verbatim against that resulting state**. Because CMake refuses to reconfigure a source-dir-mismatched cache, the canonical tree (or its `CMakeCache.txt`) must be removed/updated as part of the actual fix — not left for `scripts/verify.sh` to wipe on the next factory run. Alternatively the evidence must be produced by running the exact acceptance command on a correctly-rooted tree.

## WARN — `scripts/verify.sh` self-heal regex is path-fragile
**File:** `scripts/verify.sh` (line ~22)

The guard splices the raw `$PROJECT_ROOT` into a Basic Regular Expression:
```sh
! grep -q "^CMAKE_HOME_DIRECTORY:INTERNAL=$PROJECT_ROOT\$" "$BUILD_DIR/CMakeCache.txt"
```
No BRE metacharacter escaping. For `/workspace/project` every char is literal so it works today, but a checkout under any path containing `.`, `+`, `[`, `(`, `*`, `^`, `{`, `|`, etc. would silently mis-match — either deleting a healthy `build/` on every run or failing to self-heal a stale root. This script is declared as the per-runner verify_command and runs identically everywhere, so it must be path-hostile. Recommend parsing the cached value and comparing as literal strings (as the audit's earlier lint note also proposed). This is the exact mechanism the task relies on to meet acceptance, so it should be robust.

## WARN — full-build step is at risk from unresolved `-Werror` gates (affects the same acceptance command)
**Files:** `tests/test_profile_diagram.c` (~lines 692/697), `tests/test_assign.c`, `CMakeLists.txt` (line 67 gates `-Werror` on Debug)

`cmake --build build --parallel` (all targets, Debug ⇒ `-Werror`) can fail on `-Wunused-but-set-variable` for the genuinely dead `cnt`/`bcnt` pixel-scan counters. This was already recorded as a failing gate in the efficiency audit. Even once the cache is correctly rooted, this all-target build step in the task's literal verification command may return nonzero. Recommend either scoping the acceptance build to the representative target (`cmake --build build --target test_settings`) or removing the dead counters.

## INFO
- `build/` is gitignored (`build/` in `.gitignore`), so the committed deliverable is source + `scripts/verify.sh`; the "completed" status therefore rests on a runtime artifact whose current state does not satisfy the acceptance. This is exactly the kind of "declaring evidence is not evidence" situation `AGENTS.md` guards against.
- Task 5 (default-path install-state dependency) depends on this task; the phantom-rooted cache will keep `test_icon_map`'s default-path assertions failing until Task 6 genuinely holds.

## Verdict
**Issues found (blocking).** Task 6 cannot be considered complete: the canonical build cache is still rooted at the phantom path, and the task's acceptance/verification command fails verbatim on the current tree.
