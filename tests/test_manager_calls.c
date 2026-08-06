/*
 * test_manager_calls.c — Unit tests for Manager interface wrappers (Task 12).
 *
 * Tests the ip_manager_* wrappers using the mock backend.  Verifies:
 *   - Method calls (CreateTargetDevice, StopTargetDevice, AttachTargetDevice,
 *     SetTargetDevices) dispatch through the vtable correctly.
 *   - Method call return values (CreateTargetDevice → path).
 *   - Property gets (GamepadOrder, SupportedTargetDeviceIds,
 *     SupportedTargetDevices).
 *   - Property sets (GamepadOrder) with device model validation.
 *   - Error propagation (categorized error codes).
 *   - NULL / invalid argument handling.
 */
#include "dbus_mock.h"
#include "dbus/ip_connection.h"
#include "dbus/ip_device_model.h"
#include "dbus/ip_manager.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

/* --- Test fixture -------------------------------------------------------- */

typedef struct {
    ip_dbus_mock          mock;
    const ip_dbus_backend *backend;
    cbx_device_model      model;
} manager_fixture;

static int
setup(void **state)
{
    manager_fixture *f = malloc(sizeof(*f));
    if (!f)
        return -1;
    memset(f, 0, sizeof(*f));
    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);
    cbx_device_model_init(&f->model);
    *state = f;
    return 0;
}

static int
teardown(void **state)
{
    manager_fixture *f = *state;
    if (f) {
        ip_dbus_mock_reset(&f->mock);
        free(f);
    }
    return 0;
}

/* Convenience accessor: dereference state to get the fixture pointer. */
#define FIX(state) (*(manager_fixture **)(state))

/* --- CreateTargetDevice ------------------------------------------------- */

static void
test_create_target_device_success(void **state)
{
    manager_fixture *f = FIX(state);
    const char *path = "/org/shadowblip/InputPlumber/devices/target/gamepad0";

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                           "CreateTargetDevice", path);

    char *out_path = NULL;
    int rc = ip_manager_create_target_device(f->backend, f->mock.bus,
                                              "xb360", &out_path);
    assert_int_equal(rc, 0);
    assert_non_null(out_path);
    assert_string_equal(out_path, path);
    free(out_path);
}

static void
test_create_target_device_error(void **state)
{
    manager_fixture *f = FIX(state);

    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_MANAGER,
                               "CreateTargetDevice", IP_ERR_INVALID_ARGS);

    char *out_path = NULL;
    int rc = ip_manager_create_target_device(f->backend, f->mock.bus,
                                              "badkind", &out_path);
    assert_int_equal(rc, IP_ERR_INVALID_ARGS);
    assert_null(out_path);
}

static void
test_create_target_device_no_expectation(void **state)
{
    manager_fixture *f = FIX(state);

    char *out_path = NULL;
    int rc = ip_manager_create_target_device(f->backend, f->mock.bus,
                                              "xb360", &out_path);
    assert_int_equal(rc, -ENXIO);
    assert_null(out_path);
}

static void
test_create_target_device_null_args(void **state)
{
    manager_fixture *f = FIX(state);
    char *out_path = NULL;

    assert_int_equal(ip_manager_create_target_device(NULL, f->mock.bus,
                                                       "xb360", &out_path),
                     -EINVAL);
    assert_int_equal(ip_manager_create_target_device(f->backend, f->mock.bus,
                                                       NULL, &out_path),
                     -EINVAL);
    assert_int_equal(ip_manager_create_target_device(f->backend, f->mock.bus,
                                                       "xb360", NULL),
                     -EINVAL);
}

/* --- StopTargetDevice --------------------------------------------------- */

static void
test_stop_target_device_success(void **state)
{
    manager_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                           "StopTargetDevice", NULL);

    int rc = ip_manager_stop_target_device(f->backend, f->mock.bus,
      "/org/shadowblip/InputPlumber/devices/target/gamepad0");
    assert_int_equal(rc, 0);
}

