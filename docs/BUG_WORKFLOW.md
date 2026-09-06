# Provider-neutral bug maintenance

## Canonical state and external references

`.factory/bugs/open.md` and `.factory/bugs/closed.md` are the portable canonical workflow state. Each contains one JSON array under schema `ralph-bug-ledger/v1`. A bug may reference a GitHub issue, a Forgejo issue, both, or neither. External synchronization is manual: issue state never overrides the local ledgers.

External references must be HTTPS issue URLs with a parsed hostname and valid optional port, without user information, query strings, or fragments. GitHub and root-hosted Forgejo links use `https://HOST/OWNER/REPO/issues/N` (the legacy singular `issue` route remains accepted). A Forgejo installation hosted below a URL path must expose/copy a canonical issue URL in that expected owner/repository route shape. Never put PATs, passwords, cookies, or other credentials in the repository or in issue URLs. This workflow makes no network calls and uses no provider API clients.

## Intake and states

IDs are allocated monotonically as `BUG-0001+`. Open ticket states are `open`, `triaged`, `planned`, `in_progress`, and `blocked`; `closed` records live only in `.factory/bugs/closed.md`. Severity is `low`, `medium`, `high`, or `critical`.

A defect restores behavior already required by the committed specification. If `contract_change` is true, expected behavior needs a product decision, or the fix would edit `docs/SPEC.md`, stop and use the human specification workflow: approve and commit the spec, then run the ordinary planning/implementation lifecycle. Maintenance must never decide or silently change the product contract.

Provider templates are byte-identical at `.github/ISSUE_TEMPLATE/bug_report.md` and `.forgejo/ISSUE_TEMPLATE/bug_report.md`. Copy issue details into the local ledger and retain manual URLs as references.

## Ledger commands

```bash
./.factory/tools/bug-ledger.py validate
./.factory/tools/bug-ledger.py list [--status triaged]
./.factory/tools/bug-ledger.py show BUG-0001
./.factory/tools/bug-ledger.py fingerprint BUG-0001
./.factory/tools/bug-ledger.py add --title "Failure" --severity high \
  --reproduction "steps" --expected "result" --actual "failure" \
  --acceptance "regression passes" \
  --external github=https://github.com/ORG/REPO/issues/123 \
  --external forgejo=https://forge.example/ORG/REPO/issues/456
./.factory/tools/bug-ledger.py link BUG-0001 github https://github.com/ORG/REPO/issues/123
./.factory/tools/bug-ledger.py unlink BUG-0001 github
./.factory/tools/bug-ledger.py set-status BUG-0001 triaged
./.factory/tools/bug-ledger.py set-status BUG-0001 planned
./.factory/tools/bug-ledger.py set-status BUG-0001 in_progress
./.factory/tools/bug-ledger.py close BUG-0001 \
  --resolution "implemented correction" --verification "test command and result"
./.factory/tools/bug-ledger.py recover
```

Each ledger-file replacement is individually atomic and deterministic; moving a record between two ledgers is not transactionally atomic. All read-modify-write commands serialize on ignored `.bug-ledger.lock`. If closure is interrupted after writing the closed destination, normal validation reports the duplicate and `recover` removes the open duplicate only when immutable fingerprints match and the closed record has valid evidence. Intake fingerprints cover immutable problem/acceptance fields, not workflow status or external URLs. Links cannot duplicate a provider, closed records are immutable, and closure requires `in_progress` plus resolution and verification evidence.

## One-bug maintenance cycle

Triage an ordinary defect, then record the selected bug and cycle base in the
ignored trusted state (`.factory/tools/factory-state-file.py`), seed a canonical
`.factory/artifacts/maintenance-plan.md`, and validate it:

```bash
./.factory/tools/bug-ledger.py validate
./.factory/tools/factory-state-file.py write maintenance-bug-id BUG-0001
./.factory/tools/validate-maintenance-plan.py planning .factory/artifacts/maintenance-plan.md
./.factory/tools/check-maintenance-freshness.sh --planning
```

The maintenance lifecycle then runs through the same fresh-context Python
control plane as implementation: a fresh planner seeds the minimal
selected-bug skeleton, a fresh developer implements and tests, verification
runs the configured project verifier, and the final maintenance audit is the
only task that may close the selected record. The selected ID, selected-cycle
base commit, and loop state are volatile local state in ignored
`.factory-state/`. A fresh cycle atomically replaces the previous maintenance
plan with a minimal selected-bug skeleton; old plans remain only in Git
history and the closed ledger. The planning gate requires every new task to
be `pending`. Planning accepts `triaged` (or `planned` only when resuming),
commits the strict plan, and commits the ledger-only `planned` transition
before final completion attestation. Completion binds immutable metadata to a
clean unchanged HEAD, and no tracked commit follows that attestation.
Maintenance run accepts only `planned` or `in_progress`; its first
implementation task transitions `planned` to `in_progress`. The plan ends
with **Maintenance verification and documentation audit**.

Maintenance fails closed unless `[verification].maintenance_command` is a
non-empty argv array whose first element exists and is executable. The argv
is executed directly, without shell evaluation. The project supplies
`scripts/verify-project.sh` as that command.

Recovery is derived from Git, the canonical maintenance plan, the trusted
state, and process liveness (see [FACTORY.md](FACTORY.md) — there is no
separate recovery launcher and no resumed model session). If freshness
reports changed intake/specification or a contract change, do not bypass it;
return to human triage/specification workflow.
