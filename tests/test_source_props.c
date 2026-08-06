/*
 * test_source_props.c — Unit tests for source device property wrappers (Task 14).
 *
 * Tests the ip_source_* property getters using the mock backend.
 * Each getter is tested for: success, error, no-expectation, and NULL args.
 */
#include "dbus_mock.h"
#include "dbus/ip_source.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

/* --- Fixtures ------------------------------------------------------------ */

#define SOURCE_PATH "/org/shadowblip/InputPlumber/devices/source/event0"
#define HIDRAW_PATH "/org/shadowblip/InputPlumber/devices/source/hidraw0"
#define EXP_SENDER ":1.42"

typedef struct {
    ip_dbus_mock          mock;
    const ip_dbus_backend *backend;
} source_fixture;

static int
setup(void **state)
{
    source_fixture *f = malloc(sizeof(*f));
    memset(f, 0, sizeof(*f));
    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);
    *state = f;
    return 0;
}

static int
teardown(void **state)
{
    source_fixture *f = *state;
    if (f) {
        ip_dbus_mock_reset(&f->mock);
        free(f);
    }
    return 0;
}

#define FIX(state) (*(source_fixture **)(state))

/* --- Name property tests ------------------------------------------------- */

static void
test_get_name_success(void **state)
{
    source_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_EVENT, "Name", "Xbox Controller");

    char *out = NULL;
    int rc = ip_source_get_name(f->backend, f->mock.bus, SOURCE_PATH,
                                  IP_IFACE_SOURCE_EVENT, &out);
    assert_int_equal(rc, 0);
    assert_non_null(out);
    assert_string_equal(out, "Xbox Controller");
    free(out);
}

static void
test_get_name_hidraw(void **state)
{
    source_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_HIDRAW, "Name", "HID Device");

    char *out = NULL;
    int rc = ip_source_get_name(f->backend, f->mock.bus, HIDRAW_PATH,
                                  IP_IFACE_SOURCE_HIDRAW, &out);
    assert_int_equal(rc, 0);
    assert_non_null(out);
    assert_string_equal(out, "HID Device");
    free(out);
}

static void
test_get_name_error(void **state)
{
    source_fixture *f = FIX(state);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_SOURCE_EVENT, "Name", -EIO);

    char *out = NULL;
    int rc = ip_source_get_name(f->backend, f->mock.bus, SOURCE_PATH,
                                  IP_IFACE_SOURCE_EVENT, &out);
    assert_int_equal(rc, -EIO);
    assert_null(out);
}

static void
test_get_name_no_expectation(void **state)
{
    source_fixture *f = FIX(state);
    char *out = NULL;
    int rc = ip_source_get_name(f->backend, f->mock.bus, SOURCE_PATH,
                                  IP_IFACE_SOURCE_EVENT, &out);
    assert_int_equal(rc, -ENXIO);
    assert_null(out);
}

static void
test_get_name_null_args(void **state)
{
    source_fixture *f = FIX(state);
    char *out = NULL;

    assert_int_equal(ip_source_get_name(NULL, f->mock.bus, SOURCE_PATH,
                                          IP_IFACE_SOURCE_EVENT, &out), -EINVAL);
    assert_int_equal(ip_source_get_name(f->backend, NULL, SOURCE_PATH,
                                          IP_IFACE_SOURCE_EVENT, &out), -EINVAL);
    assert_int_equal(ip_source_get_name(f->backend, f->mock.bus, NULL,
                                          IP_IFACE_SOURCE_EVENT, &out), -EINVAL);
    assert_int_equal(ip_source_get_name(f->backend, f->mock.bus, SOURCE_PATH,
                                          NULL, &out), -EINVAL);
    assert_int_equal(ip_source_get_name(f->backend, f->mock.bus, SOURCE_PATH,
                                          IP_IFACE_SOURCE_EVENT, NULL), -EINVAL);
}

/* --- UniqueId property tests --------------------------------------------- */

static void
test_get_unique_id_success(void **state)
{
    source_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_EVENT, "UniqueId", "mac:aa:bb:cc:dd:ee:ff");

    char *out = NULL;
    int rc = ip_source_get_unique_id(f->backend, f->mock.bus, SOURCE_PATH,
                                        IP_IFACE_SOURCE_EVENT, &out);
    assert_int_equal(rc, 0);
    assert_non_null(out);
    assert_string_equal(out, "mac:aa:bb:cc:dd:ee:ff");
    free(out);
}

