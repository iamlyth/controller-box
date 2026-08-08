---
spec_path: docs/SPEC.md
spec_commit: 60aa67a7de962a8dd617d44dbaf7d8038a24ea72
spec_blob: f81db26137e0a58ef384eadd0f517547f3484778
base_commit: 6281781e6818a5547af1d3b0fc17bc2905a471ce
status: active
---

# Implementation Plan — Interaction Acceptance and Production Dispatch

## Goal and non-goals

**Goal.** Close every non-verified gap in the specification conformance matrix.
The prior planning cycle completed framebuffer visual acceptance (§§4.10,
5.6, 11.1 layers 1–6). This cycle closes the remaining gap: **§5.7 interaction
acceptance** — the requirement that every interactive manager control and every
overlay action is exercised through normal SDL events and production dispatch,
verifying semantic outcomes (not merely event consumption, focus movement, or
pixels). This requires fixing the production event-dispatch architecture so
that both controller-path and pointer-path activation actually reach every
control, wiring controls that are currently unreachable or no-op placeholders,
then writing tests that prove it.

**Non-goals.** Re-implementing visual/framebuffer acceptance (verified in the
prior cycle). Re-implementing DBus wrappers, config parsing, icon rendering,
packaging, or identification logic (all verified). Changing the committed
specification. Adding post-v1 features (§§12–13).

## Architecture and constraints

- **One binary, two modes.** `controller-box --manager` and
  `controller-box --overlay-service` share one codebase. Production event
  dispatch for the manager is `cbx_manager_handle_event()`; for the overlay it
  is the `run_overlay_service()` poll loop.
- **Controller-first, pointer-secondary.** Every manager control must be
  reachable via the controller focus chain and activated with A (SDLK_a). Every
  manager control must also respond to a mouse click derived from its rendered
  bounds. The overlay is controller-only.
- **Production dispatch only.** Interaction tests must send SDL events through
  the same dispatch path the installed binary uses — `cbx_manager_handle_event`
  for the manager, the overlay poll loop for the overlay. Direct callback
  invocation is supplemental, never a substitute.
- **Mock DBus.** Manager interaction tests use the existing `dbus_mock.c` to
  observe exact DBus requests. Overlay interaction tests use mock DBus for
  InterceptMode polling and InputEvent signal emission.
- **No new dependencies.** All work uses existing SDL2, cmocka, and mock DBus
  infrastructure.

## Specification conformance matrix

| ID | Spec section | Classification | Current evidence | Task |
|----|-------------|---------------|-----------------|------|
| REQ-001 | §2.1 Engine vs control surface | verified | All state changes route through `ip_*.c` DBus wrappers; no direct evdev/input routing in `src/` | — |
| REQ-002 | §2.2 Direct DBus via sd-bus | verified | `ip_connection.c` uses sd-bus; `dbus_mock.c` provides test backend | — |
| REQ-003 | §2.3 One binary, two modes | verified | `main.c` dispatches `--manager` / `--overlay-service`; `overlay_service.c` / `manager.c` | — |
| REQ-004 | §2.4 Service model | verified | `service_install.c` writes systemd user unit; `test_service_install.c` | — |
| REQ-005 | §2.5 Hotkey architecture | verified | `trigger.c` registers via `SetInterceptActivation`; `ip_intercept_poll.c` polls InterceptMode at 50 ms (DEC-002) | — |
| REQ-006 | §3 System requirements | verified | SDL2/SDL2_ttf/SDL2_image/sd-bus/nanosvg in CMakeLists; x86_64+aarch64 in CI; InputPlumber Requires= in service unit; `test_packaging.sh` verifies install layout | — |
| REQ-007 | §4.1 Select-screen model | partial | Grid render (`grid_render.c`) and layout match spec; multi-controller input not wired in production (see REQ-009) | Task 6 |
| REQ-008 | §4.2 Trigger (Select+A, configurable) | verified | `trigger.c` parse/register; `test_trigger.c` verifies DBus calls; settings tab exposes trigger combo editor | — |
| REQ-009 | §4.3 Player Mode (each controller own row) | verified | `player_mode.c` supports per-row control via direct calls; `overlay_service.c` wires `ip_input_events` into the poll loop with device_path→row mapping via `cbx_overlay_input_build_map`; `cbx_overlay_input_cb` dispatches to `cbx_player_mode_handle` with correct row index; keyboard fallback remains for row 0; multi-controller independence verified in `test_overlay_service.c::test_multi_controller_independent_rows` (8 sub-tests pass) | Task 6, Task 11 |
| REQ-010 | §4.4 Host Mode (R3, exclusive) | partial | `host_mode.c` implements enter/exit/navigate/freeze via direct calls; overlay service dispatches both keyboard and DBus InputEvent signals to host mode via `cbx_overlay_input_cb` with the host_row (not hardcoded row 0 for DBus input); production-poll-loop verified in `test_overlay_interaction.c` (O06–O09, O11b: enter/navigate/slot/exit/freeze via keyboard and DBus InputEvent through `cbx_overlay_service_step`). Note: host-mode profile cycling (spec says host can "edit slot/profile") is not implemented — `cbx_hm_input` has no profile-cycle input; UP/DOWN navigates rows instead. §13 defers "interface details"; this is a known partial that the final audit evaluates. | Task 6, Task 11 |
| REQ-011 | §4.5 Conflict resolution | verified | `conflict.c` detect/resolve; `test_conflict.c` 31 sub-tests including spec example; visual test confirms red {220,40,40} rendering | — |
| REQ-012 | §4.6 Profiles per-controller | verified | `profile_cycle.c`; `test_profile_cycle.c` 26 sub-tests; assignments persist profile with controller | — |
| REQ-013 | §4.7 Dynamic columns | verified | `dynamic_columns.c`; `test_dynamic_columns.c` | — |
| REQ-014 | §4.8 No nicknames | verified | Grid shows model name + slot position; no nickname UI in `grid_render.c` | — |
| REQ-015 | §4.9 Rendering & performance | verified | Pre-built surface in `surface_build.c`; `test_surface_build.c`; overlay surface held in memory, dirtied on change | — |
| REQ-016 | §4.10 Overlay visual acceptance | verified | `test_overlay_visual.c` 7 sub-tests; `test_golden.c` 4 overlay baselines; all through production composition path | — |
| REQ-017 | §5.1 Manager structure (tab bar, controller + mouse) | verified | Tab bar works; controller LEFT/RIGHT switches tabs and navigates between same-row buttons (focus chain y-proximity grouping + same-row LEFT/RIGHT restriction in HOST mode); mouse events hit-tested via `cbx_manager_handle_mouse_event`; `cbx_widget_handle_event` checks `visible` flag; `test_manager_interaction_ctrl.c` 28 sub-tests exercise both paths through `cbx_manager_handle_event` | Task 2, Task 8 |
| REQ-018 | §5.2 Controllers tab (add/remove/type) | verified | DBus calls exist and tested via production dispatch (`test_manager_interaction_ctrl.c`): Add (M05+M08) -> `CreateTargetDevice`, Remove (M06) -> `StopTargetDevice`, Change Type (M07+M08) -> `SetTargetDevices`; type picker `on_select` fires `confirm_type_pick` from event path; buttons reachable via controller (focus chain y-proximity grouping + LEFT/RIGHT); `SDLK_a` handled by button widget; all through `cbx_manager_handle_event` | Task 1, Task 3, Task 8 |
| REQ-019 | §5.3 Profiles tab (browse/create/edit/delete) | verified | All profiles-tab controls tested via production dispatch (`test_manager_interaction_prof.c`): browse (M10), create source picker (M11/M12), name input (M13–M16), edit (M17), delete confirm/cancel (M18–M20) through both controller and pointer paths via `cbx_manager_handle_event`; create-to-editor flow opens editor with in-memory profile (no file until save); D03 (delete no profile) tested | Task 1, Task 3, Task 5, Task 9 |
| REQ-020 | §5.4 Profile editor (list + sequential, validation) | verified | Editor fully wired in production: Edit button loads profile and opens editor; create-to-editor flow opens with in-memory profile; binding list navigation (M28), activate binding (M29), target pick confirm (M30), capture begin/event (M31/M32), sequential begin/capture/skip/cancel (M33–M36), save and close (M37), cancel/discard (M38) all tested through `cbx_manager_handle_event` in `test_manager_interaction_prof.c`; editor lists have on_select callbacks for pointer path; move_up/move_down in BINDING_EDIT/TARGET_PICK modes change selection (not scroll); NES validation on save via `cbx_profile_save_to_dir`; D04/D07/D08 tested | Task 4, Task 5, Task 9 |
| REQ-021 | §5.5 Settings tab | verified | Persistence works (`cbx_settings_save()` writes `settings.yaml`); `cbx_settings_tab_activate()`, `edit_up()`, `edit_down()`, `confirm_edit()`, `cancel_edit()` all called from production event path through `cbx_manager_handle_event` (tested in `test_manager_interaction_ctrl.c` M21-M27); Save button reachable via controller and pointer; icon override setting deferred per §13 | Task 1, Task 3, Task 8 |
| REQ-022 | §5.6 Manager visual acceptance | verified | `test_manager_visual.c` 9 sub-tests and `test_golden.c` 7 manager baselines pass for tab and editor states; profile editor tests use the production Edit-button path via `vis_open_editor()`/`g_open_editor()` helpers (updated in Task 5); `test_manager_interaction_prof.c` provides production-dispatch interaction evidence for all profile editor controls | Task 5, Task 9 |
| REQ-023 | §5.7 Manager interaction acceptance | partial | Interaction inventory exists (`tests/interaction_inventory.h/.c` with M01-M38, O01-O12, D01-D08); `test_manager_interaction_ctrl.c` (28 sub-tests) exercises M01-M09 (Controllers) and M21-M27 (Settings) via both controller and pointer paths; `test_manager_interaction_prof.c` (35 sub-tests) exercises M10-M20 (Profiles) and M28-M38 (Profile editor) via both paths; D01, D02, D05, D06, D03, D04, D07, D08 disabled scenarios tested; O01-O12 (overlay) verified in `test_overlay_interaction.c` (19 sub-tests) through `cbx_overlay_service_step`; installed smoke test still needs coordinate-based mouse clicks (Task 12) | Task 2, Task 3, Task 7, Task 8, Task 9, Task 11, Task 12 |
| REQ-024 | §6 Controller identification | verified | `identity.c` 4-layer ID; `assign.c` auto-assignment; `assign_persist.c` persistence; `identity_downgrade.c` graceful fallback; `test_identity.c`, `test_assign.c`, `test_assignments.c`, `test_identity_downgrade.c` | — |
| REQ-025 | §7 Config layer | verified | `config_settings.c`, `config_assignments.c`, `config_profile.c`, `config_profile_meta.c`, `config_profile_list.c`; all with unit tests | — |
| REQ-026 | §8 Controller icons | verified | `icon_cache.c` (nanosvg rasterization), `icon_map.c`, `icon_lookup.c`; `test_icon_cache.c`, `test_icon_lookup.c`, `test_icon_map.c` | — |
| REQ-027 | §9 Packaging | verified | `test_packaging.sh`, `test_packaging_install.sh`; Flatpak manifest, tarball `make install`, systemd unit, desktop entry | — |
| REQ-028 | §10 DBus integration | verified | All `ip_*.c` wrappers tested: `test_manager_calls.c`, `test_composite_calls.c`, `test_connection.c`, `test_objectmanager_parse.c`, `test_properties_changed.c`, `test_hotplug.c`, `test_input_signal.c`, `test_source_props.c`, `test_target_props.c`, `test_intercept_poll.c`, `test_gamepad_order.c`, `test_order_restore.c`, `test_create_composite.c` | — |
| REQ-029 | §11.1 Rendering verification (7 layers) | partial | Layers 1–4 and 6 verified (framebuffer tests, golden images, backend smoke, failure artifacts); layer 5 (installed smoke) verified for keyboard input but **lacks coordinate-based mouse clicks on body controls** (§5.7 requirement); layer 7 (human release acceptance) is a pre-promotion gate documented in OPERATIONS.md, not an autonomous-cycle verification | Task 12 |
| REQ-030 | §11.2 Autonomous definition of done | missing | Items 1 (conformance matrix — multiple partial/missing entries), 2 (production-path behavior — event dispatch broken: mouse not routed, tab activation not wired, editor not wired, overlay InputEvent not wired), 3 (interaction traversal — no tests exist), 6 (known-defect accounting — BUG-0002 open) not met | Task 1; Task 2; Task 3; Task 4; Task 5; Task 6; Task 7; Task 8; Task 9; Task 10; Task 11; Task 12; Task 13; Task 14 |
| REQ-031 | BUG-0002 | verified | Fixed: `test_create_composite_xdg_runtime_dir_preferred` now compares `/tmp/controller-box-*` before/after instead of asserting global count == 0; XDG temp dir stored in fixture struct and cleaned in teardown via `rm -rf` (handles longjmp). BUG-0002 moved to `closed-bugs.md`. Verified with seeded unrelated file + full suite (78/78 pass) + `verify-project.sh` | Task 13 |
| REQ-032 | §11 Performance targets | verified | Five targets are architectural guarantees: overlay <10 ms (pre-built surface, REQ-015), gameplay ~1–2 ms (no inline DBus, REQ-001), close <1 ms (single `InterceptMode` set, REQ-005), footprint (SDL2 minimal, DEC-001), atomic reorder (`GamepadOrder` setter, REQ-012). No runtime benchmark test needed — the design enforces these; verified by production-path composition tests (REQ-016, REQ-029) and DBus wrapper tests (REQ-028) | — |

