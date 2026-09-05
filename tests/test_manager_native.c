/*
 * test_manager_native.c — Manager Controllers + Settings interaction
 * acceptance with native DBus backend (Task 4).
 *
 * Exercises M04, M09, M21, M23–M26, MG-04 (topology failure), MG-15
 * (post-resize hit testing), and D06 (DBus failure) through production
 * dispatch (cbx_manager_handle_event) with a real sd-bus backend
 * connected to a private InputPlumber-compatible native-signature DBus
 * server.  No ip_dbus_mock backend used.
 *
 * Controller path: SDL_JoystickSetVirtualButton → SDL_CONTROLLERBUTTONDOWN
 * → cbx_manager_controller_to_key → cbx_manager_handle_event (same path
 * as production gamepad input).
 *
 * Pointer path: SDL_MOUSEMOTION/MOUSEBUTTONDOWN/MOUSEBUTTONUP →
 * cbx_manager_handle_mouse_event → hit_test → focus + dispatch.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <limits.h>

#include <SDL.h>

#include "dbus/dbus_client.h"
#include "dbus/ip_connection.h"
#include "dbus/ip_manager.h"
#include "dbus/ip_target.h"
#include "dbus/ip_composite.h"
#include "dbus/ip_objectmanager.h"
#include "dbus/ip_device_model.h"
#include "dbus/dbus_interface.h"             /* IP_DBUS_PATH, IP_IFACE_* — constants only */

#include "config/config_settings.h"
#include "config/config_paths.h"

#include "manager/manager.h"
#include "manager/controllers_tab.h"
#include "manager/settings_tab.h"
#include "ui/widget.h"

/* ================================================================== */
/*  Compile-time configuration                                         */
/* ================================================================== */

#ifndef DBUS_SESSION_CONFIG
#error "DBUS_SESSION_CONFIG must be defined (path to session.conf)"
#endif

/* ================================================================== */
/*  Native IP server (shared implementation)                           */
/* ================================================================== */

#include "native_ip_server.h"

/* ================================================================== */
/*  Fixture                                                            */
/* ================================================================== */

typedef struct {
    char    bus_address[512];
    pid_t   daemon_pid;
    pid_t   server_pid;
    char    tmp_home[PATH_MAX];
    int     joy_device_index;
    SDL_Joystick *joystick;
    const ip_dbus_backend *backend;
    ip_bus_handle bus;
} mn_fixture;

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static void
pump_manager(cbx_manager *mgr)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
        cbx_manager_handle_event(mgr, &ev);
}

/* Send a controller button press+release through the manager. */
static void
ctrl_press(cbx_manager *mgr, SDL_Joystick *joy, int button)
{
    SDL_JoystickSetVirtualButton(joy, button, 1);
    SDL_PumpEvents();
    pump_manager(mgr);

    SDL_JoystickSetVirtualButton(joy, button, 0);
    SDL_PumpEvents();
    pump_manager(mgr);
}

static bool
send_key_dn(cbx_manager *mgr, SDL_Keycode sym)
{
    SDL_Event ev = {0};
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = sym;
    return cbx_manager_handle_event(mgr, &ev);
}

