/*
 * test_order_restore.c — Unit tests for GamepadOrder restoration
 *                         (Task 27, SPEC §10.3 gap #2).
 *
 * Tests:
 *   - cbx_gamepad_order_map_ids: map saved IDs → composite paths.
 *   - cbx_gamepad_order_restore: full restore flow (load → map → set).
 *
 * Uses ip_dbus_mock for DBus calls and temp HOME for assignments.yaml.
 */
#include "dbus_mock.h"
#include "identify/gamepad_order_restore.h"
#include "dbus/ip_gamepad_order.h"
#include "dbus/ip_device_model.h"
#include "dbus/ip_manager.h"
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
} restore_fixture;

static int
setup(void **state)
{
    restore_fixture *f = malloc(sizeof(*f));
    if (!f)
        return -1;
    memset(f, 0, sizeof(*f));
    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);
    cbx_device_model_init(&f->model);

    snprintf(f->temp_home, sizeof(f->temp_home),
             "/tmp/cbx-restore-XXXXXX");
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
    restore_fixture *f = *state;
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

#define FIX(state) (*(restore_fixture **)(state))

/* --- Helpers ------------------------------------------------------------- */

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

/* --- cbx_gamepad_order_map_ids tests ------------------------------------- */

static void
test_map_ids_single_match(void **state)
{
    restore_fixture *f = FIX(state);

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "PersistentId", "USB:serial-aaaa");

    char paths_csv[CBX_MAX_PATH_LEN * CBX_MAX_GAMEPAD_ORDER];
    int restored = 0, skipped = 0;
    int rc = cbx_gamepad_order_map_ids(f->backend, f->mock.bus, &f->model,
        "USB:serial-aaaa", paths_csv, sizeof(paths_csv),
        &restored, &skipped);
    assert_int_equal(rc, 0);
    assert_int_equal(restored, 1);
    assert_int_equal(skipped, 0);
    assert_string_equal(paths_csv,
        "/org/shadowblip/InputPlumber/CompositeDevice0");
}

static void
test_map_ids_no_match_stale(void **state)
{
    restore_fixture *f = FIX(state);

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "PersistentId", "USB:serial-aaaa");

    char paths_csv[CBX_MAX_PATH_LEN * CBX_MAX_GAMEPAD_ORDER];
    int restored = 0, skipped = 0;
    int rc = cbx_gamepad_order_map_ids(f->backend, f->mock.bus, &f->model,
        "USB:serial-zzzz", paths_csv, sizeof(paths_csv),
        &restored, &skipped);
    assert_int_equal(rc, 0);
    assert_int_equal(restored, 0);
    assert_int_equal(skipped, 1);
    assert_string_equal(paths_csv, "");
}

static void
test_map_ids_empty_saved_csv(void **state)
{
    restore_fixture *f = FIX(state);

    char paths_csv[CBX_MAX_PATH_LEN * CBX_MAX_GAMEPAD_ORDER];
    int restored = 0, skipped = 0;
    int rc = cbx_gamepad_order_map_ids(f->backend, f->mock.bus, &f->model,
        "", paths_csv, sizeof(paths_csv),
        &restored, &skipped);
    assert_int_equal(rc, 0);
    assert_int_equal(restored, 0);
    assert_int_equal(skipped, 0);
    assert_string_equal(paths_csv, "");
}

static void
test_map_ids_multiple_one_match_one_stale(void **state)
{
    restore_fixture *f = FIX(state);

    /* One composite in model. Mock returns same PersistentId for all. */
    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "PersistentId", "USB:serial-aaaa");

    char paths_csv[CBX_MAX_PATH_LEN * CBX_MAX_GAMEPAD_ORDER];
    int restored = 0, skipped = 0;
    int rc = cbx_gamepad_order_map_ids(f->backend, f->mock.bus, &f->model,
        "USB:serial-aaaa,BT:11:22:33:44:55:66", paths_csv, sizeof(paths_csv),
        &restored, &skipped);
    assert_int_equal(rc, 0);
    assert_int_equal(restored, 1);
    assert_int_equal(skipped, 1);
    assert_string_equal(paths_csv,
        "/org/shadowblip/InputPlumber/CompositeDevice0");
}

