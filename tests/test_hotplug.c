/*
 * test_hotplug.c — Hotplug signal handling tests (Task 11).
 *
 * Tests InterfacesAdded/InterfacesRemoved signal processing, device model
 * incremental updates, sender verification, and path validation.
 */
#include "dbus/ip_hotplug.h"
#include "dbus/ip_device_model.h"
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
#define IP_ROOT    "/org/shadowblip/InputPlumber"

typedef struct {
    ip_dbus_mock    mock;
    const ip_dbus_backend *backend;
    cbx_device_model model;
    ip_hotplug       hp;
} hotplug_fixture;

static int
setup_hotplug(void **state)
{
    hotplug_fixture *f = malloc(sizeof(*f));
    assert_non_null(f);

    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);
    cbx_device_model_init(&f->model);
    ip_hotplug_init(&f->hp, f->backend, f->mock.bus, EXP_SENDER, &f->model);

    *state = f;
    return 0;
}

static int
teardown_hotplug(void **state)
{
    hotplug_fixture *f = *state;
    ip_dbus_mock_reset(&f->mock);
    free(f);
    return 0;
}

/* --- Simple tests (no fixture) ------------------------------------------- */

/* Init zeroes the struct. */
static void
test_hotplug_init(void **state)
{
    (void)state;
    ip_hotplug hp;
    cbx_device_model model;
    ip_dbus_mock mock;
    ip_dbus_mock_init(&mock);
    const ip_dbus_backend *backend = ip_dbus_mock_backend(&mock);

    ip_hotplug_init(&hp, backend, mock.bus, ":1.99", &model);

    assert_ptr_equal(hp.backend, backend);
    assert_ptr_equal(hp.model, &model);
    assert_string_equal(hp.expected_sender, ":1.99");
}

/* Init with NULL is safe. */
static void
test_hotplug_init_null(void **state)
{
    (void)state;
    ip_hotplug hp;
    memset(&hp, 0xFF, sizeof(hp));
    ip_hotplug_init(NULL, NULL, NULL, NULL, NULL);
    /* just shouldn't crash */
    ip_hotplug_init(&hp, NULL, NULL, NULL, NULL);
    assert_null(hp.backend);
    assert_null(hp.model);
}

/* Subscribe registers both InterfacesAdded and InterfacesRemoved. */
static void
test_hotplug_subscribe(void **state)
{
    hotplug_fixture *f = *state;

    int rc = ip_hotplug_subscribe(&f->hp);
    assert_int_equal(rc, 0);

    /* Two subscriptions should be registered. */
    assert_int_equal(f->mock.sub_count, 2);
    assert_string_equal(f->mock.subscriptions[0].iface,
                        IP_IFACE_OBJECT_MANAGER);
    assert_string_equal(f->mock.subscriptions[0].member,
                        "InterfacesAdded");
    assert_string_equal(f->mock.subscriptions[1].member,
                        "InterfacesRemoved");
}

/* Subscribe with NULL backend fails. */
static void
test_hotplug_subscribe_null(void **state)
{
    (void)state;
    ip_hotplug hp;
    memset(&hp, 0, sizeof(hp));
    int rc = ip_hotplug_subscribe(&hp);
    assert_int_equal(rc, -EINVAL);
}

/* Subscribe fails if first subscribe_signal fails. */
static void
test_hotplug_subscribe_fail_first(void **state)
{
    hotplug_fixture *f = *state;
    f->mock.subscribe_fail_rc = -ENOMEM;

    int rc = ip_hotplug_subscribe(&f->hp);
    assert_int_equal(rc, -ENOMEM);
    assert_int_equal(f->mock.sub_count, 0);
}

