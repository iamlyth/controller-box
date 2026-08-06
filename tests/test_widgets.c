/*
 * test_widgets.c — Tests for widget base, Button, Label, Image, Panel.
 *
 * Uses the SDL2 dummy driver (software renderer) for headless rendering.
 * Font-dependent tests are skipped if no TTF font is available at compile time.
 *
 * Task 20 — Widget base and concrete widgets.
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
#include "ui/renderer.h"

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

/* Create a minimal SDL_Texture for Image widget tests. */
static SDL_Texture *
make_test_texture(SDL_Renderer *r, int w, int h)
{
    SDL_Texture *tex = SDL_CreateTexture(r,
                                          SDL_PIXELFORMAT_RGBA8888,
                                          SDL_TEXTUREACCESS_STATIC,
                                          w, h);
    if (tex) {
        /* Upload some pixels so QueryTexture gives meaningful dims. */
        Uint32 pixels[4] = { 0xFF0000FF, 0xFF00FF00, 0xFFFF0000, 0xFFFFFFFF };
        SDL_Rect rect = {0, 0, w > 2 ? 2 : w, h > 2 ? 2 : h};
        SDL_UpdateTexture(tex, &rect, pixels,
                          w * (int)sizeof(Uint32));
    }
    return tex;
}

/* --- base widget dispatchers -------------------------------------- */

static void
test_widget_draw_null(void **state)
{
    (void)state;
    cbx_widget_draw(NULL, NULL);     /* must not crash */
}

static void
test_widget_handle_event_null(void **state)
{
    (void)state;
    assert_false(cbx_widget_handle_event(NULL, NULL));
}

static void
test_widget_focus_blur_null(void **state)
{
    (void)state;
    cbx_widget_focus(NULL);   /* must not crash */
    cbx_widget_blur(NULL);    /* must not crash */
}

static void
test_widget_get_set_rect_null(void **state)
{
    (void)state;
    cbx_widget_get_rect(NULL, NULL);    /* must not crash */
    cbx_widget_set_rect(NULL, NULL);    /* must not crash */
}

static void
test_widget_destroy_null(void **state)
{
    (void)state;
    cbx_widget_destroy(NULL);   /* must not crash */
}

static void
test_widget_is_focused_null(void **state)
{
    (void)state;
    assert_false(cbx_widget_is_focused(NULL));
}

static void
test_widget_is_visible_null(void **state)
{
    (void)state;
    assert_false(cbx_widget_is_visible(NULL));
}

static void
test_widget_set_visible_null(void **state)
{
    (void)state;
    cbx_widget_set_visible(NULL, true);   /* must not crash */
}

