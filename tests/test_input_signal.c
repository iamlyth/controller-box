/*
 * test_input_signal.c — Unit tests for InputEvent signal handling (Task 14).
 *
 * Tests:
 *   - Event string parsing (ip_input_parse) for all known events + unknown
 *   - Category lookup (ip_input_category_of) for buttons and axes
 *   - Value validation (ip_input_validate_value) for buttons and axes
 *   - Handler: init, subscribe, sender verification, unknown events,
 *     invalid values, rate limiting, integration via inject_signal
 *   - Rate limiter: reset, per-device tracking, max 200/sec
 */
#include "dbus_mock.h"
#include "dbus/ip_input_signal.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

/* --- Constants ------------------------------------------------------------ */

#define EXP_SENDER ":1.42"
#define DEVICE_PATH "/org/shadowblip/InputPlumber/devices/dbus0"

/* --- Captured callback state --------------------------------------------- */

typedef struct {
    ip_input_id       input;
    ip_input_category category;
    double            value;
    char              raw_event[64];
    char              device_path[256];
    int               call_count;
} captured_event;

static void
capture_cb(ip_input_id input, ip_input_category category, double value,
            const char *raw_event, const char *device_path, void *userdata)
{
    captured_event *c = (captured_event *)userdata;
    c->input     = input;
    c->category  = category;
    c->value    = value;
    snprintf(c->raw_event, sizeof(c->raw_event), "%s", raw_event ? raw_event : "");
    snprintf(c->device_path, sizeof(c->device_path), "%s", device_path ? device_path : "");
    c->call_count++;
}

/* --- Fixtures ------------------------------------------------------------ */

typedef struct {
    ip_dbus_mock          mock;
    const ip_dbus_backend *backend;
    ip_input_events        ie;
    captured_event         captured;
} input_fixture;

static int
setup(void **state)
{
    input_fixture *f = malloc(sizeof(*f));
    memset(f, 0, sizeof(*f));
    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);
    ip_input_events_init(&f->ie, f->backend, f->mock.bus,
                          EXP_SENDER, capture_cb, &f->captured);
    *state = f;
    return 0;
}

static int
teardown(void **state)
{
    input_fixture *f = *state;
    if (f) {
        ip_dbus_mock_reset(&f->mock);
        free(f);
    }
    return 0;
}

#define FIX(state) (*(input_fixture **)(state))

/* --- Parsing tests (no fixture needed) ----------------------------------- */

static void
test_parse_dpad(void **state)
{
    (void)state;
    assert_int_equal(ip_input_parse("Up"), IP_INPUT_UP);
    assert_int_equal(ip_input_parse("Down"), IP_INPUT_DOWN);
    assert_int_equal(ip_input_parse("Left"), IP_INPUT_LEFT);
    assert_int_equal(ip_input_parse("Right"), IP_INPUT_RIGHT);
}

static void
test_parse_face_buttons(void **state)
{
    (void)state;
    assert_int_equal(ip_input_parse("A"), IP_INPUT_A);
    assert_int_equal(ip_input_parse("B"), IP_INPUT_B);
    assert_int_equal(ip_input_parse("X"), IP_INPUT_X);
    assert_int_equal(ip_input_parse("Y"), IP_INPUT_Y);
}

static void
test_parse_center_buttons(void **state)
{
    (void)state;
    assert_int_equal(ip_input_parse("Start"), IP_INPUT_START);
    assert_int_equal(ip_input_parse("Select"), IP_INPUT_SELECT);
    assert_int_equal(ip_input_parse("Back"), IP_INPUT_SELECT);  /* alias */
    assert_int_equal(ip_input_parse("Guide"), IP_INPUT_GUIDE);
    assert_int_equal(ip_input_parse("Home"), IP_INPUT_GUIDE);   /* alias */
}

static void
test_parse_shoulders(void **state)
{
    (void)state;
    assert_int_equal(ip_input_parse("L1"), IP_INPUT_L1);
    assert_int_equal(ip_input_parse("R1"), IP_INPUT_R1);
    assert_int_equal(ip_input_parse("L2"), IP_INPUT_L2);
    assert_int_equal(ip_input_parse("R2"), IP_INPUT_R2);
    assert_int_equal(ip_input_parse("LeftBumper"), IP_INPUT_L1);
    assert_int_equal(ip_input_parse("RightBumper"), IP_INPUT_R1);
    assert_int_equal(ip_input_parse("LeftTrigger"), IP_INPUT_L2);
    assert_int_equal(ip_input_parse("RightTrigger"), IP_INPUT_R2);
}

