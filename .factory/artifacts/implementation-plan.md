---
spec_path: docs/SPEC.md
spec_commit: HEAD
base_commit: HEAD
status: active
---

## Task 1: Enforce host-mode freeze in the production dispatch path (B1)
Title: Enforce host-mode freeze in the production dispatch path (B1)
Status: completed
Dependencies: none
Acceptance: In src/app/overlay_service.c (cbx_overlay_input_cb DBus path and cbx_overlay_service_step SDL keyboard path), the actual sending row row_idx (from cbx_overlay_input_find_row) is passed to cbx_host_mode_handle, not the host row. A frozen (non-host) controller can no longer move the host's selected row across columns, navigate the host's profile, or exit host mode via R3. The intended freeze guard in cbx_host_mode_handle (row_idx != hm->host_row -> CBX_HM_RESULT_FROZEN) is actually exercised.
Verification: scripts/verify.sh; ctest --test-dir build -R 'test_host_mode|test_overlay_interaction|test_overlay_native' --output-on-failure
Runner: none
Evidence: fixed src/app/overlay_service.c so both dispatch paths pass the actual sending row to cbx_host_mode_handle (DBus cbx_overlay_input_cb passes row_idx from cbx_overlay_input_find_row; SDL cbx_overlay_service_step keyboard passes row 0), so the freeze guard row_idx != host_row -> CBX_HM_RESULT_FROZEN is exercised. Added interaction tests test_o11e_non_host_nav_keeps_host_selection and test_o11f_non_host_r3_keeps_host_mode to tests/test_overlay_interaction.c; both fail under the previous host_row-passing bug and pass after the fix. Verification run: nix-shell --run 'cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build --parallel' OK; ctest --test-dir build -R 'test_host_mode|test_overlay_interaction|test_overlay_native' --output-on-failure -> 100% passed (3/3); ./scripts/verify.sh -> 100% passed (91/91, 0 failures; only runner-gated skips test_kernel_controller and test_backend_smoke).

