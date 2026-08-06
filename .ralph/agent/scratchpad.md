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