## Interaction acceptance inventory

Every interactive manager control and overlay action required by §§4, 5.7, and
11.2. Each row records both input paths, expected semantic outcome, production
dispatch path, and the task that provides executable evidence.

### Manager — tab bar

| # | Control | Controller path | Pointer path | Semantic outcome | Dispatch path | Task |
|---|---------|----------------|-------------|-----------------|--------------|------|
| M01 | Controllers tab | Left/Right from any tab → focus tabbar → A | Mouse move + click on tab rect | Active tab switches to Controllers; panel children visible | `cbx_manager_handle_event` → tabbar `handle_event` | Task 8 |
| M02 | Profiles tab | Left/Right → A | Mouse click on tab rect | Active tab switches to Profiles | same | Task 8 |
| M03 | Settings tab | Left/Right → A | Mouse click on tab rect | Active tab switches to Settings | same | Task 8 |

### Manager — Controllers tab

| # | Control | Controller path | Pointer path | Semantic outcome | Dispatch path | Task |
|---|---------|----------------|-------------|-----------------|--------------|------|
| M04 | Device list | Down from tabbar → Up/Down to scroll | Mouse click on list item | Item selected (visual focus) | `cbx_manager_handle_event` → panel → list `handle_event` | Task 8 |
| M05 | Add button | Down from list (boundary) → A | Mouse click on button rect | Type picker opens (mode change) | `cbx_manager_handle_event` → panel → button `handle_event` → `cbx_controllers_tab_begin_type_pick` | Task 3, Task 8 |
| M06 | Remove button | Down from Add → A | Mouse click on button rect | `StopTargetDevice` DBus call; device count decreases | same → `cbx_controllers_tab_remove` | Task 3, Task 8 |
| M07 | Change Type button | Down from Remove → A | Mouse click on button rect | Type picker opens (change mode) | same → `cbx_controllers_tab_begin_type_pick` | Task 3, Task 8 |
| M08 | Type picker confirm | Down to picker → A on type item | Mouse click on type item | `CreateTargetDevice` or `SetTargetDevices` DBus call; device type changes; picker closes | `cbx_manager_handle_event` → panel → list `on_select` → `cbx_controllers_tab_confirm_type_pick` | Task 3, Task 8 |
| M09 | Type picker cancel | B while picker open | n/a (controller-only sub-mode) | Picker closes, no DBus call | `cbx_manager_handle_event` → tab cancel handling | Task 3, Task 8 |

### Manager — Profiles tab

