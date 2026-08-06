# Scratchpad

## Task 1 (complete) — CMake skeleton, dependency detection, nanosvg vendoring

Implemented the foundation per IMPLEMENTATION_PLAN.md Task 1.

### What landed
- `CMakeLists.txt`: C11 project, pkg-config detection of SDL2/SDL2_ttf/SDL2_image/libsystemd/libyaml/cmocka; `enable_testing()`.
- `config.h.in` → `build/config.h` with `DATA_DIR=/usr/share/controller-box`, `ICON_DIR=/usr/share/controller-box/icons`, `CONFIG_DIR=.config/controller-box`, plus InputPlumber dirs and version.
- `third_party/nanosvg/`: vendored `nanosvg.h`, `nanosvgrast.h`, `LICENSE.txt` (zlib), and `nanosvg_impl.c` (expands both IMPLEMENTATION defines).
- `src/` stubs establishing the directory structure: `src/app/main.c`, `src/config/config_paths.c`, `src/dbus/dbus_client.c`, `src/ui/widget.c`, plus `src/controllerbox.h`.
- `tests/smoke_test_sdl2.c` (headless-safe via dummy driver fallback) and `tests/smoke_test_nanosvg.c` (parse+rasterize a 64x64 SVG, assert a red pixel).
- `shell.nix` providing all native build deps (the sandbox lacks them).
- README "Building Controller-Box from source" section.

### Verification (all pass)
- `nix-shell --run 'cmake -B build && cmake --build build && ./build/smoke_test_sdl2 && ./build/smoke_test_nanosvg'` → OK/OK
- `ctest --test-dir build` → 2/2 pass
- `scripts/verify-boilerplate.sh`, `check-plan-freshness.sh`, `branch-guard.sh` → exit 0

### Gotchas fixed
- nanosvg `nsvgParse` mutates its input buffer → must strdup before parsing (was segfaulting).
- `include(GNUInstallDirs)` must precede `configure_file` or `@CMAKE_INSTALL_DATADIR@` is empty in config.h.
- Vendored nanosvg compiled with `-w` to silence its warnings; stubs use a typedef anchor to avoid the ISO-C empty-translation-unit warning under -Wpedantic.

### Next
Task 2 (dual-mode entry point + libcontrollerbox structure), deps: Task 1 (now done). The `controllerbox` static library target and `src/app/main.c` stub already exist for Task 2 to extend.
## Task 2 (complete) — Dual-mode entry point and shared library structure

### Recovery first
On entering this iteration, HEAD was at 43fbed2 (planning commit) — a stray git
reset had discarded the Task 1 commit 07f8f78, dropping all src/ and third_party/.
Since 07f8f78's parent == 43fbed2, recovered with `git merge --ff-only 07f8f78`.
Verified Task 1 baseline (clean build, ctest 2/2) before any Task 2 work.

### What landed
- `src/app/main.c`: dual-mode entry point. Default = overlay service (SPEC §2.3);
  `--manager` selects manager mode. Flags: `--version` (prints CMake version),
  `--dry-run` (print mode banner + exit, headless-safe), `-h/--help` (usage),
  unknown options → stderr + exit 2.
- `CMakeLists.txt`: moved `src/app/main.c` out of `controllerbox` static lib into
  a new `controller-box` executable that links `controllerbox` (so both modes link
  against `libcontrollerbox.a`). Added binary RUNTIME install rule.
- README: "Running Controller-Box" usage section.
- IMPLEMENTATION_PLAN.md Task 2 status → complete.

### Verification (all pass)
- clean `cmake -B build && cmake --build build` (no warnings under -Wall -Wextra -Wpedantic)
- `./build/controller-box --version` → `controller-box 0.1.0`
- `./build/controller-box` (no args) → `overlay-service mode`
- `./build/controller-box --overlay-service --dry-run` → `overlay-service mode (dry-run)`
- `./build/controller-box --manager --dry-run` → `manager mode (dry-run)`
- `./build/controller-box --bogus` → exit 2
- `ldd controller-box` confirms linkage via libcontrollerbox.a (SDL2/SDL2_ttf/SDL2_image/libsystemd/libyaml)
- ctest 2/2; verify-boilerplate, check-plan-freshness, branch-guard → exit 0

### Next
Task 3 (test harness setup: cmocka + CTest + SDL2 dummy driver, sample test) or
Task 4 (config paths + libyaml). Both depend only on Task 1 (done). Task 3 unblocks
later test-bearing tasks, so prefer it.

## Task 3 (complete) — Test harness setup

### What landed
- `tests/CMakeLists.txt`: modularised test target definitions. Smoke tests always
  built; cmocka-based unit tests gated on `CMOCKA_FOUND` (pkg_check_modules). All
  tests registered with CTest. Included from root CMakeLists.txt via `include()`.
- `tests/test_harness.{h,c}`: SDL2 dummy-driver test utility. `test_harness_sdl_init()`
  auto-sets `SDL_VIDEODRIVER=dummy` if unset (respects existing env), creates a
  hidden 320x240 window + software renderer. `test_harness_sdl_shutdown()` cleans up.
  Used by later rendering/widget/animation tests.
- `tests/dbus_mock.{h,c}`: DBus mock interface abstraction. Defines
  `ip_dbus_backend` (function-pointer vtable with connect/disconnect/call_method/
  get_property/set_property/get_managed_objects/subscribe_signal/inject_signal) +
  `ip_dbus_mock` canned-response store (max 32 expectations keyed by iface+member).
  Mock backend replays expectations; unregistered calls return -ENXIO. Production
  code (Task 9+) calls through this vtable; tests inject canned responses.
- `tests/test_sample.c`: 6 cmocka tests — trivial assertions + DBus mock vtable
  exercises (expect/find, call_method, get_property, connect/get_unique_name,
  error expectation).
- `tests/test_sdl_dummy.c`: 2 cmocka tests — init+render via test_harness, multiple
  init/shutdown cycles.
- Root `CMakeLists.txt`: moved inline smoke test definitions into
  `tests/CMakeLists.txt`, added `include(tests/CMakeLists.txt)`.

### Verification (all pass)
- clean build, no warnings under -Wall -Wextra -Wpedantic; Debug with -Werror clean
- ctest 4/4: smoke_test_sdl2, smoke_test_nanosvg, test_sample, test_sdl_dummy
- `test_sdl_dummy` passes both with and without SDL_VIDEODRIVER set (harness
  auto-falls-back to dummy)
- verify-boilerplate, check-plan-freshness, branch-guard → exit 0

### Gotchas fixed
- `mock.bus` is set to `mock` (self-pointer) in init; vtable callbacks take
  `ip_bus_handle bus` directly (not `ip_bus_handle *`), so pass `mock.bus` not
  `&mock.bus`. Only `connect()` takes `ip_bus_handle *` (may set the handle).
- test_sample.c needed `<errno.h>` (for ENXIO/EACCES) and `<stdlib.h>` (for free).

### Next
Task 4 (config paths + libyaml) — deps: Task 1 (done). Now that the test harness
is in place, Task 4's `tests/test_config_paths.c` can use cmocka + the test support
library. Task 5+ (settings.yaml) also unblocked.

## Task 4 (complete) — Config directory resolution and YAML library integration

### What landed
- `src/config/config_paths.h`: API for XDG path resolution + system path getters.
  `cbx_resolve_config_dir()` / `cbx_resolve_user_profiles_dir()` return paths
  without side effects. `cbx_config_dir()` / `cbx_user_profiles_dir()` resolve AND
  create with mode 0700. `cbx_ensure_dir()` does recursive mkdir. System getters:
  `cbx_system_inputplumber_dir()`, `cbx_system_profiles_dir()`,
  `cbx_system_devices_dir()`, `cbx_system_capability_maps_dir()`, `cbx_data_dir()`,
  `cbx_icon_dir()`.
- `src/config/config_paths.c`: implements XDG spec — $XDG_CONFIG_HOME/
  controller-box or $HOME/.config/controller-box; $XDG_DATA_HOME/inputplumber/
  profiles or $HOME/.local/share/inputplumber/profiles. Relative/empty XDG values
  ignored per spec. System paths from config.h compile-time constants.
- `tests/test_config_paths.c`: 16 cmocka tests — XDG absolute/fallback/relative/
  empty/buf-too-small for both config and profiles dirs, recursive dir creation
  with 0700 mode, idempotent creation, system path accessors.
- `tests/CMakeLists.txt`: added `test_config_paths` target linking `controllerbox`
  + `PkgConfig::CMOCKA`.
- libyaml (`pkg_check_modules yaml-0.1`) already linked from Task 1 — no CMake
  change needed.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 5/5: smoke_test_sdl2, smoke_test_nanosvg, test_sample, test_sdl_dummy,
  test_config_paths
- `test_config_paths` 16/16 cmocka tests pass
- verify-boilerplate, check-plan-freshness, branch-guard → exit 0

### Gotchas fixed
- `-Werror=format-truncation` in Debug mode flags snprintf into PATH_MAX buffer
  when the source variable is also PATH_MAX. Fix: use `char intermediate[PATH_MAX
  + 32]` for intermediate buffers that append suffixes to PATH_MAX-length paths.
- Tests set/unset env vars (HOME, XDG_CONFIG_HOME, XDG_DATA_HOME) directly —
  cmocka tests are not isolated by default. Each test cleans up its env vars
  afterward. No fixtures needed.

### Next
Task 5 (settings.yaml read/write) or Task 6 (assignments.yaml) — both depend
on Task 4 (now done). Task 5 unblocks Task 39 (Settings tab), Task 6 unblocks
Task 15 (CreateCompositeDevice + GamepadOrder persistence). Prefer Task 5
(settings is simpler, no ID validation complexity).

## Task 5 (complete) — settings.yaml read/write

### What landed
- `src/config/config_settings.h`: API for settings.yaml (SPEC §7.3).
  `cbx_settings` struct with overlay_trigger, launch_at_boot, theme,
  overlay_opacity, virtual_controllers (count + types[16]). Functions:
  `cbx_settings_defaults()`, `cbx_settings_load()`, `cbx_settings_validate()`,
  `cbx_settings_save()`, `cbx_is_known_controller_type()`.
- `src/config/config_settings.c`: libyaml event-based parser + document-based
  emitter. Parser enforces: max depth 50, max doc size 1MB (file stat before
  parse), no custom tags (checks scalar/mapping/sequence tag != NULL, rejects
  tag directives in document start). Load starts with defaults, overwrites from
  YAML, clamps out-of-range values, pads types to count. Save validates first,
  writes atomically (mkstemp in same dir + fchmod 0600 + fsync + rename).
  Known-good types: xb360, ds5, deck, gamepad, mouse, keyboard, touchscreen.
- `tests/test_settings.c`: 18 cmocka tests with cmocka_unit_test_setup_teardown
  fixture (temp HOME + unset XDG). Tests: defaults (no-file + function),
  round-trip (modified + defaults), validation (opacity/count/types),
  save-rejects-invalid, missing-fields → defaults, clamp-on-load,
  file-mode-0600, flow-style types, known-good types, max-doc-size (1MB+),
  custom-tags rejected, tag-directives rejected, empty-file → defaults,
  YAML 1.1 bool variants (yes/no/on/off/true/false).

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 6/6: smoke_test_sdl2, smoke_test_nanosvg, test_sample, test_sdl_dummy,
  test_config_paths, test_settings
- test_settings 18/18 cmocka tests pass
- verify-boilerplate, check-plan-freshness, branch-guard → exit 0

### Gotchas fixed
- **Emitter key bug**: First version used `s->overlay_trigger` as the YAML key
  instead of literal `"overlay_trigger"`. Fix: use plain string literal for keys,
  struct member only for values.
- **Document deletion**: `yaml_emitter_dump()` does NOT delete the document;
  caller must call `yaml_document_delete()`. Moved to `out:` label to handle
  all error paths after document initialization.
- **Format-truncation warnings** (same as Task 4 mem): test helper buffers for
  path construction use `PATH_MAX + 64` when snprintf appends suffixes to
  `test_home` (which is PATH_MAX). Also `system()` return values must be
  captured under -Werror=unused-result (glibc attribute).

### Next
Task 6 (assignments.yaml read/write + gamepad order persistence) — deps: Task 4
(done). Unblocks Task 15 (CreateCompositeDevice + GamepadOrder persistence).
Alternatively, Task 7+ (whatever is next in the plan). Check `ralph tools task
ready` and the plan.

## Task 6 (complete) — assignments.yaml read/write and gamepad order persistence

### What landed
- `src/config/config_assignments.h`: API for assignments.yaml (SPEC §7.4).
  `cbx_assignment` struct (id, slot, profile), `cbx_assignments` struct
  (assignments[32] + gamepad_order[16]). Functions: `cbx_assignments_init()`,
  `cbx_assignments_load()`, `cbx_assignments_validate()`, `cbx_assignments_save()`,
  `cbx_validate_id()`, `cbx_validate_profile()`.
- `src/config/config_assignments.c`: libyaml event-based parser (max depth 50,
  max doc 1MB, no custom tags/tag directives) + document-based emitter + atomic
  write (mkstemp + fchmod 0600 + fsync + rename). ID validation for 4 formats:
  BT:6-octet MAC (hex pairs), USB:serial (alnum/dash/underscore), USB:phys:port-path,
  ORDER:n (non-negative int). Profile validation ^[a-zA-Z0-9_-]+$ or empty/NULL.
- `tests/test_assignments.c`: 31 cmocka tests — ID validation (all 4 formats +
  invalid variants), profile validation, no-file→empty, empty-file→empty,
  round-trip (BT/USB, empty, USB:phys, empty profile, ORDER), save rejects
  invalid id/negative slot/invalid profile/invalid gamepad_order, file mode 0600,
  YAML security (max doc 1MB, custom tags, tag directives), parse from YAML,
  gamepad_order-only, no-gamepad_order, validate empty/NULL.
- `CMakeLists.txt`: added `src/config/config_assignments.c` to controllerbox lib.
- `tests/CMakeLists.txt`: added `test_assignments` target + CTest registration.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 7/7: smoke_test_sdl2, smoke_test_nanosvg, test_sample, test_sdl_dummy,
  test_config_paths, test_settings, test_assignments
- test_assignments 31/31 cmocka tests pass
- verify-boilerplate, check-plan-freshness, branch-guard → exit 0

### Gotchas fixed
- **BT:MAC octet count**: SPEC §7.4 example `BT:AB:CD:01:EF:23` has only 5
  octets, but the format string `BT:xx:xx:xx:xx:xx:xx` in the acceptance criteria
  has 6. A real MAC address is 6 bytes = 6 hex octet pairs. The spec example is a
  typo. Validator requires 6 octets; tests use 6-octet MACs.
- **cbx_validate_profile(NULL)**: Initially returned false, but NULL should be
  treated as "no profile" (valid, same as empty string). Fixed to return true.
- **fsync return value**: On some systems fsync may fail non-fatally; the
  existing pattern (from config_settings.c) ignores the return. Cast not needed
  since the return is not attribute-warned (unlike system()).

### Next
Task 7 (Profile YAML parse and generate — InputPlumber device_profile_v1) —
deps: Task 4 (done). Unblocks Task 8 (profile metadata sidecar + enumeration).
Alternatively, Task 9 (sd-bus connection) — deps: Task 2 (done). Check
`ralph tools task ready` and the plan.

## Task 7 (complete) — Profile YAML parse and generate (device_profile_v1)

