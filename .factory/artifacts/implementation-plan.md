---
spec_path: docs/SPEC.md
spec_commit: HEAD
base_commit: HEAD
status: active
---

## Task 1: Enforce host-mode freeze in production dispatch
Title: Enforce host-mode freeze in production dispatch
Status: completed
Dependencies: none
Acceptance: Both DBus and SDL service dispatch pass the actual sending row to cbx_host_mode_handle; frozen controllers cannot move the selected row or exit Host Mode.
Verification: ./scripts/verify.sh; ctest --test-dir build -R 'test_host_mode|test_overlay_interaction|test_overlay_native' --output-on-failure
Runner: none
Evidence: Retained prior-plan evidence: both dispatch paths fixed; test_o11e_non_host_nav_keeps_host_selection and test_o11f_non_host_r3_keeps_host_mode failed under the old dispatch and passed after repair. Nix build passed; targeted 3/3 passed; historical full gate reported 91/91 with runner-gated test_kernel_controller and test_backend_smoke skips. This is not fresh hardware acceptance.

## Task 2: Make virtual-icon framebuffer assertions non-vacuous
Title: Make virtual-icon framebuffer assertions non-vacuous
Status: completed
Dependencies: none
Acceptance: Icon-region assertions distinguish icon pixels from plain cell fill and fail when textures are absent. Golden changes remain explicit reviewed changes.
Verification: ctest --test-dir build -R 'test_overlay_visual|test_golden' --output-on-failure; ./scripts/verify.sh
Runner: none
Evidence: Retained prior-plan evidence: assertions compare against player_cell_fill_color, not theme background. Pointing the icon cache at /nonexistent-icons made test_virtual_device_icons and test_player_mode_grid fail through production rendering; restoring the real asset directory passed. No golden update or production rendering change. Historical targeted 2/2 and full gate passed, with kernel/GPU skips explicitly outside acceptance.

## Task 3: Make profile modal confirmation pointer-reachable
Title: Make profile modal confirmation pointer-reachable
Status: completed
Dependencies: none
Acceptance: Name and deletion dialogs expose clickable confirm/cancel through production manager mouse dispatch, with real inventory entries and pointer semantic assertions.
Verification: ./scripts/verify.sh; ctest --test-dir build -R 'test_manager_interaction_prof|test_profiles_tab|test_interaction_inventory|test_manager_visual' --output-on-failure
Runner: none
Evidence: Retained prior-plan record: verification exit 0 on local. This limited historical receipt does not establish full controller traversal; task 8 rechecks the complete inventory.

## Task 4: Finish Host Mode editing and visible transition rendering
Title: Finish Host Mode editing and visible transition rendering
Status: completed
Dependencies: 1, 14
Acceptance: SPEC §§4.4, 4.10: the exclusive host can select any row and edit both slot and profile through production events, with a documented controller affordance. Entry, selected-row change and exit visibly update host/frozen highlights without enabling non-host mutation. Existing host callbacks and dirty-trigger changes are reused, not blindly rewritten. Reconcile state-driven invalidation with §4.9 and prove substantive region differences, text/icons and frozen-row semantics through the production composition path. Evaluate the prior audit blockers against current source and close actual defects with independent review, rather than treating historical issue labels as proof.
Verification: ./scripts/verify.sh; ctest --test-dir build -R 'test_host_mode|test_overlay_interaction|test_overlay_native|test_overlay_visual|test_golden' --output-on-failure
Runner: none
Evidence: verification exit 0 on local

## Task 5: Make property signals update the correct displayed device
Title: Make property signals update the correct displayed device
Status: completed
Dependencies: 14
Acceptance: SPEC §10.1: propagate validated object path and interface through ip_properties_changed_payload and dispatch. ProfileName/ProfilePath, routing/source changes and Manager GamepadOrder affect the actual per-device model and rendered UI in Manager and overlay, not merely a global cache or dirty flag. Handle invalidation by bounded authoritative reads; reject wrong interface/path/sender. Two-composite native-bus tests prove one device's change does not overwrite another, and subscription replacement/recovery does not retain stale callbacks. Existing subscription work is retained where correct.
Verification: ./scripts/verify.sh; ctest --test-dir build -R 'test_properties_changed|test_overlay_native|test_manager_native|test_native_dbus' --output-on-failure
Runner: none
Evidence: repair 1: cbx_controllers_tab_refresh_labels now preserves the selected target by exact path across the rebuild (src/manager/controllers_tab.c), cbx_manager_on_prop_change gates the label rebuild to ProfileName/ProfilePath/TargetDevices (src/manager/manager.c), and ensure_scroll_visible skips an unmeasured list (src/ui/widget_list.c). New regression test_manager_property_change_preserves_selection in tests/test_manager_native.c selects row 2 via the production pointer path, injects a native ProfileName PropertiesChanged, asserts selection stays 2, and proves the production Remove callback removes the third controller not row 0; verified to fail before the fix (exit 8) and pass after. ./scripts/verify.sh exit 0 (91/91, 2 legitimate exit-77 skips); ctest --test-dir build -R 'test_properties_changed|test_overlay_native|test_manager_native|test_native_dbus' --output-on-failure 5/5 pass

