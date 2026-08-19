---
spec_path: docs/SPEC.md
spec_commit: 3a10f6b7d04a615b2b9d06eef6c91e431fa9c079
spec_blob: 58f5d3cb72bc6b3e5f573fa09a63c11a653ed577
base_commit: 860922af39ed7e2aef9b95705705f7ed90344e87
status: active
---

# Implementation Plan

## Goal and non-goals

**Goal.** Close all remaining implementation gaps against `docs/SPEC.md` so the
autonomous definition of done (§11.2) is satisfied: correct documentation,
complete interaction inventory with production-path controller evidence, and
accounted hardware-deferred capabilities.

**Non-goals.** No new product features. No spec changes. No retrieval of
completed tasks from Git history. Hardware-provisioning tasks (GPU, aarch64
runner, Pi 4, human reviewer) are documented as deferrals, not implemented.

## Architecture and constraints

- **One binary, two modes** (`controller-box --overlay-service | --manager`):
  shared SDL2 widget toolkit, sd-bus DBus client, and config layer.
- **InputPlumber is the engine** — every state change goes through its DBus API;
  the GUI never touches input routing.
- **Declared runner capabilities** (`.factory/environment.toml`):
  `remote-project-gate`, `systemd-user`, `kernel-uinput`, `installed-package`.
  Runner receipt at commit `26df6c0` proves all four (test_kernel_controller
  passes, Flatpak build passes, 98 CTest targets, 97 pass / 1 skip).
