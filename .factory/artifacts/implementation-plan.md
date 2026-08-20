---
spec_path: docs/SPEC.md
spec_commit: 3a10f6b7d04a615b2b9d06eef6c91e431fa9c079
spec_blob: 58f5d3cb72bc6b3e5f573fa09a63c11a653ed577
base_commit: 860922af39ed7e2aef9b95705705f7ed90344e87
status: active
---

## Blocking findings (2026-08-19, lifecycle operator)

Two human-observed production acceptance failures invalidate the previous
`status: complete` claim. The plan is returned to `active` until Ralph owns
a product diagnosis/fix for each and re-verifies with real acceptance
evidence. No completion may proceed on the old evidence.

1. **BUG-0014 — Manager shows no controller diagram in production launch.**
   Launching `./build-check/controller-box --manager` (real binary, real
   window server) shows a blank controller diagram area. Existing
   `test_manager_visual.c`, `test_golden.c`, and `test_overlay_visual.c`
   pass without catching this, so goldens/pixel tests do not prove the
   production diagram rendering path. Required: a Ralph-owned renderer/
   asset-path diagnosis and fix plus a production-window/installed-path
   semantic test proving recognizable diagram content (not a non-NULL
   texture, a fallback, or a broad pixel-count change).
2. **BUG-0015 — Manager reports `Topology incomplete: 0 of 4 virtual
   controllers active`; no virtual controller works.** Real
   InputPlumber system-bus acceptance is required: four expected
   target/controller objects present and usable through production
   dispatch. `inputplumber-system-dbus` is NOT declared in
   `.factory/environment.toml` (declared: remote-project-gate,
   systemd-user, kernel-uinput, installed-package), so private/
   native-signature sd-bus tests must not be counted as real
   system-bus evidence, and rows relying on them must not claim
   `verified`. If the capability remains unavailable, it is an explicit
   blocking finding and the affected rows stay non-verified.

Affected rows reclassified `verified` -> `partial` in the conformance matrix below:
ARCH-04, SYS-06, DBUS-02, DBUS-05, OVL-10, MGR-02, MGR-07, MGR-08,
DOD-01, DOD-09. Task 4 (final audit) is returned to `blocked`.

### Machine-readable migration (BUG-0016 hardening stage B)

The conformance matrix is bound to the machine-readable sidecar
`.factory/artifacts/conformance.json` (`ralph-conformance/v1`), which is the
only authority for `verified` claims (see `scripts/validate-conformance.py`).
Every blocked/partial row whose required evidence is unavailable references an
open entry in the append-only `.factory/artifacts/blocked-facts.json` ledger
(`ralph-blocked-facts/v1`):

- FACT-001 — BUG-0014 perceptible installed diagram acceptance;
- FACT-002 — BUG-0015 real InputPlumber system-bus acceptance;
- FACT-003 — missing `inputplumber-system-dbus` capability;
- FACT-004 — missing `target-consumer` capability (real four-target routing,
  aarch64/Pi 4 runtime);
- FACT-005 — missing `gpu-compositor` capability;
- FACT-006 — missing target-Pi latency measurement and human release
  acceptance (SPEC §11.1.7).

Facts resolve only with an exact receipt/artifact at an evidence commit or an
explicit human decision where the specification permits it (SPEC §11.2.6);
documentation/rationale alone never resolves a normative requirement. Open
facts fail implementation completion.

