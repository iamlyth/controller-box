/*
 * test_gamepad_order.c — Unit tests for GamepadOrder persistence (Task 15, gap #2).
 *
 * Tests:
 *   - Save: composite paths CSV → PersistentId query → assignments.yaml.
 *   - Save with stale paths (skipped).
 *   - Save empty CSV (clears order).
 *   - Load: assignments.yaml → CSV of IDs.
 *   - Load with no saved order.
 *   - Load with invalid IDs (skipped).
 *   - Round-trip: save then load.
 *   - NULL args.
 *   - DBus error during PersistentId query (skip entry).
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

/* --- Save tests ---------------------------------------------------------- */

static void
test_save_success(void **state)
{
    gamepad_fixture *f = FIX(state);

    /* Add one composite to the model.  (The mock matches by iface+member
     * only, so it returns the same value for every PersistentId query.
     * We test one composite here; multiple-composite paths are tested in
     * test_save_stale_path_skipped which has one valid + one stale.) */
    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "PersistentId", "USB:serial-aaaa");

    const char *paths_csv =
        "/org/shadowblip/InputPlumber/CompositeDevice0";

    int rc = ip_gamepad_order_save(f->backend, f->mock.bus,
                                    &f->model, paths_csv);
    assert_int_equal(rc, 0);

    /* Verify the saved file has the right gamepad_order. */
    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assignments_load(&a), 0);
    assert_int_equal(a.gamepad_order_count, 1);
    assert_string_equal(a.gamepad_order[0], "USB:serial-aaaa");
}

static void
test_save_empty_csv(void **state)
{
    gamepad_fixture *f = FIX(state);

    int rc = ip_gamepad_order_save(f->backend, f->mock.bus,
                                    &f->model, "");
    assert_int_equal(rc, 0);

    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assignments_load(&a), 0);
    assert_int_equal(a.gamepad_order_count, 0);
}

static void
test_save_stale_path_skipped(void **state)
{
    gamepad_fixture *f = FIX(state);

    /* Add one composite to the model. */
    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));

    /* Expect PersistentId for the valid composite only. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "PersistentId", "USB:serial-aaaa");

    /* The second path is stale (not in model) — should be skipped. */
    const char *paths_csv =
        "/org/shadowblip/InputPlumber/CompositeDevice0,"
        "/org/shadowblip/InputPlumber/CompositeDevice99";

    int rc = ip_gamepad_order_save(f->backend, f->mock.bus,
                                    &f->model, paths_csv);
    assert_int_equal(rc, 0);

    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assignments_load(&a), 0);
    assert_int_equal(a.gamepad_order_count, 1);
    assert_string_equal(a.gamepad_order[0], "USB:serial-aaaa");
}

static void
test_save_dbus_error_skipped(void **state)
{
    gamepad_fixture *f = FIX(state);

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice1"));

    /* First PersistentId query fails, second succeeds. */
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
        "PersistentId", -EIO);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "PersistentId", "USB:serial-bbbb");

    const char *paths_csv =
        "/org/shadowblip/InputPlumber/CompositeDevice0,"
        "/org/shadowblip/InputPlumber/CompositeDevice1";

    int rc = ip_gamepad_order_save(f->backend, f->mock.bus,
                                    &f->model, paths_csv);
    assert_int_equal(rc, 0);

    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assignments_load(&a), 0);
    /* Only the second composite's ID should be saved. */
    assert_int_equal(a.gamepad_order_count, 1);
    assert_string_equal(a.gamepad_order[0], "USB:serial-bbbb");
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
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "PersistentId", "USB:serial-aaaa");

    int rc = ip_gamepad_order_save(f->backend, f->mock.bus,
                                    &f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0");
    assert_int_equal(rc, 0);

    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assignments_load(&a), 0);
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
test_save_order_id(void **state)
{
    gamepad_fixture *f = FIX(state);

    /* Test ORDER:n ID format. */
    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "PersistentId", "ORDER:0");

    int rc = ip_gamepad_order_save(f->backend, f->mock.bus,
                                    &f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0");
    assert_int_equal(rc, 0);

    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assignments_load(&a), 0);
    assert_int_equal(a.gamepad_order_count, 1);
    assert_string_equal(a.gamepad_order[0], "ORDER:0");
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
test_load_invalid_id_skipped(void **state)
{
    gamepad_fixture *f = FIX(state);

    /* Write an assignments.yaml with one valid and one invalid ID. */
    write_raw_gamepad_order(f->temp_home,
        "gamepad_order:\n  - USB:serial-aaaa\n  - INVALID_ID_FORMAT\n");

    char *csv = NULL;
    int rc = ip_gamepad_order_load(&csv);
    assert_int_equal(rc, 0);
    assert_non_null(csv);
    /* Invalid ID should be skipped. */
    assert_string_equal(csv, "USB:serial-aaaa");
    free(csv);
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

    /* Use one composite (mock returns same PersistentId for all calls). */
    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "PersistentId", "USB:serial-aaaa");

    const char *paths_csv =
        "/org/shadowblip/InputPlumber/CompositeDevice0";

    int rc = ip_gamepad_order_save(f->backend, f->mock.bus,
                                    &f->model, paths_csv);
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
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "PersistentId", "USB:serial-aaaa");
    int rc = ip_gamepad_order_save(f->backend, f->mock.bus,
                                    &f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0");
    assert_int_equal(rc, 0);

    /* Second save — should replace, not append. */
    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "PersistentId", "BT:11:22:33:44:55:66");
    rc = ip_gamepad_order_save(f->backend, f->mock.bus,
                               &f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0");
    assert_int_equal(rc, 0);

    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assignments_load(&a), 0);
    /* Should have exactly 1 entry, not 2. */
    assert_int_equal(a.gamepad_order_count, 1);
    assert_string_equal(a.gamepad_order[0], "BT:11:22:33:44:55:66");
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
        cmocka_unit_test_setup_teardown(test_save_dbus_error_skipped,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_save_preserves_assignments,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_save_null_args,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_save_order_id,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_load_no_file,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_load_with_order,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_load_empty_order,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_load_invalid_id_skipped,
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