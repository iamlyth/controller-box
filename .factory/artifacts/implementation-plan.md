---
spec_path: docs/SPEC.md
spec_commit: 3a10f6b7d04a615b2b9d06eef6c91e431fa9c079
spec_blob: 58f5d3cb72bc6b3e5f573fa09a63c11a653ed577
base_commit: d110dfac9abc39ca4206986d9e7c2dcaf6b1d7c1
status: active
---

# Implementation Plan

## Goal and non-goals

**Goal:** Close every remaining gap between the committed specification
(`docs/SPEC.md`) and the production codebase so that the autonomous definition
of done (§11.2) is satisfied. The codebase is mature (23.5 KLoC source, 62 KLoC
tests, 124 source files, 80+ test files). The remaining gaps are: (1) two small
code/test deficiencies, (2) five runner-capability-dependent requirements that
cannot be verified in the current declared environment, and (3) the final audit
that binds everything together.

**Non-goals:** Re-implementing working features. Retrieving tasks from prior
plans in Git history. Modifying the committed specification. Inventing
undeclared runner capabilities.

## Architecture and constraints

- One binary, two modes: `controller-box --overlay-service` (systemd user
  daemon) and `controller-box --manager` (on-demand config UI). Shared codebase,
  SDL2 widget toolkit, sd-bus DBus client, libyaml config, nanosvg icons.
- Engine/engine split: InputPlumber owns input routing; the GUI is a control
  surface that talks over system DBus. Never bundled.
- Declared environment (`.factory/environment.toml`): one runner
  (`dev-runner-vm`) with `remote-project-gate` and `systemd-user` only. Six of
  seven campaign-required capabilities (`physical-controller`, `kernel-uinput`,
  `inputplumber-system-dbus`, `gpu-compositor`, `installed-package`,
  `target-consumer`) are **not declared** and have no runner evidence. The
  environment declaration is exhaustive; tasks must not invent capabilities.
- Build/test via Nix shell: `nix-shell --run 'cmake -S . -B build-check -DCMAKE_BUILD_TYPE=Debug && cmake --build build-check --parallel'`.
- Full verification: `nix-shell --run './scripts/verify-project.sh'`.

## Specification conformance matrix

Requirements are grouped by spec section. `verified` requires production-path
evidence (real init, dispatch, rendering, backend, persistence, shutdown).
Skips, keyboard proxies, string-only mocks, and fixture-only assembly cannot
satisfy `verified`.