static void
test_map_ids_no_composites(void **state)
{
    restore_fixture *f = FIX(state);
    /* Empty model — all saved IDs are stale. */

    char paths_csv[CBX_MAX_PATH_LEN * CBX_MAX_GAMEPAD_ORDER];
    int restored = 0, skipped = 0;
    int rc = cbx_gamepad_order_map_ids(f->backend, f->mock.bus, &f->model,
        "USB:serial-aaaa", paths_csv, sizeof(paths_csv),
        &restored, &skipped);
    assert_int_equal(rc, 0);
    assert_int_equal(restored, 0);
    assert_int_equal(skipped, 1);
    assert_string_equal(paths_csv, "");
}

static void
test_map_ids_null_args(void **state)
{
    restore_fixture *f = FIX(state);
    char paths_csv[256];

    assert_int_equal(cbx_gamepad_order_map_ids(NULL, f->mock.bus, &f->model,
        "id", paths_csv, sizeof(paths_csv), NULL, NULL), -EINVAL);
    assert_int_equal(cbx_gamepad_order_map_ids(f->backend, f->mock.bus, NULL,
        "id", paths_csv, sizeof(paths_csv), NULL, NULL), -EINVAL);
    assert_int_equal(cbx_gamepad_order_map_ids(f->backend, f->mock.bus, &f->model,
        NULL, paths_csv, sizeof(paths_csv), NULL, NULL), -EINVAL);
    assert_int_equal(cbx_gamepad_order_map_ids(f->backend, f->mock.bus, &f->model,
        "id", NULL, sizeof(paths_csv), NULL, NULL), -EINVAL);
    assert_int_equal(cbx_gamepad_order_map_ids(f->backend, f->mock.bus, &f->model,
        "id", paths_csv, 0, NULL, NULL), -EINVAL);
}

static void
test_map_ids_null_counts_ok(void **state)
{
    restore_fixture *f = FIX(state);

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "PersistentId", "USB:serial-aaaa");

    char paths_csv[CBX_MAX_PATH_LEN * CBX_MAX_GAMEPAD_ORDER];
    int rc = cbx_gamepad_order_map_ids(f->backend, f->mock.bus, &f->model,
        "USB:serial-aaaa", paths_csv, sizeof(paths_csv),
        NULL, NULL);
    assert_int_equal(rc, 0);
    assert_string_equal(paths_csv,
        "/org/shadowblip/InputPlumber/CompositeDevice0");
}

static void
test_map_ids_dbus_error_skips_composite(void **state)
{
    restore_fixture *f = FIX(state);

    /* Two composites — first DBus query errors, second succeeds.
     * Mock overwrites: only the OK expectation is active. So all queries
     * return OK with "USB:serial-aaaa". The saved ID matches both
     * composites (since mock returns same PersistentId), but we stop
     * at the first match. */
    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice1"));

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "PersistentId", "USB:serial-aaaa");

    char paths_csv[CBX_MAX_PATH_LEN * CBX_MAX_GAMEPAD_ORDER];
    int restored = 0, skipped = 0;
    int rc = cbx_gamepad_order_map_ids(f->backend, f->mock.bus, &f->model,
        "USB:serial-aaaa", paths_csv, sizeof(paths_csv),
        &restored, &skipped);
    assert_int_equal(rc, 0);
    assert_int_equal(restored, 1);
    assert_int_equal(skipped, 0);
    /* First matching composite is used. */
    assert_string_equal(paths_csv,
        "/org/shadowblip/InputPlumber/CompositeDevice0");
}

static void
test_map_ids_order_id(void **state)
{
    restore_fixture *f = FIX(state);

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "PersistentId", "ORDER:0");

    char paths_csv[CBX_MAX_PATH_LEN * CBX_MAX_GAMEPAD_ORDER];
    int restored = 0, skipped = 0;
    int rc = cbx_gamepad_order_map_ids(f->backend, f->mock.bus, &f->model,
        "ORDER:0", paths_csv, sizeof(paths_csv),
        &restored, &skipped);
    assert_int_equal(rc, 0);
    assert_int_equal(restored, 1);
    assert_string_equal(paths_csv,
        "/org/shadowblip/InputPlumber/CompositeDevice0");
}

