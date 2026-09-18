/*
 * test_composite_calls.c — Unit tests for CompositeDevice interface wrappers
 * (Task 13).
 *
 * Tests the ip_composite_* wrappers using the mock backend.  Verifies:
 *   - Method calls (SetInterceptActivation, LoadProfilePath,
 *     LoadProfileFromYaml, GetProfileYaml, SetTargetDevices, Stop)
 *   - Property gets (InterceptMode, TargetDevices, SourceDevicePaths,
 *     PersistentId, Name, Capabilities, OutputCapabilities,
 *     TargetCapabilities, DbusDevices)
 *   - Property sets (InterceptMode)
 *   - Error propagation (categorized error codes)
 *   - NULL / invalid argument handling
 */
#include "dbus_mock.h"
#include "dbus/ip_connection.h"
#include "dbus/ip_composite.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

/* --- Test fixture -------------------------------------------------------- */

typedef struct {
    ip_dbus_mock          mock;
    const ip_dbus_backend *backend;
} composite_fixture;

static int
setup(void **state)
{
    composite_fixture *f = malloc(sizeof(*f));
    if (!f)
        return -1;
    memset(f, 0, sizeof(*f));
    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);
    *state = f;
    return 0;
}

static int
teardown(void **state)
{
    composite_fixture *f = *state;
    if (f) {
        ip_dbus_mock_reset(&f->mock);
        free(f);
    }
    return 0;
}

#define FIX(state) (*(composite_fixture **)(state))
#define COMP_PATH "/org/shadowblip/InputPlumber/CompositeDevice0"

/* --- SetInterceptActivation --------------------------------------------- */

static void
test_set_intercept_activation_success(void **state)
{
    composite_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "SetInterceptActivation", NULL);

    int rc = ip_composite_set_intercept_activation(f->backend, f->mock.bus,
                                                     COMP_PATH,
                                                     "Select,A", "Select+A");
    assert_int_equal(rc, 0);
}

static void
test_set_intercept_activation_error(void **state)
{
    composite_fixture *f = FIX(state);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                               "SetInterceptActivation", IP_ERR_INVALID_ARGS);

    int rc = ip_composite_set_intercept_activation(f->backend, f->mock.bus,
                                                     COMP_PATH,
                                                     "BadEvent", "BadTarget");
    assert_int_equal(rc, IP_ERR_INVALID_ARGS);
}

static void
test_set_intercept_activation_no_expectation(void **state)
{
    composite_fixture *f = FIX(state);
    int rc = ip_composite_set_intercept_activation(f->backend, f->mock.bus,
                                                     COMP_PATH,
                                                     "Select,A", "Select+A");
    assert_int_equal(rc, -ENXIO);
}

static void
test_set_intercept_activation_null_args(void **state)
{
    composite_fixture *f = FIX(state);
    assert_int_equal(ip_composite_set_intercept_activation(
      NULL, f->mock.bus, COMP_PATH, "e", "t"), -EINVAL);
    assert_int_equal(ip_composite_set_intercept_activation(
      f->backend, f->mock.bus, NULL, "e", "t"), -EINVAL);
    assert_int_equal(ip_composite_set_intercept_activation(
      f->backend, f->mock.bus, COMP_PATH, NULL, "t"), -EINVAL);
    assert_int_equal(ip_composite_set_intercept_activation(
      f->backend, f->mock.bus, COMP_PATH, "e", NULL), -EINVAL);
}

/* --- LoadProfilePath ----------------------------------------------------- */

static void
test_load_profile_path_success(void **state)
{
    composite_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "LoadProfilePath", NULL);

    int rc = ip_composite_load_profile_path(f->backend, f->mock.bus,
                                             COMP_PATH,
                                             "/home/user/profile.yaml");
    assert_int_equal(rc, 0);
}

