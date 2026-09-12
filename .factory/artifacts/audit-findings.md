# Audit findings (round 1, task 6, repair 1)

## security Audit

## Security Audit — Task 6: Resolve orphaned study-report files

**No findings** (no BLOCKER or WARN issues).

### What I verified

Task 6 is a documentation-housekeeping task. It relocated three orphaned study-report files into `.factory/artifacts/` and committed them, and reverted incidental production-code edits that had leaked into the tree.

- `git status --porcelain --untracked-files=all` → **0 untracked files** (empty output). The acceptance criterion ("repo root contains no untracked files") is met.
- The three study artifacts are tracked under `.factory/artifacts/`; prior strays (`docs/subsystem-study-overlay.md`, `.factory/artifacts/substudy-identify.md`) were removed.
- The change adds **no** C source, no input parsing, no buffers, no network/DBus surface, no privilege boundary, no runtime filesystem writes, and no dependency changes. Production-acceptance surface is untouched.
- No credentials/secrets in the moved files (the only `token` grep hit was the substring `csv_token_count`, a CSV helper name in docs, not a secret).
- Mutable state (`build/`, `.factory-state/`, `.pi/output/`, `.ralph/`) is properly gitignored, not accidentally version-controlled.

### INFO observations (non-blocking, out of Task-6 scope)

Pre-existing `src/ui/` weaknesses surfaced by the relocated reports — already tracked as separate planner work, not introduced here:
- `src/ui/text.c` — texture leak on cache-full / over-length path (`render_to_texture`).
- `src/ui/widget_list.c` — unguarded `y / item_h` divide in `list_handle_event` (currently safe; no setter exists).
- `src/ui/widget_button.c` — `SDLK_a` triggers focus activation without the controller-event marker check (input-reliability nit, not a privilege issue).

The audit report is written to `.factory/artifacts/audit-findings-task6-security.md`. Exiting 0.


---

## compatibility Audit

My audit is complete. Here is my report.

---

# Compatibility Audit — Task 6: Resolve orphaned study-report files

**Branch:** `develop` · **HEAD:** `686045b4` (factory: task 6 repair)

## Scope of the change

Task 6 is a repository-hygiene task: relocate two orphaned study-report markdown files out of the repo root so `git status` is clean. The resolution commits (`6cdf52da` → `db20d2bd` revert → `9ab5eef0` → `686045b4`) touched **only** markdown artifacts under `.factory/artifacts/` plus reverts of incidental intermediate source edits.

## Verification I performed

1. **Clean-tree gate (task acceptance):** `git status --porcelain --untracked-files=all` → **empty, exit 0**; `git ls-files --others --exclude-standard` → **0 untracked files**; repo root (excl `.factory/`) → **0 untracked files**. ✅
2. **No dangling references from the revert:** `settings_tab.c/.h` are present at HEAD, referenced by `manager.c`, and compiled in both `controllerbox` and `controllerbox_testing` libraries in `CMakeLists.txt`; `test_settings_tab` is still registered in `tests/CMakeLists.txt`. The intermediate removal of these files (which could have broken the build/link) was fully reverted.
3. **Compatibility-critical files unchanged vs. pre-task baseline (`313ae0af`):** `git diff --stat` for `shell.nix`, `cross-shell.nix`, `CMakeLists.txt`, `tests/CMakeLists.txt`, `.factory/environment.toml`, `docs/SPEC.md` → **empty** (byte-identical). The only `src/`/`tests/` deltas from baseline are from a prior task (`test_icon_map.c`), not task 6.
4. **No build-system / dependency / platform delta:** no changes to Nix dependency pins, pkg-config modules, CMake commands, SDL2/DBus API usage, or runner capability declarations (`environment.toml`) result from this task.

## Findings

**No BLOCKER or WARN compatibility findings.**

Task 6 introduces no new platform, dependency, ABI/API, or build-system surface. All compatibility-critical files were verified identical to the pre-task baseline, and no code or build configuration was left in a broken intermediate state after the revert. The clean-tree acceptance criterion is met.

