/*
 * test_manager_tabs.c — Tests for the manager skeleton and tab bar.
 *
 * Uses the SDL2 dummy driver for headless testing.  Tests exercise:
 *   - Init creates a window, 3 tabs, active tab = 0
 *   - Left/Right switches tabs via key events
 *   - Tab change callback fires and updates visible panel
 *   - Up/Down navigates the focus chain
 *   - Render does not crash
 *   - Shutdown cleans up
 *   - NULL safety
 *
 * Task 34 — Manager skeleton and tab bar.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <setjmp.h>
#include <cmocka.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "manager/manager.h"
#include "ui/widget.h"
#include "ui/focus.h"

#ifndef CBX_FONT_PATH
#define CBX_FONT_PATH ""
#endif

/* --- helpers ------------------------------------------------------- */

static bool
font_available(void)
{
    return CBX_FONT_PATH[0] != '\0' && access(CBX_FONT_PATH, R_OK) == 0;
}

static void
ensure_dummy_driver(void)
{
    if (getenv("SDL_VIDEODRIVER") == NULL)
        SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "dummy",
                                 SDL_HINT_OVERRIDE);
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

/* --- Tests --------------------------------------------------------- */

static void
test_manager_init_basic(void **state)
{
    (void)state;
    ensure_dummy_driver();
    cbx_manager mgr;
    int rc = cbx_manager_init(&mgr, NULL);
    assert_int_equal(rc, 0);

    /* 3 tabs. */
    assert_int_equal(cbx_manager_tab_count(&mgr), CBX_MGR_TAB_COUNT);

    /* Active tab is Controllers (0). */
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_CONTROLLERS);

    /* Tab bar has 3 tabs. */
    const cbx_tabbar *tb = cbx_manager_tabbar(&mgr);
    assert_non_null(tb);
    assert_int_equal(cbx_tabbar_tab_count(tb), 3);
    assert_int_equal(cbx_tabbar_get_active(tb), 0);

    /* All three panels are populated (tab modules initialised by
     * cbx_manager_init). */
    for (int i = 0; i < CBX_MGR_TAB_COUNT; i++) {
        const cbx_panel *p = cbx_manager_panel(&mgr, i);
        assert_non_null(p);
        assert_true(p->child_count > 0);
    }

    /* Tab module accessors return non-NULL. */
    assert_non_null(cbx_manager_controllers_tab(&mgr));
    assert_non_null(cbx_manager_profiles_tab(&mgr));
    assert_non_null(cbx_manager_settings_tab(&mgr));

    cbx_manager_shutdown(&mgr);
}

static void
test_manager_panels_visibility(void **state)
{
    (void)state;
    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    /* Only the active panel (Controllers) is visible. */
    for (int i = 0; i < CBX_MGR_TAB_COUNT; i++) {
        const cbx_panel *p = cbx_manager_panel(&mgr, i);
        assert_non_null(p);
        if (i == CBX_MGR_TAB_CONTROLLERS)
            assert_true(cbx_widget_is_visible(&p->base));
        else
            assert_false(cbx_widget_is_visible(&p->base));
    }

    /* Active panel has children (populated by tab module). */
    const cbx_panel *active = cbx_manager_panel(&mgr, mgr.active_tab);
    assert_non_null(active);
    assert_true(active->child_count > 0);

    cbx_manager_shutdown(&mgr);
}

static void
test_manager_left_right_switches_tabs(void **state)
{
    (void)state;
    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    /* Right: Controllers → Profiles. */
    assert_true(send_key(&mgr, SDLK_RIGHT));
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_PROFILES);

    /* Right: Profiles → Settings. */
    assert_true(send_key(&mgr, SDLK_RIGHT));
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_SETTINGS);

    /* Right at end — consumed but no move. */
    assert_true(send_key(&mgr, SDLK_RIGHT));
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_SETTINGS);

    /* Left: Settings → Profiles. */
    assert_true(send_key(&mgr, SDLK_LEFT));
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_PROFILES);

    /* Left: Profiles → Controllers. */
    assert_true(send_key(&mgr, SDLK_LEFT));
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_CONTROLLERS);

    /* Left at start — consumed but no move. */
    assert_true(send_key(&mgr, SDLK_LEFT));
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_CONTROLLERS);

    cbx_manager_shutdown(&mgr);
}

