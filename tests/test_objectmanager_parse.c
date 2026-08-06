/*
 * test_objectmanager_parse.c — Unit tests for ObjectManager enumeration
 * (Task 10).
 *
 * Tests the GetManagedObjects reply parser and the enumerate function
 * (via mock backend).  Uses captured DBus reply fixtures in the text
 * format: "path\tiface1,iface2,…" one line per managed object.
 */
#include "dbus_mock.h"
#include "dbus/ip_connection.h"
#include "dbus/ip_device_model.h"
#include "dbus/ip_objectmanager.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

/* --- Test fixtures ------------------------------------------------------- */

/* Full object tree mirroring SPEC §10.1. */
static const char *FIXTURE_FULL =
    "# GetManagedObjects reply\n"
    "/org/shadowblip/InputPlumber/Manager\t"
        "org.shadowblip.InputManager\n"
    "/org/shadowblip/InputPlumber/CompositeDevice0\t"
        "org.shadowblip.Input.CompositeDevice\n"
    "/org/shadowblip/InputPlumber/CompositeDevice1\t"
        "org.shadowblip.Input.CompositeDevice\n"
    "/org/shadowblip/InputPlumber/devices/source/event0\t"
        "org.shadowblip.Input.Source.EventDevice\n"
    "/org/shadowblip/InputPlumber/devices/source/hidraw0\t"
        "org.shadowblip.Input.Source.HIDRawDevice\n"
    "/org/shadowblip/InputPlumber/devices/target/gamepad0\t"
        "org.shadowblip.Input.Target,org.shadowblip.Input.Gamepad\n"
    "/org/shadowblip/InputPlumber/devices/target/keyboard0\t"
        "org.shadowblip.Input.Target,org.shadowblip.Input.Keyboard\n";

static const char *FIXTURE_EMPTY = "";

static const char *FIXTURE_MANAGER_ONLY =
    "/org/shadowblip/InputPlumber/Manager\t"
        "org.shadowblip.InputManager\n";

static const char *FIXTURE_COMPOSITES_ONLY =
    "/org/shadowblip/InputPlumber/CompositeDevice0\t"
        "org.shadowblip.Input.CompositeDevice\n"
    "/org/shadowblip/InputPlumber/CompositeDevice3\t"
        "org.shadowblip.Input.CompositeDevice\n"
    "/org/shadowblip/InputPlumber/CompositeDevice12\t"
        "org.shadowblip.Input.CompositeDevice\n";

static const char *FIXTURE_SOURCES_ONLY =
    "/org/shadowblip/InputPlumber/devices/source/event0\t"
        "org.shadowblip.Input.Source.EventDevice\n"
    "/org/shadowblip/InputPlumber/devices/source/event1\t"
        "org.shadowblip.Input.Source.EventDevice\n"
    "/org/shadowblip/InputPlumber/devices/source/hidraw0\t"
        "org.shadowblip.Input.Source.HIDRawDevice\n";

static const char *FIXTURE_TARGETS_ONLY =
    "/org/shadowblip/InputPlumber/devices/target/gamepad0\t"
        "org.shadowblip.Input.Target,org.shadowblip.Input.Gamepad\n"
    "/org/shadowblip/InputPlumber/devices/target/mouse0\t"
        "org.shadowblip.Input.Target,org.shadowblip.Input.Mouse\n";

static const char *FIXTURE_INVALID_PATHS =
    "/org/shadowblip/InputPlumber/Manager\t"
        "org.shadowblip.InputManager\n"
    "/org/evil/CompositeDevice0\t"
        "org.shadowblip.Input.CompositeDevice\n"
    "/tmp/fake/device\t"
        "org.shadowblip.Input.Target\n"
    "/org/shadowblip/InputPlumber/devices/source/event0\t"
        "org.shadowblip.Input.Source.EventDevice\n";

static const char *FIXTURE_ALL_INVALID =
    "/org/evil/path\torg.shadowblip.InputManager\n"
    "/tmp/fake\torg.shadowblip.Input.Target\n";

static const char *FIXTURE_MALFORMED =
    "no_tab_line\n"
    "/org/shadowblip/InputPlumber/Manager\t"
        "org.shadowblip.InputManager\n"
    "also_no_tab\n";