static void
send_key_up(cbx_manager *mgr, SDL_Keycode sym)
{
    SDL_Event ev = {0};
    ev.type = SDL_KEYUP;
    ev.key.keysym.sym = sym;
    cbx_manager_handle_event(mgr, &ev);
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

static bool
send_window_resize(cbx_manager *mgr, int new_w, int new_h)
{
    SDL_Event ev = {0};
    ev.type = SDL_WINDOWEVENT;
    ev.window.event = SDL_WINDOWEVENT_RESIZED;
    ev.window.data1 = new_w;
    ev.window.data2 = new_h;
    return cbx_manager_handle_event(mgr, &ev);
}

static void
widget_center(const cbx_widget *w, int *cx, int *cy)
{
    assert_non_null(w);
    *cx = w->rect.x + w->rect.w / 2;
    *cy = w->rect.y + w->rect.h / 2;
}

static int
list_item_y(const cbx_list *lst, int index)
{
    return lst->base.rect.y + index * lst->item_h + lst->item_h / 2;
}

static int
list_center_x(const cbx_list *lst)
{
    return lst->base.rect.x + lst->base.rect.w / 2;
}

/* Navigate to a settings list item by index via gamepad.
 * Switches to Settings tab first, then DOWN to list, then DOWNs to item. */
static void
nav_to_setting_ctrl(cbx_manager *mgr, SDL_Joystick *joy, int index)
{
    /* Switch to Settings tab: D-pad Right × 2 */
    ctrl_press(mgr, joy, 14);  /* Right → Profiles */
    ctrl_press(mgr, joy, 14);  /* Right → Settings */
    assert_int_equal(cbx_manager_active_tab(mgr), CBX_MGR_TAB_SETTINGS);
    /* Down: tabbar → list (selected=0) */
    ctrl_press(mgr, joy, 12);
    /* More downs to reach desired index */
    for (int i = 0; i < index; i++)
        ctrl_press(mgr, joy, 12);
}

/* Wait for server readiness by polling Version property. */
static int
wait_for_server(const ip_dbus_backend *backend, ip_bus_handle bus,
                const char *version)
{
    char *value = NULL;
    int rc = -1;
    for (int i = 0; i < 200 && rc != 0; i++) {
        free(value); value = NULL;
        rc = backend->get_property(bus, IP_DBUS_NAME,
            "/org/shadowblip/InputPlumber/Manager",
            IP_IFACE_MANAGER, "Version", &value);
        if (rc != 0) usleep(10000);
    }
    if (rc == 0 && version)
        rc = (value && strcmp(value, version) == 0) ? 0 : -1;
    free(value);
    return rc;
}

/* ================================================================== */
/*  Setup / Teardown                                                   */
/* ================================================================== */

static void
mn_setup_common(mn_fixture *f, bool fail_create, bool delayed)
{
    /* Isolated HOME */
    snprintf(f->tmp_home, sizeof(f->tmp_home),
             "/tmp/cbx_mn_%d", (int)getpid());
    /* Remove stale copy if exists */
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", f->tmp_home);
    int sysrc = system(cmd);
    (void)sysrc;
    mkdir(f->tmp_home, 0700);
    setenv("HOME", f->tmp_home, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");
    unsetenv("FLATPAK_ID");

    /* Private dbus-daemon */
    assert_int_equal(nip_start_private_bus(f->bus_address,
                                            sizeof(f->bus_address),
                                            &f->daemon_pid), 0);
    setenv("DBUS_SYSTEM_BUS_ADDRESS", f->bus_address, 1);

    /* Configure server */
    nip_reset_server_state(2);
    for (int i = 0; i < 2; i++) {
        snprintf(g_nip_comp_names[i], sizeof(g_nip_comp_names[i]),
                 "TestController%d", i);
        snprintf(g_nip_dbus_devices[i], sizeof(g_nip_dbus_devices[i]),
                 "/org/shadowblip/InputPlumber/CompositeDevice%d", i);
        snprintf(g_nip_persistent_ids[i], sizeof(g_nip_persistent_ids[i]),
                 "ORDER:%d", i);
    }
    g_nip_intercept_mode[0] = 0;
    g_nip_intercept_mode[1] = 0;
    if (fail_create)
        g_nip_fail_next_create = 1;

    const nip_server_config cfg = {
        .num_composites = 2, .version = "0.78.0",
        .publication_delay_ms = delayed ? 20u : 0u,
        .removal_delay_ms = delayed ? 20u : 0u
    };
    f->server_pid = nip_fork_server(f->bus_address, &cfg);
    assert_true(f->server_pid > 0);

    /* Wait for server */
    f->backend = ip_dbus_sd_backend();
    f->bus = NULL;
    assert_int_equal(f->backend->connect(&f->bus), 0);
    assert_int_equal(wait_for_server(f->backend, f->bus, "0.78.0"), 0);

    /* SDL init + virtual gamepad */
    SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "dummy", SDL_HINT_OVERRIDE);
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);

    f->joy_device_index = SDL_JoystickAttachVirtual(
        SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 15, 0);
    assert_true(f->joy_device_index >= 0);

    f->joystick = SDL_JoystickOpen(f->joy_device_index);
    assert_non_null(f->joystick);

    char guid[33];
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(f->joystick), guid, sizeof(guid));
    char mapping[512];
    snprintf(mapping, sizeof(mapping),
             "%s,Controller-Box Virtual,a:b0,b:b1,start:b6,"
             "dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,platform:Linux,",
             guid);
    assert_true(SDL_GameControllerAddMapping(mapping) >= 0);
    SDL_GameControllerEventState(SDL_ENABLE);
}

static int
mn_setup(void **state)
{
    mn_fixture *f = calloc(1, sizeof(*f));
    assert_non_null(f);
    f->joy_device_index = -1;
    f->daemon_pid = -1;
    f->server_pid = -1;
    mn_setup_common(f, false, false);
    *state = f;
    return 0;
}

static int
mn_setup_fail(void **state)
{
    mn_fixture *f = calloc(1, sizeof(*f));
    assert_non_null(f);
    f->joy_device_index = -1;
    f->daemon_pid = -1;
    f->server_pid = -1;
    mn_setup_common(f, true, false);
    *state = f;
    return 0;
}

static int
mn_setup_delayed(void **state)
{
    mn_fixture *f = calloc(1, sizeof(*f));
    assert_non_null(f);
    f->joy_device_index = -1;
    f->daemon_pid = -1;
    f->server_pid = -1;
    mn_setup_common(f, false, true);
    *state = f;
    return 0;
}

