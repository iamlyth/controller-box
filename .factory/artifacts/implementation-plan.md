---
spec_path: docs/SPEC.md
spec_commit: 3a10f6b7d04a615b2b9d06eef6c91e431fa9c079
spec_blob: 58f5d3cb72bc6b3e5f573fa09a63c11a653ed577
base_commit: 860922af39ed7e2aef9b95705705f7ed90344e87
status: active
---

## Blocking findings (2026-08-19, lifecycle operator)

Two human-observed production acceptance failures invalidated the previous
`status: complete` claim. The plan was returned to `active` until Ralph owns
a product diagnosis/fix for each and re-verifies with real acceptance
evidence.

1. **BUG-0014 — Manager shows no controller diagram in production launch.**
   RESOLVED (Task 5). The blank diagram was a byte-order bug: nanosvg
   rasterises to RGBA byte order (byte 0 = red), but the texture was created
   with `SDL_PIXELFORMAT_RGBA8888`, whose little-endian memory byte order is
   A,B,G,R — so the opaque black controller outline was read as fully
   transparent. `src/manager/profile_diagram.c` now uses
   `SDL_PIXELFORMAT_ABGR8888` (memory byte order R,G,B,A, matching nanosvg),
   and `tests/test_installed_diagram.sh` drives the real installed binary
   through a real X11 window to the profile editor, asserting recognizable
   diagram content (outline, slot highlight, title/model label, binding list)
   via the production path. OVL-10 and MGR-07 reclassified `verified`.
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
With BUG-0014 resolved (Task 5), OVL-10 and MGR-07 are reclassified back to
`verified` with installed-window evidence; FACT-001 is resolved. Remaining
`partial` rows are bound to BUG-0015 and hardware/capability facts (FACT-002
through FACT-007).

### Machine-readable migration (BUG-0016 hardening stage B)

The conformance matrix is bound to the machine-readable sidecar
`.factory/artifacts/conformance.json` (`ralph-conformance/v1`), which is the
only authority for `verified` claims (see `scripts/validate-conformance.py`).
Every blocked/partial row whose required evidence is unavailable references an
open entry in the append-only `.factory/artifacts/blocked-facts.json` ledger
(`ralph-blocked-facts/v1`):

- FACT-001 — BUG-0014 perceptible installed diagram acceptance (RESOLVED in Task 5);
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
  Runner receipt at commit `26df6c0` is unsigned/unevidenced (FACT-007); it
  nominally covers all four (test_kernel_controller passes, Flatpak build
  passes, 100 CTest targets, 98 pass / 2 skips) pending a signed
  commit-bound receipt.
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
| SYS-02 | §3 | partial | x86_64 build and full test suite verified (100 CTest targets; 98 pass / 2 environment skips; runner receipt unsigned — FACT-007); ARM64 portability confirmed by code review and Flatpak multi-arch target; Pi 4 runtime requires physical target hardware and real target consumer (FACT-004, FACT-006) | Task 4 |
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
| OVL-10 | §4.10 | verified | `tests/test_installed_diagram.sh` drives the installed binary through a real X11 window to the profile editor and asserts recognizable diagram content (outline/slot highlight/title/binding list) via the production path; ABGR8888 byte-order fix in `profile_diagram.c`; BUG-0014 fixed (28154 outline px). Task 8 made it environment-independent: it selects a test-owned profile by its computed row (display_order sidecar) instead of a first-row click, so it passes with or without host InputPlumber profiles. Evidence commit `3ddefae` | |
| MGR-01 | §5.1 | verified | `manager.c` tab bar, 3 tabs, controller + pointer; `test_manager_tabs.c`, `test_manager_native.c` | |
| MGR-02 | §5.2 | partial | `controllers_tab.c` add/remove/type-change, topology reconcile; production launch reports 0/4 virtual controllers active (BUG-0015, FACT-002/FACT-003) | Task 6 |
| MGR-03 | §5.3 | partial | `profiles_tab.c` browse/create/edit/delete, built-in Default; `test_profiles_tab.c`, `test_installed_functional.c`; BUG-0017 resolved (commit `04e2b37`): editor tests now select the profile they wrote by filename, so the four profile-editor tests pass with or without host/system InputPlumber profiles. Remaining `partial` is FACT-007 (unsigned runner receipt — signer not provisioned) | Task 4 |
| MGR-04 | §5.4 | verified | `profile_editor_list.c`, `profile_editor_seq.c` both modes; `test_editor_list_mode.c`, `test_editor_seq_mode.c` | |
| MGR-05 | §5.4 | verified | `profile_validate.c` NES minimum (A/B/D-pad); `test_profile_validate.c`; `profile_save.c` enforces before write | |
| MGR-06 | §5.5 | verified | `settings_tab.c` all settings; `test_settings_tab.c`, `test_manager_native.c` M21–M26 | |
| MGR-07 | §5.6 | verified | `tests/test_installed_diagram.sh` drives the real installed binary through a real X11 window to the profile editor and asserts recognizable diagram content (outline, slot highlight, model label, binding list) via the production path; BUG-0014 fixed. Task 8 made it environment-independent: it selects a test-owned profile by its computed row (display_order sidecar) instead of a first-row click, so it passes with or without host InputPlumber profiles. Evidence commit `3ddefae` | |
| MGR-08 | §5.7 | partial | 52/60 inventory claimed verified with controller-transport evidence via ctrl_press and keyboard relabels; premise contradicted by 0/4 virtual controllers active (BUG-0015, FACT-002/FACT-003/FACT-004) | Task 6 |
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
| PKG-01 | §9.1 | partial | `packaging/org.shadowblip.ControllerBox.yaml`; `test_flatpak_manifest.py`; runner receipt 26df6c0 Flatpak PASS | Task 4 |
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
| VRF-05 | §11.1.5 | partial | `test_installed_functional.c` (4 tests), `test_installed_smoke.sh`, `test_installed_binary.sh`; runner receipt 26df6c0 all pass | Task 4 |
| VRF-06 | §11.1.6 | partial | Software-renderer smoke test_backend_smoke_sw.c passes (non-blank framebuffer, region content assertions); GPU backend test_backend_smoke.c exits 77 in headless, gpu-compositor undeclared (FACT-005) | Task 4 |
| VRF-07 | §11.1.7 | partial | Human release acceptance checklist documented with procedure, criteria, and evidence storage; requires human reviewer on target hardware per §11.1.7 (FACT-004, FACT-006) | Task 4 |
| DOD-01 | §11.2.1 | partial | Not all matrix rows verified: 19 of 76 rows partial (ARCH-04, SYS-01, SYS-02, SYS-06, OVL-09, MGR-02, MGR-03, MGR-08, PKG-01, DBUS-02, DBUS-05, PERF-01, VRF-05, VRF-06, VRF-07, DOD-01, DOD-05, DOD-06, DOD-09) pending BUG-0015, signer-provisioning (FACT-007), and hardware/capability facts | Task 4 |
| DOD-02 | §11.2.2 | verified | Tests use production dispatch; native DBus preserves signatures | |
| DOD-03 | §11.2.3 | verified | M39 added to inventory (Task 1); M28–M38 controller-transport evidence via ctrl_press in test_manager_native_prof.c (Task 2); keyboard tests in test_manager_interaction_prof.c relabeled to _keyboard per §5.7 | Task 1, Task 2 |
| DOD-04 | §11.2.4 | verified | `test_overlay_visual.c`, `test_manager_visual.c` cover degraded/error/recovery states | |
| DOD-05 | §11.2.5 | partial | Software-renderer smoke test_backend_smoke_sw.c provides rendering evidence; GPU backend test_backend_smoke.c skipped (exit 77), gpu-compositor undeclared — an unexplained skip per §11.2.5 (FACT-005) | Task 4 |
| DOD-06 | §11.2.6 | partial | Runner receipt evidence (dev-runner-vm manifest 26df6c0) is no longer acceptable: the manifest is unsigned (no signer provisioned, FACT-007) and not a Git blob at the declared evidence commit; installed verification evidence stays unevidenced until a signed commit-bound receipt exists | Task 4 |
| DOD-07 | §11.2.7 | verified | Campaign audit round 1 completed with 5 findings; this plan addresses all | |
| DOD-08 | §11.2.8 | verified | README.md/OPERATIONS.md capability claims and inventory counts corrected (Task 1); capability-accounted items documented in OPERATIONS.md (Task 3) | Task 1, Task 3 |
| DOD-09 | §11.2.9 | partial | Plan status is active; production acceptance failure BUG-0015 open; final gate cannot accept completion | Task 4 |

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
- Dependencies: Task 1, Task 2, Task 3, Task 5, Task 6, Task 7, Task 8, Task 9, Task 10, Task 11, Task 12, Task 13, Task 14, Task 15, Task 16, Task 17, Task 18
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
- Result: BUG-0014 resolved (evidence commit `ae4ef52`). Root cause was a byte-order bug in `src/manager/profile_diagram.c`: nanosvg rasterises to RGBA byte order (byte 0 = red) but the texture used `SDL_PIXELFORMAT_RGBA8888`, whose little-endian memory byte order is A,B,G,R — so the opaque black controller outline was read as fully transparent. Now `SDL_PIXELFORMAT_ABGR8888` (memory byte order R,G,B,A, matching nanosvg). `tests/test_installed_diagram.sh` drives the real installed binary through a real X11 window to the profile editor via the production path and asserts recognizable diagram content (outline, slot highlight, title/model label, binding list). OVL-10 and MGR-07 reclassified `verified`; FACT-001 resolved. Status kept `pending` (not `complete`) because this is an appended task after the final audit; the final audit (Task 4) is the single gate that marks the cycle complete.

