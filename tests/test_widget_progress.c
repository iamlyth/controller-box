/*
 * test_widget_progress.c — Tests for the ProgressBar widget.
 *
 * Uses the SDL2 dummy driver for headless rendering.
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
#include <string.h>

#include "ui/widget.h"
#include "ui/text.h"
#include "ui/theme.h"

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

/* --- ProgressBar tests --------------------------------------------- */

static void
test_progress_init_basic(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_progress prog;
    assert_int_equal(cbx_progress_init(&prog, &theme), 0);
    assert_true(cbx_widget_is_visible(&prog.base));
    assert_float_equal(cbx_progress_get_fraction(&prog), 0.0, 0.001);

    cbx_widget_destroy(&prog.base);
}

static void
test_progress_init_null(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);
    assert_int_equal(cbx_progress_init(NULL, &theme), -EINVAL);
    assert_int_equal(cbx_progress_init(NULL, NULL), -EINVAL);
}

static void
test_progress_set_fraction(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_progress prog;
    assert_int_equal(cbx_progress_init(&prog, &theme), 0);

    cbx_progress_set_fraction(&prog, 0.5);
    assert_float_equal(cbx_progress_get_fraction(&prog), 0.5, 0.001);

    cbx_progress_set_fraction(&prog, 1.0);
    assert_float_equal(cbx_progress_get_fraction(&prog), 1.0, 0.001);

    cbx_progress_set_fraction(&prog, 0.0);
    assert_float_equal(cbx_progress_get_fraction(&prog), 0.0, 0.001);

    /* Clamping. */
    cbx_progress_set_fraction(&prog, -0.5);
    assert_float_equal(cbx_progress_get_fraction(&prog), 0.0, 0.001);

    cbx_progress_set_fraction(&prog, 2.5);
    assert_float_equal(cbx_progress_get_fraction(&prog), 1.0, 0.001);

    cbx_widget_destroy(&prog.base);
}

static void
test_progress_set_colors(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_progress prog;
    assert_int_equal(cbx_progress_init(&prog, &theme), 0);

    SDL_Color bar = {255, 0, 0, 255};
    cbx_progress_set_bar_color(&prog, bar);
    assert_int_equal(prog.bar_color.r, 255);
    assert_int_equal(prog.bar_color.g, 0);
    assert_int_equal(prog.bar_color.b, 0);

    SDL_Color bg = {50, 50, 50, 255};
    cbx_progress_set_bg_color(&prog, bg);
    assert_int_equal(prog.bg_color.r, 50);
    assert_int_equal(prog.bg_color.g, 50);
    assert_int_equal(prog.bg_color.b, 50);

    cbx_widget_destroy(&prog.base);
}

static void
test_progress_set_colors_null(void **state)
{
    (void)state;
    cbx_progress_set_bar_color(NULL, (SDL_Color){0});
    cbx_progress_set_bg_color(NULL, (SDL_Color){0});
}

static void
test_progress_draw(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_progress prog;
    assert_int_equal(cbx_progress_init(&prog, &theme), 0);

    SDL_Rect r = {0, 0, 200, 20};
    cbx_widget_set_rect(&prog.base, &r);

    cbx_progress_set_fraction(&prog, 0.0);
    cbx_widget_draw(&prog.base, ctx.renderer);  /* must not crash */

    cbx_progress_set_fraction(&prog, 0.5);
    cbx_widget_draw(&prog.base, ctx.renderer);

    cbx_progress_set_fraction(&prog, 1.0);
    cbx_widget_draw(&prog.base, ctx.renderer);

    cbx_widget_destroy(&prog.base);
    test_teardown(&ctx);
}

static void
test_progress_no_event(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_progress prog;
    assert_int_equal(cbx_progress_init(&prog, &theme), 0);

    SDL_Event ev = {0};
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = SDLK_RETURN;
    assert_false(cbx_widget_handle_event(&prog.base, &ev));

    ev.type = SDL_MOUSEBUTTONDOWN;
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.x = 10;
    ev.button.y = 10;
    assert_false(cbx_widget_handle_event(&prog.base, &ev));

    cbx_widget_destroy(&prog.base);
}

static void
test_progress_focus_noop(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_progress prog;
    assert_int_equal(cbx_progress_init(&prog, &theme), 0);

    cbx_widget_focus(&prog.base);
    assert_false(cbx_widget_is_focused(&prog.base));

    cbx_widget_blur(&prog.base);
    assert_false(cbx_widget_is_focused(&prog.base));

    cbx_widget_destroy(&prog.base);
}

static void
test_progress_set_fraction_null(void **state)
{
    (void)state;
    cbx_progress_set_fraction(NULL, 0.5);
    assert_float_equal(cbx_progress_get_fraction(NULL), 0.0, 0.001);
}

static void
test_progress_default_colors(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_progress prog;
    assert_int_equal(cbx_progress_init(&prog, &theme), 0);

    /* Default bar_color = theme->text_accent, bg = panel_bg. */
    assert_int_equal(prog.bar_color.r, theme.text_accent.r);
    assert_int_equal(prog.bar_color.g, theme.text_accent.g);
    assert_int_equal(prog.bar_color.b, theme.text_accent.b);

    assert_int_equal(prog.bg_color.r, theme.panel_bg.r);
    assert_int_equal(prog.bg_color.g, theme.panel_bg.g);
    assert_int_equal(prog.bg_color.b, theme.panel_bg.b);

    cbx_widget_destroy(&prog.base);
}

static void
test_progress_rect(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_progress prog;
    assert_int_equal(cbx_progress_init(&prog, &theme), 0);

    SDL_Rect r = {10, 20, 300, 24};
    cbx_widget_set_rect(&prog.base, &r);

    SDL_Rect out;
    cbx_widget_get_rect(&prog.base, &out);
    assert_int_equal(out.x, 10);
    assert_int_equal(out.y, 20);
    assert_int_equal(out.w, 300);
    assert_int_equal(out.h, 24);

    cbx_widget_destroy(&prog.base);
}

static void
test_progress_draw_null(void **state)
{
    (void)state;
    cbx_widget_draw(NULL, NULL);  /* must not crash */
}

/* --- main ---------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_progress_init_basic),
        cmocka_unit_test(test_progress_init_null),
        cmocka_unit_test(test_progress_set_fraction),
        cmocka_unit_test(test_progress_set_colors),
        cmocka_unit_test(test_progress_set_colors_null),
        cmocka_unit_test(test_progress_draw),
        cmocka_unit_test(test_progress_no_event),
        cmocka_unit_test(test_progress_focus_noop),
        cmocka_unit_test(test_progress_set_fraction_null),
        cmocka_unit_test(test_progress_default_colors),
        cmocka_unit_test(test_progress_rect),
        cmocka_unit_test(test_progress_draw_null),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}