static void
test_parse_stick_clicks(void **state)
{
    (void)state;
    assert_int_equal(ip_input_parse("L3"), IP_INPUT_L3);
    assert_int_equal(ip_input_parse("R3"), IP_INPUT_R3);
    assert_int_equal(ip_input_parse("LeftStick"), IP_INPUT_L3);
    assert_int_equal(ip_input_parse("RightStick"), IP_INPUT_R3);
}

static void
test_parse_axes(void **state)
{
    (void)state;
    assert_int_equal(ip_input_parse("LeftStickX"), IP_INPUT_LEFT_STICK_X);
    assert_int_equal(ip_input_parse("LeftStickY"), IP_INPUT_LEFT_STICK_Y);
    assert_int_equal(ip_input_parse("RightStickX"), IP_INPUT_RIGHT_STICK_X);
    assert_int_equal(ip_input_parse("RightStickY"), IP_INPUT_RIGHT_STICK_Y);
}

static void
test_parse_unknown(void **state)
{
    (void)state;
    assert_int_equal(ip_input_parse("FooBar"), IP_INPUT_UNKNOWN);
    assert_int_equal(ip_input_parse(""), IP_INPUT_UNKNOWN);
    assert_int_equal(ip_input_parse(NULL), IP_INPUT_UNKNOWN);
    assert_int_equal(ip_input_parse("a"), IP_INPUT_UNKNOWN);  /* case-sensitive */
    assert_int_equal(ip_input_parse("up"), IP_INPUT_UNKNOWN); /* case-sensitive */
}

/* --- Category tests ----------------------------------------------------- */

static void
test_category_buttons(void **state)
{
    (void)state;
    assert_int_equal(ip_input_category_of(IP_INPUT_UP), IP_INPUT_CAT_BUTTON);
    assert_int_equal(ip_input_category_of(IP_INPUT_A), IP_INPUT_CAT_BUTTON);
    assert_int_equal(ip_input_category_of(IP_INPUT_START), IP_INPUT_CAT_BUTTON);
    assert_int_equal(ip_input_category_of(IP_INPUT_L1), IP_INPUT_CAT_BUTTON);
    assert_int_equal(ip_input_category_of(IP_INPUT_R2), IP_INPUT_CAT_BUTTON);
    assert_int_equal(ip_input_category_of(IP_INPUT_L3), IP_INPUT_CAT_BUTTON);
}

static void
test_category_axes(void **state)
{
    (void)state;
    assert_int_equal(ip_input_category_of(IP_INPUT_LEFT_STICK_X), IP_INPUT_CAT_AXIS);
    assert_int_equal(ip_input_category_of(IP_INPUT_LEFT_STICK_Y), IP_INPUT_CAT_AXIS);
    assert_int_equal(ip_input_category_of(IP_INPUT_RIGHT_STICK_X), IP_INPUT_CAT_AXIS);
    assert_int_equal(ip_input_category_of(IP_INPUT_RIGHT_STICK_Y), IP_INPUT_CAT_AXIS);
}

/* --- Value validation tests --------------------------------------------- */

static void
test_validate_button_values(void **state)
{
    (void)state;
    assert_true(ip_input_validate_value(IP_INPUT_CAT_BUTTON, 0.0));
    assert_true(ip_input_validate_value(IP_INPUT_CAT_BUTTON, 1.0));
    assert_false(ip_input_validate_value(IP_INPUT_CAT_BUTTON, 0.5));
    assert_false(ip_input_validate_value(IP_INPUT_CAT_BUTTON, -1.0));
    assert_false(ip_input_validate_value(IP_INPUT_CAT_BUTTON, 2.0));
    assert_false(ip_input_validate_value(IP_INPUT_CAT_BUTTON, NAN));
    assert_false(ip_input_validate_value(IP_INPUT_CAT_BUTTON, INFINITY));
}

