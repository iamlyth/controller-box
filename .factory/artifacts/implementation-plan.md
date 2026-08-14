---
spec_path: docs/SPEC.md
spec_commit: 3a10f6b7d04a615b2b9d06eef6c91e431fa9c079
spec_blob: 58f5d3cb72bc6b3e5f573fa09a63c11a653ed577
base_commit: 3290f219908f5f7571c3847c03c53c98cf9463f1
status: active
---

# Implementation Plan

## Goal and non-goals

**Goal:** Close all remaining implementation and test-quality gaps between the committed specification (`docs/SPEC.md`) and the current repository state, achieving the §11.2 autonomous definition of done for v1.

**Non-goals:** Do not reimplement already-verified functionality. Do not add features deferred to post-v1 (§13). Do not change the committed specification.

## Architecture and constraints

- Single `controller-box` binary with two modes: `--overlay-service` (systemd user service) and `--manager` (on-demand).
- InputPlumber is the engine (system DBus, `org.shadowblip.InputPlumber`); the GUI is the control surface only.
- sd-bus native type fidelity required on the wire (`u`, `b`, `as`, `s`); string-only mock is supplemental.
- Tests must exercise production dispatch paths (SDL events through `cbx_manager_handle_event` / `cbx_overlay_service_step`; DBus signals through `ip_input_events_process`), not direct callbacks.
- Build under Nix (strict C11, `CMAKE_C_EXTENSIONS OFF`); declared environment is exhaustive.
- One autonomous `develop` branch, one mutating worker.

## Specification conformance matrix

