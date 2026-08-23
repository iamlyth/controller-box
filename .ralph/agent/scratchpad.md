# Handoff: Task-21 hardening residuals closed, no evidence elevation

## Outcome this iteration
Closed runtime task `task-1787443015-9b90` (P1, "Close Task-21 hardening
residuals without evidence elevation"). All 8 documentation/test hardening
residuals implemented on `develop`; NO golden regen and NO conformance
elevation. The out-of-band final-audit gate (Task 4 / FACT-002..007) remains
blocked independently.

## What changed
- `scripts/visual-capture-driver.sh`: restored the installed-binary EXECUTABLE
  check; capture `INSTALLED_BIN_DEVINO` and revalidate binary (provenance +
  dev:ino) immediately before launch; added `invoke_atomic()` helper
  revalidation (owner/mode/exec/dev:ino) before every publish/fsync/receipt/
  recover invocation; removed the dead `power-loss recovery failed; capture
  withheld` guard (recovery is best-effort) with corrected diagnostic.
- `scripts/atomic-publish.py`: documented the post-image-rename pre-fsync
  recovery invariant (recovery is a no-op whenever OUTPUT already exists).
- `scripts/verify-project.sh`: `test-visual-audit.sh` is now a strict-rc0 gate
  (the dead 77-skip is removed for it; it is non-skipping under Nix).
- `shell.nix`: documented the unpinned-nixpkgs ceiling.
- `tests/test-visual-audit.sh`: new 13b source invariants (executable check,
  launch revalidation, invoke_atomic dev:ino, negative dead-guard); 13i case
  (f) pins the post-rename-pre-fsync invariant; 13h non-executable case (and
  the writable case changed 666->776 so it stays executable); new 13k functional
  cookie-less-Xauthority-refusal test (cookie-bearing connects, cookie-less
  refused by the `-auth` display).
- `.factory/artifacts/implementation-plan.md`: appended a Post-closure
  hardening note to Task 21's section with evidence. Plan still validates and
  is fresh (spec `3a10f6b7d04a`, blob `58f5d3cb72bc`).

## Exact verification (all pass)
- `nix-shell --run 'bash tests/test-visual-audit.sh'` exit 0, non-skipping:
  13b/13d/13e/13f/13g/13h/13i/13j/13k all pass (my new cases included).
- `nix-shell --run 'bash tests/test-nix-gate.sh'` exit 0 (9 regressions).
- `./scripts/verify-boilerplate.sh` exit 0 at host.
- `nix-shell --run 'shellcheck -x scripts/visual-capture-driver.sh tests/test-visual-audit.sh scripts/verify-project.sh'` clean.
- `python3 scripts/validate-implementation-plan.py planning ...` OK;
  `./scripts/check-plan-freshness.sh` OK.
- Full `./scripts/verify-project.sh` stops only at the pre-existing open
  BUG-0018 `test_golden` manager-editor mismatches (golden baselines protected
  under `.factory/golden-policy.json`, out-of-band re-approval required). This
  is unrelated to the hardening (only scripts/tests/docs/shell.nix changed — no
  C, no goldens); every other ctest (100 tests) passes except test_golden.

## Commit
Committed to `develop` as `c9d...` (or see `git log -1`): substantive
hardening commit (scripts, tests, shell.nix, plan note).

## Next action
Task `task-1787443015-9b90` is complete and may be closed. Do NOT touch the
out-of-band final-audit gate (Task 4 / FACT-002..007) — still blocked
independently. For the next iteration: close the runtime task, then re-confirm
no ready/open work; emit `factory.implement` with a brief payload; do not emit
the completion token (Task 4 + external facts remain open).
