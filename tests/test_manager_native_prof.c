/*
 * test_manager_native_prof.c — Manager Profiles + Profile Editor
 * interaction acceptance with native DBus backend (Task 5).
 *
 * Exercises M10, M12–M14, M16, M18, M20, M30–M38, D02–D04, D07–D08
 * through production dispatch (cbx_manager_handle_event) with a real
 * sd-bus backend connected to a private InputPlumber-compatible
 * native-signature DBus server.  No ip_dbus_mock backend used.
 *
 * Controller path: SDL_JoystickSetVirtualButton → SDL_CONTROLLERBUTTONDOWN
 * → cbx_manager_controller_to_key → cbx_manager_handle_event (same path
 * as production gamepad input).  Name-input letter keys use keyboard
 * events (as in test_installed_functional.c Phase 7).
 *
 * Pointer path: SDL_MOUSEMOTION/MOUSEBUTTONDOWN/MOUSEBUTTONUP →
 * cbx_manager_handle_mouse_event → hit_test → focus + dispatch.
 *
 * M32/M34 (InputEvent capture): tested via direct callback
 * (test_m32_capture_event, test_m34_seq_capture) and via the full
 * DBus InputEvent signal path (test_m32_capture_dbus_signal,
 * test_m34_seq_capture_dbus_signal) through the native server's
 * EmitInputEvent method.
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
#include "dbus/ip_input_signal.h"
#include "dbus/dbus_interface.h"             /* IP_DBUS_PATH, IP_IFACE_* — constants only */

#include "config/config_settings.h"
#include "config/config_paths.h"

#include "manager/manager.h"
#include "manager/controllers_tab.h"
#include "manager/profiles_tab.h"
#include "manager/profile_editor_list.h"
#include "manager/profile_editor_seq.h"
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
    char    tmp_root[PATH_MAX];     /* temp tree for profile dirs */
    char    user_dir[PATH_MAX + 64];
    char    system_dir[PATH_MAX + 64];
    char    meta_dir[PATH_MAX + 64];
    int     joy_device_index;
    SDL_Joystick *joystick;
    const ip_dbus_backend *backend;
    ip_bus_handle bus;
} mnp_fixture;

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
    *cx = w->rect.x + w->rect.w / 2;
    *cy = w->rect.y + w->rect.h / 2;
}

/* Drain pending DBus messages on a bus connection for ms milliseconds. */
static void
drain_bus(const ip_dbus_backend *backend, ip_bus_handle bus, int ms)
{
    for (int i = 0; i < ms / 10; i++) {
        int processed = backend->process(bus);
        if (processed <= 0)
            usleep(10000);
    }
}

/* Emit an InputEvent signal via the native server's EmitInputEvent method.
 * The signal is emitted on the server's DBusDevice interface at the given
 * composite device path.  After calling this, drain the receiving bus to
 * process the signal through the production dispatch chain:
 *   sd_bus_process → sd_input_event_callback → input_event_signal_cb
 *   → ip_input_events_handle → cbx_profile_editor_on_input_event. */
