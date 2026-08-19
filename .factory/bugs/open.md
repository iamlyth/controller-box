# Open Bugs

Canonical queue of defects awaiting maintenance.

Schema: `ralph-bug-ledger/v1`

```json
[
  {
    "id": "BUG-0013",
    "title": "Completion-gate attestation leaks into boilerplate scenario suite",
    "status": "open",
    "severity": "high",
    "reported": "2026-08-19",
    "external": [],
    "contract_change": false,
    "reproduction": "Run an active implementation campaign whose loop completes (LOOP_COMPLETE requested). The pre.loop.complete final-implementation-gate hook runs ralph-completion-gate.sh implementation, which sets FACTORY_FINAL_GATE_ATTEST=1 for final-gate.sh --implementation. final-gate.sh then runs verify-boilerplate.sh with that flag in the environment; test-maintenance-planning-completion.sh drives nested final-gate invocations on a deliberately dirty temp root, so the inherited attestation start check fails and final-gate returns 1, writing a completion-rejection marker. Every completion attempt is rejected and the campaign churns through bounded completion recovery (counter 3/8 in one observed campaign) with no semantic movement.",
    "expected": "Ambient FACTORY_FINAL_GATE_ATTEST must not influence the isolated boilerplate scenario suite: verify-boilerplate.sh sanitizes ambient lifecycle state before running tests, and test-maintenance-planning-completion.sh scopes its own attestation state. The completion gate passes on a completed cycle and the campaign transitions to verification/audit.",
    "actual": "verify-boilerplate.sh ran test-maintenance-planning-completion.sh with the leaked FACTORY_FINAL_GATE_ATTEST=1, nested final-gate invocations failed with 'final-gate: completion attestation requires a clean Git tree', final-gate --implementation exited 1, the completion gate wrote completion-rejected.json, and ralph-run.sh consumed the marker and incremented completion_recoveries/no_progress_recoveries (3/8 after three identical rejections) while the plan and tree were already complete and clean.",
    "acceptance": "Acceptance criteria: with the fix applied, (1) FACTORY_FINAL_GATE_ATTEST=1 ./tests/test-maintenance-planning-completion.sh exits 0; (2) tests/test-boilerplate-env-isolation.sh passes; (3) ./scripts/verify-boilerplate.sh passes when invoked with FACTORY_FINAL_GATE_ATTEST=1; (4) a clean completed cycle passes ralph-completion-gate.sh implementation with attestation and no rejection marker; (5) the full project verification gate passes; (6) the Controller-Box fix is ported to the generic parity boilerplate.",
    "resolution": "",
    "verification": "",
    "closed": null
  }
]
```
