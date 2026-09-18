/*
 * test_order_restore.c — Unit tests for GamepadOrder restoration
 *                         (Task 27, SPEC §10.3 gap #2; task 6 identity).
 *
 * Tests:
 *   - cbx_gamepad_order_map_ids: map saved IDs → composite paths by
 *     source-derived physical identity (not PersistentId).
 *   - cbx_gamepad_order_restore: full restore flow (load → map → set),
 *     including the transient-query-failure deferral.
 *
 * Uses ip_dbus_mock for DBus calls and temp HOME for assignments.yaml.
 * The mock is keyed by (interface, member) only, so each test uses a single
 * composite identity (multi-composite identity behavior is covered by the
 * native test_manager_native / test_overlay_native source-device cases).
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

/* Configure the mock so every composite reports this single evdev source. */
static void
expect_evdev_source(restore_fixture *f, const char *unique_id,
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

/* An empty (but successfully read) source list yields the ORDER fallback. */
static void
expect_no_sources(restore_fixture *f)
{
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "SourceDevicePaths", "");
}

/* The snapshot mapper is shared by production grid/order restoration. */
static void
test_snapshot_reversed_order_and_disconnected_preference(void **state)
{
    (void)state;
    cbx_composite_identity_entry entries[2] = {
        {.path = "/composite9", .ident = {.id = "USB:serial-b",
            .layer = CBX_IDENTITY_LAYER_USB_SERIAL}},
        {.path = "/composite4", .ident = {.id = "USB:serial-a",
            .layer = CBX_IDENTITY_LAYER_USB_SERIAL}},
    };
    char paths[128];
    int restored, skipped;
    bool failed;
    assert_int_equal(cbx_gamepad_order_map_snapshot(entries, 2,
        "USB:serial-a,USB:disconnected,USB:serial-b", paths, sizeof(paths),
        &restored, &skipped, &failed), 0);
    assert_false(failed);
    assert_int_equal(restored, 2);
    assert_int_equal(skipped, 1);
    assert_string_equal(paths, "/composite4,/composite9");
}

static void
test_snapshot_unique_match_with_failed_peer_is_uncertain(void **state)
{
    (void)state;
    cbx_composite_identity_entry entries[2] = {
        {.path = "/composite0", .ident = {.id = "USB:serial-a",
            .layer = CBX_IDENTITY_LAYER_USB_SERIAL}},
        {.path = "/composite1", .status = CBX_COMPOSITE_IDENTITY_QUERY_FAILED},
    };
    char paths[128];
    int restored, skipped;
    bool failed;
    assert_int_equal(cbx_gamepad_order_map_snapshot(entries, 2,
        "USB:serial-a", paths, sizeof(paths), &restored, &skipped, &failed), 0);
    assert_true(failed);
    assert_int_equal(restored, 0);
    assert_int_equal(skipped, 0);
    assert_string_equal(paths, "");
}

static void
test_snapshot_duplicate_identity_is_uncertain(void **state)
{
    (void)state;
    cbx_composite_identity_entry entries[2] = {
        {.path = "/composite0", .ident = {.id = "USB:serial-a",
            .layer = CBX_IDENTITY_LAYER_USB_SERIAL}},
        {.path = "/composite1", .ident = {.id = "USB:serial-a",
            .layer = CBX_IDENTITY_LAYER_USB_SERIAL}},
    };
    char paths[128];
    bool failed;
    int restored;
    assert_int_equal(cbx_gamepad_order_map_snapshot(entries, 2,
        "USB:serial-a", paths, sizeof(paths), &restored, NULL, &failed), 0);
    assert_true(failed);
    assert_int_equal(restored, 0);
    assert_string_equal(paths, "");
}

/* --- cbx_gamepad_order_map_ids tests ------------------------------------- */

static void
test_map_ids_single_match(void **state)
{
    restore_fixture *f = FIX(state);

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    expect_evdev_source(f, "serial-aaaa", "", "3");

    char paths_csv[CBX_MAX_PATH_LEN * CBX_MAX_GAMEPAD_ORDER];
    int restored = 0, skipped = 0;
    bool failed = false;
    int rc = cbx_gamepad_order_map_ids(f->backend, f->mock.bus, &f->model,
        "USB:serial-aaaa", paths_csv, sizeof(paths_csv),
        &restored, &skipped, &failed);
    assert_int_equal(rc, 0);
    assert_int_equal(restored, 1);
    assert_int_equal(skipped, 0);
    assert_false(failed);
    assert_string_equal(paths_csv,
        "/org/shadowblip/InputPlumber/CompositeDevice0");
}

