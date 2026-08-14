---
spec_path: docs/SPEC.md
spec_commit: 3a10f6b7d04a615b2b9d06eef6c91e431fa9c079
spec_blob: 58f5d3cb72bc6b3e5f573fa09a63c11a653ed577
base_commit: 44c4bf69d62b041df2464e63b6907783a2b0cdc3
status: active
---

# Implementation Plan

## Goal and non-goals

**Goal:** Close all remaining specification conformance gaps in the current
committed `docs/SPEC.md` so that every normative requirement is classified
`verified` with production-path evidence, the interaction inventory is
exhaustive, known v1 bugs are resolved, and the §11.2 definition of done is
met.

**Non-goals:** New features beyond the committed specification. Changes to
`docs/SPEC.md`. Reimplementation of verified subsystems. Retrieving tasks
from prior plan cycles in Git history.

## Architecture and constraints

Single C11 binary (`controller-box`) with two modes: overlay service and
manager. SDL2 widget toolkit built from scratch. sd-bus system DBus client
to InputPlumber. nanosvg for icon rasterization. libyaml for config. cmocka
for unit tests. Nix-shell for reproducible builds. One autonomous `develop`
branch, one mutating worker. Parallelism is read-only analysis only.

Constraints: strict C11 (`CMAKE_C_EXTENSIONS OFF`), `-Werror` in Debug,
production-path test evidence required (no bypasses), framebuffer pixel
assertions for visual acceptance, both controller and pointer paths for
every interactive control.

## Specification conformance matrix

Requirements are grouped by spec section. `verified` = production-path
evidence confirmed. Non-verified rows map to a Task.

**Note on manager interaction evidence:** Manager interaction tests
(`test_manager_interaction_ctrl.c`, `test_manager_interaction_prof.c`)
send SDL keyboard events through production `cbx_manager_handle_event`
dispatch. Per §5.7, keyboard events are supplemental, not controller
acceptance. INT-002 (partial → Task 8) tracks the controller-transport
gap. Structural/functional MGR requirements (tabs exist, focus chain
works, profiles save, etc.) are `verified` via production dispatch;
interaction-path-specific requirements (controller transport, pointer
motion) are `partial` per INT-002/INT-003 → Tasks 7, 8.

### §2.1–2.3 Architecture constraints

| ID | Req | Class | Evidence | Task |
|----|-----|-------|----------|------|
| ARC-001 | Never touches input routing directly — all state changes via InputPlumber DBus API | verified | No evdev/udev/libinput calls in `src/`; all input mutations via `src/dbus/ip_*.c` method wrappers | — |
| BIN-001 | Single executable `controller-box` with two modes (`--overlay-service`, `--manager`) | verified | `CMakeLists.txt` builds one binary; `src/app/main.c` mode dispatch; `packaging/controller-box.service` uses `--overlay-service` | — |

### §3 System requirements

| ID | Req | Class | Evidence | Task |
|----|-----|-------|----------|------|
| SYS-001 | x86_64 + aarch64 build targets | verified | `CMakeLists.txt` — portable C11 (`CMAKE_C_EXTENSIONS OFF`), no arch-specific code; Nix builds x86_64; aarch64 is same portable codebase, SDL2/sd-bus/libyaml all support aarch64 | — |
| SYS-002 | Runtime deps: SDL2, SDL2_ttf, SDL2_image, sd-bus, nanosvg | verified | `CMakeLists.txt:18-29` — pkg_check_modules for all; `third_party/nanosvg/` vendored | — |
| SYS-003 | Widget system fully navigable by controller | verified | `src/ui/widget*.c`, `focus.c`; interaction tests traverse via focus chain + A | — |
| SYS-004 | Polkit authorization for InputPlumber DBus methods | verified | Documented prerequisite (§9.4); runtime checks ownership + degrades on auth failure (`ip_connection.c:47-63`) | — |

### §2.4 Service model / restart policy

| ID | Req | Class | Evidence | Task |
|----|-----|-------|----------|------|
| SVC-001 | No After=/Requires= for inputplumber.service | verified | `packaging/controller-box.service` — only graphical-session.target | — |
| SVC-002 | Orders with graphical session | verified | `packaging/controller-box.service:4-5,10` | — |
| SVC-003 | Bounded restart backoff | partial | `Restart=on-failure RestartSec=2s` — flat delay, no StartLimit cap | Task 9 |
| SVC-004a | First-run service install prompt + launch_at_boot wiring | missing | `cbx_service_install()` exists but is never called from production; `launch_at_boot` toggle in settings_tab.c:337 only flips a boolean | Task 12 |
| SVC-004 | Checks InputPlumber ownership | verified | `src/dbus/ip_connection.c:86-130` | — |
| SVC-005 | Degraded state while service absent | verified | `src/app/overlay_service.c:310,1168-1175` | — |
| SVC-006 | Watches NameOwnerChanged, re-enumerates | verified | `src/dbus/ip_connection.c:31-44,196-224` | — |
| SVC-007 | Operational within 2 seconds | partial | Event-driven recovery (likely <2s) but no timing test | Task 9 |
| SVC-008 | Specific actionable error for unavailable (manager + overlay) | partial | Manager shows error (`ip_connection.c:47-63`); overlay hides window instead of showing error (`overlay_service.c:648-658`) | Task 9 |
| SVC-009 | Backend-dependent controls visibly disabled | partial | Manager disables controls; overlay hides instead of showing error | Task 9 |
| SVC-010 | Continuous DBus traffic processing | verified | `src/app/overlay_service.c:1045-1052` | — |