static void
test_stop_target_device_error(void **state)
{
    manager_fixture *f = FIX(state);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_MANAGER,
                               "StopTargetDevice", IP_ERR_NO_REPLY);

    int rc = ip_manager_stop_target_device(f->backend, f->mock.bus,
      "/org/shadowblip/InputPlumber/devices/target/gamepad0");
    assert_int_equal(rc, IP_ERR_NO_REPLY);
}

static void
test_stop_target_device_no_expectation(void **state)
{
    manager_fixture *f = FIX(state);
    int rc = ip_manager_stop_target_device(f->backend, f->mock.bus,
      "/org/shadowblip/InputPlumber/devices/target/gamepad0");
    assert_int_equal(rc, -ENXIO);
}

static void
test_stop_target_device_null_args(void **state)
{
    manager_fixture *f = FIX(state);
    assert_int_equal(ip_manager_stop_target_device(NULL, f->mock.bus,
      "/path"), -EINVAL);
    assert_int_equal(ip_manager_stop_target_device(f->backend, f->mock.bus,
      NULL), -EINVAL);
}

/* --- AttachTargetDevice ------------------------------------------------- */

static void
test_attach_target_device_success(void **state)
{
    manager_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                           "AttachTargetDevice", NULL);

    int rc = ip_manager_attach_target_device(f->backend, f->mock.bus,
      "/org/shadowblip/InputPlumber/devices/target/gamepad0",
      "/org/shadowblip/InputPlumber/CompositeDevice0");
    assert_int_equal(rc, 0);
}

static void
test_attach_target_device_error(void **state)
{
    manager_fixture *f = FIX(state);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_MANAGER,
                               "AttachTargetDevice", IP_ERR_ACCESS_DENIED);

    int rc = ip_manager_attach_target_device(f->backend, f->mock.bus,
      "/org/shadowblip/InputPlumber/devices/target/gamepad0",
      "/org/shadowblip/InputPlumber/CompositeDevice0");
    assert_int_equal(rc, IP_ERR_ACCESS_DENIED);
}

static void
test_attach_target_device_no_expectation(void **state)
{
    manager_fixture *f = FIX(state);
    int rc = ip_manager_attach_target_device(f->backend, f->mock.bus,
      "/target", "/composite");
    assert_int_equal(rc, -ENXIO);
}

static void
test_attach_target_device_null_args(void **state)
{
    manager_fixture *f = FIX(state);
    assert_int_equal(ip_manager_attach_target_device(
      NULL, f->mock.bus, "/t", "/c"), -EINVAL);
    assert_int_equal(ip_manager_attach_target_device(
      f->backend, f->mock.bus, NULL, "/c"), -EINVAL);
    assert_int_equal(ip_manager_attach_target_device(
      f->backend, f->mock.bus, "/t", NULL), -EINVAL);
}

/* --- SetTargetDevices --------------------------------------------------- */

static void
test_set_target_devices_success(void **state)
{
    manager_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "SetTargetDevices", NULL);

    int rc = ip_manager_set_target_devices(f->backend, f->mock.bus,
      "/org/shadowblip/InputPlumber/CompositeDevice0",
      "xb360,ds5");
    assert_int_equal(rc, 0);
}

static void
test_set_target_devices_error(void **state)
{
    manager_fixture *f = FIX(state);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                               "SetTargetDevices", IP_ERR_INVALID_ARGS);

    int rc = ip_manager_set_target_devices(f->backend, f->mock.bus,
      "/org/shadowblip/InputPlumber/CompositeDevice0",
      "badtype");
    assert_int_equal(rc, IP_ERR_INVALID_ARGS);
}

static void
test_set_target_devices_no_expectation(void **state)
{
    manager_fixture *f = FIX(state);
    int rc = ip_manager_set_target_devices(f->backend, f->mock.bus,
      "/org/shadowblip/InputPlumber/CompositeDevice0",
      "xb360");
    assert_int_equal(rc, -ENXIO);
}

static void
test_set_target_devices_null_args(void **state)
{
    manager_fixture *f = FIX(state);
    assert_int_equal(ip_manager_set_target_devices(
      NULL, f->mock.bus, "/c", "xb360"), -EINVAL);
    assert_int_equal(ip_manager_set_target_devices(
      f->backend, f->mock.bus, NULL, "xb360"), -EINVAL);
    assert_int_equal(ip_manager_set_target_devices(
      f->backend, f->mock.bus, "/c", NULL), -EINVAL);
}

