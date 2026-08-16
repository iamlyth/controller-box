# Campaign Round 2 Audit — Complete

## Status
Audit report at `.factory/artifacts/campaign-audit.md` with `result: findings`.
Final gate passed: `./scripts/final-gate.sh --campaign-audit` accepted round 2.

## Findings (7)
1. No physical/kernel-backed gamepad in installed functional test (§11.1.5, §5.7)
2. No compositor-visible overlay activation in installed binary test (§11.1.5)
3. Boolean DBus property SET lacks native type fidelity (§10.1)
4. test_installed_functional.c overlay phases claim coverage not delivered (test-trust)
5. Inventory M32/M34 dispatch path misrepresented, M38 untested (test-trust)
6. Documentation inaccuracies in OPERATIONS.md, README.md, plan (§11.2.8)
7. DBus InterfacesAdded/Removed lack sender verification (security)

## Key evidence
- Runner evidence valid: check-factory-runner-evidence.py PASS at 9fd528a
- Runner ran verify-project.sh — build/CTest/packaging/smoke all pass
- Runner stdout shows overlay frame mean=0 (blank — overlay never activated)
- test_installed_functional.c:9 claims "kernel-backed" but uses SDL_JoystickAttachVirtual
- test_installed_functional.c Phases 7-10 never call cbx_overlay_service_init/step
- sd_set_property (dbus_client.c:537-543) falls through to string for boolean properties
- OPERATIONS.md:1032 says "59 entries, all verified" but 8 are NOT_APPLICABLE, 1 DEFERRED

## Gate binding
- FACTORY_CAMPAIGN_AUDIT_ROUND=2
- FACTORY_CAMPAIGN_AUDIT_BASE=9fd528a829cde97951a36a38b8d98a98396cc3f9
- FACTORY_CAMPAIGN_RUNNER_EVIDENCE_SHA256=a20965df7b555c2d9bec12f4192d5ac7b46c545f20b6120c681658cb713774d3

## Next action
Emit the completion token.