static void
emit_input_event(const ip_dbus_backend *backend, ip_bus_handle bus,
                  const char *comp_path, const char *event, double value)
{
    char val_str[32];
    snprintf(val_str, sizeof(val_str), "%.1f", value);
    int rc = backend->call_method(bus, IP_DBUS_NAME, comp_path,
                                    IP_IFACE_DBUS_DEVICE, "EmitInputEvent",
                                    "ss", event, val_str, NULL);
    assert_int_equal(rc, 0);
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

/* Write a profile YAML with all 6 NES minimum button bindings. */
static void
write_nes_profile_yaml(const char *path, const char *name)
{
    static const char *btns[] = {"A", "B", "Up", "Down", "Left", "Right"};
    static const char *keys[] = {"KeyA", "KeyB", "KeyUp", "KeyDown",
                                  "KeyLeft", "KeyRight"};
    FILE *fp = fopen(path, "w");
    assert_non_null(fp);
    fprintf(fp, "version: 1\nkind: DeviceProfile\nname: %s\n", name);
    fprintf(fp, "description: NES test profile\nmapping:\n");
    for (int i = 0; i < 6; i++)
        fprintf(fp,
            "  - name: btn_%s\n"
            "    source_event:\n"
            "      gamepad:\n"
            "        button: %s\n"
            "    target_events:\n"
            "      - keyboard: %s\n",
            btns[i], btns[i], keys[i]);
    fclose(fp);
}

/* Switch to the Profiles tab from the Controllers tab via gamepad. */
static void
nav_to_profiles_ctrl(cbx_manager *mgr, SDL_Joystick *joy)
{
    ctrl_press(mgr, joy, 14);  /* Right → Profiles */
    assert_int_equal(cbx_manager_active_tab(mgr), CBX_MGR_TAB_PROFILES);
}

/* Switch to the Profiles tab via keyboard (for pointer-path tests). */
static void
nav_to_profiles_key(cbx_manager *mgr)
{
    send_key_dn(mgr, SDLK_RIGHT);
    assert_int_equal(cbx_manager_active_tab(mgr), CBX_MGR_TAB_PROFILES);
}

/* Navigate from tabbar past the profile list to the button row.
 * Profile count determines how many DOWNs are needed:
 *   1 DOWN: tabbar → list (item 0)
 *   N-1 DOWNs: scroll through remaining items
 *   1 DOWN: list bottom → focus chain → buttons
 * Total: N+1 DOWNs.
 */
static void
prof_nav_to_buttons_ctrl(cbx_manager *mgr, SDL_Joystick *joy,
                          int profile_count)
{
    ctrl_press(mgr, joy, 12);  /* tabbar → list */
    for (int i = 0; i < profile_count; i++)
        ctrl_press(mgr, joy, 12);  /* scroll / exit list */
}

/* Navigate to a specific button by index (0=Create, 1=Edit, 2=Delete).
 * After prof_nav_to_buttons, focus is on the rightmost button (Delete).
 * Navigate LEFT to reach lower-index buttons. */
static void
prof_nav_to_button_ctrl(cbx_manager *mgr, SDL_Joystick *joy,
                         int profile_count, int index)
{
    prof_nav_to_buttons_ctrl(mgr, joy, profile_count);
    for (int i = 0; i < 2 - index; i++)
        ctrl_press(mgr, joy, 13);  /* Left */
}

/* Open the editor via the production Edit-button path (controller).
 * Selects the user profile (index 1) and presses A on Edit. */
static void
open_editor_ctrl(cbx_manager *mgr, SDL_Joystick *joy, int profile_count)
{
    nav_to_profiles_ctrl(mgr, joy);

    ctrl_press(mgr, joy, 12);  /* tabbar → list */
    if (profile_count > 1)
        ctrl_press(mgr, joy, 12);  /* item 0 → item 1 */

    for (int i = 0; i < profile_count - 1; i++)
        ctrl_press(mgr, joy, 12);  /* to bottom of list */
    ctrl_press(mgr, joy, 13);  /* Left → Edit */

    ctrl_press(mgr, joy, 0);  /* A → open editor */
}

/* Open the editor via pointer (mouse click on Edit button).
 * Selects the user profile (index 1) first, then clicks Edit. */
static void
open_editor_ptr(cbx_manager *mgr)
{
    nav_to_profiles_key(mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    int px = list_center_x(&pt->profile_list_w);
    int py = list_item_y(&pt->profile_list_w, 1);
    send_mouse_click(mgr, px, py);

    int cx, cy;
    widget_center(&pt->edit_btn.base, &cx, &cy);
    send_mouse_click(mgr, cx, cy);
}

/* Enter target-pick mode from the editor LIST mode (controller). */
static void
editor_enter_target_pick_ctrl(cbx_manager *mgr, SDL_Joystick *joy)
{
    ctrl_press(mgr, joy, 0);  /* A on binding → BINDING_EDIT */
    ctrl_press(mgr, joy, 0);  /* A on "Pick Target" → TARGET_PICK */
}

/* Enter target-pick mode from the editor LIST mode (pointer). */
static void
editor_enter_target_pick_ptr(cbx_manager *mgr)
{
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    int px = list_center_x(&pt->editor.binding_list);
    int py = list_item_y(&pt->editor.binding_list, 0);
    send_mouse_click(mgr, px, py);  /* binding → BINDING_EDIT */

    px = list_center_x(&pt->editor.target_list);
    py = list_item_y(&pt->editor.target_list, 0);
    send_mouse_click(mgr, px, py);  /* "Pick Target" → TARGET_PICK */
}

/* ================================================================== */
/*  Setup / Teardown                                                   */
/* ================================================================== */

static void
mnp_setup_common(mnp_fixture *f, bool with_profiles)
{
    /* Isolated HOME */
    snprintf(f->tmp_home, sizeof(f->tmp_home),
             "/tmp/cbx_mnp_%d", (int)getpid());
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", f->tmp_home);
    int sysrc = system(cmd);
    (void)sysrc;
    mkdir(f->tmp_home, 0700);
    setenv("HOME", f->tmp_home, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");
    unsetenv("FLATPAK_ID");

    /* Temp profile directory tree */
    snprintf(f->tmp_root, sizeof(f->tmp_root),
             "/tmp/cbx_mnp_p_%d", (int)getpid());
    snprintf(cmd, sizeof(cmd), "rm -rf %s", f->tmp_root);
    sysrc = system(cmd);
    (void)sysrc;
    mkdir(f->tmp_root, 0700);

    snprintf(f->user_dir, sizeof(f->user_dir), "%s/user_profiles",
             f->tmp_root);
    snprintf(f->system_dir, sizeof(f->system_dir), "%s/system_profiles",
             f->tmp_root);
    snprintf(f->meta_dir, sizeof(f->meta_dir), "%s/meta_dir",
             f->tmp_root);
    mkdir(f->user_dir, 0700);
    mkdir(f->system_dir, 0700);
    mkdir(f->meta_dir, 0700);

    if (with_profiles) {
        /* System Default profile with NES bindings. */
        char def_path[PATH_MAX + 128];
        snprintf(def_path, sizeof(def_path), "%s/default.yaml",
                 f->system_dir);
        write_nes_profile_yaml(def_path, "Default");

        /* User profile with NES bindings (for edit/delete tests). */
        char prof_path[PATH_MAX + 128];
        snprintf(prof_path, sizeof(prof_path), "%s/myprof.yaml",
                 f->user_dir);
        write_nes_profile_yaml(prof_path, "MyProf");
    }

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

    const nip_server_config cfg = { .num_composites = 2, .version = "0.78.0" };
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
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(f->joystick), guid,
                               sizeof(guid));
    char mapping[512];
    snprintf(mapping, sizeof(mapping),
             "%s,Controller-Box Virtual,a:b0,b:b1,start:b6,"
             "dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,platform:Linux,",
             guid);
    assert_true(SDL_GameControllerAddMapping(mapping) >= 0);
    SDL_GameControllerEventState(SDL_ENABLE);
}

static int
mnp_setup(void **state)
{
    mnp_fixture *f = calloc(1, sizeof(*f));
    assert_non_null(f);
    f->joy_device_index = -1;
    f->daemon_pid = -1;
    f->server_pid = -1;
    mnp_setup_common(f, true);
    *state = f;
    return 0;
}

static int
mnp_setup_empty(void **state)
{
    mnp_fixture *f = calloc(1, sizeof(*f));
    assert_non_null(f);
    f->joy_device_index = -1;
    f->daemon_pid = -1;
    f->server_pid = -1;
    mnp_setup_common(f, false);
    *state = f;
    return 0;
}

static int
mnp_teardown(void **state)
{
    mnp_fixture *f = *state;
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
    char cmd[(PATH_MAX + 64) * 2];
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s", f->tmp_home, f->tmp_root);
    int sysrc = system(cmd);
    (void)sysrc;
    unsetenv("HOME");
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");
    free(f);
    return 0;
}

/* Helper to init manager + set test dirs + refresh profiles. */
static void
mnp_init_manager(mnp_fixture *f, cbx_manager *mgr)
{
    assert_int_equal(cbx_manager_init(mgr, NULL), 0);
    assert_true(mgr->dbus_connected);
    pump_manager(mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);
    cbx_profiles_tab_set_test_dirs(pt, f->user_dir, f->system_dir,
                                    f->meta_dir);
    cbx_profiles_tab_refresh(pt);
}

/* ================================================================== */
/*  M10 — Profile list select                                         */
/* ================================================================== */

/* M10 controller path: DOWN from tabbar → list, UP/DOWN selects items. */
static void
test_m10_list_select_controller(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);
    assert_int_equal(cbx_profiles_tab_profile_count(pt), 2);

    nav_to_profiles_ctrl(&mgr, f->joystick);

    /* DOWN: tabbar → profile list (item 0). */
    ctrl_press(&mgr, f->joystick, 12);
    assert_true(pt->profile_list_w.base.focused);
    assert_int_equal(cbx_list_get_selected(&pt->profile_list_w), 0);

    /* DOWN: item 0 → item 1. */
    ctrl_press(&mgr, f->joystick, 12);
    assert_int_equal(cbx_list_get_selected(&pt->profile_list_w), 1);

    /* UP: item 1 → item 0. */
    ctrl_press(&mgr, f->joystick, 11);
    assert_int_equal(cbx_list_get_selected(&pt->profile_list_w), 0);

    cbx_manager_shutdown(&mgr);
}

/* M10 pointer path: click on a profile list item selects it. */
static void
test_m10_list_select_pointer(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    nav_to_profiles_key(&mgr);

    /* Click on second item (index 1). */
    int px = list_center_x(&pt->profile_list_w);
    int py = list_item_y(&pt->profile_list_w, 1);
    send_mouse_click(&mgr, px, py);

    assert_int_equal(cbx_list_get_selected(&pt->profile_list_w), 1);
    assert_true(pt->profile_list_w.base.focused);

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  M12 — Create source picker confirm                                */
/* ================================================================== */

/* M12 controller path: Create → picker → DOWN to "Empty" → A → name
 * input mode opens. */
static void
test_m12_create_source_controller(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    nav_to_profiles_ctrl(&mgr, f->joystick);
    prof_nav_to_button_ctrl(&mgr, f->joystick, 2, 0);  /* Create */

    ctrl_press(&mgr, f->joystick, 0);  /* A → create picker */
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_CREATE_PICK);

    /* DOWN to select "Empty" (index 1). */
    ctrl_press(&mgr, f->joystick, 12);
    assert_int_equal(cbx_list_get_selected(&pt->create_picker), 1);

    /* A → name input mode. */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_NAME_INPUT);

    cbx_manager_shutdown(&mgr);
}

