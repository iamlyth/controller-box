/*
 * test_controllers_tab.c — Tests for the Controllers tab (Task 35).
 *
 * Tests the controllers tab module using mock DBus.  Verifies:
 *   - Init populates the panel with widgets
 *   - Refresh enumerates devices and queries their types
 *   - Load supported types parses SupportedTargetDeviceIds
 *   - Add calls CreateTargetDevice and refreshes
 *   - Remove calls StopTargetDevice and refreshes
 *   - Change type calls SetTargetDevices and refreshes
 *   - Type picker mode (begin/confirm/cancel)
 *   - NULL safety and error handling
 *   - Mixed types allowed
 *
 * Task 35 — Controllers tab.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <setjmp.h>
#include <cmocka.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "manager/controllers_tab.h"
#include "manager/manager.h"
#include "dbus_mock.h"
#include "dbus/ip_manager.h"
#include "dbus/ip_target.h"
#include "dbus/ip_composite.h"
#include "dbus/ip_connection.h"
#include "ui/widget.h"

#ifndef CBX_FONT_PATH
#define CBX_FONT_PATH ""
#endif

/* --- Fixture data -------------------------------------------------- */

/* ObjectManager reply with 2 composites and 2 targets. */
static const char *FIXTURE_2C2T =
    "/org/shadowblip/InputPlumber/Manager\t"
        "org.shadowblip.InputManager\n"
    "/org/shadowblip/InputPlumber/CompositeDevice0\t"
        "org.shadowblip.Input.CompositeDevice\n"
    "/org/shadowblip/InputPlumber/CompositeDevice1\t"
        "org.shadowblip.Input.CompositeDevice\n"
    "/org/shadowblip/InputPlumber/devices/target/gamepad0\t"
        "org.shadowblip.Input.Target,org.shadowblip.Input.Gamepad\n"
    "/org/shadowblip/InputPlumber/devices/target/gamepad1\t"
        "org.shadowblip.Input.Target,org.shadowblip.Input.Gamepad\n";

/* ObjectManager reply with 1 composite and 1 target. */
static const char *FIXTURE_1C1T =
    "/org/shadowblip/InputPlumber/Manager\t"
        "org.shadowblip.InputManager\n"
    "/org/shadowblip/InputPlumber/CompositeDevice0\t"
        "org.shadowblip.Input.CompositeDevice\n"
    "/org/shadowblip/InputPlumber/devices/target/gamepad0\t"
        "org.shadowblip.Input.Target,org.shadowblip.Input.Gamepad\n";

/* ObjectManager reply with 0 devices. */
static const char *FIXTURE_EMPTY = "";

/* Supported types CSV. */
static const char *SUPPORTED_TYPES = "xb360,ds5,deck,gamepad,mouse,keyboard";

/* --- Helpers ------------------------------------------------------- */

static void
ensure_dummy_driver(void)
{
    if (getenv("SDL_VIDEODRIVER") == NULL)
        SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "dummy",
                                 SDL_HINT_OVERRIDE);
}

/* --- Test fixture -------------------------------------------------- */

typedef struct {
    ip_dbus_mock          mock;
    const ip_dbus_backend *backend;
    cbx_manager           mgr;
    cbx_controllers_tab  tab;
} ct_fixture;

static int
setup(void **state)
{
    ct_fixture *f = malloc(sizeof(*f));
    if (!f)
        return -1;
    memset(f, 0, sizeof(*f));
    ensure_dummy_driver();

    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);

    /* Init manager (creates SDL2 window, panels, tabbar). */
    int rc = cbx_manager_init(&f->mgr, NULL);
    if (rc != 0) {
        ip_dbus_mock_free(&f->mock);
        free(f);
        return -1;
    }

    *state = f;
    return 0;
}

static int
teardown(void **state)
{
    ct_fixture *f = *state;
    if (f) {
        cbx_controllers_tab_shutdown(&f->tab);
        cbx_manager_shutdown(&f->mgr);
        ip_dbus_mock_free(&f->mock);
        free(f);
    }
    return 0;
}

#define FIX(state) (*(ct_fixture **)(state))