## Task 6: Real four-target InputPlumber routing acceptance
- Status: blocked
- Block reason: `inputplumber-system-dbus` capability is NOT declared in `.factory/environment.toml` (declared: `remote-project-gate`, `systemd-user`, `kernel-uinput`, `installed-package`). Real InputPlumber system-bus acceptance requires a real `org.shadowblip.InputPlumber` system bus with four target/controller objects usable through production dispatch, which is not provisioned on the factory. FACT-002/FACT-003 remain open; private/native-signature sd-bus tests are not real system-bus evidence and must not be counted. No fabricated evidence. Block lifts only when the capability is declared and provisioned with an exact-commit signed receipt (signer also not provisioned, FACT-007).
- Dependencies: Task 1, Task 2, Task 3
- Scope: declare and provision `inputplumber-system-dbus` (and the real system-bus `org.shadowblip.InputPlumber` service with four target devices) in `.factory/environment.toml`, route all InputPlumber operations through the DBus backend abstraction on the real system bus, add a real four-target production acceptance test, `.factory/artifacts/blocked-facts.json` (FACT-002/FACT-003), `.factory/artifacts/conformance.json`
- Acceptance criteria: four expected target/controller objects present and usable through production dispatch on the real InputPlumber system bus; `Topology incomplete: 0 of 4` no longer appears; ARCH-04, SYS-06, DBUS-02, DBUS-05, MGR-02, MGR-08 reclassify to `verified` in the matrix and sidecar; FACT-002 and FACT-003 resolve with exact system-bus probe receipts and routing acceptance evidence
- Verification: `./scripts/run-factory-runners.py` then `./scripts/check-factory-runner-evidence.py` accept exact-commit receipts; `./scripts/check-capability-evidence.py` accepts `inputplumber-system-dbus`; the real four-target routing acceptance passes on the production dispatch path
- Documentation impact: OPERATIONS.md capability declarations and acceptance evidence updated

## Task 7: Environment-independent profile editor acceptance
- Status: pending
- Dependencies: Task 1, Task 2, Task 3, Task 5, Task 6
- Scope: diagnose and fix BUG-0017 without weakening assertions or golden baselines; preserve the production profile-loading path across filesystems and runner environments; add a regression test that reproduces the local clean-build failure while retaining signed-runner coverage
- Acceptance criteria:
  - The same exact Controller-Box commit passes `test_manager_tabs`, `test_manager_production`, `test_manager_visual`, and `test_golden` in a fresh local Nix build and in signed exact-commit remote runner evidence
  - Profile editor production dispatch loads all expected bindings; focus traversal escapes the profile list; semantic and golden editor assertions pass without source-tree fallback, environment-specific bypasses, timing sleeps, or regenerated baselines that hide the defect
  - Root cause and cross-environment evidence are attached to BUG-0017 before closure