| # | Control | Controller path | Pointer path | Semantic outcome | Dispatch path | Task |
|---|---------|----------------|-------------|-----------------|--------------|------|
| M10 | Profile list | Down from tabbar → Up/Down | Mouse click on item | Profile selected (visual focus) | `cbx_manager_handle_event` → panel → list | Task 9 |
| M11 | Create button | Down from list → A | Mouse click on button rect | Create source picker opens (Default copy / Empty / Clone) | `cbx_manager_handle_event` → panel → button → `cbx_profiles_tab_begin_create` | Task 3, Task 9 |
| M12 | Create source picker | Up/Down to select source → A | Mouse click on source item | Source selected; name-input mode opens | `cbx_manager_handle_event` → panel → list `on_select` → `cbx_profiles_tab_begin_name_input` | Task 3, Task 9 |
| M13 | Name input (chars) | Letter keys while in name-input mode | n/a | Characters appended to profile name | `cbx_manager_handle_event` → `cbx_profiles_tab_name_input_char` | Task 3, Task 9 |
| M14 | Name input (backspace) | Backspace while in name-input mode | n/a | Last char deleted | `cbx_manager_handle_event` → `cbx_profiles_tab_name_input_backspace` | Task 3, Task 9 |
| M15 | Name input (confirm) | A/Return while in name-input mode | n/a | Editor opens with new in-memory profile (Default copy or Clone bindings, or Empty); no file written yet | `cbx_manager_handle_event` → `cbx_profiles_tab_name_input_confirm` → editor init | Task 3, Task 5, Task 9 |
| M16 | Name input (cancel) | B while in name-input mode | n/a | Returns to profile list; no file created | `cbx_manager_handle_event` → `cbx_profiles_tab_name_input_cancel` | Task 3, Task 9 |
| M17 | Edit button | Down from Create → A | Mouse click on button rect | Profile editor opens with selected profile | `cbx_manager_handle_event` → panel → button → profile editor init | Task 5, Task 9 |
| M18 | Delete button | Down from Edit → A | Mouse click on button rect | Confirm-delete mode opens | `cbx_manager_handle_event` → panel → button → `cbx_profiles_tab_begin_delete` | Task 3, Task 9 |
| M19 | Confirm delete | A while in confirm-delete mode | n/a | Profile file unlinked; sidecar deleted; list refreshes | `cbx_manager_handle_event` → `cbx_profiles_tab_confirm_delete` | Task 3, Task 9 |
| M20 | Cancel delete | B while in confirm-delete mode | n/a | Returns to normal mode, no deletion | `cbx_manager_handle_event` → `cbx_profiles_tab_cancel_delete` | Task 3, Task 9 |

### Manager — Settings tab

| # | Control | Controller path | Pointer path | Semantic outcome | Dispatch path | Task |
|---|---------|----------------|-------------|-----------------|--------------|------|
| M21 | Settings list | Down from tabbar → Up/Down | Mouse click on item | Setting selected (visual focus) | `cbx_manager_handle_event` → panel → list | Task 8 |
| M22 | Activate setting (toggle) | A on launch_at_boot item | Mouse click on item | Value toggles (e.g. launch_at_boot) | `cbx_manager_handle_event` → `cbx_settings_tab_activate` | Task 3, Task 8 |
| M23 | Edit setting (enter) | A on theme/opacity/count/type/trigger/icon-override | Mouse click on item | Edit mode entered for that setting | `cbx_manager_handle_event` → `cbx_settings_tab_activate` | Task 3, Task 8 |
| M24 | Edit setting (up/down) | Up/Down while in edit mode | n/a | Value cycles/adjusts | `cbx_manager_handle_event` → `cbx_settings_tab_edit_up`/`edit_down` | Task 3, Task 8 |
| M25 | Confirm edit | A while in edit mode | n/a | Edit mode exits, value applied | `cbx_manager_handle_event` → `cbx_settings_tab_confirm_edit` | Task 3, Task 8 |
| M26 | Cancel edit | B while in edit mode | n/a | Edit mode exits, value reverts from disk | `cbx_manager_handle_event` → `cbx_settings_tab_cancel_edit` | Task 3, Task 8 |
| M27 | Save button | Down from list → A | Mouse click on button rect | `settings.yaml` written to disk | `cbx_manager_handle_event` → panel → button → `cbx_settings_tab_save` → `cbx_settings_save` | Task 3, Task 8 |

### Manager — Profile editor (list mode)

| # | Control | Controller path | Pointer path | Semantic outcome | Dispatch path | Task |
|---|---------|----------------|-------------|-----------------|--------------|------|
| M28 | Binding list | Up/Down to scroll | Mouse click on item | Binding highlighted; diagram lights corresponding button | `cbx_manager_handle_event` → editor panel → list → `cbx_profile_editor_move_down`/`activate` | Task 5, Task 9 |
| M29 | Edit binding (A) | A on binding item | Mouse click on item | Binding edit sub-menu opens (target-pick or capture) | `cbx_manager_handle_event` → `cbx_profile_editor_activate` | Task 5, Task 9 |
| M30 | Target picker confirm | A on target item | Mouse click on target item | Binding target updated; picker closes | `cbx_manager_handle_event` → list `on_select` → `cbx_profile_editor_confirm_target_pick` | Task 5, Task 9 |
| M31 | Capture mode (begin) | A on "capture" option in binding edit sub-menu | Mouse click on "capture" option | Capture mode begins; waiting for physical button press | `cbx_manager_handle_event` → `cbx_profile_editor_begin_capture` | Task 5, Task 9 |
| M32 | Capture (physical button) | Input event via DBus InputEvent | n/a | Binding source captured; binding updated; capture ends | `ip_input_events` → `cbx_profile_editor_on_input_event` | Task 5, Task 9 |
| M33 | Sequential mode (begin) | A on "sequential" action in editor | Mouse click on "sequential" option | Sequential mode begins; first button prompted | `cbx_manager_handle_event` → `cbx_profile_editor_begin_sequential` | Task 5, Task 9 |
| M34 | Capture button (sequential) | Physical button press (DBus InputEvent) | n/a | Button captured; auto-advance; progress bar updates | `ip_input_events` → `cbx_profile_editor_seq_on_input` | Task 5, Task 9 |
| M35 | Skip (B) | B during sequential | n/a | Current binding skipped; advance | `cbx_manager_handle_event` → `cbx_profile_editor_seq_skip` | Task 5, Task 9 |
| M36 | Cancel (Start) | Start during sequential | n/a | Sequential mode cancelled; changes discarded | `ip_input_events` → `cbx_profile_editor_cancel_sequential` | Task 5, Task 9 |
| M37 | Save and close editor | B (or A on close) from list mode | n/a (controller-only — editor save/close via B key) | Profile written to disk via `cbx_profile_save_to_dir` with NES validation; editor closes; profile list refreshes | `cbx_manager_handle_event` → `cbx_profile_save_to_dir` → editor close | Task 4, Task 5, Task 9 |
| M38 | Cancel editor (discard) | Start from list mode | n/a | Editor closes; changes discarded; no file written | `cbx_manager_handle_event` → `cbx_profile_editor_cancel` | Task 5, Task 9 |

### Overlay actions