## Task 6: Integrate physical identity and durable order restoration
Title: Integrate physical identity and durable order restoration
Status: pending
Dependencies: 7, 14, 21
Acceptance: SPEC §§6, 7.4, 10.3: production startup, hotplug and owner reacquisition collect the proper source-interface properties, choose BT MAC then valid serial then physical port then connection order, and restore controller profile/preferred slot without assuming opaque PersistentId already implements that contract. Reuse identity/assignment/order utilities where correct. Resolve saved IDs to current paths after confirmed enumeration and restore GamepadOrder. Distinguish confirmed absence from query failure: transient reads must not erase saved order or apply misleading empty order. Test two identical devices, reversed reconnect order, stable identities, weak identities, invalid preferred properties with valid alternatives, disconnected preferences and topology shrink. Do not infer a downgrade from an unrelated stronger stored identity. Apply fallback safely and report uncertainty instead of mismatching controllers.
Verification: ./scripts/verify.sh; ctest --test-dir build -R 'test_identity|test_assign|test_order_restore|test_gamepad_order|test_overlay_native|test_manager_native' --output-on-failure
Runner: none
Evidence: none

## Task 7: Make readiness and signal trust fail closed
Title: Make readiness and signal trust fail closed
Status: completed
Dependencies: 14
Acceptance: SPEC §§2.4, 10.1: connected/ready requires a verified current owner, supported Version, complete enumeration and native typed-property validation. Clear owner/credential state on loss/disconnect and do not publish transport sender trust before validation. Startup/recovery failures in required subscriptions, trigger registration, mapping, assignment restoration or type probes leave operations disabled with actionable diagnostics and bounded retry. Reacquisition succeeds within two seconds when the service is healthy. Preserve sender authentication in all callers, reject former-owner events, propagate DBus processing errors and bound draining so UI work is not starved. Native tests cover lookup/credential/signature/subscription failures, absent service, owner replacement and partial initialization cleanup in both application modes.
Verification: ./scripts/verify.sh; ctest --test-dir build -R 'test_connection|test_input_signal|test_dbus_signatures|test_native_dbus|test_overlay_native|test_manager_native' --output-on-failure; ./scripts/verify-sanitizers.sh
Runner: none
Evidence: Implemented fail-closed readiness and sender trust in the production paths. `src/dbus/ip_connection.c`: connect now requires a credential-verified current owner AND a compatible Version before CONNECTED; an unresolvable/unverifiable owner returns the new `IP_ERR_UNVERIFIED` and enters DEGRADED with no advertised unique name; `ip_connection_disconnect`, NameOwnerChanged loss, and any failed revalidation clear `sender_verified`/`expected_pid`/`expected_uid`/`unique_name`; reconnect drops stale owner state first. `src/dbus/dbus_client.c`: the transport trust store (`expected_sender`/`expected_pid`) is now written only by `sd_get_connection_creds` after `GetConnectionCredentials` succeeds and the UID policy passes (`sd_trust_owner_creds`); `sd_get_unique_name` no longer publishes trust, and NameOwnerChanged loss clears trust so a former owner's late signals are rejected by every `sd_sender_ok` caller (InputEvent, PropertiesChanged, InterfacesAdded/Removed). `src/app/overlay_service.c`: factored `overlay_recover()` treating owner verification, complete enumeration, target reconcile, `cbx_overlay_on_save` assignment restoration, InputEvent/hotplug/PropertiesChanged subscriptions, trigger registration and intercept-poll arming as required steps; any failure records an actionable `readiness_detail`, keeps `backend_ready=false`, force-closes, and schedules a bounded retry (`overlay_recovery_tick`, `CBX_RECOVERY_MAX_ATTEMPTS=8`, 2 s monotonic deadline) so a transient startup/recovery failure becomes operational within the two-second window; `run_overlay_service()` no longer exits on reconcile/assignment/subscription/trigger/poll startup failures (it stays degraded and retries). `src/manager/manager.c`: `cbx_manager_try_ready()` gates readiness on enumeration/type query plus the PropertiesChanged subscription, `cbx_manager_backend_ready()` schedules bounded recovery on failure, `cbx_manager_recovery_tick()` retries within 2 s, and `cbx_manager_props_wire()` refuses to wire an owned connection that is not credential-verified. Bounded DBus draining and error propagation: `ip_input_events_process()` caps at `IP_INPUT_DRAIN_MAX` (64) and returns the negative `process()` errno; the overlay per-step drain, manager run loop, and controllers-tab wait loops now propagate processing errors into degraded state instead of swallowing them. Native/mock tests added: `test_connection` (fail-closed unique-name and credential-lookup failures return `IP_ERR_UNVERIFIED`+DEGRADED with no trusted sender, disconnect and version-failure clear credential state, `IP_ERR_UNVERIFIED` reason mapping); `test_input_signal` (bounded drain returns exactly 64, negative process errno propagated, NULL backend no-op); `test_native_dbus` (`test_native_absent_service_then_appears`: absent service is degraded, later acquisition connects ≤2 s; input-signal test now establishes trust through the validated `get_connection_creds` path); `test_overlay_native` (`test_readiness_fail_closed_and_recovery`: production recovery callbacks, owner loss clears sender verification/unique name and records a diagnostic, restart with an injected transient `CreateTargetDevice` failure recovers to `backend_ready` with `input_events_ready` and `poll_count==comp_count` within 2 s); `test_manager_native` (`test_manager_readiness_fail_closed_and_recovery`: owner loss disables `ct.backend`/`ct.bus` with a diagnostic, restart re-readies `dbus_connected` and re-wires `expected_sender` within 2 s). Verification receipts: `nix-shell --run './scripts/verify.sh'` exit 0 — clean build, 91/91 CTest passed, 0 failed, 2 hardware-gated exit-77 skips (test_kernel_controller, test_backend_smoke), packaging/installed checks passed; `nix-shell --run './scripts/verify-sanitizers.sh'` exit 0 — 91/91 under ASan+UBSan, no defects. This is local/native evidence, not hardware or human release acceptance.

