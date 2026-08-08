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

# --- Flatpak manifest validation (Task 42) --------------------------------
# Validates packaging/org.shadowblip.ControllerBox.yaml against SPEC §9.1.
# Requires Python3 + PyYAML (provided by nix-shell / build environment).
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -c "import yaml"
        RESULT_VARIABLE PY_YAML_RESULT
        OUTPUT_QUIET ERROR_QUIET)
    if(PY_YAML_RESULT EQUAL 0)
        add_test(NAME test_flatpak_manifest
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_flatpak_manifest.py)
    else()
        message(STATUS "PyYAML not found — test_flatpak_manifest skipped")
    endif()
else()
    message(STATUS "Python3 not found — test_flatpak_manifest skipped")
endif()

# --- Packaging integration test (Task 43) ----------------------------------
# End-to-end packaging pipeline: build → install to temp dir → verify
# layout + binary runs.  Uses bash; requires cmake to be available.
find_program(BASH_EXECUTABLE bash QUIET)
if(BASH_EXECUTABLE)
    add_test(NAME test_packaging
        COMMAND ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_packaging.sh
            ${CMAKE_CURRENT_BINARY_DIR})
else()
    message(STATUS "bash not found — test_packaging skipped")
endif()

# --- Installed production smoke test (Task 9, §11.1.5) -------------------
# Exercises the real main() entry point under Xvfb with xdotool input
# and ImageMagick screenshot capture.  Skips (exit 77) when Xvfb,
# xdotool, or ImageMagick is unavailable.
if(BASH_EXECUTABLE)
    add_test(NAME test_installed_smoke
        COMMAND ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_installed_smoke.sh
            ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(test_installed_smoke PROPERTIES
        SKIP_RETURN_CODE 77)
    add_test(NAME test_installed_functional_evidence
        COMMAND ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test-installed-functional-evidence.sh
            ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME test_factory_environment
        COMMAND ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test-factory-environment.sh)
    add_test(NAME test_campaign_audit
        COMMAND ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test-campaign-audit.sh)
    add_test(NAME test_ralph_campaign
        COMMAND ${BASH_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test-ralph-campaign.sh)
else()
    message(STATUS "bash not found — test_installed_smoke skipped")
endif()

# --- cmocka unit tests ------------------------------------------------------
# Built only when cmocka is found.  These link against the cmocka library
# and the shared test harness (test_harness + dbus_mock).
if(CMOCKA_FOUND)

    # Shared test support library: SDL2 dummy-driver utility + DBus mock
    # + framebuffer pixel assertion utilities (Task 1).
    add_library(cbx_test_support STATIC
        tests/test_harness.c
        tests/dbus_mock.c
        tests/fb_assert.c
        tests/interaction_inventory.c)
    target_include_directories(cbx_test_support PUBLIC tests)
    target_link_libraries(cbx_test_support
        PUBLIC
            PkgConfig::SDL2
            PkgConfig::SDL2IMG
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

    # Runtime font discovery test (BUG-0001, Task 1): cbx_font_path()
    # searches system font directories at runtime for DejaVuSans.ttf.
    # Headless-safe (SDL_VIDEODRIVER=dummy not required — no SDL calls).
    add_executable(test_font_path tests/test_font_path.c)
    target_link_libraries(test_font_path PRIVATE controllerbox PkgConfig::CMOCKA)
    add_test(NAME test_font_path COMMAND test_font_path)
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

    # Task 15: CreateCompositeDevice temp file workaround + GamepadOrder
    # persistence.  Both use the mock backend + controllerbox (for config).
    add_executable(test_create_composite tests/test_create_composite.c)
    target_link_libraries(test_create_composite PRIVATE controllerbox cbx_test_support)

    add_test(NAME test_create_composite COMMAND test_create_composite)

    add_executable(test_gamepad_order tests/test_gamepad_order.c)
    target_link_libraries(test_gamepad_order PRIVATE controllerbox cbx_test_support)

    add_test(NAME test_gamepad_order COMMAND test_gamepad_order)

    # Task 16: SVG assets and icon mapping table.  Tests the runtime
    # icon mapping parser: parse, load, lookup (known/unknown types),
    # default path, YAML security constraints, max entries, truncation.
    # Also verifies all SVG assets parse and rasterize via nanosvg.
    add_executable(test_icon_map tests/test_icon_map.c)
    target_link_libraries(test_icon_map PRIVATE controllerbox PkgConfig::CMOCKA nanosvg)
    target_compile_definitions(test_icon_map PRIVATE
        CBX_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

    add_test(NAME test_icon_map COMMAND test_icon_map)

    # Task 17: SVG-to-SDL2 texture cache
    add_executable(test_icon_cache tests/test_icon_cache.c)
    target_link_libraries(test_icon_cache PRIVATE
        controllerbox PkgConfig::CMOCKA nanosvg cbx_test_support)
    target_compile_definitions(test_icon_cache PRIVATE
        CBX_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME test_icon_cache COMMAND test_icon_cache)
    set_tests_properties(test_icon_cache PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # Task 18: Runtime icon lookup API
    add_executable(test_icon_lookup tests/test_icon_lookup.c)
    target_link_libraries(test_icon_lookup PRIVATE
        controllerbox PkgConfig::CMOCKA nanosvg cbx_test_support)
    target_compile_definitions(test_icon_lookup PRIVATE
        CBX_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME test_icon_lookup COMMAND test_icon_lookup)
    set_tests_properties(test_icon_lookup PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # Task 19: Renderer init, theme system, and text rendering cache.
    add_executable(test_renderer_init tests/test_renderer_init.c)
    target_link_libraries(test_renderer_init PRIVATE controllerbox PkgConfig::CMOCKA)
    add_test(NAME test_renderer_init COMMAND test_renderer_init)
    set_tests_properties(test_renderer_init PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # Text rendering cache test.  Needs a TTF font path at compile time.
    # Search common locations for DejaVuSans.ttf.
    set(CBX_FONT_PATH "")
    foreach(candidate
            /nix/store/zzs2q7lk5mn6y2rywd3snhak7098zs66-system-path/share/X11/fonts/DejaVuSans.ttf
            /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf
            /usr/share/fonts/dejavu/DejaVuSans.ttf
            /usr/share/fonts/TTF/DejaVuSans.ttf)
        if(EXISTS ${candidate})
            set(CBX_FONT_PATH ${candidate})
            break()
        endif()
    endforeach()
    add_executable(test_text tests/test_text.c)
    target_link_libraries(test_text PRIVATE controllerbox PkgConfig::CMOCKA)
    target_compile_definitions(test_text PRIVATE
        CBX_FONT_PATH="${CBX_FONT_PATH}")
    add_test(NAME test_text COMMAND test_text)
    set_tests_properties(test_text PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # Regression test for font initialization and text rendering
    # (BUG-0001, Task 3): exercises the full production font path
    # (cbx_font_path() → cbx_manager_init() → cbx_text_render()) and
    # the invalid-font-path failure path.  Headless-safe.
    add_executable(test_font_init tests/test_font_init.c)
    target_link_libraries(test_font_init PRIVATE controllerbox PkgConfig::CMOCKA)
    target_compile_definitions(test_font_init PRIVATE
        CBX_FONT_PATH="${CBX_FONT_PATH}")
    add_test(NAME test_font_init COMMAND test_font_init)
    set_tests_properties(test_font_init PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # Task 20: Widget base and concrete widgets (Button, Label, Image, Panel).
    add_executable(test_widgets tests/test_widgets.c)
    target_link_libraries(test_widgets PRIVATE controllerbox PkgConfig::CMOCKA)
    target_compile_definitions(test_widgets PRIVATE
        CBX_FONT_PATH="${CBX_FONT_PATH}")
    add_test(NAME test_widgets COMMAND test_widgets)
    set_tests_properties(test_widgets PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # Task 21: List, Grid, TabBar, and ProgressBar widgets.
    add_executable(test_widget_list tests/test_widget_list.c)
    target_link_libraries(test_widget_list PRIVATE controllerbox PkgConfig::CMOCKA)
    target_compile_definitions(test_widget_list PRIVATE
        CBX_FONT_PATH="${CBX_FONT_PATH}")
    add_test(NAME test_widget_list COMMAND test_widget_list)
    set_tests_properties(test_widget_list PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    add_executable(test_widget_grid tests/test_widget_grid.c)
    target_link_libraries(test_widget_grid PRIVATE controllerbox PkgConfig::CMOCKA)
    add_test(NAME test_widget_grid COMMAND test_widget_grid)
    set_tests_properties(test_widget_grid PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    add_executable(test_widget_tabbar tests/test_widget_tabbar.c)
    target_link_libraries(test_widget_tabbar PRIVATE controllerbox PkgConfig::CMOCKA)
    target_compile_definitions(test_widget_tabbar PRIVATE
        CBX_FONT_PATH="${CBX_FONT_PATH}")
    add_test(NAME test_widget_tabbar COMMAND test_widget_tabbar)
    set_tests_properties(test_widget_tabbar PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    add_executable(test_widget_progress tests/test_widget_progress.c)
    target_link_libraries(test_widget_progress PRIVATE controllerbox PkgConfig::CMOCKA)
    add_test(NAME test_widget_progress COMMAND test_widget_progress)
    set_tests_properties(test_widget_progress PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # Task 22: Focus chain system and input event mapping.
    add_executable(test_focus tests/test_focus.c)
    target_link_libraries(test_focus PRIVATE controllerbox PkgConfig::CMOCKA)
    add_test(NAME test_focus COMMAND test_focus)

    add_executable(test_input_map tests/test_input_map.c)
    target_link_libraries(test_input_map PRIVATE controllerbox PkgConfig::CMOCKA)
    add_test(NAME test_input_map COMMAND test_input_map)

    # Task 23 — Animation primitives and dirty rect optimization
    add_executable(test_animation tests/test_animation.c)
    target_link_libraries(test_animation PRIVATE controllerbox PkgConfig::CMOCKA)
    add_test(NAME test_animation COMMAND test_animation)
    set_tests_properties(test_animation PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # Task 24 — Pre-built overlay surface infrastructure
    add_executable(test_surface_build tests/test_surface_build.c)
    target_link_libraries(test_surface_build PRIVATE controllerbox PkgConfig::CMOCKA)
    add_test(NAME test_surface_build COMMAND test_surface_build)
    set_tests_properties(test_surface_build PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # Task 28 — Overlay state machine and lifecycle.
    add_executable(test_overlay_lifecycle tests/test_overlay_lifecycle.c)
    target_link_libraries(test_overlay_lifecycle PRIVATE controllerbox cbx_test_support)
    add_test(NAME test_overlay_lifecycle COMMAND test_overlay_lifecycle)

    # Task 25 — Identity extraction from source device properties.
    add_executable(test_identity tests/test_identity.c)
    target_link_libraries(test_identity PRIVATE controllerbox PkgConfig::CMOCKA)
    add_test(NAME test_identity COMMAND test_identity)

    add_executable(test_assign tests/test_assign.c)
    target_link_libraries(test_assign PRIVATE controllerbox PkgConfig::CMOCKA)
    add_test(NAME test_assign COMMAND test_assign)

    add_executable(test_assign_persist tests/test_assign_persist.c)
    target_link_libraries(test_assign_persist PRIVATE controllerbox PkgConfig::CMOCKA)
    add_test(NAME test_assign_persist COMMAND test_assign_persist)

    add_executable(test_identity_downgrade tests/test_identity_downgrade.c)
    target_link_libraries(test_identity_downgrade PRIVATE controllerbox PkgConfig::CMOCKA)
    add_test(NAME test_identity_downgrade COMMAND test_identity_downgrade)

    add_executable(test_order_restore tests/test_order_restore.c)
    target_link_libraries(test_order_restore PRIVATE controllerbox cbx_test_support)
    add_test(NAME test_order_restore COMMAND test_order_restore)

    # Task 29 — Character select grid rendering and Player Mode navigation.
    add_executable(test_grid_render tests/test_grid_render.c)
    target_link_libraries(test_grid_render PRIVATE controllerbox cbx_test_support)
    add_test(NAME test_grid_render COMMAND test_grid_render)
    set_tests_properties(test_grid_render PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    add_executable(test_player_mode tests/test_player_mode.c)
    target_link_libraries(test_player_mode PRIVATE controllerbox cbx_test_support)
    add_test(NAME test_player_mode COMMAND test_player_mode)

    add_executable(test_host_mode tests/test_host_mode.c)
    target_link_libraries(test_host_mode PRIVATE controllerbox cbx_test_support)
    add_test(NAME test_host_mode COMMAND test_host_mode)

    add_executable(test_conflict tests/test_conflict.c)
    target_link_libraries(test_conflict PRIVATE controllerbox cbx_test_support)
    add_test(NAME test_conflict COMMAND test_conflict)
    set_tests_properties(test_conflict PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # Task 5 — Overlay deterministic framebuffer visual tests (SPEC §4.10).
    add_executable(test_overlay_visual tests/test_overlay_visual.c)
    target_link_libraries(test_overlay_visual PRIVATE controllerbox cbx_test_support)
    target_compile_definitions(test_overlay_visual PRIVATE
        CBX_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME test_overlay_visual COMMAND test_overlay_visual)
    set_tests_properties(test_overlay_visual PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # Task 31 — Profile cycling and dynamic columns.
    add_executable(test_profile_cycle tests/test_profile_cycle.c)
    target_link_libraries(test_profile_cycle PRIVATE controllerbox cbx_test_support)
    add_test(NAME test_profile_cycle COMMAND test_profile_cycle)

    add_executable(test_dynamic_columns tests/test_dynamic_columns.c)
    target_link_libraries(test_dynamic_columns PRIVATE controllerbox cbx_test_support)
    add_test(NAME test_dynamic_columns COMMAND test_dynamic_columns)

    # Task 32 — Overlay trigger registration and activation/close.
    add_executable(test_trigger tests/test_trigger.c)
    target_link_libraries(test_trigger PRIVATE controllerbox cbx_test_support)
    add_test(NAME test_trigger COMMAND test_trigger)

    add_executable(test_close tests/test_close.c)
    target_link_libraries(test_close PRIVATE controllerbox cbx_test_support)
    add_test(NAME test_close COMMAND test_close)

    # Task 33 — Overlay integration test: full lifecycle with mock DBus +
    # SDL2 dummy driver (trigger → activate → navigate → profile → conflict →
    # host mode → close → assignments saved).
    add_executable(test_overlay_integration tests/test_overlay_integration.c)
    target_link_libraries(test_overlay_integration PRIVATE controllerbox cbx_test_support)
    add_test(NAME test_overlay_integration COMMAND test_overlay_integration)
    set_tests_properties(test_overlay_integration PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # Help later tasks: tests that need the dummy driver can set it here.
    set_tests_properties(test_sdl_dummy PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # Task 34 — Manager skeleton and tab bar.
    add_executable(test_manager_tabs tests/test_manager_tabs.c)
    target_link_libraries(test_manager_tabs PRIVATE controllerbox PkgConfig::CMOCKA)
    target_compile_definitions(test_manager_tabs PRIVATE CBX_FONT_PATH="${CBX_FONT_PATH}")
    add_test(NAME test_manager_tabs COMMAND test_manager_tabs)
    set_tests_properties(test_manager_tabs PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # Task 35 — Controllers tab.
    add_executable(test_controllers_tab tests/test_controllers_tab.c)
    target_link_libraries(test_controllers_tab PRIVATE controllerbox cbx_test_support PkgConfig::CMOCKA)
    target_compile_definitions(test_controllers_tab PRIVATE CBX_FONT_PATH="${CBX_FONT_PATH}")
    add_test(NAME test_controllers_tab COMMAND test_controllers_tab)
    set_tests_properties(test_controllers_tab PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # Task 36 — Profiles tab.
    add_executable(test_profiles_tab tests/test_profiles_tab.c)
    target_link_libraries(test_profiles_tab PRIVATE controllerbox cbx_test_support PkgConfig::CMOCKA)
    target_compile_definitions(test_profiles_tab PRIVATE CBX_FONT_PATH="${CBX_FONT_PATH}")
    add_test(NAME test_profiles_tab COMMAND test_profiles_tab)
    set_tests_properties(test_profiles_tab PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # Task 37 — Profile editor: controller diagram and binding list mode.
    add_executable(test_profile_diagram tests/test_profile_diagram.c)
    target_link_libraries(test_profile_diagram PRIVATE controllerbox cbx_test_support PkgConfig::CMOCKA)
    add_test(NAME test_profile_diagram COMMAND test_profile_diagram)
    set_tests_properties(test_profile_diagram PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    add_executable(test_editor_list_mode tests/test_editor_list_mode.c)
    target_link_libraries(test_editor_list_mode PRIVATE controllerbox cbx_test_support PkgConfig::CMOCKA)
    target_compile_definitions(test_editor_list_mode PRIVATE CBX_FONT_PATH="${CBX_FONT_PATH}")
    add_test(NAME test_editor_list_mode COMMAND test_editor_list_mode)
    set_tests_properties(test_editor_list_mode PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # Task 38 — Profile editor: sequential binding mode and validation.
    add_executable(test_profile_validate tests/test_profile_validate.c)
    target_link_libraries(test_profile_validate PRIVATE controllerbox cbx_test_support PkgConfig::CMOCKA)
    add_test(NAME test_profile_validate COMMAND test_profile_validate)

    add_executable(test_editor_seq_mode tests/test_editor_seq_mode.c)
    target_link_libraries(test_editor_seq_mode PRIVATE controllerbox cbx_test_support PkgConfig::CMOCKA)
    target_compile_definitions(test_editor_seq_mode PRIVATE CBX_FONT_PATH="${CBX_FONT_PATH}")
    add_test(NAME test_editor_seq_mode COMMAND test_editor_seq_mode)
    set_tests_properties(test_editor_seq_mode PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # Task 39 — Profile save and Settings tab.
    add_executable(test_profile_save tests/test_profile_save.c)
    target_link_libraries(test_profile_save PRIVATE controllerbox PkgConfig::CMOCKA)
    add_test(NAME test_profile_save COMMAND test_profile_save)

    add_executable(test_settings_tab tests/test_settings_tab.c)
    target_link_libraries(test_settings_tab PRIVATE controllerbox cbx_test_support PkgConfig::CMOCKA)
    target_compile_definitions(test_settings_tab PRIVATE CBX_FONT_PATH="${CBX_FONT_PATH}")
    add_test(NAME test_settings_tab COMMAND test_settings_tab)
    set_tests_properties(test_settings_tab PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # Task 40 — Systemd service installation and manager integration test.
    add_executable(test_service_install tests/test_service_install.c)
    target_link_libraries(test_service_install PRIVATE controllerbox cbx_test_support PkgConfig::CMOCKA)
    add_test(NAME test_service_install COMMAND test_service_install)

    add_executable(test_manager_integration tests/test_manager_integration.c)
    target_link_libraries(test_manager_integration PRIVATE controllerbox cbx_test_support PkgConfig::CMOCKA)
    target_compile_definitions(test_manager_integration PRIVATE CBX_FONT_PATH="${CBX_FONT_PATH}")
    add_test(NAME test_manager_integration COMMAND test_manager_integration)
    set_tests_properties(test_manager_integration PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # BUG-0003 — Production-path regression test.
    add_executable(test_manager_production tests/test_manager_production.c)
    target_link_libraries(test_manager_production PRIVATE controllerbox cbx_test_support PkgConfig::CMOCKA)
    target_compile_definitions(test_manager_production PRIVATE CBX_FONT_PATH="${CBX_FONT_PATH}")
    add_test(NAME test_manager_production COMMAND test_manager_production)
    set_tests_properties(test_manager_production PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # Task 1 — Framebuffer pixel assertion self-test.
    # Verifies fb_read_pixels, fb_region_has_content, fb_region_has_color,
    # fb_frames_differ, fb_golden_compare, fb_save_png, fb_save_diff.
    add_executable(test_fb_assert tests/test_fb_assert.c)
    target_link_libraries(test_fb_assert PRIVATE cbx_test_support)
    add_test(NAME test_fb_assert COMMAND test_fb_assert)
    set_tests_properties(test_fb_assert PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # Task 6 — Manager DBus backend injection point.
    # Verifies cbx_manager_init_with_dbus wires a mock backend into the
    # controllers tab without post-init field replacement.
    add_executable(test_manager_dbus_inject tests/test_manager_dbus_inject.c)
    target_link_libraries(test_manager_dbus_inject PRIVATE controllerbox cbx_test_support PkgConfig::CMOCKA)
    target_compile_definitions(test_manager_dbus_inject PRIVATE CBX_FONT_PATH="${CBX_FONT_PATH}")
    add_test(NAME test_manager_dbus_inject COMMAND test_manager_dbus_inject)
    set_tests_properties(test_manager_dbus_inject PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # Task 7 — Manager deterministic framebuffer visual tests (SPEC §5.6).
    add_executable(test_manager_visual tests/test_manager_visual.c)
    target_link_libraries(test_manager_visual PRIVATE controllerbox cbx_test_support)
    target_compile_definitions(test_manager_visual PRIVATE
        CBX_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME test_manager_visual COMMAND test_manager_visual)
    set_tests_properties(test_manager_visual PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # --- Task 3: Overlay service initialization -----------------------
    # Tests that run_overlay_service() handles SDL init failure and dry-run.
    add_executable(test_overlay_service tests/test_overlay_service.c)
    target_link_libraries(test_overlay_service PRIVATE controllerbox cbx_test_support PkgConfig::CMOCKA)
    add_test(NAME test_overlay_service COMMAND test_overlay_service)
    set_tests_properties(test_overlay_service PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # --- Task 11: Overlay production-dispatch interaction tests ---------
    add_executable(test_overlay_interaction tests/test_overlay_interaction.c)
    target_link_libraries(test_overlay_interaction PRIVATE controllerbox cbx_test_support PkgConfig::CMOCKA)
    add_test(NAME test_overlay_interaction COMMAND test_overlay_interaction)
    set_tests_properties(test_overlay_interaction PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # --- Task 9: Visible reusable overlay and runtime reconciliation ----
    add_executable(test_overlay_reconcile tests/test_overlay_reconcile.c)
    target_link_libraries(test_overlay_reconcile PRIVATE controllerbox cbx_test_support PkgConfig::CMOCKA)
    add_test(NAME test_overlay_reconcile COMMAND test_overlay_reconcile)
    set_tests_properties(test_overlay_reconcile PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # --- Task 8: Golden image baselines and comparison -----------------
    # Compares live framebuffer captures against reviewed baseline PNGs.
    # Set CBX_GENERATE_GOLDEN=1 to regenerate baselines instead of comparing.
    add_executable(test_golden tests/test_golden.c)
    target_link_libraries(test_golden PRIVATE controllerbox cbx_test_support PkgConfig::CMOCKA)
    target_compile_definitions(test_golden PRIVATE
        CBX_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME test_golden COMMAND test_golden)
    set_tests_properties(test_golden PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # --- Task 10: Backend smoke coverage (SPEC §11.1.6) --------------
    # Verifies accelerated backend (OpenGL/OpenGL ES) produces correct
    # pixel output.  Skips (exit 77) when no accelerated backend is
    # available (headless CI).  Does NOT set SDL_VIDEODRIVER=dummy.
    add_executable(test_backend_smoke tests/test_backend_smoke.c)
    target_link_libraries(test_backend_smoke PRIVATE controllerbox cbx_test_support)
    target_compile_definitions(test_backend_smoke PRIVATE
        CBX_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME test_backend_smoke COMMAND test_backend_smoke)
    set_tests_properties(test_backend_smoke PROPERTIES
        SKIP_RETURN_CODE 77)

    # --- Task 7: Interaction acceptance inventory (SPEC §5.7) ----------
    # Machine-readable inventory of every interactive control (M01–M38),
    # overlay action (O01–O12), and disabled/degraded scenario (D01–D08).
    # Verifies enumeration completeness, field population, uniqueness,
    # and lookup.  Links cbx_test_support for interaction_inventory.
    add_executable(test_interaction_inventory tests/test_interaction_inventory.c)
    target_link_libraries(test_interaction_inventory PRIVATE cbx_test_support PkgConfig::CMOCKA)
    add_test(NAME test_interaction_inventory COMMAND test_interaction_inventory)

    # Native InputPlumber DBus signatures must not regress to string mocks.
    find_program(DBUS_DAEMON_EXECUTABLE dbus-daemon REQUIRED)
    get_filename_component(DBUS_BIN_DIR "${DBUS_DAEMON_EXECUTABLE}" DIRECTORY)
    get_filename_component(DBUS_PREFIX "${DBUS_BIN_DIR}" DIRECTORY)
    add_executable(test_native_dbus tests/test_native_dbus.c)
    target_compile_definitions(test_native_dbus PRIVATE
        DBUS_SESSION_CONFIG="${DBUS_PREFIX}/share/dbus-1/session.conf")
    target_link_libraries(test_native_dbus PRIVATE controllerbox PkgConfig::CMOCKA)
    add_test(NAME test_native_dbus COMMAND test_native_dbus)

    add_executable(test_dbus_signatures tests/test_dbus_signatures.c)
    target_link_libraries(test_dbus_signatures PRIVATE controllerbox PkgConfig::CMOCKA)
    add_test(NAME test_dbus_signatures COMMAND test_dbus_signatures)

    # Task 10 — Mandatory installed functional acceptance gate (SPEC §11.1.5).
    # Non-skippable: exercises the full production workflow against a private
    # native-signature InputPlumber-compatible DBus service with an SDL
    # virtual game controller.  Links against the production controllerbox
    # library and uses the real sd-bus backend.  No SKIP_RETURN_CODE.
    add_executable(test_installed_functional tests/test_installed_functional.c)
    target_compile_definitions(test_installed_functional PRIVATE
        DBUS_SESSION_CONFIG="${DBUS_PREFIX}/share/dbus-1/session.conf")
    target_link_libraries(test_installed_functional PRIVATE controllerbox PkgConfig::CMOCKA)
    add_test(NAME test_installed_functional COMMAND test_installed_functional)
    set_tests_properties(test_installed_functional PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # Task 8 — Manager interaction tests: Controllers + Settings tabs
    # through production dispatch (SPEC §5.7).
    add_executable(test_manager_interaction_ctrl tests/test_manager_interaction_ctrl.c)
    target_link_libraries(test_manager_interaction_ctrl PRIVATE controllerbox cbx_test_support PkgConfig::CMOCKA)
    target_compile_definitions(test_manager_interaction_ctrl PRIVATE CBX_FONT_PATH="${CBX_FONT_PATH}")
    add_test(NAME test_manager_interaction_ctrl COMMAND test_manager_interaction_ctrl)
    set_tests_properties(test_manager_interaction_ctrl PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

    # Task 9 — Manager interaction tests: Profiles tab and profile editor
    # through production dispatch (SPEC §5.7).
    add_executable(test_manager_interaction_prof tests/test_manager_interaction_prof.c)
    target_link_libraries(test_manager_interaction_prof PRIVATE controllerbox cbx_test_support PkgConfig::CMOCKA)
    target_compile_definitions(test_manager_interaction_prof PRIVATE CBX_FONT_PATH="${CBX_FONT_PATH}")
    add_test(NAME test_manager_interaction_prof COMMAND test_manager_interaction_prof)
    set_tests_properties(test_manager_interaction_prof PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy")

else()
    message(STATUS "cmocka not found — unit tests skipped (smoke tests still built)")
endif()