static int
mn_teardown(void **state)
{
    mn_fixture *f = *state;
    if (f->bus) f->backend->disconnect(f->bus);
    if (f->joystick) SDL_JoystickClose(f->joystick);
    if (f->joy_device_index >= 0)
        SDL_JoystickDetachVirtual(f->joy_device_index);
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    SDL_Quit();
    if (f->server_pid > 1) { kill(f->server_pid, SIGTERM); waitpid(f->server_pid, NULL, 0); }
    if (f->daemon_pid > 1) { kill(f->daemon_pid, SIGTERM); waitpid(f->daemon_pid, NULL, 0); }
    unsetenv("DBUS_SYSTEM_BUS_ADDRESS");
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", f->tmp_home);
    int sysrc = system(cmd);
    (void)sysrc;
    unsetenv("HOME");
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");
    free(f);
    return 0;
}

/* ================================================================== */
/*  M04 — Controllers tab device list select                           */
/* ================================================================== */

/* M04 controller path: D-pad Down to list, verify focus + selection. */
static void
test_m04_list_select_controller(void **state)
{
    mn_fixture *f = *state;
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);
    assert_true(mgr.dbus_connected);
    pump_manager(&mgr);

    cbx_controllers_tab *ct = cbx_manager_controllers_tab(&mgr);

    /* D-pad Down → device list */
    ctrl_press(&mgr, f->joystick, 12);
    assert_true(ct->device_list.base.focused);

    /* D-pad Up → back to tabbar */
    ctrl_press(&mgr, f->joystick, 11);
    assert_true(mgr.tabbar.base.focused);

    cbx_manager_shutdown(&mgr);
}

/* M04 pointer path: click on device list → list focused. */
static void
test_m04_list_select_pointer(void **state)
{
    (void)state;  /* fixture not needed — manager inits/shuts down per test */
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);
    assert_true(mgr.dbus_connected);
    pump_manager(&mgr);

    cbx_controllers_tab *ct = cbx_manager_controllers_tab(&mgr);

    int cx, cy;
    widget_center(&ct->device_list.base, &cx, &cy);
    send_mouse_click(&mgr, cx, cy);
    assert_true(ct->device_list.base.focused);

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  Atomic mutations with delayed native publication/removal           */
/* ================================================================== */

static int native_target_count(mn_fixture *f)
{
    cbx_device_model model;
    assert_int_equal(cbx_objectmanager_enumerate(f->backend, f->bus, &model), 0);
    return model.target_count;
}

static void pointer_add_first_type(cbx_manager *mgr)
{
    cbx_controllers_tab *ct = cbx_manager_controllers_tab(mgr);
    int x, y;
    widget_center(&ct->add_btn.base, &x, &y);
    send_mouse_click(mgr, x, y);
    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_TYPE_PICK);
    send_mouse_click(mgr, list_center_x(&ct->type_picker),
                     list_item_y(&ct->type_picker, 0));
}

static void test_delayed_add_remove_pointer_atomic(void **state)
{
    mn_fixture *f = *state;
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);
    pump_manager(&mgr);
    assert_int_equal(native_target_count(f), 0);

    pointer_add_first_type(&mgr);
    assert_int_equal(native_target_count(f), 1);
    cbx_controllers_tab *ct = cbx_manager_controllers_tab(&mgr);
    assert_int_equal(cbx_controllers_tab_device_count(ct), 1);

    send_mouse_click(&mgr, list_center_x(&ct->device_list),
                     list_item_y(&ct->device_list, 0));
    int x, y;
    widget_center(&ct->remove_btn.base, &x, &y);
    send_mouse_click(&mgr, x, y);
    assert_int_equal(native_target_count(f), 0);
    assert_int_equal(cbx_controllers_tab_device_count(ct), 0);
    cbx_manager_shutdown(&mgr);
}

static void test_target_limit_checked_before_create(void **state)
{
    mn_fixture *f = *state;
    for (int i = 0; i < CBX_MAX_CONTROLLERS; i++) {
        char *path = NULL;
        assert_int_equal(ip_manager_create_target_device(f->backend, f->bus,
            "xb360", &path), 0);
        free(path);
    }
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);
    pump_manager(&mgr);
    assert_int_equal(native_target_count(f), CBX_MAX_CONTROLLERS);
    pointer_add_first_type(&mgr);
    assert_int_equal(native_target_count(f), CBX_MAX_CONTROLLERS);
    assert_non_null(strstr(cbx_manager_controllers_tab(&mgr)->status_lbl.text,
                           "Add failed"));
    cbx_manager_shutdown(&mgr);
}

static void test_settings_write_failure_leaks_no_target(void **state)
{
    mn_fixture *f = *state;
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);
    pump_manager(&mgr);

    char blocker[PATH_MAX + 32];
    snprintf(blocker, sizeof(blocker), "%s/not-a-directory", f->tmp_home);
    FILE *fp = fopen(blocker, "w");
    assert_non_null(fp);
    fclose(fp);
    setenv("XDG_CONFIG_HOME", blocker, 1);

    pointer_add_first_type(&mgr);
    assert_int_equal(native_target_count(f), 0);
    assert_int_equal(cbx_controllers_tab_device_count(
                         cbx_manager_controllers_tab(&mgr)), 0);
    assert_non_null(strstr(cbx_manager_controllers_tab(&mgr)->status_lbl.text,
                           "Add failed"));
    unsetenv("XDG_CONFIG_HOME");
    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  M09 — Type picker cancel (B key)                                    */