### §2.5 Hotkey architecture

| ID | Req | Class | Evidence | Task |
|----|-----|-------|----------|------|
| HKY-001–010 | Single hotkey, Select+A default, SetInterceptActivation, PASS→ALL→PASS, 50ms poll, hidden-not-destroyed | verified | `src/overlay/trigger.c:73-118`, `src/dbus/ip_intercept_poll.h:55`, `src/overlay/lifecycle.c:50-54,130-132` | — |

### §4.1–4.8 Overlay specification

| ID | Req | Class | Evidence | Task |
|----|-----|-------|----------|------|
| OVL-001–022 | Grid layout, icons, L/R move, U/D profile cycle, R3 host, B close, player mode, host mode, conflict red+resolve, per-controller profiles, dynamic columns, no nicknames | partial | Production code verified; O02–O10 + O10b tested via keyboard SDL events (supplemental), not DBus InputEvent production path | Task 13 |

### §4.9–4.10 Rendering & visual acceptance

| ID | Req | Class | Evidence | Task |
|----|-----|-------|----------|------|
| VIS-001 | Pre-built surface, icons cached, dirty-rect | verified | `src/overlay/surface_build.c`, `src/app/overlay_service.c:565-583` | — |
| VIS-002 | ≤75ms p99, ≤100ms max button-to-frame | missing | No performance test exists | Task 4 |
| VIS-003 | <10ms p99 detection-to-present | missing | No performance test exists | Task 4 |
| VIS-004 | Framebuffer output via production composition path | verified | `tests/test_overlay_visual.c:201-227` — SDL_RenderReadPixels | — |
| VIS-005 | Player Mode, Host Mode, conflict, Unassigned+≥2 cols, text, icons rendered | verified | `tests/test_overlay_visual.c:285-771` — pixel assertions | — |
| VIS-006 | Conflict state has red indication | verified | `tests/test_overlay_visual.c:602-641` — `fb_region_has_color({220,40,40})` | — |
| VIS-007 | Transitions produce different frames | verified | `tests/test_overlay_visual.c:773-831` | — |
| VIS-008 | Tests fail if content absent | verified | All assertions use `fb_region_has_content`/`fb_region_has_color` | — |

### §5.1–5.5 Manager specification

| ID | Req | Class | Evidence | Task |
|----|-----|-------|----------|------|
| MGR-001–008 | 3 tabs, tabbar, focus chain, A activates, pointer secondary, same behavior, production init | verified | `src/manager/manager.c:147-260,435-548`; `tests/test_manager_interaction_ctrl.c`, `test_manager_interaction_prof.c` | — |
| MGR-009–017 | Controllers add/remove/type-change, confirmed backend outcomes, topology reconciliation, mixed types, failure rollback | verified | `src/manager/controllers_tab.c:197-352`; `src/app/overlay_service.c:365-510`; `tests/test_manager_interaction_ctrl.c:226-545` | — |
| MGR-018 | Remove slot → physical controller auto-Unassigned | partial | `src/overlay/dynamic_columns.c:85-90` clamps positions; O10b test uses keyboard (supplemental, `✓*`) | Task 13 |
| MGR-019–022 | Profiles browse/create/delete, Default built-in read-only | verified | `src/manager/profiles_tab.c:207-541`; `data/profiles/default.yaml` | — |
| MGR-023 | Immutable Default profile works on clean install | verified | `data/profiles/default.yaml` installed by CMake to system dir; `config_profile_list.c` enumerates builtin Default first | — |
| MGR-024 | Empty profile add-first-binding reachable | verified | `src/manager/profile_editor_list.c:395-398`; `tests/test_manager_interaction_prof.c:765-784` | — |
| MGR-025–026 | Save/discard explicit visible controls, production events | partial | `profiles_tab.c:170-176,580-694` has controls; INT-001 says save_btn/discard_btn missing from inventory; interaction tests use keyboard (supplemental) | Task 5 |
| MGR-027 | Window close with unsaved changes prompts | missing | `src/manager/manager.c:386` — SDL_QUIT sets running=false, no prompt | Task 3 |
| MGR-028 | Profiles stored as InputPlumber YAML | verified | `src/manager/profile_save.c:104-132` | — |
| MGR-029–035 | Editor binding list, sequential mode, NES validation, diagram sync, capability scope | verified | `src/manager/profile_editor_list.c`, `profile_editor_seq.c`, `profile_validate.c`; `tests/test_manager_interaction_prof.c:537-784` | — |
| MGR-036–040 | Settings: launch_at_boot, theme, opacity, vc count/types, trigger | verified | `src/manager/settings_tab.c:101-121`; `tests/test_manager_interaction_ctrl.c:415-538` | — |
| MGR-041 | Settings: icon overrides (§8.4) | missing | Config layer supports it (`config_settings.c:87-140`) but no UI row in `settings_tab.c` | Task 2 |
| MGR-041a | launch_at_boot wires to service install/uninstall | missing | `settings_tab.c:337` toggles bool only; `cbx_service_install()` never called from production | Task 12 |
| MGR-042 | Settings persist to settings.yaml | verified | `src/manager/settings_tab.c:218-228`; `tests/test_manager_interaction_ctrl.c:503-538` | — |

### §5.6 Manager visual acceptance

