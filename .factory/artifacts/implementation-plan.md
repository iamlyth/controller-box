---
spec_path: docs/SPEC.md
spec_commit: 3a10f6b7d04a615b2b9d06eef6c91e431fa9c079
spec_blob: 58f5d3cb72bc6b3e5f573fa09a63c11a653ed577
base_commit: faa56b95dd750433cdef81813ac015045658dfc4
status: active
---

# Implementation Plan

## 1. Goal and non-goals

**Goal:** Close every specification conformance gap in `docs/SPEC.md` so the autonomous implementation loop can reach the §11.2 definition of done: all requirements `verified`, complete interaction traversal, visual acceptance, regression gates, known-defect accounting, independent review, accurate documentation, and clean Git state.

**Non-goals:** Modifying the committed specification. Inventing undeclared runner capabilities. Replacing InputPlumber's engine. Implementing post-v1 deferred items (§13).

## 2. Architecture and constraints

Controller-Box is a single C binary (`controller-box`) with two modes: overlay service and manager. It wraps InputPlumber via sd-bus DBus. SDL2 provides rendering; nanosvg rasterizes controller icons. Config is YAML (libyaml). Tests use cmocka, a private native-signature DBus server (`native_ip_server.c`), and SDL software/headless renderers.

**Constraints from `.factory/environment.toml`:** Only `remote-project-gate` and `systemd-user` capabilities are declared. Five campaign-required capabilities (`physical-controller`, `kernel-uinput`, `inputplumber-system-dbus`, `gpu-compositor`, `installed-package`, `target-consumer`) are undeclared and unevidenced. The implementation worker must not invent them; infrastructure-blocked tasks require external runner provisioning.

**Build:** Use `nix-shell --run '...'` for SDL2, systemd, libyaml, cmocka, Xvfb. Clean stale build dirs before final verification.

## Specification conformance matrix

