---
spec_path: docs/SPEC.md
spec_commit: 3a10f6b7d04a615b2b9d06eef6c91e431fa9c079
spec_blob: 58f5d3cb72bc6b3e5f573fa09a63c11a653ed577
base_commit: 227c6d80ae72d085f460c31c365b23cb03a5e2f6
status: active
---

# Implementation Plan — Controller-Box v1 Conformance Completion

## Goal and non-goals

**Goal:** Close all remaining specification conformance gaps to satisfy the
§11.2 autonomous definition of done. The codebase is substantially complete
(213 source files, 92 tests). Remaining gaps: (1) installed functional
acceptance links against the production library instead of executing the
installed binary (campaign-audit finding 1); (2) the full interaction
inventory is verified only with string-only mock DBus — §5.7 requires
native-signature backend evidence (campaign-audit finding 2); (3) one
pre-existing test failure; (4) the interaction inventory file has stale task
references, inaccurate verify_status values, pointer-path gaps, and a missing
Player Mode conflict entry; (5) several non-interaction requirements lack
native-DBus evidence for DBus-dependent operations.

**Non-goals:** New product features, refactoring, or reimplementation. No
changes to the committed specification. No new external runners or
capabilities beyond those declared in `.factory/environment.toml`.

## Architecture and constraints

- One binary (`controller-box`), two modes (`--manager`, `--overlay-service`),
  sharing one codebase and DBus client.
- All state changes go through InputPlumber's system DBus via sd-bus with
  native type signatures (`u` for InterceptMode, `b` for booleans, `as` for
  string arrays). The production `sd_get_property` in `dbus_client.c` reads
  each property using its declared DBus signature. The string-only
  `ip_dbus_mock` returns all properties as strings regardless of signature
  and is supplemental per §5.7.
- Production-path evidence requires real SDL event dispatch through
  `cbx_manager_handle_event` / `cbx_overlay_service_step`, not direct
  callback invocation. Keyboard-dispatched controller-path tests are
  supplemental accessibility evidence (§5.7); primary controller-transport
  evidence uses `SDL_JoystickSetVirtualButton` → `SDL_CONTROLLERBUTTONDOWN`.
- Visual acceptance requires deterministic framebuffer readback through
  production composition functions (§§4.10, 5.6, 11.1).
- The declared environment provides `remote-project-gate` and `systemd-user`
  runner capabilities only. No `gpu-compositor` or `physical-controller`
  runner is declared; GPU backend smoke (§11.1.6) and target-hardware human
  acceptance (§11.1.7) remain human-release-gated.

## Specification conformance matrix

