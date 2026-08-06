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