## Task 8: Complete the dual-input interaction inventory
Title: Complete the dual-input interaction inventory
Status: pending
Dependencies: 3, 9, 15, 16, 18, 22
Acceptance: SPEC §§5.1, 5.3, 5.7: enumerate every enabled control and prove focus/hover appearance plus the same semantic result through actual production controller and pointer dispatch. Include controller-only profile naming (a reachable character-entry or equivalent usable naming flow), every creation starting point, Default read-only rejection, all dialogs, list/sequential editing, save/discard, resize-derived hit bounds and unavailable/error/recovery controls. Save failure during a quit prompt must leave the editor open with data intact. No operation may secretly require keyboard input. Label synthetic key events supplemental; SDL virtual-controller events prove dispatch but not kernel/physical acceptance. Exercise press and release, not direct callbacks. Ensure mock requests are consumed and match production operations, with negative tests for disabled controls and absent outcomes.
Verification: ./scripts/verify.sh; ctest --test-dir build -R 'test_manager_interaction|test_manager_native|test_interaction_inventory|test_profiles_tab|test_manager_visual|test_installed_functional' --output-on-failure
Runner: none
Evidence: none

## Task 9: Expose all startup slots and synchronize Settings state
Title: Expose all startup slots and synchronize Settings state
Status: pending
Dependencies: 14, 21
Acceptance: SPEC §5.5: every configured slot up to CBX_MAX_CONTROLLERS has a reachable type selector, current value and controller/pointer activation, without clipping at large counts. Synchronize Settings and Controllers state so saving one tab cannot overwrite a newer topology from the other. Test count growth/shrink, slot 5 and slot 16, persistence/restart, all remaining settings and icon overrides. Display theme choices honestly within the implemented §13 theme policy; no enabled inert setting. Backend topology is confirmed by task 18, not assumed from settings storage.
Verification: ./scripts/verify.sh; ctest --test-dir build -R 'test_settings_tab|test_controllers_tab|test_manager_interaction|test_manager_visual|test_golden' --output-on-failure
Runner: none
Evidence: none

## Task 10: Repair interception polling ownership and event hygiene
Title: Repair interception polling ownership and event hygiene
Status: pending
Dependencies: 14
Acceptance: SPEC §§2.5, 4.9, 10.3: each approximately 50 ms timer event ticks only its live originating poll; sparse timer-start success cannot leave active polls outside cleanup/dispatch bounds. Stop/remove timers on errors, shutdown and rearm; SDL_AddTimer failure leaves IDLE. Reject stale queued events after rebuild with stable ownership/generation checks. A legitimate overlay session longer than ten seconds must not be treated as a failed close merely because ACTIVE elapsed. Assert linear per-device read counts, bounded idle traffic and no lifetime race or leaked timers. Preserve ObjectManager-only device discovery. Audit existing mock expectations rather than dropping unconsumed expectations without replacing exact real-operation assertions.
Verification: ./scripts/verify.sh; ctest --test-dir build -R 'test_intercept_poll|test_overlay_service|test_overlay_reconcile|test_overlay_latency|test_daemon_footprint|test_manager_calls' --output-on-failure; ./scripts/verify-sanitizers.sh
Runner: none
Evidence: none