/* --- GetGamepadOrder ----------------------------------------------------- */

static void
test_get_gamepad_order_success(void **state)
{
    manager_fixture *f = FIX(state);
    const char *val = "/org/shadowblip/InputPlumber/CompositeDevice0,"
                     "/org/shadowblip/InputPlumber/CompositeDevice1";
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                           "GamepadOrder", val);

    char *out = NULL;
    int rc = ip_manager_get_gamepad_order(f->backend, f->mock.bus, &out);
    assert_int_equal(rc, 0);
    assert_non_null(out);
    assert_string_equal(out, val);
    free(out);
}

static void
test_get_gamepad_order_empty(void **state)
{
    manager_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                           "GamepadOrder", "");

    char *out = NULL;
    int rc = ip_manager_get_gamepad_order(f->backend, f->mock.bus, &out);
    assert_int_equal(rc, 0);
    assert_non_null(out);
    assert_string_equal(out, "");
    free(out);
}

static void
test_get_gamepad_order_error(void **state)
{
    manager_fixture *f = FIX(state);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_MANAGER,
                               "GamepadOrder", IP_ERR_NO_REPLY);

    char *out = NULL;
    int rc = ip_manager_get_gamepad_order(f->backend, f->mock.bus, &out);
    assert_int_equal(rc, IP_ERR_NO_REPLY);
    assert_null(out);
}

static void
test_get_gamepad_order_no_expectation(void **state)
{
    manager_fixture *f = FIX(state);
    char *out = NULL;
    int rc = ip_manager_get_gamepad_order(f->backend, f->mock.bus, &out);
    assert_int_equal(rc, -ENXIO);
    assert_null(out);
}

static void
test_get_gamepad_order_null_args(void **state)
{
    manager_fixture *f = FIX(state);
    char *out = NULL;
    assert_int_equal(ip_manager_get_gamepad_order(
      NULL, f->mock.bus, &out), -EINVAL);
    assert_int_equal(ip_manager_get_gamepad_order(
      f->backend, f->mock.bus, NULL), -EINVAL);
}

/* --- SetGamepadOrder (with validation) ----------------------------------- */

static void
test_set_gamepad_order_success(void **state)
{
    manager_fixture *f = FIX(state);

    /* Populate the device model with two composites. */
    cbx_device_model_add_composite(&f->model,
      "/org/shadowblip/InputPlumber/CompositeDevice0");
    cbx_device_model_add_composite(&f->model,
      "/org/shadowblip/InputPlumber/CompositeDevice1");

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                           "GamepadOrder", NULL);

    int rc = ip_manager_set_gamepad_order(f->backend, f->mock.bus,
      "/org/shadowblip/InputPlumber/CompositeDevice0,"
      "/org/shadowblip/InputPlumber/CompositeDevice1",
      &f->model);
    assert_int_equal(rc, 0);
}

static void
test_set_gamepad_order_empty(void **state)
{
    manager_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                           "GamepadOrder", NULL);

    /* Empty value clears the order — always valid. */
    int rc = ip_manager_set_gamepad_order(f->backend, f->mock.bus,
                                           "", &f->model);
    assert_int_equal(rc, 0);
}

static void
test_set_gamepad_order_invalid_path(void **state)
{
    manager_fixture *f = FIX(state);

    /* Model has one composite, but the order references a different one. */
    cbx_device_model_add_composite(&f->model,
      "/org/shadowblip/InputPlumber/CompositeDevice0");

    /* Should NOT register an expectation — the validation should reject
     * before the DBus call is made. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                           "GamepadOrder", NULL);

    int rc = ip_manager_set_gamepad_order(f->backend, f->mock.bus,
      "/org/shadowblip/InputPlumber/CompositeDevice99",
      &f->model);
    assert_int_equal(rc, -EINVAL);
}

static void
test_set_gamepad_order_partial_invalid(void **state)
{
    manager_fixture *f = FIX(state);

    cbx_device_model_add_composite(&f->model,
      "/org/shadowblip/InputPlumber/CompositeDevice0");
    cbx_device_model_add_composite(&f->model,
      "/org/shadowblip/InputPlumber/CompositeDevice1");

    /* First path valid, second invalid. */
    int rc = ip_manager_set_gamepad_order(f->backend, f->mock.bus,
      "/org/shadowblip/InputPlumber/CompositeDevice0,"
      "/org/shadowblip/InputPlumber/CompositeDevice99",
      &f->model);
    assert_int_equal(rc, -EINVAL);
}

