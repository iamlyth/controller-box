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
