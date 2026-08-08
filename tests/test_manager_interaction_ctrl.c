/*
 * test_manager_interaction_ctrl.c — Manager interaction acceptance tests
 * for the Controllers and Settings tabs through production dispatch.
 *
 * Task 8 — Every control M04–M09 (Controllers) and M21–M27 (Settings),
 * plus tab switching M01–M03 and disabled scenarios D01, D02, D05, D06,
 * is exercised through cbx_manager_handle_event (the same dispatch path
 * the installed binary uses) for BOTH the controller (keyboard) and
 * pointer (mouse) input paths.  Semantic outcomes (mode transitions,
 * DBus calls, device-count changes, settings mutations, file writes)
 * are asserted — never mere handler return values.
 */
#include <cmocka.h>
#include <SDL2/SDL.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "manager/manager.h"
#include "manager/controllers_tab.h"
#include "manager/settings_tab.h"
#include "config/config_settings.h"
#include "ui/widget.h"
#include "dbus_mock.h"
#include "interaction_inventory.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */

#define SUPPORTED_TYPES "xb360,ds5,deck,gamepad,mouse,keyboard"

static const char *FIXTURE_1C1T =
    "/org/shadowblip/InputPlumber/Manager\t"
        "org.shadowblip.InputManager\n"
    "/org/shadowblip/InputPlumber/CompositeDevice0\t"
        "org.shadowblip.Input.CompositeDevice\n"
    "/org/shadowblip/InputPlumber/devices/target/gamepad0\t"
        "org.shadowblip.Input.Target,org.shadowblip.Input.Gamepad\n";

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

static const char *FIXTURE_EMPTY = "";

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

static void
ensure_dummy_driver(void)
{
    if (getenv("SDL_VIDEODRIVER") == NULL)
        SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "dummy",
                                SDL_HINT_OVERRIDE);
}

static bool
send_key_dn(cbx_manager *mgr, SDL_Keycode sym)
{
    SDL_Event ev = {0};
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = sym;
    return cbx_manager_handle_event(mgr, &ev);
}

static bool
send_key_up(cbx_manager *mgr, SDL_Keycode sym)
{
    SDL_Event ev = {0};
    ev.type = SDL_KEYUP;
    ev.key.keysym.sym = sym;
    return cbx_manager_handle_event(mgr, &ev);
}

static void
send_key_press(cbx_manager *mgr, SDL_Keycode sym)
{
    send_key_dn(mgr, sym);
    send_key_up(mgr, sym);
}

static bool
send_mouse_click(cbx_manager *mgr, int x, int y)
{
    SDL_Event ev = {0};
    ev.type = SDL_MOUSEBUTTONDOWN;
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.x = x;
    ev.button.y = y;
    bool down = cbx_manager_handle_event(mgr, &ev);

    ev.type = SDL_MOUSEBUTTONUP;
    ev.button.x = x;
    ev.button.y = y;
    cbx_manager_handle_event(mgr, &ev);
    return down;
}

static void
widget_center(const cbx_widget *w, int *cx, int *cy)
{
    assert_non_null(w);
    assert_non_null(cx);
    assert_non_null(cy);
    *cx = w->rect.x + w->rect.w / 2;
    *cy = w->rect.y + w->rect.h / 2;
}

/* Navigate DOWN from the tabbar to the first panel child (the list),
 * then past the list to the first button row.  The list returns false
 * at its bottom boundary, allowing the focus chain to move to the
 * button group. */
static void
nav_to_buttons(cbx_manager *mgr)
{
    /* DOWN: tabbar → device list */
    send_key_dn(mgr, SDLK_DOWN);
    /* DOWN: list at bottom → focus chain → button group */
    send_key_dn(mgr, SDLK_DOWN);
}

/* Navigate to a specific button by index (0=Add, 1=Remove, 2=ChangeType).
 * After nav_to_buttons, focus is on the rightmost button (closest
 * to the list center spatially).  Navigate LEFT to reach the
 * desired button: 2-index LEFTs. */
static void
nav_to_button(cbx_manager *mgr, int index)
{
    for (int i = 0; i < 2 - index; i++)
        send_key_dn(mgr, SDLK_LEFT);
}

/* Compute the y-coordinate for clicking on a specific item in a list. */
static int
list_item_y(const cbx_list *lst, int index)
{
    return lst->base.rect.y + index * lst->item_h + lst->item_h / 2;
}

/* Compute the x-coordinate for clicking on a list (center). */
static int
list_center_x(const cbx_list *lst)
{
    return lst->base.rect.x + lst->base.rect.w / 2;
}

