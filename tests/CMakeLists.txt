# tests/CMakeLists.txt — Test harness setup (Task 3).
#
# Defines all test targets and CTest registrations.  Smoke tests (no
# cmocka) are always built.  cmocka-based unit tests are built only when
# cmocka is available (CMOCKA_FOUND via pkg_check_modules in the root
# CMakeLists.txt).

# --- Smoke tests (always available, no cmocka dependency) -------------------
add_executable(smoke_test_sdl2 tests/smoke_test_sdl2.c)
target_link_libraries(smoke_test_sdl2 PRIVATE PkgConfig::SDL2)

add_executable(smoke_test_nanosvg tests/smoke_test_nanosvg.c)
target_link_libraries(smoke_test_nanosvg PRIVATE nanosvg m)

add_test(NAME smoke_test_sdl2    COMMAND smoke_test_sdl2)
add_test(NAME smoke_test_nanosvg COMMAND smoke_test_nanosvg)

# --- cmocka unit tests ------------------------------------------------------
# Built only when cmocka is found.  These link against the cmocka library
# and the shared test harness (test_harness + dbus_mock).
if(CMOCKA_FOUND)

    # Shared test support library: SDL2 dummy-driver utility + DBus mock.
    add_library(cbx_test_support STATIC
        tests/test_harness.c
        tests/dbus_mock.c)
    target_include_directories(cbx_test_support PUBLIC tests)
    target_link_libraries(cbx_test_support
        PUBLIC
            PkgConfig::SDL2
            PkgConfig::CMOCKA)

    # Sample cmocka assertion test + DBus mock infrastructure validation.
    add_executable(test_sample tests/test_sample.c)
    target_link_libraries(test_sample PRIVATE cbx_test_support)

    # SDL2 dummy-driver rendering test via the test harness.
    add_executable(test_sdl_dummy tests/test_sdl_dummy.c)
    target_link_libraries(test_sdl_dummy PRIVATE cbx_test_support)

    # Config path resolution test (Task 4): XDG dirs, directory creation.
    # Links controllerbox for config_paths functions + cmocka for assertions.
    add_executable(test_config_paths tests/test_config_paths.c)
    target_link_libraries(test_config_paths PRIVATE controllerbox PkgConfig::CMOCKA)

    # Settings.yaml read/write test (Task 5): defaults, round-trip, validation,
    # atomic write, YAML security constraints.
    add_executable(test_settings tests/test_settings.c)
    target_link_libraries(test_settings PRIVATE controllerbox PkgConfig::CMOCKA)

    add_test(NAME test_sample    COMMAND test_sample)
    add_test(NAME test_sdl_dummy  COMMAND test_sdl_dummy)
    add_test(NAME test_config_paths COMMAND test_config_paths)
    add_test(NAME test_settings     COMMAND test_settings)

    # Help later tasks: tests that need the dummy driver can set it here.
    set_tests_properties(test_sdl_dummy PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

else()
    message(STATUS "cmocka not found — unit tests skipped (smoke tests still built)")
endif()