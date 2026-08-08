/*
 * test_manager_production.c — Production-path regression test (BUG-0003).
 *
 * Exercises the same composition path as the executable: calls only
 * cbx_manager_init() and cbx_manager_shutdown() — no manual tab
 * initialization. Verifies that all three tab panels are populated,
 * rendered body content is visible, focus chain is populated, tab
 * switching works, and shutdown is clean.
 *
 * Uses SDL2 dummy driver and an isolated $HOME with a profiles directory.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include "manager/manager.h"
#include "manager/profiles_tab.h"
#include "manager/profile_editor_list.h"
#include "ui/widget.h"
#include "ui/focus.h"
#include "config/config_paths.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef CBX_FONT_PATH
#define CBX_FONT_PATH ""
#endif

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static void ensure_dummy_driver(void)
{
    if (getenv("SDL_VIDEODRIVER") == NULL)
        SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "dummy",
                                 SDL_HINT_OVERRIDE);
}

static bool font_available(void)
{
    return CBX_FONT_PATH[0] != '\0' && access(CBX_FONT_PATH, R_OK) == 0;
}

/* Helper: inject a keydown event into the manager. */
static bool
send_key(cbx_manager *mgr, SDL_Keycode sym)
{
    SDL_Event ev = {0};
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = sym;
    return cbx_manager_handle_event(mgr, &ev);
}

static bool
send_controller_button(cbx_manager *mgr, Uint8 button, Uint32 type)
{
    SDL_Event ev = {0};
    ev.type = type;
    ev.cbutton.type = type;
    ev.cbutton.button = button;
    ev.cbutton.state = type == SDL_CONTROLLERBUTTONDOWN
                         ? SDL_PRESSED : SDL_RELEASED;
    return cbx_manager_handle_event(mgr, &ev);
}

/* ------------------------------------------------------------------ */
/*  Fixture — isolated HOME with profiles directory                    */
/* ------------------------------------------------------------------ */

typedef struct {
    char tmp[256];
    char saved_home[256];
    bool saved_home_set;
} mp_fixture;

static int setup(void **state)
{
    ensure_dummy_driver();

    mp_fixture *f = malloc(sizeof(*f));
    assert_non_null(f);
    memset(f, 0, sizeof(*f));

    /* Save original HOME. */
    const char *home = getenv("HOME");
    if (home) {
        snprintf(f->saved_home, sizeof(f->saved_home), "%s", home);
        f->saved_home_set = true;
    }

    /* Isolated HOME. */
    snprintf(f->tmp, sizeof(f->tmp), "/tmp/cbx_mp_test_%d", (int)getpid());
    char cmd[PATH_MAX * 2 + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", f->tmp);
    int r0 = system(cmd);
    (void)r0;
    mkdir(f->tmp, 0700);
    setenv("HOME", f->tmp, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");
    unsetenv("FLATPAK_ID");

    /* Create a profiles directory so profiles tab refresh succeeds. */
    char profiles_dir[PATH_MAX + 64];
    snprintf(profiles_dir, sizeof(profiles_dir),
             "%s/.local/share/inputplumber/profiles", f->tmp);
    cbx_ensure_dir(profiles_dir, 0700);

    *state = f;
    return 0;
}

static int teardown(void **state)
{
    mp_fixture *f = *state;

    /* Restore HOME. */
    if (f->saved_home_set)
        setenv("HOME", f->saved_home, 1);
    else
        unsetenv("HOME");

    char cmd[PATH_MAX * 2 + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", f->tmp);
    int r = system(cmd);
    (void)r;
    free(f);
    return 0;
}

#define FIX(s) ((mp_fixture *)*(s))

/* ------------------------------------------------------------------ */
/*  Tests                                                              */
/* ------------------------------------------------------------------ */

/* (a) Nonempty panels: after cbx_manager_init(), all three panels have
 *     child_count > 0. */
static void test_nonempty_panels(void **state)
{
    (void)state;
    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    for (int i = 0; i < CBX_MGR_TAB_COUNT; i++) {
        const cbx_panel *p = cbx_manager_panel(&mgr, i);
        assert_non_null(p);
        assert_true(p->child_count > 0);
    }

    /* Accessors return non-NULL. */
    assert_non_null(cbx_manager_controllers_tab(&mgr));
    assert_non_null(cbx_manager_profiles_tab(&mgr));
    assert_non_null(cbx_manager_settings_tab(&mgr));

    cbx_manager_shutdown(&mgr);
}

/* (b) Visible rendered body content: for each tab, set active_tab,
 *     render, and assert at least one child widget has non-zero width
 *     and height and is visible. */
static void test_visible_rendered_content(void **state)
{
    (void)state;
    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    for (int tab = 0; tab < CBX_MGR_TAB_COUNT; tab++) {
        /* Switch to this tab. */
        while (cbx_manager_active_tab(&mgr) != tab)
            send_key(&mgr, SDLK_RIGHT);

        cbx_manager_render(&mgr);

        const cbx_panel *p = cbx_manager_panel(&mgr, tab);
        assert_non_null(p);
        assert_true(p->child_count > 0);

        /* At least one child is visible with non-zero dimensions. */
        bool found_visible = false;
        for (int i = 0; i < p->child_count; i++) {
            cbx_widget *child = p->children[i];
            if (!child)
                continue;
            if (cbx_widget_is_visible(child) &&
                child->rect.w > 0 && child->rect.h > 0) {
                found_visible = true;
                break;
            }
        }
        assert_true(found_visible);
    }

    cbx_manager_shutdown(&mgr);
}

/* (c) Focus chain populated: assert focus chain count > 1 (tabbar + at
 *     least one panel child). */
static void test_focus_chain_populated(void **state)
{
    (void)state;
    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    const cbx_focus_chain *fc = cbx_manager_focus(&mgr);
    assert_non_null(fc);
    assert_true(fc->count > 1);  /* tabbar + at least one panel child */

    cbx_manager_shutdown(&mgr);
}

/* (d) Tab switching: send SDLK_RIGHT/LEFT and verify active_tab changes
 *     and the newly active panel has children. */
static void test_tab_switching(void **state)
{
    (void)state;
    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    /* Right: Controllers -> Profiles. */
    assert_true(send_key(&mgr, SDLK_RIGHT));
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_PROFILES);
    assert_true(cbx_manager_panel(&mgr, CBX_MGR_TAB_PROFILES)->child_count > 0);

    /* Right: Profiles -> Settings. */
    assert_true(send_key(&mgr, SDLK_RIGHT));
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_SETTINGS);
    assert_true(cbx_manager_panel(&mgr, CBX_MGR_TAB_SETTINGS)->child_count > 0);

    /* Left: Settings -> Profiles. */
    assert_true(send_key(&mgr, SDLK_LEFT));
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_PROFILES);
    assert_true(cbx_manager_panel(&mgr, CBX_MGR_TAB_PROFILES)->child_count > 0);

    /* Left: Profiles -> Controllers. */
    assert_true(send_key(&mgr, SDLK_LEFT));
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_CONTROLLERS);
    assert_true(cbx_manager_panel(&mgr, CBX_MGR_TAB_CONTROLLERS)->child_count > 0);

    cbx_manager_shutdown(&mgr);
}

