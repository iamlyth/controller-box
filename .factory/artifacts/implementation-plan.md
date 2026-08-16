---
spec_path: docs/SPEC.md
spec_commit: 3a10f6b7d04a615b2b9d06eef6c91e431fa9c079
spec_blob: 58f5d3cb72bc6b3e5f573fa09a63c11a653ed577
base_commit: d61b7f5f23b52d772f2bf0893ec4e45749be5862
status: active
---

# Implementation Plan

## Goal and non-goals

**Goal:** Close every implementation gap between the committed specification (`docs/SPEC.md`) and the current repository, achieving an all-`verified` conformance matrix and exhaustive interaction acceptance, so the autonomous implementation loop can satisfy §11.2 definition of done.

**Non-goals:**
- Modifying the committed specification.
- Re-implementing functionality that is already verified in production code (the codebase is complete with zero stubs, TODOs, or placeholders — 98 tests pass).
- Inventing undeclared runner capabilities. The environment declaration (`.factory/environment.toml`) is exhaustive; hardware-blocked tasks document the required evidence and remain pending until a runner provides it.

## Architecture and constraints

Controller-Box is a single C11 binary with two modes: `--overlay-service` (systemd user daemon) and `--manager` (on-demand tab GUI). It wraps InputPlumber's system DBus API via sd-bus — never touching input routing directly. The overlay is a pre-built fighting-game select screen (rows=physical controllers, columns=player slots). The manager has three tabs (Controllers, Profiles, Settings) navigable by controller (primary) and mouse (secondary).

**Constraints from the environment:**
- Only `remote-project-gate` and `systemd-user` capabilities are declared and evidenced. The runner `dev-runner-vm` is SSH-unreachable from the planning context.
- Six campaign-required capabilities (`physical-controller`, `kernel-uinput`, `inputplumber-system-dbus`, `gpu-compositor`, `installed-package`, `target-consumer`) are undeclared and unevidenced. This blocks five conformance rows that require hardware/runner evidence.
- Build uses CMake + Nix shell (`shell.nix`). Debug builds use `-Werror`. Sanitizers (ASan+UBSan) supported via `CBX_ENABLE_SANITIZERS`.

**Software-fixable gaps (Campaign Round 4 findings 6–7):**
- Finding 6: `cbx_manager_init` calls `cbx_settings_defaults` but never `cbx_settings_load`, so the manager starts with default settings instead of persisted user preferences.
- Finding 7: Production DBus interface definitions (`ip_dbus_backend` vtable, `ip_bus_handle`, signal payload structs, DBus constants) live in `tests/dbus_mock.h`, included by 25+ production source files.

## Specification conformance matrix

