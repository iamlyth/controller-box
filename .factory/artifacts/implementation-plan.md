---
spec_path: docs/SPEC.md
spec_commit: 3a10f6b7d04a615b2b9d06eef6c91e431fa9c079
spec_blob: 58f5d3cb72bc6b3e5f573fa09a63c11a653ed577
base_commit: 682ba4451abbb867e6a5e453fcbbe9b943e94ce6
status: active
---

# Implementation Plan

## Goal and non-goals

Close every gap identified by Campaign Round 2 audit and independent code
analysis so the committed specification reaches full v1 conformance. The
codebase is mature — 86 CTest targets, 297+ production tests, all source
modules fully implemented with zero TODOs or stubs. Remaining gaps are
targeted fixes to DBus type fidelity, signal security, installed-test
production-path coverage, interaction-inventory accuracy, host-mode visual
rendering, and documentation accuracy.

**Non-goals:** new product features, spec changes, architecture redesign,
Git history retrieval, or work outside the declared environment
(`.factory/environment.toml`: one SSH runner with `remote-project-gate` and
`systemd-user` capabilities only).

## Architecture and constraints

One binary (`controller-box`), two modes: overlay service (systemd user,
always resident) and manager (on-demand). All InputPlumber interaction via
system DBus (sd-bus). SDL2 widget toolkit built from scratch. nanosvg for
icon rasterization. Atomic YAML config persistence (mkstemp+fsync+rename).
Multi-layered controller identification (BT MAC → USB serial → USB port path
→ connection order). Environment lacks GPU, physical-controller,
kernel-uinput, and installed-package runner capabilities; requirements
dependent on those are classified `partial` with documented limitations.

## Specification conformance matrix