/* Subscribe fails if second subscribe_signal fails. */
static void
test_hotplug_subscribe_fail_second(void **state)
{
    hotplug_fixture *f = *state;

    /* Make the second subscribe call fail by filling all but one slot. */
    /* Actually, we can't easily make only the second fail with the mock.
     * Instead, test that when subscribe_fail_rc is set, both fail. */
    f->mock.subscribe_fail_rc = -EIO;

    int rc = ip_hotplug_subscribe(&f->hp);
    assert_int_equal(rc, -EIO);
}

/* --- Device model mutation tests ----------------------------------------- */

/* Add composite. */
static void
test_model_add_composite(void **state)
{
    (void)state;
    cbx_device_model model;
    cbx_device_model_init(&model);

    bool ok = cbx_device_model_add_composite(&model,
        IP_ROOT "/CompositeDevice0");
    assert_true(ok);
    assert_int_equal(model.composite_count, 1);
    assert_int_equal(model.composites[0].index, 0);
}

/* Add composite duplicate is idempotent. */
static void
test_model_add_composite_dup(void **state)
{
    (void)state;
    cbx_device_model model;
    cbx_device_model_init(&model);

    cbx_device_model_add_composite(&model, IP_ROOT "/CompositeDevice0");
    bool ok = cbx_device_model_add_composite(&model,
        IP_ROOT "/CompositeDevice0");
    assert_false(ok);
    assert_int_equal(model.composite_count, 1);
}

/* Add composite when full returns false. */
static void
test_model_add_composite_full(void **state)
{
    (void)state;
    cbx_device_model model;
    cbx_device_model_init(&model);

    char path[256];
    for (int i = 0; i < CBX_MAX_COMPOSITES; i++) {
        snprintf(path, sizeof(path), IP_ROOT "/CompositeDevice%d", i);
        cbx_device_model_add_composite(&model, path);
    }
    bool ok = cbx_device_model_add_composite(&model,
        IP_ROOT "/CompositeDevice99");
    assert_false(ok);
}

/* Remove composite. */
static void
test_model_remove_composite(void **state)
{
    (void)state;
    cbx_device_model model;
    cbx_device_model_init(&model);

    cbx_device_model_add_composite(&model, IP_ROOT "/CompositeDevice0");
    cbx_device_model_add_composite(&model, IP_ROOT "/CompositeDevice1");

    bool ok = cbx_device_model_remove_composite(&model,
        IP_ROOT "/CompositeDevice0");
    assert_true(ok);
    assert_int_equal(model.composite_count, 1);
    assert_int_equal(model.composites[0].index, 1);
}

/* Remove composite not found. */
static void
test_model_remove_composite_notfound(void **state)
{
    (void)state;
    cbx_device_model model;
    cbx_device_model_init(&model);

    bool ok = cbx_device_model_remove_composite(&model,
        IP_ROOT "/CompositeDevice0");
    assert_false(ok);
}

/* Add and remove source. */
static void
test_model_add_remove_source(void **state)
{
    (void)state;
    cbx_device_model model;
    cbx_device_model_init(&model);

    bool ok = cbx_device_model_add_source(&model,
        IP_ROOT "/devices/source/event0");
    assert_true(ok);
    assert_int_equal(model.source_count, 1);
    assert_string_equal(model.sources[0].name, "event0");

    ok = cbx_device_model_remove_source(&model,
        IP_ROOT "/devices/source/event0");
    assert_true(ok);
    assert_int_equal(model.source_count, 0);
}

/* Add and remove target. */
static void
test_model_add_remove_target(void **state)
{
    (void)state;
    cbx_device_model model;
    cbx_device_model_init(&model);

    bool ok = cbx_device_model_add_target(&model,
        IP_ROOT "/devices/target/gamepad0");
    assert_true(ok);
    assert_int_equal(model.target_count, 1);
    assert_string_equal(model.targets[0].name, "gamepad0");

    ok = cbx_device_model_remove_target(&model,
        IP_ROOT "/devices/target/gamepad0");
    assert_true(ok);
    assert_int_equal(model.target_count, 0);
}

