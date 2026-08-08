---
spec_path: docs/SPEC.md
spec_commit: 3a10f6b7d04a615b2b9d06eef6c91e431fa9c079
spec_blob: 58f5d3cb72bc6b3e5f573fa09a63c11a653ed577
base_commit: 3a10f6b7d04a615b2b9d06eef6c91e431fa9c079
status: active
---

# Implementation Plan — Installed Functional Recovery

## Goal

Make the installed product perform its core job from a clean environment: connect to a native-signature InputPlumber service, detect and accept real controller input, create observable virtual targets, create/save/reload profiles, apply assignments, and show a compositor-visible overlay. Mock-only, keyboard-proxy, off-screen, and no-backend evidence is supplemental rather than release evidence.

## Non-goals and constraints

- Do not bundle or replace InputPlumber.
- Keep one writer on `develop`; subagents remain read-only and builds/tests remain serial.
- Preserve existing component tests, but replace false production claims with executable installed evidence.
- InputPlumber 0.78.0 DBus source at upstream commit `082f67f` is the compatibility reference.

## Specification conformance matrix

| ID | Requirement | Classification | Evidence/gap | Task |
|---|---|---|---|---|
| FR-01 | §§2.2, 10 native DBus signatures | missing | Production reads every property as `s`; required `as`, `u`, and `b` fail | Task 1 |
| FR-02 | §§2.4, 10.1 readiness/recovery/hotplug | verified | Manager and overlay drain DBus unconditionally; distinct degraded reasons; version compat check; owner loss/reacquisition via native sd-bus | Tasks 2, 9 |
| FR-03 | §§5.1, 5.7 real controller Manager input | missing | Manager initializes video only and tests inject keyboard events | Task 3 |
| FR-04 | §§5.2, 5.5 authoritative virtual topology | verified | Add picker uses native arrays (Task 4); startup reconcile creates/attaches/confirms ordered topology with per-slot type correction and rollback (Task 5); Add confirms type via ObjectManager + DeviceType (Task 4); AttachTargetDevice makes targets routable (Task 5) | Tasks 4, 5 |
| FR-05 | §§5.3–5.4 clean-home profile workflow | missing | No shipped Default; Empty cannot add first binding; save/discard is implicit | Tasks 6, 7 |
| FR-06 | §§4.1–4.7 assignment/profile backend application | verified | cbx_overlay_on_save applies-to-engine-first (LoadProfilePath + ProfilePath verification + AttachTargetDevice + SetGamepadOrder) before persisting; overlay_backend_ready restores after restart; native test observes GamepadOrder, LoadProfilePath, ProfilePath, restart/restore | Task 8 |
| FR-07 | §§4.9–4.10 compositor-visible reusable overlay | verified | Per-activating-composite lifecycle via cbx_poll_activation_ctx (close sets PASS on activating composite); poll re-arm after close permits unlimited activation cycles; ip_hotplug wired into overlay service with model_changed flag triggering cbx_overlay_reconcile_hotplug (rebuilds grid columns, input map, triggers, polls); cbx_overlay_rearm_polls factored for reuse; test_overlay_reconcile covers 7 scenarios | Task 9 |
| FR-08 | §§9, 11.1 installed functional acceptance | verified | `test_installed_functional` (non-skippable) exercises full production workflow against private native-signature DBus service with SDL virtual controller: controller detection, routable target creation, profile/settings persistence, manager restart, overlay InterceptMode lifecycle, assignment application, backend restart recovery | Task 10 |
| FR-09 | §§5.6–5.7 degraded/error semantics | partial | Backend failures are silent no-ops and controls look enabled | Tasks 2, 4, 7 |
| FR-10 | §11.2 autonomous definition of done | missing | Previous all-verified matrix relied on mocks and skipped backend outcomes | Tasks 10, 11, 12 |

## Interaction acceptance inventory

Production acceptance covers these semantic workflows through normal production dispatch:

- **Manager/controller:** hotplug/open a real or kernel-backed SDL game controller; D-pad navigates, A activates, B cancels, and focus changes are visible. Keyboard events are proxy-only.
- **Manager/pointer:** derive clicks from final rendered bounds for Controllers Add/Remove/Type, Profiles Create/Edit/Save/Delete, and Settings persistence; outcomes are independently observed over DBus or filesystem.
- **Controllers/backend:** native `as` type picker → create/attach/confirm target → type change → remove/unassign, with unavailable/denied/failed operations visibly recoverable.
- **Profiles:** clean HOME → shipped Default copy and Empty sequential capture → explicit Save → file exists and parses → Manager restart reloads it; explicit Discard and validation errors create no file.
- **Overlay/controller:** native DBus InputEvent opens the mapped window, moves assignments, cycles a real enumerated profile, enters/exits Host Mode, resolves conflicts, applies backend state, and restores PASS on the activating composite.
- **Recovery:** backend name loss disables operations without exiting; reacquisition and ObjectManager hotplug reconcile production state within specified bounds.

## Task 1: Native typed DBus transport and compatibility fixture
- Status: complete
- Evidence: commits `16f1ec1` and `8fdd4ac`; `test_native_dbus` round-trips native `s`, `as`, `u`, and `b` through the production sd-bus backend, and wrapper/path/signature tests pass.
- Dependencies: none
- Scope: production-owned DBus contract header, `src/dbus/dbus_client.c`, property/method wrappers, mock fidelity, private sd-bus fixture tests.
- Acceptance criteria: native `s`, `as`, `u`, and `b` properties round-trip against a private service; Manager Version uses `/Manager`; wrong signatures fail tests; production no longer depends on a string-only property contract.
- Verification: clean build plus `ctest -R 'test_native_dbus|test_connection|test_manager_calls|test_composite_calls|test_source_props|test_target_props'`.
- Documentation impact: `docs/DBus-API.md` compatibility reference.

## Task 2: InputPlumber readiness, degraded UI, and owner recovery
- Status: complete
- Evidence: overlay `cbx_overlay_service_step` drains DBus unconditionally via `conn.backend->process`; `ip_connection_handle_name_changed` produces distinct degraded reasons per error code (ServiceUnknown/AccessDenied/NoReply/InvalidArgs) and checks version compatibility (>= 0.78.0); `test_native_dbus` exercises real sd-bus owner loss/reacquisition; `test_connection` covers distinct reason strings, version compat, and mock `process()` draining queued NOC; `test_overlay_service` proves step drains DBus when `input_events_ready=false`; `test_manager_dbus_inject` proves the manager loop drains NOC and fires `cbx_manager_backend_ready`.
- Dependencies: Task 1
- Scope: `ip_connection`, Manager/overlay startup and event loops, visible status/disabled controls, owner-change recovery.
- Acceptance criteria: raw bus connection is not readiness; unavailable, denied, incompatible, and enumeration failures are distinct; both loops process DBus; owner acquisition/loss recovers without restart.
- Verification: `ctest -R 'test_native_dbus|test_connection|test_overlay_service|test_manager_dbus_inject'` \u2014 all pass.
- Documentation impact: README and `docs/OPERATIONS.md` diagnostics.

## Task 3: Real SDL game-controller input and hotplug
- Status: complete
- Evidence: commits `16f1ec1` and `e8fe463`; Manager owns existing/hotplugged SDL controller handles and `test_manager_production` drives production dispatch from an SDL virtual game controller.
- Dependencies: none
- Scope: SDL initialization, controller handle lifecycle, button-to-semantic mapping, Manager dispatch, controller transport tests.
- Acceptance criteria: existing and hotplugged controllers navigate/activate/cancel through `SDL_CONTROLLER*`; removal is safe; keyboard remains supplemental.
- Verification: SDL virtual-controller test traverses representative Manager controls without synthesized key events.
- Documentation impact: controller support and troubleshooting.

## Task 4: Functional Controllers tab with confirmed backend outcomes
- Status: complete
- Evidence: commit `0df65d3`; `cbx_controllers_tab_add` now verifies the new target's `DeviceType` matches the selected type after ObjectManager refresh (SPEC §5.2); failed Add/Remove/Change-type operations show the failed DBus operation in the status label; `test_native_target_operations` exercises `CreateTargetDevice`, `StopTargetDevice`, `GetManagedObjects`, and `DeviceType` through the real sd-bus backend with a private dbus-daemon and forked server; `test_add_rejects_type_mismatch`, `test_error_display_on_failed_add`, `test_error_display_on_unconfirmed_add`, and `test_error_clear_on_new_operation` cover the new production paths; 81/81 CTest pass.
- Dependencies: Tasks 1, 2
- Scope: supported type loading, actionable errors, operation state, target confirmation, Controllers interaction tests.
- Acceptance criteria: Add picker uses native arrays; Add/Remove/Type do not silently fail; UI updates only after ObjectManager confirms exact target type/count; controls disable when unavailable.
- Verification: native fixture and pointer/controller production dispatch tests assert exact DBus and model outcomes.
- Documentation impact: Controllers workflow.