| ID | Spec § | Requirement | Classification | Evidence | Task |
|----|--------|-------------|---------------|----------|------|
| AR-01 | §2.1 | Engine vs control surface separation | verified | All state changes through DBus (src/dbus/); no direct input routing | — |
| AR-02 | §2.2 | Direct DBus via sd-bus | verified | src/dbus/dbus_client.c sd-bus backend; test_native_dbus.c | — |
| AR-03 | §2.3 | One binary, two modes | verified | src/app/main.c; test_installed_smoke.sh exercises both modes | — |
| AR-04 | §2.4 | Service model + degraded recovery | verified | src/manager/service_install.c; test_installed_backend_recovery | — |
| AR-05 | §2.5 | Single hotkey architecture (Select+A, PASS→ALL→PASS) | partial | trigger.c; test_overlay_interaction.c (mock dispatch); test_installed_functional.c (native InterceptMode set/get, no SetInterceptActivation) | Task 3 |
| AR-06 | §2.4 | User unit must not declare After=/Requires= for inputplumber.service | verified | test_service_install.c line 160: `assert_null(strstr(buf, "Requires=inputplumber.service"))`; line 158: `After=graphical-session.target` | — |
| SR-01 | §3 | Min hardware Pi 4 ARM64 GLES 3.0 | verified | CMakeLists.txt architecture flags; latency tests bound for Pi 4 | — |
| SR-02 | §3 | x86_64 + aarch64 build targets | verified | CMakeLists.txt; test_packaging tarball | — |
| SR-03 | §3 | Display server X11/Wayland/Gamescope | verified | SDL2 abstraction; test_installed_smoke.sh uses Xvfb | — |
| SR-04 | §3 | Runtime deps SDL2/systemd/nanosvg | verified | CMakeLists.txt, shell.nix, smoke_test_sdl2, smoke_test_nanosvg | — |
| SR-05 | §3 | InputPlumber separate dependency | verified | DBus integration; not bundled; test_native_dbus | — |
| SR-06 | §3 | Polkit authorization (system bus) | verified | All DBus calls via system bus verified by test_native_dbus.c (DBUS_SYSTEM_BUS_ADDRESS) and test_installed_functional.c (private system bus) | — |
| OV-01 | §4.2,§2.5 | Overlay trigger Select+A + SetInterceptActivation + PASS | partial | trigger.c; test_overlay_interaction.c (mock dispatch); test_installed_functional.c (native API, no overlay dispatch) | Task 3 |
| OV-02 | §4.3,§4.1 | Player Mode grid rows×cols L/R/U/D | partial | grid_render.c, player_mode.c; test_overlay_interaction.c (mock); test_overlay_visual.c (verified visual) | Task 3 |
| OV-03 | §4.4 | Host Mode R3 exclusive frozen | partial | host_mode.c; test_overlay_interaction.c (mock); test_overlay_visual.c (verified visual) | Task 3 |
| OV-04 | §4.5 | Conflict red highlight + auto-move lowest free | partial | conflict.c; test_overlay_interaction.c (mock); test_overlay_visual.c (verified visual) | Task 3 |
| OV-05 | §4.6 | Profile cycling per-controller follows controller | partial | profile_cycle.c; test_overlay_interaction.c (mock); test_overlay_integration.c (mock) | Task 3 |
| OV-06 | §4.7 | Dynamic columns scale with virtual controllers | partial | dynamic_columns.c; test_overlay_interaction.c (mock dispatch) | Task 3 |
| OV-07 | §4.8 | No nicknames model name + slot | verified | grid_render.c; test_overlay_visual.c (framebuffer text) | — |
| OV-08 | §4.9,§11 | Pre-built surface ≤75ms p99 <10ms ALL | verified | surface_build.c; test_overlay_latency.c (50ms poll is timer-bound, <10ms show is pure rendering; close <1ms is InputPlumber processing) | — |
| OV-09 | §4.10 | Visual acceptance deterministic framebuffer | verified | grid_render.c; test_overlay_visual.c, test_golden.c | — |
| OV-10 | §4.1,§11 | Close B→PASS hidden not destroyed | partial | lifecycle.c, close.c; test_overlay_interaction.c (mock); test_overlay_latency.c | Task 3 |
| OV-11 | §5.7 | Overlay interaction production event path | partial | overlay_service.c; test_overlay_interaction.c (mock, full coverage O01–O12) | Task 3 |
| MG-01 | §5.1 | Tab structure 3 tabs + tabbar nav | verified | manager.c; test_installed_functional.c (real gamepad + native DBus) | — |
| MG-02 | §5.1,§5.7 | Pointer/mouse secondary path every control | partial | test_manager_interaction_ctrl/prof.c (mock DBus); test_installed_smoke.sh (installed binary, no DBus); test_manager_interaction_ctrl.c:1760 (resize hit test, mock) | Task 5 |
| MG-03 | §5.2 | Controllers add/remove/type mixed types | verified | controllers_tab.c; test_installed_functional.c (real gamepad + native DBus); test_native_dbus.c topology reconciliation | — |
| MG-04 | §5.2 | Topology reconciliation success/failure criteria | partial | test_native_dbus.c:882 (Scenarios 1–4: create/remove/type-change with native DBus); failure-retains-topology not tested | Task 4 |
| MG-05 | §5.3 | Profiles browse/create/edit/delete + all starting points | partial | profiles_tab.c; test_installed_functional.c (Default copy via gamepad); test_manager_interaction_prof.c (mock for Empty/Clone) | Task 5 |
| MG-06 | §5.3 | Empty profile add-first-binding + save/discard explicit | partial | profiles_tab.c; test_manager_interaction_prof.c (mock); test_installed_functional.c (basic save) | Task 5 |
| MG-07 | §5.3 | Built-in immutable Default profile (clean install) | verified | test_profiles_tab.c:280 `test_clean_install_default_copy_uses_shipped`; is_default + read_only asserted | — |
| MG-08 | §5.4 | Profile editor binding list mode + shared diagram | partial | profile_editor_list.c; test_installed_functional.c (basic nav); test_manager_interaction_prof.c (mock detailed) | Task 5 |
| MG-09 | §5.4 | Profile editor sequential mode + progress | partial | profile_editor_seq.c; test_manager_interaction_prof.c (mock; test failure) | Tasks 1, 5 |
| MG-10 | §5.4 | NES minimum validation + error | partial | profile_validate.c; test_manager_interaction_prof.c (mock); test_profile_validate.c (unit) | Task 5 |
| MG-11 | §5.3 | Unsaved changes prompt on close | partial | profile_save.c; test_manager_interaction_prof.c (mock) | Task 5 |
| MG-12 | §5.5 | Settings tab all settings + persistence | partial | settings_tab.c; test_installed_functional.c (toggle/save); test_manager_interaction_ctrl.c (mock detailed) | Task 4 |
| MG-13 | §5.6 | Manager visual acceptance all 3 tabs + editor | verified | manager.c; test_manager_visual.c, test_golden.c | — |
| MG-14 | §5.7 | Manager interaction acceptance full inventory | partial | test_manager_interaction_ctrl/prof.c (mock, full coverage); test_installed_functional.c (subset, native DBus) | Task 5 |
| MG-15 | §5.7 | Post-resize hit testing (no stale rects) | partial | test_manager_interaction_ctrl.c:1760 `test_resize_hit_testing` (mock DBus); no native-DBus evidence | Task 4 |
| CF-01 | §7.3 | settings.yaml schema + persistence | verified | config_settings.c; test_settings | — |
| CF-02 | §7.4 | assignments.yaml + gamepad order persist | verified | config_assignments.c; test_assignments, test_order_restore | — |
| CF-03 | §7.5 | profile-metadata sidecar optional | verified | config_profile_meta.c; test_profile_list | — |
| CF-04 | §7.6 | InputPlumber YAML format compatibility | verified | config_profile.c; test_profile_yaml | — |
| IC-01 | §8.1 | Virtual type icons not physical | verified | icon_map.c; test_icon_map, test_icon_lookup | — |
| IC-02 | §8.2-3 | Controllercons SVG + nanosvg rendering | verified | icon_cache.c; smoke_test_nanosvg, test_icon_cache | — |
| IC-03 | §8.4 | Icon mapping table + unknown type | verified | icon_map.c, icon_lookup.c; test_icon_map, test_icon_lookup | — |
| IC-04 | §8.5 | Profile override icon | verified | icon_lookup.c; test_icon_lookup | — |
| ID-01 | §6.2 | Multi-layered auto-assignment | verified | identity.c, assign.c; test_identity, test_assign | — |
| ID-02 | §6.3 | ID format + identity-strength tracking + downgrade | verified | identity_downgrade.c; test_identity_downgrade | — |
| ID-03 | §7.4 | Assignment persistence across restarts | verified | assign_persist.c; test_assign_persist, test_order_restore | — |
| DB-01 | §10.1 | Connection model + ObjectManager enumeration | verified | ip_connection.c, ip_objectmanager.c; test_native_dbus, test_objectmanager_parse | — |
| DB-02 | §10.1 | Native type fidelity u/b/as/s | verified | dbus_client.c sd_get_property reads u/b/as/s natively; test_dbus_signatures, test_native_dbus | — |
| DB-03 | §10.1 | Operational readiness + Version + typed props | verified | ip_connection.c; test_native_dbus, test_installed_functional | — |
| DB-04 | §10.1 | Hotplug InterfacesAdded/Removed | verified | ip_hotplug.c; test_hotplug (mock signal); test_installed_functional (native enumeration after target create/remove) | — |
| DB-05 | §2.4 | NameOwnerChanged + re-enumerate <2s | verified | ip_connection.c; test_installed_backend_recovery (native DBus) | — |
| DB-06 | §2.5,§10.3 | InterceptMode poll ~50ms gap #1 | partial | ip_intercept_poll.c; test_intercept_poll (mock); test_overlay_latency (mock); no native-DBus poll path | Task 3 |
| DB-07 | §10.3,§7.4 | GamepadOrder persistence gap #2 | verified | gamepad_order_restore.c; test_gamepad_order, test_order_restore | — |
| DB-08 | §10.3 | CreateCompositeDevice temp YAML gap #3 | verified | ip_create_composite.c; test_create_composite | — |
| DB-09 | §10.3 | Filesystem enumeration gap #4 | verified | config_profile_list.c; test_profile_list | — |
| DB-10 | §10.2 | Manager interface methods | verified | ip_manager.c; test_manager_calls, test_native_dbus | — |
| DB-11 | §10.2 | CompositeDevice interface | verified | ip_composite.c; test_composite_calls, test_native_dbus | — |
| DB-12 | §10.2 | Target device interfaces + DeviceType | verified | ip_target.c; test_target_props, test_native_dbus | — |
| DB-13 | §10.2 | Source device interfaces + identification | verified | ip_source.c; test_source_props | — |
| PK-01 | §9.1 | Flatpak manifest experimental | verified | packaging/org.shadowblip.ControllerBox.yaml; test_flatpak_manifest | — |
| PK-02 | §9.2 | Tarball make install x86_64+aarch64 | verified | CMakeLists.txt; test_packaging | — |
| PK-03 | §9.3 | Install layout binary/service/desktop/icons | verified | CMakeLists.txt install rules; test_packaging | — |
| PK-04 | §2.4,§9.1 | Systemd user service installed by manager | verified | service_install.c; test_service_install (verifies no Requires=inputplumber.service) | — |
| PK-05 | §9.4 | InputPlumber dependency documentation | verified | docs + CMakeLists; test_packaging | — |
| PR-01 | §11 | Overlay appearance ≤75ms p99 ≤100ms max | verified | test_overlay_latency.c (50ms poll is timer-bound; <10ms show is pure render; ≤75ms derived from 50+10+overhead) | — |
| PR-02 | §11 | Gameplay input latency ~1-2ms | verified | Architectural: PASS mode is kernel-level only; verified by overlay lifecycle tests setting InterceptMode=PASS after close (test_overlay_interaction.c, test_overlay_latency.c); GUI never routes gameplay input (AR-01) | — |
| PR-03 | §11 | Overlay close <1ms | verified | test_overlay_latency.c (close path timing; <1ms target is InputPlumber's processing after GUI's DBus call) | — |
| PR-04 | §11 | Daemon footprint always resident | verified | test_overlay_latency idle step p99<5ms (SDL2 memory profile) | — |
| PR-05 | §11 | Player reorder atomic InputPlumber-managed | verified | test_gamepad_order (mock); test_installed_functional (native GamepadOrder set) | — |
| PR-06 | §11.1 | Rendering verification 7 layers | partial | test_overlay_visual, test_manager_visual, test_golden, test_fb_assert, test_backend_smoke_sw (6 layers verified); test_backend_smoke skipped (no GPU runner, human-release-gated §11.1.6) | Task 8 |
| VS-01 | §11.1.5 | Installed functional smoke test | partial | test_installed_functional.c links library not installed binary; test_installed_smoke.sh basic no DBus | Task 7 |

