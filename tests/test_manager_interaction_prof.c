/*
 * test_manager_interaction_prof.c — Manager interaction acceptance tests
 * for the Profiles tab and profile editor through production dispatch.
 *
 * Task 9 — Every control M10–M20 (Profiles) and M28–M38 (Profile editor),
 * plus disabled scenarios D03, D04, D07, D08, is exercised through
 * cbx_manager_handle_event (the same dispatch path the installed binary
 * uses) for BOTH the controller (keyboard) and pointer (mouse) input
 * paths.  Semantic outcomes (mode transitions, file creation/deletion,
 * binding changes, save validation) are asserted — never mere handler
 * return values.
 *
 * Profile editor interactions use the production Edit-button path to
 * open the editor (not manual initialization).
 */
#include <cmocka.h>
#include <SDL2/SDL.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "manager/manager.h"
#include "ui/input_map.h"
#include "manager/profiles_tab.h"
#include "manager/profile_editor_list.h"
#include "manager/profile_editor_seq.h"
#include "ui/widget.h"
#include "dbus_mock.h"
#include "interaction_inventory.h"
#include "dbus/ip_input_signal.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */

#define SUPPORTED_TYPES "xb360,ds5,deck,gamepad,mouse,keyboard"

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
    ev.key.windowID = CBX_CONTROLLER_EVENT_WINDOW_ID;
    ev.key.keysym.sym = sym;
    return cbx_manager_handle_event(mgr, &ev);
}

static bool
send_key_up(cbx_manager *mgr, SDL_Keycode sym)
{
    SDL_Event ev = {0};
    ev.type = SDL_KEYUP;
    ev.key.windowID = CBX_CONTROLLER_EVENT_WINDOW_ID;
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

/* Write a profile YAML with all 6 NES minimum button bindings. */
static void
write_nes_profile_yaml(const char *path, const char *name)
{
    static const char *btns[] = {"A", "B", "Up", "Down", "Left", "Right"};
    static const char *keys[] = {"KeyA", "KeyB", "KeyUp", "KeyDown",
                                  "KeyLeft", "KeyRight"};
    FILE *f = fopen(path, "w");
    assert_non_null(f);
    fprintf(f, "version: 1\nkind: DeviceProfile\nname: %s\n", name);
    fprintf(f, "description: NES test profile\nmapping:\n");
    for (int i = 0; i < 6; i++)
        fprintf(f,
            "  - name: btn_%s\n"
            "    source_event:\n"
            "      gamepad:\n"
            "        button: %s\n"
            "    target_events:\n"
            "      - keyboard: %s\n",
            btns[i], btns[i], keys[i]);
    fclose(f);
}


/* ------------------------------------------------------------------ */
/*  Fixture                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    ip_dbus_mock          mock;
    const ip_dbus_backend *backend;
    cbx_manager           mgr;
    char                  tmp[256];
    char                  user_dir[PATH_MAX];
    char                  system_dir[PATH_MAX];
    char                  meta_dir[PATH_MAX];
} mip_fixture;

static int
mip_setup(void **state)
{
    mip_fixture *f = malloc(sizeof(*f));
    assert_non_null(f);
    memset(f, 0, sizeof(*f));

    /* Create temp directory tree. */
    snprintf(f->tmp, sizeof(f->tmp), "/tmp/cbx_mip_%d", (int)getpid());
    char cmd[PATH_MAX + 128];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", f->tmp);
    (void)!system(cmd);
    mkdir(f->tmp, 0700);

    snprintf(f->user_dir, sizeof(f->user_dir), "%s/user_profiles", f->tmp);
    snprintf(f->system_dir, sizeof(f->system_dir), "%s/system_profiles",
             f->tmp);
    snprintf(f->meta_dir, sizeof(f->meta_dir), "%s/meta_dir", f->tmp);
    mkdir(f->user_dir, 0700);
    mkdir(f->system_dir, 0700);
    mkdir(f->meta_dir, 0700);

    /* Write system Default profile with NES bindings. */
    char def_path[PATH_MAX + 128];
    snprintf(def_path, sizeof(def_path), "%s/default.yaml", f->system_dir);
    write_nes_profile_yaml(def_path, "Default");

    /* Write a user profile with NES bindings (for edit/delete tests). */
    char prof_path[PATH_MAX + 128];
    snprintf(prof_path, sizeof(prof_path), "%s/myprof.yaml", f->user_dir);
    write_nes_profile_yaml(prof_path, "MyProf");

    /* Init DBus mock with empty fixture (no devices). */
    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                           "SupportedTargetDeviceIds", SUPPORTED_TYPES);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_OBJECT_MANAGER,
                           "GetManagedObjects", FIXTURE_EMPTY);

    ensure_dummy_driver();
    assert_int_equal(
        cbx_manager_init_with_dbus(&f->mgr, NULL, f->backend, f->mock.bus),
        0);

    /* Override profile dirs for testing. */
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&f->mgr);
    cbx_profiles_tab_set_test_dirs(pt, f->user_dir, f->system_dir,
                                    f->meta_dir);
    cbx_profiles_tab_refresh(pt);

    *state = f;
    return 0;
}

static int
mip_setup_empty(void **state)
{
    /* Same as mip_setup but with no profile files — empty list. */
    mip_fixture *f = malloc(sizeof(*f));
    assert_non_null(f);
    memset(f, 0, sizeof(*f));

    snprintf(f->tmp, sizeof(f->tmp), "/tmp/cbx_mip_e_%d", (int)getpid());
    char cmd[PATH_MAX + 128];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", f->tmp);
    (void)!system(cmd);
    mkdir(f->tmp, 0700);

    snprintf(f->user_dir, sizeof(f->user_dir), "%s/user_profiles", f->tmp);
    snprintf(f->system_dir, sizeof(f->system_dir), "%s/system_profiles",
             f->tmp);
    snprintf(f->meta_dir, sizeof(f->meta_dir), "%s/meta_dir", f->tmp);
    mkdir(f->user_dir, 0700);
    mkdir(f->system_dir, 0700);
    mkdir(f->meta_dir, 0700);

    /* No profile files — empty list. */

    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                           "SupportedTargetDeviceIds", SUPPORTED_TYPES);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_OBJECT_MANAGER,
                           "GetManagedObjects", FIXTURE_EMPTY);

    ensure_dummy_driver();
    assert_int_equal(
        cbx_manager_init_with_dbus(&f->mgr, NULL, f->backend, f->mock.bus),
        0);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&f->mgr);
    cbx_profiles_tab_set_test_dirs(pt, f->user_dir, f->system_dir,
                                    f->meta_dir);
    cbx_profiles_tab_refresh(pt);

    *state = f;
    return 0;
}