static const char *FIXTURE_CRLF =
    "/org/shadowblip/InputPlumber/Manager\t"
        "org.shadowblip.InputManager\r\n"
    "/org/shadowblip/InputPlumber/CompositeDevice0\t"
        "org.shadowblip.Input.CompositeDevice\r\n";

/* --- Parser tests (no backend) ------------------------------------------ */

static void
test_parse_null_reply(void **state)
{
    (void)state;
    cbx_device_model model;
    int rc = cbx_objectmanager_parse_reply(NULL, &model);
    assert_int_equal(rc, 0);
    assert_false(model.has_manager);
    assert_int_equal(model.composite_count, 0);
    assert_int_equal(model.source_count, 0);
    assert_int_equal(model.target_count, 0);
}

static void
test_parse_empty_reply(void **state)
{
    (void)state;
    cbx_device_model model;
    int rc = cbx_objectmanager_parse_reply(FIXTURE_EMPTY, &model);
    assert_int_equal(rc, 0);
    assert_false(model.has_manager);
    assert_int_equal(model.composite_count, 0);
    assert_int_equal(model.source_count, 0);
    assert_int_equal(model.target_count, 0);
}

static void
test_parse_null_model(void **state)
{
    (void)state;
    int rc = cbx_objectmanager_parse_reply(FIXTURE_FULL, NULL);
    assert_int_equal(rc, -EINVAL);
}

static void
test_parse_manager_only(void **state)
{
    (void)state;
    cbx_device_model model;
    int rc = cbx_objectmanager_parse_reply(FIXTURE_MANAGER_ONLY, &model);
    assert_int_equal(rc, 0);
    assert_true(model.has_manager);
    assert_string_equal(model.manager_path,
                         "/org/shadowblip/InputPlumber/Manager");
    assert_int_equal(model.composite_count, 0);
    assert_int_equal(model.source_count, 0);
    assert_int_equal(model.target_count, 0);
}

static void
test_parse_composites(void **state)
{
    (void)state;
    cbx_device_model model;
    int rc = cbx_objectmanager_parse_reply(FIXTURE_COMPOSITES_ONLY, &model);
    assert_int_equal(rc, 0);
    assert_int_equal(model.composite_count, 3);
    assert_string_equal(model.composites[0].path,
                         "/org/shadowblip/InputPlumber/CompositeDevice0");
    assert_int_equal(model.composites[0].index, 0);
    assert_string_equal(model.composites[1].path,
                         "/org/shadowblip/InputPlumber/CompositeDevice3");
    assert_int_equal(model.composites[1].index, 3);
    assert_string_equal(model.composites[2].path,
                         "/org/shadowblip/InputPlumber/CompositeDevice12");
    assert_int_equal(model.composites[2].index, 12);
}

static void
test_parse_sources(void **state)
{
    (void)state;
    cbx_device_model model;
    int rc = cbx_objectmanager_parse_reply(FIXTURE_SOURCES_ONLY, &model);
    assert_int_equal(rc, 0);
    assert_int_equal(model.source_count, 3);
    assert_string_equal(model.sources[0].path,
                         "/org/shadowblip/InputPlumber/devices/source/event0");
    assert_string_equal(model.sources[0].name, "event0");
    assert_string_equal(model.sources[1].path,
                         "/org/shadowblip/InputPlumber/devices/source/event1");
    assert_string_equal(model.sources[1].name, "event1");
    assert_string_equal(model.sources[2].path,
                         "/org/shadowblip/InputPlumber/devices/source/hidraw0");
    assert_string_equal(model.sources[2].name, "hidraw0");
}

static void
test_parse_targets(void **state)
{
    (void)state;
    cbx_device_model model;
    int rc = cbx_objectmanager_parse_reply(FIXTURE_TARGETS_ONLY, &model);
    assert_int_equal(rc, 0);
    assert_int_equal(model.target_count, 2);
    assert_string_equal(model.targets[0].path,
                         "/org/shadowblip/InputPlumber/devices/target/gamepad0");
    assert_string_equal(model.targets[0].name, "gamepad0");
    assert_string_equal(model.targets[1].path,
                         "/org/shadowblip/InputPlumber/devices/target/mouse0");
    assert_string_equal(model.targets[1].name, "mouse0");
}

