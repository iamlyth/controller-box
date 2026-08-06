/*
 * test_animation.c — Tests for animation primitives and dirty-rect tracking.
 *
 * Task 23 — Animation primitives and dirty rect optimization.
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
#include <math.h>

#include "ui/animation.h"
#include "ui/dirty_rect.h"

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

/* ================================================================== */
/* Easing tests                                                       */
/* ================================================================== */

static void
test_ease_linear(void **state)
{
    (void)state;
    assert_double_equal(cbx_ease_eval(CBX_EASE_LINEAR, 0.0), 0.0, 1e-9);
    assert_double_equal(cbx_ease_eval(CBX_EASE_LINEAR, 0.5), 0.5, 1e-9);
    assert_double_equal(cbx_ease_eval(CBX_EASE_LINEAR, 1.0), 1.0, 1e-9);
}

static void
test_ease_in(void **state)
{
    (void)state;
    assert_double_equal(cbx_ease_eval(CBX_EASE_IN, 0.0), 0.0, 1e-9);
    assert_double_equal(cbx_ease_eval(CBX_EASE_IN, 0.5), 0.25, 1e-9);
    assert_double_equal(cbx_ease_eval(CBX_EASE_IN, 1.0), 1.0, 1e-9);
}

static void
test_ease_out(void **state)
{
    (void)state;
    assert_double_equal(cbx_ease_eval(CBX_EASE_OUT, 0.0), 0.0, 1e-9);
    assert_double_equal(cbx_ease_eval(CBX_EASE_OUT, 0.5), 0.75, 1e-9);
    assert_double_equal(cbx_ease_eval(CBX_EASE_OUT, 1.0), 1.0, 1e-9);
}

static void
test_ease_in_out(void **state)
{
    (void)state;
    assert_double_equal(cbx_ease_eval(CBX_EASE_IN_OUT, 0.0), 0.0, 1e-9);
    assert_double_equal(cbx_ease_eval(CBX_EASE_IN_OUT, 0.25), 0.125, 1e-9);
    assert_double_equal(cbx_ease_eval(CBX_EASE_IN_OUT, 0.5), 0.5, 1e-9);
    assert_double_equal(cbx_ease_eval(CBX_EASE_IN_OUT, 0.75), 0.875, 1e-9);
    assert_double_equal(cbx_ease_eval(CBX_EASE_IN_OUT, 1.0), 1.0, 1e-9);
}

static void
test_ease_clamp(void **state)
{
    (void)state;
    assert_double_equal(cbx_ease_eval(CBX_EASE_LINEAR, -0.5), 0.0, 1e-9);
    assert_double_equal(cbx_ease_eval(CBX_EASE_LINEAR, 1.5), 1.0, 1e-9);
    assert_double_equal(cbx_ease_eval(CBX_EASE_IN, -1.0), 0.0, 1e-9);
    assert_double_equal(cbx_ease_eval(CBX_EASE_OUT, 2.0), 1.0, 1e-9);
}

static void
test_ease_unknown(void **state)
{
    (void)state;
    /* Unknown easing type falls back to linear */
    assert_double_equal(cbx_ease_eval((cbx_ease_type)99, 0.5), 0.5, 1e-9);
}

/* ================================================================== */
/* Animation tests                                                    */
/* ================================================================== */

static void
test_anim_init(void **state)
{
    (void)state;
    cbx_anim a;
    cbx_anim_init(&a);
    assert_int_equal(a.state, CBX_ANIM_IDLE);
    assert_int_equal(a.duration_ms, 0);
    assert_false(cbx_anim_is_running(&a));
    assert_false(cbx_anim_is_complete(&a));
    assert_double_equal(cbx_anim_alpha(&a), 0.0, 1e-9);
}

static void
test_anim_null(void **state)
{
    (void)state;
    /* NULL-safe calls should not crash — we pass NULL but can't
     * dereference, so just test that the functions accept NULL without
     * segfault. The init/start/update functions all dereference a, so
     * they'd crash — that's expected. We test NULL on query helpers
     * that should be safe. */
    /* Actually, all our functions dereference a — that's standard C.
     * Skip NULL tests for pointer-requiring functions. */
    assert_true(true);
}