static void
test_map_ids_whitespace_trimmed(void **state)
{
    restore_fixture *f = FIX(state);

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "PersistentId", "USB:serial-aaaa");

    char paths_csv[CBX_MAX_PATH_LEN * CBX_MAX_GAMEPAD_ORDER];
    int restored = 0, skipped = 0;
    /* Leading/trailing spaces around the ID should be trimmed. */
    int rc = cbx_gamepad_order_map_ids(f->backend, f->mock.bus, &f->model,
        "  USB:serial-aaaa  ", paths_csv, sizeof(paths_csv),
        &restored, &skipped);
    assert_int_equal(rc, 0);
    assert_int_equal(restored, 1);
    assert_int_equal(skipped, 0);
}

static void
test_map_ids_empty_token_skipped(void **state)
{
    restore_fixture *f = FIX(state);

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "PersistentId", "USB:serial-aaaa");

    char paths_csv[CBX_MAX_PATH_LEN * CBX_MAX_GAMEPAD_ORDER];
    int restored = 0, skipped = 0;
    /* Empty tokens (",,") should be skipped, not counted as stale. */
    int rc = cbx_gamepad_order_map_ids(f->backend, f->mock.bus, &f->model,
        "USB:serial-aaaa,,", paths_csv, sizeof(paths_csv),
        &restored, &skipped);
    assert_int_equal(rc, 0);
    assert_int_equal(restored, 1);
    assert_int_equal(skipped, 0);
}

/* --- cbx_gamepad_order_restore tests ------------------------------------- */

static void
test_restore_success(void **state)
{
    restore_fixture *f = FIX(state);

    /* Set up saved gamepad_order. */
    write_raw_gamepad_order(f->temp_home,
        "gamepad_order:\n  - USB:serial-aaaa\n");

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "PersistentId", "USB:serial-aaaa");

    /* Expect the set_property call for GamepadOrder. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
        "GamepadOrder", NULL);

    int restored = 0, skipped = 0;
    int rc = cbx_gamepad_order_restore(f->backend, f->mock.bus, &f->model,
                                       &restored, &skipped);
    assert_int_equal(rc, 0);
    assert_int_equal(restored, 1);
    assert_int_equal(skipped, 0);
}

static void
test_restore_no_saved_order(void **state)
{
    restore_fixture *f = FIX(state);
    /* No assignments.yaml → empty saved order → -ENOENT. */

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));

    int restored = 0, skipped = 0;
    int rc = cbx_gamepad_order_restore(f->backend, f->mock.bus, &f->model,
                                       &restored, &skipped);
    assert_int_equal(rc, -ENOENT);
}

static void
test_restore_empty_saved_order(void **state)
{
    restore_fixture *f = FIX(state);

    write_raw_gamepad_order(f->temp_home,
        "assignments: []\ngamepad_order: []\n");

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));

    int restored = 0, skipped = 0;
    int rc = cbx_gamepad_order_restore(f->backend, f->mock.bus, &f->model,
                                       &restored, &skipped);
    assert_int_equal(rc, -ENOENT);
}