/* Real controller events, not keyboard proxies, traverse Manager dispatch. */
static void test_controller_event_tab_switching(void **state)
{
    (void)state;
    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    assert_true(send_controller_button(&mgr, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
                                       SDL_CONTROLLERBUTTONDOWN));
    send_controller_button(&mgr, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
                           SDL_CONTROLLERBUTTONUP);
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_PROFILES);

    assert_true(send_controller_button(&mgr, SDL_CONTROLLER_BUTTON_DPAD_LEFT,
                                       SDL_CONTROLLERBUTTONDOWN));
    send_controller_button(&mgr, SDL_CONTROLLER_BUTTON_DPAD_LEFT,
                           SDL_CONTROLLERBUTTONUP);
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_CONTROLLERS);

    cbx_manager_shutdown(&mgr);
}

#if SDL_VERSION_ATLEAST(2, 0, 14)
static void test_sdl_virtual_controller_transport(void **state)
{
    (void)state;
    ensure_dummy_driver();
    assert_int_equal(SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER), 0);
    int device_index = SDL_JoystickAttachVirtual(
        SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 15, 0);
    assert_true(device_index >= 0);
    SDL_Joystick *joystick = SDL_JoystickOpen(device_index);
    assert_non_null(joystick);

    char guid[33];
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick), guid,
                              sizeof(guid));
    char mapping[512];
    snprintf(mapping, sizeof(mapping),
             "%s,Controller-Box Virtual,a:b0,b:b1,start:b6,"
             "dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,platform:Linux,",
             guid);
    assert_true(SDL_GameControllerAddMapping(mapping) >= 0);

    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    assert_int_equal(SDL_JoystickSetVirtualButton(joystick, 14, 1), 0);
    SDL_PumpEvents();

    bool dispatched = false;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_CONTROLLERBUTTONDOWN) {
            dispatched = cbx_manager_handle_event(&mgr, &event) || dispatched;
        }
    }
    assert_true(dispatched);
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_PROFILES);

    SDL_JoystickSetVirtualButton(joystick, 14, 0);
    SDL_PumpEvents();
    while (SDL_PollEvent(&event))
        cbx_manager_handle_event(&mgr, &event);
    cbx_manager_shutdown(&mgr);
    SDL_JoystickClose(joystick);
    assert_int_equal(SDL_JoystickDetachVirtual(device_index), 0);
}
#endif

/* (e) Shutdown clean: cbx_manager_shutdown() does not crash; struct is
 *     zeroed; can re-init. */