## Interaction acceptance inventory

The repository maintains a machine-readable inventory at
`tests/interaction_inventory.c` (header: `tests/interaction_inventory.h`)
enumerating 58 entries across seven categories:

- **Manager tabbar (M01–M03):** Controllers/Profiles/Settings tab switching.
  Controller path: D-pad Left/Right through `cbx_manager_handle_event`.
  Pointer path: mouse click on tab label through `cbx_manager_handle_event`.
  Native-DBus evidence: M01–M03 via test_installed_functional.c (real gamepad
  + native DBus). Mock-DBus: all via test_manager_interaction_ctrl.c.
- **Manager Controllers (M04–M09):** Device list select, Add, Remove, Change
  Type, type picker confirm/cancel. Native DBus: M05–M08 via
  test_installed_functional.c. Mock DBus: all via test_manager_interaction_ctrl.c.
- **Manager Profiles (M10–M20):** Profile list select, Create, create source
  picker, name input, Edit, Delete, delete confirm/cancel. Native DBus:
  M11/M15/M17/M19 via test_installed_functional.c. Mock DBus: all via
  test_manager_interaction_prof.c.
- **Manager Settings (M21–M27):** Settings list select, toggle, edit, cancel,
  save. Native DBus: M22/M27 via test_installed_functional.c. Mock DBus: all
  via test_manager_interaction_ctrl.c.