Beyond the blocking-findings downgrades above, rows whose `verified` claim
rested on real-system/GPU/target/human evidence that was only reasoned about
(not executed) are also downgraded to `partial`: SYS-01 (aarch64 build),
SYS-02 (Pi 4 runtime), OVL-09 (Pi 4 latency bound), PERF-01 (Pi 4 p99
bound), VRF-06 (GPU backend smoke), VRF-07 (human release acceptance), and
DOD-05 (unexplained GPU skip). Two uniquely numbered implementation tasks are
appended — Task 5 (perceptible installed diagram acceptance) and Task 6 (real
four-target InputPlumber routing acceptance). Task 4 (final audit) remains
`blocked` and now depends on Tasks 1, 2, 3, 5, and 6.

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
| ARCH-04 | §2.4 | partial | `ip_connection.c` NameOwnerChanged, degraded/recovery, ≤2s re-enumerate; `test_native_dbus.c` is a private native-signature test, not real InputPlumber system-bus acceptance (BUG-0015, inputplumber-system-dbus undeclared) | Task 6 |
| ARCH-05 | §2.5 | verified | `trigger.c` SetInterceptActivation; `ip_intercept_poll.c` 50 ms poll; `test_trigger.c`, `test_intercept_poll.c` | |
| SYS-01 | §3 | partial | aarch64 toolchain files present and correctly configured (cmake/aarch64-toolchain.cmake, cross-shell.nix); cross-compile attempted but no zero-warning aarch64 build artifact exists; requires an aarch64-capable build path (FACT-004) | Task 4 |
| SYS-02 | §3 | partial | x86_64 build and full test suite verified (98 CTest targets, runner receipt); ARM64 portability confirmed by code review and Flatpak multi-arch target; Pi 4 runtime requires physical target hardware and real target consumer (FACT-004, FACT-006) | Task 4 |
| SYS-03 | §3 | verified | SDL2 supports X11/Wayland/Gamescope; `test_sdl_dummy.c` | |
| SYS-04 | §3 | verified | `CMakeLists.txt` deps: SDL2, SDL2_ttf, SDL2_image, libsystemd, libyaml; nanosvg vendored `third_party/nanosvg/` | |
| SYS-05 | §3 | verified | InputPlumber not bundled; runtime bus-name check in `ip_connection.c` | |
| SYS-06 | §3 | partial | Native sd-bus tests are private native-signature tests, not real InputPlumber system-bus acceptance; inputplumber-system-dbus undeclared (BUG-0015) | Task 6 |
| OVL-01 | §4.1 | verified | `grid_render.c` select-screen grid; `test_grid_render.c`, `test_overlay_visual.c` | |
| OVL-02 | §4.2 | verified | `trigger.c` default Select+A, configurable; `test_trigger.c` | |
| OVL-03 | §4.3 | verified | `player_mode.c` independent per-controller; `test_player_mode.c`, `test_overlay_native.c` O11 | |
| OVL-04 | §4.4 | verified | `host_mode.c` R3 toggle, freeze, navigate; `test_host_mode.c`, `test_overlay_native.c` O06–O09 | |
| OVL-05 | §4.5 | verified | `conflict.c` detect+resolve; `grid_render.c` red; `test_conflict.c`, `test_overlay_native.c` O13 | |
| OVL-06 | §4.6 | verified | `profile_cycle.c` profile follows controller; `test_profile_cycle.c` | |
| OVL-07 | §4.7 | verified | `dynamic_columns.c` scales with target count; `test_dynamic_columns.c` | |
| OVL-08 | §4.8 | verified | `grid_render.c` model name + slot, no nicknames | |
| OVL-09 | §4.9 | partial | Pre-built surface architecture (surface_build.c) and 50 ms poll cycle (ip_intercept_poll.c) verified; x86_64 latency measured by test_overlay_latency.c over 200+ iterations; Pi 4 latency bound requires physical target hardware and real target consumer (FACT-004, FACT-006) | Task 4 |
| OVL-10 | §4.10 | partial | `test_overlay_visual.c` and `test_golden.c` pass without proving the production diagram; human-observed blank diagram (BUG-0014, FACT-001) | Task 5 |
| MGR-01 | §5.1 | verified | `manager.c` tab bar, 3 tabs, controller + pointer; `test_manager_tabs.c`, `test_manager_native.c` | |
| MGR-02 | §5.2 | partial | `controllers_tab.c` add/remove/type-change, topology reconcile; production launch reports 0/4 virtual controllers active (BUG-0015, FACT-002/FACT-003) | Task 6 |
| MGR-03 | §5.3 | verified | `profiles_tab.c` browse/create/edit/delete, built-in Default; `test_profiles_tab.c`, `test_installed_functional.c` | |
| MGR-04 | §5.4 | verified | `profile_editor_list.c`, `profile_editor_seq.c` both modes; `test_editor_list_mode.c`, `test_editor_seq_mode.c` | |
| MGR-05 | §5.4 | verified | `profile_validate.c` NES minimum (A/B/D-pad); `test_profile_validate.c`; `profile_save.c` enforces before write | |
| MGR-06 | §5.5 | verified | `settings_tab.c` all settings; `test_settings_tab.c`, `test_manager_native.c` M21–M26 | |
| MGR-07 | §5.6 | partial | `test_manager_visual.c`, `test_golden.c` pass without proving the production diagram; production manager shows no controller diagram (BUG-0014, FACT-001) | Task 5 |
| MGR-08 | §5.7 | partial | 52/60 inventory claimed verified with controller-transport evidence via ctrl_press and keyboard relabels; premise contradicted by 0/4 virtual controllers active and blank diagram (BUG-0014, BUG-0015, FACT-001/FACT-002/FACT-004) | Task 6 |
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
| DBUS-02 | §10.1 | partial | `dbus_client.c` native sd-bus types (u, b, as, s); `test_dbus_signatures.c`, `test_native_dbus.c` exercise a private service, not real InputPlumber system-bus acceptance (BUG-0015) | Task 6 |
| DBUS-03 | §10.1 | verified | `ip_objectmanager.c` GetManagedObjects; `test_objectmanager_parse.c` | |
| DBUS-04 | §10.1 | verified | `ip_hotplug.c` InterfacesAdded/Removed; `test_hotplug.c` | |
| DBUS-05 | §10.1 | partial | `ip_connection.c` owner check, version, enumeration via `test_native_dbus.c` (private service); real InputPlumber system-bus acceptance pending (BUG-0015) | Task 6 |
| DBUS-06 | §10.2 | verified | `ip_manager.c`, `ip_composite.c`, `ip_target.c`, `ip_source.c` full API surface | |
| DBUS-07 | §10.3 | verified | All 5 gaps: intercept poll, gamepad order persist, temp composite YAML, filesystem enumerate, no-op gap 5 | |
| PERF-01 | §11 | partial | x86_64 latency measured by test_overlay_latency.c (p50/p99/max over 200+ iterations); surface architecture and 50 ms poll cycle verified; Pi 4 ≤75 ms p99 bound requires physical target hardware and real target consumer (FACT-004, FACT-006) | Task 4 |
| PERF-02 | §11 | verified | PASS mode kernel-level; no DBus gameplay routing | |
| PERF-03 | §11 | verified | `test_close.c` InterceptMode=PASS close <1 ms | |
| PERF-04 | §11 | verified | `test_daemon_footprint.c` resident footprint | |
| PERF-05 | §11 | verified | `ip_gamepad_order.c` atomic reorder; `test_gamepad_order.c` | |
| VRF-01 | §11.1.1 | verified | `test_overlay_visual.c`, `test_manager_visual.c` SDL_RenderReadPixels through production composition | |
| VRF-02 | §11.1.2 | verified | `fb_assert.c` region-level assertions; `test_fb_assert.c` | |
| VRF-03 | §11.1.3 | verified | `test_golden.c` 11 baselines in `tests/golden/`, ±3/channel <2% tolerance | |
| VRF-04 | §11.1.4 | verified | `fb_assert.c` saves actual/expected/diff on mismatch | |
| VRF-05 | §11.1.5 | verified | `test_installed_functional.c` (4 tests), `test_installed_smoke.sh`, `test_installed_binary.sh`; runner receipt 26df6c0 all pass | |
| VRF-06 | §11.1.6 | partial | Software-renderer smoke test_backend_smoke_sw.c passes (non-blank framebuffer, region content assertions); GPU backend test_backend_smoke.c exits 77 in headless, gpu-compositor undeclared (FACT-005) | Task 4 |
| VRF-07 | §11.1.7 | partial | Human release acceptance checklist documented with procedure, criteria, and evidence storage; requires human reviewer on target hardware per §11.1.7 (FACT-004, FACT-006) | Task 4 |
| DOD-01 | §11.2.1 | partial | Not all matrix rows verified: ARCH-04, SYS-06, DBUS-02, DBUS-05, OVL-10, MGR-02, MGR-07, MGR-08 are partial pending BUG-0014/BUG-0015 | Task 4 |
| DOD-02 | §11.2.2 | verified | Tests use production dispatch; native DBus preserves signatures | |
| DOD-03 | §11.2.3 | verified | M39 added to inventory (Task 1); M28–M38 controller-transport evidence via ctrl_press in test_manager_native_prof.c (Task 2); keyboard tests in test_manager_interaction_prof.c relabeled to _keyboard per §5.7 | Task 1, Task 2 |
| DOD-04 | §11.2.4 | verified | `test_overlay_visual.c`, `test_manager_visual.c` cover degraded/error/recovery states | |
| DOD-05 | §11.2.5 | partial | Software-renderer smoke test_backend_smoke_sw.c provides rendering evidence; GPU backend test_backend_smoke.c skipped (exit 77), gpu-compositor undeclared — an unexplained skip per §11.2.5 (FACT-005) | Task 4 |
| DOD-06 | §11.2.6 | verified | `BUG-0012` closed after repository-root-flock, descriptor-boundary, verifier/event-byte binding, quarantine-race, strict-protocol/parser, complete factory/boilerplate, and 98-target project verification passed | Task 4 |
| DOD-07 | §11.2.7 | verified | Campaign audit round 1 completed with 5 findings; this plan addresses all | |
| DOD-08 | §11.2.8 | verified | README.md/OPERATIONS.md capability claims and inventory counts corrected (Task 1); capability-accounted items documented in OPERATIONS.md (Task 3) | Task 1, Task 3 |
| DOD-09 | §11.2.9 | partial | Plan status is active; two production acceptance failures open (BUG-0014, BUG-0015); final gate cannot accept completion | Task 4 |