| ID | Spec § | Classification | Evidence | Closing task |
|---|---|---|---|---|
| R-2.4a | §2.4 | verified | `packaging/controller-box.service`: no `After=`/`Requires=` for inputplumber.service; `After=graphical-session.target` | — |
| R-2.4b | §2.4 | verified | `Restart=on-failure`, `RestartSec=2s` in service file; `service_install.c` generates unit | — |
| R-2.4c | §2.4 | verified | `ip_connection.c` `get_unique_name`, `NameOwnerChanged` subscribe, reenumerate callback | — |
| R-2.4d | §2.4 | verified | `controllers_tab.c:cbx_controllers_tab_set_available` disables controls; `test_manager_visual.c` degraded test | — |
| R-2.5a-f | §2.5 | verified | `trigger.c` register via `SetInterceptActivation` + `InterceptMode=1`; `ip_intercept_poll.c` 50ms poll; `test_overlay_interaction.c` O01–O10 | — |
| R-3.1 | §3 | verified | `CMakeLists.txt` `pkg_check_modules` for SDL2/SDL2_ttf/SDL2_image/libsystemd/yaml-0.1/cmocka; `third_party/nanosvg/` vendored; `shell.nix` declares all deps; both x86_64 and aarch64 build via CMake (architecture-agnostic C) | — |
| R-3.2 | §3 | verified | SDL2 cross-platform display support (X11/Wayland/Gamescope via `SDL_VIDEODRIVER`); `smoke_test_sdl2` verifies SDL init; Flatpak manifest declares `--socket=wayland --socket=fallback-x11 --device=dri` | — |
| R-3.3 | §3 | verified | `packaging/controller-box.service` no `Requires=inputplumber.service`; polkit is InputPlumber's responsibility (separate system service) | — |
| R-4.2 | §4.2 | verified | `trigger.c` parse/register/register_all; `overlay_service.c:1118` reads settings; `test_trigger.c`, `test_overlay_interaction.c` O01 | — |
| R-4.3 | §4.3 | partial | `player_mode.c` left/right/up/down/B/R3 implemented; `test_player_mode.c` 22 unit tests. **Interaction tests O02–O05 use `SDL_KEYDOWN`, not `SDL_CONTROLLERBUTTONDOWN`; inventory entries UNVERIFIED** | Task 7 |
| R-4.4 | §4.4 | partial | `host_mode.c` toggle/navigate/edit/exit/freeze logic implemented; `test_host_mode.c` 41 tests; `test_overlay_interaction.c` O06–O09. **Visual rendering does not differentiate HOST/SELECTED/FROZEN row states** — `grid_render.c` has no host-mode visual branch | Task 6 |
| R-4.5 | §4.5 | partial | `conflict.c` detect/resolve; `grid_render.c:264` red {220,40,40}; `test_conflict.c` 34 unit tests + visual; `test_overlay_visual.c` conflict pixel test. **Interaction test O10b uses `SDL_KEYDOWN`** | Task 7 |
| R-4.6 | §4.6 | partial | `profile_cycle.c` per-controller profiles; `test_profile_cycle.c` 27 unit tests; `test_overlay_integration.c` profile-follows-controller. **Interaction tests O04–O05 use `SDL_KEYDOWN`** | Task 7 |
| R-4.7 | §4.7 | verified | `dynamic_columns.c` rebuild/clamp/extract; `test_overlay_reconcile.c` hotplug add/remove | — |
| R-4.8 | §4.8 | verified | Structural: `cbx_grid_row` has no nickname/label/color field; `grid_render.c` label = model_name + profile | — |
| R-4.9 | §4.9 | partial | `surface_build.c` pre-built texture, single RenderCopy, dirty-rect incremental; `test_surface_build.c` 28 tests. **No automated latency measurement tests** | Task 8 |
| R-4.10 | §4.10 | partial | `test_overlay_visual.c` 7 pixel tests for player/host/conflict/unassigned/text/icons; `test_golden.c` 4 overlay goldens. **Host Mode visual test only checks frame differs via conflict, not host-state rendering; font/icon-dependent tests skip instead of fail** | Tasks 6, 10 |
| R-5.1 | §5.1 | partial | `manager.c` 3 tabs, controller+pointer dispatch; `test_manager_tabs.c`, `test_manager_production.c`. **Interaction tests use `SDL_KEYDOWN`, not `SDL_CONTROLLERBUTTONDOWN`; inventory entries UNVERIFIED** | Task 7 |
| R-5.2 | §5.2 | partial | `controllers_tab.c` add/remove/type-change with ObjectManager verification; `test_controllers_tab.c` unit tests. **Interaction tests M04–M09 use `SDL_KEYDOWN`; inventory entries UNVERIFIED** | Task 7 |
| R-5.3 | §5.3 | partial | `profiles_tab.c` browse/create/edit/delete, default read-only, save/discard buttons, empty-profile sequential action. **SDL_QUIT in `manager.c:386` breaks without unsaved-changes prompt** | Task 5 |
| R-5.4a | §5.4 | verified | `profile_editor_list.c` binding list + diagram sync; `profile_editor_seq.c` sequential + auto-advance + skip + cancel + progress; `test_editor_list_mode.c`, `test_editor_seq_mode.c` | — |
| R-5.4b | §5.4 | verified | `profile_validate.c` NES minimum (A,B,Up,Down,Left,Right); `profile_save.c:83` hard gate; `test_profile_validate.c`, `test_manager_interaction_prof.c` D04/D08 | — |
| R-5.5 | §5.5 | partial | `settings_tab.c` launch-at-boot, theme, opacity, VC count/types, trigger combo, save/cancel. **No icon override setting row despite §5.5 listing "controller icon overrides (§8.4)"** | Task 4 |
| R-5.6 | §5.6 | partial | `test_manager_visual.c` 9 pixel tests through production render path. **Validation-error visual test skips without font** | Task 10 |
| R-5.7 | §5.7 | partial | `interaction_inventory.c` 58 entries (M01–M38, O01–O12, D01–D08); `test_manager_interaction_ctrl.c` + `test_manager_interaction_prof.c` both paths. **Controller path uses SDL_KEYDOWN not SDL_CONTROLLERBUTTONDOWN; all verify_status=UNVERIFIED; Save/Discard buttons missing from inventory** | Tasks 7, 11 |
| R-6.2 | §6.2 | verified | `identity.c` 4-layer (BT MAC > USB serial > USB phys > order); `assign.c`/`assign_persist.c` auto-assign; `test_identity.c` 43 tests, `test_assign.c` 32, `test_assign_persist.c` 36 | — |
| R-6.3 | §6.3 | verified | `identity.c` prefix format; `identity_downgrade.c` downgrade detection; `test_identity_downgrade.c` 33 tests | — |
| R-7.1–7.6 | §7 | verified | `config_settings.c`, `config_assignments.c`, `config_profile.c`, `config_profile_meta.c`, `config_profile_list.c`; tests: `test_settings.c`, `test_assignments.c`, `test_profile_yaml.c`, `test_profile_list.c`, `test_config_paths.c` | — |
| R-8.1–8.5 | §8 | verified | `icon_map.c`, `icon_cache.c`, `icon_lookup.c`; 36 SVGs in `data/icons/svg/`; `data/controller-icons.yaml`; `test_icon_map.c` 41, `test_icon_cache.c` 26, `test_icon_lookup.c` 36 | — |
| R-9.1–9.4 | §9 | verified | `packaging/org.shadowblip.ControllerBox.yaml`; `CMakeLists.txt` install rules; `service_install.c`; `test_flatpak_manifest.py`, `test_packaging.sh`, `test_packaging_install.sh`, `test_service_install.c` 31 tests | — |
| R-10.1a | §10.1 | verified | `dbus_client.c:sd_connect` system bus; `ip_connection.c` owner check + version check + NameOwnerChanged; `test_native_dbus.c` real sd-bus service | — |
| R-10.1b | §10.1 | partial | `dbus_client.c` reads `u`/`b`/`as`/`s` natively; `test_native_dbus.c` verifies `u`/`b`/`as` reads. **`sd_set_property` has no `b` branch — boolean writes sent as `v<s>`** | Task 2 |
| R-10.1c | §10.1 | partial | `ip_objectmanager.c` enumeration; `ip_hotplug.c` InterfacesAdded/Removed; `ip_properties.c` PropertiesChanged for 5 tracked properties. **No "validate required typed properties" step after enumeration** | Task 3 |
| R-10.1d | §10.1 | verified | `ip_input_signal.c` InputEvent `sd` parsing, validation, rate limiting; `test_input_signal.c` | — |
| R-10.2 | §10.2 | partial | All core Manager and CompositeDevice methods/properties implemented. **`ManageAllDevices: b` get/set wrapper missing** | Task 2 |
| R-10.3 | §10.3 | verified | Gap #1: `ip_intercept_poll.c` 50ms poll; Gap #2: `ip_gamepad_order.c` save/restore; Gap #3: `ip_create_composite.c` temp file; Gap #4: filesystem read; Gap #5: not needed | — |
| R-11.1.1 | §11.1 | verified | `test_overlay_visual.c`, `test_manager_visual.c`, `test_golden.c` render via production path + `SDL_RenderReadPixels` | — |
| R-11.1.2 | §11.1 | partial | `fb_assert.c` region assertions. **Font/icon-dependent tests `skip()` instead of failing** | Task 10 |
| R-11.1.3 | §11.1 | verified | 11 golden baselines in `tests/golden/`; `fb_golden_compare` ±3/2% tolerance; manual update via `CBX_GENERATE_GOLDEN=1` | — |
| R-11.1.4 | §11.1 | partial | `fb_save_png`/`fb_save_diff` save actual/expected/diff to `tests/golden-fail/`. **No renderer metadata in failure artifacts** | Task 10 |
| R-11.1.5 | §11.1 | partial | `test_installed_functional.c` real sd-bus + synthetic controller + manager navigation + profile persistence + backend restart. **Never calls `cbx_overlay_service_*` — no compositor-visible overlay activation** | Task 9 |
| R-11.1.6 | §11.1 | partial | `test_backend_smoke.c` accelerated renderer + pixel readback + golden comparison. **Golden comparison silently skipped if baseline missing** | Task 10 |
| R-11.1.7 | §11.1 | verified | Process defined in spec; `final-gate.sh` runs verify-project; human review is manual gate before `main` promotion | — |
| R-11-perf | §11 | missing | **No automated latency tests** for ≤75ms p99 button-to-visible, <10ms p99 ALL-to-present, <1ms close | Task 8 |
| R-11-gameplay | §11 | verified | Architectural invariant: GUI does not route gameplay input over DBus (§2.5); `ip_intercept_poll.c` only polls during overlay; gameplay input stays at kernel level in PASS mode | — |
| R-11-footprint | §11 | verified | SDL2 minimal memory; pre-built surface (`surface_build.c`); idle wait on DBus signals + 50ms poll; `test_surface_build.c` verifies single texture reuse | — |
| R-11-reorder | §11 | verified | `ip_manager_set_gamepad_order` calls DBus setter; GUI does not manage suspend/resume (InputPlumber handles 100ms stagger); `test_gamepad_order.c` verifies setter | — |
| BUG-0004 | §11.2.6 | partial | Nix gating fixed in `verify-project.sh:9-16`; `dbus_mock.c:9` has `_POSIX_C_SOURCE 200809L`. **Bug still open; other test files lack POSIX defines; remote runner gate unverified** | Task 1 |
| R-5.4c | §5.4 | verified | Profiles map against virtual device capabilities: `profile_editor_list.c` loads capabilities via `ip_composite_get_target_capabilities`; `profile_diagram.c` renders virtual-device button layout; `test_profile_diagram.c` verifies diagram from capabilities | — |
| R-9.1a | §9.1 | verified | `test_flatpak_manifest.py` validates manifest; README does not contain `flatpak install flathub` command; manifest marked experimental in comments | — |