| ID | Req | Class | Evidence | Task |
|----|-----|-------|----------|------|
| MGR-043–049 | All 3 tabs + editor states rendered via production path with pixels | verified | `tests/test_manager_visual.c:261-867` — `cbx_manager_init` + `fb_read_pixels` | — |
| MGR-050 | Validation error visual through production path | partial | `tests/test_manager_visual.c:869-921` manually sets label text/color instead of triggering save→validate→error | Task 6 |
| MGR-051–053 | Tab switching changes frame, no manual module attach, pixel assertions | verified | `tests/test_manager_visual.c:437-470` | — |

### §5.7 Interaction acceptance

| ID | Req | Class | Evidence | Task |
|----|-----|-------|----------|------|
| INT-001 | Machine-readable inventory of every control | partial | `tests/interaction_inventory.c` — 38 mgr + 12 overlay + 8 degraded; missing save_btn, discard_btn, create picker cancel, binding edit cancel | Task 5 |
| INT-002 | Controller path: focus chain → A → outcome | partial | Tests use keyboard SDL events; spec says these are supplemental, not controller acceptance | Task 8 |
| INT-003 | Pointer path: rendered bounds → mouse motion + click → same outcome | partial | Tests send SDL_MOUSEBUTTONUP/DOWN but no SDL_MOUSEMOTION | Task 7 |
| INT-004 | Visible focus/hover/press indication | missing | Tests check `widget->focused` bool, not visible pixel evidence | Task 7 |
| INT-005 | Disabled controls reject both paths | partial | D01/D02/D06 test both paths; D04/D07/D08 test controller only | Task 5 |
| INT-006 | Hit testing after resize | missing | No resize-then-hit-test test | Task 7 |
| INT-007 | E2E scenarios all covered | partial | Scenarios covered via keyboard-supplemental tests; controller transport (INT-002) and pointer motion (INT-003) and overlay DBus path (INT-010) are partial | Tasks 7, 8, 13 |
| INT-008 | Backend uses real/private DBus, mock supplemental | partial | Manager tests use string-only mock; `test_installed_functional.c` uses real sd-bus | Task 8 |
| INT-009 | Controller transport (not keyboard) for acceptance | partial | `test_manager_production.c:293-328` uses SDL_JoystickAttachVirtual for 1 test; installed smoke uses xdotool key | Task 8 |
| INT-010 | Overlay interaction through production event path | partial | `tests/test_overlay_interaction.c` O02–O10 use `push_keydown` (SDL keyboard); only O11/O11b/O10c use DBus InputEvent. In production, overlay input arrives via DBus InputEvent, not keyboard | Task 13 |
| INT-011 | Installed smoke: coordinate-based body control clicks | verified | `tests/test_installed_smoke.sh` — clicks Settings list item + Save button | — |

### §6 Controller identification

| ID | Req | Class | Evidence | Task |
|----|-----|-------|----------|------|
| IDN-001–006 | Multi-layer identity, ID format, assignment lookup/persist, downgrade detection, gamepad order restore | verified | `src/identify/identity.c:172-316`, `assign.c`, `assign_persist.c`, `identity_downgrade.c`, `gamepad_order_restore.c` | — |

### §7 Config layer

| ID | Req | Class | Evidence | Task |
|----|-----|-------|----------|------|
| CFG-001–006 | settings.yaml, assignments.yaml, profile YAML, enumeration, sidecar, path resolution | verified | `src/config/config_settings.c`, `config_assignments.c`, `config_profile.c`, `config_profile_list.c`, `config_profile_meta.c`, `config_paths.c` | — |

### §8 Controller icons

| ID | Req | Class | Evidence | Task |
|----|-----|-------|----------|------|
| ICN-001–003 | Virtual type icons, Controllercons + custom, nanosvg rasterization, mapping table, profile override | verified | `data/controller-icons.yaml`, `data/icons/svg/`, `src/icons/icon_cache.c`, `icon_lookup.c`, `icon_map.c` | — |

### §9 Packaging

| ID | Req | Class | Evidence | Task |
|----|-----|-------|----------|------|
| PKG-001–006 | Install rules (binary, service, desktop, icons, yaml), Flatpak manifest | verified | `CMakeLists.txt:100-139`, `packaging/org.shadowblip.ControllerBox.yaml` | — |
| PKG-007 | Flatpak manifest marked experimental; no Flathub advertising | missing | Manifest has no experimental marker; README:36 advertises `flatpak install flathub`; PACKAGING.md:124 says "Published on Flathub" | Task 1 |
| PKG-008–011 | Service no After=/Requires=, Restart=on-failure, desktop entry, install layout | verified | `packaging/controller-box.service`, `packaging/controller-box-manager.desktop` | — |

### §10 DBus integration

| ID | Req | Class | Evidence | Task |
|----|-----|-------|----------|------|
| DBU-001–031 | System bus, ownership, Version, NameOwnerChanged, ObjectManager, hotplug, PropertiesChanged, all method/property wrappers, native type fidelity, operational readiness, continuous processing | verified | `src/dbus/` — all modules; `tests/test_native_dbus.c` (real sd-bus); `tests/test_dbus_signatures.c` | — |
| DBU-032 | ManageAllDevices wrapper | verified | Type fidelity correct (`b`); spec says "Expose in Settings if needed" — not required for v1 core flows | — |

### §11 Performance & §11.1 Rendering verification