static void
test_load_profile_path_error(void **state)
{
    composite_fixture *f = FIX(state);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                               "LoadProfilePath", IP_ERR_NO_REPLY);

    int rc = ip_composite_load_profile_path(f->backend, f->mock.bus,
                                             COMP_PATH, "/bad/path");
    assert_int_equal(rc, IP_ERR_NO_REPLY);
}

static void
test_load_profile_path_no_expectation(void **state)
{
    composite_fixture *f = FIX(state);
    int rc = ip_composite_load_profile_path(f->backend, f->mock.bus,
                                             COMP_PATH, "/path");
    assert_int_equal(rc, -ENXIO);
}

static void
test_load_profile_path_null_args(void **state)
{
    composite_fixture *f = FIX(state);
    assert_int_equal(ip_composite_load_profile_path(
      NULL, f->mock.bus, COMP_PATH, "/p"), -EINVAL);
    assert_int_equal(ip_composite_load_profile_path(
      f->backend, f->mock.bus, NULL, "/p"), -EINVAL);
    assert_int_equal(ip_composite_load_profile_path(
      f->backend, f->mock.bus, COMP_PATH, NULL), -EINVAL);
}

/* --- LoadProfileFromYaml ------------------------------------------------- */

static void
test_load_profile_from_yaml_success(void **state)
{
    composite_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "LoadProfileFromYaml", NULL);

    const char *yaml = "version: 1\nkind: DeviceProfile\nname: Test\n";
    int rc = ip_composite_load_profile_from_yaml(f->backend, f->mock.bus,
                                                   COMP_PATH, yaml);
    assert_int_equal(rc, 0);
}

static void
test_load_profile_from_yaml_error(void **state)
{
    composite_fixture *f = FIX(state);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                               "LoadProfileFromYaml", IP_ERR_INVALID_ARGS);

    int rc = ip_composite_load_profile_from_yaml(f->backend, f->mock.bus,
                                                   COMP_PATH, "bad yaml");
    assert_int_equal(rc, IP_ERR_INVALID_ARGS);
}

static void
test_load_profile_from_yaml_no_expectation(void **state)
{
    composite_fixture *f = FIX(state);
    int rc = ip_composite_load_profile_from_yaml(f->backend, f->mock.bus,
                                                   COMP_PATH, "yaml");
    assert_int_equal(rc, -ENXIO);
}

static void
test_load_profile_from_yaml_null_args(void **state)
{
    composite_fixture *f = FIX(state);
    assert_int_equal(ip_composite_load_profile_from_yaml(
      NULL, f->mock.bus, COMP_PATH, "y"), -EINVAL);
    assert_int_equal(ip_composite_load_profile_from_yaml(
      f->backend, f->mock.bus, NULL, "y"), -EINVAL);
    assert_int_equal(ip_composite_load_profile_from_yaml(
      f->backend, f->mock.bus, COMP_PATH, NULL), -EINVAL);
}

/* --- GetProfileYaml ------------------------------------------------------- */

static void
test_get_profile_yaml_success(void **state)
{
    composite_fixture *f = FIX(state);
    const char *yaml = "version: 1\nkind: DeviceProfile\nname: Test\n";
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "GetProfileYaml", yaml);

    char *out = NULL;
    int rc = ip_composite_get_profile_yaml(f->backend, f->mock.bus,
                                            COMP_PATH, &out);
    assert_int_equal(rc, 0);
    assert_non_null(out);
    assert_string_equal(out, yaml);
    free(out);
}

static void
test_get_profile_yaml_error(void **state)
{
    composite_fixture *f = FIX(state);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                               "GetProfileYaml", IP_ERR_NO_REPLY);

    char *out = NULL;
    int rc = ip_composite_get_profile_yaml(f->backend, f->mock.bus,
                                            COMP_PATH, &out);
    assert_int_equal(rc, IP_ERR_NO_REPLY);
    assert_null(out);
}