static void
test_get_unique_id_error(void **state)
{
    source_fixture *f = FIX(state);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_SOURCE_EVENT, "UniqueId", -EIO);

    char *out = NULL;
    int rc = ip_source_get_unique_id(f->backend, f->mock.bus, SOURCE_PATH,
                                        IP_IFACE_SOURCE_EVENT, &out);
    assert_int_equal(rc, -EIO);
    assert_null(out);
}

static void
test_get_unique_id_no_expectation(void **state)
{
    source_fixture *f = FIX(state);
    char *out = NULL;
    int rc = ip_source_get_unique_id(f->backend, f->mock.bus, SOURCE_PATH,
                                        IP_IFACE_SOURCE_EVENT, &out);
    assert_int_equal(rc, -ENXIO);
}

static void
test_get_unique_id_null_args(void **state)
{
    source_fixture *f = FIX(state);
    char *out = NULL;
    assert_int_equal(ip_source_get_unique_id(NULL, f->mock.bus, SOURCE_PATH,
                                                IP_IFACE_SOURCE_EVENT, &out), -EINVAL);
    assert_int_equal(ip_source_get_unique_id(f->backend, NULL, SOURCE_PATH,
                                                IP_IFACE_SOURCE_EVENT, &out), -EINVAL);
    assert_int_equal(ip_source_get_unique_id(f->backend, f->mock.bus, NULL,
                                                IP_IFACE_SOURCE_EVENT, &out), -EINVAL);
    assert_int_equal(ip_source_get_unique_id(f->backend, f->mock.bus, SOURCE_PATH,
                                                NULL, &out), -EINVAL);
    assert_int_equal(ip_source_get_unique_id(f->backend, f->mock.bus, SOURCE_PATH,
                                                IP_IFACE_SOURCE_EVENT, NULL), -EINVAL);
}

/* --- PhysPath property tests --------------------------------------------- */

static void
test_get_phys_path_success(void **state)
{
    source_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_EVENT, "PhysPath",
                           "usb-0000:00:14.0-1/input0");

    char *out = NULL;
    int rc = ip_source_get_phys_path(f->backend, f->mock.bus, SOURCE_PATH,
                                        IP_IFACE_SOURCE_EVENT, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out, "usb-0000:00:14.0-1/input0");
    free(out);
}

static void
test_get_phys_path_error(void **state)
{
    source_fixture *f = FIX(state);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_SOURCE_EVENT, "PhysPath", -EIO);

    char *out = NULL;
    int rc = ip_source_get_phys_path(f->backend, f->mock.bus, SOURCE_PATH,
                                        IP_IFACE_SOURCE_EVENT, &out);
    assert_int_equal(rc, -EIO);
}

static void
test_get_phys_path_no_expectation(void **state)
{
    source_fixture *f = FIX(state);
    char *out = NULL;
    int rc = ip_source_get_phys_path(f->backend, f->mock.bus, SOURCE_PATH,
                                        IP_IFACE_SOURCE_EVENT, &out);
    assert_int_equal(rc, -ENXIO);
}

static void
test_get_phys_path_null_args(void **state)
{
    source_fixture *f = FIX(state);
    char *out = NULL;
    assert_int_equal(ip_source_get_phys_path(NULL, f->mock.bus, SOURCE_PATH,
                                                IP_IFACE_SOURCE_EVENT, &out), -EINVAL);
    assert_int_equal(ip_source_get_phys_path(f->backend, NULL, SOURCE_PATH,
                                                IP_IFACE_SOURCE_EVENT, &out), -EINVAL);
    assert_int_equal(ip_source_get_phys_path(f->backend, f->mock.bus, NULL,
                                                IP_IFACE_SOURCE_EVENT, &out), -EINVAL);
    assert_int_equal(ip_source_get_phys_path(f->backend, f->mock.bus, SOURCE_PATH,
                                                IP_IFACE_SOURCE_EVENT, NULL), -EINVAL);
}

/* --- IdVendor property tests --------------------------------------------- */

static void
test_get_id_vendor_success(void **state)
{
    source_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_EVENT, "IdVendor", "045e");

    char *out = NULL;
    int rc = ip_source_get_id_vendor(f->backend, f->mock.bus, SOURCE_PATH,
                                        IP_IFACE_SOURCE_EVENT, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out, "045e");
    free(out);
}

