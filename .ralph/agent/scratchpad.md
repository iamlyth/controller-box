# Handoff: Task 19 (P1) atomic-publication TOCTOU + durability overclaim fixed

## Outcome this iteration
Closed the P1 security/reliability task `task-1787429610-7982` (runtime Task 19).
The `ecc009a` atomic-publication destination refusal was TOCTOU (`[[ ! -e ]]` then
ordinary `mv` could overwrite a destination created after the check) and its
best-effort `|| true` fsync overclaimed crash/power durability.

- New `scripts/atomic-publish.py`: owned no-replace publish (renameat2
  RENAME_NOREPLACE, fallback link+unlink; refuses existing/symlink/hardlink dst)
  + fail-closed fsync (returns 3 on error, never swallowed).
- `scripts/visual-capture-driver.sh`: uses the primitive for receipt-first /
  image-last (commit-point) publication; removed the TOCTOU existence pre-checks;
  every file/dir fsync failure FAILS CLOSED with owned-artifact withdrawal; added
  a test-gated (RALPH_VISUAL_AUDIT_TESTING) pre-publish hook for a deterministic
  race test. No ordinary `mv` remains; CBX_ATOMIC_PUBLISH mock is test-gated.
- `tests/test-visual-audit.sh` 13f: receipt-publish-failure mock now mocks the
  atomic-publish helper (no `/bin/mv` assumption); added fsync-failure cleanup
  cases (7b receipt-temp, 7c receipt-dir, 7d image-commit-point) and TOCTOU race
  cases (11 OUTPUT, 12 receipt) proving sentinel preservation / no
  image-without-receipt.

## Exact verification (all green)
- `nix-shell --run 'bash tests/test-visual-audit.sh'` exit 0; 13f non-skipping
  (`atomic capture publication passed (13f)`), 13d/13e pass; all adversarial
  cases pass.
- Load-bearing proof: temporary racy `mv -f` image publish makes 13f fail
  (`validated capture unexpectedly overwrote a pre-existing OUTPUT`); restored.
- `./scripts/verify-boilerplate.sh` passes at host.
- `shellcheck scripts/visual-capture-driver.sh tests/test-visual-audit.sh` clean.
- `scripts/validate-implementation-plan.py planning` exit 0;
  `scripts/check-plan-freshness.sh` passes (spec 58f5d3cb72bc @ 3a10f6b).
- Helper unit-checked directly (rc=2 on existing/symlink dst, content preserved;
  dir/file fsync rc=0; missing path rc=3).

## Commit
On `develop`, HEAD was `ecc009a`. Committed the Task 19 changes (see `git log`).

## Next
Plan Task 4 (final audit) remains `blocked` — single completion gate, blocked by
out-of-band facts FACT-002/003/004/005/006/007 + human golden re-approval
(BUG-0018) + human release acceptance. No software-fixable ready task remains
(task-1787429610-7982 closed). Continue the cycle with factory.implement; never
emit the completion token while external facts and Task 4 remain open.