## Interaction acceptance inventory

The inventory at `tests/interaction_inventory.c` enumerates 58 entries: M01–M38 (manager), O01–O12 (overlay), D01–D08 (disabled/degraded). Current verification status: 41 UNVERIFIED, 16 NOT_APPLICABLE (controller-only pointer path), 1 DEFERRED (O12 host profile cycle, per §13).

| Control | Controller path | Pointer path | Semantic outcome verified | Status |
|---|---|---|---|---|
| M01–M03 (tab bar) | SDL_KEYDOWN Left/Right → A | mouse click on tab rect | active tab switches | partial (keyboard, not controller button) |
| M04–M09 (controllers) | SDL_KEYDOWN through dispatch | mouse click | mode changes, DBus calls, device count changes | partial (keyboard) — **M06/M07 outcomes swapped in inventory: M06 should be Remove (StopTargetDevice), M07 should be Change Type** |
| M10–M20 (profiles) | SDL_KEYDOWN through dispatch | mouse click where available | file create/delete, editor open, list refresh | partial (keyboard) |
| M21–M27 (settings) | SDL_KEYDOWN through dispatch | mouse click where available | value toggle/edit, settings.yaml write | partial (keyboard) |
| M28–M38 (editor) | SDL_KEYDOWN through dispatch | mouse click where available | binding edit, capture, sequential, save/discard | partial (keyboard; InputEvent via direct callback) — **explicit Save/Discard buttons exist in `profiles_tab.c` but not in inventory** |
| O01–O10 (overlay) | SDL keydown / DBus InputEvent through `cbx_overlay_service_step` | n/a (controller-only) | lifecycle transitions, grid moves, profile cycles, host toggle, conflict resolve, close | partial (keyboard for SDL path) |
| O11 (multi-controller) | DBus InputEvent through `ip_input_events_process` → `cbx_overlay_input_cb` | n/a | independent row movement | verified |
| O12 (host profile cycle) | deferred per §13 (UX details only; §4.4 states host can "edit slot/profile" as v1 capability) | n/a | n/a | deferred |
| D01–D08 (disabled) | SDL_KEYDOWN through dispatch | n/a | activation rejected, no side effect | partial (keyboard) |

