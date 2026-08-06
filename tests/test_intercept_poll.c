/*
 * test_intercept_poll.c — Unit tests for InterceptMode polling state machine
 * (Task 13).
 *
 * Tests the ip_intercept_poll state machine using the mock backend.
 * Verifies:
 *   - init (struct fields set correctly, NULL safe)
 *   - tick in IDLE state (no-op)
 *   - tick in PASS_WAIT: PASS (keep waiting), ALL (activate), GAMEPAD_ONLY
 *     (activate), NONE (timeout)
 *   - tick in ACTIVE: ALL (keep active + timeout), PASS (deactivate), NONE
 *     (deactivate), timeout reset
 *   - error handling: transient errors, max consecutive errors, parse failure
 *   - callback firing: activating, deactivating, error
 *   - state_name helper
 *   - stop resets to IDLE
 *
 * SDL timer (start/stop) is NOT tested here — it requires SDL_Init which
 * is not needed for state machine logic.  The tick function is the core
 * logic and is tested directly.
 */
#include "dbus_mock.h"
#include "dbus/ip_connection.h"
#include "dbus/ip_composite.h"
#include "dbus/ip_intercept_poll.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

/* --- Test constants ------------------------------------------------------ */

#define COMP_PATH "/org/shadowblip/InputPlumber/CompositeDevice0"

/* --- Callback tracking --------------------------------------------------- */

typedef struct {
    int  activating_fired;
    int  deactivating_fired;
    int  error_fired;
    int  error_code;
} poll_callbacks;

static void
on_activating(void *userdata)
{
    poll_callbacks *cb = (poll_callbacks *)userdata;
    if (cb) cb->activating_fired++;
}

static void
on_deactivating(void *userdata)
{
    poll_callbacks *cb = (poll_callbacks *)userdata;
    if (cb) cb->deactivating_fired++;
}

static void
on_error(int code, void *userdata)
{
    poll_callbacks *cb = (poll_callbacks *)userdata;
    if (cb) {
        cb->error_fired++;
        cb->error_code = code;
    }
}

/* --- Test fixture -------------------------------------------------------- */

typedef struct {
    ip_dbus_mock          mock;
    const ip_dbus_backend *backend;
    ip_intercept_poll     poll;
    poll_callbacks        callbacks;
} poll_fixture;

static int
setup(void **state)
{
    poll_fixture *f = malloc(sizeof(*f));
    if (!f)
        return -1;
    memset(f, 0, sizeof(*f));
    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);
    memset(&f->callbacks, 0, sizeof(f->callbacks));
    ip_intercept_poll_init(&f->poll, f->backend, f->mock.bus,
                            COMP_PATH,
                            on_activating, &f->callbacks,
                            on_deactivating, &f->callbacks,
                            on_error, &f->callbacks);
    *state = f;
    return 0;
}

static int
teardown(void **state)
{
    poll_fixture *f = *state;
    if (f) {
        ip_dbus_mock_reset(&f->mock);
        free(f);
    }
    return 0;
}

#define FIX(state) (*(poll_fixture **)(state))

/* --- Init tests ---------------------------------------------------------- */

static void
test_poll_init(void **state)
{
    poll_fixture *f = FIX(state);
    assert_non_null(f->poll.backend);
    assert_int_equal(f->poll.state, IP_POLL_IDLE);
    assert_string_equal(f->poll.composite_path, COMP_PATH);
    assert_int_equal(f->poll.error_count, 0);
    assert_int_equal(f->poll.max_errors, IP_INTERCEPT_POLL_MAX_ERRORS);
    assert_int_equal(f->poll.max_timeout_ticks, IP_INTERCEPT_POLL_TIMEOUT_TICKS);
    assert_int_equal(f->poll.timer_id, 0);
}

static void
test_poll_init_null(void **state)
{
    (void)state;
    ip_intercept_poll_init(NULL, NULL, NULL, NULL,
                            NULL, NULL, NULL, NULL, NULL, NULL);
    /* Should not crash. */
}

/* --- IDLE state tests ---------------------------------------------------- */

static void
test_tick_idle_noop(void **state)
{
    poll_fixture *f = FIX(state);
    /* No expectation needed — tick in IDLE should not call DBus. */
    int rc = ip_intercept_poll_tick(&f->poll);
    assert_int_equal(rc, 0);
    assert_int_equal(f->poll.state, IP_POLL_IDLE);
}