| REQ ID | Spec § | Classification | Current evidence | Task |
|--------|--------|---------------|-----------------|------|
| ARCH-01 | §2.4 | partial | `ip_connection.c` NameOwnerChanged subscription + degraded state; `test_connection.c` mock tests. Unit file requirements (no `After=/Requires=inputplumber.service`, bounded `Restart=on-failure`) verified in `test_service_install.c:160,164`. Backend-dependent controls disabled in D01. No ≤2s re-enumeration timing assertion. | Task 3 |
| ARCH-02 | §2.5 | verified | `ip_composite.c` SetInterceptActivation; `ip_intercept_poll.c` 50ms poll; `trigger.c` registration; `lifecycle.c` close→PASS. `test_intercept_poll.c`, `test_trigger.c`, `test_close.c`, `test_overlay_native.c` O01. | — |
| ARCH-02 | §2.1 | verified | GUI never touches input routing directly — all state changes via DBus. `grep -r 'include.*dbus_mock.h' src/` confirms no mock in production. No direct evdev/uinput calls in `src/`. | — |
| ARCH-03 | §2.3 | verified | Single binary `controller-box` with `--overlay-service` and `--manager` modes. `main.c` mode dispatch. Both modes tested via `test_overlay_*.c` and `test_manager_*.c`. | — |
| ARCH-04 | §2.5 | verified | `ip_composite.c` SetInterceptActivation; `ip_intercept_poll.c` 50ms poll; `trigger.c` registration; `lifecycle.c` close→PASS. `test_intercept_poll.c`, `test_trigger.c`, `test_close.c`, `test_overlay_native.c` O01. | — |
| SYS-03 | §3 | verified | SDL2/SDL2_ttf/SDL2_image/systemd/libyaml/nanosvg deps via nix-shell build. Display server support via SDL2 (X11/Wayland/Gamescope). Polkit rules documented in spec. | — |
| OVL-01 | §4.1–4.3 | verified | `grid_render.c`, `player_mode.c` grid + left/right slot move + up/down profile cycle. `test_player_mode.c`, `test_grid_render.c`, `test_overlay_interaction.c` O02–O05, `test_overlay_native.c` O02–O05. | — |
| OVL-02 | §4.4 | verified | `host_mode.c` R3 enter/exit/toggle, frozen rows. `test_host_mode.c`, `test_overlay_interaction.c` O06–O09, `test_overlay_native.c` O06–O09. | — |
| OVL-03 | §4.5 | verified | `conflict.c` detect + red flag + auto-resolve to lowest unoccupied. `test_conflict.c`, `test_overlay_interaction.c` O10b/O13, `test_overlay_native.c` O13. | — |
| OVL-04 | §4.6 | verified | `profile_cycle.c` profile follows controller across columns. `test_profile_cycle.c`. | — |
| OVL-05 | §4.7 | verified | `dynamic_columns.c` column count from targets, rebuild on hotplug. `test_dynamic_columns.c`, `test_overlay_interaction.c` hotplug test. | — |
| OVL-06 | §4.8 | verified | Model name + slot position display; no nickname prompts. `grid_render.c` row labels. `test_grid_render.c`. | — |
| OVL-07 | §4.9 | verified | `surface_build.c` pre-built render-to-texture, zero-alloc show. `test_surface_build.c`. | — |
| OVL-08 | §4.10 | partial | `test_overlay_visual.c` framebuffer pixel tests for 7 states. BUT icon textures don't load through production `cbx_icon_dir()` path (BUG-0009); tests bypass with `SVG_DIR` direct to `cbx_icon_cache_init`. Virtual-device icons may be absent in production. | Task 1 |
| OVL-09 | §4.10 | partial | Overlay visual tests use `SVG_DIR=CBX_SOURCE_DIR"/data/icons/svg/"` directly, bypassing production `cbx_icon_dir()`. Tests pass while production renders without icons. | Task 2 |
| MGR-01 | §5.1 | verified | `manager.c` 3 tabs, tabbar, focus chain, pointer hit-test, controller-to-key mapping. `test_manager_tabs.c`, `test_focus.c`, `test_manager_visual.c`. | — |
| MGR-02 | §5.2 | verified | `controllers_tab.c` add/remove/type-change with DBus calls + topology reconciliation in `overlay_service.c`. `test_controllers_tab.c`, `test_manager_interaction_ctrl.c` M04–M09, `test_overlay_reconcile.c`, `test_native_dbus.c` Test 6. | — |
| MGR-03 | §5.3 | verified | `profiles_tab.c` browse/create (Default copy/Empty/Clone)/edit/delete, confirm-quit. `test_profiles_tab.c`, `test_manager_interaction_prof.c` M10–M20. | — |
| MGR-04 | §5.4 | verified | `profile_editor_list.c` binding list + `profile_editor_seq.c` sequential mode. `profile_validate.c` NES minimum. `profile_save.c` save with validation. `test_editor_list_mode.c`, `test_editor_seq_mode.c`, `test_profile_validate.c`, `test_profile_save.c`, `test_manager_interaction_prof.c` M28–M38. | — |
| MGR-05 | §5.5 | verified | `settings_tab.c` all settings (launch_at_boot, theme, opacity, VC count/types, trigger, icon overrides). `test_settings_tab.c`, `test_manager_interaction_ctrl.c` M21–M27. | — |
| MGR-06 | §5.6 | partial | `test_manager_visual.c` framebuffer tests for all 3 tabs + editor states. BUT `test_manager_visual.c:333` sets `CBX_ICON_DIR` env var, masking production icon path failure (BUG-0010). Diagram SVG not rendered from build tree (BUG-0008). | Task 2 |
| MGR-07 | §5.7 | partial | Interaction inventory M01–M38 + D01–D08 in `test_manager_interaction_ctrl.c`/`test_manager_interaction_prof.c` through `cbx_manager_handle_event`. Gaps: M09 type-picker cancel pointer path, M16 name-input cancel pointer path, VC types slots 1–3 not individually tested. | Task 4 |
| MGR-08 | §5.7 | partial | `test_golden.c` golden image comparison with `setenv("CBX_ICON_DIR",...)` bypass (BUG-0010). Golden baselines may not match production output. | Task 2 |
| OVL-10 | §5.7 | partial | Overlay interaction O01–O13 tested via keyboard events (supplemental). DBus InputEvent path (primary production transport) tested only for O11 (multi-controller) and O10c (close via DBus B). Basic navigation O02–O09 lack DBus InputEvent tests in native suite. | Task 5 |
| ID-01 | §6.2–6.3 | verified | `identity.c` 4-layer extraction with BT:/USB:/USB:phys:/ORDER: prefixes. `identity_downgrade.c` downgrade detection. `assign.c`, `assign_persist.c`. `test_identity.c`, `test_identity_downgrade.c`, `test_assign.c`, `test_assign_persist.c`. | — |
| ID-02 | §6.2 | verified | `gamepad_order_restore.c` GamepadOrder restoration via PersistentId mapping. `test_order_restore.c`. | — |
| CFG-01 | §7.1–7.4, §7.6 | verified | `config_settings.c` settings.yaml, `config_assignments.c` assignments.yaml with gamepad_order, `config_profile.c` InputPlumber device_profile_v1 YAML. `test_settings.c`, `test_assignments.c`, `test_profile_yaml.c`. | — |
| CFG-02 | §7.5 | verified | `config_profile_meta.c` sidecar with display_name/icon/display_order/description, O_NOFOLLOW, atomic write. `test_profile_list.c` sidecar tests. | — |
| ICO-01 | §8.1–8.5 | partial | `icon_cache.c` nanosvg rasterization, `icon_map.c` YAML mapping, `icon_lookup.c` runtime lookup with override. Custom SVGs exist: `arcade-stick.svg`, `hitbox.svg`, `steam-deck.svg`, `generic-gamepad.svg` in `data/icons/svg/`. BUT `icon_cache.c:99` builds `{icon_dir}/{name}.svg` without `/svg/` subdirectory — SVGs installed to `{icon_dir}/svg/`. Production icon loading fails silently. | Task 1 |
| PKG-01 | §9.1 | partial | Flatpak manifest complete and marked experimental. No Flathub install advertised. BUT no clean Flatpak build has passed installed functional gate; not published. | Task 11 |
| PKG-02 | §9.2–9.4 | verified | CMake `make install` places binary, service, desktop entry, icons, mapping YAML. `test_packaging.sh`, `test_packaging_install.sh`, `test_service_install.c`. | — |
| PKG-03 | §9.1, §9.3 | partial | `service_install.c` implements `cbx_service_install()` with atomic write, systemctl enable. BUT `cbx_service_install()` is never called from `manager.c` — not wired into manager UI. No first-run detection or "Enable overlay service?" dialog exists. Tests call `cbx_service_install()` directly, bypassing production dispatch. | Task 7 |
| DBUS-01 | §10.1 | verified | `dbus_client.c` sd-bus native type handling (u, b, as, s), Version check, ObjectManager enumeration, NameOwnerChanged. `test_dbus_signatures.c`, `test_native_dbus.c`, `test_connection.c`. | — |
| DBUS-02 | §10.1 | partial | Private native-signature DBus tests satisfy §5.7. No test against real InputPlumber system DBus service. | Task 11 |
| DBUS-03 | §10.2 | verified | All §10.2 API members implemented: Manager (CreateTargetDevice, StopTargetDevice, AttachTargetDevice, GamepadOrder, properties), Composite (SetInterceptActivation, InterceptMode, LoadProfilePath/Yaml, SetTargetDevices, properties), Target/Source interfaces. `test_composite_calls.c`, `test_manager_calls.c`, `test_native_dbus.c`. | — |
| DBUS-04 | §10.3 | verified | All 5 gaps have workarounds: InterceptMode poll (gap 1), GamepadOrder save in config (gap 2), temp composite YAML (gap 3), filesystem profile enumeration (gap 4), no source add/remove needed (gap 5). `test_intercept_poll.c`, `test_gamepad_order.c`, `test_create_composite.c`. | — |
| PERF-01 | §11 | partial | `test_overlay_latency.c` measures ALL-detection→present <10ms p99, close <1ms, idle <5ms, footprint <50MB. Button-to-frame ≤75ms p99 derived from poll+show, not measured end-to-end with real timer on target hardware. | Task 10 |
| VRF-01 | §11.1.1 | verified | Deterministic framebuffer tests via `SDL_RenderReadPixels` through production composition. `test_overlay_visual.c`, `test_manager_visual.c`. | — |
| VRF-02 | §11.1.2 | verified | Region-level pixel assertions via `fb_assert.c` helpers. All visual tests. | — |
| VRF-03 | §11.1.3 | partial | Golden images for 12 states with ±3/channel tolerance. BUT `test_backend_smoke_sw.c:429,646` prints `[SKIP]` on missing baseline instead of failing. | Task 6 |
| VRF-04 | §11.1.4 | verified | On mismatch, saves actual/expected/diff to `tests/golden-fail/`. `test_golden.c`. | — |
| VRF-05 | §11.1.5 | partial | `test_installed_functional.c` runs with private DBus + SDL virtual gamepad. BUT uses `SDL_JoystickAttachVirtual` (process-local), not kernel-backed `/dev/uinput` synthetic gamepad. `test_kernel_controller.c` skips (exit 77) — `/dev/uinput` unavailable. | Task 7 |
| VRF-06 | §11.1.6 | partial | `test_backend_smoke.c` skips (exit 77) — no accelerated GPU backend. `test_backend_smoke_sw.c` runs software renderer. | Task 8 |
| VRF-07 | §11.1.7 | missing | No human release acceptance artifact (reviewer, date, hardware, captures, criteria). | Task 10 |
| SYS-01 | §3 | partial | x86_64 build verified. No aarch64 build executed or evidenced. | Task 9 |
| SYS-02 | §3 | partial | No Pi 4 latency measurement. x86_64 latency measured but not on minimum supported hardware. | Task 10 |
| DOD-01 | §11.2.1 | partial | Conformance matrix covers all requirements; non-verified rows mapped to tasks. | Task 14 |
| DOD-02 | §11.2.2 | verified | Production-path tests through `cbx_manager_handle_event`, `cbx_overlay_service_step`, native DBus. Direct callback tests are supplemental. | — |
| DOD-03 | §11.2.3 | partial | Interaction inventory M01–M39 + O01–O13 with controller+pointer paths. Gaps in M09/M16 pointer, O02–O09 DBus InputEvent, kernel-backed controller transport. | Tasks 4, 5, 8, 9 |
| DOD-04 | §11.2.4 | partial | §§4.10, 5.6, 11.1 visual tests pass for normal/degraded/error states. Icon path bugs mask missing icons in production. | Tasks 1, 2 |
| DOD-05 | §11.2.5 | partial | Clean build + 98 tests pass. `test_kernel_controller` and `test_backend_smoke` skip. Golden baseline skip in SW smoke. | Tasks 6, 9, 10 |
| DOD-06 | §11.2.6 | partial | 3 open bugs (BUG-0008, BUG-0009, BUG-0010) contradict v1 icon rendering requirements. | Tasks 1, 2 |
| DOD-07 | §11.2.7 | verified | Read-only reviews (campaign audit round 5) found no unresolved blocking issue beyond listed tasks. | — |
| DOD-08 | §11.2.8 | partial | README/OPERATIONS.md exist. Build/install commands work on x86_64. Flatpak not verified. aarch64 not verified. | Task 14 |
| DOD-09 | §11.2.9 | verified | Git tree clean on develop; task ledger present; spec binding fresh. | — |