## Interaction acceptance inventory

Source: `tests/interaction_inventory.c` (60 entries: M01–M39, O01–O13, D01–D08).
Status: 52 verified, 7 NOT_APPLICABLE (controller-only paths: M13, M14, M16,
M32, M34, M35, M36), 1 DEFERRED (O12 host profile cycling, §13).

**M39 (first-run service install):** Added to `interaction_inventory.c`
by Task 1. Tested in `test_manager_interaction_ctrl.c` (4 tests:
controller confirm/cancel, pointer confirm/cancel).

**Controller transport for M28–M38:** Resolved by Task 2. Real
gamepad-transport tests using `ctrl_press` (SDL_JoystickSetVirtualButton →
SDL_CONTROLLERBUTTONDOWN → cbx_manager_controller_to_key →
cbx_manager_handle_event) added to `test_manager_native_prof.c` for M28
(binding nav + diagram highlight), M29 (binding edit sub-menu), M37 (save
via B). Existing ctrl_press tests for M30, M31, M33, M38 retained.
Keyboard tests in `test_manager_interaction_prof.c` relabeled from
`_controller` to `_keyboard` and annotated as supplemental per §5.7.

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
| M28 | Binding highlighted, diagram lights | D-pad U/D in editor | Mouse click row | Diagram button lights | `cbx_manager_handle_event` → list → `profile_editor_list.c` | `test_manager_native_prof.c` (ctrl_press); `test_manager_interaction_prof.c` (keyboard supplemental) |
| M29 | Binding edit sub-menu | A on binding | Mouse click row | Sub-menu opens (Pick/Capture/Seq) | `cbx_manager_handle_event` → list | `test_manager_native_prof.c` (ctrl_press); `test_manager_interaction_prof.c` (keyboard supplemental) |
| M30 | Target picked, binding updated | A on target | Mouse click target | Binding updated, picker closes | `cbx_manager_handle_event` → list | `test_manager_native_prof.c` (ctrl_press); `test_manager_interaction_prof.c` (keyboard supplemental) |
| M31 | Capture mode begins | A on Capture | Mouse click Capture | Waiting for physical button | `cbx_manager_handle_event` → list | `test_manager_native_prof.c` (ctrl_press); `test_manager_interaction_prof.c` (keyboard supplemental) |
| M32 | Binding captured (NA pointer) | Physical button via DBus InputEvent | N/A | Source event set, capture ends | DBus signal → `ip_input_signal.c` → editor | `test_manager_native_prof.c` DBus signal path |
| M33 | Sequential mode begins | A on Sequential | Mouse click Sequential | First button prompted, diagram lights | `cbx_manager_handle_event` → list | `test_manager_native_prof.c` (ctrl_press); `test_manager_interaction_prof.c` (keyboard supplemental) |
| M34 | Button captured in sequential (NA pointer) | Physical button via DBus InputEvent | N/A | Auto-advance, progress bar | DBus signal → editor | `test_manager_native_prof.c` DBus signal path |
| M35 | Skip binding in sequential (NA pointer) | B | N/A | Advance to next | `cbx_profile_editor_seq_skip()` | `test_manager_native_prof.c` |
| M36 | Sequential cancelled (NA pointer) | Start | N/A | Changes discarded, editor returns | `cbx_manager_handle_event` | `test_manager_native_prof.c` |
| M37 | Profile saved | B in LIST (save) | Mouse click Save | File written, editor closes, list refresh | `cbx_manager_handle_event` → button → `profile_save.c` | `test_manager_native_prof.c` (ctrl_press); `test_manager_interaction_prof.c` (keyboard supplemental), `test_installed_functional.c` |
| M38 | Editor discard | Start in LIST | Mouse click Discard | No file written, editor closes | `cbx_manager_handle_event` | `test_manager_native_prof.c` (ctrl_press); `test_manager_interaction_prof.c` (keyboard supplemental) |
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
- Status: complete
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
- Status: complete
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
- Status: complete
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
- Status: blocked
- Block reason: BUG-0014 (invisible diagram) and BUG-0015 (0/4 virtual controllers); real InputPlumber system-bus acceptance required; block lifts only with Ralph-owned product fixes and real acceptance evidence (Tasks 5 and 6)
- Dependencies: Task 1, Task 2, Task 3, Task 5, Task 6
- Scope: `.factory/artifacts/implementation-plan.md` (conformance matrix update), `.factory/artifacts/conformance.json` (sidecar), `.factory/artifacts/blocked-facts.json` (facts ledger), `README.md`, `docs/OPERATIONS.md`, full clean verification
- Acceptance criteria:
  - Task 5 (perceptible installed diagram acceptance) and Task 6 (real four-target InputPlumber routing acceptance) are complete, with exact receipt/artifact evidence resolving FACT-001, FACT-002, and FACT-003 (or an explicit human decision where SPEC §11.2.6 permits it)
  - All conformance matrix rows reclassified: every row that depended on Tasks 1–3 or on Tasks 5–6 is `verified` in the matrix AND in `.factory/artifacts/conformance.json` at the evidence tier actually proven; no `partial`/`missing`/`ambiguous`/`blocked` row remains unless a blocked row is a documented blocking finding that fails completion
  - Every open fact in `.factory/artifacts/blocked-facts.json` resolved by exact receipt/artifact or explicit human decision; documentation/rationale alone never resolves a normative requirement
  - Any row that remains non-verified has a human-approved deferral documented per §11.2.6 (FACT-004/FACT-005/FACT-006 resolve only with real evidence or human decision)
  - Full clean build and test suite: `nix-shell --run 'rm -rf build-check && cmake -S . -B build-check -DCMAKE_BUILD_TYPE=Debug && cmake --build build-check --parallel && ctest --test-dir build-check --output-on-failure'` — zero failures, zero unexplained skips
  - `./scripts/verify-project.sh` passes
  - `./scripts/check-docs-sync.sh` passes (docs changed alongside implementation)
  - `./scripts/bug-ledger.py validate` passes; open bugs (if any) are all human-approved deferrals, not unaddressed defects
  - `git status` clean on `develop` (only plan artifact + docs changed)
  - Independent adversarial review: launch read-only reviewer and docs-reviewer subagents; no blocking issues found
  - Interaction inventory is exhaustive: M01–M39 + O01–O13 + D01–D08 all verified or NOT_APPLICABLE, with production-path controller and pointer evidence per §5.7
  - Definition of done (§11.2) all 9 criteria satisfied or explicitly deferred with human approval