/* M12 pointer path: click Create → click "Default copy" → name input. */
static void
test_m12_create_source_pointer(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    nav_to_profiles_key(&mgr);

    int cx, cy;
    widget_center(&pt->create_btn.base, &cx, &cy);
    send_mouse_click(&mgr, cx, cy);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_CREATE_PICK);

    /* Click on "Default copy" (index 0). */
    int px = list_center_x(&pt->create_picker);
    int py = list_item_y(&pt->create_picker, 0);
    send_mouse_click(&mgr, px, py);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_NAME_INPUT);

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  M13 — Name input: characters appended                              */
/* ================================================================== */

/* M13 controller path: enter name input, type letters → chars appended. */
static void
test_m13_name_input_chars(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    nav_to_profiles_ctrl(&mgr, f->joystick);
    prof_nav_to_button_ctrl(&mgr, f->joystick, 2, 0);
    ctrl_press(&mgr, f->joystick, 0);  /* Create */
    ctrl_press(&mgr, f->joystick, 0);  /* Confirm "Default copy" */
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_NAME_INPUT);

    /* Type "hello" (avoid 'a' and 'b' which trigger confirm/cancel). */
    send_key_dn(&mgr, SDLK_h);
    send_key_dn(&mgr, SDLK_e);
    send_key_dn(&mgr, SDLK_l);
    send_key_dn(&mgr, SDLK_l);
    send_key_dn(&mgr, SDLK_o);

    assert_string_equal(cbx_profiles_tab_name_buffer(pt), "hello");

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  M14 — Name input: backspace removes last char                      */
/* ================================================================== */

/* M14 controller path: type chars, backspace → last char removed. */
static void
test_m14_name_input_backspace(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    nav_to_profiles_ctrl(&mgr, f->joystick);
    prof_nav_to_button_ctrl(&mgr, f->joystick, 2, 0);
    ctrl_press(&mgr, f->joystick, 0);  /* Create */
    ctrl_press(&mgr, f->joystick, 0);  /* Confirm */

    /* Type "xyz". */
    send_key_dn(&mgr, SDLK_x);
    send_key_dn(&mgr, SDLK_y);
    send_key_dn(&mgr, SDLK_z);
    assert_string_equal(cbx_profiles_tab_name_buffer(pt), "xyz");

    /* Backspace → "xy". */
    send_key_dn(&mgr, SDLK_BACKSPACE);
    assert_string_equal(cbx_profiles_tab_name_buffer(pt), "xy");

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  M16 — Name input cancel (B → returns to list)                      */
/* ================================================================== */

/* M16 controller path: type name → B → cancel → back to list. */
static void
test_m16_name_input_cancel_controller(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    nav_to_profiles_ctrl(&mgr, f->joystick);
    prof_nav_to_button_ctrl(&mgr, f->joystick, 2, 0);
    ctrl_press(&mgr, f->joystick, 0);  /* Create */
    ctrl_press(&mgr, f->joystick, 0);  /* Confirm */
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_NAME_INPUT);

    /* Type a char. */
    send_key_dn(&mgr, SDLK_x);
    assert_string_equal(cbx_profiles_tab_name_buffer(pt), "x");

    /* B → cancel. */
    ctrl_press(&mgr, f->joystick, 1);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_LIST);
    assert_string_equal(cbx_profiles_tab_name_buffer(pt), "");

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  M18 — Delete open (confirm-delete mode)                            */
/* ================================================================== */

/* M18 controller path: select user profile → Delete → A → confirm-delete. */
static void
test_m18_delete_open_controller(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    nav_to_profiles_ctrl(&mgr, f->joystick);

    /* Select user profile (index 1). */
    ctrl_press(&mgr, f->joystick, 12);  /* tabbar → list */
    ctrl_press(&mgr, f->joystick, 12);  /* item 0 → item 1 */

    /* Navigate to Delete button (rightmost). */
    ctrl_press(&mgr, f->joystick, 12);  /* list bottom → buttons */

    ctrl_press(&mgr, f->joystick, 0);  /* A → delete confirm */
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_CONFIRM_DELETE);

    cbx_manager_shutdown(&mgr);
}

/* M18 pointer path: click user profile → click Delete → confirm-delete. */
static void
test_m18_delete_open_pointer(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    nav_to_profiles_key(&mgr);

    /* Select user profile via pointer. */
    int px = list_center_x(&pt->profile_list_w);
    int py = list_item_y(&pt->profile_list_w, 1);
    send_mouse_click(&mgr, px, py);

    /* Click Delete button. */
    int cx, cy;
    widget_center(&pt->delete_btn.base, &cx, &cy);
    send_mouse_click(&mgr, cx, cy);

    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_CONFIRM_DELETE);

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  M20 — Delete cancel (B → returns to list, no deletion)           */
/* ================================================================== */