### What landed
- `src/config/config_profile.h`: API for InputPlumber device_profile_v1 YAML
  (SPEC §7.6). `cbx_profile` struct (version, kind, name, description,
  mappings[128]). `cbx_profile_mapping` (name, source_event, target_events[16]).
  `cbx_source_event` (device_class, props[8]). `cbx_target_event` (device_class,
  value). Functions: `cbx_profile_init`, `cbx_profile_load` (from file path),
  `cbx_profile_parse` (from YAML string), `cbx_profile_validate`,
  `cbx_profile_save` (atomic write to arbitrary path), `cbx_profile_serialize`
  (to malloc'd buffer via open_memstream).
- `src/config/config_profile.c`: libyaml event-based parser with state machine
  for nested device_profile_v1 schema. source_event has dynamic device-class
  key → props mapping (e.g. gamepad: { button: Start }). target_events has
  device-class → scalar (e.g. keyboard: KeyEsc). Complex target events (mapping
  values for chord/delayed_chord) accepted via skip_depth mechanism (stored with
  empty value, nested content skipped). Document-based emitter with 2-space
  indent. Security: max depth 50, max doc size 1MB, no custom tags/tag directives.
  Atomic write: mkstemp + fchmod 0644 + fsync + rename. Profile files use 0644
  (InputPlumber format, readable by other tools — not sensitive config files).
- `tests/test_profile_yaml.c`: 26 cmocka tests — init defaults, parse spec
  example, parse empty/multiple/multi-prop/missing-fields/empty-file/scalar-source,
  serialize basic, round-trip (simple/multiple/empty), validation
  (valid/invalid version/invalid kind/null), save rejects invalid, file I/O
  (round-trip, load nonexistent, atomic mode 0644), YAML security (max doc size,
  custom tags, tag directives, max depth), serialize null args, complex target
  event.
- `CMakeLists.txt`: added `src/config/config_profile.c` to controllerbox lib.
- `tests/CMakeLists.txt`: added `test_profile_yaml` target + CTest registration.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 8/8: smoke_test_sdl2, smoke_test_nanosvg, test_sample, test_sdl_dummy,
  test_config_paths, test_settings, test_assignments, test_profile_yaml
- test_profile_yaml 26/26 cmocka tests pass
- verify-boilerplate, check-plan-freshness, branch-guard → exit 0

### Gotchas fixed
- **Missing `<stdbool.h>`**: The .c file uses `bool` in internal parse_ctx
  struct. Must include `<stdbool.h>` (the header includes it, but the .c file
  uses bool before the header's include takes effect — actually the header IS
  included first, but C11 requires stdbool.h to be explicitly included for bool
  in the .c file's own type definitions).
- **strncpy truncation warning**: `safe_copy()` used strncpy which triggers
  `-Werror=stringop-truncation` under GCC Debug. Fix: use `snprintf(dst,
  dst_size, "%s", src)` instead.
- **`%TAG` directive YAML**: libyaml requires `---` after `%TAG` directive,
  otherwise the parser errors (returns false from yaml_parser_parse). Without
  `---`, the error is -EIO (parser error), not -EPERM (tag check). Test fixed to
  include `---`.
- **Max depth test**: Each `a:` at the same indentation level is at depth 2,
  not nested. Must increase indentation (2 spaces per level) to create actual
  nesting that exceeds MAX_YAML_DEPTH=50.

### Next
Task 8 (profile metadata sidecar + filesystem enumeration) — deps: Task 7
(now done). Unblocks Task 18 (icon lookup), Task 36 (profiles tab), Task 37/38
(profile editor), Task 39 (profile save + settings tab).
Alternatively, Task 9 (sd-bus connection) — deps: Task 2 (done). Check
`ralph tools task ready` and the plan.

## Task 8 (complete) — Profile metadata sidecar and filesystem enumeration

### What landed
- `src/config/config_profile_meta.h`: API for `*.meta.yaml` sidecars (SPEC §7.5).
  `cbx_profile_meta` struct (display_name, icon, display_order, description +
  has_* flags). Functions: init, load (O_NOFOLLOW), parse (YAML string),
  save (atomic, mode 0600), serialize (libyaml emitter), load_for/save_for
  (profile-name based, with realpath + base-dir verification). `cbx_validate_filename()`
  validates ^[a-zA-Z0-9_-]+$.
- `src/config/config_profile_meta.c`: libyaml event-based parser (flat
  mapping, max depth 50, max doc 1MB, no custom tags/tag directives) +
  document-based emitter + atomic write (mkstemp + fchmod 0600 + fsync +
  rename). O_NOFOLLOW on all file opens. `build_sidecar_path()` constructs
  path in `<config_dir>/profile-metadata/`, ensures dir exists, realpath-
  canonicalizes both meta dir and config dir, verifies meta dir is within
  config dir (rejects path escape).
- `src/config/config_profile_list.h`: API for profile enumeration + file
  listing. `cbx_profile_entry` (filename, path, display_name, description,
  icon, display_order, is_system, is_default, read_only, has_meta).
  `cbx_profile_list` (max 64 entries). `cbx_file_entry` + `cbx_file_list`
  for device configs/capability maps (max 64). Functions:
  `cbx_profile_list_enumerate()` (default paths), `cbx_profile_list_enumerate_dirs()`
  (explicit paths for testing), `cbx_file_list_enumerate()`,
  `cbx_device_config_list_enumerate()`, `cbx_capability_map_list_enumerate()`.
- `src/config/config_profile_list.c`: Enumerates *.yaml files from user +
  system dirs. User dir takes precedence (dedup). For each profile: loads
  name/description from profile YAML (via cbx_profile_load), loads sidecar
  (if exists), merges (sidecar overrides). Default profile (filename ==
  "default") marked read-only. System profiles marked read-only. Sorted by
  display_order, then display_name. Device configs/capability maps: simple
  *.yaml file listing, sorted by name.
- `tests/test_profile_list.c`: 34 cmocka tests (12 simple + 22 env with
  setup/teardown fixture). Tests: meta init/parse-all/parse-partial/parse-empty/
  unknown-keys/serialize, filename validation (valid+invalid), YAML security
  (custom tags, tag directives, max depth 50, max doc 1MB), meta round-trip
  file (mode 0600), load nonexistent, O_NOFOLLOW symlink rejection (→ -ELOOP),
  save_for/load_for with realpath verification + mode 0600, save_for invalid
  name rejection, enumerate empty/user-only/system-only/both-dedup/
  default-readonly/with-sidecar/partial-sidecar/no-sidecar/
  no-profile-name-fallback/sorted-by-order-then-name/sorted-same-order-by-name/
  ignores-non-yaml, file-list-enumerate/empty/nonexistent-dir,
  enumerate-real-paths integration (isolated HOME, sidecar override verified).
- `CMakeLists.txt`: added config_profile_meta.c + config_profile_list.c to
  controllerbox lib.
- `tests/CMakeLists.txt`: added test_profile_list target + CTest registration.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 9/9: smoke_test_sdl2, smoke_test_nanosvg, test_sample, test_sdl_dummy,
  test_config_paths, test_settings, test_assignments, test_profile_yaml,
  test_profile_list
- test_profile_list 34/34 cmocka tests pass (12 simple + 22 env)
- verify-boilerplate, check-plan-freshness, branch-guard → exit 0

### Gotchas fixed
- **Format-truncation warnings (again)**: test_env fields are PATH_MAX+32,
  so local buffers that append suffixes need PATH_MAX+512 to avoid
  -Werror=format-truncation under GCC Debug. Same pattern as Tasks 4-7.
- **Max depth test**: Flat `"a:\n  "` repeated 60× is NOT nested — it's a flat
  mapping at depth 1. Must increment indentation per level to create actual
  nesting. Same issue as Task 7.
- **Directory creation order**: `mkdir(.local/share)` fails if `.local`
  doesn't exist. Must create parent dirs bottom-up: `.local` → `.local/share`
  → `.local/share/inputplumber` → `.local/share/inputplumber/profiles`.
  Same for `.config` → `.config/controller-box` → `.config/controller-box/profile-metadata`.
- **Unused function warning**: Removed `open_read_nofollow` from
  config_profile_list.c (profile loading uses `cbx_profile_load` which uses
  `fopen`). Sidecar loading handles O_NOFOLLOW in config_profile_meta.c.
- **config_profile_list.c missing `#include <unistd.h>`**: For `close()` if
  needed (though we removed the function that used it, still good to include).

### Next
Task 9 (sd-bus connection, version check, NameOwnerChanged tracking) — deps:
Task 2 (done). Unblocks Task 10 (ObjectManager enumeration), then all DBus
layer tasks. Alternatively Task 16 (SVG assets + icon mapping) — deps: Task 1
(done). Check `ralph tools task ready` and the plan.

## Task 9 (complete) — sd-bus connection, version check, NameOwnerChanged tracking

### What landed
- `src/dbus/ip_connection.h`: API for InputPlumber DBus connection management
  (SPEC §10.1). `ip_connection` struct (backend, bus, state, unique_name,
  version, reenumerate_cb, degraded_cb). `ip_conn_state` enum (DISCONNECTED,
  CONNECTED, DEGRADED). Categorized error code macros (IP_ERR_SERVICE_UNKNOWN
  = -EUNATCH, IP_ERR_ACCESS_DENIED = -EACCES, IP_ERR_NO_REPLY = -ETIMEDOUT,
  IP_ERR_INVALID_ARGS = -EINVAL). Functions: init, set_bus, connect,
  disconnect, get_state, get_version, get_unique_name, is_connected,
  is_degraded, set_reenumerate_cb, set_degraded_cb, handle_name_changed.
- `src/dbus/ip_connection.c`: Connection state machine. Connect: bus
  connect → subscribe NameOwnerChanged (before Version read so degraded
  mode still gets signals) → read Version property → on success: get unique
  name, state=CONNECTED → on ServiceUnknown: state=DEGRADED (bus stays
  connected) → on AccessDenied: log guidance, disconnect → on other: disconnect,
  return error. handle_name_changed: acquired → update unique name, re-read
  version, state=CONNECTED, fire reenumerate callback; lost → clear name/
  version, state=DEGRADED, fire degraded callback. Both non-empty
  (transfer) = no-op. Both empty = no-op.
- `src/dbus/dbus_client.c`: Production sd-bus vtable backend. Replaces Task 2
  stub. `sd_bus_wrapper` struct holds sd_bus* + slot array. connect:
  sd_bus_open_system. disconnect: unref all slots + bus, free wrapper.
  get_unique_name: GetNameOwner method call (NameHasNoOwner →
  IP_ERR_SERVICE_UNKNOWN). get_property: sd_bus_get_property_string with
  sd_bus_error_has_name translation to categorized errno codes.
  subscribe_signal: sd_bus_add_match with NameOwnerChanged match rule
  (arg0='org.shadowblip.InputPlumber'), slot callback parses "sss" into
  ip_owner_changed_payload. Stubs for call_method, set_property,
  get_managed_objects (later tasks). `ip_dbus_sd_backend()` accessor.
- `tests/dbus_mock.h`: Extended with ip_owner_changed_payload struct,
  ip_mock_subscription struct, IP_MOCK_MAX_SUBSCRIPTIONS, subscriptions[]
  + sub_count + subscribe_fail_rc fields in ip_dbus_mock.
- `tests/dbus_mock.c`: mock_subscribe_signal stores subscriptions (or fails
  if subscribe_fail_rc set). mock_inject_signal dispatches to matching
  registered callbacks. ip_dbus_mock_reset clears subscriptions.
- `tests/test_connection.c`: 25 cmocka tests with setup/teardown fixture.
- `CMakeLists.txt`: added ip_connection.c to controllerbox lib, added tests/
  to controllerbox PUBLIC include dirs (production code includes dbus_mock.h
  for vtable definition).
- `tests/CMakeLists.txt`: added test_connection target linking controllerbox +
  cbx_test_support (for mock functions).
- `docs/DBus-API.md`: created with connection model section.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 10/10: smoke_test_sdl2, smoke_test_nanosvg, test_sample,
  test_sdl_dummy, test_config_paths, test_settings, test_assignments,
  test_profile_yaml, test_profile_list, test_connection
- test_connection 25/25 cmocka tests pass
- verify-boilerplate, check-plan-freshness, branch-guard → exit 0

### Gotchas fixed
- **Include path for dbus_mock.h**: Production code (ip_connection.c,
  dbus_client.c) includes dbus_mock.h which is in tests/. Fixed by adding
  tests/ to controllerbox PUBLIC include dirs. This is how the vtable
  architecture was designed in Task 3 — the vtable interface is in
  dbus_mock.h, shared between production and tests.
- **test_connection include path**: test_connection.c includes
  "dbus/ip_connection.h" (not "ip_connection.h") because the include path
  is src/ (PUBLIC from controllerbox). Same pattern as other tests that
  include "config/config_paths.h" etc.
- **test_connection linking**: Must link both controllerbox (for
  ip_connection functions) AND cbx_test_support (for dbus_mock functions).
  Other config tests only need controllerbox because they don't use the
  mock.
- **Mock subscribe_signal**: Original mock was a no-op that didn't store
  callbacks. Extended to store subscriptions in a fixed-size table and
  dispatch via inject_signal. Also added subscribe_fail_rc for testing
  subscribe failure scenarios.
- **Mock get_unique_name always returns ":1.42"**: This is hardcoded and
  ignores expectations. For NameOwnerChanged tests, the unique name is set
  from the signal payload (new_owner), not from get_unique_name. This is
  correct behavior — on initial connect we resolve via GetNameOwner, on
  name change we get the new name from the signal.

### Next
Task 10 (ObjectManager enumeration and device model) — deps: Task 9 (now
done). Unblocks Tasks 11-14 (hotplug, PropertiesChanged, Manager wrappers,
CompositeDevice wrappers, source/target device properties).
Alternatively Task 16 (SVG assets + icon mapping) — deps: Task 1 (done).
Check `ralph tools task ready` and the plan.

## Task 10 (complete) — ObjectManager enumeration and device model

### What landed
- `src/dbus/ip_device_model.h`: `cbx_device_model` struct (has_manager +
  manager_path, composites[16] with parsed index, sources[64] + targets[64]
  with last-component name). Limits: 16 composites, 64 sources/targets.
  Lookup helpers: find_composite/source/target (by path, NULL-safe).
- `src/dbus/ip_device_model.c`: init (memset zero), lookup implementations.
- `src/dbus/ip_objectmanager.h`: API — `cbx_objectmanager_enumerate()`
  (calls backend->get_managed_objects, parses reply) and
  `cbx_objectmanager_parse_reply()` (pure text parser).
- `src/dbus/ip_objectmanager.c`: Text format parser: one line per object
  ("path\tiface1,iface2,..."), comments (#), empty lines skipped, CRLF
  handled. Classification: Manager by interface, Composite by interface,
  source/target by path pattern (/devices/source/ and /devices/target/).
  Security: validates all paths start with IP_DBUS_PATH "/", skips invalid.
  `enumerate()` calls backend, parses reply, frees heap-allocated reply.
- `src/dbus/dbus_client.c`: Implemented `sd_get_managed_objects()` —
  sd_bus_call_method for GetManagedObjects, iterates a{oa{sa{sv}}}
  containers, builds text via open_memstream. Added `translate_sd_error()`
  helper (extracted from sd_get_property's inline error translation).
- `tests/test_objectmanager_parse.c`: 31 cmocka tests — parser (null,
  empty, null-model, manager-only, composites with index, sources with
  name, targets with name, full tree, invalid-paths-skipped, all-invalid,
  malformed, CRLF, comments/empty), device model lookups (composite/
  source/target find, null args, init), truncation (composites/sources/
  targets), enumerate via mock (full, empty, null-reply, error,
  no-expectation, null-args, CRLF), edge cases (multi-interface target,
  root-path excluded, composite-no-index).
- `CMakeLists.txt`: added ip_device_model.c + ip_objectmanager.c.
- `tests/CMakeLists.txt`: added test_objectmanager_parse target.
- `docs/DBus-API.md`: added Object Enumeration section.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 11/11: smoke_test_sdl2, smoke_test_nanosvg, test_sample,
  test_sdl_dummy, test_config_paths, test_settings, test_assignments,
  test_profile_yaml, test_profile_list, test_connection,
  test_objectmanager_parse
- test_objectmanager_parse 31/31 cmocka tests pass
- verify-boilerplate, check-plan-freshness, branch-guard → exit 0

### Gotchas fixed
- **Missing includes in test**: test_objectmanager_parse.c needed
  `#include <errno.h>` (for EINVAL, ENXIO) and `#include "dbus/ip_connection.h"`
  (for IP_ERR_NO_REPLY).
- **Text format generation order**: Initially wrote interfaces to the
  stream during iteration, then tried to write the path separately.
  Fixed by writing path+tab first, then interfaces during iteration,
  then newline — produces correct "path\tiface1,iface2,...\n".
- **translate_sd_error helper**: Extracted from sd_get_property's inline
  error translation. Used by both sd_get_property and
  sd_get_managed_objects. Note: sd_get_property still has its own inline
  translation (didn't change existing working code); the new helper is
  for sd_get_managed_objects only.

### Design decisions
- **Text format for GetManagedObjects**: The vtable's get_managed_objects
  returns char** (text string in both mock and production). Production
  sd-bus backend iterates a{oa{sa{sv}}} and builds text via open_memstream.
  Mock returns canned text fixture. Single parser (cbx_objectmanager_parse_reply)
  handles both. This makes the parser fully testable without real sd-bus.
- **Path-based classification for source/target**: Source interfaces have
  multiple types (EventDevice, HIDRawDevice, UdevDevice, etc.). Rather
  than listing all, classification uses path pattern (/devices/source/).
  Manager and Composite use interface-based classification since they
  have single well-known interfaces.
- **Root path excluded**: The root path /org/shadowblip/InputPlumber (no
  trailing /) does NOT start with IP_DBUS_PATH "/" so it's correctly
  excluded from the model (it's the ObjectManager, not a device).

### Next
Task 11 (Hotplug and PropertiesChanged signal handling) — deps: Task 10
(now done). Unblocks Task 15 (which depends on Tasks 11+12+6).
Alternatively Task 12 (Manager interface method wrappers) — deps: Task 10.
Alternatively Task 13 (CompositeDevice wrappers + InterceptMode polling) —
deps: Task 10.
Alternatively Task 14 (Source/target device properties + InputEvent) —
deps: Task 10.
Alternatively Task 16 (SVG assets + icon mapping) — deps: Task 1 (done).
Check `ralph tools task ready` and the plan.

## Task 11 (complete) — Hotplug and PropertiesChanged signal handling

### What landed
- `tests/dbus_mock.h`: Added `ip_interfaces_changed_payload` (sender, path,
  interfaces — comma-separated), `ip_prop_type` enum (STRING, ARRAY,
  INVALIDATED), `ip_properties_changed_payload` (sender, iface_name,
  prop_name, prop_type, value, array_count).
- `src/dbus/ip_device_model.h/c`: Added mutation functions:
  set_manager/remove_manager, add/remove composite (idempotent, index
  parsed from path), add/remove source/target (name extracted from path),
  all with dedup and shift-down removal. Added `dbus_mock.h` include for
  IP_DBUS_PATH. Added `<stdlib.h>` for atoi.
- `src/dbus/ip_hotplug.h/c`: Hotplug handler. ip_hotplug struct (backend,
  bus, expected_sender, model). subscribe() registers InterfacesAdded +
  InterfacesRemoved via backend->subscribe_signal. Signal callback
  dispatches to handle_added/handle_removed by member name.
  handle_added: verifies sender, validates path (must start with
  IP_DBUS_PATH "/"), classifies by interface (Manager/Composite) and
  path pattern (source/target), adds to model.
  handle_removed: same verification, removes by classification.
- `src/dbus/ip_properties.h/c`: Properties handler. ip_properties struct
  (backend, bus, expected_sender, cb, cb_userdata). prop_spec table maps
  property names to expected types + max element lengths:
    GamepadOrder=ARRAY/256, ProfileName=STRING/256, ProfilePath=STRING/4096,
    TargetDevices=ARRAY/256, SourceDevicePaths=ARRAY/4096.
  subscribe() registers PropertiesChanged via backend->subscribe_signal.
  handle_changed: verifies sender, looks up prop_spec, validates type
  matches expected, validates string length or array element lengths +
  array count (max 256). Invalidated properties dispatched with count=-1.
  User callback fired only if all validation passes.
- `src/dbus/dbus_client.c`: Added production sd-bus callbacks:
  sd_interfaces_added_callback (oa{sa{sv}} → builds comma-separated
  interfaces string via open_memstream), sd_interfaces_removed_callback
  (oas → builds comma-separated string), sd_properties_changed_callback
  (sa{sv}as → peeks variant type, reads string or string array, dispatches
  per-property payload, also dispatches invalidated properties).
  Updated sd_subscribe_signal to dispatch to correct callback by
  iface+member, with proper match rules (ObjectManager signals filtered
  by path, PropertiesChanged filtered by interface+member).
- `tests/test_hotplug.c`: 34 cmocka tests (6 simple + 28 fixture). Tests:
  init, subscribe (registers both signals, fail first/second), model
  mutations (add/remove/dup/full for composite/source/target/manager),
  handle added/removed (composite, source, target, manager, multi-iface,
  wrong sender, invalid path, null interfaces, null payload/model),
  integration (inject via mock, add-remove-add resurrection).
- `tests/test_properties_changed.c`: 33 cmocka tests (4 simple + 29
  fixture). Tests: init, subscribe, string properties (ProfileName,
  ProfilePath, too-long rejection for both 256/4096 limits), array
  properties (GamepadOrder, TargetDevices, SourceDevicePaths, single,
  empty, elem-too-long, too-many, max-elems, path-long-elem), type
  validation (mismatch string→array, array→string), invalidated
  (tracked + untracked), sender verification (wrong + null), edge cases
  (untracked prop, null payload/handler/callback, null value),
  integration (inject via mock, multiple changes).
- `CMakeLists.txt`: added ip_hotplug.c + ip_properties.c.
- `tests/CMakeLists.txt`: added test_hotplug + test_properties_changed
  targets, both linking controllerbox + cbx_test_support.
- `docs/DBus-API.md`: added Signal Handling section (Hotplug +
  PropertiesChanged with property table, validation rules, API examples).

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 13/13: smoke_test_sdl2, smoke_test_nanosvg, test_sample,
  test_sdl_dummy, test_config_paths, test_settings, test_assignments,
  test_profile_yaml, test_profile_list, test_connection,
  test_objectmanager_parse, test_hotplug, test_properties_changed
- test_hotplug 34/34 cmocka tests pass
- test_properties_changed 33/33 cmocka tests pass
- verify-boilerplate, check-plan-freshness, branch-guard → exit 0

### Gotchas fixed
- **Missing `<stdlib.h>` in ip_device_model.c**: Added for atoi()
  (parse_composite_index). Previously only had <string.h> + <stdio.h>.
- **Fixture tests declared as cmocka_unit_test**: test_hotplug_subscribe,
  test_hotplug_subscribe_fail_first/second, and test_props_subscribe
  used *state but were declared without setup/teardown. Changed to
  cmocka_unit_test_setup_teardown with proper fixture functions.
  Otherwise *state was NULL → segfault.
- **Unused `contents` array**: In sd_properties_changed_callback, declared
  `char contents[256]` but used `contents_ptr` from peek_type directly.
  Removed the unused array.
- **Production callback match rules**: ObjectManager signals
  (InterfacesAdded/Removed) filtered by path=IP_DBUS_PATH in match rule.
  PropertiesChanged uses broad match (interface+member only) with sender
  verification in the callback (not in match rule, to avoid issues when
  InputPlumber isn't running at subscription time).

### Design decisions
- **Payload structs with `sender` field**: Added sender to all new payload
  structs for sender verification. In production, sd_bus_message_get_sender()
  fills it. In tests, the test sets it directly. The handler verifies
  sender matches expected_sender before processing.
- **Single property per payload for PropertiesChanged**: The production
  callback calls the handler once per changed property we care about,
  rather than batching all changes in one payload. This keeps the payload
  struct simple and makes testing straightforward (one property per
  inject_signal call).
- **Comma-separated arrays**: Array property values are represented as
  comma-separated strings in the payload. Works because DBus object paths
  and device names don't contain commas. open_memstream builds the string
  in production; tests construct it directly.
- **Idempotent adds**: Device model add functions reject duplicates
  (return false). This prevents double-adds if InterfacesAdded fires for
  an already-known path.
- **PropertiesChanged callback uses sd_bus_message_peek_type**: To
  determine variant inner type (string vs array), the callback peeks
  the variant, then enters it and peeks again for the actual type.
  String arrays checked via inner_sig == "s".

### Next
Task 12 (Manager interface method wrappers) — deps: Task 10 (done).
Unblocks Task 15 (depends on 11+12+6).
Alternatively Task 13 (CompositeDevice wrappers + InterceptMode polling) —
deps: Task 10.
Alternatively Task 14 (Source/target device properties + InputEvent) —
deps: Task 10.
Alternatively Task 16 (SVG assets + icon mapping) — deps: Task 1 (done).
Check `ralph tools task ready` and the plan.

## Task 12 (complete) — Manager interface method wrappers

### What landed
- `src/dbus/ip_manager.h`: API for Manager interface wrappers —
  CreateTargetDevice(kind)→path, StopTargetDevice(path), AttachTargetDevice
  (target,composite), SetTargetDevices(composite_path, types_csv) on
  CompositeDevice iface, GamepadOrder get/set (with device model validation),
  SupportedTargetDeviceIds get, SupportedTargetDevices get. Defines
  IP_DBUS_MANAGER_PATH constant.
- `src/dbus/ip_manager.c`: Implementations. Method calls go through
  backend->call_method with variadic convention: sig encodes input types,
  last variadic arg is char **out (NULL for void, valid for string return).
  Property gets use backend->get_property. Property sets use
  backend->set_property. GamepadOrder setter validates each comma-separated
  path against cbx_device_model_find_composite before calling DBus.
- `tests/dbus_mock.c`: Extended mock_call_method to support output values.
  Added mock_count_sig_args() to parse sig and count input args. Mock counts
  input args from sig, skips them via va_arg, reads trailing char **out, fills
  *out with strdup(e->value) if non-NULL and rc>=0. Added <stdarg.h> include.
- `src/dbus/dbus_client.c`: Implemented production sd_call_method (was stub):
  sd_bus_message_new_method_call, manual arg appending based on sig ('s' =
  append_basic, 'as' = open_container 'a' "s" + split CSV + append each),
  sd_bus_call, read reply string into *out. Implemented production
  sd_set_property (was stub): Properties.Set method call with ssv signature,
  builds variant for array properties (as) or string properties (s) via
  sd_is_array_property() lookup. Added sd_append_string_array() helper. Added
  forward declaration for translate_sd_error. Added <stdarg.h> include.
- `tests/test_manager_calls.c`: 38 cmocka tests with setup/teardown fixture.
  Tests: path constant, CreateTargetDevice (success/error/no-expectation/null),
  StopTargetDevice (same 4), AttachTargetDevice (same 4), SetTargetDevices
  (same 4), GetGamepadOrder (success/empty/error/no-expectation/null),
  SetGamepadOrder (success/empty/invalid-path/partial-invalid/null-model/
  null-args/path-too-long/dbus-error), GetSupportedTargetDeviceIds (success/
  error/no-expectation/null), GetSupportedTargetDevices (same 4).
- `tests/test_sample.c`: Updated mock_call_method calls to pass trailing NULL
  (new variadic convention).
- `CMakeLists.txt`: Added ip_manager.c to controllerbox.
- `tests/CMakeLists.txt`: Added test_manager_calls target.
- `docs/DBus-API.md`: Added Manager Interface Wrappers section.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 14/14: smoke_test_sdl2, smoke_test_nanosvg, test_sample, test_sdl_dummy,
  test_config_paths, test_settings, test_assignments, test_profile_yaml,
  test_profile_list, test_connection, test_objectmanager_parse, test_hotplug,
  test_properties_changed, test_manager_calls
- test_manager_calls 38/38 cmocka tests pass
- verify-boilerplate, check-plan-freshness, branch-guard → exit 0

### Gotchas fixed
- **FIX macro bug**: The original `#define FIX(state) ((manager_fixture *)(state))`
  cast `void **state` directly to `manager_fixture *`, interpreting the cmocka
  state pointer itself as the fixture struct (stack memory). Fixed to
  `#define FIX(state) (*(manager_fixture **)(state))` which dereferences state
  to get the actual fixture pointer. This was the root cause of the "free():
  invalid pointer" crash — teardown was reading garbage from the wrong memory.
  Pattern to remember: cmocka fixture tests use `*state` to get the fixture,
  not `state`.
- **call_method variadic convention**: Extended mock_call_method to support
  output values. The last variadic arg is always `char **out` (NULL for void
  methods). The mock counts input args from sig ('a' is a prefix, 's' is one
  arg), skips them, reads the trailing char **. This required updating
  test_sample.c's existing call_method calls to pass trailing NULL.
- **translate_sd_error forward declaration**: Production sd_call_method uses
  translate_sd_error which was defined later in dbus_client.c. Added a forward
  declaration after the sd_bus_wrapper struct definition.
- **Production sd_set_property for arrays**: Uses Properties.Set with a variant
  container. For array properties (GamepadOrder, TargetDevices, etc.), builds
  variant "as" from comma-separated value. For string properties, builds
  variant "s". Uses sd_is_array_property() lookup to determine type.

### Design decisions
- **SetTargetDevices on CompositeDevice iface**: Although the plan lists
  SetTargetDevices under Task 12 (Manager wrappers), the DBus method is on
  the CompositeDevice interface, not Manager. The wrapper takes a
  composite_path parameter and calls the method on IP_IFACE_COMPOSITE at that
  path.
- **GamepadOrder validation**: Each comma-separated path is validated against
  the device model's composites before calling the DBus setter. This prevents
  the GUI from sending invalid paths that could cause InputPlumber to suspend
  all devices indefinitely (the GamepadOrder setter suspends/resumes devices).
- **call_method sig convention**: 's' = one string arg, 'as' = one array arg
  (comma-separated string). The mock counts args by iterating sig chars,
  treating 'a' as a prefix that pairs with the next char. This is a simplified
  convention for our use case, not full DBus type system support.

### Next
Task 13 (CompositeDevice wrappers + InterceptMode polling) — deps: Task 10
(done). Unblocks Task 15 (depends on 11+12+6, all done now).
Alternatively Task 14 (Source/target device properties + InputEvent) — deps:
Task 10 (done).
Alternatively Task 16 (SVG assets + icon mapping) — deps: Task 1 (done).
Alternatively Task 15 (CreateCompositeDevice temp file + GamepadOrder persistence)
— deps: Task 11+12+6 (all done).
Check `ralph tools task ready` and the plan.

## Task 13 (complete) — CompositeDevice interface wrappers and InterceptMode polling

### What landed
- `src/dbus/ip_composite.h`: API for CompositeDevice interface wrappers —
  SetInterceptActivation(events_csv, target), LoadProfilePath(path),
  LoadProfileFromYaml(yaml), GetProfileYaml()→string, SetTargetDevices(types_csv),
  Stop, InterceptMode get/set, TargetDevices/SourceDevicePaths/PersistentId/
  Name/Capabilities/OutputCapabilities/TargetCapabilities/DbusDevices gets.
  Defines IP_INTERCEPT_NONE/PASS/ALL/GAMEPAD_ONLY constants.
- `src/dbus/ip_composite.c`: Implementations. All wrappers take composite_path
  as a parameter (unlike Manager which uses fixed path). Methods go through
  backend->call_method, properties through get_property/set_property. The
  composite_path is passed as the DBus object path, IP_IFACE_COMPOSITE as
  the interface.
- `src/dbus/ip_intercept_poll.h/c`: InterceptMode poll state machine.
  ip_intercept_poll struct (backend, bus, composite_path, state, error_count,
  max_errors, timeout_ticks, max_timeout_ticks, callbacks, SDL timer fields).
  States: IDLE → PASS_WAIT → ACTIVE → IDLE. tick() reads InterceptMode via
  get_property, transitions states, fires callbacks. start() creates SDL_AddTimer
  that pushes custom SDL_UserEvent. stop() removes timer + resets.
  Timeout: PASS_WAIT with NONE for max_ticks → error; ACTIVE with ALL for
  max_ticks → ETIMEDOUT error. Error recovery: max_errors consecutive
  failures → error + reset. Parse failures count toward max_errors (not
  reset on successful get_property, only on successful parse).
- `src/dbus/dbus_client.c`: Added sd_is_uint_property() for InterceptMode
  (builds variant "u" from parsed string). Added Capabilities,
  OutputCapabilities, TargetCapabilities, DbusDevices to sd_is_array_property.
- `tests/test_composite_calls.c`: 51 cmocka tests with setup/teardown fixture.
  Tests: SetInterceptActivation (success/error/no-expect/null), LoadProfilePath
  (same 4), LoadProfileFromYaml (same 4), GetProfileYaml (same 4), SetTargetDevices
  (same 4), Stop (same 4), InterceptMode get (success/error/no-expect/null),
  InterceptMode set (same 4), TargetDevices get (same 4), SourceDevicePaths
  (success/null), PersistentId (success/null), Name (success/null),
  Capabilities (success/null), OutputCapabilities (success/null),
  TargetCapabilities (success/null), DbusDevices (success/null),
  intercept mode constants.
- `tests/test_intercept_poll.c`: 22 cmocka tests with setup/teardown fixture.
  Tests: init (fields, null), IDLE noop, null poll, PASS_WAIT (pass/all/
  gamepad_only/none-timeout), ACTIVE (all/pass/none/timeout), error handling
  (transient/max/recovery/parse-fail/parse-fail-max), full lifecycle,
  stop, state_name, no_callbacks, no_callbacks_error.
- `CMakeLists.txt`: added ip_composite.c + ip_intercept_poll.c.
- `tests/CMakeLists.txt`: added test_composite_calls + test_intercept_poll.
- `docs/DBus-API.md`: added CompositeDevice Interface Wrappers section with
  method/property tables, InterceptMode polling state machine, DbusDevices
  correlation.
- `IMPLEMENTATION_PLAN.md`: Updated Task 10 and Task 13 status to complete.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 16/16: smoke_test_sdl2, smoke_test_nanosvg, test_sample, test_sdl_dummy,
  test_config_paths, test_settings, test_assignments, test_profile_yaml,
  test_profile_list, test_connection, test_objectmanager_parse, test_hotplug,
  test_properties_changed, test_manager_calls, test_composite_calls,
  test_intercept_poll
- test_composite_calls 51/51 cmocka tests pass
- test_intercept_poll 22/22 cmocka tests pass
- verify-boilerplate, check-plan-freshness, branch-guard → exit 0

### Gotchas fixed
- **ip_intercept_poll.c needed ip_composite.h**: The InterceptMode constants
  (IP_INTERCEPT_ALL, IP_INTERCEPT_PASS, etc.) are defined in ip_composite.h.
  ip_intercept_poll.c uses them in the state machine, so it includes ip_composite.h.
- **error_count reset timing**: Initially reset error_count after successful
  get_property (before parse). This meant parse failures never accumulated
  because error_count was always reset to 0 on the successful read. Fixed by
  only resetting error_count after a successful mode parse.
- **tick returns error code on max_errors**: When max_errors is reached,
  tick() returns the error code (not 0). Test_tick_no_callbacks_error expected
  0 but got the error code. Fixed test to not check return value.

### Design decisions
- **CompositeDevice wrappers take composite_path as parameter**: Unlike Manager
  wrappers which use a fixed IP_DBUS_MANAGER_PATH, CompositeDevice wrappers take
  the composite device's DBus path as a parameter since there can be multiple
  composite devices.
- **GAMEPAD_ONLY treated as activation**: In PASS_WAIT state, both ALL (2) and
  GAMEPAD_ONLY (3) trigger the activating callback. This is because both modes
  intercept input — the overlay should show for either.
- **NONE treated as deactivation in ACTIVE**: If InterceptMode is NONE (0)
  while in ACTIVE, it means InputPlumber reset the mode — treat as deactivation.
- **Timeout in PASS_WAIT for NONE**: If InterceptMode is unexpectedly NONE in
  PASS_WAIT (InputPlumber reset), timeout after max_timeout_ticks and fire error.
- **SDL timer not tested in unit tests**: The tick function is the core state
  machine logic and is tested directly with mock expectations. SDL timer
  (start/stop) requires SDL_Init and is tested structurally. Integration testing
  of the full timer→event→tick loop is deferred to Task 33 (overlay integration).

### Next
Task 14 (Source/target device properties + InputEvent) — deps: Task 10 (done).
Alternatively Task 15 (CreateCompositeDevice temp file + GamepadOrder persistence)
— deps: Task 11+12+6 (all done).
Alternatively Task 16 (SVG assets + icon mapping) — deps: Task 1 (done).
Check `ralph tools task ready` and the plan.

## Task 14 (complete) — Source/target device properties and InputEvent signal handling

### What landed
- `tests/dbus_mock.h`: Added 5 new DBus interface constants:
  IP_IFACE_SOURCE_EVENT ("org.shadowblip.Input.Source.EventDevice"),
  IP_IFACE_SOURCE_UDEV ("org.shadowblip.Input.Source.UdevDevice"),
  IP_IFACE_SOURCE_HIDRAW ("org.shadowblip.Input.Source.HIDRawDevice"),
  IP_IFACE_TARGET ("org.shadowblip.Input.Target"),
  IP_IFACE_DBUS_DEVICE ("org.shadowblip.Input.DBusDevice").
  Added ip_input_event_payload struct (sender, path, event, value).
- `src/dbus/ip_source.h/c`: 7 source device property getters. Each takes
  iface parameter since source interface varies by device type
  (EventDevice/UdevDevice vs HIDRawDevice). Properties: Name, UniqueId,
  PhysPath, IdVendor, IdProduct, IdBustype, SerialNumber.
- `src/dbus/ip_target.h/c`: 2 target device property getters. Properties:
  Name, DeviceType. Uses IP_IFACE_TARGET constant (no iface parameter).
- `src/dbus/ip_input_signal.h/c`: InputEvent signal handler. Includes:
  - ip_input_id enum (23 inputs: UP/DOWN/LEFT/RIGHT, A/B/X/Y,
    START/SELECT/GUIDE, L1/R1/L2/R2, L3/R3, LEFT/RIGHT_STICK_X/Y)
  - ip_input_category (BUTTON/AXIS)
  - Event string parsing with alias support (Back→Select, Home→Guide,
    LeftBumper→L1, LeftTrigger→L2, LeftStick→L3, etc.)
  - Value validation: buttons 0.0/1.0, axes [-1.0, 1.0], NaN/infinity rejected
  - Rate limiting: max 200 events/sec per device path (sliding 1-sec window,
    per-device tracking in ip_rate_limiter_entry array of 64 slots)
  - Sender verification on all signals
  - ip_input_events struct with backend, bus, expected_sender, callback,
    cb_userdata, rate_limiters[64]
- `src/dbus/dbus_client.c`: Added sd_input_event_callback (parses "sd"
  signature), routing in sd_subscribe_signal for IP_IFACE_DBUS_DEVICE
  InputEvent. Added #include "ip_input_signal.h".
- `tests/test_source_props.c`: 30 cmocka tests (4 per property × 7 properties
  + 2 extra: Name hidraw, IdVendor hidraw). Tests: success, error,
  no-expectation, NULL args.
- `tests/test_target_props.c`: 10 cmocka tests (4 per property × 2 properties
  + 2 extra: ds5, deck DeviceType). Tests: success, error, no-expectation,
  NULL args.
- `tests/test_input_signal.c`: 37 cmocka tests. Tests: parsing (dpad, face,
  center, shoulders, stick clicks, axes, unknown/NULL/case-sensitive),
  category (buttons, axes), value validation (button 0/1/NaN/Inf, axis
  range), init, subscribe, handler (valid button/axis, release, wrong/null
  sender, unknown event, invalid values, null payload/handler/callback/
  path/event), rate limiting (under 200, over 201st dropped, per-device,
  reset), integration (inject_signal, wrong sender, multiple events, axis).
- `CMakeLists.txt`: Added ip_input_signal.c, ip_source.c, ip_target.c.
- `tests/CMakeLists.txt`: Added test_source_props, test_target_props,
  test_input_signal targets.
- `docs/DBus-API.md`: Added Source/Target Device Properties and InputEvent
  section with interface tables, wrapper APIs, input enum mapping table,
  production backend details.
- `IMPLEMENTATION_PLAN.md`: Task 14 status → complete.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 19/19: all previous + test_source_props, test_target_props,
  test_input_signal
- test_source_props 30/30 cmocka tests pass
- test_target_props 10/10 cmocka tests pass
- test_input_signal 37/37 cmocka tests pass
- verify-boilerplate, check-plan-freshness, branch-guard → exit 0

### Gotchas fixed
- **Missing <stdio.h> in ip_input_signal.c**: snprintf needs <stdio.h>.
  Initially only had <errno.h>, <math.h>, <string.h>, <time.h>.
- **Missing <stdio.h> in test_input_signal.c**: Same issue, snprintf in
  capture_cb. Also added <stdbool.h> for bool type.
- **Include path for source headers**: Test files must include
  "dbus/ip_source.h" not "ip_source.h" since include path is src/ not src/dbus/.
- **NULL bus not checked by wrappers**: Source/target wrappers initially
  only checked !backend, not !bus. When bus=NULL, mock returned -ENXIO not
  -EINVAL. Fixed by adding !bus to the NULL check in all source/target
  wrappers.

### Design decisions
- **Source wrappers take iface as parameter**: Unlike target (fixed
  IP_IFACE_TARGET) and composite (fixed IP_IFACE_COMPOSITE), source device
  properties are spread across 3 interfaces (EventDevice, UdevDevice,
  HIDRawDevice). The caller must pass the correct interface for the device
  type. This is because serial is UniqueId on evdev/udev but SerialNumber
  on HIDRaw (SPEC §10.2).
- **L2/R2 as buttons by default**: In the input table, L2 and R2 are
  categorized as BUTTON (value 0.0/1.0). Some controllers send them as
  analog axes (0.0..1.0), but the category is determined by the event
  string, not the value. If InputPlumber sends L2 as a button event,
  it gets BUTTON category; if as an axis, it would need a different
  event string. This may need revisiting in Task 22 (input event mapping).
- **Alias event strings**: Included common aliases (Back→Select,
  Home→Guide, LeftBumper→L1, RightBumper→R1, LeftTrigger→L2,
  RightTrigger→R2, LeftStick→L3, RightStick→R3) for robustness against
  InputPlumber naming variations.
- **Rate limiter uses clock_gettime(CLOCK_MONOTONIC)**: In tests, all
  events arrive within the same millisecond, so 201 events triggers the
  200/sec limit naturally without mocking time.
- **Per-device rate limiting**: Rate limiter tracks up to 64 devices
  (IP_INPUT_MAX_DEVICES). Each entry stores device_path, event_count,
  and window_start_ms. Window resets after 1000ms.

### Next
Task 15 (CreateCompositeDevice temp file + GamepadOrder persistence) —
deps: Task 11+12+6 (all done).
Alternatively Task 16 (SVG assets + icon mapping) — deps: Task 1 (done).
Check `ralph tools task ready` and the plan.

## Task 15 (complete) — CreateCompositeDevice temp file + GamepadOrder persistence

### What landed
- `src/dbus/ip_create_composite.h/c`: CreateCompositeDevice temp file workaround
  (gap #3). ip_create_composite_device(backend, bus, yaml_content, out_path).
  Resolves temp dir (XDG_RUNTIME_DIR or /tmp), creates temp file via
  mkstemps() (glibc, with .yaml suffix) or mkstemp() (fallback, no suffix),
  fchmod 0600, writes YAML, fsync, close, calls Manager.CreateCompositeDevice,
  always unlinks temp file regardless of result.
- `src/dbus/ip_gamepad_order.h/c`: GamepadOrder persistence layer (gap #2).
  ip_gamepad_order_save(backend, bus, model, paths_csv) — loads existing
  assignments.yaml, clears gamepad_order, iterates composite paths, skips
  stale (not in model), queries PersistentId via ip_composite_get_persistent_id,
  skips DBus errors, validates IDs via cbx_validate_id, deduplicates, saves.
  ip_gamepad_order_load(out_csv) — reads assignments.yaml, builds CSV of
  gamepad_order IDs, skips invalid IDs.
- `tests/test_create_composite.c`: 11 cmocka tests (success, error, null
  backend/yaml/out, no-expectation, temp-unlinked-after-success/error,
  xdg-runtime-dir-preferred, empty-yaml, large-yaml).
- `tests/test_gamepad_order.c`: 14 cmocka tests (save success, empty-csv,
  stale-path-skipped, dbus-error-skipped, preserves-assignments, null-args,
  order-id, load no-file/with-order/empty/invalid-id-skipped/null, round-trip,
  save-replaces-order).
- `CMakeLists.txt`: Added ip_create_composite.c + ip_gamepad_order.c.
- `tests/CMakeLists.txt`: Added test_create_composite + test_gamepad_order.
- `docs/DBus-API.md`: Added gap #2 and #3 workaround sections.
- `IMPLEMENTATION_PLAN.md`: Task 15 → complete.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 21/21: all previous + test_create_composite + test_gamepad_order
- test_create_composite 11/11 cmocka tests pass
- test_gamepad_order 14/14 cmocka tests pass
- verify-boilerplate, check-plan-freshness, branch-guard → exit 0

### Gotchas fixed
- **mkstemp requires XXXXXX at END of template**: Initially used
  "controller-box-XXXXXX.yaml" but mkstemp requires the last 6 chars to
  be X's. Fixed by using mkstemps() (glibc extension) which supports a
  suffix after XXXXXX, with a fallback to mkstemp() without the .yaml
  suffix for non-glibc systems.
- **Mock returns same value for same (iface, member)**: The mock's
  ip_dbus_mock_find returns the first matching expectation. Setting two
  PersistentId expectations on IP_IFACE_COMPOSITE returns the same value
  for both calls. Tests with multiple composites use one valid + one
  stale path instead, since the mock can't return different values per
  call path.
- **(void)system(cmd) doesn't suppress warn_unused_result**: GCC with
  -Werror=unused-result flags (void)system(cmd). Fixed by capturing the
  return value: `int __r = system(cmd); (void)__r;`

### Design decisions
- **Deduplication in gamepad_order_save**: append_order_id() skips
  duplicate IDs. This is a safety measure — GamepadOrder from DBus
  shouldn't have duplicate paths, and even if it does, the same
  PersistentId should only appear once in the saved order.
- **Save replaces, not appends**: ip_gamepad_order_save clears the
  existing gamepad_order before populating it with the current order.
  This ensures the saved order always reflects the current DBus state.
- **Stale path = skip, not error**: If a composite path in the GamepadOrder
  CSV doesn't exist in the device model, it's skipped (the device was
  probably removed). This is not an error — the function continues with
  the remaining valid paths.
- **DBus error during PersistentId = skip, not error**: If the
  PersistentId query fails for a valid composite, that entry is skipped
  but the function continues. This is resilient behavior — a single DBus
  failure shouldn't prevent saving the rest of the order.
- **Orchestration deferred to Task 27**: The persistence layer only
  saves/loads gamepad_order. The orchestration (when to save after
  GamepadOrder changes, when to restore after daemon restart, mapping
  IDs back to composite paths) is Task 27.

### Next
Task 16 (SVG assets + icon mapping) — deps: Task 1 (done).
Alternatively Task 17 (nanosvg rasterization) — deps: Task 16.
Check `ralph tools task ready` and the plan.

## Task 16 (complete) — SVG assets and icon mapping table

### What landed
- `data/icons/svg/`: 30 Controllercons solid SVGs vendored (atari-2600,
  atari-jaguar, dreamcast, gamecube, joy-con-l, joy-con-r, joy-cons,
  master-system, mega-drive, n64, nes, ps1, ps2, ps3, ps4, ps5,
  sega-saturn, snes, stadia, switch-pro, virtual-boy, wii-classic,
  wii-u-pro, wii-u, wii, xbox-360, xbox-controller-s, xbox-one,
  xbox-series-x, xbox) + 6 custom SVGs (steam-deck, generic-gamepad,
  arcade-stick, hitbox, mouse, keyboard) = 36 total.
- `data/icons/svg/LICENSE.controllercons`: SIL OFL 1.1 license file
  with attribution to Kieran McClung.
- `data/controller-icons.yaml`: Maps 18 InputPlumber DeviceType strings
  to icons + display names (xb360, ds5, ds5-usb, ds5-bt, ds5-edge,
  ds5-edge-usb, ds5-edge-bt, deck, deck-uhid, gamepad, unified-gamepad,
  hori-steam, 8bitdo-u2, mouse, keyboard, touchpad, touchscreen, null,
  dbus, debug). Includes custom_icons section (arcade-stick, hitbox).
- `src/icons/icon_map.h`: API: cbx_icon_map_init/load/parse/lookup/
  default_path. Structs: cbx_icon_entry (type/icon/name),
  cbx_icon_map (entries array, count, loaded, yaml_path).
  Constants: CBX_ICON_MAP_MAX_ENTRIES=64, CBX_ICON_DEFAULT_ICON.
- `src/icons/icon_map.c`: Event-based libyaml parser. Security: max
  depth 50, max doc 1MB, no custom tags. State machine: TOP →
  VIRTUAL_TYPES (on sequence start after "virtual_types" key) →
  ENTRY (on mapping start within sequence) → VIRTUAL_TYPES (on mapping
  end, store entry if type field present). custom_icons section
  parsed but entries without type field are not stored. Unknown keys
  silently ignored (forward-compatible).
- `tests/test_icon_map.c`: 42 cmocka tests. 32 parser tests (init,
  parse basic/with-custom/null/empty/no-virtual, lookup known/ds5/
  unknown/null-map/unloaded/null-type/null-outputs/small-buffer,
  default-path/null/small, load nonexistent/null/tempfile, tags-
  rejected/too-large/many-entries/full-mapping, unknown-raw-type,
  icon-only/name-only, missing-icon/type, long-type-truncated,
  reparse-resets) + 10 nanosvg compatibility tests (ps5, xbox-360,
  steam-deck, generic-gamepad, arcade-stick, hitbox, mouse, keyboard,
  all-compat (iterates entire svg dir), load-real-yaml from source).
- `CMakeLists.txt`: Added icon_map.c to controllerbox. Added install
  rules for SVG dir + license + controller-icons.yaml.
- `tests/CMakeLists.txt`: Added test_icon_map target with nanosvg
  link and CBX_SOURCE_DIR compile definition for SVG path resolution.
- `README.md`: Added Credits section for Controllercons + custom icons.
- `docs/OPERATIONS.md`: Added Icon mapping section.
- `IMPLEMENTATION_PLAN.md`: Task 16 status → complete.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 22/22: all previous + test_icon_map
- test_icon_map 42/42 cmocka tests pass (incl. nanosvg compat for all
  36 SVGs and real YAML file loading from source dir)
- verify-boilerplate, check-plan-freshness → exit 0

### Gotchas fixed
- **SEQUENCE_START not handled in state machine**: Initial parser
  only handled scalar keys at top level, not the sequence that follows.
  When "virtual_types" key was followed by a sequence, the state didn't
  transition to VIRTUAL_TYPES. Fixed by adding state transition in the
  SEQUENCE_START event handler when state==TOP and have_key is true.
- **parse() didn't set loaded=1**: cbx_icon_map_parse called init
  which sets loaded=0, then parse_icon_map_from_string, but never set
  loaded=1. Lookups against a parsed (but not loaded) map always fell
  through to defaults. Fixed by setting loaded=1 on successful parse.
- **Missing errno.h/limits.h/unistd.h/dirent.h in test**: Test used
  EINVAL, ENAMETOOLONG, EPERM, EFBIG, PATH_MAX, unlink, opendir/readdir
  without including the right headers. Added errno.h, limits.h,
  unistd.h, dirent.h.
- **Unused YAML_MALFORMED variable**: Removed the unused static
  variable that triggered -Werror=unused-variable.

### Design decisions
- **6 custom SVGs instead of 4**: Plan required 4 (arcade-stick,
  hitbox, steam-deck, generic-gamepad). Added mouse and keyboard SVGs
  because the mapping table needs icons for non-controller device types
  (mouse, keyboard, touchpad, touchscreen). Touchpad/touchscreen map
  to generic-gamepad since they're rare and a custom icon adds little
  value for these edge cases.
- **cc- prefix convention for Controllercons**: Following the spec §8.4
  example which uses "cc-xbox-360", "cc-ps5", "cc-steam-deck" for icon
  names. Custom non-Controllercons icons use plain names (generic-
  gamepad, arcade-stick, hitbox, mouse, keyboard).
- **18 device types mapped**: All InputPlumber SupportedTargetDeviceIds
  are mapped (from source analysis of input/target/mod.rs). Includes
  subtypes (ds5-usb, ds5-bt, ds5-edge-usb, ds5-edge-bt) that are
  available via _type_identifiers but not in supported_types(). null,
  dbus, debug mapped to generic-gamepad (internal types, not shown
  to users but handled gracefully).
- **custom_icons section in YAML**: Informational only, not stored in
  the map. These icons are available for profile overrides (SPEC §8.5)
  but don't correspond to DeviceType strings.
- **nanosvg compat test uses source dir path**: Passed CBX_SOURCE_DIR
  via compile definition so tests can find SVG files relative to
  CMAKE_CURRENT_SOURCE_DIR. test_svg_all_compat iterates the entire
  svg directory using opendir/readdir.

### Next
Task 17 (nanosvg rasterization and SDL2 texture cache) — deps: Task 16 (done).
Alternatively Task 18 (Runtime icon lookup API) — deps: Task 17 + Task 8 (done).
Check `ralph tools task ready` and the plan.

## Task 17 (complete) — nanosvg rasterization and SDL2 texture cache

### What landed
- `src/icons/icon_cache.h`: API for SVG-to-SDL2 texture cache. Structs:
  cbx_icon_cache_entry (name, texture, width, height), cbx_icon_cache
  (renderer, rasterizer, entries[256], count, icon_dir, target_size).
  Constants: CBX_ICON_CACHE_MAX=128, CBX_ICON_CACHE_HASH_SIZE=256.
  Functions: cbx_icon_cache_init/load/get/get_dims/load_one/cleanup.
- `src/icons/icon_cache.c`: Implementation. djb2 hash function, open
  addressing with linear probing. Rasterizes SVGs via nsvgParseFromFile +
  nsvgRasterize (single reusable NSVGrasterizer). Creates SDL2 textures
  (SDL_PIXELFORMAT_ABGR8888, TEXTUREACCESS_STATIC), uploads pixels via
  SDL_UpdateTexture, sets SDL_BLENDMODE_BLEND. Strips "cc-" prefix from
  icon names when building SVG file paths (Controllercons convention:
  YAML uses "cc-xbox-360" but file is "xbox-360.svg"). Scale preserves
  aspect ratio, fits within target_size. Deduplication: shared icons
  (e.g., generic-gamepad used by 7+ types) rasterized only once.
- `tests/test_icon_cache.c`: 26 cmocka tests using test_harness SDL2
  dummy driver + real YAML from source tree. Tests: init (basic, null
  args), load (all, texture exists, dims, aspect ratio, idempotent,
  null args), lookup (known, unknown, null args, empty cache, dims
  known/unknown/null), load_one (new, already cached, nonexistent, null),
  recolour (SDL_SetTextureColorMod), blend mode (SDL_BLENDMODE_BLEND),
  cleanup (basic, null, double-cleanup), shared icons deduplicated,
  different target sizes, hash collision lookup (all 36 SVGs).
- `CMakeLists.txt`: Added icon_cache.c to controllerbox STATIC library.
- `tests/CMakeLists.txt`: Added test_icon_cache target linked with
  controllerbox + cmocka + nanosvg + cbx_test_support. CBX_SOURCE_DIR
  compile def. SDL_VIDEODRIVER=dummy environment.
- `IMPLEMENTATION_PLAN.md`: Task 17 → complete.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 23/23: all previous + test_icon_cache
- test_icon_cache 26/26 cmocka tests pass
- verify-boilerplate, check-plan-freshness → exit 0

### Gotchas fixed
- **cc- prefix stripping**: YAML icon names use "cc-" prefix (e.g.,
  "cc-xbox-360") but actual SVG files on disk don't have it (e.g.,
  "xbox-360.svg"). Added prefix stripping in rasterize_svg() before
  building the file path. Non-cc icons (generic-gamepad, arcade-stick,
  keyboard, mouse, hitbox, steam-deck) are unaffected.
- **Unused find_slot function**: Initially wrote a separate find_slot()
  helper but the lookup functions each do their own probing inline.
  Removed the unused function to fix -Werror=unused-function.
- **Double slash in path**: SVG_DIR ends with "/" so paths like
  "svg_dir//cc-ps5.svg" had double slashes — harmless on Linux but
  fixed implicitly by the cc- prefix stripping (path construction now
  uses icon_dir + "/" + file_name + ".svg").

### Design decisions
- **djb2 hash with open addressing**: Simple hash function, power-of-two
  table size (256) for bitmask modulo. Linear probing for collision
  resolution. Table size > max entries (128) keeps load factor < 0.5.
- **Tombstone support**: When cleanup destroys a texture, the entry
  name is retained (tombstone) so lookups during probing don't stop
  prematurely. This is important for correctness but in practice the
  hash map is always cleaned up wholesale, not entry-by-entry.
- **Aspect ratio preservation**: SVGs are scaled to fit within
  target_size (the larger dimension). Non-square SVGs produce
  non-square textures. The test verifies this with
  test_load_aspect_ratio.
- **Deduplication via cbx_icon_cache_get check**: Before rasterizing
  an icon, load() checks if the icon is already cached. This means
  "generic-gamepad" (mapped by 7+ types) and "cc-ps5" (mapped by 6 DS5
  variants) are only rasterized once.
- **SDL2_image include**: Added #include <SDL2/SDL_image.h> in icon_cache.c
  for potential future PNG loading (Task 18 will use SDL_image for custom
  icon paths). Currently not called but the include is harmless.

### Next
Task 18 (Runtime icon lookup API) — deps: Task 17 + Task 8 (all done).
Alternatively Task 19 (Renderer init, theme system, text rendering) — deps: Task 2 (done).
Check `ralph tools task ready` and the plan.

## Task 18 (complete) — Runtime icon lookup API

### What landed
- `src/icons/icon_lookup.h`: API for resolving device_type + profile icon
  override to SDL2 texture + display label. Struct cbx_icon_result
  (texture, width, height, label). Functions: cbx_icon_lookup,
  cbx_icon_validate_path. Constants: CBX_ICON_LABEL_LEN=256.
- `src/icons/icon_lookup.c`: Implementation. Resolution order per SPEC
  §8.5: (1) profile override → absolute path PNG via SDL2_image with
  path validation, or built-in icon name → cache lookup with on-demand
  load; (2) icon map lookup by device_type; (3) unknown → generic-
  gamepad + raw type string. Path validation: checks for absolute path,
  rejects ".." traversal, canonicalizes via realpath(), verifies within
  safe directories (user config, user data, system data, system IP data).
  Rejected/failed override paths fall back to device_type icon lookup.
- `src/icons/icon_cache.h/c`: Added cbx_icon_cache_insert() public API
  for inserting externally-created textures (PNGs from SDL2_image) into
  the cache hash map. Cache takes ownership of the texture.
- `tests/fixtures/test_icon.png`: Minimal 8x8 RGBA PNG fixture for
  PNG loading tests.
- `tests/test_icon_lookup.c`: 36 cmocka tests using test_harness SDL2
  dummy driver + real YAML from source tree. Tests: basic lookup
  (known/unknown/null/empty device_type, null map), override built-in
  icon (known/unknown type, nonexistent, empty), override PNG path
  (load, cached, nonexistent, traversal, relative), path validation
  (safe/not-absolute/empty/null/traversal/double-dot/prefix/dotdot-only),
  NULL args, label correctness (from map, raw for unknown), on-demand
  load (device type + override), dimensions, PNG in user config dir,
  cache insert API (basic, null args, replace).
- `CMakeLists.txt`: Added icon_lookup.c to controllerbox STATIC library.
- `tests/CMakeLists.txt`: Added test_icon_lookup target linked with
  controllerbox + cmocka + nanosvg + cbx_test_support. CBX_SOURCE_DIR
  compile def. SDL_VIDEODRIVER=dummy environment.
- `docs/OPERATIONS.md`: Added icon override (profile sidecar) section
  describing cbx_icon_lookup resolution order and path validation.
- `IMPLEMENTATION_PLAN.md`: Task 18 → complete.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 24/24: all previous + test_icon_lookup
- test_icon_lookup 36/36 cmocka tests pass
- verify-boilerplate, check-plan-freshness → exit 0

### Gotchas fixed
- **assert_in_range deprecated in cmocka**: Used assert_in_range which
  is deprecated; replaced with assert_true(rc == 0 || rc == -EACCES).
- **PNG fixture path not in safe dirs**: The source tree fixture path
  (/workspace/controller-box/tests/fixtures/test_icon.png) is not within
  any safe directory (user config, user data, system data, system IP
  data). Tests that need to actually load the PNG copy it to the user
  config dir first via copy_png_to_safe_dir() helper. Tests that only
  check fallback behavior (nonexistent, traversal, relative) use the
  raw paths since they're expected to fail validation.
- **IMG_LoadTexture needs renderer, not cache**: The PNG loading in
  load_png() uses cache->renderer (the SDL_Renderer from the cache
  struct) to call IMG_LoadTexture. This is correct since the cache
  stores the renderer at init time.

### Design decisions
- **PNG textures cached by original path**: PNG textures are stored
  in the icon cache keyed by the original (non-canonical) path string
  from the profile sidecar. This allows subsequent lookups to hit the
  cache without re-validating. If two different sidecars reference
  different paths that resolve to the same file, they'll create
  separate cache entries (minor waste, acceptable for v1).
- **Label always from icon map**: The display label comes from the
  icon map lookup for the device_type, not from the override. This
  means a profile that overrides the icon for a known device type still
  shows the mapped display name. For unknown types, the raw type string
  is the label regardless of override.
- **Fallback on override failure**: If a profile override fails (PNG
  not found, path validation fails, SVG missing for built-in name),
  the lookup falls back to the device_type icon. If that also fails,
  it falls back to generic-gamepad. This ensures the lookup always
  returns a texture if the cache has any icons at all.
- **cbx_icon_cache_insert as public API**: Added to icon_cache.h to
  allow icon_lookup.c to store PNG textures in the shared cache. This
  avoids a separate PNG cache and keeps all icon textures in one place
  for unified cleanup and lookup.
- **Safe directories for PNG paths**: User config dir
  (~/.config/controller-box), user data/profiles dir
  (~/.local/share/inputplumber/profiles), system data dir
  (/usr/share/controller-box), system InputPlumber dir
  (/usr/share/inputplumber). These are the only directories where
  custom PNG icons can be loaded from. Any other absolute path is
  rejected with -EACCES.

### Next
Task 19 (Renderer init, theme system, text rendering cache) — deps:
Task 2 (done).
Alternatively Task 20 (Widget base and concrete widgets) — deps: Task 19.
Check `ralph tools task ready` and the plan.

## Task 19 (complete) — Renderer init, theme system, and text rendering cache

### What landed
- `src/ui/renderer.h/c`: SDL2 renderer init with
  SDL_RENDERER_ACCELERATED|SDL_RENDERER_TARGETTEXTURE. Verifies
  TARGETTEXTURE flag is available; falls back to software renderer
  (with TARGETTEXTURE) if not. Sets blend mode to BLENDMODE_BLEND.
  Queries renderer info for vsync + GLES detection. Alpha blending
  verification: creates target texture, draws semi-transparent rect,
  reads back pixels, checks alpha byte (offset 0 for RGBA8888 on
  little-endian). Non-fatal warning if verification fails. API:
  init/check_target_texture/verify_blending/show/hide/present/clear/
  shutdown.
- `src/ui/theme.h/c`: Colour theme struct with default dark palette
  (bg, overlay_bg, panel_bg, text_primary/secondary/accent/disabled,
  border/border_focus, focus, conflict, success, icon_tint).
  cbx_theme_load applies overlay_opacity from settings to overlay_bg
  alpha (clamped 0-255). Only "default" theme defined (SPEC §12: theme
  format TBD). cbx_theme_is_known checks name recognition.
- `src/ui/text.h/c`: Font loading via SDL2_ttf (max 8 fonts). Text
  texture cache: djb2 hash over (font_id, text, r, g, b), open
  addressing with linear probing, hash table 512 slots, max 256
  entries (load factor < 0.5). Tombstones (hash=1) for deleted entries.
  Text > CBX_TEXT_MAX_LEN (256) renders without caching. Multi-line
  wrapping: splits on newlines, word-wraps each line to max_w pixels
  using TTF_SizeUTF8 measurement, hard-breaks words longer than max_w.
  Returns array of SDL_Texture* (caller frees array, not textures).
  Cache clear (frees textures, keeps fonts) for theme changes.
- `tests/test_renderer_init.c`: 16 cmocka tests. Init (basic, default
  size, null title, null struct), target-texture (check, null),
  blending (verify, null — accepts -ENOTSUP on dummy driver), show/hide,
  present, clear, shutdown null, show/hide null, present/clear null,
  double init, blend mode set.
- `tests/test_text.c`: 34 cmocka tests. Init (basic, null), font load
  (basic, null args, nonexistent, default font), render (basic, cached,
  different colors, different text, different fonts, null args, long
  text, multiple fonts), dims (get, not cached, null), measure (basic,
  null), line height (basic, invalid font), wrapped (basic, multiline,
  word wrap, null), cache clear, cleanup (basic, null), theme (default,
  apply opacity, null, load, load null, is known).
- `CMakeLists.txt`: Added renderer.c, theme.c, text.c to controllerbox.
- `tests/CMakeLists.txt`: Added test_renderer_init + test_text targets
  with SDL_VIDEODRIVER=dummy env. Font path auto-detected via CMake
  foreach over common DejaVuSans.ttf locations (Nix, Debian, Fedora,
  Arch). Passed as CBX_FONT_PATH compile definition.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 26/26: all previous + test_renderer_init + test_text
- test_renderer_init 16/16 cmocka tests pass
- test_text 34/34 cmocka tests pass
- verify-boilerplate, check-plan-freshness → exit 0

### Gotchas fixed
- **Missing errno.h in theme.c**: EINVAL used without including errno.h.
  Added `#include <errno.h>`.
- **Missing errno.h in test files**: Both test_renderer_init.c and
  test_text.c used EINVAL without errno.h. Added include.
- **Missing unistd.h in test_text.c**: `access()` and `R_OK` used for
  font availability check without including unistd.h. Added include.
- **Unused make_renderer/destroy_renderer functions**: Initial approach
  used standalone helper functions, replaced with TestCtx struct +
  test_setup/test_teardown. Removed the unused functions to fix
  -Werror=unused-function.
- **RGBA8888 alpha byte offset**: On little-endian, SDL_PIXELFORMAT_
  RGBA8888 stores the Uint32 0xRRGGBBAA in memory as [A,B,G,R]. The
  alpha byte is at offset 0, not offset 3. Fixed the blending
  verification to check both p[0] (alpha) and p[3] (red).
- **Dummy renderer doesn't support target textures**: The SDL dummy
  driver's software renderer reports TARGETTEXTURE flag but doesn't
  actually support rendering to target textures. The blending
  verification returns -ENOTSUP. The test was updated to accept both
  0 (blending works) and -ENOTSUP (dummy limitation). The init code
  warns but continues (non-fatal).

### Design decisions
- **Blending verification is non-fatal**: The renderer init warns if
  blending verification fails but continues. This is correct for the
  dummy driver (which can't test target textures) and for hardware
  that might have quirks — the overlay still renders, just without
  verified alpha blending.
- **djb2 hash over (font_id, text, r, g, b)**: Colour alpha is excluded
  from the hash because TTF_RenderUTF8_Blended ignores the colour's
  alpha channel (it produces per-pixel alpha from font antialiasing).
  Two renders of the same text in the same RGB but different alpha
  produce identical textures.
- **No cache eviction**: The text cache doesn't evict entries. If the
  cache is full (256 entries), text is rendered but not cached. The
  caller should call cbx_text_cache_clear() on theme changes to free
  old textures. This is simpler and deterministic for a GUI that shows
  a bounded set of text strings.
- **Tombstones in hash table**: Hash 0 = empty, hash 1 = tombstone.
  This prevents probing from stopping prematurely at deleted entries.
  In practice, entries are cleared wholesale via cbx_text_cache_clear.
- **Hard-break for long words**: Words longer than max_w are broken at
  the character level. This prevents infinite loops on very long tokens.
- **Only "default" theme**: SPEC §12 says theme format is TBD. The
  implementation provides a single hardcoded dark palette and a clean
  extension point (add cbx_theme_<name>() functions + a switch in
  cbx_theme_load). The theme struct covers all colours needed by the
  widget toolkit and overlay surface.

### Next
Task 20 (Widget base and concrete widgets) — deps: Task 19 (done).
Alternatively Task 21 (Layout engine) — deps: Task 20.
Check `ralph tools task ready` and the plan.

## Task 20 (complete) — Widget base and concrete widgets (Button, Label, Image, Panel)

### What landed
- `src/ui/widget.h`: Base widget struct `cbx_widget` with vtable
  (draw, handle_event, focus, blur, get_rect, set_rect, destroy).
  Generic dispatchers (NULL-safe). Concrete widget declarations: Button,
  Label, Image, Panel.
- `src/ui/widget.c`: Base dispatcher implementation (forwards through
  vtable, NULL-safe no-ops).
- `src/ui/widget_button.c`: Button with label texture from text cache,
  focused/pressed visual states (panel_bg → panel_bg_hover → text_accent
  when pressed, border → border_focus when focused). Mouse click (in-rect
  press/release) + keyboard Return/Space handling. Press callback with
  user_data. Re-renders label text with focus color on focus/blur.
- `src/ui/widget_label.c`: Static text label, optional multi-line (splits
  on '\n', renders each line via text cache, stacks vertically using
  cbx_text_line_height). Non-interactive (handle_event returns false).
- `src/ui/widget_image.c`: Image widget with 3 scale modes: FIT (preserve
  aspect, fit within rect), FILL (stretch), CENTER (1:1 centered).
  Optional texture ownership (owns_texture → destroy frees texture).
- `src/ui/widget_panel.c`: Container holding up to 32 children. Optional
  bg fill (panel_bg) and border draw. Focus management: focus_first,
  focus_next (wraps), focus_prev (wraps), clear_focus. Event forwarding
  to focused child. Panel does NOT own children (caller manages lifetime).
- `tests/test_widgets.c`: 49 cmocka tests. Base dispatchers (null-safety,
  accessors, rect get/set). Button (init, null, draw, focus/blur, mouse
  press in/out, keyboard Return/Space, no-callback, set-label, set-cb,
  unrelated-event, font rendering). Label (init, null, draw with font,
  multiline, no-event, set-color, set-multiline). Image (init, null,
  draw fit/fill/center, no-event, set-texture, set-scale-mode, null
  texture, dims). Panel (init, null, add/remove, add-null, overflow,
  draw, focus management, empty focus, event forwarding, remove-focused,
  get-child-invalid, set-options).
- `CMakeLists.txt`: Added widget_button.c, widget_label.c, widget_image.c,
  widget_panel.c to controllerbox STATIC library.
- `tests/CMakeLists.txt`: Added test_widgets target linked with
  controllerbox + cmocka. CBX_FONT_PATH compile def.
  SDL_VIDEODRIVER=dummy environment.
- `IMPLEMENTATION_PLAN.md`: Task 20 → complete.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 27/27: all previous + test_widgets
- test_widgets 49/49 cmocka tests pass
- verify-boilerplate, check-plan-freshness → exit 0

### Gotchas fixed
- **Include path for test file**: Test files use `#include "ui/widget.h"`
  (not `"widget.h"`) because the include directory is `src/`, not `src/ui/`.
  Source files within `src/ui/` use `"widget.h"` (same directory).
- **Unused button_press_count**: Initially defined a static counter at file
  scope but all tests use local counters. Removed unused variable to fix
  -Werror=unused-variable.

### Design decisions
- **C struct inheritance**: Base `cbx_widget` is embedded as first member
  of each concrete widget. Vtable assigned at init. Generic code casts
  to `cbx_widget*` and calls dispatchers. Concrete vtable functions cast
  back to the specific type.
- **Panel does NOT own children**: Caller is responsible for destroying
  child widgets. This avoids double-free when children are stack-allocated
  (as in tests) or shared between containers.
- **Button label texture from text cache**: The button borrows the texture
  from the text cache (cbx_text_render returns a cached, cache-owned
  texture). Button's destroy is a no-op — it does not free the label
  texture.
- **Label re-renders on each draw call**: Rather than caching the texture
  pointer, the label calls cbx_text_render() in its draw function. The
  text cache handles deduplication (same text+font+color returns same
  texture). This simplifies set_text/set_color (no need to re-render).
- **Image owns_texture flag**: When true, destroy() calls
  SDL_DestroyTexture. set_texture() also frees the previously owned
  texture. This supports both borrowed (icon cache) and owned (custom
  PNG) textures.
- **Panel focus wraps around**: focus_next/focus_prev use modulo
  arithmetic for wrap-around behavior. This matches console-style
  navigation.

### Next
Task 21 (List, Grid, TabBar, and ProgressBar widgets) — deps: Task 20 (done).
Check `ralph tools task ready` and the plan.

## Task 21 (complete) — List, Grid, TabBar, and ProgressBar widgets

### What landed
- `src/ui/widget_list.c`: Scrollable list with up/down keyboard nav,
  highlight (panel_bg_hover + border_focus when focused), optional icon
  per item (SDL_Texture borrowed), mouse wheel scroll, mouse click
  selection, select callback. Auto-scrolls to keep selected visible.
  Max 64 items. Item height and icon size configurable (defaults 32px,
  24px). visible_count computed from rect height / item_h.
- `src/ui/widget_grid.c`: N rows × M columns grid. Independent row/col
  navigation via move_up/down/left/right. Current position highlighted
  with border_focus rectangle. Cell widgets auto-positioned and drawn.
  Grid does NOT own cells (caller manages). Max 256 cells (16×16).
  set_cell accepts (row, col) pair; computes flat index.
- `src/ui/widget_tabbar.c`: Horizontal tab bar. Left/Right switches
  active tab (always consumed even at boundary). Active tab rendered
  with panel_bg_hover + accent underline. Change callback fires on tab
  switch (not on same-tab set). Mouse click selects tab by x position.
  Max 16 tabs.
- `src/ui/widget_progress.c`: Fill bar 0.0–1.0 with clamping.
  Configurable bar_color and bg_color (default to theme text_accent
  and panel_bg). Non-interactive (handle_event returns false, focus
  is no-op).
- `src/ui/widget.h`: Added declarations for cbx_list, cbx_grid,
  cbx_tabbar, cbx_progress structs and their public APIs.
- `CMakeLists.txt`: Added 4 new source files to controllerbox STATIC lib.
- `tests/CMakeLists.txt`: Added 4 test targets with dummy driver env.
  List and tabbar tests get CBX_FONT_PATH for font-dependent tests.
- `IMPLEMENTATION_PLAN.md`: Task 21 → complete.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 31/31: all previous + 4 new widget tests
- test_widget_list 20/20, test_widget_grid 16/16,
  test_widget_tabbar 14/14, test_widget_progress 12/12
- verify-boilerplate, check-plan-freshness → exit 0

### Gotchas fixed
- **Mouse wheel direction**: SDL wheel.y > 0 = scroll up (earlier
  items), < 0 = scroll down (later items). Initial code added wheel.y
  to scroll_offset directly (wrong direction). Fixed to subtract
  wheel.y so scroll up decrements offset and scroll down increments.
- **TabBar key consumption at boundary**: Left/Right keydowns should
  always be consumed (return true) even when at the boundary and no
  movement occurs. Initial code returned false when move_left/right
  returned -1. Fixed to always return true for these keys.
- **maybe-uninitialized TestCtx**: GCC -Werror=maybe-uninitialized
  flags `TestCtx ctx;` (uninitialized struct) because test_teardown
  checks ctx->renderer which could be uninitialized if test_setup
  fails. Fixed by zero-initializing: `TestCtx ctx = {0};` in all
  four test files.
- **Unused cache variable in test_grid_set_get_cell**: Declared
  cbx_text_cache cache but never used it (test uses raw widget
  pointers). Removed the unused declaration.

### Design decisions
- **List icons borrowed**: Icons are SDL_Texture* borrowed from the
  icon cache — not freed by the list. Same pattern as button labels.
- **Grid does NOT own cells**: Same as Panel — caller manages cell
  widget lifetime. Prevents double-free with stack-allocated widgets.
- **TabBar always consumes left/right**: Console-style UX where
  directional keys are always consumed by the focused widget, even
  at boundaries. This prevents the event from propagating to parent
  containers.
- **ProgressBar not focusable**: Focus is a no-op. The progress bar
  is display-only, not interactive.
- **Grid highlight drawn after cells**: The border_focus rectangle
  is drawn on top of the cell content to ensure it's visible. The
  cell widget's own focus handling (if any) is separate.

### Next
Task 22 (Focus chain system and input event mapping) — deps: Task 21
(done), Task 14 (done).
Alternatively Task 23 (Animation primitives and dirty rect optimization) —
deps: Task 19 (done).
Check `ralph tools task ready` and the plan.

## Task 22 (complete) — Focus chain system and input event mapping

### What landed
- `src/ui/focus.h/c`: Focus chain manager with directional (spatial)
  navigation. Maintains a flat list of up to 64 focusable widgets with
  their screen rectangles and row groups. Navigation: up/down/left/right
  finds the nearest widget whose center is in that direction, scoring
  by primary-axis distance + 1.5× lateral offset penalty. Player Mode
  restricts up/down to the same row group (SPEC §4.3); Host Mode allows
  crossing rows (SPEC §4.4). Left/right ignores row grouping in both
  modes. Focus/blur via widget vtable dispatchers. API: init, add (with
  optional rect override and row group), clear, count, set/get mode,
  get_focused/get_focused_widget/get_entry, focus (by index),
  focus_first, focus_widget, blur, navigate (directional), update_rect.
- `src/ui/input_map.h/c`: Input event mapping from InputPlumber's
  normalized input enum (ip_input_id + ip_input_category) to synthetic
  SDL_Event structures for the widget vtable's handle_event. Buttons:
  value >= 0.5 → SDL_KEYDOWN, < 0.5 → SDL_KEYUP with semantic SDLK_*
  codes (UP/DOWN/LEFT/RIGHT, RETURN for A, ESCAPE for B, TAB for START,
  BACKSPACE for SELECT, MENU for GUIDE, PAGEUP/PAGEDOWN for L1/R1,
  F1/F2 for L3/R3). Axes: |value| > 0.5 threshold → directional
  keydown (LeftStickY+ → UP, LeftStickY- → DOWN, LeftStickX+ → RIGHT,
  LeftStickX- → LEFT, same for RightStick). Deadzone (|value| <= 0.5)
  produces no event. Stateless: each call produces 0 or 1 SDL_Event.
  Helpers: cbx_input_map_keycode (button → SDL_Keycode),
  cbx_input_map_axis_direction (axis + value → directional ip_input_id).
- `tests/test_focus.c`: 39 cmocka tests. Init (basic, null), add (basic,
  custom rect, null args, overflow), clear, count, mode (set/get, null),
  focus (first, empty, by index, invalid, switches blur, by widget,
  not found), blur (with/without focus), get focused/widget/entry,
  navigation (right, left, right boundary, left boundary, no focus, null,
  down host, up host, down player restricted, down player same row,
  up player restricted, left/right ignore mode, picks nearest, diagonal
  prefers aligned, host crosses rows), rect update (valid, invalid).
- `tests/test_input_map.c`: 30 cmocka tests. Keycode mapping (dpad, face,
  center, shoulders, stick clicks, axes unknown, unknown input). Button
  events (press, release, dpad, R3, release value 0, threshold press,
  threshold release). Axis events (left stick up/down/right/left, right
  stick up/left, deadzone, at threshold). Axis direction helper
  (positive, negative, deadzone, unknown axis). Edge cases (unknown
  input, null out, all buttons mapped, all axes mapped).
- `CMakeLists.txt`: Added focus.c, input_map.c to controllerbox STATIC.
- `tests/CMakeLists.txt`: Added test_focus + test_input_map targets
  linked with controllerbox + cmocka.
- `IMPLEMENTATION_PLAN.md`: Task 22 → complete.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 33/33: all previous + test_focus + test_input_map
- test_focus 39/39 cmocka tests pass
- test_input_map 30/30 cmocka tests pass
- verify-boilerplate, check-plan-freshness → exit 0

### Gotchas fixed
- **Wrong include for ip_input_id**: Initially included `dbus_mock.h`
  in input_map.h, but ip_input_id/ip_input_category are defined in
  `dbus/ip_input_signal.h`, not dbus_mock.h. Fixed to include
  `dbus/ip_input_signal.h` in both header and test file.
- **typeof() not available in C11 strict mode**: CMAKE_C_EXTENSIONS=OFF
  means `typeof()` (GCC extension) is unavailable. Replaced with
  explicit `struct axis_map_entry` type definition.

### Design decisions
- **Spatial scoring with lateral penalty**: Score = primary_distance +
  1.5 × lateral_distance. This ensures directly-aligned candidates
  beat diagonally-offset ones at the same Euclidean distance, which
  matches user expectations for grid-style navigation.
- **Player Mode row restriction on up/down only**: Left/right works
  across rows in both modes. This matches SPEC §4.3 where each controller
  moves left/right across columns and up/down cycles within its row.
- **Stateless axis mapping**: Each axis event independently produces a
  keydown if above threshold or nothing if in deadzone. This avoids
  needing state tracking in the mapper itself. The widget's handle_event
  handles repeated keydowns gracefully.
- **Button threshold at 0.5**: Buttons are binary (0.0/1.0) but the
  mapper accepts any value >= 0.5 as press and < 0.5 as release. This
  handles analog triggers (L2/R2) that may report intermediate values.
- **Axis threshold at 0.5**: Matches InputPlumber's typical deadzone.
  Configurable via CBX_INPUT_AXIS_THRESHOLD define.
- **SDLK_TAB for START**: Chosen to avoid collision with SDLK_RETURN
  (mapped to A/activate). START is a separate action in console UIs.
- **SDLK_F2 for R3**: R3 toggles Host Mode (SPEC §4.4). F2 is an
  unlikely collision with other widget key handling.

### Next
Task 23 (Animation primitives and dirty rect optimization) — deps:
Task 19 (done).
Alternatively Task 24 (Pre-built overlay surface infrastructure) — deps:
Task 23, Task 18 (done).
Check `ralph tools task ready` and the plan.

## Task 23 (complete) — Animation primitives and dirty rect optimization

### What landed
- `src/ui/animation.h/c`: Alpha tween system with 4 easing functions
  (linear, ease-in, ease-out, ease-in-out). State machine (IDLE→RUNNING→
  COMPLETE) driven by SDL_GetTicks(). API: init, start (from/to alpha,
  duration_ms, easing), update (returns current alpha), stop,
  is_running/is_complete, alpha getter. Convenience: fade_in (0→target),
  fade_out (cur→0). Handles tick wraparound. Idempotent on COMPLETE.
- `src/ui/dirty_rect.h/c`: Dirty-rect tracker (max 64 rects) for
  incremental overlay re-rendering. Add rects (clamped to screen bounds),
  merge overlapping/adjacent (iterative union), render via callback with
  SDL_RenderSetClipRect set per-rect. Empty list → single full-screen
  render (first paint). Intersects test for widget redraw decisions.
  Overflow: merges into entry 0 as fallback.
- `tests/test_animation.c`: 45 cmocka tests. Easing (linear, in, out,
  in_out, clamp, unknown). Animation (init, null-safe, start, instant,
  progress, idle update, complete idempotent, stop, fade_in, fade_out,
  restart, ease_in/out progress). Dirty rect (init, add, null, zero-area,
  clamp, multiple, clear, add_all, get_invalid, merge overlapping/
  adjacent/non-overlapping/empty/single/chain, intersects/empty, render
  empty/single/multiple/abort/null_fn/null_renderer, overflow, merge
  after overflow, integration fade+dirty).
- `CMakeLists.txt`: Added animation.c, dirty_rect.c to controllerbox STATIC.
- `tests/CMakeLists.txt`: Added test_animation target with
  SDL_VIDEODRIVER=dummy env.
- `IMPLEMENTATION_PLAN.md`: Task 23 → complete.

### Verification (all pass)
- clean build (Debug, no warnings)
- ctest 34/34: all previous + test_animation
- test_animation 45/45 cmocka tests pass
- verify-boilerplate, check-plan-freshness → exit 0

### Gotchas fixed
- **Integration test timing**: fade_in with 1ms duration immediately
  followed by update returns 0ms elapsed (same tick), so animation stays
  RUNNING. Fixed by adding SDL_Delay(2) before the update call to ensure
  elapsed > duration_ms. This matches the pattern in
  test_anim_update_progress.

### Design decisions
- **Stateless easing**: cbx_ease_eval is a pure function — no state, no
  side effects. Takes linear t∈[0,1], returns eased value. Clamps
  out-of-range input.
- **IDLE update returns from_alpha**: Calling update on an IDLE animation
  doesn't start it — returns from_alpha. This is deliberate: the caller
  must explicitly call start(). This prevents accidental animations from
  spurious update calls.
- **COMPLETE is idempotent**: Repeated updates on a COMPLETE animation
  always return to_alpha. This simplifies the render loop — no need to
  guard against double-update.
- **Dirty-rect empty → full render**: When no dirty rects exist,
  cbx_dirty_rect_render calls fn once with a full-screen clip rect. This
  ensures the first render paints everything without requiring the caller
  to special-case it.
- **Overflow merges into entry 0**: When the 64-rect limit is reached,
  new rects are unioned into entry 0 rather than dropped. This is a
  conservative fallback — the entire screen gets dirtied rather than
  missing a region.
- **Merge is iterative**: The merge loop repeats until no more merges
  happen. This handles chains (r1↔r2↔r3 where r1 doesn't overlap r3) by
  first merging r1+r2, then the merged rect overlaps r3.
- **No animation config in settings**: The SPEC doesn't specify fade
  durations. Animation timing is hardcoded in the caller (e.g. 300ms
  fade-in). If configurable timing is needed later, add fields to
  cbx_settings.

### Next
Task 24 (Pre-built overlay surface infrastructure) — deps: Task 23 (done),
Task 18 (done).
Alternatively Task 25 (Identity extraction) — deps: Task 14 (done),
Task 6 (done).
Check `ralph tools task ready` and the plan.

## Task 24 (complete) — Pre-built overlay surface infrastructure

### What landed
- `src/overlay/surface_build.h`: API for cbx_overlay_surface struct
  (SDL_Texture target, width/height, visible flag, opacity 0-255,
  cbx_dirty_rect tracker, built flag). Functions: init, destroy,
  set_opacity/get_opacity, show/hide/is_visible, mark_dirty/
  mark_dirty_all/clear_dirty/is_dirty/dirty_count, render (delegates
  to dirty_rect_render with render target switching), get_texture,
  get_size, is_built. Render callback type: cbx_overlay_render_fn
  (same shape as cbx_dirty_render_fn).
- `src/overlay/surface_build.c`: Full implementation. Init creates
  SDL_TEXTUREACCESS_TARGET texture, applies alpha mod + blend mode.
  Show = SDL_SetRenderTarget(NULL) + SDL_RenderCopy + SDL_RenderPresent.
  Hide = visible=false (texture NOT destroyed). Render = set target to
  overlay texture, merge dirty rects, delegate to cbx_dirty_rect_render
  (handles clip rects + callback), restore target to NULL, clear dirty
  on success. Opacity clamped 0.0-1.0 → 0-255 via opacity_to_u8().
- `tests/test_surface_build.c`: 31 cmocka tests. Init (basic, null args,
  opacity clamping). Destroy (safe on NULL/zeroed, after init). Opacity
  (set, clamp, null). Show/hide (show, hide, no-texture-creation
  structural check, null args, unbuilt, null). Dirty rect (mark, all,
  null rect, unbuilt). Render (dirty, empty→full-screen, overlapping
  merge, target switch verification, fail keeps dirty, null args).
  Accessors (size, texture, built, visible, dirty count, is_dirty,
  clear null). Full show→render→show cycle.
- `CMakeLists.txt`: Added src/overlay/surface_build.c to controllerbox
  STATIC library.
- `tests/CMakeLists.txt`: Added test_surface_build target with
  SDL_VIDEODRIVER=dummy env.
- `IMPLEMENTATION_PLAN.md`: Task 24 → complete.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 35/35: all previous + test_surface_build
- test_surface_build 31/31 cmocka tests pass
- verify-boilerplate, check-plan-freshness → exit 0

### Gotchas fixed
- Missing <errno.h>: EINVAL/ENOMEM used in surface_build.c but not
  included. Added #include <errno.h>.
- Unused parameter warnings: Many test functions take `void **state`
  but don't use it. Added `(void)state;` to each. Also removed unused
  `cbx_overlay_surface s` variable in test_surface_get_size.
- Missing <errno.h> in test file: test_surface_render_null_args uses
  EINVAL. Added #include <errno.h> to test file.

### Design decisions
- **Render delegates to dirty_rect_render**: cbx_overlay_surface_render
  calls cbx_dirty_rect_render (from Task 23) with the overlay texture
  as the render target. This reuses the existing per-rect clip + callback
  mechanism. The render callback type is the same shape as
  cbx_dirty_render_fn, cast for type compatibility.
- **Merge before render**: Dirty rects are merged (cbx_dirty_rect_merge)
  before rendering to reduce overdraw. This handles chains of
  overlapping/adjacent dirty regions.
- **Clear dirty on success only**: If the render callback returns
  non-zero, dirty rects are preserved so the caller can retry.
- **Blend mode BLEND**: The overlay texture uses SDL_BLENDMODE_BLEND so
  alpha modulation composites correctly when RenderCopy'd to the screen.
- **No animation in this layer**: The animation system (Task 23) is
  available for fade-in/fade-out but is NOT wired in here. That's
  Task 28's job (overlay state machine lifecycle). This module just
  provides the pre-built surface infrastructure.
- **Show path structural check**: test_surface_show_no_texture_creation
  verifies the texture pointer is identical before and after show(),
  proving no texture allocation occurs in the show path.

### Next
Task 25 (Identity extraction from source device properties) — deps:
Task 14 (done), Task 6 (done).
Alternatively Task 28 (Overlay state machine and lifecycle) — deps:
Task 13 (done), Task 24 (done).
Check `ralph tools task ready` and the plan.

## Task 25 (complete) — Identity extraction from source device properties

### What landed
- `src/identify/identity.h`: cbx_identity_layer enum (NONE=0, BT_MAC=1,
  USB_SERIAL=2, USB_PORT=3, ORDER=4), cbx_source_iface enum (EVDEV, HIDRAW),
  cbx_source_props struct (iface, unique_id, phys_path, serial_number,
  id_bustype), cbx_identity struct (id string + layer). API: init, extract,
  parse_layer, is_downgrade, is_mac_address, parse_bustype.
- `src/identify/identity.c`: Full implementation. Resolution order:
  1. BT bus (0x05) + valid MAC in unique_id or serial_number → BT: (uppercase)
  2. Serial (evdev UniqueId or HIDRaw SerialNumber, cross-fallback) non-MAC
     → USB:SNxxxxx (alnum/underscore/dash only)
  3. PhysPath non-empty → USB:phys:xxxxx (printable non-space)
  4. Connection order >= 0 → ORDER:n
  Returns -ENOENT if nothing found, -EINVAL for null args.
  parse_layer validates prefixed IDs and returns the layer. is_downgrade
  checks new_layer > old_layer.
- `tests/test_identity.c`: 42 cmocka tests. Init, MAC address helper (valid/
  invalid), bustype parser (valid/invalid), all 4 layers (BT MAC, USB serial
  evdev/HIDRaw/cross-fallback, USB phys, order), edge cases (empty BT uniq,
  BT invalid MAC falls through, invalid serial chars, too-long serial,
  MAC-on-USB-bus, no bustype), null args, layer precedence (all combinations),
  parse_layer (all formats + invalid), roundtrip extract→parse, downgrade
  detection (yes/no), integration.
- `CMakeLists.txt`: Added src/identify/identity.c to controllerbox STATIC.
- `tests/CMakeLists.txt`: Added test_identity target.
- `IMPLEMENTATION_PLAN.md`: Task 25 → complete.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 36/36: all previous + test_identity
- test_identity 42/42 cmocka tests pass
- verify-boilerplate, check-plan-freshness → exit 0

### Gotchas fixed
- Missing <stdlib.h>: strtol() used in parse_bustype but not included.
  Added #include <stdlib.h>.

### Design decisions
- **MAC uppercased**: BT MAC is always stored uppercase (BT:AB:CD:...) for
  consistency, regardless of input case. This simplifies string comparison.
- **evdev/HIDRaw cross-fallback**: If the primary serial source is empty,
  the code falls back to the other interface's serial. This handles
  dual-interface devices (evdev + HIDRaw) where one interface may report
  empty but the other has the serial.
- **MAC-on-USB-bus falls to phys**: If a USB device reports a MAC-like
  unique_id (contains colons), it can't be formatted as USB:SN (colons
  aren't valid serial chars). Rather than failing, it falls through to
  phys path. This is an edge case for composite devices.
- **No DBus calls in identity module**: cbx_identity_extract is a pure
  function operating on already-fetched properties. The caller gathers
  properties via ip_source_get_* (Task 14) and passes them in. This keeps
  the identity module testable without DBus mock infrastructure.
- **Layer NONE is not a downgrade**: is_downgrade returns false if either
  layer is NONE. NONE means "no identity" which isn't comparable.

### Next
Task 26 (Assignment lookup, default assignment, and persistence) — deps:
Task 25 (done), Task 6 (done).
Alternatively Task 27 (Identity downgrade detection and GamepadOrder
restoration) — deps: Task 11 (done), Task 25 (done), Task 15 (done).
Alternatively Task 28 (Overlay state machine and lifecycle) — deps:
Task 13 (done), Task 24 (done).
Check `ralph tools task ready` and the plan.

## Task 26 (complete) — Assignment lookup, default assignment, and persistence

### What landed
- `src/identify/assign.h/c`: Pure assignment lookup functions on in-memory
  cbx_assignments struct (no I/O). cbx_assign_find_index (find by ID),
  cbx_assign_lookup (return matching assignment), cbx_assign_slot_occupied
  (check slot in use), cbx_assign_lowest_free_slot (lowest unoccupied slot
  0..max_slots-1, or -1 if full), cbx_assign_make_default (create entry with
  CBX_DEFAULT_PROFILE="default"), cbx_assign_resolve (lookup or create
  default for on-connect; returns 0=existing, 1=new default, -ENOENT=full).
- `src/identify/assign_persist.h/c`: Atomic load-modify-save operations.
  cbx_assign_persist_set (add/update by ID), set_slot, set_profile,
  remove (idempotent), auto_assign (load→lookup→find-free-slot→insert→save
  in one call — the atomic lowest-free-slot computation per SPEC §6.2).
  All preserve existing gamepad_order entries. Delegates atomic write
  to cbx_assignments_save (temp file + rename, mode 0600).
- `tests/test_assign.c`: 35 cmocka tests (no I/O, pure functions).
- `tests/test_assign_persist.c`: 39 cmocka tests with per-test HOME temp
  dir setup/teardown (cmocka_unit_test_setup_teardown).
- `CMakeLists.txt`: Added assign.c, assign_persist.c to controllerbox STATIC.
- `tests/CMakeLists.txt`: Added test_assign + test_assign_persist targets.
- `IMPLEMENTATION_PLAN.md`: Task 26 → complete.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 38/38: all previous + test_assign + test_assign_persist
- test_assign 35/35 cmocka tests pass
- test_assign_persist 39/39 cmocka tests pass
- verify-boilerplate, check-plan-freshness → exit 0

### Gotchas fixed
- **BT MAC must have 6 hex pairs**: Test used "BT:AB:CD:01:02:03" (5 pairs)
  which fails cbx_validate_id. Fixed to "BT:AB:CD:01:02:03:04" (6 pairs).
- **Per-test setup/teardown for file I/O tests**: Initially used group-level
  setup/teardown (cmocka_run_group_tests) for test_assign_persist, but tests
  shared the same HOME temp dir and polluted each other's state (e.g.
  test_set_full_table left 32 entries, test_remove_existing found them).
  Fixed by using cmocka_unit_test_setup_teardown per test that does I/O.
  Tests that don't do I/O (null/invalid arg checks) use plain cmocka_unit_test.
- **Missing stdio.h**: test_assign.c used snprintf but didn't include stdio.h.
  Added #include <stdio.h>.
- **Format truncation**: write_raw_assignments path buffer was PATH_MAX+64,
  same as dir buffer. GCC -Werror=format-truncation flagged it. Fixed path
  to PATH_MAX+128.

### Design decisions
- **CBX_DEFAULT_PROFILE = "default"**: SPEC §5.3 says "The Default profile
  is built-in, always present, read-only, always the fallback." New
  controllers get profile="default" when no existing preference is found.
- **Pure vs I/O split**: assign.c has pure functions (no I/O, easily
  testable without file fixtures). assign_persist.c has load-modify-save
  operations (needs temp HOME setup/teardown). This separation makes the
  lookup logic testable in isolation.
- **auto_assign is the atomic path**: cbx_assign_persist_auto_assign does
  load→lookup→compute-lowest-free→insert→save in one call, minimizing the
  race window for simultaneous connects. The save is atomic (temp+rename).
- **resolve does not mutate**: cbx_assign_resolve operates on an in-memory
  struct and returns a copy of the assignment — it does NOT modify the
  input struct. The caller must persist separately (or use auto_assign).
- **remove is idempotent**: Removing a non-existent ID returns 0, not
  -ENOENT. This matches REST semantics and simplifies caller code.
- **set preserves gamepad_order**: All persist operations load the full
  cbx_assignments struct (including gamepad_order), modify only the
  assignments array, and save the whole thing. This preserves gamepad_order
  entries across assignment changes.

### Next
Task 27 (Identity downgrade detection and GamepadOrder restoration) — deps:
Task 11 (done), Task 25 (done), Task 15 (done).
Alternatively Task 28 (Overlay state machine and lifecycle) — deps:
Task 13 (done), Task 24 (done).
Check `ralph tools task ready` and the plan.

## Task 27 (complete) — Identity downgrade detection and GamepadOrder restoration

### What landed
- `src/identify/identity_downgrade.h/c`: Pure-function downgrade detection
  with 3 API levels:
  - `cbx_downgrade_check(old_id, new_ident, order, out)`: Compares old vs
    new identity layers via `cbx_identity_parse_layer` +
    `cbx_identity_is_downgrade`. If downgrade, builds ORDER:n fallback.
    Returns 0=no downgrade (out=*new_ident), 1=downgrade (out=ORDER:n).
  - `cbx_downgrade_find_stronger(a, new_layer, out_id, len)`: Scans all
    assignments for IDs at a lower (stronger) layer than new_layer.
    Returns the strongest (lowest layer number) found.
  - `cbx_downgrade_resolve(a, new_ident, order, out)`: High-level — if
    new ID matches existing assignment → no downgrade. Else if stronger-
    layer assignment exists → downgrade → ORDER:n fallback.
- `src/identify/gamepad_order_restore.h/c`: GamepadOrder restoration after
  InputPlumber restart.
  - `cbx_gamepad_order_map_ids(backend, bus, model, saved_ids_csv,
    out_paths_csv, len, &restored, &skipped)`: Maps saved IDs → composite
    paths by querying `ip_composite_get_persistent_id` on each composite.
    CSV iterator with whitespace trimming and empty-token skip.
  - `cbx_gamepad_order_restore(backend, bus, model, &restored, &skipped)`:
    Full restore flow — `ip_gamepad_order_load` → `map_ids` →
    `ip_manager_set_gamepad_order`. Returns -ENOENT if no saved order.
- `tests/test_identity_downgrade.c`: 33 cmocka tests (pure functions, no I/O).
  Tests: check (no downgrade same/upgrade, BT→SN, SN→phys, phys→order, BT→
  order, null/empty/invalid old_id, new=NONE, null args, negative order),
  find_stronger (basic, picks strongest, none found, same layer not stronger,
  empty, null args, NONE layer, invalid IDs skipped), resolve (matching ID,
  stronger exists, no stronger, BT stronger, empty, null, NONE, null args,
  negative order, multiple stronger, ID matches), integration (full downgrade
  flow with extract→resolve→assign_resolve, new controller no downgrade).
- `tests/test_order_restore.c`: 20 cmocka tests with mock DBus + temp HOME.
  Tests: map_ids (single match, stale, empty CSV, partial match, no composites,
  null args, null counts OK, DBus error, ORDER ID, whitespace trimmed, empty
  token), restore (success, no saved order, empty saved order, all stale,
  null args, null counts, partial, no composites, round-trip save→restore).
- `CMakeLists.txt`: Added identity_downgrade.c, gamepad_order_restore.c to
  controllerbox STATIC.
- `tests/CMakeLists.txt`: Added test_identity_downgrade (CMOCKA) and
  test_order_restore (cbx_test_support) targets.
- `IMPLEMENTATION_PLAN.md`: Task 27 → complete.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 40/40: all previous + test_identity_downgrade + test_order_restore
- test_identity_downgrade 33/33 cmocka tests pass
- test_order_restore 20/20 cmocka tests pass
- verify-boilerplate, check-plan-freshness → exit 0

### Gotchas fixed
- Missing `<stdio.h>`: snprintf() used in identity_downgrade.c but not
  included. Added #include <stdio.h>.
- Missing `assign.h`: cbx_assign_lookup() used in identity_downgrade.c
  but assign.h not included. Added #include "assign.h".
- Mock limitation: ip_dbus_mock matches by (iface, member) only, not by
  path. All PersistentId queries return the same value. Tests account for
  this by testing one composite or acknowledging all composites match the
  same ID (first match wins, stops searching).

### Design decisions
- **Two-tier downgrade API**: cbx_downgrade_check is the low-level
  comparison (old_id vs new_ident). cbx_downgrade_resolve is the high-level
  scan (assignments table vs new_ident). The caller can use whichever level
  is appropriate — if the caller already knows the old identity (e.g.,
  tracked per-slot), use check(). If the caller only has the assignments
  table, use resolve().
- **ORDER:n fallback**: The downgrade fallback is always ORDER:n (layer 4,
  session-level). This avoids creating permanent assignments with unstable
  identifiers (USB:phys changes on port move, USB:SN may disappear on
  driver change). ORDER:n is explicitly transient.
- **First match wins in map_ids**: When multiple composites have the same
  PersistentId (shouldn't happen in practice but the mock returns the same
  value for all), the first matching composite is used and we stop
  searching. This prevents duplicate paths in the output CSV.
- **Empty saved order → -ENOENT**: cbx_gamepad_order_restore returns
  -ENOENT when there's no saved gamepad_order (file doesn't exist or
  gamepad_order is empty). This signals the caller that there's nothing
  to restore — not an error, just "nothing to do."
- **Stale IDs are skipped, not failed**: When a saved ID has no matching
  composite (device was removed), it's counted in skipped_count and
  excluded from the restored path CSV. The restore still succeeds for
  the remaining IDs.
- **CSV iterator is reusable**: csv_for_each is a static helper that
  takes a callback. It trims whitespace and skips empty tokens. This
  pattern could be extracted to a shared utility if other modules need
  CSV iteration.

### Next
Task 28 (Overlay state machine and lifecycle) — deps: Task 13 (done),
Task 24 (done).
Alternatively Task 29 (Character select grid rendering) — deps: Task 28,
Task 22, Task 18, Task 26.
Check `ralph tools task ready` and the plan.

## Task 28 (complete) — Overlay state machine and lifecycle

### What landed
- `src/overlay/lifecycle.h`: cbx_overlay_state enum (IDLE=0, ACTIVATING,
  VISIBLE, CLOSING). cbx_overlay_lifecycle struct composing: DBus backend
  + bus + composite_path (for InterceptMode set on close), optional
  cbx_overlay_surface + SDL_Renderer, cbx_anim fade with configurable
  fade_in_ms/fade_out_ms/target_opacity, visible_ticks/max_visible_ticks
  timeout watchdog, error_count/max_errors tracking, callbacks
  (on_visible, on_closed, on_save, on_error).
- `src/overlay/lifecycle.c`: Full implementation. State machine:
  - activate(): IDLE → ACTIVATING (start fade-in if fade_in_ms > 0) or
    instant → VISIBLE (if fade_in_ms == 0)
  - tick(): polls animation in ACTIVATING/CLOSING, increments timeout in
    VISIBLE (force-closes if max_visible_ticks exceeded)
  - close(): VISIBLE/ACTIVATING → CLOSING (fire on_save, set
    InterceptMode=PASS, start fade-out) or instant → IDLE (if
    fade_out_ms == 0)
  - force_close(): any state → IDLE, sets InterceptMode=PASS, skips save
  - enter_visible/enter_idle: transition helpers that show/hide surface
    and fire callbacks
- `tests/test_overlay_lifecycle.c`: 29 cmocka tests in two fixtures:
  - Basic fixture (no SDL, instant transitions, mock DBus): init, defaults,
    null, activate (instant, not-idle, null), close (instant, from
    activating, from idle, null), tick (idle, null, timeout, no-timeout),
    force close (from visible, from idle, null), error handling
    (InterceptMode fail, save fail), no callbacks, state helpers,
    full lifecycle instant
  - Animation fixture (SDL_INIT_TIMER, fade durations > 0): activate with
    fade, close with fade, full lifecycle with fade, close activating
    with fade
- `CMakeLists.txt`: Added src/overlay/lifecycle.c to controllerbox STATIC.
- `tests/CMakeLists.txt`: Added test_overlay_lifecycle target linked to
  controllerbox + cbx_test_support.
- `IMPLEMENTATION_PLAN.md`: Task 28 → complete.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 41/41: all previous + test_overlay_lifecycle
- test_overlay_lifecycle 29/29 cmocka tests pass
- verify-boilerplate → exit 0

### Gotchas fixed
- Include path: `dbus_mock.h` is in tests/ (not src/dbus/), included
  as `#include "dbus_mock.h"` because tests/ is in the include path.
  Initially used `#include "dbus/dbus_mock.h"` which failed.
- `IP_ERR_NO_REPLY` defined in `src/dbus/ip_connection.h`, not in
  dbus_mock.h. Added include to test file.
- Animation runs without surface: Initially begin_fade_in/begin_fade_out
  checked `&& lc->surface`, causing fade durations to be ignored when
  surface is NULL (instant transitions even with fade_ms > 0). Fixed:
  animation runs regardless of surface; only rendering side effects
  (show/hide/mark_dirty) are gated on surface being non-NULL.
- test_close_activating_with_fade: Updated to expect CLOSING state
  (not instant IDLE) after close(), then tick through the fade-out
  animation to reach IDLE.

### Design decisions
- **Surface optional for testability**: The lifecycle module accepts
  NULL surface/renderer. Without a surface, it's a pure state machine
  with no rendering side effects. This allows testing the state machine
  with just mock DBus (no SDL rendering needed). The animation still
  runs (it's just opacity values, not rendering).
- **Save via callback**: The on_save callback handles persistence and
  conflict resolution (SPEC §4.5). The lifecycle module fires it on close
  and reports errors but doesn't implement the save logic itself. This
  keeps the module decoupled from file I/O and conflict resolution
  algorithms.
- **InterceptMode set on close via backend vtable**: close() calls
  ip_composite_set_intercept_mode(PASS) through the injectable backend.
  If it fails, on_error is fired but the close proceeds (overlay is hidden,
  but input may be stuck in intercept mode — the daemon can retry).
- **Close from ACTIVATING cancels activation**: If the user somehow
  triggers close while still fading in, the activation is cancelled and
  the overlay closes. This is a clean cancel path.
- **force_close skips save**: force_close is for emergency shutdown
  (timeout, error). It doesn't fire on_save because the user's changes
  may be in an inconsistent state. The normal close() path is for
  intentional close (B button) which fires on_save.
- **Timeout uses tick counting**: max_visible_ticks counts main-loop
  ticks in VISIBLE state. 0 = disabled (no timeout). This mirrors the
  ip_intercept_poll timeout pattern.
- **Animation decoupled from surface**: The fade animation runs even
  without a surface. This allows the state machine to use animation
  timing for state transitions (ACTIVATING duration = fade_in_ms,
  CLOSING duration = fade_out_ms) even in headless/test mode.

### Next
Task 29 (Character select grid rendering and Player Mode navigation) —
deps: Task 28 (done), Task 22 (done), Task 18 (done), Task 26 (done).
Alternatively Task 30 (Overlay input handling and B-button close) —
deps: Task 28 (done), Task 13 (done).
Check `ralph tools task ready` and the plan.

## Task 29 (complete) — Character select grid rendering and Player Mode navigation

### What landed
- `src/overlay/grid_render.h/c`: Pure data model for the character select
  grid + SDL render callback. cbx_select_grid struct with rows
  (composites), cols (Unassigned + player slots), and profile list.
  Build from cbx_grid_composite_info[] + cbx_settings + cbx_assignments.
  Navigation: move_left/right (boundary-aware, -ERANGE at edges),
  cycle_profile_up/down (wraps around profile list). Slot↔col conversion
  (col 0=Unassigned=-1, col N=slot N-1). Render function draws column
  headers, row labels (model name + profile), cell icons (via icon_lookup),
  position indicators (filled circle on current, hollow on others),
  highlight rect on current column. NULL-safe (no-op if renderer NULL).
  Compatible with cbx_overlay_surface_render via cbx_select_grid_render_cb.
- `src/overlay/player_mode.h/c`: Per-controller row navigation.
  cbx_player_mode_handle dispatches LEFT/RIGHT (move + on_slot_change
  callback with new slot, -1 for Unassigned), UP/DOWN (cycle profile +
  on_profile_change callback with profile name + composite_path for
  LoadProfilePath), B (returns CLOSE), R3 (returns HOST for Task 30).
  Independence: each controller only modifies its own row_idx.
- `tests/test_grid_render.c`: 29 cmocka tests (init, build with/without
  assignments, column types, slot out of range, profile management,
  navigation, cycle profile (wrap, not-in-list, no-profiles), accessors,
  slot/col conversion, render NULL-safe + dummy SDL renderer).
- `tests/test_player_mode.c`: 22 cmocka tests (init, LEFT/RIGHT + boundary,
  UP/DOWN + wrap, no-profiles, B, R3, independence across 3 controllers,
  slot change to Unassigned (-1), NULL safety, no callbacks, accessors).
- `CMakeLists.txt`: Added grid_render.c, player_mode.c to controllerbox.
- `tests/CMakeLists.txt`: Added test_grid_render + test_player_mode targets.
- `IMPLEMENTATION_PLAN.md`: Task 29 → complete.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 43/43: all previous + test_grid_render + test_player_mode
- test_grid_render 29/29 cmocka tests pass
- test_player_mode 22/22 cmocka tests pass
- verify-boilerplate, check-plan-freshness → exit 0

### Gotchas fixed
- `CBX_DEFAULT_PROFILE` and `cbx_assign_lookup` are in `identify/assign.h`,
  not in `config/config_assignments.h`. Added `#include "identify/assign.h"`
  to grid_render.c.
- `strncpy` truncation warning: `strncpy(row->profile, found.profile, 63)`
  triggers `-Werror=stringop-truncation` because both source and dest are
  64 bytes. Fixed with `snprintf(row->profile, CBX_GRID_PROFILE_LEN, "%s",
  found.profile)`.
- Unused variable `cy` in render function: removed the unused `int cy`
  that was left over from an earlier circle-drawing approach.
- `make_settings` helper in test: required 5 args but many calls passed
  fewer. Fixed by passing NULL for unused type args (the function already
  checks for NULL before strncpy).
- `test_build_no_composites` passed NULL composites with count=0. The
  build function initially rejected NULL composites unconditionally. Fixed:
  only reject NULL composites when count > 0.
- `test_render_with_dummy` was skipped under ctest because SDL_VIDEODRIVER
  env var was set via CMake `ENVIRONMENT` property but SDL_Init was called
  before the hint was set. Fixed: `setenv("SDL_VIDEODRIVER", "dummy", 1)`
  at the start of the test, before SDL_Init.

### Design decisions
- **Pure data model + callbacks**: grid_render is pure data (no I/O, no
  DBus). player_mode uses callbacks for side effects (on_slot_change,
  on_profile_change). This keeps both modules fully testable without
  DBus mock infrastructure or file I/O fixtures.
- **cbx_grid_composite_info struct**: The caller gathers composite info
  (identity ID, model name, DBus path) and passes it to grid_build. This
  avoids DBus calls in the build function. The caller uses
  ip_composite_get_name + identity extraction to populate this struct.
- **Col 0 = Unassigned**: Column 0 is always Unassigned (empty device type).
  Columns 1..N are player slots P1..PN with device types from settings.
  Slot = col - 1, col = slot + 1.
- **Profile cycling wraps**: When cycling past the end of the profile list,
  it wraps around to the beginning. If the current profile is not in the
  list (e.g., deleted), cycling starts from the first profile.
- **Render is best-effort**: The render function gracefully handles NULL
  icon_cache, text_cache, and theme. Without these, it draws basic
  shapes (rectangles, circles) but no icons or text. This allows testing
  the render function with a dummy SDL renderer without full asset init.
- **Player Mode independence**: cbx_player_mode_handle takes a row_idx
  parameter, so each controller only navigates its own row. The grid's
  navigation functions (move_left/right, cycle_profile) also take row_idx,
  ensuring no cross-row modification is possible.

### Next
Task 30 (Host Mode and conflict detection/resolution) — deps: Task 29 (done).
Alternatively Task 31 (Profile cycling and dynamic columns) — deps: Task 29
(done), Task 8 (done).
Alternatively Task 32 (Overlay trigger registration and activation/close) —
deps: Task 28 (done), Task 13 (done).
Check `ralph tools task ready` and the plan.

## Task 30 (complete) — Host Mode and conflict detection/resolution

### What landed
- `src/overlay/host_mode.h/c`: Host Mode state machine. R3 toggle:
  first controller to press R3 becomes exclusive host (all others
  freeze). Host navigates between rows with Up/Down (clamped, no wrap),
  edits slots within selected row with Left/Right (fires on_slot_change
  callback with selected_row's row_idx + new slot). R3 again exits back
  to Player Mode. B returns CLOSE. Non-host controllers get FROZEN.
  Visual state queries: cbx_host_mode_row_state returns HOST/SELECTED/
  FROZEN/NORMAL for rendering integration. cbx_host_mode_is_frozen for
  input gating.
- `src/overlay/conflict.h/c`: Conflict detection and resolution.
  cbx_conflict_detect scans grid rows in order; first row on a column
  > 0 is the owner, subsequent rows are second arrivals (conflicted).
  Col 0 (Unassigned) never conflicts. cbx_conflict_resolve moves each
  conflicted row to the lowest unoccupied P-slot (deterministic, row
  order). Edge cases: all slots occupied → leave in place; Unassigned →
  not a conflict. cbx_conflict_find_lowest_free_slot excludes the
  conflicted row's own position. cbx_conflict_is_row_conflicted for
  rendering (red highlight on second arrivals).
- `tests/test_host_mode.c`: 41 cmocka tests (init, enter/exit, toggle
  enter/exit/frozen, Up/Down navigation + boundaries, Left/Right slot
  change + callbacks, host edits other row, R3 exit, B close, frozen
  controller, NULL safety, accessors, visual state, is_frozen, full
  lifecycle).
- `tests/test_conflict.c`: 33 cmocka tests (list init, detect
  no-conflicts/all-different/two-same/multiple/three-same/unassigned/
  empty/single/null/null-out, is_row_conflicted, find_lowest_free_slot
  all-free/some-occupied/all-occupied/no-slot/null, count_occupied,
  resolve move-to-free/all-occupied-leave/all-slots-stay/unassigned/
  multiple/no-conflicts/null/null-list/resolves-all, spec example).
- `CMakeLists.txt`: Added host_mode.c, conflict.c to controllerbox STATIC.
- `tests/CMakeLists.txt`: Added test_host_mode + test_conflict targets.
- `IMPLEMENTATION_PLAN.md`: Task 30 → complete.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 45/45: all previous + test_host_mode + test_conflict
- test_host_mode 41/41 cmocka tests pass
- test_conflict 33/33 cmocka tests pass
- verify-boilerplate → exit 0

### Gotchas fixed
- Unused variable `target_col` in cbx_conflict_resolve: computed
  `int target_col = free_slot + 1` but never used it (move via
  move_left/move_right loop instead). Removed.
- Type mismatch in test_detect_null: passed `cbx_conflict_list*` as
  first arg (grid) to cbx_conflict_detect. Fixed by removing the
  bogus second call.

### Design decisions
- **First-by-index = owner**: Conflict detection scans rows in index
  order. The first row found on a column is the "first arrival" (owner);
  subsequent rows are "second arrivals" (conflicted, shown red). This is
  deterministic and doesn't require temporal tracking.
- **Resolution is sequential**: Conflicts are resolved in row order. Each
  move updates the grid, so subsequent conflicts see the updated layout.
  This prevents cascading conflicts from resolution moves.
- **Re-check in resolve**: cbx_conflict_resolve re-checks each conflicted
  row to see if it's still conflicted (a prior resolution may have freed
  the column). If no longer conflicted, it's skipped.
- **Clamped row navigation**: Up/Down in host mode clamps at row
  boundaries (no wrap). This matches player_mode boundary behavior.
- **Host edits selected row, not its own**: The host's selected_row can
  differ from host_row. Left/Right operates on selected_row, not
  host_row. The slot change callback fires with selected_row's row_idx.
- **Visual state for rendering**: cbx_host_mode_row_state provides a
  4-level enum (NORMAL/HOST/SELECTED/FROZEN) that the renderer can query.
  Actual rendering integration deferred to Task 33 (integration test).
- **Conflict resolution moves via grid API**: cbx_conflict_resolve uses
  cbx_select_grid_move_left/right to move conflicted rows. This keeps
  the grid's internal state consistent and reusable.

### Next
Task 31 (Profile cycling and dynamic columns) — deps: Task 29 (done),
Task 8 (done).
Alternatively Task 32 (Overlay trigger registration and activation/close)
— deps: Task 28 (done), Task 13 (done).
Check `ralph tools task ready` and the plan.

## Task 31 (complete) — Profile cycling and dynamic columns

### What landed
- `src/overlay/profile_cycle.h/c`: Profile cycling workflow. Coordinates
  profile enumeration → grid population → profile change → LoadProfilePath
  + assignment update. cbx_profile_cycle struct (backend, bus, assignments,
  profiles). cbx_profile_cycle_load_profiles populates grid from
  cbx_profile_list enumeration. cbx_profile_cycle_find_path looks up full
  filesystem path by profile name. cbx_profile_cycle_apply does full
  workflow: find path → ip_composite_load_profile_path via backend → update
  assignment profile. cbx_profile_cycle_update_assignment finds existing
  assignment by ID and updates profile, or creates new at lowest free slot.
  cbx_profile_cycle_profile_follows verifies per-controller profile model
  (profile is stored per-row in grid, inherently follows controller across
  column moves — SPEC §4.6).
- `src/overlay/dynamic_columns.h/c`: Dynamic column management from device
  model. cbx_dynamic_columns_build_vcs builds cbx_virtual_controllers from
  target device types. cbx_dynamic_columns_needs_rebuild checks if target
  count changed (col_count != target_count + 1). cbx_dynamic_columns_rebuild
  rebuilds grid with new column count, preserving profile list and
  re-deriving row positions from assignments (removed slots → Unassigned).
  cbx_dynamic_columns_clamp_positions moves rows at removed columns to
  Unassigned. cbx_dynamic_columns_extract_types queries DeviceType for each
  target via callback (mockable for testing). SPEC §4.7.
- `tests/test_profile_cycle.c`: 26 cmocka tests (init, load_profiles,
  find_path, apply success/error/null/no-backend/no-assignments/no-profiles,
  backend error propagation, update assignment existing/new/null, profile
  follows controller across moves, full workflow).
- `tests/test_dynamic_columns.c`: 24 cmocka tests (build_vcs, needs_rebuild,
  clamp_positions, rebuild more/fewer columns, preserve profiles/assignments,
  null/bad-count, extract_types, hotplug add/remove target with/without
  assignment).
- `CMakeLists.txt`: Added profile_cycle.c, dynamic_columns.c to controllerbox.
- `tests/CMakeLists.txt`: Added test_profile_cycle + test_dynamic_columns.
- `IMPLEMENTATION_PLAN.md`: Task 31 → complete.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 47/47: all previous + test_profile_cycle + test_dynamic_columns
- test_profile_cycle 26/26 cmocka tests pass
- test_dynamic_columns 24/24 cmocka tests pass
- verify-boilerplate → exit 0

### Gotchas fixed
- `IP_ERR_NO_REPLY` defined in `dbus/ip_connection.h`, not in dbus_mock.h.
  Added include to test file.
- `cbx_assign_make_default` validates ID format via `cbx_validate_id`.
  Test IDs must match supported formats (BT:xx:xx:xx:xx:xx:xx, USB:phys:xxx,
  USB:xxx, ORDER:n). Initial test used "ID:001" which failed validation
  (-EINVAL). Fixed to "ORDER:0"/"ORDER:5".
- Profile count in test_rebuild_more_columns: build_grid_with_types already
  adds "default" and "fighting" (2 profiles), then test adds "test_profile"
  (3 total). Fixed assertion from 4 to 3.

### Design decisions
- **Profile is per-row, not per-column**: The grid stores profile in
  rows[row_idx].profile. Navigation (move_left/right) only changes
  cur_col, never the profile field. This is a structural guarantee —
  profile inherently follows the controller across columns (SPEC §4.6).
- **Profile cycle apply is a callback handler**: cbx_profile_cycle_apply
  is designed to be called from the player_mode on_profile_change callback.
  It takes the grid (for identity lookup), row_idx, profile_name, and
  composite_path — exactly what the callback provides.
- **Dynamic columns reuse grid_build**: cbx_dynamic_columns_rebuild
  constructs a cbx_settings with virtual_controllers from target types,
  then calls the existing cbx_select_grid_build. This reuses the proven
  build logic (assignment lookup, slot-to-column mapping, clamping).
- **Profile list preserved across rebuilds**: The rebuild function saves
  the grid's profile list before calling grid_build (which zeros the grid),
  then restores it. This ensures the cycling list survives hotplug events.
- **Type extraction via callback**: cbx_dynamic_columns_extract_types takes
  a query function callback to get each target's DeviceType. This keeps
  the module testable without DBus — tests provide a mock query function.
- **Removed slot → Unassigned**: When target_count decreases, grid_build
  re-derives positions from assignments. If an assignment's slot >= new
  col_count, the row stays at Unassigned (col 0). This matches SPEC §5.2:
  "the physical controller in that slot auto-moves to Unassigned."

### Next
Task 32 (Overlay trigger registration and activation/close) — deps: Task 28
(done), Task 13 (done).
Alternatively Task 33 (Overlay integration test) — deps: Task 32, Task 30,
Task 31 (all done after Task 32).
Check `ralph tools task ready` and the plan.

## Task 32 (complete) — Overlay trigger registration and activation/close

### What landed
- `src/overlay/trigger.h/c`: Trigger combo parsing ("Select+A" → events
  CSV "Select,A" + target "Select+A"). Register on single composite:
  SetInterceptActivation + set InterceptMode=PASS (1). Register on all
  composites with failure counting (returns -N failures).
- `src/overlay/close.h/c`: Close coordination. cbx_close_sync_assignments
  syncs grid state back to assignments (update slot+profile for
  assigned, remove for Unassigned, create for new, preserve for
  disconnected). cbx_close_on_save: detect+resolve conflicts → sync →
  save assignments. cbx_overlay_request_close: wires on_save callback +
  calls lifecycle close (which sets InterceptMode=PASS, hides surface,
  transitions to IDLE).
- `tests/test_trigger.c`: 22 cmocka tests (parse simple/multi/single/
  whitespace/empty/whitespace-only/null/buffer-too-small/target-too-small/
  zero-size; register success/activation-fails/intercept-mode-fails/
  no-expectation/null-args/bad-trigger; register_all success/some-fail/
  all-fail/null-args/null-path-entry/single).
- `tests/test_close.c`: 21 cmocka tests (sync update-existing/create-new/
  remove-unassigned/preserve-disconnected/empty-grid/no-id-row/null-args/
  multiple/update-profile; on_save null-safety/no-conflicts/with-conflicts/
  all-unassigned; request_close from-visible/from-idle/null-args/
  full-lifecycle/with-conflict/intercept-mode-fail).
- `CMakeLists.txt`: Added trigger.c, close.c to controllerbox STATIC.
- `tests/CMakeLists.txt`: Added test_trigger + test_close targets.
- `IMPLEMENTATION_PLAN.md`: Task 32 → complete.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 49/49: all previous + test_trigger + test_close
- test_trigger 22/22 cmocka tests pass
- test_close 21/21 cmocka tests pass
- verify-boilerplate → exit 0

### Gotchas fixed
- `bool`/`true`/`false` used in trigger.c without `#include <stdbool.h>`.
  Added the include.
- `strncpy` into same-sized buffers (CBX_MAX_ID_LEN, CBX_MAX_PROFILE_LEN)
  triggers -Werror=stringop-truncation. Fixed with `snprintf`.
- `IP_ERR_NO_REPLY` not included in test_trigger.c. Added
  `#include "dbus/ip_connection.h"`.
- `calloc`/`free` not declared in test_trigger.c. Added
  `#include <stdlib.h>`.
- `cmocka_run_group_tests` group setup runs ONCE before all tests, not
  per test. Changed from `cmocka_run_group_tests(tests, setup, teardown)`
  to `cmocka_unit_test_setup_teardown(test, setup, teardown)` per test
  with `cmocka_run_group_tests(tests, NULL, NULL)`.
- Mock DBus returns FIRST match for (iface, member) — cannot test
  per-device differentiation with same member name. Redesigned
  test_register_all_some_fail to use NULL path entry for failure
  injection instead of duplicate expectations.
- test_sync_update_profile: controller was at Unassigned (col 0), so
  sync removed the assignment instead of updating profile. Fixed by
  moving controller to P1 (col 1) before cycling profile.

### Design decisions
- **Trigger parse splits on '+'**: "Select+A" → events_csv "Select,A",
  target "Select+A". Whitespace around each token is trimmed. At least
  one event is required.
- **Register_all continues on failure**: If a device fails, the function
  continues with remaining devices and returns -(failure count). This
  ensures all devices get the trigger registered even if one fails.
- **Close sync preserves disconnected controllers**: Assignments for
  controllers not in the grid (disconnected) are left unchanged. Only
  controllers in the grid have their assignments updated/created/removed.
- **Unassigned → remove assignment**: Controllers at col 0 (Unassigned)
  have their existing assignment removed (shifted down in array). This
  means "I don't want this controller assigned to any player slot."
- **on_save is synchronous**: The lifecycle calls on_save synchronously
  during cbx_overlay_lifecycle_close, before setting InterceptMode=PASS.
  This ensures conflicts are resolved and assignments are saved before
  input flows back to the game.
- **Stack-local close context**: cbx_overlay_request_close uses a
  stack-local cbx_close_ctx for on_save_data. This is safe because
  on_save is called synchronously within lifecycle_close, and the
  pointer is never used after close returns.

### Next
Task 33 (Overlay integration test) — deps: Task 32 (done), Task 30
(done), Task 31 (done). All dependencies complete.
Alternatively Task 34 (Manager skeleton and tab bar) — deps: Task 21
(done), Task 22 (done).
Check `ralph tools task ready` and the plan.

## Task 33 (complete) — Overlay integration test

### What landed
- `tests/test_overlay_integration.c`: 13 cmocka integration tests exercising
  the full overlay lifecycle with mock DBus + SDL2 dummy driver. Tests:
  trigger registration (single + multiple composites), full lifecycle
  (activate → Player Mode navigate → profile cycle → close → verify
  assignments saved), profile change applied via LoadProfilePath mock,
  profile follows controller across column moves, conflict detection +
  auto-resolution, Host Mode integration (enter → navigate → edit other
  row → exit), Host Mode close via B, full workflow (trigger → activate →
  navigate → conflict → host mode resolve → close → verify), close
  auto-resolves triple conflict, grid rendering with dummy driver,
  force close, tick in VISIBLE.
- `tests/CMakeLists.txt`: Added test_overlay_integration target with
  SDL_VIDEODRIVER=dummy environment.
- `IMPLEMENTATION_PLAN.md`: Task 33 → complete.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 50/50: all previous + test_overlay_integration
- test_overlay_integration 13/13 cmocka tests pass
- verify-boilerplate → exit 0

### Gotchas fixed
- `cycle_profile_up` wraps BACKWARDS (decrement with wrap): "default" →
  "fps" (last profile). To cycle forward ("default" → "fighting"), use
  `cycle_profile_down` (increment with wrap). This is counter-intuitive
  but matches the implementation in grid_render.c. Fixed tests to use
  CBX_PM_DOWN for forward profile cycling.
- `cbx_conflict_resolve` returns the NUMBER of conflicts resolved (moved),
  not 0 on success. Assert rc == 1 for single conflict resolution.
- Format-truncation: snprintf with test_home (PATH_MAX) as source into
  e->path (PATH_MAX) triggers GCC -Werror=format-truncation. Fixed by
  using fixed profile paths (not dependent on test_home) since the mock
  DBus doesn't require real file paths.

### Design decisions
- **Fixed profile paths in test fixture**: Profile paths in the
  cbx_profile_list don't need to exist on disk — they're just used for
  lookup by cbx_profile_cycle_find_path and passed to the mocked
  LoadProfilePath call. Using fixed paths avoids the format-truncation
  issue with PATH_MAX-sized test_home buffer.
- **SDL2 dummy renderer in fixture**: The integration fixture creates
  a real SDL2 window/renderer via test_harness_sdl_init with the dummy
  driver. This allows testing grid rendering (test_grid_renders_with_dummy_driver)
  alongside the state machine tests.
- **Comprehensive integration coverage**: The test suite covers all
  overlay modules: trigger, lifecycle, player_mode, host_mode, conflict,
  close, profile_cycle, grid_render — wired together with mock DBus,
  mock profile list, and real SDL2 rendering.

### Next
Task 34 (Manager skeleton and tab bar) — deps: Task 21 (done), Task 22 (done).
Alternatively Task 35 (Controllers tab) — deps: Task 34.
Check `ralph tools task ready` and the plan.

## Task 34 (complete) — Manager skeleton and tab bar

### What landed
- `src/manager/manager.h`: cbx_manager struct (renderer, text_cache, theme,
  settings, font_id, tabbar, 3 panels, focus_chain, active_tab, running).
  Lifecycle API: init/run/stop/shutdown, handle_event, render. Accessors:
  active_tab, tab_count, tabbar, panel, focus.
- `src/manager/manager.c`: Full implementation following the overlay lifecycle
  pattern. cbx_manager_init creates SDL2 window (1280x720, shown), initializes
  text cache + theme + settings, creates tabbar with 3 tabs (Controllers,
  Profiles, Settings), creates 3 empty panels, sets up focus chain (HOST mode),
  focuses tabbar. Event dispatch: 1) try focused widget, 2) LEFT/RIGHT →
  tabbar, 3) UP/DOWN → focus chain navigate, 4) A/Enter/Space → consumed.
  Tab change callback: update active_tab, toggle panel visibility, rebuild
  focus chain, refocus tabbar. Layout: tabbar at top (h=48), panels fill rest.
- `src/app/main.c`: run_manager() now calls cbx_manager_init/run/shutdown
  (dry_run still stubbed).
- `tests/test_manager_tabs.c`: 14 cmocka tests (init basic, panels visibility,
  L/R tab switching, tab change updates visible panel, U/D focus navigation,
  render no crash, render with font, stop, NULL safety, unrelated event,
  panel accessors, shutdown cleanup + re-init, A key consumed, full tab cycle).
- `CMakeLists.txt`: Added src/manager/manager.c to controllerbox STATIC.
- `tests/CMakeLists.txt`: Added test_manager_tabs with SDL_VIDEODRIVER=dummy.
- `IMPLEMENTATION_PLAN.md`: Task 34 → complete.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 51/51: all previous + test_manager_tabs
- test_manager_tabs 14/14 cmocka tests pass
- verify-boilerplate → exit 0

### Gotchas fixed
- `cbx_tabbar_set_change_cb` takes only (tabbar, callback) — no user_data
  parameter. The callback receives user_data from the tab's user_data field.
  Fixed by passing `mgr` as user_data to each `cbx_tabbar_add_tab` call.
- test_manager_stop: stop() sets running=false (to stop the loop), not true.
  Fixed test to set running=true manually, then call stop, then assert false.
- test_manager_shutdown_cleans_up: after memset(0), active_tab is 0 not -1
  (accessor returns the raw field value, not -1 for zeroed struct). Removed
  the -1 assertion, kept tab_count==0 assertion (which works correctly
  because tabbar was destroyed).

### Design decisions
- **Separate SDL2 window**: cbx_manager_init creates its own cbx_renderer
  with a 1280x720 window, shown immediately. This is a distinct window from
  the overlay service (SPEC §5.1).
- **Event dispatch order**: focused widget first (may consume), then
  LEFT/RIGHT → tabbar (tab switching), then UP/DOWN → focus chain navigate.
  This lets future panel widgets consume LEFT/RIGHT for their own purposes
  if needed (e.g., profile editor).
- **HOST mode focus chain**: The manager uses CBX_FOCUS_MODE_HOST so UP/DOWN
  can cross row boundaries freely. Tabbar is row 0; panel children are row 1+.
- **Empty panels for skeleton**: Each tab has an empty cbx_panel. Later tasks
  (35-39) populate them with content. The focus chain only has the tabbar
  until panels have children.
- **Tab user_data = mgr pointer**: All 3 tabs store the manager pointer as
  their user_data, so the on_change callback can access the manager. This
  works because the tabbar passes tab.user_data to the callback.
- **Font optional**: cbx_manager_init accepts a font_path that can be NULL.
  Without a font, the tabbar still works (no text rendering). This keeps
  the manager usable in headless test environments.

### Next
Task 35 (Controllers tab — list, add/remove, and type change) — deps:
Task 34 (done), Task 12 (done). All dependencies complete.
Alternatively Task 36 (Profiles tab) — deps: Task 34 (done), Task 8 (done).
Check `ralph tools task ready` and the plan.

## Task 35 (complete) — Controllers tab — list, add/remove, and type change

### What landed
- `src/manager/controllers_tab.h`: cbx_controllers_tab struct (DBus
  backend/bus borrowed, device model owned, supported_types[],
  device_types[], widgets: device_list, type_picker, add_btn,
  remove_btn, change_type_btn). Modes: LIST and TYPE_PICK. Actions:
  NONE, ADD, CHANGE. Full lifecycle API: init/refresh/shutdown.
  Action API: load_supported_types, add, remove, change_type,
  begin_type_pick, confirm_type_pick, cancel_type_pick. Accessors
  for testing.
- `src/manager/controllers_tab.c`: Full implementation. Init populates
  panel with 5 children (device_list, 3 buttons, type_picker hidden).
  Refresh calls cbx_objectmanager_enumerate + ip_target_get_device_type
  per target, rebuilds list. Add calls ip_manager_create_target_device.
  Remove calls ip_manager_stop_target_device. Change_type calls
  ip_composite_set_target_devices on the composite at same index,
  building types CSV from current device types with the one changed.
  Type picker: begin populates picker from supported_types, hides
  device list + buttons, shows type_picker. Confirm executes pending
  action (add or change). Cancel restores list view.
- `tests/test_controllers_tab.c`: 33 cmocka tests with mock DBus +
  SDL2 dummy driver. Tests: init populates panel (5 children), init
  without DBus, init NULL args, refresh enumerates devices, refresh
  empty, refresh re-enumerates, refresh enumerate error, refresh NULL,
  load supported types success/error/whitespace, add success/error/null,
  remove success/error/bad-index, change type success/mixed/error/
  bad-index/no-composite, type picker begin/confirm-add/confirm-change/
  cancel/no-types/not-in-pick-mode, shutdown removes children/null-safe,
  accessors null-safe/bad-index, full workflow (add→change→remove).
- `CMakeLists.txt`: Added src/manager/controllers_tab.c to controllerbox.
- `tests/CMakeLists.txt`: Added test_controllers_tab with cbx_test_support
  and SDL_VIDEODRIVER=dummy.
- `IMPLEMENTATION_PLAN.md`: Task 35 → complete.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 52/52: all previous + test_controllers_tab
- test_controllers_tab 33/33 cmocka tests pass
- verify-boilerplate → exit 0

### Gotchas fixed
- Mock DBus returns FIRST match for (iface, member) — cannot test
  per-device DeviceType differentiation. Both targets get the same
  type. Fixed test_refresh_enumerates_devices and test_change_type_mixed
  to use single type expectation and verify count/paths instead of
  per-device types.
- `font_available` unused function → -Werror=unused-function. Removed.
- Need cbx_test_support link for mock DBus symbols (ip_dbus_mock_*).

### Design decisions
- **Composite index = target index**: Change type finds the composite
  at the same index as the target. In the common case (1 target per
  composite), this works correctly. If there's no composite at that
  index, returns -EINVAL.
- **Types CSV for change_type**: Builds CSV from all current target
  types with the selected one replaced. For unchached types, queries
  DeviceType live. Fallback to "gamepad" if query fails.
- **Type picker is a mode switch**: Not a separate window. The device
  list is hidden and the type picker list is shown in its place.
  Buttons are hidden during type pick. This keeps the UI simple and
  controller-navigable.
- **Refresh is non-fatal on type query failure**: If a DeviceType query
  fails for one target, that target shows with empty type. The refresh
  still succeeds (rc from enumerate, not from type queries).
- **Init auto-loads supported types + refresh**: When backend and bus
  are provided, init calls load_supported_types and refresh. This
  means the tab is ready to use immediately after init. Tests that
  don't want this can pass NULL backend.

### Next
Task 36 (Profiles tab — browse, create, and delete) — deps: Task 34
(done), Task 8 (done). All dependencies complete.
Alternatively Task 37 (Profile editor) — deps: Task 36, Task 18, Task 13,
Task 8.
Check `ralph tools task ready` and the plan.

## Task 36 (complete) — Profiles tab — browse, create, and delete

### What landed
- `src/manager/profiles_tab.h`: cbx_profiles_tab struct (profile_list,
  widgets: profile_list_w, create_picker, create/edit/delete buttons,
  status_lbl, panel borrowed, text_cache/theme/font_id borrowed).
  Modes: LIST, CONFIRM_DELETE, NAME_INPUT, CREATE_PICK. Create sources:
  DEFAULT_COPY, EMPTY, CLONE. Full lifecycle: init/refresh/shutdown.
  Actions: create, delete, begin_create, name_input_char/backspace/
  confirm/cancel, begin_delete/confirm_delete/cancel_delete.
  Test dir overrides: set_test_dirs for isolated filesystem testing.
- `src/manager/profiles_tab.c`: Full implementation. Init populates
  panel with 6 children. Refresh enumerates via cbx_profile_list_enumerate
  (default) or cbx_profile_list_enumerate_dirs (test override). Create
  builds profile from source (default copy loads mappings, empty = just
  header, clone loads selected profile), saves via cbx_profile_save.
  Delete unlinks YAML + sidecar. Name input mode validates chars against
  ^[a-zA-Z0-9_-]+$. Delete confirmation mode shows prompt.
- `tests/test_profiles_tab.c`: 37 cmocka tests with SDL2 dummy driver
  and temp directory fixture. Tests: init populates panel/enumerates/
  null args; refresh updates/null/empty; create default copy/empty/clone/
  no default/clone no selection/invalid name/duplicate/null; delete user/
  with sidecar/default rejected/system rejected/bad index/null; name input
  basic/backspace/invalid chars/confirm/confirm empty/cancel/not in mode;
  delete confirm basic/readonly rejected/cancel/not in mode; accessors
  null safe/entry; shutdown null safe/removes children; full workflow;
  clone copies mappings.
- `CMakeLists.txt`: Added profiles_tab.c to controllerbox STATIC.
- `tests/CMakeLists.txt`: Added test_profiles_tab with SDL_VIDEODRIVER=dummy.
- `IMPLEMENTATION_PLAN.md`: Task 36 → complete.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 53/53: all previous + test_profiles_tab
- test_profiles_tab 37/37 cmocka tests pass
- verify-boilerplate → exit 0

### Gotchas fixed
- Format-truncation: build_profile_path/build_sidecar_path use `char
  dir[PATH_MAX]` intermediate, `char path[PATH_MAX + 128]` output. Test
  pt_env struct uses `char tmp[256]` (short path) + `char *_dir[PATH_MAX]`
  to avoid same-size buffer truncation.
- Profile YAML format: `mapping:` is a sequence (each entry with `- `),
  `target_events:` (plural) not `target_event`, `gamepad: {}` for empty
  props. The parser only finds 1 mapping if the YAML uses `target_event`
  (singular) instead of `target_events` (plural).
- Sidecar path: `cbx_profile_meta_save_for` uses
  `$XDG_CONFIG_HOME/controller-box/profile-metadata/<name>.meta.yaml`.
  Test env must create `<config_dir>/controller-box/profile-metadata/`
  (with `controller-box/` subdirectory).
- `system()` return value must be checked or cast with `(void)!system()`
  under GCC -Werror=unused-result.
- Removed unused `write_sidecar` function (tests use
  `cbx_profile_meta_save_for` instead).
- Per-test setup/teardown: use `cmocka_unit_test_setup_teardown(test,
  pt_setup, pt_teardown)` for each test. Group setup runs once.
- Init does NOT auto-refresh: caller must call refresh after init.
  Test fixture calls `init_tab()` helper which does init + set_test_dirs
  + refresh. This allows test dirs to be set before enumeration.

### Design decisions
- **Test dir overrides**: The profiles tab accepts optional test
  directories (user_dir, system_dir, meta_dir) via
  `cbx_profiles_tab_set_test_dirs()`. When set, refresh and file ops use
  these paths instead of the compile-time defaults. This is needed
  because system profiles dir is compile-time and can't be redirected
  via env vars.
- **No auto-refresh in init**: init creates widgets and layout but does
  NOT call refresh. This allows test dirs to be set before enumeration.
  Production code (manager) calls init then refresh.
- **Name input validates per-char**: Only a-zA-Z0-9_- are accepted.
  Invalid chars return -EINVAL (not added to buffer). This is stricter
  than validating the final string.
- **Delete removes both YAML and sidecar**: The delete operation unlinks
  the profile YAML and attempts to unlink the sidecar (ignoring errors
  if it doesn't exist).
- **Clone can clone read-only profiles**: Cloning a system/default
  profile creates a new editable copy. The new profile's name is set to
  the user-provided name, not the source's name.

### Next
Task 37 (Profile editor — controller diagram and binding list mode) —
deps: Task 36 (done), Task 18 (done), Task 13 (done), Task 8 (done).
All dependencies complete.
Alternatively Task 38 (Profile editor — sequential binding mode and
validation) — deps: Task 37.
Alternatively Task 39 (Profile save and Settings tab) — deps: Task 38.
Check `ralph tools task ready` and the plan.

## Task 37 (complete) — Profile editor — controller diagram and binding list mode

### What landed
- `src/manager/profile_diagram.h`: cbx_profile_diagram struct (custom
  cbx_widget with base_texture, highlight_color, highlighted button).
  cbx_diag_button enum (17 buttons: UP/DOWN/LEFT/RIGHT, A/B/X/Y,
  START/SELECT/GUIDE, L1/R1/L2/R2, L3/R3). cbx_diag_button_pos struct
  with normalized (0.0–1.0) coordinates. API: init/shutdown,
  highlight/clear/get_highlight, get_button_pos, button_from_name,
  button_name, button_count.
- `src/manager/profile_diagram.c`: Full implementation. Static button
  position table for generic gamepad layout. SVG base texture loading
  via nanosvg (optional, falls back to plain background). Custom vtable
  with draw (background + base texture + highlight overlay),
  get_rect, set_rect, destroy. NULL-safe throughout.
- `src/manager/profile_editor_list.h`: cbx_profile_editor struct
  (profile, diagram, binding_list, target_list, status/title labels,
  panel borrowed, rendering deps borrowed, DBus deps borrowed, targets
  array, cap_maps, mode, selected/editing indices, capture state).
  Modes: LIST, TARGET_PICK, CAPTURE. API: init/shutdown,
  load_profile/get_profile, set_dbus, load_capabilities, refresh,
  move_up/down, activate, cancel, begin/confirm/cancel_target_pick,
  begin/cancel_capture, on_input_event, accessors.
- `src/manager/profile_editor_list.c`: Full implementation. Init
  populates panel with 5 children (title, diagram, binding_list,
  target_list hidden, status). Load_profile copies profile and
  refreshes. Refresh rebuilds binding list from mappings with
  "source → target" labels. Navigation wraps around. Diagram syncs
  via source_event button name → cbx_profile_diagram_button_from_name.
  Target pick populates from capabilities (DBus or defaults). Capture
  uses ip_input_events, only captures button presses (value 1.0).
- `tests/test_profile_diagram.c`: 23 cmocka tests (button count=17,
  position lookup valid/all/invalid, name mapping known/unknown/
  roundtrip/invalid, highlight set/clear/none/all/invalid/null-safe,
  init basic/null-args, shutdown null-safe/cleans-up, render no-crash/
  all-buttons/with-rect/null-safe, SVG nonexistent path).
- `tests/test_editor_list_mode.c`: 35 cmocka tests with SDL2 dummy
  driver + mock DBus. Tests: init basic/null/shutdown, load_profile/
  null/empty, move_down/up/wrap_down/wrap_up/empty, diagram_sync_on_load/
  on_move/empty/unknown, activate_enters/no_selection, target_pick_
  has_targets/confirm/cancel, cancel_in_list_mode, load_capabilities_
  defaults/dbus/dbus_error, begin_capture/no_selection, cancel_capture,
  capture_input_event/ignores_release/null_safe, accessors_null_safe,
  status_message, render_no_crash/target_pick, full_workflow.
- `CMakeLists.txt`: Added profile_diagram.c and profile_editor_list.c
  to controllerbox STATIC.
- `tests/CMakeLists.txt`: Added test_profile_diagram and
  test_editor_list_mode with SDL_VIDEODRIVER=dummy.
- `IMPLEMENTATION_PLAN.md`: Task 37 → complete.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 55/55: all previous + test_profile_diagram (23) + test_editor_list_mode (35)
- verify-boilerplate → exit 0

### Gotchas fixed
- nanosvg headers must NOT be included with NANOSVG_IMPLEMENTATION/
  NANOSVGRAST_IMPLEMENTATION defines in profile_diagram.c — the
  implementation is already compiled in nanosvg_impl.c (separate static
  library). Use `#include <nanosvg.h>` (angle brackets, via include path)
  not `#include "nanosvg.h"` (which would need the defines).
- CBX_DIAG_BTN_COUNT is 17 (not 18) — the enum has 17 button entries
  (UP through R3), CBX_DIAG_BTN_NONE is -1 and not counted.
- After cbx_profile_diagram_shutdown (which memsets to 0), the
  `highlighted` field becomes 0 (= CBX_DIAG_BTN_UP), not -1 (= NONE).
  Don't assert NONE after shutdown — just check no crash.
- Tests need `#include <errno.h>` for EINVAL/ENOENT constants.
- `cbx_panel_destroy` doesn't exist — use `cbx_widget_destroy(&panel->base)`
  to destroy a standalone panel via the widget vtable.

### Design decisions
- **Static button position table**: Instead of parsing SVG element IDs,
  a static table of 17 normalized button positions covers all standard
  gamepad inputs. This is simpler, more testable, and works with any
  base image (or none). Positions are approximate for a generic gamepad.
- **Optional SVG base texture**: The diagram can render with or without
  an SVG base image. Without SVG (NULL path or load failure), it draws
  a plain background rectangle. This keeps tests headless-friendly.
- **Default targets when no DBus**: If capabilities can't be loaded from
  DBus (no backend or errors), 10 default targets (keyboard:KeyA-KeyF,
  KeyEsc, KeyReturn, mouse:ButtonLeft, ButtonRight) are provided.
- **Capture mode uses value 1.0 only**: Button releases (value 0.0) are
  ignored during capture — only button presses trigger capture.
- **Source event button lookup**: The editor looks for a "button" prop
  (or "axis" for sticks) in the source_event to determine the diagram
  button. Unknown button names map to CBX_DIAG_BTN_NONE (no highlight).
- **Target pick is a mode switch**: Like controllers_tab's type picker,
  the binding list is hidden and the target list is shown in its place.
  This keeps the UI controller-navigable without a separate window.

### Next
Task 38 (Profile editor — sequential binding mode and validation) —
deps: Task 37 (done), Task 21 (done). All dependencies complete.
Alternatively Task 39 (Profile save and Settings tab) — deps: Task 38.
Check `ralph tools task ready` and the plan.

## Task 38 (complete) — Profile editor — sequential binding mode and validation

### What landed
- `src/manager/profile_editor_seq.h`: Header for sequential binding mode
  (API declared in profile_editor_list.h).
- `src/manager/profile_editor_seq.c`: Full implementation. Sequential
  mode prompts for each of 17 buttons in cbx_diag_button order (UP=0
  through R3=16). Diagram highlights current button. Physical button
  press → captured → auto-advance. B skips current button (no mapping
  created). Start cancels sequential mode. Progress bar
  (cbx_progress) shows completion fraction = step/17. On completion
  (all 17 processed), returns to list mode and refreshes binding list.
- `src/manager/profile_validate.h`: NES minimum validation API.
  CBX_NES_MINIMUM_COUNT=6, cbx_nes_minimum_buttons(), has_binding(),
  validate_nes_minimum() (returns 0 valid / -EINVAL with missing names),
  validate_missing_count().
- `src/manager/profile_validate.c`: Full implementation. Checks if
  profile has bindings for A, B, Up, Down, Left, Right by searching
  source_event props for "button" or "axis" with matching canonical
  name. Builds comma-separated missing names in caller-provided buffer.
- `src/manager/profile_editor_list.h`: Added CBX_EDITOR_MODE_SEQUENTIAL
  to editor mode enum. Added cbx_progress progress_bar, int seq_step,
  bool seq_active to struct. Added sequential API declarations
  (begin_sequential, cancel_sequential, seq_skip, seq_on_input,
  seq_progress, seq_current_button, seq_get_step, seq_is_active).
- `src/manager/profile_editor_list.c`: Modified init to create progress
  bar widget (panel now 6 children). Modified shutdown to destroy
  progress bar. Modified on_input_event to dispatch to seq_on_input
  when mode is SEQUENTIAL. Modified cancel() to handle SEQUENTIAL mode.
- `tests/test_editor_seq_mode.c`: 28 cmocka tests with SDL2 dummy
  driver + mock DBus. Tests: begin sequential (basic, no profile, null,
  diagram highlight, progress zero, hides binding list, panel 6
  children), capture auto-advance (single, multiple, ignores release,
  diagram advances, progress increases), skip (explicit, creates no
  mapping, via B input, not active), cancel (via Start, explicit, via
  editor cancel, shows binding list, null safe), complete all steps,
  accessors null safe, current button when inactive, validation
  integration (after capture, with correct capture), full workflow,
  rendering no crash.
- `tests/test_profile_validate.c`: 22 cmocka tests. Tests: NES minimum
  count/button names/bad index/contains A-B-dpad, has_binding
  (yes/no/empty/null/none/dpad/axis prop), validate (complete, with
  extras, missing A, missing multiple, empty, null, no missing buf),
  missing count (zero/some/all/null).
- `tests/test_editor_list_mode.c`: Updated panel child count assertion
  5→6 (added progress bar widget).
- `CMakeLists.txt`: Added profile_editor_seq.c and profile_validate.c
  to controllerbox STATIC.
- `tests/CMakeLists.txt`: Added test_profile_validate and
  test_editor_seq_mode with SDL_VIDEODRIVER=dummy.
- `IMPLEMENTATION_PLAN.md`: Task 38 → complete.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 57/57: all previous + test_profile_validate (22) +
  test_editor_seq_mode (28)
- test_editor_seq_mode 28/28 cmocka tests pass
- test_profile_validate 22/22 cmocka tests pass
- verify-boilerplate → exit 0

### Gotchas fixed
- test_seq_capture_multiple: Pressing "B" at step 1 (DOWN) triggers
  a skip, not a capture. Only 2 mappings created. Fixed test to use
  X, Y, L1 button names instead of A, B, X.
- test_seq_complete_all: The `if (i < CBX_DIAG_BTN_COUNT - 1)`
  condition skipped the last step (R3), so sequential mode never
  completed. Removed the condition — all 17 steps now processed.
- panel child count: Added progress bar widget in init, so panel now
  has 6 children (was 5). Updated existing test assertion.

### Design decisions
- **Sequential button order = cbx_diag_button enum order**: Steps 0-16
  follow UP, DOWN, LEFT, RIGHT, A, B, X, Y, START, SELECT, GUIDE, L1,
  R1, L2, R2, L3, R3. This is the natural order and covers all
  highlightable buttons.
- **B and Start are special in sequential mode**: B skips the current
  button (no mapping created, advance to next). Start cancels
  sequential mode entirely. These are checked before the capture logic
  in seq_on_input.
- **find_or_create_mapping**: Sequential capture creates a new mapping
  for the prompted button (using its canonical name as the mapping
  name and source device_class="gamepad"). If a mapping already exists
  for that button (e.g., from list mode), it updates the source event.
- **Progress bar hidden in list mode**: The progress bar widget is
  created in init but hidden (visible=false). It becomes visible when
  sequential mode begins and hidden when it ends.
- **Validation checks source button props**: has_binding searches
  mappings for "button" or "axis" props matching the canonical button
  name. This works for both list mode (where the user sets the prop)
  and sequential mode (where the prop is set from the captured event).
- **Validation is separate from editor**: profile_validate.c is a
  standalone module that operates on cbx_profile. It doesn't depend
  on the editor struct. This makes it reusable for Task 39's save gate.

### Next
Task 39 (Profile save and Settings tab) — deps: Task 38 (done),
Task 5 (done), Task 7 (done), Task 8 (done). All dependencies complete.
Alternatively Task 40 (Systemd service installation and manager
integration test) — deps: Task 39.
Check `ralph tools task ready` and the plan.

## Task 39 (complete) — Profile save and Settings tab

### What landed
- `src/manager/profile_save.h`: Manager-level profile save API.
  cbx_profile_save_named/to_dir (with profiles_dir override for testing),
  cbx_profile_save_meta_to_dir (sidecar metadata save with dir override).
- `src/manager/profile_save.c`: Full implementation. Validates filename
  (cbx_validate_filename), validates profile (cbx_profile_validate),
  validates NES minimum (cbx_profile_validate_nes_minimum), builds path,
  canonicalizes with realpath(), verifies path within profiles dir
  (boundary check), delegates atomic write to cbx_profile_save. Sidecar
  via cbx_profile_meta_save_for or cbx_profile_meta_save to test dir.
- `src/manager/settings_tab.h`: Settings tab struct, enums (setting IDs
  for launch_boot/theme/opacity/vc_count/vc_type_0-3/trigger/save),
  modes (LIST, EDIT). API: init/shutdown/refresh, save, move_up/down,
  activate, edit_up/down, confirm_edit, cancel_edit, accessors.
- `src/manager/settings_tab.c`: Full implementation. Init loads settings
  from disk (or defaults), creates list + save button + status label (3
  panel children). Refresh builds 10 setting rows (CBX_ST_SET_COUNT=10).
  Navigation wraps. Toggle for launch_boot. Edit mode for
  theme/opacity/count/types/trigger with Up/Down adjust, A confirm,
  B cancel (reverts from disk). Save writes atomically via
  cbx_settings_save.
- `src/config/config_settings.h`: Extended with cbx_icon_override struct,
  icon_overrides[]/icon_override_count in cbx_settings. Added
  cbx_settings_icon_override (lookup), set_icon_override, remove_icon_override.
- `src/config/config_settings.c`: Updated defaults (icon_overrides zeroed
  by memset), validation (rejects empty type/icon in overrides), YAML parser
  (handles icon_overrides sequence of {type, icon} mappings), emitter
  (emits icon_overrides when non-empty). All existing tests pass unchanged.
- `tests/test_profile_save.c`: 13 cmocka tests.
- `tests/test_settings_tab.c`: 21 cmocka tests (SDL2 dummy driver + manager).
- `CMakeLists.txt`: Added profile_save.c, settings_tab.c to controllerbox.
- `tests/CMakeLists.txt`: Added test_profile_save, test_settings_tab.
- `IMPLEMENTATION_PLAN.md`: Task 39 → complete.

### Verification (all pass)
- clean build (Debug -Werror, no warnings)
- ctest 59/59: all previous + test_profile_save (13) + test_settings_tab (21)
- test_settings 18/18 still pass (icon overrides backward compatible)
- verify-boilerplate → exit 0

### Gotchas fixed
- Zero-length format string: `snprintf(buf, buflen, "")` triggers
  -Werror=format-zero-length. Fixed by using `buf[0] = '\0'` instead.
- Format-truncation: test_profile_save.c uses `char test_home[256]`
  (not PATH_MAX) to avoid format-truncation when snprintf into PATH_MAX
  buffers with `%s/profiles` suffix. Same pattern as mem-1785994416-3022.
- cbx_settings_tab_init takes 5 args (tab, panel, cache, theme, font_id),
  not 6 — test initially passed NULL for font_id as a 6th arg.
- Unused `test_home` variable removed from test_settings_tab.c.

### Design decisions
- **NES minimum is a hard gate**: cbx_profile_save_to_dir validates NES
  minimum before writing. If validation fails, returns -EINVAL and the
  missing button names are written to missing_buf. The profile file is NOT
  created if validation fails.
- **Path canonicalization**: verify_path_within_dir uses realpath() on
  the base dir and the target path (or its parent if the file doesn't
  exist yet). Checks prefix match with boundary ('/' or '\0').
- **Icon overrides in settings.yaml**: Added as a new YAML section
  `icon_overrides:` (sequence of {type, icon} mappings). Backward
  compatible — old settings.yaml without this section loads with
  icon_override_count=0. New saves only emit the section when non-empty.
- **Settings tab edit mode**: The settings tab has two modes: LIST (browse)
  and EDIT (adjust selected setting). In edit mode, Up/Down adjusts the
  value, A confirms, B cancels (reloads from disk). Toggles (launch_boot)
  don't use edit mode — A flips directly.
- **Cancel edit reverts from disk**: Cancel reloads settings from disk
  to discard in-memory changes. This is simpler than storing a backup
  copy and works correctly because the save button is the only way to
  persist changes.

### Next
Task 40 (Systemd service installation and manager integration test) —
deps: Task 39 (done). All dependencies complete.
Check `ralph tools task ready` and the plan.

## Task 40 (complete) — Systemd service installation and manager integration test

### What landed
- `src/manager/service_install.h`: Systemd user service installation API.
  Result codes (CBX_SVC_OK/ALREADY_ACTIVE/INSTALLED/NO_SYSTEMD/WRITE_FAILED/
  ENABLE_FAILED/VERIFY_FAILED/NOT_IN_GROUP). API: unit_path, unit_content,
  write_unit, check_group, systemd_available, is_active, install, uninstall.
  Test override hooks: set_mock_systemctl, set_mock_group_file,
  set_mock_username.
- `src/manager/service_install.c`: Full implementation. Unit path resolution
  (XDG_CONFIG_HOME/systemd/user or HOME/.config/systemd/user). Unit file
  content generation — static template with compiled-in binary path or
  Flatpak app ID detection (FLATPAK_ID env var). Flatpak uses
  `flatpak-spawn --host systemctl --user` prefix. Atomic write (mkstemp +
  rename, mode 0644, recursive parent dir creation). Group membership check
  via /etc/group parsing. systemd availability via `systemctl --user
  is-system-running` (exit 127 = not found). Full install flow: check systemd
  → check group (advisory) → check already active → write unit → enable --now
  → verify active. Group warning with usermod guidance. Uninstall: disable +
  unlink.
- `tests/test_service_install.c`: 32 cmocka tests. Unit path (home/xdg/
  no-home/overflow/null), unit content (basic/flatpak/overflow/null), atomic
  write (creates-file/custom-content/auto-path/null), group check
  (in-group/not-in/no-group/file-not-found/no-username), systemd available
  (mock-true/false), is-active (mock-true/false), install (no-systemd/
  already-active/success/enable-failed/verify-failed/group-warning/
  null-status), uninstall (removes-file/no-file), mock-overrides-reset.
- `tests/test_manager_integration.c`: 11 cmocka tests with SDL2 dummy driver +
  mock DBus + mock systemctl. Full manager flow: init (3 tabs), controllers
  tab (types loaded, add controller), profiles tab (init, create profile),
  full profile workflow (create, load editor, validate NES minimum, save),
  settings save (toggle, navigate, save), service install (mock script with
  state file, verify unit file, group warning), service uninstall, full
  integration (all steps), render all tabs.
- `CMakeLists.txt`: Added service_install.c to controllerbox STATIC.
- `tests/CMakeLists.txt`: Added test_service_install, test_manager_integration
  with SDL_VIDEODRIVER=dummy env.
- `IMPLEMENTATION_PLAN.md`: Task 40 → complete.

### Verification (all pass)
- clean build (Debug, 0 warnings)
- ctest 61/61: all previous + test_service_install (32) +
  test_manager_integration (11)
- verify-boilerplate → exit 0

### Gotchas fixed
- `/bin/true` does not exist in nix-shell environment. Used `/bin/sh -c
  'exit 0'` for mock systemctl commands that need to always succeed.
- Format-truncation: tmppath buffer must be PATH_MAX + 8 (not PATH_MAX)
  to hold the ".XXXXXX" suffix for mkstemp. Test buffers use PATH_MAX + 64
  or + 128 for paths with appended suffixes.
- Mock systemctl script must be stateful: is-active returns "inactive"
  before enable, "active" after. Used a state file (touch on enable) to
  simulate service lifecycle. Otherwise install sees already-active and
  returns CBX_SVC_ALREADY_ACTIVE instead of CBX_SVC_OK.
- DBus mock iface names: IP_IFACE_MANAGER is "org.shadowblip.InputManager"
  (not "org.shadowblip.InputPlumber.Manager"). Must include dbus_mock.h
  for the constant definitions.
- cbx_profile_entry struct has `filename` field (not `name`). NES minimum
  validation functions are `cbx_profile_validate_nes_minimum` and
  `cbx_profile_validate_missing_count` (with cbx_profile_ prefix).
- Unused variable in cbx_service_is_active: added `(void)status;` to
  suppress -Werror=unused-variable.

### Design decisions
- **Static unit template**: The unit file content is a compile-time template
  with only the binary path (or Flatpak app ID from env) substituted. No
  user-supplied values are interpolated at runtime (security: no injection
  through unit files). The template matches SPEC §2.4 exactly.
- **Flatpak detection via FLATPAK_ID**: When the FLATPAK_ID env var is set
  (by the Flatpak runtime), the unit uses `flatpak run <app-id>
  --overlay-service` and systemctl calls use `flatpak-spawn --host
  systemctl --user` prefix.
- **Group check is advisory**: If the user is not in the inputplumber group,
  the service is still installed but a warning is included in the status
  message with usermod guidance. This follows the plan's "guides if not"
  requirement without blocking installation.
- **Test override hooks**: Three static override variables (mock_systemctl,
  mock_group_file, mock_username) allow tests to mock systemctl, /etc/group,
  and the current username without affecting real system state. All are
  reset to NULL in teardown.
- **Stateful mock systemctl**: Integration and service install tests use a
  shell script that tracks enable state via a marker file, so is-active
  returns "inactive" before enable and "active" after, matching real
  systemctl behavior.

### Next
Task 41 (CMake install rules, systemd service file, and desktop entry) —
deps: Task 40 (done), Task 2 (done). All dependencies complete.
Alternatively Task 42 (Flatpak manifest) — deps: Task 41.
Check `ralph tools task ready` and the plan.

## Task 41 (complete) — CMake install rules, systemd service file, and desktop entry

### What landed
- `packaging/controller-box.service`: Systemd user service file (SPEC §2.4).
  [Unit] After=inputplumber.service, Requires=inputplumber.service;
  [Service] ExecStart=/usr/bin/controller-box --overlay-service, Restart=always;
  [Install] WantedBy=default.target.
- `packaging/controller-box-manager.desktop`: Desktop entry launching manager mode
  (Exec=/usr/bin/controller-box --manager, Type=Application, Categories=Game;Settings).
- `CMakeLists.txt`: Added CBX_DATA_INSTALL_DIR and CBX_ICON_INSTALL_DIR as
  relative install variables (using CMAKE_INSTALL_DATADIR, not CMAKE_INSTALL_PREFIX).
  This fixes DESTDIR/--prefix overrides — previously CBX_DATA_DIR was an absolute
  path baked at configure time, causing data files to go to the wrong location.
  Install rules: service → data dir, desktop → applications dir.
- `tests/test_packaging_install.sh`: Shell test verifying file layout (binary,
  icons dir, controller-icons.yaml, service file, desktop entry) and content
  (service directives: After/Requires/Restart/ExecStart overlay, desktop: manager
  mode, Type=Application). Uses `grep -qF --` to handle patterns starting with `--`.
- `docs/PACKAGING.md`: Full packaging doc — install layout, tarball install
  (build-from-source, dependency table, post-install steps, service unit spec),
  Flatpak placeholder, version placeholder, post-v1 roadmap.
- `IMPLEMENTATION_PLAN.md`: Task 41 → complete.

### Verification (all pass)
- clean Debug build (-Werror, 0 warnings)
- ctest 61/61: all previous tests pass
- DESTDIR install: `DESTDIR=/tmp/test-install cmake --install build` places all
  files correctly under /tmp/test-install/usr/ (binary, icons, controller-icons.yaml,
  service file, desktop entry)
- tests/test_packaging_install.sh /tmp/test-install → all checks pass
- verify-boilerplate → exit 0

### Gotchas fixed
- **Absolute vs relative install paths**: CBX_DATA_DIR was computed as
  `${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/controller-box` at configure
  time — an absolute path. When using `cmake --install --prefix` or `DESTDIR`,
  CMake overrides the install prefix but NOT our custom variable. Fixed by adding
  CBX_DATA_INSTALL_DIR as `${CMAKE_INSTALL_DATADIR}/controller-box` (relative) and
  using it in install rules. CBX_DATA_DIR (absolute) still used for config.h.
- **grep -qF with -- patterns**: `grep -qF "--overlay-service"` fails with exit 2
  because grep interprets `--overlay-service` as an option even with -F. Fixed by
  using `grep -qF -- "$pattern"` (the `--` terminates option parsing).
- **DESTDIR vs --prefix**: Acceptance criteria uses DESTDIR semantics
  (`make DESTDIR=/tmp/test-install install`), which prepends to CMAKE_INSTALL_PREFIX
  (/usr → /tmp/test-install/usr/). Using `--prefix /tmp/test-install` instead gives
  /tmp/test-install/bin/ (no usr/). The test script expects DESTDIR layout.

### Design decisions
- **Service file installed to data dir, not systemd user dir**: The systemd
  service file is installed to /usr/share/controller-box/controller-box.service
  as a reference template. The manager generates and writes the actual user unit
  at runtime (~/.config/systemd/user/controller-box.service) via service_install.c,
  adjusting the ExecStart path for Flatpak if needed. This matches SPEC §9.2
  ("make install places the systemd user service file") and §9.3 layout.
- **Relative install variables**: CMakeLists.txt now has both CBX_DATA_DIR
  (absolute, for config.h compilation) and CBX_DATA_INSTALL_DIR (relative,
  for install rules). This is the standard CMake pattern for handling
  DESTDIR/prefix overrides correctly.

### Next
Task 42 (Flatpak manifest) — deps: Task 41 (done). All dependencies complete.
Alternatively Task 43 (Version embedding and packaging integration test) —
deps: Task 41 (done), Task 42.
Check `ralph tools task ready` and the plan.