**Gaps to close:** (1) Controller-path tests must send `SDL_CONTROLLERBUTTONDOWN` events, not `SDL_KEYDOWN` with controller windowID (Task 7). (2) Inventory `verify_status` must reflect actual test evidence (Task 11). (3) Explicit Save/Discard editor buttons need inventory entries (Task 11). (4) M06/M07 outcomes must be corrected in `interaction_inventory.c` (Task 7 — same task that rewrites interaction tests). (5) Pointer-path NA entries for visible controls (save/discard buttons, confirm-delete buttons) must be reviewed and either given pointer paths or justified as keyboard-only modal (Task 11). §5.1 requires every visible enabled control to respond to pointer; true keyboard-only modal modes (name input text field, edit-mode value cycling) may remain NA if they have no clickable buttons.

## Task 1: Resolve BUG-0004 — POSIX declarations and remote gate
- Status: pending
- Dependencies: none
- Scope: `tests/*.c` POSIX feature-test macros; `scripts/verify-project.sh`; `.factory/bugs/open.md` → `.factory/bugs/closed.md`
- Acceptance criteria:
  - All test files using `strdup`/`setenv`/`popen`/`realpath`/`mkstemp` define `_POSIX_C_SOURCE 200809L` (or equivalent) before system headers, enabling strict-C11 compilation without implicit declarations.
  - `nix-shell --run './scripts/verify-project.sh'` passes on a clean checkout.
  - If the declared runner (`dev-runner-vm`) is accessible, `scripts/run-factory-runners.py` + `scripts/check-factory-runner-evidence.py` accept exact-commit evidence and the remote gate passes.
  - If the runner is inaccessible (network/SSH failure), the bug **cannot be closed** — it remains open with a blocker note, and a separate task is created to verify the remote gate when the runner becomes available. The bug's own acceptance criteria require the remote gate to pass.
  - BUG-0004 is moved from `open.md` to `closed.md` with resolution evidence.