/* ------------------------------------------------------------------ */
/*  Fixture                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    ip_dbus_mock          mock;
    const ip_dbus_backend *backend;
    cbx_manager           mgr;
    char                  tmp_home[4096];
} mi_fixture;

/* Common expectation set for controllers-tab init. */
static void
mi_init_ctrl(mi_fixture *f, const char *om_fixture,
             const char *type0, const char *type1)
{
    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                           "SupportedTargetDeviceIds", SUPPORTED_TYPES);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_OBJECT_MANAGER,
                           "GetManagedObjects", om_fixture);
    if (type0)
        ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_TARGET,
                               "DeviceType", type0);
    if (type1)
        ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_TARGET,
                               "DeviceType", type1);

    ensure_dummy_driver();
    assert_int_equal(
        cbx_manager_init_with_dbus(&f->mgr, NULL, f->backend, f->mock.bus),
        0);
}

static int
mi_setup(void **state)
{
    mi_fixture *f = malloc(sizeof(*f));
    assert_non_null(f);
    memset(f, 0, sizeof(*f));

    /* Isolated HOME for settings tests. */
    snprintf(f->tmp_home, sizeof(f->tmp_home),
             "/tmp/cbx_mi_%d", (int)getpid());
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", f->tmp_home);
    int r0 = system(cmd);
    (void)r0;
    mkdir(f->tmp_home, 0700);
    setenv("HOME", f->tmp_home, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");

    mi_init_ctrl(f, FIXTURE_1C1T, "xb360", NULL);

    *state = f;
    return 0;
}

static int
mi_teardown(void **state)
{
    mi_fixture *f = *state;
    if (f) {
        cbx_manager_shutdown(&f->mgr);
        ip_dbus_mock_free(&f->mock);

        char cmd[8192];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", f->tmp_home);
        int r = system(cmd);
        (void)r;
        unsetenv("HOME");
        unsetenv("XDG_CONFIG_HOME");
        unsetenv("XDG_DATA_HOME");
        free(f);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Tab switching (M01–M03)                                          */
/* ------------------------------------------------------------------ */

/* M01–M03 controller path: Left/Right from tabbar switches tabs. */
static void
test_tab_switch_controller_path(void **state)
{
    mi_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;

    /* Start on Controllers (tab 0). */
    assert_int_equal(cbx_manager_active_tab(mgr), CBX_MGR_TAB_CONTROLLERS);

    /* RIGHT → Profiles (tab 1). */
    assert_true(send_key_dn(mgr, SDLK_RIGHT));
    assert_int_equal(cbx_manager_active_tab(mgr), CBX_MGR_TAB_PROFILES);

    /* RIGHT → Settings (tab 2). */
    assert_true(send_key_dn(mgr, SDLK_RIGHT));
    assert_int_equal(cbx_manager_active_tab(mgr), CBX_MGR_TAB_SETTINGS);

    /* LEFT → Profiles (tab 1). */
    assert_true(send_key_dn(mgr, SDLK_LEFT));
    assert_int_equal(cbx_manager_active_tab(mgr), CBX_MGR_TAB_PROFILES);

    /* LEFT → Controllers (tab 0). */
    assert_true(send_key_dn(mgr, SDLK_LEFT));
    assert_int_equal(cbx_manager_active_tab(mgr), CBX_MGR_TAB_CONTROLLERS);
}

/* M01–M03 pointer path: mouse click on each tab rect switches tabs. */
static void
test_tab_switch_pointer_path(void **state)
{
    mi_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;

    SDL_Rect tb_rect;
    cbx_widget_get_rect(&mgr->tabbar.base, &tb_rect);
    int tab_w = tb_rect.w / CBX_MGR_TAB_COUNT;

    /* Click on Profiles tab (index 1). */
    int px = tb_rect.x + tab_w * 1 + tab_w / 2;
    int py = tb_rect.y + tb_rect.h / 2;
    send_mouse_click(mgr, px, py);
    assert_int_equal(cbx_manager_active_tab(mgr), CBX_MGR_TAB_PROFILES);

    /* Click on Settings tab (index 2). */
    px = tb_rect.x + tab_w * 2 + tab_w / 2;
    send_mouse_click(mgr, px, py);
    assert_int_equal(cbx_manager_active_tab(mgr), CBX_MGR_TAB_SETTINGS);

    /* Click on Controllers tab (index 0). */
    px = tb_rect.x + tab_w / 2;
    send_mouse_click(mgr, px, py);
    assert_int_equal(cbx_manager_active_tab(mgr), CBX_MGR_TAB_CONTROLLERS);
}

/* ------------------------------------------------------------------ */
/*  Controllers tab — Device list (M04)                              */
/* ------------------------------------------------------------------ */

/* M04 controller path: Down from tabbar → list gets focus, Up/Down
 * selects items. */
static void
test_ctrl_list_select_controller_path(void **state)
{
    mi_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_controllers_tab *ct = cbx_manager_controllers_tab(mgr);

    /* DOWN from tabbar → device list gets focus. */
    send_key_dn(mgr, SDLK_DOWN);
    assert_true(ct->device_list.base.focused);

    /* List has 1 item, selected = 0.  DOWN at bottom returns false →
     * focus chain takes over.  But first verify list selection state. */
    assert_int_equal(cbx_list_get_selected(&ct->device_list), 0);

    /* UP from list → tabbar. */
    send_key_dn(mgr, SDLK_UP);
    assert_true(mgr->tabbar.base.focused);
}

/* M04 pointer path: mouse click on a list item selects it. */
static void
test_ctrl_list_select_pointer_path(void **state)
{
    mi_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_controllers_tab *ct = cbx_manager_controllers_tab(mgr);

    int cx, cy;
    widget_center(&ct->device_list.base, &cx, &cy);
    send_mouse_click(mgr, cx, cy);
    assert_true(ct->device_list.base.focused);
}

/* ------------------------------------------------------------------ */
/*  Controllers tab — Add button (M05) + Type picker confirm (M08)  */
/* ------------------------------------------------------------------ */

/* M05 controller path: navigate to Add, press A → type picker opens. */
static void
test_ctrl_add_open_controller_path(void **state)
{
    mi_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_controllers_tab *ct = cbx_manager_controllers_tab(mgr);

    nav_to_buttons(mgr);       /* tabbar → list → buttons */
    nav_to_button(mgr, 0);     /* Add is leftmost */
    send_key_press(mgr, SDLK_a);

    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_TYPE_PICK);
    assert_true(ct->type_picker.base.visible);
}

/* M05 pointer path: click on Add button → type picker opens. */
static void
test_ctrl_add_open_pointer_path(void **state)
{
    mi_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_controllers_tab *ct = cbx_manager_controllers_tab(mgr);

    int cx, cy;
    widget_center(&ct->add_btn.base, &cx, &cy);
    send_mouse_click(mgr, cx, cy);

    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_TYPE_PICK);
    assert_true(ct->type_picker.base.visible);
}

/* M08 controller path: open picker → navigate → A to confirm →
 * CreateTargetDevice called → device count changes. */
static void
test_ctrl_add_confirm_controller_path(void **state)
{
    mi_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_controllers_tab *ct = cbx_manager_controllers_tab(mgr);

    /* Open the type picker. */
    nav_to_buttons(mgr);
    nav_to_button(mgr, 0);
    send_key_press(mgr, SDLK_a);
    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_TYPE_PICK);

    /* Set expectations for the Add + refresh. */
    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                           "CreateTargetDevice",
                           "/org/shadowblip/InputPlumber/devices/target/"
                           "gamepad1");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_OBJECT_MANAGER,
                           "GetManagedObjects", FIXTURE_2C2T);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_TARGET,
                           "DeviceType", "xb360");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_TARGET,
                           "DeviceType", "ds5");

    int before = cbx_controllers_tab_device_count(ct);

    /* Navigate type picker and confirm. */
    send_key_dn(mgr, SDLK_DOWN);  /* select second type */
    send_key_press(mgr, SDLK_a);  /* confirm → on_select → confirm_type_pick */

    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_LIST);
    assert_int_equal(cbx_controllers_tab_device_count(ct), before + 1);
}