static void
test_get_profile_yaml_no_expectation(void **state)
{
    composite_fixture *f = FIX(state);
    char *out = NULL;
    int rc = ip_composite_get_profile_yaml(f->backend, f->mock.bus,
                                            COMP_PATH, &out);
    assert_int_equal(rc, -ENXIO);
    assert_null(out);
}

static void
test_get_profile_yaml_null_args(void **state)
{
    composite_fixture *f = FIX(state);
    char *out = NULL;
    assert_int_equal(ip_composite_get_profile_yaml(
      NULL, f->mock.bus, COMP_PATH, &out), -EINVAL);
    assert_int_equal(ip_composite_get_profile_yaml(
      f->backend, f->mock.bus, NULL, &out), -EINVAL);
    assert_int_equal(ip_composite_get_profile_yaml(
      f->backend, f->mock.bus, COMP_PATH, NULL), -EINVAL);
}

/* --- SetTargetDevices --------------------------------------------------- */

static void
test_composite_set_target_devices_success(void **state)
{
    composite_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "SetTargetDevices", NULL);

    int rc = ip_composite_set_target_devices(f->backend, f->mock.bus,
                                               COMP_PATH, "xb360,ds5");
    assert_int_equal(rc, 0);
}

static void
test_composite_set_target_devices_error(void **state)
{
    composite_fixture *f = FIX(state);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                               "SetTargetDevices", IP_ERR_INVALID_ARGS);

    int rc = ip_composite_set_target_devices(f->backend, f->mock.bus,
                                               COMP_PATH, "badtype");
    assert_int_equal(rc, IP_ERR_INVALID_ARGS);
}

static void
test_composite_set_target_devices_no_expectation(void **state)
{
    composite_fixture *f = FIX(state);
    int rc = ip_composite_set_target_devices(f->backend, f->mock.bus,
                                               COMP_PATH, "xb360");
    assert_int_equal(rc, -ENXIO);
}

static void
test_composite_set_target_devices_null_args(void **state)
{
    composite_fixture *f = FIX(state);
    assert_int_equal(ip_composite_set_target_devices(
      NULL, f->mock.bus, COMP_PATH, "t"), -EINVAL);
    assert_int_equal(ip_composite_set_target_devices(
      f->backend, f->mock.bus, NULL, "t"), -EINVAL);
    assert_int_equal(ip_composite_set_target_devices(
      f->backend, f->mock.bus, COMP_PATH, NULL), -EINVAL);
}

/* --- Stop ---------------------------------------------------------------- */

static void
test_composite_stop_success(void **state)
{
    composite_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "Stop", NULL);

    int rc = ip_composite_stop(f->backend, f->mock.bus, COMP_PATH);
    assert_int_equal(rc, 0);
}

static void
test_composite_stop_error(void **state)
{
    composite_fixture *f = FIX(state);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                               "Stop", IP_ERR_NO_REPLY);

    int rc = ip_composite_stop(f->backend, f->mock.bus, COMP_PATH);
    assert_int_equal(rc, IP_ERR_NO_REPLY);
}

static void
test_composite_stop_no_expectation(void **state)
{
    composite_fixture *f = FIX(state);
    int rc = ip_composite_stop(f->backend, f->mock.bus, COMP_PATH);
    assert_int_equal(rc, -ENXIO);
}

static void
test_composite_stop_null_args(void **state)
{
    composite_fixture *f = FIX(state);
    assert_int_equal(ip_composite_stop(NULL, f->mock.bus, COMP_PATH), -EINVAL);
    assert_int_equal(ip_composite_stop(f->backend, f->mock.bus, NULL), -EINVAL);
}

/* --- InterceptMode get/set ----------------------------------------------- */

static void
test_get_intercept_mode_success(void **state)
{
    composite_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", "1");

    char *out = NULL;
    int rc = ip_composite_get_intercept_mode(f->backend, f->mock.bus,
                                               COMP_PATH, &out);
    assert_int_equal(rc, 0);
    assert_non_null(out);
    assert_string_equal(out, "1");
    free(out);
}