| ID | Spec § | Classification | Evidence | Task |
|----|--------|---------------|----------|------|
| ARCH-01 | §2.1 | verified | GUI calls only DBus API; no direct input routing in `src/` | — |
| ARCH-02 | §2.2 | verified | `ip_connection.c` sd-bus connect; `dbus_client.c` native reads/writes | — |
| ARCH-03 | §2.3 | verified | `main.c:49-68` `run_manager`; `overlay_service.c:490` `run_overlay_service` | — |
| ARCH-04 | §2.4 | verified | `ip_connection.c:220` NameOwnerChanged → re-enumerate; degraded mode in both binaries; `test_overlay_service.c` recovery tests | — |
| ARCH-05 | §2.5 | verified | `trigger.c` SetInterceptActivation+PASS; `ip_intercept_poll.c` 50ms poll; `test_trigger.c` 22 tests | — |
| SYS-01 | §3 | verified | CMakeLists.txt deps: SDL2, SDL2_ttf, SDL2_image, libsystemd, yaml, nanosvg vendored | — |
| SYS-02 | §3 | partial | Only x86_64 tested; no aarch64 runner declared in environment | Task 8 |
| SYS-03 | §3 | partial | X11 tested via Xvfb; Wayland/Gamescope not explicitly tested | Task 8 |
| OV-01 | §4.1 | verified | `grid_render.c` rows=controllers, cols=slots, Unassigned col 0; `test_grid_render.c` 31 tests | — |
| OV-02 | §4.2 | verified | `trigger.c:39` parse "Select+A"; settings configurable; `test_trigger.c` | — |
| OV-03 | §4.3 | verified | `player_mode.c` independent row editing; `test_player_mode.c` 23 tests | — |
| OV-04 | §4.4 | verified | `host_mode.c` logic complete (enter/exit/freeze); `grid_render.c` consumes `cbx_host_mode_row_state()` via `cbx_grid_render_ctx.hm`; HOST rows render green cell + indicator bar, SELECTED rows render blue cell + accent border, FROZEN rows render dimmed (no highlight, disabled indicators, secondary text); `test_overlay_visual.c` `test_host_mode_row_states` verifies green/blue/dimmed pixel assertions; golden baseline `overlay_host_mode.png` updated | — |
| OV-05 | §4.5 | verified | `conflict.c` red highlight + auto-resolve to lowest free slot; `test_conflict.c` 33 tests including visual red pixel assertion | — |
| OV-06 | §4.6 | verified | `profile_cycle.c:130` profile_follows; profile stored per-row, navigation only modifies cur_col | — |
| OV-07 | §4.7 | verified | `dynamic_columns.c` rebuild on target count change; `test_dynamic_columns.c` 24 tests | — |
| OV-08 | §4.8 | verified | `grid_render.c` shows model name + slot position; no nickname prompts in `src/` | — |
| OV-09 | §4.9 | partial | Pre-built surface verified (`surface_build.c` render-to-texture); latency not measurable on minimum hardware (Pi 4 absent) | Task 8 |
| OV-10 | §4.10 | verified | Player Mode, conflict, unassigned, icons, text, Host Mode visual states verified in `test_overlay_visual.c` 8 tests + `test_golden.c` 11 baselines; Host Mode row states (HOST/SELECTED/FROZEN) rendered distinctly with green/blue/dimmed visuals | — |
| MGR-01 | §5.1 | verified | `manager.c` tab bar + controller + pointer dispatch; `test_manager_native.c` SDL virtual gamepad + mouse | — |
| MGR-02 | §5.2 | verified | `controllers_tab.c` add/remove/type-change with DBus verification; `test_controllers_tab.c` + `test_manager_native.c` | — |
| MGR-03 | §5.3 | verified | `profiles_tab.c` browse/create(3 sources)/edit/delete; empty-profile sequential entry; unsaved-changes prompt; `test_profiles_tab.c` | — |
| MGR-04 | §5.4 | verified | `profile_editor_list.c` + `profile_editor_seq.c`; NES minimum in `profile_validate.c`; diagram sync; `test_editor_list_mode.c` + `test_editor_seq_mode.c` | — |
| MGR-05 | §5.5 | verified | `settings_tab.c` 9 settings + save; `test_settings_tab.c` | — |
| MGR-06 | §5.6 | verified | `test_manager_visual.c` 13 tests: all tabs, editor modes, degraded, focus/press indication | — |
| MGR-07 | §5.7 | partial | 50/59 inventory entries verified via production dispatch using SDL virtual gamepads (SDL_JoystickAttachVirtual) and native DBus; M32/M34 dispatch paths corrected as supplemental direct callback; M32 DBus InputEvent signal path tested via native server EmitInputEvent in test_manager_native_prof; M38 controller-path (Start discard) tested in native; M35/M36 included in na_ids test guard. SPEC §5.7 requires controller acceptance with a physical or kernel-backed synthetic gamepad; SDL virtual joystick is SDL-userspace, not kernel-backed. test_kernel_controller.c created (Task 7) to test uinput-backed evdev gamepad, but skips (exit 77) because no kernel-uinput runner capability is declared in environment.toml. Classification: partial pending a runner with kernel-uinput capability. | Task 7 |
| ID-01 | §6.2 | verified | `identity.c` 4-layer extraction; `test_identity.c` all layers + edge cases | — |
| ID-02 | §6.3 | verified | `identity_downgrade.c` downgrade detection + ORDER fallback; `test_identity_downgrade.c` | — |
| CFG-01 | §7.1 | verified | `config_profile.c` writes InputPlumber device_profile_v1 YAML; no duplicate format | — |
| CFG-02 | §7.2 | verified | `config_paths.c` XDG + fallback paths; `test_config_paths.c` | — |
| CFG-03 | §7.3 | verified | `config_settings.c` all fields + atomic save; `test_settings.c` | — |
| CFG-04 | §7.4 | verified | `config_assignments.c` assignments + gamepad_order + atomic save; `test_assignments.c` | — |
| CFG-05 | §7.5 | verified | `config_profile_meta.c` sidecar with O_NOFOLLOW + realpath containment; `test_profile_list.c` | — |
| CFG-06 | §7.6 | verified | `config_profile.c` InputPlumber-compatible emit; round-trip tested in `test_profile_yaml.c` | — |
| ICO-01 | §8.1 | verified | `icon_lookup.c` maps DeviceType → icon; `test_icon_lookup.c` | — |
| ICO-02 | §8.2 | verified | 35 SVG files in `data/icons/svg/` including arcade-stick, hitbox, steam-deck, generic-gamepad | — |
| ICO-03 | §8.3 | verified | `icon_cache.c` nanosvg rasterize → SDL_Texture, cached at startup; `test_icon_cache.c` | — |
| ICO-04 | §8.4 | verified | `icon_map.c` YAML parser; unknown type → generic-gamepad + raw label; `test_icon_map.c` | — |
| ICO-05 | §8.5 | verified | `icon_lookup.c` profile override (absolute path PNG or built-in name); path traversal protection | — |
| PKG-01 | §9.1 | partial | Flatpak manifest exists, marked experimental (publication=false); clean build not verified in CI; not published | Task 8 |
| PKG-02 | §9.2 | verified | CMake install rules; `test_packaging.sh`; `verify-project.sh` runs packaging test | — |
| PKG-03 | §9.3 | verified | CMakeLists.txt installs binary, service, desktop, icons, YAML, profiles | — |
| PKG-04 | §9.4 | verified | `ip_connection.c` runtime bus-name check; `service_install.c` no Requires=inputplumber.service; `test_service_install.c` 29 tests | — |
| DB-01 | §10.1 | verified | GET reads u/b/as/s natively; SET writes u/as/b natively (boolean fix in `dbus_client.c:911-924`); InterfacesAdded/Removed callbacks verify sender against InputPlumber's tracked unique bus name (`dbus_client.c:sd_sender_ok`, `sd_interfaces_added_callback`, `sd_interfaces_removed_callback`); spoofed signals silently dropped | — |
| DB-02 | §10.2 | verified | All Manager/Composite/Target/Source DBus wrappers implemented and tested | — |
| DB-03 | §10.3 | verified | All 5 gaps have workarounds implemented (poll, assignments persist, temp YAML, filesystem read, no source add/remove) | — |
| PERF-01 | §11.1 | partial | Deterministic framebuffer, region assertions, golden images, failure artifacts, software backend smoke all verified; installed functional test exercises overlay lifecycle through production poll path (init, InterceptMode PASS→ALL activation, framebuffer readback, B-close, assignment save); installed binary test verifies compositor-visible overlay activation via screenshot; test_kernel_controller.c (Task 7) creates a uinput-backed evdev gamepad and exercises the installed Manager binary through real kernel gamepad events → SDL joystick → production event loop, but skips (exit 77) because no kernel-uinput runner capability is declared in environment.toml. Installed smoke uses xdotool keyboard/mouse (supplemental, not controller acceptance per §5.7). Classification: partial pending a runner with kernel-uinput capability. | Task 7 |
| PERF-02 | §11 | partial | Pre-built surface + poll architecture verified; `test_overlay_latency.c` exists; latency not tested on minimum hardware | Task 8 |
| PERF-03 | §11.2 | partial | Definition of done is the final audit task; depends on all other tasks achieving verified status | Task 8 |