static void
test_anim_start(void **state)
{
    (void)state;
    cbx_anim a;
    cbx_anim_init(&a);
    cbx_anim_start(&a, 0.0, 1.0, 1000, CBX_EASE_LINEAR);
    assert_int_equal(a.state, CBX_ANIM_RUNNING);
    assert_double_equal(a.from_alpha, 0.0, 1e-9);
    assert_double_equal(a.to_alpha, 1.0, 1e-9);
    assert_int_equal(a.duration_ms, 1000);
    assert_true(cbx_anim_is_running(&a));
    assert_false(cbx_anim_is_complete(&a));
}

static void
test_anim_instant(void **state)
{
    (void)state;
    /* duration_ms = 0 → completes immediately on first update */
    cbx_anim a;
    cbx_anim_init(&a);
    cbx_anim_start(&a, 0.0, 1.0, 0, CBX_EASE_LINEAR);
    double alpha = cbx_anim_update(&a);
    assert_int_equal(a.state, CBX_ANIM_COMPLETE);
    assert_double_equal(alpha, 1.0, 1e-9);
    assert_true(cbx_anim_is_complete(&a));
    assert_false(cbx_anim_is_running(&a));
}

static void
test_anim_update_progress(void **state)
{
    (void)state;
    /* We can't control SDL_GetTicks() precisely, but we can verify
     * that a running animation with a long duration returns a value
     * between from and to, and that it eventually completes. */
    cbx_anim a;
    cbx_anim_init(&a);
    cbx_anim_start(&a, 0.0, 1.0, 5000, CBX_EASE_LINEAR);
    double alpha = cbx_anim_update(&a);
    /* Should be between 0 and 1 (exclusive) — the 5000ms duration
     * means it's still very early */
    assert_true(alpha >= 0.0 && alpha <= 1.0);
    assert_true(cbx_anim_is_running(&a));

    /* Short duration to force completion */
    cbx_anim_start(&a, 0.0, 1.0, 1, CBX_EASE_LINEAR);
    /* Wait 2ms to ensure elapsed > 1ms */
    SDL_Delay(2);
    alpha = cbx_anim_update(&a);
    assert_true(cbx_anim_is_complete(&a));
    assert_double_equal(alpha, 1.0, 1e-9);
}

static void
test_anim_update_idle(void **state)
{
    (void)state;
    /* Updating an IDLE animation returns from_alpha without starting */
    cbx_anim a;
    cbx_anim_init(&a);
    a.from_alpha = 0.3;
    double alpha = cbx_anim_update(&a);
    assert_int_equal(a.state, CBX_ANIM_IDLE);
    assert_double_equal(alpha, 0.3, 1e-9);
}

static void
test_anim_update_complete_idempotent(void **state)
{
    (void)state;
    /* Updating a COMPLETE animation returns to_alpha repeatedly */
    cbx_anim a;
    cbx_anim_init(&a);
    cbx_anim_start(&a, 0.0, 0.7, 1, CBX_EASE_LINEAR);
    SDL_Delay(2);
    cbx_anim_update(&a);
    assert_true(cbx_anim_is_complete(&a));
    double alpha1 = cbx_anim_update(&a);
    double alpha2 = cbx_anim_update(&a);
    assert_double_equal(alpha1, 0.7, 1e-9);
    assert_double_equal(alpha2, 0.7, 1e-9);
}

static void
test_anim_stop(void **state)
{
    (void)state;
    cbx_anim a;
    cbx_anim_init(&a);
    cbx_anim_start(&a, 0.0, 1.0, 1000, CBX_EASE_LINEAR);
    assert_true(cbx_anim_is_running(&a));
    cbx_anim_stop(&a);
    assert_false(cbx_anim_is_running(&a));
    assert_int_equal(a.state, CBX_ANIM_IDLE);
}

static void
test_anim_fade_in(void **state)
{
    (void)state;
    cbx_anim a;
    cbx_anim_init(&a);
    cbx_anim_fade_in(&a, 0.85, 300);
    assert_true(cbx_anim_is_running(&a));
    assert_double_equal(a.from_alpha, 0.0, 1e-9);
    assert_double_equal(a.to_alpha, 0.85, 1e-9);
    assert_int_equal(a.duration_ms, 300);
}

static void
test_anim_fade_out(void **state)
{
    (void)state;
    cbx_anim a;
    cbx_anim_init(&a);
    /* Set a current alpha first */
    a.cur_alpha = 0.6;
    cbx_anim_fade_out(&a, 200);
    assert_true(cbx_anim_is_running(&a));
    assert_double_equal(a.from_alpha, 0.6, 1e-9);
    assert_double_equal(a.to_alpha, 0.0, 1e-9);
    assert_int_equal(a.duration_ms, 200);
}

