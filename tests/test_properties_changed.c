/*
 * test_properties_changed.c — PropertiesChanged signal handling tests (Task 11).
 *
 * Tests type validation, string/array length limits, invalidated properties,
 * sender verification, and mock signal injection.
 */
#include "dbus/ip_properties.h"
#include "dbus_mock.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Fixtures ------------------------------------------------------------- */

#define EXP_SENDER ":1.42"

/* Captured callback data. */
typedef struct {
    char          prop_name[64];
    ip_prop_type  type;
    char          value[8192];
    int           count;
    int           call_count;
} captured_change;

static void
capture_cb(const char *prop_name, ip_prop_type type,
           const char *value, int count, void *userdata)
{
    captured_change *c = (captured_change *)userdata;
    if (!c || !prop_name)
        return;
    snprintf(c->prop_name, sizeof(c->prop_name), "%s", prop_name);
    c->type = type;
    snprintf(c->value, sizeof(c->value), "%s", value ? value : "");
    c->count = count;
    c->call_count++;
}

typedef struct {
    ip_dbus_mock       mock;
    const ip_dbus_backend *backend;
    ip_properties      props;
    captured_change    captured;
} props_fixture;

static int
setup_props(void **state)
{
    props_fixture *f = malloc(sizeof(*f));
    assert_non_null(f);

    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);
    memset(&f->captured, 0, sizeof(f->captured));
    ip_properties_init(&f->props, f->backend, f->mock.bus, EXP_SENDER,
                       capture_cb, &f->captured);

    *state = f;
    return 0;
}

static int
teardown_props(void **state)
{
    props_fixture *f = *state;
    ip_dbus_mock_reset(&f->mock);
    free(f);
    return 0;
}

/* --- Simple tests --------------------------------------------------------- */

/* Init zeroes the struct. */
static void
test_props_init(void **state)
{
    (void)state;
    ip_properties props;
    ip_dbus_mock mock;
    ip_dbus_mock_init(&mock);
    const ip_dbus_backend *backend = ip_dbus_mock_backend(&mock);

    ip_properties_init(&props, backend, mock.bus, ":1.99",
                        capture_cb, NULL);
    assert_ptr_equal(props.backend, backend);
    assert_string_equal(props.expected_sender, ":1.99");
    assert_ptr_equal(props.cb, capture_cb);
}

/* Init with NULL is safe. */
static void
test_props_init_null(void **state)
{
    (void)state;
    ip_properties props;
    ip_properties_init(NULL, NULL, NULL, NULL, NULL, NULL);
    ip_properties_init(&props, NULL, NULL, NULL, NULL, NULL);
    assert_null(props.backend);
}

/* Subscribe registers PropertiesChanged. */
static void
test_props_subscribe(void **state)
{
    props_fixture *f = *state;
    int rc = ip_properties_subscribe(&f->props);
    assert_int_equal(rc, 0);
    assert_int_equal(f->mock.sub_count, 1);
    assert_string_equal(f->mock.subscriptions[0].iface,
                        IP_IFACE_PROPERTIES);
    assert_string_equal(f->mock.subscriptions[0].member,
                        "PropertiesChanged");
}

/* Subscribe with NULL fails. */
static void
test_props_subscribe_null(void **state)
{
    (void)state;
    ip_properties props;
    memset(&props, 0, sizeof(props));
    int rc = ip_properties_subscribe(&props);
    assert_int_equal(rc, -EINVAL);
}

/* --- Handle Changed: string properties ----------------------------------- */

/* ProfileName (string) is dispatched. */
static void
test_handle_profilename(void **state)
{
    props_fixture *f = *state;

    ip_properties_changed_payload p = {
        .sender      = EXP_SENDER,
        .iface_name  = IP_IFACE_COMPOSITE,
        .prop_name   = "ProfileName",
        .prop_type   = IP_PROP_TYPE_STRING,
        .value       = "Default Profile",
        .array_count = 0,
    };
    ip_properties_handle_changed(&f->props, &p);

    assert_int_equal(f->captured.call_count, 1);
    assert_string_equal(f->captured.prop_name, "ProfileName");
    assert_int_equal(f->captured.type, IP_PROP_TYPE_STRING);
    assert_string_equal(f->captured.value, "Default Profile");
    assert_int_equal(f->captured.count, 0);
}