| ID | Req | Class | Evidence | Task |
|----|-----|-------|----------|------|
| PRF-001 | ≤75ms p99 button-to-frame | missing | No test | Task 4 |
| PRF-002 | <10ms p99 detection-to-present | missing | No test | Task 4 |
| PRF-003 | Close <1ms | partial | Architecture supports it (single property set); no timing test | Task 4 |
| VER-001–005 | Deterministic framebuffer, region assertions, golden images, failure artifacts | verified | `tests/fb_assert.c`, `test_golden.c`, `test_overlay_visual.c`, `test_manager_visual.c` | — |
| VER-006 | Installed functional smoke test | partial | `tests/test_installed_functional.c` exists but creates targets/profiles via wrappers, activates overlay via direct DBus, no controller transport for UI nav | Task 8 |
| VER-007 | Backend smoke (OpenGL) | verified | `tests/test_backend_smoke.c` exercises accelerated renderer with framebuffer invariants; skips when no GPU (acceptable per "where available") | — |
| VER-008 | Human release acceptance | verified | Pre-`main` human gate documented in `docs/OPERATIONS.md`; spec final paragraph confirms this is outside autonomous scope | — |

### §11.2 Definition of done

| ID | Req | Class | Evidence | Task |
|----|-----|-------|----------|------|
| DOD-001 | All requirements verified | partial | 6 missing, 19 partial rows above (SVC-003/004a/007/008/009, MGR-018/025-026/027/041/041a, OVL, VIS-002/003, INT-001-006/007/008/009/010, PRF-001-003, VER-006, PKG-007, DOD-002-004/006/008) | Tasks 1–10 |
| DOD-002 | Production-path behavior | partial | Validation error + sequential visual tests bypass dispatch | Task 6 |
| DOD-003 | Complete interaction traversal | partial | Inventory gaps, missing pointer paths, overlay O02–O10 on keyboard not DBus InputEvent, no controller transport | Tasks 5, 7, 8, 13 |
| DOD-004 | Visual + degraded-state acceptance | partial | Degraded overlay shows nothing; font-dependent skips | Task 9 |
| DOD-005 | Regression + quality gates | verified | Full clean build + 87 tests pass; supplemental "no crash" tests don't replace pixel-level production-path evidence; no unexplained skips | — |
| DOD-006 | Known-defect accounting | partial | BUG-0004 open (runner evidence missing) | Task 10 |
| DOD-007 | Independent review | pending | Final audit task | Task 11 |
| DOD-008 | Documentation + reproducibility | partial | Flatpak docs advertise premature publication | Task 1 |
| DOD-009 | Repository integrity | pending | Final audit task | Task 11 |

## Interaction acceptance inventory

Every interactive manager control and overlay action required by §§4, 5.7,
and 11.2. `C` = controller path, `P` = pointer path, `✓` = tested via
production dispatch, `✓*` = tested via supplemental keyboard path only
(production DBus InputEvent path pending), `✗` = untested, `N/A` = not
applicable.

**Note on manager C-path evidence:** Manager controller-path tests
(`test_manager_interaction_ctrl.c`, `test_manager_interaction_prof.c`)
send SDL keyboard events through the production `cbx_manager_handle_event`
dispatch. Per §5.7, keyboard-generated SDL events are supplemental
accessibility evidence, not controller acceptance. INT-002 (partial →
Task 8) tracks the gap: Task 8 adds `SDL_JoystickAttachVirtual`-
based controller transport evidence for representative installed
functional flows. The keyboard tests exercise the same post-translation
code path (manager.c:135-156 translates `SDL_CONTROLLERBUTTONDOWN` →
`SDL_KEYDOWN`), so they verify semantic outcomes through production
dispatch but do not satisfy the controller-transport requirement alone.

### Manager controls