static void
test_get_intercept_mode_error(void **state)
{
    composite_fixture *f = FIX(state);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                               "InterceptMode", IP_ERR_NO_REPLY);

    char *out = NULL;
    int rc = ip_composite_get_intercept_mode(f->backend, f->mock.bus,
                                               COMP_PATH, &out);
    assert_int_equal(rc, IP_ERR_NO_REPLY);
    assert_null(out);
}

static void
test_get_intercept_mode_no_expectation(void **state)
{
    composite_fixture *f = FIX(state);
    char *out = NULL;
    int rc = ip_composite_get_intercept_mode(f->backend, f->mock.bus,
                                               COMP_PATH, &out);
    assert_int_equal(rc, -ENXIO);
}

static void
test_get_intercept_mode_null_args(void **state)
{
    composite_fixture *f = FIX(state);
    char *out = NULL;
    assert_int_equal(ip_composite_get_intercept_mode(
      NULL, f->mock.bus, COMP_PATH, &out), -EINVAL);
    assert_int_equal(ip_composite_get_intercept_mode(
      f->backend, f->mock.bus, NULL, &out), -EINVAL);
    assert_int_equal(ip_composite_get_intercept_mode(
      f->backend, f->mock.bus, COMP_PATH, NULL), -EINVAL);
}

static void
test_set_intercept_mode_success(void **state)
{
    composite_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", NULL);

    int rc = ip_composite_set_intercept_mode(f->backend, f->mock.bus,
                                               COMP_PATH, "1");
    assert_int_equal(rc, 0);
}

static void
test_set_intercept_mode_error(void **state)
{
    composite_fixture *f = FIX(state);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                               "InterceptMode", IP_ERR_ACCESS_DENIED);

    int rc = ip_composite_set_intercept_mode(f->backend, f->mock.bus,
                                               COMP_PATH, "2");
    assert_int_equal(rc, IP_ERR_ACCESS_DENIED);
}

static void
test_set_intercept_mode_no_expectation(void **state)
{
    composite_fixture *f = FIX(state);
    int rc = ip_composite_set_intercept_mode(f->backend, f->mock.bus,
                                               COMP_PATH, "1");
    assert_int_equal(rc, -ENXIO);
}

static void
test_set_intercept_mode_null_args(void **state)
{
    composite_fixture *f = FIX(state);
    assert_int_equal(ip_composite_set_intercept_mode(
      NULL, f->mock.bus, COMP_PATH, "1"), -EINVAL);
    assert_int_equal(ip_composite_set_intercept_mode(
      f->backend, f->mock.bus, NULL, "1"), -EINVAL);
    assert_int_equal(ip_composite_set_intercept_mode(
      f->backend, f->mock.bus, COMP_PATH, NULL), -EINVAL);
}

/* --- Property getters (array properties) --------------------------------- */

static void
test_get_target_devices_success(void **state)
{
    composite_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "TargetDevices",
                           "/org/shadowblip/InputPlumber/devices/target/gamepad0");

    char *out = NULL;
    int rc = ip_composite_get_target_devices(f->backend, f->mock.bus,
                                                COMP_PATH, &out);
    assert_int_equal(rc, 0);
    assert_non_null(out);
    assert_string_equal(out,
      "/org/shadowblip/InputPlumber/devices/target/gamepad0");
    free(out);
}

static void
test_get_target_devices_error(void **state)
{
    composite_fixture *f = FIX(state);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                               "TargetDevices", IP_ERR_NO_REPLY);

    char *out = NULL;
    int rc = ip_composite_get_target_devices(f->backend, f->mock.bus,
                                                COMP_PATH, &out);
    assert_int_equal(rc, IP_ERR_NO_REPLY);
}

