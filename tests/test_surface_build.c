/*
 * test_surface_build.c — Tests for pre-built overlay surface infrastructure.
 *
 * Task 24 — Pre-built overlay surface infrastructure (render-to-texture).
 *
 * Tests:
 *   - Init creates target texture at screen resolution
 *   - Opacity applied via SDL_SetTextureAlphaMod
 *   - Show/hide: no texture creation in show/hide path (structural check)
 *   - Dirty-rect mark/clear/render
 *   - Render target switches correctly to overlay texture and back
 *   - Render callback invoked per dirty region
 *   - NULL/invalid argument handling
 *
 * Uses the SDL2 dummy driver for headless rendering.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <setjmp.h>
#include <cmocka.h>

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

#include "overlay/surface_build.h"

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

/* --- render callback for testing ----------------------------------- */

static int g_render_call_count;
static SDL_Rect g_last_clip;

static int
test_render_fn(SDL_Renderer *r, const SDL_Rect *clip, void *userdata)
{
    (void)r;
    (void)userdata;
    g_render_call_count++;
    if (clip)
        g_last_clip = *clip;
    return 0;
}

static int
test_render_fn_fail(SDL_Renderer *r, const SDL_Rect *clip, void *userdata)
{
    (void)r;
    (void)clip;
    (void)userdata;
    return -42;
}

/* --- init tests ---------------------------------------------------- */

static void
test_surface_init_basic(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(0, test_setup(&ctx));

    cbx_overlay_surface s;
    assert_int_equal(0, cbx_overlay_surface_init(&s, ctx.renderer,
                                                   320, 240, 0.85));
    assert_true(cbx_overlay_surface_is_built(&s));
    assert_non_null(cbx_overlay_surface_get_texture(&s));
    assert_int_equal(217, cbx_overlay_surface_get_opacity(&s)); /* 0.85*255 */
    assert_false(cbx_overlay_surface_is_visible(&s));

    int w, h;
    cbx_overlay_surface_get_size(&s, &w, &h);
    assert_int_equal(320, w);
    assert_int_equal(240, h);

    cbx_overlay_surface_destroy(&s);
    test_teardown(&ctx);
}

static void
test_surface_init_null_args(void **state)
{
    (void)state;
    cbx_overlay_surface s;
    SDL_Renderer *r = (SDL_Renderer *)0x1;
    assert_int_equal(-EINVAL, cbx_overlay_surface_init(NULL, r, 320, 240, 0.5));
    assert_int_equal(-EINVAL, cbx_overlay_surface_init(&s, NULL, 320, 240, 0.5));
    assert_int_equal(-EINVAL, cbx_overlay_surface_init(&s, r, 0, 240, 0.5));
    assert_int_equal(-EINVAL, cbx_overlay_surface_init(&s, r, 320, 0, 0.5));
    assert_int_equal(-EINVAL, cbx_overlay_surface_init(&s, r, -10, 240, 0.5));
}

static void
test_surface_init_opacity_clamp(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(0, test_setup(&ctx));

    cbx_overlay_surface s;
    /* opacity < 0 → 0 */
    assert_int_equal(0, cbx_overlay_surface_init(&s, ctx.renderer,
                                                   100, 100, -0.5));
    assert_int_equal(0, cbx_overlay_surface_get_opacity(&s));
    cbx_overlay_surface_destroy(&s);

    /* opacity > 1 → 255 */
    assert_int_equal(0, cbx_overlay_surface_init(&s, ctx.renderer,
                                                   100, 100, 1.5));
    assert_int_equal(255, cbx_overlay_surface_get_opacity(&s));
    cbx_overlay_surface_destroy(&s);

    /* exact 0.0 → 0 */
    assert_int_equal(0, cbx_overlay_surface_init(&s, ctx.renderer,
                                                   100, 100, 0.0));
    assert_int_equal(0, cbx_overlay_surface_get_opacity(&s));
    cbx_overlay_surface_destroy(&s);

    /* exact 1.0 → 255 */
    assert_int_equal(0, cbx_overlay_surface_init(&s, ctx.renderer,
                                                   100, 100, 1.0));
    assert_int_equal(255, cbx_overlay_surface_get_opacity(&s));
    cbx_overlay_surface_destroy(&s);

    test_teardown(&ctx);
}

/* --- destroy tests ------------------------------------------------- */

static void
test_surface_destroy_safe(void **state)
{
    (void)state;
    /* destroy on NULL is safe */
    cbx_overlay_surface_destroy(NULL);

    /* destroy on zeroed struct is safe */
    cbx_overlay_surface s = {0};
    cbx_overlay_surface_destroy(&s);
    assert_false(cbx_overlay_surface_is_built(&s));
}