/* ================================================================== */

/* M09 controller path: open type picker, B to cancel. */
static void
test_m09_type_picker_cancel_controller(void **state)
{
    mn_fixture *f = *state;
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);
    assert_true(mgr.dbus_connected);
    pump_manager(&mgr);

    cbx_controllers_tab *ct = cbx_manager_controllers_tab(&mgr);

    /* Navigate to Add button */
    ctrl_press(&mgr, f->joystick, 12);  /* Down → list */
    ctrl_press(&mgr, f->joystick, 12);  /* Down → button row */
    ctrl_press(&mgr, f->joystick, 13);   /* Left → Remove */
    ctrl_press(&mgr, f->joystick, 13);   /* Left → Add */

    /* A → open type picker */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_TYPE_PICK);

    /* B → cancel */
    ctrl_press(&mgr, f->joystick, 1);
    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_LIST);

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  M21 — Settings list select                                         */
/* ================================================================== */

/* M21 controller path: navigate settings list via D-pad. */
static void
test_m21_settings_list_select_controller(void **state)
{
    mn_fixture *f = *state;
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);
    assert_true(mgr.dbus_connected);
    pump_manager(&mgr);

    cbx_settings_tab *st = cbx_manager_settings_tab(&mgr);

    /* Navigate to Settings tab */
    ctrl_press(&mgr, f->joystick, 14);  /* Right → Profiles */
    ctrl_press(&mgr, f->joystick, 14);  /* Right → Settings */
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_SETTINGS);

    /* Down → settings list (selected=0) */
    ctrl_press(&mgr, f->joystick, 12);
    assert_true(st->settings_list.base.focused);
    assert_int_equal(cbx_list_get_selected(&st->settings_list), 0);

    /* Down → item 1 */
    ctrl_press(&mgr, f->joystick, 12);
    assert_int_equal(cbx_list_get_selected(&st->settings_list), 1);

    /* Up → back to item 0 */
    ctrl_press(&mgr, f->joystick, 11);
    assert_int_equal(cbx_list_get_selected(&st->settings_list), 0);

    cbx_manager_shutdown(&mgr);
}

/* M21 pointer path: click on a settings list item. */
static void
test_m21_settings_list_select_pointer(void **state)
{
    (void)state;
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);
    assert_true(mgr.dbus_connected);
    pump_manager(&mgr);

    cbx_settings_tab *st = cbx_manager_settings_tab(&mgr);

    /* Switch to Settings tab via keyboard */
    send_key_dn(&mgr, SDLK_RIGHT);  /* → Profiles */
    send_key_dn(&mgr, SDLK_RIGHT);  /* → Settings */

    /* Click on item index 2 (opacity) */
    int px = list_center_x(&st->settings_list);
    int py = list_item_y(&st->settings_list, 2);
    send_mouse_click(&mgr, px, py);

    assert_true(st->settings_list.base.focused);
    assert_int_equal(cbx_list_get_selected(&st->settings_list), 2);

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  M23+M24+M25 — Settings edit flow: enter, adjust, confirm           */
/* ================================================================== */

/* M23+M24+M25 controller path: opacity edit flow via gamepad. */
static void
test_m23_24_25_opacity_controller(void **state)
{
    mn_fixture *f = *state;
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);
    assert_true(mgr.dbus_connected);
    pump_manager(&mgr);

    cbx_settings_tab *st = cbx_manager_settings_tab(&mgr);

    /* Navigate to Settings → opacity (index 2) */
    nav_to_setting_ctrl(&mgr, f->joystick, CBX_ST_SET_OPACITY);
    assert_int_equal(cbx_list_get_selected(&st->settings_list), CBX_ST_SET_OPACITY);

    /* M23: A → enter edit mode */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(cbx_settings_tab_mode(st), CBX_ST_MODE_EDIT);

    /* M24: Down → adjust value (opacity decreases by 0.05) */
    float before = cbx_settings_tab_settings(st)->overlay_opacity;
    ctrl_press(&mgr, f->joystick, 12);
    float after = cbx_settings_tab_settings(st)->overlay_opacity;
    assert_float_equal(after, before - 0.05f, 0.001f);

    /* M25: A → confirm edit → mode returns to LIST */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(cbx_settings_tab_mode(st), CBX_ST_MODE_LIST);

    cbx_manager_shutdown(&mgr);
}