/* M08 pointer path: click Add → click type item → confirm →
 * CreateTargetDevice called → device count changes. */
static void
test_ctrl_add_confirm_pointer_path(void **state)
{
    mi_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_controllers_tab *ct = cbx_manager_controllers_tab(mgr);

    /* Click Add button to open picker. */
    int cx, cy;
    widget_center(&ct->add_btn.base, &cx, &cy);
    send_mouse_click(mgr, cx, cy);
    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_TYPE_PICK);

    /* Set expectations for Add + refresh. */
    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                           "CreateTargetDevice",
                           "/org/shadowblip/InputPlumber/devices/target/"
                           "gamepad1");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_OBJECT_MANAGER,
                           "GetManagedObjects", FIXTURE_2C2T);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_TARGET,
                           "DeviceType", "xb360");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_TARGET,
                           "DeviceType", "ds5");

    int before = cbx_controllers_tab_device_count(ct);

    /* Click on the second type picker item. */
    int px = list_center_x(&ct->type_picker);
    int py = list_item_y(&ct->type_picker, 1);
    send_mouse_click(mgr, px, py);

    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_LIST);
    assert_int_equal(cbx_controllers_tab_device_count(ct), before + 1);
}

/* ------------------------------------------------------------------ */
/*  Controllers tab — Remove button (M06)                           */
/* ------------------------------------------------------------------ */

/* M06 controller path: navigate to Remove, press A → StopTargetDevice
 * called → device count decreases. */
static void
test_ctrl_remove_controller_path(void **state)
{
    mi_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_controllers_tab *ct = cbx_manager_controllers_tab(mgr);

    /* selected_device = 0 (set during init refresh). */
    assert_int_equal(cbx_controllers_tab_selected_device(ct), 0);

    /* Expect StopTargetDevice + refresh (empty). */
    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                           "StopTargetDevice", NULL);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_OBJECT_MANAGER,
                           "GetManagedObjects", FIXTURE_EMPTY);

    int before = cbx_controllers_tab_device_count(ct);
    assert_int_equal(before, 1);

    nav_to_buttons(mgr);
    nav_to_button(mgr, 1);  /* Remove */
    send_key_press(mgr, SDLK_a);

    assert_int_equal(cbx_controllers_tab_device_count(ct), 0);
}