## Task 11: Verify kernel-backed controller transport on the declared VM
Title: Verify kernel-backed controller transport on the declared VM
Status: pending
Dependencies: 8, 23
Acceptance: On dev-runner-vm, test_kernel_controller actually uses /dev/uinput with InputPlumber-compatible native backend behavior and traverses production Manager dispatch. Record producer/device identity, runtime backend and semantic outcomes. A skip, SDL_JoystickAttachVirtual fallback or keyboard-only event stream fails this task. This proves kernel transport only, not real-system routing or target-consumer acceptance, which belong to task 24.
Verification: ./scripts/verify.sh; ctest --test-dir build -R '^test_kernel_controller$' --output-on-failure
Runner: kernel-uinput
Evidence: none

## Task 12: Verify accelerated rendering and installed licensed diagrams
Title: Verify accelerated rendering and installed licensed diagrams
Status: pending
Dependencies: 4, 8, 20, 22, 23
Acceptance: On gpurunner, the accelerated test_backend_smoke really uses OpenGL/OpenGL ES and produces compositor/framebuffer evidence with region-level invariants. Installed diagrams resolve packaged licensed assets without source-tree fallbacks. Capture Manager and overlay states with renderer metadata; absence of GPU or a required installed-diagram prerequisite blocks rather than substitutes software evidence. This does not assert Pi 4 timing or human visual approval.
Verification: ./scripts/verify.sh; ctest --test-dir build -R '^(test_backend_smoke|test_installed_diagram)$' --output-on-failure
Runner: gpu-compositor
Evidence: none

## Task 13: Complete the normative conformance matrix
Title: Complete the normative conformance matrix
Status: pending
Dependencies: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27
Acceptance: Produce docs/CONFORMANCE.md with a stable row for every normative SPEC requirement, exact source/test/command/artifact references, evidence environment and owning task. Map §§2–10 as well as §11, including installed packaging, identity, icons, degraded states and all interaction inventory entries. Mechanically check coverage and references; a grep for the word verified is insufficient. Only actual passing evidence permits verified. Any unresolved or unavailable requirement remains explicitly blocked/partial and prevents this task and final audit from completing. Recheck BUG-0016/0017 and historical completed-task evidence against current production paths; no helper-only proof or prose completion claim.
Verification: ./scripts/verify.sh; ./scripts/verify-sanitizers.sh; independent read-only comparison of docs/CONFORMANCE.md against every normative clause in docs/SPEC.md and the actual test receipts
Runner: none
Evidence: none

