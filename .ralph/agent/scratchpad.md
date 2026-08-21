# Handoff: Task 11 (§5.7 semantic-outcome test-quality) work complete; ledger open

## Outcome
- Completed the Task 11 remediation of all 8 §5.7 semantic-outcome gaps across
  7 test files + 1 production file. Full ctest green: 100 tests, 98 pass /
  2 environmental skips (`test_kernel_controller`, `test_backend_smoke`).
  verify-boilerplate, check-docs-sync, validate-implementation-plan (planning),
  validate-conformance (planning) all pass.
- Finding 1 (kernel): settings.yaml check is now a HARD semantic assertion; the
  navigation deliberately reaches the Settings tab and the settings list's
  "Save" entry (writes settings.yaml); Xbox 360 identity (0x045E:0x028E) is
  recognized via SDL's built-in controller DB (no manual mapping needed).
  Environmental skip locally (no /dev/uinput); needs kernel-uinput runner.
- Finding 2 (overlay): O10/O10b/O13 close now via DBus InputEvent transport
  (emit_input_event(...,"B",1.0)+drain_bus), not push_keydown(SDLK_b).
- Findings 3-5 (native_prof): M36 uses ctrl_press(6) Start (not SDLK_TAB);
  M37 asserts on-disk profile change (st_mtim.tv_nsec advanced, or file created);
  M30 asserts the picked target was applied to mappings[editing_index].
- Finding 6: validation-error visual produced by the production save path —
  test loads incomplete profile into ed->profile and clicks the real Save button;
  cbx_profiles_tab_save_editor now also sets status_lbl to theme->conflict (red).
- Finding 7 (installed_smoke): overlay early-exit is FAILURE per §11.1.5, not
  pass; the script now starts a private native-signature InputPlumber-compatible
  service (build-check/test_ip_server) and points DBUS_SYSTEM_BUS_ADDRESS at it
  so the installed overlay is genuinely exercised. test_installed_smoke passes.
- Finding 8: added a runtime verification ledger to interaction_inventory
  (mark_verified/is_verified/reset); verified flags now reflect actual passing
  dispatch tests, not static claims. Dispatch tests (M30/M36/M37, O10/O13) mark
  after passing; each binary's main() asserts runtime-verified. Linked
  cbx_test_support to test_overlay_native and test_manager_native_prof.

## Verification
- `nix-shell --run 'ctest --test-dir build-check --output-on-failure'`: 98 pass / 2 skips.
- `./scripts/verify-boilerplate.sh`, `./scripts/check-docs-sync.sh`: pass.
- `./scripts/validate-implementation-plan.py planning`: exit 0.
- `./scripts/validate-conformance.py planning`: valid (76 requirements).

## Plan / task state
- Plan Task 11 kept `Status: pending` (appended-after-final-audit convention;
  only Task 4 gates completion). Result section documents all 8 remediations.
- Runtime task task-1787286665-c52b closed (work done).
- No change to open facts (FACT-002..FACT-008), 19 partial rows, Task 6 blocked,
  Task 4 audit blocked — unchanged; do not emit the completion token (ledger open).

## Next
No ready runtime tasks remain. The final audit (Task 4) remains blocked pending
resolved facts. If a fresh cycle continues, re-check `ralph tools task ready`
and the plan's open/blocked rows (Task 6, Task 4). Commit the checkpoint to
`develop` with the scratchpad riding along.