## Interaction acceptance inventory

### Manager controls (M01–M38 + D01–D08)

Every control is tested through production dispatch (`cbx_manager_handle_event` for SDL events, `cbx_manager_handle_mouse_event` for pointer). Controller path uses keyboard-mapped SDL events (supplemental per §5.7) in mock-DBus tests and `SDL_JoystickSetVirtualButton` (primary controller transport) in native-DBus tests. Pointer path uses `SDL_MOUSEMOTION` + `SDL_MOUSEBUTTONDOWN/UP` → `cbx_manager_hit_test`.

**Tab bar:** M01 switch→Controllers, M02 switch→Profiles, M03 switch→Settings. Both paths verified (mock + native).

**Controllers tab:** M04 list select, M05 Add button, M06 Remove button, M07 Change Type button, M08 type picker confirm, M09 type picker cancel (B). M04–M08 both paths verified. **M09 pointer path missing.**

**Profiles tab:** M10 list select, M11 Create button, M12 create source picker, M13–M16 name input (chars/backspace/confirm/cancel), M17 Edit button, M18 Delete button, M19 delete confirm, M20 delete cancel. M10–M15, M17–M20 both paths verified. **M16 pointer path missing** (controller-only, `NOT_APPLICABLE` for pointer per inventory — but inventory marks `AVAIL`).