static void
test_get_id_vendor_hidraw(void **state)
{
    source_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_HIDRAW, "IdVendor", "054c");

    char *out = NULL;
    int rc = ip_source_get_id_vendor(f->backend, f->mock.bus, HIDRAW_PATH,
                                        IP_IFACE_SOURCE_HIDRAW, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out, "054c");
    free(out);
}

static void
test_get_id_vendor_error(void **state)
{
    source_fixture *f = FIX(state);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_SOURCE_EVENT, "IdVendor", -EIO);

    char *out = NULL;
    int rc = ip_source_get_id_vendor(f->backend, f->mock.bus, SOURCE_PATH,
                                        IP_IFACE_SOURCE_EVENT, &out);
    assert_int_equal(rc, -EIO);
}

static void
test_get_id_vendor_no_expectation(void **state)
{
    source_fixture *f = FIX(state);
    char *out = NULL;
    int rc = ip_source_get_id_vendor(f->backend, f->mock.bus, SOURCE_PATH,
                                        IP_IFACE_SOURCE_EVENT, &out);
    assert_int_equal(rc, -ENXIO);
}

static void
test_get_id_vendor_null_args(void **state)
{
    source_fixture *f = FIX(state);
    char *out = NULL;
    assert_int_equal(ip_source_get_id_vendor(NULL, f->mock.bus, SOURCE_PATH,
                                                IP_IFACE_SOURCE_EVENT, &out), -EINVAL);
    assert_int_equal(ip_source_get_id_vendor(f->backend, f->mock.bus, SOURCE_PATH,
                                                NULL, &out), -EINVAL);
    assert_int_equal(ip_source_get_id_vendor(f->backend, f->mock.bus, SOURCE_PATH,
                                                IP_IFACE_SOURCE_EVENT, NULL), -EINVAL);
}

/* --- IdProduct property tests -------------------------------------------- */

static void
test_get_id_product_success(void **state)
{
    source_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_EVENT, "IdProduct", "028e");

    char *out = NULL;
    int rc = ip_source_get_id_product(f->backend, f->mock.bus, SOURCE_PATH,
                                         IP_IFACE_SOURCE_EVENT, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out, "028e");
    free(out);
}

static void
test_get_id_product_error(void **state)
{
    source_fixture *f = FIX(state);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_SOURCE_EVENT, "IdProduct", -EIO);

    char *out = NULL;
    int rc = ip_source_get_id_product(f->backend, f->mock.bus, SOURCE_PATH,
                                         IP_IFACE_SOURCE_EVENT, &out);
    assert_int_equal(rc, -EIO);
}

static void
test_get_id_product_no_expectation(void **state)
{
    source_fixture *f = FIX(state);
    char *out = NULL;
    int rc = ip_source_get_id_product(f->backend, f->mock.bus, SOURCE_PATH,
                                         IP_IFACE_SOURCE_EVENT, &out);
    assert_int_equal(rc, -ENXIO);
}

static void
test_get_id_product_null_args(void **state)
{
    source_fixture *f = FIX(state);
    char *out = NULL;
    assert_int_equal(ip_source_get_id_product(NULL, f->mock.bus, SOURCE_PATH,
                                                 IP_IFACE_SOURCE_EVENT, &out), -EINVAL);
    assert_int_equal(ip_source_get_id_product(f->backend, f->mock.bus, SOURCE_PATH,
                                                 IP_IFACE_SOURCE_EVENT, NULL), -EINVAL);
}

/* --- IdBustype property tests -------------------------------------------- */

static void
test_get_id_bustype_success(void **state)
{
    source_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_EVENT, "IdBustype", "0x0003");

    char *out = NULL;
    int rc = ip_source_get_id_bustype(f->backend, f->mock.bus, SOURCE_PATH,
                                         IP_IFACE_SOURCE_EVENT, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out, "0x0003");
    free(out);
}

static void
test_get_id_bustype_error(void **state)
{
    source_fixture *f = FIX(state);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_SOURCE_EVENT, "IdBustype", -EIO);

    char *out = NULL;
    int rc = ip_source_get_id_bustype(f->backend, f->mock.bus, SOURCE_PATH,
                                         IP_IFACE_SOURCE_EVENT, &out);
    assert_int_equal(rc, -EIO);
}

static void
test_get_id_bustype_no_expectation(void **state)
{
    source_fixture *f = FIX(state);
    char *out = NULL;
    int rc = ip_source_get_id_bustype(f->backend, f->mock.bus, SOURCE_PATH,
                                         IP_IFACE_SOURCE_EVENT, &out);
    assert_int_equal(rc, -ENXIO);
}

