/*
 * test_widget_tabbar.c — Tests for the TabBar widget.
 *
 * Uses the SDL2 dummy driver for headless rendering.  Font-dependent
 * tests are skipped if no TTF font is available at compile time.
 *
 * Task 21 — List, Grid, TabBar, and ProgressBar widgets.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <setjmp.h>
#include <cmocka.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>

#include "ui/widget.h"
#include "ui/text.h"
#include "ui/theme.h"

#ifndef CBX_FONT_PATH
#define CBX_FONT_PATH ""
#endif

/* --- test context -------------------------------------------------- */

typedef struct {
    SDL_Window   *window;
    SDL_Renderer *renderer;
} TestCtx;

static void
ensure_dummy_driver(void)
{
    if (getenv("SDL_VIDEODRIVER") == NULL)
        SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "dummy",
                                 SDL_HINT_OVERRIDE);
}

static int
test_setup(TestCtx *ctx)
{
    ensure_dummy_driver();
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        return -1;
    ctx->window = SDL_CreateWindow("test", 0, 0, 320, 240,
                                   SDL_WINDOW_HIDDEN);
    if (!ctx->window) { SDL_Quit(); return -1; }
    ctx->renderer = SDL_CreateRenderer(ctx->window, -1,
                                       SDL_RENDERER_SOFTWARE);
    if (!ctx->renderer) {
        SDL_DestroyWindow(ctx->window);
        SDL_Quit();
        return -1;
    }
    return 0;
}

static void
test_teardown(TestCtx *ctx)
{
    if (ctx->renderer) SDL_DestroyRenderer(ctx->renderer);
    if (ctx->window)   SDL_DestroyWindow(ctx->window);
    SDL_Quit();
}

static bool
font_available(void)
{
    return CBX_FONT_PATH[0] != '\0' && access(CBX_FONT_PATH, R_OK) == 0;
}

/* --- TabBar tests -------------------------------------------------- */