**Settings tab:** M21 list select, M22 toggle (launch_at_boot), M23 enter edit, M24 adjust value (theme/opacity/VC count/VC type/trigger/icon override), M25 confirm edit, M26 cancel edit, M27 Save button. All both paths verified (mock + native). **VC types slots 1–3 not individually tested** (only slot 0).

**Profile editor:** M28 binding list nav, M29 edit binding (A), M30 target pick confirm, M31 capture begin, M32 capture physical button (DBus InputEvent), M33 sequential begin, M34 sequential capture (DBus InputEvent), M35 sequential skip (B), M36 sequential cancel (Start), M37 save and close, M38 cancel/discard. M28–M38 verified through production dispatch. M32/M34 use `backend->inject_signal` (DBus InputEvent path). M35/M36 controller-only (`NOT_APPLICABLE` for pointer — physical button actions).

**Degraded scenarios:** D01 InputPlumber unavailable, D02 remove no device, D03 delete no profile, D04 save missing NES, D05 settings cancel, D06 DBus failure, D07 filesystem failure, D08 empty profile create. All verified through production dispatch, both paths where applicable.

**First-run service install (M39):** Spec §9.1 requires a first-run "Enable overlay service?" dialog in the manager that installs the systemd user service. `cbx_service_install()` exists in `service_install.c` but is **not wired into `manager.c`** — no first-run detection, no interactive dialog, no production-dispatch test. This is a missing interactive control.