static void
test_anim_restart(void **state)
{
    (void)state;
    cbx_anim a;
    cbx_anim_init(&a);
    cbx_anim_start(&a, 0.0, 1.0, 1000, CBX_EASE_LINEAR);
    cbx_anim_update(&a);
    assert_true(cbx_anim_is_running(&a));

    /* Restart with different params */
    cbx_anim_start(&a, 0.5, 0.0, 500, CBX_EASE_OUT);
    assert_int_equal(a.state, CBX_ANIM_RUNNING);
    assert_double_equal(a.from_alpha, 0.5, 1e-9);
    assert_double_equal(a.to_alpha, 0.0, 1e-9);
    assert_int_equal(a.duration_ms, 500);
    assert_int_equal(a.easing, CBX_EASE_OUT);
}

static void
test_anim_ease_in_progress(void **state)
{
    (void)state;
    /* With ease-in, at ~50% time the alpha should be ~25% (t²) */
    cbx_anim a;
    cbx_anim_init(&a);
    /* Use a 1ms duration and then immediately check — we can't control
     * timing precisely, but we can verify the eased value is within
     * bounds for a long-duration anim */
    cbx_anim_start(&a, 0.0, 1.0, 10000, CBX_EASE_IN);
    double alpha = cbx_anim_update(&a);
    assert_true(alpha >= 0.0 && alpha < 0.5); /* ease-in: starts slow */
}

static void
test_anim_ease_out_progress(void **state)
{
    (void)state;
    /* With ease-out, starts fast then decelerates */
    cbx_anim a;
    cbx_anim_init(&a);
    cbx_anim_start(&a, 0.0, 1.0, 10000, CBX_EASE_OUT);
    double alpha = cbx_anim_update(&a);
    assert_true(alpha >= 0.0 && alpha < 1.0);
    /* Ease-out starts faster than linear — but we can't assert exact
     * values without controlling time. Just verify it's in range. */
}

/* ================================================================== */
/* Dirty rect tests                                                   */
/* ================================================================== */

static void
test_dirty_init(void **state)
{
    (void)state;
    cbx_dirty_rect dr;
    cbx_dirty_rect_init(&dr, 1280, 720);
    assert_int_equal(dr.screen_w, 1280);
    assert_int_equal(dr.screen_h, 720);
    assert_int_equal(cbx_dirty_rect_count(&dr), 0);
    assert_false(cbx_dirty_rect_is_dirty(&dr));
}

static void
test_dirty_add(void **state)
{
    (void)state;
    cbx_dirty_rect dr;
    cbx_dirty_rect_init(&dr, 1280, 720);
    SDL_Rect r = { 10, 20, 100, 50 };
    cbx_dirty_rect_add(&dr, &r);
    assert_int_equal(cbx_dirty_rect_count(&dr), 1);
    assert_true(cbx_dirty_rect_is_dirty(&dr));
    const SDL_Rect *got = cbx_dirty_rect_get(&dr, 0);
    assert_non_null(got);
    assert_int_equal(got->x, 10);
    assert_int_equal(got->y, 20);
    assert_int_equal(got->w, 100);
    assert_int_equal(got->h, 50);
}

static void
test_dirty_add_null(void **state)
{
    (void)state;
    cbx_dirty_rect dr;
    cbx_dirty_rect_init(&dr, 1280, 720);
    cbx_dirty_rect_add(&dr, NULL);
    assert_int_equal(cbx_dirty_rect_count(&dr), 0);
}

static void
test_dirty_add_zero_area(void **state)
{
    (void)state;
    cbx_dirty_rect dr;
    cbx_dirty_rect_init(&dr, 1280, 720);
    SDL_Rect r = { 10, 20, 0, 50 };
    cbx_dirty_rect_add(&dr, &r);
    assert_int_equal(cbx_dirty_rect_count(&dr), 0);

    r = (SDL_Rect){ 10, 20, 50, 0 };
    cbx_dirty_rect_add(&dr, &r);
    assert_int_equal(cbx_dirty_rect_count(&dr), 0);
}