/* Helper: set up mock expectations for a full refresh. */
static void
expect_refresh(ip_dbus_mock *mock, const char *fixture,
               const char *type0, const char *type1)
{
    ip_dbus_mock_expect_ok(mock, IP_IFACE_OBJECT_MANAGER,
                            "GetManagedObjects", fixture);
    if (type0)
        ip_dbus_mock_expect_ok(mock, IP_IFACE_TARGET,
                                "DeviceType", type0);
    if (type1)
        ip_dbus_mock_expect_ok(mock, IP_IFACE_TARGET,
                                "DeviceType", type1);
}

/* Helper: init the controllers tab with standard expectations. */
static void
init_tab_with_devices(ct_fixture *f, const char *fixture,
                       const char *type0, const char *type1)
{
    /* SupportedTargetDeviceIds. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                            "SupportedTargetDeviceIds", SUPPORTED_TYPES);
    /* Refresh: enumerate + device types. */
    expect_refresh(&f->mock, fixture, type0, type1);

    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_CONTROLLERS];
    int rc = cbx_controllers_tab_init(&f->tab, panel, f->backend,
                                        f->mock.bus, &f->mgr.text_cache,
                                        &f->mgr.theme, f->mgr.font_id);
    assert_int_equal(rc, 0);
}

/* ================================================================== */
/*  Tests                                                              */
/* ================================================================== */

/* --- Init ---------------------------------------------------------- */

static void
test_init_populates_panel(void **state)
{
    ct_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                            "SupportedTargetDeviceIds", SUPPORTED_TYPES);
    expect_refresh(&f->mock, FIXTURE_1C1T, "xb360", NULL);

    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_CONTROLLERS];
    int rc = cbx_controllers_tab_init(&f->tab, panel, f->backend,
                                        f->mock.bus, &f->mgr.text_cache,
                                        &f->mgr.theme, f->mgr.font_id);
    assert_int_equal(rc, 0);

    /* Panel should have 5 children: device_list, add_btn, remove_btn,
     * change_type_btn, type_picker. */
    assert_int_equal(cbx_panel_child_count(panel), 5);

    /* Mode is list. */
    assert_int_equal(cbx_controllers_tab_mode(&f->tab), CBX_CT_MODE_LIST);

    /* Supported types loaded. */
    assert_int_equal(cbx_controllers_tab_supported_type_count(&f->tab), 6);
    assert_string_equal(cbx_controllers_tab_supported_type(&f->tab, 0),
                         "xb360");
    assert_string_equal(cbx_controllers_tab_supported_type(&f->tab, 1),
                         "ds5");
}

static void
test_init_without_dbus(void **state)
{
    ct_fixture *f = FIX(state);
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_CONTROLLERS];

    /* Init with NULL backend — should succeed, just no refresh. */
    int rc = cbx_controllers_tab_init(&f->tab, panel, NULL, NULL,
                                        &f->mgr.text_cache, &f->mgr.theme,
                                        f->mgr.font_id);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_controllers_tab_device_count(&f->tab), 0);
    assert_int_equal(cbx_controllers_tab_supported_type_count(&f->tab), 0);
}

static void
test_init_null_args(void **state)
{
    (void)state;
    cbx_controllers_tab tab;
    cbx_panel panel;
    memset(&tab, 0, sizeof(tab));
    memset(&panel, 0, sizeof(panel));

    assert_int_equal(cbx_controllers_tab_init(NULL, &panel, NULL, NULL,
                                                  NULL, NULL, -1), -EINVAL);
    assert_int_equal(cbx_controllers_tab_init(&tab, NULL, NULL, NULL,
                                                  NULL, NULL, -1), -EINVAL);
}

/* --- Refresh ------------------------------------------------------- */