- Verification: `./scripts/final-gate.sh --implementation` passes (complete mode; rejected while any row is non-verified or any fact is open); `./scripts/verify-project.sh` passes; `./scripts/validate-conformance.py complete .factory/artifacts/conformance.json` and `./scripts/validate-blocked-facts.py complete` accept; conformance matrix, sidecar, and facts ledger agree with no drift. BUG-0012 correction evidence: changed syntax/static/mode/diff checks; focused lock, orchestration, campaign state/sequence, completion/stale/recovery, plan-parser, checkpoint, bug, audit, runner/environment, Pi-wrapper, and bypass tests; `./scripts/verify-boilerplate.sh`; and `nix-shell --run './scripts/verify-project.sh'` all passed serially. Project CTest passed 98/98 with the two declared environment skips (`test_kernel_controller`, `test_backend_smoke`); installed-functional acceptance, packaging, and installed smoke passed. Campaign state SHA-256 remained `800ced3fd2c6889913d1035906fdc9093d76c0ad2ebe4162c1c380b547561b91`.
- Documentation impact: Final reconciliation of README.md, OPERATIONS.md, implementation-plan conformance matrix, conformance sidecar, and blocked-facts ledger

## Task 5: Perceptible installed diagram acceptance
- Status: pending
- Dependencies: Task 1, Task 2, Task 3
- Scope: renderer/asset-path diagnosis and fix for the production diagram (BUG-0014), a production-window/installed-path semantic test proving recognizable diagram content, `.factory/artifacts/blocked-facts.json` (resolve FACT-001 with receipt/artifact or human decision), `.factory/artifacts/conformance.json`
- Acceptance criteria:
  - `./build-check/controller-box --manager` on the installed X11 production path renders a controller diagram with recognizable content (outline, model label, slot highlight) in the diagram region — not a non-NULL texture, a fallback, or a broad pixel-count change
  - A semantic test on the installed production window asserts recognizable diagram content (region-level assertions or equivalent), and `test_manager_visual`/`test_overlay_visual`/`test_golden` no longer pass while the production diagram is blank
  - OVL-10 and MGR-07 reclassify `partial` -> `verified` in the conformance matrix and sidecar with the installed-window evidence; FACT-001 resolves with an exact receipt/artifact at the evidence commit
  - No test injects an env var or source-tree path to load the diagram assets; the installed layout must load them through the production path
