/*
 * test_sample.c — Task 3 sample cmocka test.
 *
 * A trivial assertion test that verifies cmocka is linked and the
 * assertion macros work.  Also exercises the DBus mock interface
 * abstraction (dbus_mock) to validate the harness infrastructure.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include <errno.h>
#include <stdlib.h>

#include "dbus_mock.h"

/* --- Trivial cmocka assertions --------------------------------------------- */

static void test_assert_macros(void **state) {
    (void)state;
    assert_int_equal(1 + 1, 2);
    assert_int_not_equal(1, 2);
    assert_true(1);
    assert_false(0);
    assert_string_equal("controller-box", "controller-box");
}

/* --- DBus mock infrastructure ---------------------------------------------- */

static void test_mock_expect_and_find(void **state) {
    (void)state;
    ip_dbus_mock mock;
    ip_dbus_mock_init(&mock);

    /* Empty mock: lookup returns NULL. */
    assert_null(ip_dbus_mock_find(&mock, IP_IFACE_MANAGER, "Version"));
    assert_int_equal(
        ip_dbus_mock_expect_ok(&mock, IP_IFACE_MANAGER, "Version", "1.0.0"),
        0);

    const ip_mock_expectation *e =
        ip_dbus_mock_find(&mock, IP_IFACE_MANAGER, "Version");
    assert_non_null(e);
    assert_int_equal(e->rc, 0);
    assert_string_equal(e->value, "1.0.0");

    ip_dbus_mock_free(&mock);
}

static void test_mock_backend_call(void **state) {
    (void)state;
    ip_dbus_mock mock;
    ip_dbus_mock_init(&mock);

    /* Register a canned method call response. */
    assert_int_equal(
        ip_dbus_mock_expect_ok(&mock, IP_IFACE_MANAGER,
                                "GetSupportedTargetDevices", "[gamepad]"),
        0);

    const ip_dbus_backend *be = ip_dbus_mock_backend(&mock);
    assert_non_null(be);
    assert_non_null(be->call_method);

    /* Unregistered call returns -ENXIO. */
    assert_int_equal(be->call_method(mock.bus, IP_DBUS_NAME, IP_DBUS_PATH,
                                     IP_IFACE_MANAGER, "NoSuchMethod", "",
                                     NULL),
                     -ENXIO);

    /* Registered call returns the canned rc (0). */
    assert_int_equal(be->call_method(mock.bus, IP_DBUS_NAME, IP_DBUS_PATH,
                                     IP_IFACE_MANAGER,
                                     "GetSupportedTargetDevices", "",
                                     NULL),
                     0);

    ip_dbus_mock_free(&mock);
}

static void test_mock_backend_get_property(void **state) {
    (void)state;
    ip_dbus_mock mock;
    ip_dbus_mock_init(&mock);

    assert_int_equal(
        ip_dbus_mock_expect_ok(&mock, IP_IFACE_COMPOSITE, "Name", "Deck1"),
        0);

    const ip_dbus_backend *be = ip_dbus_mock_backend(&mock);
    assert_non_null(be->get_property);

    char *value = NULL;
    assert_int_equal(be->get_property(mock.bus, IP_DBUS_NAME,
                                      "/org/shadowblip/InputPlumber/CompositeDevice0",
                                      IP_IFACE_COMPOSITE, "Name", &value),
                     0);
    assert_non_null(value);
    assert_string_equal(value, "Deck1");
    free(value);

    ip_dbus_mock_free(&mock);
}

static void test_mock_backend_connect(void **state) {
    (void)state;
    ip_dbus_mock mock;
    ip_dbus_mock_init(&mock);

    const ip_dbus_backend *be = ip_dbus_mock_backend(&mock);
    assert_non_null(be);
    assert_non_null(be->connect);
    assert_non_null(be->disconnect);
    assert_non_null(be->get_unique_name);
    assert_non_null(be->subscribe_signal);
    assert_non_null(be->inject_signal);

    /* Connect with a valid handle succeeds. */
    assert_int_equal(be->connect(&mock.bus), 0);

    char *unique = NULL;
    assert_int_equal(be->get_unique_name(mock.bus, IP_DBUS_NAME, &unique), 0);
    assert_non_null(unique);
    assert_string_equal(unique, ":1.42");
    free(unique);

    be->disconnect(mock.bus);
    ip_dbus_mock_free(&mock);
}

static void test_mock_error_expectation(void **state) {
    (void)state;
    ip_dbus_mock mock;
    ip_dbus_mock_init(&mock);

    /* Register an error return. */
    assert_int_equal(
        ip_dbus_mock_expect_error(&mock, IP_IFACE_MANAGER,
                                  "CreateTargetDevice", -EACCES),
        0);

    const ip_dbus_backend *be = ip_dbus_mock_backend(&mock);
    assert_int_equal(be->call_method(mock.bus, IP_DBUS_NAME, IP_DBUS_PATH,
                                     IP_IFACE_MANAGER, "CreateTargetDevice",
                                     "", NULL),
                     -EACCES);

    ip_dbus_mock_free(&mock);
}

static const struct CMUnitTest sample_tests[] = {
    cmocka_unit_test(test_assert_macros),
    cmocka_unit_test(test_mock_expect_and_find),
    cmocka_unit_test(test_mock_backend_call),
    cmocka_unit_test(test_mock_backend_get_property),
    cmocka_unit_test(test_mock_backend_connect),
    cmocka_unit_test(test_mock_error_expectation),
};

int main(void) {
    return cmocka_run_group_tests(sample_tests, NULL, NULL);
}