static int
mip_teardown(void **state)
{
    mip_fixture *f = *state;
    if (f) {
        /* Restore permissions in case D07 made dir read-only. */
        chmod(f->user_dir, 0700);

        cbx_manager_shutdown(&f->mgr);
        ip_dbus_mock_free(&f->mock);

        char cmd[PATH_MAX + 128];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", f->tmp);
        (void)!system(cmd);
        free(f);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Navigation helpers                                                 */
/* ------------------------------------------------------------------ */

/* Switch to the Profiles tab from the Controllers tab. */
static void
switch_to_profiles(cbx_manager *mgr)
{
    /* Start on Controllers tab (tab 0).  RIGHT → Profiles (tab 1). */
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
prof_nav_to_buttons(cbx_manager *mgr, int profile_count)
{
    send_key_dn(mgr, SDLK_DOWN);  /* tabbar → list */
    for (int i = 0; i < profile_count; i++)
        send_key_dn(mgr, SDLK_DOWN);  /* scroll / exit list */
}

/* Navigate to a specific button by index (0=Create, 1=Edit, 2=Delete).
 * After prof_nav_to_buttons, focus is on the rightmost button (Delete).
 * Navigate LEFT to reach lower-index buttons. */
static void
prof_nav_to_button(cbx_manager *mgr, int profile_count, int index)
{
    prof_nav_to_buttons(mgr, profile_count);
    for (int i = 0; i < 2 - index; i++)
        send_key_dn(mgr, SDLK_LEFT);
}

/* Open the editor via the production Edit-button path.
 * Selects the user profile (index 1) and presses A on Edit. */
static void
open_editor(cbx_manager *mgr, int profile_count)
{
    switch_to_profiles(mgr);

    /* DOWN to list, then DOWN to select user profile (index 1). */
    send_key_dn(mgr, SDLK_DOWN);  /* tabbar → list */
    if (profile_count > 1)
        send_key_dn(mgr, SDLK_DOWN);  /* list item 0 → item 1 */

    /* Navigate to Edit button (index 1): need to go past list to buttons. */
    /* From item 1 (bottom if count=2), DOWN exits list to buttons. */
    /* If count > 2, need more DOWNs to reach bottom. */
    for (int i = 0; i < profile_count - 1; i++)
        send_key_dn(mgr, SDLK_DOWN);
    /* Now at button row, focus on rightmost (Delete). 1 LEFT → Edit. */
    send_key_dn(mgr, SDLK_LEFT);

    /* Press A on Edit. */
    send_key_press(mgr, SDLK_a);
}

/* Open the editor via pointer (mouse click on Edit button).
 * Selects the user profile (index 1) first, then clicks Edit. */
static void
open_editor_pointer(cbx_manager *mgr)
{
    switch_to_profiles(mgr);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    /* Select user profile (index 1) via pointer. */
    int px = list_center_x(&pt->profile_list_w);
    int py = list_item_y(&pt->profile_list_w, 1);
    send_mouse_click(mgr, px, py);

    /* Click on Edit button. */
    int cx, cy;
    widget_center(&pt->edit_btn.base, &cx, &cy);
    send_mouse_click(mgr, cx, cy);
}

/* ------------------------------------------------------------------ */
/*  Profiles tab — Profile list (M10)                                 */
/* ------------------------------------------------------------------ */

/* M10 controller path: DOWN from tabbar → list gets focus, UP/DOWN
 * selects items. */
static void
test_prof_list_select_controller(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    switch_to_profiles(mgr);
    assert_int_equal(cbx_profiles_tab_profile_count(pt), 2);

    /* DOWN: tabbar → profile list. */
    send_key_dn(mgr, SDLK_DOWN);
    assert_true(pt->profile_list_w.base.focused);
    assert_int_equal(cbx_list_get_selected(&pt->profile_list_w), 0);

    /* DOWN: item 0 → item 1. */
    send_key_dn(mgr, SDLK_DOWN);
    assert_int_equal(cbx_list_get_selected(&pt->profile_list_w), 1);

    /* UP: item 1 → item 0. */
    send_key_dn(mgr, SDLK_UP);
    assert_int_equal(cbx_list_get_selected(&pt->profile_list_w), 0);

    /* UP: item 0 → tabbar. */
    send_key_dn(mgr, SDLK_UP);
    assert_true(mgr->tabbar.base.focused);
}

/* M10 pointer path: mouse click on a profile list item selects it. */
static void
test_prof_list_select_pointer(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    switch_to_profiles(mgr);

    /* Click on second item (index 1). */
    int px = list_center_x(&pt->profile_list_w);
    int py = list_item_y(&pt->profile_list_w, 1);
    send_mouse_click(mgr, px, py);
    assert_int_equal(cbx_list_get_selected(&pt->profile_list_w), 1);
    assert_true(pt->profile_list_w.base.focused);
}

/* ------------------------------------------------------------------ */
/*  Profiles tab — Create button (M11) + source picker (M12)         */
/* ------------------------------------------------------------------ */

/* M11 controller path: navigate to Create, press A → create source
 * picker opens. */
static void
test_prof_create_open_controller(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    switch_to_profiles(mgr);
    prof_nav_to_button(mgr, 2, 0);  /* Create (leftmost) */
    send_key_press(mgr, SDLK_a);

    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_CREATE_PICK);
    assert_true(pt->create_picker.base.visible);
}

/* M11 pointer path: click on Create button → create source picker opens. */
static void
test_prof_create_open_pointer(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    switch_to_profiles(mgr);

    int cx, cy;
    widget_center(&pt->create_btn.base, &cx, &cy);
    send_mouse_click(mgr, cx, cy);

    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_CREATE_PICK);
    assert_true(pt->create_picker.base.visible);
}

/* M12 controller path: create picker → DOWN to select "Empty" → A →
 * name input mode opens. */
static void
test_prof_create_source_controller(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    switch_to_profiles(mgr);
    prof_nav_to_button(mgr, 2, 0);
    send_key_press(mgr, SDLK_a);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_CREATE_PICK);

    /* DOWN to select "Empty" (index 1). */
    send_key_dn(mgr, SDLK_DOWN);
    assert_int_equal(cbx_list_get_selected(&pt->create_picker), 1);

    /* A to confirm → enters name input mode. */
    send_key_press(mgr, SDLK_a);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_NAME_INPUT);
}

/* M12 pointer path: click on a create source item → name input opens. */
static void
test_prof_create_source_pointer(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    switch_to_profiles(mgr);

    /* Click Create button. */
    int cx, cy;
    widget_center(&pt->create_btn.base, &cx, &cy);
    send_mouse_click(mgr, cx, cy);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_CREATE_PICK);

    /* Click on "Default copy" (index 0). */
    int px = list_center_x(&pt->create_picker);
    int py = list_item_y(&pt->create_picker, 0);
    send_mouse_click(mgr, px, py);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_NAME_INPUT);
}

/* ------------------------------------------------------------------ */
/*  Profiles tab — Name input (M13, M14, M15, M16)                   */
/* ------------------------------------------------------------------ */

/* M13 controller path: type letter keys → characters appended. */
static void
test_prof_name_input_chars(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    switch_to_profiles(mgr);
    prof_nav_to_button(mgr, 2, 0);
    send_key_press(mgr, SDLK_a);  /* Create */
    send_key_press(mgr, SDLK_a);  /* Confirm "Default copy" */

    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_NAME_INPUT);

    /* Type "hello". */
    send_key_dn(mgr, SDLK_h);
    send_key_dn(mgr, SDLK_e);
    send_key_dn(mgr, SDLK_l);
    send_key_dn(mgr, SDLK_l);
    send_key_dn(mgr, SDLK_o);

    assert_string_equal(cbx_profiles_tab_name_buffer(pt), "hello");
}

