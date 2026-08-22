# Handoff: Task 21 point 4 closed (symlinked output parent refusal regression)

## Outcome this iteration
Advanced the P1 task `task-1787434673-f6a4` (Task 21) by closing point 4's
"add targeted tests" branch. The driver already refused a symlinked output
parent/ancestor via `reject_symlinked_path` + `verify_output_parent`, but only
OUTPUT-as-symlink had a regression. Added a new 13j functional regression in
`tests/test-visual-audit.sh` that targets an OUTPUT whose parent is a symlink
directory and asserts the driver refuses with the exact
"parent/ancestor is a symlink" reason, leaves no OUTPUT, never writes into the
real target directory, and leaves no owned temp there. The honest scoping note
("dirfd-relative ops are impractical in a shell driver") remains the boundary of
the canonicalization guarantee.

## What changed
- `tests/test-visual-audit.sh`: new 13j inside the ATOM block (display toolchain
  + real installed binary available) — creates `$tmp/parsym/real` with a
  sentinel, symlinks `$tmp/parsym/link -> real`, runs the driver targeting
  `$tmp/parsym/link/out.png`, asserts: nonzero exit, refusal message
  "parent/ancestor is a symlink", no OUTPUT at link or real, real target dir
  undisturbed, no owned temp in the real dir.
- `.factory/artifacts/implementation-plan.md` Task 21: scope line notes point 4
  closed this iteration; still-open list is now points 1, 3 + the Nix-boundary
  half of 6/7 (point 4 removed); verification text documents 13j.

## Verification (all green)
- `nix-shell --run 'bash tests/test-visual-audit.sh'` exit 0 — runs twice; 13j
  `symlinked output parent/ancestor refusal passed (13j)` is non-skipping, and
  all prior 13d/13e/13f/13g/13h/13i regressions remain green.
- `nix-shell --run 'shellcheck scripts/visual-capture-driver.sh tests/test-visual-audit.sh'`
  exit 0.
- `./scripts/verify-boilerplate.sh` exit 0 at host; `./scripts/check-plan-freshness.sh`
  exit 0 (spec 3a10f6b7d04a/blob 58f5d3cb72bc); `validate-implementation-plan.py
  planning` exit 0; `./scripts/check-context-summary.py` OK (task-level, no regen).
- No production code touched (only the test + plan doc); no golden regen; no
  conformance evidence elevation.

## Known pre-existing (not introduced by this iteration)
Full `./scripts/verify-project.sh` still fails its ctest gate on
`test_golden_manager_editor_*` — BUG-0018 golden baselines encode the pre-BUG-0018
diagram; regeneration is forbidden/out-of-band (Task 15/18). Task 4 remains blocked
on FACT-002..007 + golden re-approval.

## Next
Factory Worker hat, next fresh iteration, continues Task 21 closing points 1, 3
and the remaining Nix-boundary half of 6/7 (see Task 21 scope). Point 1 depends
on point 7 (authenticated Nix inner gate). Point 3 (deterministic TERM barriers)
is substantially addressed by the pre-armed dev:ino cleanup; remaining work is to
make the barrier mechanism explicit. Do not emit the completion token while
Task 4 and FACT-002..007 / golden re-approval remain open.