static void
test_get_target_devices_no_expectation(void **state)
{
    composite_fixture *f = FIX(state);
    char *out = NULL;
    int rc = ip_composite_get_target_devices(f->backend, f->mock.bus,
                                                COMP_PATH, &out);
    assert_int_equal(rc, -ENXIO);
}

static void
test_get_target_devices_null_args(void **state)
{
    composite_fixture *f = FIX(state);
    char *out = NULL;
    assert_int_equal(ip_composite_get_target_devices(
      NULL, f->mock.bus, COMP_PATH, &out), -EINVAL);
    assert_int_equal(ip_composite_get_target_devices(
      f->backend, f->mock.bus, NULL, &out), -EINVAL);
    assert_int_equal(ip_composite_get_target_devices(
      f->backend, f->mock.bus, COMP_PATH, NULL), -EINVAL);
}

static void
test_get_source_device_paths_success(void **state)
{
    composite_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "SourceDevicePaths",
                           "/dev/input/event0,/dev/input/event1");

    char *out = NULL;
    int rc = ip_composite_get_source_device_paths(f->backend, f->mock.bus,
                                                     COMP_PATH, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out, "/dev/input/event0,/dev/input/event1");
    free(out);
}

static void
test_get_source_device_paths_null_args(void **state)
{
    composite_fixture *f = FIX(state);
    char *out = NULL;
    assert_int_equal(ip_composite_get_source_device_paths(
      NULL, f->mock.bus, COMP_PATH, &out), -EINVAL);
    assert_int_equal(ip_composite_get_source_device_paths(
      f->backend, f->mock.bus, COMP_PATH, NULL), -EINVAL);
}

/* --- Property getters (string properties) -------------------------------- */

static void
test_get_persistent_id_success(void **state)
{
    composite_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "PersistentId", "usb-1234-5678");

    char *out = NULL;
    int rc = ip_composite_get_persistent_id(f->backend, f->mock.bus,
                                              COMP_PATH, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out, "usb-1234-5678");
    free(out);
}

static void
test_get_persistent_id_null_args(void **state)
{
    composite_fixture *f = FIX(state);
    char *out = NULL;
    assert_int_equal(ip_composite_get_persistent_id(
      NULL, f->mock.bus, COMP_PATH, &out), -EINVAL);
    assert_int_equal(ip_composite_get_persistent_id(
      f->backend, f->mock.bus, COMP_PATH, NULL), -EINVAL);
}

static void
test_get_name_success(void **state)
{
    composite_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "Name", "8BitDo Ultimate 2C");

    char *out = NULL;
    int rc = ip_composite_get_name(f->backend, f->mock.bus,
                                     COMP_PATH, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out, "8BitDo Ultimate 2C");
    free(out);
}

static void
test_get_name_null_args(void **state)
{
    composite_fixture *f = FIX(state);
    char *out = NULL;
    assert_int_equal(ip_composite_get_name(
      NULL, f->mock.bus, COMP_PATH, &out), -EINVAL);
    assert_int_equal(ip_composite_get_name(
      f->backend, f->mock.bus, COMP_PATH, NULL), -EINVAL);
}

/* --- ProfileName / ProfilePath property getters --- */

static void
test_get_profile_name_success(void **state)
{
    composite_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "ProfileName", "default");

    char *out = NULL;
    int rc = ip_composite_get_profile_name(f->backend, f->mock.bus,
                                              COMP_PATH, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out, "default");
    free(out);
}

static void
test_get_profile_name_null_args(void **state)
{
    composite_fixture *f = FIX(state);
    char *out = NULL;
    assert_int_equal(ip_composite_get_profile_name(
      NULL, f->mock.bus, COMP_PATH, &out), -EINVAL);
    assert_int_equal(ip_composite_get_profile_name(
      f->backend, f->mock.bus, NULL, &out), -EINVAL);
    assert_int_equal(ip_composite_get_profile_name(
      f->backend, f->mock.bus, COMP_PATH, NULL), -EINVAL);
}