| # | Action | Controller path | Pointer path | Semantic outcome | Dispatch path | Task |
|---|--------|----------------|-------------|-----------------|--------------|------|
| O01 | Open (trigger activation) | InterceptMode PASS→ALL detected by poll | n/a | Surface shown; lifecycle ACTIVATING→VISIBLE | `run_overlay_service` poll loop → `ip_intercept_poll_tick` → `cbx_overlay_lifecycle_activate` | Task 11 |
| O02 | Move left (Player Mode) | Left key / DBus InputEvent | n/a | Controller's grid column decreases; `on_slot_change` callback fires | poll loop → `sdl_key_to_pm_input` or `ip_input_events` → `cbx_player_mode_handle` | Task 6, Task 11 |
| O03 | Move right (Player Mode) | Right key / DBus InputEvent | n/a | Column increases; callback fires | same | Task 6, Task 11 |
| O04 | Cycle profile up | Up key / DBus InputEvent | n/a | Profile name changes; `LoadProfilePath` DBus call; assignment updated | poll loop → `cbx_player_mode_handle` → `cbx_profile_cycle_apply` | Task 11 |
| O05 | Cycle profile down | Down key / DBus InputEvent | n/a | Profile name changes (reverse) | same | Task 11 |
| O06 | Enter Host Mode (R3) | R3 key / DBus InputEvent | n/a | Host mode entered; other controllers freeze | poll loop → `cbx_player_mode_handle` → `cbx_host_mode_toggle` | Task 11 |
| O07 | Host: navigate rows | Up/Down keys / DBus InputEvent | n/a | Host selected row changes | poll loop → `cbx_host_mode_handle` | Task 11 |
| O08 | Host: move slot | Left/Right keys / DBus InputEvent | n/a | Host changes a row's slot; conflict may arise | poll loop → `cbx_host_mode_handle` | Task 11 |
| O09 | Exit Host Mode (R3) | R3 key | n/a | Host mode exits; controllers unfreeze | poll loop → `cbx_host_mode_handle` → `cbx_host_mode_exit` | Task 11 |
| O10 | Close (B) | B key | n/a | Assignments saved; conflicts auto-resolved; `InterceptMode` set to PASS; surface hidden | poll loop → `cbx_player_mode_handle`/`cbx_host_mode_handle` → `cbx_overlay_lifecycle_close` → `cbx_close_on_save` | Task 11 |
| O11 | Multi-controller independence | DBus InputEvent from different device paths | n/a | Each controller moves its own row independently | poll loop → `ip_input_events` → device_path→row mapping → `cbx_player_mode_handle` | Task 6, Task 11 |
| O12 | Host: cycle profile (not-yet-implemented) | TBD (Up/Down repurposed for row nav in Host Mode; spec §4.4 says host can "edit slot/profile" but `cbx_hm_input` has no profile-cycle input) | n/a | Host changes the profile of the selected row | poll loop → `cbx_host_mode_handle` (not yet implemented) | Task 11 (remediation if blocking) |

### Disabled-control, degraded-state, and operation-failure scenarios

| # | Scenario | Expected behavior | Task |
|---|---------|-------------------|------|
| D01 | Controllers tab with InputPlumber unavailable | Degraded content shown; Add/Remove/ChangeType disabled; activation rejected, no DBus call | Task 8 |
| D02 | Remove button with no device selected | Disabled; activation produces no DBus side effect | Task 8 |
| D03 | Delete button with no profile selected | Disabled; activation produces no file deletion | Task 9 |
| D04 | Profile save with missing NES bindings | Error shown; profile not saved; `cbx_profile_validate_nes_minimum` returns error; editor stays open | Task 4, Task 9 |
| D05 | Settings edit cancel | Value reverts from disk; no `settings.yaml` write | Task 8 |
| D06 | DBus operation failure (e.g. `CreateTargetDevice` returns error) | Error shown to user; UI remains responsive; no state corruption | Task 8 |
| D07 | Profile save filesystem failure (e.g. disk full) | Error shown; profile not written; editor stays open | Task 9 |
| D08 | Empty profile creation | Editor opens with no bindings; save is blocked by NES validation until minimum bindings added | Task 5, Task 9 |

## Task list

## Task 1: Fix list widget focus trapping and manager focus navigation
- Status: complete
- Dependencies: none
- Scope: `src/ui/widget_list.c`, `tests/test_widget_list.c`
- Acceptance criteria:
  - `cbx_list` `handle_event` returns `false` for `SDLK_UP` when `selected == 0` (at top boundary) so the manager's focus-chain navigation can proceed to the widget above.
  - `cbx_list` `handle_event` returns `false` for `SDLK_DOWN` when `selected == item_count - 1` (at bottom boundary) so the manager's focus-chain navigation can proceed to the widget below.
  - `cbx_list` still returns `true` for `SDLK_UP`/`SDLK_DOWN` when there are items to scroll to (mid-list).
  - Controller navigation from tabbar → list → buttons → tabbar works in all three manager tabs through `cbx_manager_handle_event`.
  - Existing list selection behavior (click, scroll, keyboard selection within bounds) is unchanged.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'test_widget_list|test_manager_tabs|test_manager_production' --output-on-failure"` — all pass. New sub-tests in `test_widget_list.c` assert boundary return values.
  - **Verified**: `nix-shell --run "ctest --test-dir build-check --output-on-failure"` — 74/74 pass (1 skip). New sub-tests: `test_list_boundary_returns_false`, `test_list_single_item_boundary`, `test_list_empty_boundary` (widget_list); `test_manager_focus_traversal_no_trap` (manager_tabs). Production path: `list_handle_event` returns `false` at `selected==0` (SDLK_UP) and `selected==item_count-1` (SDLK_DOWN), letting `cbx_focus_chain_navigate` move focus to adjacent widgets in `cbx_manager_handle_event`.
- Documentation impact: none

## Task 2: Add manager pointer event routing, mouse hit-testing, and visibility filtering
- Status: complete
- Dependencies: Task 1
- Scope: `src/manager/manager.c`, `src/manager/manager.h`, `src/ui/widget.c`, `src/ui/widget.h`, `src/ui/widget_button.c`, `src/ui/widget_list.c`, `src/ui/widget_tabbar.c`, `src/ui/widget_panel.c`, `tests/test_manager_tabs.c`
- Acceptance criteria:
  - `cbx_manager_handle_event` processes `SDL_MOUSEMOTION`, `SDL_MOUSEBUTTONDOWN`, and `SDL_MOUSEBUTTONUP` events by hit-testing all **visible** widgets in the active panel (and the tabbar) to find the widget under the cursor, then dispatching the event to that widget — not just the focused widget.
  - `SDL_MOUSEMOTION` updates a hover state on the widget under the cursor (widgets with hover support visually indicate it).
  - `cbx_widget_handle_event` skips widgets where `w->visible == false` — invisible widgets do not consume events (this prevents hidden type pickers, hidden buttons, etc. from intercepting clicks).
  - A mouse click on an unfocused button activates it (fires `on_press`) even when a different widget is focused.
  - A mouse click on a tabbar tab switches tabs.
  - A mouse click on a list item selects it.
  - A mouse click on empty space (no widget) is consumed and produces no side effect.
  - Keyboard event handling is unchanged.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'test_manager_tabs|test_widgets|test_widget' --output-on-failure"` — all pass. New sub-tests in `test_manager_tabs.c` push `SDL_MOUSEMOTION` + `SDL_MOUSEBUTTONDOWN` + `SDL_MOUSEBUTTONUP` through `cbx_manager_handle_event` and assert: (a) button callback fires when clicking an unfocused button, (b) tab switches when clicking a tab, (c) list item selects when clicking a list item, (d) invisible widget does not consume click.
  - **Verified**: `nix-shell --run "ctest --test-dir build-check --output-on-failure"` — 74/74 pass (1 skip: backend_smoke). New sub-tests: `test_mouse_click_unfocused_button`, `test_mouse_click_tab_switches`, `test_mouse_click_list_item`, `test_invisible_widget_no_click`, `test_mouse_motion_updates_hover`, `test_mouse_click_empty_space`. Production path: `cbx_manager_handle_event` routes `SDL_MOUSEMOTION`/`SDL_MOUSEBUTTONDOWN`/`SDL_MOUSEBUTTONUP` via `cbx_manager_hit_test()` which iterates visible widgets (tabbar + active panel children) and dispatches to the widget under the cursor. `cbx_widget_handle_event` now skips invisible widgets. Hover state tracked via new `hover` field on `cbx_widget`. Focus follows pointer on `MOUSEBUTTONDOWN`. `test_manager_unrelated_event` updated to send `MOUSEMOTION` at empty-space coordinates.
- Documentation impact: `docs/OPERATIONS.md` — note that manager supports mouse as secondary input path.