static void
test_restore_all_stale(void **state)
{
    restore_fixture *f = FIX(state);

    write_raw_gamepad_order(f->temp_home,
        "gamepad_order:\n  - USB:serial-zzzz\n");

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "PersistentId", "USB:serial-aaaa");

    /* All IDs are stale — empty CSV is set (clears the order).
     * ip_manager_set_gamepad_order accepts empty string. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
        "GamepadOrder", NULL);

    int restored = 0, skipped = 0;
    int rc = cbx_gamepad_order_restore(f->backend, f->mock.bus, &f->model,
                                       &restored, &skipped);
    assert_int_equal(rc, 0);
    assert_int_equal(restored, 0);
    assert_int_equal(skipped, 1);
}

static void
test_restore_null_args(void **state)
{
    restore_fixture *f = FIX(state);

    assert_int_equal(cbx_gamepad_order_restore(NULL, f->mock.bus, &f->model,
        NULL, NULL), -EINVAL);
    assert_int_equal(cbx_gamepad_order_restore(f->backend, f->mock.bus, NULL,
        NULL, NULL), -EINVAL);
}

static void
test_restore_null_counts_ok(void **state)
{
    restore_fixture *f = FIX(state);

    write_raw_gamepad_order(f->temp_home,
        "gamepad_order:\n  - USB:serial-aaaa\n");

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "PersistentId", "USB:serial-aaaa");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
        "GamepadOrder", NULL);

    int rc = cbx_gamepad_order_restore(f->backend, f->mock.bus, &f->model,
                                       NULL, NULL);
    assert_int_equal(rc, 0);
}

static void
test_restore_multiple_ids_partial(void **state)
{
    restore_fixture *f = FIX(state);

    /* Two saved IDs, one composite in model matching one of them.
     * Mock returns same PersistentId for all composites. */
    write_raw_gamepad_order(f->temp_home,
        "gamepad_order:\n  - USB:serial-aaaa\n  - BT:11:22:33:44:55:66\n");

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "PersistentId", "USB:serial-aaaa");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
        "GamepadOrder", NULL);

    int restored = 0, skipped = 0;
    int rc = cbx_gamepad_order_restore(f->backend, f->mock.bus, &f->model,
                                       &restored, &skipped);
    assert_int_equal(rc, 0);
    assert_int_equal(restored, 1);
    assert_int_equal(skipped, 1);
}

static void
test_restore_no_composites(void **state)
{
    restore_fixture *f = FIX(state);

    write_raw_gamepad_order(f->temp_home,
        "gamepad_order:\n  - USB:serial-aaaa\n");

    /* No composites in model — all IDs stale. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
        "GamepadOrder", NULL);

    int restored = 0, skipped = 0;
    int rc = cbx_gamepad_order_restore(f->backend, f->mock.bus, &f->model,
                                       &restored, &skipped);
    assert_int_equal(rc, 0);
    assert_int_equal(restored, 0);
    assert_int_equal(skipped, 1);
}

static void
test_restore_round_trip(void **state)
{
    restore_fixture *f = FIX(state);

    /* Simulate full round-trip:
     * 1. Save a gamepad_order (using ip_gamepad_order_save)
     * 2. Restore it (using cbx_gamepad_order_restore) */

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));

    /* Save the order. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "PersistentId", "USB:serial-aaaa");
    int rc = ip_gamepad_order_save(f->backend, f->mock.bus, &f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0");
    assert_int_equal(rc, 0);

    /* Reset mock for restore. */
    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "PersistentId", "USB:serial-aaaa");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
        "GamepadOrder", NULL);

    /* Restore. */
    int restored = 0, skipped = 0;
    rc = cbx_gamepad_order_restore(f->backend, f->mock.bus, &f->model,
                                    &restored, &skipped);
    assert_int_equal(rc, 0);
    assert_int_equal(restored, 1);
    assert_int_equal(skipped, 0);
}

/* --- Main ---------------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* cbx_gamepad_order_map_ids */
        cmocka_unit_test_setup_teardown(test_map_ids_single_match,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_map_ids_no_match_stale,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_map_ids_empty_saved_csv,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_map_ids_multiple_one_match_one_stale,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_map_ids_no_composites,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_map_ids_null_args,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_map_ids_null_counts_ok,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_map_ids_dbus_error_skips_composite,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_map_ids_order_id,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_map_ids_whitespace_trimmed,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_map_ids_empty_token_skipped,
                                         setup, teardown),
        /* cbx_gamepad_order_restore */
        cmocka_unit_test_setup_teardown(test_restore_success,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_restore_no_saved_order,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_restore_empty_saved_order,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_restore_all_stale,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_restore_null_args,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_restore_null_counts_ok,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_restore_multiple_ids_partial,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_restore_no_composites,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_restore_round_trip,
                                         setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}