## Task 14: Establish a clean reproducible verification baseline
Title: Establish a clean reproducible verification baseline
Status: completed
Dependencies: none
Acceptance: Run existing scripts/verify.sh in this checkout so its stale-cache detection regenerates canonical build registrations. Record the real first clean build and CTest results, packaging/installed checks and every capability skip. Determine the cause of each failure, fixing safe infrastructure faults within this task or recording the precise blocking defect against its owning task. Do not infer application failures from the bugs study's unusable stale build, nor call any fresh failure pre-existing without investigation. No placeholder pass is allowed; retained historical evidence is separate from this receipt.
Verification: ./scripts/verify.sh
Runner: none
Evidence: Fresh baseline on develop @ 8f1df4a1 (host Linux 7.1.6-cachyos x86_64, GCC 15.3.0, cmake/ctest 4.3.4, Nix 2.34.8). `./scripts/verify.sh` exited 0. Its stale-cache branch fired ("canonical build cache pinned to a stale source root; rebuilding tree"): the pre-existing build/CMakeCache.txt pinned CMAKE_HOME_DIRECTORY=/home/lalobied/repos/controller-box, so the tree was removed and fully reconfigured/rebuilt against /workspace/project (cache now CMAKE_HOME_DIRECTORY=/workspace/project). Clean build: 0 compiler warnings, 0 errors. CTest registered and ran 91 tests: "100% tests passed, 0 tests failed out of 91", total 170.09 s. Packaging/installed checks all passed: test_flatpak_manifest, test_packaging (74.78 s, default-prefix DESTDIR staging + layout + --version/--dry-run + clean configure), test_service_install, test_installed_smoke (17.00 s, Xvfb/X11 real main() path), test_installed_binary (26.00 s), test_installed_diagram (12.74 s), test_installed_functional (9.26 s). Flatpak execution is the documented CBX_REQUIRE_FLATPAK=0 deferral to the installed-package runner contract, not a ctest skip. Exactly two CTest skips, both exit 77 hardware gates and not failures: test_kernel_controller — "/dev/uinput is not available (No such file or directory)" (kernel-uinput capability; owning task 11 on dev-runner-vm); test_backend_smoke — "SDL_Init failed: No available video device" with no /dev/dri (gpu-compositor capability; owning task 12 on gpurunner), with test_backend_smoke_sw passing as the software-renderer fallback. No tracked source hardcodes the stale root, and the bugs-study 86-unrunnable/5-script-failure result does not recur; the stale-cache path required no fix. Full log (gitignored): .factory/artifacts/logs/task14-verify.log, sha256 f9b8b58838f95515bf57ca2ac57d09018c810c3ef709db6e0b1ebf558d1b0a64. A second `./scripts/verify.sh` run on the now-canonical cache (no stale-cache branch) again exited 0 with 91/91 passed, 0 failed and the same two skips in 168.12 s, confirming reproducibility (log .factory/artifacts/logs/task14-verify-rerun.log, sha256 36de6145c00cd8ebe8f5def6e62ff8d3067474c99bef2b8ebb8f38c327d1ab7c). Repaired a test-infrastructure fault found while re-running the sanitizer gate: tests/test_profile_diagram.c's build_cache_resolved_diagram passed a stack-local cbx_theme to cbx_profile_diagram_init, which borrows the theme for the diagram's lifetime, so diag_draw read the dead stack object; the helper now borrows the fixture-owned theme. scripts/verify.sh's stale-cache probe now uses fixed-string whole-line matching (grep -qxF) so a checkout path containing regex metacharacters cannot misclassify the cache. A/B proof on the existing build-sanitizer target: HEAD's test version aborted with AddressSanitizer stack-use-after-return at src/manager/profile_diagram.c:718 in diag_draw, and the fixed version passed 1/1. Repair re-verification: `./scripts/verify.sh` exited 0 with no stale-cache removal (canonical cache), a clean build with 0 warnings/0 errors, and 91/91 passed, 0 failed, the same two exit-77 skips, in 168.84 s (log .factory/artifacts/logs/task14-verify-repair.log, sha256 0c057e348afa2794f0064552c7869cca59a3ddeae4cc84271faf600642612ac4). Repair cycle 2: the stale-cache probe no longer compares raw path strings (grep -qxF against pwd -P), which misclassified a symlinked checkout's logical CMAKE_HOME_DIRECTORY as stale and deleted/rebuilt the canonical tree on every symlinked run. It now extracts CMAKE_HOME_DIRECTORY and resolves it physically (cd -- "$cache_home" && pwd -P), rebuilding only when the root is empty, unresolvable, or physically different from PROJECT_ROOT; a checkout reached through any symlink to the same physical root is retained. A/B proof against the exact probe block from scripts/verify.sh on scratch roots: the old probe removed a root whose cache recorded a symlink to it, while the new probe retained physical-root, symlink-A, and a different symlink-B-to-same-root caches and still removed missing, empty, and relative-unresolvable caches (stderr "canonical build cache pinned to a stale source root; rebuilding tree"). A scratch CMake project confirmed CMake accepts a symlinked CMAKE_HOME_DIRECTORY when reconfigured from the physical path (exit 0, no source/binary mismatch), so skipping removal cannot introduce an abort. Re-verification: `./scripts/verify.sh` exited 0, no stale-cache removal on the canonical cache (CMAKE_HOME_DIRECTORY=/workspace/project), clean build with 0 warnings/0 errors, 91/91 passed, 0 failed, total 169.11 s, the same two exit-77 skips (test_kernel_controller: "SKIP: /dev/uinput is not available (No such file or directory)"; test_backend_smoke: "SDL_Init failed: No available video device") with test_backend_smoke_sw passing; packaging/installed all passed (test_packaging 73.19 s, test_installed_smoke 17.01 s, test_installed_binary 26.01 s, test_installed_diagram 12.90 s, test_installed_functional 9.33 s, test_service_install, test_flatpak_manifest). Log .factory/artifacts/logs/task14-verify-repair2.log, sha256 c4dde30a5c75a500bd76f7dec4e888bcb70e3cf4d31d388c84d740de3b751ba7.

## Task 15: Show and edit every supported profile binding
Title: Show and edit every supported profile binding
Status: pending
Dependencies: 14
Acceptance: Resolve BUG-0016 in profile_editor_list.c: enumerate the supported virtual-button catalog, including unbound entries, rather than only profile.mapping_count or an expanded Default YAML. Selection synchronizes the always-visible diagram. Activating an unbound row creates the intended mapping without duplicating an existing mapping; empty profiles retain a reachable add/sequential action. Default remains immutable, and cloning/new profiles remain editable. Test all seventeen currently supported buttons, zero mappings, scrolling, diagram regions and both normal production activation paths.
Verification: ./scripts/verify.sh; ctest --test-dir build -R 'test_editor_list_mode|test_profile_diagram|test_profiles_tab|test_manager_native|test_manager_visual' --output-on-failure
Runner: none
Evidence: none