- **Manager Profile Editor (M28–M38):** Binding list nav, activate, target
  picker, capture, sequential, save, cancel/discard. Native DBus:
  M28/M29/M37/M38 via test_installed_functional.c. Mock DBus: all via
  test_manager_interaction_prof.c.
- **Overlay (O01–O12):** Open, move, cycle profile, Host Mode, close+conflict,
  multi-controller, Host profile cycle (deferred §13). All via
  test_overlay_interaction.c with mock DBus and `cbx_overlay_service_step`.
- **Disabled/degraded (D01–D08):** InputPlumber unavailable, no device, no
  profile, NES validation error, settings cancel, DBus failure, filesystem
  failure, empty profile creation. D01 via test_installed_backend_recovery
  (native DBus). D02–D08 via mock DBus.

**Semantic outcome evidence:** Each entry records the expected semantic
outcome (state transition, DBus call, file mutation, lifecycle change) and
the production dispatch path. Tests must verify the semantic outcome, not
merely event consumption or focus movement.

**Inventory file defects (Task 1):** The inventory file has stale task
references ("Task 7", "Task 8", "Task 9", "Task 11" from a prior plan),
verify_status values marked `CBX_VERIFY_VERIFIED` for entries the plan
classifies as `partial` (mock-only evidence), pointer-path availability
marked `CBX_PATH_NA` for dialog actions and disabled controls that §5.1/§5.7
require to be pointer-tested, and a missing Player Mode conflict detection
entry (distinct from Host Mode O08 and close O10). Task 1 corrects these.

