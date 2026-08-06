---
spec_path: docs/SPEC.md
spec_commit: 12f82db38f999986de4216dfc50a6e13452db4c9
spec_blob: 0522f7f1aa79d70b343ed6022956683a7c11695f
base_commit: 12f82db38f999986de4216dfc50a6e13452db4c9
status: active
---

# Controller-Box Implementation Plan

## Goal

Implement Controller-Box: a single-binary, two-mode SDL2 GUI for Linux that wraps
InputPlumber via system DBus to provide a controller-only, console-like experience.

- **Overlay service** (`controller-box --overlay-service`): an always-resident systemd
  user service that shows a fighting-game-style character-select screen when a player
  presses the trigger combo (default Select+A), allowing per-controller slot assignment
  and profile cycling mid-game.
- **Manager** (`controller-box --manager`): a tab-based configuration app for creating
  virtual controllers, building/editing profiles with a visual controller diagram, and
  adjusting settings.

Both modes share one codebase, one config directory, and one DBus connection pattern.

## Non-Goals (v1)

- Bare DRM/framebuffer support (no compositor)
- Pi Zero / Pi 3 class hardware
- Touch/mouse-only navigation
- Replacing InputPlumber's input routing engine
- Steam integration
- Quick-action hotkey combos (only the overlay trigger exists)
- Bundling InputPlumber
- Network input routing
- Per-game profile auto-switching
- Adding/removing source devices on running composites
- User-assigned controller names/colors
- Advanced profile mappings (chord, delayed_chord, gamepad→mouse)
- Distro packages (.deb, .rpm, Nix, AUR, Batocera)

## Architecture and Constraints

**Language:** C11. All dependencies (SDL2, SDL2_ttf, SDL2_image, sd-bus, nanosvg) are C
libraries. The spec's emphasis on minimal Pi 4 footprint favors C over C++ (no C++
runtime overhead). Function-pointer vtables provide widget polymorphism; libyaml handles
YAML parsing.

**Engine vs. control surface:** InputPlumber is the engine (evdev grab, virtual devices,
event translation, profiles, intercept mode, player ordering). Controller-Box is the
control surface — every state change goes through InputPlumber's DBus API. No direct
input routing.