## Interaction acceptance inventory

The inventory (`tests/interaction_inventory.c`) contains 59 entries across
three categories:

**Manager controls (M01–M38, 38 entries):** Tab bar (M01–M03), Controllers
tab (M04–M09), Profiles tab (M10–M20), Settings tab (M21–M27), Profile
editor (M28–M38). Each entry records controller path (focus chain navigation
+ A activation), pointer path (rendered-bounds click), expected semantic
outcome, and production dispatch path.

**Overlay actions (O01–O13, 13 entries):** Activation (O01), movement
(O02–O05), host mode (O06–O09), close (O10), multi-controller (O11),
deferred host profile cycle (O12), conflict resolution (O13). Controller
path via SDL keydown or DBus InputEvent signal through `cbx_overlay_service_step`.

**Disabled/degraded scenarios (D01–D08, 8 entries):** InputPlumber
unavailable, no selection, validation error, cancel, DBus failure,
filesystem failure, empty profile validation.

**Current status:** 50 verified, 8 NOT_APPLICABLE (M13, M14, M32, M34, M35,
M36, M37, M38 — controller-only name input and capture/sequential/discard
actions), 1 DEFERRED (O12 — host-mode profile cycling per §13). Both
controller and pointer paths are exercised through production SDL event
dispatch in `test_manager_native.c`, `test_manager_native_prof.c`,
`test_manager_interaction_ctrl.c`, `test_manager_interaction_prof.c`, and
`test_overlay_native.c`. Native DBus tests use SDL virtual gamepads
(`SDL_JoystickAttachVirtual`); mock DBus tests use keyboard-dispatched
events as supplemental accessibility evidence. Task 4 addresses remaining
gaps in M32/M34 dispatch path accuracy, M38 native coverage, and M35/M36
test guard completeness.