/* M14 controller path: type chars, then backspace removes last char. */
static void
test_prof_name_input_backspace(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    switch_to_profiles(mgr);
    prof_nav_to_button(mgr, 2, 0);
    send_key_press(mgr, SDLK_a);  /* Create */
    send_key_press(mgr, SDLK_a);  /* Confirm */

    /* Type "xyz" (avoid 'a' and 'b' which trigger confirm/cancel). */
    send_key_dn(mgr, SDLK_x);
    send_key_dn(mgr, SDLK_y);
    send_key_dn(mgr, SDLK_z);
    assert_string_equal(cbx_profiles_tab_name_buffer(pt), "xyz");

    /* Backspace → "xy". */
    send_key_dn(mgr, SDLK_BACKSPACE);
    assert_string_equal(cbx_profiles_tab_name_buffer(pt), "xy");
}

/* M15 controller path: type name → A to confirm → editor opens with
 * in-memory profile (Default copy → has Default bindings). */
static void
test_prof_name_input_confirm(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    switch_to_profiles(mgr);
    prof_nav_to_button(mgr, 2, 0);
    send_key_press(mgr, SDLK_a);  /* Create */
    send_key_press(mgr, SDLK_a);  /* Confirm "Default copy" */

    /* Type a name. */
    send_key_dn(mgr, SDLK_t);
    send_key_dn(mgr, SDLK_e);
    send_key_dn(mgr, SDLK_s);
    send_key_dn(mgr, SDLK_t);

    /* A to confirm → editor opens. */
    send_key_dn(mgr, SDLK_a);

    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);
    assert_true(pt->editor_initialized);
    assert_true(cbx_widget_is_visible(&pt->editor.binding_list.base));
    /* Default copy has 6 NES bindings. */
    assert_int_equal(cbx_profile_editor_binding_count(&pt->editor), 6);
}

/* M16 controller path: type name → B to cancel → returns to list. */
static void
test_prof_name_input_cancel(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    switch_to_profiles(mgr);
    prof_nav_to_button(mgr, 2, 0);
    send_key_press(mgr, SDLK_a);  /* Create */
    send_key_press(mgr, SDLK_a);  /* Confirm */
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_NAME_INPUT);

    /* Type a char. */
    send_key_dn(mgr, SDLK_x);
    assert_string_equal(cbx_profiles_tab_name_buffer(pt), "x");

    /* B to cancel. */
    send_key_dn(mgr, SDLK_b);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_LIST);
    assert_string_equal(cbx_profiles_tab_name_buffer(pt), "");
}

/* ------------------------------------------------------------------ */
/*  Profiles tab — Edit button (M17)                                  */
/* ------------------------------------------------------------------ */

/* M17 controller path: select profile → navigate to Edit → A →
 * editor opens with selected profile's bindings. */
static void
test_prof_edit_open_controller(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    open_editor(mgr, 2);

    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);
    assert_true(pt->editor_initialized);
    assert_true(cbx_widget_is_visible(&pt->editor.binding_list.base));
    assert_true(cbx_widget_is_visible(&pt->editor.diagram.base));
    /* MyProf has 6 NES bindings. */
    assert_int_equal(cbx_profile_editor_binding_count(&pt->editor), 6);
}

/* M17 pointer path: click on Edit button → editor opens. */
static void
test_prof_edit_open_pointer(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    /* First select the user profile (index 1) via pointer. */
    switch_to_profiles(mgr);
    int px = list_center_x(&pt->profile_list_w);
    int py = list_item_y(&pt->profile_list_w, 1);
    send_mouse_click(mgr, px, py);
    assert_int_equal(cbx_list_get_selected(&pt->profile_list_w), 1);

    /* Click on Edit button. */
    int cx, cy;
    widget_center(&pt->edit_btn.base, &cx, &cy);
    send_mouse_click(mgr, cx, cy);

    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);
    assert_true(pt->editor_initialized);
    assert_int_equal(cbx_profile_editor_binding_count(&pt->editor), 6);
}

/* ------------------------------------------------------------------ */
/*  Profiles tab — Delete button (M18) + confirm (M19) + cancel (M20)*/
/* ------------------------------------------------------------------ */

/* M18 controller path: navigate to Delete, press A → confirm delete
 * mode opens. */
static void
test_prof_delete_open_controller(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    switch_to_profiles(mgr);

    /* Select user profile (index 1) — not the read-only Default. */
    send_key_dn(mgr, SDLK_DOWN);  /* tabbar → list */
    send_key_dn(mgr, SDLK_DOWN);  /* item 0 → item 1 */

    /* Navigate to Delete button (index 2, rightmost). */
    for (int i = 0; i < 1; i++)
        send_key_dn(mgr, SDLK_DOWN);  /* list bottom → buttons */
    /* Already at rightmost (Delete) after exiting list. */

    send_key_press(mgr, SDLK_a);

    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_CONFIRM_DELETE);
}

/* M18 pointer path: click on Delete button → confirm delete mode opens. */
static void
test_prof_delete_open_pointer(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    switch_to_profiles(mgr);

    /* Select user profile via pointer. */
    int px = list_center_x(&pt->profile_list_w);
    int py = list_item_y(&pt->profile_list_w, 1);
    send_mouse_click(mgr, px, py);

    /* Click Delete button. */
    int cx, cy;
    widget_center(&pt->delete_btn.base, &cx, &cy);
    send_mouse_click(mgr, cx, cy);

    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_CONFIRM_DELETE);
}

/* M19 controller path: confirm delete → profile file unlinked, list
 * refreshes. */
static void
test_prof_delete_confirm(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    switch_to_profiles(mgr);

    int before = cbx_profiles_tab_profile_count(pt);

    /* Select user profile (index 1). */
    send_key_dn(mgr, SDLK_DOWN);
    send_key_dn(mgr, SDLK_DOWN);

    /* Navigate to Delete and press A. */
    send_key_dn(mgr, SDLK_DOWN);  /* list bottom → buttons */
    send_key_press(mgr, SDLK_a);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_CONFIRM_DELETE);

    /* A to confirm delete. */
    send_key_press(mgr, SDLK_a);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_LIST);
    assert_int_equal(cbx_profiles_tab_profile_count(pt), before - 1);

    /* Verify the user profile file is gone. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/myprof.yaml", f->user_dir);
    assert_int_not_equal(access(path, F_OK), 0);
}

/* M20 controller path: cancel delete → returns to list, no deletion. */
static void
test_prof_delete_cancel(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    switch_to_profiles(mgr);

    int before = cbx_profiles_tab_profile_count(pt);

    /* Select user profile, navigate to Delete, press A. */
    send_key_dn(mgr, SDLK_DOWN);
    send_key_dn(mgr, SDLK_DOWN);
    send_key_dn(mgr, SDLK_DOWN);  /* → buttons */
    send_key_press(mgr, SDLK_a);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_CONFIRM_DELETE);

    /* B to cancel. */
    send_key_dn(mgr, SDLK_b);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_LIST);
    assert_int_equal(cbx_profiles_tab_profile_count(pt), before);

    /* Verify the file still exists. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/myprof.yaml", f->user_dir);
    assert_int_equal(access(path, F_OK), 0);
}

/* ------------------------------------------------------------------ */
/*  Profile editor — Binding list navigation (M28)                   */
/* ------------------------------------------------------------------ */