## Task 2: Make overlay virtual-device-icon visual assertions non-vacuous (B2)
Title: Make overlay virtual-device-icon visual assertions non-vacuous (B2)
Status: completed
Dependencies: none
Acceptance: tests/test_overlay_visual.c icon-region content checks must fail when the icon texture is absent. Replace has-content vs theme background assertions (which pass because the plain-cell highlight/dim fill already differs from background) with assertions that icon pixels differ from the plain cell fill color, or by reading back the specific pixels/dimensions of the rendered icon texture. Golden baselines tests/golden/overlay_*.png are regenerated deliberately (reviewed change, gated by CBX_GENERATE_GOLDEN), never merely to force a pass.
Verification: ctest --test-dir build -R 'test_overlay_visual|test_golden' --output-on-failure; scripts/verify.sh
Runner: none
Evidence: tests/test_overlay_visual.c icon-region content checks now compare against the plain cell fill colour, not the theme background. Added player_cell_fill_color() mirroring the production precedence block in cbx_select_grid_render() (current column = theme.border_focus, others = theme.border) and switched the icon-region assertions in test_player_mode_grid and test_virtual_device_icons from fb_region_has_content(.., bg, ..) to fb_region_has_content(.., fill, ..). Demonstrably non-vacuous: with the icon assets genuinely dropped (icon cache dir pointed at a non-existent path so both the fixture batch load and cbx_icon_lookup's on-demand load_one fail), test_virtual_device_icons and test_player_mode_grid FAIL — the region is left uniformly filled with the highlight colour, which the OLD background-based check falsely passed. This was reproduced on a fresh reconfigured build: pointing the fixture's cbx_icon_cache_init(...) at /nonexistent-icons made exactly those two tests FAIL (6 of 8 passed) through the real production render path; reverting to cbx_icon_dir() restored 100% pass — demonstrating the icon checks (not the generic cell-fill checks) are what fail when icons are dropped. Production rendering unchanged, so golden baselines tests/golden/overlay_*.png are unaffected (no regeneration needed). Verification run: cmake --build build --parallel OK; ctest --test-dir build -R 'test_overlay_visual|test_golden' --output-on-failure -> 100% passed (2/2); ./scripts/verify.sh -> 100% passed (91/91, 0 failures; only runner-gated skips test_kernel_controller and test_backend_smoke).

## Task 3: Add pointer-reachable confirm/cancel to profile dialogs (B3)
Title: Add pointer-reachable confirm/cancel to profile dialogs (B3)
Status: blocked
Dependencies: none
Acceptance: The name-input and delete-confirm modal dialogs in src/manager/profiles_tab.c expose clickable confirm/cancel controls routed through cbx_manager_handle_mouse_event. The requirement that every visible enabled dialog action respond to pointer hover + left-button click is met. The interaction inventory (tests/interaction_inventory.c M15/M19/M20) reflects the real, pointer-reachable controls, and pointer-path tests are added.
Verification: scripts/verify.sh; ctest --test-dir build -R 'test_manager_interaction_prof|test_profiles_tab|test_interaction_inventory|test_manager_visual' --output-on-failure
Runner: none
Evidence: verification passed but audit BLOCKERs unresolved after 3 repair cycles

## Task 4: Re-render overlay on host-mode entry and reconcile dirty triggers (W1, W2)
Title: Re-render overlay on host-mode entry and reconcile dirty triggers (W1, W2)
Status: completed
Dependencies: 1
Acceptance: Entering/exiting host mode marks the pre-built surface dirty so the presented frame reflects Host Mode as a materially different state on entry (W1). Surface dirtied-trigger policy is reconciled to spec section 4.9: dirt is triggered by device/slot/profile change and by host-mode state transitions, and the inconsistent show/save/close mark_dirty_all calls are reviewed and made deliberate/consistent rather than ad hoc.
Verification: scripts/verify.sh; ctest --test-dir build -R 'test_overlay_visual|test_overlay_interaction|test_golden' --output-on-failure
Runner: none
Evidence: W1 (host-mode entry re-render): the production overlay service wires a deliberate dirty trigger for host-mode state transitions — src/overlay/host_mode.[ch] adds cbx_hm_state_change_cb fired by cbx_host_mode_enter()/cbx_host_mode_exit() (covering the direct API and cbx_host_mode_toggle()), and src/app/overlay_service.c exports cbx_overlay_on_host_mode_change() wired as svc->hm.on_state_change in run_overlay_service step 10, so entering/exiting host mode mark the pre-built surface dirty through both live dispatch paths. Tests drive the production paths: test_host_mode.c (state-change trigger on enter/exit/toggle; MOVED/FROZEN do not fire), test_overlay_interaction.c O13/O13b (DBus dispatch marks surface dirty on host entry and again on exit, with lifecycle inactive so the flag is observed before the active-only re-render consumes it), and test_overlay_visual.c (the dirty trigger is consumed and the rendered Host Mode frame materially differs from Player Mode on the real render path). W2 (dirty-trigger policy reconciled to SPEC §4.9): dirt is deliberately triggered by device change (cbx_overlay_reconcile_hotplug / overlay_backend_ready), slot change (cbx_overlay_on_slot_change), profile change (cbx_overlay_on_profile_change), host-mode transitions (cbx_overlay_on_host_mode_change), and host navigation (CBX_HM_RESULT_MOVED/SLOT in both dispatch paths). Show/save/close reviewed for consistency: show is deliberate (lifecycle show_surface marks the freshly-presented surface dirty for a full re-render), save is deliberate (cbx_overlay_on_save marks dirty after persisting assignments/gamepad order), and close correctly does not mark dirty — the surface is hidden (hide_surface) and the next show re-dirties unconditionally, so no ad-hoc/duplicate trigger is needed. Repair: the redundant caller-side cbx_host_mode_exit() after CBX_HM_RESULT_EXIT was removed from both the DBus and keyboard dispatch paths — cbx_host_mode_handle's R3 case already exits host mode and fires the state-change trigger exactly once, so the old double-exit double-fired the dirty trigger; host-mode exit now dirties exactly once (deliberate/consistent). Verification run: ctest --test-dir build -R 'test_overlay_visual|test_overlay_interaction|test_golden|test_host_mode' --output-on-failure -> 100% passed; scripts/verify.sh -> 100% passed (91/91, only runner-gated skips). Repair 2 (doc-consistency): the compatibility audit flagged two minor documentation/API-contract consistency items — the state-change callback comment in src/overlay/host_mode.h referenced SPEC §4.4/§4.10 (Visual acceptance) while every other task-4 file consistently referenced SPEC §4.4/§4.9 (Rendering & performance, the dirty-trigger policy section). Fixed host_mode.h to SPEC §4.4/§4.9, matching overlay_service.c, overlay_service.h, test_host_mode.c, test_overlay_interaction.c, and test_overlay_visual.c; the remaining §4.10 references are all in legitimately visual-acceptance contexts (test_overlay_visual.c header, fb_assert.h). Rebuild OK; focused ctest (test_host_mode/test_overlay_visual/test_overlay_interaction/test_golden) 100% passed (4/4); scripts/verify.sh 100% passed (91/91; only runner-gated skips test_kernel_controller and test_backend_smoke). Note: one verify.sh run reported a load-induced Timeout on test_overlay_native under the full 296s suite — it passes deterministically in isolation (4.47s) and the re-run of verify.sh passed 91/91.

## Task 5: Subscribe PropertiesChanged in the production binary (W3)
Title: Subscribe PropertiesChanged in the production binary (W3)
Status: pending
Dependencies: none
Acceptance: src/dbus/ip_properties.c (ip_properties_init()/subscribe()) is actually wired into the overlay/manager backend wiring (src/app/overlay_service.c), so external changes to GamepadOrder, ProfileName, ProfilePath, TargetDevices, SourceDevicePaths are observed reactively instead of leaving the fully-implemented handler as dead code. An integration test demonstrates an externally-injected PropertiesChanged message updates internal state.
Verification: scripts/verify.sh; ctest --test-dir build -R 'test_properties_changed|test_overlay_native|test_manager_native|test_native_dbus' --output-on-failure
Runner: none
Evidence: a test showing reactive state update on an injected PropertiesChanged.

## Task 6: Re-apply persisted GamepadOrder after daemon restart (W4)
Title: Re-apply persisted GamepadOrder after daemon restart (W4)
Status: pending
Dependencies: none
Acceptance: The GUI saves GamepadOrder (already present in cbx_overlay_on_save) and now re-applies it after daemon restart / backend reacquisition. cbx_gamepad_order_restore / cbx_gamepad_order_map_ids (src/identify/gamepad_order_restore.c) are no longer dead code: they are invoked on the backend-ready / recovery path after a NameOwnerChanged re-acquisition, mapping saved persistent IDs to composite paths and setting GamepadOrder via ip_manager_set_gamepad_order.
Verification: scripts/verify.sh; ctest --test-dir build -R 'test_order_restore|test_gamepad_order|test_overlay_native|test_manager_native' --output-on-failure
Runner: none
Evidence: native restart test proving GamepadOrder re-application.

## Task 7: Add explicit typed-property readiness validation probe (W5)
Title: Add explicit typed-property readiness validation probe (W5)
Status: pending
Dependencies: none
Acceptance: Operational readiness explicitly validates required typed properties at startup / reacquisition, rather than relying only on native-signature reads failing on mismatch plus the Version gate. The requirement to validate required typed properties is satisfied by an explicit, testable probe (e.g. reading and type-checking GamepadOrder, Version, and a representative composite property with the declared u/b/as/s signatures).
Verification: scripts/verify.sh; ctest --test-dir build -R 'test_connection|test_native_dbus|test_dbus_signatures' --output-on-failure
Runner: none
Evidence: test that readiness is not declared on a property type/signature mismatch.

## Task 8: Correct controller-acceptance labeling and transport (W6)
Title: Correct controller-acceptance labeling and transport (W6)
Status: pending
Dependencies: none
Acceptance: Tests and the interaction inventory no longer label keyboard-dispatched SDL events (SDLK_a/UP/DOWN) as controller path; they are labeled supplemental keyboard evidence. At least one manager controller-acceptance test exercises the production controller transport via SDL_CONTROLLERBUTTONDOWN (the real controller signal), not synthetic keyboard events. The interaction traversal inventory is consistent with correct controller labeling.
Verification: scripts/verify.sh; ctest --test-dir build -R 'test_manager_interaction_ctrl|test_manager_interaction_prof|test_interaction_inventory|test_installed_functional' --output-on-failure
Runner: none
Evidence: relabeled inventory/tests; a SDL_CONTROLLERBUTTONDOWN-driven activation test.

## Task 9: Extend Settings virtual-controller type configuration to all configured slots (W7)
Title: Extend Settings virtual-controller type configuration to all configured slots (W7)
Status: pending
Dependencies: none
Acceptance: The Settings tab no longer limits type configuration to slots 0-3 (CBX_ST_SET_VC_TYPE_0..3). For a configured startup count up to CBX_MAX_CONTROLLERS (16), every configured slot's type is configurable in Settings. The data model already supports 16; the UI exposes it.
Verification: scripts/verify.sh; ctest --test-dir build -R 'test_settings_tab|test_manager_visual|test_golden' --output-on-failure
Runner: none
Evidence: visual/functional test for slot-4+ type configuration.

## Task 10: Harden mock expectations and intercept-poll timer hygiene (W9, C1/C2/C10/C11)
Title: Harden mock expectations and intercept-poll timer hygiene (W9, C1/C2/C10/C11)
Status: pending
Dependencies: none
Acceptance: Dead DBus expectations in Add/type-change mock tests (expecting TargetDevices read / AttachTargetDevice calls production does not make) are removed (W9). ip_intercept_poll_start resets state to IDLE on SDL_AddTimer failure (C1); poll_error_reset calls SDL_RemoveTimer(timer_id) (C2). Tests exercise the SDL_AddTimer failure path (C10) and verify timer_id is cleared after poll_error_reset (C11). No sanitizer/static defects introduced.
Verification: scripts/verify.sh; ctest --test-dir build -R 'test_intercept_poll|test_manager_interaction_ctrl|test_manager_interaction_prof|test_manager_calls' --output-on-failure; ./scripts/verify-sanitizers.sh
Runner: none
Evidence: passing poll-timer hygiene tests; clean sanitizer gate.

## Task 11: Kernel-backed controller acceptance evidence on dev-runner-vm/iprunner
Title: Kernel-backed controller acceptance evidence on dev-runner-vm/iprunner
Status: pending
Dependencies: none
Acceptance: test_kernel_controller runs with a real /dev/uinput kernel-backed synthetic gamepad and produces passing controller-transport evidence - not a silent skip and no SDL_JoystickAttachVirtual fallback being labeled kernel acceptance. The runner-capability contract routes this test to a runner that actually has /dev/uinput (declared on dev-runner-vm). Controller acceptance is satisfied with real evidence.
Verification: ctest --test-dir build -R test_kernel_controller --output-on-failure
Runner: kernel-uinput
Evidence: real test_kernel_controller pass + recorded exit code on the correct runner.

## Task 12: GPU-compositor backend acceptance evidence on gpurunner
Title: GPU-compositor backend acceptance evidence on gpurunner
Status: pending
Dependencies: none
Acceptance: The accelerated test_backend_smoke variant (OpenGL/OpenGL ES) runs on a GPU-compositor runner and produces a passing broad-framebuffer-invariant result - hardware backend acceptance, not a silent skip. (The always-runs software twin test_backend_smoke_sw already passes; this task provides the accelerated evidence.) Unreachable runner -> blocked, never fake pass.
Verification: ctest --test-dir build -R test_backend_smoke --output-on-failure
Runner: gpu-compositor
Evidence: real accelerated test_backend_smoke pass on gpurunner.

## Task 13: Build the complete spec section 11.2.1 conformance matrix
Title: Build the complete spec section 11.2.1 conformance matrix
Status: pending
Dependencies: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12
Acceptance: A machine-sectioned conformance matrix exists (committed artifact, e.g. a section of docs/ and/or a test-enumerated checklist) mapping every normative requirement in docs/SPEC.md to a classification (verified for specific source evidence + an executable test/acceptance command; nothing left partial/missing/ambiguous/assumed or verified only by prose). Every visual/interaction/backend requirement that was previously WARN or INFO is now verified with the task that closed it cited. Any requirement that cannot be verified on available runners is marked with its blocked/handoff reason, never silently passing.
Verification: ./scripts/verify.sh; test -f docs/CONFORMANCE.md && grep -q 'verified' docs/CONFORMANCE.md
Runner: none
Evidence: the committed conformance matrix + cross-reference to task evidence.

## Task 14: Final documentation and specification audit
Title: Final documentation and specification audit
Status: pending
Dependencies: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13
Acceptance: README and operational documentation match observed behavior; the canonical spec binding is fresh on develop; the full clean-build, unit, integration, end-to-end, installed-package, and project verification suites pass with no unexplained skips or weakened assertions; every active-cycle task in this ledger is complete with evidence; the conformance matrix is complete with no partial/missing requirement left unresolved; open bug ledgers contain no contradiction of a v1 requirement; and the Git tree is clean on develop. The final audit does not claim product acceptance - human release acceptance on target hardware remains required before promotion to main.
Verification: ./scripts/verify.sh; ./scripts/verify-sanitizers.sh
Runner: none
Evidence: exact verification commands and results; conformance matrix; clean tree.