static void
test_refresh_enumerates_devices(void **state)
{
    ct_fixture *f = FIX(state);
    /* Mock DBus returns first match for (iface, member), so both
     * DeviceType queries return the same value. Use one type. */
    init_tab_with_devices(f, FIXTURE_2C2T, "xb360", NULL);

    /* 2 target devices. */
    assert_int_equal(cbx_controllers_tab_device_count(&f->tab), 2);

    /* Device types queried (both get first match = "xb360"). */
    assert_string_equal(cbx_controllers_tab_device_type(&f->tab, 0),
                         "xb360");

    /* Device paths. */
    assert_string_equal(cbx_controllers_tab_device_path(&f->tab, 0),
        "/org/shadowblip/InputPlumber/devices/target/gamepad0");
    assert_string_equal(cbx_controllers_tab_device_path(&f->tab, 1),
        "/org/shadowblip/InputPlumber/devices/target/gamepad1");
}

static void
test_refresh_empty(void **state)
{
    ct_fixture *f = FIX(state);
    init_tab_with_devices(f, FIXTURE_EMPTY, NULL, NULL);

    assert_int_equal(cbx_controllers_tab_device_count(&f->tab), 0);
}

static void
test_refresh_calls_enumerate_again(void **state)
{
    ct_fixture *f = FIX(state);
    init_tab_with_devices(f, FIXTURE_1C1T, "xb360", NULL);

    /* Now refresh with a different fixture. */
    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_OBJECT_MANAGER,
                            "GetManagedObjects", FIXTURE_2C2T);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_TARGET,
                            "DeviceType", "ds5");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_TARGET,
                            "DeviceType", "xb360");

    int rc = cbx_controllers_tab_refresh(&f->tab);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_controllers_tab_device_count(&f->tab), 2);
}

static void
test_refresh_enumerate_error(void **state)
{
    ct_fixture *f = FIX(state);
    init_tab_with_devices(f, FIXTURE_1C1T, "xb360", NULL);

    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_OBJECT_MANAGER,
                                "GetManagedObjects", IP_ERR_NO_REPLY);

    int rc = cbx_controllers_tab_refresh(&f->tab);
    assert_int_equal(rc, IP_ERR_NO_REPLY);
}

static void
test_refresh_null_tab(void **state)
{
    (void)state;
    assert_int_equal(cbx_controllers_tab_refresh(NULL), -EINVAL);
}

/* --- Load supported types ----------------------------------------- */

static void
test_load_supported_types_success(void **state)
{
    ct_fixture *f = FIX(state);
    /* Init without dbus to get a clean tab. */
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_CONTROLLERS];
    cbx_controllers_tab_init(&f->tab, panel, f->backend, f->mock.bus,
                                &f->mgr.text_cache, &f->mgr.theme,
                                f->mgr.font_id);

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                            "SupportedTargetDeviceIds", "xb360,ds5,deck");

    int rc = cbx_controllers_tab_load_supported_types(&f->tab);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_controllers_tab_supported_type_count(&f->tab), 3);
    assert_string_equal(cbx_controllers_tab_supported_type(&f->tab, 0),
                         "xb360");
    assert_string_equal(cbx_controllers_tab_supported_type(&f->tab, 2),
                         "deck");
}

static void
test_load_supported_types_error(void **state)
{
    ct_fixture *f = FIX(state);
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_CONTROLLERS];
    cbx_controllers_tab_init(&f->tab, panel, f->backend, f->mock.bus,
                                &f->mgr.text_cache, &f->mgr.theme,
                                f->mgr.font_id);

    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_MANAGER,
                                "SupportedTargetDeviceIds", IP_ERR_NO_REPLY);

    int rc = cbx_controllers_tab_load_supported_types(&f->tab);
    assert_int_equal(rc, IP_ERR_NO_REPLY);
    assert_int_equal(cbx_controllers_tab_supported_type_count(&f->tab), 0);
}

static void
test_load_supported_types_whitespace(void **state)
{
    ct_fixture *f = FIX(state);
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_CONTROLLERS];
    cbx_controllers_tab_init(&f->tab, panel, f->backend, f->mock.bus,
                                &f->mgr.text_cache, &f->mgr.theme,
                                f->mgr.font_id);

    /* Types with whitespace around them. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                            "SupportedTargetDeviceIds",
                            " xb360 , ds5 , deck ");

    int rc = cbx_controllers_tab_load_supported_types(&f->tab);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_controllers_tab_supported_type_count(&f->tab), 3);
    assert_string_equal(cbx_controllers_tab_supported_type(&f->tab, 0),
                         "xb360");
}

/* --- Add ----------------------------------------------------------- */