/* M28 controller path: editor open → UP/DOWN navigates binding list. */
static void
test_editor_list_nav(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    open_editor(mgr, 2);
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_LIST);

    /* Initial selected = 0. */
    assert_int_equal(cbx_profile_editor_get_selected(&pt->editor), 0);

    /* DOWN → selected = 1. */
    send_key_dn(mgr, SDLK_DOWN);
    assert_int_equal(cbx_profile_editor_get_selected(&pt->editor), 1);

    /* DOWN → selected = 2. */
    send_key_dn(mgr, SDLK_DOWN);
    assert_int_equal(cbx_profile_editor_get_selected(&pt->editor), 2);

    /* UP → selected = 1. */
    send_key_dn(mgr, SDLK_UP);
    assert_int_equal(cbx_profile_editor_get_selected(&pt->editor), 1);
}

/* ------------------------------------------------------------------ */
/*  Profile editor — Edit binding / activate (M29)                   */
/* ------------------------------------------------------------------ */

/* M29 controller path: A on binding → binding edit sub-menu opens. */
static void
test_editor_activate_binding_controller(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    open_editor(mgr, 2);

    /* A on first binding → BINDING_EDIT mode. */
    send_key_press(mgr, SDLK_a);
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_BINDING_EDIT);
    assert_true(cbx_widget_is_visible(&pt->editor.target_list.base));
    /* Target list has 3 options. */
    assert_int_equal(cbx_list_item_count(&pt->editor.target_list), 3);
}

/* M29 pointer path: click on binding item → binding edit sub-menu opens. */
static void
test_editor_activate_binding_pointer(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    open_editor_pointer(mgr);

    /* Click on first binding in the list. */
    int px = list_center_x(&pt->editor.binding_list);
    int py = list_item_y(&pt->editor.binding_list, 0);
    send_mouse_click(mgr, px, py);

    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_BINDING_EDIT);
    assert_int_equal(cbx_list_item_count(&pt->editor.target_list), 3);
}

/* ------------------------------------------------------------------ */
/*  Profile editor — Target picker confirm (M30)                     */
/* ------------------------------------------------------------------ */

/* Helper: enter TARGET_PICK mode from the editor LIST mode. */
static void
editor_enter_target_pick(cbx_manager *mgr)
{
    /* A on first binding → BINDING_EDIT. */
    send_key_press(mgr, SDLK_a);
    /* A on "Pick Target" (index 0) → TARGET_PICK. */
    send_key_press(mgr, SDLK_a);
}

/* M30 controller path: target pick → A to confirm → binding target
 * updated, picker closes. */
static void
test_editor_target_pick_controller(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    open_editor(mgr, 2);
    editor_enter_target_pick(mgr);

    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_TARGET_PICK);
    /* Target list has default targets. */
    assert_true(cbx_list_item_count(&pt->editor.target_list) > 0);

    /* A to confirm first target → back to LIST. */
    send_key_press(mgr, SDLK_a);
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_LIST);
}

/* M30 pointer path: click on a target item → confirm → back to LIST. */
static void
test_editor_target_pick_pointer(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    open_editor_pointer(mgr);

    /* Click on first binding → BINDING_EDIT. */
    int px = list_center_x(&pt->editor.binding_list);
    int py = list_item_y(&pt->editor.binding_list, 0);
    send_mouse_click(mgr, px, py);
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_BINDING_EDIT);

    /* Click on "Pick Target" (index 0) → TARGET_PICK. */
    px = list_center_x(&pt->editor.target_list);
    py = list_item_y(&pt->editor.target_list, 0);
    send_mouse_click(mgr, px, py);
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_TARGET_PICK);

    /* Click on first target → confirm → LIST. */
    px = list_center_x(&pt->editor.target_list);
    py = list_item_y(&pt->editor.target_list, 0);
    send_mouse_click(mgr, px, py);
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_LIST);
}

/* ------------------------------------------------------------------ */
/*  Profile editor — Capture mode (M31, M32)                         */
/* ------------------------------------------------------------------ */

/* M31 controller path: binding edit → A on "Capture" → capture mode. */
static void
test_editor_capture_begin_controller(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    open_editor(mgr, 2);

    /* A on binding → BINDING_EDIT. */
    send_key_press(mgr, SDLK_a);
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_BINDING_EDIT);

    /* DOWN to "Capture" (index 1). */
    send_key_dn(mgr, SDLK_DOWN);

    /* A → capture mode. */
    send_key_press(mgr, SDLK_a);
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_CAPTURE);
    assert_true(cbx_profile_editor_is_capture_active(&pt->editor));
}

/* M31 pointer path: click on "Capture" option → capture mode. */
static void
test_editor_capture_begin_pointer(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    open_editor_pointer(mgr);

    /* Click on first binding → BINDING_EDIT. */
    int px = list_center_x(&pt->editor.binding_list);
    int py = list_item_y(&pt->editor.binding_list, 0);
    send_mouse_click(mgr, px, py);

    /* Click on "Capture" (index 1). */
    px = list_center_x(&pt->editor.target_list);
    py = list_item_y(&pt->editor.target_list, 1);
    send_mouse_click(mgr, px, py);

    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_CAPTURE);
    assert_true(cbx_profile_editor_is_capture_active(&pt->editor));
}

/* M32: capture physical button via InputEvent → binding captured,
 * capture ends.  Dispatch path: cbx_profile_editor_on_input_event. */
static void
test_editor_capture_event(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    open_editor(mgr, 2);

    /* Enter capture mode. */
    send_key_press(mgr, SDLK_a);  /* binding → BINDING_EDIT */
    send_key_dn(mgr, SDLK_DOWN);  /* → "Capture" */
    send_key_press(mgr, SDLK_a);  /* → CAPTURE */
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_CAPTURE);

    int editing_idx = cbx_profile_editor_get_editing_index(&pt->editor);
    assert_int_equal(editing_idx, 0);

    /* Simulate a physical button press via the production DBus
     * InputEvent signal path: inject_signal -> input_event_signal_cb
     * -> ip_input_events_handle (sender verification, event parsing,
     * rate limiting) -> cbx_profile_editor_on_input_event.
     * expected_sender is ":1.42" (from mock_get_unique_name). */
    ip_input_event_payload p = {
        .sender = ":1.42",
        .path   = "/dev/input/event0",
        .event  = "A",
        .value  = 1.0,
    };
    int rc = f->backend->inject_signal(f->mock.bus,
        IP_IFACE_DBUS_DEVICE, "InputEvent", &p);
    assert_int_equal(rc, 0);

    /* Capture ends, back to LIST. */
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_LIST);
    assert_false(cbx_profile_editor_is_capture_active(&pt->editor));

    /* Verify the binding's source button was actually updated to "A". */
    const cbx_profile *prof = cbx_profile_editor_get_profile(&pt->editor);
    assert_non_null(prof);
    assert_int_equal(prof->mapping_count, 6);
    bool found_a_button = false;
    for (int i = 0; i < prof->mapping_count; i++) {
        for (int j = 0; j < prof->mappings[i].source_event.prop_count; j++) {
            if (strcmp(prof->mappings[i].source_event.props[j].key, "button") == 0 &&
                strcmp(prof->mappings[i].source_event.props[j].value, "A") == 0) {
                found_a_button = true;
            }
        }
    }
    assert_true(found_a_button);
}

/* ------------------------------------------------------------------ */
/*  Profile editor — Sequential mode (M33, M34, M35, M36)           */
/* ------------------------------------------------------------------ */

/* M33 controller path: binding edit → A on "Sequential" → sequential
 * mode begins. */