static void
test_validate_axis_values(void **state)
{
    (void)state;
    assert_true(ip_input_validate_value(IP_INPUT_CAT_AXIS, 0.0));
    assert_true(ip_input_validate_value(IP_INPUT_CAT_AXIS, 1.0));
    assert_true(ip_input_validate_value(IP_INPUT_CAT_AXIS, -1.0));
    assert_true(ip_input_validate_value(IP_INPUT_CAT_AXIS, 0.5));
    assert_true(ip_input_validate_value(IP_INPUT_CAT_AXIS, -0.5));
    assert_false(ip_input_validate_value(IP_INPUT_CAT_AXIS, 1.01));
    assert_false(ip_input_validate_value(IP_INPUT_CAT_AXIS, -1.01));
    assert_false(ip_input_validate_value(IP_INPUT_CAT_AXIS, NAN));
    assert_false(ip_input_validate_value(IP_INPUT_CAT_AXIS, INFINITY));
}

/* --- Init tests ---------------------------------------------------------- */

static void
test_init(void **state)
{
    input_fixture *f = FIX(state);
    assert_non_null(f->ie.backend);
    assert_ptr_equal(f->ie.bus, f->mock.bus);
    assert_string_equal(f->ie.expected_sender, EXP_SENDER);
    assert_ptr_equal(f->ie.cb, capture_cb);
    assert_ptr_equal(f->ie.cb_userdata, &f->captured);
}

static void
test_init_null(void **state)
{
    (void)state;
    ip_input_events ie;
    ip_input_events_init(NULL, NULL, NULL, NULL, NULL, NULL);
    /* should not crash */
    ip_input_events_init(&ie, NULL, NULL, NULL, NULL, NULL);
    assert_null(ie.backend);
}

/* --- Subscribe tests ----------------------------------------------------- */

static void
test_subscribe(void **state)
{
    input_fixture *f = FIX(state);
    int rc = ip_input_events_subscribe(&f->ie);
    assert_int_equal(rc, 0);
    assert_int_equal(f->mock.sub_count, 1);
    assert_string_equal(f->mock.subscriptions[0].iface, IP_IFACE_DBUS_DEVICE);
    assert_string_equal(f->mock.subscriptions[0].member, "InputEvent");
}

static void
test_subscribe_fail(void **state)
{
    input_fixture *f = FIX(state);
    f->mock.subscribe_fail_rc = -ENOMEM;
    int rc = ip_input_events_subscribe(&f->ie);
    assert_int_equal(rc, -ENOMEM);
}

static void
test_subscribe_null(void **state)
{
    (void)state;
    assert_int_equal(ip_input_events_subscribe(NULL), -EINVAL);
}

/* --- Handler tests ------------------------------------------------------- */

static void
test_handle_valid_button(void **state)
{
    input_fixture *f = FIX(state);

    ip_input_event_payload p = {
        .sender = EXP_SENDER,
        .path   = DEVICE_PATH,
        .event  = "A",
        .value  = 1.0,
    };
    ip_input_events_handle(&f->ie, &p);

    assert_int_equal(f->captured.call_count, 1);
    assert_int_equal(f->captured.input, IP_INPUT_A);
    assert_int_equal(f->captured.category, IP_INPUT_CAT_BUTTON);
    assert_float_equal(f->captured.value, 1.0, 0.001);
    assert_string_equal(f->captured.raw_event, "A");
    assert_string_equal(f->captured.device_path, DEVICE_PATH);
}

static void
test_handle_valid_axis(void **state)
{
    input_fixture *f = FIX(state);

    ip_input_event_payload p = {
        .sender = EXP_SENDER,
        .path   = DEVICE_PATH,
        .event  = "LeftStickX",
        .value  = 0.5,
    };
    ip_input_events_handle(&f->ie, &p);

    assert_int_equal(f->captured.call_count, 1);
    assert_int_equal(f->captured.input, IP_INPUT_LEFT_STICK_X);
    assert_int_equal(f->captured.category, IP_INPUT_CAT_AXIS);
    assert_float_equal(f->captured.value, 0.5, 0.001);
}

static void
test_handle_button_release(void **state)
{
    input_fixture *f = FIX(state);

    ip_input_event_payload p = {
        .sender = EXP_SENDER,
        .path   = DEVICE_PATH,
        .event  = "A",
        .value  = 0.0,
    };
    ip_input_events_handle(&f->ie, &p);

    assert_int_equal(f->captured.call_count, 1);
    assert_int_equal(f->captured.input, IP_INPUT_A);
    assert_float_equal(f->captured.value, 0.0, 0.001);
}