## Task 16: Make native physical capture produce valid portable mappings
Title: Make native physical capture produce valid portable mappings
Status: pending
Dependencies: 7, 15, 17
Acceptance: Resolve BUG-0017: profiles_tab passes an explicitly selected valid composite into the editor; resolve and authenticate that composite's DBus target, load its virtual capabilities and reject other devices' events. List and sequential capture acquire interception, check subscription errors and restore the prior interception/subscription ownership on completion, cancel, editor close, disconnect, shutdown and failed initialization. Backend replacement updates an open editor safely. Sequential capture records the pressed source and prompted virtual target, so a clean empty profile can acquire the six NES bindings and pass native load/save/restart. Preserve B-skip/Start-cancel affordances without confusing navigation streams with captured binding input or making required B impossible to bind. Native service tests observe exact interception and input dispatch; direct capture callbacks are only supplemental.
Verification: ./scripts/verify.sh; ctest --test-dir build -R 'test_editor_list_mode|test_editor_seq_mode|test_manager_native|test_profile_save|test_installed_functional' --output-on-failure; ./scripts/verify-sanitizers.sh
Runner: none
Evidence: none

## Task 17: Prevent destructive or unsafe profile YAML round trips
Title: Prevent destructive or unsafe profile YAML round trips
Status: pending
Dependencies: 14
Acceptance: SPEC §§5.4, 7.1, 7.6, 13: simple gamepad/keyboard/mouse/touch mappings serialize into valid native DeviceProfile data and preserve meaning. Existing advanced mappings remain loadable; cloning/editing must either preserve unsupported structures losslessly or explicitly reject destructive editing before any write, leaving originals intact. Validate nested source-property/target-event counts before every serializer access, including serialize(), and consistently enforce depth, aliases, shape, document boundaries and field capacities in skipped complex content. Parse failures never publish partial profiles. Test malformed in-memory counts under sanitizers and semantic source/target round trips with native backend load, including scalar sources and complex targets.
Verification: ./scripts/verify.sh; ctest --test-dir build -R 'test_profile_yaml|test_profile_save|test_profiles_tab|test_manager_native|test_native_dbus' --output-on-failure; ./scripts/verify-sanitizers.sh
Runner: none
Evidence: none

## Task 18: Preserve confirmed routable topology through late failures
Title: Preserve confirmed routable topology through late failures
Status: pending
Dependencies: 7, 9, 14
Acceptance: SPEC §5.2: stage Manager and startup topology until exact target publication, type and attachment readback succeed. Cover late failures after replacement attachment, old-target stop, persistence and refresh type reads; never publish partially queried topology or stop a replacement while leaving the composite routed to it. Roll back routing and desired state where possible, verify compensation, and surface the exact primary/cleanup operation when the backend cannot restore state. Do not advertise success or fictitious preserved live targets after irreversible failure: disable assignment until re-enumeration confirms recovery. Add/remove/type-change preserve other slot identities and route removed assignments to Unassigned. Native tests verify exact paths, delayed publication, same-count replacement, persistence failure and cleanup failure; games seeing a routable target is separately verified by task 24.
Verification: ./scripts/verify.sh; ctest --test-dir build -R 'test_controllers_tab|test_manager_native|test_native_dbus|test_overlay_service|test_overlay_native' --output-on-failure
Runner: none
Evidence: none

## Task 19: Preserve active edits and second-arrival conflict ownership
Title: Preserve active edits and second-arrival conflict ownership
Status: pending
Dependencies: 4, 5, 6, 18
Acceptance: SPEC §§4.3–4.7: conflict red marking follows second arrival, not row index. Test a lower-index row entering a higher-index incumbent's slot and vice versa, including Host edits and departure/re-entry. On close resolve to the lowest available P slot; when none exists, prevent duplicate routing and visibly retain an actionable safe state rather than claiming successful resolution. Hotplug/rebuild preserves surviving rows' unsaved slot/profile choices and host identity by stable identity/path, updates same-count type changes and handles zero targets without constructing fictitious columns. Host disconnect exits/remaps safely; active-session reconciliation must not reset all surviving controllers to PASS and discard edits.
Verification: ./scripts/verify.sh; ctest --test-dir build -R 'test_conflict|test_dynamic_columns|test_host_mode|test_overlay_reconcile|test_overlay_interaction|test_overlay_native|test_overlay_visual' --output-on-failure
Runner: none
Evidence: none

## Task 20: Make overlay activation, save and close truthful
Title: Make overlay activation, save and close truthful
Status: pending
Dependencies: 7, 10, 18, 19, 21
Acceptance: Production DBus input cannot mutate slots/profiles or enter Host Mode while hidden/idle or backend-unready; preserve legitimate activation dispatch. Propagate navigation/profile callback errors and restore the displayed confirmed selection on failure. Stage assignment/profile changes until verified routing/order/save succeeds; avoid clearing all routes then silently closing after a later error. Wire observable lifecycle errors and bounded PASS recovery so failed input release is never reported as successful hidden/IDLE gameplay restoration. Restore temporary callback/context fields in cbx_overlay_request_close even on rejected close. Present intermediate fade frames without rebuilding the grid and honor configured opacity; dirty regions clip a stable full layout rather than relaying out inside each region. Test repeated activation, long sessions, failed save/PASS, error visibility, configured alpha and full-versus-partial redraw equivalence through the real service path. Task 25 measures the actual latency requirements.
Verification: ./scripts/verify.sh; ctest --test-dir build -R 'test_close|test_overlay_lifecycle|test_overlay_interaction|test_overlay_native|test_overlay_visual|test_surface_build|test_grid_render|test_animation' --output-on-failure; ./scripts/verify-sanitizers.sh
Runner: none
Evidence: none