static void
test_tick_null_poll(void **state)
{
    (void)state;
    int rc = ip_intercept_poll_tick(NULL);
    assert_int_equal(rc, -EINVAL);
}

/* --- PASS_WAIT state tests ----------------------------------------------- */

static void
test_tick_pass_wait_mode_pass(void **state)
{
    poll_fixture *f = FIX(state);
    f->poll.state = IP_POLL_PASS_WAIT;

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", "1");

    int rc = ip_intercept_poll_tick(&f->poll);
    assert_int_equal(rc, 0);
    assert_int_equal(f->poll.state, IP_POLL_PASS_WAIT);
    assert_int_equal(f->callbacks.activating_fired, 0);
}

static void
test_tick_pass_wait_mode_all(void **state)
{
    poll_fixture *f = FIX(state);
    f->poll.state = IP_POLL_PASS_WAIT;

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", "2");

    int rc = ip_intercept_poll_tick(&f->poll);
    assert_int_equal(rc, 0);
    assert_int_equal(f->poll.state, IP_POLL_ACTIVE);
    assert_int_equal(f->callbacks.activating_fired, 1);
}

static void
test_tick_pass_wait_mode_gamepad_only(void **state)
{
    poll_fixture *f = FIX(state);
    f->poll.state = IP_POLL_PASS_WAIT;

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", "3");

    int rc = ip_intercept_poll_tick(&f->poll);
    assert_int_equal(rc, 0);
    assert_int_equal(f->poll.state, IP_POLL_ACTIVE);
    assert_int_equal(f->callbacks.activating_fired, 1);
}

static void
test_tick_pass_wait_mode_none_timeout(void **state)
{
    poll_fixture *f = FIX(state);
    f->poll.state = IP_POLL_PASS_WAIT;
    f->poll.max_timeout_ticks = 3;

    /* Each tick returns NONE (0).  After 3 ticks, error should fire. */
    for (int i = 0; i < 3; i++) {
        ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                               "InterceptMode", "0");
        ip_intercept_poll_tick(&f->poll);
        /* Reset expectation for next iteration. */
        if (i < 2) {
            assert_int_equal(f->poll.state, IP_POLL_PASS_WAIT);
        }
    }

    assert_int_equal(f->poll.state, IP_POLL_IDLE);
    assert_int_equal(f->callbacks.error_fired, 1);
}

/* --- ACTIVE state tests -------------------------------------------------- */

static void
test_tick_active_mode_all(void **state)
{
    poll_fixture *f = FIX(state);
    f->poll.state = IP_POLL_ACTIVE;

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", "2");

    int rc = ip_intercept_poll_tick(&f->poll);
    assert_int_equal(rc, 0);
    assert_int_equal(f->poll.state, IP_POLL_ACTIVE);
    assert_int_equal(f->callbacks.deactivating_fired, 0);
}

static void
test_tick_active_mode_pass(void **state)
{
    poll_fixture *f = FIX(state);
    f->poll.state = IP_POLL_ACTIVE;

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", "1");

    int rc = ip_intercept_poll_tick(&f->poll);
    assert_int_equal(rc, 0);
    assert_int_equal(f->poll.state, IP_POLL_IDLE);
    assert_int_equal(f->callbacks.deactivating_fired, 1);
}

static void
test_tick_active_mode_none(void **state)
{
    poll_fixture *f = FIX(state);
    f->poll.state = IP_POLL_ACTIVE;

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", "0");

    int rc = ip_intercept_poll_tick(&f->poll);
    assert_int_equal(rc, 0);
    assert_int_equal(f->poll.state, IP_POLL_IDLE);
    assert_int_equal(f->callbacks.deactivating_fired, 1);
}

static void
test_tick_active_timeout(void **state)
{
    poll_fixture *f = FIX(state);
    f->poll.state = IP_POLL_ACTIVE;
    f->poll.max_timeout_ticks = 3;

    /* Mode stays at ALL for max_timeout_ticks → timeout fires. */
    for (int i = 0; i < 3; i++) {
        ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                               "InterceptMode", "2");
        ip_intercept_poll_tick(&f->poll);
    }

    assert_int_equal(f->poll.state, IP_POLL_IDLE);
    assert_int_equal(f->callbacks.error_fired, 1);
    assert_int_equal(f->callbacks.error_code, -ETIMEDOUT);
}