- Verification: build pristine archives of the same commit in both environments; run `ctest --test-dir <fresh-build> -R '^(test_manager_tabs|test_manager_production|test_manager_visual|test_golden)$' --output-on-failure`; run signed `remote-project-gate`; compare exact commit/tree/environment bindings; then run `./scripts/verify-project.sh`
- Documentation impact: record any newly discovered production profile-path constraint in OPERATIONS.md; no documentation-only closure
- Result: BUG-0017 resolved (commit `04e2b37`). Root cause was environment-dependent test selection: `cbx_profile_list_enumerate` sorts the built-in Default alongside any host/system InputPlumber profiles (`/usr/share/inputplumber/profiles`), so the editor tests' index-0 default selection loaded a host profile with a different mapping count on hosts with InputPlumber installed. Reproduced by staging a host profile that sorts first (all four tests failed at the reported line numbers), then fixed so the editor tests select the profile they wrote by filename and the focus test presses DOWN until focus escapes the list. Cross-environment verified: a pristine archive of `04e2b37` passes all four tests both without and with a host profile that sorts first. `test_editor_edits_selected_profile` regression added (verified to fail without the fix). MGR-03 sidecar stays `partial` bound to FACT-007 (unsigned runner receipt / signer not provisioned — outside this task's scope).

## Task 8: Environment-independent installed diagram acceptance
- Status: pending
- Dependencies: Task 1, Task 2, Task 3, Task 5
- Scope: fix the deterministic `tests/test_installed_diagram.sh` environment dependency (host InputPlumber profile at first row) by selecting a test-owned profile, without weakening the semantic pixel assertions; `.factory/artifacts/blocked-facts.json` (resolve FACT-008 with receipt/artifact or human decision), `.factory/artifacts/conformance.json`
- Acceptance criteria:
  - The installed-window diagram acceptance drives the profile editor to a profile the test itself owns/creates (selected by name, never by first-row click position), so the diagram content assertions are deterministic with or without host/system InputPlumber profiles (`/usr/share/inputplumber/profiles`) on the machine
  - The semantic pixel assertions are preserved (controller outline, slot highlight, title/model label, binding list regions) — no weakened thresholds, no regenerated baselines, no skip
  - A regression case reproduces the host-profile-sorts-first failure (the operator gate observed `verify-project` exit 8 with only `test_installed_diagram` failing) and passes after the fix
  - OVL-10 and MGR-07 reclassify `partial` -> `verified` in the matrix and sidecar with the environment-independent evidence; FACT-008 resolves with an exact receipt/artifact at the evidence commit
  - `nix-shell --run 'bash tests/test_installed_diagram.sh build-check'` passes both without and with a staged host profile that sorts first
- Verification: reproduce the host-profile failure, apply the fix, and verify both environments pass; `nix-shell --run './scripts/verify-project.sh'`; `./scripts/validate-conformance.py planning` and `./scripts/validate-blocked-facts.py planning` accept; bug-ledger/BUG-0018 harness entry attached as evidence of the binding-stability mechanism only
- Documentation impact: OPERATIONS.md production launch notes note the environment-independent installed diagram acceptance
- Result: the test now creates a test-owned profile (`cbx-diagram-test`, 4 diagram-button mappings) in the isolated user profiles dir with a `display_order: -50` sidecar, computes that profile's sorted row (never a first-row click), and verifies the outline/slot-highlight/title/binding-list content. `CBX_DIAGRAM_STAGE_HOST=1` stages a zero-binding profile at `display_order: -100` that sorts ahead, reproducing the host-sorts-first failure. Verified passing without staging (row 0) and with staging (row 1); forcing selection of the staged first-row profile fails the slot-highlight and binding-list assertions, confirming the regression is real. Evidence commit `3ddefae`. OVL-10/MGR-07 reclassified `verified` in matrix and sidecar; FACT-008 resolved with an artifact resolution.

## Task 9: Security hardening of process-execution and parsing trust boundaries
- Status: pending
- Dependencies: Task 1, Task 2, Task 3
- Scope: `src/manager/service_install.c`, `src/dbus/ip_objectmanager.c`, `src/dbus/ip_device_model.c`, `src/dbus/ip_create_composite.c`, `tests/test_service_install.c`, `tests/test_hotplug.c`
- Acceptance criteria:
  - Process-execution hardening: `systemctl`/`flatpak-spawn` resolved via absolute paths (no PATH search) in `systemctl_prefix()` (F1)
  - `FLATPAK_ID` bound to the exact expected app ID `org.shadowblip.ControllerBox` (not any app-ID-shaped string) in both `cbx_service_unit_content` and `systemctl_prefix` (F2)
  - Composite temp-file TOCTOU hardening: fstat/lstat inode comparison immediately before the `CreateCompositeDevice` call rejects a path swapped for a symlink (F4)
  - `atoi` replaced with bounds-checked `strtol` in `parse_composite_index` (both `ip_objectmanager.c` and `ip_device_model.c`) so an attacker-influenced oversized path component cannot wrap (F5)
  - New regression tests: hostile `FLATPAK_ID` rejected (falls back to binary path, no `flatpak run <attacker>`); out-of-range composite index parsed safely
  - Full suite passes; ASan+UBSan clean on the touched paths
- Verification: build + full ctest (100/100 pass, same 2 environmental skips); targeted ASan+UBSan on the four touched test targets passes; full sanitizer build passes 100/100
- Documentation impact: none — internal hardening only; no public behavior change
- Result: All four F1/F2/F4/F5 fixes implemented and verified in this cycle (see commit). `parse_composite_index` returns -1 on any out-of-range numeric suffix instead of wrapping; `systemctl_prefix()` returns absolute tool paths; `FLATPAK_ID` is accepted only when it equals `org.shadowblip.ControllerBox`; `ip_create_composite_device` verifies the temp path's dev/inode before handing it to InputPlumber. New tests `test_unit_content_flatpak_rejects_other_id` and `test_model_add_composite_overflow` added and passing. Status kept `pending` (not `complete`) per the appended-after-final-audit convention: Task 4 is the single gate that marks the cycle complete. The two remaining security-reviewer findings (F3 DBus name-squatting credential verification, F6 test-mock gating) are deferred to Task 10; the docs-reviewer and reviewer findings are tracked as Task 11 (test-quality) and Task 12 (documentation accuracy).

## Task 10: Security hardening — DBus sender credential verification and test-mock gating
- Status: pending
- Dependencies: Task 1, Task 2, Task 3, Task 9
- Scope: `src/dbus/dbus_client.c`, `src/dbus/ip_connection.c`, `src/manager/service_install.c`, `src/manager/service_install.h`, `tests/CMakeLists.txt`
- Acceptance criteria:
  - After resolving InputPlumber's unique bus name (`GetNameOwner`), verify the sender via `GetConnectionCredentials`/`GetConnectionUnixProcessID` and re-verify on every `NameOwnerChanged`; reject signals/method replies from an unverified sender even when InputPlumber is down (F3, prevents name squatting)
  - Gate the `cbx_service_set_mock_*` test overrides behind `#ifdef CBX_TESTING` (or move them to a test-only compilation unit) so they cannot appear in release builds (F6); ensure test targets still compile with the gate defined