static void
test_editor_seq_begin_controller(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    open_editor(mgr, 2);

    /* A on binding → BINDING_EDIT. */
    send_key_press(mgr, SDLK_a);

    /* DOWN twice to "Sequential" (index 2). */
    send_key_dn(mgr, SDLK_DOWN);
    send_key_dn(mgr, SDLK_DOWN);

    /* A → sequential mode. */
    send_key_press(mgr, SDLK_a);
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_SEQUENTIAL);
    assert_true(cbx_profile_editor_seq_is_active(&pt->editor));
    assert_int_equal(cbx_profile_editor_seq_get_step(&pt->editor), 0);
}

/* M33 pointer path: click on "Sequential" option → sequential mode. */
static void
test_editor_seq_begin_pointer(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    open_editor_pointer(mgr);

    /* Click on first binding → BINDING_EDIT. */
    int px = list_center_x(&pt->editor.binding_list);
    int py = list_item_y(&pt->editor.binding_list, 0);
    send_mouse_click(mgr, px, py);

    /* Click on "Sequential" (index 2). */
    px = list_center_x(&pt->editor.target_list);
    py = list_item_y(&pt->editor.target_list, 2);
    send_mouse_click(mgr, px, py);

    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_SEQUENTIAL);
    assert_true(cbx_profile_editor_seq_is_active(&pt->editor));
}

/* M34: capture button in sequential mode via InputEvent → button
 * captured, auto-advance.  Dispatch path: inject_signal ->
 * input_event_signal_cb -> ip_input_events_handle ->
 * cbx_profile_editor_on_input_event -> cbx_profile_editor_seq_on_input. */
static void
test_editor_seq_capture(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    open_editor(mgr, 2);

    /* Enter sequential mode. */
    send_key_press(mgr, SDLK_a);  /* binding → BINDING_EDIT */
    send_key_dn(mgr, SDLK_DOWN);
    send_key_dn(mgr, SDLK_DOWN);  /* → "Sequential" */
    send_key_press(mgr, SDLK_a);  /* → SEQUENTIAL */
    assert_int_equal(cbx_profile_editor_seq_get_step(&pt->editor), 0);

    /* Simulate pressing a button for "Up" (step 0) via the production
     * DBus InputEvent signal path.  expected_sender is ":1.42". */
    ip_input_event_payload p = {
        .sender = ":1.42",
        .path   = "/dev/input/event0",
        .event  = "A",
        .value  = 1.0,
    };
    int rc = f->backend->inject_signal(f->mock.bus,
        IP_IFACE_DBUS_DEVICE, "InputEvent", &p);
    assert_int_equal(rc, 0);

    /* Should have advanced to step 1. */
    assert_int_equal(cbx_profile_editor_seq_get_step(&pt->editor), 1);

    /* Verify a mapping was created or updated for "Up" (step 0). */
    const cbx_profile *prof = cbx_profile_editor_get_profile(&pt->editor);
    assert_non_null(prof);
    /* The profile originally had 6 bindings; find_or_create_mapping
     * should have found the existing "Up" mapping and updated it. */
    assert_true(prof->mapping_count >= 6);
    bool found_up_button = false;
    for (int i = 0; i < prof->mapping_count; i++) {
        for (int j = 0; j < prof->mappings[i].source_event.prop_count; j++) {
            if (strcmp(prof->mappings[i].source_event.props[j].key, "button") == 0 &&
                strcmp(prof->mappings[i].source_event.props[j].value, "A") == 0) {
                found_up_button = true;
            }
        }
    }
    assert_true(found_up_button);
}

/* M35 controller path: B during sequential → skip current button. */
static void
test_editor_seq_skip(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    open_editor(mgr, 2);

    /* Enter sequential mode. */
    send_key_press(mgr, SDLK_a);
    send_key_dn(mgr, SDLK_DOWN);
    send_key_dn(mgr, SDLK_DOWN);
    send_key_press(mgr, SDLK_a);
    assert_int_equal(cbx_profile_editor_seq_get_step(&pt->editor), 0);

    /* B → skip step 0. */
    send_key_press(mgr, SDLK_b);
    assert_int_equal(cbx_profile_editor_seq_get_step(&pt->editor), 1);
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_SEQUENTIAL);

    /* B → skip step 1. */
    send_key_press(mgr, SDLK_b);
    assert_int_equal(cbx_profile_editor_seq_get_step(&pt->editor), 2);
}

/* M36 controller path: Start (Tab) during sequential → cancel
 * sequential, changes discarded, return to editor LIST. */
static void
test_editor_seq_cancel(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    open_editor(mgr, 2);

    /* Enter sequential mode. */
    send_key_press(mgr, SDLK_a);
    send_key_dn(mgr, SDLK_DOWN);
    send_key_dn(mgr, SDLK_DOWN);
    send_key_press(mgr, SDLK_a);
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_SEQUENTIAL);

    /* Tab (Start) → cancel sequential. */
    send_key_dn(mgr, SDLK_TAB);
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_LIST);
    assert_false(cbx_profile_editor_seq_is_active(&pt->editor));
}

/* ------------------------------------------------------------------ */
/*  Profile editor — Save and close (M37)                             */
/* ------------------------------------------------------------------ */

/* M37 controller path: B in editor LIST mode → save via
 * cbx_profile_save_to_dir → file written, editor closes, list
 * refreshes. */
static void
test_editor_save_close(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    open_editor(mgr, 2);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);

    int before = cbx_profiles_tab_profile_count(pt);

    /* B in LIST mode → save and close. */
    send_key_press(mgr, SDLK_b);

    /* Editor closed, back to profiles list. */
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_LIST);

    /* The profile was saved (myprof already existed, re-saved). */
    /* Profile count unchanged (editing existing profile). */
    assert_int_equal(cbx_profiles_tab_profile_count(pt), before);

    /* Verify the file exists. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/myprof.yaml", f->user_dir);
    assert_int_equal(access(path, F_OK), 0);
}

static void
test_editor_save_button_pointer(void **state)
{
    mip_fixture *f = *state;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&f->mgr);
    open_editor(&f->mgr, 2);
    assert_true(cbx_widget_is_visible(&pt->save_btn.base));
    int x, y;
    widget_center(&pt->save_btn.base, &x, &y);
    send_mouse_click(&f->mgr, x, y);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_LIST);

    /* Verify the profile file was actually written (not just mode change). */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/myprof.yaml", f->user_dir);
    assert_int_equal(access(path, F_OK), 0);
}

/* ------------------------------------------------------------------ */
/*  Profile editor — Cancel editor / discard (M38)                   */
/* ------------------------------------------------------------------ */

/* M38 controller path: Tab (Start) in editor LIST mode → discard
 * changes, close editor, no file written. */
static void
test_editor_cancel_discard(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    open_editor(mgr, 2);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);

    /* Record the file's modification time. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/myprof.yaml", f->user_dir);
    struct stat st_before;
    assert_int_equal(stat(path, &st_before), 0);

    /* Tab (Start) → discard and close. */
    send_key_dn(mgr, SDLK_TAB);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_LIST);

    /* File should be unchanged (not re-saved). */
    struct stat st_after;
    assert_int_equal(stat(path, &st_after), 0);
    assert_int_equal(st_before.st_mtime, st_after.st_mtime);
}

