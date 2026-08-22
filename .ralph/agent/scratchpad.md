# Handoff: Task 21 point 3 closed (TERM barrier made explicit)

## Outcome this iteration
Advanced P1 task `task-1787434673-f6a4` (Task 21) by closing point 3's
"make the barrier mechanism explicit" branch. The pre-armed dev:ino identity
cleanup already closed the post-publish signal window and functional
regressions 5a/5b proved both windows deterministically; the remaining work was
explicit-ness. Two stale comments in `scripts/visual-capture-driver.sh` (the
`cleanup()` block and the durable-publication block) still referenced the
removed boolean `RECEIPT_PUBLISHED`/`IMAGE_PUBLISHED` barrier flags,
misdescribing the mechanism — corrected to state the barrier is the pre-armed
inode identity (`CAPTURE_DEVINO`/`RECEIPT_DEVINO`, captured BEFORE each syscall)
plus the `COMMITTED` durable-commit gate, deliberately no racy boolean flag.
`tests/test-visual-audit.sh` 13b gained source-level invariants pinning this so
it cannot regress to a boolean-flag design: `rm_identity_match` present,
`RECEIPT_DEVINO=$(stat ...)`/`CAPTURE_DEVINO=$(stat ...)` pre-armed before each
publish, identity-matched withdrawal against exactly the published
receipt/image, the `if [[ "$COMMITTED" == 0 ]]` gate, and a negative guard
rejecting any reintroduced racy flag. Shellcheck-flagged literal `$` greps were
written with escaped dollars in double quotes (SC2016-clean under `-x`).

## What changed
- `scripts/visual-capture-driver.sh`: comment-only rewrite of the two stale
  barrier-flag comments to describe the actual identity+COMMITTED mechanism.
- `tests/test-visual-audit.sh`: 13b now pins the explicit identity-based TERM
  barrier (pre-arm before syscall, identity-matched withdrawal, COMMITTED gate,
  no racy boolean flag).
- `.factory/artifacts/implementation-plan.md` Task 21: scope opens with point 3
  closed; "still open" is now points 1 + the Nix-boundary half of 6/7 (point 3
  removed); verification documents the new invariants + comment correction.

## Verification (all green)
- `nix-shell --run 'bash tests/test-visual-audit.sh'` exit 0 — 13d/13e/13f/13g
  + 13h/13i/13j non-skipping; the atomic block (13f, containing the 5a/5b
  post-publish TERM regressions) completed, so the TERM-barrier windows ran and
  passed.
- `nix-shell --run 'shellcheck -x scripts/visual-capture-driver.sh tests/test-visual-audit.sh'`
  exit 0 (SC1091 on the sourced nix-gate resolves with `-x`; SC2016-clean).
- `bash -n` on both scripts OK; `./scripts/verify-boilerplate.sh` exit 0 at
  host; `./scripts/check-plan-freshness.sh` exit 0 (spec 3a10f6b/blob
  58f5d3cb72bc); `validate-implementation-plan.py planning` exit 0;
  `./scripts/check-context-summary.py` OK.
- No production behavior changed (comment-only driver diff); no golden regen;
  no conformance evidence elevation.

## Known pre-existing (not introduced by this iteration)
Full `./scripts/verify-project.sh` still fails its ctest gate on
`test_golden_manager_editor_*` — BUG-0018 golden baselines encode the pre-BUG-0018
diagram; regeneration is forbidden/out-of-band (Task 15/18). Task 4 remains blocked
on FACT-002..007 + golden re-approval.

## Next
Factory Worker hat, next fresh iteration, continues Task 21 closing point 1 and
the Nix-boundary half of 6/7. Point 1 depends on point 7 (authenticated Nix inner
gate): replace forgeable `CBX_VERIFY_IN_NIX_SHELL`/`IN_NIX_SHELL` trust with an
authenticated wrapper/inner boundary and add `test-visual-audit` to the bound
verifier manifest/CTest with a 13f installed-prefix skip failure under the Nix
inner gate. Do not emit the completion token while Task 4 and FACT-002..007 /
golden re-approval remain open.