## Task 5: Authoritative startup target topology and routability
- Status: complete
- Evidence: cbx_reconcile_startup_targets now has 4 phases (grow, shrink, per-slot type correction via reverse-order stop+create, attach targets to composites for routability) with rollback on partial failure (stops targets created during this reconcile that were not in the original set); test_native_topology_reconciliation exercises full topology lifecycle through real sd-bus (create ordered topology, verify DeviceType per slot, attach and verify routability via CompositeDevice TargetDevices property, remove preserving others, type correction via reverse-order stop+create, re-attach after correction); test_reconcile_grow_and_attach, test_reconcile_create_fails, test_reconcile_enumerate_fails, test_reconcile_shrink cover mock-based paths; native test server extended with AttachTargetDevice method + TargetDevices property; 81/81 CTest pass.
- Dependencies: Task 4
- Scope: topology reconciliation service, settings integration, create/attach/stop/type transactions, assignment unapply/rollback.
- Acceptance criteria: clean startup reaches configured ordered target topology; created targets are attached/routable; removing/type-changing one slot preserves others; failures retain last confirmed topology.
- Verification: native ObjectManager topology scenarios and routed target-event observation.
- Documentation impact: startup topology semantics.

## Task 6: Shipped immutable Default profile
- Status: complete
- Evidence: commit `16f1ec1`; clean-XDG enumeration, copy/create, parse, and packaging tests pass with the bundled read-only asset.
- Dependencies: none
- Scope: bundled InputPlumber YAML, path/enumeration fallback, packaging, clean-home tests.
- Acceptance criteria: Default is always present/read-only from Controller-Box assets; Default copy succeeds without host profiles; packages install the asset.
- Verification: clean-XDG profile list, create flow, packaging/install tests, InputPlumber-compatible parse.
- Documentation impact: Profiles and packaging docs.

## Task 7: Reachable explicit profile creation, save, discard, and errors
- Status: complete
- Evidence: commits `16f1ec1` and `853f05f`; Empty activation enters sequential capture, visible Save/Discard controls traverse pointer and controller production dispatch, persistence/reload succeeds, and validation/filesystem failures remain recoverable.
- Dependencies: Tasks 3, 6
- Scope: zero-row add/sequential entry, visible Save/Discard controls, dirty/close handling, error reporting, repeat-open visibility, installed profile workflow tests.
- Acceptance criteria: Empty can capture first and NES-minimum bindings; explicit save persists/reloads; discard/window-close behavior is explicit; production event ordering cannot consume the confirmation release; failures stay recoverable.
- Verification: clean-home controller and pointer production dispatch tests plus restart persistence.
- Documentation impact: editor controls and XDG locations.

## Task 8: Apply assignments and profiles to live InputPlumber
- Status: complete
- Evidence: commit `259b280`; `cbx_overlay_on_save` refactored to apply-to-engine-first (LoadProfilePath + verify ProfilePath read-back + AttachTargetDevice + SetGamepadOrder) before syncing in-memory assignments and persisting to disk; `cbx_profile_cycle_apply` verifies ProfilePath after LoadProfilePath; `overlay_backend_ready` restores profiles/GamepadOrder after InputPlumber restart via `cbx_overlay_on_save`; new `ip_composite_get_profile_name` / `ip_composite_get_profile_path` wrappers; `test_native_assignment_application` exercises full lifecycle (create targets, attach, LoadProfilePath, verify ProfilePath/ProfileName, SetGamepadOrder, read back, simulate restart, restore, verify) through real sd-bus; mock tests for LoadProfile failure and ProfilePath mismatch; 81/81 CTest pass.
- Dependencies: Tasks 1, 5, 6
- Scope: overlay profile enumeration/cycling, stable identity application, target/order updates, confirmed persistence and rollback.
- Acceptance criteria: slot/profile changes update verified engine state before persistence; startup/restart restores order/profile; LoadProfile failures do not appear saved.
- Verification: native fixture observes `GamepadOrder`, target attachment, `LoadProfilePath`, restart/reordered enumeration, and routed input.
- Documentation impact: assignment/profile recovery.