static void
test_tabbar_init_basic(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_tabbar tb;
    assert_int_equal(cbx_tabbar_init(&tb, 0, &cache, &theme), 0);
    assert_true(cbx_widget_is_visible(&tb.base));
    assert_int_equal(cbx_tabbar_tab_count(&tb), 0);
    assert_int_equal(cbx_tabbar_get_active(&tb), -1);

    cbx_widget_destroy(&tb.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_tabbar_init_null(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    cbx_tabbar tb;

    assert_int_equal(cbx_tabbar_init(NULL, 0, &cache, &theme), -EINVAL);
    assert_int_equal(cbx_tabbar_init(&tb, 0, NULL, &theme), -EINVAL);
    assert_int_equal(cbx_tabbar_init(&tb, 0, &cache, NULL), -EINVAL);
}

static void
test_tabbar_add_tabs(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_tabbar tb;
    assert_int_equal(cbx_tabbar_init(&tb, 0, &cache, &theme), 0);

    int rc = cbx_tabbar_add_tab(&tb, "Controllers", NULL);
    assert_int_equal(rc, 0);
    rc = cbx_tabbar_add_tab(&tb, "Profiles", NULL);
    assert_int_equal(rc, 1);
    rc = cbx_tabbar_add_tab(&tb, "Settings", NULL);
    assert_int_equal(rc, 2);

    assert_int_equal(cbx_tabbar_tab_count(&tb), 3);
    assert_int_equal(cbx_tabbar_get_active(&tb), 0);  /* first auto-active */

    cbx_widget_destroy(&tb.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_tabbar_add_overflow(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_tabbar tb;
    assert_int_equal(cbx_tabbar_init(&tb, 0, &cache, &theme), 0);

    for (int i = 0; i < CBX_TABBAR_MAX_TABS; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Tab %d", i);
        assert_int_equal(cbx_tabbar_add_tab(&tb, buf, NULL), i);
    }
    assert_int_equal(cbx_tabbar_add_tab(&tb, "overflow", NULL), -ENOMEM);

    cbx_widget_destroy(&tb.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static int change_called = 0;
static int change_new_tab = -1;
static void
on_change_cb(cbx_widget *w, int new_tab, void *user_data)
{
    (void)w;
    (void)user_data;
    change_called++;
    change_new_tab = new_tab;
}

static void
test_tabbar_move_left_right(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_tabbar tb;
    assert_int_equal(cbx_tabbar_init(&tb, 0, &cache, &theme), 0);
    cbx_tabbar_add_tab(&tb, "A", NULL);
    cbx_tabbar_add_tab(&tb, "B", NULL);
    cbx_tabbar_add_tab(&tb, "C", NULL);
    cbx_tabbar_set_change_cb(&tb, on_change_cb);

    change_called = 0;
    change_new_tab = -1;

    /* Move right: A→B. */
    assert_int_equal(cbx_tabbar_move_right(&tb), 0);
    assert_int_equal(cbx_tabbar_get_active(&tb), 1);
    assert_int_equal(change_called, 1);
    assert_int_equal(change_new_tab, 1);

    /* Move right: B→C. */
    assert_int_equal(cbx_tabbar_move_right(&tb), 0);
    assert_int_equal(cbx_tabbar_get_active(&tb), 2);
    assert_int_equal(change_called, 2);

    /* Right at end — fail. */
    assert_int_equal(cbx_tabbar_move_right(&tb), -1);
    assert_int_equal(cbx_tabbar_get_active(&tb), 2);
    assert_int_equal(change_called, 2);  /* no new call */

    /* Move left: C→B. */
    assert_int_equal(cbx_tabbar_move_left(&tb), 0);
    assert_int_equal(cbx_tabbar_get_active(&tb), 1);
    assert_int_equal(change_called, 3);

    /* Left at start — fail. */
    cbx_tabbar_set_active(&tb, 0);
    change_called = 0;
    assert_int_equal(cbx_tabbar_move_left(&tb), -1);
    assert_int_equal(cbx_tabbar_get_active(&tb), 0);
    assert_int_equal(change_called, 0);

    cbx_widget_destroy(&tb.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_tabbar_set_active(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_tabbar tb;
    assert_int_equal(cbx_tabbar_init(&tb, 0, &cache, &theme), 0);
    cbx_tabbar_add_tab(&tb, "A", NULL);
    cbx_tabbar_add_tab(&tb, "B", NULL);
    cbx_tabbar_add_tab(&tb, "C", NULL);
    cbx_tabbar_set_change_cb(&tb, on_change_cb);

    change_called = 0;
    cbx_tabbar_set_active(&tb, 2);
    assert_int_equal(cbx_tabbar_get_active(&tb), 2);
    assert_int_equal(change_called, 1);

    /* Setting same tab — no callback. */
    change_called = 0;
    cbx_tabbar_set_active(&tb, 2);
    assert_int_equal(change_called, 0);

    /* Out of range — no change. */
    cbx_tabbar_set_active(&tb, 99);
    assert_int_equal(cbx_tabbar_get_active(&tb), 2);
    cbx_tabbar_set_active(&tb, -1);
    assert_int_equal(cbx_tabbar_get_active(&tb), 2);

    cbx_widget_destroy(&tb.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_tabbar_key_events(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_tabbar tb;
    assert_int_equal(cbx_tabbar_init(&tb, 0, &cache, &theme), 0);
    cbx_tabbar_add_tab(&tb, "A", NULL);
    cbx_tabbar_add_tab(&tb, "B", NULL);
    cbx_tabbar_add_tab(&tb, "C", NULL);

    SDL_Event ev = {0};
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = SDLK_RIGHT;
    assert_true(cbx_widget_handle_event(&tb.base, &ev));
    assert_int_equal(cbx_tabbar_get_active(&tb), 1);

    ev.key.keysym.sym = SDLK_LEFT;
    assert_true(cbx_widget_handle_event(&tb.base, &ev));
    assert_int_equal(cbx_tabbar_get_active(&tb), 0);

    /* Left at start — consumed but no movement. */
    assert_true(cbx_widget_handle_event(&tb.base, &ev));
    assert_int_equal(cbx_tabbar_get_active(&tb), 0);

    cbx_widget_destroy(&tb.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_tabbar_mouse_click(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_tabbar tb;
    assert_int_equal(cbx_tabbar_init(&tb, 0, &cache, &theme), 0);
    cbx_tabbar_add_tab(&tb, "A", NULL);
    cbx_tabbar_add_tab(&tb, "B", NULL);
    cbx_tabbar_add_tab(&tb, "C", NULL);

    SDL_Rect r = {0, 0, 300, 40};
    cbx_widget_set_rect(&tb.base, &r);
    /* tab_w = 100. Click at x=210 → tab 2. */
    SDL_Event ev = {0};
    ev.type = SDL_MOUSEBUTTONDOWN;
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.x = 210;
    ev.button.y = 10;
    assert_true(cbx_widget_handle_event(&tb.base, &ev));
    assert_int_equal(cbx_tabbar_get_active(&tb), 2);

    /* Click outside — not consumed. */
    ev.button.x = 500;
    ev.button.y = 10;
    assert_false(cbx_widget_handle_event(&tb.base, &ev));

    cbx_widget_destroy(&tb.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_tabbar_draw(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_tabbar tb;
    assert_int_equal(cbx_tabbar_init(&tb, 0, &cache, &theme), 0);
    cbx_tabbar_add_tab(&tb, "A", NULL);
    cbx_tabbar_add_tab(&tb, "B", NULL);

    SDL_Rect r = {0, 0, 200, 40};
    cbx_widget_set_rect(&tb.base, &r);
    cbx_widget_focus(&tb.base);
    cbx_widget_draw(&tb.base, ctx.renderer);  /* must not crash */

    cbx_widget_blur(&tb.base);
    cbx_widget_draw(&tb.base, ctx.renderer);

    cbx_widget_destroy(&tb.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_tabbar_draw_with_font(void **state)
{
    (void)state;
    if (!font_available())
        skip();
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);
    int font_id = cbx_text_load_font(&cache, CBX_FONT_PATH, 16);
    assert_int_not_equal(font_id, -1);

    cbx_tabbar tb;
    assert_int_equal(cbx_tabbar_init(&tb, font_id, &cache, &theme), 0);
    cbx_tabbar_add_tab(&tb, "Controllers", NULL);
    cbx_tabbar_add_tab(&tb, "Profiles", NULL);
    cbx_tabbar_add_tab(&tb, "Settings", NULL);

    SDL_Rect r = {0, 0, 300, 40};
    cbx_widget_set_rect(&tb.base, &r);
    cbx_widget_focus(&tb.base);
    cbx_widget_draw(&tb.base, ctx.renderer);

    cbx_widget_destroy(&tb.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_tabbar_focus_blur(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_tabbar tb;
    assert_int_equal(cbx_tabbar_init(&tb, 0, &cache, &theme), 0);
    cbx_tabbar_add_tab(&tb, "A", NULL);

    cbx_widget_focus(&tb.base);
    assert_true(cbx_widget_is_focused(&tb.base));

    cbx_widget_blur(&tb.base);
    assert_false(cbx_widget_is_focused(&tb.base));

    cbx_widget_destroy(&tb.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_tabbar_null_args(void **state)
{
    (void)state;
    assert_int_equal(cbx_tabbar_add_tab(NULL, "A", NULL), -EINVAL);
    assert_int_equal(cbx_tabbar_tab_count(NULL), 0);
    assert_int_equal(cbx_tabbar_get_active(NULL), -1);
    cbx_tabbar_set_active(NULL, 0);
    cbx_tabbar_set_change_cb(NULL, NULL);
}

static void
test_tabbar_unrelated_event(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_tabbar tb;
    assert_int_equal(cbx_tabbar_init(&tb, 0, &cache, &theme), 0);
    cbx_tabbar_add_tab(&tb, "A", NULL);

    SDL_Event ev = {0};
    ev.type = SDL_MOUSEMOTION;
    assert_false(cbx_widget_handle_event(&tb.base, &ev));

    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = SDLK_a;
    assert_false(cbx_widget_handle_event(&tb.base, &ev));

    cbx_widget_destroy(&tb.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_tabbar_empty_tabs_move(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_tabbar tb;
    assert_int_equal(cbx_tabbar_init(&tb, 0, &cache, &theme), 0);

    /* No tabs — can't move. */
    assert_int_equal(cbx_tabbar_move_left(&tb), -EINVAL);
    assert_int_equal(cbx_tabbar_move_right(&tb), -EINVAL);

    cbx_widget_destroy(&tb.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

/* --- main ---------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_tabbar_init_basic),
        cmocka_unit_test(test_tabbar_init_null),
        cmocka_unit_test(test_tabbar_add_tabs),
        cmocka_unit_test(test_tabbar_add_overflow),
        cmocka_unit_test(test_tabbar_move_left_right),
        cmocka_unit_test(test_tabbar_set_active),
        cmocka_unit_test(test_tabbar_key_events),
        cmocka_unit_test(test_tabbar_mouse_click),
        cmocka_unit_test(test_tabbar_draw),
        cmocka_unit_test(test_tabbar_draw_with_font),
        cmocka_unit_test(test_tabbar_focus_blur),
        cmocka_unit_test(test_tabbar_null_args),
        cmocka_unit_test(test_tabbar_unrelated_event),
        cmocka_unit_test(test_tabbar_empty_tabs_move),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}