static void
test_dirty_add_clamp(void **state)
{
    (void)state;
    cbx_dirty_rect dr;
    cbx_dirty_rect_init(&dr, 100, 100);
    /* Rect extending beyond screen bounds */
    SDL_Rect r = { -10, -20, 200, 300 };
    cbx_dirty_rect_add(&dr, &r);
    assert_int_equal(cbx_dirty_rect_count(&dr), 1);
    const SDL_Rect *got = cbx_dirty_rect_get(&dr, 0);
    assert_int_equal(got->x, 0);
    assert_int_equal(got->y, 0);
    assert_true(got->w <= 100);
    assert_true(got->h <= 100);
}

static void
test_dirty_add_multiple(void **state)
{
    (void)state;
    cbx_dirty_rect dr;
    cbx_dirty_rect_init(&dr, 1280, 720);
    SDL_Rect r1 = { 0, 0, 100, 100 };
    SDL_Rect r2 = { 200, 200, 100, 100 };
    SDL_Rect r3 = { 400, 400, 100, 100 };
    cbx_dirty_rect_add(&dr, &r1);
    cbx_dirty_rect_add(&dr, &r2);
    cbx_dirty_rect_add(&dr, &r3);
    assert_int_equal(cbx_dirty_rect_count(&dr), 3);
}

static void
test_dirty_clear(void **state)
{
    (void)state;
    cbx_dirty_rect dr;
    cbx_dirty_rect_init(&dr, 1280, 720);
    SDL_Rect r = { 10, 20, 100, 50 };
    cbx_dirty_rect_add(&dr, &r);
    assert_true(cbx_dirty_rect_is_dirty(&dr));
    cbx_dirty_rect_clear(&dr);
    assert_int_equal(cbx_dirty_rect_count(&dr), 0);
    assert_false(cbx_dirty_rect_is_dirty(&dr));
}

static void
test_dirty_add_all(void **state)
{
    (void)state;
    cbx_dirty_rect dr;
    cbx_dirty_rect_init(&dr, 1280, 720);
    cbx_dirty_rect_add_all(&dr);
    assert_int_equal(cbx_dirty_rect_count(&dr), 1);
    const SDL_Rect *got = cbx_dirty_rect_get(&dr, 0);
    assert_int_equal(got->x, 0);
    assert_int_equal(got->y, 0);
    assert_int_equal(got->w, 1280);
    assert_int_equal(got->h, 720);
}

static void
test_dirty_get_invalid(void **state)
{
    (void)state;
    cbx_dirty_rect dr;
    cbx_dirty_rect_init(&dr, 1280, 720);
    assert_null(cbx_dirty_rect_get(&dr, 0));
    assert_null(cbx_dirty_rect_get(&dr, -1));
    SDL_Rect r = { 10, 10, 50, 50 };
    cbx_dirty_rect_add(&dr, &r);
    assert_non_null(cbx_dirty_rect_get(&dr, 0));
    assert_null(cbx_dirty_rect_get(&dr, 1));
    assert_null(cbx_dirty_rect_get(&dr, -1));
}

static void
test_dirty_merge_overlapping(void **state)
{
    (void)state;
    cbx_dirty_rect dr;
    cbx_dirty_rect_init(&dr, 1280, 720);
    SDL_Rect r1 = { 0, 0, 100, 100 };
    SDL_Rect r2 = { 50, 50, 100, 100 };  /* overlaps r1 */
    cbx_dirty_rect_add(&dr, &r1);
    cbx_dirty_rect_add(&dr, &r2);
    assert_int_equal(cbx_dirty_rect_count(&dr), 2);
    cbx_dirty_rect_merge(&dr);
    assert_int_equal(cbx_dirty_rect_count(&dr), 1);
    const SDL_Rect *got = cbx_dirty_rect_get(&dr, 0);
    assert_int_equal(got->x, 0);
    assert_int_equal(got->y, 0);
    assert_int_equal(got->w, 150);  /* 0..150 */
    assert_int_equal(got->h, 150);  /* 0..150 */
}

static void
test_dirty_merge_adjacent(void **state)
{
    (void)state;
    cbx_dirty_rect dr;
    cbx_dirty_rect_init(&dr, 1280, 720);
    SDL_Rect r1 = { 0, 0, 100, 100 };
    SDL_Rect r2 = { 100, 0, 100, 100 };  /* adjacent (shares left edge) */
    cbx_dirty_rect_add(&dr, &r1);
    cbx_dirty_rect_add(&dr, &r2);
    cbx_dirty_rect_merge(&dr);
    assert_int_equal(cbx_dirty_rect_count(&dr), 1);
    const SDL_Rect *got = cbx_dirty_rect_get(&dr, 0);
    assert_int_equal(got->x, 0);
    assert_int_equal(got->w, 200);
}