**Additional:** Post-resize hit testing, decorative widget exclusion, focus chain traversal, unsaved-close prompt, orphan columns error — all verified.

### Overlay actions (O01–O13)

All overlay actions tested through `cbx_overlay_service_step` (production poll loop). Input arrives via keyboard (`sdl_key_to_pm_input`, supplemental) or DBus InputEvent (`ip_input_events_process` → `cbx_overlay_input_cb`, primary production transport).

- O01 open (trigger activation): poll-driven, verified mock + native.
- O01b deactivation: poll detects PASS, verified.
- O02–O05 move left/right, cycle profile up/down: keyboard only (supplemental). **DBus InputEvent tests missing for basic navigation.**
- O06–O09 host mode enter/navigate/move-slot/exit: keyboard only. **DBus InputEvent tests missing.**
- O10 close (B): keyboard + DBus B (`inject_input`), verified.
- O10b/O13 conflict resolution: verified mock + native.
- O11 multi-controller independence: DBus InputEvent, verified mock + native.
- O12 host profile cycle: DEFERRED per §13, tests pin current behavior.

**Semantic outcome verification:** Tests assert state transitions (mode changes, DBus calls, file mutations, device counts, assignment sync), not mere handler return values.

## Task list

## Task 1: Fix icon cache SVG path mismatch (BUG-0008, BUG-0009)
- Status: pending
- Dependencies: none
- Scope: `src/icons/icon_cache.c` (path construction), `src/config/config_paths.c` (verify `cbx_icon_dir()` return value), `src/app/overlay_service.c` (verify init call)
- Acceptance criteria: `cbx_icon_cache_load` constructs SVG paths as `{icon_dir}/svg/{name}.svg`, matching the CMake install layout (`CMakeLists.txt:204` installs to `${CBX_ICON_INSTALL_DIR}/svg`). A new test exercises the production `cbx_icon_dir()` → `cbx_icon_cache_init()` → `cbx_icon_cache_load()` path and asserts at least one icon texture loads from the installed directory structure. BUG-0008 and BUG-0009 are resolved in `.factory/bugs/open.md` (moved to closed ledger).
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'icon_cache' --output-on-failure"`; new production-path icon load test passes. Verify `cbx_icon_dir()` returns a path whose `svg/` subdirectory contains SVGs in the build tree.
- Documentation impact: None beyond bug ledger update.

## Task 2: Remove test icon-path env-var bypasses (BUG-0010)
- Status: pending
- Dependencies: Task 1
- Scope: `tests/test_manager_visual.c`, `tests/test_golden.c`, `tests/test_overlay_visual.c`, `tests/test_installed_functional.c`, `CMakeLists.txt` (test compile definitions)
- Acceptance criteria: No test calls `setenv("CBX_ICON_DIR", ...)`. No test passes a hardcoded `SVG_DIR`/`OVERLAY_SVG_DIR` directly to `cbx_icon_cache_init` — all use `cbx_icon_dir()` (the production path). CMake configures test targets with a compile-time `CBX_ICON_DIR` pointing to the source tree `data/icons` directory (or equivalent build-tree install) so `cbx_icon_dir()` resolves correctly without runtime env var injection. `test-production-path-bypass.sh` passes. All visual, golden, and installed functional tests still pass. BUG-0010 is resolved in bug ledger.
- Verification: `grep -r 'setenv.*CBX_ICON_DIR' tests/` returns no matches. `./scripts/test-production-path-bypass.sh` passes. `nix-shell --run "ctest --test-dir build-check -R 'golden|manager_visual|overlay_visual|installed_functional' --output-on-failure"`.
- Documentation impact: None beyond bug ledger update.