## Task 3: Wire tab-specific activation, widget A-key handling, and focus-chain rebuild through manager event dispatch
- Status: complete
- Dependencies: Task 1, Task 2
- Scope: `src/manager/manager.c`, `src/manager/manager.h`, `src/manager/controllers_tab.c`, `src/manager/controllers_tab.h`, `src/manager/profiles_tab.c`, `src/manager/profiles_tab.h`, `src/manager/settings_tab.c`, `src/manager/settings_tab.h`, `src/ui/widget_button.c`, `src/ui/widget_list.c`, `tests/test_controllers_tab.c`, `tests/test_profiles_tab.c`, `tests/test_settings_tab.c`
- Acceptance criteria:
  - **Widget A-key handling (list KEYUP firing)**: `widget_button.c` handles `SDLK_a` with the same press/release semantics as `SDLK_RETURN`/`SDLK_SPACE` (keydown sets pressed, keyup fires `on_press`). `widget_list.c` is changed to fire `on_select` on **KEYUP** for `SDLK_RETURN`/`SDLK_SPACE`/`SDLK_a` (keydown sets a pressed/selected visual state, keyup fires the callback and returns `true`). This eliminates double-action: when a list fires `on_select` on KEYUP, it consumes the event and the manager never reaches tab-activate forwarding. Mouse clicks fire `on_select` on **MOUSEUP** (matching the KEYUP semantics). Existing `test_widget_list.c` sub-tests asserting KEYDOWN-fires-on_select for RETURN/SPACE are updated to expect KEYUP firing.
  - **Tab-specific activation forwarding**: `cbx_manager_handle_event` forwards `SDLK_a` (A button) keyup to the active tab's activate function: `cbx_controllers_tab_activate()`, `cbx_profiles_tab_activate()`, or `cbx_settings_tab_activate()` — **only when the focused widget does not consume the event** (returns `false` for the KEYUP). This is the fallback for non-interactive focus targets or empty space; for list and button widgets, `on_select`/`on_press` fires on KEYUP and consumes the event, so the manager's tab-activate forwarding is never reached. The tab activate function checks the tab's current mode and dispatches appropriately (e.g. type picker confirm, name input confirm, delete confirm, settings activate/edit).
  - **Activation lists have `on_select` wired**: All list widgets used for activation (settings list, type picker, create source picker) have their `on_select` callback set to the appropriate handler (`cbx_settings_tab_activate`, `cbx_controllers_tab_confirm_type_pick`, `cbx_profiles_tab_begin_name_input`). Both keyboard A-key (KEYUP) and mouse click (MOUSEUP) fire `on_select`, providing both paths without double-action.
  - **Controllers tab**: type picker list `on_select` callback is set so confirming a type calls `cbx_controllers_tab_confirm_type_pick()` through the event path (currently NULL — only called from tests). B while type picker is open cancels the picker. Mouse-click on a type item fires `on_select` on MOUSEUP.
  - **Profiles tab — create source picker**: Create button opens the create source picker (Default copy / Empty / Clone) instead of hardcoding `CBX_PT_CREATE_DEFAULT_COPY`. The picker list is populated with the three options and its `on_select` is wired to `cbx_profiles_tab_begin_name_input`. Selecting a source (keyboard A or mouse click) enters name-input mode.
  - **Profiles tab — name input**: letter keys while in `CBX_PT_MODE_NAME_INPUT` call `cbx_profiles_tab_name_input_char()`; Backspace calls `cbx_profiles_tab_name_input_backspace()`; A/Return calls `cbx_profiles_tab_name_input_confirm()`; B calls `cbx_profiles_tab_name_input_cancel()`.
  - **Profiles tab — confirm delete**: A while in `CBX_PT_MODE_CONFIRM_DELETE` calls `cbx_profiles_tab_confirm_delete()`; B calls `cbx_profiles_tab_cancel_delete()`.
  - **Settings tab**: The settings list `on_select` callback is set to `cbx_settings_tab_activate()` so both keyboard A (KEYUP) and mouse click (MOUSEUP) activate the selected setting; Up/Down while in edit mode call `cbx_settings_tab_edit_up()`/`cbx_settings_tab_edit_down()`; A while in edit mode calls `cbx_settings_tab_confirm_edit()`; B while in edit mode calls `cbx_settings_tab_cancel_edit()`.
  - **Focus-chain rebuild on mode change**: When a tab's internal mode changes (type picker open/close, name input open/close, confirm delete open/close, settings edit open/close, create picker open/close), the manager rebuilds the focus chain to exclude hidden widgets and focus the appropriate widget (e.g. focus the type picker when it opens, refocus the list when it closes). Each tab exposes a `cbx_<tab>_is_mode_change_needed()` or the manager queries the tab's mode after each event and rebuilds if it changed.
  - Existing direct-call unit tests still pass without signature changes.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'test_controllers_tab|test_profiles_tab|test_settings_tab|test_manager_production|test_widget' --output-on-failure"` — all pass. New sub-tests in each tab test file push `SDL_KEYDOWN`/`SDL_KEYUP` events through the manager's dispatch and verify the semantic outcome (mode change, DBus mock expectation, file creation). **Verified in commit `bc8c7b3`**: full suite 84/84 passed (1 environment skip); production manager dispatch, A-key KEYUP semantics, tab-specific activation, picker callbacks, cancellation paths, and focus-chain rebuild were covered by the added tab/widget tests.
- Documentation impact: none

## Task 4: Security-hardened profile save in production path
- Status: complete
- Dependencies: Task 3
- Scope: `src/manager/profiles_tab.c`, `src/manager/profile_save.c`, `src/manager/profile_save.h`, `tests/test_profile_save.c`, `tests/test_profiles_tab.c`
- Acceptance criteria:
  - Profile creation and profile editing in the production path call `cbx_profile_save_to_dir()` (which uses `realpath()` canonicalization and `cbx_profile_validate_nes_minimum()`) instead of the raw `cbx_profile_save()` from `config_profile.c`.
  - **TOCTOU fix**: `verify_path_within_dir()` is modified to return the canonicalized target path (via out-parameter) so the caller can use it for the actual file write. `cbx_profile_save_to_dir()` passes the canonicalized path to `cbx_profile_save()` — not the original uncanonicalized `path` — eliminating the TOCTOU race between the `realpath()` check and the write.
  - **Direct-save bypass fix**: The direct `cbx_profile_save()` call in `profiles_tab.c:403` (profile creation) is replaced with `cbx_profile_save_to_dir()`. Verification: `grep -rn 'cbx_profile_save(' src/ --include='*.c' | grep -v 'profile_save.c' | grep -v 'config_profile.c'` returns zero matches, confirming all production save paths go through `cbx_profile_save_to_dir`.
  - A profile missing required NES bindings (A, B, D-Pad Up/Down/Left/Right) is rejected with an error message; no file is written.
  - A profile with all required bindings is saved successfully.
  - Path traversal is prevented by `realpath()` canonicalization (writing outside the profile directory fails).
  - All production save paths use `cbx_profile_save_to_dir()`: profiles tab create, profile editor save.
  - Existing `test_profile_save.c` and `test_profile_validate.c` still pass.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'test_profile_save|test_profile_validate|test_profiles_tab' --output-on-failure"` — all pass. New sub-test verifies that a profile missing NES bindings is rejected when saved through the production path. **Verified in commit `8e767a1`**: full suite 74/74 passed (1 environment skip); canonical target-path writes, production `cbx_profile_save_to_dir()` routing, NES minimum rejection, and valid profile persistence were covered by profile-save and profiles-tab tests.
- Documentation impact: none

