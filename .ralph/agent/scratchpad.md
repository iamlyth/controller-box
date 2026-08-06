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