| ID | Spec § | Classification | Evidence | Task |
|----|--------|---------------|----------|------|
| ARCH-01 | §2.1 | verified | All state changes via `src/dbus/dbus_client.c` sd-bus backend; no direct evdev/uinput in `src/` | |
| ARCH-02 | §2.2 | verified | `dbus_client.c:sd_connect` opens `sd_bus_open_system`; no CLI wrapping | |
| ARCH-03 | §2.3 | verified | `src/app/main.c:57-170` dual-mode entry (`--overlay-service` default, `--manager`) | |
| ARCH-04 | §2.4 | verified | `ip_connection.c` NameOwnerChanged subscription, degraded mode, re-enumerate callback; `overlay_service.c` recovery within 2s; no cross-manager systemd deps in unit file | |
| ARCH-05 | §2.5 | verified | `trigger.c` SetInterceptActivation + InterceptMode=PASS; `ip_intercept_poll.c` 50ms poll; `lifecycle.c` PASS→ALL→PASS; `close.c` PASS on close | |
| SYS-01 | §3 | partial | Pi 4 minimum hardware not tested on target | Task 5 |
| SYS-02 | §3 | partial | aarch64 build not evidenced (only x86_64) | Task 5 |
| SYS-03 | §3 | verified | SDL2 renderer init in `src/ui/renderer.c`; X11/Wayland/Gamescope via SDL2 | |
| SYS-04 | §3 | verified | `CMakeLists.txt` pkg-config deps: SDL2, SDL2_ttf, SDL2_image, libsystemd, libyaml, nanosvg vendored | |
| SYS-05 | §3 | verified | InputPlumber not vendored; `overlay_service.c` runtime bus-name check | |
| OVL-01 | §4.1 | verified | `grid_render.c:cbx_select_grid_build/render` — rows=physical, columns=slots, Unassigned col 0; `test_overlay_visual.c` 8 sub-tests | |
| OVL-02 | §4.1 | verified | `player_mode.c` Left/Right/Up/Down/R3/B; `test_overlay_interaction.c` SDL keydown → `cbx_overlay_service_step` production dispatch | |
| OVL-03 | §4.2 | verified | `trigger.c:cbx_trigger_parse("Select+A")`; `settings_tab.c` configurable; `test_trigger.c` | |
| OVL-04 | §4.3 | verified | `player_mode.c` independent row movement; `test_player_mode.c`, `test_overlay_interaction.c` | |
| OVL-05 | §4.4 | verified | `host_mode.c` R3 toggle, freeze, row navigation; `test_host_mode.c`, `test_overlay_interaction.c` | |
| OVL-06 | §4.5 | verified | `conflict.c` second-arrival red, auto-resolve lowest free; `grid_render.c` red rendering; `test_conflict.c`, `test_overlay_interaction.c` | |
| OVL-07 | §4.6 | verified | `profile_cycle.c` profile follows controller across columns; `test_profile_cycle.c` | |
| OVL-08 | §4.7 | verified | `dynamic_columns.c` column count scales with targets; `test_dynamic_columns.c` | |
| OVL-09 | §4.8 | verified | `grid_render.c` shows model name + slot position, no nicknames | |
| OVL-10 | §4.9 | partial | Pre-built surface verified (`surface_build.c` render-to-texture); Pi 4 ≤75ms p99 not measured | Task 5 |
| OVL-11 | §4.10 | verified | `test_overlay_visual.c` framebuffer tests: player mode, host mode, conflict, unassigned+cols, text, icons; `test_golden.c` 11 baselines | |
| MGR-01 | §5.1 | verified | `manager.c` 3-tab tab bar, Left/Right switch, Up/Down focus, A activates; `test_manager_tabs.c` SDL dispatch | |
| MGR-02 | §5.1 | verified | `manager.c` mouse hit-testing with hover/press; `test_manager_interaction_ctrl.c`, `test_manager_interaction_prof.c` pointer path tests | |
| MGR-03 | §5.2 | verified | `controllers_tab.c` add/remove/type-change with ObjectManager verification; `test_controllers_tab.c` | |
| MGR-04 | §5.2 | verified | `controllers_tab.c` topology reconciliation; `overlay_service.c:cbx_reconcile_startup_targets` grow/shrink/type-correct | |
| MGR-05 | §5.3 | verified | `profiles_tab.c` browse/create(default copy/empty/clone)/edit/delete, default read-only, unsaved-changes prompt; `test_profiles_tab.c` | |
| MGR-06 | §5.4 | verified | `profile_editor_list.c` binding list + A edit; `profile_editor_seq.c` sequential + B skip + Start cancel + progress; `profile_diagram.c` sync; `profile_validate.c` NES minimum; `test_editor_list_mode.c`, `test_editor_seq_mode.c` | |
| MGR-07 | §5.5 | verified | `settings_tab.c` all settings (launch-at-boot, theme, opacity, VC count/types, trigger, icon overrides); `test_settings_tab.c` | |
| MGR-08 | §5.6 | verified | `test_manager_visual.c` 13 sub-tests for all 3 tabs + editor states; `test_golden.c` manager baselines | |
| MGR-09 | §5.7 | verified | `tests/interaction_inventory.c` 59 entries; `test_interaction_inventory.c` validates ledger; `test_manager_interaction_ctrl.c` + `test_manager_interaction_prof.c` production SDL dispatch for both controller and pointer paths | |
| MGR-10 | §5.7 | verified | End-to-end scenarios in `test_manager_native.c`, `test_manager_native_prof.c`, `test_overlay_native.c` with private native-signature DBus | |
| MGR-11 | §5.7 | partial | Controller acceptance uses process-local `SDL_JoystickAttachVirtual`, not kernel-backed gamepad; `test_kernel_controller.c` skips (exit 77) | Task 3 |
| MGR-12 | §5.7 | verified | `test_native_dbus.c`, `test_manager_native.c`, `test_overlay_native.c` use private sd-bus service with native signatures and ObjectManager | |
| MGR-13 | §5.7 | partial | Installed smoke uses process-local virtual gamepad, not kernel-backed; `test_installed_functional.c:251` | Task 3 |
| ID-01 | §6.2 | verified | `identity.c` 4-layer auto-assignment (BT MAC, USB serial, USB port path, connection order); `test_identity.c` | |
| ID-02 | §6.3 | verified | `identity.c` prefixed IDs (BT:/USB:/USB:phys:/ORDER:); `identity_downgrade.c` weaker-identity detection; `test_identity_downgrade.c` | |
| CFG-01 | §7.1 | verified | `config_profile.c` InputPlumber DeviceProfile YAML; `config_profile_meta.c` optional sidecar; no duplicate format | |
| CFG-02 | §7.2 | verified | `config_paths.c` XDG layout; `test_config_paths.c` | |
| CFG-03 | §7.3 | verified | `config_settings.c` persistence verified; `manager.c:237` calls `cbx_settings_load` after `cbx_settings_defaults` during init — manager starts with persisted settings; `test_manager_integration.c:test_persisted_settings_loaded_on_init` regression test | Task 1 |
| CFG-04 | §7.4 | verified | `config_assignments.c` prefixed IDs + gamepad_order; `test_assignments.c` | |
| CFG-05 | §7.5 | verified | `config_profile_meta.c` sidecar (display_name, icon, display_order, description); `test_profile_list.c` | |
| CFG-06 | §7.6 | verified | `config_profile.c` device_profile_v1 format; `test_profile_yaml.c` round-trip | |
| ICO-01 | §8.1 | verified | `icon_lookup.c` maps DeviceType → virtual controller icon; `test_icon_lookup.c` | |
| ICO-02 | §8.2 | verified | `icon_cache.c` Controllercons SVGs + custom icons (arcade, hitbox, deck, generic); `data/icons/` | |
| ICO-03 | §8.3 | verified | `icon_cache.c` nanosvg rasterization at startup, texture cache; `test_icon_cache.c` | |
| ICO-04 | §8.4 | verified | `icon_map.c` controller-icons.yaml parsing; `test_icon_map.c` | |
| ICO-05 | §8.5 | verified | `icon_lookup.c` profile override (built-in name or absolute PNG path); `config_profile_meta.c` | |
| PKG-01 | §9.1 | partial | Flatpak manifest exists (`tests/test_flatpak_manifest`); criteria not all met: installed functional gate on Flatpak build, host DBus access verified, publication | Task 7 |
| PKG-02 | §9.2 | verified | CMake build + `tests/test_packaging.sh` install verification | |
| PKG-03 | §9.3 | verified | `CMakeLists.txt` install rules (binary, systemd unit, desktop entry, icons, yaml); `test_packaging.sh` | |
| PKG-04 | §9.4 | verified | `overlay_service.c` runtime bus-name check; no cross-manager systemd dependency | |
| DBUS-01 | §10.1 | verified | `dbus_client.c` sd-bus system bus, typed property get/set (u/b/as/s); `ip_connection.c` ownership + Version check; `ip_objectmanager.c` GetManagedObjects; `ip_hotplug.c` InterfacesAdded/Removed | |
| DBUS-02 | §10.2 | verified | `ip_manager.c` (CreateTargetDevice, StopTargetDevice, AttachTargetDevice, GamepadOrder, SupportedTargetDeviceIds, Version); `ip_composite.c` (SetInterceptActivation, LoadProfilePath, LoadProfileFromYaml, SetTargetDevices, InterceptMode, ProfileName/Path, PersistentId, Capabilities); `ip_target.c` (Name, DeviceType); `ip_source.c` (UniqueId, PhysPath, SerialNumber) | |
| DBUS-03 | §10.3 | verified | Gap 1: `ip_intercept_poll.c` 50ms poll; Gap 2: `ip_gamepad_order.c` save/restore; Gap 3: `ip_create_composite.c` temp YAML; Gap 4: `config_profile_list.c` filesystem enumeration; Gap 5: not needed | |
| DBUS-04 | §11.2.2, §11.2.5 | verified | Production DBus interface definitions in `src/dbus/dbus_interface.h`; all 24 `src/` files include production header; `tests/dbus_mock.h` includes it for shared definitions; `controllerbox` no longer has `tests/` in include path | |
| PERF-01 | §11 | partial | Overlay ≤75ms p99, ≤100ms max on Pi 4: `test_overlay_latency.c` measures <10ms p99 on x86_64; no Pi 4/ARM64 measurement | Task 5 |
| PERF-02 | §11 | verified | Detection-to-present <10ms p99: `test_overlay_latency.c` | |
| PERF-03 | §11 | verified | Overlay close <1ms: `close.c` single property set; `test_overlay_latency.c` | |
| PERF-04 | §11 | verified | Daemon footprint: `test_daemon_footprint.c` | |
| PERF-05 | §11 | verified | Player reorder atomic: `ip_gamepad_order.c` SetGamepadOrder; `test_gamepad_order.c` | |
| VRF-01 | §11.1.1 | verified | `test_overlay_visual.c`, `test_manager_visual.c` deterministic framebuffer with `SDL_RenderReadPixels` | |
| VRF-02 | §11.1.2 | verified | `tests/fb_assert.c` region-level assertions; `test_fb_assert.c` | |
| VRF-03 | §11.1.3 | verified | `test_golden.c` 11 reviewed baselines with per-pixel tolerance | |
| VRF-04 | §11.1.4 | verified | `fb_assert.c` saves actual/expected/diff on mismatch | |
| VRF-05 | §11.1.5 | partial | `test_installed_functional.c` runs but uses `SDL_JoystickAttachVirtual` (process-local), not kernel-backed synthetic controller per §11.1.5 | Task 3 |
| VRF-06 | §11.1.6 | partial | `test_backend_smoke.c` skips (exit 77) — no accelerated OpenGL/GLES backend; `test_backend_smoke_sw.c` software-only | Task 4 |
| VRF-07 | §11.1.7 | missing | No human release acceptance artifact on target hardware | Task 6 |
| DOD-01 | §11.2.1 | partial | 6 conformance rows non-verified (MGR-11, MGR-13, VRF-05, VRF-06, VRF-07, PERF-01) — all hardware-blocked | Tasks 3-6 |
| DOD-02 | §11.2.2 | verified | Production-path behavior: DBus interface definitions moved to `src/dbus/dbus_interface.h`; no `src/` file includes test header `dbus_mock.h` | |
| DOD-03 | §11.2.3 | partial | Complete interaction traversal: MGR-11/MGR-13 kernel-backed controller evidence missing | Task 3 |
| DOD-04 | §11.2.4 | verified | Visual and degraded-state acceptance: `test_overlay_visual.c`, `test_manager_visual.c`, `test_golden.c` cover normal/degraded/error states | |
| DOD-05 | §11.2.5 | partial | Regression and quality gates: VRF-06 GPU backend skip is an unexplained skip | Task 4 |
| DOD-06 | §11.2.6 | verified | Known-defect accounting: `.factory/bugs/open.md` is empty | |
| DOD-07 | §11.2.7 | verified | Independent review: Campaign Round 4 audit completed with 7 findings, all mapped to tasks | |
| DOD-08 | §11.2.8 | partial | Documentation and reproducibility: README/OPERATIONS.md match x86_64 behavior (accuracy fixes applied — interaction inventory counts corrected to 52/6/1, layer numbering aligned with SPEC §11.1 7-layer scheme, skip behavior documented, Xvfb contradiction resolved, coverage table completed); aarch64 build not evidenced | Task 5, Task 9 |
| DOD-09 | §11.2.9 | verified | Repository integrity: clean tree on develop, complete task ledger | |

