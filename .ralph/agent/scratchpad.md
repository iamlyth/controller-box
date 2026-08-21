# Handoff: cycle blocked on Task 6 (BUG-0015); ledger made consistent

## Outcome
- The only remaining implementation task, Task 6 (BUG-0015 real four-target
  InputPlumber routing), is a hardware/capability blocker: `inputplumber-system-dbus`
  is NOT declared in `.factory/environment.toml` (declared: remote-project-gate,
  systemd-user, kernel-uinput, installed-package). FACT-002/FACT-003 open; BUG-0015/
  BUG-0016 open in ledger. No real org.shadowblip.InputPlumber system bus is
  provisioned. Cannot fabricate system-bus evidence. Marked Task 6 `blocked` with
  exact reason.
- Fixed a pre-existing ledger inconsistency: `validate-implementation-plan.py planning`
  failed because Task 5 (and Task 7) were marked `complete` while they are appended
  tasks AFTER the final audit (Task 4), which is still `blocked`. The validator
  requires appended tasks to stay `pending`/`blocked` until the final audit gates the
  cycle. Reclassified Task 5 -> `pending` (added Result note: BUG-0014 resolved at
  `ae4ef52`), Task 7 -> `pending` (BUG-0017 resolved at `04e2b37`), Task 6 -> `blocked`.
  Updated `.factory/artifacts/context-summary.md` active/open-task set to match.

## Verification (exact, this session)
- `validate-implementation-plan.py planning` -> OK (was failing before this fix).
- `validate-conformance.py planning` -> valid (76 requirements).
- `validate-blocked-facts.py planning` -> valid (7 facts).
- `bug-ledger.py validate` -> valid (2 open, 15 closed).
- `check-context-summary.py` -> valid (active task, unresolved facts, exact receipt refs).
- `check-plan-freshness.sh` -> spec commit/blob match.
- `verify-boilerplate.sh` -> passes.

## Commit
- Ledger-consistency fix on `develop`: Task 5/6/7 statuses + Result notes in
  implementation-plan.md, context-summary.md active/open-task set.

## Next (supervisor/planning)
- Task 6 stays `blocked` on the undeclared `inputplumber-system-dbus` capability
  (FACT-002/FACT-003). Do NOT fabricate system-bus evidence. Either provision the
  capability on a real bus with a signed exact-commit receipt (signer also not
  provisioned, FACT-007) or keep Task 6 blocked with this recovery handoff.
- Task 4 (final audit) stays `blocked` until Task 6 resolves.
- Tasks 5 and 7 are `pending` (work done, awaiting the final-audit gate); do not
  redo their work.
- Do NOT emit the completion token: ledger still open (FACT-002..FACT-007), matrix
  has partial rows, Task 6 blocked, Task 4 audit blocked.