/* M23+M24+M25 pointer path: opacity edit flow — click to enter edit. */
static void
test_m23_24_25_opacity_pointer(void **state)
{
    (void)state;
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);
    assert_true(mgr.dbus_connected);
    pump_manager(&mgr);

    cbx_settings_tab *st = cbx_manager_settings_tab(&mgr);

    /* Switch to Settings tab */
    send_key_dn(&mgr, SDLK_RIGHT);
    send_key_dn(&mgr, SDLK_RIGHT);

    /* M23: Click on opacity item (index 2) → enter edit mode */
    int px = list_center_x(&st->settings_list);
    int py = list_item_y(&st->settings_list, CBX_ST_SET_OPACITY);
    send_mouse_click(&mgr, px, py);
    assert_int_equal(cbx_settings_tab_mode(st), CBX_ST_MODE_EDIT);

    /* M24: Down → adjust value (opacity decreases by 0.05) */
    float before = cbx_settings_tab_settings(st)->overlay_opacity;
    send_key_dn(&mgr, SDLK_DOWN);
    float after = cbx_settings_tab_settings(st)->overlay_opacity;
    assert_float_equal(after, before - 0.05f, 0.001f);

    /* M25: A → confirm edit */
    send_key_press(&mgr, SDLK_a);
    assert_int_equal(cbx_settings_tab_mode(st), CBX_ST_MODE_LIST);

    cbx_manager_shutdown(&mgr);
}

/* M23+M24+M25 controller path: VC count edit flow. */
static void
test_m23_24_25_vc_count_controller(void **state)
{
    mn_fixture *f = *state;
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);
    assert_true(mgr.dbus_connected);
    pump_manager(&mgr);

    cbx_settings_tab *st = cbx_manager_settings_tab(&mgr);

    /* Navigate to VC count (index 3) */
    nav_to_setting_ctrl(&mgr, f->joystick, CBX_ST_SET_VC_COUNT);
    assert_int_equal(cbx_list_get_selected(&st->settings_list), CBX_ST_SET_VC_COUNT);

    /* M23: A → enter edit mode */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(cbx_settings_tab_mode(st), CBX_ST_MODE_EDIT);

    /* M24: Down → VC count decreases by 1 */
    int before = cbx_settings_tab_settings(st)->virtual_controllers.count;
    ctrl_press(&mgr, f->joystick, 12);
    int after = cbx_settings_tab_settings(st)->virtual_controllers.count;
    assert_int_equal(after, before - 1);

    /* M25: A → confirm */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(cbx_settings_tab_mode(st), CBX_ST_MODE_LIST);

    cbx_manager_shutdown(&mgr);
}

/* M23+M24+M25 controller path: VC type edit flow. */
static void
test_m23_24_25_vc_type_controller(void **state)
{
    mn_fixture *f = *state;
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);
    assert_true(mgr.dbus_connected);
    pump_manager(&mgr);

    cbx_settings_tab *st = cbx_manager_settings_tab(&mgr);

    /* Navigate to VC type 0 (index 4) */
    nav_to_setting_ctrl(&mgr, f->joystick, CBX_ST_SET_VC_TYPE_0);
    assert_int_equal(cbx_list_get_selected(&st->settings_list), CBX_ST_SET_VC_TYPE_0);

    /* M23: A → enter edit mode */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(cbx_settings_tab_mode(st), CBX_ST_MODE_EDIT);

    /* M24: Down → cycle to prev type (wraps: xb360 → touchscreen) */
    char before[64];
    snprintf(before, sizeof(before), "%s",
             cbx_settings_tab_settings(st)->virtual_controllers.types[0]);
    ctrl_press(&mgr, f->joystick, 12);
    const char *after = cbx_settings_tab_settings(st)->virtual_controllers.types[0];
    assert_string_equal(after, "touchscreen");

    /* M25: A → confirm */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(cbx_settings_tab_mode(st), CBX_ST_MODE_LIST);

    cbx_manager_shutdown(&mgr);
}

/* M23+M24+M25 controller path: trigger combo edit flow. */
static void
test_m23_24_25_trigger_controller(void **state)
{
    mn_fixture *f = *state;
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);
    assert_true(mgr.dbus_connected);
    pump_manager(&mgr);

    cbx_settings_tab *st = cbx_manager_settings_tab(&mgr);

    /* Navigate to trigger (index 8) */
    nav_to_setting_ctrl(&mgr, f->joystick, CBX_ST_SET_TRIGGER);
    assert_int_equal(cbx_list_get_selected(&st->settings_list), CBX_ST_SET_TRIGGER);

    /* M23: A → enter edit mode */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(cbx_settings_tab_mode(st), CBX_ST_MODE_EDIT);

    /* M24: Down → cycle trigger (wraps: Select+A → L3+R3) */
    char before[64];
    snprintf(before, sizeof(before), "%s",
             cbx_settings_tab_settings(st)->overlay_trigger);
    ctrl_press(&mgr, f->joystick, 12);
    const char *after = cbx_settings_tab_settings(st)->overlay_trigger;
    assert_string_equal(after, "L3+R3");

    /* M25: A → confirm */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(cbx_settings_tab_mode(st), CBX_ST_MODE_LIST);

    cbx_manager_shutdown(&mgr);
}

