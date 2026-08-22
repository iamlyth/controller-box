# Handoff: atomic-publish commit-point guarantees complete; c01b software acceptance closed

## Outcome this iteration
Completed runtime task `task-1787427829-3013` (atomic image receipt commit-point
guarantees), which closes the software acceptance of `task-1787424775-c01b`
(atomically publish only validated visual captures). The external BUG-0018 facts
stay on the umbrella task, not c01b.

## What changed and verification
- `scripts/visual-capture-driver.sh`: fail-closed publication now (1) refuses
  pre-existing OUTPUT / receipt destinations (regular file, symlink, dangling
  symlink, or hardlink) instead of `mv -f` replacing unowned paths; (2) publishes
  the receipt FIRST and the image as the final commit-point rename, so an image
  at OUTPUT always has its validated receipt; a receipt-publish failure withholds
  the capture entirely, and an image-publish failure removes the just-published
  owned receipt; (3) fsyncs the receipt temp and the output directory (best-effort
  `fsync_path` helper) around the renames.
- `tests/test-visual-audit.sh` 13f: fixed the signal case (park marker is no longer
  pre-created with `: >`; it now appears only when the mock import parks, so TERM
  is delivered while the owned temp exists); added receipt-publish-failure
  (blocking `mv` on PATH proves a validated capture whose receipt cannot land is
  withheld entirely), pre-existing OUTPUT refusal and pre-existing receipt refusal
  on validated captures, and a direct rejected-overlay case (blank/uniform frame
  through the real installed overlay binary is rejected and leaves neither OUTPUT
  nor a receipt nor a temp).
- Verified: `nix-shell --run 'bash tests/test-visual-audit.sh'` exit 0, 13f
  non-skipping (`atomic capture publication passed (13f)`), all new adversarial
  cases pass; `./scripts/verify-boilerplate.sh` passes at host; `shellcheck`
  clean on both files. Evidence recorded in `.factory/artifacts/implementation-plan.md`
  Task 18 result.

## Commit
On `develop`. Single coherent checkpoint carrying the scratchpad.

## Next
Emit `factory.implement` to continue; never the completion token. The remaining
blockers for implementation completion are unchanged and external: real system
InputPlumber bus (FACT-002/003), target consumer hardware + Pi 4 runtime
(FACT-004), GPU compositor/degraded state (FACT-005), signed runner receipt
(FACT-006), human release acceptance (FACT-007), and out-of-band golden
re-approval. Recovery path: once external facts are provisioned and golden
re-approval granted, run the final audit gate (11.2) and reclassify partial rows.