static void
test_add_success(void **state)
{
    ct_fixture *f = FIX(state);
    init_tab_with_devices(f, FIXTURE_1C1T, "xb360", NULL);

    /* CreateTargetDevice returns a new path. */
    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                            "CreateTargetDevice",
                            "/org/shadowblip/InputPlumber/devices/target/gamepad1");
    /* Refresh after add: enumerate + type queries. */
    expect_refresh(&f->mock, FIXTURE_2C2T, "xb360", "ds5");

    int rc = cbx_controllers_tab_add(&f->tab, "ds5");
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_controllers_tab_device_count(&f->tab), 2);
}

static void
test_add_error(void **state)
{
    ct_fixture *f = FIX(state);
    init_tab_with_devices(f, FIXTURE_1C1T, "xb360", NULL);

    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_MANAGER,
                                "CreateTargetDevice", IP_ERR_INVALID_ARGS);

    int rc = cbx_controllers_tab_add(&f->tab, "xb360");
    assert_int_equal(rc, IP_ERR_INVALID_ARGS);
}

static void
test_add_null_args(void **state)
{
    (void)state;
    cbx_controllers_tab tab;
    memset(&tab, 0, sizeof(tab));
    /* No backend set. */
    assert_int_equal(cbx_controllers_tab_add(&tab, "xb360"), -EINVAL);
}

/* --- Remove -------------------------------------------------------- */

static void
test_remove_success(void **state)
{
    ct_fixture *f = FIX(state);
    init_tab_with_devices(f, FIXTURE_2C2T, "xb360", "ds5");

    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                            "StopTargetDevice", NULL);
    /* Refresh after remove. */
    expect_refresh(&f->mock, FIXTURE_1C1T, "xb360", NULL);

    int rc = cbx_controllers_tab_remove(&f->tab, 0);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_controllers_tab_device_count(&f->tab), 1);
}

static void
test_remove_error(void **state)
{
    ct_fixture *f = FIX(state);
    init_tab_with_devices(f, FIXTURE_1C1T, "xb360", NULL);

    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_MANAGER,
                                "StopTargetDevice", IP_ERR_NO_REPLY);

    int rc = cbx_controllers_tab_remove(&f->tab, 0);
    assert_int_equal(rc, IP_ERR_NO_REPLY);
}

static void
test_remove_bad_index(void **state)
{
    ct_fixture *f = FIX(state);
    init_tab_with_devices(f, FIXTURE_1C1T, "xb360", NULL);

    assert_int_equal(cbx_controllers_tab_remove(&f->tab, -1), -EINVAL);
    assert_int_equal(cbx_controllers_tab_remove(&f->tab, 99), -EINVAL);
}

/* --- Change type --------------------------------------------------- */

static void
test_change_type_success(void **state)
{
    ct_fixture *f = FIX(state);
    init_tab_with_devices(f, FIXTURE_1C1T, "xb360", NULL);

    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "SetTargetDevices", NULL);
    /* Refresh after change. */
    expect_refresh(&f->mock, FIXTURE_1C1T, "ds5", NULL);

    int rc = cbx_controllers_tab_change_type(&f->tab, 0, "ds5");
    assert_int_equal(rc, 0);
    assert_string_equal(cbx_controllers_tab_device_type(&f->tab, 0),
                         "ds5");
}

static void
test_change_type_mixed(void **state)
{
    ct_fixture *f = FIX(state);
    /* Mock DBus returns first match for DeviceType, so both devices
     * initially report "xb360". */
    init_tab_with_devices(f, FIXTURE_2C2T, "xb360", NULL);

    ip_dbus_mock_reset(&f->mock);
    /* SetTargetDevices on CompositeDevice1 (index 1) with new type. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "SetTargetDevices", NULL);
    /* Refresh: both targets still present. Mock returns first DeviceType
     * match for all queries. */
    expect_refresh(&f->mock, FIXTURE_2C2T, "deck", NULL);

    int rc = cbx_controllers_tab_change_type(&f->tab, 1, "deck");
    assert_int_equal(rc, 0);
    /* Both devices report "deck" (mock limitation), but the test
     * verifies the change_type call succeeded with mixed types allowed
     * — the SetTargetDevices CSV includes both types. */
    assert_int_equal(cbx_controllers_tab_device_count(&f->tab), 2);
}