static void
test_handle_wrong_sender(void **state)
{
    input_fixture *f = FIX(state);

    ip_input_event_payload p = {
        .sender = ":1.99",
        .path   = DEVICE_PATH,
        .event  = "A",
        .value  = 1.0,
    };
    ip_input_events_handle(&f->ie, &p);

    assert_int_equal(f->captured.call_count, 0);
}

static void
test_handle_null_sender(void **state)
{
    input_fixture *f = FIX(state);

    ip_input_event_payload p = {
        .sender = NULL,
        .path   = DEVICE_PATH,
        .event  = "A",
        .value  = 1.0,
    };
    ip_input_events_handle(&f->ie, &p);

    assert_int_equal(f->captured.call_count, 0);
}

static void
test_handle_unknown_event(void **state)
{
    input_fixture *f = FIX(state);

    ip_input_event_payload p = {
        .sender = EXP_SENDER,
        .path   = DEVICE_PATH,
        .event  = "FooBar",
        .value  = 1.0,
    };
    ip_input_events_handle(&f->ie, &p);

    assert_int_equal(f->captured.call_count, 0);
}

static void
test_handle_invalid_button_value(void **state)
{
    input_fixture *f = FIX(state);

    ip_input_event_payload p = {
        .sender = EXP_SENDER,
        .path   = DEVICE_PATH,
        .event  = "A",
        .value  = 0.5,  /* buttons must be 0.0 or 1.0 */
    };
    ip_input_events_handle(&f->ie, &p);

    assert_int_equal(f->captured.call_count, 0);
}

static void
test_handle_invalid_axis_value(void **state)
{
    input_fixture *f = FIX(state);

    ip_input_event_payload p = {
        .sender = EXP_SENDER,
        .path   = DEVICE_PATH,
        .event  = "LeftStickX",
        .value  = 2.0,  /* axes must be in [-1, 1] */
    };
    ip_input_events_handle(&f->ie, &p);

    assert_int_equal(f->captured.call_count, 0);
}

static void
test_handle_null_payload(void **state)
{
    input_fixture *f = FIX(state);
    ip_input_events_handle(&f->ie, NULL);
    assert_int_equal(f->captured.call_count, 0);
}

static void
test_handle_null_handler(void **state)
{
    (void)state;
    ip_input_event_payload p = { .sender = EXP_SENDER };
    ip_input_events_handle(NULL, &p);
    /* should not crash */
}

static void
test_handle_null_callback(void **state)
{
    input_fixture *f = FIX(state);
    f->ie.cb = NULL;

    ip_input_event_payload p = {
        .sender = EXP_SENDER,
        .path   = DEVICE_PATH,
        .event  = "A",
        .value  = 1.0,
    };
    ip_input_events_handle(&f->ie, &p);
    /* should not crash, no callback fired */
}

static void
test_handle_null_path(void **state)
{
    input_fixture *f = FIX(state);

    ip_input_event_payload p = {
        .sender = EXP_SENDER,
        .path   = NULL,
        .event  = "A",
        .value  = 1.0,
    };
    ip_input_events_handle(&f->ie, &p);

    assert_int_equal(f->captured.call_count, 0);
}

static void
test_handle_null_event(void **state)
{
    input_fixture *f = FIX(state);

    ip_input_event_payload p = {
        .sender = EXP_SENDER,
        .path   = DEVICE_PATH,
        .event  = NULL,
        .value  = 1.0,
    };
    ip_input_events_handle(&f->ie, &p);

    assert_int_equal(f->captured.call_count, 0);
}

/* --- Rate limiting tests ------------------------------------------------- */

static void
test_rate_limit_under_limit(void **state)
{
    input_fixture *f = FIX(state);

    /* Send 200 events — all should be accepted. */
    for (int i = 0; i < 200; i++) {
        ip_input_event_payload p = {
            .sender = EXP_SENDER,
            .path   = DEVICE_PATH,
            .event  = "A",
            .value  = 1.0,
        };
        ip_input_events_handle(&f->ie, &p);
    }
    assert_int_equal(f->captured.call_count, 200);
}

