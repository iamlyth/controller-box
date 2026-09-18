/*
 * test_target_props.c — Unit tests for target device property wrappers (Task 14).
 *
 * Tests the ip_target_* property getters using the mock backend.
 * Each getter is tested for: success, error, no-expectation, and NULL args.
 */
#include "dbus_mock.h"
#include "dbus/ip_target.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

/* --- Fixtures ------------------------------------------------------------ */

#define TARGET_PATH "/org/shadowblip/InputPlumber/devices/target/gamepad0"

typedef struct {
    ip_dbus_mock          mock;
    const ip_dbus_backend *backend;
} target_fixture;

static int
setup(void **state)
{
    target_fixture *f = malloc(sizeof(*f));
    memset(f, 0, sizeof(*f));
    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);
    *state = f;
    return 0;
}

static int
teardown(void **state)
{
    target_fixture *f = *state;
    if (f) {
        ip_dbus_mock_reset(&f->mock);
        free(f);
    }
    return 0;
}

#define FIX(state) (*(target_fixture **)(state))

/* --- Name property tests ------------------------------------------------- */

static void
test_get_name_success(void **state)
{
    target_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_TARGET, "Name", "Virtual Gamepad");

    char *out = NULL;
    int rc = ip_target_get_name(f->backend, f->mock.bus, TARGET_PATH, &out);
    assert_int_equal(rc, 0);
    assert_non_null(out);
    assert_string_equal(out, "Virtual Gamepad");
    free(out);
}

static void
test_get_name_error(void **state)
{
    target_fixture *f = FIX(state);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_TARGET, "Name", -EIO);

    char *out = NULL;
    int rc = ip_target_get_name(f->backend, f->mock.bus, TARGET_PATH, &out);
    assert_int_equal(rc, -EIO);
    assert_null(out);
}

static void
test_get_name_no_expectation(void **state)
{
    target_fixture *f = FIX(state);
    char *out = NULL;
    int rc = ip_target_get_name(f->backend, f->mock.bus, TARGET_PATH, &out);
    assert_int_equal(rc, -ENXIO);
}

static void
test_get_name_null_args(void **state)
{
    target_fixture *f = FIX(state);
    char *out = NULL;

    assert_int_equal(ip_target_get_name(NULL, f->mock.bus, TARGET_PATH, &out), -EINVAL);
    assert_int_equal(ip_target_get_name(f->backend, NULL, TARGET_PATH, &out), -EINVAL);
    assert_int_equal(ip_target_get_name(f->backend, f->mock.bus, NULL, &out), -EINVAL);
    assert_int_equal(ip_target_get_name(f->backend, f->mock.bus, TARGET_PATH, NULL), -EINVAL);
}

/* --- DeviceType property tests ------------------------------------------- */

static void
test_get_device_type_success(void **state)
{
    target_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_TARGET, "DeviceType", "xb360");

    char *out = NULL;
    int rc = ip_target_get_device_type(f->backend, f->mock.bus, TARGET_PATH, &out);
    assert_int_equal(rc, 0);
    assert_non_null(out);
    assert_string_equal(out, "xb360");
    free(out);
}

static void
test_get_device_type_ds5(void **state)
{
    target_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_TARGET, "DeviceType", "ds5");

    char *out = NULL;
    int rc = ip_target_get_device_type(f->backend, f->mock.bus, TARGET_PATH, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out, "ds5");
    free(out);
}

static void
test_get_device_type_deck(void **state)
{
    target_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_TARGET, "DeviceType", "deck");

    char *out = NULL;
    int rc = ip_target_get_device_type(f->backend, f->mock.bus, TARGET_PATH, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out, "deck");
    free(out);
}

static void
test_get_device_type_error(void **state)
{
    target_fixture *f = FIX(state);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_TARGET, "DeviceType", -EIO);

    char *out = NULL;
    int rc = ip_target_get_device_type(f->backend, f->mock.bus, TARGET_PATH, &out);
    assert_int_equal(rc, -EIO);
    assert_null(out);
}

static void
test_get_device_type_no_expectation(void **state)
{
    target_fixture *f = FIX(state);
    char *out = NULL;
    int rc = ip_target_get_device_type(f->backend, f->mock.bus, TARGET_PATH, &out);
    assert_int_equal(rc, -ENXIO);
}

static void
test_get_device_type_null_args(void **state)
{
    target_fixture *f = FIX(state);
    char *out = NULL;

    assert_int_equal(ip_target_get_device_type(NULL, f->mock.bus, TARGET_PATH, &out), -EINVAL);
    assert_int_equal(ip_target_get_device_type(f->backend, NULL, TARGET_PATH, &out), -EINVAL);
    assert_int_equal(ip_target_get_device_type(f->backend, f->mock.bus, NULL, &out), -EINVAL);
    assert_int_equal(ip_target_get_device_type(f->backend, f->mock.bus, TARGET_PATH, NULL), -EINVAL);
}

/* --- Test runner --------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* Name */
        cmocka_unit_test_setup_teardown(test_get_name_success, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_name_error, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_name_no_expectation, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_name_null_args, setup, teardown),

        /* DeviceType */
        cmocka_unit_test_setup_teardown(test_get_device_type_success, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_device_type_ds5, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_device_type_deck, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_device_type_error, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_device_type_no_expectation, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_device_type_null_args, setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}