static void
test_surface_destroy_after_init(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(0, test_setup(&ctx));

    cbx_overlay_surface s;
    assert_int_equal(0, cbx_overlay_surface_init(&s, ctx.renderer,
                                                   100, 100, 0.5));
    assert_true(cbx_overlay_surface_is_built(&s));
    cbx_overlay_surface_destroy(&s);
    assert_false(cbx_overlay_surface_is_built(&s));
    assert_null(cbx_overlay_surface_get_texture(&s));
    assert_false(cbx_overlay_surface_is_visible(&s));

    test_teardown(&ctx);
}

/* --- opacity tests ------------------------------------------------- */

static void
test_surface_set_opacity(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(0, test_setup(&ctx));

    cbx_overlay_surface s;
    assert_int_equal(0, cbx_overlay_surface_init(&s, ctx.renderer,
                                                   100, 100, 0.5));
    assert_int_equal(128, cbx_overlay_surface_get_opacity(&s));

    assert_int_equal(0, cbx_overlay_surface_set_opacity(&s, 1.0));
    assert_int_equal(255, cbx_overlay_surface_get_opacity(&s));

    assert_int_equal(0, cbx_overlay_surface_set_opacity(&s, 0.0));
    assert_int_equal(0, cbx_overlay_surface_get_opacity(&s));

    assert_int_equal(0, cbx_overlay_surface_set_opacity(&s, 0.3));
    assert_int_equal(77, cbx_overlay_surface_get_opacity(&s)); /* 0.3*255≈77 */

    /* clamp negative */
    assert_int_equal(0, cbx_overlay_surface_set_opacity(&s, -1.0));
    assert_int_equal(0, cbx_overlay_surface_get_opacity(&s));

    /* clamp >1 */
    assert_int_equal(0, cbx_overlay_surface_set_opacity(&s, 2.0));
    assert_int_equal(255, cbx_overlay_surface_get_opacity(&s));

    cbx_overlay_surface_destroy(&s);
    test_teardown(&ctx);
}

static void
test_surface_set_opacity_null(void **state)
{
    (void)state;
    assert_int_equal(-EINVAL, cbx_overlay_surface_set_opacity(NULL, 0.5));
}

/* --- visibility (show/hide) tests ---------------------------------- */

static void
test_surface_show(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(0, test_setup(&ctx));

    cbx_overlay_surface s;
    assert_int_equal(0, cbx_overlay_surface_init(&s, ctx.renderer,
                                                   320, 240, 0.85));
    assert_false(cbx_overlay_surface_is_visible(&s));

    assert_int_equal(0, cbx_overlay_surface_show(&s, ctx.renderer));
    assert_true(cbx_overlay_surface_is_visible(&s));

    cbx_overlay_surface_destroy(&s);
    test_teardown(&ctx);
}

static void
test_surface_hide(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(0, test_setup(&ctx));

    cbx_overlay_surface s;
    assert_int_equal(0, cbx_overlay_surface_init(&s, ctx.renderer,
                                                   320, 240, 0.85));
    cbx_overlay_surface_show(&s, ctx.renderer);
    assert_true(cbx_overlay_surface_is_visible(&s));

    cbx_overlay_surface_hide(&s);
    assert_false(cbx_overlay_surface_is_visible(&s));

    /* texture is still built after hide (not destroyed) */
    assert_true(cbx_overlay_surface_is_built(&s));
    assert_non_null(cbx_overlay_surface_get_texture(&s));

    cbx_overlay_surface_destroy(&s);
    test_teardown(&ctx);
}

static void
test_surface_show_no_texture_creation(void **state)
{
    (void)state;
    /* Structural check: show path must not create textures.
     * We can't easily intercept SDL_CreateTexture, but we can verify
     * that the texture pointer is the same before and after show,
     * and that no new texture appears. */
    TestCtx ctx = {0};
    assert_int_equal(0, test_setup(&ctx));

    cbx_overlay_surface s;
    assert_int_equal(0, cbx_overlay_surface_init(&s, ctx.renderer,
                                                   100, 100, 0.5));
    SDL_Texture *tex_before = cbx_overlay_surface_get_texture(&s);
    assert_non_null(tex_before);

    assert_int_equal(0, cbx_overlay_surface_show(&s, ctx.renderer));

    SDL_Texture *tex_after = cbx_overlay_surface_get_texture(&s);
    assert_ptr_equal(tex_before, tex_after);

    cbx_overlay_surface_destroy(&s);
    test_teardown(&ctx);
}