static void
test_parse_full_tree(void **state)
{
    (void)state;
    cbx_device_model model;
    int rc = cbx_objectmanager_parse_reply(FIXTURE_FULL, &model);
    assert_int_equal(rc, 0);
    assert_true(model.has_manager);
    assert_int_equal(model.composite_count, 2);
    assert_int_equal(model.source_count, 2);
    assert_int_equal(model.target_count, 2);
    assert_string_equal(model.composites[0].path,
                         "/org/shadowblip/InputPlumber/CompositeDevice0");
    assert_int_equal(model.composites[0].index, 0);
    assert_string_equal(model.composites[1].path,
                         "/org/shadowblip/InputPlumber/CompositeDevice1");
    assert_int_equal(model.composites[1].index, 1);
}

static void
test_parse_invalid_paths_skipped(void **state)
{
    (void)state;
    cbx_device_model model;
    int rc = cbx_objectmanager_parse_reply(FIXTURE_INVALID_PATHS, &model);
    assert_int_equal(rc, 0);
    /* Manager and one source should be parsed; invalid paths skipped. */
    assert_true(model.has_manager);
    assert_int_equal(model.composite_count, 0);
    assert_int_equal(model.source_count, 1);
    assert_int_equal(model.target_count, 0);
}

static void
test_parse_all_invalid_paths(void **state)
{
    (void)state;
    cbx_device_model model;
    int rc = cbx_objectmanager_parse_reply(FIXTURE_ALL_INVALID, &model);
    assert_int_equal(rc, 0);
    assert_false(model.has_manager);
    assert_int_equal(model.composite_count, 0);
    assert_int_equal(model.source_count, 0);
    assert_int_equal(model.target_count, 0);
}

static void
test_parse_malformed_lines(void **state)
{
    (void)state;
    cbx_device_model model;
    int rc = cbx_objectmanager_parse_reply(FIXTURE_MALFORMED, &model);
    assert_int_equal(rc, 0);
    /* Only the valid line (Manager) should be parsed. */
    assert_true(model.has_manager);
    assert_int_equal(model.composite_count, 0);
}

static void
test_parse_crlf_lines(void **state)
{
    (void)state;
    cbx_device_model model;
    int rc = cbx_objectmanager_parse_reply(FIXTURE_CRLF, &model);
    assert_int_equal(rc, 0);
    assert_true(model.has_manager);
    assert_int_equal(model.composite_count, 1);
    assert_string_equal(model.composites[0].path,
                         "/org/shadowblip/InputPlumber/CompositeDevice0");
}

static void
test_parse_comments_and_empty_lines(void **state)
{
    (void)state;
    cbx_device_model model;
    const char *fixture =
        "# Header comment\n"
        "\n"
        "/org/shadowblip/InputPlumber/Manager\t"
            "org.shadowblip.InputManager\n"
        "# Another comment\n"
        "\n"
        "/org/shadowblip/InputPlumber/CompositeDevice0\t"
            "org.shadowblip.Input.CompositeDevice\n";
    int rc = cbx_objectmanager_parse_reply(fixture, &model);
    assert_int_equal(rc, 0);
    assert_true(model.has_manager);
    assert_int_equal(model.composite_count, 1);
}

/* --- Device model lookup tests ----------------------------------------- */

static void
test_model_find_composite(void **state)
{
    (void)state;
    cbx_device_model model;
    cbx_objectmanager_parse_reply(FIXTURE_FULL, &model);

    const cbx_composite_entry *e =
        cbx_device_model_find_composite(&model,
            "/org/shadowblip/InputPlumber/CompositeDevice0");
    assert_non_null(e);
    assert_int_equal(e->index, 0);

    const cbx_composite_entry *notfound =
        cbx_device_model_find_composite(&model,
            "/org/shadowblip/InputPlumber/CompositeDevice99");
    assert_null(notfound);
}

