---
spec_path: docs/SPEC.md
spec_commit: HEAD
base_commit: HEAD
status: active
---

## Task 1: Enforce host-mode freeze in the production dispatch path (B1)
Title: Enforce host-mode freeze in the production dispatch path (B1)
Status: pending
Dependencies: none
Acceptance: In src/app/overlay_service.c (cbx_overlay_input_cb DBus path and cbx_overlay_service_step SDL keyboard path), the actual sending row row_idx (from cbx_overlay_input_find_row) is passed to cbx_host_mode_handle, not the host row. A frozen (non-host) controller can no longer move the host's selected row across columns, navigate the host's profile, or exit host mode via R3. The intended freeze guard in cbx_host_mode_handle (row_idx != hm->host_row -> CBX_HM_RESULT_FROZEN) is actually exercised.
Verification: scripts/verify.sh; ctest --test-dir build -R 'test_host_mode|test_overlay_interaction|test_overlay_native' --output-on-failure. Extended interaction tests must assert the host's selected row/column is unchanged after a non-host controller navigates and that a non-host R3 does not exit host mode.
Runner: none
Evidence: updated interaction tests asserting host selection stability.

## Task 2: Make overlay virtual-device-icon visual assertions non-vacuous (B2)
Title: Make overlay virtual-device-icon visual assertions non-vacuous (B2)
Status: pending
Dependencies: none
Acceptance: tests/test_overlay_visual.c icon-region content checks must fail when the icon texture is absent. Replace has-content vs theme background assertions (which pass because the plain-cell highlight/dim fill already differs from background) with assertions that icon pixels differ from the plain cell fill color, or by reading back the specific pixels/dimensions of the rendered icon texture. Golden baselines tests/golden/overlay_*.png are regenerated deliberately (reviewed change, gated by CBX_GENERATE_GOLDEN), never merely to force a pass.
Verification: ctest --test-dir build -R 'test_overlay_visual|test_golden' --output-on-failure; scripts/verify.sh.
Runner: none
Evidence: a test that demonstrably fails when icons are dropped.

## Task 3: Add pointer-reachable confirm/cancel to profile dialogs (B3)
Title: Add pointer-reachable confirm/cancel to profile dialogs (B3)
Status: pending
Dependencies: none
Acceptance: The name-input and delete-confirm modal dialogs in src/manager/profiles_tab.c expose clickable confirm/cancel controls routed through cbx_manager_handle_mouse_event. The requirement that every visible enabled dialog action respond to pointer hover + left-button click is met. The interaction inventory (tests/interaction_inventory.c M15/M19/M20) reflects the real, pointer-reachable controls, and pointer-path tests are added.
Verification: scripts/verify.sh; ctest --test-dir build -R 'test_manager_interaction_prof|test_profiles_tab|test_interaction_inventory|test_manager_visual' --output-on-failure. Pointer-path tests activate confirm and cancel through production SDL dispatch (mouse motion + down/up) and assert the semantic outcome.
Runner: none
Evidence: passing pointer-path tests for name-input and delete-confirm dialogs.

## Task 4: Re-render overlay on host-mode entry and reconcile dirty triggers (W1, W2)
Title: Re-render overlay on host-mode entry and reconcile dirty triggers (W1, W2)
Status: pending
Dependencies: 1
Acceptance: Entering/exiting host mode marks the pre-built surface dirty so the presented frame reflects Host Mode as a materially different state on entry (W1). Surface dirtied-trigger policy is reconciled to spec section 4.9: dirt is triggered by device/slot/profile change and by host-mode state transitions, and the inconsistent show/save/close mark_dirty_all calls are reviewed and made deliberate/consistent rather than ad hoc.
Verification: scripts/verify.sh; ctest --test-dir build -R 'test_overlay_visual|test_overlay_interaction|test_golden' --output-on-failure. Region assertions must show a materially different frame between Player Mode and Host Mode without any host navigation key press.
Runner: none
Evidence: visual region diff on host-mode entry.

