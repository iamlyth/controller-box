# Handoff: Closed Task 21 points 6/8/9 (visual transaction & verifier trust)

## Outcome this iteration
Implemented and verified the first coherent cluster of the P1 ready task
`task-1787434673-f6a4` (Task 21 in the plan): points 8 (private Xauthority for
Xvfb), 9 (installed prefix/binary owner/mode/provenance validation), and the
helper-owner half of point 6. Points 1,2,3,4,5,7 and the Nix-boundary half of
6/7 remain open on the same task (documented in the Task 21 scope).

## What changed
- `scripts/visual-capture-driver.sh`: Xvfb launched with `-auth` using a private
  MIT-MAGIC-COOKIE-1 created via `xauth add` into a mode-0600 file in the owned
  TMPDIR; `XAUTHORITY` exported so app/xdotool/import inherit it; `xauth` added
  to the display-tool gate (SKIP 77 if absent). Added `validate_installed_binary`
  (regular non-symlink file, owned by current user or root, not group/world-
  writable, non-symlink prefix chain) so a mutable VISUAL_AUDIT_INSTALL_PREFIX
  can no longer select an arbitrary executable. Helper selection now requires a
  current-user-owned regular non-symlink helper not writable by group/other.
- `shell.nix`: added `xauth`.
- `tests/test-visual-audit.sh`: 13b source-invariant asserts for the new checks;
  new 13h functional regression (666 binary + symlink substitute refused, no
  OUTPUT).
- Plan: appended Task 21 (points closed/open), Task 4 dependency updated;
  context-summary regenerated.

## Verification (all green)
- `nix-shell --run 'bash tests/test-visual-audit.sh'` exit 0 — 13d/13e/13f/13g +
  new 13h (`installed-binary provenance hardening passed (13h)`) all pass.
- Real installed driver captured manager-main under private Xvfb auth to a valid
  PNG with an exact-commit receipt; no-cookie client refused, cookie client
  connects; 666 binary -> "writable by group/other", symlink -> "not a regular
  non-symlink file" (both rejected, no OUTPUT).
- `./scripts/verify-boilerplate.sh` exit 0 at host.
- `validate-implementation-plan.py planning` OK; `check-context-summary.py` OK;
  `check-plan-freshness.sh` OK (spec 58f5d3cb72bc@3a10f6b).

## Known pre-existing (not introduced by this iteration)
Full `./scripts/verify-project.sh` still fails its ctest gate on
`test_golden_manager_editor_*` — the BUG-0018 golden baselines encode the
pre-BUG-0018 diagram; regeneration is forbidden/out-of-band (documented finding,
Task 15/18). My changes (shell scripts + shell.nix only) do not touch goldens or
the C build. This is the same out-of-band blocker that keeps Task 4 blocked.

## Next
Factory Worker hat, on the next fresh iteration, continues Task 21
(`task-1787434673-f6a4`) closing points 1, 2, 3, 4, 5, 7 and the Nix-boundary
half of 6/7 (see Task 21 scope for the exact still-open list). Do not emit the
completion token while Task 4 and FACT-002..007 / golden re-approval remain open.