- Verification: `nix-shell --run './scripts/verify-project.sh'`; installed diagram acceptance test passes on the real window server; `./scripts/validate-conformance.py planning` and `./scripts/validate-blocked-facts.py planning` accept; bug-ledger/BUG-0014 evidence attached
- Documentation impact: README.md and OPERATIONS.md production launch notes updated

## Task 6: Real four-target InputPlumber routing acceptance
- Status: pending
- Dependencies: Task 1, Task 2, Task 3
- Scope: declare and provision `inputplumber-system-dbus` (and the real system-bus `org.shadowblip.InputPlumber` service with four target devices) in `.factory/environment.toml`, route all InputPlumber operations through the DBus backend abstraction on the real system bus, add a real four-target production acceptance test, `.factory/artifacts/blocked-facts.json` (FACT-002/FACT-003), `.factory/artifacts/conformance.json`
- Acceptance criteria: four expected target/controller objects present and usable through production dispatch on the real InputPlumber system bus; `Topology incomplete: 0 of 4` no longer appears; ARCH-04, SYS-06, DBUS-02, DBUS-05, MGR-02, MGR-08 reclassify to `verified` in the matrix and sidecar; FACT-002 and FACT-003 resolve with exact system-bus probe receipts and routing acceptance evidence
- Verification: `./scripts/run-factory-runners.py` then `./scripts/check-factory-runner-evidence.py` accept exact-commit receipts; `./scripts/check-capability-evidence.py` accepts `inputplumber-system-dbus`; the real four-target routing acceptance passes on the production dispatch path
- Documentation impact: OPERATIONS.md capability declarations and acceptance evidence updated

## Remediation rule

When the final audit (Task 4) finds a gap that earlier tasks did not close:
1. Preserve the existing task ledger, conformance matrix, sidecar, and facts ledger.
2. Append a uniquely numbered pending task (Task 7, 8, …) with bounded scope; the final
   audit may be followed by appended tasks and must depend on every other task.
3. Bind every affected blocked/partial conformance row to an open entry in
   `.factory/artifacts/blocked-facts.json` with the exact unavailable evidence.
4. Keep Task 4 `blocked` while any blocking finding or open fact remains;
   return it to `pending` only when the facts it depends on are resolvable.
5. Continue the loop.

Reaching an iteration, runtime, or session ceiling leaves the cycle
incomplete (`status: active`); it never satisfies the plan.