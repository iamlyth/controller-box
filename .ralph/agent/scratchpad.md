# Handoff: corrected stale FACT-007 rationale; ledger accurate; still no ready task / completion

## Outcome this iteration
Corrected a genuine documentation-accuracy defect in the blocked-facts ledger and
conformance sidecar: FACT-007's stated rationale ("no signer provisioned,
`enabled=false`, no public keys") was **factually stale**. Re-verified reality:

- `.factory/signer-trust.json` is `enabled=true`, `require_signature=true`, has a
  committed public key for `dev-runner-vm`, and the detached signature on the
  c45336a manifest **verifies** via `ssh-keygen -Y verify`.
- A valid signed, commit-bound runner receipt exists at c45336a (ancestor of HEAD).

FACT-007 stays `open` for the accurate reason: the receipt is bound to c45336a,
not the current implementation commit b66dbdb; the runner-evidence aggregate
reports "stale" relative to HEAD; and the receipts live in untracked
`.factory-state/runner-evidence/` (not Git blobs at any evidence commit), so
complete-mode conformance cannot accept them. The `dev-runner-vm` runner is not
reachable from this sandbox (no `~/.ssh/factory-ssh`, hostname unresolvable), so a
fresh signed receipt at HEAD cannot be produced here.

## Verified this iteration
- `ralph tools task ready` → no ready tasks.
- Plan fresh (spec=3a10f6b7d04a), `status: active`, tree clean on `develop` @ b66dbdb.
- Signer provisioned; signed commit-bound receipt exists at c45336a but is stale
  relative to HEAD and untracked (not a Git blob).
- Corrected: `.factory/artifacts/blocked-facts.json` FACT-007 (title +
  blocking_evidence), `.factory/artifacts/conformance.json` reasons for
  MGR-03/PKG-01/VRF-05/DOD-06, regenerated `.factory/artifacts/context-summary.md`.
- Validators pass: validate-blocked-facts (8 facts), validate-conformance planning
  (76 reqs), check-context-summary, verify-boilerplate.

## Remaining blockers (unchanged, external)
- FACT-002/003 BUG-0015 real InputPlumber system-bus — capability undeclared.
- FACT-004 target-consumer (four-target routing, aarch64/Pi 4) undeclared.
- FACT-005 gpu-compositor undeclared.
- FACT-006 target Pi runtime + human release acceptance (out-of-band).
- FACT-007 (corrected): needs runner re-run at current HEAD producing a signed
  commit-bound receipt tracked as a Git blob; runner VM unreachable from sandbox.
- environment.toml forbids inventing these capabilities until acceptance
  contracts exist. A human must provision hardware/capabilities/signer re-run or
  make an explicit spec-scoped deferral before Task 4 (final audit) can run.

## Rule
No ready task, no further software-fixable work available. Do not emit the
completion token; the ledger stays open. Re-check `ralph tools task ready` and the
plan's open/blocked rows and facts on each fresh iteration.