/* M06 pointer path: click Remove button → StopTargetDevice →
 * device count decreases. */
static void
test_ctrl_remove_pointer_path(void **state)
{
    mi_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_controllers_tab *ct = cbx_manager_controllers_tab(mgr);

    assert_int_equal(cbx_controllers_tab_selected_device(ct), 0);

    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                           "StopTargetDevice", NULL);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_OBJECT_MANAGER,
                           "GetManagedObjects", FIXTURE_EMPTY);

    int before = cbx_controllers_tab_device_count(ct);
    assert_int_equal(before, 1);

    int cx, cy;
    widget_center(&ct->remove_btn.base, &cx, &cy);
    send_mouse_click(mgr, cx, cy);

    assert_int_equal(cbx_controllers_tab_device_count(ct), 0);
}

/* ------------------------------------------------------------------ */
/*  Controllers tab — Change Type (M07) + confirm (M08)             */
/* ------------------------------------------------------------------ */

/* M07 controller path: navigate to Change Type, press A → picker opens. */
static void
test_ctrl_change_type_open_controller_path(void **state)
{
    mi_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_controllers_tab *ct = cbx_manager_controllers_tab(mgr);

    nav_to_buttons(mgr);
    nav_to_button(mgr, 2);  /* Change Type */
    send_key_press(mgr, SDLK_a);

    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_TYPE_PICK);
    assert_int_equal(ct->pending_action, CBX_CT_ACTION_CHANGE);
}

/* M07 pointer path: click Change Type → picker opens. */
static void
test_ctrl_change_type_open_pointer_path(void **state)
{
    mi_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_controllers_tab *ct = cbx_manager_controllers_tab(mgr);

    int cx, cy;
    widget_center(&ct->change_type_btn.base, &cx, &cy);
    send_mouse_click(mgr, cx, cy);

    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_TYPE_PICK);
    assert_int_equal(ct->pending_action, CBX_CT_ACTION_CHANGE);
}

/* M08 controller path (change): open picker → confirm → SetTargetDevices
 * called → mode returns to LIST. */
static void
test_ctrl_change_type_confirm_controller_path(void **state)
{
    mi_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_controllers_tab *ct = cbx_manager_controllers_tab(mgr);

    /* Open picker via Change Type. */
    nav_to_buttons(mgr);
    nav_to_button(mgr, 2);
    send_key_press(mgr, SDLK_a);
    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_TYPE_PICK);

    /* Expect SetTargetDevices + refresh. */
    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "SetTargetDevices", NULL);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_OBJECT_MANAGER,
                           "GetManagedObjects", FIXTURE_1C1T);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_TARGET,
                           "DeviceType", "ds5");

    /* Confirm with current selection (type 0 = xb360). */
    send_key_press(mgr, SDLK_a);

    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_LIST);
}

/* M08 pointer path (change): click Change Type → click type → confirm →
 * SetTargetDevices called → mode returns to LIST. */
static void
test_ctrl_change_type_confirm_pointer_path(void **state)
{
    mi_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_controllers_tab *ct = cbx_manager_controllers_tab(mgr);

    /* Click Change Type. */
    int cx, cy;
    widget_center(&ct->change_type_btn.base, &cx, &cy);
    send_mouse_click(mgr, cx, cy);
    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_TYPE_PICK);

    /* Expect SetTargetDevices + refresh. */
    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "SetTargetDevices", NULL);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_OBJECT_MANAGER,
                           "GetManagedObjects", FIXTURE_1C1T);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_TARGET,
                           "DeviceType", "ds5");

    /* Click on first type item. */
    int px = list_center_x(&ct->type_picker);
    int py = list_item_y(&ct->type_picker, 0);
    send_mouse_click(mgr, px, py);

    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_LIST);
}

/* ------------------------------------------------------------------ */
/*  Controllers tab — Type picker cancel (M09, controller only)     */
/* ------------------------------------------------------------------ */

/* M09 controller path: open picker → B to cancel → back to LIST. */
static void
test_ctrl_type_pick_cancel_controller_path(void **state)
{
    mi_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_controllers_tab *ct = cbx_manager_controllers_tab(mgr);

    /* Open picker via Add. */
    nav_to_buttons(mgr);
    nav_to_button(mgr, 0);
    send_key_press(mgr, SDLK_a);
    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_TYPE_PICK);

    /* B to cancel. */
    send_key_dn(mgr, SDLK_b);
    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_LIST);
    assert_int_equal(ct->pending_action, CBX_CT_ACTION_NONE);
}

/* ------------------------------------------------------------------ */
/*  Settings tab — list select (M21) + toggle (M22)                 */
/* ------------------------------------------------------------------ */

