# Handoff: Task 10 (F3 sender credential verification + F6 mock gating) complete; Task 11 remains

## Outcome
- Completed Task 10 (plan:task-10): Security hardening — DBus sender credential
  verification (F3) + test-mock gating (F6). Commit a890ba1 (code/tests), 5b454fd
  (plan evidence).
- F3: added `ip_dbus_backend.get_connection_creds` (sd-bus GetConnectionCredentials;
  mock-configurable pid/uid). `ip_connection` now credential-verifies the owner after
  `GetNameOwner` and on every `NameOwnerChanged`, applies anti-squatting UID policy
  (`ip_connection_uid_is_trusted`: root or self-uid), exposes `is_sender_verified`,
  and `sd_sender_ok` rejects signals when no trusted sender is tracked (down-state).
  Untrusted/squatting owners stay degraded and never become a trusted sender.
- F6: `cbx_service_set_mock_*` gated behind `#ifdef CBX_TESTING`. New
  `controllerbox_testing` static library (same sources + CBX_TESTING) serves the 3
  tests that use the overrides; `nm` confirms the release library and binary carry no
  mock symbols.
- Verification: full ctest 100% pass (98 pass / 2 environmental skips:
  `test_kernel_controller`, `test_backend_smoke`); ASan+UBSan clean with project LSAN
  suppressions (the only bare-leak hit, test_text/harfbuzz, is the known suppressed
  third-party leak); +6 connection tests; verify-boilerplate + check-docs-sync pass;
  validate-implementation-plan.py planning exit 0. OPERATIONS.md trust-boundary note added.

## Remaining ready tasks (all P2, unblocked)
- Task 11 (task-1787286665-c52b): Test-quality — §5.7 semantic-outcome gaps (findings
  1-8). Scope: test_kernel_controller, test_overlay_native, test_manager_native_prof,
  test_manager_visual, test_golden, test_installed_smoke.sh, test_interaction_inventory.

## Next
Pick Task 11 next (only ready task). Re-check `ralph tools task ready` at start.
No change to open facts (FACT-002..FACT-007), 19 partial rows, Task 6 blocked, Task 4
audit blocked — unchanged; do not emit the completion token (ledger open).