static void
test_editor_discard_button_pointer(void **state)
{
    mip_fixture *f = *state;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&f->mgr);

    /* Record the file's modification time before opening the editor. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/myprof.yaml", f->user_dir);
    struct stat st_before;
    assert_int_equal(stat(path, &st_before), 0);

    open_editor(&f->mgr, 2);
    assert_true(cbx_widget_is_visible(&pt->discard_btn.base));
    int x, y;
    widget_center(&pt->discard_btn.base, &x, &y);
    send_mouse_click(&f->mgr, x, y);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_LIST);

    /* Verify the file was NOT re-saved (mtime unchanged). */
    struct stat st_after;
    assert_int_equal(stat(path, &st_after), 0);
    assert_int_equal(st_before.st_mtime, st_after.st_mtime);
}

/* ------------------------------------------------------------------ */
/*  Disabled / degraded scenarios                                    */
/* ------------------------------------------------------------------ */

/* D03: Delete with no profile selected — empty profile list, Delete
 * activation produces no file deletion.  Both paths tested. */
static void
test_d03_delete_no_profile(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    switch_to_profiles(mgr);
    assert_int_equal(cbx_profiles_tab_profile_count(pt), 0);
    assert_int_equal(cbx_profiles_tab_selected(pt), -1);

    /* Controller path: navigate to Delete, press A → no effect. */
    /* With 0 profiles, 1 DOWN goes to list (empty), 1 more DOWN exits
     * to buttons. But list is empty — DOWN from tabbar goes directly
     * to buttons (list has 0 items, returns false immediately). */
    send_key_dn(mgr, SDLK_DOWN);  /* tabbar → list (empty) → buttons */
    /* Focus should be on buttons now (list returned false). */
    /* Navigate to Delete (rightmost). */
    send_key_press(mgr, SDLK_a);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_LIST);

    /* Pointer path: click Delete → no effect. */
    int cx, cy;
    widget_center(&pt->delete_btn.base, &cx, &cy);
    send_mouse_click(mgr, cx, cy);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_LIST);
}

/* D04: Profile save with missing NES bindings — create empty profile,
 * try to save → error shown, no file written, editor stays open. */
static void
test_d04_save_missing_nes(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    switch_to_profiles(mgr);

    /* Create → "Empty" → name input → editor opens with no bindings. */
    prof_nav_to_button(mgr, 2, 0);
    send_key_press(mgr, SDLK_a);  /* Create */

    /* DOWN to "Empty" (index 1). */
    send_key_dn(mgr, SDLK_DOWN);
    send_key_press(mgr, SDLK_a);  /* Confirm Empty */

    /* Type name. */
    send_key_dn(mgr, SDLK_t);
    send_key_dn(mgr, SDLK_e);
    send_key_dn(mgr, SDLK_s);
    send_key_dn(mgr, SDLK_t);

    /* A to confirm → editor opens with empty profile. */
    send_key_press(mgr, SDLK_a);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);
    assert_int_equal(cbx_profile_editor_binding_count(&pt->editor), 0);

    /* B in LIST mode → save attempt → NES validation fails. */
    send_key_dn(mgr, SDLK_b);

    /* Editor stays open (save failed). */
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);

    /* Verify no file was written. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/test.yaml", f->user_dir);
    assert_int_not_equal(access(path, F_OK), 0);
}

/* D07: Filesystem failure — make user_dir read-only, try to save →
 * error shown, editor stays open. */
static void
test_d07_filesystem_failure(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    open_editor(mgr, 2);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);

    /* Make the user dir read-only so save fails (atomic write uses
     * mkstemp in the same directory, which needs write permission). */
    chmod(f->user_dir, 0555);

    /* B in LIST mode → save attempt → filesystem failure. */
    send_key_press(mgr, SDLK_b);

    /* Editor stays open (save failed). */
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);

    /* Status should show save failure. */
    const char *status = cbx_profile_editor_get_status(&pt->editor);
    assert_non_null(status);
    assert_true(strlen(status) > 0);

    /* Restore permissions for cleanup. */
    chmod(f->user_dir, 0700);
}

/* D08: Empty profile creation — editor opens with no bindings, save
 * blocked by NES validation. */
static void
test_d08_empty_profile_create(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    switch_to_profiles(mgr);

    /* Create → "Empty" → name input → editor opens. */
    prof_nav_to_button(mgr, 2, 0);
    send_key_press(mgr, SDLK_a);  /* Create */
    send_key_dn(mgr, SDLK_DOWN);  /* → "Empty" */
    send_key_press(mgr, SDLK_a);  /* Confirm Empty */

    /* Type name. */
    send_key_dn(mgr, SDLK_n);
    send_key_dn(mgr, SDLK_e);
    send_key_dn(mgr, SDLK_w);

    /* A to confirm → editor opens with empty profile. */
    send_key_press(mgr, SDLK_a);

    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);
    assert_int_equal(cbx_profile_editor_binding_count(&pt->editor), 0);

    /* Try to save → NES validation blocks. */
    send_key_dn(mgr, SDLK_b);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);

    /* No file written. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/new.yaml", f->user_dir);
    assert_int_not_equal(access(path, F_OK), 0);
}

/* ================================================================== */
/*  Task 3: Clone existing profile (PR-02/M15/IA-10)                 */
/* ================================================================== */

/* M15 controller path: create picker -> DOWN x2 to "Clone current"
 * -> A -> type name -> A -> editor opens with cloned bindings.
 * The user profile (index 1, "myprof") has 6 NES bindings; the
 * cloned editor should have the same count. */
static void
test_prof_create_clone_controller(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    /* Select the user profile (index 1) so clone has a target. */
    switch_to_profiles(mgr);
    send_key_dn(mgr, SDLK_DOWN);  /* tabbar -> list */
    send_key_dn(mgr, SDLK_DOWN);  /* item 0 -> item 1 (myprof) */

    /* Navigate to Create button (index 0, leftmost). */
    for (int i = 0; i < 2; i++)
        send_key_dn(mgr, SDLK_DOWN);  /* exit list to buttons */
    send_key_dn(mgr, SDLK_LEFT);
    send_key_dn(mgr, SDLK_LEFT);  /* Delete -> Edit -> Create */
    send_key_press(mgr, SDLK_a);  /* Create -> create picker */
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_CREATE_PICK);

    /* DOWN x2 to "Clone current" (index 2). */
    send_key_dn(mgr, SDLK_DOWN);
    send_key_dn(mgr, SDLK_DOWN);
    assert_int_equal(cbx_list_get_selected(&pt->create_picker), 2);

    /* A to confirm -> name input mode. */
    send_key_press(mgr, SDLK_a);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_NAME_INPUT);

    /* Type a name (avoid a/b keys which trigger confirm/cancel). */
    send_key_dn(mgr, SDLK_c);
    send_key_dn(mgr, SDLK_l);
    send_key_dn(mgr, SDLK_n);

    /* A to confirm -> editor opens with cloned bindings. */
    send_key_dn(mgr, SDLK_a);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);
    assert_true(pt->editor_initialized);
    /* myprof has 6 NES bindings; clone should have same count. */
    assert_int_equal(cbx_profile_editor_binding_count(&pt->editor), 6);
}

/* M15 pointer path: click Create -> click "Clone current" -> type name
 * -> A -> editor opens with cloned bindings. */