static void
test_set_gamepad_order_null_model(void **state)
{
    manager_fixture *f = FIX(state);

    int rc = ip_manager_set_gamepad_order(f->backend, f->mock.bus,
      "/org/shadowblip/InputPlumber/CompositeDevice0", NULL);
    assert_int_equal(rc, -EINVAL);
}

static void
test_set_gamepad_order_null_args(void **state)
{
    manager_fixture *f = FIX(state);
    assert_int_equal(ip_manager_set_gamepad_order(
      NULL, f->mock.bus, "val", &f->model), -EINVAL);
    assert_int_equal(ip_manager_set_gamepad_order(
      f->backend, f->mock.bus, NULL, &f->model), -EINVAL);
}

static void
test_set_gamepad_order_path_too_long(void **state)
{
    manager_fixture *f = FIX(state);

    /* Construct a path longer than CBX_MAX_PATH_LEN. */
    char long_path[CBX_MAX_PATH_LEN + 32];
    memset(long_path, 'A', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';

    int rc = ip_manager_set_gamepad_order(f->backend, f->mock.bus,
                                           long_path, &f->model);
    assert_int_equal(rc, -EINVAL);
}

static void
test_set_gamepad_order_dbus_error(void **state)
{
    manager_fixture *f = FIX(state);

    cbx_device_model_add_composite(&f->model,
      "/org/shadowblip/InputPlumber/CompositeDevice0");

    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_MANAGER,
                               "GamepadOrder", IP_ERR_NO_REPLY);

    int rc = ip_manager_set_gamepad_order(f->backend, f->mock.bus,
      "/org/shadowblip/InputPlumber/CompositeDevice0",
      &f->model);
    assert_int_equal(rc, IP_ERR_NO_REPLY);
}

/* --- GetSupportedTargetDeviceIds ---------------------------------------- */

static void
test_get_supported_ids_success(void **state)
{
    manager_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                           "SupportedTargetDeviceIds", "xb360,ds5,deck");

    char *out = NULL;
    int rc = ip_manager_get_supported_target_device_ids(f->backend,
                                                          f->mock.bus, &out);
    assert_int_equal(rc, 0);
    assert_non_null(out);
    assert_string_equal(out, "xb360,ds5,deck");
    free(out);
}

static void
test_get_supported_ids_error(void **state)
{
    manager_fixture *f = FIX(state);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_MANAGER,
                               "SupportedTargetDeviceIds", IP_ERR_NO_REPLY);

    char *out = NULL;
    int rc = ip_manager_get_supported_target_device_ids(f->backend,
                                                          f->mock.bus, &out);
    assert_int_equal(rc, IP_ERR_NO_REPLY);
    assert_null(out);
}

static void
test_get_supported_ids_no_expectation(void **state)
{
    manager_fixture *f = FIX(state);
    char *out = NULL;
    int rc = ip_manager_get_supported_target_device_ids(f->backend,
                                                          f->mock.bus, &out);
    assert_int_equal(rc, -ENXIO);
}

static void
test_get_supported_ids_null_args(void **state)
{
    manager_fixture *f = FIX(state);
    char *out = NULL;
    assert_int_equal(ip_manager_get_supported_target_device_ids(
      NULL, f->mock.bus, &out), -EINVAL);
    assert_int_equal(ip_manager_get_supported_target_device_ids(
      f->backend, f->mock.bus, NULL), -EINVAL);
}

/* --- GetSupportedTargetDevices ----------------------------------------- */