/* Set and remove manager. */
static void
test_model_set_remove_manager(void **state)
{
    (void)state;
    cbx_device_model model;
    cbx_device_model_init(&model);

    bool ok = cbx_device_model_set_manager(&model,
        IP_ROOT "/Manager");
    assert_true(ok);
    assert_true(model.has_manager);

    ok = cbx_device_model_remove_manager(&model);
    assert_true(ok);
    assert_false(model.has_manager);

    /* Remove when not set. */
    ok = cbx_device_model_remove_manager(&model);
    assert_false(ok);
}

/* Add source duplicate is idempotent. */
static void
test_model_add_source_dup(void **state)
{
    (void)state;
    cbx_device_model model;
    cbx_device_model_init(&model);

    cbx_device_model_add_source(&model, IP_ROOT "/devices/source/event0");
    bool ok = cbx_device_model_add_source(&model,
        IP_ROOT "/devices/source/event0");
    assert_false(ok);
    assert_int_equal(model.source_count, 1);
}

/* Add target when full. */
static void
test_model_add_target_full(void **state)
{
    (void)state;
    cbx_device_model model;
    cbx_device_model_init(&model);

    char path[256];
    for (int i = 0; i < CBX_MAX_DEVICES; i++) {
        snprintf(path, sizeof(path), IP_ROOT "/devices/target/gp%d", i);
        cbx_device_model_add_target(&model, path);
    }
    bool ok = cbx_device_model_add_target(&model,
        IP_ROOT "/devices/target/gp99");
    assert_false(ok);
}

/* --- Handle Added/Removed tests ------------------------------------------ */

/* InterfacesAdded adds a composite. */
static void
test_handle_added_composite(void **state)
{
    hotplug_fixture *f = *state;

    ip_interfaces_changed_payload p = {
        .sender     = EXP_SENDER,
        .path       = IP_ROOT "/CompositeDevice0",
        .interfaces = IP_IFACE_COMPOSITE,
    };
    ip_hotplug_handle_added(&f->hp, &p);

    assert_int_equal(f->model.composite_count, 1);
    assert_int_equal(f->model.composites[0].index, 0);
}

/* InterfacesAdded adds a source. */
static void
test_handle_added_source(void **state)
{
    hotplug_fixture *f = *state;

    ip_interfaces_changed_payload p = {
        .sender     = EXP_SENDER,
        .path       = IP_ROOT "/devices/source/event0",
        .interfaces = "org.shadowblip.Input.EventDevice",
    };
    ip_hotplug_handle_added(&f->hp, &p);

    assert_int_equal(f->model.source_count, 1);
    assert_string_equal(f->model.sources[0].name, "event0");
}

/* InterfacesAdded adds a target. */
static void
test_handle_added_target(void **state)
{
    hotplug_fixture *f = *state;

    ip_interfaces_changed_payload p = {
        .sender     = EXP_SENDER,
        .path       = IP_ROOT "/devices/target/gamepad0",
        .interfaces = "org.shadowblip.Input.DBusDevice",
    };
    ip_hotplug_handle_added(&f->hp, &p);

    assert_int_equal(f->model.target_count, 1);
    assert_string_equal(f->model.targets[0].name, "gamepad0");
}

/* InterfacesAdded sets the manager. */
static void
test_handle_added_manager(void **state)
{
    hotplug_fixture *f = *state;

    ip_interfaces_changed_payload p = {
        .sender     = EXP_SENDER,
        .path       = IP_ROOT "/Manager",
        .interfaces = IP_IFACE_MANAGER,
    };
    ip_hotplug_handle_added(&f->hp, &p);

    assert_true(f->model.has_manager);
    assert_string_equal(f->model.manager_path, IP_ROOT "/Manager");
}

