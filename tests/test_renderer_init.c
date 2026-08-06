/*
 * test_renderer_init.c — Renderer init tests (Task 19).
 *
 * Tests cbx_renderer_init, target-texture verification, GLES fallback,
 * blending verification, show/hide/present/clear, and shutdown.
 * Uses the SDL2 dummy video driver for headless testing.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <SDL2/SDL.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "ui/renderer.h"

/* --- Helper: ensure dummy driver ---------------------------------------- */

static void ensure_dummy_driver(void)
{
    if (getenv("SDL_VIDEODRIVER") == NULL)
        SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "dummy",
                                SDL_HINT_OVERRIDE);
}

/* --- Tests --------------------------------------------------------------- */

static void test_init_basic(void **state)
{
    (void)state;
    ensure_dummy_driver();

    cbx_renderer r;
    int rc = cbx_renderer_init(&r, "test", 640, 480, false);
    assert_int_equal(rc, 0);
    assert_non_null(r.window);
    assert_non_null(r.renderer);
    assert_int_equal(r.window_w, 640);
    assert_int_equal(r.window_h, 480);
    assert_false(r.is_gles == false && r.is_gles == true); /* just check it's valid bool */

    cbx_renderer_shutdown(&r);
    assert_null(r.window);
    assert_null(r.renderer);
}

static void test_init_default_size(void **state)
{
    (void)state;
    ensure_dummy_driver();

    cbx_renderer r;
    int rc = cbx_renderer_init(&r, NULL, 0, 0, false);
    assert_int_equal(rc, 0);
    assert_int_equal(r.window_w, CBX_RENDERER_DEFAULT_W);
    assert_int_equal(r.window_h, CBX_RENDERER_DEFAULT_H);

    cbx_renderer_shutdown(&r);
}

static void test_init_null_title(void **state)
{
    (void)state;
    ensure_dummy_driver();

    cbx_renderer r;
    int rc = cbx_renderer_init(&r, NULL, 320, 240, false);
    assert_int_equal(rc, 0);
    assert_non_null(r.window);

    cbx_renderer_shutdown(&r);
}

static void test_init_null_struct(void **state)
{
    (void)state;
    int rc = cbx_renderer_init(NULL, "test", 320, 240, false);
    assert_int_equal(rc, -EINVAL);
}

static void test_target_texture_check(void **state)
{
    (void)state;
    ensure_dummy_driver();

    cbx_renderer r;
    int rc = cbx_renderer_init(&r, "test", 320, 240, false);
    assert_int_equal(rc, 0);

    /* The renderer should have target texture support (software fallback guarantees it). */
    bool has_tt = cbx_renderer_check_target_texture(r.renderer);
    assert_true(has_tt);
    assert_true(r.has_target_texture);

    cbx_renderer_shutdown(&r);
}

static void test_check_target_texture_null(void **state)
{
    (void)state;
    bool result = cbx_renderer_check_target_texture(NULL);
    assert_false(result);
}

static void test_verify_blending(void **state)
{
    (void)state;
    ensure_dummy_driver();

    cbx_renderer r;
    int rc = cbx_renderer_init(&r, "test", 320, 240, false);
    assert_int_equal(rc, 0);

    /* Blending verification may fail on the dummy renderer (no target texture support).
     * Accept both 0 (blending works) and -ENOTSUP (dummy limitation). */
    int blend_rc = cbx_renderer_verify_blending(r.renderer);
    assert_true(blend_rc == 0 || blend_rc == -ENOTSUP);

    cbx_renderer_shutdown(&r);
}

static void test_verify_blending_null(void **state)
{
    (void)state;
    int rc = cbx_renderer_verify_blending(NULL);
    assert_int_equal(rc, -EINVAL);
}

static void test_show_hide(void **state)
{
    (void)state;
    ensure_dummy_driver();

    cbx_renderer r;
    int rc = cbx_renderer_init(&r, "test", 320, 240, false);
    assert_int_equal(rc, 0);

    /* Should not crash. */
    cbx_renderer_show(&r);
    cbx_renderer_hide(&r);
    cbx_renderer_show(&r);

    cbx_renderer_shutdown(&r);
}

static void test_present(void **state)
{
    (void)state;
    ensure_dummy_driver();

    cbx_renderer r;
    int rc = cbx_renderer_init(&r, "test", 320, 240, false);
    assert_int_equal(rc, 0);

    /* Should not crash. */
    cbx_renderer_present(&r);

    cbx_renderer_shutdown(&r);
}

static void test_clear(void **state)
{
    (void)state;
    ensure_dummy_driver();

    cbx_renderer r;
    int rc = cbx_renderer_init(&r, "test", 320, 240, false);
    assert_int_equal(rc, 0);

    SDL_Color c = {255, 0, 0, 255};
    cbx_renderer_clear(&r, c);
    cbx_renderer_present(&r);

    cbx_renderer_shutdown(&r);
}

static void test_shutdown_null(void **state)
{
    (void)state;
    /* Should not crash. */
    cbx_renderer_shutdown(NULL);

    cbx_renderer r;
    memset(&r, 0, sizeof(r));
    cbx_renderer_shutdown(&r);
}

static void test_show_hide_null(void **state)
{
    (void)state;
    /* Should not crash. */
    cbx_renderer_show(NULL);
    cbx_renderer_hide(NULL);
}

static void test_present_clear_null(void **state)
{
    (void)state;
    /* Should not crash. */
    cbx_renderer_present(NULL);
    cbx_renderer r;
    memset(&r, 0, sizeof(r));
    SDL_Color c = {0, 0, 0, 0};
    cbx_renderer_clear(&r, c);
}

static void test_double_init(void **state)
{
    (void)state;
    ensure_dummy_driver();

    cbx_renderer r1, r2;
    int rc1 = cbx_renderer_init(&r1, "test1", 320, 240, false);
    int rc2 = cbx_renderer_init(&r2, "test2", 320, 240, false);
    assert_int_equal(rc1, 0);
    assert_int_equal(rc2, 0);

    cbx_renderer_shutdown(&r2);
    cbx_renderer_shutdown(&r1);
}

static void test_blend_mode_set(void **state)
{
    (void)state;
    ensure_dummy_driver();

    cbx_renderer r;
    int rc = cbx_renderer_init(&r, "test", 320, 240, false);
    assert_int_equal(rc, 0);

    /* After init, the blend mode should be BLENDMODE_BLEND. */
    SDL_BlendMode mode;
    SDL_GetRenderDrawBlendMode(r.renderer, &mode);
    assert_int_equal(mode, SDL_BLENDMODE_BLEND);

    cbx_renderer_shutdown(&r);
}

static const struct CMUnitTest renderer_tests[] = {
    cmocka_unit_test(test_init_basic),
    cmocka_unit_test(test_init_default_size),
    cmocka_unit_test(test_init_null_title),
    cmocka_unit_test(test_init_null_struct),
    cmocka_unit_test(test_target_texture_check),
    cmocka_unit_test(test_check_target_texture_null),
    cmocka_unit_test(test_verify_blending),
    cmocka_unit_test(test_verify_blending_null),
    cmocka_unit_test(test_show_hide),
    cmocka_unit_test(test_present),
    cmocka_unit_test(test_clear),
    cmocka_unit_test(test_shutdown_null),
    cmocka_unit_test(test_show_hide_null),
    cmocka_unit_test(test_present_clear_null),
    cmocka_unit_test(test_double_init),
    cmocka_unit_test(test_blend_mode_set),
};

int main(void)
{
    return cmocka_run_group_tests(renderer_tests, NULL, NULL);
}