static void
test_get_profile_path_success(void **state)
{
    composite_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "ProfilePath", "/usr/share/inputplumber/profiles/default.yaml");

    char *out = NULL;
    int rc = ip_composite_get_profile_path(f->backend, f->mock.bus,
                                              COMP_PATH, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out, "/usr/share/inputplumber/profiles/default.yaml");
    free(out);
}

static void
test_get_profile_path_null_args(void **state)
{
    composite_fixture *f = FIX(state);
    char *out = NULL;
    assert_int_equal(ip_composite_get_profile_path(
      NULL, f->mock.bus, COMP_PATH, &out), -EINVAL);
    assert_int_equal(ip_composite_get_profile_path(
      f->backend, f->mock.bus, NULL, &out), -EINVAL);
    assert_int_equal(ip_composite_get_profile_path(
      f->backend, f->mock.bus, COMP_PATH, NULL), -EINVAL);
}

/* --- Property getters (capability arrays) -------------------------------- */

static void
test_get_capabilities_success(void **state)
{
    composite_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "Capabilities", "gamepad:dpad,gamepad:btn_a");

    char *out = NULL;
    int rc = ip_composite_get_capabilities(f->backend, f->mock.bus,
                                             COMP_PATH, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out, "gamepad:dpad,gamepad:btn_a");
    free(out);
}

static void
test_get_capabilities_null_args(void **state)
{
    composite_fixture *f = FIX(state);
    char *out = NULL;
    assert_int_equal(ip_composite_get_capabilities(
      NULL, f->mock.bus, COMP_PATH, &out), -EINVAL);
}

static void
test_get_output_capabilities_success(void **state)
{
    composite_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "OutputCapabilities", "rumble,led");

    char *out = NULL;
    int rc = ip_composite_get_output_capabilities(f->backend, f->mock.bus,
                                                     COMP_PATH, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out, "rumble,led");
    free(out);
}

static void
test_get_output_capabilities_null_args(void **state)
{
    composite_fixture *f = FIX(state);
    char *out = NULL;
    assert_int_equal(ip_composite_get_output_capabilities(
      NULL, f->mock.bus, COMP_PATH, &out), -EINVAL);
}

static void
test_get_target_capabilities_success(void **state)
{
    composite_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "TargetCapabilities", "gamepad:dpad,gamepad:btn_a");

    char *out = NULL;
    int rc = ip_composite_get_target_capabilities(f->backend, f->mock.bus,
                                                     COMP_PATH, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out, "gamepad:dpad,gamepad:btn_a");
    free(out);
}

static void
test_get_target_capabilities_null_args(void **state)
{
    composite_fixture *f = FIX(state);
    char *out = NULL;
    assert_int_equal(ip_composite_get_target_capabilities(
      NULL, f->mock.bus, COMP_PATH, &out), -EINVAL);
}

static void
test_get_dbus_devices_success(void **state)
{
    composite_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "DbusDevices",
                           "/org/shadowblip/InputPlumber/DBusDevice0");

    char *out = NULL;
    int rc = ip_composite_get_dbus_devices(f->backend, f->mock.bus,
                                              COMP_PATH, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out,
      "/org/shadowblip/InputPlumber/DBusDevice0");
    free(out);
}

static void
test_get_dbus_devices_null_args(void **state)
{
    composite_fixture *f = FIX(state);
    char *out = NULL;
    assert_int_equal(ip_composite_get_dbus_devices(
      NULL, f->mock.bus, COMP_PATH, &out), -EINVAL);
    assert_int_equal(ip_composite_get_dbus_devices(
      f->backend, f->mock.bus, COMP_PATH, NULL), -EINVAL);
}

/* --- Simple (no fixture) tests ------------------------------------------ */