- Verification: build + full ctest; ASan+UBSan clean; confirm release (non-test) build has no mock symbols
- Documentation impact: OPERATIONS.md trust-boundary notes if behavior changes
- Result: F3 and F6 implemented and verified in commit a890ba1. **F3** — adds `ip_dbus_backend.get_connection_creds` (production: `sd_bus_get_name_creds` GetConnectionCredentials; mock: configurable pid/uid); `ip_connection` tracks `expected_pid`/`expected_uid`/`sender_verified`, credential-verifies the owner after `GetNameOwner` and again on every `NameOwnerChanged` via `verify_sender`, applies the anti-squatting UID policy (`ip_connection_uid_is_trusted`: root or self-uid), exposes `ip_connection_is_sender_verified`, and `sd_sender_ok` now rejects signals when no trusted sender is tracked (down-state). An untrusted/squatting owner is never advertised as a trusted sender and keeps the connection degraded. **F6** — `cbx_service_set_mock_*` gated behind `#ifdef CBX_TESTING`; new `controllerbox_testing` static library compiles the product sources with `CBX_TESTING` for the three unit tests that need the overrides; `nm` confirms the release `controllerbox` library and the `controller-box` binary contain no mock symbols. Verification: full ctest 100% pass (98 pass / 2 environmental skips: `test_kernel_controller`, `test_backend_smoke`); ASan+UBSan clean with the project's LSAN suppressions; +6 connection tests. OPERATIONS.md documents the sender-verification trust boundary. Status kept `pending` per the appended-after-final-audit convention: Task 4 is the single gate that marks the cycle complete.

## Task 11: Test-quality remediation for §5.7 semantic-outcome gaps
- Status: pending
- Dependencies: Task 1, Task 2, Task 3, Task 9
- Scope: `tests/test_kernel_controller.c`, `tests/test_overlay_native.c`, `tests/test_manager_native_prof.c`, `tests/test_manager_visual.c`, `tests/test_golden.c`, `tests/test_installed_smoke.sh`, `tests/test_interaction_inventory.c`
- Acceptance criteria:
  - `test_kernel_controller.c` asserts a semantic outcome produced by kernel-backed events (e.g. settings.yaml read-back or tab change) and registers the gamecontroller mapping, not merely "manager did not crash" (finding 1)
  - Overlay close (O10/O10b/O13) exercised via the DBus `InputEvent` transport (`emit_input_event(...,"B",1.0)`) instead of only the keyboard `SDLK_b` path (finding 2)
  - M36 uses the controller Start button (`ctrl_press`), M37 asserts on-disk profile change (not a pre-existing file), M30 asserts the target event was applied (findings 3-5)
  - Validation-error visual states rendered through the production save path, not manually injected (finding 6)
  - `test_installed_smoke.sh` no longer blesses an early exit as pass where §11.1.5 declares it a failure (finding 7); inventory ledger ties `verified` flags to actual test pass status (finding 8)
- Verification: build + full ctest; affected tests pass with the semantic assertions in place
- Documentation impact: interaction inventory rows updated if dispatch evidence changes
- Result: All eight findings remediated and verified by a clean full `ctest` (98 pass / 2 environmental skips: `test_kernel_controller`, `test_backend_smoke`) plus `verify-boilerplate.sh`, `check-docs-sync.sh`, `validate-implementation-plan.py planning`, and `validate-conformance.py planning`.
  - Finding 1 (`test_kernel_controller.c`): the settings.yaml check is now a HARD semantic-outcome assertion (`fail()` when settings.yaml is not persisted, replacing the `INFO` that blessed survival-only), the navigation deliberately drives the manager to the Settings tab, toggles a setting, and activates the list's "Save" entry so `cbx_settings_tab_save` writes settings.yaml, and the file documents that the Xbox 360 device identity (0x045E:0x028E) is recognized via SDL's built-in game-controller database through the production path. The test remains an environmental skip (77) locally without `/dev/uinput`; it must be validated on the `kernel-uinput` runner.
  - Finding 2 (`test_overlay_native.c`): O10/O10b/O13 close is now driven through the production DBus `InputEvent` transport — `emit_input_event(svc->conn.backend, svc->conn.bus, COMP_PATH_0, "B", 1.0); drain_bus(...); cbx_overlay_service_step(svc)` — replacing `push_keydown(SDLK_b)`.
  - Finding 3 (`test_manager_native_prof.c` M36): sequential cancel now uses the controller Start button via `ctrl_press(&mgr, f->joystick, 6)` (mapped to `SDLK_TAB` through `cbx_manager_controller_to_key`) instead of `send_key_dn(&mgr, SDLK_TAB)`.
  - Finding 4 (M37): asserts the edited profile file was actually re-written on disk — the file either did not pre-exist and the save created it, or its `st_mtim.tv_nsec` advanced — instead of `access(path, F_OK)` on a fixture pre-created file.
  - Finding 5 (M30): both controller and pointer tests now assert the picked target event was applied to the edited binding (`mappings[editing_index].target_events[0].device_class`/`.value` == `targets[0]`), not just a mode transition.
  - Finding 6 (`test_manager_visual.c` + `src/manager/profiles_tab.c`): the validation-error visual is now produced by the production save path — the test loads an incomplete profile into the in-editor profile and clicks the real Save button through the pointer event path, and `cbx_profiles_tab_save_editor` now also sets the status label to the theme's `conflict` (red) color so the error indicator is rendered by production code, not injected by the test.
  - Finding 7 (`test_installed_smoke.sh`): the overlay early-exit is no longer blessed as pass. The script now starts a private native-signature InputPlumber-compatible service (`test_ip_server`) on its own dbus-daemon and points `DBUS_SYSTEM_BUS_ADDRESS` at it so the installed overlay binary is genuinely exercised against a backend (§11.1.5); if the backend or the overlay cannot start, the step reports FAILURE (not pass/skip). `test_installed_smoke` passes with the overlay actually running.
  - Finding 8 (`interaction_inventory.c/.h` + `test_interaction_inventory.c` + dispatch tests): added a runtime verification ledger — `cbx_interaction_inventory_mark_verified/is_verified/reset` — so a control's `verified` flag reflects an actual passing production-dispatch test rather than a hardcoded static claim. `test_interaction_inventory.c` proves the ledger starts unseeded (no static claim reads back verified) and only flips when a passing test marks it; the touched dispatch tests (M30/M36/M37 in `test_manager_native_prof.c`, O10/O13 in `test_overlay_native.c`) call `mark_verified()` after their assertions pass, and each binary's `main()` asserts those controls are runtime-verified after the suite passes (linked via `cbx_test_support`).

