/*
 * test_trigger.c — Unit tests for overlay trigger registration.
 *
 * Task 32 — Overlay trigger registration and activation/close.
 *
 * Tests:
 *   - Parse: simple "Select+A" → "Select,A" + "Select+A"
 *   - Parse: multi-button "Select+Start+A" → "Select,Start,A"
 *   - Parse: single button "A" → "A" (no comma)
 *   - Parse: whitespace trimming "Select + A" → "Select,A"
 *   - Parse: empty string → -EINVAL
 *   - Parse: only whitespace → -EINVAL
 *   - Parse: NULL args → -EINVAL
 *   - Parse: buffer too small → -ENAMETOOLONG
 *   - Register: success (SetInterceptActivation + InterceptMode=PASS)
 *   - Register: SetInterceptActivation fails
 *   - Register: InterceptMode set fails
 *   - Register: NULL args
 *   - Register: bad trigger string
 *   - Register_all: all succeed
 *   - Register_all: some fail (returns -count)
 *   - Register_all: all fail
 *   - Register_all: NULL args
 *   - Register_all: NULL path entry
 */
#include "overlay/trigger.h"
#include "dbus/ip_composite.h"
#include "dbus/ip_connection.h"   /* IP_ERR_NO_REPLY */
#include "dbus_mock.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include <cmocka.h>

/* --- Fixtures --------------------------------------------------------- */

#define COMP_PATH "/org/shadowblip/InputPlumber/CompositeDevice0"

typedef struct {
    ip_dbus_mock    mock;
    const ip_dbus_backend *backend;
} trigger_fixture;

static int
setup(void **state)
{
    trigger_fixture *f = calloc(1, sizeof(*f));
    assert_non_null(f);
    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);
    *state = f;
    return 0;
}

static int
teardown(void **state)
{
    trigger_fixture *f = *state;
    ip_dbus_mock_free(&f->mock);
    free(f);
    return 0;
}

#define FIX(s) ((trigger_fixture *)(*s))

/* --- Parse tests ------------------------------------------------------ */

static void
test_parse_simple(void **state)
{
    (void)state;
    char csv[128], target[128];
    int rc = cbx_trigger_parse("Select+A", csv, sizeof(csv),
                               target, sizeof(target));
    assert_int_equal(rc, 0);
    assert_string_equal(csv, "Select,A");
    assert_string_equal(target, "Select+A");
}

static void
test_parse_multi(void **state)
{
    (void)state;
    char csv[128], target[128];
    int rc = cbx_trigger_parse("Select+Start+A", csv, sizeof(csv),
                               target, sizeof(target));
    assert_int_equal(rc, 0);
    assert_string_equal(csv, "Select,Start,A");
    assert_string_equal(target, "Select+Start+A");
}

static void
test_parse_single(void **state)
{
    (void)state;
    char csv[128], target[128];
    int rc = cbx_trigger_parse("A", csv, sizeof(csv),
                               target, sizeof(target));
    assert_int_equal(rc, 0);
    assert_string_equal(csv, "A");
    assert_string_equal(target, "A");
}

static void
test_parse_whitespace(void **state)
{
    (void)state;
    char csv[128], target[128];
    int rc = cbx_trigger_parse(" Select + A ", csv, sizeof(csv),
                               target, sizeof(target));
    assert_int_equal(rc, 0);
    assert_string_equal(csv, "Select,A");
    assert_string_equal(target, " Select + A ");
}

static void
test_parse_empty(void **state)
{
    (void)state;
    char csv[128], target[128];
    int rc = cbx_trigger_parse("", csv, sizeof(csv),
                               target, sizeof(target));
    assert_int_equal(rc, -EINVAL);
}

static void
test_parse_whitespace_only(void **state)
{
    (void)state;
    char csv[128], target[128];
    int rc = cbx_trigger_parse("   ", csv, sizeof(csv),
                               target, sizeof(target));
    assert_int_equal(rc, -EINVAL);
}

static void
test_parse_null_args(void **state)
{
    (void)state;
    char csv[128], target[128];
    assert_int_equal(cbx_trigger_parse(NULL, csv, sizeof(csv),
                                       target, sizeof(target)), -EINVAL);
    assert_int_equal(cbx_trigger_parse("Select+A", NULL, sizeof(csv),
                                       target, sizeof(target)), -EINVAL);
    assert_int_equal(cbx_trigger_parse("Select+A", csv, sizeof(csv),
                                       NULL, sizeof(target)), -EINVAL);
}