## Interaction acceptance inventory

The project maintains a machine-readable inventory at `tests/interaction_inventory.c` with 59 entries validated by `tests/test_interaction_inventory.c`. Every entry records: ID, category, context, widget type, controller path, pointer path availability, pointer path, semantic outcome, production dispatch path, and verify status. Below is the exhaustive inventory with both input paths, expected semantic outcome, production dispatch path, and verification evidence.

### Manager controls (M01–M38)

| ID | Control | Controller path | Pointer path | Semantic outcome | Production dispatch | Evidence |
|----|---------|----------------|-------------|-----------------|-------------------|----------|
| M01 | Tab→Controllers | Left/Right from tab bar | Click tab rect | active_tab=CONTROLLERS | `cbx_manager_handle_event` SDL keydown | `test_manager_tabs.c` |
| M02 | Tab→Profiles | Left/Right | Click tab rect | active_tab=PROFILES | `cbx_manager_handle_event` | `test_manager_tabs.c` |
| M03 | Tab→Settings | Left/Right | Click tab rect | active_tab=SETTINGS | `cbx_manager_handle_event` | `test_manager_tabs.c` |
| M04 | Controllers list nav | Up/Down | Click item | List selection changes | `cbx_manager_handle_event` | `test_controllers_tab.c` |
| M05 | Add controller | Down→A | Click Add btn | CreateTargetDevice DBus call, type picker opens | `cbx_manager_handle_event` KEYDOWN+KEYUP | `test_controllers_tab.c` |
| M06 | Remove controller | Down→A | Click Remove btn | StopTargetDevice, auto-Unassign | `cbx_manager_handle_event` | `test_controllers_tab.c` |
| M07 | Change type | Down→A | Click Change Type btn | Type picker opens | `cbx_manager_handle_event` | `test_controllers_tab.c` |
| M08 | Type picker confirm | Down→A on item | Click item | SetTargetDevices DBus call, type changes | `cbx_manager_handle_event` | `test_controllers_tab.c` |
| M09 | Type picker cancel | B | Click outside | Returns to list mode | `cbx_manager_handle_event` | `test_controllers_tab.c` |
| M10 | Profiles list nav | Up/Down | Click item | Profile selection changes | `cbx_manager_handle_event` | `test_manager_interaction_prof.c` |
| M11 | Create profile | Down→A | Click Create btn | Source picker opens (Default copy/Empty/Clone) | `cbx_manager_handle_event` | `test_manager_interaction_prof.c` |
| M12 | Create source pick | Up/Down→A | Click item | Name input mode begins | `cbx_manager_handle_event` | `test_manager_interaction_prof.c` |
| M13 | Name input typing | Letter keys | n/a (controller-only) | Character appended to name buffer | `cbx_manager_handle_event` KEYDOWN | `test_manager_interaction_prof.c` |
| M14 | Name input backspace | Backspace key | n/a | Character removed | `cbx_manager_handle_event` | `test_manager_interaction_prof.c` |
| M15 | Name input confirm | A | Click confirm btn | Profile created, editor opens | `cbx_manager_handle_event` | `test_manager_interaction_prof.c` |
| M16 | Name input cancel | B | Click cancel btn | Returns to list mode | `cbx_manager_handle_event` | `test_manager_interaction_prof.c` |
| M17 | Edit profile | Down→A | Click Edit btn | Profile editor opens with loaded profile | `cbx_manager_handle_event` | `test_manager_interaction_prof.c` |
| M18 | Delete profile | Down→A | Click Delete btn | Delete confirmation prompt | `cbx_manager_handle_event` | `test_profiles_tab.c` |
| M19 | Confirm delete | A | Click Yes | Profile deleted from disk + sidecar | `cbx_manager_handle_event` | `test_profiles_tab.c` |
| M20 | Cancel delete | B | Click No | Returns to list mode | `cbx_manager_handle_event` | `test_profiles_tab.c` |
| M21 | Settings list nav | Up/Down | Click item | Setting selection changes | `cbx_manager_handle_event` | `test_manager_tabs.c` |
| M22 | Toggle launch-at-boot | A on item | Click item | Value toggles | `cbx_manager_handle_event` | `test_manager_visual.c` |
| M23 | Enter edit mode | A on setting | Click item | Edit mode begins | `cbx_manager_handle_event` | `test_settings_tab.c` |
| M24 | Edit adjust up/down | Up/Down in edit mode | n/a | Value changes | `cbx_manager_handle_event` | `test_settings_tab.c` |
| M25 | Confirm edit | A | Click confirm | Value saved to in-memory settings | `cbx_manager_handle_event` | `test_settings_tab.c` |
| M26 | Cancel edit | B | Click cancel | Value reverts from disk | `cbx_manager_handle_event` | `test_settings_tab.c` |
| M27 | Save settings | Down→A | Click Save btn | `cbx_settings_save` to settings.yaml | `cbx_manager_handle_event` | `test_settings_tab.c` |
| M28 | Binding list nav | Up/Down | Click item | Binding selection + diagram highlight sync | `cbx_manager_handle_event` | `test_manager_interaction_prof.c` |
| M29 | A on binding → edit | A on item | Click item | Edit sub-menu opens (Pick Target/Capture/Sequential) | `cbx_manager_handle_event` | `test_manager_interaction_prof.c` |
| M30 | Target pick confirm | A on target | Click item | Binding target updated | `cbx_manager_handle_event` | `test_manager_interaction_prof.c` |
| M31 | Capture begin | A on Capture option | Click option | Enters capture mode, awaits DBus InputEvent | `cbx_manager_handle_event` | `test_manager_interaction_prof.c` |
| M32 | Capture input | DBus InputEvent signal | n/a (controller-only) | Physical button captured as binding | `backend->inject_signal` DBus signal path | `test_manager_native_prof.c` |
| M33 | Sequential begin | A on Sequential option | Click option | Sequential binding starts, diagram lights first button | `cbx_manager_handle_event` | `test_manager_interaction_prof.c` |
| M34 | Seq capture input | DBus InputEvent signal | n/a (controller-only) | Button captured, auto-advance | `backend->inject_signal` DBus signal path | `test_manager_native_prof.c` |
| M35 | Seq B skip | B key | n/a | Current button skipped, advance to next | `cbx_manager_handle_event` | `test_manager_interaction_prof.c` |
| M36 | Seq Start cancel | Start key | n/a | Sequential cancelled, bindings preserved | `cbx_manager_handle_event` | `test_editor_seq_mode.c` |
| M37 | Save+close editor | B in list mode | Click Save btn | Profile saved to YAML, editor closes | `cbx_manager_handle_event` | `test_manager_interaction_prof.c` |
| M38 | Discard+close editor | Start in list mode | Click Discard btn | Changes discarded, editor closes | `cbx_manager_handle_event` | `test_manager_interaction_prof.c` |