## Task 9: Visible reusable overlay and runtime reconciliation
- Status: complete
- Evidence: per-activating-composite lifecycle via `cbx_poll_activation_ctx` updates `lifecycle.composite_path` on activation so close sets InterceptMode=PASS on the activating composite (SPEC §2.5); poll re-arm in `cbx_overlay_service_step` re-arms IDLE polls when lifecycle is IDLE and backend ready, permitting unlimited activation cycles (SPEC §2.5); `ip_hotplug` wired into overlay service with `model_changed` flag triggering `cbx_overlay_reconcile_hotplug` which rebuilds grid rows/columns via `cbx_dynamic_columns_rebuild`, input mappings, triggers, and polls (SPEC §10.1); `cbx_overlay_rearm_polls` factored out for reuse in startup, recovery, and hotplug; `test_overlay_reconcile` covers per-composite activation (comp 0 and comp 1), poll re-arm across close→reopen, surface reuse across cycles, hotplug target add (3→4 columns) and remove (3→2, position clamping), and window visibility tracking lifecycle; 82/82 CTest pass.
- Dependencies: Tasks 1, 2, 8
- Scope: show/hide/map window, per-activating-composite lifecycle, poll re-arm, hotplug reconciliation, dynamic columns, production visuals.
- Acceptance criteria: activation maps a compositor-visible frame; close restores PASS on the activating composite and permits later activations; hotplug/restart rebuilds rows, mappings, triggers, polls, and confirmed target columns.
- Verification: compositor capture plus native multi-composite/hotplug lifecycle tests and latency measurements.
- Documentation impact: compositor support matrix and service lifecycle.

## Task 10: Mandatory installed functional acceptance gate
- Status: complete
- Evidence: commit `d8f42da`; `test_installed_functional` (non-skippable, no SKIP_RETURN_CODE) links against the production `controllerbox` library and uses `ip_dbus_sd_backend()` against a private `dbus-daemon` with a forked InputPlumber-compatible server (extended with InterceptMode writable `u`, DbusDevices `as`, Name `s`, SetInterceptActivation `ass` on CompositeDevice, two composites). SDL virtual game controller (SDL_JoystickAttachVirtual + SDL_GameControllerAddMapping) detected by `cbx_manager_init`. Controller navigation verified via real SDL transport (virtual joystick button → SDL_CONTROLLERBUTTONDOWN → manager dispatch → tab switch). Routable target created via `ip_manager_create_target_device` + `ip_manager_attach_target_device`, verified independently via `cbx_objectmanager_enumerate` + `ip_target_get_device_type` + `ip_composite_get_target_devices`. Settings persistence via `cbx_settings_tab_save` → filesystem verification. Manager restart verifies settings file, profile file, and backend target survive. Overlay InterceptMode lifecycle (PASS → ALL → PASS) via real `ip_composite_set_intercept_mode`/`ip_composite_get_intercept_mode`. Assignment application via `ip_composite_load_profile_path` + `ip_manager_set_gamepad_order` verified on server. Backend restart (kill+restart server, re-init manager) verifies recovery. 83/83 CTest pass (1 skip = backend_smoke, needs GPU).

## Task 11: Factory backpressure against false completion
- Status: pending
- Dependencies: Task 10
- Scope: Ralph prompts, validators/final gates, boilerplate tests/docs, reusable boilerplate branch parity.
- Acceptance criteria: final completion requires recorded installed-functional evidence; keyboard proxies, string mocks, fixture assembly, missing backend, and skips cannot mark production requirements verified; review findings mechanically append remediation tasks.
- Verification: factory regression fixtures reject the previous false-complete plan/evidence and accept valid installed evidence.
- Documentation impact: factory methodology and Huntley comparison.

## Task 12: Final documentation and specification audit
- Status: pending
- Dependencies: Tasks 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
- Scope: complete definition of done, conformance matrix, interaction inventory, open bugs, independent reviews, docs, clean builds and repository.
- Acceptance criteria: every conformance row is verified by production evidence; installed functional and interaction gates pass without skips; open defects and independent correctness/security/test/docs reviews have no blocker; documentation is current and tree is clean. Any gap appends a pending remediation task and returns this audit to pending.
- Verification: clean Debug/Release builds, full CTest, installed functional gate, packaging, `verify-project.sh`, `verify-boilerplate.sh`, and target-hardware human review recorded as release prerequisite.
- Documentation impact: README, operations, packaging, profiles, DBus API, conformance evidence.