- Verification: `nix-shell --run 'cmake -S . -B build-check -DCMAKE_BUILD_TYPE=Debug && cmake --build build-check --parallel && ctest --test-dir build-check --output-on-failure'`
- Documentation impact: Note BUG-0004 closure in `.factory/bugs/closed.md`

## Task 2: Fix sd_set_property boolean variant and add ManageAllDevices wrapper
- Status: pending
- Dependencies: none
- Scope: `src/dbus/dbus_client.c` (`sd_set_property`), `src/dbus/ip_manager.c/.h`, `tests/test_native_dbus.c`, `tests/test_dbus_signatures.c`
- Acceptance criteria:
  - `sd_set_property` sends `v<b>` (boolean variant) for properties where `sd_is_bool_property` returns true, not `v<s>`.
  - `ip_manager_get_manage_all_devices` and `ip_manager_set_manage_all_devices` wrappers exist and call `get_property`/`set_property` with the correct signature.
  - `test_native_dbus.c` verifies setting `Enabled` or `ManageAllDevices` against the real sd-bus server and reads back the boolean value.
  - `test_dbus_signatures.c` verifies `ip_dbus_property_signature("ManageAllDevices")` returns `"b"`.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'test_native_dbus|test_dbus_signatures' --output-on-failure"`
- Documentation impact: None

## Task 3: Add operational readiness property validation
- Status: pending
- Dependencies: Task 2
- Scope: `src/dbus/ip_connection.c`, `src/dbus/ip_objectmanager.c`, `tests/test_connection.c`, `tests/test_native_dbus.c`
- Acceptance criteria:
  - After `cbx_objectmanager_enumerate` succeeds, the connection validates that at least one required typed property (e.g., `Version` already checked; additionally `InterceptMode` on at least one composite, `TargetDevices` on at least one composite, `DeviceType` on at least one target) is readable. If all fail, the connection enters degraded state with a specific reason.
  - `test_native_dbus.c` verifies that operational readiness passes when properties are readable and degrades when they are not.
  - The validation step does not block enumeration-only use cases (e.g., device listing before assignment).
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'test_native_dbus|test_connection' --output-on-failure"`
- Documentation impact: None

