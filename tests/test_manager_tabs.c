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

/* Helper: send a mouse motion event at (x, y). */
static bool
send_mouse_motion(cbx_manager *mgr, int x, int y)
{
    SDL_Event ev = {0};
    ev.type = SDL_MOUSEMOTION;
    ev.motion.x = x;
    ev.motion.y = y;
    return cbx_manager_handle_event(mgr, &ev);
}

/* Helper: send a full mouse click (down + up) at (x, y).
 * Returns the result of the MOUSEBUTTONDOWN dispatch. */
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

    /* Mouse motion in empty space — not consumed, no side effect. */
    SDL_Event ev = {0};
    ev.type = SDL_MOUSEMOTION;
    ev.motion.x = 640;
    ev.motion.y = 600;
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

/* --- Pointer (mouse) event tests (Task 2) ---------------------- */

static int mouse_press_count = 0;
static void
on_mouse_press_test(cbx_widget *w, void *user_data)
{
    (void)w;
    (void)user_data;
    mouse_press_count++;
}

/* A mouse click on an unfocused button activates it (fires on_press)
 * even when a different widget is focused. */
static void
test_mouse_click_unfocused_button(void **state)
{
    (void)state;
    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    /* Focus is on the tabbar initially — the button is NOT focused. */
    const cbx_focus_chain *fc = cbx_manager_focus(&mgr);
    assert_int_equal(fc->focused, 0);  /* tabbar */

    /* Get the controllers tab add button. */
    cbx_controllers_tab *ct = cbx_manager_controllers_tab(&mgr);
    assert_non_null(ct);

    /* Install a test callback so we can verify the click fires it. */
    mouse_press_count = 0;
    cbx_button_set_press_cb(&ct->add_btn, on_mouse_press_test, NULL);

    /* Compute the center of the add button. */
    SDL_Rect btn_rect;
    cbx_widget_get_rect(&ct->add_btn.base, &btn_rect);
    int cx = btn_rect.x + btn_rect.w / 2;
    int cy = btn_rect.y + btn_rect.h / 2;

    /* Mouse down + up on the unfocused button. */
    assert_true(send_mouse_click(&mgr, cx, cy));

    /* The callback fired exactly once. */
    assert_int_equal(mouse_press_count, 1);

    /* The button is now focused (focus follows pointer). */
    assert_true(ct->add_btn.base.focused);

    cbx_manager_shutdown(&mgr);
}

/* A mouse click on a tabbar tab switches tabs. */
static void
test_mouse_click_tab_switches(void **state)
{
    (void)state;
    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    /* Active tab is Controllers (0). */
    assert_int_equal(cbx_manager_active_tab(&mgr), 0);

    /* Compute the centre of the Profiles tab (second tab). */
    const cbx_tabbar *tb = cbx_manager_tabbar(&mgr);
    assert_non_null(tb);
    SDL_Rect tb_rect = tb->base.rect;
    int tab_w = tb_rect.w / tb->tab_count;
    int profiles_x = tb_rect.x + tab_w + tab_w / 2;
    int tab_y = tb_rect.y + tb_rect.h / 2;

    /* Click on the Profiles tab. */
    assert_true(send_mouse_click(&mgr, profiles_x, tab_y));

    /* Tab switched to Profiles (1). */
    assert_int_equal(cbx_manager_active_tab(&mgr), 1);

    /* Click on the Settings tab (third). */
    int settings_x = tb_rect.x + tab_w * 2 + tab_w / 2;
    assert_true(send_mouse_click(&mgr, settings_x, tab_y));
    assert_int_equal(cbx_manager_active_tab(&mgr), 2);

    /* Click on the Controllers tab (first) to go back. */
    int controllers_x = tb_rect.x + tab_w / 2;
    assert_true(send_mouse_click(&mgr, controllers_x, tab_y));
    assert_int_equal(cbx_manager_active_tab(&mgr), 0);

    cbx_manager_shutdown(&mgr);
}

/* A mouse click on a list item selects it. */
static void
test_mouse_click_list_item(void **state)
{
    (void)state;
    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    /* Use the settings tab list (already populated by init). */
    send_key(&mgr, SDLK_RIGHT);  /* Controllers → Profiles */
    send_key(&mgr, SDLK_RIGHT);  /* Profiles → Settings */
    assert_int_equal(cbx_manager_active_tab(&mgr), 2);

    cbx_settings_tab *st = cbx_manager_settings_tab(&mgr);
    assert_non_null(st);
    assert_true(cbx_list_item_count(&st->settings_list) > 0);

    /* Get the settings list rect and item height. */
    SDL_Rect list_rect;
    cbx_widget_get_rect(&st->settings_list.base, &list_rect);
    int item_h = st->settings_list.item_h;
    assert_true(item_h > 0);

    /* Click on the third item. */
    int click_x = list_rect.x + 10;
    int click_y = list_rect.y + item_h * 2 + item_h / 2;
    assert_true(send_mouse_click(&mgr, click_x, click_y));

    /* The third item is selected. */
    assert_int_equal(cbx_list_get_selected(&st->settings_list), 2);

    /* Click on the first item. */
    click_y = list_rect.y + item_h / 2;
    assert_true(send_mouse_click(&mgr, click_x, click_y));
    assert_int_equal(cbx_list_get_selected(&st->settings_list), 0);

    cbx_manager_shutdown(&mgr);
}