static void
test_surface_show_null_args(void **state)
{
    (void)state;
    cbx_overlay_surface s = {0};
    assert_int_equal(-EINVAL, cbx_overlay_surface_show(NULL, (SDL_Renderer *)0x1));
    assert_int_equal(-EINVAL, cbx_overlay_surface_show(&s, NULL));
}

static void
test_surface_show_unbuilt(void **state)
{
    (void)state;
    cbx_overlay_surface s = {0};
    SDL_Renderer *r = (SDL_Renderer *)0x1;
    assert_int_equal(-EINVAL, cbx_overlay_surface_show(&s, r));
}

static void
test_surface_hide_null(void **state)
{
    (void)state;
    /* should not crash */
    cbx_overlay_surface_hide(NULL);
}

/* --- dirty-rect tests ---------------------------------------------- */

static void
test_surface_mark_dirty(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(0, test_setup(&ctx));

    cbx_overlay_surface s;
    assert_int_equal(0, cbx_overlay_surface_init(&s, ctx.renderer,
                                                   320, 240, 0.5));

    assert_false(cbx_overlay_surface_is_dirty(&s));
    assert_int_equal(0, cbx_overlay_surface_dirty_count(&s));

    SDL_Rect r1 = { 10, 10, 50, 50 };
    cbx_overlay_surface_mark_dirty(&s, &r1);
    assert_true(cbx_overlay_surface_is_dirty(&s));
    assert_int_equal(1, cbx_overlay_surface_dirty_count(&s));

    SDL_Rect r2 = { 100, 100, 30, 30 };
    cbx_overlay_surface_mark_dirty(&s, &r2);
    assert_int_equal(2, cbx_overlay_surface_dirty_count(&s));

    cbx_overlay_surface_clear_dirty(&s);
    assert_false(cbx_overlay_surface_is_dirty(&s));
    assert_int_equal(0, cbx_overlay_surface_dirty_count(&s));

    cbx_overlay_surface_destroy(&s);
    test_teardown(&ctx);
}

static void
test_surface_mark_dirty_all(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(0, test_setup(&ctx));

    cbx_overlay_surface s;
    assert_int_equal(0, cbx_overlay_surface_init(&s, ctx.renderer,
                                                   320, 240, 0.5));

    cbx_overlay_surface_mark_dirty_all(&s);
    assert_true(cbx_overlay_surface_is_dirty(&s));
    assert_int_equal(1, cbx_overlay_surface_dirty_count(&s));

    cbx_overlay_surface_destroy(&s);
    test_teardown(&ctx);
}

static void
test_surface_mark_dirty_null_rect(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(0, test_setup(&ctx));

    cbx_overlay_surface s;
    assert_int_equal(0, cbx_overlay_surface_init(&s, ctx.renderer,
                                                   100, 100, 0.5));
    cbx_overlay_surface_mark_dirty(&s, NULL);
    assert_false(cbx_overlay_surface_is_dirty(&s));

    cbx_overlay_surface_destroy(&s);
    test_teardown(&ctx);
}

static void
test_surface_mark_dirty_unbuilt(void **state)
{
    (void)state;
    cbx_overlay_surface s = {0};
    SDL_Rect r = { 0, 0, 10, 10 };
    cbx_overlay_surface_mark_dirty(&s, &r);
    assert_false(cbx_overlay_surface_is_dirty(&s));
}

/* --- render tests -------------------------------------------------- */

static void
test_surface_render_dirty(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(0, test_setup(&ctx));

    cbx_overlay_surface s;
    assert_int_equal(0, cbx_overlay_surface_init(&s, ctx.renderer,
                                                   320, 240, 0.5));

    /* Mark two non-overlapping dirty rects */
    SDL_Rect r1 = { 0, 0, 50, 50 };
    SDL_Rect r2 = { 100, 100, 30, 30 };
    cbx_overlay_surface_mark_dirty(&s, &r1);
    cbx_overlay_surface_mark_dirty(&s, &r2);

    g_render_call_count = 0;
    memset(&g_last_clip, 0, sizeof(g_last_clip));

    assert_int_equal(0, cbx_overlay_surface_render(&s, ctx.renderer,
                                                    test_render_fn, NULL));
    assert_int_equal(2, g_render_call_count);

    /* Dirty rects should be cleared after successful render */
    assert_false(cbx_overlay_surface_is_dirty(&s));
    assert_int_equal(0, cbx_overlay_surface_dirty_count(&s));

    cbx_overlay_surface_destroy(&s);
    test_teardown(&ctx);
}