## Task 12: Documentation accuracy remediation
- Status: pending
- Dependencies: Task 1, Task 2, Task 3, Task 9
- Scope: `README.md`, `docs/REVIEW.md`, `docs/OPERATIONS.md`, `.factory/artifacts/implementation-plan.md`
- Acceptance criteria:
  - CTest target count corrected from 98 to 100 (with 98 pass / 2 environmental skips) in README.md, docs/REVIEW.md, and the implementation-plan matrix/runner block (finding 1)
  - README.md "Known environment limitations" intro enumerates all four declared runner capabilities (`remote-project-gate`, `systemd-user`, `kernel-uinput`, `installed-package`), not just two (finding 2)
  - "Proven by runner receipt 26df6c0" downgraded to note the receipt is unsigned/unevidenced pending a signed commit-bound receipt (FACT-007) wherever it appears (finding 3)
  - OPERATIONS.md receipt count corrected from 12 to 13 (finding 4); REVIEW.md "98/98 passing … 2 skips" made internally consistent (finding 5)
- Verification: `./scripts/check-docs-sync.sh` passes; grep confirms no stale "98" or "proven by runner receipt" claims remain; `./scripts/validate-conformance.py planning` and `./scripts/validate-implementation-plan.py planning` accept
- Documentation impact: README.md, docs/REVIEW.md, docs/OPERATIONS.md, implementation-plan conformance matrix
- Result: All five findings remediated. CTest counts corrected to 100 targets / 98 pass / 2 environment skips (`test_kernel_controller`, `test_backend_smoke`) in README.md (aarch64 row), docs/REVIEW.md Evidence, and the plan runner block + SYS-02 matrix row. README.md "Known environment limitations" intro now enumerates all four declared runner capabilities (`remote-project-gate`, `systemd-user`, `kernel-uinput`, `installed-package`). The legacy receipt at commit `26df6c0` is now described as unsigned/unevidenced pending a signed commit-bound receipt (FACT-007) in README.md (5c row + kernel-backed limitation row), docs/OPERATIONS.md (Current limitation), and the plan runner block. OPERATIONS.md receipt count corrected from 12 to 13 (13 receipts on file). REVIEW.md evidence made internally consistent. `check-docs-sync.sh`, `validate-implementation-plan.py planning`, and `validate-conformance.py planning` all pass. Status kept `pending` per the appended-after-final-audit convention: Task 4 is the single gate that marks the cycle complete.

## Task 13: Close software-fixable test-quality gaps (reviewer audit)
- Status: pending
- Dependencies: Task 1, Task 2, Task 3, Task 9, Task 10
- Source: reviewer audit of the cycle (mem-1786909852-cc11 lesson) — software-fixable, independent of FACT-002..007 hardware/capability/signer blockers. Runtime task `task-1787293432-7333`.
- Scope: `tests/test_editor_list_mode.c`, `tests/test_profile_diagram.c`, `tests/test_overlay_native.c`, `tests/test_connection.c`, `tests/dbus_mock.{c,h}`, `src/app/overlay_service.{c,h}`, `tests/CMakeLists.txt`
- Acceptance criteria:
  - No no-op assertions remain: `test_editor_list_mode.c` tautology `assert_true(strlen(status)==0 || strlen(status)>0)` and both `assert_true(1)`-after-draw stubs, and `test_profile_diagram.c` `assert_true(1)` in test_render_no_crash / test_render_all_buttons / test_render_with_rect / test_shutdown_cleans_up are replaced with real framebuffer/semantic assertions (fb_read_pixels + fb_region_has_content against known backgrounds; highlight differs from panel_bg).
  - test_overlay_native.c registers the production `on_intercept_activating` / `on_intercept_deactivating` / `on_intercept_error` callbacks (exposed from overlay_service.c under CBX_TESTING) instead of local test copies; the test links `controllerbox_testing` and defines CBX_TESTING; release `controllerbox` keeps the callbacks static.
  - `activate_overlay()` (and test_o01/test_o01b) drive the poll through the production `ip_intercept_poll_start()` IDLE→PASS_WAIT transition and `ip_intercept_poll_tick()` PASS_WAIT→ACTIVE via InterceptMode read; no `polls[...].state = IP_POLL_*` manual mutation remains.
  - `test_connection.c` `test_connect_unique_name_fail` exercises the real `get_unique_name` failure path via a new `ip_dbus_mock_set_unique_name_fail()` mock capability, asserting connect still succeeds but the sender stays unverified and no unique name is advertised.
- Verification: `ctest --test-dir build-check -R 'test_profile_diagram|test_editor_list_mode|test_connection|test_overlay_native' --output-on-failure` all pass; full suite 100/100 pass (2 pre-existing hardware skips: `test_kernel_controller`, `test_backend_smoke`); `test_overlay_native` repeated 10× deterministic; `verify-boilerplate.sh` passes; release `controllerbox` library shows `on_intercept_*` as local `t` symbols (no CBX_TESTING leak, F6).
- Documentation impact: none (production behavior unchanged; the CBX_TESTING exposure is test-only and the test-only mock knob does not alter release symbols).
- Result: All four reviewer findings closed. Status kept `pending` per the appended-after-final-audit convention; Task 4 remains the single completion gate.

## Task 14: Fix virtual-controller type-change topology preservation (BUG-0015 software portion)
- Status: pending
- Dependencies: Task 1, Task 2, Task 3, Task 5, Task 9, Task 10, Task 13
- Source: BUG-0015 software-addressable surface (runtime task `task-1787407706-cd8d`). The full BUG-0015 real four-target system-bus acceptance remains externally blocked on FACT-002/FACT-003 and Task 6; this task closes only the production topology-reconciliation defect that is machine-testable through the DBus backend abstraction. External system-bus evidence stays honestly open.
- Scope: `src/manager/controllers_tab.c`, `tests/test_controllers_tab.c`, `tests/dbus_mock.{c,h}`
- Acceptance criteria:
  - `cbx_controllers_tab_change_type` replaces only the selected slot's type and preserves all other topology (SPEC §5.2): it calls `SetTargetDevices` on the composite for `device_index` with only that slot's new type (per the slot model target[i]↔composite[i] enforced by `cbx_reconcile_startup_targets` Phase 1/4), NOT a CSV assembled from every model target's type applied to one composite (which would make the selected composite instantiate N target types and corrupt the other slots).
  - The mock DBus records the string input args of the most recent `call_method`; a semantic regression asserts the exact `SetTargetDevices` CSV is the single new type and that `Add` sends the correct `AttachTargetDevice` target→composite paths. The prior false-positive `test_change_type_mixed` (asserting only rc/device-count) is strengthened to assert the request payload, so the topology-preservation behavior is machine-verified.
  - Full suite passes; regression proven real (buggy code fails `test_change_type_mixed`); no golden regen; no human/visual acceptance claim; FACT-002/FACT-003 stay open; Task 6 stays externally blocked.