/* Helper: switch to Settings tab.  Focus starts on tabbar. */
static void
switch_to_settings(cbx_manager *mgr)
{
    send_key_dn(mgr, SDLK_RIGHT);  /* → Profiles */
    send_key_dn(mgr, SDLK_RIGHT);  /* → Settings */
    assert_int_equal(cbx_manager_active_tab(mgr), CBX_MGR_TAB_SETTINGS);
}

/* M21 controller path: DOWN to list, verify focus and selection. */
static void
test_settings_list_select_controller_path(void **state)
{
    mi_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_settings_tab *st = cbx_manager_settings_tab(mgr);

    switch_to_settings(mgr);

    /* DOWN: tabbar → settings list. */
    send_key_dn(mgr, SDLK_DOWN);
    assert_true(st->settings_list.base.focused);
    assert_int_equal(cbx_list_get_selected(&st->settings_list), 0);

    /* DOWN: move selection to item 1. */
    send_key_dn(mgr, SDLK_DOWN);
    assert_int_equal(cbx_list_get_selected(&st->settings_list), 1);

    /* UP: back to item 0. */
    send_key_dn(mgr, SDLK_UP);
    assert_int_equal(cbx_list_get_selected(&st->settings_list), 0);
}

/* M21 pointer path: click on a settings list item selects it. */
static void
test_settings_list_select_pointer_path(void **state)
{
    mi_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_settings_tab *st = cbx_manager_settings_tab(mgr);

    switch_to_settings(mgr);

    /* Click on second item (index 1 = theme). */
    int px = list_center_x(&st->settings_list);
    int py = list_item_y(&st->settings_list, 1);
    send_mouse_click(mgr, px, py);
    assert_int_equal(cbx_list_get_selected(&st->settings_list), 1);
    assert_true(st->settings_list.base.focused);
}

/* M22 controller path: A on launch_at_boot toggles the value. */
static void
test_settings_toggle_controller_path(void **state)
{
    mi_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_settings_tab *st = cbx_manager_settings_tab(mgr);

    switch_to_settings(mgr);
    send_key_dn(mgr, SDLK_DOWN);  /* tabbar → list, item 0 = launch_at_boot */
    assert_int_equal(cbx_list_get_selected(&st->settings_list), 0);

    bool initial = cbx_settings_tab_settings(st)->launch_at_boot;
    send_key_press(mgr, SDLK_a);
    assert_int_equal(cbx_settings_tab_settings(st)->launch_at_boot, !initial);
}

/* M22 pointer path: click on launch_at_boot row toggles the value. */
static void
test_settings_toggle_pointer_path(void **state)
{
    mi_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_settings_tab *st = cbx_manager_settings_tab(mgr);

    switch_to_settings(mgr);

    bool initial = cbx_settings_tab_settings(st)->launch_at_boot;

    /* Click on first item (launch_at_boot = index 0). */
    int px = list_center_x(&st->settings_list);
    int py = list_item_y(&st->settings_list, 0);
    send_mouse_click(mgr, px, py);

    assert_int_equal(cbx_settings_tab_settings(st)->launch_at_boot, !initial);
}

/* ------------------------------------------------------------------ */
/*  Settings tab — edit flow (M23 + M24 + M25)                       */
/* ------------------------------------------------------------------ */

/* M23+M24+M25 controller path: A on theme → enter edit → Up/Down to
 * cycle → A to confirm → value changed. */
static void
test_settings_edit_flow_controller_path(void **state)
{
    mi_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_settings_tab *st = cbx_manager_settings_tab(mgr);

    switch_to_settings(mgr);
    send_key_dn(mgr, SDLK_DOWN);  /* → list */

    /* Navigate to theme (item 1). */
    send_key_dn(mgr, SDLK_DOWN);
    assert_int_equal(cbx_list_get_selected(&st->settings_list),
                     CBX_ST_SET_THEME);

    /* A → enter edit mode (fires on_select → st->selected = 1 → activate). */
    send_key_press(mgr, SDLK_a);
    assert_int_equal(cbx_settings_tab_mode(st), CBX_ST_MODE_EDIT);

    /* DOWN → cycle theme value. */
    char before[256];
    snprintf(before, sizeof(before), "%s",
             cbx_settings_tab_settings(st)->theme);
    send_key_dn(mgr, SDLK_DOWN);
    const char *after = cbx_settings_tab_settings(st)->theme;
    assert_string_not_equal(before, after);

    /* A → confirm edit → mode returns to LIST. */
    send_key_press(mgr, SDLK_a);
    assert_int_equal(cbx_settings_tab_mode(st), CBX_ST_MODE_LIST);
}

/* M23 pointer path: click on theme row → enter edit mode. */
static void
test_settings_edit_enter_pointer_path(void **state)
{
    mi_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_settings_tab *st = cbx_manager_settings_tab(mgr);

    switch_to_settings(mgr);

    /* Click on theme (item 1) → on_select → activate → edit mode. */
    int px = list_center_x(&st->settings_list);
    int py = list_item_y(&st->settings_list, 1);
    send_mouse_click(mgr, px, py);

    assert_int_equal(cbx_settings_tab_selected(st), CBX_ST_SET_THEME);
    assert_int_equal(cbx_settings_tab_mode(st), CBX_ST_MODE_EDIT);
}