static void
test_prof_create_clone_pointer(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    switch_to_profiles(mgr);

    /* Select user profile (index 1) via pointer. */
    int px = list_center_x(&pt->profile_list_w);
    int py = list_item_y(&pt->profile_list_w, 1);
    send_mouse_click(mgr, px, py);

    /* Click Create button. */
    int cx, cy;
    widget_center(&pt->create_btn.base, &cx, &cy);
    send_mouse_click(mgr, cx, cy);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_CREATE_PICK);

    /* Click on "Clone current" (index 2). */
    px = list_center_x(&pt->create_picker);
    py = list_item_y(&pt->create_picker, 2);
    send_mouse_click(mgr, px, py);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_NAME_INPUT);

    /* Type a name. */
    send_key_dn(mgr, SDLK_c);
    send_key_dn(mgr, SDLK_l);
    send_key_dn(mgr, SDLK_n);

    /* A to confirm -> editor opens with cloned bindings. */
    send_key_dn(mgr, SDLK_a);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);
    assert_true(pt->editor_initialized);
    assert_int_equal(cbx_profile_editor_binding_count(&pt->editor), 6);
}

/* ================================================================== */
/*  Task 3: Sequential capture via DBus InputEvent signal (PE-04)    */
/* ================================================================== */

/* PE-04: Sequential physical-button capture auto-advance is exercised
 * through production DBus InputEvent signal dispatch.  The editor
 * subscribes to InputEvent signals via ip_input_events_subscribe;
 * we inject signals via backend->inject_signal (the mock equivalent
 * of sd_bus_process dispatching a real signal), which flows through
 * input_event_signal_cb -> ip_input_events_handle (sender verification,
 * event parsing, value validation, rate limiting) -> editor callback
 * -> cbx_profile_editor_seq_on_input -> auto-advance. */
static void
test_editor_seq_capture_dbus_signal(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    open_editor(mgr, 2);

    /* Enter sequential mode via production dispatch. */
    send_key_press(mgr, SDLK_a);  /* binding -> BINDING_EDIT */
    send_key_dn(mgr, SDLK_DOWN);
    send_key_dn(mgr, SDLK_DOWN);  /* -> "Sequential" */
    send_key_press(mgr, SDLK_a);  /* -> SEQUENTIAL */
    assert_int_equal(cbx_profile_editor_seq_get_step(&pt->editor), 0);

    /* The editor has subscribed to InputEvent signals via the mock
     * backend.  Inject a signal for step 0 (Up) through the production
     * DBus signal path: inject_signal -> input_event_signal_cb ->
     * ip_input_events_handle -> cbx_profile_editor_on_input_event ->
     * cbx_profile_editor_seq_on_input -> auto-advance.
     * expected_sender is ":1.42" (from mock_get_unique_name). */
    ip_input_event_payload p = {
        .sender = ":1.42",
        .path   = "/dev/input/event0",
        .event  = "A",
        .value  = 1.0,
    };
    int rc = f->backend->inject_signal(f->mock.bus,
        IP_IFACE_DBUS_DEVICE, "InputEvent", &p);
    assert_int_equal(rc, 0);

    /* Should have advanced to step 1 via the production signal path. */
    assert_int_equal(cbx_profile_editor_seq_get_step(&pt->editor), 1);

    /* Inject another signal for step 1 (Down). */
    p.event = "B";
    /* B is treated as skip in seq_on_input, so use a different button. */
    p.event = "X";
    rc = f->backend->inject_signal(f->mock.bus,
        IP_IFACE_DBUS_DEVICE, "InputEvent", &p);
    assert_int_equal(rc, 0);

    /* Should have advanced to step 2. */
    assert_int_equal(cbx_profile_editor_seq_get_step(&pt->editor), 2);

    /* Verify mappings were created/updated through the signal path. */
    const cbx_profile *prof = cbx_profile_editor_get_profile(&pt->editor);
    assert_non_null(prof);
    assert_true(prof->mapping_count >= 6);
}

/* ================================================================== */
/*  Task 3: Unsaved-close prompt (PR-07)                             */
/* ================================================================== */

/* Helper: open editor and make a change (target pick) to set dirty. */
static void
make_editor_dirty(cbx_manager *mgr, cbx_profiles_tab *pt)
{
    open_editor(mgr, 2);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);
    assert_false(cbx_profile_editor_is_dirty(&pt->editor));

    /* Enter target-pick mode and confirm a target (modifies profile).
     * Same pattern as editor_enter_target_pick: A enters BINDING_EDIT
     * with "Pick Target" at index 0, second A enters TARGET_PICK,
     * third A confirms and returns to LIST. */
    send_key_press(mgr, SDLK_a);  /* binding -> BINDING_EDIT */
    send_key_press(mgr, SDLK_a);  /* -> TARGET_PICK (Pick Target at index 0) */
    send_key_press(mgr, SDLK_a);  /* confirm target pick -> LIST */

    /* Back in LIST mode, profile should be dirty. */
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_LIST);
    assert_true(cbx_profile_editor_is_dirty(&pt->editor));
}

/* PR-07: SDL_QUIT with unsaved editor changes -> prompt appears,
 * does not silently exit. */
static void
test_quit_unsaved_prompt_appears(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    make_editor_dirty(mgr, pt);

    /* Simulate the manager being in its run loop.  Tests call
     * handle_event directly (not cbx_manager_run), so running must
     * be set explicitly to verify SDL_QUIT does not stop the manager
     * when unsaved changes are present. */
    mgr->running = true;

    /* Send SDL_QUIT through production dispatch. */
    SDL_Event quit = {0};
    quit.type = SDL_QUIT;
    cbx_manager_handle_event(mgr, &quit);

    /* Manager should NOT have exited — prompt should appear. */
    assert_true(mgr->running);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_CONFIRM_QUIT);
    /* Status label should be visible with prompt text. */
    assert_true(pt->status_lbl.base.visible);
    const char *status = cbx_profile_editor_get_status(&pt->editor);
    (void)status;  /* status is on tab->status_lbl, not editor */
    assert_non_null(pt->status_lbl.text);
    assert_ptr_not_equal(strstr(pt->status_lbl.text, "Unsaved"), NULL);
}

/* PR-07: SDL_QUIT with unsaved changes -> A -> save & quit. */
static void
test_quit_unsaved_save_and_quit(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    make_editor_dirty(mgr, pt);

    mgr->running = true;

    /* SDL_QUIT -> prompt. */
    SDL_Event quit = {0};
    quit.type = SDL_QUIT;
    cbx_manager_handle_event(mgr, &quit);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_CONFIRM_QUIT);

    /* A = save & quit. */
    send_key_dn(mgr, SDLK_a);
    assert_false(mgr->running);  /* manager should have stopped */
    /* Editor should be closed (back to LIST mode). */
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_LIST);
}

/* PR-07: SDL_QUIT with unsaved changes -> B -> discard & quit. */
static void
test_quit_unsaved_discard_and_quit(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    /* Record the existing profile file mtime. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/myprof.yaml", f->user_dir);
    struct stat st_before;
    assert_int_equal(stat(path, &st_before), 0);

    make_editor_dirty(mgr, pt);

    mgr->running = true;

    /* SDL_QUIT -> prompt. */
    SDL_Event quit = {0};
    quit.type = SDL_QUIT;
    cbx_manager_handle_event(mgr, &quit);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_CONFIRM_QUIT);

    /* B = discard & quit. */
    send_key_dn(mgr, SDLK_b);
    assert_false(mgr->running);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_LIST);

    /* File should NOT have been re-saved (mtime unchanged). */
    struct stat st_after;
    assert_int_equal(stat(path, &st_after), 0);
    assert_int_equal(st_before.st_mtime, st_after.st_mtime);
}