static void
test_get_id_bustype_null_args(void **state)
{
    source_fixture *f = FIX(state);
    char *out = NULL;
    assert_int_equal(ip_source_get_id_bustype(NULL, f->mock.bus, SOURCE_PATH,
                                                 IP_IFACE_SOURCE_EVENT, &out), -EINVAL);
    assert_int_equal(ip_source_get_id_bustype(f->backend, f->mock.bus, SOURCE_PATH,
                                                 IP_IFACE_SOURCE_EVENT, NULL), -EINVAL);
}

/* --- SerialNumber property tests ----------------------------------------- */

static void
test_get_serial_number_success(void **state)
{
    source_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_HIDRAW, "SerialNumber", "SN12345");

    char *out = NULL;
    int rc = ip_source_get_serial_number(f->backend, f->mock.bus, HIDRAW_PATH,
                                            IP_IFACE_SOURCE_HIDRAW, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out, "SN12345");
    free(out);
}

static void
test_get_serial_number_error(void **state)
{
    source_fixture *f = FIX(state);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_SOURCE_HIDRAW, "SerialNumber", -EIO);

    char *out = NULL;
    int rc = ip_source_get_serial_number(f->backend, f->mock.bus, HIDRAW_PATH,
                                            IP_IFACE_SOURCE_HIDRAW, &out);
    assert_int_equal(rc, -EIO);
}

static void
test_get_serial_number_no_expectation(void **state)
{
    source_fixture *f = FIX(state);
    char *out = NULL;
    int rc = ip_source_get_serial_number(f->backend, f->mock.bus, HIDRAW_PATH,
                                            IP_IFACE_SOURCE_HIDRAW, &out);
    assert_int_equal(rc, -ENXIO);
}

static void
test_get_serial_number_null_args(void **state)
{
    source_fixture *f = FIX(state);
    char *out = NULL;
    assert_int_equal(ip_source_get_serial_number(NULL, f->mock.bus, HIDRAW_PATH,
                                                    IP_IFACE_SOURCE_HIDRAW, &out), -EINVAL);
    assert_int_equal(ip_source_get_serial_number(f->backend, NULL, HIDRAW_PATH,
                                                    IP_IFACE_SOURCE_HIDRAW, &out), -EINVAL);
    assert_int_equal(ip_source_get_serial_number(f->backend, f->mock.bus, NULL,
                                                    IP_IFACE_SOURCE_HIDRAW, &out), -EINVAL);
    assert_int_equal(ip_source_get_serial_number(f->backend, f->mock.bus, HIDRAW_PATH,
                                                    NULL, &out), -EINVAL);
    assert_int_equal(ip_source_get_serial_number(f->backend, f->mock.bus, HIDRAW_PATH,
                                                    IP_IFACE_SOURCE_HIDRAW, NULL), -EINVAL);
}

/* --- Test runner --------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* Name */
        cmocka_unit_test_setup_teardown(test_get_name_success, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_name_hidraw, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_name_error, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_name_no_expectation, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_name_null_args, setup, teardown),

        /* UniqueId */
        cmocka_unit_test_setup_teardown(test_get_unique_id_success, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_unique_id_error, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_unique_id_no_expectation, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_unique_id_null_args, setup, teardown),

        /* PhysPath */
        cmocka_unit_test_setup_teardown(test_get_phys_path_success, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_phys_path_error, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_phys_path_no_expectation, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_phys_path_null_args, setup, teardown),

        /* IdVendor */
        cmocka_unit_test_setup_teardown(test_get_id_vendor_success, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_id_vendor_hidraw, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_id_vendor_error, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_id_vendor_no_expectation, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_id_vendor_null_args, setup, teardown),

        /* IdProduct */
        cmocka_unit_test_setup_teardown(test_get_id_product_success, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_id_product_error, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_id_product_no_expectation, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_id_product_null_args, setup, teardown),

        /* IdBustype */
        cmocka_unit_test_setup_teardown(test_get_id_bustype_success, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_id_bustype_error, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_id_bustype_no_expectation, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_id_bustype_null_args, setup, teardown),

        /* SerialNumber */
        cmocka_unit_test_setup_teardown(test_get_serial_number_success, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_serial_number_error, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_serial_number_no_expectation, setup, teardown),
        cmocka_unit_test_setup_teardown(test_get_serial_number_null_args, setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}