static void
test_parse_buffer_too_small(void **state)
{
    (void)state;
    char csv[4], target[128];
    /* "Select,A" is 8 chars, buffer is 4 → too small */
    int rc = cbx_trigger_parse("Select+A", csv, sizeof(csv),
                               target, sizeof(target));
    assert_int_equal(rc, -ENAMETOOLONG);
}

static void
test_parse_target_too_small(void **state)
{
    (void)state;
    char csv[128], target[4];
    /* "Select+A" is 8 chars, target buffer is 4 → too small */
    int rc = cbx_trigger_parse("Select+A", csv, sizeof(csv),
                               target, sizeof(target));
    assert_int_equal(rc, -ENAMETOOLONG);
}

static void
test_parse_zero_size_buffer(void **state)
{
    (void)state;
    char csv[1], target[1];
    assert_int_equal(cbx_trigger_parse("Select+A", csv, 0,
                                       target, sizeof(target)), -EINVAL);
    assert_int_equal(cbx_trigger_parse("Select+A", csv, sizeof(csv),
                                       target, 0), -EINVAL);
}

/* --- Register tests --------------------------------------------------- */

static void
test_register_success(void **state)
{
    trigger_fixture *f = FIX(state);
    /* Expect SetInterceptActivation call. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "SetInterceptActivation", NULL);
    /* Expect InterceptMode set to "1" (PASS). */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", NULL);

    int rc = cbx_trigger_register(f->backend, f->mock.bus,
                                   COMP_PATH, "Select+A");
    assert_int_equal(rc, 0);
}

static void
test_register_activation_fails(void **state)
{
    trigger_fixture *f = FIX(state);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                               "SetInterceptActivation", IP_ERR_INVALID_ARGS);

    int rc = cbx_trigger_register(f->backend, f->mock.bus,
                                   COMP_PATH, "Select+A");
    assert_int_equal(rc, IP_ERR_INVALID_ARGS);
}

static void
test_register_intercept_mode_fails(void **state)
{
    trigger_fixture *f = FIX(state);
    /* SetInterceptActivation succeeds. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "SetInterceptActivation", NULL);
    /* InterceptMode set fails. */
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                               "InterceptMode", IP_ERR_NO_REPLY);

    int rc = cbx_trigger_register(f->backend, f->mock.bus,
                                   COMP_PATH, "Select+A");
    assert_int_equal(rc, IP_ERR_NO_REPLY);
}

static void
test_register_no_expectation(void **state)
{
    trigger_fixture *f = FIX(state);
    /* No expectations registered → -ENXIO from mock. */
    int rc = cbx_trigger_register(f->backend, f->mock.bus,
                                   COMP_PATH, "Select+A");
    assert_int_equal(rc, -ENXIO);
}

static void
test_register_null_args(void **state)
{
    trigger_fixture *f = FIX(state);
    assert_int_equal(cbx_trigger_register(NULL, f->mock.bus,
                                            COMP_PATH, "Select+A"), -EINVAL);
    assert_int_equal(cbx_trigger_register(f->backend, f->mock.bus,
                                            NULL, "Select+A"), -EINVAL);
    assert_int_equal(cbx_trigger_register(f->backend, f->mock.bus,
                                            COMP_PATH, NULL), -EINVAL);
}

static void
test_register_bad_trigger(void **state)
{
    trigger_fixture *f = FIX(state);
    /* Empty trigger string → parse fails. */
    int rc = cbx_trigger_register(f->backend, f->mock.bus,
                                   COMP_PATH, "");
    assert_int_equal(rc, -EINVAL);
}

/* --- Register_all tests ----------------------------------------------- */

static void
test_register_all_success(void **state)
{
    trigger_fixture *f = FIX(state);
    const char *paths[] = {
        "/org/shadowblip/InputPlumber/CompositeDevice0",
        "/org/shadowblip/InputPlumber/CompositeDevice1",
        "/org/shadowblip/InputPlumber/CompositeDevice2",
    };
    int count = 3;

    /* Each device needs SetInterceptActivation + InterceptMode. */
    for (int i = 0; i < count; i++) {
        ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                               "SetInterceptActivation", NULL);
        ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                               "InterceptMode", NULL);
    }

    int rc = cbx_trigger_register_all(f->backend, f->mock.bus,
                                       paths, count, "Select+A");
    assert_int_equal(rc, 0);
}