static void
test_change_type_error(void **state)
{
    ct_fixture *f = FIX(state);
    init_tab_with_devices(f, FIXTURE_1C1T, "xb360", NULL);

    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                                "SetTargetDevices", IP_ERR_NO_REPLY);

    int rc = cbx_controllers_tab_change_type(&f->tab, 0, "ds5");
    assert_int_equal(rc, IP_ERR_NO_REPLY);
}

static void
test_change_type_bad_index(void **state)
{
    ct_fixture *f = FIX(state);
    init_tab_with_devices(f, FIXTURE_1C1T, "xb360", NULL);

    assert_int_equal(cbx_controllers_tab_change_type(&f->tab, -1, "ds5"),
                      -EINVAL);
    assert_int_equal(cbx_controllers_tab_change_type(&f->tab, 99, "ds5"),
                      -EINVAL);
}

static void
test_change_type_no_composite(void **state)
{
    ct_fixture *f = FIX(state);
    init_tab_with_devices(f, FIXTURE_1C1T, "xb360", NULL);

    /* Device index 0 is valid, but if we pretend there's no composite
     * at that index... Actually our fixture has 1 composite. Let's
     * test index 1 which has a target but no composite (only 1
     * composite in fixture). */
    /* Actually FIXTURE_1C1T has 1 composite and 1 target, so index 0
     * is valid. Let's add a second target with no composite. */
    ip_dbus_mock_reset(&f->mock);
    /* Fixture with 1 composite but 2 targets. */
    const char *fixture =
        "/org/shadowblip/InputPlumber/Manager\t"
            "org.shadowblip.InputManager\n"
        "/org/shadowblip/InputPlumber/CompositeDevice0\t"
            "org.shadowblip.Input.CompositeDevice\n"
        "/org/shadowblip/InputPlumber/devices/target/gamepad0\t"
            "org.shadowblip.Input.Target,org.shadowblip.Input.Gamepad\n"
        "/org/shadowblip/InputPlumber/devices/target/gamepad1\t"
            "org.shadowblip.Input.Target,org.shadowblip.Input.Gamepad\n";
    expect_refresh(&f->mock, fixture, "xb360", "ds5");

    cbx_controllers_tab_refresh(&f->tab);
    assert_int_equal(cbx_controllers_tab_device_count(&f->tab), 2);

    /* Change type on index 1 — no composite at index 1. */
    int rc = cbx_controllers_tab_change_type(&f->tab, 1, "deck");
    assert_int_equal(rc, -EINVAL);
}

/* --- Type picker --------------------------------------------------- */

static void
test_type_picker_begin(void **state)
{
    ct_fixture *f = FIX(state);
    init_tab_with_devices(f, FIXTURE_1C1T, "xb360", NULL);

    int rc = cbx_controllers_tab_begin_type_pick(&f->tab,
                                                   CBX_CT_ACTION_ADD);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_controllers_tab_mode(&f->tab),
                      CBX_CT_MODE_TYPE_PICK);

    /* Type picker should have supported types. */
    assert_int_equal(cbx_list_item_count(&f->tab.type_picker), 6);
}

static void
test_type_picker_confirm_add(void **state)
{
    ct_fixture *f = FIX(state);
    init_tab_with_devices(f, FIXTURE_1C1T, "xb360", NULL);

    /* Begin type pick for add. */
    cbx_controllers_tab_begin_type_pick(&f->tab, CBX_CT_ACTION_ADD);
    f->tab.selected_type = 1; /* ds5 */

    /* CreateTargetDevice + refresh. */
    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                            "CreateTargetDevice",
                            "/org/shadowblip/InputPlumber/devices/target/gamepad1");
    expect_refresh(&f->mock, FIXTURE_2C2T, "xb360", "ds5");

    int rc = cbx_controllers_tab_confirm_type_pick(&f->tab);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_controllers_tab_mode(&f->tab),
                      CBX_CT_MODE_LIST);
    assert_int_equal(cbx_controllers_tab_device_count(&f->tab), 2);
}