static void
test_manager_tab_change_updates_visible_panel(void **state)
{
    (void)state;
    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    /* Switch to Profiles (tab 1). */
    send_key(&mgr, SDLK_RIGHT);
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_PROFILES);

    /* Profiles panel visible, others hidden. */
    for (int i = 0; i < CBX_MGR_TAB_COUNT; i++) {
        const cbx_panel *p = cbx_manager_panel(&mgr, i);
        if (i == CBX_MGR_TAB_PROFILES)
            assert_true(cbx_widget_is_visible(&p->base));
        else
            assert_false(cbx_widget_is_visible(&p->base));
    }

    /* Switch to Settings (tab 2). */
    send_key(&mgr, SDLK_RIGHT);
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_SETTINGS);

    for (int i = 0; i < CBX_MGR_TAB_COUNT; i++) {
        const cbx_panel *p = cbx_manager_panel(&mgr, i);
        if (i == CBX_MGR_TAB_SETTINGS)
            assert_true(cbx_widget_is_visible(&p->base));
        else
            assert_false(cbx_widget_is_visible(&p->base));
    }

    /* Switch back to Controllers (tab 0). */
    send_key(&mgr, SDLK_LEFT);
    send_key(&mgr, SDLK_LEFT);
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_CONTROLLERS);

    for (int i = 0; i < CBX_MGR_TAB_COUNT; i++) {
        const cbx_panel *p = cbx_manager_panel(&mgr, i);
        if (i == CBX_MGR_TAB_CONTROLLERS)
            assert_true(cbx_widget_is_visible(&p->base));
        else
            assert_false(cbx_widget_is_visible(&p->base));
    }

    cbx_manager_shutdown(&mgr);
}

static void
test_manager_up_down_focus_navigation(void **state)
{
    (void)state;
    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    /* The focus chain should have the tab bar as the first entry, plus
     * at least one panel child (panels are now populated). */
    const cbx_focus_chain *fc = cbx_manager_focus(&mgr);
    assert_non_null(fc);
    assert_true(fc->count > 1);  /* tabbar + at least one panel child */
    assert_int_equal(fc->focused, 0);  /* tabbar is focused */

    /* UP from tabbar — no candidate above, focus unchanged. */
    bool consumed = send_key(&mgr, SDLK_UP);
    assert_false(consumed);  /* no candidate = not consumed */
    assert_int_equal(fc->focused, 0);

    /* DOWN from tabbar — navigates to a panel child. */
    consumed = send_key(&mgr, SDLK_DOWN);
    assert_true(consumed);  /* navigated to a panel child */
    assert_true(fc->focused > 0);  /* moved past tabbar */

    cbx_manager_shutdown(&mgr);
}

/* Full focus-chain traversal: tabbar → list → buttons → tabbar.
 * Verifies that the list widget does NOT trap focus at boundaries
 * (Task 1). */
static void
test_manager_focus_traversal_no_trap(void **state)
{
    (void)state;
    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    const cbx_focus_chain *fc = cbx_manager_focus(&mgr);
    assert_non_null(fc);
    assert_true(fc->count > 1);

    /* Start at tabbar (index 0). */
    assert_int_equal(fc->focused, 0);

    /* DOWN: tabbar → first panel child (list). */
    assert_true(send_key(&mgr, SDLK_DOWN));
    assert_true(fc->focused > 0);
    int list_idx = fc->focused;

    /* UP at list top boundary: returns false from list → manager
     * navigates focus back to tabbar. */
    assert_true(send_key(&mgr, SDLK_UP));
    assert_int_equal(fc->focused, 0);  /* back to tabbar */

    /* DOWN again: back to list. */
    assert_true(send_key(&mgr, SDLK_DOWN));
    assert_int_equal(fc->focused, list_idx);

    /* DOWN from list: list returns false at boundary → manager
     * navigates to the next widget (button). */
    assert_true(send_key(&mgr, SDLK_DOWN));
    assert_true(fc->focused > list_idx);  /* moved past list */

    /* UP from button: button returns false for UP → manager
     * navigates back to list. */
    assert_true(send_key(&mgr, SDLK_UP));
    assert_int_equal(fc->focused, list_idx);

    /* Switch to Settings tab (which also has a list + button). */
    send_key(&mgr, SDLK_RIGHT);  /* → Profiles */
    send_key(&mgr, SDLK_RIGHT);  /* → Settings */
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_SETTINGS);
    assert_int_equal(fc->focused, 0);  /* tabbar re-focused on tab change */

    /* DOWN: tabbar → settings list. */
    assert_true(send_key(&mgr, SDLK_DOWN));
    assert_true(fc->focused > 0);

    /* UP at settings list top: returns to tabbar. */
    assert_true(send_key(&mgr, SDLK_UP));
    assert_int_equal(fc->focused, 0);

    cbx_manager_shutdown(&mgr);
}

static void
test_manager_render_does_not_crash(void **state)
{
    (void)state;
    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    /* Render in each tab. */
    cbx_manager_render(&mgr);

    send_key(&mgr, SDLK_RIGHT);
    cbx_manager_render(&mgr);

    send_key(&mgr, SDLK_RIGHT);
    cbx_manager_render(&mgr);

    cbx_manager_shutdown(&mgr);
}

static void
test_manager_render_with_font(void **state)
{
    (void)state;
    if (!font_available())
        skip();
    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, CBX_FONT_PATH), 0);

    cbx_manager_render(&mgr);

    send_key(&mgr, SDLK_RIGHT);
    cbx_manager_render(&mgr);

    cbx_manager_shutdown(&mgr);
}