| ID | Control | C | P | Dispatch path | Outcome | Gap |
|----|---------|---|---|---------------|---------|-----|
| M01–03 | Tab bar: Controllers/Profiles/Settings | ✓ | ✓ | `cbx_manager_handle_event` → tabbar | active_tab changes | — |
| M04 | Controllers device list | ✓ | ✓ | focus chain → list select | selection changes | — |
| M05 | Add button | ✓ | ✓ | activate → TYPE_PICK | mode changes | — |
| M06 | Remove button | ✓ | ✓ | activate → StopTargetDevice | count decreases | — |
| M07 | Change Type button | ✓ | ✓ | activate → TYPE_PICK | mode changes | — |
| M08 | Type picker confirm | ✓ | ✓ | activate → CreateTargetDevice/SetTargetDevices | count/type changes | — |
| M09 | Type picker cancel (B) | ✓ | N/A | key handler → cancel | mode→LIST | — |
| M10 | Profiles list | ✓ | ✓ | focus → list select | selection changes | — |
| M11 | Create button | ✓ | ✓ | activate → CREATE_PICK | mode changes | — |
| M12 | Create source picker | ✓ | ✓ | activate → NAME_INPUT | mode changes | — |
| M13–16 | Name input: chars/backspace/confirm/cancel | ✓ | N/A | key handler | name buffer/editor | — |
| M17 | Edit button | ✓ | ✓ | activate → editor opens | bindings loaded | — |
| M18 | Delete button | ✓ | ✓ | activate → CONFIRM_DELETE | mode changes | — |
| M19 | Confirm delete | ✓ | N/A | activate → unlink file | file gone, count-- | — |
| M20 | Cancel delete | ✓ | N/A | B → LIST | file intact | — |
| M21 | Settings list | ✓ | ✓ | focus → select | selection changes | — |
| M22 | Settings toggle (launch_at_boot) | ✓ | ✓ | activate → toggle | value changes | — |
| M23 | Settings edit enter | ✓ | ✓ | activate → EDIT | mode changes | — |
| M24 | Settings edit up/down | ✓ | N/A | key handler | value changes | — |
| M25 | Settings edit confirm | ✓ | N/A | A → LIST | value applied | — |
| M26 | Settings edit cancel | ✓ | ✓ | B → revert from disk | value reverts | — |
| M27 | Settings save button | ✓ | ✓ | activate → settings_save | settings.yaml written | — |
| M28 | Editor binding list nav | ✓ | ✗ | Up/Down → select | selection + diagram | Task 5 (P) |
| M29 | Editor activate binding | ✓ | ✓ | A → BINDING_EDIT | mode changes | — |
| M30 | Editor target pick confirm | ✓ | ✓ | activate → LIST | binding updated | — |
| M31 | Editor capture begin | ✓ | ✓ | activate → CAPTURE | mode changes | — |
| M32 | Editor capture event | ✓* | N/A | DBus InputEvent → editor | binding captured | Task 8 (dispatch) |
| M33 | Editor sequential begin | ✓ | ✓ | activate → SEQUENTIAL | mode + progress | — |
| M34 | Editor sequential capture | ✓* | N/A | DBus InputEvent → editor | step advances | Task 8 (dispatch) |
| M35 | Editor sequential skip (B) | ✓ | N/A | key handler | step advances | — |
| M36 | Editor sequential cancel (Start) | ✓ | N/A | key handler → LIST | seq inactive | — |
| M37 | Editor save (B / save_btn) | ✓ | ✓ | B in LIST / click save_btn | file written | Task 5 (C for btn) |
| M38 | Editor discard (Tab / discard_btn) | ✓ | ✓ | Tab in LIST / click discard_btn | mtime unchanged | Task 5 (C for btn) |
| M39 | Create picker cancel (B) | ✗ | ✗ | key handler → cancel | mode→LIST | Task 5 |
| M40 | Binding edit cancel (B) | ✗ | ✗ | key handler → cancel | mode→LIST | Task 5 |
| M41 | Unsaved-changes prompt: Save | ✗ | ✗ | SDL_QUIT → prompt → A/click | file written, window closes | Task 3 |
| M42 | Unsaved-changes prompt: Discard | ✗ | ✗ | prompt → A/click | window closes, no save | Task 3 |
| M43 | Unsaved-changes prompt: Cancel | ✗ | ✗ | prompt → A/click | returns to editor | Task 3 |
| M44 | First-run prompt: Yes (enable service) | ✗ | ✗ | prompt → A/click | service installed, launch_at_boot=true | Task 12 |
| M45 | First-run prompt: No (skip) | ✗ | ✗ | prompt → A/click | launch_at_boot=false, no install | Task 12 |

### Overlay actions

| ID | Action | C | Dispatch path | Outcome | Gap |
|----|--------|---|---------------|---------|-----|
| O01 | Open (lifecycle activate) | ✓ | `cbx_overlay_service_step` → poll → activate | state→VISIBLE | — |
| O02 | Move left | ✓* | step → cbx_overlay_input_cb → player_mode | column-- | Task 13 |
| O03 | Move right | ✓* | step → player_mode | column++ | Task 13 |
| O04 | Cycle profile up | ✓* | step → profile_cycle | profile changes | Task 13 |
| O05 | Cycle profile down | ✓* | step → profile_cycle | profile changes | Task 13 |
| O06 | Enter Host Mode (R3) | ✓* | step → host_mode_toggle | host active | Task 13 |
| O06b | Host freezes others | ✓* | step → host_mode_handle | frozen row no move | Task 13 |
| O07 | Host navigate rows | ✓* | step → host_mode | selected_row changes | Task 13 |
| O08 | Host move slot | ✓* | step → host_mode | column changes | Task 13 |
| O09 | Exit Host Mode (R3) | ✓* | step → host_mode_exit | host inactive | Task 13 |
| O10 | Close (save + PASS) | ✓* | step → lifecycle_close | state→IDLE, synced | Task 13 |
| O10b | Close conflict resolution | ✓* | step → conflict_resolve | row moved to free slot | Task 13 |
| O11 | Multi-controller independent | ✓ | step → per-row dispatch | each row moves | — |
| O12 | Host profile cycle | deferred (§13) | — | — | — |

### Degraded / failure scenarios

| ID | Scenario | C | P | Outcome | Gap |
|----|----------|---|---|---------|-----|
| D01 | InputPlumber unavailable | ✓ | ✓ | Add rejected, no DBus call | — |
| D02 | Remove with no device | ✓ | ✓ | Count stays 0 | — |
| D03 | Delete with no profile | ✓ | ✓ | Mode stays LIST | — |
| D04 | Save with missing NES bindings | ✓ | ✗ | Editor stays open, no file | Task 5 |
| D05 | Settings edit cancel | ✓ | ✓ | Value reverts | — |
| D06 | DBus operation failure | ✓ | ✓ | Mode→LIST, count unchanged | — |
| D07 | Filesystem failure | ✓ | ✗ | Editor stays open, error shown | Task 5 |
| D08 | Empty profile creation | ✓ | ✗ | Editor stays open, no file | Task 5 |

## Tasks