- Verification: `nix-shell --run 'cmake -S . -B build-check -DCMAKE_BUILD_TYPE=Debug && cmake --build build-check --parallel'`; `nix-shell --run "ctest --test-dir build-check -R 'test_controllers_tab|test_manager_calls' --output-on-failure"`; full ctest 100/100 (98 pass / 2 pre-existing hardware skips `test_kernel_controller`, `test_backend_smoke`); temporary revert of the fix fails `test_change_type_mixed`, confirming the regression.
- Documentation impact: none to public behavior (a corrected DBus request parameter). BUG-0015 stays open; the plan matrix rows bound to FACT-002/FACT-003 remain `partial`/non-verified.
- Result: BUG-0015 software portion implemented and verified (evidence HEAD `69ccf9e`, re-verified this cycle at the current HEAD with no tree change). `cbx_controllers_tab_change_type` now sends only the selected slot's new type via `ip_composite_set_target_devices` (SPEC §5.2 preserves all other topology) instead of assembling a CSV of every model target's type. The mock DBus gained `ip_dbus_mock_last_call` (records the most recent `call_method` string args); `test_change_type_mixed` and `test_change_type_success` now assert the exact `SetTargetDevices` CSV is the single new type, and the Add path asserts the exact `AttachTargetDevice` target→composite paths. Full suite 100% pass (98 pass / 2 pre-existing hardware skips `test_kernel_controller`, `test_backend_smoke`); `test_change_type_mixed` is proven a real regression (temporarily reverting to the buggy CSV assembly makes it fail). No golden regen; no human/visual acceptance claim; FACT-002/FACT-003 stay open; Task 6 stays externally blocked. Status kept `pending` per the appended-after-final-audit convention: Task 4 remains the single completion gate.

## Task 15: Fix installed mapping-editor diagram geometry (BUG-0018 software portion)
- Status: pending
- Dependencies: Task 1, Task 2, Task 3, Task 5, Task 8, Task 9, Task 10, Task 13
- Source: BUG-0018 (runtime task `task-1787407706-bacd`). Human dogfooding of the fresh installed manager reports the mapping-editor controller diagram is pixelated and stretched and its mapped-button markers are misaligned with the represented controls. The software-addressable defect is the source aspect/raster scaling and marker-coordinate transform in the production renderer (`src/manager/profile_diagram.c`) and the source `data/icons/svg/generic-gamepad.svg`. Per the bug's acceptance, acceptance is semantic installed-window and machine-vision findings-only: goldens are NOT regenerated (golden approval is out-of-band and non-automatable) and no human/visual acceptance claim is made.
- Scope: `src/manager/profile_diagram.c`, `data/icons/svg/generic-gamepad.svg`, `tests/test_profile_diagram.c`, `tests/test_installed_diagram.sh`, capture infrastructure used by the installed-window semantic test.
- Acceptance criteria:
  - The base texture is rasterised at a resolution higher than the largest on-screen diagram rect (so it is down-scaled, never up-scaled → no pixelation), and the content rect preserves the source SVG's aspect ratio (→ no stretching) for the editor's square 300×300 widget rect.
  - Every mapped-button marker/highlight is anchored inside the same aspect-fitted content rect as the rendered controller (the marker-coordinate transform uses the content box, not the full widget rect), so a marker lands on the control it represents regardless of widget/aspect differences.
  - A production-path semantic regression (`test_geometry_pixelation_and_stretch`, `test_geometry_marker_control_alignment`) loads the real installed `generic-gamepad.svg` through the production rasteriser and asserts, from real framebuffer readback: (1) raster resolution ≥ on-screen content size (pixelation guard), (2) content rect preserves texture aspect (stretch guard), (3) every button's marker rect overlaps the light-grey control drawn in the SVG (marker-to-control alignment). Reverting the renderer rounding to truncation fails the geometry test, proving the regression is real.
  - `test_installed_diagram.sh` (installed production-window semantic acceptance) still passes, and the full suite is green except for the pre-existing golden-editor rows whose baselines encode the old (BUG-0018-buggy) diagram and therefore require out-of-band golden re-approval (not performed here; recorded as a finding).
  - Full suite passes except the golden-editor baselines; no golden regen; no human/visual acceptance claim.
- Verification: `nix-shell --run 'cmake -S . -B build-check -DCMAKE_BUILD_TYPE=Debug && cmake --build build-check --parallel'`; `nix-shell --run "ctest --test-dir build-check -R 'test_profile_diagram|test_installed_diagram' --output-on-failure"`; full ctest 99/100 (98 pass / 2 pre-existing hardware skips `test_kernel_controller`, `test_backend_smoke`, plus the pre-existing golden-editor baseline staleness); temporary revert of the renderer rounding fails `test_geometry_pixelation_and_stretch`, confirming the regression is real.
- Documentation impact: no public-behavior change beyond the corrected diagram geometry. OPERATIONS.md/README already describe the diagram; no golden baseline change.
- Result: BUG-0018 software portion implemented and verified at evidence HEAD `ba82a7e` + this cycle's renderer/test corrections (no golden regen). The production renderer now rasterises at 512 and preserves aspect in the content rect, and the button-marker transform is anchored to that content box (not the full widget rect). `generic-gamepad.svg` was redrawn with its controls at the exact normalised coordinates of the button-position table, so marker geometry == control geometry. `test_geometry_pixelation_and_stretch` and `test_geometry_marker_control_alignment` assert pixelation, stretch, and marker-to-control alignment from real framebuffer readback and are proven real regressions (reverting the rounding fix fails them). Full suite 99/100 pass (98 pass / 2 pre-existing hardware skips); `test_golden`'s three editor baselines still fail because they encode the pre-BUG-0018 diagram and golden regeneration is out-of-scope/forbidden — recorded as a finding requiring out-of-band golden re-approval, not performed here. No golden regen; no human/visual acceptance claim; status kept `pending` per the appended-after-final-audit convention; Task 4 remains the single completion gate.

