cmake_minimum_required(VERSION 3.16)
project(controller-box
    VERSION 0.1.0
    DESCRIPTION "Controller-Box: a controller-only SDL2 GUI wrapping InputPlumber"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

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
    src/identify/gamepad_order_restore.c)
target_include_directories(controllerbox
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests"
        "${CMAKE_CURRENT_BINARY_DIR}")  # for generated config.h
target_link_libraries(controllerbox
    PUBLIC
        PkgConfig::SDL2
        PkgConfig::SDL2TTF
        PkgConfig::SDL2IMG
        PkgConfig::SYSTEMD
        PkgConfig::YAML
        nanosvg)

# --- Dual-mode executable (Task 2, SPEC §2.3) --------------------------------
# controller-box is one binary with two modes (--overlay-service default,
# --manager). Both link against the shared static library.
add_executable(controller-box src/app/main.c)
target_link_libraries(controller-box PRIVATE controllerbox)

# --- Testing ------------------------------------------------------------------
enable_testing()
# Test targets and CTest registrations live in tests/CMakeLists.txt (Task 3).
include(tests/CMakeLists.txt)

# --- Install rules (expanded in Task 41) -------------------------------------
install(TARGETS controller-box
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
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
    DESTINATION "${CBX_DATA_INSTALL_DIR}")

# Bundled font directory (Task 1, maintenance BUG-0001).  Created empty with
# a .gitkeep so future bundled fonts are installed automatically.
install(DIRECTORY data/fonts/ DESTINATION "${CBX_FONT_INSTALL_DIR}"
    FILES_MATCHING PATTERN "*")
install(FILES data/fonts/.gitkeep
    DESTINATION "${CBX_FONT_INSTALL_DIR}")

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