## Task 3: Add re-enumeration timing test (§2.4)
- Status: pending
- Dependencies: none
- Scope: `tests/test_connection.c` or `tests/test_native_dbus.c` (new timing test)
- Acceptance criteria: A test verifies that after `NameOwnerChanged` (service acquisition), re-enumeration completes within 2 seconds. Test uses private native-signature DBus server, simulates service loss/acquisition, measures elapsed time from NameOwnerChanged callback to re-enumeration completion, asserts ≤2s. Test runs through production `ip_connection.c` callback path.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'connection|native_dbus' --output-on-failure"`; new timing test passes.
- Documentation impact: None.

## Task 4: Complete missing interaction test paths
- Status: pending
- Dependencies: none
- Scope: `tests/test_manager_interaction_ctrl.c` (M09 pointer), `tests/test_manager_interaction_prof.c` (M16 pointer), `tests/test_settings_tab.c` or `tests/test_manager_interaction_ctrl.c` (VC types 1–3)
- Acceptance criteria: M09 (type picker cancel) has a pointer-path test: mouse click to open type picker, then mouse click on a cancel/dismiss area or ESC, asserts mode returns to LIST with no DBus call. M16 (name input cancel) pointer path: if the inventory marks `AVAIL`, add a pointer test or document `NOT_APPLICABLE` with justification. VC types slots 1–3 each have a test that cycles the type and verifies the persisted value. All new tests go through `cbx_manager_handle_event` / `cbx_manager_handle_mouse_event`.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'interaction' --output-on-failure"`; all interaction tests pass.
- Documentation impact: Update `tests/interaction_inventory.c` if any `AVAIL`/`NOT_APPLICABLE` status changes.

## Task 5: Add overlay DBus InputEvent navigation tests
- Status: pending
- Dependencies: none
- Scope: `tests/test_overlay_native.c` (new DBus InputEvent tests for O02–O09)
- Acceptance criteria: Tests for O02 (move left), O03 (move right), O04 (cycle profile up), O05 (cycle profile down), O06 (enter host mode), O07 (host navigate rows), O08 (host move slot), O09 (exit host mode) that dispatch input via `emit_input_event` on the native DBus server (the production InputEvent signal path), not keyboard events. Tests assert the same semantic outcomes as the existing keyboard tests. Existing keyboard tests remain as supplemental evidence.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'overlay_native' --output-on-failure"`; new DBus InputEvent navigation tests pass.
- Documentation impact: None.

## Task 6: Fix golden comparison silent skip in backend smoke SW
- Status: pending
- Dependencies: none
- Scope: `tests/test_backend_smoke_sw.c` (lines ~429, ~646)
- Acceptance criteria: When a golden baseline file is not found, the test fails with a clear diagnostic message instead of printing `[SKIP]` and continuing. Golden baselines exist for all required states (verified: 12 PNGs in `tests/golden/`). The `printf("[SKIP]")` pattern is replaced with `fail()` or `assert_false` with a message identifying the missing baseline.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'backend_smoke_sw' --output-on-failure"`; test passes with existing baselines. Temporarily remove a baseline and verify the test fails.
- Documentation impact: None.

## Task 7: Wire first-run service installation into manager UI (§9.1, §9.3)
- Status: pending
- Dependencies: none
- Scope: `src/manager/manager.c` (first-run detection + dialog), `src/manager/service_install.c` (verify called from UI), `tests/test_manager_interaction_ctrl.c` or `tests/test_service_install.c` (production-dispatch test)
- Acceptance criteria: Manager detects first-run (no `controller-box.service` in `~/.config/systemd/user/`) and shows an "Enable overlay service?" dialog. Controller A confirms → calls `cbx_service_install()` → writes unit file → `systemctl --user enable --now`. Controller B cancels. Pointer path: mouse click on confirm/cancel buttons. Both paths tested through `cbx_manager_handle_event` / `cbx_manager_handle_mouse_event` (production dispatch), not direct `cbx_service_install()` calls. Existing direct-call tests remain as supplemental. Conformance row PKG-03 moves to verified.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'service_install|interaction_ctrl' --output-on-failure"`; new production-dispatch test passes. Verify `cbx_service_install` is called from `manager.c` event handler path.
- Documentation impact: Document first-run flow in README.md.