## Task 1: Fix boolean DBus property SET native type fidelity
- Status: complete
- Dependencies: none
- Scope: `src/dbus/dbus_client.c` (`sd_set_property`), `tests/native_ip_server.c` (add boolean SET handler), `tests/test_dbus_signatures.c` (add round-trip test)
- Acceptance criteria: `sd_set_property` writes `v<b>` variant for boolean properties (`ManageAllDevices`, `Enabled`) instead of falling through to string `v<s>`. Native test server accepts boolean SET and returns success. Test verifies boolean property SET round-trip through native-signature DBus server. Existing `test_dbus_signatures.c` signature mapping tests still pass.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'test_dbus_signatures|test_native_dbus' --output-on-failure"`
- Documentation impact: none

## Task 2: Add sender verification for DBus InterfacesAdded/Removed signals
- Status: complete
- Dependencies: none
- Acceptance criteria: Both callbacks verify signal sender matches InputPlumber's tracked unique bus name (from `ip_connection_get_unique_name`) before processing, consistent with `ip_properties.c:sender_ok()`. Spoofed signals from a non-matching sender are silently dropped. Existing hotplug tests still pass. New test injects a signal with wrong sender and asserts it is ignored.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'test_hotplug' --output-on-failure"`
- Documentation impact: none

## Task 3: Implement real overlay lifecycle in installed tests and compositor-visible overlay activation
- Status: complete
- Dependencies: none
- Scope: `tests/test_installed_functional.c` (Phases 7–10), `tests/test_installed_binary.sh` (add overlay activation phase), `tests/test_installed_smoke.sh` (add overlay activation if feasible)
- Acceptance criteria: `test_installed_functional.c` Phases 7–10 call `cbx_overlay_service_init` and `cbx_overlay_service_step` through the production poll path — not just DBus property set/get. InterceptMode PASS→ALL transition triggers overlay activation via poll detection. Framebuffer is read back and verified non-blank (grid, player position, controller text, icons). Comments accurately describe what is tested. `test_installed_binary.sh` adds an overlay activation phase: launch overlay service, trigger InterceptMode PASS→ALL via DBus, verify compositor-visible framebuffer output (non-blank screenshot), close overlay (B or InterceptMode→PASS), verify clean close. Test comments and conformance matrix accurately reflect production-path coverage.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'test_installed_functional|test_installed_binary|test_installed_smoke' --output-on-failure"` and `nix-shell --run './scripts/verify-project.sh'`
- Evidence: `test_installed_functional.c` Phases 8–12 allocate a production `cbx_overlay_service_ctx`, initialize all components (renderer, DBus connection, device enumeration, grid, surface, lifecycle with fade=0, player/host mode, input events, InterceptMode polls, triggers), activate the overlay via poll detection (InterceptMode PASS→ALL → `ip_intercept_poll_tick` → `on_activating` → `cbx_overlay_lifecycle_activate`), read back the framebuffer via `fb_read_pixels` and verify non-blank regions (grid area, header, row labels, cells) via `fb_region_has_content`, close via B keydown → `cbx_overlay_lifecycle_close` → `cbx_overlay_on_save` (assignment persistence + InterceptMode→PASS), and verify assignment count, InterceptMode on wire. `test_installed_binary.sh` Phase 6 launches `--overlay-service`, sets InterceptMode to ALL via `busctl set-property`, captures screenshots via `import -window root`, verifies non-blank framebuffer (mean > 5.0) and frame difference, then closes via InterceptMode→PASS. Full CTest suite: 96/96 pass (1 pre-existing skip).
- Documentation impact: Update README §11.1 table to include `test_installed_functional` and `test_installed_binary`