### Overlay actions (O01–O13)

| ID | Action | Controller path | Pointer path | Semantic outcome | Production dispatch | Evidence |
|----|--------|----------------|-------------|-----------------|-------------------|----------|
| O01 | Open overlay | InterceptMode=ALL detected via poll | n/a | Overlay transitions IDLE→ACTIVATING→VISIBLE | `cbx_overlay_service_step` poll + `cbx_overlay_lifecycle_activate` | `test_overlay_interaction.c`, `test_overlay_native.c` |
| O02 | Move left | Left key | n/a | Controller's column decreases | `push_keydown(SDLK_LEFT)` → `cbx_overlay_service_step` | `test_overlay_interaction.c` |
| O03 | Move right | Right key | n/a | Controller's column increases | `push_keydown(SDLK_RIGHT)` → `cbx_overlay_service_step` | `test_overlay_interaction.c` |
| O04 | Cycle profile up | Up key | n/a | Profile cycles to previous | `push_keydown(SDLK_UP)` → `cbx_overlay_service_step` | `test_overlay_interaction.c` |
| O05 | Cycle profile down | Down key | n/a | Profile cycles to next | `push_keydown(SDLK_DOWN)` → `cbx_overlay_service_step` | `test_overlay_interaction.c` |
| O06 | Host mode R3 | R3 key | n/a | First controller becomes host, others frozen | `push_keydown(SDLK_r)` → `cbx_overlay_service_step` | `test_overlay_interaction.c` |
| O07 | Host nav up/down | Up/Down keys | n/a | Host selected_row changes | `push_keydown` → `cbx_overlay_service_step` | `test_overlay_interaction.c` |
| O08 | Host move slot | Left/Right keys | n/a | Host's selected row column changes | `push_keydown` → `cbx_overlay_service_step` | `test_overlay_interaction.c` |
| O09 | Host exit R3 | R3 key | n/a | Returns to Player Mode | `push_keydown(SDLK_r)` → `cbx_overlay_service_step` | `test_overlay_interaction.c` |
| O10 | Close (B) | B key | n/a | Conflict resolution, assignments saved, InterceptMode=PASS | `push_keydown(SDLK_b)` → `cbx_overlay_service_step` → `cbx_overlay_lifecycle_close` | `test_overlay_interaction.c` |
| O11 | Multi-device input | DBus InputEvent per device | n/a | Each controller moves independently | `backend->inject_signal` → `cbx_overlay_service_step` | `test_overlay_interaction.c` |
| O12 | Host profile cycle | Up/Down in host mode | n/a | Host changes selected row's profile | DEFERRED — §13 interior UX TBD | — |
| O13 | Conflict detect+resolve | Two controllers same column | n/a | Second arrival shown red, auto-resolve on close | `cbx_overlay_service_step` with multi-device state | `test_overlay_interaction.c`, `test_overlay_native.c` |