static void test_shutdown_clean(void **state)
{
    (void)state;
    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    cbx_manager_shutdown(&mgr);

    /* After shutdown, struct is zeroed — tab_count returns 0. */
    assert_int_equal(cbx_manager_tab_count(&mgr), 0);

    /* Can re-init after shutdown. */
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);
    assert_int_equal(cbx_manager_tab_count(&mgr), 3);

    /* Panels are populated after re-init. */
    for (int i = 0; i < CBX_MGR_TAB_COUNT; i++) {
        const cbx_panel *p = cbx_manager_panel(&mgr, i);
        assert_non_null(p);
        assert_true(p->child_count > 0);
    }

    cbx_manager_shutdown(&mgr);
}

/* (f) Render with font: if a font is available, verify rendering with
 *     font-loaded manager produces visible content. */
static void test_render_with_font(void **state)
{
    (void)state;
    if (!font_available())
        skip();
    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, CBX_FONT_PATH), 0);

    for (int tab = 0; tab < CBX_MGR_TAB_COUNT; tab++) {
        while (cbx_manager_active_tab(&mgr) != tab)
            send_key(&mgr, SDLK_RIGHT);
        cbx_manager_render(&mgr);

        const cbx_panel *p = cbx_manager_panel(&mgr, tab);
        assert_true(p->child_count > 0);
    }

    cbx_manager_shutdown(&mgr);
}

/* (g) Profile editor opens via production dispatch: write a NES profile,
 *     switch to Profiles tab, click the Edit button, verify editor is open. */
static void test_editor_opens_via_dispatch(void **state)
{
    mp_fixture *f = FIX(state);
    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    /* Write a user profile with NES bindings to the profiles directory. */
    char prof_path[PATH_MAX + 64];
    snprintf(prof_path, sizeof(prof_path),
             "%s/.local/share/inputplumber/profiles/testprof.yaml", f->tmp);
    FILE *fp = fopen(prof_path, "w");
    assert_non_null(fp);
    fprintf(fp, "version: 1\nkind: DeviceProfile\nname: TestProf\n");
    fprintf(fp, "description: NES test profile\nmapping:\n");
    const char *btns[] = {"A", "B", "Up", "Down", "Left", "Right"};
    const char *keys[] = {"KeyA", "KeyB", "KeyUp", "KeyDown",
                         "KeyLeft", "KeyRight"};
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

    /* Switch to Profiles tab (triggers refresh). */
    while (cbx_manager_active_tab(&mgr) != CBX_MGR_TAB_PROFILES)
        send_key(&mgr, SDLK_RIGHT);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);
    assert_non_null(pt);
    assert_true(cbx_profiles_tab_profile_count(pt) > 0);

    /* Click on the Edit button to open the editor. */
    SDL_Rect btn_rect;
    cbx_widget_get_rect(&pt->edit_btn.base, &btn_rect);
    int cx = btn_rect.x + btn_rect.w / 2;
    int cy = btn_rect.y + btn_rect.h / 2;
    SDL_Event mev = {0};
    mev.type = SDL_MOUSEBUTTONDOWN;
    mev.button.button = SDL_BUTTON_LEFT;
    mev.button.x = cx; mev.button.y = cy;
    cbx_manager_handle_event(&mgr, &mev);
    mev.type = SDL_MOUSEBUTTONUP;
    cbx_manager_handle_event(&mgr, &mev);

    /* Verify the editor is open via production dispatch. */
    assert_int_equal(pt->mode, CBX_PT_MODE_EDITOR);
    assert_true(pt->editor_initialized);
    assert_true(cbx_widget_is_visible(&pt->editor.binding_list.base));
    assert_true(cbx_widget_is_visible(&pt->editor.diagram.base));

    /* Verify the profile was loaded into the editor. */
    assert_int_equal(cbx_profile_editor_binding_count(&pt->editor), 6);

    cbx_manager_shutdown(&mgr);
}

/* ------------------------------------------------------------------ */
/*  Test runner                                                        */
/* ------------------------------------------------------------------ */

static const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(test_nonempty_panels, setup, teardown),
    cmocka_unit_test_setup_teardown(test_visible_rendered_content, setup, teardown),
    cmocka_unit_test_setup_teardown(test_focus_chain_populated, setup, teardown),
    cmocka_unit_test_setup_teardown(test_tab_switching, setup, teardown),
    cmocka_unit_test_setup_teardown(test_controller_event_tab_switching, setup, teardown),
#if SDL_VERSION_ATLEAST(2, 0, 14)
    cmocka_unit_test_setup_teardown(test_sdl_virtual_controller_transport, setup, teardown),
#endif
    cmocka_unit_test_setup_teardown(test_shutdown_clean, setup, teardown),
    cmocka_unit_test_setup_teardown(test_render_with_font, setup, teardown),
    cmocka_unit_test_setup_teardown(test_editor_opens_via_dispatch, setup, teardown),
};

int main(void)
{
    return cmocka_run_group_tests(tests, NULL, NULL);
}