## Task 1: Fix Flatpak documentation and mark manifest experimental
- Status: pending
- Dependencies: none
- Scope: `README.md`, `docs/PACKAGING.md`, `packaging/org.shadowblip.ControllerBox.yaml`
- Acceptance criteria:
  - README.md does not contain `flatpak install flathub org.shadowblip.ControllerBox`
  - README.md and docs/PACKAGING.md describe Flatpak as experimental/planned, not published
  - docs/PACKAGING.md does not say "Published on Flathub"
  - Flatpak manifest header comment includes experimental status
  - `nix-shell --run './scripts/verify-project.sh'` passes
- Verification: `grep -rn 'flathub\|Published on Flathub' README.md docs/PACKAGING.md` returns no app-install advertising; full test suite passes
- Documentation impact: README §Installation, docs/PACKAGING.md §Flatpak

## Task 2: Add icon override setting to Settings tab
- Status: pending
- Dependencies: none
- Scope: `src/manager/settings_tab.c`, `src/manager/settings_tab.h`, `tests/test_manager_interaction_ctrl.c`, `tests/test_manager_visual.c`
- Acceptance criteria:
  - Settings tab exposes an icon-override setting row that lets the user set/remove an override for a virtual device type
  - Setting persists to `settings.yaml` via existing `cbx_settings_icon_override_set` API
  - Controller-path test: navigate to setting via focus chain, activate with A, verify `icon_overrides` array in loaded settings
  - Pointer-path test: click setting row, verify same outcome
  - Visual test: Settings tab pixel assertion covers the new setting row region
  - Overlay icon override test: set an icon override for a virtual type, activate the overlay, verify the overridden icon renders (not the default) via framebuffer pixel assertion in the icon region
  - `nix-shell --run 'ctest --test-dir build-check -R "settings" --output-on-failure'` passes
- Verification: `ctest --test-dir build-check -R 'settings' --output-on-failure`; `nix-shell --run './scripts/verify-project.sh'`
- Documentation impact: docs/PROFILES.md §File layout (profile metadata sidecar), README §Manager usage (Settings tab), docs/OPERATIONS.md §Configuration

## Task 3: Implement unsaved-changes prompt on manager window close
- Status: pending
- Dependencies: none
- Scope: `src/manager/manager.c`, `src/manager/manager.h`, `tests/test_manager_interaction_prof.c`
- Acceptance criteria:
  - When the profile editor has unsaved changes and the user closes the manager window (SDL_QUIT), a prompt dialog appears with Save / Discard / Cancel options
  - Save: writes profile then closes; Discard: closes without saving; Cancel: returns to editor
  - When no unsaved changes exist, SDL_QUIT closes immediately without prompt
  - Controller-path test: trigger SDL_QUIT with dirty editor, navigate prompt with controller, verify each option's outcome
  - Pointer-path test: click prompt buttons, verify same outcomes
  - `nix-shell --run 'ctest --test-dir build-check -R "manager" --output-on-failure'` passes
- Verification: `ctest --test-dir build-check -R 'manager_interaction_prof' --output-on-failure`; full verify-project.sh
- Documentation impact: README §Manager usage note

## Task 4: Add overlay performance measurement tests
- Status: pending
- Dependencies: none
- Scope: `tests/test_overlay_performance.c`, `tests/CMakeLists.txt`
- Acceptance criteria:
  - Test measures button-to-first-visible-frame latency: simulate trigger activation, run poll cycle, measure time from activation to `cbx_overlay_surface_render` completion; assert ≤75ms at p99 over ≥100 iterations, ≤100ms max
  - Test measures detection-to-present latency: from InterceptMode=ALL detection to first `SDL_RenderPresent` / framebuffer readback; assert <10ms p99
  - Test measures close latency: from close request to `InterceptMode=PASS` set; assert <1ms (single property set, measure call duration)
  - All measurements use deterministic software renderer with `SDL_VIDEODRIVER=dummy`
  - Tests fail if latency exceeds bounds
  - `nix-shell --run 'ctest --test-dir build-check -R "overlay_performance" --output-on-failure'` passes
- Verification: `ctest --test-dir build-check -R 'overlay_perf' --output-on-failure`; full verify-project.sh
- Documentation impact: docs/OPERATIONS.md §Performance expectations

## Task 5: Complete interaction inventory and fix control coverage gaps
- Status: pending
- Dependencies: none
- Scope: `tests/interaction_inventory.c`, `tests/test_manager_interaction_ctrl.c`, `tests/test_manager_interaction_prof.c`
- Acceptance criteria:
  - Inventory includes M39 (create picker cancel) and M40 (binding edit cancel) with controller-path tests verifying mode returns to LIST
  - M37 save_btn has controller-path test: navigate focus to save_btn, press A, verify file written
  - M38 discard_btn has controller-path test: navigate focus to discard_btn, press A, verify mtime unchanged
  - M28 pointer path description corrected: clicking a binding list item activates (M29), not just highlights
  - D04, D07, D08 have pointer-path tests: click save_btn with invalid/incomplete profile, verify rejection (no file, editor stays open)
  - `interaction_inventory.c` verification status updated to reflect passing tests
  - `nix-shell --run 'ctest --test-dir build-check -R "interaction" --output-on-failure'` passes
- Verification: `ctest --test-dir build-check -R 'interaction' --output-on-failure`; `ctest --test-dir build-check -R 'interaction_inventory' --output-on-failure`
- Documentation impact: none (test-only)

