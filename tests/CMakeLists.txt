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

    # Assignments.yaml read/write test (Task 6): round-trip, validation, defaults,
    # gamepad_order persistence, YAML security constraints.
    add_executable(test_assignments tests/test_assignments.c)
    target_link_libraries(test_assignments PRIVATE controllerbox PkgConfig::CMOCKA)

    add_test(NAME test_sample    COMMAND test_sample)
    add_test(NAME test_sdl_dummy  COMMAND test_sdl_dummy)
    add_test(NAME test_config_paths COMMAND test_config_paths)
    add_test(NAME test_settings     COMMAND test_settings)
    add_test(NAME test_assignments   COMMAND test_assignments)

    # Profile YAML parse/generate test (Task 7): device_profile_v1 round-trip,
    # validation, YAML security constraints.
    add_executable(test_profile_yaml tests/test_profile_yaml.c)
    target_link_libraries(test_profile_yaml PRIVATE controllerbox PkgConfig::CMOCKA)

    add_test(NAME test_profile_yaml COMMAND test_profile_yaml)

    # Profile metadata sidecar + filesystem enumeration test (Task 8):
    # sidecar parse/serialize/save/load, filename validation, O_NOFOLLOW,
    # realpath verification, YAML security, profile enumeration, metadata
    # merging, sorting, device config/capability map listing.
    add_executable(test_profile_list tests/test_profile_list.c)
    target_link_libraries(test_profile_list PRIVATE controllerbox PkgConfig::CMOCKA)

    add_test(NAME test_profile_list COMMAND test_profile_list)

    # DBus connection management test (Task 9): connect, version check,
    # NameOwnerChanged tracking, degraded mode, categorized errors.
    add_executable(test_connection tests/test_connection.c)
    target_link_libraries(test_connection PRIVATE controllerbox cbx_test_support)

    add_test(NAME test_connection COMMAND test_connection)

    # ObjectManager enumeration test (Task 10): GetManagedObjects parsing,
    # device model population, path validation, mock backend integration.
    add_executable(test_objectmanager_parse tests/test_objectmanager_parse.c)
    target_link_libraries(test_objectmanager_parse PRIVATE controllerbox cbx_test_support)

    add_test(NAME test_objectmanager_parse COMMAND test_objectmanager_parse)

    # Hotplug signal handling test (Task 11): InterfacesAdded/Removed,
    # device model incremental update, sender verification, path validation.
    add_executable(test_hotplug tests/test_hotplug.c)
    target_link_libraries(test_hotplug PRIVATE controllerbox cbx_test_support)

    add_test(NAME test_hotplug COMMAND test_hotplug)

    # PropertiesChanged signal handling test (Task 11): type validation,
    # string/array length limits, invalidated properties, sender verification.
    add_executable(test_properties_changed tests/test_properties_changed.c)
    target_link_libraries(test_properties_changed PRIVATE controllerbox cbx_test_support)

    add_test(NAME test_properties_changed COMMAND test_properties_changed)

    # Manager interface method wrappers test (Task 12): CreateTargetDevice,
    # StopTargetDevice, AttachTargetDevice, SetTargetDevices, GamepadOrder
    # get/set (with device model validation), SupportedTargetDeviceIds/Devices get.
    add_executable(test_manager_calls tests/test_manager_calls.c)
    target_link_libraries(test_manager_calls PRIVATE controllerbox cbx_test_support)

    add_test(NAME test_manager_calls COMMAND test_manager_calls)

    # CompositeDevice interface method wrappers test (Task 13):
    # SetInterceptActivation, InterceptMode get/set, LoadProfilePath,
    # LoadProfileFromYaml, GetProfileYaml, SetTargetDevices, Stop,
    # TargetDevices/SourceDevicePaths/PersistentId/Name/Capabilities/
    # OutputCapabilities/TargetCapabilities/DbusDevices gets.
    add_executable(test_composite_calls tests/test_composite_calls.c)
    target_link_libraries(test_composite_calls PRIVATE controllerbox cbx_test_support)

    add_test(NAME test_composite_calls COMMAND test_composite_calls)

    # InterceptMode polling state machine test (Task 13): state transitions
    # (IDLE/PASS_WAIT/ACTIVE), activation/deactivation callbacks, timeout,
    # error handling, parse failures, full lifecycle simulation.
    add_executable(test_intercept_poll tests/test_intercept_poll.c)
    target_link_libraries(test_intercept_poll PRIVATE controllerbox cbx_test_support)

    add_test(NAME test_intercept_poll COMMAND test_intercept_poll)

    # Task 14: Source/target device properties and InputEvent signal handling.
    # Source device property getters (UniqueId, PhysPath, IdVendor, IdProduct,
    # IdBustype, SerialNumber, Name) for EventDevice/UdevDevice/HIDRawDevice.
    add_executable(test_source_props tests/test_source_props.c)
    target_link_libraries(test_source_props PRIVATE controllerbox cbx_test_support)

    add_test(NAME test_source_props COMMAND test_source_props)

    # Target device property getters (Name, DeviceType).
    add_executable(test_target_props tests/test_target_props.c)
    target_link_libraries(test_target_props PRIVATE controllerbox cbx_test_support)

    add_test(NAME test_target_props COMMAND test_target_props)

    # InputEvent signal handling: event parsing (button/axis), value
    # validation, sender verification, rate limiting (200/sec/device),
    # integration via inject_signal.
    add_executable(test_input_signal tests/test_input_signal.c)
    target_link_libraries(test_input_signal PRIVATE controllerbox cbx_test_support)

    add_test(NAME test_input_signal COMMAND test_input_signal)

    # Help later tasks: tests that need the dummy driver can set it here.
    set_tests_properties(test_sdl_dummy PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

else()
    message(STATUS "cmocka not found — unit tests skipped (smoke tests still built)")
endif()