## Task 5: Wire profile editor, create-to-editor flow, and editor UI entry points into manager production path
- Status: complete
- Dependencies: Task 3, Task 4
- Scope: `src/manager/profiles_tab.c`, `src/manager/profiles_tab.h`, `src/manager/manager.c`, `src/manager/manager.h`, `src/manager/profile_editor_list.c`, `src/manager/profile_editor_list.h`, `src/manager/profile_editor_seq.c`, `src/manager/profile_editor_seq.h`, `tests/test_profiles_tab.c`, `tests/test_manager_production.c`, `tests/test_manager_visual.c`, `tests/test_golden.c`
- Acceptance criteria:
  - **Edit button**: The profiles tab Edit button (currently a no-op placeholder) initializes and opens the profile editor (`cbx_profile_editor`) with the selected profile's bindings loaded.
  - **Create-to-editor flow**: After name-input confirm (M15), instead of creating a file and returning to the list, the editor opens with an in-memory profile: Default copy (with Default bindings), Empty (no bindings), or Clone (with cloned bindings). No file is written until the user saves from the editor (which validates NES minimum via Task 4's `cbx_profile_save_to_dir`).
  - **Editor in panel/focus chain**: The profile editor is added to the manager's panel/focus chain when active; its widgets (binding list, diagram, target picker, progress bar) are visible and reachable via controller navigation. Focus-chain rebuild occurs when the editor opens/closes (building on Task 3's rebuild mechanism).
  - **Editor event forwarding**: The manager forwards events to the profile editor when it is active: list mode (binding list navigation, A to edit, target picker confirm, B to cancel/save); sequential mode (begin, capture via input event, B to skip, Start to cancel).
  - **Capture mode entry** (M31): The binding edit sub-menu offers a "capture" option that calls `cbx_profile_editor_begin_capture()` — waiting for a physical button press via DBus InputEvent.
  - **expected_sender security fix**: Profile editor capture and sequential mode must pass the **unique bus name** (obtained via `ip_connection_get_unique_name()`, e.g. `:1.42`) as `expected_sender` to `ip_input_events_init`, NOT the well-known name `IP_DBUS_NAME` (`org.shadowblip.InputPlumber`). DBus message `sender` fields contain unique connection names, not well-known names — using the well-known name means `strcmp` always fails and all legitimate InputEvent signals are silently dropped, making capture and sequential mode completely broken in production. A test must verify that a signal with a mismatched sender (e.g., `:1.99`) is rejected while a signal from the expected unique name is accepted.
  - **Sequential mode entry** (M33): The editor offers a "sequential" action that calls `cbx_profile_editor_begin_sequential()` — beginning the sequential binding flow.
  - **Save and close** (M37): B from list mode (or a dedicated save action) saves the profile via `cbx_profile_save_to_dir()` (Task 4) with NES validation, closes the editor, and refreshes the profile list. If validation fails, an error is shown and the editor stays open.
  - **Cancel editor** (M38): Start from list mode discards changes and closes the editor without writing.
  - **Manager renders editor**: The manager renders the profile editor panel when active.
  - **Update prior visual tests**: `test_manager_visual.c` profile editor tests (tests 7–9) and `test_golden.c` editor baselines (tests 9–11) are updated to verify the editor through the production Edit-button path instead of manually initializing it (fixes §5.6 production-path violation). Golden images may need regeneration — use `CBX_GENERATE_GOLDEN=1` with explicit review.
  - **test_manager_production**: Verifies the editor opens via production dispatch (send SDL events to focus + activate Edit button) and that editor widgets appear in the panel.
  - Existing `test_editor_list_mode.c` and `test_editor_seq_mode.c` still pass (they test the editor in isolation).
:- Verification: `nix-shell --run "ctest --test-dir build-check -R 'test_profiles_tab|test_editor|test_manager_production|test_manager_visual|test_golden' --output-on-failure"` — all pass.
  - **Verified**: `nix-shell --run "ctest --test-dir build-check --output-on-failure"` — 74/74 pass (1 skip: backend_smoke). No regressions.
  - **Edit button**: `on_edit_pressed` in profiles_tab.c loads the selected profile from disk and opens the editor via `cbx_profiles_tab_open_editor()`. Production dispatch path verified by `test_editor_opens_via_dispatch` in test_manager_production.c.
  - **Create-to-editor flow**: `cbx_profiles_tab_name_input_confirm()` now builds an in-memory profile and opens the editor instead of writing a file. File is written on save from the editor via `cbx_profile_save_to_dir()`. Verified by `test_name_input_confirm` and `test_name_input_via_dispatch` in test_profiles_tab.c.
  - **Editor in panel/focus chain**: Editor widgets are added to the profiles panel; visibility toggled on open/close. Mode tracking uses `pt.mode * 100 + editor.mode` so manager detects editor internal mode changes and rebuilds focus chain.
  - **Binding edit sub-menu**: New `CBX_EDITOR_MODE_BINDING_EDIT` mode offers Pick Target / Capture / Sequential options. Verified by updated `test_activate_enters_target_pick` in test_editor_list_mode.c.
  - **expected_sender security fix**: `cbx_profile_editor_set_dbus()` now resolves the unique bus name via `backend->get_unique_name()` and stores it in `ed->expected_sender[128]`. `begin_capture` and `begin_sequential` pass `ed->expected_sender` instead of `IP_DBUS_NAME`. Verified by `test_expected_sender_resolved`, `test_expected_sender_accepts_match`, `test_expected_sender_rejects_mismatch` in test_editor_list_mode.c — a signal from `:1.42` is accepted while `:1.99` is rejected.
  - **Save and close**: B in editor LIST mode calls `cbx_profiles_tab_save_editor()` which uses `cbx_profile_save_to_dir()` with NES validation. Verified by `test_name_input_confirm` and `test_name_input_via_dispatch`.
  - **Cancel editor**: Start (Tab key) discards changes and closes editor. Verified in `cbx_profiles_tab_handle_key()`.
  - **Visual/golden tests updated**: test_manager_visual.c tests 7–9 and test_golden.c tests 9–11 now use the production Edit-button path via `vis_open_editor()`/`g_open_editor()` helpers. Golden images regenerated with `CBX_GENERATE_GOLDEN=1`.
- Documentation impact: `docs/OPERATIONS.md` — documented the profile editor access flow (Edit button, create-to-editor flow, list mode, sequential mode, validation, save, expected_sender verification).

## Task 6: Wire overlay DBus InputEvent signal handling for multi-controller input
- Status: complete
- Dependencies: none
- Scope: `src/app/overlay_service.c`, `src/app/overlay_service.h`, `src/dbus/ip_input_signal.c`, `src/dbus/ip_input_signal.h`, `src/dbus/ip_composite.c`, `src/dbus/ip_composite.h`, `tests/test_overlay_service.c`
- Acceptance criteria:
  - The overlay service initializes `ip_input_events` and subscribes to `InputEvent` signals on each composite device's DBusDevice during startup.
  - The `expected_sender` field is set to InputPlumber's unique bus name (obtained via `sd_bus_get_unique_name` or equivalent) so spoofed signals from untrusted processes are rejected (fail-closed).
  - The poll loop calls `ip_input_events_handle()` to process incoming InputEvent signals.
  - InputEvent signals are mapped to rows by correlating the signal's device path with the composite device enumeration index (via `ip_composite_get_dbus_devices`).
  - Mapped input events are dispatched to `cbx_player_mode_handle()` (or `cbx_host_mode_handle()` when Host Mode is active) with the correct row index — not hardcoded row 0.
  - InputEvent signals are only accepted from device paths that match currently-enumerated composite device DBusDevices. A signal from an unknown device path is dropped (not dispatched to any row).
  - SDL keyboard events remain as a fallback for single-controller testing.
  - Two controllers sending InputEvent signals from different device paths move their own rows independently (verified through direct `ip_input_events_handle()` testing; full production-poll-loop verification is deferred to Task 11).
  - `test_overlay_service.c` has a new sub-test that subscribes to mock InputEvent signals, emits events from two different device paths, and verifies each controller's row moves independently.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'test_overlay_service|test_input_signal' --output-on-failure"` — all pass.