/* M20 controller path: delete → B → cancel → no deletion. */
static void
test_m20_delete_cancel_controller(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    nav_to_profiles_ctrl(&mgr, f->joystick);

    int before = cbx_profiles_tab_profile_count(pt);

    /* Select user profile (index 1). */
    ctrl_press(&mgr, f->joystick, 12);
    ctrl_press(&mgr, f->joystick, 12);

    /* Navigate to Delete. */
    ctrl_press(&mgr, f->joystick, 12);  /* → buttons */
    ctrl_press(&mgr, f->joystick, 0);  /* A → confirm delete mode */
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_CONFIRM_DELETE);

    /* B → cancel. */
    ctrl_press(&mgr, f->joystick, 1);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_LIST);
    assert_int_equal(cbx_profiles_tab_profile_count(pt), before);

    /* Verify file still exists. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/myprof.yaml", f->user_dir);
    assert_int_equal(access(path, F_OK), 0);

    cbx_manager_shutdown(&mgr);
}

/* M20 pointer path: click Delete → B → cancel → no deletion. */
static void
test_m20_delete_cancel_pointer(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    nav_to_profiles_key(&mgr);

    int before = cbx_profiles_tab_profile_count(pt);

    /* Select user profile via pointer. */
    int px = list_center_x(&pt->profile_list_w);
    int py = list_item_y(&pt->profile_list_w, 1);
    send_mouse_click(&mgr, px, py);

    /* Click Delete. */
    int cx, cy;
    widget_center(&pt->delete_btn.base, &cx, &cy);
    send_mouse_click(&mgr, cx, cy);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_CONFIRM_DELETE);

    /* B → cancel. */
    send_key_dn(&mgr, SDLK_b);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_LIST);
    assert_int_equal(cbx_profiles_tab_profile_count(pt), before);

    /* Verify file still exists. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/myprof.yaml", f->user_dir);
    assert_int_equal(access(path, F_OK), 0);

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  M30 — Target picker confirm                                       */
/* ================================================================== */

/* M30 controller path: target pick → A → confirm → back to LIST. */
static void
test_m30_target_pick_controller(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    open_editor_ctrl(&mgr, f->joystick, 2);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);

    editor_enter_target_pick_ctrl(&mgr, f->joystick);
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_TARGET_PICK);
    assert_true(cbx_list_item_count(&pt->editor.target_list) > 0);

    /* A → confirm target → back to LIST. */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_LIST);

    cbx_manager_shutdown(&mgr);
}

/* M30 pointer path: click target → confirm → back to LIST. */
static void
test_m30_target_pick_pointer(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    open_editor_ptr(&mgr);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);

    editor_enter_target_pick_ptr(&mgr);
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_TARGET_PICK);

    /* Click on first target → confirm → LIST. */
    int px = list_center_x(&pt->editor.target_list);
    int py = list_item_y(&pt->editor.target_list, 0);
    send_mouse_click(&mgr, px, py);
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_LIST);

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  M31 — Capture mode begin                                          */
/* ================================================================== */

/* M31 controller path: binding edit → DOWN to "Capture" → A → capture. */
static void
test_m31_capture_begin_controller(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    open_editor_ctrl(&mgr, f->joystick, 2);

    /* A on binding → BINDING_EDIT. */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_BINDING_EDIT);

    /* DOWN to "Capture" (index 1). */
    ctrl_press(&mgr, f->joystick, 12);

    /* A → capture mode. */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_CAPTURE);
    assert_true(cbx_profile_editor_is_capture_active(&pt->editor));

    cbx_manager_shutdown(&mgr);
}

/* M31 pointer path: click binding → click "Capture" → capture mode. */
static void
test_m31_capture_begin_pointer(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    open_editor_ptr(&mgr);

    /* Click on first binding → BINDING_EDIT. */
    int px = list_center_x(&pt->editor.binding_list);
    int py = list_item_y(&pt->editor.binding_list, 0);
    send_mouse_click(&mgr, px, py);

    /* Click on "Capture" (index 1). */
    px = list_center_x(&pt->editor.target_list);
    py = list_item_y(&pt->editor.target_list, 1);
    send_mouse_click(&mgr, px, py);

    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_CAPTURE);
    assert_true(cbx_profile_editor_is_capture_active(&pt->editor));

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  M32 — Capture event (InputEvent → binding captured)               */
/* ================================================================== */

/* M32: capture physical button via InputEvent callback → binding
 * captured, capture ends.  Dispatch path: cbx_profile_editor_on_input_event
 * (production callback wired to DBus InputEvent signal handler). */
static void
test_m32_capture_event(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    open_editor_ctrl(&mgr, f->joystick, 2);

    /* Enter capture mode. */
    ctrl_press(&mgr, f->joystick, 0);  /* binding → BINDING_EDIT */
    ctrl_press(&mgr, f->joystick, 12);  /* → "Capture" */
    ctrl_press(&mgr, f->joystick, 0);  /* → CAPTURE */
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_CAPTURE);

    int editing_idx = cbx_profile_editor_get_editing_index(&pt->editor);
    assert_int_equal(editing_idx, 0);

    /* Simulate a physical button press via the production InputEvent
     * callback (same path as DBus InputEvent signal → handler). */
    cbx_profile_editor_on_input_event(IP_INPUT_A, IP_INPUT_CAT_BUTTON,
                                       1.0, "A", "/dev/test", &pt->editor);

    /* Capture ends, back to LIST. */
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_LIST);
    assert_false(cbx_profile_editor_is_capture_active(&pt->editor));

    /* Verify the profile still has mappings. */
    const cbx_profile *prof = cbx_profile_editor_get_profile(&pt->editor);
    assert_non_null(prof);
    assert_int_equal(prof->mapping_count, 6);

    /* Verify the captured binding was actually updated — the source
     * event's button prop value should now be "A" (the physical
     * button pressed during capture).  Asserting only mapping_count
     * would not prove the capture modified anything. */
    const cbx_profile_mapping *m = &prof->mappings[editing_idx];
    bool found_btn = false;
    for (int j = 0; j < m->source_event.prop_count; j++) {
        if (strcmp(m->source_event.props[j].key, "button") == 0) {
            assert_string_equal(m->source_event.props[j].value, "A");
            found_btn = true;
            break;
        }
    }
    assert_true(found_btn);

    cbx_manager_shutdown(&mgr);
}

/* M32 DBus signal path: emit InputEvent via native server EmitInputEvent
 * method → server emits InputEvent(sd) signal → sd_bus_process on
 * manager bus → sd_input_event_callback → input_event_signal_cb →
 * ip_input_events_handle (sender verification, event parse, value
 * validation, rate limit) → cbx_profile_editor_on_input_event →
 * binding captured, capture ends.
 * This exercises the full production DBus signal dispatch chain, not
 * just the terminal callback. */