| Req ID | Spec § | Classification | Evidence | Task |
|--------|--------|---------------|----------|------|
| OVL-01 | 4.1 | verified | `grid_render.c:95-132` build; `test_overlay_visual.c:test_player_mode_grid` framebuffer; `test_golden.c:overlay_player_mode` | — |
| OVL-02 | 4.2 | verified | `trigger.c:62-152` parse+register; `test_trigger.c` 19 tests; `test_overlay_interaction.c:test_o01` production dispatch | — |
| OVL-03 | 4.3 | verified | `player_mode.c:41-48`; `test_player_mode.c:test_independence`; `test_o11_multi_controller_independent` DBus InputEvent | — |
| OVL-04 | 4.4 | verified | `host_mode.c:51-137` enter/toggle/freeze/navigate; `test_host_mode.c` 42 tests; `test_host_mode_row_states` framebuffer; `test_o06-o09` production | — |
| OVL-05 | 4.5 | verified | `conflict.c:35-140` detect+resolve; `test_conflict.c` 34 tests incl spec example; `test_conflict_highlighting` red pixels; `test_o10b` production | — |
| OVL-06 | 4.6 | verified | `profile_cycle.c:113-189`; `test_profile_cycle.c` 18 tests; `test_o04/o05` production dispatch | — |
| OVL-07 | 4.7 | verified | `dynamic_columns.c:15-119`; `test_dynamic_columns.c` 24 tests; `test_hotplug_target_add_remove_through_dispatch` | — |
| OVL-08 | 4.8 | verified | `grid_render.c:116-120,430-446` model_name from DBus only; no nickname UI; `test_model_profile_text` framebuffer | — |
| OVL-09 | 4.9 | verified | `surface_build.c:38-108` pre-built; `test_overlay_latency.c` 8 tests: p99<10ms show, median<1ms close, p99<5ms idle | — |
| OVL-10 | 4.10 | verified | `test_overlay_visual.c` 8 framebuffer tests (player, host, conflict, unassigned, text, icons, transitions); `test_golden.c` 4 overlay golden images | — |
| MGR-01 | 5.1 | verified | `manager.c:160-167` 3 tabs; `test_tab_switch_controller/pointer_path`; `test_tab_switch_differs` framebuffer | — |
| MGR-02 | 5.1 | verified | `manager.c:290-375` controller-to-key + A activates; `test_ctrl_list_select_controller_path`; native `test_m04` | — |
| MGR-03 | 5.1 | verified | `manager.c:390-440` hit-test all visible widgets; 20+ pointer-path tests; `test_mg15_resize_hit_testing` | — |
| MGR-04 | 5.1 | verified | `manager.c:447-465` focus chain skips non-interactive; `test_init_without_dbus` asserts `interactive==false` | — |
| MGR-05 | 5.2 | verified | `controllers_tab.c` add/remove; `test_add_success`, `test_remove_success`; `test_ctrl_add_confirm_*` both paths; native `test_d06` | — |
| MGR-06 | 5.2 | verified | `controllers_tab.c` type picker from `SupportedTargetDeviceIds`; `test_change_type_success`; `test_ctrl_change_type_confirm_*` | — |
| MGR-07 | 5.2 | verified | `test_change_type_mixed` preserves other slots; mock DBus verifies `SetTargetDevices` called | — |
| MGR-08 | 5.2 | verified | `controllers_tab.c` auto-unassign on remove; `test_remove_auto_unassign`, `test_remove_shifts_higher_slots`; production dispatch test | — |
| MGR-09 | 5.2 | verified | `test_add_rejects_unconfirmed_model`, `test_add_rejects_type_mismatch`, `test_orphan_columns_shows_error`; native `test_mg04_topology_failure` | — |
| MGR-10 | 5.3 | verified | `profiles_tab.c` enumerate/create/delete; `test_full_workflow`; `test_create_picker_via_dispatch`, `test_confirm_delete_via_dispatch` | — |
| MGR-11 | 5.3 | verified | `test_clean_install_default_copy_uses_shipped` builtin dir; `test_delete_default_rejected` `-EINVAL` | — |
| MGR-12 | 5.3 | verified | `test_create_default_copy/empty/clone`; `test_name_input_confirm` editor mode + save/discard visible; dispatch tests | — |
| MGR-13 | 5.3 | verified | `profile_save.c` atomic YAML write; `test_save_valid_profile` round-trip; `test_save_through_symlink_uses_canonical` | — |
| MGR-14 | 5.3 | verified | `test_activate_empty_starts_sequential`; `manager.c:240-258` SDL_QUIT dirty-check → confirm_quit; explicit save/discard buttons | — |
| MGR-15 | 5.4 | verified | `profile_editor_list.c` shared diagram; `test_diagram_sync_on_load/move`; `test_profile_editor_list_mode` framebuffer | — |
| MGR-16 | 5.4 | verified | `test_activate_enters_target_pick`, `test_begin_capture`, `test_capture_input_event`, `test_expected_sender_rejects_mismatch` | — |
| MGR-17 | 5.4 | verified | `profile_editor_seq.c`; `test_seq_capture_auto_advance`, `test_seq_skip_via_b_input`, `test_seq_cancel_via_start`, `test_seq_progress_increases` | — |
| MGR-18 | 5.4 | verified | `profile_validate.c` 6 NES buttons; `test_nes_minimum_count==6`, `test_validate_missing_a`, `test_reject_missing_nes_minimum` | — |
| MGR-19 | 5.4 | verified | `test_load_capabilities_dbus` queries TargetCapabilities; `test_profile_portability_same_result` round-trip | — |
| MGR-20 | 5.4 | verified | `test_profile_determinism_load_twice` asserts identical mappings on reload | — |
| MGR-21 | 5.5 | verified | `settings_tab.c` all settings; `test_init_loads_defaults`, `test_edit_opacity/theme/vc_count/vc_type/trigger/icon_override` | — |
| MGR-22 | 5.5 | verified | `test_save_writes_settings` reload from disk; `test_settings_save_controller/pointer_path` production dispatch + file exists | — |
| MGR-23 | 5.6 | verified | `test_manager_visual.c` all 3 tabs framebuffer; `test_controllers_tab_degraded/connected`, `test_profiles_tab`, `test_settings_tab` | — |
| MGR-24 | 5.6 | verified | `test_controllers_tab_degraded` action buttons absent; `test_controllers_connected_vs_degraded` frames differ | — |
| MGR-25 | 5.6 | verified | `test_profile_editor_list/sequential/validation_error` framebuffer; progress bar region differs; red error pixels | — |
| MGR-26 | 5.6 | verified | `test_tab_switch_differs` all transitions; `test_settings_edit_changes_region` | — |
| MGR-27 | 5.6 | verified | `test_focus_visual_indication`, `test_press_visual_indication` framebuffer differs on hover+press | — |
| MGR-28 | 5.7 | verified | `interaction_inventory.c` 59 entries (M01-M38, O01-O13, D01-D08); `test_interaction_inventory.c` 13 tests | — |
| MGR-29 | 5.7 | verified | Every M-control has `_controller_path` + `_pointer_path` tests; native tests with virtual gamepad both paths | — |
| MGR-30 | 5.7 | verified | All tests call `cbx_manager_handle_event` (controller) or `cbx_manager_handle_mouse_event` (pointer); no direct callback | — |
| MGR-31 | 5.7 | verified | Tests assert DBus calls, file mutations, persisted reloads, mode transitions, framebuffer diffs — not return values | — |
| MGR-32 | 5.7 | verified | `test_init_without_dbus` `interactive==false`; `test_d06_dbus_failure` count unchanged + error visible | — |
| MGR-33 | 5.7 | verified | `test_mg15_resize_hit_testing` post-resize click hits correct widget at new position | — |
| MGR-34 | 5.7 | verified | E2E scenarios: add/remove/type, create/edit/validate/save/delete, settings persist, editor cancel/error, tab switch, recovery | — |
| MGR-35 | 5.7 | verified | `test_manager_native.c` uses `ip_dbus_sd_backend()` + private dbus-daemon + forked native IP server; mock is supplemental | — |
| MGR-36 | 5.7 | partial | `test_manager_native.c` uses `SDL_JoystickAttachVirtual` (process-local virtual gamepad). SPEC §5.7 requires "physical or kernel-backed synthetic gamepad." `test_kernel_controller.c` skips (exit 77, no `/dev/uinput`). `physical-controller`/`kernel-uinput` capabilities not declared in environment. | Task 3 |
| ID-01 | 6.2 | verified | `identity.c:176-296` 4-layer extract; `test_extract_bt_mac`, `test_extract_usb_serial_evdev/hidraw`, `test_extract_usb_phys`, `test_extract_order` | — |
| ID-02 | 6.2 | verified | `test_layer_precedence_bt_over_serial/serial_over_phys/phys_over_order/all_present` | — |
| ID-03 | 6.2 | verified | `assign_persist.c:auto_assign` returns existing on ID match; `test_auto_assign_existing` zero-adjustment | — |
| ID-04 | 6.3 | verified | `identity.c:299-372` parse_layer + is_downgrade; `test_parse_layer_roundtrip`, `test_extract_and_parse_all_layers` | — |
| ID-05 | 6.3 | verified | `identity_downgrade.c:59-177` check+find+resolve; `test_identity_downgrade.c` 33 tests all downgrade paths | — |
| CFG-01 | 7.1 | verified | `config_profile.c` generates `version:1, kind:DeviceProfile`; `test_parse_spec_example` SPEC example; `test_native_assignment_application` LoadProfilePath | — |
| CFG-02 | 7.2 | verified | `config_paths.c:47-68` XDG resolution; `test_config_dir_xdg`, `test_profiles_dir_xdg` | — |
| CFG-03 | 7.3 | verified | `config_settings.c` all fields; `test_defaults_function`, `test_round_trip`, `test_validate_opacity/count_out_of_range` | — |
| CFG-04 | 7.3 | verified | `config_settings.c:360-420` mkstemp+fchmod0600+fsync+rename; `test_file_mode_0600` | — |
| CFG-05 | 7.4 | verified | `config_assignments.c` parse+emit; `test_round_trip` with BT/USB/phys/ORDER IDs; `test_validate_id_*` | — |
| CFG-06 | 7.4 | verified | `ip_gamepad_order.c` save/load; `gamepad_order_restore.c`; `test_save_success`, `test_restore_success`, `test_restore_round_trip` | — |
| CFG-07 | 7.5 | verified | `config_profile_meta.c` sidecar; `test_profile_meta` optional; realpath containment; O_NOFOLLOW; atomic write | — |
| CFG-08 | 7.6 | verified | `config_profile.c` InputPlumber YAML; `test_parse_spec_example`, `test_round_trip`, `test_file_round_trip`; advanced types skipped via `skip_depth` | — |
| DBUS-01 | 10.1 | verified | `ip_connection.c:92-147` connect+subscribe+Version+enumerate; `test_connect_success`, `test_native_owner_loss_and_reacquisition` | — |
| DBUS-02 | 10.1 | verified | `ip_hotplug.c:60-105` InterfacesAdded/Removed; `test_hotplug_subscribe`, `test_handle_added/removed_composite` | — |
| DBUS-03 | 10.1 | verified | `ip_properties.c:36-46` 5 properties; `test_handle_gamepadorder/profilename/profilepath/targetdevices/sourcedevicepaths` | — |
| DBUS-04 | 10.1 | verified | `dbus_client.c:540-562` native signatures (`u`,`b`,`as`); `test_dbus_signatures.c` asserts exact types; `test_native_dbus.c` round-trip | — |
| DBUS-05 | 10.1 | verified | `native_ip_server.c` exports real vtables: InterceptMode `u`, GamepadOrder `as`, InputEvent `sd`, SetInterceptActivation `ass` | — |
| DBUS-06 | 10.2 | verified | `ip_intercept_poll.c` state machine; `test_intercept_poll.c` 24 tests; `test_o01_open_lifecycle_activates` | — |
| DBUS-07 | 10.2 | verified | `SupportedTargetDevices: as` wrapper (`ip_manager.c:185-196`) + native server export (`native_ip_server.c` manager_property_get + vtable) + native round-trip test (`test_native_dbus.c` Test 1 + Test 3 via `ip_manager_get_supported_target_devices`) | — |
| DBUS-08 | 10.2 | verified | All other Manager/Composite/Target/Source methods+properties verified via mock + native tests (43 of 46 DBus API surface items) | — |
| DBUS-09 | 10.3 | verified | All 5 gaps have implemented workarounds: poll (gap 1), persist+restore (gap 2), temp file (gap 3), filesystem enum (gap 4), not needed (gap 5) | — |
| DBUS-10 | 10.1 | verified | Sender verification on all signal types: `ip_hotplug.c:50-57`, `ip_properties.c:63-68`, `ip_input_signal.c:128-131`; wrong-sender tests | — |
| DBUS-11 | 10.1 | verified | Rate limiting: `ip_input_signal.c:85-130` 200 events/s/device; `test_rate_limit_over_limit/per_device/reset` | — |
| ICON-01 | 8.1 | verified | `icon_map.c:273-282` DeviceType→icon lookup; `test_lookup_known/ds5`, `test_lookup_unknown` fallback | — |
| ICON-02 | 8.2 | verified | 36 SVGs incl custom arcade-stick, hitbox, steam-deck, generic-gamepad; `test_svg_all_compat` all parse+rasterize | — |
| ICON-03 | 8.3 | verified | `icon_cache.c:66-127` nanosvg→SDL_Texture at startup; `test_load_all`, `test_recolour`, `test_blend_mode` | — |
| ICON-04 | 8.4 | verified | `data/controller-icons.yaml` 23 types; `test_load_real_yaml`, `test_default_path`; unknown→generic+raw type | — |
| ICON-05 | 8.5 | verified | `icon_lookup.c:155-200` override resolution; `test_override_builtin/png_path/traversal` 37 tests | — |
| PKG-01 | 9.1 | verified | `packaging/org.shadowblip.ControllerBox.yaml` experimental marker, all permissions; `test_flatpak_manifest.py` 30+ checks | — |
| PKG-02 | 9.2 | verified | CMake install rules; `test_packaging.sh` + `test_packaging_install.sh` verify all layout paths | — |
| PKG-03 | 9.3 | verified | Binary, service, desktop, icons, yaml installed; `test_packaging_install.sh` verifies all paths + content | — |
| PKG-04 | 9.4 | verified | No `After=inputplumber.service`; `After=graphical-session.target`; `test_packaging_install.sh:check_contains graphical` | — |
| PKG-05 | 9.1 | verified | `service_install.c:392-470` first-run install; `test_service_install.c` full flow + mock systemctl | — |
| PERF-01 | 11 | verified | `test_overlay_latency.c:test_poll_interval_and_structure` 50+10=60≤75ms; `test_show_path_render_present_latency` p99<10ms | — |
| PERF-02 | 11 | verified | `test_show_path_render_present_latency` 200 iterations p99<10ms; `test_show_path_no_texture_allocation` | — |
| PERF-03 | 11 | verified | `test_close_path_timing` 200 iterations median<1ms; hidden not destroyed | — |
| PERF-04 | 11 | verified | `test_idle_step_no_busy_loop` p99<5ms (CPU); `test_daemon_footprint_bounded` RSS<50 MB after init and after 100 idle steps, growth<1 MB (no leak). PERF-04 fully verified. | — |
| PERF-05 | 11 | partial | ≤100ms max on Pi 4 not measured; structural bound <60ms on test machine. Requires target hardware. | Task 7 |
| PERF-06 | 11 | verified | `ip_manager_set_gamepad_order` delegates to InputPlumber (GUI doesn't manage suspend/resume); `test_native_assignment_application` | — |
| VRF-01 | 11.1.1 | verified | `fb_assert.c` `fb_read_pixels` via `SDL_RenderReadPixels`; `test_overlay_visual.c`, `test_manager_visual.c` production composition | — |
| VRF-02 | 11.1.2 | verified | `fb_region_has_content` assertions in overlay+manager visual tests; state changes alter regions; tolerance parameterized | — |
| VRF-03 | 11.1.3 | verified | `test_golden.c` 11 golden images (±3/channel, <2% diff); `CBX_GENERATE_GOLDEN` manual baselines; no auto-update | — |
| VRF-04 | 11.1.4 | verified | `test_golden.c:77-97` saves actual/expected/diff PNGs to `tests/golden-fail/` on mismatch | — |
| VRF-05 | 11.1.5 | partial | `test_installed_functional.c` runs (no skip) with private DBus + SDL virtual gamepad through production dispatch. However SPEC §11.1.5 requires "physical or kernel-backed synthetic controller." SDL virtual gamepads are process-local, not kernel-backed. `test_installed_binary.sh`/`test_installed_smoke.sh` use xdotool keyboard events (supplemental only). `installed-package` capability not declared. | Task 4 |
| VRF-06 | 11.1.6 | partial | `test_backend_smoke.c` skips (exit 77, no GPU). `test_backend_smoke_sw.c` passes (software). SPEC requires hardware backend. `gpu-compositor` capability not declared. | Task 6 |
| VRF-07 | 11.1.7 | partial | No human review artifact or signed acceptance record. `target-consumer` capability not declared. Screenshots captured by installed smoke tests are evidence artifacts but no human sign-off exists. | Task 8 |
| DOD-01 | 11.2.1 | partial | Conformance matrix exists (this plan) but 7 rows are `partial` pending runner capabilities and 2 code fixes. | Task 9 |
| DOD-02 | 11.2.2 | verified | `test_installed_functional.c` uses `cbx_manager_init`, `cbx_overlay_service_step`, production sd-bus; `test_golden.c` production render | — |
| DOD-03 | 11.2.3 | partial | Every M-control has controller+pointer evidence via SDL virtual gamepad and keyboard. SPEC requires "physical or kernel-backed" for controller path. | Task 3 |
| DOD-04 | 11.2.4 | verified | Golden images + visual tests cover normal, degraded, error, recovery states; `test_installed_functional.c` Phase 13 backend restart | — |
| DOD-05 | 11.2.5 | partial | Sanitizer gate (ASan+UBSan) verified: `scripts/verify-sanitizers.sh` builds with `-fsanitize=address,undefined` and runs full CTest suite — 98/98 pass, zero ASan/UBSan errors. Fixed production bug: `cbx_trigger_parse` stack-use-after-scope (`trigger.c`). Fixed production leak: `cbx_manager_shutdown` not freeing connection strings when DBus not owned. Fixed `cbx_icon_cache_init` leaking rasterizer on re-init. GPU smoke test (`test_backend_smoke`) still requires physical GPU (Task 6). | Task 6 |
| DOD-06 | 11.2.6 | verified | `.factory/bugs/open.md` contains empty JSON array `[]` — no open bugs | — |
| DOD-07 | 11.2.7 | partial | No independent review artifact exists in repository. | Task 9 |
| DOD-08 | 11.2.8 | partial | Docs exist (`README.md`, `docs/OPERATIONS.md`, `docs/PACKAGING.md`, etc.); `test_flatpak_manifest.py` checks README. Full documentation audit pending. | Task 9 |
| DOD-09 | 11.2.9 | partial | Git tree has scratchpad deletion pending; task ledger incomplete until all tasks complete. | Task 9 |

## Interaction acceptance inventory

The codebase maintains a machine-readable inventory in
`tests/interaction_inventory.c` (59 entries: M01–M38 manager, O01–O13 overlay,
D01–D08 disabled/degraded). `test_interaction_inventory.c` verifies all entries
have populated fields, unique IDs, correct categories, and verify-status
consistency.

**Manager controls (M01–M38):** Every visible enabled control has both
controller-path (focus chain → A activation through `cbx_manager_handle_event`)
and pointer-path (mouse motion + click through `cbx_manager_handle_mouse_event`)
evidence. Tests assert semantic outcomes (DBus calls, file mutations, mode
transitions, framebuffer diffs), not return values. Native DBus tests
(`test_manager_native.c`) use `SDL_JoystickAttachVirtual` + private dbus-daemon
+ forked InputPlumber-compatible server. Keyboard-dispatched tests
(`test_manager_interaction_ctrl.c`) are explicitly labeled supplemental.

**Overlay actions (O01–O13):** Open (trigger poll → lifecycle activate), move
left/right (DBus InputEvent + SDL key), cycle profile up/down, Host Mode
enter/exit/navigate/move-slot, conflict resolution on close, multi-controller
independence, close (save + set PASS). All through `cbx_overlay_service_step`
production dispatch.

**Disabled/degraded (D01–D08):** Degraded mode (no InputPlumber), DBus
operation failures, validation errors, topology mismatch — all reject both
activation paths with no side effects.

**Coverage gap:** SPEC §5.7 requires "physical or kernel-backed synthetic
gamepad" for controller acceptance. Current evidence uses SDL virtual gamepads
(process-local, not kernel-backed) and keyboard events (supplemental only).
`test_kernel_controller.c` would provide kernel-backed evidence via `/dev/uinput`
but skips (exit 77) because the capability is not declared. Task 3 addresses
this gap.

## Task 1: Add SupportedTargetDevices to native test server
- Status: complete
- Dependencies: none
- Scope: `tests/native_ip_server.c` (add `SupportedTargetDevices` property to
  manager vtable), `tests/test_native_dbus.c` (add native round-trip test)
- Acceptance criteria: `SupportedTargetDevices: as` property is exported by
  `native_ip_server.c` with human-readable names matching
  `SupportedTargetDeviceIds`. A native test reads and verifies the property
  value through `ip_dbus_sd_backend()`. DBUS-07 reclassified to `verified`.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'test_native_dbus' --output-on-failure"` — 10/10 tests passed (5.25s). Native `as` property read via both `backend->get_property` (Test 1) and `ip_manager_get_supported_target_devices` wrapper (Test 3). Values: "Xbox 360 Controller,DualSense,Generic Gamepad" matching `controller-icons.yaml` names for IDs "xb360,ds5,gamepad".
- Documentation impact: none
- Evidence: commit on `develop`; DBUS-07 reclassified to `verified` in conformance matrix.

## Task 2: Add daemon memory footprint test
- Status: complete
- Dependencies: none
- Scope: `tests/test_daemon_footprint.c` (new file, measure RSS of overlay
  service in idle state via `/proc/self/statm`)
- Acceptance criteria: Test measures the overlay service context's memory
  footprint (RSS via `/proc/self/statm`) after init and after 100 idle steps.
  Asserts footprint is bounded (<50 MB RSS for the daemon context). Test does
  not skip. PERF-04 reclassified to `verified`.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'footprint' --output-on-failure"`
  — 1/1 test passed (0.04s). RSS after init: 19.07 MB; after 100 idle steps:
  19.23 MB; growth: 164 KB (< 1 MB limit). All under 50 MB.
- Documentation impact: updated `docs/OPERATIONS.md` Performance expectations
  table with RSS bound and test reference.
- Evidence: commit on `develop`; PERF-04 reclassified to `verified`.

## Task 3: Declare physical-controller or kernel-uinput runner capability
- Status: blocked
- Dependencies: none
- Blocker: No accessible environment has `/dev/uinput` or a physical gamepad. The local machine has no `/dev/input/event*` devices. The declared runner (`dev-runner-vm`) is unreachable — `ssh dev-runner-vm` fails with "Could not resolve hostname" and the required `~/.ssh/factory-ssh` symlink is not provisioned. The environment declaration is exhaustive; capabilities cannot be invented without proof.
- Scope: `.factory/environment.toml` (add capability declaration to
  `dev-runner-vm` or new runner), `scripts/check-factory-runner-evidence.py`
  (validate evidence)
- Acceptance criteria: `.factory/environment.toml` declares
  `physical-controller` or `kernel-uinput` capability with exact-commit
  evidence accepted by `scripts/check-factory-runner-evidence.py`. The runner
  can access `/dev/uinput` or a physical gamepad. MGR-36, DOD-03, and the
  controller-acceptance portion of VRF-05 can be addressed by Tasks 4–5.
- Verification: `python3 scripts/check-factory-runner-evidence.py --print-capabilities`
  shows the new capability; `./scripts/run-factory-runners.py` passes
- Documentation impact: update `.factory/environment.toml` comments

## Task 4: Run kernel-backed controller acceptance
- Status: pending
- Dependencies: Task 3
- Scope: `tests/test_kernel_controller.c` (run to completion on runner with
  `/dev/uinput`), `tests/test_installed_functional.c` (optionally augment to
  use kernel-backed controller if available)
- Acceptance criteria: `test_kernel_controller.c` passes (exit 0, not 77) on
  the declared runner. Evidence demonstrates manager navigation, target
  creation, profile save/reload, overlay activation, and persistence
  verification through kernel-backed evdev input. MGR-36, DOD-03 reclassified
  to `verified`.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'test_kernel_controller' --output-on-failure"` on runner; exact-commit evidence recorded
- Documentation impact: document kernel-backed acceptance in `docs/OPERATIONS.md`

## Task 5: Run installed functional smoke with kernel-backed controller
- Status: pending
- Dependencies: Task 3
- Scope: `tests/test_installed_functional.c` (verify it runs with kernel-backed
  controller on declared runner), `tests/test_installed_binary.sh` (optionally
  add controller-event path alongside xdotool)
- Acceptance criteria: `test_installed_functional` passes on the declared
  runner with `installed-package` capability. The test exercises the full
  production workflow with a kernel-backed or physical controller navigating
  the installed manager binary. VRF-05 reclassified to `verified`.
- Verification: `ctest --test-dir build-check -R 'test_installed_functional' --output-on-failure` on runner with installed package
- Documentation impact: document installed acceptance procedure in `docs/OPERATIONS.md`

## Task 6: Declare gpu-compositor capability and run GPU backend smoke
- Status: blocked
- Dependencies: none
- Blocker: No GPU hardware accessible. Local machine has no `/dev/dri/` directory. The declared runner (`dev-runner-vm`) is unreachable (SSH hostname unresolvable, no `~/.ssh/factory-ssh` symlink). Cannot declare `gpu-compositor` capability or run `test_backend_smoke.c` with a hardware renderer.
- Scope: `.factory/environment.toml` (declare `gpu-compositor`),
  `tests/test_backend_smoke.c` (run to completion on GPU runner)
- Acceptance criteria: `test_backend_smoke.c` passes (exit 0, not 77) on the
  declared runner with an accelerated OpenGL/OpenGL ES backend. Framebuffer
  invariants (non-blank, region content, golden comparison) pass through the
  hardware renderer. VRF-06, DOD-05 reclassified to `verified`.
- Verification: `python3 scripts/check-factory-runner-evidence.py --print-capabilities` shows `gpu-compositor`; `ctest --test-dir build-check -R 'test_backend_smoke' --output-on-failure` on runner
- Documentation impact: document GPU backend verification in `docs/OPERATIONS.md`

## Task 7: Declare target-consumer capability and measure Pi-4 performance
- Status: blocked
- Dependencies: none
- Blocker: No target hardware accessible. Local machine is x86_64, not ARM64/Pi 4. The declared runner (`dev-runner-vm`) is unreachable (SSH hostname unresolvable, no `~/.ssh/factory-ssh` symlink). Cannot declare `target-consumer` capability or measure overlay appearance latency on Pi 4.
- Scope: `.factory/environment.toml` (declare `target-consumer`),
  performance measurement on Pi 4 or equivalent target hardware
- Acceptance criteria: Overlay appearance ≤100 ms maximum is measured on
  minimum supported hardware (Pi 4 ARM64). Measurement uses the production
  overlay service with real SDL rendering. Evidence recorded as a test result
  or signed artifact. PERF-05 reclassified to `verified`.
- Verification: performance measurement artifact with timing data on target hardware; `python3 scripts/check-factory-runner-evidence.py --print-capabilities` shows `target-consumer`
- Documentation impact: document Pi-4 performance results in `docs/OPERATIONS.md`

## Task 8: Perform human release acceptance on target hardware
- Status: pending
- Dependencies: Task 7
- Scope: Human review of representative manager and overlay captures on target
  hardware; signed acceptance record
- Acceptance criteria: A human reviews manager (all 3 tabs, editor states) and
  overlay (player mode, host mode, conflict, unassigned) captures for
  legibility, clipping, focus indication, contrast, and controller-only
  usability. Review outcome recorded as a verifiable artifact (signed file in
  `docs/` or `.factory/`). VRF-07, DOD-07 reclassified to `verified`.
- Verification: acceptance artifact exists and is referenced in the plan
- Documentation impact: `docs/OPERATIONS.md` documents the human release
  acceptance process and outcome

## Task 9: Final documentation and specification audit
- Status: pending
- Dependencies: Tasks 1, 2, 3, 4, 5, 6, 7, 8, 10
- Scope: Execute the canonical definition of done in `docs/SPEC.md` §11.2.
  Verify all conformance matrix rows are `verified`. Verify interaction
  inventory is exhaustive. Verify no contradictory open v1 bugs. Run independent
  adversarial reviews (correctness, test-quality, security, documentation).
  Run full clean verification. Verify documentation accuracy. Verify clean Git
  state on `develop`.
- Acceptance criteria:
  1. All conformance matrix rows classified `verified` with source evidence
     and executable test or acceptance command.
  2. Every enabled control in the §5.7 inventory has passing controller and
     pointer activation evidence through production dispatch.
  3. `.factory/bugs/open.md` contains no unresolved v1-impacting defects.
  4. Independent read-only reviews (correctness, test-quality, security,
     documentation) find no unresolved blocking issue.
  5. `nix-shell --run './scripts/verify-project.sh'` passes with no unexplained
     skips, flaky reruns, weakened assertions, or compiler warnings.
  6. README and `docs/OPERATIONS.md` match observed behavior; build, install,
     and acceptance commands work from a clean checkout.
  7. Git tree is clean on `develop` with complete task ledger.
  8. If any gap is found, append a uniquely numbered pending task, add it to
     this task's dependencies, and return this task to pending.
- Verification: `./scripts/final-gate.sh --planning` (planning) or
  `./scripts/final-gate.sh --implementation` (implementation);
  `nix-shell --run './scripts/verify-project.sh'`;
  `nix-shell --run "ctest --test-dir build-check --output-on-failure"`
- Documentation impact: final review of all documentation sections

## Task 10: Sanitizer build gate (ASan+UBSan) — remediation
- Status: complete
- Dependencies: (none)
- Scope: Add CMake option `CBX_ENABLE_SANITIZERS` to build with
  `-fsanitize=address,undefined`. Create `scripts/verify-sanitizers.sh`
  to build and run the full CTest suite under sanitizers. Fix all
  ASan/UBSan defects found. Addresses DOD-05 sanitizer gap.
- Acceptance criteria:
  1. `CBX_ENABLE_SANITIZERS` CMake option compiles and links with
     `-fsanitize=address,undefined -fno-omit-frame-pointer`.
  2. `scripts/verify-sanitizers.sh` exits 0 with all tests passing.
  3. No ASan (memory errors, use-after-scope, use-after-return) or
     UBSan (undefined behaviour) reports from production code.
  4. LSan suppressions cover third-party library leaks (harfbuzz,
     SDL2_ttf, SDL2) only — no production code leak suppressions.
- Verification: `nix-shell --run './scripts/verify-sanitizers.sh'`
  — 98/98 tests pass, 0 failures, 0 sanitizer errors.
- Evidence: Commit on `develop`. Production fixes:
  - `src/overlay/trigger.c`: `cbx_trigger_parse` stack-use-after-scope
    (moved `tmp[128]` to outer loop scope so `tok_start` doesn't dangle).
  - `src/manager/manager.c`: `cbx_manager_shutdown` freed
    `connection.unique_name` and `connection.version` even when DBus
    bus handle is externally owned.
  - `src/icons/icon_cache.c`: `cbx_icon_cache_init` cleans up existing
    rasterizer/textures before re-initialising (prevents leak on
    repeated init calls).
- Documentation impact: `scripts/verify-sanitizers.sh` and
  `scripts/lsan-suppressions.txt` added to project verification suite.