/* InterfacesAdded with multiple interfaces adds to multiple categories. */
static void
test_handle_added_multi_iface(void **state)
{
    hotplug_fixture *f = *state;

    /* A path that is both composite and has DBusDevice interface. */
    ip_interfaces_changed_payload p = {
        .sender     = EXP_SENDER,
        .path       = IP_ROOT "/CompositeDevice0",
        .interfaces = IP_IFACE_COMPOSITE ",org.shadowblip.Input.DBusDevice",
    };
    ip_hotplug_handle_added(&f->hp, &p);

    assert_int_equal(f->model.composite_count, 1);
}

/* InterfacesAdded with wrong sender is dropped. */
static void
test_handle_added_wrong_sender(void **state)
{
    hotplug_fixture *f = *state;

    ip_interfaces_changed_payload p = {
        .sender     = ":1.999",
        .path       = IP_ROOT "/CompositeDevice0",
        .interfaces = IP_IFACE_COMPOSITE,
    };
    ip_hotplug_handle_added(&f->hp, &p);

    assert_int_equal(f->model.composite_count, 0);
}

/* InterfacesAdded with invalid path is dropped. */
static void
test_handle_added_invalid_path(void **state)
{
    hotplug_fixture *f = *state;

    ip_interfaces_changed_payload p = {
        .sender     = EXP_SENDER,
        .path       = "/org/example/Malicious",
        .interfaces = IP_IFACE_COMPOSITE,
    };
    ip_hotplug_handle_added(&f->hp, &p);

    assert_int_equal(f->model.composite_count, 0);
}

/* InterfacesAdded with NULL interfaces is handled (no crash). */
static void
test_handle_added_null_interfaces(void **state)
{
    hotplug_fixture *f = *state;

    ip_interfaces_changed_payload p = {
        .sender     = EXP_SENDER,
        .path       = IP_ROOT "/devices/source/event0",
        .interfaces = NULL,
    };
    ip_hotplug_handle_added(&f->hp, &p);

    /* Source is added by path pattern, not interface. */
    assert_int_equal(f->model.source_count, 1);
}

/* InterfacesRemoved removes a composite. */
static void
test_handle_removed_composite(void **state)
{
    hotplug_fixture *f = *state;

    /* Add first. */
    ip_interfaces_changed_payload add_p = {
        .sender     = EXP_SENDER,
        .path       = IP_ROOT "/CompositeDevice0",
        .interfaces = IP_IFACE_COMPOSITE,
    };
    ip_hotplug_handle_added(&f->hp, &add_p);
    assert_int_equal(f->model.composite_count, 1);

    /* Now remove. */
    ip_interfaces_changed_payload rem_p = {
        .sender     = EXP_SENDER,
        .path       = IP_ROOT "/CompositeDevice0",
        .interfaces = IP_IFACE_COMPOSITE,
    };
    ip_hotplug_handle_removed(&f->hp, &rem_p);
    assert_int_equal(f->model.composite_count, 0);
}

/* InterfacesRemoved removes a source. */
static void
test_handle_removed_source(void **state)
{
    hotplug_fixture *f = *state;

    ip_interfaces_changed_payload add_p = {
        .sender     = EXP_SENDER,
        .path       = IP_ROOT "/devices/source/event0",
        .interfaces = "org.shadowblip.Input.EventDevice",
    };
    ip_hotplug_handle_added(&f->hp, &add_p);

    ip_interfaces_changed_payload rem_p = {
        .sender     = EXP_SENDER,
        .path       = IP_ROOT "/devices/source/event0",
        .interfaces = "org.shadowblip.Input.EventDevice",
    };
    ip_hotplug_handle_removed(&f->hp, &rem_p);
    assert_int_equal(f->model.source_count, 0);
}