static void
test_m32_capture_dbus_signal(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    open_editor_ctrl(&mgr, f->joystick, 2);

    /* Enter capture mode. */
    ctrl_press(&mgr, f->joystick, 0);  /* binding → BINDING_EDIT */
    ctrl_press(&mgr, f->joystick, 12);  /* → "Capture" */
    ctrl_press(&mgr, f->joystick, 0);  /* → CAPTURE */
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_CAPTURE);

    int editing_idx = cbx_profile_editor_get_editing_index(&pt->editor);
    assert_int_equal(editing_idx, 0);

    /* Emit InputEvent signal via native server.  The signal travels:
     *   server EmitInputEvent method → sd_bus_emit_signal(InputEvent, sd)
     *   → daemon broadcasts → manager bus sd_bus_process
     *   → sd_input_event_callback → input_event_signal_cb
     *   → ip_input_events_handle (sender == expected_sender)
     *   → cbx_profile_editor_on_input_event → binding captured. */
    emit_input_event(f->backend, f->bus,
                     "/org/shadowblip/InputPlumber/CompositeDevice0",
                     "A", 1.0);
    drain_bus(mgr.dbus_backend, mgr.dbus_bus, 200);

    /* Capture ends, back to LIST. */
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_LIST);
    assert_false(cbx_profile_editor_is_capture_active(&pt->editor));

    /* Verify the profile still has mappings. */
    const cbx_profile *prof = cbx_profile_editor_get_profile(&pt->editor);
    assert_non_null(prof);
    assert_int_equal(prof->mapping_count, 6);

    /* Verify the captured binding was actually updated — the source
     * event's button prop value should now be "A". */
    const cbx_profile_mapping *m = &prof->mappings[editing_idx];
    bool found_btn = false;
    for (int j = 0; j < m->source_event.prop_count; j++) {
        if (strcmp(m->source_event.props[j].key, "button") == 0) {
            assert_string_equal(m->source_event.props[j].value, "A");
            found_btn = true;
            break;
        }
    }
    assert_true(found_btn);

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  M33 — Sequential mode begin                                       */
/* ================================================================== */

/* M33 controller path: binding edit → DOWN×2 to "Sequential" → A. */
static void
test_m33_seq_begin_controller(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    open_editor_ctrl(&mgr, f->joystick, 2);

    /* A on binding → BINDING_EDIT. */
    ctrl_press(&mgr, f->joystick, 0);

    /* DOWN twice to "Sequential" (index 2). */
    ctrl_press(&mgr, f->joystick, 12);
    ctrl_press(&mgr, f->joystick, 12);

    /* A → sequential mode. */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_SEQUENTIAL);
    assert_true(cbx_profile_editor_seq_is_active(&pt->editor));
    assert_int_equal(cbx_profile_editor_seq_get_step(&pt->editor), 0);

    cbx_manager_shutdown(&mgr);
}

/* M33 pointer path: click binding → click "Sequential" → sequential. */
static void
test_m33_seq_begin_pointer(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    open_editor_ptr(&mgr);

    /* Click on first binding → BINDING_EDIT. */
    int px = list_center_x(&pt->editor.binding_list);
    int py = list_item_y(&pt->editor.binding_list, 0);
    send_mouse_click(&mgr, px, py);

    /* Click on "Sequential" (index 2). */
    px = list_center_x(&pt->editor.target_list);
    py = list_item_y(&pt->editor.target_list, 2);
    send_mouse_click(&mgr, px, py);

    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_SEQUENTIAL);
    assert_true(cbx_profile_editor_seq_is_active(&pt->editor));

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  M34 — Sequential capture (InputEvent → auto-advance)             */
/* ================================================================== */

/* M34: capture button in sequential mode via InputEvent callback →
 * auto-advance.  Dispatch path: cbx_profile_editor_on_input_event →
 * cbx_profile_editor_seq_on_input. */
static void
test_m34_seq_capture(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    open_editor_ctrl(&mgr, f->joystick, 2);

    /* Enter sequential mode. */
    ctrl_press(&mgr, f->joystick, 0);  /* binding → BINDING_EDIT */
    ctrl_press(&mgr, f->joystick, 12);
    ctrl_press(&mgr, f->joystick, 12);  /* → "Sequential" */
    ctrl_press(&mgr, f->joystick, 0);  /* → SEQUENTIAL */
    assert_int_equal(cbx_profile_editor_seq_get_step(&pt->editor), 0);

    /* Simulate pressing a button for step 0 (Up). */
    cbx_profile_editor_on_input_event(IP_INPUT_A, IP_INPUT_CAT_BUTTON,
                                       1.0, "A", "/dev/test", &pt->editor);

    /* Should have advanced to step 1. */
    assert_int_equal(cbx_profile_editor_seq_get_step(&pt->editor), 1);

    /* Verify mapping was created/updated. */
    const cbx_profile *prof = cbx_profile_editor_get_profile(&pt->editor);
    assert_non_null(prof);
    assert_true(prof->mapping_count >= 6);

    /* Verify the step-0 (Up) mapping was actually updated — the
     * existing "btn_Up" mapping (whose source event button prop was
     * "Up") should now have button prop value "A" (the physical
     * button pressed).  find_or_create_mapping locates the existing
     * mapping by button prop value, then seq_on_input overwrites it.
     * Asserting only mapping_count >= 6 would not prove the capture
     * modified any binding — the count could be the pre-existing
     * baseline. */
    bool found_up = false;
    for (int i = 0; i < prof->mapping_count; i++) {
        if (strcmp(prof->mappings[i].name, "btn_Up") != 0)
            continue;
        for (int j = 0; j < prof->mappings[i].source_event.prop_count; j++) {
            if (strcmp(prof->mappings[i].source_event.props[j].key, "button") == 0) {
                assert_string_equal(prof->mappings[i].source_event.props[j].value, "A");
                found_up = true;
                break;
            }
        }
    }
    assert_true(found_up);

    cbx_manager_shutdown(&mgr);
}

/* M34 DBus signal path: sequential capture via InputEvent signal through
 * the full production DBus dispatch chain (EmitInputEvent → sd_bus_process
 * → input_event_signal_cb → ip_input_events_handle → on_input_event →
 * seq_on_input).  This exercises the same dispatch as M32's DBus signal
 * test but for sequential mode auto-advance. */