## Task 6: Strengthen production-dispatch evidence for visual tests
- Status: pending
- Dependencies: none
- Scope: `tests/test_manager_visual.c`, `tests/test_golden.c`
- Acceptance criteria:
  - Validation error visual test triggers the error through the production save path: open editor with incomplete profile, send save event through `cbx_manager_handle_event`, verify validation error label renders with red-colored pixels in the status region — no direct `cbx_label_set_text`/`cbx_label_set_color` calls
  - Sequential mode visual test enters sequential mode through production dispatch: navigate to binding, activate "Sequential" option via SDL event, verify progress bar and diagram render — no direct `cbx_profile_editor_begin_sequential` call
  - Golden validation error test uses production save path, not manual label injection
  - Existing pixel assertions preserved (content in error region, red-colored pixels, frame differs from non-error)
  - `nix-shell --run 'ctest --test-dir build-check -R "visual\|golden" --output-on-failure'` passes
- Verification: `ctest --test-dir build-check -R 'manager_visual' --output-on-failure`; `ctest --test-dir build-check -R 'golden' --output-on-failure`
- Documentation impact: none (test-only)

## Task 7: Add mouse motion, visible focus/hover, and resize hit-test to interaction tests
- Status: pending
- Dependencies: 5
- Scope: `tests/test_manager_interaction_ctrl.c`, `tests/test_manager_interaction_prof.c`, `tests/test_harness.c`, `tests/test_harness.h`
- Acceptance criteria:
  - All pointer-path tests send `SDL_MOUSEMOTION` to the control's center before `SDL_MOUSEBUTTONDOWN`/`UP`, satisfying §5.7 "mouse motion plus left-button down/up"
  - At least one controller-path test per tab verifies visible focus indication: after navigating focus to a control, render and assert a focus-highlight-colored pixel region differs from the unfocused state
  - At least one pointer-path test verifies hover state: after mouse motion to a control, render and assert a hover-indication pixel region differs from the non-hovered state
  - A new test resizes the manager window (SDL_WINDOWEVENT_RESIZED), re-renders, and verifies hit testing uses post-layout coordinates (click point derived from final `widget->rect`, not stale pre-layout values)
  - All existing semantic outcome assertions preserved
  - `nix-shell --run 'ctest --test-dir build-check -R "interaction" --output-on-failure'` passes
- Verification: `ctest --test-dir build-check -R 'interaction' --output-on-failure`; full verify-project.sh
- Documentation impact: none (test-only)

## Task 8: Strengthen installed functional test with UI-based flows and controller transport
- Status: pending
- Dependencies: none
- Scope: `tests/test_installed_functional.c`
- Acceptance criteria:
  - Test creates a virtual target through the Manager UI: send SDL events through `cbx_manager_handle_event` to navigate Controllers tab → Add button → type picker → confirm, verify ObjectManager exposes the new target
  - Test creates and saves a profile through the Manager UI: navigate Profiles tab → Create → source picker → name input → editor → add bindings → save, verify profile YAML file exists on disk
  - Test activates the overlay through the poll mechanism: set InterceptMode=ALL on the composite, run `cbx_overlay_service_step` until the overlay becomes visible (not direct lifecycle state injection), verify compositor-visible output
  - Test uses `SDL_JoystickAttachVirtual` (kernel-backed synthetic gamepad) for at least one Manager navigation flow and at least one overlay interaction, producing controller-transport evidence labeled as controller acceptance (not keyboard)
  - Test verifies persistence after process/backend restart: after creating target + saving profile + activating overlay, restart the service process and verify the target, profile, and assignment persist (reload from disk)
  - Test captures a screenshot/window framebuffer of the manager and overlay states as evidence artifacts
  - Test must not be a skip: missing backend, skipped package build, expected early exit, keyboard-only interaction, or a merely nonblank window is failure, not a skip
  - Existing DBus ObjectManager and filesystem inspection assertions preserved
  - `nix-shell --run 'ctest --test-dir build-check -R "installed_functional" --output-on-failure'` passes
- Verification: `ctest --test-dir build-check -R 'installed_functional' --output-on-failure`; full verify-project.sh
- Documentation impact: none (test-only)

## Task 9: Add degraded overlay error rendering and service hardening
- Status: pending
- Dependencies: none
- Scope: `src/app/overlay_service.c`, `src/overlay/surface_build.c`, `packaging/controller-box.service`, `tests/test_overlay_visual.c`, `tests/test_connection_timing.c`
- Acceptance criteria:
  - When the overlay service is in degraded mode (InputPlumber unavailable), it renders a visible error message on the overlay surface (not just hidden), per §2.4 "show/report a specific actionable error"
  - Visual test renders the degraded overlay state and asserts non-background content in the error message region
  - `packaging/controller-box.service` adds `StartLimitIntervalSec=30s` and `StartLimitBurst=5` for bounded restart backoff
  - A timing test verifies that NameOwnerChanged → re-enumeration completes within 2 seconds (simulate name acquisition, measure time to `reenumerate_cb` completion)
  - `nix-shell --run 'ctest --test-dir build-check -R "overlay" --output-on-failure'` passes
  - `nix-shell --run './scripts/verify-project.sh'` passes
- Verification: `ctest --test-dir build-check -R 'overlay' --output-on-failure`; full verify-project.sh
- Documentation impact: docs/OPERATIONS.md §Systemd management, docs/PACKAGING.md §Tarball install (service unit example)