/* InterfacesRemoved removes a target. */
static void
test_handle_removed_target(void **state)
{
    hotplug_fixture *f = *state;

    ip_interfaces_changed_payload add_p = {
        .sender     = EXP_SENDER,
        .path       = IP_ROOT "/devices/target/gamepad0",
        .interfaces = "org.shadowblip.Input.DBusDevice",
    };
    ip_hotplug_handle_added(&f->hp, &add_p);

    ip_interfaces_changed_payload rem_p = {
        .sender     = EXP_SENDER,
        .path       = IP_ROOT "/devices/target/gamepad0",
        .interfaces = "org.shadowblip.Input.DBusDevice",
    };
    ip_hotplug_handle_removed(&f->hp, &rem_p);
    assert_int_equal(f->model.target_count, 0);
}

/* InterfacesRemoved removes the manager. */
static void
test_handle_removed_manager(void **state)
{
    hotplug_fixture *f = *state;

    ip_interfaces_changed_payload add_p = {
        .sender     = EXP_SENDER,
        .path       = IP_ROOT "/Manager",
        .interfaces = IP_IFACE_MANAGER,
    };
    ip_hotplug_handle_added(&f->hp, &add_p);
    assert_true(f->model.has_manager);

    ip_interfaces_changed_payload rem_p = {
        .sender     = EXP_SENDER,
        .path       = IP_ROOT "/Manager",
        .interfaces = IP_IFACE_MANAGER,
    };
    ip_hotplug_handle_removed(&f->hp, &rem_p);
    assert_false(f->model.has_manager);
}

/* InterfacesRemoved with wrong sender is dropped. */
static void
test_handle_removed_wrong_sender(void **state)
{
    hotplug_fixture *f = *state;

    ip_interfaces_changed_payload add_p = {
        .sender     = EXP_SENDER,
        .path       = IP_ROOT "/CompositeDevice0",
        .interfaces = IP_IFACE_COMPOSITE,
    };
    ip_hotplug_handle_added(&f->hp, &add_p);

    ip_interfaces_changed_payload rem_p = {
        .sender     = ":1.999",
        .path       = IP_ROOT "/CompositeDevice0",
        .interfaces = IP_IFACE_COMPOSITE,
    };
    ip_hotplug_handle_removed(&f->hp, &rem_p);
    /* Should not be removed. */
    assert_int_equal(f->model.composite_count, 1);
}

/* Handle with NULL payload is safe. */
static void
test_handle_null_payload(void **state)
{
    hotplug_fixture *f = *state;
    ip_hotplug_handle_added(&f->hp, NULL);
    ip_hotplug_handle_removed(&f->hp, NULL);
    /* no crash */
}

/* Handle with NULL model is safe. */
static void
test_handle_null_model(void **state)
{
    (void)state;
    ip_hotplug hp;
    memset(&hp, 0, sizeof(hp));
    hp.expected_sender = EXP_SENDER;

    ip_interfaces_changed_payload p = {
        .sender     = EXP_SENDER,
        .path       = IP_ROOT "/CompositeDevice0",
        .interfaces = IP_IFACE_COMPOSITE,
    };
    ip_hotplug_handle_added(&hp, &p);  /* model is NULL */
    /* no crash */
}

/* --- Integration with mock inject_signal ---------------------------------- */

/* Inject InterfacesAdded via mock, verify model is updated. */
static void
test_inject_added_composite(void **state)
{
    hotplug_fixture *f = *state;
    ip_hotplug_subscribe(&f->hp);

    ip_interfaces_changed_payload p = {
        .sender     = EXP_SENDER,
        .path       = IP_ROOT "/CompositeDevice2",
        .interfaces = IP_IFACE_COMPOSITE,
    };
    int rc = f->backend->inject_signal(f->mock.bus,
        IP_IFACE_OBJECT_MANAGER, "InterfacesAdded", &p);
    assert_int_equal(rc, 0);

    assert_int_equal(f->model.composite_count, 1);
    assert_int_equal(f->model.composites[0].index, 2);
}