static void
test_m34_seq_capture_dbus_signal(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    open_editor_ctrl(&mgr, f->joystick, 2);

    /* Enter sequential mode. */
    ctrl_press(&mgr, f->joystick, 0);  /* binding → BINDING_EDIT */
    ctrl_press(&mgr, f->joystick, 12);
    ctrl_press(&mgr, f->joystick, 12);  /* → "Sequential" */
    ctrl_press(&mgr, f->joystick, 0);  /* → SEQUENTIAL */
    assert_int_equal(cbx_profile_editor_seq_get_step(&pt->editor), 0);

    /* Emit InputEvent signal via native server.  The signal travels:
     *   server EmitInputEvent method → sd_bus_emit_signal(InputEvent, sd)
     *   → daemon broadcasts → manager bus sd_bus_process
     *   → sd_input_event_callback → input_event_signal_cb
     *   → ip_input_events_handle (sender == expected_sender)
     *   → cbx_profile_editor_on_input_event → seq captured, auto-advance. */
    emit_input_event(f->backend, f->bus,
                     "/org/shadowblip/InputPlumber/CompositeDevice0",
                     "A", 1.0);
    drain_bus(mgr.dbus_backend, mgr.dbus_bus, 200);

    /* Should have advanced to step 1. */
    assert_int_equal(cbx_profile_editor_seq_get_step(&pt->editor), 1);

    /* Verify mapping was created/updated. */
    const cbx_profile *prof = cbx_profile_editor_get_profile(&pt->editor);
    assert_non_null(prof);
    assert_true(prof->mapping_count >= 6);

    /* Verify the step-0 (Up) mapping was actually updated — the
     * existing "btn_Up" mapping should now have button prop "A". */
    bool found_up = false;
    for (int i = 0; i < prof->mapping_count; i++) {
        if (strcmp(prof->mappings[i].name, "btn_Up") != 0)
            continue;
        for (int j = 0; j < prof->mappings[i].source_event.prop_count; j++) {
            if (strcmp(prof->mappings[i].source_event.props[j].key, "button") == 0) {
                assert_string_equal(prof->mappings[i].source_event.props[j].value, "A");
                found_up = true;
                break;
            }
        }
    }
    assert_true(found_up);

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  M35 — Sequential skip (B → skip current button)                  */
/* ================================================================== */

/* M35 controller path: B during sequential → skip current button. */
static void
test_m35_seq_skip(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    open_editor_ctrl(&mgr, f->joystick, 2);

    /* Enter sequential mode. */
    ctrl_press(&mgr, f->joystick, 0);
    ctrl_press(&mgr, f->joystick, 12);
    ctrl_press(&mgr, f->joystick, 12);
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(cbx_profile_editor_seq_get_step(&pt->editor), 0);

    /* B → skip step 0. */
    ctrl_press(&mgr, f->joystick, 1);
    assert_int_equal(cbx_profile_editor_seq_get_step(&pt->editor), 1);
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_SEQUENTIAL);

    /* B → skip step 1. */
    ctrl_press(&mgr, f->joystick, 1);
    assert_int_equal(cbx_profile_editor_seq_get_step(&pt->editor), 2);

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  M36 — Sequential cancel (Start/Tab → cancel, discard)            */
/* ================================================================== */

/* M36 controller path: Start (Tab) during sequential → cancel. */
static void
test_m36_seq_cancel(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    open_editor_ctrl(&mgr, f->joystick, 2);

    /* Enter sequential mode. */
    ctrl_press(&mgr, f->joystick, 0);
    ctrl_press(&mgr, f->joystick, 12);
    ctrl_press(&mgr, f->joystick, 12);
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_SEQUENTIAL);

    /* Start (Tab) → cancel sequential. */
    send_key_dn(&mgr, SDLK_TAB);
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_LIST);
    assert_false(cbx_profile_editor_seq_is_active(&pt->editor));

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  M38 — Discard changes (Start from editor LIST → close, no save)  */
/* ================================================================== */

/* M38 controller path: Start (button 6) in editor LIST mode → discard
 * changes, close editor, no file written.  Uses the virtual gamepad
 * Start button through the production controller event dispatch path. */
static void
test_m38_discard_ctrl(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    open_editor_ctrl(&mgr, f->joystick, 2);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_LIST);

    /* Record the profile file's modification time. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/myprof.yaml", f->user_dir);
    struct stat st_before;
    assert_int_equal(stat(path, &st_before), 0);

    /* Start (button 6 on virtual gamepad) → discard and close.
     * Controller path: SDL_CONTROLLERBUTTONDOWN START →
     * cbx_manager_controller_to_key → SDLK_TAB →
     * cbx_profiles_tab_handle_key → cbx_profiles_tab_close_editor. */
    ctrl_press(&mgr, f->joystick, 6);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_LIST);

    /* File should be unchanged (not re-saved). */
    struct stat st_after;
    assert_int_equal(stat(path, &st_after), 0);
    assert_int_equal(st_before.st_mtime, st_after.st_mtime);

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  D02 — No device selected (Controllers tab)                       */
/* ================================================================== */

/* D02: server starts with 0 targets → no devices. Remove button
 * activation produces no side effect. */
static void
test_d02_no_device_controller(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_controllers_tab *ct = cbx_manager_controllers_tab(&mgr);
    assert_int_equal(cbx_controllers_tab_device_count(ct), 0);

    /* Navigate to Remove button (left of Add). */
    ctrl_press(&mgr, f->joystick, 12);  /* Down → list (empty) */
    ctrl_press(&mgr, f->joystick, 12);  /* Down → button row */
    ctrl_press(&mgr, f->joystick, 13);   /* Left → Remove */

    /* A → no effect (no device to remove). */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(cbx_controllers_tab_device_count(ct), 0);
    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_LIST);

    cbx_manager_shutdown(&mgr);
}

/* D02 pointer path: click Remove with no device → no effect. */
static void
test_d02_no_device_pointer(void **state)
{
    (void)state;
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);
    assert_true(mgr.dbus_connected);
    pump_manager(&mgr);

    cbx_controllers_tab *ct = cbx_manager_controllers_tab(&mgr);
    assert_int_equal(cbx_controllers_tab_device_count(ct), 0);

    /* Click on Remove button. */
    int cx, cy;
    widget_center(&ct->remove_btn.base, &cx, &cy);
    send_mouse_click(&mgr, cx, cy);

    /* No effect — count unchanged, mode stays LIST. */
    assert_int_equal(cbx_controllers_tab_device_count(ct), 0);
    assert_int_equal(cbx_controllers_tab_mode(ct), CBX_CT_MODE_LIST);

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  D03 — No profile selected (empty profile list)                    */
/* ================================================================== */

/* D03: empty profile list → Delete activation produces no file deletion.
 * Uses mnp_setup_empty (no profile files). */
static void
test_d03_no_profile_controller(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);
    assert_int_equal(cbx_profiles_tab_profile_count(pt), 0);
    assert_int_equal(cbx_profiles_tab_selected(pt), -1);

    nav_to_profiles_ctrl(&mgr, f->joystick);

    /* DOWN: tabbar → empty list → buttons (Delete rightmost). */
    ctrl_press(&mgr, f->joystick, 12);
    /* A on Delete → no effect. */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_LIST);

    cbx_manager_shutdown(&mgr);
}