## Task 5: Subscribe PropertiesChanged in the production binary (W3)
Title: Subscribe PropertiesChanged in the production binary (W3)
Status: pending
Dependencies: none
Acceptance: src/dbus/ip_properties.c (ip_properties_init()/subscribe()) is actually wired into the overlay/manager backend wiring (src/app/overlay_service.c), so external changes to GamepadOrder, ProfileName, ProfilePath, TargetDevices, SourceDevicePaths are observed reactively instead of leaving the fully-implemented handler as dead code. An integration test demonstrates an externally-injected PropertiesChanged message updates internal state.
Verification: scripts/verify.sh; ctest --test-dir build -R 'test_properties_changed|test_overlay_native|test_manager_native|test_native_dbus' --output-on-failure.
Runner: none
Evidence: a test showing reactive state update on an injected PropertiesChanged.

## Task 6: Re-apply persisted GamepadOrder after daemon restart (W4)
Title: Re-apply persisted GamepadOrder after daemon restart (W4)
Status: pending
Dependencies: none
Acceptance: The GUI saves GamepadOrder (already present in cbx_overlay_on_save) and now re-applies it after daemon restart / backend reacquisition. cbx_gamepad_order_restore / cbx_gamepad_order_map_ids (src/identify/gamepad_order_restore.c) are no longer dead code: they are invoked on the backend-ready / recovery path after a NameOwnerChanged re-acquisition, mapping saved persistent IDs to composite paths and setting GamepadOrder via ip_manager_set_gamepad_order.
Verification: scripts/verify.sh; ctest --test-dir build -R 'test_order_restore|test_gamepad_order|test_overlay_native|test_manager_native' --output-on-failure. A native-signature test restarts the DBus server and asserts the order is restored after re-enumeration.
Runner: none
Evidence: native restart test proving GamepadOrder re-application.

## Task 7: Add explicit typed-property readiness validation probe (W5)
Title: Add explicit typed-property readiness validation probe (W5)
Status: pending
Dependencies: none
Acceptance: Operational readiness explicitly validates required typed properties at startup / reacquisition, rather than relying only on native-signature reads failing on mismatch plus the Version gate. The requirement to validate required typed properties is satisfied by an explicit, testable probe (e.g. reading and type-checking GamepadOrder, Version, and a representative composite property with the declared u/b/as/s signatures).
Verification: scripts/verify.sh; ctest --test-dir build -R 'test_connection|test_native_dbus|test_dbus_signatures' --output-on-failure.
Runner: none
Evidence: test that readiness is not declared on a property type/signature mismatch.

## Task 8: Correct controller-acceptance labeling and transport (W6)
Title: Correct controller-acceptance labeling and transport (W6)
Status: pending
Dependencies: none
Acceptance: Tests and the interaction inventory no longer label keyboard-dispatched SDL events (SDLK_a/UP/DOWN) as controller path; they are labeled supplemental keyboard evidence. At least one manager controller-acceptance test exercises the production controller transport via SDL_CONTROLLERBUTTONDOWN (the real controller signal), not synthetic keyboard events. The interaction traversal inventory is consistent with correct controller labeling.
Verification: scripts/verify.sh; ctest --test-dir build -R 'test_manager_interaction_ctrl|test_manager_interaction_prof|test_interaction_inventory|test_installed_functional' --output-on-failure.
Runner: none
Evidence: relabeled inventory/tests; a SDL_CONTROLLERBUTTONDOWN-driven activation test.

## Task 9: Extend Settings virtual-controller type configuration to all configured slots (W7)
Title: Extend Settings virtual-controller type configuration to all configured slots (W7)
Status: pending
Dependencies: none
Acceptance: The Settings tab no longer limits type configuration to slots 0-3 (CBX_ST_SET_VC_TYPE_0..3). For a configured startup count up to CBX_MAX_CONTROLLERS (16), every configured slot's type is configurable in Settings. The data model already supports 16; the UI exposes it.
Verification: scripts/verify.sh; ctest --test-dir build -R 'test_settings_tab|test_manager_visual|test_golden' --output-on-failure. Visual test asserts a slot-4+ type row is rendered and configurable.
Runner: none
Evidence: visual/functional test for slot-4+ type configuration.