- **Undeclared capabilities**: `gpu-compositor`, `physical-controller`,
  `inputplumber-system-dbus`, `target-consumer`. Native-signature private
  sd-bus tests cover `inputplumber-system-dbus` per §10.1 ("real/private
  sd-bus service"). `physical-controller` is covered by `test_kernel_controller`
  (uinput gamepad on runner). `gpu-compositor` and `target-consumer` are
  genuinely unavailable and require documented deferrals.
- **Constraints**: build via `nix-shell --run 'cmake ...'`; tests via ctest;
  production-path dispatch required for interaction acceptance (§5.7);
  no keyboard-labeled-as-controller (§5.7); no string-only mock for native type
  fidelity (§10.1).

## Specification conformance matrix

| ID | Spec § | Classification | Evidence | Task |
|----|--------|--------------|----------|------|
| ARCH-01 | §2.1 | verified | `dbus_client.c` — all ops through DBus vtable; no direct input routing | |
| ARCH-02 | §2.2 | verified | `dbus_client.c` sd-bus; `ip_connection.c` system bus | |
| ARCH-03 | §2.3 | verified | `main.c` mode dispatch; `overlay_service.c`, `manager.c` | |
| ARCH-04 | §2.4 | verified | `ip_connection.c` NameOwnerChanged, degraded/recovery, ≤2s re-enumerate; `test_native_dbus.c` test 2–3 | |
| ARCH-05 | §2.5 | verified | `trigger.c` SetInterceptActivation; `ip_intercept_poll.c` 50 ms poll; `test_trigger.c`, `test_intercept_poll.c` | |
| SYS-01 | §3 | partial | `cmake/aarch64-toolchain.cmake` + `cross-shell.nix` exist; no evidenced aarch64 build; `target-consumer` undeclared | Task 3 |
| SYS-02 | §3 | partial | x86_64 build verified; Pi 4 ARM64 GLES 3.0 latency not measured; `target-consumer` undeclared | Task 3 |
| SYS-03 | §3 | verified | SDL2 supports X11/Wayland/Gamescope; `test_sdl_dummy.c` | |
| SYS-04 | §3 | verified | `CMakeLists.txt` deps: SDL2, SDL2_ttf, SDL2_image, libsystemd, libyaml; nanosvg vendored `third_party/nanosvg/` | |
| SYS-05 | §3 | verified | InputPlumber not bundled; runtime bus-name check in `ip_connection.c` | |
| SYS-06 | §3 | verified | Native sd-bus tests pass with polkit-equivalent access; `test_native_dbus.c` | |
| OVL-01 | §4.1 | verified | `grid_render.c` select-screen grid; `test_grid_render.c`, `test_overlay_visual.c` | |
| OVL-02 | §4.2 | verified | `trigger.c` default Select+A, configurable; `test_trigger.c` | |
| OVL-03 | §4.3 | verified | `player_mode.c` independent per-controller; `test_player_mode.c`, `test_overlay_native.c` O11 | |
| OVL-04 | §4.4 | verified | `host_mode.c` R3 toggle, freeze, navigate; `test_host_mode.c`, `test_overlay_native.c` O06–O09 | |
| OVL-05 | §4.5 | verified | `conflict.c` detect+resolve; `grid_render.c` red; `test_conflict.c`, `test_overlay_native.c` O13 | |
| OVL-06 | §4.6 | verified | `profile_cycle.c` profile follows controller; `test_profile_cycle.c` | |
| OVL-07 | §4.7 | verified | `dynamic_columns.c` scales with target count; `test_dynamic_columns.c` | |
| OVL-08 | §4.8 | verified | `grid_render.c` model name + slot, no nicknames | |
| OVL-09 | §4.9 | partial | Pre-built surface verified (`surface_build.c`); ≤75 ms p99 / ≤100 ms max on Pi 4 not measured; <10 ms p99 detection-to-present measured on x86_64 only | Task 3 |
| OVL-10 | §4.10 | verified | `test_overlay_visual.c` (8 tests), `test_golden.c` (4 overlay baselines) | |
| MGR-01 | §5.1 | verified | `manager.c` tab bar, 3 tabs, controller + pointer; `test_manager_tabs.c`, `test_manager_native.c` | |
| MGR-02 | §5.2 | verified | `controllers_tab.c` add/remove/type-change, topology reconcile; `test_controllers_tab.c`, `test_manager_native.c` MG-04 | |
| MGR-03 | §5.3 | verified | `profiles_tab.c` browse/create/edit/delete, built-in Default; `test_profiles_tab.c`, `test_installed_functional.c` | |
| MGR-04 | §5.4 | verified | `profile_editor_list.c`, `profile_editor_seq.c` both modes; `test_editor_list_mode.c`, `test_editor_seq_mode.c` | |
| MGR-05 | §5.4 | verified | `profile_validate.c` NES minimum (A/B/D-pad); `test_profile_validate.c`; `profile_save.c` enforces before write | |
| MGR-06 | §5.5 | verified | `settings_tab.c` all settings; `test_settings_tab.c`, `test_manager_native.c` M21–M26 | |
| MGR-07 | §5.6 | verified | `test_manager_visual.c` (13 tests), `test_golden.c` (7 manager baselines) | |
| MGR-08 | §5.7 | partial | 51/59 inventory verified, 7 NA, 1 deferred; M39 missing from inventory; M28–M38 editor tests use keyboard labeled as controller, not real gamepad transport | Task 1, Task 2 |
| ID-01 | §6.2 | verified | `identity.c` 4-layer auto-assignment; `test_identity.c` | |
| ID-02 | §6.3 | verified | `identity.c` BT:/USB:/USB:phys:/ORDER: prefixes; `config_assignments.c` validation | |
| ID-03 | §6.3 | verified | `identity_downgrade.c` fallback to ORDER:n; `test_identity_downgrade.c` | |
| CFG-01 | §7.1 | verified | `config_profile.c` InputPlumber YAML; `config_profile_list.c` filesystem enumeration | |
| CFG-02 | §7.2 | verified | `config_paths.c` file layout; `test_config_paths.c` | |
| CFG-03 | §7.3 | verified | `config_settings.c` settings.yaml; `test_settings.c` | |
| CFG-04 | §7.4 | verified | `config_assignments.c` assignments.yaml + gamepad_order; `test_assignments.c`, `test_assign_persist.c` | |
| CFG-05 | §7.5 | verified | `config_profile_meta.c` optional sidecar; `test_profile_save.c` | |
| CFG-06 | §7.6 | verified | `config_profile.c` device_profile_v1 schema; `test_profile_yaml.c` | |
| ICN-01 | §8.1 | verified | `icon_map.c` virtual-type mapping; `test_icon_map.c` | |
| ICN-02 | §8.2 | verified | `icon_cache.c` Controllercons + custom icons in `data/icons/` | |
| ICN-03 | §8.3 | verified | `icon_cache.c` nanosvg rasterize at startup, cached; `test_icon_cache.c`, `smoke_test_nanosvg.c` | |
| ICN-04 | §8.4 | verified | `icon_map.c` controller-icons.yaml; `test_icon_map.c` | |
| ICN-05 | §8.5 | verified | `icon_lookup.c` profile override; `test_icon_lookup.c` | |
| PKG-01 | §9.1 | verified | `packaging/org.shadowblip.ControllerBox.yaml`; `test_flatpak_manifest.py`; runner receipt 26df6c0 Flatpak PASS | |
| PKG-02 | §9.2 | verified | CMake install rules; `test_packaging.sh` | |
| PKG-03 | §9.3 | verified | `CMakeLists.txt` install layout; `test_packaging.sh` | |
| PKG-04 | §9.4 | verified | `ip_connection.c` runtime bus-name check; no cross-manager dependency | |
| PKG-05 | §9.1 | verified | `service_install.c` first-run install; `test_service_install.c`; `test_manager_interaction_ctrl.c` M39 tests | |
| DBUS-01 | §10.1 | verified | `ip_connection.c`, `dbus_client.c` system bus connection; `test_connection.c` | |
| DBUS-02 | §10.1 | verified | `dbus_client.c` native sd-bus types (u, b, as, s); `test_dbus_signatures.c`, `test_native_dbus.c` 11 tests | |
| DBUS-03 | §10.1 | verified | `ip_objectmanager.c` GetManagedObjects; `test_objectmanager_parse.c` | |
| DBUS-04 | §10.1 | verified | `ip_hotplug.c` InterfacesAdded/Removed; `test_hotplug.c` | |
| DBUS-05 | §10.1 | verified | `ip_connection.c` owner check, version, enumeration; `test_native_dbus.c` test 2 | |
| DBUS-06 | §10.2 | verified | `ip_manager.c`, `ip_composite.c`, `ip_target.c`, `ip_source.c` full API surface | |
| DBUS-07 | §10.3 | verified | All 5 gaps: intercept poll, gamepad order persist, temp composite YAML, filesystem enumerate, no-op gap 5 | |
| PERF-01 | §11 | partial | x86_64 latency measured (`test_overlay_latency.c`); Pi 4 ≤75 ms p99 not measured | Task 3 |
| PERF-02 | §11 | verified | PASS mode kernel-level; no DBus gameplay routing | |
| PERF-03 | §11 | verified | `test_close.c` InterceptMode=PASS close <1 ms | |
| PERF-04 | §11 | verified | `test_daemon_footprint.c` resident footprint | |
| PERF-05 | §11 | verified | `ip_gamepad_order.c` atomic reorder; `test_gamepad_order.c` | |
| VRF-01 | §11.1.1 | verified | `test_overlay_visual.c`, `test_manager_visual.c` SDL_RenderReadPixels through production composition | |
| VRF-02 | §11.1.2 | verified | `fb_assert.c` region-level assertions; `test_fb_assert.c` | |
| VRF-03 | §11.1.3 | verified | `test_golden.c` 11 baselines in `tests/golden/`, ±3/channel <2% tolerance | |
| VRF-04 | §11.1.4 | verified | `fb_assert.c` saves actual/expected/diff on mismatch | |
| VRF-05 | §11.1.5 | verified | `test_installed_functional.c` (4 tests), `test_installed_smoke.sh`, `test_installed_binary.sh`; runner receipt 26df6c0 all pass | |
| VRF-06 | §11.1.6 | partial | `test_backend_smoke_sw.c` software coverage; `test_backend_smoke.c` GPU skips (exit 77); `gpu-compositor` undeclared | Task 3 |
| VRF-07 | §11.1.7 | missing | No human release acceptance artifact; `target-consumer` undeclared | Task 3 |
| DOD-01 | §11.2.1 | partial | This matrix; non-verified rows map to Tasks 1–3 | Task 4 |
| DOD-02 | §11.2.2 | verified | Tests use production dispatch; native DBus preserves signatures | |
| DOD-03 | §11.2.3 | partial | M39 missing from inventory; M28–M38 editor tests lack real controller-transport evidence | Task 1, Task 2 |
| DOD-04 | §11.2.4 | verified | `test_overlay_visual.c`, `test_manager_visual.c` cover degraded/error/recovery states | |
| DOD-05 | §11.2.5 | partial | `test_backend_smoke` GPU skip unexplained; no human-approved deferral documented | Task 3 |
| DOD-06 | §11.2.6 | verified | `.factory/bugs/open.md` is empty `[]` | |
| DOD-07 | §11.2.7 | verified | Campaign audit round 1 completed with 5 findings; this plan addresses all | |
| DOD-08 | §11.2.8 | partial | README.md/OPERATIONS.md stale capability claims, wrong inventory counts (52/6/1 vs 51/7/1) | Task 1 |
| DOD-09 | §11.2.9 | verified | Clean tree on `develop`; plan metadata preserved | |

## Interaction acceptance inventory

Source: `tests/interaction_inventory.c` (59 entries: M01–M38, O01–O13, D01–D08).
Status: 51 verified, 7 NOT_APPLICABLE (controller-only paths: M13, M14, M16,
M32, M34, M35, M36), 1 DEFERRED (O12 host profile cycling, §13).

**Gap — M39 (first-run service install):** Tested in
`test_manager_interaction_ctrl.c` (4 tests: controller confirm/cancel, pointer
confirm/cancel) but NOT enumerated in `interaction_inventory.c`. Task 1 adds
M39 to the inventory data structure and updates the expected count.

**Gap — controller transport for M28–M38:** Profile editor interaction tests in
`test_manager_interaction_prof.c` use `send_key_dn` (SDL_KEYDOWN via
`cbx_manager_handle_event`) with functions labeled `_controller`. Per §5.7,
keyboard-generated SDL events are supplemental and must never be labeled
controller acceptance. Real controller-transport evidence exists in
`test_installed_functional.c::test_installed_controller_acceptance` for
Profiles tab (create/save, edit/cancel, delete/confirm) but NOT for editor
binding-list navigation (M28–M31), sequential mode start (M33), or
save/discard in editor (M37–M38). Task 2 adds `ctrl_press`-based tests through
production gamepad transport for these entries.

### Manager controls (M01–M38 + M39)

| ID | Control | Controller path | Pointer path | Semantic outcome | Production dispatch | Evidence |
|----|---------|----------------|-------------|-------------------|---------------------|----------|
| M01–M03 | Tab switch (Controllers/Profiles/Settings) | D-pad L/R on tabbar → A | Mouse click tab | Active tab changes, panel visible | `cbx_manager_handle_event` → tabbar | `test_manager_interaction_ctrl.c`, `test_manager_native.c` |
| M04 | Controllers list select | D-pad Down → A | Mouse click row | Visual focus on device | `cbx_manager_handle_event` → list | `test_manager_native.c` |
| M05 | Add button → type picker | A on Add | Mouse click Add | Picker dialog opens, CreateTargetDevice ready | `cbx_manager_handle_event` → button | `test_manager_interaction_ctrl.c`, `test_installed_functional.c` |
| M06 | Remove → device count decreases | A on Remove | Mouse click Remove | StopTargetDevice called, count−1 | `cbx_manager_handle_event` → button → `controllers_tab.c` | `test_manager_interaction_ctrl.c`, `test_installed_functional.c` |
| M07 | Change Type → picker | A on Change | Mouse click Change | Picker opens | `cbx_manager_handle_event` → button | `test_manager_interaction_ctrl.c` |
| M08 | Type change confirmed | A in picker | Mouse click item | SetTargetDevices called, type changes | `cbx_manager_handle_event` → list | `test_manager_interaction_ctrl.c` |
| M09 | Picker cancel (B) | B | Mouse click outside | Picker closes, no DBus call | `cbx_manager_handle_event` → list | `test_manager_native.c` |
| M10 | Profile list select | D-pad Down → A | Mouse click row | Visual focus on profile | `cbx_manager_handle_event` → list | `test_manager_interaction_prof.c` (keyboard) |
| M11 | Create source picker | A on Create | Mouse click Create | Picker opens (Default/Empty/Clone) | `cbx_manager_handle_event` → button | `test_manager_interaction_prof.c` (keyboard) |
| M12 | Source selected → name input | A on source | Mouse click source | Name input mode opens | `cbx_manager_handle_event` → list | `test_manager_interaction_prof.c` (keyboard) |
| M13–M14 | Name input typing (NA pointer) | D-pad/c-z keys | N/A | Chars appended/deleted | `cbx_manager_handle_event` → text | `test_manager_interaction_prof.c` (keyboard) |
| M15 | Editor opens with new profile | A on confirm | Mouse click confirm | Editor mode, in-memory profile | `cbx_manager_handle_event` → button | `test_manager_interaction_prof.c` (keyboard) |
| M16 | Cancel name input (NA pointer) | B | N/A | Returns to list, no file | `cbx_manager_handle_event` | `test_manager_interaction_prof.c` (keyboard) |
| M17 | Edit existing profile | A on profile | Mouse click profile | Editor opens with loaded profile | `cbx_manager_handle_event` → list | `test_manager_interaction_prof.c` (keyboard) |
| M18 | Delete confirm mode | A on Delete | Mouse click Delete | Confirm dialog opens | `cbx_manager_handle_event` → button | `test_manager_interaction_prof.c` (keyboard) |
| M19 | Delete confirmed | A | Mouse click Yes | File unlinked, sidecar deleted, list refresh | `cbx_manager_handle_event` → button | `test_manager_interaction_prof.c` (keyboard) |
| M20 | Delete cancel (B) | B | Mouse click No | Returns to normal, no deletion | `cbx_manager_handle_event` | `test_manager_interaction_prof.c` (keyboard) |
| M21 | Settings list select | D-pad Down → A | Mouse click row | Visual focus on setting | `cbx_manager_handle_event` → list | `test_manager_native.c` |
| M22 | Toggle launch_at_boot | A | Mouse click | Value toggles | `cbx_manager_handle_event` → list | `test_manager_native.c` |
| M23 | Edit mode entered | A on setting | Mouse click setting | Edit mode, value adjustable | `cbx_manager_handle_event` → list | `test_manager_native.c` |
| M24 | Value adjusted | D-pad U/D in edit | N/A (edit mode) | Value cycles/adjusts | `cbx_manager_handle_event` → list | `test_manager_native.c` |
| M25 | Edit confirmed (A) | A | Mouse click | Edit exits, value applied | `cbx_manager_handle_event` → list | `test_manager_native.c` |
| M26 | Edit cancelled (B) | B | Mouse click outside | Edit exits, value reverts | `cbx_manager_handle_event` → list | `test_manager_native.c` |
| M27 | Save settings | A on Save | Mouse click Save | settings.yaml written | `cbx_manager_handle_event` → button | `test_manager_native.c`, `test_installed_functional.c` |
| M28 | Binding highlighted, diagram lights | D-pad U/D in editor | Mouse click row | Diagram button lights | `cbx_manager_handle_event` → list → `profile_editor_list.c` | `test_manager_interaction_prof.c` (keyboard — Task 2 adds ctrl_press) |
| M29 | Binding edit sub-menu | A on binding | Mouse click row | Sub-menu opens (Pick/Capture/Seq) | `cbx_manager_handle_event` → list | `test_manager_interaction_prof.c` (keyboard — Task 2 adds ctrl_press) |
| M30 | Target picked, binding updated | A on target | Mouse click target | Binding updated, picker closes | `cbx_manager_handle_event` → list | `test_manager_interaction_prof.c` (keyboard — Task 2 adds ctrl_press) |
| M31 | Capture mode begins | A on Capture | Mouse click Capture | Waiting for physical button | `cbx_manager_handle_event` → list | `test_manager_interaction_prof.c` (keyboard — Task 2 adds ctrl_press) |
| M32 | Binding captured (NA pointer) | Physical button via DBus InputEvent | N/A | Source event set, capture ends | DBus signal → `ip_input_signal.c` → editor | `test_manager_native_prof.c` DBus signal path |
| M33 | Sequential mode begins | A on Sequential | Mouse click Sequential | First button prompted, diagram lights | `cbx_manager_handle_event` → list | `test_manager_interaction_prof.c` (keyboard — Task 2 adds ctrl_press) |
| M34 | Button captured in sequential (NA pointer) | Physical button via DBus InputEvent | N/A | Auto-advance, progress bar | DBus signal → editor | `test_manager_native_prof.c` DBus signal path |
| M35 | Skip binding in sequential (NA pointer) | B | N/A | Advance to next | `cbx_profile_editor_seq_skip()` | `test_manager_native_prof.c` |
| M36 | Sequential cancelled (NA pointer) | Start | N/A | Changes discarded, editor returns | `cbx_manager_handle_event` | `test_manager_native_prof.c` |
| M37 | Profile saved | B in LIST (save) | Mouse click Save | File written, editor closes, list refresh | `cbx_manager_handle_event` → button → `profile_save.c` | `test_manager_interaction_prof.c` (keyboard — Task 2 adds ctrl_press), `test_installed_functional.c` |
| M38 | Editor discard | Start in LIST | Mouse click Discard | No file written, editor closes | `cbx_manager_handle_event` | `test_manager_interaction_prof.c` (keyboard — Task 2 adds ctrl_press) |
| M39 | First-run service install | A=confirm / B=cancel | Mouse click Yes/No | Service installed or dismissed | `cbx_manager_handle_event` → dialog → `service_install.c` | `test_manager_interaction_ctrl.c` (4 tests) — Task 1 adds to inventory |

### Overlay actions (O01–O13)

| ID | Action | Controller path | Pointer path | Semantic outcome | Production dispatch | Evidence |
|----|--------|-----------------|-------------|-------------------|---------------------|----------|
| O01 | Open (intercept activation) | InterceptMode PASS→ALL poll | N/A | Surface shown, lifecycle ACTIVATING→VISIBLE | `ip_intercept_poll.c` → `lifecycle.c` → `surface_build.c` | `test_overlay_native.c` O01 |
| O02 | Move column left | D-pad Left via DBus InputEvent | N/A | Grid column decreases | DBus signal → `cbx_overlay_input_cb` → `player_mode.c` | `test_overlay_native.c` O02 |
| O03 | Move column right | D-pad Right | N/A | Column increases | Same dispatch | `test_overlay_native.c` O03 |
| O04 | Cycle profile up | D-pad Up | N/A | Profile changes, LoadProfilePath called | Same dispatch → `profile_cycle.c` | `test_overlay_native.c` O04 |
| O05 | Cycle profile down | D-pad Down | N/A | Profile changes (reverse) | Same dispatch | `test_overlay_native.c` O05 |
| O06 | Enter Host Mode (R3) | R3 | N/A | Host mode, others freeze | Same dispatch → `host_mode.c` | `test_overlay_native.c` O06 |
| O07 | Host navigate rows | D-pad U/D | N/A | Selected row changes | Same dispatch | `test_overlay_native.c` O07 |
| O08 | Host change slot | D-pad L/R | N/A | Row slot changes, conflict may arise | Same dispatch | `test_overlay_native.c` O08 |
| O09 | Exit Host Mode (R3) | R3 | N/A | Controllers unfreeze | Same dispatch | `test_overlay_native.c` O09 |
| O10 | Close overlay (B) | B | N/A | Save: conflicts resolved, assignments persisted, InterceptMode=PASS, hidden | Same dispatch → `lifecycle.c` → `overlay_service.c:on_save` | `test_overlay_native.c` O10, O10b |
| O11 | Multi-controller independence | Each controller moves own row | N/A | Independent row movement | Same dispatch | `test_overlay_native.c` O11 |
| O12 | Host cycle profile (DEFERRED) | D-pad U/D in host | N/A | Selected row's profile changes | Deferred per §13 | Pinned in inventory |
| O13 | Conflict detection + resolution | Two controllers same column | N/A | Red highlight, auto-move on save | `conflict.c` → `grid_render.c` | `test_overlay_native.c` O13 |

### Disabled-state controls (D01–D08)

| ID | Control | Both paths reject | Semantic outcome | Evidence |
|----|---------|-------------------|-----------------|----------|
| D01 | IP unavailable → degraded | Controller + pointer | Controls disabled, status shown, no side effect | `test_installed_functional.c` D01, `test_manager_visual.c` |
| D02 | No device selected → Remove disabled | Controller + pointer | No DBus call | `test_manager_interaction_ctrl.c` D02 |
| D03 | No profile selected → Delete disabled | Controller + pointer | No file deletion | `test_manager_interaction_ctrl.c` D03 |
| D04 | Save with missing NES bindings | Controller + pointer | Validation fails, no write | `test_profile_validate.c`, `test_installed_functional.c` D08 |
| D05 | Settings edit cancel | Controller + pointer | Value reverts, no write | `test_manager_native.c` M26 |
| D06 | CreateTargetDevice error | Controller + pointer | Error shown, UI responsive | `test_manager_native.c` D06 |
| D07 | Filesystem failure on save | Controller + pointer | Error shown, not written | `test_manager_interaction_ctrl.c` D07 |
| D08 | Empty profile save blocked | Controller + pointer | NES validation blocks save | `test_installed_functional.c` D08 |

## Task 1: Complete interaction inventory and fix stale documentation
- Status: pending
- Dependencies: none
- Scope: `tests/interaction_inventory.c`, `tests/test_interaction_inventory.c`, `README.md`, `docs/OPERATIONS.md`, `tests/CMakeLists.txt`
- Acceptance criteria:
  - M39 (first-run service install: confirm/cancel, controller+pointer) added to `interaction_inventory.c` with correct `category`, `description`, `dispatch_path`, and `verify_status = CBX_VERIFY_VERIFIED` for both confirm and cancel entries
  - `test_interaction_inventory.c` `EXPECTED_TOTAL` and per-status counts updated to match the new inventory
  - `nix-shell --run 'ctest --test-dir build-check -R test_interaction_inventory --output-on-failure'` passes
  - README.md line 204 and 283: "no `kernel-uinput` runner capability declared" corrected to reflect that the capability IS declared and proven by runner receipt at commit 26df6c0 (test_kernel_controller passes); test still skips locally without /dev/uinput
  - README.md line 283: interaction inventory counts corrected from "52/59… 6 NOT_APPLICABLE… 1 DEFERRED" to the actual post-M39 count
  - README.md line 284: CTest count and pass/skip numbers reconciled with actual `ctest` output (98 targets; note runner shows 97 pass / 1 skip, local headless shows 96 pass / 2 skip)
  - `docs/OPERATIONS.md` line 1127: inventory counts corrected to match
  - `docs/OPERATIONS.md` lines 865–867: stale "lacks SSH launcher" claim corrected — runner IS reachable (12 receipts); /dev/uinput provisioned on runner per 26df6c0 evidence
  - `tests/CMakeLists.txt` line 23: stale comment about "no kernel-uinput runner capability declared" corrected
- Verification: `nix-shell --run 'cmake -S . -B build-check -DCMAKE_BUILD_TYPE=Debug && cmake --build build-check --parallel && ctest --test-dir build-check -R "test_interaction_inventory" --output-on-failure'`; `grep -n 'kernel-uinput' README.md docs/OPERATIONS.md tests/CMakeLists.txt` shows no stale claims; `./scripts/check-docs-sync.sh` passes
- Documentation impact: README.md test layer table, known limitations table; OPERATIONS.md coverage table and current limitations

## Task 2: Add controller-transport evidence for profile editor interactions
- Status: pending
- Dependencies: none
- Scope: `tests/test_manager_interaction_prof.c` or new `tests/test_manager_editor_ctrl.c`, `tests/interaction_inventory.c` (update dispatch_path annotations)
- Acceptance criteria:
  - Real controller-transport tests using `ctrl_press()` (SDL_CONTROLLERBUTTONDOWN via `SDL_PushEvent` + `SDL_PumpEvents` + `cbx_manager_handle_event`, through `cbx_manager_controller_to_key`) added for M28 (binding highlight + diagram), M29 (edit sub-menu), M30 (target pick), M33 (sequential mode start), M37 (save), M38 (discard)
  - Tests assert semantic outcomes (binding updated, file written/not written, editor mode changed), not merely event consumption
  - `test_manager_interaction_prof.c` `_controller` suffixed functions relabeled to `_keyboard` or annotated as supplemental per §5.7 ("keyboard-generated SDL events are supplemental accessibility evidence and must never be labeled controller acceptance")
  - `interaction_inventory.c` `dispatch_path` fields for M28–M31, M33, M37–M38 updated to reference the new controller-transport test file
  - `nix-shell --run 'ctest --test-dir build-check -R "test_manager_editor_ctrl|test_manager_interaction_prof" --output-on-failure'` passes
- Verification: `nix-shell --run 'cmake -S . -B build-check -DCMAKE_BUILD_TYPE=Debug && cmake --build build-check --parallel && ctest --test-dir build-check -R "test_manager_editor_ctrl|test_manager_interaction_prof" --output-on-failure'`; grep for `ctrl_press` in new test file confirms production gamepad transport; grep for `_controller` in `test_manager_interaction_prof.c` confirms no misleading labels
- Documentation impact: README.md interaction inventory row updated; OPERATIONS.md §5.7 coverage table updated

## Task 3: Attempt aarch64 cross-compile and document hardware-deferred capabilities
- Status: pending
- Dependencies: none
- Scope: `docs/OPERATIONS.md`, `README.md`, `.factory/bugs/open.md` (if deferral warrants a bug entry), `tests/test_backend_smoke.c` (deferral comment only)
- Acceptance criteria:
  - aarch64 cross-compile attempted via `nix-shell --run 'cmake -S . -B build-aarch64 -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake -DCMAKE_BUILD_TYPE=Release && cmake --build build-aarch64 --parallel'` using `cross-shell.nix`; if it succeeds with zero warnings, evidence the build log; if it fails, document the failure reason and the human-approved deferral per §11.2.6
  - GPU backend smoke (`test_backend_smoke.c`) deferral documented in OPERATIONS.md with: capability `gpu-compositor` undeclared, `test_backend_smoke_sw.c` provides software-renderer partial evidence, human-approved deferral per §11.2.6 (or task remains pending if no human approval available)
  - Pi 4 / ARM64 latency deferral documented: `test_overlay_latency.c` measures x86_64 only; `target-consumer` undeclared; ≤75 ms p99 on Pi 4 requires physical Pi 4 hardware
  - Human release acceptance (§11.1.7) deferral documented: requires human reviewer on target hardware; no autonomous substitute; `target-consumer` undeclared
  - If any deferral cannot be human-approved autonomously, open a bug in `.factory/bugs/open.md` with the deferral description and the blocking capability, so the final audit can account for it
- Verification: `nix-shell --run 'cmake -S . -B build-aarch64 -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake 2>&1 | tee /tmp/aarch64-build.log; echo "exit=$?"'` (attempt); `grep -r 'deferral\|deferred\|human-approved' docs/OPERATIONS.md README.md` shows documented deferrals; `./scripts/bug-ledger.py validate` passes if bugs added
- Documentation impact: OPERATIONS.md current limitations section; README.md known limitations table

## Task 4: Final documentation and specification audit
- Status: pending
- Dependencies: Task 1, Task 2, Task 3
- Scope: `.factory/artifacts/implementation-plan.md` (conformance matrix update), `README.md`, `docs/OPERATIONS.md`, full clean verification
- Acceptance criteria:
  - All conformance matrix rows that depended on Tasks 1–3 reclassified: DOD-08 → verified (docs fixed), DOD-03 → verified (M39 added + controller transport evidence), MGR-08 → verified, DOD-01 → verified, DOD-05 → verified or deferral-accounted, VRF-06 → verified or deferral-accounted, VRF-07 → verified or deferral-accounted, SYS-01 → verified or deferral-accounted, SYS-02 → verified or deferral-accounted, OVL-09 → verified or deferral-accounted, PERF-01 → verified or deferral-accounted
  - Any row that remains non-verified after Tasks 1–3 has a human-approved deferral documented in `.factory/bugs/open.md` or OPERATIONS.md per §11.2.6
  - Full clean build and test suite: `nix-shell --run 'rm -rf build-check && cmake -S . -B build-check -DCMAKE_BUILD_TYPE=Debug && cmake --build build-check --parallel && ctest --test-dir build-check --output-on-failure'` — zero failures, zero unexplained skips
  - `./scripts/verify-project.sh` passes
  - `./scripts/check-docs-sync.sh` passes (docs changed alongside implementation)
  - `./scripts/bug-ledger.py validate` passes; open bugs (if any) are all human-approved deferrals, not unaddressed defects
  - `git status` clean on `develop` (only plan artifact + docs changed)
  - Independent adversarial review: launch read-only reviewer and docs-reviewer subagents; no blocking issues found
  - Interaction inventory is exhaustive: M01–M39 + O01–O13 + D01–D08 all verified or NOT_APPLICABLE, with production-path controller and pointer evidence per §5.7
  - Definition of done (§11.2) all 9 criteria satisfied or explicitly deferred with human approval
- Verification: `./scripts/final-gate.sh --planning` passes (planning mode); `./scripts/verify-project.sh` passes; conformance matrix has no unverified rows without task references or documented deferrals
- Documentation impact: Final reconciliation of README.md, OPERATIONS.md, and implementation-plan conformance matrix

## Remediation rule

When the final audit (Task 4) finds a gap that Tasks 1–3 did not close:
1. Preserve the existing task ledger and conformance matrix.
2. Append a uniquely numbered pending task (Task 5, 6, …) with bounded scope.
3. Add the new task to Task 4's dependencies.
4. Return Task 4 to `pending` status.
5. Continue the loop.

Reaching an iteration, runtime, or session ceiling leaves the cycle
incomplete (`status: active`); it never satisfies the plan.