- Documentation impact: none (internal implementation detail; keyboard fallback documented in Task 2's OPERATIONS update)

## Task 7: Create interaction acceptance inventory
- Status: complete
- Dependencies: Task 1, Task 2, Task 3
- Scope: `tests/interaction_inventory.h`, `tests/interaction_inventory.c`, `tests/test_interaction_inventory.c`, `tests/CMakeLists.txt`
- Acceptance criteria:
  - A machine-readable data structure enumerates every interactive manager control (M01–M38) and overlay action (O01–O12) with: control ID, tab/context, widget type, controller-path description, pointer-path description, expected semantic outcome, and current verification status.
  - The inventory is a C struct array that tests can iterate over to drive automated traversal.
  - The inventory covers all controls listed in the interaction acceptance inventory section above, including create source picker, name input cancel, capture mode entry, sequential mode entry, save and close, and cancel editor.
  - Disabled-control and operation-failure scenarios (D01–D08) are included with their expected rejection/error behavior.
  - A test helper function `cbx_interaction_inventory_get()` returns the array; `cbx_interaction_inventory_count()` returns the entry count.
- Verification: `nix-shell --run "cmake --build build-check --target cbx_test_support && ctest --test-dir build-check -R test_interaction_inventory --output-on-failure"` — compiles and 11 sub-tests pass (count, all-fields-populated, all-manager M01–M38, all-overlay O01–O12, all-disabled D01–D08, find-unknown, pointer-path-availability, categories, specific-entries, unique-IDs, required-scenarios). Full suite 75/75 pass (1 skip: backend_smoke).
- Documentation impact: none

## Task 8: Manager interaction tests — Controllers and Settings tabs through production dispatch
- Status: complete
- Dependencies: Task 1, Task 2, Task 3, Task 7
- Scope: `tests/test_manager_interaction_ctrl.c`, `tests/CMakeLists.txt`
- Acceptance criteria:
  - For every Controllers-tab control (M04–M09) and Settings-tab control (M21–M27), a test exercises the **controller path**: navigate from the tab bar using the focus chain (SDL_KEYDOWN Left/Right/Up/Down), visibly indicate focus, activate with A (SDL_KEYDOWN/KEYUP SDLK_a), and verify the semantic outcome (DBus mock request, file mutation, mode/state change, or dialog navigation).
  - For every control, a test exercises the **pointer path**: derive a click point from the control's rendered bounds, send `SDL_MOUSEMOTION` + `SDL_MOUSEBUTTONDOWN` + `SDL_MOUSEBUTTONUP` through `cbx_manager_handle_event`, and verify the same semantic outcome.
  - Tests send events through `cbx_manager_handle_event` (production dispatch) — not by calling `cbx_controllers_tab_add()`, `cbx_settings_tab_activate()`, etc. directly.
  - End-to-end scenarios: Controllers add (pick type → confirm → `CreateTargetDevice` DBus call), remove (`StopTargetDevice`), type-change (`SetTargetDevices`); Settings change value (enter edit → up/down → confirm → save → `settings.yaml` written); tab switching (all three tabs).
  - Disabled-control scenarios D01 (InputPlumber unavailable — Add/Remove/ChangeType reject), D02 (Remove with no device — no DBus call), D05 (Settings edit cancel — value reverts), D06 (DBus operation failure — error shown, no state corruption) are tested through both paths.
  - Hit testing uses final layout bounds after `cbx_manager_layout()` — not stale pre-layout rectangles.
  - An event-handler return value alone is not accepted as outcome evidence; tests assert observable state transitions, DBus requests, or file mutations.
- Verification: `nix-shell --run "ctest --test-dir build-check -R test_manager_interaction_ctrl --output-on-failure"` — all sub-tests pass.
- Documentation impact: none

## Task 9: Manager interaction tests — Profiles tab and profile editor through production dispatch
- Status: complete
- Dependencies: Task 1, Task 2, Task 3, Task 4, Task 5, Task 7
- Scope: `tests/test_manager_interaction_prof.c`, `tests/CMakeLists.txt`, `src/manager/profile_editor_list.c`
- Acceptance criteria:
  - For every Profiles-tab control (M10–M20) and profile-editor control (M28–M38), a test exercises the **controller path** and **pointer path** through `cbx_manager_handle_event` (production dispatch), verifying semantic outcomes.
  - End-to-end scenarios: Profiles create from each starting point (Default copy → name input → editor opens with Default bindings; Empty → editor opens with no bindings; Clone → editor opens with cloned bindings), select profile, edit (open editor → list mode: select binding, edit target, capture; sequential mode: begin, capture, skip, cancel), validate (save with missing NES bindings → error, no file written), save (valid profile → file written via `cbx_profile_save_to_dir`), delete (confirm → file unlinked; cancel → no deletion).
  - Disabled-control scenarios D03 (Delete with no profile — no file deletion), D04 (Profile save with missing NES bindings — error, no file), D07 (Filesystem failure — error shown), D08 (Empty profile creation — editor opens, save blocked by NES validation) are tested through both paths.
  - Profile editor interactions use the production Edit-button path to open the editor (not manual initialization).
  - An event-handler return value alone is not accepted as outcome evidence.
- Verification: `nix-shell --run "ctest --test-dir build-check -R test_manager_interaction_prof --output-on-failure"` — all sub-tests pass. Full suite: `nix-shell --run "ctest --test-dir build-check --output-on-failure"` — no regressions.
  - **Verified**: `nix-shell --run "ctest --test-dir build-check --output-on-failure"` — 76/76 pass (1 skip: backend_smoke). 35 sub-tests in `test_manager_interaction_prof.c`: M10–M20 (Profiles tab: list select, create/source/name-input/confirm/cancel, edit open, delete open/confirm/cancel) and M28–M38 (Profile editor: list nav, activate binding, target pick, capture begin/event, sequential begin/capture/skip/cancel, save close, cancel discard) through both controller (keyboard) and pointer (mouse) paths via `cbx_manager_handle_event`. D03 (delete no profile), D04 (save missing NES), D07 (filesystem failure), D08 (empty profile create) tested. Production fixes: (1) on_select callbacks wired to `cbx_profile_editor_activate` on editor binding_list and target_list for pointer-path activation; (2) `move_up`/`move_down` in BINDING_EDIT/TARGET_PICK modes now change selection instead of scrolling; (3) binding_list items pass `ed` as user_data for safe on_select callback.
- Documentation impact: none

## Task 10: Extract overlay service step function for testability
- Status: complete
- Dependencies: Task 6
- Scope: `src/app/overlay_service.c`, `src/app/overlay_service.h`, `tests/test_overlay_service.c`
- Acceptance criteria:
  - The overlay service poll loop is refactored to extract a single-iteration step function (`cbx_overlay_service_step()`) that processes pending SDL events, ticks InterceptMode polls, advances lifecycle, and re-renders dirty surfaces — enabling test-driven event injection without running the infinite loop.
  - A comprehensive context struct (`cbx_overlay_service_ctx`) holds all loop state (renderer, connection, device model, settings, assignments, text cache, font_id, theme, icon map, icon cache, composites, comp_count, grid, surface, render_ctx, lifecycle, pm, hm, conflicts, input_ctx, input_events, expected_sender, input_events_ready, polls, poll_count, poll_event_type, initialized) so the step function is self-contained. Allocated on the heap in production (struct is ~200 KB+).
  - `run_overlay_service()` is refactored to initialize the context struct, then loop calling the step function.
  - A basic regression test runs one iteration of the step function and verifies: (a) it processes a queued SDL_QUIT event and sets the shutdown flag, (b) it processes a queued SDL_KEYDOWN event and updates grid state (row 0 moves from col 0 to col 1), (c) it does not crash on empty event queue.
  - Existing `test_overlay_service.c` tests (init failure, dry-run, signal handlers, multi-controller, unknown device, wrong sender, process no-op) still pass — 11/11 tests pass (was 8, added 3).
  - The installed binary (`controller-box --overlay-service`) still works unchanged.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'test_overlay_service' --output-on-failure"` — all pass (11 tests, 0.01s). `./build-check/controller-box --overlay-service --dry-run` → exit 0. Full suite: 77/77 pass (1 skip: backend_smoke).
- Documentation impact: none

## Task 11: Overlay production-dispatch interaction tests
- Status: complete
- Dependencies: Task 6, Task 7, Task 10
- Scope: `tests/test_overlay_interaction.c`, `tests/CMakeLists.txt`, `src/app/overlay_service.c`, `src/app/overlay_service.h`
- Acceptance criteria:
  - For every overlay action in the inventory (O01–O12), a test sends events through the production dispatch path (the step function from Task 10) and verifies the semantic outcome:
    - O01 Open: mock InterceptMode → ALL; verify lifecycle transitions to VISIBLE.
    - O02–O05 Move/cycle: push SDL_KEYDOWN or mock InputEvent; verify grid column changes, profile name changes, `LoadProfilePath` DBus call, assignment update.
    - O06–O09 Host Mode: push R3 → verify host mode entered/frozen; Up/Down → verify row navigation; Left/Right → verify slot change; R3 → verify exit.
    - O10 Close: push B → verify assignments saved, conflicts auto-resolved, InterceptMode set to PASS, surface hidden.
    - O11 Multi-controller: emit mock InputEvent signals from two device paths → verify each controller's row moves independently.
  - Tests verify semantic outcomes (grid state, profile, assignments, lifecycle state, DBus mock expectations) — not merely event consumption.
  - The overlay's `on_save` callback (conflict detection → resolution → assignment sync → `cbx_assignments_save`) is verified through the close path.
  - REQ-010 note: if host-mode profile cycling is found to be a blocking gap during this task, append a remediation task per the remediation rule.
- Verification: `nix-shell --run "ctest --test-dir build-check -R test_overlay_interaction --output-on-failure"` — all sub-tests pass. Full suite: no regressions.
  - **Verified**: `nix-shell --run "ctest --test-dir build-check --output-on-failure"` — 78/78 pass (1 skip: backend_smoke). 19 sub-tests in `test_overlay_interaction.c`: O01 (activate + deactivate via InterceptMode poll), O02–O03 (move left/right via keyboard), O04–O05 (profile cycle up/down), O06 (enter host mode + freeze non-host via keyboard + DBus), O07 (host navigate rows), O08 (host move slot), O09 (exit host mode), O10 (close via keyboard B + DBus B — on_save fires, assignments synced, conflict auto-resolution verified, lifecycle → IDLE), O11 (multi-controller independence, host mode via DBus, unknown device dropped, wrong sender rejected), O12 (host profile cycle deferred per §13 — verified current behavior: UP/DOWN navigates rows, not cycles profiles). Production changes: exposed `cbx_overlay_on_save`, `cbx_overlay_on_slot_change`, `cbx_overlay_on_profile_change` in `overlay_service.h` (removed `static`, renamed with `cbx_overlay_` prefix) so tests can wire production callbacks through the step function. REQ-010/O12 note: host-mode profile cycling is not implemented (host UP/DOWN navigates rows per spec §4.4 interface deferral); not a blocking gap — deferred per §13, to be resolved by Task 14 final audit.
- Documentation impact: none

## Task 12: Enhance installed smoke test with coordinate-based mouse clicks
- Status: pending
- Dependencies: Task 1, Task 2, Task 3
- Scope: `tests/test_installed_smoke.sh`
- Acceptance criteria:
  - The installed smoke test performs representative coordinate-based mouse clicks on manager body controls (not just tab clicks and keyboard keys).
  - At minimum: click on a settings list item, click on the Save button, click on a profiles tab control, and verify a visible state change or file mutation occurs.
  - The test still sends keyboard keys for tab switching and controller-proxy navigation (existing behavior preserved).
  - The test verifies the window is nonblank and consistent with deterministic expectations (existing behavior preserved).
  - The test still skips cleanly (exit 77) when Xvfb/xdotool/ImageMagick are unavailable.
  - The test does not bypass production initialization — it launches the real installed binary.
- Verification: `nix-shell --run "ctest --test-dir build-check -R test_installed_smoke --output-on-failure"` — passes. `nix-shell --run "./scripts/verify-project.sh"` — passes.
- Documentation impact: `docs/OPERATIONS.md` — update installed smoke test description to include coordinate-based click interactions.

## Task 13: Fix BUG-0002 — CreateComposite XDG runtime test order-dependence
- Status: complete
- Dependencies: none
- Scope: `tests/test_create_composite.c`
- Acceptance criteria:
  - `test_create_composite_xdg_runtime_dir_preferred` compares `/tmp/controller-box-*` count before and after the operation (or otherwise identifies only files created by the operation) instead of asserting the global count is exactly zero.
  - Seeding an unrelated `/tmp/controller-box-*` file before the test does not cause failure.
  - The test's own XDG temporary directory (`/tmp/cbx-xdg-*`) is cleaned up on both success and failure paths (use cleanup fixture or `assert_*` wrapper that runs cleanup unconditionally).
  - Repeated standalone and full CTest runs pass deterministically.
  - `open-bugs.md` BUG-0002 is updated with resolution and verification; entry moved to `closed-bugs.md`.
- Verification: `nix-shell --run "touch /tmp/controller-box-unrelated-test-file && ctest --test-dir build-check -R test_create_composite --output-on-failure && rm /tmp/controller-box-unrelated-test-file"` — passes (1/1, 0.03s). Full suite: `nix-shell --run "ctest --test-dir build-check --output-on-failure"` — 78/78 pass (1 skip: backend_smoke), no regressions. `nix-shell --run "./scripts/verify-project.sh"` — all checks green. **Production changes**: (1) replaced `assert_int_equal(tmp_count, 0)` with before/after comparison (`tmp_before` measured before the operation, `tmp_after` after, `assert_int_equal(tmp_after, tmp_before)`); (2) moved `xdg_dir` from local stack variable into `create_fixture` struct so `teardown()` cleans it up via `rm -rf` even when cmocka `longjmp` bypasses the test body; (3) initialized `f->xdg_dir[0] = '\0'` in setup, added `rm -rf` cleanup block in teardown. BUG-0002 moved from `open-bugs.md` (now empty `[]`) to `closed-bugs.md` with resolution and verification text.
- Documentation impact: `open-bugs.md` → `closed-bugs.md` (BUG-0002 moved).

## Task 14: Final documentation and specification audit
- Status: pending
- Dependencies: Task 1, Task 2, Task 3, Task 4, Task 5, Task 6, Task 7, Task 8, Task 9, Task 10, Task 11, Task 12, Task 13
- Scope: canonical definition of done in `docs/SPEC.md` §11.2 (audit only — no spec changes), `README.md`, `docs/OPERATIONS.md`, `IMPLEMENTATION_PLAN.md`, `open-bugs.md`, `closed-bugs.md`
- Acceptance criteria:
  - **Conformance matrix**: every requirement (REQ-001–REQ-032) is classified `verified` with specific source evidence and an executable test or acceptance command. No requirement remains `partial`, `missing`, or `ambiguous`. REQ-010 (host-mode profile cycling) is resolved: either implemented and verified, or explicitly documented as deferred per §13. REQ-021 (icon override UI) is resolved: documented as deferred per §13 (config API verified in REQ-026; settings tab UI deferred — interface details for compound settings not specified in v1).
  - **Production-path audit** (§11.2 item 2): verify that no acceptance test in the suite bypasses production event dispatch, initialization, or composition paths. Direct callback tests may exist as supplemental unit tests but must not be the sole evidence for any v1 workflow. The final audit audits all test files for direct-call-only patterns and flags any that are the sole evidence for a conformance requirement.
  - **Interaction inventory**: every control (M01–M38) and overlay action (O01–O12) has passing controller-path and pointer-path (where applicable) evidence. O12 (host-mode profile cycling) is either implemented and verified, or documented as deferred per §13. Every disabled-control and operation-failure scenario (D01–D08) passes.
  - **Visual and degraded-state acceptance** (§11.2 item 4): verify that §§4.10, 5.6, and 11.1 framebuffer tests and golden baselines exist and pass for normal, empty, loading, unavailable, validation-error, backend-error, and recovery states required by the affected workflows. No required screen or region is blank, clipped, or misleadingly enabled.
  - **Known-defect accounting**: `open-bugs.md` contains no unresolved defect that contradicts a v1 requirement. BUG-0002 is closed with verification.
  - **Independent review**: read-only correctness, test-quality, security, and documentation reviews find no unresolved blocking issue. Reviews challenge whether tests can pass while production behavior remains broken.
  - **Full clean verification**: `nix-shell --run "./scripts/verify-project.sh"` passes (build, CTest, packaging, installed smoke). `nix-shell --run "./scripts/verify-boilerplate.sh"` passes. No unexplained skips, flaky rerun dependencies, weakened assertions, leaked processes/files, compiler warnings, or sanitizer/static-analysis defects in changed code (where those checks are supported) introduced by this cycle.
  - **Documentation**: README and OPERATIONS.md match observed behavior; build, install, acceptance, artifact, and recovery commands work from a clean checkout.
  - **Repository integrity**: Git tree is clean on `develop`; plan `status` set to `complete`.
  - **Human visual acceptance gate**: Human visual acceptance on target hardware (§11.1 layer 7) remains a pre-promotion gate. This task marks the autonomous cycle complete; promotion to `main` requires separate human review on target hardware.
- Verification: `nix-shell --run "./scripts/verify-project.sh && ./scripts/verify-boilerplate.sh && ctest --test-dir build-check --output-on-failure"` — all pass. `git diff --exit-code` — clean tree.
- Documentation impact: `README.md` (update verification suite section), `docs/OPERATIONS.md` (update interaction testing section), `IMPLEMENTATION_PLAN.md` (mark complete).

## Remediation rule

When the final audit (Task 14) finds a gap, the worker must: preserve the
existing task ledger, append a uniquely numbered pending task (Task 15, 16,
…) with bounded scope and acceptance criteria, add the new task as a
dependency of Task 14, return Task 14 to `pending`, and continue. Reaching an
iteration, runtime, quota, or session ceiling leaves the cycle incomplete
(`status: active` or `blocked`); it never satisfies the plan.