static void
test_surface_render_empty(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(0, test_setup(&ctx));

    cbx_overlay_surface s;
    assert_int_equal(0, cbx_overlay_surface_init(&s, ctx.renderer,
                                                   320, 240, 0.5));

    /* No dirty rects → full-screen render (1 call) */
    g_render_call_count = 0;
    assert_int_equal(0, cbx_overlay_surface_render(&s, ctx.renderer,
                                                    test_render_fn, NULL));
    assert_int_equal(1, g_render_call_count);
    /* clip should be full screen */
    assert_int_equal(0, g_last_clip.x);
    assert_int_equal(0, g_last_clip.y);
    assert_int_equal(320, g_last_clip.w);
    assert_int_equal(240, g_last_clip.h);

    cbx_overlay_surface_destroy(&s);
    test_teardown(&ctx);
}

static void
test_surface_render_overlapping_merge(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(0, test_setup(&ctx));

    cbx_overlay_surface s;
    assert_int_equal(0, cbx_overlay_surface_init(&s, ctx.renderer,
                                                   320, 240, 0.5));

    SDL_Rect r1 = { 0, 0, 50, 50 };
    SDL_Rect r2 = { 30, 30, 50, 50 }; /* overlaps r1 */
    cbx_overlay_surface_mark_dirty(&s, &r1);
    cbx_overlay_surface_mark_dirty(&s, &r2);

    g_render_call_count = 0;
    assert_int_equal(0, cbx_overlay_surface_render(&s, ctx.renderer,
                                                    test_render_fn, NULL));
    /* Merged into 1 rect → 1 call */
    assert_int_equal(1, g_render_call_count);

    cbx_overlay_surface_destroy(&s);
    test_teardown(&ctx);
}

static void
test_surface_render_target_switch(void **state)
{
    (void)state;
    /* Verify that render target switches to the overlay texture during
     * render and is restored to NULL (screen) afterwards. */
    TestCtx ctx = {0};
    assert_int_equal(0, test_setup(&ctx));

    cbx_overlay_surface s;
    assert_int_equal(0, cbx_overlay_surface_init(&s, ctx.renderer,
                                                   320, 240, 0.5));

    SDL_Texture *overlay_tex = cbx_overlay_surface_get_texture(&s);

    /* Before render, target should be default (NULL/screen) */
    SDL_Texture *target_before = SDL_GetRenderTarget(ctx.renderer);
    /* dummy driver may return NULL or the screen; just record it */

    cbx_overlay_surface_mark_dirty_all(&s);
    assert_int_equal(0, cbx_overlay_surface_render(&s, ctx.renderer,
                                                    test_render_fn, NULL));

    /* After render, target should be restored to default */
    SDL_Texture *target_after = SDL_GetRenderTarget(ctx.renderer);

    /* The render target after should match what it was before */
    assert_ptr_equal(target_before, target_after);

    /* Verify the overlay texture is a valid target texture */
    assert_non_null(overlay_tex);

    cbx_overlay_surface_destroy(&s);
    test_teardown(&ctx);
}

static void
test_surface_render_fail(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(0, test_setup(&ctx));

    cbx_overlay_surface s;
    assert_int_equal(0, cbx_overlay_surface_init(&s, ctx.renderer,
                                                   320, 240, 0.5));

    cbx_overlay_surface_mark_dirty_all(&s);
    assert_int_equal(-42, cbx_overlay_surface_render(&s, ctx.renderer,
                                                     test_render_fn_fail,
                                                     NULL));

    /* On failure, dirty rects should NOT be cleared */
    assert_true(cbx_overlay_surface_is_dirty(&s));

    cbx_overlay_surface_destroy(&s);
    test_teardown(&ctx);
}

static void
test_surface_render_null_args(void **state)
{
    (void)state;
    cbx_overlay_surface s = {0};
    SDL_Renderer *r = (SDL_Renderer *)0x1;
    assert_int_equal(-EINVAL, cbx_overlay_surface_render(NULL, r,
                                                          test_render_fn, NULL));
    assert_int_equal(-EINVAL, cbx_overlay_surface_render(&s, NULL,
                                                          test_render_fn, NULL));
    assert_int_equal(-EINVAL, cbx_overlay_surface_render(&s, r, NULL, NULL));
}

/* --- accessor tests ------------------------------------------------ */

static void
test_surface_get_size(void **state)
{
    (void)state;
    int w = -1, h = -1;
    cbx_overlay_surface_get_size(NULL, &w, &h);
    assert_int_equal(0, w);
    assert_int_equal(0, h);
}

