/*
 * test_gamepad_order.c — Unit tests for GamepadOrder persistence (Task 15,
 *                        gap #2; task 6 identity).
 *
 * Tests:
 *   - Save: composite paths CSV → source-derived identity → assignments.yaml.
 *   - Save with stale paths (skipped).
 *   - Save with a transient identity query failure (skipped, not persisted).
 *   - Save empty CSV (clears order).
 *   - Load: assignments.yaml → CSV of IDs.
 *   - Load with no saved order.
 *   - Load with invalid IDs (rejected — malformed assignments file).
 *   - Round-trip: save then load.
 *   - NULL args.
 */
#include "dbus_mock.h"
#include "dbus/ip_gamepad_order.h"
#include "dbus/ip_device_model.h"
#include "config/config_assignments.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

/* --- Test fixture -------------------------------------------------------- */

typedef struct {
    ip_dbus_mock           mock;
    const ip_dbus_backend  *backend;
    cbx_device_model       model;
    char                   temp_home[512];
} gamepad_fixture;

static int
setup(void **state)
{
    gamepad_fixture *f = malloc(sizeof(*f));
    if (!f)
        return -1;
    memset(f, 0, sizeof(*f));
    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);
    cbx_device_model_init(&f->model);

    snprintf(f->temp_home, sizeof(f->temp_home),
             "/tmp/cbx-gpo-XXXXXX");
    if (!mkdtemp(f->temp_home)) {
        free(f);
        return -1;
    }
    setenv("HOME", f->temp_home, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_RUNTIME_DIR");

    *state = f;
    return 0;
}

static int
teardown(void **state)
{
    gamepad_fixture *f = *state;
    if (f) {
        ip_dbus_mock_reset(&f->mock);
        if (f->temp_home[0]) {
            char cmd[600];
            snprintf(cmd, sizeof(cmd), "rm -rf %s", f->temp_home);
            int __r = system(cmd); (void)__r;
        }
        free(f);
    }
    return 0;
}

#define FIX(state) (*(gamepad_fixture **)(state))

/* --- Helpers ------------------------------------------------------------- */

/* Write raw assignments.yaml with a gamepad_order section. */
static void
write_raw_gamepad_order(const char *home, const char *order_yaml)
{
    char path[700];
    snprintf(path, sizeof(path), "%s/.config/controller-box", home);
    char cmd[800];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", path);
    int __r = system(cmd); (void)__r;

    snprintf(path, sizeof(path), "%s/.config/controller-box/assignments.yaml",
             home);
    FILE *fp = fopen(path, "w");
    assert_non_null(fp);
    fputs(order_yaml, fp);
    fclose(fp);
}

/* Every composite reports this single evdev source (the mock is keyed by
 * iface+member only, so one set of expectations covers all composites). */
static void
expect_evdev_source(gamepad_fixture *f, const char *unique_id,
                    const char *phys_path, const char *bustype)
{
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "SourceDevicePaths",
        "/org/shadowblip/InputPlumber/devices/source/event0");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_EVENT, "UniqueId",
                           unique_id);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_EVENT, "PhysPath",
                           phys_path);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_EVENT, "IdBustype",
                           bustype);
}

static cbx_assignments
load_assignments(void)
{
    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assignments_load(&a), 0);
    return a;
}

/* --- Save tests ---------------------------------------------------------- */