/* --- Error handling tests ------------------------------------------------ */

static void
test_tick_transient_error(void **state)
{
    poll_fixture *f = FIX(state);
    f->poll.state = IP_POLL_PASS_WAIT;

    /* Single error — should keep polling. */
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                               "InterceptMode", IP_ERR_NO_REPLY);

    int rc = ip_intercept_poll_tick(&f->poll);
    assert_int_equal(rc, 0);
    assert_int_equal(f->poll.state, IP_POLL_PASS_WAIT);
    assert_int_equal(f->poll.error_count, 1);
}

static void
test_tick_max_errors(void **state)
{
    poll_fixture *f = FIX(state);
    f->poll.state = IP_POLL_PASS_WAIT;
    f->poll.max_errors = 3;

    /* 3 consecutive errors → error callback + reset to IDLE. */
    for (int i = 0; i < 3; i++) {
        ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                                   "InterceptMode", IP_ERR_NO_REPLY);
        ip_intercept_poll_tick(&f->poll);
    }

    assert_int_equal(f->poll.state, IP_POLL_IDLE);
    assert_int_equal(f->callbacks.error_fired, 1);
    assert_int_equal(f->callbacks.error_code, IP_ERR_NO_REPLY);
}

static void
test_tick_error_recovery(void **state)
{
    poll_fixture *f = FIX(state);
    f->poll.state = IP_POLL_PASS_WAIT;
    f->poll.max_errors = 3;

    /* 2 errors, then success — error_count should reset. */
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                               "InterceptMode", IP_ERR_NO_REPLY);
    ip_intercept_poll_tick(&f->poll);
    assert_int_equal(f->poll.error_count, 1);

    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                               "InterceptMode", IP_ERR_NO_REPLY);
    ip_intercept_poll_tick(&f->poll);
    assert_int_equal(f->poll.error_count, 2);

    /* Now success with PASS — error_count resets, state stays. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", "1");
    ip_intercept_poll_tick(&f->poll);
    assert_int_equal(f->poll.error_count, 0);
    assert_int_equal(f->poll.state, IP_POLL_PASS_WAIT);
}

static void
test_tick_parse_failure(void **state)
{
    poll_fixture *f = FIX(state);
    f->poll.state = IP_POLL_PASS_WAIT;

    /* Invalid mode string — should be treated as transient error. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", "not_a_number");

    int rc = ip_intercept_poll_tick(&f->poll);
    assert_int_equal(rc, 0);
    assert_int_equal(f->poll.error_count, 1);
}

static void
test_tick_parse_failure_max(void **state)
{
    poll_fixture *f = FIX(state);
    f->poll.state = IP_POLL_PASS_WAIT;
    f->poll.max_errors = 2;

    /* 2 parse failures → error + reset. */
    for (int i = 0; i < 2; i++) {
        ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                               "InterceptMode", "xyz");
        ip_intercept_poll_tick(&f->poll);
    }

    assert_int_equal(f->poll.state, IP_POLL_IDLE);
    assert_int_equal(f->callbacks.error_fired, 1);
}

/* --- Full lifecycle simulation ------------------------------------------- */

static void
test_full_lifecycle(void **state)
{
    poll_fixture *f = FIX(state);

    /* Start in PASS_WAIT. */
    f->poll.state = IP_POLL_PASS_WAIT;

    /* Tick 1: PASS — keep waiting. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", "1");
    ip_intercept_poll_tick(&f->poll);
    assert_int_equal(f->poll.state, IP_POLL_PASS_WAIT);

    /* Tick 2: ALL — activate! */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", "2");
    ip_intercept_poll_tick(&f->poll);
    assert_int_equal(f->poll.state, IP_POLL_ACTIVE);
    assert_int_equal(f->callbacks.activating_fired, 1);

    /* Tick 3: ALL — still active. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", "2");
    ip_intercept_poll_tick(&f->poll);
    assert_int_equal(f->poll.state, IP_POLL_ACTIVE);

    /* Tick 4: PASS — deactivate! */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", "1");
    ip_intercept_poll_tick(&f->poll);
    assert_int_equal(f->poll.state, IP_POLL_IDLE);
    assert_int_equal(f->callbacks.deactivating_fired, 1);
}

