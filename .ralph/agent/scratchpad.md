# Handoff: BUG-0018 still fully blocked on external facts (re-verified, unchanged)

## Outcome this iteration
Re-verified the fully blocked state; identical to prior handoff in every substantive way. No ready runtime tasks (task list empty). No software-fixable work remains and no new gap discovered -> no remediation task (AGENTS.md step 9).
- Plan freshness EXIT 0 (spec 3a10f6b, blob 58f5d3); HEAD 9ec8d7f on develop; tree clean except scratchpad.
- Conformance sidecar: 57 verified / 19 partial / 0 blocked; all 19 partial rows bind exclusively to open external facts FACT-002..007 (verified live from fact_refs in conformance.json requirement rows).
- Blocked-facts ledger: FACT-001/008 resolved; FACT-002..007 open (verified live).
- No `.factory/runner-evidence/` dir; `ssh dev-runner-vm` -> "Could not resolve hostname" (exit 255). `.factory/environment.toml` declares only `dev-runner-vm` (unreachable).

## FACT-007 (signer) — still open
`.factory/signer-trust.json` IS tracked (committed c45336a, enabled=true, namespace factory-runner-receipt). FACT-007 stays open because signed receipts live in untracked `.factory-state/runner-evidence/` (0 files Git-tracked) and the aggregate binds to older commits (c45336a/26df6c0/3c372a5), not current HEAD 9ec8d7f, so check-factory-runner-evidence.py reports the Git/environment binding stale. With dev-runner-vm unreachable, no fresh exact-commit Git-blob receipt can be generated.

## Open external facts binding all 19 partial rows
- FACT-002/003 (ARCH-04, DBUS-02/05, MGR-02/08, SYS-06, DOD-01/09) -> undeclared `inputplumber-system-dbus` (BUG-0015).
- FACT-004/006 (SYS-01/02, OVL-09, MGR-08, PERF-01, VRF-07, DOD-01/09) -> undeclared `target-consumer` (Pi runtime + human release acceptance 11.1.7).
- FACT-005 (VRF-06, DOD-05, DOD-01/09) -> undeclared `gpu-compositor`.
- FACT-007 (MGR-03, PKG-01, VRF-05, DOD-06) -> `remote-project-gate`/`systemd-user`/`kernel-uinput`/`installed-package` declared only on unreachable `dev-runner-vm`; needs a fresh signed Git-blob receipt for HEAD + runner reachability.

All software-fixable tasks already implemented/resolved with evidence. Every acceptance path depends on external provisioning (real system bus, target hardware, reachable signed runner producing a fresh exact-commit Git-blob receipt, human release acceptance) plus out-of-band golden re-approval. Scratchpad-only change; not committed (AGENTS.md boundary).

## Next
Emit `factory.implement` to continue; never the completion token. Recovery: once external facts are provisioned (real system bus, target hardware, reachable runner producing a fresh exact-commit Git-blob receipt, human release acceptance) AND golden re-approval granted, run Task 4 final audit gate (11.2) and reclassify the 19 partial rows.
