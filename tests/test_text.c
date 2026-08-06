/*
 * test_text.c — Text rendering cache tests (Task 19).
 *
 * Tests cbx_text_cache_init, font loading, text rendering + caching,
 * colour separation in cache, dimensions, multi-line wrapping,
 * cache clear, cleanup, and NULL-safety.
 * Uses the SDL2 dummy video driver for headless testing.
 *
 * A system TTF font (DejaVuSans.ttf) is located at test time via the
 * CBX_FONT_PATH compile definition.  If the font is not found, font-
 * dependent tests are skipped (cmocka skip).
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ui/text.h"
#include "ui/theme.h"

/* Font path from compile definition, or empty. */
#ifndef CBX_FONT_PATH
#define CBX_FONT_PATH ""
#endif

/* --- Helpers ------------------------------------------------------------- */

static void ensure_dummy_driver(void)
{
    if (getenv("SDL_VIDEODRIVER") == NULL)
        SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "dummy",
                                SDL_HINT_OVERRIDE);
}

/* Test context: keep window + renderer in a struct. */
typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
} TestCtx;

static int test_setup(TestCtx *ctx)
{
    ensure_dummy_driver();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return -1;
    ctx->window = SDL_CreateWindow("test", 0, 0, 320, 240, SDL_WINDOW_HIDDEN);
    if (!ctx->window) { SDL_Quit(); return -1; }
    ctx->renderer = SDL_CreateRenderer(ctx->window, -1, SDL_RENDERER_SOFTWARE);
    if (!ctx->renderer) {
        SDL_DestroyWindow(ctx->window);
        SDL_Quit();
        return -1;
    }
    return 0;
}

static void test_teardown(TestCtx *ctx)
{
    if (ctx->renderer) SDL_DestroyRenderer(ctx->renderer);
    if (ctx->window) SDL_DestroyWindow(ctx->window);
    SDL_Quit();
}

static bool font_available(void)
{
    return CBX_FONT_PATH[0] != '\0' && access(CBX_FONT_PATH, R_OK) == 0;
}

/* --- Text cache tests ---------------------------------------------------- */