## Task 4: Fix interaction inventory accuracy and missing coverage
- Status: complete
- Dependencies: none
- Scope: `tests/interaction_inventory.c` (correct M32/M34 dispatch path descriptions), `tests/test_interaction_inventory.c` (add M35, M36 to `na_ids` array), `tests/test_manager_native_prof.c` (add DBus InputEvent signal path test for capture mode, add M38 controller-path test)
- Acceptance criteria: M32 and M34 inventory entries describe their dispatch path as direct callback invocation (supplemental), not `ip_input_events → cbx_profile_editor_on_input_event`. A new test in `test_manager_native_prof.c` exercises the DBus InputEvent signal path for capture mode via `emit_input_event` (native server) through production signal dispatch to `cbx_profile_editor_on_input_event`. M38 has a controller-path test in native tests pressing Start from editor LIST mode to discard changes. `test_interaction_inventory.c` `na_ids` array includes M35 and M36 alongside M13, M14, M32, M34, M37, M38. All existing interaction tests pass.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'test_interaction_inventory|test_manager_native_prof|test_manager_interaction_prof' --output-on-failure"` → all 3 tests pass. Full suite: 96/96 pass (1 pre-existing skip).
- Evidence: `interaction_inventory.c` M32 dispatch_path = "direct callback: cbx_profile_editor_on_input_event (supplemental; DBus signal path tested in test_manager_native_prof)"; M34 = "direct callback: cbx_profile_editor_on_input_event → cbx_profile_editor_seq_on_input (supplemental; DBus signal path tested in test_manager_native_prof)". `test_interaction_inventory.c` na_ids = {M13, M14, M32, M34, M35, M36, M37, M38}. `test_manager_native_prof.c` adds `test_m32_capture_dbus_signal` (emits InputEvent via native server EmitInputEvent method at CompositeDevice0 path, drains manager bus via sd_bus_process, verifies capture ends + binding count) and `test_m38_discard_ctrl` (Start button via virtual gamepad button 6, verifies editor closes to LIST mode, file mtime unchanged). Both registered in test runner array. File header updated to M30–M38 range.
- Documentation impact: Update OPERATIONS.md inventory description to reflect 50 verified, 8 NOT_APPLICABLE, 1 DEFERRED (not "all verified") — deferred to Task 6

## Task 5: Fix host mode visual rendering in overlay grid
- Status: complete
- Dependencies: none
- Scope: `src/overlay/grid_render.c` (consume `cbx_host_mode_row_state()`), `tests/test_overlay_visual.c` (verify host mode visual differences), `tests/test_golden.c` (update host mode golden baseline if needed)
- Acceptance criteria: `cbx_select_grid_render` calls `cbx_host_mode_row_state()` when host mode is active and renders SELECTED rows with a distinct highlight (e.g., accent color border), HOST rows with a host indicator, and FROZEN rows with dimmed appearance — visually differentiating them from normal Player Mode rows. `test_overlay_visual.c` verifies that Host Mode rows have visually distinct coloring compared to Player Mode rows (not just frame differ). Golden baseline for host mode is updated if rendering changes. Existing overlay visual and golden tests pass. `CBX_GENERATE_GOLDEN=1` may be used to regenerate the baseline with explicit review.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'test_overlay_visual|test_golden|test_grid_render' --output-on-failure"`
- Documentation impact: none

## Task 6: Fix documentation inaccuracies
- Status: complete
- Dependencies: none
- Scope: `docs/OPERATIONS.md` (line ~1032 inventory description), `README.md` (§11.1 verification table)
- Acceptance criteria: OPERATIONS.md inventory description accurately states "50 verified, 8 NOT_APPLICABLE, 1 DEFERRED" instead of "59 entries, all verified". README §11.1 verification table includes `test_installed_functional` and `test_installed_binary` alongside `test_installed_smoke`. `scripts/check-docs-sync.sh` passes. No other documentation claims are inaccurate relative to observed test behavior.
- Verification: `./scripts/check-docs-sync.sh` and `nix-shell --run './scripts/verify-boilerplate.sh'`
- Documentation impact: OPERATIONS.md and README.md corrected
- Evidence: OPERATIONS.md line 1032 inventory description updated from "59 entries, all verified" to "59 entries: 50 verified, 8 NOT_APPLICABLE, 1 DEFERRED". README §11.1 verification table already included `test_installed_functional` (5a) and `test_installed_binary` (5b) from Task 3; added individual run command for functional+binary tests. Fixed two SC2181 shellcheck warnings in `tests/test_installed_binary.sh` (if [ $? -eq 0 ] → if cmd; then) that caused `verify-boilerplate.sh` to fail. `./scripts/check-docs-sync.sh` → pass. `nix-shell --run './scripts/verify-boilerplate.sh'` → pass ("verify: boilerplate checks passed").