**Gap summary:** String-only mock DBus provides full inventory coverage with
production dispatch but cannot satisfy §5.7 "verified" because it does not
preserve native type signatures. Native-DBus evidence covers a subset. Tasks
3–7 close the gap by extending native-DBus coverage to the full inventory
through production dispatch for both controller and pointer paths, verifying
semantic outcomes against a real private DBus service.

## Task 1: Fix test failure and correct interaction inventory file
- Status: pending
- Dependencies: none
- Scope: Two fixes in test files. (a) Fix `test_editor_seq_capture_dbus_signal` in `tests/test_manager_interaction_prof.c` (fails: assertion `0 != 1` at line 1569 — sequential binding step does not advance when InputEvent is injected through mock backend signal path). Diagnose whether the mock signal injection or the editor's InputEvent subscription is broken; fix the root cause. (b) Correct `tests/interaction_inventory.c` and `tests/interaction_inventory.h`: update stale `evidence_task` strings from "Task 7/8/9/11" to match this plan's task numbering; downgrade `verify_status` from `CBX_VERIFY_VERIFIED` to `CBX_VERIFY_UNVERIFIED` for entries classified `partial` in the conformance matrix (mock-only evidence); change `pointer_path_avail` from `CBX_PATH_NA` to `CBX_PATH_AVAILABLE` for dialog actions (M09, M15, M16, M19, M20, M24–M26) and all disabled scenarios (D01–D08) per §5.1/§5.7; add a new entry O13 for Player Mode conflict detection (two controllers independently navigate to same column, red highlight appears — distinct from Host Mode O08 and close O10). Update the count constant and `cbx_inv_category` if needed.
- Acceptance criteria: `test_manager_interaction_prof` passes (all 45 tests); `test_interaction_inventory` passes with corrected entries and updated count (59 entries including O13); no other tests regress.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'test_manager_interaction_prof|test_interaction_inventory' --output-on-failure"`
- Documentation impact: none