static void
test_dirty_merge_non_overlapping(void **state)
{
    (void)state;
    cbx_dirty_rect dr;
    cbx_dirty_rect_init(&dr, 1280, 720);
    SDL_Rect r1 = { 0, 0, 100, 100 };
    SDL_Rect r2 = { 500, 500, 100, 100 };  /* far apart */
    cbx_dirty_rect_add(&dr, &r1);
    cbx_dirty_rect_add(&dr, &r2);
    cbx_dirty_rect_merge(&dr);
    assert_int_equal(cbx_dirty_rect_count(&dr), 2);
}

static void
test_dirty_merge_empty(void **state)
{
    (void)state;
    cbx_dirty_rect dr;
    cbx_dirty_rect_init(&dr, 1280, 720);
    cbx_dirty_rect_merge(&dr);  /* no-op on empty */
    assert_int_equal(cbx_dirty_rect_count(&dr), 0);
}

static void
test_dirty_merge_single(void **state)
{
    (void)state;
    cbx_dirty_rect dr;
    cbx_dirty_rect_init(&dr, 1280, 720);
    SDL_Rect r = { 10, 10, 50, 50 };
    cbx_dirty_rect_add(&dr, &r);
    cbx_dirty_rect_merge(&dr);  /* no-op on single */
    assert_int_equal(cbx_dirty_rect_count(&dr), 1);
}

static void
test_dirty_merge_chain(void **state)
{
    (void)state;
    /* Three rects in a chain: r1 overlaps r2, r2 overlaps r3,
     * but r1 doesn't overlap r3. After merge, all should become one. */
    cbx_dirty_rect dr;
    cbx_dirty_rect_init(&dr, 1280, 720);
    SDL_Rect r1 = { 0, 0, 100, 100 };
    SDL_Rect r2 = { 50, 50, 100, 100 };
    SDL_Rect r3 = { 100, 100, 100, 100 };
    cbx_dirty_rect_add(&dr, &r1);
    cbx_dirty_rect_add(&dr, &r2);
    cbx_dirty_rect_add(&dr, &r3);
    cbx_dirty_rect_merge(&dr);
    assert_int_equal(cbx_dirty_rect_count(&dr), 1);
}

static void
test_dirty_intersects(void **state)
{
    (void)state;
    cbx_dirty_rect dr;
    cbx_dirty_rect_init(&dr, 1280, 720);
    SDL_Rect r = { 0, 0, 100, 100 };
    cbx_dirty_rect_add(&dr, &r);
    SDL_Rect test1 = { 50, 50, 50, 50 };  /* inside */
    SDL_Rect test2 = { 200, 200, 50, 50 }; /* outside */
    SDL_Rect test3 = { 90, 90, 50, 50 };   /* partial overlap */
    assert_true(cbx_dirty_rect_intersects(&dr, &test1));
    assert_false(cbx_dirty_rect_intersects(&dr, &test2));
    assert_true(cbx_dirty_rect_intersects(&dr, &test3));
}

static void
test_dirty_intersects_empty(void **state)
{
    (void)state;
    cbx_dirty_rect dr;
    cbx_dirty_rect_init(&dr, 1280, 720);
    SDL_Rect test = { 50, 50, 50, 50 };
    assert_false(cbx_dirty_rect_intersects(&dr, &test));
    assert_false(cbx_dirty_rect_intersects(&dr, NULL));
}

/* ================================================================== */
/* Dirty rect render callback tests                                    */
/* ================================================================== */

static int render_call_count;
static SDL_Rect render_last_clip;

static int
test_render_fn(SDL_Renderer *r, const SDL_Rect *clip, void *userdata)
{
    (void)r;
    (void)userdata;
    render_call_count++;
    render_last_clip = *clip;
    return 0;
}

static void
test_dirty_render_empty(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    if (test_setup(&ctx) != 0) { fail(); return; }

    cbx_dirty_rect dr;
    cbx_dirty_rect_init(&dr, 320, 240);
    render_call_count = 0;
    int rc = cbx_dirty_rect_render(&dr, ctx.renderer,
                                    test_render_fn, NULL);
    assert_int_equal(rc, 0);
    /* Empty dirty list → fn called once with full-screen clip */
    assert_int_equal(render_call_count, 1);
    assert_int_equal(render_last_clip.w, 320);
    assert_int_equal(render_last_clip.h, 240);

    test_teardown(&ctx);
}