## Task 10: Harden mock expectations and intercept-poll timer hygiene (W9, C1/C2/C10/C11)
Title: Harden mock expectations and intercept-poll timer hygiene (W9, C1/C2/C10/C11)
Status: pending
Dependencies: none
Acceptance: Dead DBus expectations in Add/type-change mock tests (expecting TargetDevices read / AttachTargetDevice calls production does not make) are removed (W9). ip_intercept_poll_start resets state to IDLE on SDL_AddTimer failure (C1); poll_error_reset calls SDL_RemoveTimer(timer_id) (C2). Tests exercise the SDL_AddTimer failure path (C10) and verify timer_id is cleared after poll_error_reset (C11). No sanitizer/static defects introduced.
Verification: scripts/verify.sh; ctest --test-dir build -R 'test_intercept_poll|test_manager_interaction_ctrl|test_manager_interaction_prof|test_manager_calls' --output-on-failure; ./scripts/verify-sanitizers.sh.
Runner: none
Evidence: passing poll-timer hygiene tests; clean sanitizer gate.

## Task 11: Kernel-backed controller acceptance evidence on dev-runner-vm/iprunner
Title: Kernel-backed controller acceptance evidence on dev-runner-vm/iprunner
Status: pending
Dependencies: none
Runner: kernel-uinput
Acceptance: test_kernel_controller runs with a real /dev/uinput kernel-backed synthetic gamepad and produces passing controller-transport evidence - not a silent skip and no SDL_JoystickAttachVirtual fallback being labeled kernel acceptance. The runner-capability contract routes this test to a runner that actually has /dev/uinput (declared on dev-runner-vm). Controller acceptance is satisfied with real evidence.
Verification: ctest --test-dir build -R test_kernel_controller --output-on-failure on a runner with kernel-uinput/physical-controller; result recorded. Unreachable runner -> blocked, never fake pass.
Evidence: real test_kernel_controller pass + recorded exit code on the correct runner.

## Task 12: GPU-compositor backend acceptance evidence on gpurunner
Title: GPU-compositor backend acceptance evidence on gpurunner
Status: pending
Dependencies: none
Runner: gpu-compositor
Acceptance: The accelerated test_backend_smoke variant (OpenGL/OpenGL ES) runs on a GPU-compositor runner and produces a passing broad-framebuffer-invariant result - hardware backend acceptance, not a silent skip. (The always-runs software twin test_backend_smoke_sw already passes; this task provides the accelerated evidence.) Unreachable runner -> blocked, never fake pass.
Verification: ctest --test-dir build -R test_backend_smoke --output-on-failure on a runner with gpu-compositor; result recorded.
Evidence: real accelerated test_backend_smoke pass on gpurunner.

## Task 13: Build the complete spec section 11.2.1 conformance matrix
Title: Build the complete spec section 11.2.1 conformance matrix
Status: pending
Dependencies: 1,2,3,4,5,6,7,8,9,10,11,12
Acceptance: A machine-sectioned conformance matrix exists (committed artifact, e.g. a section of docs/ and/or a test-enumerated checklist) mapping every normative requirement in docs/SPEC.md to a classification (verified for specific source evidence + an executable test/acceptance command; nothing left partial/missing/ambiguous/assumed or verified only by prose). Every visual/interaction/backend requirement that was previously WARN or INFO is now verified with the task that closed it cited. Any requirement that cannot be verified on available runners is marked with its blocked/handoff reason, never silently passing.
Verification: matrix parses and every normative spec requirement has a row with a classification and an executable acceptance command; spot-check that each remediation task's acceptance maps to a verified row.
Runner: none
Evidence: the committed conformance matrix + cross-reference to task evidence.

## Task 14: Final documentation and specification audit
Title: Final documentation and specification audit
Status: pending
Dependencies: 1,2,3,4,5,6,7,8,9,10,11,12,13
Acceptance: README and operational documentation match observed behavior; the canonical spec binding is fresh on develop; the full clean-build, unit, integration, end-to-end, installed-package, and project verification suites pass with no unexplained skips or weakened assertions; every active-cycle task in this ledger is complete with evidence; the conformance matrix is complete with no partial/missing requirement left unresolved; open bug ledgers contain no contradiction of a v1 requirement; and the Git tree is clean on develop. The final audit does not claim product acceptance - human release acceptance on target hardware remains required before promotion to main.
Verification: ./scripts/verify.sh; ./scripts/verify-sanitizers.sh; clean-checkout rebuild from develop; plan parser accepts the ledger.
Runner: none
Evidence: exact verification commands and results; conformance matrix; clean tree.