/* ProfilePath (string) is dispatched. */
static void
test_handle_profilepath(void **state)
{
    props_fixture *f = *state;

    ip_properties_changed_payload p = {
        .sender      = EXP_SENDER,
        .iface_name  = IP_IFACE_COMPOSITE,
        .prop_name   = "ProfilePath",
        .prop_type   = IP_PROP_TYPE_STRING,
        .value       = "/usr/share/inputplumber/profiles/default.yaml",
        .array_count = 0,
    };
    ip_properties_handle_changed(&f->props, &p);

    assert_int_equal(f->captured.call_count, 1);
    assert_string_equal(f->captured.prop_name, "ProfilePath");
    assert_string_equal(f->captured.value,
                         "/usr/share/inputplumber/profiles/default.yaml");
}

/* ProfileName exceeding 256-byte limit is rejected. */
static void
test_handle_profilename_too_long(void **state)
{
    props_fixture *f = *state;

    char long_name[512];
    memset(long_name, 'A', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';

    ip_properties_changed_payload p = {
        .sender      = EXP_SENDER,
        .iface_name  = IP_IFACE_COMPOSITE,
        .prop_name   = "ProfileName",
        .prop_type   = IP_PROP_TYPE_STRING,
        .value       = long_name,
        .array_count = 0,
    };
    ip_properties_handle_changed(&f->props, &p);

    assert_int_equal(f->captured.call_count, 0);
}

/* ProfilePath exceeding 4096-byte limit is rejected. */
static void
test_handle_profilepath_too_long(void **state)
{
    props_fixture *f = *state;

    char long_path[5000];
    memset(long_path, '/', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';

    ip_properties_changed_payload p = {
        .sender      = EXP_SENDER,
        .iface_name  = IP_IFACE_COMPOSITE,
        .prop_name   = "ProfilePath",
        .prop_type   = IP_PROP_TYPE_STRING,
        .value       = long_path,
        .array_count = 0,
    };
    ip_properties_handle_changed(&f->props, &p);

    assert_int_equal(f->captured.call_count, 0);
}

/* --- Handle Changed: array properties ------------------------------------ */

/* GamepadOrder (array) is dispatched. */
static void
test_handle_gamepadorder(void **state)
{
    props_fixture *f = *state;

    ip_properties_changed_payload p = {
        .sender      = EXP_SENDER,
        .iface_name  = IP_IFACE_MANAGER,
        .prop_name   = "GamepadOrder",
        .prop_type   = IP_PROP_TYPE_ARRAY,
        .value       = "gamepad0,gamepad1,gamepad2",
        .array_count = 3,
    };
    ip_properties_handle_changed(&f->props, &p);

    assert_int_equal(f->captured.call_count, 1);
    assert_string_equal(f->captured.prop_name, "GamepadOrder");
    assert_int_equal(f->captured.type, IP_PROP_TYPE_ARRAY);
    assert_string_equal(f->captured.value, "gamepad0,gamepad1,gamepad2");
    assert_int_equal(f->captured.count, 3);
}

/* TargetDevices (array) is dispatched. */
static void
test_handle_targetdevices(void **state)
{
    props_fixture *f = *state;

    ip_properties_changed_payload p = {
        .sender      = EXP_SENDER,
        .iface_name  = IP_IFACE_COMPOSITE,
        .prop_name   = "TargetDevices",
        .prop_type   = IP_PROP_TYPE_ARRAY,
        .value       = "gamepad0,keyboard0",
        .array_count = 2,
    };
    ip_properties_handle_changed(&f->props, &p);

    assert_int_equal(f->captured.call_count, 1);
    assert_string_equal(f->captured.prop_name, "TargetDevices");
    assert_int_equal(f->captured.count, 2);
}

/* SourceDevicePaths (array) is dispatched. */
static void
test_handle_sourcedevicepaths(void **state)
{
    props_fixture *f = *state;

    ip_properties_changed_payload p = {
        .sender      = EXP_SENDER,
        .iface_name  = IP_IFACE_COMPOSITE,
        .prop_name   = "SourceDevicePaths",
        .prop_type   = IP_PROP_TYPE_ARRAY,
        .value       = "/dev/input/event0,/dev/input/event1",
        .array_count = 2,
    };
    ip_properties_handle_changed(&f->props, &p);

    assert_int_equal(f->captured.call_count, 1);
    assert_string_equal(f->captured.prop_name, "SourceDevicePaths");
}

/* Single-element array. */
static void
test_handle_array_single(void **state)
{
    props_fixture *f = *state;

    ip_properties_changed_payload p = {
        .sender      = EXP_SENDER,
        .iface_name  = IP_IFACE_MANAGER,
        .prop_name   = "GamepadOrder",
        .prop_type   = IP_PROP_TYPE_ARRAY,
        .value       = "gamepad0",
        .array_count = 1,
    };
    ip_properties_handle_changed(&f->props, &p);

    assert_int_equal(f->captured.call_count, 1);
    assert_int_equal(f->captured.count, 1);
}

/* Empty array. */
static void
test_handle_array_empty(void **state)
{
    props_fixture *f = *state;

    ip_properties_changed_payload p = {
        .sender      = EXP_SENDER,
        .iface_name  = IP_IFACE_MANAGER,
        .prop_name   = "GamepadOrder",
        .prop_type   = IP_PROP_TYPE_ARRAY,
        .value       = "",
        .array_count = 0,
    };
    ip_properties_handle_changed(&f->props, &p);

    /* Empty string → 0 elements, which is valid. */
    assert_int_equal(f->captured.call_count, 1);
    assert_int_equal(f->captured.count, 0);
}

/* Array element exceeding 256-byte limit is rejected (for name-type props). */
static void
test_handle_array_elem_too_long_name(void **state)
{
    props_fixture *f = *state;

    char long_elem[512];
    memset(long_elem, 'X', sizeof(long_elem) - 1);
    long_elem[sizeof(long_elem) - 1] = '\0';

    ip_properties_changed_payload p = {
        .sender      = EXP_SENDER,
        .iface_name  = IP_IFACE_MANAGER,
        .prop_name   = "GamepadOrder",
        .prop_type   = IP_PROP_TYPE_ARRAY,
        .value       = long_elem,
        .array_count = 1,
    };
    ip_properties_handle_changed(&f->props, &p);

    assert_int_equal(f->captured.call_count, 0);
}

/* Array exceeding 256 elements is rejected. */
static void
test_handle_array_too_many(void **state)
{
    props_fixture *f = *state;

    /* Build a string with 257 comma-separated elements. */
    char buf[8192];
    size_t pos = 0;
    for (int i = 0; i < 257; i++) {
        if (i > 0)
            buf[pos++] = ',';
        pos += snprintf(buf + pos, sizeof(buf) - pos, "gp%d", i);
    }
    buf[pos] = '\0';

    ip_properties_changed_payload p = {
        .sender      = EXP_SENDER,
        .iface_name  = IP_IFACE_MANAGER,
        .prop_name   = "GamepadOrder",
        .prop_type   = IP_PROP_TYPE_ARRAY,
        .value       = buf,
        .array_count = 257,
    };
    ip_properties_handle_changed(&f->props, &p);

    assert_int_equal(f->captured.call_count, 0);
}

/* Array exactly 256 elements is accepted. */
static void
test_handle_array_max_elems(void **state)
{
    props_fixture *f = *state;

    char buf[8192];
    size_t pos = 0;
    for (int i = 0; i < 256; i++) {
        if (i > 0)
            buf[pos++] = ',';
        pos += snprintf(buf + pos, sizeof(buf) - pos, "g%d", i);
    }
    buf[pos] = '\0';

    ip_properties_changed_payload p = {
        .sender      = EXP_SENDER,
        .iface_name  = IP_IFACE_MANAGER,
        .prop_name   = "GamepadOrder",
        .prop_type   = IP_PROP_TYPE_ARRAY,
        .value       = buf,
        .array_count = 256,
    };
    ip_properties_handle_changed(&f->props, &p);

    assert_int_equal(f->captured.call_count, 1);
}

/* SourceDevicePaths allows 4096-byte elements (path-type). */
static void
test_handle_array_path_long_elem(void **state)
{
    props_fixture *f = *state;

    /* 3000-char path element — should be accepted (max 4096). */
    char long_path[3100];
    memset(long_path, '/', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';

    ip_properties_changed_payload p = {
        .sender      = EXP_SENDER,
        .iface_name  = IP_IFACE_COMPOSITE,
        .prop_name   = "SourceDevicePaths",
        .prop_type   = IP_PROP_TYPE_ARRAY,
        .value       = long_path,
        .array_count = 1,
    };
    ip_properties_handle_changed(&f->props, &p);

    assert_int_equal(f->captured.call_count, 1);
}

/* SourceDevicePaths rejects 4097-byte elements. */
static void
test_handle_array_path_elem_too_long(void **state)
{
    props_fixture *f = *state;

    char long_path[5000];
    memset(long_path, '/', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';

    ip_properties_changed_payload p = {
        .sender      = EXP_SENDER,
        .iface_name  = IP_IFACE_COMPOSITE,
        .prop_name   = "SourceDevicePaths",
        .prop_type   = IP_PROP_TYPE_ARRAY,
        .value       = long_path,
        .array_count = 1,
    };
    ip_properties_handle_changed(&f->props, &p);

    assert_int_equal(f->captured.call_count, 0);
}

/* --- Handle Changed: type validation ------------------------------------- */

/* ProfileName with array type is rejected (expects string). */
static void
test_handle_type_mismatch_string_expected(void **state)
{
    props_fixture *f = *state;

    ip_properties_changed_payload p = {
        .sender      = EXP_SENDER,
        .iface_name  = IP_IFACE_COMPOSITE,
        .prop_name   = "ProfileName",
        .prop_type   = IP_PROP_TYPE_ARRAY,
        .value       = "something",
        .array_count = 1,
    };
    ip_properties_handle_changed(&f->props, &p);

    assert_int_equal(f->captured.call_count, 0);
}

/* GamepadOrder with string type is rejected (expects array). */
static void
test_handle_type_mismatch_array_expected(void **state)
{
    props_fixture *f = *state;

    ip_properties_changed_payload p = {
        .sender      = EXP_SENDER,
        .iface_name  = IP_IFACE_MANAGER,
        .prop_name   = "GamepadOrder",
        .prop_type   = IP_PROP_TYPE_STRING,
        .value       = "gamepad0",
        .array_count = 0,
    };
    ip_properties_handle_changed(&f->props, &p);

    assert_int_equal(f->captured.call_count, 0);
}

/* --- Handle Changed: invalidated ----------------------------------------- */

/* Invalidated property is dispatched with NULL value. */
static void
test_handle_invalidated(void **state)
{
    props_fixture *f = *state;

    ip_properties_changed_payload p = {
        .sender      = EXP_SENDER,
        .iface_name  = IP_IFACE_COMPOSITE,
        .prop_name   = "ProfileName",
        .prop_type   = IP_PROP_TYPE_INVALIDATED,
        .value       = NULL,
        .array_count = 0,
    };
    ip_properties_handle_changed(&f->props, &p);

    assert_int_equal(f->captured.call_count, 1);
    assert_int_equal(f->captured.type, IP_PROP_TYPE_INVALIDATED);
    assert_int_equal(f->captured.count, -1);
    assert_string_equal(f->captured.value, "");
}

/* Invalidated for untracked property is ignored. */
static void
test_handle_invalidated_untracked(void **state)
{
    props_fixture *f = *state;

    ip_properties_changed_payload p = {
        .sender      = EXP_SENDER,
        .iface_name  = IP_IFACE_MANAGER,
        .prop_name   = "InterceptMode",
        .prop_type   = IP_PROP_TYPE_INVALIDATED,
        .value       = NULL,
        .array_count = 0,
    };
    ip_properties_handle_changed(&f->props, &p);

    assert_int_equal(f->captured.call_count, 0);
}

/* --- Handle Changed: sender verification --------------------------------- */

/* Wrong sender is rejected. */
static void
test_handle_wrong_sender(void **state)
{
    props_fixture *f = *state;

    ip_properties_changed_payload p = {
        .sender      = ":1.999",
        .iface_name  = IP_IFACE_COMPOSITE,
        .prop_name   = "ProfileName",
        .prop_type   = IP_PROP_TYPE_STRING,
        .value       = "test",
        .array_count = 0,
    };
    ip_properties_handle_changed(&f->props, &p);

    assert_int_equal(f->captured.call_count, 0);
}

/* NULL sender is rejected. */
static void
test_handle_null_sender(void **state)
{
    props_fixture *f = *state;

    ip_properties_changed_payload p = {
        .sender      = NULL,
        .iface_name  = IP_IFACE_COMPOSITE,
        .prop_name   = "ProfileName",
        .prop_type   = IP_PROP_TYPE_STRING,
        .value       = "test",
        .array_count = 0,
    };
    ip_properties_handle_changed(&f->props, &p);

    assert_int_equal(f->captured.call_count, 0);
}

/* --- Handle Changed: edge cases ------------------------------------------ */

/* Untracked property is ignored. */
static void
test_handle_untracked_prop(void **state)
{
    props_fixture *f = *state;

    ip_properties_changed_payload p = {
        .sender      = EXP_SENDER,
        .iface_name  = IP_IFACE_MANAGER,
        .prop_name   = "InterceptMode",
        .prop_type   = IP_PROP_TYPE_STRING,
        .value       = "1",
        .array_count = 0,
    };
    ip_properties_handle_changed(&f->props, &p);

    assert_int_equal(f->captured.call_count, 0);
}

/* NULL payload is safe. */
static void
test_handle_null_payload(void **state)
{
    props_fixture *f = *state;
    ip_properties_handle_changed(&f->props, NULL);
    assert_int_equal(f->captured.call_count, 0);
}

/* NULL handler is safe. */
static void
test_handle_null_handler(void **state)
{
    (void)state;
    ip_properties_changed_payload p = {
        .sender      = EXP_SENDER,
        .iface_name  = IP_IFACE_MANAGER,
        .prop_name   = "GamepadOrder",
        .prop_type   = IP_PROP_TYPE_ARRAY,
        .value       = "gp0",
        .array_count = 1,
    };
    ip_properties_handle_changed(NULL, &p);
    /* no crash */
}

/* NULL callback is safe (no dispatch). */
static void
test_handle_null_callback(void **state)
{
    (void)state;
    ip_dbus_mock mock;
    ip_dbus_mock_init(&mock);
    const ip_dbus_backend *backend = ip_dbus_mock_backend(&mock);
    ip_properties props;
    ip_properties_init(&props, backend, mock.bus, EXP_SENDER, NULL, NULL);

    ip_properties_changed_payload p = {
        .sender      = EXP_SENDER,
        .iface_name  = IP_IFACE_MANAGER,
        .prop_name   = "GamepadOrder",
        .prop_type   = IP_PROP_TYPE_ARRAY,
        .value       = "gp0",
        .array_count = 1,
    };
    ip_properties_handle_changed(&props, &p);
    /* no crash */
    ip_dbus_mock_reset(&mock);
}

/* String value NULL is rejected (string requires non-NULL value). */
static void
test_handle_string_null_value(void **state)
{
    props_fixture *f = *state;

    ip_properties_changed_payload p = {
        .sender      = EXP_SENDER,
        .iface_name  = IP_IFACE_COMPOSITE,
        .prop_name   = "ProfileName",
        .prop_type   = IP_PROP_TYPE_STRING,
        .value       = NULL,
        .array_count = 0,
    };
    ip_properties_handle_changed(&f->props, &p);

    assert_int_equal(f->captured.call_count, 0);
}

/* Array value NULL is rejected. */
static void
test_handle_array_null_value(void **state)
{
    props_fixture *f = *state;

    ip_properties_changed_payload p = {
        .sender      = EXP_SENDER,
        .iface_name  = IP_IFACE_MANAGER,
        .prop_name   = "GamepadOrder",
        .prop_type   = IP_PROP_TYPE_ARRAY,
        .value       = NULL,
        .array_count = 0,
    };
    ip_properties_handle_changed(&f->props, &p);

    assert_int_equal(f->captured.call_count, 0);
}

/* --- Integration with mock inject_signal --------------------------------- */

/* Inject PropertiesChanged via mock. */
static void
test_inject_properties_changed(void **state)
{
    props_fixture *f = *state;
    ip_properties_subscribe(&f->props);

    ip_properties_changed_payload p = {
        .sender      = EXP_SENDER,
        .iface_name  = IP_IFACE_COMPOSITE,
        .prop_name   = "ProfileName",
        .prop_type   = IP_PROP_TYPE_STRING,
        .value       = "My Profile",
        .array_count = 0,
    };
    int rc = f->backend->inject_signal(f->mock.bus,
        IP_IFACE_PROPERTIES, "PropertiesChanged", &p);
    assert_int_equal(rc, 0);

    assert_int_equal(f->captured.call_count, 1);
    assert_string_equal(f->captured.prop_name, "ProfileName");
    assert_string_equal(f->captured.value, "My Profile");
}

/* Inject array property via mock. */
static void
test_inject_array_property(void **state)
{
    props_fixture *f = *state;
    ip_properties_subscribe(&f->props);

    ip_properties_changed_payload p = {
        .sender      = EXP_SENDER,
        .iface_name  = IP_IFACE_MANAGER,
        .prop_name   = "GamepadOrder",
        .prop_type   = IP_PROP_TYPE_ARRAY,
        .value       = "gp0,gp1,gp2,gp3",
        .array_count = 4,
    };
    f->backend->inject_signal(f->mock.bus,
        IP_IFACE_PROPERTIES, "PropertiesChanged", &p);

    assert_int_equal(f->captured.call_count, 1);
    assert_string_equal(f->captured.value, "gp0,gp1,gp2,gp3");
    assert_int_equal(f->captured.count, 4);
}

/* Multiple changes accumulate in the capture. */
static void
test_multiple_changes(void **state)
{
    props_fixture *f = *state;
    ip_properties_subscribe(&f->props);

    /* First change. */
    ip_properties_changed_payload p1 = {
        .sender      = EXP_SENDER,
        .iface_name  = IP_IFACE_COMPOSITE,
        .prop_name   = "ProfileName",
        .prop_type   = IP_PROP_TYPE_STRING,
        .value       = "Profile A",
        .array_count = 0,
    };
    f->backend->inject_signal(f->mock.bus,
        IP_IFACE_PROPERTIES, "PropertiesChanged", &p1);
    assert_int_equal(f->captured.call_count, 1);
    assert_string_equal(f->captured.value, "Profile A");

    /* Second change. */
    ip_properties_changed_payload p2 = {
        .sender      = EXP_SENDER,
        .iface_name  = IP_IFACE_COMPOSITE,
        .prop_name   = "ProfilePath",
        .prop_type   = IP_PROP_TYPE_STRING,
        .value       = "/path/to/profile.yaml",
        .array_count = 0,
    };
    f->backend->inject_signal(f->mock.bus,
        IP_IFACE_PROPERTIES, "PropertiesChanged", &p2);
    assert_int_equal(f->captured.call_count, 2);
    assert_string_equal(f->captured.value, "/path/to/profile.yaml");
}

/* --- Main ----------------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* Simple */
        cmocka_unit_test(test_props_init),
        cmocka_unit_test(test_props_init_null),
        cmocka_unit_test_setup_teardown(test_props_subscribe,
            setup_props, teardown_props),
        cmocka_unit_test(test_props_subscribe_null),

        /* String properties */
        cmocka_unit_test_setup_teardown(test_handle_profilename,
            setup_props, teardown_props),
        cmocka_unit_test_setup_teardown(test_handle_profilepath,
            setup_props, teardown_props),
        cmocka_unit_test_setup_teardown(test_handle_profilename_too_long,
            setup_props, teardown_props),
        cmocka_unit_test_setup_teardown(test_handle_profilepath_too_long,
            setup_props, teardown_props),

        /* Array properties */
        cmocka_unit_test_setup_teardown(test_handle_gamepadorder,
            setup_props, teardown_props),
        cmocka_unit_test_setup_teardown(test_handle_targetdevices,
            setup_props, teardown_props),
        cmocka_unit_test_setup_teardown(test_handle_sourcedevicepaths,
            setup_props, teardown_props),
        cmocka_unit_test_setup_teardown(test_handle_array_single,
            setup_props, teardown_props),
        cmocka_unit_test_setup_teardown(test_handle_array_empty,
            setup_props, teardown_props),
        cmocka_unit_test_setup_teardown(test_handle_array_elem_too_long_name,
            setup_props, teardown_props),
        cmocka_unit_test_setup_teardown(test_handle_array_too_many,
            setup_props, teardown_props),
        cmocka_unit_test_setup_teardown(test_handle_array_max_elems,
            setup_props, teardown_props),
        cmocka_unit_test_setup_teardown(test_handle_array_path_long_elem,
            setup_props, teardown_props),
        cmocka_unit_test_setup_teardown(test_handle_array_path_elem_too_long,
            setup_props, teardown_props),

        /* Type validation */
        cmocka_unit_test_setup_teardown(test_handle_type_mismatch_string_expected,
            setup_props, teardown_props),
        cmocka_unit_test_setup_teardown(test_handle_type_mismatch_array_expected,
            setup_props, teardown_props),

        /* Invalidated */
        cmocka_unit_test_setup_teardown(test_handle_invalidated,
            setup_props, teardown_props),
        cmocka_unit_test_setup_teardown(test_handle_invalidated_untracked,
            setup_props, teardown_props),

        /* Sender verification */
        cmocka_unit_test_setup_teardown(test_handle_wrong_sender,
            setup_props, teardown_props),
        cmocka_unit_test_setup_teardown(test_handle_null_sender,
            setup_props, teardown_props),

        /* Edge cases */
        cmocka_unit_test_setup_teardown(test_handle_untracked_prop,
            setup_props, teardown_props),
        cmocka_unit_test_setup_teardown(test_handle_null_payload,
            setup_props, teardown_props),
        cmocka_unit_test(test_handle_null_handler),
        cmocka_unit_test(test_handle_null_callback),
        cmocka_unit_test_setup_teardown(test_handle_string_null_value,
            setup_props, teardown_props),
        cmocka_unit_test_setup_teardown(test_handle_array_null_value,
            setup_props, teardown_props),

        /* Integration */
        cmocka_unit_test_setup_teardown(test_inject_properties_changed,
            setup_props, teardown_props),
        cmocka_unit_test_setup_teardown(test_inject_array_property,
            setup_props, teardown_props),
        cmocka_unit_test_setup_teardown(test_multiple_changes,
            setup_props, teardown_props),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}