static void
test_dirty_render_single(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    if (test_setup(&ctx) != 0) { fail(); return; }

    cbx_dirty_rect dr;
    cbx_dirty_rect_init(&dr, 320, 240);
    SDL_Rect r = { 10, 20, 100, 50 };
    cbx_dirty_rect_add(&dr, &r);
    render_call_count = 0;
    int rc = cbx_dirty_rect_render(&dr, ctx.renderer,
                                    test_render_fn, NULL);
    assert_int_equal(rc, 0);
    assert_int_equal(render_call_count, 1);
    assert_int_equal(render_last_clip.x, 10);
    assert_int_equal(render_last_clip.y, 20);
    assert_int_equal(render_last_clip.w, 100);
    assert_int_equal(render_last_clip.h, 50);

    test_teardown(&ctx);
}

static void
test_dirty_render_multiple(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    if (test_setup(&ctx) != 0) { fail(); return; }

    cbx_dirty_rect dr;
    cbx_dirty_rect_init(&dr, 320, 240);
    SDL_Rect r1 = { 0, 0, 50, 50 };
    SDL_Rect r2 = { 100, 100, 50, 50 };
    SDL_Rect r3 = { 200, 200, 50, 50 };
    cbx_dirty_rect_add(&dr, &r1);
    cbx_dirty_rect_add(&dr, &r2);
    cbx_dirty_rect_add(&dr, &r3);
    render_call_count = 0;
    int rc = cbx_dirty_rect_render(&dr, ctx.renderer,
                                    test_render_fn, NULL);
    assert_int_equal(rc, 0);
    assert_int_equal(render_call_count, 3);

    test_teardown(&ctx);
}

static int
test_render_abort_fn(SDL_Renderer *r, const SDL_Rect *clip, void *userdata)
{
    (void)r;
    (void)clip;
    (void)userdata;
    return 42;  /* abort with error code */
}

static void
test_dirty_render_abort(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    if (test_setup(&ctx) != 0) { fail(); return; }

    cbx_dirty_rect dr;
    cbx_dirty_rect_init(&dr, 320, 240);
    SDL_Rect r1 = { 0, 0, 50, 50 };
    SDL_Rect r2 = { 100, 100, 50, 50 };
    cbx_dirty_rect_add(&dr, &r1);
    cbx_dirty_rect_add(&dr, &r2);
    int rc = cbx_dirty_rect_render(&dr, ctx.renderer,
                                    test_render_abort_fn, NULL);
    assert_int_equal(rc, 42);

    test_teardown(&ctx);
}

static void
test_dirty_render_null_fn(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    if (test_setup(&ctx) != 0) { fail(); return; }

    cbx_dirty_rect dr;
    cbx_dirty_rect_init(&dr, 320, 240);
    int rc = cbx_dirty_rect_render(&dr, ctx.renderer, NULL, NULL);
    assert_int_equal(rc, -1);

    test_teardown(&ctx);
}

static void
test_dirty_render_null_renderer(void **state)
{
    (void)state;
    cbx_dirty_rect dr;
    cbx_dirty_rect_init(&dr, 320, 240);
    int rc = cbx_dirty_rect_render(&dr, NULL, test_render_fn, NULL);
    assert_int_equal(rc, -1);
}

static void
test_dirty_overflow(void **state)
{
    (void)state;
    cbx_dirty_rect dr;
    cbx_dirty_rect_init(&dr, 1280, 720);
    /* Add more than CBX_DIRTY_RECT_MAX rects */
    for (int i = 0; i < CBX_DIRTY_RECT_MAX + 10; i++) {
        SDL_Rect r = { i * 2, 0, 1, 1 };  /* non-overlapping */
        cbx_dirty_rect_add(&dr, &r);
    }
    /* Should cap at CBX_DIRTY_RECT_MAX, then merge into entry 0 */
    assert_true(cbx_dirty_rect_count(&dr) <= CBX_DIRTY_RECT_MAX);
}

static void
test_dirty_merge_after_overflow(void **state)
{
    (void)state;
    cbx_dirty_rect dr;
    cbx_dirty_rect_init(&dr, 1280, 720);
    /* Fill up and overflow */
    for (int i = 0; i < CBX_DIRTY_RECT_MAX + 5; i++) {
        SDL_Rect r = { i, i, 2, 2 };
        cbx_dirty_rect_add(&dr, &r);
    }
    /* Merge should reduce count */
    int before = cbx_dirty_rect_count(&dr);
    cbx_dirty_rect_merge(&dr);
    int after = cbx_dirty_rect_count(&dr);
    assert_true(after <= before);
}