static void
test_get_supported_devices_success(void **state)
{
    manager_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                           "SupportedTargetDevices",
                           "Xbox 360,DualSense,Steam Deck");

    char *out = NULL;
    int rc = ip_manager_get_supported_target_devices(f->backend,
                                                       f->mock.bus, &out);
    assert_int_equal(rc, 0);
    assert_non_null(out);
    assert_string_equal(out, "Xbox 360,DualSense,Steam Deck");
    free(out);
}

static void
test_get_supported_devices_error(void **state)
{
    manager_fixture *f = FIX(state);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_MANAGER,
                               "SupportedTargetDevices", IP_ERR_SERVICE_UNKNOWN);

    char *out = NULL;
    int rc = ip_manager_get_supported_target_devices(f->backend,
                                                       f->mock.bus, &out);
    assert_int_equal(rc, IP_ERR_SERVICE_UNKNOWN);
    assert_null(out);
}

static void
test_get_supported_devices_no_expectation(void **state)
{
    manager_fixture *f = FIX(state);
    char *out = NULL;
    int rc = ip_manager_get_supported_target_devices(f->backend,
                                                       f->mock.bus, &out);
    assert_int_equal(rc, -ENXIO);
}

static void
test_get_supported_devices_null_args(void **state)
{
    manager_fixture *f = FIX(state);
    char *out = NULL;
    assert_int_equal(ip_manager_get_supported_target_devices(
      NULL, f->mock.bus, &out), -EINVAL);
    assert_int_equal(ip_manager_get_supported_target_devices(
      f->backend, f->mock.bus, NULL), -EINVAL);
}

/* --- Simple (no fixture) tests ------------------------------------------ */

static void
test_manager_path_constant(void **state)
{
    (void)state;
    assert_string_equal(IP_DBUS_MANAGER_PATH,
      "/org/shadowblip/InputPlumber/Manager");
}

/* --- Test runner -------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* Simple tests (no fixture). */
        cmocka_unit_test(test_manager_path_constant),

        /* CreateTargetDevice. */
        cmocka_unit_test_setup_teardown(test_create_target_device_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_target_device_error,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_target_device_no_expectation,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_target_device_null_args,
                                          setup, teardown),

        /* StopTargetDevice. */
        cmocka_unit_test_setup_teardown(test_stop_target_device_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_stop_target_device_error,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_stop_target_device_no_expectation,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_stop_target_device_null_args,
                                          setup, teardown),

        /* AttachTargetDevice. */
        cmocka_unit_test_setup_teardown(test_attach_target_device_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_attach_target_device_error,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_attach_target_device_no_expectation,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_attach_target_device_null_args,
                                          setup, teardown),

        /* SetTargetDevices. */
        cmocka_unit_test_setup_teardown(test_set_target_devices_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_set_target_devices_error,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_set_target_devices_no_expectation,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_set_target_devices_null_args,
                                          setup, teardown),

        /* GetGamepadOrder. */
        cmocka_unit_test_setup_teardown(test_get_gamepad_order_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_gamepad_order_empty,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_gamepad_order_error,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_gamepad_order_no_expectation,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_gamepad_order_null_args,
                                          setup, teardown),

        /* SetGamepadOrder (with validation). */
        cmocka_unit_test_setup_teardown(test_set_gamepad_order_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_set_gamepad_order_empty,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_set_gamepad_order_invalid_path,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_set_gamepad_order_partial_invalid,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_set_gamepad_order_null_model,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_set_gamepad_order_null_args,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_set_gamepad_order_path_too_long,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_set_gamepad_order_dbus_error,
                                          setup, teardown),

        /* GetSupportedTargetDeviceIds. */
        cmocka_unit_test_setup_teardown(test_get_supported_ids_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_supported_ids_error,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_supported_ids_no_expectation,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_supported_ids_null_args,
                                          setup, teardown),

        /* GetSupportedTargetDevices. */
        cmocka_unit_test_setup_teardown(test_get_supported_devices_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_supported_devices_error,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_supported_devices_no_expectation,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_supported_devices_null_args,
                                          setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}