**DBus communication:** System bus via sd-bus (part of systemd, zero additional
dependencies). Bus name `org.shadowblip.InputPlumber`, root path
`/org/shadowblip/InputPlumber`. ObjectManager enumeration, PropertiesChanged signals for
most properties (except InterceptMode — gap #1, polled at 50ms).

**InterceptMode poll interval note:** The spec (§2.5, §10.3) says ~500ms, but §11
requires <10ms from button press to visible. These are contradictory: a 500ms poll means
worst-case detection is 500ms. This plan uses 50ms (DEC-002), yielding ~51ms worst case.
The <10ms target is achievable only for the render path (pre-built surface); detection
latency is bounded by the poll interval.

**Binary architecture:** Single executable, two modes dispatched by CLI flag. Shared
static library links both modes. Shared DBus client code, config parsing, widget toolkit.

**Service model:** `inputplumber.service` (system, prerequisite) → `controller-box.service`
(user, `After=`/`Requires=` inputplumber, `Restart=always`).

**Pre-built overlay:** Surface rendered to an SDL2 target texture at daemon startup.
Icons pre-rasterized via nanosvg. Appearance is a single `SDL_RenderCopy` (<1ms render,
~50ms worst-case detection at 50ms poll interval). Nothing constructed on demand.

**Security constraints (from security review):**
- All DBus signal handlers must verify sender unique bus name matches InputPlumber's
  known unique name (tracked via `NameOwnerChanged`).
- Temp files for `CreateCompositeDevice` must use `mkstemp()` or `O_TMPFILE` with mode
  0600, preferably in `XDG_RUNTIME_DIR`. Unlink after the DBus call returns.
- Profile/config filenames must match `^[a-zA-Z0-9_-]+$`. Paths canonicalized with
  `realpath()`. Files opened with `O_NOFOLLOW` where appropriate.
- All YAML parsing (settings, assignments, profiles, sidecars) must use libyaml with
  max depth 50, max document size 1MB, no custom tags. YAML written via libyaml emitter,
  not string concatenation.
- systemd unit files written atomically (mkstemp + rename), static template only.
- User must be in the `inputplumber` group for polkit authorization. First-run setup
  must check and guide. Overlay service must handle `AccessDenied` with a clear log
  message guiding the user.
- Config files (`settings.yaml`, `assignments.yaml`, sidecar metadata) at permissions
  0600.
- Deferred from v1 (noted, not implemented): `ForceFeedback` interface,
  `ManageAllDevices` property, `SendEvent`/`SendButtonChord` (spec marks as
  optional).

**Testing strategy:** cmocka for unit tests, SDL2 dummy video driver for display-free
rendering tests, CTest integration. DBus client tested via interface abstraction with
mock backends. Integration tests use python-dbusmock on a test bus.

**Build system:** CMake. `find_package`/`pkg_check_modules` for SDL2, SDL2_ttf,
SDL2_image, libsystemd. nanosvg vendored as a static library in `third_party/`.
Install rules for tarball. Flatpak manifest for primary distribution.

## Task List

### Phase 1: Foundation

## Task 1: CMake skeleton, dependency detection, and nanosvg vendoring
- Status: complete
- Evidence: `cmake -B build && cmake --build build` clean; `./build/smoke_test_sdl2` OK (dummy-driver fallback for headless); `./build/smoke_test_nanosvg` OK (parses+rasterizes a 64x64 SVG, finds red pixel); `ctest` 2/2 pass; `build/config.h` has `DATA_DIR=/usr/share/controller-box`, `ICON_DIR=/usr/share/controller-box/icons`, `CONFIG_DIR=.config/controller-box`. Vendored nanosvg (zlib license) in `third_party/nanosvg/` with `nanosvg_impl.c`. `shell.nix` provides native deps.
- Dependencies: none
- Scope: `CMakeLists.txt` (root), `config.h.in`, `third_party/nanosvg/` (nanosvg.h,
  nanosvgrast.h, nanosvg_impl.c), `src/` directory structure with empty `.c` stubs
- Acceptance criteria:
  - `cmake -B build && cmake --build build` succeeds on x86_64 with SDL2, SDL2_ttf,
    SDL2_image, and libsystemd dev packages installed
  - A trivial test program opens an SDL2 window and renders one frame
  - nanosvg parses a small SVG and rasterizes to RGBA pixels (smoke test)
  - `config.h` generated with install paths (`DATA_DIR`, `ICON_DIR`, `CONFIG_DIR`)
- Verification: `cmake -B build && cmake --build build && ./build/smoke_test_sdl2 &&
  ./build/smoke_test_nanosvg`
- Documentation impact: README build-from-source section (placeholder)

## Task 2: Dual-mode entry point and shared library structure
- Status: complete
- Dependencies: Task 1
- Scope: `src/app/main.c` with `--overlay-service` / `--manager` arg parsing, shared
  static library target `libcontrollerbox.a` aggregating all source
- Acceptance criteria:
  - `controller-box` with no args runs overlay service mode (prints mode, exits stub)
  - `controller-box --manager` runs manager mode (prints mode, exits stub)
  - `controller-box --version` prints version from CMake
  - Both modes link against the shared static library
- Verification: `cmake --build build && ./build/controller-box --version &&
  ./build/controller-box --overlay-service --dry-run && ./build/controller-box --manager --dry-run`
- Documentation impact: README usage section (binary invocation)

## Task 3: Test harness setup
- Status: complete
- Dependencies: Task 1
- Scope: `tests/CMakeLists.txt`, cmocka integration, SDL2 dummy driver test utility,
  CTest `enable_testing()`, one passing sample test
- Acceptance criteria:
  - `cmake --build build && ctest --test-dir build --output-on-failure` passes
  - Sample test uses `SDL_VIDEODRIVER=dummy` to create a renderer without a display
  - cmocka linked and a trivial assertion test passes
  - Test fixture helper for DBus mock interface abstraction created
- Verification: `cmake --build build && ctest --test-dir build --output-on-failure`
- Documentation impact: none
- Evidence: `cmake --build build` clean (no warnings under -Wall -Wextra -Wpedantic, Debug -Werror); ctest 4/4 pass (smoke_test_sdl2, smoke_test_nanosvg, test_sample [6 cmocka assertions incl. DBus mock vtable], test_sdl_dummy [SDL2 dummy driver via test_harness]); `tests/CMakeLists.txt` modularises test targets; `tests/test_harness.{h,c}` provide headless SDL2 init (auto-falls-back to dummy driver); `tests/dbus_mock.{h,c}` provide `ip_dbus_backend` function-pointer vtable + canned-response mock backend for DBus client testing (Tasks 9-15); verify-boilerplate, check-plan-freshness, branch-guard exit 0.

## Task 4: Config directory resolution and YAML library integration
- Status: complete
- Dependencies: Task 1
- Scope: `src/config/config_paths.c`, `src/config/config_paths.h`, CMake addition of
  libyaml (`pkg_check_modules(YAML REQUIRED yaml)`), XDG base directory resolution
- Acceptance criteria:
  - Resolves and creates `~/.config/controller-box/` (mode 0700)
  - Locates `~/.local/share/inputplumber/profiles/` (create if missing, mode 0700)
  - Locates `/usr/share/inputplumber/` (read-only system dir)
  - Locates `/usr/share/controller-box/` (icons, icon mapping)
  - Returns correct paths on systems with and without `XDG_*` env vars set
  - `tests/test_config_paths.c` passes with cmocka
- Verification: `cmake --build build && ./build/test_config_paths`
- Documentation impact: OPERATIONS config file locations section
- Evidence: `cmake --build build` clean (Debug -Werror); ctest 5/5 pass; `test_config_paths` 16/16 cmocka tests pass (XDG absolute/fallback/relative/empty/buf-too-small for both config+profiles dirs, recursive dir creation with 0700, idempotent create, system path accessors); `src/config/config_paths.{h,c}` implement XDG resolution + `cbx_ensure_dir()` recursive mkdir + system path getters; libyaml already linked via `PkgConfig::YAML` from Task 1; verify-boilerplate, check-plan-freshness, branch-guard exit 0.

### Phase 2: Config Layer

## Task 5: settings.yaml read/write
- Status: complete
- Dependencies: Task 4
- Scope: `src/config/config_settings.c`, `src/config/config_settings.h`
- Acceptance criteria:
  - Loads all fields from §7.3: `overlay_trigger`, `launch_at_boot`, `theme`,
    `overlay_opacity`, `virtual_controllers.count`, `virtual_controllers.types`
  - Generates default settings if file does not exist
  - Atomic write (temp file + rename, mode 0600)
  - Validates `overlay_opacity` is 0.0–1.0, `count` is 1–16
  - Validates `types` entries against a known-good list
  - YAML parser configured with: max depth 50, max document size 1MB, no custom tags
  - `tests/test_settings.c` passes (round-trip read/write, defaults, validation)
- Verification: `cmake --build build && ./build/test_settings`
- Documentation impact: OPERATIONS settings.yaml format section
- Evidence: `cmake --build build` clean (Debug -Werror, no warnings); ctest 6/6 pass; `test_settings` 18/18 cmocka tests pass (defaults no-file, defaults function, round-trip save/load, round-trip defaults, opacity validation, count validation, unknown type validation, save rejects invalid, missing fields → defaults, out-of-range clamp on load, file mode 0600, flow-style types array, known-good type list, max doc size 1MB rejection, custom tags rejection, tag directives rejection, empty file → defaults, YAML 1.1 bool variants); `src/config/config_settings.{h,c}` implement libyaml event-based parser (max depth 50, max doc size 1MB, no custom tags/tag directives) + document-based emitter + atomic write (mkstemp + fchmod 0600 + fsync + rename); known-good types: xb360/ds5/deck/gamepad/mouse/keyboard/touchscreen; verify-boilerplate, check-plan-freshness, branch-guard exit 0.

## Task 6: assignments.yaml read/write and gamepad order persistence
- Status: complete
- Dependencies: Task 4
- Scope: `src/config/config_assignments.c`, `src/config/config_assignments.h`
- Acceptance criteria:
  - Loads/saves assignments list: `id` (string with type prefix), `slot` (int),
    `profile` (string) per §7.4
  - Persists `gamepad_order` array (gap #2 workaround)
  - Validates `id` format: `BT:xx:xx:xx:xx:xx:xx`, `USB:xxxxx`, `USB:phys:xxxxx`,
    `ORDER:n`
  - Validates `slot` is non-negative integer, `profile` matches `^[a-zA-Z0-9_-]+$`
  - Atomic write (temp file + rename, mode 0600)
  - YAML parser configured with: max depth 50, max document size 1MB, no custom tags
  - `tests/test_assignments.c` passes (round-trip, validation, defaults)
- Verification: `cmake --build build && ./build/test_assignments`
- Documentation impact: OPERATIONS assignments.yaml format section
- Evidence: `cmake --build build` clean (Debug -Werror, no warnings); ctest 7/7 pass; `test_assignments` 31/31 cmocka tests pass (BT:MAC 6-octet valid/invalid, USB:serial valid/invalid, USB:phys valid/invalid, ORDER valid/invalid, other rejects, profile valid/invalid, no-file→empty, empty-file→empty, round-trip with BT/USB, round-trip empty, round-trip USB:phys, round-trip empty profile, round-trip ORDER, save rejects invalid id/negative slot/invalid profile/invalid gamepad_order, file mode 0600, max doc 1MB, custom tags rejected, tag directives rejected, parse from YAML, gamepad_order-only, no-gamepad_order, validate empty/NULL); `src/config/config_assignments.{h,c}` implement libyaml event-based parser (max depth 50, max doc 1MB, no custom tags/tag directives) + document-based emitter + atomic write (mkstemp + fchmod 0600 + fsync + rename); ID validation: BT:6-octet MAC (hex pairs), USB:serial (alnum/dash/underscore), USB:phys:port-path, ORDER:n (non-negative int); profile validation ^[a-zA-Z0-9_-]+$ or empty/NULL; verify-boilerplate, check-plan-freshness, branch-guard exit 0.

## Task 7: Profile YAML parse and generate (InputPlumber device_profile_v1)
- Status: pending
- Dependencies: Task 4
- Scope: `src/config/config_profile.c`, `src/config/config_profile.h`
- Acceptance criteria:
  - Parses InputPlumber `device_profile_v1` YAML: `version`, `kind`, `name`,
    `description`, `mapping` (array of `{name, source_event, target_events}`)
  - Serializes back to valid `device_profile_v1` YAML via libyaml emitter (not string
    concatenation)
  - Round-trip test: parse → serialize → parse yields identical structure
  - YAML parser configured with: max depth 50, max document size 1MB, no custom tags
  - `tests/test_profile_yaml.c` passes with a sample InputPlumber profile fixture
- Verification: `cmake --build build && ./build/test_profile_yaml`
- Documentation impact: PROFILES.md YAML schema section

## Task 8: Profile metadata sidecar and filesystem enumeration
- Status: pending
- Dependencies: Task 7
- Scope: `src/config/config_profile_meta.c`, `src/config/config_profile_list.c`
- Acceptance criteria:
  - Reads optional `*.meta.yaml` sidecar: `display_name`, `icon`, `display_order`,
    `description` per §7.5
  - Writes sidecar files with atomic write pattern (temp + rename, mode 0600)
  - Profile and sidecar filenames validated against `^[a-zA-Z0-9_-]+$` before writing
  - Paths canonicalized with `realpath()` and verified within expected base directory
  - File opens use `O_NOFOLLOW` to reject symlink attacks
  - Enumerates profiles from `~/.local/share/inputplumber/profiles/` (user) and
    `/usr/share/inputplumber/profiles/` (system, read-only)
  - Also enumerates device configs from `/usr/share/inputplumber/devices/` and
    capability maps from `/usr/share/inputplumber/capability_maps/` (read-only,
    gap #4 filesystem reads for profile editor target capabilities)
  - Merges sidecar metadata with base profile data
  - Default profile (`default.yaml`) is always present and marked read-only
  - Produces sorted list (by `display_order`, then name)
  - YAML parser configured with: max depth 50, max document size 1MB, no custom tags
  - `tests/test_profile_list.c` passes with fixture directory
- Verification: `cmake --build build && ./build/test_profile_list`
- Documentation impact: PROFILES.md file layout section

### Phase 3: DBus Client Layer

## Task 9: sd-bus connection, version check, and NameOwnerChanged tracking
- Status: pending
- Dependencies: Task 2
- Scope: `src/dbus/ip_connection.c`, `src/dbus/ip_connection.h`
- Acceptance criteria:
  - Connects to system bus via `sd_bus_open_system()`
  - Reads `Version` property from Manager interface; fails gracefully with clear
    error if InputPlumber is not running (`ServiceUnknown`)
  - Subscribes to `NameOwnerChanged` for `org.shadowblip.InputPlumber`
  - Tracks InputPlumber's unique bus name (updated on name acquisition/loss)
  - On name loss: marks all cached state as stale, enters degraded mode
  - On name acquisition: triggers re-enumeration (via callback hook)
  - All DBus errors handled with categorized return codes (ServiceUnknown,
    AccessDenied, NoReply, InvalidArgs)
  - `AccessDenied` errors logged with guidance to add user to `inputplumber` group
  - `ServiceUnknown` triggers user-facing degraded mode (not just return code)
  - `tests/test_connection.c` passes with mock backend
- Verification: `cmake --build build && ./build/test_connection`
- Documentation impact: DBus-API.md connection model section

## Task 10: ObjectManager enumeration and device model
- Status: pending
- Dependencies: Task 9
- Scope: `src/dbus/ip_objectmanager.c`, `src/dbus/ip_device_model.c`,
  `src/dbus/ip_device_model.h`
- Acceptance criteria:
  - Calls `GetManagedObjects()` at root path, parses `a{oa{sa{sv}}}` response
  - Populates in-memory device model: Manager, CompositeDevices (with paths),
    source devices, target devices
  - Validates object paths start with `/org/shadowblip/InputPlumber/`
  - Handles empty response (InputPlumber starting up) gracefully
  - `tests/test_objectmanager_parse.c` passes with a captured DBus reply fixture
- Verification: `cmake --build build && ./build/test_objectmanager_parse`
- Documentation impact: DBus-API.md enumeration section

## Task 11: Hotplug and PropertiesChanged signal handling
- Status: pending
- Dependencies: Task 10
- Scope: `src/dbus/ip_hotplug.c`, `src/dbus/ip_properties.c`
- Acceptance criteria:
  - Subscribes to `InterfacesAdded`/`InterfacesRemoved` signals, updates device model
  - Subscribes to `PropertiesChanged` for `GamepadOrder`, `ProfileName`,
    `ProfilePath`, `TargetDevices`, `SourceDevicePaths`
  - All signal handlers verify sender unique name matches InputPlumber's tracked name
  - Validates variant types before unpacking (rejects type mismatches)
  - Enforces string length limits (256 bytes for names, 4096 for paths)
  - Enforces array size limits (max 256 elements)
  - `tests/test_hotplug.c` and `tests/test_properties_changed.c` pass with mock signals
- Verification: `cmake --build build && ./build/test_hotplug && ./build/test_properties_changed`
- Documentation impact: DBus-API.md signal handling section

## Task 12: Manager interface method wrappers
- Status: pending
- Dependencies: Task 10
- Scope: `src/dbus/ip_manager.c`, `src/dbus/ip_manager.h`
- Acceptance criteria:
  - Wrappers for: `CreateTargetDevice(kind) → path`, `StopTargetDevice(path)`,
    `AttachTargetDevice(target, composite)`, `SetTargetDevices(types)`,
    `GamepadOrder` get/set, `SupportedTargetDeviceIds` get, `SupportedTargetDevices` get
  - All wrappers return categorized error codes
  - `GamepadOrder` setter validates device paths exist in device model before calling
  - `tests/test_manager_calls.c` passes with mock backend
- Verification: `cmake --build build && ./build/test_manager_calls`
- Documentation impact: DBus-API.md Manager interface section

## Task 13: CompositeDevice interface wrappers and InterceptMode polling
- Status: pending
- Dependencies: Task 10
- Scope: `src/dbus/ip_composite.c`, `src/dbus/ip_intercept_poll.c`
- Acceptance criteria:
  - Wrappers for: `SetInterceptActivation(events, target)`, `InterceptMode` get/set,
    `LoadProfilePath(path)`, `LoadProfileFromYaml(yaml)`, `GetProfileYaml() → string`,
    `SetTargetDevices(types)`, `TargetDevices` get, `SourceDevicePaths` get,
    `PersistentId` get, `Name` get, `Capabilities` get, `OutputCapabilities` get,
    `TargetCapabilities` get, `DbusDevices` get, `Stop()`
  - `DbusDevices` property used to correlate InputEvent signals to composite devices
    (Task 14 uses this to discover DBusDevice object paths per composite)
  - InterceptMode poll state machine: IDLE → polling at 50ms (PASS expected) →
    detect ALL → ACTIVATING callback → ACTIVE → detect PASS → IDLE
  - Poll uses `SDL_AddTimer` + `SDL_PushEvent` (custom event type) to trigger poll on
    main thread, not blocking the event loop
  - Timeout handling: if InterceptMode stays in ALL unexpectedly, no infinite wait
  - `tests/test_composite_calls.c` and `tests/test_intercept_poll.c` pass
- Verification: `cmake --build build && ./build/test_composite_calls && ./build/test_intercept_poll`
- Documentation impact: DBus-API.md CompositeDevice section, gap #1 workaround

## Task 14: Source/target device properties and InputEvent signal handling
- Status: pending
- Dependencies: Task 10
- Scope: `src/dbus/ip_source.c`, `src/dbus/ip_target.c`, `src/dbus/ip_input_signal.c`
- Acceptance criteria:
  - Source device reads: `UniqueId`, `PhysPath`, `IdVendor`, `IdProduct`,
    `IdBustype`, `SerialNumber` (HIDRaw), `Name`
  - Target device reads: `DeviceType`, `Name`
  - InputEvent signal subscription on DBusDevice interfaces
  - Event string parsing to normalized input enum (INPUT_UP, INPUT_DOWN, INPUT_LEFT,
    INPUT_RIGHT, INPUT_A, INPUT_B, INPUT_START, INPUT_SELECT, INPUT_R3, etc.)
  - Sender verification on all signals
  - Event value validation (buttons: 0.0 or 1.0; axes: clamped to [-1.0, 1.0])
  - Rate limiting: max 200 events/second per device
  - `tests/test_source_props.c`, `tests/test_target_props.c`,
    `tests/test_input_signal.c` pass
- Verification: `cmake --build build && ./build/test_source_props && ./build/test_target_props && ./build/test_input_signal`
- Documentation impact: DBus-API.md source/target interfaces section

## Task 15: CreateCompositeDevice temp file workaround and GamepadOrder persistence
- Status: pending
- Dependencies: Task 11, Task 12, Task 6
- Scope: `src/dbus/ip_create_composite.c`, `src/dbus/ip_gamepad_order.c`
- Acceptance criteria:
  - Temp file created via `mkstemp()` or `O_TMPFILE` with mode 0600
  - Prefers `XDG_RUNTIME_DIR` over `/tmp`
  - Temp file unlinked after `CreateCompositeDevice` returns (success or failure)
  - Never passes user-controlled paths to `CreateCompositeDevice`
  - GamepadOrder saved to `assignments.yaml` immediately on every change (persistence
    layer only — orchestration of restart re-application is in Task 27)
  - Handles stale device paths in saved order (skip or move to Unassigned)
  - `tests/test_create_composite.c` and `tests/test_gamepad_order.c` pass
- Verification: `cmake --build build && ./build/test_create_composite && ./build/test_gamepad_order`
- Documentation impact: DBus-API.md gaps #2 and #3, OPERATIONS troubleshooting

### Phase 4: Controller Icons

## Task 16: SVG assets and icon mapping table
- Status: pending
- Dependencies: Task 1
- Scope: `data/icons/svg/` (Controllercons SVGs + custom SVGs), `LICENSE.controllercons`,
  `data/controller-icons.yaml`, `src/icons/icon_map.c`, `src/icons/icon_map.h`
- Acceptance criteria:
  - Controllercons SVGs vendored with SIL OFL 1.1 license file
  - 4 custom SVGs created: arcade-stick, hitbox, steam-deck, generic-gamepad
  - All SVGs verified nanosvg-compatible: each SVG successfully parses via
    `nsvgParseFromFile()` and rasterizes to non-zero-dimension RGBA pixels
  - `controller-icons.yaml` maps all types from §8.4: xb360, ds5, deck, gamepad, mouse,
    keyboard, etc.
  - Icon map parser loads mapping at runtime, returns icon name + display name for a
    given `DeviceType` string
  - Unknown types return generic-gamepad + raw type string
  - `tests/test_icon_map.c` passes
- Verification: `cmake --build build && ./build/test_icon_map`
- Documentation impact: README credits section, OPERATIONS icon mapping section

## Task 17: nanosvg rasterization and SDL2 texture cache
- Status: pending
- Dependencies: Task 16
- Scope: `src/icons/icon_cache.c`, `src/icons/icon_cache.h`
- Acceptance criteria:
  - At init, rasterizes all mapped SVGs to SDL2 textures at display resolution
  - Reuses single `NSVGrasterizer` for all icons
  - Textures cached in hash map keyed by icon name
  - Recoloring support via `SDL_SetTextureColorMod` for theming
  - Cleanup on shutdown (free pixel buffers, delete rasterizer, destroy textures)
  - Works with SDL2 dummy driver in tests
  - `tests/test_icon_cache.c` passes (load, rasterize, check texture dimensions)
- Verification: `cmake --build build && ./build/test_icon_cache`
- Documentation impact: none

## Task 18: Runtime icon lookup API
- Status: pending
- Dependencies: Task 17, Task 8
- Scope: `src/icons/icon_lookup.c`, `src/icons/icon_lookup.h`
- Acceptance criteria:
  - `icon_lookup(device_type, profile_icon_override)` returns texture + label
  - Profile icon override (from sidecar metadata) takes precedence
  - Absolute path icon override loads custom PNG via SDL2_image
  - Icon path validation: absolute paths checked against `..` traversal, resolved
    against safe directories only
  - Unknown device type → generic gamepad silhouette + raw type string
  - `tests/test_icon_lookup.c` passes
- Verification: `cmake --build build && ./build/test_icon_lookup`
- Documentation impact: PROFILES.md icon override section

### Phase 5: SDL2 Rendering and Widget System

## Task 19: Renderer init, theme system, and text rendering cache
- Status: pending
- Dependencies: Task 2
- Scope: `src/ui/renderer.c`, `src/ui/theme.c`, `src/ui/text.c`
- Acceptance criteria:
  - SDL2 renderer init with `SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE`
  - Verifies `SDL_RENDERER_TARGETTEXTURE` flag is available (required for pre-built
    overlay)
  - GLES fallback for Pi 4 (verify alpha blending works)
  - Theme struct loads colors from settings (`overlay_opacity`, `theme`)
  - Font loading via SDL2_ttf, text texture cache keyed by (font_id, text, color_hash)
  - Multi-line text wrapping support
  - `tests/test_renderer_init.c` and `tests/test_text.c` pass with dummy driver
- Verification: `cmake --build build && SDL_VIDEODRIVER=dummy ./build/test_renderer_init && SDL_VIDEODRIVER=dummy ./build/test_text`
- Documentation impact: none

## Task 20: Widget base and concrete widgets (Button, Label, Image, Panel)
- Status: pending
- Dependencies: Task 19
- Scope: `src/ui/widget.c`, `src/ui/widget.h`, `src/ui/widget_button.c`,
  `src/ui/widget_label.c`, `src/ui/widget_image.c`, `src/ui/widget_panel.c`
- Acceptance criteria:
  - Widget base struct with function-pointer vtable: `draw`, `handle_event`, `focus`,
    `blur`, `get_rect`, `set_rect`, `destroy`
  - Button: label texture + press callback, focused/pressed visual states
  - Label: static text, optional multi-line
  - Image: renders an SDL_Texture (for icons), supports scaling
  - Panel: container that holds and lays out child widgets
  - All widgets work with SDL2 dummy driver
  - `tests/test_widgets.c` passes (create, draw, focus, destroy for each type)
- Verification: `cmake --build build && SDL_VIDEODRIVER=dummy ./build/test_widgets`
- Documentation impact: none

## Task 21: List, Grid, TabBar, and ProgressBar widgets
- Status: pending
- Dependencies: Task 20
- Scope: `src/ui/widget_list.c`, `src/ui/widget_grid.c`, `src/ui/widget_tabbar.c`,
  `src/ui/widget_progress.c`
- Acceptance criteria:
  - List: scrollable, up/down navigation, highlight, optional icon per item
  - Grid: N rows × M columns, each cell is a widget, independent row/column
    navigation, current-position highlight
  - TabBar: horizontal tabs, left/right to switch, callback on tab change
  - ProgressBar: fill bar (0.0–1.0), configurable color
  - All widgets work with dummy driver
  - `tests/test_widget_list.c`, `tests/test_widget_grid.c`,
    `tests/test_widget_tabbar.c`, `tests/test_widget_progress.c` pass
- Verification: `cmake --build build && SDL_VIDEODRIVER=dummy ctest --test-dir build -R widget_ --output-on-failure`
- Documentation impact: none

## Task 22: Focus chain system and input event mapping
- Status: pending
- Dependencies: Task 21, Task 14
- Scope: `src/ui/focus.c`, `src/ui/input_map.c`
- Acceptance criteria:
  - Focus manager: directional navigation (up/down/left/right) between focusable widgets
  - Spatial neighbor computation for grids (context-dependent: Player vs Host mode)
  - Each widget declares directional neighbors or parent computes spatially
  - Input event mapping: DBusDevice `InputEvent` strings → normalized input enum
  - Validates event strings against known set; rejects unknown events
  - `tests/test_focus.c` and `tests/test_input_map.c` pass
- Verification: `cmake --build build && ./build/test_focus && ./build/test_input_map`
- Documentation impact: none

## Task 23: Animation primitives and dirty rect optimization
- Status: pending
- Dependencies: Task 19
- Scope: `src/ui/animation.c`, `src/ui/dirty_rect.c`
- Acceptance criteria:
  - Alpha tween (for overlay fade in/out)
  - Timing based on `SDL_GetTicks()`
  - Dirty rect tracking: only re-render changed regions of target texture
  - `SDL_RenderSetClipRect` for incremental cell updates
  - `tests/test_animation.c` passes with dummy driver
- Verification: `cmake --build build && SDL_VIDEODRIVER=dummy ./build/test_animation`
- Documentation impact: none

## Task 24: Pre-built overlay surface infrastructure (render-to-texture)
- Status: pending
- Dependencies: Task 23, Task 18
- Scope: `src/overlay/surface_build.c`, `src/overlay/surface_build.h`
- Acceptance criteria:
  - Creates `SDL_TEXTUREACCESS_TARGET` texture at screen resolution
  - Provides API for Task 29 to render grid content into the target texture
  - Provides dirty-rect mechanism for incremental re-render of changed cells
  - `SDL_SetTextureAlphaMod` applies `overlay_opacity` from settings
  - Show/hide via single `SDL_RenderCopy` + `SDL_RenderPresent` — test asserts no
    texture creation occurs in the show/hide path (structural check, not timing)
  - `tests/test_surface_build.c` passes with dummy driver (creates target texture,
    marks dirty rect, renders, verifies render target switches correctly)
- Verification: `cmake --build build && SDL_VIDEODRIVER=dummy ./build/test_surface_build`
- Documentation impact: none

### Phase 6: Controller Identification

## Task 25: Identity extraction from source device properties
- Status: pending
- Dependencies: Task 14, Task 6
- Scope: `src/identify/identity.c`, `src/identify/identity.h`
- Acceptance criteria:
  - Extracts strongest available identity per §6.2: BT MAC (layer 1) > USB serial
    (layer 2) > USB port path (layer 3) > connection order (layer 4)
  - Formats ID with type prefix: `BT:xx:xx:xx:xx:xx:xx`, `USB:SNxxxxx`,
    `USB:phys:xxxxx`, `ORDER:n`
  - Returns identity strength layer (1–4) alongside the ID
  - Handles empty `uniq` for Bluetooth devices (fallback to phys or order)
  - Picks correct source device interface per device type (evdev `UniqueId` vs HIDRaw
    `SerialNumber`)
  - `tests/test_identity.c` passes with fixtures for each layer
- Verification: `cmake --build build && ./build/test_identity`
- Documentation impact: OPERATIONS controller identification section

## Task 26: Assignment lookup, default assignment, and persistence
- Status: pending
- Dependencies: Task 25, Task 6
- Scope: `src/identify/assign.c`, `src/identify/assign_persist.c`
- Acceptance criteria:
  - On connect, looks up ID in `assignments.yaml`; if found, applies preferred
    slot + profile
  - If not found, computes lowest unoccupied slot, assigns default profile
  - On slot/profile change in overlay, saves to `assignments.yaml` atomically
  - Atomic "lowest unoccupied slot" computation (no race with simultaneous connects)
  - `tests/test_assign.c` and `tests/test_assign_persist.c` pass
- Verification: `cmake --build build && ./build/test_assign && ./build/test_assign_persist`
- Documentation impact: none

## Task 27: Identity downgrade detection and GamepadOrder restoration
- Status: pending
- Dependencies: Task 11, Task 25, Task 15
- Scope: `src/identify/identity_downgrade.c`, `src/identify/gamepad_order_restore.c`
- Acceptance criteria:
  - When a controller reconnects with a weaker identity than previously stored,
    detects the downgrade and falls back gracefully (to order-based)
  - GamepadOrder restoration orchestration: on InputPlumber restart (detected via
    NameOwnerChanged from Task 9/11), waits for full re-enumeration (InterfacesAdded
    signals settle), validates device paths, calls Task 15's persistence layer to
    load and re-apply saved GamepadOrder
  - Handles stale device paths (skip or move to Unassigned)
  - `tests/test_identity_downgrade.c` and `tests/test_order_restore.c` pass
- Verification: `cmake --build build && ./build/test_identity_downgrade && ./build/test_order_restore`
- Documentation impact: OPERATIONS troubleshooting (identity downgrade)

### Phase 7: Overlay Mode

## Task 28: Overlay state machine and lifecycle
- Status: pending
- Dependencies: Task 13, Task 24
- Scope: `src/overlay/lifecycle.c`, `src/overlay/lifecycle.h`
- Acceptance criteria:
  - State machine: IDLE → ACTIVATING (poll detected ALL) → VISIBLE → CLOSING (B
    pressed, setting InterceptMode back to PASS) → IDLE
  - Integrates with InterceptMode poll (Task 13) and pre-built surface (Task 24)
  - Timeout handling: if InterceptMode stays ALL unexpectedly, no infinite wait
  - On close: hide surface (not destroy), save assignments, resolve conflicts
  - `tests/test_overlay_lifecycle.c` passes with mock DBus
- Verification: `cmake --build build && ./build/test_overlay_lifecycle`
- Documentation impact: none

## Task 29: Character select grid rendering and Player Mode navigation
- Status: pending
- Dependencies: Task 28, Task 22, Task 18, Task 26
- Scope: `src/overlay/grid_render.c`, `src/overlay/player_mode.c`
- Acceptance criteria:
  - Renders grid: rows = physical controllers, columns = player slots + Unassigned
  - Each cell shows virtual device type icon (Task 18 lookup)
  - Current position highlighted per controller
  - Profile label per row (controller model name + current profile)
  - No user-assigned names or colors shown (§4.8 — positional display only)
  - Player Mode: each controller moves its own row independently (left/right across
    columns, up/down cycles profile)
  - No controller can affect another's row in Player Mode
  - On column change, calls assignment update (Task 26)
  - On profile change, calls `LoadProfilePath` (Task 13)
  - `tests/test_grid_render.c` and `tests/test_player_mode.c` pass
- Verification: `cmake --build build && ./build/test_grid_render && ./build/test_player_mode`
- Documentation impact: README overlay usage section

## Task 30: Host Mode and conflict detection/resolution
- Status: pending
- Dependencies: Task 29
- Scope: `src/overlay/host_mode.c`, `src/overlay/conflict.c`
- Acceptance criteria:
  - R3 toggle: first controller to press R3 becomes host, all others freeze
  - Host navigates to any row with Up/Down, then navigates within row with Left/Right
  - Host row highlighted in distinct color, frozen rows dimmed
  - R3 again exits back to Player Mode
  - Conflict detection: two controllers on same column → second arrival shown in red
  - On overlay exit: conflicted controller moved to lowest unoccupied P slot
    (deterministic, automatic)
  - Edge cases: all slots occupied (leave conflicted), no unoccupied slots (stay),
    conflicted controller on Unassigned (no conflict)
  - `tests/test_host_mode.c` and `tests/test_conflict.c` pass
- Verification: `cmake --build build && ./build/test_host_mode && ./build/test_conflict`
- Documentation impact: README overlay usage (Host Mode, conflict resolution)

## Task 31: Profile cycling and dynamic columns
- Status: pending
- Dependencies: Task 29, Task 8
- Scope: `src/overlay/profile_cycle.c`, `src/overlay/dynamic_columns.c`
- Acceptance criteria:
  - Up/down cycles through available profiles (from Task 8 enumeration)
  - Profile follows controller across columns (per-controller, not per-slot)
  - On profile change, calls `LoadProfilePath` and updates profile label
  - Column count = number of virtual (target) controllers from device model
  - Adjusts on hotplug/target device change (re-layout grid)
  - `tests/test_profile_cycle.c` and `tests/test_dynamic_columns.c` pass
- Verification: `cmake --build build && ./build/test_profile_cycle && ./build/test_dynamic_columns`
- Documentation impact: README overlay usage (profile cycling, dynamic columns)

## Task 32: Overlay trigger registration and activation/close
- Status: pending
- Dependencies: Task 28, Task 13
- Scope: `src/overlay/trigger.c`, `src/overlay/close.c`
- Acceptance criteria:
  - At startup: calls `SetInterceptActivation` on each composite device with trigger
    combo from settings, sets `InterceptMode = 1` (PASS)
  - On activation detection (poll): transitions to ACTIVATING, shows overlay
  - On B press: sets `InterceptMode` back to PASS, hides surface (not destroy),
    saves assignments, resolves conflicts, transitions to IDLE
  - Trigger combo configurable from settings (`overlay_trigger`)
  - `tests/test_trigger.c` and `tests/test_close.c` pass
- Verification: `cmake --build build && ./build/test_trigger && ./build/test_close`
- Documentation impact: README overlay usage (trigger, close), OPERATIONS config

## Task 33: Overlay integration test
- Status: pending
- Dependencies: Task 32, Task 30, Task 31
- Scope: `tests/test_overlay_integration.c`
- Acceptance criteria:
  - Full lifecycle with mock DBus: trigger → show → navigate (Player Mode) → change
    profile → conflict → Host Mode → close
  - Verifies: overlay appears, grid renders, Left/Right changes active column for
    issuing controller, Up/Down changes profile, R3 enters Host Mode, B closes overlay,
    profile changes applied to InputPlumber, conflicts detected and resolved, assignments
    saved
  - Uses SDL2 dummy driver + mock DBus backend
  - Test passes
- Verification: `cmake --build build && SDL_VIDEODRIVER=dummy ./build/test_overlay_integration`
- Documentation impact: none

### Phase 8: Manager Mode

## Task 34: Manager skeleton and tab bar
- Status: pending
- Dependencies: Task 21, Task 22
- Scope: `src/manager/manager.c`, `src/manager/manager.h`
- Acceptance criteria:
  - Separate SDL2 window for manager mode
  - Tab bar with 3 tabs: Controllers, Profiles, Settings
  - Left/Right switches tabs, Up/Down navigates within panel
  - Tab switching callback updates visible panel
  - `tests/test_manager_tabs.c` passes with dummy driver
- Verification: `cmake --build build && SDL_VIDEODRIVER=dummy ./build/test_manager_tabs`
- Documentation impact: README manager usage section

## Task 35: Controllers tab — list, add/remove, and type change
- Status: pending
- Dependencies: Task 34, Task 12
- Scope: `src/manager/controllers_tab.c`
- Acceptance criteria:
  - Lists current target devices with their type (from device model)
  - Add button: type picker from `SupportedTargetDeviceIds`, calls
    `CreateTargetDevice`
  - Remove button: calls `StopTargetDevice`, controller auto-Unassigned
  - Change type: select slot, pick new type, calls `SetTargetDevices`
  - Mixed types allowed (P1=xb360, P2=ds5)
  - `tests/test_controllers_tab.c` passes with mock DBus
- Verification: `cmake --build build && ./build/test_controllers_tab`
- Documentation impact: README manager usage (Controllers tab)

## Task 36: Profiles tab — browse, create, and delete
- Status: pending
- Dependencies: Task 34, Task 8
- Scope: `src/manager/profiles_tab.c`
- Acceptance criteria:
  - Lists profiles from filesystem enumeration (Task 8), sorted, with icons
  - Default profile shown as read-only
  - Create new: pick starting point (Default copy / Empty / Clone existing) → opens
    editor (Task 37/38)
  - New profile name validated against `^[a-zA-Z0-9_-]+$` before creation
  - Delete: removes user-created profile YAML + sidecar (not default)
  - Delete validates target is not the default profile and not a system profile
  - `tests/test_profiles_tab.c` passes
- Verification: `cmake --build build && ./build/test_profiles_tab`
- Documentation impact: README manager usage (Profiles tab), PROFILES.md

## Task 37: Profile editor — controller diagram and binding list mode
- Status: pending
- Dependencies: Task 36, Task 18, Task 13, Task 8
- Scope: `src/manager/profile_diagram.c`, `src/manager/profile_editor_list.c`
- Acceptance criteria:
  - SVG-based controller diagram with individually highlightable buttons (left panel)
  - Binding list (right panel): Up/Down scrolls, highlighted row lights corresponding
    button on diagram
  - A edits binding: choose target from list or capture physical button press
  - Target list populated from virtual device capabilities (read from `Capabilities`/
    `OutputCapabilities`/`TargetCapabilities` properties via Task 13, and from
    capability maps on filesystem via Task 8)
  - Capture mode listens to InputEvent signals (Task 14)
  - Diagram and binding list always synchronized
  - `tests/test_profile_diagram.c` and `tests/test_editor_list_mode.c` pass
- Verification: `cmake --build build && ./build/test_profile_diagram && ./build/test_editor_list_mode`
- Documentation impact: PROFILES.md editor section

## Task 38: Profile editor — sequential binding mode and validation
- Status: pending
- Dependencies: Task 37, Task 21
- Scope: `src/manager/profile_editor_seq.c`, `src/manager/profile_validate.c`
- Acceptance criteria:
  - Sequential mode: prompts for each button in order, diagram lights current button
  - Press physical button → captured → auto-advance
  - B skips, Start cancels
  - Progress bar (Task 21 widget) shows completion
  - Validation: NES minimum (A, B, D-Pad Up, D-Pad Down, D-Pad Left, D-Pad Right)
    must be bound; error shown if any missing
  - Validation is a hard gate before saving
  - `tests/test_editor_seq_mode.c` and `tests/test_profile_validate.c` pass
- Verification: `cmake --build build && ./build/test_editor_seq_mode && ./build/test_profile_validate`
- Documentation impact: PROFILES.md editor + validation sections

## Task 39: Profile save and Settings tab
- Status: pending
- Dependencies: Task 38, Task 5, Task 7, Task 8
- Scope: `src/manager/profile_save.c`, `src/manager/settings_tab.c`
- Acceptance criteria:
  - Profile save: serializes to `device_profile_v1` YAML via libyaml emitter, writes
    to inputplumber profiles dir, writes sidecar metadata
  - Profile filename validated against `^[a-zA-Z0-9_-]+$` before writing
  - Profile file path canonicalized with `realpath()` and verified within profiles dir
  - Profile file opened with `O_NOFOLLOW` on write
  - NES minimum validation enforced before save (cannot save invalid profile)
  - Settings tab: all settings from §5.5 — launch at boot, theme, overlay opacity,
    virtual controller startup config (count + types), overlay trigger combo
  - Settings tab icon overrides: allows per-type icon mapping overrides stored in
    settings (overrides the system `controller-icons.yaml` mapping for the user's
    session; distinct from per-profile icon override in §8.5 sidecar)
  - Settings changes written to `settings.yaml` atomically
  - `tests/test_profile_save.c` and `tests/test_settings_tab.c` pass
- Verification: `cmake --build build && ./build/test_profile_save && ./build/test_settings_tab`
- Documentation impact: README manager usage (Settings tab), OPERATIONS config

## Task 40: Systemd service installation and manager integration test
- Status: pending
- Dependencies: Task 39
- Scope: `src/manager/service_install.c`, `tests/test_manager_integration.c`
- Acceptance criteria:
  - First-run prompt: "Enable overlay service?" → writes
    `~/.config/systemd/user/controller-box.service` atomically (mkstemp + rename)
  - Unit file is a static template (only binary path / Flatpak app ID is compiled-in)
  - Calls `systemctl --user enable --now controller-box` (or `flatpak-spawn --host`
    in Flatpak)
  - Verifies service started (`systemctl --user is-active`)
  - Handles systemd not available gracefully
  - Checks if user is in `inputplumber` group; guides if not
  - Integration test: full manager flow with mock DBus — add controller, create
    profile, edit in sequential mode, validate, save, set settings, install service
  - `tests/test_service_install.c` and `tests/test_manager_integration.c` pass
- Verification: `cmake --build build && ./build/test_service_install && ./build/test_manager_integration`
- Documentation impact: README first-run setup, PACKAGING.md service install

### Phase 9: Packaging

## Task 41: CMake install rules, systemd service file, and desktop entry
- Status: pending
- Dependencies: Task 40, Task 2
- Scope: `CMakeLists.txt` install targets, `packaging/controller-box.service`,
  `packaging/controller-box-manager.desktop`
- Acceptance criteria:
  - `make DESTDIR=/tmp/test-install install` places:
    `/tmp/test-install/usr/bin/controller-box`,
    `/tmp/test-install/usr/share/controller-box/icons/`,
    `/tmp/test-install/usr/share/controller-box/controller-icons.yaml`,
    `/tmp/test-install/usr/share/applications/controller-box-manager.desktop`
  - systemd service file with `After=inputplumber.service`,
    `Requires=inputplumber.service`, `Restart=always`,
    `ExecStart=/usr/bin/controller-box --overlay-service`
  - Desktop entry launches manager mode
  - `tests/test_packaging_install.sh` verifies file layout
- Verification: `cmake --build build && cmake --install build --prefix /tmp/test-install &&
  tests/test_packaging_install.sh /tmp/test-install`
- Documentation impact: PACKAGING.md tarball install section

## Task 42: Flatpak manifest
- Status: pending
- Dependencies: Task 41
- Scope: `packaging/org.shadowblip.ControllerBox.yaml`
- Acceptance criteria:
  - Flatpak manifest builds Controller-Box with CMake
  - SDL2, SDL2_ttf, SDL2_image available (from SDK extension or built from source
    modules)
  - nanosvg vendored in source tree
  - Permissions per §9.1 (narrowed per security review):
    `--system-talk-name=org.shadowblip.InputPlumber`,
    `--filesystem=~/.local/share/inputplumber/profiles`,
    `--filesystem=/usr/share/inputplumber:ro`,
    `--filesystem=~/.config/controller-box`,
    `--filesystem=~/.config/systemd/user`
  - Each permission documented with rationale comment in manifest
  - `flatpak-builder` succeeds (or manifest validates with `flatpak-builder --show-deps`)
- Verification: `flatpak-builder --user --install --force tests/test-repo packaging/org.shadowblip.ControllerBox.yaml`
  (or `flatpak-builder --show-deps packaging/org.shadowblip.ControllerBox.yaml` if
  builder is unavailable)
- Documentation impact: PACKAGING.md Flatpak section

## Task 43: Version embedding and packaging integration test
- Status: pending
- Dependencies: Task 41, Task 42
- Scope: CMake version injection into `config.h.in`, `--version` flag, end-to-end
  packaging test
- Acceptance criteria:
  - Version from CMake `project(VERSION ...)` injected into `config.h`
  - `controller-box --version` prints version in both modes
  - Packaging integration test: build tarball, `make install` to temp dir, verify
    file layout and binary runs
  - If Flatpak builder available: build Flatpak, verify binary runs inside sandbox
  - `tests/test_packaging.sh` passes
- Verification: `cmake --build build && ./build/controller-box --version && tests/test_packaging.sh`
- Documentation impact: PACKAGING.md version section

### Phase 10: Final

## Task 44: Final documentation and specification audit
- Status: pending
- Dependencies: Task 1, Task 2, Task 3, Task 4, Task 5, Task 6, Task 7, Task 8, Task 9,
  Task 10, Task 11, Task 12, Task 13, Task 14, Task 15, Task 16, Task 17, Task 18, Task 19,
  Task 20, Task 21, Task 22, Task 23, Task 24, Task 25, Task 26, Task 27, Task 28, Task 29,
  Task 30, Task 31, Task 32, Task 33, Task 34, Task 35, Task 36, Task 37, Task 38, Task 39,
  Task 40, Task 41, Task 42, Task 43
- Scope: `README.md` (full rewrite), `docs/OPERATIONS.md` (full rewrite),
  `docs/DBus-API.md` (new), `docs/PROFILES.md` (new), `docs/PACKAGING.md` (new),
  `docs/FACTORY.md` (relocated factory boilerplate), specification coverage audit
- Acceptance criteria:
  - README.md rewritten as product README: what Controller-Box is, requirements, install
    (Flatpak + tarball), overlay usage, manager usage, config file locations, credits.
    No factory boilerplate remains.
  - docs/OPERATIONS.md rewritten as product operations: service architecture, systemd
    management, InputPlumber dependency, polkit rules, config formats, icon mapping,
    DBus gaps, performance expectations, troubleshooting (all 9 entries from docs
    review).
  - docs/DBus-API.md created: full DBus API reference (connection model, Manager/
    CompositeDevice/Target/Source interfaces, five gaps and workarounds, object tree).
  - docs/PROFILES.md created: profile ownership model, file layout, DeviceProfile YAML
    schema, editor modes, NES minimum validation, default profile, new-profile flow,
    per-controller scope, advanced mappings note.
  - docs/PACKAGING.md created: Flatpak (permissions, Flathub, service install),
    tarball (CMake, make install), install layout, InputPlumber dependency, post-v1
    roadmap, build-from-source.
  - Factory boilerplate relocated to docs/FACTORY.md (or noted as branch-only).
  - Documentation consistency checklist verified: all 14 sync points (trigger combo,
    config paths, settings.yaml keys, assignments.yaml prefixes, DeviceProfile schema,
    virtual type list, icon mapping, systemd unit, Flatpak permissions, DBus gaps,
    NES minimum, performance targets, install order, out-of-scope items) match across
    all docs and the spec.
  - Specification coverage audit: every spec section (§1–§13) has corresponding
    implementation and documentation. No spec requirement is unaddressed.
  - `scripts/check-docs-sync.sh` passes.
- Verification: `scripts/check-docs-sync.sh && scripts/final-gate.sh`
- Documentation impact: all documentation artifacts

## Final Completion Gates

1. **Specification coverage:** Every section of `docs/SPEC.md` (§1–§13) has
   corresponding implementation tasks and documentation. No requirement is unaddressed.
2. **Tests:** All unit tests pass (`ctest --output-on-failure`). Integration tests pass
   with mock DBus. SDL2 dummy driver used for display-free testing.
3. **Documentation:** README, OPERATIONS, DBus-API.md, PROFILES.md, PACKAGING.md all
   match actual behavior. Documentation consistency checklist passes.
4. **Clean Git state:** All changes committed to `develop` branch with atomic commits
   (one logical change per commit). Working tree is clean.
5. **Build:** `cmake --build build` succeeds with `-Wall -Wextra` (warnings reviewed,
   no errors). `make install` produces correct file layout. Flatpak manifest builds (if
   builder available).
6. **Security review items addressed:** DBus sender verification, temp file security,
  path validation, YAML safety, systemd unit atomic write, Flatpak least-privilege
  permissions, polkit group check — all implemented and tested.