static void
test_model_find_source(void **state)
{
    (void)state;
    cbx_device_model model;
    cbx_objectmanager_parse_reply(FIXTURE_FULL, &model);

    const cbx_device_entry *e =
        cbx_device_model_find_source(&model,
            "/org/shadowblip/InputPlumber/devices/source/event0");
    assert_non_null(e);
    assert_string_equal(e->name, "event0");

    assert_null(cbx_device_model_find_source(&model,
        "/org/shadowblip/InputPlumber/devices/source/nonexistent"));
}

static void
test_model_find_target(void **state)
{
    (void)state;
    cbx_device_model model;
    cbx_objectmanager_parse_reply(FIXTURE_FULL, &model);

    const cbx_device_entry *e =
        cbx_device_model_find_target(&model,
            "/org/shadowblip/InputPlumber/devices/target/gamepad0");
    assert_non_null(e);
    assert_string_equal(e->name, "gamepad0");

    assert_null(cbx_device_model_find_target(&model,
        "/org/shadowblip/InputPlumber/devices/target/nonexistent"));
}

static void
test_model_find_null_args(void **state)
{
    (void)state;
    cbx_device_model model;
    cbx_device_model_init(&model);

    assert_null(cbx_device_model_find_composite(NULL, "/path"));
    assert_null(cbx_device_model_find_composite(&model, NULL));
    assert_null(cbx_device_model_find_source(NULL, "/path"));
    assert_null(cbx_device_model_find_source(&model, NULL));
    assert_null(cbx_device_model_find_target(NULL, "/path"));
    assert_null(cbx_device_model_find_target(&model, NULL));
}

static void
test_model_init(void **state)
{
    (void)state;
    cbx_device_model model;
    /* Set garbage first. */
    memset(&model, 0xAA, sizeof(model));
    cbx_device_model_init(&model);
    assert_false(model.has_manager);
    assert_int_equal(model.composite_count, 0);
    assert_int_equal(model.source_count, 0);
    assert_int_equal(model.target_count, 0);
    assert_int_equal(model.manager_path[0], '\0');
}

/* --- Truncation tests --------------------------------------------------- */

static void
test_parse_truncate_composites(void **state)
{
    (void)state;
    /* Build a fixture with more composites than CBX_MAX_COMPOSITES. */
    char fixture[8192];
    fixture[0] = '\0';
    for (int i = 0; i < CBX_MAX_COMPOSITES + 5; i++) {
        char line[256];
        snprintf(line, sizeof(line),
                 "/org/shadowblip/InputPlumber/CompositeDevice%d\t"
                 "org.shadowblip.Input.CompositeDevice\n", i);
        strncat(fixture, line, sizeof(fixture) - strlen(fixture) - 1);
    }
    cbx_device_model model;
    int rc = cbx_objectmanager_parse_reply(fixture, &model);
    assert_int_equal(rc, 0);
    assert_int_equal(model.composite_count, CBX_MAX_COMPOSITES);
}

static void
test_parse_truncate_sources(void **state)
{
    (void)state;
    char fixture[32768];
    fixture[0] = '\0';
    for (int i = 0; i < CBX_MAX_DEVICES + 5; i++) {
        char line[512];
        snprintf(line, sizeof(line),
                 "/org/shadowblip/InputPlumber/devices/source/event%d\t"
                 "org.shadowblip.Input.Source.EventDevice\n", i);
        strncat(fixture, line, sizeof(fixture) - strlen(fixture) - 1);
    }
    cbx_device_model model;
    int rc = cbx_objectmanager_parse_reply(fixture, &model);
    assert_int_equal(rc, 0);
    assert_int_equal(model.source_count, CBX_MAX_DEVICES);
}

static void
test_parse_truncate_targets(void **state)
{
    (void)state;
    char fixture[32768];
    fixture[0] = '\0';
    for (int i = 0; i < CBX_MAX_DEVICES + 5; i++) {
        char line[512];
        snprintf(line, sizeof(line),
                 "/org/shadowblip/InputPlumber/devices/target/gamepad%d\t"
                 "org.shadowblip.Input.Target,org.shadowblip.Input.Gamepad\n", i);
        strncat(fixture, line, sizeof(fixture) - strlen(fixture) - 1);
    }
    cbx_device_model model;
    int rc = cbx_objectmanager_parse_reply(fixture, &model);
    assert_int_equal(rc, 0);
    assert_int_equal(model.target_count, CBX_MAX_DEVICES);
}