## Task 21: Make shared settings and assignment persistence reliable
Title: Make shared settings and assignment persistence reliable
Status: pending
Dependencies: 14
Acceptance: SPEC §§6–7 and process-restart requirements: load into temporary structures and publish only complete validated values, keeping safe defaults or prior confirmed state on malformed files. Reject nonfinite opacity, numeric overflow, invalid IDs/counts and malformed multi-document shapes; incomplete icon-override items must not combine across entries. File reads use bounded regular-file handling rather than stat/open races or blocking FIFO reads. Propagate write/flush/fsync/rename failures and preserve originals and owned-temp cleanup. Serialize read-modify-write transactions shared by Manager/overlay assignment/order writers so independent updates cannot erase each other; test concurrent updates and failed persistence without global /tmp assumptions. Reuse the existing config APIs and keep user paths/permissions compatible.
Verification: ./scripts/verify.sh; ctest --test-dir build -R 'test_settings|test_assignments|test_assign_persist|test_gamepad_order|test_profile_list|test_config_paths|test_manager_native' --output-on-failure; ./scripts/verify-sanitizers.sh
Runner: none
Evidence: none

## Task 22: Repair UI and asset lifetime defects on required paths
Title: Repair UI and asset lifetime defects on required paths
Status: pending
Dependencies: 14
Acceptance: SPEC §§5.1, 5.6, 8, 11.2: reproduce and repair short/empty-list wheel scrolling that produces negative draw indices, wrapped-text allocation-failure double frees, and grid activation that drops release events. Establish safe cache ownership for long/full-cache text and icon results; eliminate stale button textures after cache clearing and same-pointer texture destruction. Enforce safe icon initialization, reject or support oversized keys, and permit legitimate full-cache replacement. Keep icon-map section structure correct and canonical allowed custom-image paths bounded in resource use. Tests cover list wheel-and-draw, allocation failure after several wrapped lines, controller/pointer press-release, cache capacity/replacement and packaged fallback/override icons. Verify actual renderer capability metadata after software fallback. Scope changes to these concrete required-path correctness/safety defects, not a toolkit rewrite; no unreadable or blank required text/icon regions.
Verification: ./scripts/verify.sh; ctest --test-dir build -R 'test_widget|test_text|test_focus|test_icon|test_profile_diagram|test_renderer_init|test_manager_visual|test_overlay_visual|test_golden' --output-on-failure; ./scripts/verify-sanitizers.sh
Runner: none
Evidence: none

## Task 23: Close installed packaging and service integration gaps
Title: Close installed packaging and service integration gaps
Status: pending
Dependencies: 7, 9, 16, 17, 18, 20, 22
Acceptance: SPEC §§2, 9, 11.1: on dev-runner-vm install the default-prefix tarball in a clean runtime-only environment and run installed production initialization, body-control coordinate clicks, controller navigation, profile create/save/reload, backend/process restart and mapped overlay checks with independent filesystem/DBus outcomes. Verify explicit consent and actual systemd-user enable/start, graphical-session ordering and bounded restart, without cross-manager InputPlumber dependencies. Build/install Flatpak experimentally and verify host profile visibility and authorized native system-service access where available; missing Flatpak prerequisites block that evidence rather than silently skip. Review src/inputplumber-mediator.c and packaging against §2.2's direct sd-bus/no-proxy requirement: remove runtime reliance on a separate proxy/mediator from required product flows without broadening authorization, or keep this task blocked for an explicit human contract decision. Preserve separate InputPlumber installation and honest experimental/no-unpublished-Flathub wording. Verify packaged icon/diagram provenance and licenses. Custom-prefix checks use a separate build and do not weaken default-prefix assertions. Architecture/platform evidence belongs to task 25.
Verification: ./scripts/verify.sh; ctest --test-dir build -R 'test_packaging|test_flatpak_manifest|test_installed|test_service' --output-on-failure
Runner: installed-package
Evidence: none

## Task 24: Prove real InputPlumber routing to a target consumer
Title: Prove real InputPlumber routing to a target consumer
Status: pending
Dependencies: 6, 8, 16, 18, 19, 20, 23
Acceptance: On iprunner use the actual InputPlumber system service and physical controllers, with an independent game/target-consumer observation. Verify add/remove/mixed type replacement changes what the consumer sees, profile capture and native loading work, two controllers route independently, Host freeze/conflicts/Unassigned behave correctly, Select+A opens the mapped overlay and B restores gameplay, and reconnect/backend restart restore assignments/order. Identify service version, controllers, consumer and installed binary in the receipt. Private-bus fixtures, kernel producers alone and emitted mock InputEvent messages cannot satisfy this task. If current tests do not observe the consumer, add a production acceptance scenario with exact runnable commands and artifacts before claiming completion; do not pretend an existing test name proves that coverage.
Verification: ./scripts/verify.sh; ctest --test-dir build -R 'test_kernel_controller|test_installed_functional|test_overlay_native' --output-on-failure; execute the task's documented real-system physical-controller/target-consumer scenario and record its exact command and exit code
Runner: controller-production-routing
Evidence: none

