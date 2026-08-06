/*
 * test_widget_list.c — Tests for the List widget.
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

/* --- helpers ------------------------------------------------------- */

static SDL_Texture *
make_test_texture(SDL_Renderer *r, int w, int h)
{
    SDL_Texture *tex = SDL_CreateTexture(r,
                                          SDL_PIXELFORMAT_RGBA8888,
                                          SDL_TEXTUREACCESS_STATIC,
                                          w, h);
    return tex;
}

/* --- List tests ---------------------------------------------------- */

static void
test_list_init_basic(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_list lst;
    assert_int_equal(cbx_list_init(&lst, 0, &cache, &theme), 0);
    assert_true(cbx_widget_is_visible(&lst.base));
    assert_int_equal(cbx_list_item_count(&lst), 0);
    assert_int_equal(cbx_list_get_selected(&lst), -1);

    cbx_widget_destroy(&lst.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_list_init_null(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    /* Don't need full setup for null arg tests. */
    cbx_list lst;
    assert_int_equal(cbx_list_init(NULL, 0, &cache, &theme), -EINVAL);
    assert_int_equal(cbx_list_init(&lst, 0, NULL, &theme), -EINVAL);
    assert_int_equal(cbx_list_init(&lst, 0, &cache, NULL), -EINVAL);
}

static void
test_list_add_items(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_list lst;
    assert_int_equal(cbx_list_init(&lst, 0, &cache, &theme), 0);

    int rc = cbx_list_add_item(&lst, "Item 1", NULL, NULL);
    assert_int_equal(rc, 0);
    rc = cbx_list_add_item(&lst, "Item 2", NULL, NULL);
    assert_int_equal(rc, 1);
    rc = cbx_list_add_item(&lst, "Item 3", NULL, NULL);
    assert_int_equal(rc, 2);

    assert_int_equal(cbx_list_item_count(&lst), 3);
    /* First item auto-selected. */
    assert_int_equal(cbx_list_get_selected(&lst), 0);

    cbx_widget_destroy(&lst.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_list_add_with_icon(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_list lst;
    assert_int_equal(cbx_list_init(&lst, 0, &cache, &theme), 0);

    SDL_Texture *icon = make_test_texture(ctx.renderer, 16, 16);
    assert_non_null(icon);
    int rc = cbx_list_add_item(&lst, "With Icon", icon, NULL);
    assert_int_equal(rc, 0);
    assert_ptr_equal(lst.items[0].icon, icon);

    cbx_widget_destroy(&lst.base);
    SDL_DestroyTexture(icon);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_list_add_overflow(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_list lst;
    assert_int_equal(cbx_list_init(&lst, 0, &cache, &theme), 0);

    for (int i = 0; i < CBX_LIST_MAX_ITEMS; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Item %d", i);
        assert_int_equal(cbx_list_add_item(&lst, buf, NULL, NULL), i);
    }
    assert_int_equal(cbx_list_add_item(&lst, "overflow", NULL, NULL), -ENOMEM);

    cbx_widget_destroy(&lst.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_list_clear(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_list lst;
    assert_int_equal(cbx_list_init(&lst, 0, &cache, &theme), 0);
    cbx_list_add_item(&lst, "A", NULL, NULL);
    cbx_list_add_item(&lst, "B", NULL, NULL);
    assert_int_equal(cbx_list_item_count(&lst), 2);

    cbx_list_clear(&lst);
    assert_int_equal(cbx_list_item_count(&lst), 0);
    assert_int_equal(cbx_list_get_selected(&lst), -1);

    cbx_widget_destroy(&lst.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_list_navigation(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_list lst;
    assert_int_equal(cbx_list_init(&lst, 0, &cache, &theme), 0);
    cbx_list_add_item(&lst, "A", NULL, NULL);
    cbx_list_add_item(&lst, "B", NULL, NULL);
    cbx_list_add_item(&lst, "C", NULL, NULL);

    /* Down from first. */
    SDL_Event ev = {0};
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = SDLK_DOWN;
    assert_true(cbx_widget_handle_event(&lst.base, &ev));
    assert_int_equal(cbx_list_get_selected(&lst), 1);

    /* Down again. */
    assert_true(cbx_widget_handle_event(&lst.base, &ev));
    assert_int_equal(cbx_list_get_selected(&lst), 2);

    /* Down at end — stays. */
    assert_true(cbx_widget_handle_event(&lst.base, &ev));
    assert_int_equal(cbx_list_get_selected(&lst), 2);

    /* Up. */
    ev.key.keysym.sym = SDLK_UP;
    assert_true(cbx_widget_handle_event(&lst.base, &ev));
    assert_int_equal(cbx_list_get_selected(&lst), 1);

    /* Up again. */
    assert_true(cbx_widget_handle_event(&lst.base, &ev));
    assert_int_equal(cbx_list_get_selected(&lst), 0);

    /* Up at start — stays. */
    assert_true(cbx_widget_handle_event(&lst.base, &ev));
    assert_int_equal(cbx_list_get_selected(&lst), 0);

    cbx_widget_destroy(&lst.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_list_set_selected(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_list lst;
    assert_int_equal(cbx_list_init(&lst, 0, &cache, &theme), 0);
    cbx_list_add_item(&lst, "A", NULL, NULL);
    cbx_list_add_item(&lst, "B", NULL, NULL);
    cbx_list_add_item(&lst, "C", NULL, NULL);

    cbx_list_set_selected(&lst, 2);
    assert_int_equal(cbx_list_get_selected(&lst), 2);

    cbx_list_set_selected(&lst, 0);
    assert_int_equal(cbx_list_get_selected(&lst), 0);

    /* Out of range — clears selection. */
    cbx_list_set_selected(&lst, 99);
    assert_int_equal(cbx_list_get_selected(&lst), -1);

    /* Negative — clears. */
    cbx_list_set_selected(&lst, -1);
    assert_int_equal(cbx_list_get_selected(&lst), -1);

    cbx_widget_destroy(&lst.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static int select_called = 0;
static int select_index = -1;
static void
on_select_cb(cbx_widget *w, int index, void *user_data)
{
    (void)w;
    (void)user_data;
    select_called++;
    select_index = index;
}

static void
test_list_select_callback(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_list lst;
    assert_int_equal(cbx_list_init(&lst, 0, &cache, &theme), 0);
    cbx_list_add_item(&lst, "A", NULL, NULL);
    cbx_list_add_item(&lst, "B", NULL, NULL);
    cbx_list_set_select_cb(&lst, on_select_cb);

    select_called = 0;
    select_index = -1;

    /* Return key triggers select. */
    SDL_Event ev = {0};
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = SDLK_RETURN;
    assert_true(cbx_widget_handle_event(&lst.base, &ev));
    assert_int_equal(select_called, 1);
    assert_int_equal(select_index, 0);

    /* Space also triggers. */
    ev.key.keysym.sym = SDLK_SPACE;
    assert_true(cbx_widget_handle_event(&lst.base, &ev));
    assert_int_equal(select_called, 2);

    cbx_widget_destroy(&lst.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_list_scroll(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_list lst;
    assert_int_equal(cbx_list_init(&lst, 0, &cache, &theme), 0);
    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Item %d", i);
        cbx_list_add_item(&lst, buf, NULL, NULL);
    }

    SDL_Rect r = {0, 0, 200, 128};  /* 128/32 = 4 visible */
    cbx_widget_set_rect(&lst.base, &r);
    assert_int_equal(lst.visible_count, 4);

    /* Scroll down — moves scroll_offset. */
    assert_int_equal(cbx_list_scroll_down(&lst), 0);
    assert_int_equal(lst.scroll_offset, 1);
    assert_int_equal(cbx_list_scroll_down(&lst), 0);
    assert_int_equal(lst.scroll_offset, 2);

    /* Max scroll = 10 - 4 = 6. */
    for (int i = 0; i < 4; i++)
        cbx_list_scroll_down(&lst);
    assert_int_equal(lst.scroll_offset, 6);
    assert_int_equal(cbx_list_scroll_down(&lst), -1);
    assert_int_equal(lst.scroll_offset, 6);

    /* Scroll up. */
    assert_int_equal(cbx_list_scroll_up(&lst), 0);
    assert_int_equal(lst.scroll_offset, 5);
    /* Up at 0. */
    lst.scroll_offset = 0;
    assert_int_equal(cbx_list_scroll_up(&lst), -1);
    assert_int_equal(lst.scroll_offset, 0);

    cbx_widget_destroy(&lst.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_list_scroll_auto_on_nav(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_list lst;
    assert_int_equal(cbx_list_init(&lst, 0, &cache, &theme), 0);
    for (int i = 0; i < 8; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Item %d", i);
        cbx_list_add_item(&lst, buf, NULL, NULL);
    }
    SDL_Rect r = {0, 0, 200, 96};  /* 96/32 = 3 visible */
    cbx_widget_set_rect(&lst.base, &r);
    assert_int_equal(lst.visible_count, 3);

    /* Navigate down past visible area. */
    SDL_Event ev = {0};
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = SDLK_DOWN;
    for (int i = 0; i < 5; i++)
        cbx_widget_handle_event(&lst.base, &ev);
    /* selected=5, scroll_offset should be 3 (5 - 3 + 1). */
    assert_int_equal(cbx_list_get_selected(&lst), 5);
    assert_int_equal(lst.scroll_offset, 3);

    cbx_widget_destroy(&lst.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_list_mouse_wheel(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_list lst;
    assert_int_equal(cbx_list_init(&lst, 0, &cache, &theme), 0);
    for (int i = 0; i < 8; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Item %d", i);
        cbx_list_add_item(&lst, buf, NULL, NULL);
    }
    SDL_Rect r = {0, 0, 200, 96};  /* 3 visible, max scroll = 5 */
    cbx_widget_set_rect(&lst.base, &r);

    SDL_Event ev = {0};
    ev.type = SDL_MOUSEWHEEL;
    ev.wheel.y = -1;  /* scroll down */
    assert_true(cbx_widget_handle_event(&lst.base, &ev));
    assert_int_equal(lst.scroll_offset, 1);

    ev.wheel.y = 1;  /* scroll up */
    assert_true(cbx_widget_handle_event(&lst.base, &ev));
    assert_int_equal(lst.scroll_offset, 0);

    cbx_widget_destroy(&lst.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_list_mouse_click(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_list lst;
    assert_int_equal(cbx_list_init(&lst, 0, &cache, &theme), 0);
    cbx_list_add_item(&lst, "A", NULL, NULL);
    cbx_list_add_item(&lst, "B", NULL, NULL);
    cbx_list_add_item(&lst, "C", NULL, NULL);

    SDL_Rect r = {0, 0, 200, 96};
    cbx_widget_set_rect(&lst.base, &r);
    /* item_h = 32, so clicking at y=64 selects item 2. */
    SDL_Event ev = {0};
    ev.type = SDL_MOUSEBUTTONDOWN;
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.x = 10;
    ev.button.y = 64;
    assert_true(cbx_widget_handle_event(&lst.base, &ev));
    assert_int_equal(cbx_list_get_selected(&lst), 2);

    cbx_widget_destroy(&lst.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_list_draw(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_list lst;
    assert_int_equal(cbx_list_init(&lst, 0, &cache, &theme), 0);
    cbx_list_add_item(&lst, "A", NULL, NULL);
    cbx_list_add_item(&lst, "B", NULL, NULL);

    SDL_Rect r = {0, 0, 200, 128};
    cbx_widget_set_rect(&lst.base, &r);
    cbx_widget_focus(&lst.base);
    cbx_widget_draw(&lst.base, ctx.renderer);  /* should not crash */

    /* Unfocused draw. */
    cbx_widget_blur(&lst.base);
    cbx_widget_draw(&lst.base, ctx.renderer);

    cbx_widget_destroy(&lst.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_list_draw_with_font(void **state)
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

    cbx_list lst;
    assert_int_equal(cbx_list_init(&lst, font_id, &cache, &theme), 0);
    cbx_list_add_item(&lst, "Hello", NULL, NULL);
    cbx_list_add_item(&lst, "World", NULL, NULL);

    SDL_Rect r = {0, 0, 200, 128};
    cbx_widget_set_rect(&lst.base, &r);
    cbx_widget_focus(&lst.base);
    cbx_widget_draw(&lst.base, ctx.renderer);  /* should not crash */

    cbx_widget_destroy(&lst.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_list_focus_blur(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_list lst;
    assert_int_equal(cbx_list_init(&lst, 0, &cache, &theme), 0);
    cbx_list_add_item(&lst, "A", NULL, NULL);

    cbx_widget_focus(&lst.base);
    assert_true(cbx_widget_is_focused(&lst.base));
    assert_int_equal(cbx_list_get_selected(&lst), 0);

    cbx_widget_blur(&lst.base);
    assert_false(cbx_widget_is_focused(&lst.base));

    cbx_widget_destroy(&lst.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_list_null_args(void **state)
{
    (void)state;
    cbx_list_add_item(NULL, "A", NULL, NULL);
    cbx_list_clear(NULL);
    assert_int_equal(cbx_list_item_count(NULL), 0);
    assert_int_equal(cbx_list_get_selected(NULL), -1);
    cbx_list_set_selected(NULL, 0);
    assert_int_equal(cbx_list_scroll_up(NULL), -EINVAL);
    assert_int_equal(cbx_list_scroll_down(NULL), -EINVAL);
    cbx_list_set_select_cb(NULL, NULL);
}

static void
test_list_draw_null(void **state)
{
    (void)state;
    cbx_widget_draw(NULL, NULL);  /* must not crash */
}

static void
test_list_unrelated_event(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_list lst;
    assert_int_equal(cbx_list_init(&lst, 0, &cache, &theme), 0);
    cbx_list_add_item(&lst, "A", NULL, NULL);

    SDL_Event ev = {0};
    ev.type = SDL_MOUSEMOTION;
    assert_false(cbx_widget_handle_event(&lst.base, &ev));

    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = SDLK_a;
    assert_false(cbx_widget_handle_event(&lst.base, &ev));

    cbx_widget_destroy(&lst.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_list_user_data(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_list lst;
    assert_int_equal(cbx_list_init(&lst, 0, &cache, &theme), 0);

    int data1 = 42, data2 = 99;
    cbx_list_add_item(&lst, "A", NULL, &data1);
    cbx_list_add_item(&lst, "B", NULL, &data2);

    assert_ptr_equal(lst.items[0].user_data, &data1);
    assert_ptr_equal(lst.items[1].user_data, &data2);

    cbx_widget_destroy(&lst.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

/* --- main ---------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_list_init_basic),
        cmocka_unit_test(test_list_init_null),
        cmocka_unit_test(test_list_add_items),
        cmocka_unit_test(test_list_add_with_icon),
        cmocka_unit_test(test_list_add_overflow),
        cmocka_unit_test(test_list_clear),
        cmocka_unit_test(test_list_navigation),
        cmocka_unit_test(test_list_set_selected),
        cmocka_unit_test(test_list_select_callback),
        cmocka_unit_test(test_list_scroll),
        cmocka_unit_test(test_list_scroll_auto_on_nav),
        cmocka_unit_test(test_list_mouse_wheel),
        cmocka_unit_test(test_list_mouse_click),
        cmocka_unit_test(test_list_draw),
        cmocka_unit_test(test_list_draw_with_font),
        cmocka_unit_test(test_list_focus_blur),
        cmocka_unit_test(test_list_null_args),
        cmocka_unit_test(test_list_draw_null),
        cmocka_unit_test(test_list_unrelated_event),
        cmocka_unit_test(test_list_user_data),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}