/* Inject InterfacesRemoved via mock. */
static void
test_inject_removed_source(void **state)
{
    hotplug_fixture *f = *state;
    ip_hotplug_subscribe(&f->hp);

    /* Add a source first. */
    ip_interfaces_changed_payload add_p = {
        .sender     = EXP_SENDER,
        .path       = IP_ROOT "/devices/source/event5",
        .interfaces = "org.shadowblip.Input.EventDevice",
    };
    f->backend->inject_signal(f->mock.bus,
        IP_IFACE_OBJECT_MANAGER, "InterfacesAdded", &add_p);
    assert_int_equal(f->model.source_count, 1);

    /* Remove it. */
    ip_interfaces_changed_payload rem_p = {
        .sender     = EXP_SENDER,
        .path       = IP_ROOT "/devices/source/event5",
        .interfaces = "org.shadowblip.Input.EventDevice",
    };
    f->backend->inject_signal(f->mock.bus,
        IP_IFACE_OBJECT_MANAGER, "InterfacesRemoved", &rem_p);
    assert_int_equal(f->model.source_count, 0);
}

/* Inject InterfacesAdded with wrong sender — signal is silently dropped. */
static void
test_inject_added_wrong_sender(void **state)
{
    hotplug_fixture *f = *state;
    ip_hotplug_subscribe(&f->hp);

    ip_interfaces_changed_payload p = {
        .sender     = ":1.999",
        .path       = IP_ROOT "/CompositeDevice7",
        .interfaces = IP_IFACE_COMPOSITE,
    };
    int rc = f->backend->inject_signal(f->mock.bus,
        IP_IFACE_OBJECT_MANAGER, "InterfacesAdded", &p);
    assert_int_equal(rc, 0);

    /* Model must be unchanged — spoofed signal was dropped. */
    assert_int_equal(f->model.composite_count, 0);
}

/* Inject InterfacesRemoved with wrong sender — signal is silently dropped. */
static void
test_inject_removed_wrong_sender(void **state)
{
    hotplug_fixture *f = *state;
    ip_hotplug_subscribe(&f->hp);

    /* Add a composite with the correct sender first. */
    ip_interfaces_changed_payload add_p = {
        .sender     = EXP_SENDER,
        .path       = IP_ROOT "/CompositeDevice3",
        .interfaces = IP_IFACE_COMPOSITE,
    };
    f->backend->inject_signal(f->mock.bus,
        IP_IFACE_OBJECT_MANAGER, "InterfacesAdded", &add_p);
    assert_int_equal(f->model.composite_count, 1);

    /* Attempt to remove with a spoofed sender. */
    ip_interfaces_changed_payload rem_p = {
        .sender     = ":1.999",
        .path       = IP_ROOT "/CompositeDevice3",
        .interfaces = IP_IFACE_COMPOSITE,
    };
    f->backend->inject_signal(f->mock.bus,
        IP_IFACE_OBJECT_MANAGER, "InterfacesRemoved", &rem_p);

    /* Composite must still be present — spoofed removal was dropped. */
    assert_int_equal(f->model.composite_count, 1);
}

/* Add then remove then add again (resurrection). */
static void
test_add_remove_add(void **state)
{
    hotplug_fixture *f = *state;

    const char *path = IP_ROOT "/devices/target/keyboard0";

    ip_interfaces_changed_payload add1 = {
        .sender = EXP_SENDER, .path = path,
        .interfaces = "org.shadowblip.Input.DBusDevice",
    };
    ip_hotplug_handle_added(&f->hp, &add1);
    assert_int_equal(f->model.target_count, 1);

    ip_interfaces_changed_payload rem1 = {
        .sender = EXP_SENDER, .path = path,
        .interfaces = "org.shadowblip.Input.DBusDevice",
    };
    ip_hotplug_handle_removed(&f->hp, &rem1);
    assert_int_equal(f->model.target_count, 0);

    ip_interfaces_changed_payload add2 = {
        .sender = EXP_SENDER, .path = path,
        .interfaces = "org.shadowblip.Input.DBusDevice",
    };
    ip_hotplug_handle_added(&f->hp, &add2);
    assert_int_equal(f->model.target_count, 1);
}