/* ------------------------------------------------------------------ */
/*  Settings tab — cancel edit (M26 / D05)                            */
/* ------------------------------------------------------------------ */

/* M26/D05 controller path: enter edit → change value → B to cancel →
 * value reverts from disk. */
static void
test_settings_cancel_edit_controller_path(void **state)
{
    mi_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_settings_tab *st = cbx_manager_settings_tab(mgr);

    switch_to_settings(mgr);
    send_key_dn(mgr, SDLK_DOWN);  /* → list */
    send_key_dn(mgr, SDLK_DOWN);  /* → theme (item 1) */
    assert_int_equal(cbx_list_get_selected(&st->settings_list),
                     CBX_ST_SET_THEME);

    const char *original = cbx_settings_tab_settings(st)->theme;
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", original);

    /* Enter edit. */
    send_key_press(mgr, SDLK_a);
    assert_int_equal(cbx_settings_tab_mode(st), CBX_ST_MODE_EDIT);

    /* Change value. */
    send_key_dn(mgr, SDLK_DOWN);
    assert_string_not_equal(cbx_settings_tab_settings(st)->theme, saved);

    /* B → cancel → value reverts. */
    send_key_dn(mgr, SDLK_b);
    assert_int_equal(cbx_settings_tab_mode(st), CBX_ST_MODE_LIST);
    assert_string_equal(cbx_settings_tab_settings(st)->theme, saved);
}

/* M23 pointer + M26 controller: enter edit via click, then B to cancel
 * → value reverts. */
static void
test_settings_cancel_edit_pointer_path(void **state)
{
    mi_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_settings_tab *st = cbx_manager_settings_tab(mgr);

    switch_to_settings(mgr);

    const char *original = cbx_settings_tab_settings(st)->theme;
    char saved[256];
    snprintf(saved, sizeof(saved), "%s", original);

    /* Click on theme row → enters edit mode. */
    int px = list_center_x(&st->settings_list);
    int py = list_item_y(&st->settings_list, 1);
    send_mouse_click(mgr, px, py);
    assert_int_equal(cbx_settings_tab_selected(st), CBX_ST_SET_THEME);
    assert_int_equal(cbx_settings_tab_mode(st), CBX_ST_MODE_EDIT);

    /* Change value via keyboard. */
    send_key_dn(mgr, SDLK_DOWN);
    assert_string_not_equal(cbx_settings_tab_settings(st)->theme, saved);

    /* B → cancel → value reverts. */
    send_key_dn(mgr, SDLK_b);
    assert_int_equal(cbx_settings_tab_mode(st), CBX_ST_MODE_LIST);
    assert_string_equal(cbx_settings_tab_settings(st)->theme, saved);
}

/* ------------------------------------------------------------------ */
/*  Settings tab — save (M27)                                        */
/* ------------------------------------------------------------------ */

/* Helper: build the expected settings.yaml path. */
static void
settings_yaml_path(mi_fixture *f, char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "%s/.config/controller-box/settings.yaml",
             f->tmp_home);
}

/* M27 controller path: toggle a setting, navigate to Save, press A →
 * settings.yaml written. */
static void
test_settings_save_controller_path(void **state)
{
    mi_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_settings_tab *st = cbx_manager_settings_tab(mgr);

    switch_to_settings(mgr);
    send_key_dn(mgr, SDLK_DOWN);  /* → list, item 0 = launch_at_boot */

    /* Toggle launch_at_boot (A on row 0). */
    bool initial = cbx_settings_tab_settings(st)->launch_at_boot;
    send_key_press(mgr, SDLK_a);
    assert_int_equal(cbx_settings_tab_settings(st)->launch_at_boot,
                     !initial);

    /* Navigate to Save button (below the list).  First navigate to
     * the bottom of the list (9 DOWNs to item 9 = Save row), then
     * one more DOWN to fall through to the save_btn. */
    for (int i = 0; i < 9; i++)
        send_key_dn(mgr, SDLK_DOWN);
    send_key_dn(mgr, SDLK_DOWN);  /* list bottom → focus to save_btn */
    assert_true(st->save_btn.base.focused);

    /* Press A to save. */
    send_key_press(mgr, SDLK_a);

    /* Verify settings.yaml was written. */
    char path[4096 + 128];
    settings_yaml_path(f, path, sizeof(path));
    assert_int_equal(access(path, F_OK), 0);
    assert_string_equal(cbx_settings_tab_status(st), "Settings saved.");
}

/* M27 pointer path: toggle a setting via click, then click Save →
 * settings.yaml written. */