## Task 8: Prepare kernel-backed controller test code (software)
- Status: pending
- Dependencies: Task 2
- Scope: `tests/test_installed_functional.c` (prefer `/dev/uinput` with SDL fallback, fix misleading comment), `.factory/environment.toml` (declare `kernel-uinput` capability)
- Acceptance criteria: `test_installed_functional.c` detects `/dev/uinput` availability and uses a kernel-backed synthetic gamepad when present, falling back to `SDL_JoystickAttachVirtual` only when `/dev/uinput` is absent. The misleading comment at line 9 ("kernel-backed" when code uses process-local SDL API) is corrected. `kernel-uinput` capability is declared in `environment.toml` (evidence may not yet be available — that is Task 9). The test code is ready to run to exit 0 when `/dev/uinput` is provisioned.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'installed_functional' --output-on-failure"`; test still passes on current runner (SDL fallback path). Code inspection confirms `/dev/uinput` detection logic exists.
- Documentation impact: None beyond environment.toml update.

## Task 9: Kernel-backed controller runner provisioning (kernel-uinput, physical-controller)
- Status: pending
- Dependencies: Task 8
- Scope: `.factory-state/runner-evidence/` (runner receipt), `tests/test_kernel_controller.c` (run to completion)
- Acceptance criteria: `kernel-uinput` (and/or `physical-controller`) capability has a matching runner receipt validated by `scripts/check-factory-runner-evidence.py`. `test_kernel_controller` runs to exit 0 (not 77). `test_installed_functional` uses kernel-backed path when `/dev/uinput` available. Conformance rows VRF-05, DOD-03 move to verified.
- Verification: `python3 scripts/check-factory-runner-evidence.py --print-capabilities` shows `kernel-uinput`. `nix-shell --run "ctest --test-dir build-check -R 'kernel_controller' --output-on-failure"` exits 0.
- Documentation impact: Document runner setup requirements in `docs/OPERATIONS.md`.
- Note: Requires a runner with `/dev/uinput` access (`modprobe uinput` + permissions). If the runner cannot be provisioned, mark `blocked` with evidence.

## Task 10: GPU backend smoke acceptance (gpu-compositor)
- Status: pending
- Dependencies: none
- Scope: `.factory/environment.toml` (capability declaration), `.factory-state/runner-evidence/` (runner receipt), `tests/test_backend_smoke.c` (run to completion)
- Acceptance criteria: `gpu-compositor` capability is declared in `environment.toml` with a matching runner receipt. `test_backend_smoke` runs to exit 0 (not 77) with an accelerated OpenGL/GLES backend. Framebuffer invariants are verified on the hardware backend. Conformance rows VRF-06, DOD-05 move to verified.
- Verification: `python3 scripts/check-factory-runner-evidence.py --print-capabilities` shows `gpu-compositor`. `nix-shell --run "ctest --test-dir build-check -R 'backend_smoke$' --output-on-failure"` exits 0.
- Documentation impact: Update `.factory/environment.toml`. Document GPU backend requirements in `docs/OPERATIONS.md`.
- Note: Requires a runner with an accelerated GPU backend. If unavailable, mark `blocked` with evidence.

## Task 11: aarch64 build target (target-consumer)
- Status: pending
- Dependencies: none
- Scope: `CMakeLists.txt` (cross-compile toolchain), `cmake/aarch64-toolchain.cmake` (new), build verification
- Acceptance criteria: An aarch64 cross-compilation CMake toolchain file exists. `cmake -S . -B build-aarch64 -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake` configures successfully. `cmake --build build-aarch64` compiles with zero warnings. If a cross-compiler is available in the nix-shell, the build is executed and evidenced. Conformance row SYS-01 moves to verified (or partial with evidence if cross-compile only, no runtime test).
- Verification: `nix-shell --run "cmake -S . -B build-aarch64 -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake -DCMAKE_BUILD_TYPE=Debug && cmake --build build-aarch64 --parallel 2>&1 | tee /tmp/aarch64-build.log"`; zero warnings. If cross-compiler unavailable, mark `blocked` with evidence and the toolchain file as deliverable.
- Documentation impact: Document aarch64 build instructions in README.md.