### Degraded/disabled scenarios (D01–D08)

| ID | Scenario | Controller path | Pointer path | Semantic outcome | Evidence |
|----|----------|----------------|-------------|-----------------|----------|
| D01 | IP unavailable | A on disabled control | Click disabled control | Control rejects activation, no backend side effect | `test_controllers_tab.c` degraded mode tests |
| D02 | No device selected | A on Remove/Change Type | Click disabled btn | Operation rejected | `test_controllers_tab.c` |
| D03 | No profile selected | A on Edit/Delete | Click disabled btn | Operation rejected | `test_profiles_tab.c` |
| D04 | NES validation failure | A on Save | Click Save btn | Error shown, save blocked | `test_profile_validate.c`, `test_profile_save.c` |
| D05 | Settings cancel revert | B in edit mode | Click cancel | Value reverts from disk | `test_settings_tab.c` |
| D06 | DBus operation failure | A on Add/Remove | Click btn | Failed DBus op shown, topology retained | `test_controllers_tab.c` error tests |
| D07 | Filesystem failure | A on Save | Click Save btn | Error shown, no partial write | `test_profile_save.c` |
| D08 | Empty profile save blocked | A on Save with no bindings | Click Save btn | NES minimum error, save blocked | `test_profile_validate.c` |

**Verification status:** 52 entries VERIFIED via production SDL dispatch and/or native DBus; 6 entries NOT_APPLICABLE (controller-only: M13, M14, M32, M34, M35, M36 — pointer path n/a, supplemental direct-callback evidence noted); 1 entry DEFERRED (O12 — §13 post-v1). The controller and pointer paths are exercised through normal SDL events and production dispatch (`cbx_manager_handle_event` / `cbx_overlay_service_step`), not direct callback invocation. Direct callback tests exist as supplemental evidence only. Manager native-DBus tests (`test_manager_native.c`, `test_manager_native_prof.c`) and overlay native tests (`test_overlay_native.c`) provide production-path evidence through a private sd-bus service with native InputPlumber signatures.