static void
test_widget_accessors(void **state)
{
    (void)state;
    /* Use a label as a concrete widget for accessor testing. */
    cbx_label lbl;
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    assert_int_equal(cbx_label_init(&lbl, "test", 0, &cache, &theme), 0);
    assert_true(cbx_widget_is_visible(&lbl.base));
    assert_false(cbx_widget_is_focused(&lbl.base));

    cbx_widget_set_visible(&lbl.base, false);
    assert_false(cbx_widget_is_visible(&lbl.base));
    cbx_widget_set_visible(&lbl.base, true);
    assert_true(cbx_widget_is_visible(&lbl.base));

    /* Set / get rect. */
    SDL_Rect r = {10, 20, 100, 30};
    cbx_widget_set_rect(&lbl.base, &r);
    SDL_Rect out;
    cbx_widget_get_rect(&lbl.base, &out);
    assert_int_equal(out.x, 10);
    assert_int_equal(out.y, 20);
    assert_int_equal(out.w, 100);
    assert_int_equal(out.h, 30);

    cbx_widget_destroy(&lbl.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

/* --- Button tests -------------------------------------------------- */

static void
on_press(cbx_widget *w, void *user_data)
{
    (void)w;
    int *counter = user_data;
    if (counter)
        (*counter)++;
}

static void
test_button_init_basic(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_button btn;
    int rc = cbx_button_init(&btn, "OK", 0, &cache, &theme, NULL, NULL);
    assert_int_equal(rc, 0);
    assert_true(cbx_widget_is_visible(&btn.base));
    assert_false(cbx_widget_is_focused(&btn.base));
    assert_false(btn.pressed);
    assert_string_equal(btn.label, "OK");

    cbx_widget_destroy(&btn.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_button_init_null(void **state)
{
    (void)state;
    assert_int_equal(cbx_button_init(NULL, "x", 0, NULL, NULL, NULL, NULL),
                     -EINVAL);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_button btn;
    assert_int_equal(cbx_button_init(&btn, "x", 0, NULL, &theme, NULL, NULL),
                     -EINVAL);
    assert_int_equal(cbx_button_init(&btn, "x", 0, &cache, NULL, NULL, NULL),
                     -EINVAL);

    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_button_draw(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_button btn;
    assert_int_equal(cbx_button_init(&btn, "Test", 0, &cache, &theme,
                                      NULL, NULL), 0);
    SDL_Rect r = {0, 0, 120, 40};
    cbx_widget_set_rect(&btn.base, &r);

    SDL_RenderClear(ctx.renderer);
    cbx_widget_draw(&btn.base, ctx.renderer);   /* must not crash */
    SDL_RenderPresent(ctx.renderer);

    /* Focus and draw — should not crash. */
    cbx_widget_focus(&btn.base);
    cbx_widget_draw(&btn.base, ctx.renderer);

    /* Press and draw — should not crash. */
    cbx_button_set_pressed(&btn, true);
    cbx_widget_draw(&btn.base, ctx.renderer);

    cbx_widget_destroy(&btn.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_button_focus_blur(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_button btn;
    assert_int_equal(cbx_button_init(&btn, "F", 0, &cache, &theme,
                                      NULL, NULL), 0);

    cbx_widget_focus(&btn.base);
    assert_true(cbx_widget_is_focused(&btn.base));

    cbx_widget_blur(&btn.base);
    assert_false(cbx_widget_is_focused(&btn.base));
    assert_false(btn.pressed);   /* blur resets pressed */

    cbx_widget_destroy(&btn.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_button_mouse_press(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    int counter = 0;
    cbx_button btn;
    assert_int_equal(cbx_button_init(&btn, "Go", 0, &cache, &theme,
                                      on_press, &counter), 0);
    SDL_Rect r = {10, 10, 100, 50};
    cbx_widget_set_rect(&btn.base, &r);

    /* Mouse down inside button. */
    SDL_Event ev;
    ev.type = SDL_MOUSEBUTTONDOWN;
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.x = 50;
    ev.button.y = 30;
    assert_true(cbx_widget_handle_event(&btn.base, &ev));
    assert_true(btn.pressed);

    /* Mouse up inside button → callback fired. */
    ev.type = SDL_MOUSEBUTTONUP;
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.x = 50;
    ev.button.y = 30;
    assert_true(cbx_widget_handle_event(&btn.base, &ev));
    assert_false(btn.pressed);
    assert_int_equal(counter, 1);

    cbx_widget_destroy(&btn.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_button_mouse_press_outside(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    int counter = 0;
    cbx_button btn;
    assert_int_equal(cbx_button_init(&btn, "Go", 0, &cache, &theme,
                                      on_press, &counter), 0);
    SDL_Rect r = {10, 10, 100, 50};
    cbx_widget_set_rect(&btn.base, &r);

    /* Mouse down inside. */
    SDL_Event ev;
    ev.type = SDL_MOUSEBUTTONDOWN;
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.x = 50;
    ev.button.y = 30;
    cbx_widget_handle_event(&btn.base, &ev);
    assert_true(btn.pressed);

    /* Mouse up OUTSIDE button → no callback, but pressed cleared. */
    ev.type = SDL_MOUSEBUTTONUP;
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.x = 200;
    ev.button.y = 200;
    assert_true(cbx_widget_handle_event(&btn.base, &ev));
    assert_false(btn.pressed);
    assert_int_equal(counter, 0);   /* callback not fired */

    cbx_widget_destroy(&btn.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_button_key_press(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    int counter = 0;
    cbx_button btn;
    assert_int_equal(cbx_button_init(&btn, "OK", 0, &cache, &theme,
                                      on_press, &counter), 0);

    /* KeyDown Return. */
    SDL_Event ev;
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = SDLK_RETURN;
    assert_true(cbx_widget_handle_event(&btn.base, &ev));
    assert_true(btn.pressed);

    /* KeyUp Return → callback fired. */
    ev.type = SDL_KEYUP;
    ev.key.keysym.sym = SDLK_RETURN;
    assert_true(cbx_widget_handle_event(&btn.base, &ev));
    assert_false(btn.pressed);
    assert_int_equal(counter, 1);

    /* KeyDown/KeyUp Space. */
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = SDLK_SPACE;
    assert_true(cbx_widget_handle_event(&btn.base, &ev));
    ev.type = SDL_KEYUP;
    ev.key.keysym.sym = SDLK_SPACE;
    assert_true(cbx_widget_handle_event(&btn.base, &ev));
    assert_int_equal(counter, 2);

    cbx_widget_destroy(&btn.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_button_no_callback(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_button btn;
    assert_int_equal(cbx_button_init(&btn, "X", 0, &cache, &theme,
                                      NULL, NULL), 0);

    /* KeyDown/KeyUp Return with no callback — must not crash. */
    SDL_Event ev;
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = SDLK_RETURN;
    assert_true(cbx_widget_handle_event(&btn.base, &ev));
    ev.type = SDL_KEYUP;
    ev.key.keysym.sym = SDLK_RETURN;
    assert_true(cbx_widget_handle_event(&btn.base, &ev));

    cbx_widget_destroy(&btn.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_button_set_label(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_button btn;
    assert_int_equal(cbx_button_init(&btn, "Old", 0, &cache, &theme,
                                      NULL, NULL), 0);
    assert_string_equal(btn.label, "Old");

    cbx_button_set_label(&btn, "New");
    assert_string_equal(btn.label, "New");

    cbx_button_set_label(&btn, NULL);
    assert_string_equal(btn.label, "");

    cbx_widget_destroy(&btn.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_button_set_press_cb(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_button btn;
    assert_int_equal(cbx_button_init(&btn, "X", 0, &cache, &theme,
                                      NULL, NULL), 0);
    assert_null(btn.on_press);

    int counter = 0;
    cbx_button_set_press_cb(&btn, on_press, &counter);
    assert_non_null(btn.on_press);
    assert_ptr_equal(btn.user_data, &counter);

    /* Trigger via keyboard. */
    SDL_Event ev;
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = SDLK_RETURN;
    cbx_widget_handle_event(&btn.base, &ev);
    ev.type = SDL_KEYUP;
    ev.key.keysym.sym = SDLK_RETURN;
    cbx_widget_handle_event(&btn.base, &ev);
    assert_int_equal(counter, 1);

    cbx_widget_destroy(&btn.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_button_unrelated_event(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_button btn;
    assert_int_equal(cbx_button_init(&btn, "X", 0, &cache, &theme,
                                      NULL, NULL), 0);

    /* Unrelated key should not be consumed. */
    SDL_Event ev;
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = SDLK_a;
    assert_false(cbx_widget_handle_event(&btn.base, &ev));

    /* Right mouse button should not be consumed. */
    ev.type = SDL_MOUSEBUTTONDOWN;
    ev.button.button = SDL_BUTTON_RIGHT;
    ev.button.x = 0;
    ev.button.y = 0;
    assert_false(cbx_widget_handle_event(&btn.base, &ev));

    cbx_widget_destroy(&btn.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_button_with_font(void **state)
{
    (void)state;
    if (!font_available()) { skip(); return; }
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);
    int fid = cbx_text_load_font(&cache, CBX_FONT_PATH, 16);
    assert_true(fid >= 0);

    cbx_button btn;
    assert_int_equal(cbx_button_init(&btn, "Hello", fid, &cache, &theme,
                                      NULL, NULL), 0);
    assert_non_null(btn.label_tex);
    assert_true(btn.label_w > 0);
    assert_true(btn.label_h > 0);

    SDL_Rect r = {0, 0, 200, 60};
    cbx_widget_set_rect(&btn.base, &r);
    SDL_RenderClear(ctx.renderer);
    cbx_widget_draw(&btn.base, ctx.renderer);   /* should render label */
    SDL_RenderPresent(ctx.renderer);

    cbx_widget_destroy(&btn.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

/* --- Label tests --------------------------------------------------- */

static void
test_label_init_basic(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_label lbl;
    assert_int_equal(cbx_label_init(&lbl, "Hello", 0, &cache, &theme), 0);
    assert_true(cbx_widget_is_visible(&lbl.base));
    assert_string_equal(lbl.text, "Hello");
    assert_false(lbl.multiline);

    cbx_widget_destroy(&lbl.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_label_init_null(void **state)
{
    (void)state;
    assert_int_equal(cbx_label_init(NULL, "x", 0, NULL, NULL), -EINVAL);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_label lbl;
    assert_int_equal(cbx_label_init(&lbl, "x", 0, NULL, &theme), -EINVAL);
    assert_int_equal(cbx_label_init(&lbl, "x", 0, &cache, NULL), -EINVAL);

    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_label_draw(void **state)
{
    (void)state;
    if (!font_available()) { skip(); return; }
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);
    int fid = cbx_text_load_font(&cache, CBX_FONT_PATH, 16);
    assert_true(fid >= 0);

    cbx_label lbl;
    assert_int_equal(cbx_label_init(&lbl, "Test Label", fid, &cache, &theme),
                     0);
    SDL_Rect r = {10, 10, 200, 30};
    cbx_widget_set_rect(&lbl.base, &r);

    SDL_RenderClear(ctx.renderer);
    cbx_widget_draw(&lbl.base, ctx.renderer);
    SDL_RenderPresent(ctx.renderer);

    /* Multiline draw. */
    cbx_label_set_multiline(&lbl, true);
    cbx_label_set_text(&lbl, "Line 1\nLine 2\nLine 3");
    cbx_widget_draw(&lbl.base, ctx.renderer);

    /* Empty text draw — should not crash. */
    cbx_label_set_multiline(&lbl, false);
    cbx_label_set_text(&lbl, "");
    cbx_widget_draw(&lbl.base, ctx.renderer);

    cbx_widget_destroy(&lbl.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_label_no_event(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_label lbl;
    assert_int_equal(cbx_label_init(&lbl, "x", 0, &cache, &theme), 0);

    SDL_Event ev;
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = SDLK_RETURN;
    assert_false(cbx_widget_handle_event(&lbl.base, &ev));

    ev.type = SDL_MOUSEBUTTONDOWN;
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.x = 0;
    ev.button.y = 0;
    assert_false(cbx_widget_handle_event(&lbl.base, &ev));

    cbx_widget_destroy(&lbl.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_label_set_color(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_label lbl;
    assert_int_equal(cbx_label_init(&lbl, "x", 0, &cache, &theme), 0);
    assert_int_equal(lbl.color.r, theme.text_primary.r);

    SDL_Color c = {255, 0, 0, 255};
    cbx_label_set_color(&lbl, c);
    assert_int_equal(lbl.color.r, 255);
    assert_int_equal(lbl.color.g, 0);

    cbx_widget_destroy(&lbl.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_label_set_multiline(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_label lbl;
    assert_int_equal(cbx_label_init(&lbl, "x", 0, &cache, &theme), 0);
    assert_false(lbl.multiline);

    cbx_label_set_multiline(&lbl, true);
    assert_true(lbl.multiline);

    cbx_label_set_multiline(&lbl, false);
    assert_false(lbl.multiline);

    cbx_widget_destroy(&lbl.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

/* --- Image tests --------------------------------------------------- */

static void
test_image_init_basic(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    SDL_Texture *tex = make_test_texture(ctx.renderer, 64, 32);
    assert_non_null(tex);

    cbx_image img;
    assert_int_equal(cbx_image_init(&img, tex, true), 0);
    assert_true(cbx_widget_is_visible(&img.base));
    assert_int_equal(img.scale_mode, CBX_IMAGE_SCALE_FIT);
    assert_true(img.owns_texture);

    int w, h;
    assert_int_equal(cbx_image_get_natural_dims(&img, &w, &h), 0);
    assert_int_equal(w, 64);
    assert_int_equal(h, 32);

    cbx_widget_destroy(&img.base);
    test_teardown(&ctx);
}

static void
test_image_init_null(void **state)
{
    (void)state;
    assert_int_equal(cbx_image_init(NULL, NULL, false), -EINVAL);

    cbx_image img;
    assert_int_equal(cbx_image_init(&img, NULL, false), 0);
    assert_null(img.texture);
    assert_int_equal(img.tex_w, 0);
    assert_int_equal(img.tex_h, 0);

    cbx_widget_destroy(&img.base);
}

static void
test_image_draw_fit(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    SDL_Texture *tex = make_test_texture(ctx.renderer, 64, 32);
    assert_non_null(tex);

    cbx_image img;
    assert_int_equal(cbx_image_init(&img, tex, true), 0);
    SDL_Rect r = {0, 0, 100, 100};
    cbx_widget_set_rect(&img.base, &r);

    /* FIT mode — scale to fit within 100x100, preserving aspect. */
    SDL_RenderClear(ctx.renderer);
    cbx_widget_draw(&img.base, ctx.renderer);   /* must not crash */
    SDL_RenderPresent(ctx.renderer);

    cbx_widget_destroy(&img.base);
    test_teardown(&ctx);
}

static void
test_image_draw_fill(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    SDL_Texture *tex = make_test_texture(ctx.renderer, 64, 32);
    cbx_image img;
    assert_int_equal(cbx_image_init(&img, tex, true), 0);
    cbx_image_set_scale_mode(&img, CBX_IMAGE_SCALE_FILL);
    SDL_Rect r = {0, 0, 200, 50};
    cbx_widget_set_rect(&img.base, &r);

    SDL_RenderClear(ctx.renderer);
    cbx_widget_draw(&img.base, ctx.renderer);
    SDL_RenderPresent(ctx.renderer);

    cbx_widget_destroy(&img.base);
    test_teardown(&ctx);
}

static void
test_image_draw_center(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    SDL_Texture *tex = make_test_texture(ctx.renderer, 32, 32);
    cbx_image img;
    assert_int_equal(cbx_image_init(&img, tex, true), 0);
    cbx_image_set_scale_mode(&img, CBX_IMAGE_SCALE_CENTER);
    SDL_Rect r = {0, 0, 100, 100};
    cbx_widget_set_rect(&img.base, &r);

    SDL_RenderClear(ctx.renderer);
    cbx_widget_draw(&img.base, ctx.renderer);
    SDL_RenderPresent(ctx.renderer);

    cbx_widget_destroy(&img.base);
    test_teardown(&ctx);
}

static void
test_image_no_event(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    SDL_Texture *tex = make_test_texture(ctx.renderer, 32, 32);
    cbx_image img;
    assert_int_equal(cbx_image_init(&img, tex, true), 0);

    SDL_Event ev;
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = SDLK_a;
    assert_false(cbx_widget_handle_event(&img.base, &ev));

    cbx_widget_destroy(&img.base);
    test_teardown(&ctx);
}

static void
test_image_set_texture(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    SDL_Texture *tex1 = make_test_texture(ctx.renderer, 32, 32);
    SDL_Texture *tex2 = make_test_texture(ctx.renderer, 64, 64);

    cbx_image img;
    assert_int_equal(cbx_image_init(&img, tex1, true), 0);
    assert_ptr_equal(img.texture, tex1);
    assert_int_equal(img.tex_w, 32);

    cbx_image_set_texture(&img, tex2, true);
    assert_ptr_equal(img.texture, tex2);
    assert_int_equal(img.tex_w, 64);
    assert_int_equal(img.tex_h, 64);
    /* tex1 should have been destroyed (owned). */

    cbx_image_set_texture(&img, NULL, false);
    assert_null(img.texture);
    assert_int_equal(img.tex_w, 0);

    cbx_widget_destroy(&img.base);
    test_teardown(&ctx);
}

static void
test_image_set_scale_mode(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    SDL_Texture *tex = make_test_texture(ctx.renderer, 32, 32);
    cbx_image img;
    assert_int_equal(cbx_image_init(&img, tex, true), 0);
    assert_int_equal(img.scale_mode, CBX_IMAGE_SCALE_FIT);

    cbx_image_set_scale_mode(&img, CBX_IMAGE_SCALE_FILL);
    assert_int_equal(img.scale_mode, CBX_IMAGE_SCALE_FILL);

    cbx_image_set_scale_mode(&img, CBX_IMAGE_SCALE_CENTER);
    assert_int_equal(img.scale_mode, CBX_IMAGE_SCALE_CENTER);

    cbx_widget_destroy(&img.base);
    test_teardown(&ctx);
}

static void
test_image_get_natural_dims_null(void **state)
{
    (void)state;
    assert_int_equal(cbx_image_get_natural_dims(NULL, NULL, NULL), -EINVAL);
}

static void
test_image_draw_null_texture(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    cbx_image img;
    assert_int_equal(cbx_image_init(&img, NULL, false), 0);
    SDL_Rect r = {0, 0, 100, 100};
    cbx_widget_set_rect(&img.base, &r);

    SDL_RenderClear(ctx.renderer);
    cbx_widget_draw(&img.base, ctx.renderer);   /* should not crash */
    SDL_RenderPresent(ctx.renderer);

    cbx_widget_destroy(&img.base);
    test_teardown(&ctx);
}

/* --- Panel tests --------------------------------------------------- */

static void
test_panel_init_basic(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);

    cbx_panel pnl;
    assert_int_equal(cbx_panel_init(&pnl, &theme), 0);
    assert_true(cbx_widget_is_visible(&pnl.base));
    assert_int_equal(pnl.child_count, 0);
    assert_int_equal(pnl.focused_child, -1);
    assert_true(pnl.draw_bg);
    assert_true(pnl.draw_border);
    assert_int_equal(pnl.padding, 8);

    cbx_widget_destroy(&pnl.base);
}

static void
test_panel_init_null(void **state)
{
    (void)state;
    assert_int_equal(cbx_panel_init(NULL, NULL), -EINVAL);
}

static void
test_panel_add_remove_child(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_panel pnl;
    assert_int_equal(cbx_panel_init(&pnl, &theme), 0);

    cbx_label lbl1, lbl2;
    cbx_label_init(&lbl1, "One", 0, &cache, &theme);
    cbx_label_init(&lbl2, "Two", 0, &cache, &theme);

    assert_int_equal(cbx_panel_add_child(&pnl, &lbl1.base), 0);
    assert_int_equal(cbx_panel_child_count(&pnl), 1);
    assert_ptr_equal(cbx_panel_get_child(&pnl, 0), &lbl1.base);

    assert_int_equal(cbx_panel_add_child(&pnl, &lbl2.base), 0);
    assert_int_equal(cbx_panel_child_count(&pnl), 2);
    assert_ptr_equal(cbx_panel_get_child(&pnl, 1), &lbl2.base);

    /* Remove lbl1. */
    assert_int_equal(cbx_panel_remove_child(&pnl, &lbl1.base), 0);
    assert_int_equal(cbx_panel_child_count(&pnl), 1);
    assert_ptr_equal(cbx_panel_get_child(&pnl, 0), &lbl2.base);

    /* Remove non-existent. */
    assert_int_equal(cbx_panel_remove_child(&pnl, &lbl1.base), -ENOENT);

    /* Remove null. */
    assert_int_equal(cbx_panel_remove_child(&pnl, NULL), -EINVAL);

    cbx_widget_destroy(&lbl1.base);
    cbx_widget_destroy(&lbl2.base);
    cbx_widget_destroy(&pnl.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_panel_add_null(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_panel pnl;
    assert_int_equal(cbx_panel_init(&pnl, &theme), 0);

    assert_int_equal(cbx_panel_add_child(&pnl, NULL), -EINVAL);
    assert_int_equal(cbx_panel_child_count(&pnl), 0);

    cbx_widget_destroy(&pnl.base);
}

static void
test_panel_add_overflow(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_panel pnl;
    assert_int_equal(cbx_panel_init(&pnl, &theme), 0);

    cbx_label labels[CBX_PANEL_MAX_CHILDREN + 1];
    for (int i = 0; i < CBX_PANEL_MAX_CHILDREN; i++) {
        cbx_label_init(&labels[i], "x", 0, &cache, &theme);
        assert_int_equal(cbx_panel_add_child(&pnl, &labels[i].base), 0);
    }
    assert_int_equal(cbx_panel_child_count(&pnl), CBX_PANEL_MAX_CHILDREN);

    /* Adding one more should fail. */
    cbx_label_init(&labels[CBX_PANEL_MAX_CHILDREN], "x", 0, &cache, &theme);
    assert_int_equal(cbx_panel_add_child(&pnl,
                      &labels[CBX_PANEL_MAX_CHILDREN].base), -ENOMEM);

    for (int i = 0; i <= CBX_PANEL_MAX_CHILDREN; i++)
        cbx_widget_destroy(&labels[i].base);
    cbx_widget_destroy(&pnl.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_panel_draw(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_panel pnl;
    assert_int_equal(cbx_panel_init(&pnl, &theme), 0);
    SDL_Rect r = {0, 0, 300, 200};
    cbx_widget_set_rect(&pnl.base, &r);

    cbx_label lbl1, lbl2;
    cbx_label_init(&lbl1, "A", 0, &cache, &theme);
    cbx_label_init(&lbl2, "B", 0, &cache, &theme);
    SDL_Rect lr1 = {10, 10, 50, 20};
    SDL_Rect lr2 = {10, 40, 50, 20};
    cbx_widget_set_rect(&lbl1.base, &lr1);
    cbx_widget_set_rect(&lbl2.base, &lr2);
    cbx_panel_add_child(&pnl, &lbl1.base);
    cbx_panel_add_child(&pnl, &lbl2.base);

    SDL_RenderClear(ctx.renderer);
    cbx_widget_draw(&pnl.base, ctx.renderer);   /* draws bg, border, children */
    SDL_RenderPresent(ctx.renderer);

    /* Draw without bg/border. */
    cbx_panel_set_draw_bg(&pnl, false);
    cbx_panel_set_draw_border(&pnl, false);
    cbx_widget_draw(&pnl.base, ctx.renderer);

    /* Hidden child should not be drawn. */
    cbx_widget_set_visible(&lbl2.base, false);
    cbx_widget_draw(&pnl.base, ctx.renderer);

    cbx_widget_destroy(&lbl1.base);
    cbx_widget_destroy(&lbl2.base);
    cbx_widget_destroy(&pnl.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_panel_focus_management(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_panel pnl;
    assert_int_equal(cbx_panel_init(&pnl, &theme), 0);

    cbx_button btn1, btn2, btn3;
    cbx_button_init(&btn1, "1", 0, &cache, &theme, NULL, NULL);
    cbx_button_init(&btn2, "2", 0, &cache, &theme, NULL, NULL);
    cbx_button_init(&btn3, "3", 0, &cache, &theme, NULL, NULL);
    cbx_panel_add_child(&pnl, &btn1.base);
    cbx_panel_add_child(&pnl, &btn2.base);
    cbx_panel_add_child(&pnl, &btn3.base);

    /* focus_first. */
    assert_int_equal(cbx_panel_focus_first(&pnl), 0);
    assert_int_equal(pnl.focused_child, 0);
    assert_true(cbx_widget_is_focused(&btn1.base));

    /* focus_next wraps. */
    assert_int_equal(cbx_panel_focus_next(&pnl), 1);
    assert_true(cbx_widget_is_focused(&btn2.base));
    assert_false(cbx_widget_is_focused(&btn1.base));

    assert_int_equal(cbx_panel_focus_next(&pnl), 2);
    assert_int_equal(cbx_panel_focus_next(&pnl), 0);   /* wraps */
    assert_true(cbx_widget_is_focused(&btn1.base));

    /* focus_prev wraps. */
    assert_int_equal(cbx_panel_focus_prev(&pnl), 2);
    assert_true(cbx_widget_is_focused(&btn3.base));
    assert_false(cbx_widget_is_focused(&btn1.base));

    /* clear_focus. */
    cbx_panel_clear_focus(&pnl);
    assert_int_equal(pnl.focused_child, -1);
    assert_false(cbx_widget_is_focused(&btn3.base));

    /* Panel focus → auto-focuses first child. */
    cbx_widget_focus(&pnl.base);
    assert_int_equal(pnl.focused_child, 0);
    assert_true(cbx_widget_is_focused(&btn1.base));

    /* Panel blur → blurs focused child. */
    cbx_widget_blur(&pnl.base);
    assert_false(cbx_widget_is_focused(&btn1.base));

    cbx_widget_destroy(&btn1.base);
    cbx_widget_destroy(&btn2.base);
    cbx_widget_destroy(&btn3.base);
    cbx_widget_destroy(&pnl.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_panel_focus_empty(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_panel pnl;
    assert_int_equal(cbx_panel_init(&pnl, &theme), 0);

    assert_int_equal(cbx_panel_focus_first(&pnl), -1);
    assert_int_equal(cbx_panel_focus_next(&pnl), -1);
    assert_int_equal(cbx_panel_focus_prev(&pnl), -1);

    cbx_widget_destroy(&pnl.base);
}

static void
test_panel_event_forwarding(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    int counter = 0;
    cbx_panel pnl;
    assert_int_equal(cbx_panel_init(&pnl, &theme), 0);

    cbx_button btn;
    cbx_button_init(&btn, "Go", 0, &cache, &theme, on_press, &counter);
    cbx_panel_add_child(&pnl, &btn.base);
    cbx_panel_focus_first(&pnl);   /* focus btn */

    /* Event should be forwarded to focused child (btn). */
    SDL_Event ev;
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = SDLK_RETURN;
    assert_true(cbx_widget_handle_event(&pnl.base, &ev));
    ev.type = SDL_KEYUP;
    ev.key.keysym.sym = SDLK_RETURN;
    assert_true(cbx_widget_handle_event(&pnl.base, &ev));
    assert_int_equal(counter, 1);

    /* No focused child → event not consumed. */
    cbx_panel_clear_focus(&pnl);
    assert_false(cbx_widget_handle_event(&pnl.base, &ev));

    cbx_widget_destroy(&btn.base);
    cbx_widget_destroy(&pnl.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_panel_remove_focused(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_text_cache cache;
    assert_int_equal(cbx_text_cache_init(&cache, ctx.renderer), 0);

    cbx_panel pnl;
    assert_int_equal(cbx_panel_init(&pnl, &theme), 0);

    cbx_button btn1, btn2, btn3;
    cbx_button_init(&btn1, "1", 0, &cache, &theme, NULL, NULL);
    cbx_button_init(&btn2, "2", 0, &cache, &theme, NULL, NULL);
    cbx_button_init(&btn3, "3", 0, &cache, &theme, NULL, NULL);
    cbx_panel_add_child(&pnl, &btn1.base);
    cbx_panel_add_child(&pnl, &btn2.base);
    cbx_panel_add_child(&pnl, &btn3.base);

    /* Focus middle child (index 1). */
    cbx_panel_focus_first(&pnl);    /* index 0 */
    cbx_panel_focus_next(&pnl);     /* index 1 */
    assert_int_equal(pnl.focused_child, 1);

    /* Remove focused child. */
    assert_int_equal(cbx_panel_remove_child(&pnl, &btn2.base), 0);
    assert_int_equal(pnl.focused_child, -1);   /* focus cleared */
    assert_int_equal(cbx_panel_child_count(&pnl), 2);
    assert_ptr_equal(cbx_panel_get_child(&pnl, 0), &btn1.base);
    assert_ptr_equal(cbx_panel_get_child(&pnl, 1), &btn3.base);

    cbx_widget_destroy(&btn1.base);
    cbx_widget_destroy(&btn2.base);
    cbx_widget_destroy(&btn3.base);
    cbx_widget_destroy(&pnl.base);
    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void
test_panel_get_child_invalid(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_panel pnl;
    assert_int_equal(cbx_panel_init(&pnl, &theme), 0);

    assert_null(cbx_panel_get_child(&pnl, -1));
    assert_null(cbx_panel_get_child(&pnl, 0));   /* empty */
    assert_null(cbx_panel_get_child(NULL, 0));

    assert_null(cbx_panel_get_focused_child(&pnl));
    assert_null(cbx_panel_get_focused_child(NULL));

    cbx_widget_destroy(&pnl.base);
}

static void
test_panel_set_options(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_panel pnl;
    assert_int_equal(cbx_panel_init(&pnl, &theme), 0);

    cbx_panel_set_padding(&pnl, 16);
    assert_int_equal(pnl.padding, 16);

    cbx_panel_set_draw_bg(&pnl, false);
    assert_false(pnl.draw_bg);

    cbx_panel_set_draw_border(&pnl, false);
    assert_false(pnl.draw_border);

    /* NULL args. */
    cbx_panel_set_padding(NULL, 16);
    cbx_panel_set_draw_bg(NULL, false);
    cbx_panel_set_draw_border(NULL, false);

    cbx_widget_destroy(&pnl.base);
}

/* --- main ---------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* Base dispatchers. */
        cmocka_unit_test(test_widget_draw_null),
        cmocka_unit_test(test_widget_handle_event_null),
        cmocka_unit_test(test_widget_focus_blur_null),
        cmocka_unit_test(test_widget_get_set_rect_null),
        cmocka_unit_test(test_widget_destroy_null),
        cmocka_unit_test(test_widget_is_focused_null),
        cmocka_unit_test(test_widget_is_visible_null),
        cmocka_unit_test(test_widget_set_visible_null),
        cmocka_unit_test(test_widget_accessors),
        /* Button. */
        cmocka_unit_test(test_button_init_basic),
        cmocka_unit_test(test_button_init_null),
        cmocka_unit_test(test_button_draw),
        cmocka_unit_test(test_button_focus_blur),
        cmocka_unit_test(test_button_mouse_press),
        cmocka_unit_test(test_button_mouse_press_outside),
        cmocka_unit_test(test_button_key_press),
        cmocka_unit_test(test_button_no_callback),
        cmocka_unit_test(test_button_set_label),
        cmocka_unit_test(test_button_set_press_cb),
        cmocka_unit_test(test_button_unrelated_event),
        cmocka_unit_test(test_button_with_font),
        /* Label. */
        cmocka_unit_test(test_label_init_basic),
        cmocka_unit_test(test_label_init_null),
        cmocka_unit_test(test_label_draw),
        cmocka_unit_test(test_label_no_event),
        cmocka_unit_test(test_label_set_color),
        cmocka_unit_test(test_label_set_multiline),
        /* Image. */
        cmocka_unit_test(test_image_init_basic),
        cmocka_unit_test(test_image_init_null),
        cmocka_unit_test(test_image_draw_fit),
        cmocka_unit_test(test_image_draw_fill),
        cmocka_unit_test(test_image_draw_center),
        cmocka_unit_test(test_image_no_event),
        cmocka_unit_test(test_image_set_texture),
        cmocka_unit_test(test_image_set_scale_mode),
        cmocka_unit_test(test_image_get_natural_dims_null),
        cmocka_unit_test(test_image_draw_null_texture),
        /* Panel. */
        cmocka_unit_test(test_panel_init_basic),
        cmocka_unit_test(test_panel_init_null),
        cmocka_unit_test(test_panel_add_remove_child),
        cmocka_unit_test(test_panel_add_null),
        cmocka_unit_test(test_panel_add_overflow),
        cmocka_unit_test(test_panel_draw),
        cmocka_unit_test(test_panel_focus_management),
        cmocka_unit_test(test_panel_focus_empty),
        cmocka_unit_test(test_panel_event_forwarding),
        cmocka_unit_test(test_panel_remove_focused),
        cmocka_unit_test(test_panel_get_child_invalid),
        cmocka_unit_test(test_panel_set_options),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}