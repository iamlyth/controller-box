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
include(GNUInstallDirs)
set(CBX_DATA_DIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/controller-box")
set(CBX_ICON_DIR "${CBX_DATA_DIR}/icons")

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
# Aggregates all product sources. Later tasks append their .c files here.
add_library(controllerbox STATIC
    src/app/main.c
    src/config/config_paths.c
    src/dbus/dbus_client.c
    src/ui/widget.c)
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

# --- Task 1 smoke tests -------------------------------------------------------
add_executable(smoke_test_sdl2 tests/smoke_test_sdl2.c)
target_link_libraries(smoke_test_sdl2 PRIVATE PkgConfig::SDL2)

add_executable(smoke_test_nanosvg tests/smoke_test_nanosvg.c)
target_link_libraries(smoke_test_nanosvg PRIVATE nanosvg m)

# --- Testing ------------------------------------------------------------------
enable_testing()
add_test(NAME smoke_test_sdl2    COMMAND smoke_test_sdl2)
add_test(NAME smoke_test_nanosvg COMMAND smoke_test_nanosvg)

# --- Install rules (expanded in Task 41) -------------------------------------
install(TARGETS controllerbox nanosvg
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")