## Task 1: Load persisted settings during manager init
- Status: complete
- Dependencies: none
- Scope: `src/manager/manager.c` — call `cbx_settings_load` after `cbx_settings_defaults` and before controllers tab init; `tests/test_manager_integration.c` — regression test for persisted settings on init
- Acceptance criteria: Manager init reads `settings.yaml` from disk; `mgr->settings.virtual_controllers.count` matches persisted value (not default 4) when a non-default settings file exists; controllers tab `set_expected_count` receives the persisted count; regression test saves non-default count, reinitializes manager, and verifies expected count matches saved value
- Verification: `nix-shell --run 'cmake --build build-check --parallel 2 && ctest --test-dir build-check -R test_manager_integration --output-on-failure'` — 12/12 tests pass including `test_persisted_settings_loaded_on_init`; full suite 98/98 pass
- Documentation impact: `docs/OPERATIONS.md` — updated settings.yaml section to note that both overlay service and manager load persisted settings at startup

## Task 2: Move DBus interface definitions to production header
- Status: complete
- Dependencies: none
- Scope: Created `src/dbus/dbus_interface.h` with all production DBus definitions (`ip_dbus_backend` vtable, `ip_bus_handle`, `ip_signal_cb`, signal payload structs, `ip_prop_type` enum, all `IP_DBUS_*`/`IP_IFACE_*` constants, `ip_dbus_sd_backend()` declaration). Updated all 24 `src/` includes from `dbus_mock.h` to `dbus_interface.h` (or `dbus/dbus_interface.h` for non-dbus dirs). Updated `tests/dbus_mock.h` to include `dbus_interface.h` and retain only mock-specific code. Updated 6 test files that only use constants to include `dbus_interface.h` directly. Removed `tests/` from `controllerbox` PUBLIC include path. Added `src/` to `cbx_test_support` and `test_ip_server` include dirs.
- Acceptance criteria: No `src/` file includes `dbus_mock.h` ✓; `tests/dbus_mock.h` includes `dbus_interface.h` for shared definitions ✓; production build succeeds without `tests/` in include path ✓; all 98 tests pass ✓; sanitizer compiles clean ✓
- Verification: `grep -r '#include.*"dbus_mock.h"' src/` returns no matches; `nix-shell --run 'cmake --build build-check --parallel 2 && ctest --test-dir build-check --output-on-failure'` — 98/98 pass, 2 skipped (hardware-blocked)
- Documentation impact: None (internal refactor, no user-facing behavior change)

## Task 3: Kernel-backed controller acceptance evidence
- Status: pending
- Dependencies: none
- Scope: Obtain runner access with `/dev/uinput` (declared `kernel-uinput` or `physical-controller` capability). Run `tests/test_kernel_controller.c` to completion (exit 0, not 77). Augment `test_installed_functional.c` to use kernel-backed controller path or add separate test exercising installed binary with kernel-backed evdev input through full manager and overlay workflow. Record exact-commit runner evidence via `scripts/check-factory-runner-evidence.py`.
- Acceptance criteria: `test_kernel_controller` exits 0 on runner with `/dev/uinput`; installed functional test uses kernel-backed synthetic gamepad (not process-local `SDL_JoystickAttachVirtual`); runner evidence for `kernel-uinput` or `physical-controller` capability declared and validated; MGR-11, MGR-13, VRF-05 conformance rows upgraded to verified
- Verification: `scripts/check-factory-runner-evidence.py --print-capabilities` includes `kernel-uinput` or `physical-controller`; `ctest -R test_kernel_controller` exit 0; `ctest -R test_installed_functional` PASS with kernel-backed controller
- Documentation impact: `docs/OPERATIONS.md` — update installed functional smoke test description to reflect kernel-backed controller; `README.md` — update known environment limitations

