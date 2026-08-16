# Campaign Round 2 Audit — Scratchpad

## Status
Audit report written to `.factory/artifacts/campaign-audit.md` with `result: findings`.
7 findings identified from 4 parallel read-only subagent reviews + direct investigation.

## Findings summary
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

## Next action
Run final gate. If it passes, emit the completion token. If it fails, repair deficiencies.