/* --- Enumerate via mock backend ----------------------------------------- */

/* Fixture: create a mock with a GetManagedObjects expectation. */
struct enum_ctx {
    ip_dbus_mock           mock;
    const ip_dbus_backend *backend;
};

static int
setup_enum(void **state)
{
    struct enum_ctx *ctx = malloc(sizeof(*ctx));
    assert_non_null(ctx);

    ip_dbus_mock_init(&ctx->mock);
    ctx->backend = ip_dbus_mock_backend(&ctx->mock);
    assert_non_null(ctx->backend);

    *state = ctx;
    return 0;
}

static int
teardown_enum(void **state)
{
    struct enum_ctx *ctx = *state;
    ip_dbus_mock_free(&ctx->mock);
    free(ctx);
    return 0;
}

static void
test_enumerate_full(void **state)
{
    struct enum_ctx *ctx = *state;
    ip_dbus_mock_expect_ok(&ctx->mock, IP_IFACE_OBJECT_MANAGER,
                            "GetManagedObjects", FIXTURE_FULL);

    cbx_device_model model;
    int rc = cbx_objectmanager_enumerate(ctx->backend, ctx->mock.bus, &model);
    assert_int_equal(rc, 0);
    assert_true(model.has_manager);
    assert_int_equal(model.composite_count, 2);
    assert_int_equal(model.source_count, 2);
    assert_int_equal(model.target_count, 2);
}

static void
test_enumerate_empty(void **state)
{
    struct enum_ctx *ctx = *state;
    ip_dbus_mock_expect_ok(&ctx->mock, IP_IFACE_OBJECT_MANAGER,
                            "GetManagedObjects", "");

    cbx_device_model model;
    int rc = cbx_objectmanager_enumerate(ctx->backend, ctx->mock.bus, &model);
    assert_int_equal(rc, 0);
    assert_false(model.has_manager);
    assert_int_equal(model.composite_count, 0);
}

static void
test_enumerate_null_reply(void **state)
{
    struct enum_ctx *ctx = *state;
    /* Register expectation with NULL value (simulates empty reply). */
    ip_dbus_mock_expect(&ctx->mock, IP_IFACE_OBJECT_MANAGER,
                         "GetManagedObjects", 0, NULL);

    cbx_device_model model;
    int rc = cbx_objectmanager_enumerate(ctx->backend, ctx->mock.bus, &model);
    assert_int_equal(rc, 0);
    assert_false(model.has_manager);
}

static void
test_enumerate_error(void **state)
{
    struct enum_ctx *ctx = *state;
    ip_dbus_mock_expect_error(&ctx->mock, IP_IFACE_OBJECT_MANAGER,
                               "GetManagedObjects", IP_ERR_NO_REPLY);

    cbx_device_model model;
    int rc = cbx_objectmanager_enumerate(ctx->backend, ctx->mock.bus, &model);
    assert_int_equal(rc, IP_ERR_NO_REPLY);
}

static void
test_enumerate_no_expectation(void **state)
{
    struct enum_ctx *ctx = *state;
    /* No expectation registered — mock returns -ENXIO. */

    cbx_device_model model;
    int rc = cbx_objectmanager_enumerate(ctx->backend, ctx->mock.bus, &model);
    assert_int_equal(rc, -ENXIO);
}

static void
test_enumerate_null_args(void **state)
{
    (void)state;
    cbx_device_model model;
    assert_int_equal(cbx_objectmanager_enumerate(NULL, NULL, &model), -EINVAL);
    assert_int_equal(cbx_objectmanager_enumerate(
        (const ip_dbus_backend *)0x1, NULL, NULL), -EINVAL);
}

static void
test_enumerate_crlf(void **state)
{
    struct enum_ctx *ctx = *state;
    ip_dbus_mock_expect_ok(&ctx->mock, IP_IFACE_OBJECT_MANAGER,
                            "GetManagedObjects", FIXTURE_CRLF);

    cbx_device_model model;
    int rc = cbx_objectmanager_enumerate(ctx->backend, ctx->mock.bus, &model);
    assert_int_equal(rc, 0);
    assert_true(model.has_manager);
    assert_int_equal(model.composite_count, 1);
}