## Task 7: Add kernel-backed controller test or document environment limitation
- Status: complete
- Dependencies: none
- Scope: `tests/test_kernel_controller.c` (new test), `tests/CMakeLists.txt` (register test), `tests/test_installed_binary.sh` or `tests/test_installed_smoke.sh` (integrate if feasible)
- Acceptance criteria: A new test creates a synthetic controller via `uinput` (kernel-backed evdev device), navigates the installed Manager binary with real gamepad events through the production event loop, and verifies semantic outcomes (tab navigation, settings change, profile creation). If `/dev/uinput` is unavailable in the test environment, the test skips with exit code 77 and a diagnostic message explaining the missing capability. The test is registered in CMakeLists.txt with `SKIP_RETURN_CODE 77`. If the test cannot run in the declared environment, the conformance matrix row PERF-01 and MGR-07 document the limitation and the task classifies the requirement as `partial` pending a runner with `kernel-uinput` capability.
- Verification: `nix-shell --run "ctest --test-dir build-check -R 'test_kernel_controller' --output-on-failure"` (skip acceptable with diagnostic); `nix-shell --run './scripts/verify-project.sh'` must still pass
- Documentation impact: Document kernel-backed controller test status in README
- Evidence: `tests/test_kernel_controller.c` created — a standalone C binary (no cmocka/libcontrollerbox dependency) that opens /dev/uinput, creates a virtual gamepad (BTN_SOUTH/EAST/NORTH/WEST/SELECT/START/MODE/DPAD/TL/TR/THUMBL/THUMBR + ABS_X/Y/RX/RY/Z/RZ/HAT0X/HAT0Y), and if a build-dir argument is provided, forks test_ip_server (private DBus), sets up temp HOME with fonts/config, launches installed controller-box --manager with SDL_VIDEODRIVER=dummy, sends gamepad events (D-pad navigation, A/B/Start button presses) through uinput → kernel evdev → SDL joystick → production event loop, and verifies manager survival + settings.yaml existence. Exits 77 with diagnostic when /dev/uinput is unavailable. Registered in CMakeLists.txt with `SKIP_RETURN_CODE 77`. ctest result: `Skipped` (exit 77) — /dev/uinput not available in sandbox (no kernel-uinput runner capability declared). Conformance matrix: PERF-01 and MGR-07 updated to `partial` with explicit rationale documenting the kernel-uinput limitation. README updated with kernel-backed controller test documentation.

## Task 8: Final documentation and specification audit
- Status: pending
- Dependencies: Tasks 1, 2, 3, 4, 5, 6, 7
- Scope: `.factory/artifacts/implementation-plan.md` (conformance matrix update), `docs/OPERATIONS.md`, `README.md`, `docs/SPEC.md` §11.2 definition of done
- Acceptance criteria: All conformance matrix rows are `verified` or have documented environment limitations acceptable to the definition of done. Every enabled control in the §5.7 interaction acceptance inventory has passing controller and pointer activation evidence through production dispatch. Every overlay action has passing controller-event evidence. No contradictory open v1 bugs in `.factory/bugs/open.md`. Independent adversarial reviews (correctness, test-quality, security, documentation) find no unresolved blocking issue. Full clean verification passes: `nix-shell --run './scripts/verify-project.sh'` with zero unexplained skips, no weakened assertions, no compiler warnings, no sanitizer defects. README and OPERATIONS.md match observed behavior. Git tree is clean on `develop`. Environment limitations (aarch64, Wayland/Gamescope, GPU backend, Pi 4 latency, kernel-backed controller) are documented as `partial` with explicit rationale where no runner capability is declared.
- Verification: `nix-shell --run './scripts/verify-project.sh'` and `./scripts/final-gate.sh --planning` (for plan completion) or `./scripts/final-gate.sh --implementation` (for implementation completion)
- Documentation impact: Final README, OPERATIONS.md, and conformance matrix accuracy

## Remediation rule

When the final audit (Task 8) finds a gap, preserve the task ledger, append a
uniquely numbered pending task (Task 9, 10, …), add it to Task 8's
dependencies, return Task 8 to pending, and continue. Reaching an
iteration/runtime/session ceiling leaves the cycle incomplete; it never
satisfies the plan.