**INFO** (non-blocking observation, already noted in the committed `audit-findings.md`):
- **File:** `.factory/artifacts/architecture-study.md` (line 4)
- **Severity:** INFO
- **Description:** The committed study report hardcodes the run-specific absolute workspace path `/workspace/project` (and references a nonexistent `/workspace/controller-box`). This is documentation-only and has zero runtime/build/API impact, but it is environment-specific and will be stale on any other runner or future task.
- **Recommendation:** Reword to a portable reference such as "the repository root" instead of the literal `/workspace/project`. Non-blocking.

No compatibility issues found. **Exit 0.** ✅


---

## linting Audit

I've completed my audit. The task acceptance is satisfied — the working tree is clean, `git status --porcelain` returns empty, both orphaned study artifacts (`architecture-study.md`, `subsystem-ui-report.md`) are relocated under `.factory/artifacts/` and tracked. This is a documentation-only change (no C/CMake/scripts touched), so my lint findings are scoped to the markdown artifacts it added/committed.

---

# Lint Audit Report — Task 6: Resolve orphaned study-report files

**Scope.** Task 6 relocated two study-report markdown files from the repo root into `.factory/artifacts/` and committed them (`9ab5eef0`, plus audit documentation in `686045b4`). No production source, build, or test files changed. Verified: `git status --porcelain` empty; `git ls-files --others` in repo root empty; both artifacts tracked under `.factory/artifacts/`; no stale references to the old root-level paths outside `.factory/`.

## Findings

### WARN-1 — Study report omits its own sibling report in the "notes for the planner"
- **File:** `.factory/artifacts/architecture-study.md` (§5 "Notes for the planner", ~line 119)
- **Issue:** The prior-studies note reads `(subsystem-manager-study.md, subsystem-report-app.md)` but omits `subsystem-ui-report.md`, which was created in the **same commit**. A planner studying `src/ui/` would not discover the dedicated UI report. This is a documentation-completeness/consistency gap. It was independently flagged as audit-findings finding #3 but remains unaddressed in the source document.
- **Recommendation:** Add `subsystem-ui-report.md` to the referenced list so the note is complete: `(subsystem-manager-study.md, subsystem-report-app.md, subsystem-ui-report.md)`.