static void
test_settings_save_pointer_path(void **state)
{
    mi_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_settings_tab *st = cbx_manager_settings_tab(mgr);

    switch_to_settings(mgr);

    bool initial = cbx_settings_tab_settings(st)->launch_at_boot;

    /* Click on launch_at_boot row (item 0) to toggle. */
    int px = list_center_x(&st->settings_list);
    int py = list_item_y(&st->settings_list, 0);
    send_mouse_click(mgr, px, py);
    assert_int_equal(cbx_settings_tab_settings(st)->launch_at_boot,
                     !initial);

    /* Click on Save button. */
    int cx, cy;
    widget_center(&st->save_btn.base, &cx, &cy);
    send_mouse_click(mgr, cx, cy);

    /* Verify settings.yaml was written. */
    char path[4096 + 128];
    settings_yaml_path(f, path, sizeof(path));
    assert_int_equal(access(path, F_OK), 0);
    assert_string_equal(cbx_settings_tab_status(st), "Settings saved.");
}

/* ------------------------------------------------------------------ */
/*  Disabled / degraded scenarios                                    */
/* ------------------------------------------------------------------ */

/* D01: InputPlumber unavailable — mock with no expectations → all DBus
 * calls fail.  Add button activation doesn't open the type picker.
 * Both controller and pointer paths tested. */
static void
test_d01_inputplumber_unavailable(void **state)
{
    (void)state;
    ip_dbus_mock mock;
    ip_dbus_mock_init(&mock);
    const ip_dbus_backend *backend = ip_dbus_mock_backend(&mock);

    /* No expectations → all DBus calls return -ENXIO.  Init still
     * succeeds (load_supported_types + refresh fail gracefully). */
    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(
        cbx_manager_init_with_dbus(&mgr, NULL, backend, mock.bus), 0);

    cbx_controllers_tab *ct = cbx_manager_controllers_tab(&mgr);
    assert_int_equal(cbx_controllers_tab_supported_type_count(ct), 0);

    /* Controller path: navigate to Add, press A → mode stays LIST. */
    nav_to_buttons(&mgr);
    nav_to_button(&mgr, 0);
    send_key_press(&mgr, SDLK_a);
    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_LIST);

    /* Pointer path: click Add → mode stays LIST. */
    int cx, cy;
    widget_center(&ct->add_btn.base, &cx, &cy);
    send_mouse_click(&mgr, cx, cy);
    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_LIST);

    cbx_manager_shutdown(&mgr);
    ip_dbus_mock_free(&mock);
}

/* D02: Remove with no device — empty device list, Remove activation
 * produces no DBus side effect.  Both paths tested. */
static void
test_d02_remove_no_device(void **state)
{
    (void)state;
    ip_dbus_mock mock;
    ip_dbus_mock_init(&mock);
    const ip_dbus_backend *backend = ip_dbus_mock_backend(&mock);

    ip_dbus_mock_expect_ok(&mock, IP_IFACE_MANAGER,
                           "SupportedTargetDeviceIds", SUPPORTED_TYPES);
    ip_dbus_mock_expect_ok(&mock, IP_IFACE_OBJECT_MANAGER,
                           "GetManagedObjects", FIXTURE_EMPTY);

    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(
        cbx_manager_init_with_dbus(&mgr, NULL, backend, mock.bus), 0);

    cbx_controllers_tab *ct = cbx_manager_controllers_tab(&mgr);
    assert_int_equal(cbx_controllers_tab_device_count(ct), 0);
    assert_int_equal(cbx_controllers_tab_selected_device(ct), -1);

    /* Controller path: navigate to Remove, press A → no effect. */
    nav_to_buttons(&mgr);
    nav_to_button(&mgr, 1);
    send_key_press(&mgr, SDLK_a);
    assert_int_equal(cbx_controllers_tab_device_count(ct), 0);

    /* Pointer path: click Remove → no effect. */
    int cx, cy;
    widget_center(&ct->remove_btn.base, &cx, &cy);
    send_mouse_click(&mgr, cx, cy);
    assert_int_equal(cbx_controllers_tab_device_count(ct), 0);

    cbx_manager_shutdown(&mgr);
    ip_dbus_mock_free(&mock);
}

/* D06: DBus operation failure — CreateTargetDevice returns error.
 * Error handling: mode returns to LIST, device count unchanged.
 * Both controller and pointer paths tested. */