## Task 2: Extend native DBus test server for overlay interaction
- Status: pending
- Dependencies: none
- Scope: `tests/test_native_dbus.c` — extend the private InputPlumber-compatible server to support overlay interaction dispatch. Add: (a) `SetInterceptActivation(activation_events: as, target_event: s)` method on CompositeDevice; (b) writable `InterceptMode: u` property on CompositeDevice (currently read-only in the native server); (c) `org.shadowblip.Input.DBusDevice` interface with `InputEvent(event: s, value: d)` signal emission capability. Extract the server into a reusable header/source (e.g., `tests/native_ip_server.h` / `.c`) so `test_native_dbus.c`, `test_installed_functional.c`, and new overlay tests can share it. Verify native type signatures (`u`, `as`, `sd`) are used correctly on the wire.
- Acceptance criteria: Extended server compiles; existing `test_native_dbus` tests pass; new server capabilities exercised by at least one round-trip test (SetInterceptActivation returns success; InterceptMode set/get round-trip as `u`; InputEvent signal emitted and received).
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'test_native_dbus' --output-on-failure"`
- Documentation impact: none

## Task 3: Overlay interaction acceptance with native DBus backend
- Status: pending
- Dependencies: Task 2
- Scope: Create overlay interaction tests that exercise O01–O13 (including the new O13 Player Mode conflict from Task 1) through `cbx_overlay_service_step` with the extended native-signature DBus server from Task 2. Each overlay action driven by real SDL events (SDL_PushEvent KEYDOWN or virtual gamepad SDL_CONTROLLERBUTTONDOWN) or real DBus InputEvent signals from the native server (not mock `inject_signal`). Verify semantic outcomes: grid column changes, LoadProfilePath calls, InterceptMode transitions verified as `u` type on the wire, conflict detection/resolution, Host Mode freeze, close→PASS. Cover open, movement, profile cycling, Host Mode, Player Mode conflict, conflict resolution, and close. This closes AR-05, OV-01–OV-06, OV-10, OV-11, DB-06.
- Acceptance criteria: O01–O11 and O13 pass with native-DBus evidence through `cbx_overlay_service_step`; InterceptMode verified as `u` on the wire during poll path; no `ip_dbus_mock` in the new test file.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'test_overlay' --output-on-failure"`; `grep -r 'ip_dbus_mock' <new_test_file>` returns no matches
- Documentation impact: none