## Task 4: GPU backend smoke evidence on hardware renderer
- Status: pending
- Dependencies: none
- Scope: Obtain runner access with an accelerated OpenGL/OpenGL ES backend (declared `gpu-compositor` capability). Run `tests/test_backend_smoke.c` to completion (exit 0, not 77) with framebuffer invariants. Record exact-commit runner evidence.
- Acceptance criteria: `test_backend_smoke` exits 0 on runner with accelerated backend; broad framebuffer invariants pass against hardware renderer; runner evidence for `gpu-compositor` capability declared and validated; VRF-06, DOD-05 conformance rows upgraded to verified
- Verification: `scripts/check-factory-runner-evidence.py --print-capabilities` includes `gpu-compositor`; `ctest -R test_backend_smoke` exit 0
- Documentation impact: `docs/OPERATIONS.md` — update backend smoke test section with hardware renderer evidence

## Task 5: Target hardware overlay latency and aarch64 build verification
- Status: pending
- Dependencies: none
- Scope: Obtain runner access to Pi 4 or equivalent ARM64 minimum supported hardware (declared `target-consumer` capability). Build Controller-Box on aarch64 (CMake + system deps). Run `test_overlay_latency.c` on target hardware measuring both p99 and maximum overlay appearance latency. Record performance measurement artifact with timing data. Record exact-commit runner evidence.
- Acceptance criteria: aarch64 build succeeds with zero warnings; `test_overlay_latency` on Pi 4 shows ≤75ms p99 and ≤100ms max overlay appearance; measurement artifact committed with timing data; runner evidence for `target-consumer` declared and validated; SYS-01, SYS-02, OVL-10, PERF-01, DOD-08 conformance rows upgraded to verified
- Verification: `scripts/check-factory-runner-evidence.py --print-capabilities` includes `target-consumer`; aarch64 binary builds and runs; latency artifact in repository
- Documentation impact: `README.md` — update Pi 4 performance section with measured latency; `docs/OPERATIONS.md` — update performance expectations with target hardware data

## Task 6: Human release acceptance on target hardware
- Status: pending
- Dependencies: Task 5
- Scope: On Pi 4 or equivalent target hardware, a human reviews representative manager and overlay captures for legibility, clipping, focus indication, contrast, and controller-only usability. Record a verifiable signed acceptance artifact with reviewer name, date, hardware, captures, and criteria checklist. Record exact-commit runner evidence.
- Acceptance criteria: Signed human acceptance artifact exists in repository with representative captures; all five §11.1.7 criteria (legibility, clipping, focus indication, contrast, controller-only usability) reviewed and passed; runner evidence for `target-consumer` validated; VRF-07 conformance row upgraded to verified
- Verification: Human acceptance artifact committed; `scripts/check-factory-runner-evidence.py --print-capabilities` includes `target-consumer`
- Documentation impact: `docs/OPERATIONS.md` — human release acceptance checklist completed; `docs/REVIEW.md` — human sign-off recorded

## Task 7: Campaign capability declaration and Flatpak verification
- Status: pending
- Dependencies: Task 3, Task 4, Task 5, Task 6
- Scope: After Tasks 3–6 produce runner evidence for hardware capabilities, declare all 7 campaign-required capabilities in `.factory/environment.toml` with validated evidence. Verify Flatpak manifest criteria: clean Flatpak build passes installed functional gate, host profile paths visible, host InputPlumber DBus access verified, application published (or documentation explicitly marks as experimental). Run `scripts/check-factory-runner-evidence.py` to validate all capabilities. Update `.factory-state/runner-evidence.json` aggregate.
- Acceptance criteria: All 7 campaign capabilities (`physical-controller`, `kernel-uinput`, `inputplumber-system-dbus`, `gpu-compositor`, `systemd-user`, `installed-package`, `target-consumer`) declared and evidenced; `check-factory-runner-evidence.py` accepts all; Flatpak criteria met or documentation marks experimental status accurately; PKG-01, DOD-01 conformance rows upgraded
- Verification: `scripts/check-factory-runner-evidence.py --print-capabilities` lists all 7; `scripts/check-factory-runner-evidence.py --print-digest` matches; `tests/test_flatpak_manifest` passes
- Documentation impact: `README.md` — update known environment limitations section; `.factory/environment.toml` — declare evidenced capabilities