static void
test_d06_dbus_failure(void **state)
{
    mi_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_controllers_tab *ct = cbx_manager_controllers_tab(mgr);

    assert_int_equal(cbx_controllers_tab_device_count(ct), 1);

    /* --- Controller path --- */

    /* Open picker via Add. */
    nav_to_buttons(mgr);
    nav_to_button(mgr, 0);
    send_key_press(mgr, SDLK_a);
    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_TYPE_PICK);

    /* Set CreateTargetDevice to fail. */
    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_MANAGER,
                              "CreateTargetDevice", -EIO);

    /* Confirm → CreateTargetDevice fails → mode returns to LIST. */
    send_key_press(mgr, SDLK_a);
    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_LIST);
    assert_int_equal(cbx_controllers_tab_device_count(ct), 1);

    /* --- Pointer path --- */

    /* Open picker via Add click. */
    int cx, cy;
    widget_center(&ct->add_btn.base, &cx, &cy);
    send_mouse_click(mgr, cx, cy);
    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_TYPE_PICK);

    /* Set CreateTargetDevice to fail again. */
    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_MANAGER,
                              "CreateTargetDevice", -EIO);

    /* Click type item → confirm fails → mode returns to LIST. */
    int px = list_center_x(&ct->type_picker);
    int py = list_item_y(&ct->type_picker, 0);
    send_mouse_click(mgr, px, py);
    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_LIST);
    assert_int_equal(cbx_controllers_tab_device_count(ct), 1);
}

/* ------------------------------------------------------------------ */
/*  Runner                                                            */
/* ------------------------------------------------------------------ */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* Tab switching (M01–M03) */
        cmocka_unit_test_setup_teardown(
            test_tab_switch_controller_path, mi_setup, mi_teardown),
        cmocka_unit_test_setup_teardown(
            test_tab_switch_pointer_path, mi_setup, mi_teardown),

        /* Controllers tab — device list (M04) */
        cmocka_unit_test_setup_teardown(
            test_ctrl_list_select_controller_path, mi_setup, mi_teardown),
        cmocka_unit_test_setup_teardown(
            test_ctrl_list_select_pointer_path, mi_setup, mi_teardown),

        /* Controllers tab — Add (M05) + confirm (M08) */
        cmocka_unit_test_setup_teardown(
            test_ctrl_add_open_controller_path, mi_setup, mi_teardown),
        cmocka_unit_test_setup_teardown(
            test_ctrl_add_open_pointer_path, mi_setup, mi_teardown),
        cmocka_unit_test_setup_teardown(
            test_ctrl_add_confirm_controller_path, mi_setup, mi_teardown),
        cmocka_unit_test_setup_teardown(
            test_ctrl_add_confirm_pointer_path, mi_setup, mi_teardown),

        /* Controllers tab — Remove (M06) */
        cmocka_unit_test_setup_teardown(
            test_ctrl_remove_controller_path, mi_setup, mi_teardown),
        cmocka_unit_test_setup_teardown(
            test_ctrl_remove_pointer_path, mi_setup, mi_teardown),

        /* Controllers tab — Change Type (M07) + confirm (M08) */
        cmocka_unit_test_setup_teardown(
            test_ctrl_change_type_open_controller_path,
            mi_setup, mi_teardown),
        cmocka_unit_test_setup_teardown(
            test_ctrl_change_type_open_pointer_path,
            mi_setup, mi_teardown),
        cmocka_unit_test_setup_teardown(
            test_ctrl_change_type_confirm_controller_path,
            mi_setup, mi_teardown),
        cmocka_unit_test_setup_teardown(
            test_ctrl_change_type_confirm_pointer_path,
            mi_setup, mi_teardown),

        /* Controllers tab — Type picker cancel (M09) */
        cmocka_unit_test_setup_teardown(
            test_ctrl_type_pick_cancel_controller_path,
            mi_setup, mi_teardown),

        /* Settings tab — list (M21) + toggle (M22) */
        cmocka_unit_test_setup_teardown(
            test_settings_list_select_controller_path,
            mi_setup, mi_teardown),
        cmocka_unit_test_setup_teardown(
            test_settings_list_select_pointer_path,
            mi_setup, mi_teardown),
        cmocka_unit_test_setup_teardown(
            test_settings_toggle_controller_path,
            mi_setup, mi_teardown),
        cmocka_unit_test_setup_teardown(
            test_settings_toggle_pointer_path,
            mi_setup, mi_teardown),

        /* Settings tab — edit flow (M23+M24+M25) */
        cmocka_unit_test_setup_teardown(
            test_settings_edit_flow_controller_path,
            mi_setup, mi_teardown),
        cmocka_unit_test_setup_teardown(
            test_settings_edit_enter_pointer_path,
            mi_setup, mi_teardown),

        /* Settings tab — cancel edit (M26 / D05) */
        cmocka_unit_test_setup_teardown(
            test_settings_cancel_edit_controller_path,
            mi_setup, mi_teardown),
        cmocka_unit_test_setup_teardown(
            test_settings_cancel_edit_pointer_path,
            mi_setup, mi_teardown),

        /* Settings tab — save (M27) */
        cmocka_unit_test_setup_teardown(
            test_settings_save_controller_path,
            mi_setup, mi_teardown),
        cmocka_unit_test_setup_teardown(
            test_settings_save_pointer_path,
            mi_setup, mi_teardown),

        /* Disabled / degraded scenarios */
        cmocka_unit_test(test_d01_inputplumber_unavailable),
        cmocka_unit_test(test_d02_remove_no_device),
        cmocka_unit_test_setup_teardown(
            test_d06_dbus_failure, mi_setup, mi_teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}