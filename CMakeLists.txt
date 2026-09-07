cmake_minimum_required(VERSION 3.16)
project(controller-box
    VERSION 0.1.0
    DESCRIPTION "Controller-Box: a controller-only SDL2 GUI wrapping InputPlumber"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Linux/POSIX interfaces used throughout the production sources, including
# open_memstream(), mkstemp(), and O_NOFOLLOW, require feature-test macros
# when compiling in strict C11 mode.
add_compile_definitions(_GNU_SOURCE)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Default install prefix: /usr. Allow override via -DCMAKE_INSTALL_PREFIX.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Install prefix" FORCE)
endif()

# Canonical install layout (SPEC §9.3). Computed before config.h generation.
# These absolute paths go into config.h (compiled into the binary for
# runtime lookups). Install rules use the *relative* GNUInstallDirs
# variables so that `cmake --install --prefix` overrides work correctly.
include(GNUInstallDirs)
set(CBX_DATA_DIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/controller-box")
set(CBX_ICON_DIR "${CBX_DATA_DIR}/icons")
set(CBX_FONT_DIR "${CBX_DATA_DIR}/fonts")
set(CBX_PROFILE_DIR "${CBX_DATA_DIR}/profiles")
set(CBX_BINARY_PATH "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/controller-box")
set(CBX_DATA_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/controller-box")
set(CBX_ICON_INSTALL_DIR "${CBX_DATA_INSTALL_DIR}/icons")
set(CBX_FONT_INSTALL_DIR "${CBX_DATA_INSTALL_DIR}/fonts")

# Warn early on missing build dependencies rather than failing deep in the build.
find_package(PkgConfig REQUIRED)

# --- Dependency detection -----------------------------------------------------
# SDL2 core + satellite libraries
pkg_check_modules(SDL2    REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(SDL2TTF REQUIRED IMPORTED_TARGET SDL2_ttf)
pkg_check_modules(SDL2IMG REQUIRED IMPORTED_TARGET SDL2_image)

# sd-bus ships with libsystemd (used for DBus in later tasks).
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

# YAML parsing (libyaml) — added structurally here, used from Task 4 on.
pkg_check_modules(YAML REQUIRED IMPORTED_TARGET yaml-0.1)

# cmocka unit testing (used from Task 3 on).
# CMOCKA_FOUND is set by pkg_check_modules when cmocka is available.
pkg_check_modules(CMOCKA IMPORTED_TARGET cmocka)

# --- Compile / link flags -----------------------------------------------------
# CMake already sets CMAKE_C_FLAGS_<CONFIG> (-O2/-DNDEBUG for Release, -g for
# Debug). We only add warnings here; -Werror is gated on Debug so release
# packaging builds are not blocked by new-compiler warnings.
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wshadow -Wstrict-prototypes
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options($<$<CONFIG:Debug>:-Werror>)
endif()

# --- Sanitizer support (DOD-05, §11.2.5) ------------------------------------
# Optional ASan + UBSan build gate.  Enable with:
#   cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DCBX_ENABLE_SANITIZERS=ON
# The verify-sanitizers.sh script automates this.
option(CBX_ENABLE_SANITIZERS "Enable AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(CBX_ENABLE_SANITIZERS)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "Sanitizers require GCC or Clang")
    endif()
    # GCC 15 enables more aggressive stringop-truncation analysis under
    # sanitizers.  All strncpy(dst, src, sizeof(dst)-1) calls in this codebase
    # are followed by explicit null-termination, making the warning a false
    # positive.  Suppress it in the sanitizer build only.
    add_compile_options(-Wno-stringop-truncation)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
    # ASan needs detect_leaks=0 for test harnesses that intentionally
    # leave global state alive at exit (SDL_Quit is optional in tests).
    # Leak detection is still ON by default; override per-test via
    # ASAN_OPTIONS if needed.
    message(STATUS "Sanitizers enabled: AddressSanitizer + UndefinedBehaviorSanitizer")
endif()

# Generated config.h with install paths.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/config.h"
    @ONLY)

# --- Vendored nanosvg ---------------------------------------------------------
add_library(nanosvg STATIC third_party/nanosvg/nanosvg_impl.c)
target_include_directories(nanosvg PUBLIC third_party/nanosvg)
target_link_libraries(nanosvg PUBLIC m)
# Vendored third-party code: suppress its compiler warnings entirely.
target_compile_options(nanosvg PRIVATE -w)

# --- Shared static library (libcontrollerbox) --------------------------------
# Aggregates all reusable product sources (everything except the thin entry
# point in src/app/main.c, which lives in the executable). Later tasks append
# their .c files here.
add_library(controllerbox STATIC
    src/config/config_paths.c
    src/config/config_settings.c
    src/config/config_assignments.c
    src/config/config_profile.c
    src/config/config_profile_meta.c
    src/config/config_profile_list.c
    src/dbus/dbus_client.c
    src/dbus/ip_composite.c
    src/dbus/ip_create_composite.c
    src/dbus/ip_gamepad_order.c
    src/dbus/ip_connection.c
    src/dbus/ip_device_model.c
    src/dbus/ip_hotplug.c
    src/dbus/ip_input_signal.c
    src/dbus/ip_intercept_poll.c
    src/dbus/ip_manager.c
    src/dbus/ip_objectmanager.c
    src/dbus/ip_properties.c
    src/dbus/ip_source.c
    src/dbus/ip_target.c
    src/icons/icon_map.c
    src/icons/icon_cache.c
    src/icons/icon_lookup.c
    src/ui/renderer.c
    src/ui/theme.c
    src/ui/text.c
    src/ui/widget.c
    src/ui/widget_button.c
    src/ui/widget_label.c
    src/ui/widget_image.c
    src/ui/widget_panel.c
    src/ui/widget_list.c
    src/ui/widget_grid.c
    src/ui/widget_tabbar.c
    src/ui/widget_progress.c
    src/ui/focus.c
    src/ui/input_map.c
    src/ui/animation.c
    src/ui/dirty_rect.c
    src/overlay/surface_build.c
    src/overlay/lifecycle.c
    src/overlay/grid_render.c
    src/overlay/player_mode.c
    src/overlay/host_mode.c
    src/overlay/conflict.c
    src/overlay/profile_cycle.c
    src/overlay/dynamic_columns.c
    src/overlay/trigger.c
    src/overlay/close.c
    src/manager/manager.c
    src/manager/controllers_tab.c
    src/manager/profiles_tab.c
    src/manager/profile_diagram.c
    src/manager/profile_editor_list.c
    src/manager/profile_editor_seq.c
    src/manager/profile_validate.c
    src/manager/profile_save.c
    src/manager/settings_tab.c
    src/manager/service_install.c
    src/identify/identity.c
    src/identify/assign.c
    src/identify/assign_persist.c
    src/identify/identity_downgrade.c
    src/identify/gamepad_order_restore.c
    src/app/overlay_service.c)
target_include_directories(controllerbox
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_BINARY_DIR}")  # for generated config.h
target_link_libraries(controllerbox
    PUBLIC
        PkgConfig::SDL2
        PkgConfig::SDL2TTF
        PkgConfig::SDL2IMG
        PkgConfig::SYSTEMD
        PkgConfig::YAML
        nanosvg)

# --- Test-instrumented library (CBX_TESTING) --------------------------------
# A second copy of the same product sources compiled with CBX_TESTING so the
# gated cbx_service_set_mock_* overrides in service_install.c are available to
# the unit tests that exercise systemd installation.  The release library and
# the production binary are built from `controllerbox` WITHOUT CBX_TESTING, so
# the mock override symbols can never appear in a release build (F6).
add_library(controllerbox_testing STATIC
    src/config/config_paths.c
    src/config/config_settings.c
    src/config/config_assignments.c
    src/config/config_profile.c
    src/config/config_profile_meta.c
    src/config/config_profile_list.c
    src/dbus/dbus_client.c
    src/dbus/ip_composite.c
    src/dbus/ip_create_composite.c
    src/dbus/ip_gamepad_order.c
    src/dbus/ip_connection.c
    src/dbus/ip_device_model.c
    src/dbus/ip_hotplug.c
    src/dbus/ip_input_signal.c
    src/dbus/ip_intercept_poll.c
    src/dbus/ip_manager.c
    src/dbus/ip_objectmanager.c
    src/dbus/ip_properties.c
    src/dbus/ip_source.c
    src/dbus/ip_target.c
    src/icons/icon_map.c
    src/icons/icon_cache.c
    src/icons/icon_lookup.c
    src/ui/renderer.c
    src/ui/theme.c
    src/ui/text.c
    src/ui/widget.c
    src/ui/widget_button.c
    src/ui/widget_label.c
    src/ui/widget_image.c
    src/ui/widget_panel.c
    src/ui/widget_list.c
    src/ui/widget_grid.c
    src/ui/widget_tabbar.c
    src/ui/widget_progress.c
    src/ui/focus.c
    src/ui/input_map.c
    src/ui/animation.c
    src/ui/dirty_rect.c
    src/overlay/surface_build.c
    src/overlay/lifecycle.c
    src/overlay/grid_render.c
    src/overlay/player_mode.c
    src/overlay/host_mode.c
    src/overlay/conflict.c
    src/overlay/profile_cycle.c
    src/overlay/dynamic_columns.c
    src/overlay/trigger.c
    src/overlay/close.c
    src/manager/manager.c
    src/manager/controllers_tab.c
    src/manager/profiles_tab.c
    src/manager/profile_diagram.c
    src/manager/profile_editor_list.c
    src/manager/profile_editor_seq.c
    src/manager/profile_validate.c
    src/manager/profile_save.c
    src/manager/settings_tab.c
    src/manager/service_install.c
    src/identify/identity.c
    src/identify/assign.c
    src/identify/assign_persist.c
    src/identify/identity_downgrade.c
    src/identify/gamepad_order_restore.c
    src/app/overlay_service.c)
target_include_directories(controllerbox_testing
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(controllerbox_testing
    PUBLIC
        PkgConfig::SDL2
        PkgConfig::SDL2TTF
        PkgConfig::SDL2IMG
        PkgConfig::SYSTEMD
        PkgConfig::YAML
        nanosvg)
target_compile_definitions(controllerbox_testing PRIVATE CBX_TESTING)

# --- Dual-mode executable (Task 2, SPEC §2.3) --------------------------------
# controller-box is one binary with two modes (--overlay-service default,
# --manager). Both link against the shared static library.
add_executable(controller-box src/app/main.c)
target_link_libraries(controller-box PRIVATE controllerbox)

# Root runner helper: all InputPlumber mutation decisions occur before the
# helper's independently opened system-bus connection can forward a call.
# BUG-0020: compile from a byte-identical non-hidden generated copy so the
# retained build tree never emits a nested .factory/... object path (the
# boilerplate live inventory rejects a hidden namespace inside the product
# build tree). configure_file registers the source as a configure dependency,
# so editing .factory/runner/inputplumber-mediator.c still triggers a
# reconfigure + rebuild; the target name and install rule are unchanged.
#
# A pre-fix build may have left a stale nested .factory object dir inside an
# existing retained build tree (CMake does not delete orphaned object files
# when a source is removed from a target), so a subsequent live inventory
# would still fail without manual cleanup. Remove that exact legacy object
# dir at configure time, before target generation. The removal is narrowly
# scoped to the one legacy object dir and never touches the retained
# build/cache or any other path. Symlink safety: if the legacy path is a
# symlink we remove only the link itself (never following it), so a
# malicious/external symlink cannot cause deletion outside the build tree.
set(CBX_MEDIATOR_LEGACY_OBJ_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/inputplumber-mediator.dir/.factory")
if(EXISTS "${CBX_MEDIATOR_LEGACY_OBJ_DIR}")
    if(IS_SYMLINK "${CBX_MEDIATOR_LEGACY_OBJ_DIR}")
        # Remove only the link; never follow it outside the build tree.
        file(REMOVE "${CBX_MEDIATOR_LEGACY_OBJ_DIR}")
    else()
        file(REMOVE_RECURSE "${CBX_MEDIATOR_LEGACY_OBJ_DIR}")
    endif()
endif()
set(CBX_MEDIATOR_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated/inputplumber-mediator.c")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/.factory/runner/inputplumber-mediator.c"
    "${CBX_MEDIATOR_GENERATED}"
    COPYONLY)
# Preserve the original source path in DWARF debug info (the mediator has no
# __FILE__/__LINE__ diagnostics, but keeping the path aids gdb/addr2line).
set_source_files_properties("${CBX_MEDIATOR_GENERATED}" PROPERTIES
    COMPILE_FLAGS "-fdebug-prefix-map=${CMAKE_CURRENT_BINARY_DIR}/generated=${CMAKE_CURRENT_SOURCE_DIR}/.factory/runner")
add_executable(inputplumber-mediator "${CBX_MEDIATOR_GENERATED}")
target_link_libraries(inputplumber-mediator PRIVATE PkgConfig::SYSTEMD)

# --- Testing ------------------------------------------------------------------
enable_testing()
# Test targets and CTest registrations live in tests/CMakeLists.txt (Task 3).
include(tests/CMakeLists.txt)

# --- Install rules (expanded in Task 41) -------------------------------------
install(TARGETS controller-box
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(TARGETS inputplumber-mediator
    RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}")
install(FILES .factory/runner/inputplumber-runner-dbus.conf
    DESTINATION "${CMAKE_INSTALL_DATADIR}/dbus-1/system.d")
install(TARGETS controllerbox nanosvg
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")

# --- Data files (Task 16) ---------------------------------------------------
# Install icon SVGs and controller-icons.yaml to the data directory.
# Uses relative install dirs so `cmake --install --prefix` works correctly.
install(DIRECTORY data/icons/svg/ DESTINATION "${CBX_ICON_INSTALL_DIR}/svg"
    FILES_MATCHING PATTERN "*.svg")
install(FILES data/icons/svg/LICENSE.controllercons
    DESTINATION "${CBX_ICON_INSTALL_DIR}/svg")
install(FILES data/controller-icons.yaml
              data/licensed-diagram-authority.json
              data/licensed-diagram-oracle.json
    DESTINATION "${CBX_DATA_INSTALL_DIR}")
install(DIRECTORY data/controller-layouts/ DESTINATION "${CBX_DATA_INSTALL_DIR}/controller-layouts"
    FILES_MATCHING PATTERN "*.json")

# Bundled font directory (Task 1, maintenance BUG-0001).  Created empty with
# a .gitkeep so future bundled fonts are installed automatically.
install(DIRECTORY data/fonts/ DESTINATION "${CBX_FONT_INSTALL_DIR}"
    FILES_MATCHING PATTERN "*")
install(FILES data/fonts/.gitkeep
    DESTINATION "${CBX_FONT_INSTALL_DIR}")
install(DIRECTORY data/profiles/ DESTINATION "${CBX_DATA_INSTALL_DIR}/profiles"
    FILES_MATCHING PATTERN "*.yaml")

# --- Packaging files (Task 41) ----------------------------------------------
# Systemd user service file (SPEC §2.4, §9.2).  Installed to the data
# directory as a reference template; the manager copies it to
# ~/.config/systemd/user/ on first run (or generates the unit content
# at runtime via service_install.c).
install(FILES packaging/controller-box.service
    DESTINATION "${CBX_DATA_INSTALL_DIR}")
# Desktop entry (SPEC §9.2–§9.3).  Launches the manager mode.
install(FILES packaging/controller-box-manager.desktop
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")