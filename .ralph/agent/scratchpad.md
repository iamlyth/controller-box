# Handoff: Task 21 complete — points 1 and 7 closed (authenticated Nix gate)

## Outcome this iteration
Closed the last two open audit points of Task 21 (`task-1787434673-f6a4`):
point 1 (bind `test-visual-audit` into the complete verifier with a 13f
installed-prefix skip failure under the Nix inner gate) and the Nix-boundary
half of point 7 (replace forgeable `CBX_VERIFY_IN_NIX_SHELL`/`IN_NIX_SHELL`
trust with an authenticated wrapper/inner boundary). The implementation was
already present at `59cee41` (iteration 219: `scripts/nix-gate.sh`,
`scripts/nix-gate-exec.sh`, `scripts/nix-gate-check.py`, `tests/test-nix-gate.sh`,
and the `test-visual-audit.sh` manifest binding + 13f/13h fail-closed branches),
but the plan scope and scratchpad had not been reconciled — they still listed
points 1/7 as open. This iteration verified every required behavior directly
and closed them in the plan. All nine Task 21 audit points are now closed.

## Verification (all green)
- `bash tests/test-nix-gate.sh` exit 0 — 9 regressions: retired marker;
  optional-SKIP at host; forged nonce / non-0600 / symlink / incomplete
  capability; valid-capability-outside-nix; missing declared store tool;
  Nix-unavailable — all refused with exit 2; plus the store-authority +
  wrapper round-trip inside nix-shell.
- `source scripts/nix-gate.sh; nix_gate_require optional` returns 1 at host
  (legit host SKIP for 13e/13f/13h) and 0 under `nix-shell` (fail-closed Nix
  inner gate — missing prerequisite or installed binary FAILS, never skips).
- Full `nix-shell --run 'bash tests/test-visual-audit.sh'` exit 0, non-skipping:
  13d/13e/13f/13g/13h/13i/13j all passed against a test-installed production
  binary (13f auto-installs build-check to a temp prefix when
  `VISUAL_AUDIT_INSTALL_PREFIX` is unset).
- `test-visual-audit.sh` is a gate in `.factory/verifier-acceptance.json`
  (`ralph-verifier-acceptance/v1`); `scripts/verify-project.sh` installs the
  built artifact to a test-owned prefix and exports `VISUAL_AUDIT_INSTALL_PREFIX`.
- Plan edited to close points 1/7 (scope, acceptance, verification);
  `validate-implementation-plan.py planning` exit 0; `./scripts/verify-boilerplate.sh`
  exit 0 at host. No golden regen; no conformance evidence elevation.

## What changed this iteration
- `.factory/artifacts/implementation-plan.md` Task 21: the stale "Still open:
  point 1 and the remaining Nix-boundary half of 6/7" sentence is replaced by a
  closure paragraph documenting the authenticated nix-gate (wrapper-generated
  nonce + nix-store authority, fail if Nix unavailable), the fail-closed
  13f installed-prefix branch under the Nix inner gate, and the manifest
  binding; notes that all nine audit points are closed. Acceptance + Verification
  extended with the nix-gate and binding evidence.
- No production/script/test code changed — the implementation was already
  committed at `59cee41`; this iteration only reconciled and verified it.

## Known pre-existing (not introduced here)
Full `./scripts/verify-project.sh` still fails its ctest gate on
`test_golden_manager_editor_*` — BUG-0018 golden baselines encode the
pre-BUG-0018 diagram; regeneration is out-of-band (Task 15/18). Task 4 (final
audit) remains the single completion gate and stays blocked on out-of-band
FACT-002..007 (inputplumber-system-dbus, target-consumer, gpu-compositor,
human release acceptance + Pi latency, runner receipt) plus golden re-approval.

## Next
With Task 21 fully closed, no implementation task remains ready. Task 4 (final
audit) is blocked on out-of-band facts/golden; do NOT emit the completion token
while Task 4 and FACT-002..007 / golden re-approval remain open. Continue the
cycle with a fresh verification pass against the declared environment only;
no software-fixable work is pending inside Task 21.