static void
test_rate_limit_over_limit(void **state)
{
    input_fixture *f = FIX(state);

    /* Send 201 events — the 201st should be rate-limited. */
    for (int i = 0; i < 201; i++) {
        ip_input_event_payload p = {
            .sender = EXP_SENDER,
            .path   = DEVICE_PATH,
            .event  = "A",
            .value  = 1.0,
        };
        ip_input_events_handle(&f->ie, &p);
    }
    assert_int_equal(f->captured.call_count, 200);
}

static void
test_rate_limit_per_device(void **state)
{
    input_fixture *f = FIX(state);

    /* Send 200 events from device A, then 1 from device B — all accepted. */
    for (int i = 0; i < 200; i++) {
        ip_input_event_payload p = {
            .sender = EXP_SENDER,
            .path   = DEVICE_PATH,
            .event  = "A",
            .value  = 1.0,
        };
        ip_input_events_handle(&f->ie, &p);
    }

    ip_input_event_payload p2 = {
        .sender = EXP_SENDER,
        .path   = "/org/shadowblip/InputPlumber/devices/dbus1",
        .event  = "B",
        .value  = 1.0,
    };
    ip_input_events_handle(&f->ie, &p2);

    assert_int_equal(f->captured.call_count, 201);
    assert_int_equal(f->captured.input, IP_INPUT_B);
}

static void
test_rate_limit_reset(void **state)
{
    input_fixture *f = FIX(state);

    /* Send some events, then reset rate limiters. */
    for (int i = 0; i < 100; i++) {
        ip_input_event_payload p = {
            .sender = EXP_SENDER,
            .path   = DEVICE_PATH,
            .event  = "A",
            .value  = 1.0,
        };
        ip_input_events_handle(&f->ie, &p);
    }
    assert_int_equal(f->captured.call_count, 100);

    ip_input_events_reset_rate_limiters(&f->ie);

    /* After reset, should be able to send more events. */
    for (int i = 0; i < 100; i++) {
        ip_input_event_payload p = {
            .sender = EXP_SENDER,
            .path   = DEVICE_PATH,
            .event  = "A",
            .value  = 1.0,
        };
        ip_input_events_handle(&f->ie, &p);
    }
    assert_int_equal(f->captured.call_count, 200);
}

/* --- Integration tests (via inject_signal) ------------------------------- */

static void
test_inject_signal(void **state)
{
    input_fixture *f = FIX(state);
    ip_input_events_subscribe(&f->ie);

    ip_input_event_payload p = {
        .sender = EXP_SENDER,
        .path   = DEVICE_PATH,
        .event  = "Start",
        .value  = 1.0,
    };
    int rc = f->backend->inject_signal(f->mock.bus,
        IP_IFACE_DBUS_DEVICE, "InputEvent", &p);
    assert_int_equal(rc, 0);
    assert_int_equal(f->captured.call_count, 1);
    assert_int_equal(f->captured.input, IP_INPUT_START);
}

static void
test_inject_wrong_sender(void **state)
{
    input_fixture *f = FIX(state);
    ip_input_events_subscribe(&f->ie);

    ip_input_event_payload p = {
        .sender = ":1.99",
        .path   = DEVICE_PATH,
        .event  = "A",
        .value  = 1.0,
    };
    f->backend->inject_signal(f->mock.bus,
        IP_IFACE_DBUS_DEVICE, "InputEvent", &p);
    assert_int_equal(f->captured.call_count, 0);
}

static void
test_inject_multiple(void **state)
{
    input_fixture *f = FIX(state);
    ip_input_events_subscribe(&f->ie);

    /* Press A, release A, press B */
    ip_input_event_payload p1 = {
        .sender = EXP_SENDER, .path = DEVICE_PATH,
        .event = "A", .value = 1.0,
    };
    ip_input_event_payload p2 = {
        .sender = EXP_SENDER, .path = DEVICE_PATH,
        .event = "A", .value = 0.0,
    };
    ip_input_event_payload p3 = {
        .sender = EXP_SENDER, .path = DEVICE_PATH,
        .event = "B", .value = 1.0,
    };
    f->backend->inject_signal(f->mock.bus, IP_IFACE_DBUS_DEVICE, "InputEvent", &p1);
    f->backend->inject_signal(f->mock.bus, IP_IFACE_DBUS_DEVICE, "InputEvent", &p2);
    f->backend->inject_signal(f->mock.bus, IP_IFACE_DBUS_DEVICE, "InputEvent", &p3);

    assert_int_equal(f->captured.call_count, 3);
    /* Last event should be B press */
    assert_int_equal(f->captured.input, IP_INPUT_B);
    assert_float_equal(f->captured.value, 1.0, 0.001);
}