/* --- Interface classification edge cases -------------------------------- */

static void
test_parse_multi_interface_target(void **state)
{
    (void)state;
    cbx_device_model model;
    const char *fixture =
        "/org/shadowblip/InputPlumber/devices/target/dbus0\t"
        "org.shadowblip.Input.Target,org.shadowblip.Input.DBusDevice\n";
    int rc = cbx_objectmanager_parse_reply(fixture, &model);
    assert_int_equal(rc, 0);
    assert_int_equal(model.target_count, 1);
    assert_string_equal(model.targets[0].name, "dbus0");
}

static void
test_parse_root_path_excluded(void **state)
{
    (void)state;
    cbx_device_model model;
    /* The root path itself (no trailing /) should not be classified. */
    const char *fixture =
        "/org/shadowblip/InputPlumber\t"
        "org.freedesktop.DBus.ObjectManager\n";
    int rc = cbx_objectmanager_parse_reply(fixture, &model);
    assert_int_equal(rc, 0);
    /* Root path doesn't start with IP_DBUS_PATH "/" — skipped. */
    assert_false(model.has_manager);
    assert_int_equal(model.composite_count, 0);
}

static void
test_parse_composite_no_index(void **state)
{
    (void)state;
    cbx_device_model model;
    const char *fixture =
        "/org/shadowblip/InputPlumber/CompositeDeviceABC\t"
        "org.shadowblip.Input.CompositeDevice\n";
    int rc = cbx_objectmanager_parse_reply(fixture, &model);
    assert_int_equal(rc, 0);
    assert_int_equal(model.composite_count, 1);
    /* Index should be -1 (no digits found). */
    assert_int_equal(model.composites[0].index, -1);
}

/* --- Test runner -------------------------------------------------------- */

static const struct CMUnitTest tests[] = {
    /* Parser tests */
    cmocka_unit_test(test_parse_null_reply),
    cmocka_unit_test(test_parse_empty_reply),
    cmocka_unit_test(test_parse_null_model),
    cmocka_unit_test(test_parse_manager_only),
    cmocka_unit_test(test_parse_composites),
    cmocka_unit_test(test_parse_sources),
    cmocka_unit_test(test_parse_targets),
    cmocka_unit_test(test_parse_full_tree),
    cmocka_unit_test(test_parse_invalid_paths_skipped),
    cmocka_unit_test(test_parse_all_invalid_paths),
    cmocka_unit_test(test_parse_malformed_lines),
    cmocka_unit_test(test_parse_crlf_lines),
    cmocka_unit_test(test_parse_comments_and_empty_lines),
    /* Device model lookup */
    cmocka_unit_test(test_model_find_composite),
    cmocka_unit_test(test_model_find_source),
    cmocka_unit_test(test_model_find_target),
    cmocka_unit_test(test_model_find_null_args),
    cmocka_unit_test(test_model_init),
    /* Truncation */
    cmocka_unit_test(test_parse_truncate_composites),
    cmocka_unit_test(test_parse_truncate_sources),
    cmocka_unit_test(test_parse_truncate_targets),
    /* Enumerate via mock */
    cmocka_unit_test_setup_teardown(test_enumerate_full,
        setup_enum, teardown_enum),
    cmocka_unit_test_setup_teardown(test_enumerate_empty,
        setup_enum, teardown_enum),
    cmocka_unit_test_setup_teardown(test_enumerate_null_reply,
        setup_enum, teardown_enum),
    cmocka_unit_test_setup_teardown(test_enumerate_error,
        setup_enum, teardown_enum),
    cmocka_unit_test_setup_teardown(test_enumerate_no_expectation,
        setup_enum, teardown_enum),
    cmocka_unit_test(test_enumerate_null_args),
    cmocka_unit_test_setup_teardown(test_enumerate_crlf,
        setup_enum, teardown_enum),
    /* Interface classification edge cases */
    cmocka_unit_test(test_parse_multi_interface_target),
    cmocka_unit_test(test_parse_root_path_excluded),
    cmocka_unit_test(test_parse_composite_no_index),
};

int
main(void)
{
    return cmocka_run_group_tests(tests, NULL, NULL);
}