- Result: BUG-0018 software portion implemented and verified at evidence HEAD `ba82a7e` + this cycle's renderer/test corrections (no golden regen). The production renderer now rasterises at 512 and preserves aspect in the content rect, and the button-marker transform is anchored to that content box (not the full widget rect). `generic-gamepad.svg` was redrawn with its controls at the exact normalised coordinates of the button-position table, so marker geometry == control geometry. `test_geometry_pixelation_and_stretch` and `test_geometry_marker_control_alignment` assert pixelation, stretch, and marker-to-control alignment from real framebuffer readback and are proven real regressions (reverting the rounding fix fails them). Full suite 99/100 pass (98 pass / 2 pre-existing hardware skips); `test_golden`'s three editor baselines still fail because they encode the pre-BUG-0018 diagram and golden regeneration is out-of-scope/forbidden — recorded as a finding requiring out-of-band golden re-approval, not performed here. No golden regen; no human/visual acceptance claim; status kept `pending` per the appended-after-final-audit convention; Task 4 remains the single completion gate.

## Task 16: Finish BUG-0018 installed visual capture adapter (visual-capture-driver.sh)
- Status: pending
- Dependencies: Task 1, Task 2, Task 3, Task 5, Task 8, Task 9, Task 10, Task 13, Task 15
- Source: BUG-0018 (runtime task `task-1787416288-52b7`). BUG-0018 is NOT fully software-complete: `scripts/visual-capture-driver.sh` still contained the previously diagnosed generic-consumer adapter defects — unbounded `xdotool search --sync` polling for the wrong title "Controller Box" (the real manager title is "Controller-Box Manager"), the invalid `--overlay` flag (the real mode is `--overlay-service`), a fixed `:97` display with a global `pkill`, first-profile nondeterminism, weak exact-installed-binary/commit binding, and single-PID cleanup. This task repairs the installed exact-commit capture adapter so the BUG-0018 machine visual-audit acceptance path can produce exact-commit installed captures for the manager-main/profiles/editor/overlay states.
- Scope: `scripts/visual-capture-driver.sh`, `.factory/visual-audit-inventory.json` (overlay nav `--overlay` → `--overlay-service`), `tests/test-visual-audit.sh` (adapter invariants + hanging-child/wrong-window regression tests).
- Acceptance criteria:
  - Bounded title/class polling for the exact production window titles (`Controller-Box Manager`, `Controller-Box Overlay`), never `xdotool search --sync`; a missing/wrong-titled window fails closed within a bounded time instead of hanging.
  - Private collision-free display selection (probed against X11 sockets/locks), never a fixed `:N` with a global `pkill`.
  - Isolated HOME/XDG and a deterministic test-owned profile (low display_order sidecar) so the manager's first profile row and the editor it opens are environment-independent.
  - Correct overlay state via `--overlay-service` (never the invalid `--overlay`), backed by a private dbus-daemon system bus when no system InputPlumber bus exists so the installed overlay's production connect/render path runs.
  - Exact installed commit receipt: the working tree must match the requested commit; a retained receipt (schema, commit, install prefix, binary sha256, image sha256, state, window title) is written next to every capture for external human review.
  - Owned process-group cleanup: Xvfb, the app, and any dbus-daemon run in their own `setsid` session/group; cleanup TERM→KILLs the whole owned group (never a single PID, never a global kill).
  - Regression tests: `tests/test-visual-audit.sh` section 13b asserts the driver's source invariants (correct titles, `--overlay-service` only, time-bounded poll, no global `pkill`, collision-free display, owned-group cleanup); section 13c runs the real adapter against a mock installed prefix whose binary never opens the expected window and ignores TERM, asserting bounded fail-closed within a bounded wall time, no partial image, the hanging child reaped (owned-group cleanup), and no leaked Xvfb. Both pass with the real `scripts/visual-capture-driver.sh`; the hanging-child test SKIPs (77) only when Xvfb/xdotool/import are unavailable.
- Verification: `nix-shell --run 'bash tests/test-visual-audit.sh'` passes (all adversarial cases incl. 13b/13c); `./scripts/verify-boilerplate.sh` passes; the real adapter captures all four states against an installed custom-prefix binary under Xvfb with clean owned-group teardown and a valid receipt. No golden regeneration; no human/visual acceptance claim; captures are findings-only for external human review.
- Documentation impact: no public-behavior change to the product; `.factory/visual-audit-inventory.json` overlay-state navigation corrected to `--overlay-service`.
- Result: Implemented and verified at HEAD `a8126a8` (earlier this cycle): the adapter repairs the bounded window polling, `--overlay-service` mode, collision-free display, isolated HOME/XDG + deterministic test-owned profile, exact-installed-commit receipt, and owned process-group cleanup, with adapter-invariant (13b) and hanging-child (13c) regressions. Subsequent human exact-commit capture discovered the adapter's keyboard `Right/Return` navigation still produced wrong states (the profile list captured as the manager-editor). That wrong-state defect is closed by Task 17 (this remediation); Task 16's keyboard navigation was the root cause surface.