static void
test_intercept_mode_constants(void **state)
{
    (void)state;
    assert_int_equal(IP_INTERCEPT_NONE, 0);
    assert_int_equal(IP_INTERCEPT_PASS, 1);
    assert_int_equal(IP_INTERCEPT_ALL, 2);
    assert_int_equal(IP_INTERCEPT_GAMEPAD_ONLY, 3);
}

/* --- Test runner -------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* Simple tests. */
        cmocka_unit_test(test_intercept_mode_constants),

        /* SetInterceptActivation. */
        cmocka_unit_test_setup_teardown(test_set_intercept_activation_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_set_intercept_activation_error,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_set_intercept_activation_no_expectation,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_set_intercept_activation_null_args,
                                          setup, teardown),

        /* LoadProfilePath. */
        cmocka_unit_test_setup_teardown(test_load_profile_path_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_load_profile_path_error,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_load_profile_path_no_expectation,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_load_profile_path_null_args,
                                          setup, teardown),

        /* LoadProfileFromYaml. */
        cmocka_unit_test_setup_teardown(test_load_profile_from_yaml_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_load_profile_from_yaml_error,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_load_profile_from_yaml_no_expectation,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_load_profile_from_yaml_null_args,
                                          setup, teardown),

        /* GetProfileYaml. */
        cmocka_unit_test_setup_teardown(test_get_profile_yaml_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_profile_yaml_error,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_profile_yaml_no_expectation,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_profile_yaml_null_args,
                                          setup, teardown),

        /* SetTargetDevices. */
        cmocka_unit_test_setup_teardown(test_composite_set_target_devices_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_composite_set_target_devices_error,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_composite_set_target_devices_no_expectation,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_composite_set_target_devices_null_args,
                                          setup, teardown),

        /* Stop. */
        cmocka_unit_test_setup_teardown(test_composite_stop_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_composite_stop_error,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_composite_stop_no_expectation,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_composite_stop_null_args,
                                          setup, teardown),

        /* InterceptMode get. */
        cmocka_unit_test_setup_teardown(test_get_intercept_mode_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_intercept_mode_error,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_intercept_mode_no_expectation,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_intercept_mode_null_args,
                                          setup, teardown),

        /* InterceptMode set. */
        cmocka_unit_test_setup_teardown(test_set_intercept_mode_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_set_intercept_mode_error,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_set_intercept_mode_no_expectation,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_set_intercept_mode_null_args,
                                          setup, teardown),

        /* TargetDevices get. */
        cmocka_unit_test_setup_teardown(test_get_target_devices_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_target_devices_error,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_target_devices_no_expectation,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_target_devices_null_args,
                                          setup, teardown),

        /* SourceDevicePaths get. */
        cmocka_unit_test_setup_teardown(test_get_source_device_paths_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_source_device_paths_null_args,
                                          setup, teardown),

        /* PersistentId get. */
        cmocka_unit_test_setup_teardown(test_get_persistent_id_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_persistent_id_null_args,
                                          setup, teardown),

        /* Name get. */
        cmocka_unit_test_setup_teardown(test_get_name_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_name_null_args,
                                          setup, teardown),

        /* ProfileName / ProfilePath get. */
        cmocka_unit_test_setup_teardown(test_get_profile_name_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_profile_name_null_args,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_profile_path_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_profile_path_null_args,
                                          setup, teardown),

        /* Capabilities get. */
        cmocka_unit_test_setup_teardown(test_get_capabilities_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_capabilities_null_args,
                                          setup, teardown),

        /* OutputCapabilities get. */
        cmocka_unit_test_setup_teardown(test_get_output_capabilities_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_output_capabilities_null_args,
                                          setup, teardown),

        /* TargetCapabilities get. */
        cmocka_unit_test_setup_teardown(test_get_target_capabilities_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_target_capabilities_null_args,
                                          setup, teardown),

        /* DbusDevices get. */
        cmocka_unit_test_setup_teardown(test_get_dbus_devices_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_dbus_devices_null_args,
                                          setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}