## Task 10: Collect runner evidence and close BUG-0004
- Status: pending
- Dependencies: none
- Scope: `.factory/bugs/open.md`, `.factory/bugs/closed.md`, `.factory-state/runner-evidence/`
- Acceptance criteria:
  - `scripts/run-factory-runners.py` is executed against the declared `dev-runner-vm` runner at the exact clean commit on `develop`
  - `scripts/check-factory-runner-evidence.py` passes, validating the aggregate JSON, per-runner manifest, logs, and Git bindings
  - `.factory-state/runner-evidence.json` exists with valid evidence for the current commit
  - BUG-0004 is moved from `.factory/bugs/open.md` to `.factory/bugs/closed.md` with resolution and verification notes
  - If the runner is inaccessible (infrastructure unavailable), document the blocker in the bug ledger and mark the task as blocked with evidence of the attempt
  - `nix-shell --run './scripts/verify-boilerplate.sh'` passes
- Verification: `./scripts/check-factory-runner-evidence.py`; `./scripts/verify-boilerplate.sh`
- Documentation impact: `.factory/bugs/closed.md` BUG-0004 entry

## Task 12: Wire launch_at_boot to service install/uninstall and add first-run prompt
- Status: pending
- Dependencies: none
- Scope: `src/manager/settings_tab.c`, `src/manager/manager.c`, `src/manager/service_install.c`, `tests/test_manager_interaction_ctrl.c`, `tests/test_manager_visual.c`
- Acceptance criteria:
  - When the user enables `launch_at_boot` in Settings, `cbx_service_install()` is called: writes `~/.config/systemd/user/controller-box.service` and runs `systemctl --user enable --now controller-box`
  - When the user disables `launch_at_boot`, `cbx_service_uninstall()` is called: stops and disables the service, removes the unit file
  - On first manager launch with no existing service unit, a prompt dialog appears: "Enable overlay service? This will install a systemd user service." with Yes/No options (§9.1)
  - Yes → installs service and sets `launch_at_boot=true`; No → skips, `launch_at_boot=false`
  - Controller-path test: navigate to launch_at_boot setting, toggle on via A, verify `cbx_service_install` was called (mock systemctl), verify service unit file written
  - Pointer-path test: click launch_at_boot setting, verify same outcome
  - Visual test: first-run prompt dialog renders with non-background content in Yes/No button regions
  - `nix-shell --run 'ctest --test-dir build-check -R "settings\|service" --output-on-failure'` passes
- Verification: `ctest --test-dir build-check -R 'settings' --output-on-failure`; `ctest --test-dir build-check -R 'service_install' --output-on-failure`; full verify-project.sh
- Documentation impact: docs/OPERATIONS.md §Systemd management, docs/PACKAGING.md §Flatpak (systemd service under Flatpak), README §Install

## Task 13: Add overlay DBus InputEvent interaction tests for O02–O10
- Status: pending
- Dependencies: none
- Scope: `tests/test_overlay_interaction.c`
- Acceptance criteria:
  - Tests for O02 (move left), O03 (move right), O04 (cycle profile up), O05 (cycle profile down), O06 (enter Host Mode), O07 (host navigate rows), O08 (host move slot), O09 (exit Host Mode), O10 (close), and O10b (close with conflict resolution) use `backend->inject_signal` with `InputEvent` payloads (the production DBus InputEvent path), not `push_keydown`
  - Each test injects a DBus InputEvent signal (e.g., `{event="DPadRight", value=1.0}` for move right, `{event="R3", value=1.0}` for Host Mode) and calls `cbx_overlay_service_step` to process it through `ip_input_events` → `cbx_overlay_input_cb` → player/host mode handlers
  - Each test verifies the same semantic outcome as the existing keyboard-based test (column change, profile change, host mode state, close state)
  - Existing keyboard-based tests are retained as supplemental accessibility evidence
  - `nix-shell --run 'ctest --test-dir build-check -R "overlay_interaction" --output-on-failure'` passes
- Verification: `ctest --test-dir build-check -R 'overlay_interaction' --output-on-failure`; full verify-project.sh
- Documentation impact: none (test-only)

## Task 11: Final documentation and specification audit
- Status: pending
- Dependencies: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13
- Scope: `.factory/artifacts/implementation-plan.md`, README.md, docs/
- Acceptance criteria:
  - Conformance matrix: every requirement row is `verified` with specific source evidence and executable test — no `partial`, `missing`, or `ambiguous` rows remain
  - Interaction inventory: every enabled control has passing controller and pointer evidence; every overlay action has passing controller-event evidence; no unenumerated controls
  - No contradictory open v1 bugs in `.factory/bugs/open.md`
  - Independent adversarial reviews (correctness, test-quality, security, documentation) find no unresolved blocking issue
  - `nix-shell --run './scripts/verify-project.sh'` passes cleanly (build, all tests, functional acceptance, smoke, packaging)
  - `nix-shell --run './scripts/verify-boilerplate.sh'` passes
  - README and docs match observed behavior; build, install, and acceptance commands work from clean checkout
  - Git tree is clean on `develop`
  - Front-matter `status` changed from `active` to `complete`
- Verification: full `verify-project.sh`; `verify-boilerplate.sh`; `check-factory-runner-evidence.py`; conformance matrix audit
- Documentation impact: final README/OPERATIONS/PACKAGING/PROFILES sync

## Remediation rule

When the final audit (Task 11) finds a gap, preserve the task ledger, append
a uniquely numbered pending task (Task 14, 15, … — the next available
number), add it to Task 11's
dependencies, return Task 11 to pending, and continue. Reaching an
iteration, runtime, quota, or session ceiling leaves the cycle incomplete
and the front-matter `status` as `active`; it never satisfies the plan.