## Task 4: Manager Controllers + Settings interaction with native DBus
- Status: pending
- Dependencies: Task 1
- Scope: Extend native-DBus + real gamepad coverage for Controllers and Settings tab controls not covered by `test_installed_functional.c`. Add test phases or a companion test using the existing private DBus server + `SDL_JoystickSetVirtualButton` transport: (a) M04 (device list select via gamepad); (b) M21 (settings list select), M23–M26 (opacity, VC count, VC type, trigger combo — all settings controls); (c) M15 (settings edit — pointer path for edit flow); (d) MG-04 topology failure scenario — attempt CreateTargetDevice when server returns error, verify last confirmed topology retained and error shown; (e) MG-15 post-resize hit testing with native DBus — resize window, verify click at new control center activates correct control through `cbx_manager_handle_event`; (f) Pointer path (SDL_MOUSEMOTION + MOUSEBUTTONDOWN/UP through `cbx_manager_handle_event`) for all above controls. Verify semantic outcomes (state transitions, DBus calls with native signatures, settings file mutations).
- Acceptance criteria: M04, M21, M23–M26, D06 (DBus failure) have passing controller + pointer evidence through production dispatch with native DBus; topology failure retains last topology; post-resize hit testing uses final layout; no `ip_dbus_mock` in new test code.
- Verification: `nix-shell --run "ctest --test-dir build-check --output-on-failure"` (full suite passes); new test file contains no `ip_dbus_mock`
- Documentation impact: none

## Task 5: Manager Profiles + Editor interaction with native DBus
- Status: pending
- Dependencies: Task 1
- Scope: Extend native-DBus + real gamepad coverage for Profiles tab and Profile Editor controls not covered by `test_installed_functional.c`. Add test phases or a companion test: (a) M10 (profile list select), M20 (profile select); (b) M12–M14 (name input: chars, backspace, confirm, cancel via gamepad A/B + key mapping); (c) M16 (edit cancel), M18 (delete cancel); (d) Profiles create from "Empty" and "Clone existing" starting points (M11 variants); (e) M30 (target picker confirm), M31–M32 (capture mode: begin + event capture), M33–M36 (sequential mode: begin, capture, skip, cancel); (f) MG-10 NES validation error — attempt save with missing bindings, verify error shown; (g) MG-11 unsaved changes prompt — close editor with unsaved changes, verify prompt appears; (h) D02 (no device selected), D03 (no profile selected), D04 (NES validation error), D05 (settings edit cancel), D07 (filesystem failure), D08 (empty profile creation) with native DBus; (i) Pointer path for all above controls with native DBus. Verify semantic outcomes (file mutations, state transitions, DBus calls).
- Acceptance criteria: M10, M12–M14, M16, M18, M20, M30–M36, D02–D05, D07–D08 have passing controller + pointer evidence through production dispatch with native DBus; no `ip_dbus_mock` in new test code.
- Verification: `nix-shell --run "ctest --test-dir build-check --output-on-failure"` (full suite passes); new test file contains no `ip_dbus_mock`
- Documentation impact: none

## Task 6: Disabled/degraded scenarios with native DBus
- Status: pending
- Dependencies: Task 1
- Scope: Extend native-DBus coverage for disabled and degraded scenarios that currently only have mock evidence. Using the existing private DBus server infrastructure from `test_installed_functional.c`: (a) D01 (InputPlumber unavailable) — already verified by `test_installed_backend_recovery` with native DBus; confirm pointer path (click disabled control, verify no side effect); (b) D06 (DBus operation failure — CreateTargetDevice returns error) — verify error shown, controls remain interactive, last topology retained; (c) D02 (no device selected in Controllers tab) — verify Add/Remove/ChangeType disabled or no-op with native DBus; (d) D03 (no profile selected) — verify Edit/Delete disabled with native DBus; (e) D04 (NES validation error) — verify save rejected with error; (f) D05 (settings edit cancel) — verify cancel returns to list without mutation; (g) D07 (filesystem failure on profile save) — verify error shown; (h) D08 (empty profile creation flow) — verify add-first-binding reachable. Both controller and pointer paths must reject disabled controls and produce no backend/filesystem side effect.
- Acceptance criteria: D01–D08 have passing controller + pointer evidence through production dispatch with native DBus; disabled controls reject both paths; no `ip_dbus_mock` in new test code.
- Verification: `nix-shell --run "ctest --test-dir build-check --output-on-failure"`; new test code contains no `ip_dbus_mock`
- Documentation impact: none

