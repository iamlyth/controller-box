# Handoff: test-quality gaps closed; final audit still blocked on external facts

## Outcome this iteration
Closed the ready runtime task `task-1787293432-7333` (software-fixable test-quality
gaps from reviewer audit). Plan remains `active` + fresh (spec=3a10f6b7d04a,
blob=58f5d3cb72bc). Uncommitted changes on `develop`.

- **(1) No-op assertions removed.** test_editor_list_mode.c: fixed the
  `strlen(status)==0||>0` tautology (now asserts empty in list mode) and replaced
  `assert_true(1)`-after-draw in test_render_no_crash/test_render_target_pick with
  real `fb_read_pixels`+`fb_region_has_content` checks on the diagram region.
  test_profile_diagram.c: replaced `assert_true(1)` in test_render_no_crash,
  test_render_all_buttons, test_render_with_rect, test_shutdown_cleans_up with
  framebuffer content + `diag_assert_highlight` (highlighted button differs from
  panel_bg {30,30,42}, tol 15) + resource-release/idempotency checks.
- **(2) Production callbacks wired.** overlay_service.c exposes
  `on_intercept_activating/deactivating/error` as non-static under CBX_TESTING
  (still static in release); declared in overlay_service.h under the gate.
  test_overlay_native.c now registers the production callbacks (dropped local
  test_on_* copies); tests/CMakeLists.txt links `controllerbox_testing` +
  defines CBX_TESTING. nm confirms release `controllerbox` keeps them `t`.
- **(3) Poll transition via production path.** activate_overlay()/test_o01/test_o01b
  now use `ip_intercept_poll_start()` (IDLE→PASS_WAIT) + `ip_intercept_poll_tick`
  (PASS_WAIT→ACTIVE via InterceptMode read); no `polls[].state = IP_POLL_*`
  mutation remains. Repeated test_overlay_native 10× — deterministic.
- **(4) get_unique_name failure branch tested.** dbus_mock gains
  `unique_name_rc` + `ip_dbus_mock_set_unique_name_fail()`; test_connect_unique_name_fail
  asserts connect still succeeds (rc 0, CONNECTED) but sender unverified +
  unique_name NULL.
- Full suite 100/100 pass (2 pre-existing hardware skips: test_kernel_controller,
  test_backend_smoke). verify-boilerplate passes. Added Task 13 section to
  implementation-plan recording evidence.

## Blocked-facts ledger (unchanged)
8 facts; FACT-001/008 resolved; FACT-002..007 open — all external hardware/
capability/signer blockers (real InputPlumber system-bus, target consumer / Pi 4,
gpu-compositor, target Pi runtime + human release acceptance, runner-evidence
signer re-run). Runner VM unreachable (no ~/.ssh/factory-ssh); FACT-007 signer
provisioned but receipt stale vs HEAD and untracked.

## Next
- Commit the substantive test-quality checkpoint to `develop` (guard allows
  substantive commits carrying scratchpad). Final audit (Task 4) remains blocked
  on external facts; requires human hardware/capability/signer re-run or explicit
  spec-scoped deferral.
- Do not emit the completion token; ledger stays open.