static void
test_map_ids_no_match_stale(void **state)
{
    restore_fixture *f = FIX(state);

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    expect_evdev_source(f, "serial-aaaa", "", "3");

    char paths_csv[CBX_MAX_PATH_LEN * CBX_MAX_GAMEPAD_ORDER];
    int restored = 0, skipped = 0;
    bool failed = false;
    int rc = cbx_gamepad_order_map_ids(f->backend, f->mock.bus, &f->model,
        "USB:serial-zzzz", paths_csv, sizeof(paths_csv),
        &restored, &skipped, &failed);
    assert_int_equal(rc, 0);
    assert_int_equal(restored, 0);
    assert_int_equal(skipped, 1);
    assert_false(failed);
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
        &restored, &skipped, NULL);
    assert_int_equal(rc, 0);
    assert_int_equal(restored, 0);
    assert_int_equal(skipped, 0);
    assert_string_equal(paths_csv, "");
}

static void
test_map_ids_multiple_one_match_one_stale(void **state)
{
    restore_fixture *f = FIX(state);

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    expect_evdev_source(f, "serial-aaaa", "", "3");

    char paths_csv[CBX_MAX_PATH_LEN * CBX_MAX_GAMEPAD_ORDER];
    int restored = 0, skipped = 0;
    bool failed = false;
    int rc = cbx_gamepad_order_map_ids(f->backend, f->mock.bus, &f->model,
        "USB:serial-aaaa,BT:11:22:33:44:55:66", paths_csv, sizeof(paths_csv),
        &restored, &skipped, &failed);
    assert_int_equal(rc, 0);
    assert_int_equal(restored, 1);
    assert_int_equal(skipped, 1);
    assert_false(failed);
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
        &restored, &skipped, NULL);
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
        "id", paths_csv, sizeof(paths_csv), NULL, NULL, NULL), -EINVAL);
    assert_int_equal(cbx_gamepad_order_map_ids(f->backend, f->mock.bus, NULL,
        "id", paths_csv, sizeof(paths_csv), NULL, NULL, NULL), -EINVAL);
    assert_int_equal(cbx_gamepad_order_map_ids(f->backend, f->mock.bus, &f->model,
        NULL, paths_csv, sizeof(paths_csv), NULL, NULL, NULL), -EINVAL);
    assert_int_equal(cbx_gamepad_order_map_ids(f->backend, f->mock.bus, &f->model,
        "id", NULL, sizeof(paths_csv), NULL, NULL, NULL), -EINVAL);
    assert_int_equal(cbx_gamepad_order_map_ids(f->backend, f->mock.bus, &f->model,
        "id", paths_csv, 0, NULL, NULL, NULL), -EINVAL);
}

static void
test_map_ids_null_counts_ok(void **state)
{
    restore_fixture *f = FIX(state);

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    expect_evdev_source(f, "serial-aaaa", "", "3");

    char paths_csv[CBX_MAX_PATH_LEN * CBX_MAX_GAMEPAD_ORDER];
    int rc = cbx_gamepad_order_map_ids(f->backend, f->mock.bus, &f->model,
        "USB:serial-aaaa", paths_csv, sizeof(paths_csv),
        NULL, NULL, NULL);
    assert_int_equal(rc, 0);
    assert_string_equal(paths_csv,
        "/org/shadowblip/InputPlumber/CompositeDevice0");
}

/*
 * A transient SourceDevicePaths read failure is not absence: the ID is not
 * counted as stale and query_failed is reported so the caller can defer.
 */
static void
test_map_ids_query_failure_reports_uncertain(void **state)
{
    restore_fixture *f = FIX(state);

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                              "SourceDevicePaths", -EIO);

    char paths_csv[CBX_MAX_PATH_LEN * CBX_MAX_GAMEPAD_ORDER];
    int restored = 0, skipped = 0;
    bool failed = false;
    int rc = cbx_gamepad_order_map_ids(f->backend, f->mock.bus, &f->model,
        "USB:serial-aaaa", paths_csv, sizeof(paths_csv),
        &restored, &skipped, &failed);
    assert_int_equal(rc, 0);
    assert_int_equal(restored, 0);
    assert_int_equal(skipped, 0);
    assert_true(failed);
    assert_string_equal(paths_csv, "");
}

/*
 * The source list reads but every per-source property read fails: an ORDER:n
 * fallback is produced for display, yet it must not be matched to a saved
 * ORDER:n preference (that would reroute a different weak controller).
 */