## Task 4: Implement Settings tab icon overrides
- Status: pending
- Dependencies: none
- Scope: `src/manager/settings_tab.c`, `src/manager/settings_tab.h`, `tests/test_settings_tab.c`, `tests/test_manager_interaction_ctrl.c`, `tests/test_manager_visual.c`, `tests/interaction_inventory.c`
- Acceptance criteria:
  - A new settings row for "Controller icon overrides" is visible and interactive, wired to `cbx_settings_icon_override`/`cbx_settings_set_icon_override`/`cbx_settings_remove_icon_override` (already in `config_settings.c`).
  - The row allows selecting a device type and choosing an icon override (built-in name or absolute path).
  - Controller-path activation (via `SDL_CONTROLLERBUTTONDOWN`) and pointer-path activation both produce the same semantic outcome: `settings.yaml` is updated with the icon override.
  - Visual test (`test_manager_visual.c`) asserts the icon-override row contains non-background framebuffer content.
  - Interaction inventory is updated with new entries for the icon-override controls.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'test_settings_tab|test_manager_interaction_ctrl|test_manager_visual' --output-on-failure"`
- Documentation impact: Settings tab description in README

## Task 5: Implement unsaved-changes prompt on window close
- Status: pending
- Dependencies: Task 7
- Scope: `src/manager/manager.c`, `src/manager/manager.h`, `tests/test_manager_production.c`, `tests/test_manager_interaction_prof.c`
- Acceptance criteria:
  - When the profile editor has unsaved changes and an `SDL_QUIT` event arrives, the manager shows a confirmation dialog ("Save / Discard / Cancel") instead of silently discarding.
  - "Cancel" returns to the editor without quitting. "Discard" quits without saving. "Save" saves via `cbx_profile_save_to_dir` (with NES validation) and then quits; if save fails, the editor stays open.
  - When there are no unsaved changes, `SDL_QUIT` quits immediately without a prompt.
  - The confirmation dialog is navigable by both controller (A/D-pad/B) and pointer (click on buttons).
  - Test sends `SDL_QUIT` through the production event loop and verifies the dialog appears, then verifies each branch.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'test_manager_production|test_manager_interaction_prof' --output-on-failure"`
- Documentation impact: Profiles tab section in README — note unsaved-changes behavior

