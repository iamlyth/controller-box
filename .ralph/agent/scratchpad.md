# Resumed: Task 5 (BUG-0014) — perceptible installed controller diagram acceptance

## State (refreshed 2026-08-20)

- Loop resumed via `task.resume`. On-disk scratchpad was stale (described the
  prior graceful stop); actual repo has advanced well past that. Tree clean on
  `develop` at 837d417.
- Runtime task `task-1787192943-7d10` = **BUG-0014** is `in_progress` (P1).
  Plan Task 5 "Perceptible installed diagram acceptance" now marked
  `in_progress` (deps Tasks 1,2,3 all complete).
- Plan front-matter `status: active`; Task 4 (final audit) blocked on Tasks
  5,6,7. Conformance rows OVL-10, MGR-07, MGR-08, DOD-01, DOD-09 all `partial`
  bound to FACT-001 (BUG-0014).
- Capability posture (`.factory/environment.toml`): runner `dev-runner-vm`
  declares only `remote-project-gate, systemd-user, kernel-uinput,
  installed-package`. `inputplumber-system-dbus`, `target-consumer`,
  `gpu-compositor` are NOT declared -> BUG-0015/FACT-002..007 remain open.

## This iteration

- Mark Task 5 in_progress (done) and delegate `factory.implement` for BUG-0014
  product work.

## Delegation target (Factory Worker)

Task 5 scope:
- Diagnose and fix BUG-0014: `./build-check/controller-box --manager` shows a
  blank controller diagram on the installed X11 production path.
- Add a semantic production-window/installed-path test asserting recognizable
  diagram content (outline/model label/slot highlight) — NOT non-NULL texture,
  fallback, or broad pixel count.
- No env-var or source-tree path injection to load diagram assets; installed
  layout must load them via the production path (see memory
  mem-1786926804-0788 / mem-1786926134-2b16 on `cbx_icon_dir()` + `/svg/`
  append in `icon_cache.c` and `profile_diagram.c`).
- Reclassify OVL-10, MGR-07 partial->verified in matrix + conformance.json
  with installed-window evidence; resolve FACT-001.
- Guardrails: run backpressure first, then `./scripts/verify-project.sh`; do
  not weaken existing goldens/assertions; keep runner receipts exact-commit.

## Next (after worker completes)

- Verify commit + receipts, then plan Task 6 (BUG-0015 real InputPlumber
  system-bus) or append remediation if Task 5 unblocks more.
- Do NOT emit completion token while ledger open / matrix has partial rows.

<!-- factory-stale-recovery:start -->
## Supervisor recovery feedback

- The previous `implementation` Ralph attempt terminated as a stale loop.
- Run `./scripts/final-gate.sh --implementation` yourself and fix every reported failure.
- Do not repeat a completion summary until that command passes. Replace this section in the next scratchpad handoff before requesting completion.
<!-- factory-stale-recovery:end -->