static void
test_map_ids_per_source_read_failure_uncertain(void **state)
{
    restore_fixture *f = FIX(state);

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "SourceDevicePaths",
                           "/org/shadowblip/InputPlumber/devices/source/event0");

    char paths_csv[CBX_MAX_PATH_LEN * CBX_MAX_GAMEPAD_ORDER];
    int restored = 0, skipped = 0;
    bool failed = false;
    int rc = cbx_gamepad_order_map_ids(f->backend, f->mock.bus, &f->model,
        "ORDER:0", paths_csv, sizeof(paths_csv), &restored, &skipped,
        &failed);
    assert_int_equal(rc, 0);
    assert_int_equal(restored, 0);
    assert_int_equal(skipped, 0);
    assert_true(failed);
    assert_string_equal(paths_csv, "");
}

static void
test_map_ids_order_id(void **state)
{
    restore_fixture *f = FIX(state);

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    expect_no_sources(f);

    char paths_csv[CBX_MAX_PATH_LEN * CBX_MAX_GAMEPAD_ORDER];
    int restored = 0, skipped = 0;
    int rc = cbx_gamepad_order_map_ids(f->backend, f->mock.bus, &f->model,
        "ORDER:0", paths_csv, sizeof(paths_csv),
        &restored, &skipped, NULL);
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
    expect_evdev_source(f, "serial-aaaa", "", "3");

    char paths_csv[CBX_MAX_PATH_LEN * CBX_MAX_GAMEPAD_ORDER];
    int restored = 0, skipped = 0;
    /* Leading/trailing spaces around the ID should be trimmed. */
    int rc = cbx_gamepad_order_map_ids(f->backend, f->mock.bus, &f->model,
        "  USB:serial-aaaa  ", paths_csv, sizeof(paths_csv),
        &restored, &skipped, NULL);
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
    expect_evdev_source(f, "serial-aaaa", "", "3");

    char paths_csv[CBX_MAX_PATH_LEN * CBX_MAX_GAMEPAD_ORDER];
    int restored = 0, skipped = 0;
    /* Empty tokens (",,") should be skipped, not counted as stale. */
    int rc = cbx_gamepad_order_map_ids(f->backend, f->mock.bus, &f->model,
        "USB:serial-aaaa,,", paths_csv, sizeof(paths_csv),
        &restored, &skipped, NULL);
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
    expect_evdev_source(f, "serial-aaaa", "", "3");

    /* Expect the set_property call for GamepadOrder. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
        "GamepadOrder", NULL);

    int restored = 0, skipped = 0;
    bool failed = false;
    int rc = cbx_gamepad_order_restore(f->backend, f->mock.bus, &f->model,
                                       &restored, &skipped, &failed);
    assert_int_equal(rc, 0);
    assert_int_equal(restored, 1);
    assert_int_equal(skipped, 0);
    assert_false(failed);
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
                                       &restored, &skipped, NULL);
    assert_int_equal(rc, -ENOENT);
}

static void
test_restore_empty_saved_order(void **state)
{
    restore_fixture *f = FIX(state);

    write_raw_gamepad_order(f->temp_home,
        "assignments: []\ngamepad_order: []\n");

    int restored = 0, skipped = 0;
    int rc = cbx_gamepad_order_restore(f->backend, f->mock.bus, &f->model,
                                       &restored, &skipped, NULL);
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
    expect_evdev_source(f, "serial-aaaa", "", "3");

    /* All IDs are stale — empty CSV is set (clears the order).
     * ip_manager_set_gamepad_order accepts empty string. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
        "GamepadOrder", NULL);

    int restored = 0, skipped = 0;
    int rc = cbx_gamepad_order_restore(f->backend, f->mock.bus, &f->model,
                                       &restored, &skipped, NULL);
    assert_int_equal(rc, 0);
    assert_int_equal(restored, 0);
    assert_int_equal(skipped, 1);
}

/*
 * Transient query failure: the saved order must be left untouched and no
 * misleading (empty/partial) order applied.  The setter must not be called.
 */
static void
test_restore_query_failure_defers(void **state)
{
    restore_fixture *f = FIX(state);

    write_raw_gamepad_order(f->temp_home,
        "gamepad_order:\n  - USB:serial-aaaa\n");

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                              "SourceDevicePaths", -EIO);

    int restored = 0, skipped = 0;
    bool failed = false;
    int rc = cbx_gamepad_order_restore(f->backend, f->mock.bus, &f->model,
                                       &restored, &skipped, &failed);
    assert_int_equal(rc, -EAGAIN);
    assert_true(failed);
    /* No GamepadOrder write happened. */
    assert_int_equal(f->mock.set_property_count, 0);

    /* The persisted order is still intact. */
    char *saved = NULL;
    assert_int_equal(ip_gamepad_order_load(&saved), 0);
    assert_string_equal(saved, "USB:serial-aaaa");
    free(saved);
}