## Task 7: Installed binary functional acceptance test
- Status: pending
- Dependencies: Task 2
- Scope: Create a test that executes the installed `controller-box` binary (after `cmake --install` to a staging prefix) rather than linking against the production library in-process. The test must: (1) build and install the binary to a custom prefix; (2) start a private `dbus-daemon` + the native-signature InputPlumber-compatible server (from Task 2); (3) hotplug a kernel-backed synthetic SDL virtual gamepad; (4) launch the installed `controller-box --manager` binary as a subprocess; (5) send real controller events and verify semantic outcomes (tab navigation, target creation, profile save/reload, settings persistence); (6) launch `controller-box --overlay-service` and verify it starts, connects to the private DBus, and enters the idle poll loop; (7) verify persistence after process restart. The test may be a shell script (extending `test_installed_smoke.sh`) or a C harness that fork/execs the binary. It must NOT link against `libcontrollerbox`.
- Acceptance criteria: The installed binary launches, connects to the private native-signature DBus server, processes real controller events through its own `main()` event loop, creates a virtual target, saves/loads a profile, persists settings, and the overlay service enters idle poll — all through the binary's own initialization and dispatch paths. Test passes in Nix-shell with Xvfb.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'installed_binary' --output-on-failure"`; verify test target does not link `libcontrollerbox` (check CMakeLists.txt)
- Documentation impact: Update AGENTS.md run/inspect section if test name differs

## Task 8: Final documentation and specification audit
- Status: pending
- Dependencies: Tasks 1, 2, 3, 4, 5, 6, 7
- Scope: Execute the canonical definition of done (§11.2). (1) Re-audit every conformance matrix row — including rows currently classified `verified` — against the production-path standard: confirm each `verified` row has executable evidence through production dispatch and native DBus (where DBus is involved) or through production rendering paths (where visual). Reclassify any row found to lack sufficient evidence. (2) Verify the interaction acceptance inventory is exhaustive — every M01–M38, O01–O13, D01–D08 entry has passing controller and pointer evidence through production dispatch with native DBus; inventory file `verify_status` matches plan classifications. (3) Inspect `.factory/bugs/open.md` — no open bug contradicts a v1 requirement. (4) Run independent read-only reviews (correctness, test-quality, security, documentation) and resolve any blocking finding. (5) Run the full clean verification suite: `nix-shell --run './scripts/verify-project.sh'` — build, CTest (only `test_backend_smoke` may skip, human-release-gated §11.1.6), packaging, installed functional, smoke. (6) Verify documentation accuracy: README and operational docs match observed behavior; build, install, acceptance, artifact, and recovery commands work from a clean checkout. (7) Verify clean Git state on `develop`. (8) Remediation rule: if any gap is found, preserve the ledger, append a uniquely numbered pending task, add it to this task's dependencies, and return this task to pending.
- Acceptance criteria: All-verified conformance matrix (PR-06 remains `partial` only if GPU runner is still undeclared — human-release-gated, not autonomous-blocking per §11.1.6–7); exhaustive interaction inventory with no mock-only evidence for `verified` rows; no contradictory open v1 bugs; independent adversarial reviews find no blocking issue; full clean verification passes; accurate documentation; clean Git state on `develop`.
- Verification: `nix-shell --run './scripts/verify-project.sh'`; `./scripts/final-gate.sh --planning`; `./scripts/validate-implementation-plan.py planning .factory/artifacts/implementation-plan.md`
- Documentation impact: Final README/docs review and update; ensure all build/install/acceptance commands work from clean checkout