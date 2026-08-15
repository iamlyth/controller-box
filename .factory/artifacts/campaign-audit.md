---
schema: ralph-campaign-audit/v1
round: 1
audit_base_commit: 5aea09a87a1385ff34cfb803e2ffba27fab59d77
plan_commit: 3bfe9f8b701b71245ba58617a55f90cfdaf071fe
plan_blob: 0c5b0f9fa4047fb35f5a8506af20a58c4a389501
environment_blob: 0acf81d25bde43ac5d97d8067189d0191116496b
runner_evidence_sha256: 76cdc7cbb00a1aa2f48aa51670ad97737ed9564db6b9f63d14bdf55a56c766ae
result: findings
---
# Campaign Round 1 Independent Gap Audit

## Evidence reviewed
- Specification: `docs/SPEC.md` (§11.1.5, §5.7) <Verified claims challenged>
- Production paths: `tests/test_installed_functional.c`, `tests/test_manager_interaction_ctrl.c`, `tests/test_manager_interaction_prof.c`, `tests/test_installed_smoke.sh` <Traced installation, event dispatch, and backend communication>
- Executable evidence: `nix-shell --run 'ctest --test-dir build-check'` <PASS, but analyzed for synthetic vs. production evidence>
- Environment limits: `.factory/environment.toml` <Confirmed lack of `gpu-compositor` and `physical-controller` runners; verified that visual tests use software renderer and virtual SDL devices>

## Finding 1: Installed Functional Smoke Gap
- Requirement: `VS-01` (Installed functional smoke §11.1.5)
- Production evidence: `test_installed_functional.c` links directly against the production library and runs as a C test harness. It does not execute the installed `controller-box` binary after a `make install` cycle, thereby bypassing installation layout, library search paths, and entry-point initialization. While `test_installed_smoke.sh` exercises the installed binary, it is a basic smoke test and does not cover the full functional requirements of `VS-01` (e.g., profile persistence, target creation, and lapped recovery).
- Required remediation: Implement a full functional acceptance test that executes the installed `controller-box` binary in a clean environment and verifies all `VS-01` outcomes.

## Finding 2: Interaction Acceptance based on Synthetic Evidence
- Requirement: `IA-02`, `IA-09` through `IA-15` (Interaction E2E §5.7)
- Production evidence: The majority of the Interaction Inventory is verified via `test_manager_interaction_ctrl.c` and `test_manager_interaction_prof.c` using `ip_dbus_mock`. The implementation plan explicitly states that string-only mocks cannot satisfy `verified` status. While `test_installed_functional.c` uses a real private DBus server, it only exercises a subset of the interaction flows.
- Required remediation: Expand the use of a real private DBus server (or a running InputPlumber instance) to cover the full Interaction Inventory, replacing the reliance on `ip_dbus_mock` for `verified` claims.
