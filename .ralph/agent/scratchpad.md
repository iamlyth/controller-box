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