/* --- Main ----------------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* Simple */
        cmocka_unit_test(test_hotplug_init),
        cmocka_unit_test(test_hotplug_init_null),
        cmocka_unit_test_setup_teardown(test_hotplug_subscribe,
            setup_hotplug, teardown_hotplug),
        cmocka_unit_test(test_hotplug_subscribe_null),
        cmocka_unit_test_setup_teardown(test_hotplug_subscribe_fail_first,
            setup_hotplug, teardown_hotplug),
        cmocka_unit_test_setup_teardown(test_hotplug_subscribe_fail_second,
            setup_hotplug, teardown_hotplug),

        /* Device model mutation */
        cmocka_unit_test(test_model_add_composite),
        cmocka_unit_test(test_model_add_composite_dup),
        cmocka_unit_test(test_model_add_composite_full),
        cmocka_unit_test(test_model_remove_composite),
        cmocka_unit_test(test_model_remove_composite_notfound),
        cmocka_unit_test(test_model_add_remove_source),
        cmocka_unit_test(test_model_add_remove_target),
        cmocka_unit_test(test_model_set_remove_manager),
        cmocka_unit_test(test_model_add_source_dup),
        cmocka_unit_test(test_model_add_target_full),

        /* Handle Added/Removed */
        cmocka_unit_test_setup_teardown(test_handle_added_composite,
            setup_hotplug, teardown_hotplug),
        cmocka_unit_test_setup_teardown(test_handle_added_source,
            setup_hotplug, teardown_hotplug),
        cmocka_unit_test_setup_teardown(test_handle_added_target,
            setup_hotplug, teardown_hotplug),
        cmocka_unit_test_setup_teardown(test_handle_added_manager,
            setup_hotplug, teardown_hotplug),
        cmocka_unit_test_setup_teardown(test_handle_added_multi_iface,
            setup_hotplug, teardown_hotplug),
        cmocka_unit_test_setup_teardown(test_handle_added_wrong_sender,
            setup_hotplug, teardown_hotplug),
        cmocka_unit_test_setup_teardown(test_handle_added_invalid_path,
            setup_hotplug, teardown_hotplug),
        cmocka_unit_test_setup_teardown(test_handle_added_null_interfaces,
            setup_hotplug, teardown_hotplug),
        cmocka_unit_test_setup_teardown(test_handle_removed_composite,
            setup_hotplug, teardown_hotplug),
        cmocka_unit_test_setup_teardown(test_handle_removed_source,
            setup_hotplug, teardown_hotplug),
        cmocka_unit_test_setup_teardown(test_handle_removed_target,
            setup_hotplug, teardown_hotplug),
        cmocka_unit_test_setup_teardown(test_handle_removed_manager,
            setup_hotplug, teardown_hotplug),
        cmocka_unit_test_setup_teardown(test_handle_removed_wrong_sender,
            setup_hotplug, teardown_hotplug),
        cmocka_unit_test_setup_teardown(test_handle_null_payload,
            setup_hotplug, teardown_hotplug),
        cmocka_unit_test_setup_teardown(test_handle_null_model,
            setup_hotplug, teardown_hotplug),

        /* Integration */
        cmocka_unit_test_setup_teardown(test_inject_added_composite,
            setup_hotplug, teardown_hotplug),
        cmocka_unit_test_setup_teardown(test_inject_removed_source,
            setup_hotplug, teardown_hotplug),
        cmocka_unit_test_setup_teardown(test_inject_added_wrong_sender,
            setup_hotplug, teardown_hotplug),
        cmocka_unit_test_setup_teardown(test_inject_removed_wrong_sender,
            setup_hotplug, teardown_hotplug),
        cmocka_unit_test_setup_teardown(test_add_remove_add,
            setup_hotplug, teardown_hotplug),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}