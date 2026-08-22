# Handoff: BUG-0018 installed visual capture adapter finished (task-1787416288-52b7)

## Outcome this iteration
- Fixed `scripts/visual-capture-driver.sh` (the ready task) and verified it end-to-end
  against a real installed custom-prefix `controller-box` binary under Xvfb.
  All four states capture successfully with clean owned-group teardown.
- HEAD = `b4f4577` (unchanged); only the listed files changed (tree otherwise clean
  except the handoff scratchpad). Plan freshness green (spec `3a10f6b`); plan validates
  `planning` mode.

## What changed
- `scripts/visual-capture-driver.sh`: bounded window polling for the real production
  titles (`Controller-Box Manager`/`Controller-Box Overlay`), never `xdotool search --sync`;
  collision-free display probe (socket/lock check), no global `pkill`; isolated HOME/XDG +
  deterministic test-owned profile (`display_order -50`) for the editor; `--overlay-service`
  for the overlay state backed by a private dbus-daemon system bus (so the installed
  overlay's production connect/render path runs without system InputPlumber); exact-commit
  receipt sidecar (`<output>.receipt.json`: commit, binary sha256, prefix, state, window
  title); all children in `setsid` process groups with TERM→KILL group cleanup.
- `.factory/visual-audit-inventory.json`: overlay nav corrected `--overlay` → `--overlay-service`.
- `tests/test-visual-audit.sh`: added 13b (driver source invariants: titles, only
  `--overlay-service`, time-bounded poll, no global pkill, collision-free display, owned-group
  cleanup) and 13c (hanging-child: mock installed binary never opens the window + ignores TERM;
  asserts bounded fail-closed wall time, no partial image, hanging child reaped, no leaked
  Xvfb; SKIP 77 only when display tools absent).
- `.factory/artifacts/implementation-plan.md`: appended remediation Task 16 (this adapter),
  added to Task 4 dependencies; Task 4 stays `blocked` (hardware/facts unresolved).

## Verification (all green)
- `nix-shell --run 'bash tests/test-visual-audit.sh'` → EXIT 0 (all adversarial cases incl. 13b/13c).
- `./scripts/verify-boilerplate.sh` → passes (hanging-child test SKIPs 77 outside nix-shell).
- `shellcheck` clean on driver + test; plan `planning` valid; `check-plan-freshness.sh` green.
- Real installed captures: manager-main/profiles/editor + overlay-active all rc=0, valid
  PNGs, valid receipts, no stray Xvfb/app/dbus-daemon after.

## Next
- Runtime task `task-1787416288-52b7` → close (verified). Sole open BUG-0018 runtime task
  `task-1787407706-bacd` stays in_progress/blocked (external facts + golden re-approval).
- Do not emit the completion token. Ledger stays open; Task 4 final audit remains the sole
  completion gate, blocked on external facts plus golden re-approval.