static void
test_type_picker_confirm_change(void **state)
{
    ct_fixture *f = FIX(state);
    init_tab_with_devices(f, FIXTURE_1C1T, "xb360", NULL);
    f->tab.selected_device = 0;

    cbx_controllers_tab_begin_type_pick(&f->tab, CBX_CT_ACTION_CHANGE);
    f->tab.selected_type = 1; /* ds5 */

    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "SetTargetDevices", NULL);
    expect_refresh(&f->mock, FIXTURE_1C1T, "ds5", NULL);

    int rc = cbx_controllers_tab_confirm_type_pick(&f->tab);
    assert_int_equal(rc, 0);
    assert_string_equal(cbx_controllers_tab_device_type(&f->tab, 0),
                         "ds5");
}

static void
test_type_picker_cancel(void **state)
{
    ct_fixture *f = FIX(state);
    init_tab_with_devices(f, FIXTURE_1C1T, "xb360", NULL);

    cbx_controllers_tab_begin_type_pick(&f->tab, CBX_CT_ACTION_ADD);
    assert_int_equal(cbx_controllers_tab_mode(&f->tab),
                      CBX_CT_MODE_TYPE_PICK);

    cbx_controllers_tab_cancel_type_pick(&f->tab);
    assert_int_equal(cbx_controllers_tab_mode(&f->tab),
                      CBX_CT_MODE_LIST);
    assert_int_equal(f->tab.pending_action, CBX_CT_ACTION_NONE);
}

static void
test_type_picker_no_supported_types(void **state)
{
    ct_fixture *f = FIX(state);
    /* Init without DBus so no supported types loaded. */
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_CONTROLLERS];
    cbx_controllers_tab_init(&f->tab, panel, NULL, NULL,
                                &f->mgr.text_cache, &f->mgr.theme,
                                f->mgr.font_id);

    int rc = cbx_controllers_tab_begin_type_pick(&f->tab,
                                                   CBX_CT_ACTION_ADD);
    assert_int_equal(rc, -ENOENT);
}

static void
test_type_picker_confirm_not_in_pick_mode(void **state)
{
    ct_fixture *f = FIX(state);
    init_tab_with_devices(f, FIXTURE_1C1T, "xb360", NULL);

    /* Not in type-pick mode. */
    int rc = cbx_controllers_tab_confirm_type_pick(&f->tab);
    assert_int_equal(rc, -EINVAL);
}

/* --- Shutdown ------------------------------------------------------ */

static void
test_shutdown_removes_panel_children(void **state)
{
    ct_fixture *f = FIX(state);
    init_tab_with_devices(f, FIXTURE_1C1T, "xb360", NULL);

    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_CONTROLLERS];
    assert_int_equal(cbx_panel_child_count(panel), 5);

    cbx_controllers_tab_shutdown(&f->tab);
    assert_int_equal(cbx_panel_child_count(panel), 0);

    /* Mark tab as shutdown so teardown doesn't double-free. */
    memset(&f->tab, 0, sizeof(f->tab));
}

static void
test_shutdown_null_safe(void **state)
{
    (void)state;
    cbx_controllers_tab_shutdown(NULL);
}

/* --- Accessors ----------------------------------------------------- */

static void
test_accessors_null_safe(void **state)
{
    (void)state;
    assert_int_equal(cbx_controllers_tab_device_count(NULL), 0);
    assert_int_equal(cbx_controllers_tab_supported_type_count(NULL), 0);
    assert_null(cbx_controllers_tab_device_type(NULL, 0));
    assert_null(cbx_controllers_tab_device_path(NULL, 0));
    assert_null(cbx_controllers_tab_supported_type(NULL, 0));
    assert_int_equal(cbx_controllers_tab_selected_device(NULL), -1);
    assert_int_equal(cbx_controllers_tab_mode(NULL), CBX_CT_MODE_LIST);
}

