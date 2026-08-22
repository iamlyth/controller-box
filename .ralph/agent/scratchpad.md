# Handoff: Task 21 point 5 closed (honest commit-marker claim + bounded recovery)

## Outcome this iteration
Advanced the P1 task `task-1787434673-f6a4` (Task 21) by closing point 5 via the
honest-narrowing branch: the RECEIPT is the durable COMMIT MARKER (bytes + dir
entry fsynced before the image rename); a power loss in the commit window leaves
the committed image durable but stranded at an orphaned `.cbx-capture.*` temp.

## What changed
- `scripts/atomic-publish.py`: new `recover <receipt> <output>` subcommand.
  If OUTPUT is absent and the receipt is a current-user committed file whose
  `image_sha256` matches an owned `.cbx-capture.*` temp's bytes, that temp IS the
  committed image and is restored atomically (no-replace) + dir-fsynced, then the
  temp is consumed. Provably-uncommitted owned `.cbx-receipt.*` temps are removed.
  Recovery never sweeps arbitrary files (a sweep could orphan another committed
  receipt's image) and never treats an orphan temp as evidence. Always exits 0
  (best-effort; never withholds a fresh capture).
- `scripts/visual-capture-driver.sh`: calls `recover` once at startup (idempotent
  no-op unless a committed receipt is present); durability comments narrowed to
  state the commit-marker contract + power-loss orphan reality instead of an
  unqualified "crash/power-durable" claim.
- `tests/test-visual-audit.sh`: new 13i module-level recover regressions — restores
  committed-but-unpublished image from matching-hash orphan + consumes it; never
  promotes a mismatched/unowned orphan; removes uncommitted `.cbx-receipt.*` temp;
  never sweeps an unrelated `.cbx-capture.*` orphan; leaves an already-committed
  OUTPUT untouched; never creates OUTPUT without a committed receipt.
- `.factory/artifacts/implementation-plan.md` Task 21: point 5 closed; still-open
  list now points 1, 3, 4 + Nix-boundary half of 6/7. context-summary unchanged
  (regenerated identical; check passes).

## Verification (all green)
- `nix-shell --run 'bash tests/test-visual-audit.sh'` exit 0 — 13d/13e/13f/13g/13h
  pass non-skipping and new 13i `bounded power-loss recovery passed (13i)` runs.
  The startup `recover` call is a no-op in every pre-existing test and passes
  through every test-only mock (they delegate unknown subcommands to the real
  helper), so no existing regression was disturbed.
- `nix-shell --run 'shellcheck scripts/visual-capture-driver.sh tests/test-visual-audit.sh'`
  exit 0; `python3 -m py_compile scripts/atomic-publish.py` + standalone recover
  smoke both OK.
- `./scripts/verify-boilerplate.sh` exit 0 at host; `validate-implementation-plan.py
  planning` exit 0; `check-context-summary.py` OK.

## Known pre-existing (not introduced by this iteration)
Full `./scripts/verify-project.sh` still fails its ctest gate on
`test_golden_manager_editor_*` — BUG-0018 golden baselines encode the pre-BUG-0018
diagram; regeneration is forbidden/out-of-band (Task 15/18). Task 4 remains blocked
on FACT-002..007 + golden re-approval.

## Next
Factory Worker hat, next fresh iteration, continues Task 21 closing points 1, 3,
4 and the remaining Nix-boundary half of 6/7 (see Task 21 scope). Point 1 depends
on point 7 (authenticated Nix inner gate). Do not emit the completion token while
Task 4 and FACT-002..007 / golden re-approval remain open.