static void
test_inject_axis(void **state)
{
    input_fixture *f = FIX(state);
    ip_input_events_subscribe(&f->ie);

    ip_input_event_payload p = {
        .sender = EXP_SENDER,
        .path   = DEVICE_PATH,
        .event  = "RightStickY",
        .value  = -0.7,
    };
    f->backend->inject_signal(f->mock.bus,
        IP_IFACE_DBUS_DEVICE, "InputEvent", &p);
    assert_int_equal(f->captured.call_count, 1);
    assert_int_equal(f->captured.input, IP_INPUT_RIGHT_STICK_Y);
    assert_int_equal(f->captured.category, IP_INPUT_CAT_AXIS);
    assert_float_equal(f->captured.value, -0.7, 0.001);
}

/* --- Test runner --------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* Parsing */
        cmocka_unit_test(test_parse_dpad),
        cmocka_unit_test(test_parse_face_buttons),
        cmocka_unit_test(test_parse_center_buttons),
        cmocka_unit_test(test_parse_shoulders),
        cmocka_unit_test(test_parse_stick_clicks),
        cmocka_unit_test(test_parse_axes),
        cmocka_unit_test(test_parse_unknown),

        /* Category */
        cmocka_unit_test(test_category_buttons),
        cmocka_unit_test(test_category_axes),

        /* Value validation */
        cmocka_unit_test(test_validate_button_values),
        cmocka_unit_test(test_validate_axis_values),

        /* Init */
        cmocka_unit_test_setup_teardown(test_init, setup, teardown),
        cmocka_unit_test_setup_teardown(test_init_null, setup, teardown),

        /* Subscribe */
        cmocka_unit_test_setup_teardown(test_subscribe, setup, teardown),
        cmocka_unit_test_setup_teardown(test_subscribe_fail, setup, teardown),
        cmocka_unit_test_setup_teardown(test_subscribe_null, setup, teardown),

        /* Handler */
        cmocka_unit_test_setup_teardown(test_handle_valid_button, setup, teardown),
        cmocka_unit_test_setup_teardown(test_handle_valid_axis, setup, teardown),
        cmocka_unit_test_setup_teardown(test_handle_button_release, setup, teardown),
        cmocka_unit_test_setup_teardown(test_handle_wrong_sender, setup, teardown),
        cmocka_unit_test_setup_teardown(test_handle_null_sender, setup, teardown),
        cmocka_unit_test_setup_teardown(test_handle_unknown_event, setup, teardown),
        cmocka_unit_test_setup_teardown(test_handle_invalid_button_value, setup, teardown),
        cmocka_unit_test_setup_teardown(test_handle_invalid_axis_value, setup, teardown),
        cmocka_unit_test_setup_teardown(test_handle_null_payload, setup, teardown),
        cmocka_unit_test_setup_teardown(test_handle_null_handler, setup, teardown),
        cmocka_unit_test_setup_teardown(test_handle_null_callback, setup, teardown),
        cmocka_unit_test_setup_teardown(test_handle_null_path, setup, teardown),
        cmocka_unit_test_setup_teardown(test_handle_null_event, setup, teardown),

        /* Rate limiting */
        cmocka_unit_test_setup_teardown(test_rate_limit_under_limit, setup, teardown),
        cmocka_unit_test_setup_teardown(test_rate_limit_over_limit, setup, teardown),
        cmocka_unit_test_setup_teardown(test_rate_limit_per_device, setup, teardown),
        cmocka_unit_test_setup_teardown(test_rate_limit_reset, setup, teardown),

        /* Integration */
        cmocka_unit_test_setup_teardown(test_inject_signal, setup, teardown),
        cmocka_unit_test_setup_teardown(test_inject_wrong_sender, setup, teardown),
        cmocka_unit_test_setup_teardown(test_inject_multiple, setup, teardown),
        cmocka_unit_test_setup_teardown(test_inject_axis, setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}