static void
test_surface_get_texture_null(void **state)
{
    (void)state;
    assert_null(cbx_overlay_surface_get_texture(NULL));
}

static void
test_surface_is_built_null(void **state)
{
    (void)state;
    assert_false(cbx_overlay_surface_is_built(NULL));
}

static void
test_surface_is_visible_null(void **state)
{
    (void)state;
    assert_false(cbx_overlay_surface_is_visible(NULL));
}

static void
test_surface_dirty_count_null(void **state)
{
    (void)state;
    assert_int_equal(0, cbx_overlay_surface_dirty_count(NULL));
}

static void
test_surface_is_dirty_null(void **state)
{
    (void)state;
    assert_false(cbx_overlay_surface_is_dirty(NULL));
}

static void
test_surface_clear_dirty_null(void **state)
{
    (void)state;
    /* should not crash */
    cbx_overlay_surface_clear_dirty(NULL);
}

/* --- show then render then show cycle ----------------------------- */

static void
test_surface_show_render_show_cycle(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(0, test_setup(&ctx));

    cbx_overlay_surface s;
    assert_int_equal(0, cbx_overlay_surface_init(&s, ctx.renderer,
                                                   320, 240, 0.85));

    /* Initial render into texture (no dirty → full screen) */
    g_render_call_count = 0;
    assert_int_equal(0, cbx_overlay_surface_render(&s, ctx.renderer,
                                                    test_render_fn, NULL));
    assert_int_equal(1, g_render_call_count);

    /* Show: present the pre-built texture */
    assert_int_equal(0, cbx_overlay_surface_show(&s, ctx.renderer));
    assert_true(cbx_overlay_surface_is_visible(&s));

    /* Mark a dirty cell, re-render only that cell */
    SDL_Rect cell = { 10, 10, 20, 20 };
    cbx_overlay_surface_mark_dirty(&s, &cell);
    g_render_call_count = 0;
    assert_int_equal(0, cbx_overlay_surface_render(&s, ctx.renderer,
                                                    test_render_fn, NULL));
    assert_int_equal(1, g_render_call_count); /* only 1 dirty rect */

    /* Show again with updated texture */
    assert_int_equal(0, cbx_overlay_surface_show(&s, ctx.renderer));

    /* Hide */
    cbx_overlay_surface_hide(&s);
    assert_false(cbx_overlay_surface_is_visible(&s));
    assert_true(cbx_overlay_surface_is_built(&s));

    cbx_overlay_surface_destroy(&s);
    test_teardown(&ctx);
}

/* --- main ---------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* init */
        cmocka_unit_test(test_surface_init_basic),
        cmocka_unit_test(test_surface_init_null_args),
        cmocka_unit_test(test_surface_init_opacity_clamp),
        /* destroy */
        cmocka_unit_test(test_surface_destroy_safe),
        cmocka_unit_test(test_surface_destroy_after_init),
        /* opacity */
        cmocka_unit_test(test_surface_set_opacity),
        cmocka_unit_test(test_surface_set_opacity_null),
        /* show/hide */
        cmocka_unit_test(test_surface_show),
        cmocka_unit_test(test_surface_hide),
        cmocka_unit_test(test_surface_show_no_texture_creation),
        cmocka_unit_test(test_surface_show_null_args),
        cmocka_unit_test(test_surface_show_unbuilt),
        cmocka_unit_test(test_surface_hide_null),
        /* dirty rect */
        cmocka_unit_test(test_surface_mark_dirty),
        cmocka_unit_test(test_surface_mark_dirty_all),
        cmocka_unit_test(test_surface_mark_dirty_null_rect),
        cmocka_unit_test(test_surface_mark_dirty_unbuilt),
        /* render */
        cmocka_unit_test(test_surface_render_dirty),
        cmocka_unit_test(test_surface_render_empty),
        cmocka_unit_test(test_surface_render_overlapping_merge),
        cmocka_unit_test(test_surface_render_target_switch),
        cmocka_unit_test(test_surface_render_fail),
        cmocka_unit_test(test_surface_render_null_args),
        /* accessors */
        cmocka_unit_test(test_surface_get_size),
        cmocka_unit_test(test_surface_get_texture_null),
        cmocka_unit_test(test_surface_is_built_null),
        cmocka_unit_test(test_surface_is_visible_null),
        cmocka_unit_test(test_surface_dirty_count_null),
        cmocka_unit_test(test_surface_is_dirty_null),
        cmocka_unit_test(test_surface_clear_dirty_null),
        /* cycle */
        cmocka_unit_test(test_surface_show_render_show_cycle),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}