/* M23+M24+M25 pointer path: trigger combo edit flow — click to enter. */
static void
test_m23_24_25_trigger_pointer(void **state)
{
    (void)state;
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);
    assert_true(mgr.dbus_connected);
    pump_manager(&mgr);

    cbx_settings_tab *st = cbx_manager_settings_tab(&mgr);

    /* Switch to Settings tab */
    send_key_dn(&mgr, SDLK_RIGHT);
    send_key_dn(&mgr, SDLK_RIGHT);

    /* M23: Click on trigger item (index 8) → enter edit mode */
    int px = list_center_x(&st->settings_list);
    int py = list_item_y(&st->settings_list, CBX_ST_SET_TRIGGER);
    send_mouse_click(&mgr, px, py);
    assert_int_equal(cbx_settings_tab_mode(st), CBX_ST_MODE_EDIT);

    /* M24: Down → cycle trigger (wraps: Select+A → L3+R3) */
    char before[64];
    snprintf(before, sizeof(before), "%s",
             cbx_settings_tab_settings(st)->overlay_trigger);
    send_key_dn(&mgr, SDLK_DOWN);
    const char *after = cbx_settings_tab_settings(st)->overlay_trigger;
    assert_string_equal(after, "L3+R3");

    /* M25: A → confirm */
    send_key_press(&mgr, SDLK_a);
    assert_int_equal(cbx_settings_tab_mode(st), CBX_ST_MODE_LIST);

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  M26 — Cancel edit (value reverts from disk)                        */
/* ================================================================== */

/* M26 controller path: enter edit, change, B → cancel → revert. */
static void
test_m26_cancel_edit_controller(void **state)
{
    mn_fixture *f = *state;
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);
    assert_true(mgr.dbus_connected);
    pump_manager(&mgr);

    cbx_settings_tab *st = cbx_manager_settings_tab(&mgr);

    /* Navigate to theme (index 1) */
    nav_to_setting_ctrl(&mgr, f->joystick, CBX_ST_SET_THEME);
    assert_int_equal(cbx_list_get_selected(&st->settings_list), CBX_ST_SET_THEME);

    /* Save original value */
    char saved[64];
    snprintf(saved, sizeof(saved), "%s",
             cbx_settings_tab_settings(st)->theme);

    /* Enter edit mode */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(cbx_settings_tab_mode(st), CBX_ST_MODE_EDIT);

    /* Change value (wraps: default → light) */
    ctrl_press(&mgr, f->joystick, 12);
    assert_string_equal(cbx_settings_tab_settings(st)->theme, "light");

    /* B → cancel → value reverts */
    ctrl_press(&mgr, f->joystick, 1);
    assert_int_equal(cbx_settings_tab_mode(st), CBX_ST_MODE_LIST);
    assert_string_equal(cbx_settings_tab_settings(st)->theme, saved);

    cbx_manager_shutdown(&mgr);
}

/* M26 pointer path: click to enter edit, change, B → cancel → revert. */
static void
test_m26_cancel_edit_pointer(void **state)
{
    (void)state;
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);
    assert_true(mgr.dbus_connected);
    pump_manager(&mgr);

    cbx_settings_tab *st = cbx_manager_settings_tab(&mgr);

    /* Switch to Settings tab */
    send_key_dn(&mgr, SDLK_RIGHT);
    send_key_dn(&mgr, SDLK_RIGHT);

    /* Save original value */
    char saved[64];
    snprintf(saved, sizeof(saved), "%s",
             cbx_settings_tab_settings(st)->theme);

    /* Click on theme (index 1) → enter edit mode */
    int px = list_center_x(&st->settings_list);
    int py = list_item_y(&st->settings_list, CBX_ST_SET_THEME);
    send_mouse_click(&mgr, px, py);
    assert_int_equal(cbx_settings_tab_mode(st), CBX_ST_MODE_EDIT);

    /* Change value (wraps: default → light) */
    send_key_dn(&mgr, SDLK_DOWN);
    assert_string_equal(cbx_settings_tab_settings(st)->theme, "light");

    /* B → cancel → value reverts */
    send_key_dn(&mgr, SDLK_b);
    assert_int_equal(cbx_settings_tab_mode(st), CBX_ST_MODE_LIST);
    assert_string_equal(cbx_settings_tab_settings(st)->theme, saved);

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  MG-04 — Topology failure (expected > actual → error shown)        */
/* ================================================================== */