/* D03 pointer path: click Delete with no profiles → no effect. */
static void
test_d03_no_profile_pointer(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);
    assert_int_equal(cbx_profiles_tab_profile_count(pt), 0);

    nav_to_profiles_key(&mgr);

    /* Click Delete button → no effect. */
    int cx, cy;
    widget_center(&pt->delete_btn.base, &cx, &cy);
    send_mouse_click(&mgr, cx, cy);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_LIST);

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  D04 — NES validation error (save with missing bindings)          */
/* ================================================================== */

/* D04: create empty profile → save → NES validation fails → error shown,
 * editor stays open, no file written. */
static void
test_d04_save_missing_nes(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    nav_to_profiles_ctrl(&mgr, f->joystick);
    prof_nav_to_button_ctrl(&mgr, f->joystick, 2, 0);  /* Create */

    ctrl_press(&mgr, f->joystick, 0);  /* Create */
    /* DOWN to "Empty" (index 1). */
    ctrl_press(&mgr, f->joystick, 12);
    ctrl_press(&mgr, f->joystick, 0);  /* Confirm Empty */

    /* Type name. */
    send_key_dn(&mgr, SDLK_t);
    send_key_dn(&mgr, SDLK_e);
    send_key_dn(&mgr, SDLK_s);
    send_key_dn(&mgr, SDLK_t);

    /* A to confirm → editor opens with empty profile. */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);
    assert_int_equal(cbx_profile_editor_binding_count(&pt->editor), 0);

    /* B in LIST mode → save attempt → NES validation fails. */
    ctrl_press(&mgr, f->joystick, 1);

    /* Editor stays open (save failed). */
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);

    /* Verify no file was written. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/test.yaml", f->user_dir);
    assert_int_not_equal(access(path, F_OK), 0);

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  D07 — Filesystem failure on profile save                          */
/* ================================================================== */

/* D07: make user_dir read-only, try to save → error shown, editor stays. */
static void
test_d07_filesystem_failure(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    open_editor_ctrl(&mgr, f->joystick, 2);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);

    /* Make the user dir read-only so save fails. */
    chmod(f->user_dir, 0555);

    /* B in LIST mode → save attempt → filesystem failure. */
    ctrl_press(&mgr, f->joystick, 1);

    /* Editor stays open (save failed). */
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);

    /* Status should show save failure — check for error content, not
     * just non-empty (a stale "ready" message would pass the old
     * check).  Production code sets "Save failed." on filesystem error. */
    const char *status = cbx_profile_editor_get_status(&pt->editor);
    assert_non_null(status);
    assert_true(strstr(status, "fail") != NULL || strstr(status, "Fail") != NULL
                 || strstr(status, "error") != NULL || strstr(status, "Error") != NULL
                 || strstr(status, "Missing") != NULL);

    /* Restore permissions for cleanup. */
    chmod(f->user_dir, 0700);

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  D08 — Empty profile creation (add-first-binding reachable)       */
/* ================================================================== */

/* D08: create empty profile → editor opens with 0 bindings → save blocked
 * by NES validation.  Verifies the empty profile creation flow is
 * reachable and the editor opens correctly. */
static void
test_d08_empty_profile_create(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    nav_to_profiles_ctrl(&mgr, f->joystick);
    prof_nav_to_button_ctrl(&mgr, f->joystick, 2, 0);

    ctrl_press(&mgr, f->joystick, 0);  /* Create */
    ctrl_press(&mgr, f->joystick, 12);  /* → "Empty" */
    ctrl_press(&mgr, f->joystick, 0);  /* Confirm Empty */

    /* Type name. */
    send_key_dn(&mgr, SDLK_n);
    send_key_dn(&mgr, SDLK_e);
    send_key_dn(&mgr, SDLK_w);

    /* A to confirm → editor opens with empty profile. */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);
    assert_int_equal(cbx_profile_editor_binding_count(&pt->editor), 0);

    /* Try to save → NES validation blocks. */
    ctrl_press(&mgr, f->joystick, 1);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);

    /* No file written. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/new.yaml", f->user_dir);
    assert_int_not_equal(access(path, F_OK), 0);

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  D04 pointer path — NES validation error (pointer-initiated save)  */
/* ================================================================== */

/* D04 pointer: Create empty profile via pointer (mouse clicks), then
 * save via B KEYUP.  NES validation blocks save; editor stays open,
 * no file written. */
static void
test_d04_save_missing_nes_pointer(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    /* Switch to Profiles tab via keyboard. */
    nav_to_profiles_key(&mgr);

    /* Click Create button. */
    int cx, cy;
    widget_center(&pt->create_btn.base, &cx, &cy);
    send_mouse_click(&mgr, cx, cy);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_CREATE_PICK);

    /* Click "Empty" (index 1) in the source picker. */
    int px = list_center_x(&pt->create_picker);
    int py = list_item_y(&pt->create_picker, 1);
    send_mouse_click(&mgr, px, py);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_NAME_INPUT);

    /* Type name (keyboard — no mouse text input). */
    send_key_dn(&mgr, SDLK_t);
    send_key_dn(&mgr, SDLK_e);
    send_key_dn(&mgr, SDLK_s);
    send_key_dn(&mgr, SDLK_t);

    /* Confirm name with Enter (KEYDOWN triggers confirm). */
    send_key_dn(&mgr, SDLK_RETURN);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);
    assert_int_equal(cbx_profile_editor_binding_count(&pt->editor), 0);

    /* B KEYUP → save attempt → NES validation fails. */
    send_key_press(&mgr, SDLK_b);

    /* Editor stays open (save failed). */
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);

    /* Verify no file was written. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/test.yaml", f->user_dir);
    assert_int_not_equal(access(path, F_OK), 0);

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  D07 pointer path — filesystem failure (pointer-initiated save)     */
/* ================================================================== */

/* D07 pointer: Open editor via pointer, make user_dir read-only, save
 * via B KEYUP → filesystem failure.  Editor stays open, error shown. */
static void
test_d07_filesystem_failure_pointer(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    /* Open editor via pointer path. */
    open_editor_ptr(&mgr);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);

    /* Make the user dir read-only so save fails. */
    chmod(f->user_dir, 0555);

    /* B KEYUP → save attempt → filesystem failure. */
    send_key_press(&mgr, SDLK_b);

    /* Editor stays open (save failed). */
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);

    /* Status should show save failure — check for error content. */
    const char *status = cbx_profile_editor_get_status(&pt->editor);
    assert_non_null(status);
    assert_true(strstr(status, "fail") != NULL || strstr(status, "Fail") != NULL
                 || strstr(status, "error") != NULL || strstr(status, "Error") != NULL
                 || strstr(status, "Missing") != NULL);

    /* Restore permissions for cleanup. */
    chmod(f->user_dir, 0700);

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  D08 pointer path — empty profile creation (pointer-initiated)      */
/* ================================================================== */