static void test_init_basic(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    cbx_text_cache cache;
    int rc = cbx_text_cache_init(&cache, ctx.renderer);
    assert_int_equal(rc, 0);
    assert_int_equal(cache.font_count, 0);
    assert_int_equal(cache.entry_count, 0);

    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void test_init_null(void **state)
{
    (void)state;
    int rc = cbx_text_cache_init(NULL, NULL);
    assert_int_equal(rc, -EINVAL);
}

static void test_load_font(void **state)
{
    (void)state;
    if (!font_available()) { skip(); return; }

    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    cbx_text_cache cache;
    cbx_text_cache_init(&cache, ctx.renderer);
    int id = cbx_text_load_font(&cache, CBX_FONT_PATH, 16);
    assert_int_equal(id, 0);
    assert_int_equal(cache.font_count, 1);

    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void test_load_font_null(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    cbx_text_cache cache;
    cbx_text_cache_init(&cache, ctx.renderer);

    int rc = cbx_text_load_font(&cache, NULL, 16);
    assert_int_equal(rc, -EINVAL);

    rc = cbx_text_load_font(&cache, CBX_FONT_PATH, 0);
    assert_int_equal(rc, -EINVAL);

    rc = cbx_text_load_font(NULL, CBX_FONT_PATH, 16);
    assert_int_equal(rc, -EINVAL);

    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void test_load_nonexistent_font(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    cbx_text_cache cache;
    cbx_text_cache_init(&cache, ctx.renderer);

    int rc = cbx_text_load_font(&cache, "/nonexistent/font.ttf", 16);
    assert_int_equal(rc, -EIO);

    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void test_default_font(void **state)
{
    (void)state;
    if (!font_available()) { skip(); return; }

    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    cbx_text_cache cache;
    cbx_text_cache_init(&cache, ctx.renderer);

    /* No fonts loaded → -1. */
    int id = cbx_text_default_font(&cache);
    assert_int_equal(id, -1);

    cbx_text_load_font(&cache, CBX_FONT_PATH, 16);
    id = cbx_text_default_font(&cache);
    assert_int_equal(id, 0);

    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void test_render_basic(void **state)
{
    (void)state;
    if (!font_available()) { skip(); return; }

    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    cbx_text_cache cache;
    cbx_text_cache_init(&cache, ctx.renderer);
    int font = cbx_text_load_font(&cache, CBX_FONT_PATH, 16);
    assert_int_equal(font, 0);

    SDL_Color white = {255, 255, 255, 255};
    SDL_Texture *tex = cbx_text_render(&cache, font, "Hello", white);
    assert_non_null(tex);
    assert_int_equal(cache.entry_count, 1);

    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void test_render_cached(void **state)
{
    (void)state;
    if (!font_available()) { skip(); return; }

    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    cbx_text_cache cache;
    cbx_text_cache_init(&cache, ctx.renderer);
    int font = cbx_text_load_font(&cache, CBX_FONT_PATH, 16);

    SDL_Color white = {255, 255, 255, 255};
    SDL_Texture *tex1 = cbx_text_render(&cache, font, "Test", white);
    assert_non_null(tex1);

    /* Second render should return the same texture (cached). */
    SDL_Texture *tex2 = cbx_text_render(&cache, font, "Test", white);
    assert_non_null(tex2);
    assert_ptr_equal(tex1, tex2);
    assert_int_equal(cache.entry_count, 1);

    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void test_render_different_colors(void **state)
{
    (void)state;
    if (!font_available()) { skip(); return; }

    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    cbx_text_cache cache;
    cbx_text_cache_init(&cache, ctx.renderer);
    int font = cbx_text_load_font(&cache, CBX_FONT_PATH, 16);

    SDL_Color white = {255, 255, 255, 255};
    SDL_Color red = {255, 0, 0, 255};

    SDL_Texture *tex1 = cbx_text_render(&cache, font, "Hello", white);
    SDL_Texture *tex2 = cbx_text_render(&cache, font, "Hello", red);
    assert_non_null(tex1);
    assert_non_null(tex2);
    /* Different colours → different cache entries → different textures. */
    assert_ptr_not_equal(tex1, tex2);
    assert_int_equal(cache.entry_count, 2);

    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void test_render_different_text(void **state)
{
    (void)state;
    if (!font_available()) { skip(); return; }

    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    cbx_text_cache cache;
    cbx_text_cache_init(&cache, ctx.renderer);
    int font = cbx_text_load_font(&cache, CBX_FONT_PATH, 16);

    SDL_Color white = {255, 255, 255, 255};
    SDL_Texture *tex1 = cbx_text_render(&cache, font, "Hello", white);
    SDL_Texture *tex2 = cbx_text_render(&cache, font, "World", white);
    assert_ptr_not_equal(tex1, tex2);
    assert_int_equal(cache.entry_count, 2);

    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void test_render_different_fonts(void **state)
{
    (void)state;
    if (!font_available()) { skip(); return; }

    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    cbx_text_cache cache;
    cbx_text_cache_init(&cache, ctx.renderer);
    int font1 = cbx_text_load_font(&cache, CBX_FONT_PATH, 16);
    int font2 = cbx_text_load_font(&cache, CBX_FONT_PATH, 24);
    assert_int_equal(font1, 0);
    assert_int_equal(font2, 1);

    SDL_Color white = {255, 255, 255, 255};
    SDL_Texture *tex1 = cbx_text_render(&cache, font1, "Hi", white);
    SDL_Texture *tex2 = cbx_text_render(&cache, font2, "Hi", white);
    assert_ptr_not_equal(tex1, tex2);
    assert_int_equal(cache.entry_count, 2);

    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void test_render_null_args(void **state)
{
    (void)state;
    if (!font_available()) { skip(); return; }

    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    cbx_text_cache cache;
    cbx_text_cache_init(&cache, ctx.renderer);
    int font = cbx_text_load_font(&cache, CBX_FONT_PATH, 16);

    SDL_Color white = {255, 255, 255, 255};
    assert_null(cbx_text_render(NULL, font, "Hi", white));
    assert_null(cbx_text_render(&cache, font, NULL, white));
    assert_null(cbx_text_render(&cache, font, "", white));
    assert_null(cbx_text_render(&cache, -1, "Hi", white));
    assert_null(cbx_text_render(&cache, 99, "Hi", white));

    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void test_get_dims(void **state)
{
    (void)state;
    if (!font_available()) { skip(); return; }

    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    cbx_text_cache cache;
    cbx_text_cache_init(&cache, ctx.renderer);
    int font = cbx_text_load_font(&cache, CBX_FONT_PATH, 16);

    SDL_Color white = {255, 255, 255, 255};
    cbx_text_render(&cache, font, "Hello", white);

    int w = 0, h = 0;
    int rc = cbx_text_get_dims(&cache, font, "Hello", white, &w, &h);
    assert_int_equal(rc, 0);
    assert_true(w > 0);
    assert_true(h > 0);
}

static void test_get_dims_not_cached(void **state)
{
    (void)state;
    if (!font_available()) { skip(); return; }

    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    cbx_text_cache cache;
    cbx_text_cache_init(&cache, ctx.renderer);
    int font = cbx_text_load_font(&cache, CBX_FONT_PATH, 16);

    SDL_Color white = {255, 255, 255, 255};
    int rc = cbx_text_get_dims(&cache, font, "NotCached", white, NULL, NULL);
    assert_int_equal(rc, -ENOENT);

    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void test_get_dims_null(void **state)
{
    (void)state;
    SDL_Color white = {255, 255, 255, 255};
    int rc = cbx_text_get_dims(NULL, 0, "Hi", white, NULL, NULL);
    assert_int_equal(rc, -EINVAL);
}

static void test_measure(void **state)
{
    (void)state;
    if (!font_available()) { skip(); return; }

    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    cbx_text_cache cache;
    cbx_text_cache_init(&cache, ctx.renderer);
    int font = cbx_text_load_font(&cache, CBX_FONT_PATH, 16);

    int w = 0, h = 0;
    int rc = cbx_text_measure(&cache, font, "Hello World", &w, &h);
    assert_int_equal(rc, 0);
    assert_true(w > 0);
    assert_true(h > 0);

    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void test_measure_null(void **state)
{
    (void)state;
    int rc = cbx_text_measure(NULL, 0, "Hi", NULL, NULL);
    assert_int_equal(rc, -EINVAL);
}

static void test_line_height(void **state)
{
    (void)state;
    if (!font_available()) { skip(); return; }

    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    cbx_text_cache cache;
    cbx_text_cache_init(&cache, ctx.renderer);
    int font = cbx_text_load_font(&cache, CBX_FONT_PATH, 16);

    int lh = cbx_text_line_height(&cache, font);
    assert_true(lh > 0);

    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void test_line_height_invalid_font(void **state)
{
    (void)state;
    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    cbx_text_cache cache;
    cbx_text_cache_init(&cache, ctx.renderer);

    int lh = cbx_text_line_height(&cache, -1);
    assert_int_equal(lh, 0);

    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void test_render_wrapped_basic(void **state)
{
    (void)state;
    if (!font_available()) { skip(); return; }

    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    cbx_text_cache cache;
    cbx_text_cache_init(&cache, ctx.renderer);
    int font = cbx_text_load_font(&cache, CBX_FONT_PATH, 16);

    SDL_Color white = {255, 255, 255, 255};

    /* No wrapping needed — single line. */
    SDL_Texture **lines = NULL;
    int count = 0, total_h = 0;
    int rc = cbx_text_render_wrapped(&cache, font, "Short", white, 0,
                                     &lines, &count, &total_h);
    assert_int_equal(rc, 0);
    assert_int_equal(count, 1);
    assert_non_null(lines[0]);
    assert_true(total_h > 0);
    free(lines);

    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void test_render_wrapped_multiline(void **state)
{
    (void)state;
    if (!font_available()) { skip(); return; }

    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    cbx_text_cache cache;
    cbx_text_cache_init(&cache, ctx.renderer);
    int font = cbx_text_load_font(&cache, CBX_FONT_PATH, 16);

    SDL_Color white = {255, 255, 255, 255};

    /* Two lines separated by newline. */
    SDL_Texture **lines = NULL;
    int count = 0, total_h = 0;
    int rc = cbx_text_render_wrapped(&cache, font, "Line 1\nLine 2",
                                     white, 0, &lines, &count, &total_h);
    assert_int_equal(rc, 0);
    assert_int_equal(count, 2);
    assert_true(total_h > 0);
    free(lines);

    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void test_render_wrapped_word_wrap(void **state)
{
    (void)state;
    if (!font_available()) { skip(); return; }

    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    cbx_text_cache cache;
    cbx_text_cache_init(&cache, ctx.renderer);
    int font = cbx_text_load_font(&cache, CBX_FONT_PATH, 16);

    SDL_Color white = {255, 255, 255, 255};

    /* Measure a single character to determine a small wrap width. */
    int char_w = 0, char_h = 0;
    cbx_text_measure(&cache, font, "W", &char_w, &char_h);
    /* Wrap width = ~3 characters. */
    int wrap_w = char_w * 3;

    SDL_Texture **lines = NULL;
    int count = 0, total_h = 0;
    int rc = cbx_text_render_wrapped(&cache, font, "Hello World Test",
                                     white, wrap_w, &lines, &count, &total_h);
    assert_int_equal(rc, 0);
    assert_true(count >= 2);  /* Should wrap into multiple lines */
    free(lines);

    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void test_render_wrapped_null(void **state)
{
    (void)state;
    SDL_Color white = {255, 255, 255, 255};
    SDL_Texture **lines = NULL;
    int count = 0;
    int rc = cbx_text_render_wrapped(NULL, 0, "Hi", white, 0,
                                     &lines, &count, NULL);
    assert_int_equal(rc, -EINVAL);
}

static void test_cache_clear(void **state)
{
    (void)state;
    if (!font_available()) { skip(); return; }

    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    cbx_text_cache cache;
    cbx_text_cache_init(&cache, ctx.renderer);
    int font = cbx_text_load_font(&cache, CBX_FONT_PATH, 16);

    SDL_Color white = {255, 255, 255, 255};
    cbx_text_render(&cache, font, "One", white);
    cbx_text_render(&cache, font, "Two", white);
    assert_int_equal(cache.entry_count, 2);

    cbx_text_cache_clear(&cache);
    assert_int_equal(cache.entry_count, 0);

    /* Fonts should still be loaded. */
    assert_int_equal(cache.font_count, 1);

    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void test_cleanup(void **state)
{
    (void)state;
    if (!font_available()) { skip(); return; }

    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    cbx_text_cache cache;
    cbx_text_cache_init(&cache, ctx.renderer);
    int font = cbx_text_load_font(&cache, CBX_FONT_PATH, 16);

    SDL_Color white = {255, 255, 255, 255};
    cbx_text_render(&cache, font, "Text", white);

    cbx_text_cache_cleanup(&cache);
    assert_int_equal(cache.font_count, 0);
    assert_int_equal(cache.entry_count, 0);
    assert_null(cache.renderer);

    test_teardown(&ctx);
}

static void test_cleanup_null(void **state)
{
    (void)state;
    /* Should not crash. */
    cbx_text_cache_cleanup(NULL);
}

static void test_render_long_text(void **state)
{
    (void)state;
    if (!font_available()) { skip(); return; }

    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    cbx_text_cache cache;
    cbx_text_cache_init(&cache, ctx.renderer);
    int font = cbx_text_load_font(&cache, CBX_FONT_PATH, 16);

    SDL_Color white = {255, 255, 255, 255};
    /* Text longer than CBX_TEXT_MAX_LEN should still render (just not cached). */
    char long_text[CBX_TEXT_MAX_LEN + 64];
    memset(long_text, 'A', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = '\0';

    SDL_Texture *tex = cbx_text_render(&cache, font, long_text, white);
    assert_non_null(tex);
    /* Should not be cached (exceeds CBX_TEXT_MAX_LEN). */
    assert_int_equal(cache.entry_count, 0);

    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

static void test_render_multiple_fonts(void **state)
{
    (void)state;
    if (!font_available()) { skip(); return; }

    TestCtx ctx;
    assert_int_equal(test_setup(&ctx), 0);

    cbx_text_cache cache;
    cbx_text_cache_init(&cache, ctx.renderer);

    int font16 = cbx_text_load_font(&cache, CBX_FONT_PATH, 16);
    int font24 = cbx_text_load_font(&cache, CBX_FONT_PATH, 24);
    int font32 = cbx_text_load_font(&cache, CBX_FONT_PATH, 32);
    assert_int_equal(font16, 0);
    assert_int_equal(font24, 1);
    assert_int_equal(font32, 2);
    assert_int_equal(cache.font_count, 3);

    SDL_Color white = {255, 255, 255, 255};
    SDL_Texture *t1 = cbx_text_render(&cache, font16, "Hi", white);
    SDL_Texture *t2 = cbx_text_render(&cache, font24, "Hi", white);
    SDL_Texture *t3 = cbx_text_render(&cache, font32, "Hi", white);
    assert_non_null(t1);
    assert_non_null(t2);
    assert_non_null(t3);
    assert_ptr_not_equal(t1, t2);
    assert_ptr_not_equal(t2, t3);
    assert_int_equal(cache.entry_count, 3);

    cbx_text_cache_cleanup(&cache);
    test_teardown(&ctx);
}

/* --- Theme tests --------------------------------------------------------- */

static void test_theme_default(void **state)
{
    (void)state;
    cbx_theme t;
    cbx_theme_default(&t);

    /* Check some expected values. */
    assert_int_equal(t.bg.r, 18);
    assert_int_equal(t.bg.a, 255);
    assert_int_equal(t.text_primary.r, 240);
    assert_int_equal(t.icon_tint.r, 255);
}

static void test_theme_apply_opacity(void **state)
{
    (void)state;
    cbx_theme t;
    cbx_theme_default(&t);

    cbx_theme_apply_opacity(&t, 0.5);
    assert_int_equal(t.overlay_bg.a, 128);  /* 0.5 * 255 = 127.5 → 128 */

    cbx_theme_apply_opacity(&t, 0.0);
    assert_int_equal(t.overlay_bg.a, 0);

    cbx_theme_apply_opacity(&t, 1.0);
    assert_int_equal(t.overlay_bg.a, 255);

    cbx_theme_apply_opacity(&t, -1.0);
    assert_int_equal(t.overlay_bg.a, 0);

    cbx_theme_apply_opacity(&t, 2.0);
    assert_int_equal(t.overlay_bg.a, 255);
}

static void test_theme_apply_opacity_null(void **state)
{
    (void)state;
    /* Should not crash. */
    cbx_theme_apply_opacity(NULL, 0.5);
}

static void test_theme_load(void **state)
{
    (void)state;
    cbx_settings s;
    cbx_settings_defaults(&s);
    /* overlay_opacity default = 0.85 → alpha = 217 */
    cbx_theme t;
    int rc = cbx_theme_load(&t, &s);
    assert_int_equal(rc, 0);
    assert_int_equal(t.overlay_bg.a, 217);
}

static void test_theme_load_null(void **state)
{
    (void)state;
    int rc = cbx_theme_load(NULL, NULL);
    assert_int_equal(rc, -EINVAL);

    cbx_settings s;
    cbx_settings_defaults(&s);
    rc = cbx_theme_load(NULL, &s);
    assert_int_equal(rc, -EINVAL);

    cbx_theme t;
    rc = cbx_theme_load(&t, NULL);
    assert_int_equal(rc, -EINVAL);
}

static void test_theme_is_known(void **state)
{
    (void)state;
    assert_true(cbx_theme_is_known("default"));
    assert_false(cbx_theme_is_known("dark"));
    assert_false(cbx_theme_is_known(""));
    assert_false(cbx_theme_is_known(NULL));
}

static const struct CMUnitTest text_tests[] = {
    /* Renderer-adjacent: init */
    cmocka_unit_test(test_init_basic),
    cmocka_unit_test(test_init_null),

    /* Font loading */
    cmocka_unit_test(test_load_font),
    cmocka_unit_test(test_load_font_null),
    cmocka_unit_test(test_load_nonexistent_font),
    cmocka_unit_test(test_default_font),

    /* Text rendering + cache */
    cmocka_unit_test(test_render_basic),
    cmocka_unit_test(test_render_cached),
    cmocka_unit_test(test_render_different_colors),
    cmocka_unit_test(test_render_different_text),
    cmocka_unit_test(test_render_different_fonts),
    cmocka_unit_test(test_render_null_args),
    cmocka_unit_test(test_render_long_text),
    cmocka_unit_test(test_render_multiple_fonts),

    /* Dimensions */
    cmocka_unit_test(test_get_dims),
    cmocka_unit_test(test_get_dims_not_cached),
    cmocka_unit_test(test_get_dims_null),
    cmocka_unit_test(test_measure),
    cmocka_unit_test(test_measure_null),
    cmocka_unit_test(test_line_height),
    cmocka_unit_test(test_line_height_invalid_font),

    /* Multi-line wrapping */
    cmocka_unit_test(test_render_wrapped_basic),
    cmocka_unit_test(test_render_wrapped_multiline),
    cmocka_unit_test(test_render_wrapped_word_wrap),
    cmocka_unit_test(test_render_wrapped_null),

    /* Cache management */
    cmocka_unit_test(test_cache_clear),
    cmocka_unit_test(test_cleanup),
    cmocka_unit_test(test_cleanup_null),

    /* Theme */
    cmocka_unit_test(test_theme_default),
    cmocka_unit_test(test_theme_apply_opacity),
    cmocka_unit_test(test_theme_apply_opacity_null),
    cmocka_unit_test(test_theme_load),
    cmocka_unit_test(test_theme_load_null),
    cmocka_unit_test(test_theme_is_known),
};

int main(void)
{
    return cmocka_run_group_tests(text_tests, NULL, NULL);
}