/* MG-04: server has 0 targets, settings default expects 4 → error visible. */
static void
test_mg04_topology_failure(void **state)
{
    (void)state;
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);
    assert_true(mgr.dbus_connected);
    pump_manager(&mgr);

    cbx_controllers_tab *ct = cbx_manager_controllers_tab(&mgr);

    /* Default settings have VC count=4, server has 0 targets. */
    assert_int_equal(ct->expected_target_count, 4);
    assert_int_equal(cbx_controllers_tab_device_count(ct), 0);

    /* Status label should show topology incomplete error. */
    assert_true(cbx_widget_is_visible(&ct->status_lbl.base));
    const char *text = ct->status_lbl.text;
    assert_non_null(text);
    assert_true(strstr(text, "Topology incomplete") != NULL);
    assert_true(strstr(text, "0 of 4") != NULL);

    /* Create four targets through the production DBus wrappers, then drive
     * the Manager's bounded visible-tab refresh.  A confirmed 4/4 topology
     * must clear the stale 0/4 status without restarting Manager. */
    for (int i = 0; i < 4; i++) {
        char *path = NULL;
        assert_int_equal(ip_manager_create_target_device(mgr.dbus_backend,
            mgr.dbus_bus, "xb360", &path), 0);
        assert_non_null(path);
        free(path);
    }
    assert_int_equal(cbx_manager_refresh_controllers_if_due(&mgr,
        mgr.last_controller_refresh_ms + CBX_MGR_CONTROLLERS_REFRESH_MS), 0);
    assert_int_equal(cbx_controllers_tab_device_count(ct), 4);
    assert_false(cbx_widget_is_visible(&ct->status_lbl.base));
    assert_true(strstr(ct->status_lbl.text, "Topology incomplete") == NULL);

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  MG-15 — Post-resize hit testing with native DBus                  */
/* ================================================================== */

/* MG-15: resize window, click at new widget center → correct widget
 * activated (no stale rects). */
static void
test_mg15_resize_hit_testing(void **state)
{
    (void)state;
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);
    assert_true(mgr.dbus_connected);
    pump_manager(&mgr);

    cbx_controllers_tab *ct = cbx_manager_controllers_tab(&mgr);

    /* Record original Add button rect at 1280x720. */
    SDL_Rect orig_add_rect;
    cbx_widget_get_rect(&ct->add_btn.base, &orig_add_rect);
    assert_true(orig_add_rect.w > 0);

    /* Resize to 800x600. */
    send_window_resize(&mgr, 800, 600);
    assert_int_equal(mgr.rend.window_w, 800);
    assert_int_equal(mgr.rend.window_h, 600);

    /* After resize, panel rect should reflect new dimensions. */
    SDL_Rect panel_rect;
    cbx_widget_get_rect(&mgr.panels[CBX_MGR_TAB_CONTROLLERS].base,
                        &panel_rect);
    assert_int_equal(panel_rect.w, 800);
    assert_int_equal(panel_rect.h, 600 - 48);  /* minus tabbar height */

    /* Click at the Add button's current (post-resize) center. */
    SDL_Rect new_add_rect;
    cbx_widget_get_rect(&ct->add_btn.base, &new_add_rect);
    assert_true(new_add_rect.w > 0);

    int cx = new_add_rect.x + new_add_rect.w / 2;
    int cy = new_add_rect.y + new_add_rect.h / 2;
    send_mouse_click(&mgr, cx, cy);

    /* Type picker opens → click hit the Add button at its new position. */
    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_TYPE_PICK);

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  D06 — DBus operation failure (CreateTargetDevice returns error)  */
/* ================================================================== */

/* D06 controller path: Add → CreateTargetDevice fails → error shown,
 * topology retained (count unchanged). */
static void
test_d06_dbus_failure_controller(void **state)
{
    mn_fixture *f = *state;
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);
    assert_true(mgr.dbus_connected);
    pump_manager(&mgr);

    cbx_controllers_tab *ct = cbx_manager_controllers_tab(&mgr);

    int count_before = cbx_controllers_tab_device_count(ct);

    /* Navigate to Add button */
    ctrl_press(&mgr, f->joystick, 12);  /* Down → list */
    ctrl_press(&mgr, f->joystick, 12);  /* Down → button row */
    ctrl_press(&mgr, f->joystick, 13);   /* Left → Remove */
    ctrl_press(&mgr, f->joystick, 13);   /* Left → Add */

    /* A → open type picker */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_TYPE_PICK);

    /* A → confirm first type → CreateTargetDevice fails */
    ctrl_press(&mgr, f->joystick, 0);

    /* Mode returns to LIST after confirm_type_pick (even on error). */
    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_LIST);

    /* Device count unchanged (failure didn't create anything). */
    assert_int_equal(cbx_controllers_tab_device_count(ct), count_before);

    /* Error should be visible in status label with meaningful text. */
    assert_true(cbx_widget_is_visible(&ct->status_lbl.base));
    assert_non_null(ct->status_lbl.text);
    assert_true(strstr(ct->status_lbl.text, "Add failed:") != NULL);

    cbx_manager_shutdown(&mgr);
}

/* D06 pointer path: click Add → type picker → click type → fails. */
static void
test_d06_dbus_failure_pointer(void **state)
{
    (void)state;
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);
    assert_true(mgr.dbus_connected);
    pump_manager(&mgr);

    cbx_controllers_tab *ct = cbx_manager_controllers_tab(&mgr);

    int count_before = cbx_controllers_tab_device_count(ct);

    /* Click on Add button */
    int cx, cy;
    widget_center(&ct->add_btn.base, &cx, &cy);
    send_mouse_click(&mgr, cx, cy);
    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_TYPE_PICK);

    /* Click on first type item → CreateTargetDevice fails */
    int px = list_center_x(&ct->type_picker);
    int py = list_item_y(&ct->type_picker, 0);
    send_mouse_click(&mgr, px, py);

    /* Mode returns to LIST (error path). */
    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_LIST);

    /* Device count unchanged. */
    assert_int_equal(cbx_controllers_tab_device_count(ct), count_before);

    /* Error should be visible with meaningful text. */
    assert_true(cbx_widget_is_visible(&ct->status_lbl.base));
    assert_non_null(ct->status_lbl.text);
    assert_true(strstr(ct->status_lbl.text, "Add failed:") != NULL);

    cbx_manager_shutdown(&mgr);
}