## Task 25: Establish target-platform build and latency evidence
Title: Establish target-platform build and latency evidence
Status: blocked
Dependencies: 10, 12, 20, 23, 24
Acceptance: Resolve F2 with an approved declared environment, without inventing runner capabilities. Produce installed x86_64 and aarch64 tarballs and X11, Wayland and Gamescope production evidence. On minimum supported Pi 4 hardware measure button-to-first-compositor-visible frame separately from ALL-detection-to-visible frame and close-to-consumer-input; require ≤75 ms p99/≤100 ms maximum, <10 ms p99 and <1 ms respectively. Record sample counts, clocks/measurement boundaries, renderer/compositor, InputPlumber version and raw timing artifacts. Measure idle CPU/memory and report methodology/results without inventing a numeric footprint threshold. Unit timing, SDL dummy/offscreen rendering and GPU availability alone do not establish these claims. Investigate actual timing failures rather than redefining the measurement boundary.
Verification: ./scripts/verify.sh; ctest --test-dir build -R 'test_overlay_latency|test_daemon_footprint|test_backend_smoke|test_installed' --output-on-failure; execute exact approved on-target build/compositor/consumer timing commands and preserve raw measurements
Runner: none
Evidence: Blocked by F2: .factory/environment.toml does not declare minimum-hardware, architecture-build or compositor-specific coverage; no matching acceptance receipt is supplied.

## Task 26: Obtain human target-hardware visual release review
Title: Obtain human target-hardware visual release review
Status: blocked
Dependencies: 8, 12, 24, 25
Acceptance: Resolve F1: a human reviews representative installed Manager/overlay captures and controller-only operation on target hardware, including all tabs, editor modes/errors, Host/conflict states and degraded/recovery screens. Record reviewer, hardware, commit, captures and decisions on legibility, clipping, contrast, focus and usability. Required fixes must pass their owning task's verification before approval. Automated image similarity is not human approval; this task never promotes main.
Verification: Human review of the exact task 12, 24 and 25 capture/scenario commands and their resulting artifacts against docs/SPEC.md §§4.10, 5.6 and 11.1 item 7
Runner: none
Evidence: Blocked by F1: no human release-review decision is provided.

## Task 27: Resolve contradictory campaign completion contracts
Title: Resolve contradictory campaign completion contracts
Status: blocked
Dependencies: none
Acceptance: An authorized human/control-plane owner supplies an explicit resolution for F3, including fixed-plan versus appended-remediation behavior, commit ownership and permitted study inputs. The planner/developer does not modify committed specifications or runtime stores to resolve this. Within this campaign, newly discovered unscheduled blockers prevent completion and remain clearly identified; they are not silently deferred or interpreted as permission to mutate the fixed task set. Any specification revision belongs outside this implementation campaign with a fresh binding/plan. Do not invent waiver of product requirements or accept unavailable evidence.
Verification: Read-only comparison of the authorized decision with AGENTS.md, the planner role, docs/FACTORY-LOOP-SPEC.md §§3.1–3.3 and 4.2–4.3, and docs/SPEC.md §11.2; control-plane validation of the resulting campaign contract
Runner: none
Evidence: Blocked by F3's exact conflicting clauses; no authorized resolution is supplied. No factory runtime source/state inspection or specification edit was attempted.

## Task 28: Final documentation and specification audit
Title: Final documentation and specification audit
Status: pending
Dependencies: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27
Acceptance: Final task 28. Independently audit every preceding task, the complete normative conformance matrix, bug ledgers and real verification receipts. README/build/install/recovery/acceptance documentation must describe observed behavior and limitations accurately. Require fresh clean build, complete local/native/installed/remote suites, supported sanitizer/static checks and correctness/test-quality/security/documentation reviews, without unexplained skips, weakened assertions, unresolved required defects or prose-only evidence. All prior tasks, including external/human blockers, must actually be complete before this task can complete. Verify the specification binding and clean develop tree at the control-plane commit boundary; roles do not commit or promote main. Any gap prevents completion under the resolved task-27 contract. The planner makes no product-acceptance claim.
Verification: ./scripts/verify.sh; ./scripts/verify-sanitizers.sh; rerun the exact required runner/platform commands from tasks 11, 12 and 23–25; independent read-only conformance, interaction-inventory, artifact, bug-ledger and documentation review; control-plane binding and clean-tree check
Runner: none
Evidence: none