/* D08 pointer: Create empty profile via pointer, verify editor opens
 * with 0 bindings, save via B KEYUP → NES validation blocks. */
static void
test_d08_empty_profile_create_pointer(void **state)
{
    mnp_fixture *f = *state;
    cbx_manager mgr;
    mnp_init_manager(f, &mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);

    /* Switch to Profiles tab via keyboard. */
    nav_to_profiles_key(&mgr);

    /* Click Create button. */
    int cx, cy;
    widget_center(&pt->create_btn.base, &cx, &cy);
    send_mouse_click(&mgr, cx, cy);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_CREATE_PICK);

    /* Click "Empty" (index 1) in the source picker. */
    int px = list_center_x(&pt->create_picker);
    int py = list_item_y(&pt->create_picker, 1);
    send_mouse_click(&mgr, px, py);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_NAME_INPUT);

    /* Type name. */
    send_key_dn(&mgr, SDLK_n);
    send_key_dn(&mgr, SDLK_e);
    send_key_dn(&mgr, SDLK_w);

    /* Confirm name with Enter. */
    send_key_dn(&mgr, SDLK_RETURN);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);
    assert_int_equal(cbx_profile_editor_binding_count(&pt->editor), 0);

    /* B KEYUP → save attempt → NES validation blocks. */
    send_key_press(&mgr, SDLK_b);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);

    /* No file written. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/new.yaml", f->user_dir);
    assert_int_not_equal(access(path, F_OK), 0);

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  Test registration                                                  */
/* ================================================================== */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* M10 — Profile list select */
        cmocka_unit_test_setup_teardown(test_m10_list_select_controller,
                                        mnp_setup, mnp_teardown),
        cmocka_unit_test_setup_teardown(test_m10_list_select_pointer,
                                        mnp_setup, mnp_teardown),
        /* M12 — Create source picker confirm */
        cmocka_unit_test_setup_teardown(test_m12_create_source_controller,
                                        mnp_setup, mnp_teardown),
        cmocka_unit_test_setup_teardown(test_m12_create_source_pointer,
                                        mnp_setup, mnp_teardown),
        /* M13 — Name input characters */
        cmocka_unit_test_setup_teardown(test_m13_name_input_chars,
                                        mnp_setup, mnp_teardown),
        /* M14 — Name input backspace */
        cmocka_unit_test_setup_teardown(test_m14_name_input_backspace,
                                        mnp_setup, mnp_teardown),
        /* M16 — Name input cancel */
        cmocka_unit_test_setup_teardown(test_m16_name_input_cancel_controller,
                                        mnp_setup, mnp_teardown),
        /* M18 — Delete open */
        cmocka_unit_test_setup_teardown(test_m18_delete_open_controller,
                                        mnp_setup, mnp_teardown),
        cmocka_unit_test_setup_teardown(test_m18_delete_open_pointer,
                                        mnp_setup, mnp_teardown),
        /* M20 — Delete cancel */
        cmocka_unit_test_setup_teardown(test_m20_delete_cancel_controller,
                                        mnp_setup, mnp_teardown),
        cmocka_unit_test_setup_teardown(test_m20_delete_cancel_pointer,
                                        mnp_setup, mnp_teardown),
        /* M30 — Target picker confirm */
        cmocka_unit_test_setup_teardown(test_m30_target_pick_controller,
                                        mnp_setup, mnp_teardown),
        cmocka_unit_test_setup_teardown(test_m30_target_pick_pointer,
                                        mnp_setup, mnp_teardown),
        /* M31 — Capture begin */
        cmocka_unit_test_setup_teardown(test_m31_capture_begin_controller,
                                        mnp_setup, mnp_teardown),
        cmocka_unit_test_setup_teardown(test_m31_capture_begin_pointer,
                                        mnp_setup, mnp_teardown),
        /* M32 — Capture event */
        cmocka_unit_test_setup_teardown(test_m32_capture_event,
                                        mnp_setup, mnp_teardown),
        cmocka_unit_test_setup_teardown(test_m32_capture_dbus_signal,
                                        mnp_setup, mnp_teardown),
        /* M33 — Sequential begin */
        cmocka_unit_test_setup_teardown(test_m33_seq_begin_controller,
                                        mnp_setup, mnp_teardown),
        cmocka_unit_test_setup_teardown(test_m33_seq_begin_pointer,
                                        mnp_setup, mnp_teardown),
        /* M34 — Sequential capture */
        cmocka_unit_test_setup_teardown(test_m34_seq_capture,
                                        mnp_setup, mnp_teardown),
        cmocka_unit_test_setup_teardown(test_m34_seq_capture_dbus_signal,
                                        mnp_setup, mnp_teardown),
        /* M35 — Sequential skip */
        cmocka_unit_test_setup_teardown(test_m35_seq_skip,
                                        mnp_setup, mnp_teardown),
        /* M36 — Sequential cancel */
        cmocka_unit_test_setup_teardown(test_m36_seq_cancel,
                                        mnp_setup, mnp_teardown),
        /* M38 — Discard changes (Start from editor LIST) */
        cmocka_unit_test_setup_teardown(test_m38_discard_ctrl,
                                        mnp_setup, mnp_teardown),
        /* D02 — No device selected */
        cmocka_unit_test_setup_teardown(test_d02_no_device_controller,
                                        mnp_setup, mnp_teardown),
        cmocka_unit_test_setup_teardown(test_d02_no_device_pointer,
                                        mnp_setup, mnp_teardown),
        /* D03 — No profile selected (uses empty setup) */
        cmocka_unit_test_setup_teardown(test_d03_no_profile_controller,
                                        mnp_setup_empty, mnp_teardown),
        cmocka_unit_test_setup_teardown(test_d03_no_profile_pointer,
                                        mnp_setup_empty, mnp_teardown),
        /* D04 — NES validation error */
        cmocka_unit_test_setup_teardown(test_d04_save_missing_nes,
                                        mnp_setup, mnp_teardown),
        cmocka_unit_test_setup_teardown(test_d04_save_missing_nes_pointer,
                                        mnp_setup, mnp_teardown),
        /* D07 — Filesystem failure */
        cmocka_unit_test_setup_teardown(test_d07_filesystem_failure,
                                        mnp_setup, mnp_teardown),
        cmocka_unit_test_setup_teardown(test_d07_filesystem_failure_pointer,
                                        mnp_setup, mnp_teardown),
        /* D08 — Empty profile creation */
        cmocka_unit_test_setup_teardown(test_d08_empty_profile_create,
                                        mnp_setup, mnp_teardown),
        cmocka_unit_test_setup_teardown(test_d08_empty_profile_create_pointer,
                                        mnp_setup, mnp_teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}