/* Native async-capable backend: exact-path standalone add/type/remove with
 * no physical-composite cardinality assumption. */
static void
test_native_manager_exact_path_lifecycle(void **state)
{
    (void)state;
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);
    cbx_controllers_tab *ct = cbx_manager_controllers_tab(&mgr);
    assert_int_equal(cbx_controllers_tab_device_count(ct), 0);

    assert_int_equal(cbx_controllers_tab_add(ct, "xb360"), 0);
    assert_int_equal(cbx_controllers_tab_device_count(ct), 1);
    char original[CBX_MAX_PATH_LEN];
    snprintf(original, sizeof(original), "%s",
             cbx_controllers_tab_device_path(ct, 0));

    assert_int_equal(cbx_controllers_tab_change_type(ct, 0, "ds5"), 0);
    assert_int_equal(cbx_controllers_tab_device_count(ct), 1);
    assert_string_equal(cbx_controllers_tab_device_type(ct, 0), "ds5");
    assert_string_not_equal(cbx_controllers_tab_device_path(ct, 0), original);

    assert_int_equal(cbx_controllers_tab_remove(ct, 0), 0);
    assert_int_equal(cbx_controllers_tab_device_count(ct), 0);
    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  Test registration                                                  */
/* ================================================================== */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* Delayed publication/removal and persistence compensation */
        cmocka_unit_test_setup_teardown(test_delayed_add_remove_pointer_atomic,
                                        mn_setup_delayed, mn_teardown),
        cmocka_unit_test_setup_teardown(test_settings_write_failure_leaks_no_target,
                                        mn_setup_delayed, mn_teardown),
        cmocka_unit_test_setup_teardown(test_target_limit_checked_before_create,
                                        mn_setup, mn_teardown),
        /* M04 — Controllers tab list select */
        cmocka_unit_test_setup_teardown(test_m04_list_select_controller,
                                        mn_setup, mn_teardown),
        cmocka_unit_test_setup_teardown(test_m04_list_select_pointer,
                                        mn_setup, mn_teardown),
        /* M09 — Type picker cancel */
        cmocka_unit_test_setup_teardown(test_m09_type_picker_cancel_controller,
                                        mn_setup, mn_teardown),
        /* M21 — Settings list select */
        cmocka_unit_test_setup_teardown(test_m21_settings_list_select_controller,
                                        mn_setup, mn_teardown),
        cmocka_unit_test_setup_teardown(test_m21_settings_list_select_pointer,
                                        mn_setup, mn_teardown),
        /* M23+M24+M25 — Settings edit flow */
        cmocka_unit_test_setup_teardown(test_m23_24_25_opacity_controller,
                                        mn_setup, mn_teardown),
        cmocka_unit_test_setup_teardown(test_m23_24_25_opacity_pointer,
                                        mn_setup, mn_teardown),
        cmocka_unit_test_setup_teardown(test_m23_24_25_vc_count_controller,
                                        mn_setup, mn_teardown),
        cmocka_unit_test_setup_teardown(test_m23_24_25_vc_type_controller,
                                        mn_setup, mn_teardown),
        cmocka_unit_test_setup_teardown(test_m23_24_25_trigger_controller,
                                        mn_setup, mn_teardown),
        cmocka_unit_test_setup_teardown(test_m23_24_25_trigger_pointer,
                                        mn_setup, mn_teardown),
        /* M26 — Cancel edit (revert) */
        cmocka_unit_test_setup_teardown(test_m26_cancel_edit_controller,
                                        mn_setup, mn_teardown),
        cmocka_unit_test_setup_teardown(test_m26_cancel_edit_pointer,
                                        mn_setup, mn_teardown),
        cmocka_unit_test_setup_teardown(test_native_manager_exact_path_lifecycle,
                                        mn_setup, mn_teardown),
        /* MG-04 — Topology failure */
        cmocka_unit_test_setup_teardown(test_mg04_topology_failure,
                                        mn_setup, mn_teardown),
        /* MG-15 — Post-resize hit testing */
        cmocka_unit_test_setup_teardown(test_mg15_resize_hit_testing,
                                        mn_setup, mn_teardown),
        /* D06 — DBus failure (uses fail_create server) */
        cmocka_unit_test_setup_teardown(test_d06_dbus_failure_controller,
                                        mn_setup_fail, mn_teardown),
        cmocka_unit_test_setup_teardown(test_d06_dbus_failure_pointer,
                                        mn_setup_fail, mn_teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}