/* --- Stop test ----------------------------------------------------------- */

static void
test_poll_stop(void **state)
{
    poll_fixture *f = FIX(state);
    f->poll.state = IP_POLL_ACTIVE;
    f->poll.timeout_ticks = 5;
    f->poll.error_count = 2;

    ip_intercept_poll_stop(&f->poll);

    assert_int_equal(f->poll.state, IP_POLL_IDLE);
    assert_int_equal(f->poll.timeout_ticks, 0);
    assert_int_equal(f->poll.error_count, 0);
}

/* --- State name helper --------------------------------------------------- */

static void
test_state_name(void **state)
{
    (void)state;
    assert_string_equal(ip_intercept_poll_state_name(IP_POLL_IDLE), "IDLE");
    assert_string_equal(ip_intercept_poll_state_name(IP_POLL_PASS_WAIT),
                         "PASS_WAIT");
    assert_string_equal(ip_intercept_poll_state_name(IP_POLL_ACTIVE), "ACTIVE");
    assert_string_equal(ip_intercept_poll_state_name(99), "UNKNOWN");
}

/* --- No callback tests --------------------------------------------------- */

static void
test_tick_no_callbacks(void **state)
{
    poll_fixture *f = FIX(state);

    /* Re-init with NULL callbacks. */
    ip_intercept_poll_init(&f->poll, f->backend, f->mock.bus,
                            COMP_PATH,
                            NULL, NULL, NULL, NULL, NULL, NULL);
    f->poll.state = IP_POLL_PASS_WAIT;
    f->poll.max_errors = 1;

    /* ALL → should not crash even with NULL callbacks. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", "2");
    int rc = ip_intercept_poll_tick(&f->poll);
    assert_int_equal(rc, 0);
    assert_int_equal(f->poll.state, IP_POLL_ACTIVE);
}

static void
test_tick_no_callbacks_error(void **state)
{
    poll_fixture *f = FIX(state);

    ip_intercept_poll_init(&f->poll, f->backend, f->mock.bus,
                            COMP_PATH,
                            NULL, NULL, NULL, NULL, NULL, NULL);
    f->poll.state = IP_POLL_PASS_WAIT;
    f->poll.max_errors = 1;

    /* Error → should not crash with NULL error callback. */
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                               "InterceptMode", IP_ERR_NO_REPLY);
    ip_intercept_poll_tick(&f->poll);
    assert_int_equal(f->poll.state, IP_POLL_IDLE);
}

/* --- Test runner -------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* Init tests. */
        cmocka_unit_test_setup_teardown(test_poll_init, setup, teardown),
        cmocka_unit_test(test_poll_init_null),

        /* IDLE state tests. */
        cmocka_unit_test_setup_teardown(test_tick_idle_noop, setup, teardown),
        cmocka_unit_test(test_tick_null_poll),

        /* PASS_WAIT state tests. */
        cmocka_unit_test_setup_teardown(test_tick_pass_wait_mode_pass,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_tick_pass_wait_mode_all,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_tick_pass_wait_mode_gamepad_only,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_tick_pass_wait_mode_none_timeout,
                                          setup, teardown),

        /* ACTIVE state tests. */
        cmocka_unit_test_setup_teardown(test_tick_active_mode_all,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_tick_active_mode_pass,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_tick_active_mode_none,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_tick_active_timeout,
                                          setup, teardown),

        /* Error handling tests. */
        cmocka_unit_test_setup_teardown(test_tick_transient_error,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_tick_max_errors,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_tick_error_recovery,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_tick_parse_failure,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_tick_parse_failure_max,
                                          setup, teardown),

        /* Full lifecycle. */
        cmocka_unit_test_setup_teardown(test_full_lifecycle,
                                          setup, teardown),

        /* Stop test. */
        cmocka_unit_test_setup_teardown(test_poll_stop,
                                          setup, teardown),

        /* State name helper. */
        cmocka_unit_test(test_state_name),

        /* No callback tests. */
        cmocka_unit_test_setup_teardown(test_tick_no_callbacks,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_tick_no_callbacks_error,
                                          setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}