static void
test_restore_null_args(void **state)
{
    restore_fixture *f = FIX(state);

    assert_int_equal(cbx_gamepad_order_restore(NULL, f->mock.bus, &f->model,
        NULL, NULL, NULL), -EINVAL);
    assert_int_equal(cbx_gamepad_order_restore(f->backend, f->mock.bus, NULL,
        NULL, NULL, NULL), -EINVAL);
}

static void
test_restore_null_counts_ok(void **state)
{
    restore_fixture *f = FIX(state);

    write_raw_gamepad_order(f->temp_home,
        "gamepad_order:\n  - USB:serial-aaaa\n");

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    expect_evdev_source(f, "serial-aaaa", "", "3");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
        "GamepadOrder", NULL);

    int rc = cbx_gamepad_order_restore(f->backend, f->mock.bus, &f->model,
                                       NULL, NULL, NULL);
    assert_int_equal(rc, 0);
}

static void
test_restore_multiple_ids_partial(void **state)
{
    restore_fixture *f = FIX(state);

    /* Two saved IDs, one composite in model matching one of them. */
    write_raw_gamepad_order(f->temp_home,
        "gamepad_order:\n  - USB:serial-aaaa\n  - BT:11:22:33:44:55:66\n");

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    expect_evdev_source(f, "serial-aaaa", "", "3");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
        "GamepadOrder", NULL);

    int restored = 0, skipped = 0;
    int rc = cbx_gamepad_order_restore(f->backend, f->mock.bus, &f->model,
                                       &restored, &skipped, NULL);
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
                                       &restored, &skipped, NULL);
    assert_int_equal(rc, 0);
    assert_int_equal(restored, 0);
    assert_int_equal(skipped, 1);
}

static void
test_restore_bt_identity(void **state)
{
    restore_fixture *f = FIX(state);

    /* A Bluetooth MAC identity (layer 1) must resolve too. */
    write_raw_gamepad_order(f->temp_home,
        "gamepad_order:\n  - BT:AB:CD:01:EF:23:45\n");

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    expect_evdev_source(f, "ab:cd:01:ef:23:45", "", "5");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
        "GamepadOrder", NULL);

    int restored = 0, skipped = 0;
    int rc = cbx_gamepad_order_restore(f->backend, f->mock.bus, &f->model,
                                       &restored, &skipped, NULL);
    assert_int_equal(rc, 0);
    assert_int_equal(restored, 1);
    assert_int_equal(skipped, 0);
}

static void
test_restore_phys_identity(void **state)
{
    restore_fixture *f = FIX(state);

    write_raw_gamepad_order(f->temp_home,
        "gamepad_order:\n  - USB:phys:usb-3-2\n");

    assert_true(cbx_device_model_add_composite(&f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    expect_evdev_source(f, "", "usb-3-2", "3");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
        "GamepadOrder", NULL);

    int restored = 0, skipped = 0;
    int rc = cbx_gamepad_order_restore(f->backend, f->mock.bus, &f->model,
                                       &restored, &skipped, NULL);
    assert_int_equal(rc, 0);
    assert_int_equal(restored, 1);
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
    expect_evdev_source(f, "serial-aaaa", "", "3");
    int rc = ip_gamepad_order_save(f->backend, f->mock.bus, &f->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0");
    assert_int_equal(rc, 0);

    /* Reset mock for restore. */
    ip_dbus_mock_reset(&f->mock);
    expect_evdev_source(f, "serial-aaaa", "", "3");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
        "GamepadOrder", NULL);

    /* Restore. */
    int restored = 0, skipped = 0;
    rc = cbx_gamepad_order_restore(f->backend, f->mock.bus, &f->model,
                                    &restored, &skipped, NULL);
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
        cmocka_unit_test(test_snapshot_reversed_order_and_disconnected_preference),
        cmocka_unit_test(test_snapshot_unique_match_with_failed_peer_is_uncertain),
        cmocka_unit_test(test_snapshot_duplicate_identity_is_uncertain),
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
        cmocka_unit_test_setup_teardown(test_map_ids_query_failure_reports_uncertain,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_map_ids_per_source_read_failure_uncertain,
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
        cmocka_unit_test_setup_teardown(test_restore_query_failure_defers,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_restore_null_args,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_restore_null_counts_ok,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_restore_multiple_ids_partial,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_restore_no_composites,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_restore_bt_identity,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_restore_phys_identity,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_restore_round_trip,
                                         setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