## Task 12: Target hardware latency and human release acceptance (target-consumer)
- Status: pending
- Dependencies: none
- Scope: `tests/test_overlay_latency.c` (Pi 4 measurement), `docs/human-release-acceptance.md` (new template/artifact)
- Acceptance criteria: Overlay appearance latency is measured on Pi 4 (or equivalent ARM64 with GLES 3.0) and recorded: ≤75ms p99, ≤100ms max, <10ms p99 detection-to-present. A human release acceptance artifact exists with reviewer name, date, hardware, representative captures, and criteria checklist (legibility, clipping, focus indication, contrast, controller-only usability). Conformance rows SYS-02, PERF-01, VRF-07 move to verified.
- Verification: Latency measurement log or artifact committed. Human acceptance artifact signed and committed.
- Documentation impact: `docs/human-release-acceptance.md` created. README.md updated with Pi 4 performance results.
- Note: Requires Pi 4 or equivalent ARM64 hardware and a human reviewer. If unavailable, mark `blocked` with evidence. The human acceptance template can be created in software as a partial deliverable.

## Task 13: Flatpak build and real InputPlumber system DBus acceptance (installed-package, inputplumber-system-dbus)
- Status: pending
- Dependencies: none
- Scope: `packaging/org.shadowblip.ControllerBox.yaml` (Flatpak build verification), `.factory/environment.toml` (capability declarations), end-to-end test against real InputPlumber
- Acceptance criteria: `installed-package` and `inputplumber-system-dbus` capabilities declared with runner receipts. A clean Flatpak build passes the installed functional gate (host profile paths visible, host InputPlumber DBus access verified). An end-to-end test runs against a real InputPlumber system DBus service (not a private mock) and verifies: ObjectManager enumeration, CreateTargetDevice, InterceptMode lifecycle, profile loading, GamepadOrder. If Flatpak publication is achieved, documentation may advertise the install command; otherwise the manifest remains experimental. Conformance rows PKG-01, DBUS-02 move to verified.
- Verification: `flatpak-builder ... build-flatpak packaging/org.shadowblip.ControllerBox.yaml` succeeds. Installed functional gate passes in Flatpak. `python3 scripts/check-factory-runner-evidence.py --print-capabilities` shows both capabilities.
- Documentation impact: README.md updated with Flatpak install instructions only if published. `docs/OPERATIONS.md` updated with InputPlumber system DBus setup.
- Note: Requires `flatpak-builder` + SDK runtimes and InputPlumber installed on the runner. If unavailable, mark `blocked` with evidence.

## Task 14: Final documentation and specification audit
- Status: pending
- Dependencies: Tasks 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13
- Scope: `.factory/artifacts/implementation-plan.md` (conformance matrix update), `README.md`, `docs/OPERATIONS.md`, `.factory/bugs/open.md` (empty), `.factory/bugs/closed.md` (all bugs closed), full clean verification
- Acceptance criteria: Every conformance matrix row is `verified` or has a `blocked` task with evidence and human-approved deferral. The interaction inventory is exhaustive with all controller and pointer paths evidenced. `.factory/bugs/open.md` contains no unresolved v1-impacting defects. Independent adversarial reviews (correctness, test-quality, security, documentation) find no blocking issues. Full clean verification passes: `nix-shell --run './scripts/verify-project.sh'` with zero warnings, zero unexplained skips, zero sanitizer defects. `./scripts/final-gate.sh --implementation` passes. Documentation matches observed behavior. Git tree is clean on `develop`. The canonical definition of done (§11.2) is satisfied for all software-addressable requirements; blocked requirements have documented evidence and human-approved deferrals.
- Verification: `./scripts/final-gate.sh --implementation` passes. `nix-shell --run './scripts/verify-project.sh'` passes. `grep -r 'setenv.*CBX_ICON_DIR' tests/` returns no matches. `.factory/bugs/open.md` JSON array is empty. All conformance rows verified or blocked-with-deferral.
- Documentation impact: Final README.md and docs/OPERATIONS.md accuracy audit. All documentation reflects observed behavior.

## Remediation rule

When the final audit (Task 14) finds a gap, preserve the existing task ledger, append a uniquely numbered pending task (Task 15, 16, …), add it to Task 14's dependencies, return Task 14 to `pending`, and continue. Reaching an iteration, runtime, or session ceiling leaves the cycle `active` or `blocked` with a recovery handoff — never success.