## Task 17: Fix visual adapter wrong-state captures and semantic validation (runtime `task-1787418213-7bb2`)
- Status: pending
- Dependencies: Task 1, Task 2, Task 3, Task 5, Task 8, Task 9, Task 10, Task 13, Task 15, Task 16
- Source: runtime task `task-1787418213-7bb2` (BUG-0018 wrong-state capture). Operator exact-commit capture for manager-editor still showed only the Profiles list (Capture-Test-Profile, Default, Create/Edit/Delete buttons), not the profile editor or controller diagram. Root cause: `scripts/visual-capture-driver.sh` navigated with keyboard `Right`/`Return`; after switching to the Profiles tab the profile list is not activated from the tab-bar focus, so `Return` never opens the editor and the driver captured the original window (the profile list) without verifying the state transition — recording a wrong-state frame as evidence.
- Scope: `scripts/visual-capture-driver.sh` (deterministic coordinate-click navigation, window re-acquisition after navigation, semantic frame validation that fails closed on wrong-state content), `.factory/visual-audit-inventory.json` (navigation/expected comments corrected to the verified reality), `tests/test-visual-audit.sh` (13b invariants extended; new 13d real-installed wrong-state regression).
- Acceptance criteria:
  - Navigation uses deterministic coordinate clicks through the production X11 event path (Profiles tab at 640,24; Edit Profile at 302,522 per the fixed 1280x720 manager layout) — never fragile keyboard `Right`/`Return`.
  - The exact production window is re-acquired after navigation before capture (never a stale/replaced handle).
  - The freshly captured frame is semantically validated against the requested state and the driver fails closed (exit 1, refusing to record a wrong-state frame) when the expected content is absent. `manager-editor` requires the controller-diagram region (16,40 300x300) to have content and the profile-list button row (16,500 400x44) to be hidden; `manager-profiles` requires the button row present; `manager-main` requires neither.
  - A test-only validation hook (gated behind `RALPH_VISUAL_AUDIT_TESTING=1`, which the production gate rejects) lets the regression feed a captured frame to the validator.
  - Regression: `tests/test-visual-audit.sh` section 13d runs the real installed binary under Nix/Xvfb, drives manager-main/profiles/editor, asserts each captures with real semantic validation, and asserts the profile-list frame is REJECTED as `manager-editor` (the BUG-0018 wrong-state behavior). 13b asserts the new invariants (coordinate clicks, no keyboard Right/Return, `validate_state`, `convert` dependency). 13d SKIPs (77-equivalent) only when the display tools or an installed binary are unavailable.
- Verification: `nix-shell --run 'bash tests/test-visual-audit.sh'` passes (incl. 13d real-installed regression); `./scripts/verify-boilerplate.sh` passes (13d/13c SKIP outside nix-shell); the real adapter captures manager-main/profiles/editor + overlay-active with genuine semantic validation and clean owned-group teardown. No golden regeneration; no human/visual acceptance claim; captures remain findings-only for external human review.
- Documentation impact: no public-behavior change; `.factory/visual-audit-inventory.json` navigation/expected comments corrected to verified state content.
- Result: Implemented and verified this iteration at HEAD (see verification below). BUG-0018 wrong-state capture closed. Task 4 remains the single completion gate; out-of-band golden re-approval and external facts (FACT-002/003/004/005/006/007) remain open and block implementation completion.

## Task 18: Reject blank overlay and unselected-profile visual captures (runtime `task-1787422841-a8d8`)
- Status: pending
- Dependencies: Task 1, Task 2, Task 3, Task 5, Task 8, Task 9, Task 10, Task 13, Task 15, Task 16, Task 17
- Source: runtime task `task-1787422841-a8d8`. The exact-commit retained-capture audit found two further false-positive states:
  - `overlay-active` returned rc=0 and a receipt for a 250-byte uniform black PNG, despite the inventory requiring active icon/status content. `validate_state` applied no overlay check at all (only a non-empty file check).
  - `manager-profiles` showed `Default [read-only]` with no visibly selected/highlighted row, despite the inventory requiring a selected row; the validator only checked button-row variance.
- Scope: `scripts/visual-capture-driver.sh` (fail-closed semantic validation for overlay-active and manager-profiles selection; deterministic row-0 selection navigation; test-only validation hook moved ahead of the display-tool gate so negative regressions are non-skipping), `tests/test-visual-audit.sh` (13b invariants extended; new non-skipping 13e negative regressions).
- Acceptance criteria:
  - `overlay-active` validation fails closed (exit 1) on a uniform/blank frame, honestly blocking the state when the required active gamepad stimulus via the real system service is unavailable, instead of recording a black false-positive as evidence.
  - `manager-profiles` validation requires a visibly selected profile row (row 0 background differs from an unselected row's background, panel_bg_hover vs panel_bg) in addition to the button row; an unselected list fails closed.
  - Deterministic navigation clicks the first profile row after switching to the Profiles tab so the declared selection is genuinely rendered.
  - The test-only validation hook runs before the Xvfb/xdotool/import gate so the validator can be exercised without a display server or installed binary.
  - Regression: `tests/test-visual-audit.sh` section 13e (non-skipping, needs only ImageMagick `convert`) feeds a uniform-black frame as `overlay-active` and an unselected list as `manager-profiles`, asserts both are rejected with the intended diagnostics, and asserts a selected list still passes; 13b asserts the new source invariants (`overlay frame is blank/uniform`, `no selected profile row`, `row_selected`).
- Verification: `nix-shell --run 'bash tests/test-visual-audit.sh'` passes (incl. 13d real-installed and 13e non-skipping negative regressions); `./scripts/verify-boilerplate.sh` passes; `shellcheck scripts/visual-capture-driver.sh tests/test-visual-audit.sh` clean; the real installed adapter captures manager-main/profiles/editor with rc=0 including the new selection check. `overlay-active` remains honestly blocked on the unavailable real-system service stimulus (FACT-002/003). No golden regeneration; no human/visual acceptance claim.
- Documentation impact: no public-behavior change; overlay-active is documented as requiring the real system InputPlumber service stimulus (blocked in this environment).
- Result: Implemented and verified this iteration at HEAD. Task 4 remains the single completion gate; out-of-band golden re-approval and external facts (FACT-002/003/004/005/006/007) remain open and block implementation completion.

  Atomic-publish commit-point hardening (runtime `task-1787427829-3013`, closes `task-1787424775-c01b` software acceptance): `scripts/visual-capture-driver.sh` now publishes the receipt FIRST and the image as the final commit-point rename, so an image is visible at OUTPUT only with its matching validated receipt; it refuses pre-existing OUTPUT/receipt/symlink/hardlink destinations instead of `mv -f` replacing unowned paths; it fsyncs the receipt temp and the output directory before/after the renames. `tests/test-visual-audit.sh` 13f was extended with a receipt-publish-failure case (blocking `mv` on PATH proves a validated capture whose receipt cannot land is withheld entirely), pre-existing OUTPUT refusal and pre-existing receipt refusal on validated captures, and a direct rejected-overlay case (a uniform/blank frame launched through the real installed overlay is rejected and leaves neither OUTPUT nor a receipt nor a temp). The 13f signal case no longer pre-creates the park marker (`rm -f` instead of `: >`), so TERM is delivered only after the mock import actually parks with the owned temp present. Verified: `nix-shell --run 'bash tests/test-visual-audit.sh'` exit 0 with 13f non-skipping (`atomic capture publication passed (13f)`), all new adversarial cases pass; `./scripts/verify-boilerplate.sh` passes at host. `shellcheck scripts/visual-capture-driver.sh tests/test-visual-audit.sh` clean. No golden regen; no human/visual acceptance claim.

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