static void
test_register_all_some_fail(void **state)
{
    trigger_fixture *f = FIX(state);
    const char *paths[] = {
        "/org/shadowblip/InputPlumber/CompositeDevice0",  /* succeeds */
        NULL,                                               /* fails (NULL) */
        "/org/shadowblip/InputPlumber/CompositeDevice2",  /* succeeds */
    };
    int count = 3;

    /* Mock expectations for the valid paths.  The mock returns the first
     * match for each member, so both valid paths get the same response. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "SetInterceptActivation", NULL);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", NULL);

    int rc = cbx_trigger_register_all(f->backend, f->mock.bus,
                                       paths, count, "Select+A");
    /* 1 failure (the NULL path) → returns -1. */
    assert_int_equal(rc, -1);
}

static void
test_register_all_all_fail(void **state)
{
    trigger_fixture *f = FIX(state);
    const char *paths[] = {
        "/org/shadowblip/InputPlumber/CompositeDevice0",
        "/org/shadowblip/InputPlumber/CompositeDevice1",
    };
    int count = 2;

    /* Both fail at SetInterceptActivation. */
    for (int i = 0; i < count; i++) {
        ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                                   "SetInterceptActivation", IP_ERR_NO_REPLY);
    }

    int rc = cbx_trigger_register_all(f->backend, f->mock.bus,
                                       paths, count, "Select+A");
    /* 2 failures → returns -2. */
    assert_int_equal(rc, -2);
}

static void
test_register_all_null_args(void **state)
{
    trigger_fixture *f = FIX(state);
    const char *paths[] = { COMP_PATH };
    assert_int_equal(cbx_trigger_register_all(NULL, f->mock.bus,
                                                paths, 1, "Select+A"), -EINVAL);
    assert_int_equal(cbx_trigger_register_all(f->backend, f->mock.bus,
                                                NULL, 1, "Select+A"), -EINVAL);
    assert_int_equal(cbx_trigger_register_all(f->backend, f->mock.bus,
                                                paths, 1, NULL), -EINVAL);
    assert_int_equal(cbx_trigger_register_all(f->backend, f->mock.bus,
                                                paths, 0, "Select+A"), -EINVAL);
}

static void
test_register_all_null_path_entry(void **state)
{
    trigger_fixture *f = FIX(state);
    const char *paths[] = { COMP_PATH, NULL };
    /* The NULL path entry counts as a failure, but no DBus call is made. */
    /* For the first (valid) path, we need expectations. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "SetInterceptActivation", NULL);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", NULL);

    int rc = cbx_trigger_register_all(f->backend, f->mock.bus,
                                       paths, 2, "Select+A");
    /* 1 failure (the NULL path) → returns -1. */
    assert_int_equal(rc, -1);
}

static void
test_register_all_single(void **state)
{
    trigger_fixture *f = FIX(state);
    const char *paths[] = { COMP_PATH };

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "SetInterceptActivation", NULL);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", NULL);

    int rc = cbx_trigger_register_all(f->backend, f->mock.bus,
                                       paths, 1, "Select+A");
    assert_int_equal(rc, 0);
}

/* --- Main ------------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* Parse tests (no fixture needed) */
        cmocka_unit_test(test_parse_simple),
        cmocka_unit_test(test_parse_multi),
        cmocka_unit_test(test_parse_single),
        cmocka_unit_test(test_parse_whitespace),
        cmocka_unit_test(test_parse_empty),
        cmocka_unit_test(test_parse_whitespace_only),
        cmocka_unit_test(test_parse_null_args),
        cmocka_unit_test(test_parse_buffer_too_small),
        cmocka_unit_test(test_parse_target_too_small),
        cmocka_unit_test(test_parse_zero_size_buffer),
        /* Register tests (need trigger fixture with mock DBus) */
        cmocka_unit_test_setup_teardown(test_register_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_register_activation_fails,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_register_intercept_mode_fails,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_register_no_expectation,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_register_null_args,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_register_bad_trigger,
                                          setup, teardown),
        /* Register_all tests (need trigger fixture) */
        cmocka_unit_test_setup_teardown(test_register_all_success,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_register_all_some_fail,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_register_all_all_fail,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_register_all_null_args,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_register_all_null_path_entry,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_register_all_single,
                                          setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}