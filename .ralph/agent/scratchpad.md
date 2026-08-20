# Handoff: Delegate Task 5 (BUG-0014) to Factory Worker

## Outcome
- Plan fresh: spec commit `3a10f6b7d04a` blob `58f5d3cb72bc`, plan `status: active`.
- Runtime task `task-1787192943-7d10` = BUG-0014 is `in_progress` (P1). Plan Task 5
  "Perceptible installed diagram acceptance" is `in_progress` (deps Tasks 1,2,3 complete).
- Conformance rows OVL-10, MGR-07 are `partial` bound to FACT-001 (BUG-0014).
- Environment (`dev-runner-vm`) declares only `remote-project-gate, systemd-user,
  kernel-uinput, installed-package`; `gpu-compositor` is NOT declared. Tree clean at `a4a4d80`.

## Delegation target (Factory Worker) — Task 5 scope
- Diagnose and fix BUG-0014: `./build-check/controller-box --manager` on the installed
  X11 production path shows a blank controller diagram region.
- Add a semantic production-window/installed-path test asserting recognizable diagram
  content (outline / model label / slot highlight) — NOT non-NULL texture, fallback,
  or broad pixel count. `test_manager_visual`/`test_overlay_visual`/`test_golden`
  must no longer pass while the production diagram is blank.
- No env-var or source-tree path injection to load diagram assets; the installed layout
  must load them via the production path. See `cbx_icon_dir()` + `/svg/` append in
  `icon_cache.c` / `profile_diagram.c`.
- Reclassify OVL-10, MGR-07 partial->verified in matrix + `conformance.json` with
  installed-window evidence; resolve FACT-001 in `blocked-facts.json` with an exact
  receipt/artifact at the evidence commit.
- Verification: `nix-shell --run './scripts/verify-project.sh'`; installed diagram
  acceptance passes on the real window server; `validate-conformance.py` and
  `validate-blocked-facts.py` planning accept; bug-ledger/BUG-0014 evidence attached.

## Guardrails
- Run targeted backpressure first, then `verify-project.sh`; do not weaken existing
  goldens/assertions; keep runner receipts exact-commit; BUG-0014 fix must not touch the
  committed spec.
- The worker is the only repo writer and must commit to `develop`; verify the commit
  and receipts land before the next Ralph step.

## Next (after worker completes)
- Verify commit + receipts, then plan Task 6 (BUG-0015 real InputPlumber system-bus) or
  append remediation if Task 5 unblocks more. Do NOT emit the completion token while the
  ledger is open / matrix has partial rows.
