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
Status: completed
Dependencies: none
Acceptance: The name-input and delete-confirm modal dialogs in src/manager/profiles_tab.c expose clickable confirm/cancel controls routed through cbx_manager_handle_mouse_event. The requirement that every visible enabled dialog action respond to pointer hover + left-button click is met. The interaction inventory (tests/interaction_inventory.c M15/M19/M20) reflects the real, pointer-reachable controls, and pointer-path tests are added.
Verification: scripts/verify.sh; ctest --test-dir build -R 'test_manager_interaction_prof|test_profiles_tab|test_interaction_inventory|test_manager_visual' --output-on-failure
Runner: none
Evidence: Repair cycle 1: the only BLOCKER finding (compatibility: `shell.nix` unpinned nixpkgs) is pre-existing, already documented in the `shell.nix` header comment, and not introduced or affected by task 3 — no product-code change needed. Implementation at commit 336b8623 verified intact and passing.

Added pointer-reachable `dialog_confirm_btn`/`dialog_cancel_btn` cbx_buttons to src/manager/profiles_tab.c, built in cbx_profiles_tab_init, added to the panel (child count 8→10, updated in test_profiles_tab init/shutdown assertions), positioned in cbx_profiles_tab_layout below the status label, and removed in shutdown. They are shown only while a modal dialog is active — name-input shows Confirm only (M15; name-input cancel stays keyboard-only B/ESC, inventory M16); delete-confirm shows Confirm + Cancel (M19/M20) — and hidden on name-input cancel, confirm-delete cancel, editor open, and tab hide. They route through the production path cbx_manager_handle_mouse_event → panel hit-test → button on_press (on_dialog_confirm_pressed → cbx_profiles_tab_name_input_confirm / cbx_profiles_tab_confirm_delete; on_dialog_cancel_pressed → cbx_profiles_tab_cancel_delete), so the pointer path invokes the same production actions as the A/B controller path and every visible enabled dialog action responds to pointer hover (cbx_manager_update_hover sets base.hover) + left-button click.

Added pointer-path tests to tests/test_manager_interaction_prof.c that exercise hover + left-click via cbx_manager_handle_event (SDL_MOUSEMOTION + button down/up): test_prof_name_input_confirm_pointer (M15 — Confirm in name-input → editor opens with 6-bindings Default copy; checks base.hover then semantic outcome), test_prof_delete_confirm_pointer (M19 — delete-confirm Confirm → profile file myprof.yaml unlinked + list refreshed), test_prof_delete_cancel_pointer (M20 — delete-confirm Cancel → returns to list, no deletion, file retained). Each asserts semantic outcomes and calls cbx_interaction_inventory_mark_verified("M15/M19/M20"); the main epilogue asserts cbx_interaction_inventory_is_verified for all three, proving the inventory reflects the real pointer-reachable controls. Updated the M15/M19/M20 dispatch paths in tests/interaction_inventory.c to the real mouse → dialog_confirm_btn/dialog_cancel_btn → production-action routing.

Verification run: cmake --build build --parallel OK; ctest --test-dir build -R 'test_manager_interaction_prof|test_profiles_tab|test_interaction_inventory|test_manager_visual' --output-on-failure -> 100% passed (4/4); ./scripts/verify.sh -> 100% passed (91/91, 0 failures; only runner-gated skips test_kernel_controller and test_backend_smoke).

Repair Cycle 2 (security BLOCKER — modal dialog isolation): the always-present list / Create / Edit / Delete action buttons remained visible and pointer-reachable during CBX_PT_MODE_NAME_INPUT and CBX_PT_MODE_CONFIRM_DELETE, so cbx_manager_hit_test (manager.c) would dispatch clicks on them — a stray pointer click could abandon the name-input and open the delete-confirm (or vice-versa) with no feedback, undercutting the modal intent. Fixed in src/manager/profiles_tab.c by hiding profile_list_w/create_btn/edit_btn/delete_btn in cbx_profiles_tab_begin_create and cbx_profiles_tab_begin_delete (mirroring what cbx_profiles_tab_begin_create_pick does), and restoring them via show_tab_widgets in cbx_profiles_tab_name_input_cancel and cbx_profiles_tab_cancel_delete. Added a forward declaration of show_tab_widgets. Added pointer-path regression tests test_prof_name_input_modal_isolation and test_prof_delete_confirm_modal_isolation (tests/test_manager_interaction_prof.c) asserting the action buttons + list are hidden while each dialog is modal, that a cbx_manager_handle_mouse_event click on a hidden action button's rect causes no mode change (hit-test dispatches only to visible children), and that cancel restores the list/action buttons; both registered in the suite. Verification run: cmake --build build --parallel OK; ctest --test-dir build -R 'test_manager_interaction_prof|test_profiles_tab|test_interaction_inventory|test_manager_visual' --output-on-failure -> 100% passed (4/4); broader ctest --test-dir build -R 'manager|profiles_tab|profiles' --output-on-failure -> 100% passed (12/12). Full 91-test suite shows 2 failures unrelated to task 3 and pre-existing/environmental: test_packaging (clean-configure step fails to find sdl2 on pkg-config outside Nix) and test_flatpak_manifest (deferred to the isolated installed-package runner).

Repair Cycle 3 (efficiency audit — BLOCKER: none): no blocker findings to fix; implementation from repair cycles 1 and 2 verified intact and passing. Rebuilt (cmake --build build --parallel OK) and reran the focused task-3 command `ctest --test-dir build -R 'test_manager_interaction_prof|test_profiles_tab|test_interaction_inventory|test_manager_visual' --output-on-failure` -> 100% passed (4/4). Full gate `./scripts/verify.sh` -> 100% passed (91/91, 0 failures; only runner-gated skips test_kernel_controller needing /dev/uinput and test_backend_smoke needing a GPU runner, the software twin passing). No product-code change required this cycle.

## Task 4: Re-render overlay on host-mode entry and reconcile dirty triggers (W1, W2)
Title: Re-render overlay on host-mode entry and reconcile dirty triggers (W1, W2)
Status: pending
Dependencies: 1
Acceptance: Entering/exiting host mode marks the pre-built surface dirty so the presented frame reflects Host Mode as a materially different state on entry (W1). Surface dirtied-trigger policy is reconciled to spec section 4.9: dirt is triggered by device/slot/profile change and by host-mode state transitions, and the inconsistent show/save/close mark_dirty_all calls are reviewed and made deliberate/consistent rather than ad hoc.
Verification: scripts/verify.sh; ctest --test-dir build -R 'test_overlay_visual|test_overlay_interaction|test_golden' --output-on-failure
Runner: none
Evidence: visual region diff on host-mode entry.

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