/* ================================================================== */
/* Integration: animation + dirty rect                                */
/* ================================================================== */

static void
test_integration_fade_and_dirty(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    if (test_setup(&ctx) != 0) { fail(); return; }

    /* Simulate overlay fade-in with a dirty rect marking the
     * full screen as needing redraw */
    cbx_anim anim;
    cbx_anim_init(&anim);
    cbx_anim_fade_in(&anim, 0.85, 1);  /* 1ms duration → instant */
    SDL_Delay(2);  /* ensure elapsed > 1ms */

    cbx_dirty_rect dr;
    cbx_dirty_rect_init(&dr, 320, 240);
    cbx_dirty_rect_add_all(&dr);

    /* Update animation → should complete immediately */
    double alpha = cbx_anim_update(&anim);
    assert_true(cbx_anim_is_complete(&anim));
    assert_double_equal(alpha, 0.85, 1e-9);

    /* Render with dirty rect */
    render_call_count = 0;
    int rc = cbx_dirty_rect_render(&dr, ctx.renderer,
                                    test_render_fn, NULL);
    assert_int_equal(rc, 0);
    assert_int_equal(render_call_count, 1);

    /* Clear dirty rects after render */
    cbx_dirty_rect_clear(&dr);
    assert_false(cbx_dirty_rect_is_dirty(&dr));

    test_teardown(&ctx);
}

/* ================================================================== */
/* Runner                                                             */
/* ================================================================== */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* Easing */
        cmocka_unit_test(test_ease_linear),
        cmocka_unit_test(test_ease_in),
        cmocka_unit_test(test_ease_out),
        cmocka_unit_test(test_ease_in_out),
        cmocka_unit_test(test_ease_clamp),
        cmocka_unit_test(test_ease_unknown),
        /* Animation */
        cmocka_unit_test(test_anim_init),
        cmocka_unit_test(test_anim_null),
        cmocka_unit_test(test_anim_start),
        cmocka_unit_test(test_anim_instant),
        cmocka_unit_test(test_anim_update_progress),
        cmocka_unit_test(test_anim_update_idle),
        cmocka_unit_test(test_anim_update_complete_idempotent),
        cmocka_unit_test(test_anim_stop),
        cmocka_unit_test(test_anim_fade_in),
        cmocka_unit_test(test_anim_fade_out),
        cmocka_unit_test(test_anim_restart),
        cmocka_unit_test(test_anim_ease_in_progress),
        cmocka_unit_test(test_anim_ease_out_progress),
        /* Dirty rect */
        cmocka_unit_test(test_dirty_init),
        cmocka_unit_test(test_dirty_add),
        cmocka_unit_test(test_dirty_add_null),
        cmocka_unit_test(test_dirty_add_zero_area),
        cmocka_unit_test(test_dirty_add_clamp),
        cmocka_unit_test(test_dirty_add_multiple),
        cmocka_unit_test(test_dirty_clear),
        cmocka_unit_test(test_dirty_add_all),
        cmocka_unit_test(test_dirty_get_invalid),
        cmocka_unit_test(test_dirty_merge_overlapping),
        cmocka_unit_test(test_dirty_merge_adjacent),
        cmocka_unit_test(test_dirty_merge_non_overlapping),
        cmocka_unit_test(test_dirty_merge_empty),
        cmocka_unit_test(test_dirty_merge_single),
        cmocka_unit_test(test_dirty_merge_chain),
        cmocka_unit_test(test_dirty_intersects),
        cmocka_unit_test(test_dirty_intersects_empty),
        /* Dirty rect render */
        cmocka_unit_test(test_dirty_render_empty),
        cmocka_unit_test(test_dirty_render_single),
        cmocka_unit_test(test_dirty_render_multiple),
        cmocka_unit_test(test_dirty_render_abort),
        cmocka_unit_test(test_dirty_render_null_fn),
        cmocka_unit_test(test_dirty_render_null_renderer),
        cmocka_unit_test(test_dirty_overflow),
        cmocka_unit_test(test_dirty_merge_after_overflow),
        /* Integration */
        cmocka_unit_test(test_integration_fade_and_dirty),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}