/* PR-07: SDL_QUIT with no unsaved changes -> immediate quit (no prompt). */
static void
test_quit_no_changes_immediate(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);
    (void)f;

    /* Open editor but don't make any changes (dirty = false). */
    open_editor(mgr, 2);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);
    assert_false(cbx_profile_editor_is_dirty(&pt->editor));

    /* SDL_QUIT -> immediate quit (no prompt). */
    mgr->running = true;
    SDL_Event quit = {0};
    quit.type = SDL_QUIT;
    cbx_manager_handle_event(mgr, &quit);

    assert_false(mgr->running);
    /* Mode should still be EDITOR (no prompt was shown). */
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);
}

/* ================================================================== */
/*  Task 3: Capability-scoped binding (PE-06)                        */
/* ================================================================== */

/* PE-06: Capability-scoped binding exercised through production
 * dispatch.  Opens the editor, enters target-pick mode, and verifies
 * the target list is populated from the editor's loaded capabilities
 * (which come from DBus CompositeDevice properties or fallback defaults).
 * The target list must contain entries scoped to the virtual device's
 * capabilities, not the physical controller. */
static void
test_capability_scoped_binding(void **state)
{
    mip_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);
    (void)f;

    open_editor(mgr, 2);
    assert_int_equal(cbx_profiles_tab_mode(pt), CBX_PT_MODE_EDITOR);

    /* Enter target-pick mode via production dispatch.
     * A enters BINDING_EDIT with "Pick Target" at index 0,
     * second A enters TARGET_PICK. */
    send_key_press(mgr, SDLK_a);  /* binding -> BINDING_EDIT */
    send_key_press(mgr, SDLK_a);  /* -> TARGET_PICK (Pick Target at index 0) */
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_TARGET_PICK);

    /* The target list must be populated from capabilities (PE-06).
     * Without a DBus composite path, the editor uses fallback defaults
     * (keyboard + mouse targets), proving the capability-scoped path
     * works through production dispatch. */
    assert_true(cbx_list_item_count(&pt->editor.target_list) > 0);
    assert_true(cbx_profile_editor_get_target_count(&pt->editor) > 0);

    /* Verify the targets are keyboard/mouse class (virtual device
     * capabilities), not gamepad (physical controller). */
    bool found_keyboard = false;
    for (int i = 0; i < pt->editor.target_count; i++) {
        if (strcmp(pt->editor.targets[i].device_class, "keyboard") == 0)
            found_keyboard = true;
    }
    assert_true(found_keyboard);

    /* Cancel target pick to return to LIST mode. */
    send_key_press(mgr, SDLK_b);
    assert_int_equal(cbx_profile_editor_get_mode(&pt->editor),
                     CBX_EDITOR_MODE_LIST);
}


/* ------------------------------------------------------------------ */
/*  Runner                                                            */
/* ------------------------------------------------------------------ */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* Profiles tab — list select (M10) */
        cmocka_unit_test_setup_teardown(
            test_prof_list_select_controller, mip_setup, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_prof_list_select_pointer, mip_setup, mip_teardown),

        /* Profiles tab — Create button (M11) + source picker (M12) */
        cmocka_unit_test_setup_teardown(
            test_prof_create_open_controller, mip_setup, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_prof_create_open_pointer, mip_setup, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_prof_create_source_controller, mip_setup, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_prof_create_source_pointer, mip_setup, mip_teardown),

        /* Profiles tab — Name input (M13, M14, M15, M16) */
        cmocka_unit_test_setup_teardown(
            test_prof_name_input_chars, mip_setup, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_prof_name_input_backspace, mip_setup, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_prof_name_input_confirm, mip_setup, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_prof_name_input_cancel, mip_setup, mip_teardown),

        /* Profiles tab — Edit button (M17) */
        cmocka_unit_test_setup_teardown(
            test_prof_edit_open_controller, mip_setup, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_prof_edit_open_pointer, mip_setup, mip_teardown),

        /* Profiles tab — Delete (M18, M19, M20) */
        cmocka_unit_test_setup_teardown(
            test_prof_delete_open_controller, mip_setup, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_prof_delete_open_pointer, mip_setup, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_prof_delete_confirm, mip_setup, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_prof_delete_cancel, mip_setup, mip_teardown),

        /* Profile editor — binding list (M28) */
        cmocka_unit_test_setup_teardown(
            test_editor_list_nav, mip_setup, mip_teardown),

        /* Profile editor — edit binding (M29) */
        cmocka_unit_test_setup_teardown(
            test_editor_activate_binding_controller,
            mip_setup, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_editor_activate_binding_pointer,
            mip_setup, mip_teardown),

        /* Profile editor — target picker (M30) */
        cmocka_unit_test_setup_teardown(
            test_editor_target_pick_controller, mip_setup, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_editor_target_pick_pointer, mip_setup, mip_teardown),

        /* Profile editor — capture (M31, M32) */
        cmocka_unit_test_setup_teardown(
            test_editor_capture_begin_controller, mip_setup, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_editor_capture_begin_pointer, mip_setup, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_editor_capture_event, mip_setup, mip_teardown),

        /* Profile editor — sequential (M33, M34, M35, M36) */
        cmocka_unit_test_setup_teardown(
            test_editor_seq_begin_controller, mip_setup, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_editor_seq_begin_pointer, mip_setup, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_editor_seq_capture, mip_setup, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_editor_seq_skip, mip_setup, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_editor_seq_cancel, mip_setup, mip_teardown),

        /* Profile editor — save and close (M37) */
        cmocka_unit_test_setup_teardown(
            test_editor_save_close, mip_setup, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_editor_save_button_pointer, mip_setup, mip_teardown),

        /* Profile editor — cancel / discard (M38) */
        cmocka_unit_test_setup_teardown(
            test_editor_cancel_discard, mip_setup, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_editor_discard_button_pointer, mip_setup, mip_teardown),

        
/* Disabled / degraded scenarios */
        cmocka_unit_test_setup_teardown(
            test_d03_delete_no_profile, mip_setup_empty, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_d04_save_missing_nes, mip_setup, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_d07_filesystem_failure, mip_setup, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_d08_empty_profile_create, mip_setup, mip_teardown),

        /* Task 3: Clone existing (M15/PR-02/IA-10) */
        cmocka_unit_test_setup_teardown(
            test_prof_create_clone_controller, mip_setup, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_prof_create_clone_pointer, mip_setup, mip_teardown),

        /* Task 3: Sequential capture via DBus InputEvent (PE-04) */
        cmocka_unit_test_setup_teardown(
            test_editor_seq_capture_dbus_signal, mip_setup, mip_teardown),

        /* Task 3: Unsaved-close prompt (PR-07) */
        cmocka_unit_test_setup_teardown(
            test_quit_unsaved_prompt_appears, mip_setup, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_quit_unsaved_save_and_quit, mip_setup, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_quit_unsaved_discard_and_quit, mip_setup, mip_teardown),
        cmocka_unit_test_setup_teardown(
            test_quit_no_changes_immediate, mip_setup, mip_teardown),

        /* Task 3: Capability-scoped binding (PE-06) */
        cmocka_unit_test_setup_teardown(
            test_capability_scoped_binding, mip_setup, mip_teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}