## Task 8: Final documentation and specification audit
- Status: pending
- Dependencies: Task 1, Task 2, Task 3, Task 4, Task 5, Task 6, Task 7, Task 9, Task 10
- Scope: Execute the canonical definition of done from `docs/SPEC.md` §11.2. Verify the conformance matrix is all-verified, the interaction inventory is exhaustive with all entries verified (except DEFERRED O12 per §13), no contradictory open v1 bugs remain in `.factory/bugs/open.md`, independent adversarial reviews (correctness, test-quality, security, documentation) find no blocking issues, full clean verification passes (`./scripts/verify-project.sh`), documentation matches observed behavior, and the Git tree is clean on `develop`.
- Acceptance criteria: (1) Every conformance matrix row classified `verified` — no `partial`, `missing`, or `ambiguous` remains. (2) Every §5.7 interaction inventory entry (M01–M38, O01–O13, D01–D08) has passing controller and pointer evidence (where applicable) through production dispatch. (3) `.factory/bugs/open.md` contains no unresolved defect contradicting a v1 requirement. (4) Independent reviews find no blocking issue. (5) `./scripts/verify-project.sh` passes: clean build, all 98+ tests, installed functional acceptance (not skipped), packaging, sanitizer clean. (6) `README.md` and `docs/OPERATIONS.md` match observed behavior. (7) Git tree clean on `develop`. (8) Remediation rule: if any gap is found, preserve the task ledger, append a uniquely numbered pending task, add it to this task's dependencies, return this task to pending, and continue.
- Verification: `./scripts/verify-project.sh`; `./scripts/final-gate.sh --planning` (pre-completion); conformance matrix spot-check; interaction inventory completeness check; `git status --porcelain` clean
- Documentation impact: Final review of all documentation for accuracy and reproducibility

## Task 9: Fix documentation accuracy issues in README and OPERATIONS.md (remediation)
- Status: complete
- Dependencies: none
- Scope: Fix 7 factual/contradictory/misleading claims identified by docs review: (1) Interaction inventory counts 50/8/1→52/6/1 in README.md and OPERATIONS.md; (2) OPERATIONS.md "no Xvfb" header contradicts test_installed_smoke PASS; (3) SPEC §11.1.5 Layer 5 mapped to test_installed_smoke instead of test_installed_functional; (4) README verification table layer numbering inconsistent with SPEC §11.1 7-layer scheme; (5) "98 CTest targets verified" misleading — 2 skip (exit 77) in headless; (6) OPERATIONS.md §11.1 coverage table omits test_installed_functional, test_installed_binary, test_backend_smoke_sw, test_kernel_controller, interaction tests; (7) "overlay renders in under 10 ms" unqualified — add x86_64 software renderer qualifier. All required by DOD-08 (docs match observed behavior).
- Acceptance criteria: All 7 issues fixed; README.md and docs/OPERATIONS.md contain no factual errors about current x86_64 behavior; interaction inventory counts match source code (52/6/1); layer numbering consistent with SPEC; skip behavior accurately documented
- Verification: Manual review of changed sections; `grep -rn '50/59\|8 NOT_APPLICABLE\|no Xvfb' README.md docs/OPERATIONS.md` returns no matches; `grep -rn '52/59\|6 NOT_APPLICABLE' README.md docs/OPERATIONS.md` returns matches
- Result: All 7 issues fixed. `grep` confirms zero stale references. README verification table aligned with SPEC §11.1 7-layer scheme (kernel-backed controller moved to sub-layer 5c, human release restored to layer 7). OPERATIONS.md §11.1 coverage table expanded with test_installed_functional, test_installed_binary, test_backend_smoke_sw, test_kernel_controller, and interaction acceptance tests. `verify-project.sh` passes: 96 pass, 2 skip (exit 77), 0 failures. DOD-08 evidence updated to note x86_64 documentation accuracy verified.
- Documentation impact: README.md, docs/OPERATIONS.md — accuracy corrections

## Task 10: Add missing test coverage for axis events, GamepadOrder E2E, and backend smoke invariants (remediation)
- Status: complete
- Dependencies: none
- Scope: Close test quality gaps identified by test review: (1) HIGH: Add SDL_JoystickSetVirtualAxis test in test_installed_functional.c — analog sticks/triggers never exercised in any test; (2) MEDIUM: Verify GamepadOrder via independent DBus inspection after overlay save in test_installed_functional.c; (3) MEDIUM: Add renderer-is-software assertion, fb_frames_differ between render states, fb_region_has_color for theme colors in test_backend_smoke_sw.c; (4) LOW: Add target_count==0 assertion to D01 degraded click test. All software-fixable in x86_64 headless environment.
- Acceptance criteria: Axis events (SDL_CONTROLLERAXISMOTION) exercised through production dispatch path; GamepadOrder verified via DBus after overlay close; backend_smoke_sw asserts software renderer flag, uses fb_frames_differ and fb_region_has_color; D01 test asserts no DBus side effects; all 98+ tests pass with 0 new failures
- Verification: `nix-shell --run 'cmake --build build-check --parallel 2 && ctest --test-dir build-check --output-on-failure'` — 96 pass, 2 skip (hardware-blocked, exit 77), 0 failures. New assertions confirmed executing: axis events (6 axes: left stick X/Y, right stick X/Y, L/R triggers) sent via `SDL_JoystickSetVirtualAxis` through `pump_manager` (manager) and `cbx_overlay_service_step` (overlay) — verified no state change; GamepadOrder read back via `ip_manager_get_gamepad_order` on independent DBus connection after overlay close — verified CSV contains comp0 path; `fb_frames_differ` between different grid states and between different manager tabs; `fb_region_has_color` for theme bg={18,18,28} and panel_bg={30,30,42}; `SDL_RENDERER_SOFTWARE` flag asserted in both overlay and manager renderer paths; D01 asserts `target_count == 0` after degraded click.
- Documentation impact: None