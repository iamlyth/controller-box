# Handoff: Task 21 point 2 closed (non-vacuous publish race regression)

## Outcome this iteration
Advanced the P1 task `task-1787434673-f6a4` (Task 21) by closing point 2: the
13f(14) "race during the publish syscall" regression was vacuous. It fed the
driver a plaintext non-PNG frame (`racer-src`), so the driver exited at semantic
validation and never reached the image publish syscall — the test only ever saw
the pre-publish refusal and passed in the else branch without exercising the race.

## What changed
- `tests/test-visual-audit.sh` 13f(14): the driver now receives the valid
  `frame-ok.png` manager-main frame (passes the btn/diag <= 0.09 guard). A new
  test-only mock publish helper (`CBX_ATOMIC_PUBLISH` -> `fakepub/atomic-race.sh`,
  gated behind `RALPH_VISUAL_AUDIT_TESTING`) raises a BARRIER marker immediately
  before the image no-replace publish and holds until a concurrent racer has
  claimed OUTPUT via `noclobber`, then delegates to the real `atomic-publish.py`
  (which refuses the now-existing OUTPUT with EEXIST). The test asserts the
  marker is present (proving the publish path was exercised, not exited at
  validation), the driver fails closed without clobbering the sentinel, no
  image-without-receipt remains, and no owned temp is left. The racer no longer
  early-breaks on the pre-published receipt (which would defeat the barrier).
- `.factory/artifacts/implementation-plan.md` Task 21 scope updated: point 2
  marked closed; acceptance criterion + verification line added; still-open list
  now points 1, 3, 4, 5 + Nix-boundary half of 6/7.
- `context-summary.md` regenerated (unchanged — task status/conformance/facts
  untouched); `check-context-summary.py` passes.

## Verification (all green)
- `nix-shell --run 'bash tests/test-visual-audit.sh'` exit 0 — 13d/13e/13f/13g +
  13h pass non-skipping; 13f's barrier-marker assertion ran (a vacuous run that
  exited at semantic validation would fail on the missing marker).
- `nix-shell --run 'shellcheck tests/test-visual-audit.sh'` exit 0.
- `./scripts/verify-boilerplate.sh` exit 0 at host.
- `validate-implementation-plan.py planning` exit 0; `check-context-summary.py` OK.

## Known pre-existing (not introduced by this iteration)
Full `./scripts/verify-project.sh` still fails its ctest gate on
`test_golden_manager_editor_*` — BUG-0018 golden baselines encode the
pre-BUG-0018 diagram; regeneration is forbidden/out-of-band (Task 15/18). My
change is test-script + plan prose only, no goldens, no C build. Task 4 remains
blocked on FACT-002..007 + golden re-approval.

## Next
Factory Worker hat, next fresh iteration, continues Task 21 closing points 1, 3,
4, 5 and the remaining Nix-boundary half of 6/7 (see Task 21 scope). Do not emit
the completion token while Task 4 and FACT-002..007 / golden re-approval remain open.