### WARN-2 — Stale task-anchor references from a prior cycle
- **File:** `.factory/artifacts/subsystem-ui-report.md`
- **Issue:** Lines 8 and 188 anchor files to "Tasks 19–24" and "Task 22". The active factory cycle is Tasks 1–10 (see `.factory/artifacts/implementation-plan.md`); Tasks 19–24 belong to a prior cycle. These anchors are dead references that can mislead a reader who looks them up in the current plan.
- **Recommendation:** Either drop the task-anchor numbers, qualify them as belonging to a prior cycle, or re-anchor to the active Task 9 identity/input wiring work (which the report's §Potential-issues #2 directly relates to).

### INFO-1 — Inconsistent naming among sibling report files
- **File:** `.factory/artifacts/`
- **Issue:** The four sibling reports use three different naming patterns: `subsystem-manager-study.md`, `subsystem-report-app.md`, `subsystem-ui-report.md`, and `architecture-study.md` (`audit-findings.md`/`implementation-plan.md` are a different class). Pattern drift across sibling artifacts makes the directory harder to scan.
- **Recommendation:** For future study runs, adopt one consistent pattern (e.g. `subsystem-<area>.md` or `<area>-study.md`). Non-blocking; the existing files are fine to keep as-is.

### INFO-2 — Study outputs are now permanently version-controlled
- **File:** `.factory/artifacts/audit-findings.md` (finding #2) — observation echoed here.
- **Issue:** Relocating the orphaned reports under `.factory/artifacts/` and committing them means every future study-run output will accumulate as part of the tracked history. The task acceptance explicitly sanctioned this ("committed deliberately"), so it is not a violation, but it is a growth policy decision worth making explicit.
- **Recommendation:** Document the convention (or add a gitignore for future `subsystem-*/architecture-*/study-*` outputs) if the intent is to track only the canonical `implementation-plan.md`.

## BLOCKERs

None. The task acceptance is met (clean `git status --porcelain`, empty repo-root untracked set, both artifacts relocated and committed).

---

**Verdict:** No blocking issues. Two WARN-level documentation-consistency improvements (WARN-1, WARN-2) and two INFO observations. All findings are confined to the markdown artifacts this task added; no code readability or language-convention concerns because the task touched no source code.


---

## efficiency Audit

I've completed my investigation. Here's my efficiency audit for Task 6.

## Summary of the audit

The task 6 acceptance criterion — `git status --porcelain` shows **zero untracked files** — is genuinely met (verified: `git status --porcelain` exit 0, empty; `git ls-files --others --exclude-standard` empty; the two study artifacts `architecture-study.md` / `subsystem-ui-report.md` are preserved under `.factory/artifacts/` and tracked). No production source or build config is affected by the final resolution (the `src/manager` fix was correctly reverted in `db20d2bd`; only `.md` documentation was added by the final commits `9ab5eef0` / `686045b4`).

There are **no BLOCKERs**. There is one efficiency finding.

---

## Findings

### 1. **WARN** — Orphaned, unbuilt test file left in the tree (dead code / redundant work)

**Files:** `tests/test_settings_controllers_sync.c` (161 lines, new), added by commit `6cdf52da`; never re-added to build in `tests/CMakeLists.txt`.

**Description:** The task that was meant to *resolve orphaned files* actually leaves behind a new orphan. A 161-line regression test ("Regression test for Task 6") was written against the `src/manager` settings-tab save-path fix, but that entire `src/manager` fix was reverted in `db20d2bd` ("clean: restore tree"). The test source file was **not** reverted and is **not registered** in `tests/CMakeLists.txt` — there is no `add_executable`/`add_test` for `test_settings_controllers_sync` anywhere in the tree. The result is dead code: it is never compiled, never linked, and never run (CTest `91/91 pass, 2 skip` confirms it contributes zero test coverage), yet it is committed to the repository and will keep building confusion.

The test even claims to "Eliminate the stale dual-settings-copy problem" and drives the exact save-path reload that was reverted — so it documents behavior that no longer exists in the shipped code.

**Recommendation:** Either (a) wire it into `tests/CMakeLists.txt` (`add_executable` + `add_test`, matching the `test_settings_tab` pattern) and make its assertions hold against the current `src/manager` behavior, **or** (b) delete the file. Given the fix it tests was deliberately reverted, option (b) is the cleaner outcome and is squarely within the task's own mandate of keeping the tree free of orphaned artifacts. Do not silently carry an unbuilt test into the next task.

---

### 2. **INFO** — Repetitive redo-churn across the task-6 series (redundant work)

**Files:** `.factory/artifacts/implementation-plan.md`, commit history.

**Description:** Task 6 required an unusually high number of cycles: `1788c658`, `6dd7a095`, `a96a9a7f` (blocked "after 3 repairs"), `16bf9bf1`, `6cdf52da`, `9ab5eef0`, `686045b4` — an implementation (`6cdf52da`, src changes) that was then wholly reverted (`db20d2bd`), then re-implemented as documentation-only. Much of this was redundant work that did not contribute to the final state.

**Recommendation:** This is a process observation, not a code fix. It is surfaced because it is the root cause of Finding 1 (the reverted implementation's test was dropped rather than deliberately deleted). Capture the "restore tree / revert" decision explicitly in the plan so future tasks do not re-synthesize reverted changes, and run an explicit orphan-audit (any committed file missing from the build) as part of the clean-tree gate.

---

### Not-in-scope / verified clean

- **Algorithmic complexity:** No production code changed in the final task-6 resolution; nothing to audit.
- **Resource leaks / hot paths / I/O patterns:** No runtime code touched.
- **Build efficiency:** No build targets added; `./scripts/verify.sh` reuses normal incremental CMake build. Note: if Finding 1 is resolved by registering the test, it adds one small build target only — no efficiency concern.
- **Acceptance:** `git status --porcelain` is clean (exit 0, no untracked files) — the task's own gate passes.

**Exit code:** 0 (findings are non-blocking; no BLOCKERs raised).


---

## spec-compliance Audit

## Audit Complete

Task 6 is **compliant** with the specification. The core work is done correctly:

- **Repo root clean** — `git ls-files --others --exclude-standard` (excluding `.factory/`) returns empty.
- **Both study artifacts relocated & tracked** — `architecture-study.md` (119 lines) and `subsystem-ui-report.md` (230 lines) now live under `.factory/artifacts/` with substantive content preserved (moved, not discarded).
- **No dangling references** — no `architecture-study` / `subsystem-ui-report` strings remain in `src/`, `tests/CMakeLists.txt`, `CMakeLists.txt`, or `docs/`.
- **Production tree reverted cleanly** — the incidental `manager.c` / `settings_tab.*` / `tests/CMakeLists.txt` edits were stripped; no residual `test_settings_controllers_sync` or subsystem-study references remain.
- **Build/state dirs correctly ignored** — `build/`, `.factory-state/`, `.ralph/`, etc. are gitignored.

### One WARN (not a Task-6 defect)

**Path:** `.factory/artifacts/audit-findings-task6-security.md`

The advertised verification `git status --porcelain` is not strictly empty right now — it shows `?? .factory/artifacts/audit-findings-task6-security.md`. This is a leftover from a *concurrent security-auditor* run (produced after the final task-6 commit `686045b`), not something Task 6 introduced. It's under `.factory/artifacts/` so it doesn't violate the "repo root clean" acceptance, but to make the verification literally empty, that audit output file should be committed (or removed) as audit-accounting hygiene.

**Recommendation:** commit that audit-output file so `git status --porcelain` returns empty, matching the Task-6 verification exactly. No fix needed in the Task-6 change itself.

No BLOCKERs. Report saved to `.factory/artifacts/audit-findings-task6.md`.


---

## functional Audit

## Functional Audit — Task 6: Resolve orphaned study-report files

**Verdict: PASS.** No blocking or warning issues found. The task is functionally correct.

### Verification performed

**1. Relocation is complete and correct (the core change)**
- `architecture-study.md` and `subsystem-ui-report.md` are now **tracked** under `.factory/artifacts/` (AGENTS.md artifact convention), confirmed via `git ls-files`.
- Both files are **absent** from the repo root (`ls` → "No such file or directory").
- No source, test, CMake, script, config, or documentation file outside `.factory/artifacts/` and `.factory/prompts/` references the old root filenames (`grep` returned no matches) — the move introduced no dangling references.
- `git ls-files --others --exclude-standard | grep -v '^\.factory/'` → **empty**: the repo root holds no untracked files, satisfying the acceptance criterion exactly as written and as established in the task's evidence.

**2. Build + test suite (acceptance requires `./scripts/verify.sh` to pass)**
- Ran `nix-shell --run './scripts/verify.sh'`: build completed and **100% tests passed, 0 failed out of 91**.
- The only 2 non-run tests are **skips**, not failures: `test_kernel_controller` and `test_backend_smoke`, each exiting 77 because their `kernel-uinput` / `gpu-compositor` capabilities are absent on this runner — correct per-runner behavior per AGENTS.md, not a regression.

**3. Clean-tree / orphan audit**
- `git status --porcelain` is clean apart from factory-loop audit outputs (see INFO below).

### Findings

**INFO** — `.factory/artifacts/audit-findings-task6.md` and `.factory/artifacts/audit-findings-task6-security.md` are currently untracked and appear during the audit cycle. These are **not** orphaned study reports and are **not** at the repo root (so they do not violate the task's "no untracked files at repo root" acceptance). They are factory-loop audit outputs generated alongside this review, not by `scripts/verify.sh` (no reference to them exists there). Per the factory process, the orchestrator is the sole Git writer and will sweep them up in the next `git add -A && git commit` cycle. **No action required for task 6** — this is expected transient loop state, not a defect in the change.

### Recommendation
None — the acceptance criteria (relocate the two study artifacts under `.factory/artifacts/`, preserve by moving not discarding, root tree clean, `./scripts/verify.sh` passes) are all met and verified with real command evidence.