static void
test_save_success(void **state)
{
    gamepad_fixture *f = FIX(state);

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    expect_evdev_source(f, "serial-aaaa", "", "3");

    int rc = ip_gamepad_order_save(f->backend, f->mock.bus, &f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0");
    assert_int_equal(rc, 0);

    cbx_assignments a = load_assignments();
    assert_int_equal(a.gamepad_order_count, 1);
    assert_string_equal(a.gamepad_order[0], "USB:serial-aaaa");
}

static void
test_save_empty_csv(void **state)
{
    gamepad_fixture *f = FIX(state);

    int rc = ip_gamepad_order_save(f->backend, f->mock.bus, &f->model, "");
    assert_int_equal(rc, 0);

    cbx_assignments a = load_assignments();
    assert_int_equal(a.gamepad_order_count, 0);
}

static void
test_save_stale_path_skipped(void **state)
{
    gamepad_fixture *f = FIX(state);

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    expect_evdev_source(f, "serial-aaaa", "", "3");

    /* The second path is stale (not in model) — should be skipped. */
    const char *paths_csv =
        "/org/shadowblip/InputPlumber/CompositeDevice0,"
        "/org/shadowblip/InputPlumber/CompositeDevice99";

    int rc = ip_gamepad_order_save(f->backend, f->mock.bus,
                                    &f->model, paths_csv);
    assert_int_equal(rc, 0);

    cbx_assignments a = load_assignments();
    assert_int_equal(a.gamepad_order_count, 1);
    assert_string_equal(a.gamepad_order[0], "USB:serial-aaaa");
}

/*
 * A transient identity query failure must not persist a wrong/opaque id:
 * the entry is skipped so the saved order is never corrupted.
 */
static void
test_save_query_failure_skipped(void **state)
{
    gamepad_fixture *f = FIX(state);

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
        "SourceDevicePaths", -EIO);

    int rc = ip_gamepad_order_save(f->backend, f->mock.bus, &f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0");
    assert_int_equal(rc, 0);

    cbx_assignments a = load_assignments();
    assert_int_equal(a.gamepad_order_count, 0);
}

static void
test_save_preserves_assignments(void **state)
{
    gamepad_fixture *f = FIX(state);

    /* Pre-populate assignments.yaml with an assignment entry. */
    char path[700];
    snprintf(path, sizeof(path), "%s/.config/controller-box",
             f->temp_home);
    char cmd[800];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", path);
    int __r = system(cmd); (void)__r;

    snprintf(path, sizeof(path), "%s/.config/controller-box/assignments.yaml",
             f->temp_home);
    FILE *fp = fopen(path, "w");
    assert_non_null(fp);
    fputs("assignments:\n  - id: USB:serial-test\n    slot: 0\n    profile: \"\"\n",
          fp);
    fclose(fp);

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    expect_evdev_source(f, "serial-aaaa", "", "3");

    int rc = ip_gamepad_order_save(f->backend, f->mock.bus, &f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0");
    assert_int_equal(rc, 0);

    cbx_assignments a = load_assignments();
    /* Assignment should still be there. */
    assert_int_equal(a.assignment_count, 1);
    assert_string_equal(a.assignments[0].id, "USB:serial-test");
    /* Gamepad order should be saved. */
    assert_int_equal(a.gamepad_order_count, 1);
    assert_string_equal(a.gamepad_order[0], "USB:serial-aaaa");
}

static void
test_save_null_args(void **state)
{
    gamepad_fixture *f = FIX(state);

    assert_int_equal(ip_gamepad_order_save(NULL, f->mock.bus,
        &f->model, "path"), -EINVAL);
    assert_int_equal(ip_gamepad_order_save(f->backend, f->mock.bus,
        NULL, "path"), -EINVAL);
    assert_int_equal(ip_gamepad_order_save(f->backend, f->mock.bus,
        &f->model, NULL), -EINVAL);
}

static void
test_save_order_id_when_no_stable_identity(void **state)
{
    gamepad_fixture *f = FIX(state);

    /* Successfully-read but empty source list → ORDER:<index> fallback. */
    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "SourceDevicePaths", "");

    int rc = ip_gamepad_order_save(f->backend, f->mock.bus, &f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0");
    assert_int_equal(rc, 0);

    cbx_assignments a = load_assignments();
    assert_int_equal(a.gamepad_order_count, 1);
    assert_string_equal(a.gamepad_order[0], "ORDER:0");
}

static void
test_save_bt_identity(void **state)
{
    gamepad_fixture *f = FIX(state);

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    expect_evdev_source(f, "ab:cd:01:ef:23:45", "", "5");

    int rc = ip_gamepad_order_save(f->backend, f->mock.bus, &f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0");
    assert_int_equal(rc, 0);

    cbx_assignments a = load_assignments();
    assert_int_equal(a.gamepad_order_count, 1);
    assert_string_equal(a.gamepad_order[0], "BT:AB:CD:01:EF:23:45");
}

/* --- Load tests ---------------------------------------------------------- */

static void
test_load_no_file(void **state)
{
    gamepad_fixture *f = FIX(state);
    (void)f;

    char *csv = NULL;
    int rc = ip_gamepad_order_load(&csv);
    assert_int_equal(rc, 0);
    assert_non_null(csv);
    assert_string_equal(csv, "");
    free(csv);
}

static void
test_load_with_order(void **state)
{
    gamepad_fixture *f = FIX(state);

    write_raw_gamepad_order(f->temp_home,
        "gamepad_order:\n  - USB:serial-aaaa\n  - BT:11:22:33:44:55:66\n");

    char *csv = NULL;
    int rc = ip_gamepad_order_load(&csv);
    assert_int_equal(rc, 0);
    assert_non_null(csv);
    assert_string_equal(csv, "USB:serial-aaaa,BT:11:22:33:44:55:66");
    free(csv);
}

static void
test_load_empty_order(void **state)
{
    gamepad_fixture *f = FIX(state);

    write_raw_gamepad_order(f->temp_home,
        "assignments: []\ngamepad_order: []\n");

    char *csv = NULL;
    int rc = ip_gamepad_order_load(&csv);
    assert_int_equal(rc, 0);
    assert_non_null(csv);
    assert_string_equal(csv, "");
    free(csv);
}

static void
test_load_invalid_id_rejected(void **state)
{
    gamepad_fixture *f = FIX(state);

    /* assignments.yaml is validated as a whole during load (Task 21): an
     * invalid ID makes the file malformed, so the load is rejected and no
     * partial order is published. */
    write_raw_gamepad_order(f->temp_home,
        "gamepad_order:\n  - USB:serial-aaaa\n  - INVALID_ID_FORMAT\n");

    char *csv = NULL;
    int rc = ip_gamepad_order_load(&csv);
    assert_int_equal(rc, -EINVAL);
    assert_null(csv);
}

static void
test_load_null_arg(void **state)
{
    (void)state;
    int rc = ip_gamepad_order_load(NULL);
    assert_int_equal(rc, -EINVAL);
}

/* --- Round-trip test ----------------------------------------------------- */

static void
test_round_trip(void **state)
{
    gamepad_fixture *f = FIX(state);

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    expect_evdev_source(f, "serial-aaaa", "", "3");

    int rc = ip_gamepad_order_save(f->backend, f->mock.bus, &f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0");
    assert_int_equal(rc, 0);

    char *csv = NULL;
    rc = ip_gamepad_order_load(&csv);
    assert_int_equal(rc, 0);
    assert_non_null(csv);
    assert_string_equal(csv, "USB:serial-aaaa");
    free(csv);
}

static void
test_save_replaces_order(void **state)
{
    gamepad_fixture *f = FIX(state);

    /* First save. */
    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    expect_evdev_source(f, "serial-aaaa", "", "3");
    int rc = ip_gamepad_order_save(f->backend, f->mock.bus,
                                    &f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0");
    assert_int_equal(rc, 0);

    /* Second save — should replace, not append. */
    ip_dbus_mock_reset(&f->mock);
    expect_evdev_source(f, "ab:cd:01:ef:23:45", "", "5");
    rc = ip_gamepad_order_save(f->backend, f->mock.bus,
                               &f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0");
    assert_int_equal(rc, 0);

    cbx_assignments a = load_assignments();
    assert_int_equal(a.gamepad_order_count, 1);
    assert_string_equal(a.gamepad_order[0], "BT:AB:CD:01:EF:23:45");
}

/* --- Main ---------------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_save_success,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_save_empty_csv,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_save_stale_path_skipped,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_save_query_failure_skipped,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_save_preserves_assignments,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_save_null_args,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_save_order_id_when_no_stable_identity,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_save_bt_identity,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_load_no_file,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_load_with_order,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_load_empty_order,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_load_invalid_id_rejected,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_load_null_arg,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_round_trip,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_save_replaces_order,
                                         setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