/* An invisible widget does not consume clicks (no side effect). */
static void
test_invisible_widget_no_click(void **state)
{
    (void)state;
    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    cbx_controllers_tab *ct = cbx_manager_controllers_tab(&mgr);
    assert_non_null(ct);

    /* Install a test callback on the add button. */
    mouse_press_count = 0;
    cbx_button_set_press_cb(&ct->add_btn, on_mouse_press_test, NULL);

    /* Make the add button invisible. */
    cbx_widget_set_visible(&ct->add_btn.base, false);

    /* Compute the centre of where the (now invisible) button is. */
    SDL_Rect btn_rect;
    cbx_widget_get_rect(&ct->add_btn.base, &btn_rect);
    int cx = btn_rect.x + btn_rect.w / 2;
    int cy = btn_rect.y + btn_rect.h / 2;

    /* Click on the invisible button. */
    send_mouse_click(&mgr, cx, cy);

    /* The callback did NOT fire. */
    assert_int_equal(mouse_press_count, 0);

    /* The button is NOT focused. */
    assert_false(ct->add_btn.base.focused);

    cbx_manager_shutdown(&mgr);
}

/* SDL_MOUSEMOTION updates hover state on the widget under the cursor. */
static void
test_mouse_motion_updates_hover(void **state)
{
    (void)state;
    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    /* Hover is initially false on all widgets. */
    assert_false(mgr.tabbar.base.hover);

    /* Move the mouse over the tabbar centre. */
    const cbx_tabbar *tb = cbx_manager_tabbar(&mgr);
    SDL_Rect tb_rect = tb->base.rect;
    int tb_cx = tb_rect.x + tb_rect.w / 2;
    int tb_cy = tb_rect.y + tb_rect.h / 2;
    send_mouse_motion(&mgr, tb_cx, tb_cy);

    /* The tabbar is now hovered. */
    assert_true(mgr.tabbar.base.hover);

    /* Move the mouse to empty space (below all widgets). */
    send_mouse_motion(&mgr, 640, 600);

    /* The tabbar is no longer hovered. */
    assert_false(mgr.tabbar.base.hover);

    /* Move the mouse over the add button. */
    cbx_controllers_tab *ct = cbx_manager_controllers_tab(&mgr);
    SDL_Rect btn_rect;
    cbx_widget_get_rect(&ct->add_btn.base, &btn_rect);
    int btn_cx = btn_rect.x + btn_rect.w / 2;
    int btn_cy = btn_rect.y + btn_rect.h / 2;
    send_mouse_motion(&mgr, btn_cx, btn_cy);

    /* The add button is now hovered. */
    assert_true(ct->add_btn.base.hover);
    /* The tabbar is not. */
    assert_false(mgr.tabbar.base.hover);

    cbx_manager_shutdown(&mgr);
}

/* A mouse click on empty space (no widget) produces no side effect. */
static void
test_mouse_click_empty_space(void **state)
{
    (void)state;
    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    /* Record current tab and focused widget. */
    int tab_before = cbx_manager_active_tab(&mgr);
    const cbx_focus_chain *fc = cbx_manager_focus(&mgr);
    int focus_before = fc->focused;

    /* Click on empty space (below all widgets in the controllers tab). */
    bool consumed = send_mouse_click(&mgr, 640, 600);

    /* The click was not consumed (no widget found). */
    assert_false(consumed);

    /* No side effects: tab unchanged, focus unchanged. */
    assert_int_equal(cbx_manager_active_tab(&mgr), tab_before);
    assert_int_equal(fc->focused, focus_before);

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
        cmocka_unit_test(test_mouse_click_unfocused_button),
        cmocka_unit_test(test_mouse_click_tab_switches),
        cmocka_unit_test(test_mouse_click_list_item),
        cmocka_unit_test(test_invisible_widget_no_click),
        cmocka_unit_test(test_mouse_motion_updates_hover),
        cmocka_unit_test(test_mouse_click_empty_space),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}