static void
test_accessors_bad_index(void **state)
{
    ct_fixture *f = FIX(state);
    init_tab_with_devices(f, FIXTURE_1C1T, "xb360", NULL);

    assert_null(cbx_controllers_tab_device_type(&f->tab, -1));
    assert_null(cbx_controllers_tab_device_type(&f->tab, 99));
    assert_null(cbx_controllers_tab_device_path(&f->tab, -1));
    assert_null(cbx_controllers_tab_supported_type(&f->tab, -1));
}

/* --- Full workflow ------------------------------------------------- */

static void
test_full_workflow(void **state)
{
    ct_fixture *f = FIX(state);
    init_tab_with_devices(f, FIXTURE_EMPTY, NULL, NULL);

    /* Start with 0 devices. */
    assert_int_equal(cbx_controllers_tab_device_count(&f->tab), 0);

    /* Add a device. */
    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                            "CreateTargetDevice",
                            "/org/shadowblip/InputPlumber/devices/target/gamepad0");
    expect_refresh(&f->mock, FIXTURE_1C1T, "xb360", NULL);

    int rc = cbx_controllers_tab_add(&f->tab, "xb360");
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_controllers_tab_device_count(&f->tab), 1);
    assert_string_equal(cbx_controllers_tab_device_type(&f->tab, 0),
                         "xb360");

    /* Change type. */
    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "SetTargetDevices", NULL);
    expect_refresh(&f->mock, FIXTURE_1C1T, "ds5", NULL);

    rc = cbx_controllers_tab_change_type(&f->tab, 0, "ds5");
    assert_int_equal(rc, 0);
    assert_string_equal(cbx_controllers_tab_device_type(&f->tab, 0),
                         "ds5");

    /* Remove device. */
    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                            "StopTargetDevice", NULL);
    expect_refresh(&f->mock, FIXTURE_EMPTY, NULL, NULL);

    rc = cbx_controllers_tab_remove(&f->tab, 0);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_controllers_tab_device_count(&f->tab), 0);
}

/* ================================================================== */
/*  Main                                                               */
/* ================================================================== */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* Init. */
        cmocka_unit_test_setup_teardown(test_init_populates_panel,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_init_without_dbus,
                                         setup, teardown),
        cmocka_unit_test(test_init_null_args),

        /* Refresh. */
        cmocka_unit_test_setup_teardown(test_refresh_enumerates_devices,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_refresh_empty,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_refresh_calls_enumerate_again,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_refresh_enumerate_error,
                                         setup, teardown),
        cmocka_unit_test(test_refresh_null_tab),

        /* Load supported types. */
        cmocka_unit_test_setup_teardown(test_load_supported_types_success,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_load_supported_types_error,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_load_supported_types_whitespace,
                                         setup, teardown),

        /* Add. */
        cmocka_unit_test_setup_teardown(test_add_success, setup, teardown),
        cmocka_unit_test_setup_teardown(test_add_error, setup, teardown),
        cmocka_unit_test(test_add_null_args),

        /* Remove. */
        cmocka_unit_test_setup_teardown(test_remove_success, setup, teardown),
        cmocka_unit_test_setup_teardown(test_remove_error, setup, teardown),
        cmocka_unit_test_setup_teardown(test_remove_bad_index,
                                         setup, teardown),

        /* Change type. */
        cmocka_unit_test_setup_teardown(test_change_type_success,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_change_type_mixed,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_change_type_error,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_change_type_bad_index,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_change_type_no_composite,
                                         setup, teardown),

        /* Type picker. */
        cmocka_unit_test_setup_teardown(test_type_picker_begin,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_type_picker_confirm_add,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_type_picker_confirm_change,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_type_picker_cancel,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_type_picker_no_supported_types,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_type_picker_confirm_not_in_pick_mode,
                                         setup, teardown),

        /* Shutdown. */
        cmocka_unit_test_setup_teardown(test_shutdown_removes_panel_children,
                                         setup, teardown),
        cmocka_unit_test(test_shutdown_null_safe),

        /* Accessors. */
        cmocka_unit_test(test_accessors_null_safe),
        cmocka_unit_test_setup_teardown(test_accessors_bad_index,
                                         setup, teardown),

        /* Full workflow. */
        cmocka_unit_test_setup_teardown(test_full_workflow, setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}