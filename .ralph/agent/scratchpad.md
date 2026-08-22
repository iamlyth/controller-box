# Handoff: Close atomic capture security/durability gaps (task-1787431347-58a9)

## Outcome this iteration
Implemented the P1 7-point security/durability remediation on the atomic
capture publication path. All 7 audit points closed; committed to `develop`.

## Verification (all green)
- `nix-shell --run 'bash tests/test-visual-audit.sh'` exit 0: 13f non-skipping
  (`atomic capture publication passed (13f)`) incl. new capture-temp fsync (7a),
  validated symlink/hardlink refusal (13), true syscall-window race (14); 13g
  primitive regressions (link() fallback, source validation, fail-on-unlink
  withdrawal, receipt quote/backslash/tab/newline escaping) pass; C3b/C3c pass.
- `./scripts/verify-boilerplate.sh` exit 0 at host.
- `shellcheck scripts/visual-capture-driver.sh scripts/verify-project.sh tests/test-visual-audit.sh` clean.
- `check-context-summary.py` valid; `check-plan-freshness.sh` exit 0
  (spec 3a10f6b7d04a @ blob 58f5d3cb72bc). Plan Task 4 deps += Task 20.
- No golden regen; no human/visual acceptance claim; no evidence elevation.

## What changed
- `scripts/atomic-publish.py`: fallback to link() only on ENOSYS/EINVAL/EOPNOTSUPP
  (other errno = rc 3); dst existence via lexists (follow_symlinks=False); source
  validated as current-user-owned single-link regular (O_NOFOLLOW); fail-on-source-
  unlink now withdraws the just-created dst to restore pre-publish state; new
  `receipt` JSON-serializer subcommand with strict 64-hex hash validation.
- `scripts/visual-capture-driver.sh`: fsync CAPTURE_TMP before hash/publish; hash
  the stable inode; canonical non-symlink current-user-owned output parent with no
  group/other writes; single-link owned temps (fixed "regular empty file" stat);
  identity-matched cleanup driven by RECEIPT_PUBLISHED/IMAGE_PUBLISHED/COMMITTED;
  barriers after each publish; helper requires regular AND executable; fail-closed
  test hooks above the display-tool gate; fixed reject_symlinked_path relative-path
  hang.
- `tests/test-visual-audit.sh`: 13f fsync ordering renumbered (CAPTURE_TMP is fsync
  #1) + 7a; cases 13 (symlink/hardlink refusal) + 14 (concurrent syscall race); new
  13g primitive section; 13f gate fails under IN_NIX_SHELL if tools missing.
- `scripts/verify-project.sh`: Nix gate asserting Xvfb/xdotool/import/convert/
  dbus-daemon so 13f is non-skipping under complete verification.
- Plan: appended Task 20 (this remediation); Task 4 deps += Task 20; context-summary
  regenerated.

## Next
Factory Worker hat receives `factory.implement` on the next ready task. Do not emit
the completion token while FACT-002..007 and Task 4 remain open (blocked on
out-of-band provisioning: inputplumber-system-dbus, target-consumer, gpu-compositor,
aarch64/Pi4, human release acceptance, signed runner receipt, golden re-approval).