static void
test_manager_stop(void **state)
{
    (void)state;
    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    /* After init, running is false. */
    assert_false(mgr.running);

    /* Simulate the loop starting, then stop. */
    mgr.running = true;
    cbx_manager_stop(&mgr);
    assert_false(mgr.running);

    cbx_manager_shutdown(&mgr);
}

static void
test_manager_null_safety(void **state)
{
    (void)state;
    /* Init with NULL. */
    assert_int_equal(cbx_manager_init(NULL, NULL), -EINVAL);

    /* Run with NULL. */
    assert_int_equal(cbx_manager_run(NULL), -EINVAL);

    /* Handle event with NULL. */
    SDL_Event ev = {0};
    assert_false(cbx_manager_handle_event(NULL, &ev));

    /* Render with NULL — must not crash. */
    cbx_manager_render(NULL);

    /* Stop with NULL — must not crash. */
    cbx_manager_stop(NULL);

    /* Shutdown with NULL — must not crash. */
    cbx_manager_shutdown(NULL);

    /* Accessors with NULL. */
    assert_int_equal(cbx_manager_active_tab(NULL), -1);
    assert_int_equal(cbx_manager_tab_count(NULL), 0);
    assert_null(cbx_manager_tabbar(NULL));
    assert_null(cbx_manager_panel(NULL, 0));
    assert_null(cbx_manager_focus(NULL));
}

static void
test_manager_unrelated_event(void **state)
{
    (void)state;
    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    /* Mouse motion — not consumed. */
    SDL_Event ev = {0};
    ev.type = SDL_MOUSEMOTION;
    assert_false(cbx_manager_handle_event(&mgr, &ev));

    /* Unrelated key — not consumed. */
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = SDLK_x;
    assert_false(cbx_manager_handle_event(&mgr, &ev));

    cbx_manager_shutdown(&mgr);
}

static void
test_manager_panel_accessors(void **state)
{
    (void)state;
    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    /* Valid panel. */
    const cbx_panel *p = cbx_manager_panel(&mgr, CBX_MGR_TAB_CONTROLLERS);
    assert_non_null(p);

    /* Out of range. */
    assert_null(cbx_manager_panel(&mgr, -1));
    assert_null(cbx_manager_panel(&mgr, CBX_MGR_TAB_COUNT));
    assert_null(cbx_manager_panel(&mgr, 99));

    cbx_manager_shutdown(&mgr);
}

static void
test_manager_shutdown_cleans_up(void **state)
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
    cbx_manager_shutdown(&mgr);
}

static void
test_manager_a_key_consumed(void **state)
{
    (void)state;
    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    /* A key (controller 'A' button) is consumed for activation. */
    assert_true(send_key(&mgr, SDLK_a));

    /* Enter is also consumed. */
    assert_true(send_key(&mgr, SDLK_RETURN));

    /* Space is also consumed. */
    assert_true(send_key(&mgr, SDLK_SPACE));

    cbx_manager_shutdown(&mgr);
}

static void
test_manager_full_tab_cycle(void **state)
{
    (void)state;
    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    /* Full cycle: Controllers → Profiles → Settings → (boundary). */
    send_key(&mgr, SDLK_RIGHT);
    assert_int_equal(cbx_manager_active_tab(&mgr), 1);
    send_key(&mgr, SDLK_RIGHT);
    assert_int_equal(cbx_manager_active_tab(&mgr), 2);

    /* Render at each tab. */
    cbx_manager_render(&mgr);

    /* Back: Settings → Profiles → Controllers. */
    send_key(&mgr, SDLK_LEFT);
    assert_int_equal(cbx_manager_active_tab(&mgr), 1);
    send_key(&mgr, SDLK_LEFT);
    assert_int_equal(cbx_manager_active_tab(&mgr), 0);
    cbx_manager_render(&mgr);

    cbx_manager_shutdown(&mgr);
}

/* --- main ---------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_manager_init_basic),
        cmocka_unit_test(test_manager_panels_visibility),
        cmocka_unit_test(test_manager_left_right_switches_tabs),
        cmocka_unit_test(test_manager_tab_change_updates_visible_panel),
        cmocka_unit_test(test_manager_up_down_focus_navigation),
        cmocka_unit_test(test_manager_focus_traversal_no_trap),
        cmocka_unit_test(test_manager_render_does_not_crash),
        cmocka_unit_test(test_manager_render_with_font),
        cmocka_unit_test(test_manager_stop),
        cmocka_unit_test(test_manager_null_safety),
        cmocka_unit_test(test_manager_unrelated_event),
        cmocka_unit_test(test_manager_panel_accessors),
        cmocka_unit_test(test_manager_shutdown_cleans_up),
        cmocka_unit_test(test_manager_a_key_consumed),
        cmocka_unit_test(test_manager_full_tab_cycle),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}