## Task 6: Render Host Mode visual states
- Status: pending
- Dependencies: none
- Scope: `src/overlay/grid_render.c`, `src/overlay/grid_render.h`, `tests/test_overlay_visual.c`, `tests/test_golden.c`, `tests/golden/`
- Acceptance criteria:
  - `grid_render.c` uses `cbx_host_mode_row_state` to visually differentiate row states: HOST (host's own row — highlighted), SELECTED (row the host is navigating to — highlighted differently), FROZEN (non-host rows in host mode — dimmed). Normal rows in Player Mode are unchanged.
  - The visual differentiation is meaningful (different colors or brightness) and produces materially different pixels from Player Mode.
  - `test_overlay_visual.c` asserts that a host-mode frame differs from a player-mode frame at the same grid state (without relying on conflict introduction to make them differ).
  - A golden baseline `tests/golden/overlay_host_mode.png` is regenerated or updated to reflect the new rendering (reviewed, not auto-generated).
  - `test_golden.c` overlay_host_mode comparison passes.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'test_overlay_visual|test_golden' --output-on-failure"`
- Documentation impact: None (host mode is a runtime behavior)

## Task 7: Fix interaction tests to use SDL_CONTROLLERBUTTONDOWN
- Status: pending
- Dependencies: none
- Scope: `tests/test_manager_interaction_ctrl.c`, `tests/test_manager_interaction_prof.c`, `tests/test_overlay_interaction.c`, `tests/test_harness.h`, `tests/test_harness.c`
- Acceptance criteria:
  - All "controller path" tests in `test_manager_interaction_ctrl.c` and `test_manager_interaction_prof.c` send `SDL_CONTROLLERBUTTONDOWN`/`UP` events (or virtual joystick button events) instead of `SDL_KEYDOWN` with `CBX_CONTROLLER_EVENT_WINDOW_ID`. The events flow through `cbx_manager_handle_event` → `cbx_manager_controller_to_key` → normal dispatch.
  - A test harness helper `send_controller_button(mgr, SDL_GameControllerButton btn)` is added to `test_harness.h`/`.c` for reuse.
  - Keyboard-path tests remain as a separate "supplemental accessibility" set, clearly labeled, not mixed with controller acceptance.
  - Overlay interaction tests (`test_overlay_interaction.c`) that use SDL keydown for the "controller" path are updated to use `SDL_CONTROLLERBUTTONDOWN` where the overlay step function processes controller events.
  - Fix M06/M07 swapped outcomes in `interaction_inventory.c`: M06 must be Remove (StopTargetDevice, device count decreases), M07 must be Change Type (type picker opens).
  - All semantic outcome assertions remain unchanged and pass.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'test_manager_interaction|test_overlay_interaction' --output-on-failure"`
- Documentation impact: None

## Task 8: Add performance/latency tests
- Status: pending
- Dependencies: Task 6
- Scope: `tests/test_overlay_lifecycle.c` or new `tests/test_overlay_latency.c`, `tests/CMakeLists.txt`
- Acceptance criteria:
  - A test measures button-to-first-visible-frame latency: records `SDL_GetTicks()` before simulating InterceptMode→ALL, calls `cbx_overlay_service_step` until lifecycle reaches VISIBLE, records elapsed time. Asserts ≤100ms maximum on the test hardware (software renderer, dummy driver). The p99 assertion is documented as a target, not a hard CI gate (hardware-dependent).
  - A test measures ALL-detection-to-present latency: records time between `ip_intercept_poll_tick` detecting ALL and `cbx_overlay_surface_show` completing. Asserts <10ms (this is pure CPU work — pre-built surface, single RenderCopy).
  - A test measures close latency: records time between `cbx_overlay_lifecycle_close` and InterceptMode set to PASS. Asserts <1ms (single DBus property set).
  - Tests are deterministic and reproducible in the Nix/headless environment using the software renderer.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'overlay_latency|overlay_lifecycle' --output-on-failure"`
- Documentation impact: Note performance test methodology in `docs/OPERATIONS.md`

## Task 9: Fix installed functional test — run overlay service
- Status: pending
- Dependencies: Task 6
- Scope: `tests/test_installed_functional.c`, `tests/CMakeLists.txt`
- Acceptance criteria:
  - The test calls `cbx_overlay_service_init` with the production sd-bus backend against the private bus, initializes the overlay surface, and registers triggers.
  - The test simulates InterceptMode→ALL on the server, calls `cbx_overlay_service_step` in a loop until the overlay lifecycle reaches VISIBLE, and verifies pixel output (non-background content in grid cell regions) via `fb_read_pixels`.
  - The test simulates InterceptMode→PASS, calls `cbx_overlay_service_step` until the lifecycle returns to IDLE, and verifies the surface is hidden.
  - The test verifies assignment persistence (LoadProfilePath + GamepadOrder) through the production `cbx_overlay_on_save` callback, not direct DBus calls.
  - The test verifies persistence after backend restart: server killed + restarted, overlay service reconnected and re-enumerated.
  - No SKIP_RETURN_CODE — missing prerequisites are failure.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'test_installed_functional' --output-on-failure"`
- Documentation impact: None

## Task 10: Fix font/icon visual tests and weakened assertions
- Status: pending
- Dependencies: none
- Scope: `tests/test_overlay_visual.c`, `tests/test_manager_visual.c`, `tests/test_golden.c`, `tests/test_backend_smoke.c`, `tests/test_installed_smoke.sh`, `tests/fb_assert.c`
- Acceptance criteria:
  - Font-dependent visual tests (`test_overlay_visual.c:655,720`, `test_manager_visual.c:825`, `test_golden.c:809`) fail with a clear assertion message when the font is unavailable, instead of `skip()`. The test harness must find a font in the declared Nix environment; if none is found, that is a test infrastructure failure, not a pass.
  - `test_backend_smoke.c` fails (not silently skips) golden comparison when a golden baseline is missing. The `[SKIP]` messages at lines 272–284 and 345–355 are replaced with `fail_msg`.
  - `test_installed_smoke.sh` does not accept arbitrary non-zero overlay exit codes as "not a crash" (lines 479–483). Only exit 0 (clean) or the documented expected exit codes are accepted.
  - `test_installed_smoke.sh` overlay screenshot capture does not pass-by-default when capture fails (lines 461–463). If the overlay should be visible, a capture failure is a test failure.
  - Failure artifacts (`fb_save_png`/`fb_save_diff`) write a sidecar `.txt` with renderer name and backend info from `SDL_GetRendererInfo`.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'test_overlay_visual|test_manager_visual|test_golden|test_backend_smoke|test_installed_smoke' --output-on-failure"`
- Documentation impact: Note test infrastructure changes in `docs/OPERATIONS.md`

## Task 11: Update interaction inventory verification status
- Status: pending
- Dependencies: Tasks 7, 4, 5, 6
- Scope: `tests/interaction_inventory.c`, `tests/test_interaction_inventory.c`
- Acceptance criteria:
    - All entries with passing tests have `verify_status` updated from `CBX_VERIFY_UNVERIFIED` to `CBX_VERIFY_VERIFIED`.
  - New entries are added for the explicit editor Save button and Discard button (both controller and pointer paths).
  - New entries are added for the Settings tab icon-override controls (from Task 4).
  - New entries are added for the unsaved-changes dialog buttons (Save/Discard/Cancel) from Task 5.
  - All pointer-path `CBX_PATH_NA` entries are reviewed: entries for visible clickable controls (save/discard buttons, confirm-delete buttons) are given pointer paths; true keyboard-only modal modes (name input text field, edit-mode value cycling) remain NA with justification.
  - `test_interaction_inventory.c` asserts that all non-deferred entries have `verify_status == CBX_VERIFY_VERIFIED` and that the total count matches the new entries.
  - O12 remains `CBX_VERIFY_DEFERRED`.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'test_interaction_inventory' --output-on-failure"`
- Documentation impact: None

## Task 12: Final documentation and specification audit
- Status: pending
- Dependencies: Tasks 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
- Scope: `docs/SPEC.md` §11.2, `.factory/artifacts/implementation-plan.md`, `README.md`, `docs/OPERATIONS.md`, `.factory/bugs/open.md`, `.factory/bugs/closed.md`
- Acceptance criteria:
  - **Conformance matrix:** Every row in the conformance matrix above is classified `verified` with specific source evidence and an executable test or acceptance command. No row remains `partial`, `missing`, or `ambiguous`.
  - **Interaction inventory:** All non-deferred entries have passing controller and pointer evidence. The inventory is exhaustive per §§4, 5.7, and 11.2.
  - **Bug ledger:** No open bug contradicts a v1 requirement. BUG-0004 is closed with evidence.
  - **Independent review:** Read-only correctness, test-quality, security, and documentation reviews find no unresolved blocking issue. Reviews challenge whether tests can pass while production behavior remains broken.
  - **Full clean verification:** `nix-shell --run './scripts/verify-project.sh'` passes from a clean checkout with no unexplained skips, flaky rerun dependencies, or weakened assertions.
  - **Documentation:** `README.md` and `docs/OPERATIONS.md` match observed behavior; build, install, acceptance, and recovery commands work from a clean checkout.
  - **Git state:** Clean tree on `develop`; all tasks complete with evidence.
- Verification: `nix-shell --run './scripts/verify-project.sh'` + `./scripts/final-gate.sh --implementation`
- Documentation impact: Final README and OPERATIONS updates; front-matter `status` changes from `active` to `complete`

## Remediation rule

When the final audit (Task 12) finds a gap:
1. Preserve the existing task ledger — do not delete or rewrite completed tasks.
2. Append a uniquely numbered pending task (Task 13, 14, ...) describing the gap with scope, acceptance criteria, and verification.
3. Add the new task to Task 12's dependency list.
4. Set Task 12 status back to `pending`.
5. Continue the implementation loop until the new task passes and Task 12 re-audits.

Reaching an iteration, runtime, or session ceiling leaves the cycle `active` or `blocked` with a recovery handoff; reaching a ceiling is never success.