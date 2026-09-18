/*
 * test_fb_assert.c — Self-test for framebuffer assertion utilities (Task 1).
 *
 * Renders known colored rectangles via the SDL2 software renderer, reads
 * back pixels, and verifies each fb_assert function behaves correctly:
 *
 *   - fb_read_pixels        — readback succeeds, buffer has expected colors
 *   - fb_region_has_content — detects drawn content vs background
 *   - fb_region_has_color   — finds specific color in a region
 *   - fb_frames_differ      — distinguishes two different renders
 *   - fb_golden_compare     — generates a golden PNG and compares against it
 *   - fb_save_png           — writes a PNG file
 *   - fb_save_diff          — writes a visual diff PNG
 */
#include "fb_assert.h"
#include "test_harness.h"

#include <cmocka.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Helpers ───────────────────────────────────────────────────────── */

static const uint8_t BG_COLOR[3]   = { 10, 20, 30 };
static const uint8_t FG_COLOR[3]   = { 200, 100, 50 };
static const int    TOLERANCE      = 5;
static const int     FB_W           = 320;
static const int     FB_H           = 240;

/*
 * Clear the renderer to bg_color and return.  The test harness creates
 * a 320×240 software renderer.
 */
static void
clear_bg(SDL_Renderer *r)
{
    SDL_SetRenderDrawColor(r, BG_COLOR[0], BG_COLOR[1], BG_COLOR[2], 255);
    SDL_RenderClear(r);
}

/*
 * Fill a rectangle with fg_color.
 */
static void
fill_rect(SDL_Renderer *r, const SDL_Rect *rect)
{
    SDL_SetRenderDrawColor(r, FG_COLOR[0], FG_COLOR[1], FG_COLOR[2], 255);
    SDL_RenderFillRect(r, rect);
}

/* ── Tests ─────────────────────────────────────────────────────────── */

/*
 * fb_read_pixels succeeds and returns the background color in the
 * background region.
 */
static void
test_fb_read_pixels_basic(void **state)
{
    (void)state;
    TestSdlState sdl;
    assert_int_equal(test_harness_sdl_init(&sdl), 0);

    clear_bg(sdl.renderer);
    SDL_RenderPresent(sdl.renderer);

    uint8_t buf[FB_W * FB_H * 4];
    assert_int_equal(fb_read_pixels(sdl.renderer, NULL, buf, sizeof(buf)), 0);

    /* Top-left pixel should be background color. */
    assert_int_in_range(buf[0], BG_COLOR[0] - TOLERANCE, BG_COLOR[0] + TOLERANCE);
    assert_int_in_range(buf[1], BG_COLOR[1] - TOLERANCE, BG_COLOR[1] + TOLERANCE);
    assert_int_in_range(buf[2], BG_COLOR[2] - TOLERANCE, BG_COLOR[2] + TOLERANCE);

    test_harness_sdl_shutdown(&sdl);
}

/*
 * fb_read_pixels with a sub-rect reads only that region.
 */
static void
test_fb_read_pixels_subrect(void **state)
{
    (void)state;
    TestSdlState sdl;
    assert_int_equal(test_harness_sdl_init(&sdl), 0);

    clear_bg(sdl.renderer);
    SDL_Rect r = { 10, 20, 30, 40 };
    fill_rect(sdl.renderer, &r);
    SDL_RenderPresent(sdl.renderer);

    /* Read the full buffer. */
    uint8_t buf[FB_W * FB_H * 4];
    assert_int_equal(fb_read_pixels(sdl.renderer, NULL, buf, sizeof(buf)), 0);

    /* Pixel inside the rect should be fg_color. */
    int idx = (25 * FB_W + 25) * 4;  /* (x=25, y=25) inside the rect */
    assert_int_in_range(buf[idx],     FG_COLOR[0] - TOLERANCE, FG_COLOR[0] + TOLERANCE);
    assert_int_in_range(buf[idx + 1], FG_COLOR[1] - TOLERANCE, FG_COLOR[1] + TOLERANCE);
    assert_int_in_range(buf[idx + 2], FG_COLOR[2] - TOLERANCE, FG_COLOR[2] + TOLERANCE);

    /* Pixel outside the rect should be bg_color. */
    idx = (0 * FB_W + 0) * 4;  /* (0,0) — outside the rect */
    assert_int_in_range(buf[idx],     BG_COLOR[0] - TOLERANCE, BG_COLOR[0] + TOLERANCE);
    assert_int_in_range(buf[idx + 1], BG_COLOR[1] - TOLERANCE, BG_COLOR[1] + TOLERANCE);
    assert_int_in_range(buf[idx + 2], BG_COLOR[2] - TOLERANCE, BG_COLOR[2] + TOLERANCE);

    test_harness_sdl_shutdown(&sdl);
}

/*
 * fb_read_pixels rejects bad arguments.
 */
static void
test_fb_read_pixels_bad_args(void **state)
{
    (void)state;
    TestSdlState sdl;
    assert_int_equal(test_harness_sdl_init(&sdl), 0);

    uint8_t buf[FB_W * FB_H * 4];

    /* NULL renderer. */
    assert_int_equal(fb_read_pixels(NULL, NULL, buf, sizeof(buf)), -1);
    /* NULL buffer. */
    assert_int_equal(fb_read_pixels(sdl.renderer, NULL, NULL, sizeof(buf)), -1);
    /* Buffer too small. */
    assert_int_equal(fb_read_pixels(sdl.renderer, NULL, buf, 16), -1);

    test_harness_sdl_shutdown(&sdl);
}

/*
 * fb_region_has_content returns true for a region with drawn content
 * and false for a background-only region.
 */
static void
test_fb_region_has_content(void **state)
{
    (void)state;
    TestSdlState sdl;
    assert_int_equal(test_harness_sdl_init(&sdl), 0);

    clear_bg(sdl.renderer);
    SDL_Rect r = { 50, 60, 100, 80 };
    fill_rect(sdl.renderer, &r);
    SDL_RenderPresent(sdl.renderer);

    uint8_t buf[FB_W * FB_H * 4];
    assert_int_equal(fb_read_pixels(sdl.renderer, NULL, buf, sizeof(buf)), 0);

    /* Region inside the drawn rect should have content. */
    SDL_Rect content_rect = { 60, 70, 40, 30 };
    assert_true(fb_region_has_content(buf, FB_W, FB_H, &content_rect,
                                       BG_COLOR, TOLERANCE));

    /* Region outside the drawn rect should be background-only. */
    SDL_Rect bg_rect = { 200, 200, 50, 30 };
    assert_false(fb_region_has_content(buf, FB_W, FB_H, &bg_rect,
                                        BG_COLOR, TOLERANCE));

    test_harness_sdl_shutdown(&sdl);
}

/*
 * fb_region_has_color finds the specific drawn color in the right region.
 */
static void
test_fb_region_has_color(void **state)
{
    (void)state;
    TestSdlState sdl;
    assert_int_equal(test_harness_sdl_init(&sdl), 0);

    clear_bg(sdl.renderer);
    SDL_Rect r = { 50, 60, 100, 80 };
    fill_rect(sdl.renderer, &r);
    SDL_RenderPresent(sdl.renderer);

    uint8_t buf[FB_W * FB_H * 4];
    assert_int_equal(fb_read_pixels(sdl.renderer, NULL, buf, sizeof(buf)), 0);

    /* Region inside the drawn rect should contain fg_color. */
    SDL_Rect content_rect = { 60, 70, 40, 30 };
    assert_true(fb_region_has_color(buf, FB_W, FB_H, &content_rect,
                                    FG_COLOR, TOLERANCE));

    /* Region outside should NOT contain fg_color. */
    SDL_Rect bg_rect = { 200, 200, 50, 30 };
    assert_false(fb_region_has_color(buf, FB_W, FB_H, &bg_rect,
                                     FG_COLOR, TOLERANCE));

    test_harness_sdl_shutdown(&sdl);
}

/*
 * fb_frames_differ distinguishes two different renders and reports
 * identical frames as not differing.
 */
static void
test_fb_frames_differ(void **state)
{
    (void)state;
    TestSdlState sdl;
    assert_int_equal(test_harness_sdl_init(&sdl), 0);

    /* Frame A: background only. */
    clear_bg(sdl.renderer);
    SDL_RenderPresent(sdl.renderer);
    uint8_t buf_a[FB_W * FB_H * 4];
    assert_int_equal(fb_read_pixels(sdl.renderer, NULL, buf_a, sizeof(buf_a)), 0);

    /* Frame B: background + a rect. */
    clear_bg(sdl.renderer);
    SDL_Rect r = { 0, 0, 200, 200 };
    fill_rect(sdl.renderer, &r);
    SDL_RenderPresent(sdl.renderer);
    uint8_t buf_b[FB_W * FB_H * 4];
    assert_int_equal(fb_read_pixels(sdl.renderer, NULL, buf_b, sizeof(buf_b)), 0);

    /* The two frames should differ (200×200 rect in a 320×240 window
     * is ~52% of pixels, well above a 5% threshold). */
    assert_true(fb_frames_differ(buf_a, buf_b, FB_W, FB_H, 5));

    /* Identical frames should not differ. */
    assert_false(fb_frames_differ(buf_a, buf_a, FB_W, FB_H, 5));

    test_harness_sdl_shutdown(&sdl);
}

/*
 * fb_save_png writes a valid PNG file.
 */
static void
test_fb_save_png(void **state)
{
    (void)state;
    TestSdlState sdl;
    assert_int_equal(test_harness_sdl_init(&sdl), 0);

    clear_bg(sdl.renderer);
    SDL_Rect r = { 40, 50, 60, 70 };
    fill_rect(sdl.renderer, &r);
    SDL_RenderPresent(sdl.renderer);

    uint8_t buf[FB_W * FB_H * 4];
    assert_int_equal(fb_read_pixels(sdl.renderer, NULL, buf, sizeof(buf)), 0);

    const char *path = "/tmp/cbx_test_fb_save.png";
    assert_int_equal(fb_save_png(buf, FB_W, FB_H, path), 0);

    /* Verify the file exists and is non-empty. */
    FILE *f = fopen(path, "rb");
    assert_non_null(f);
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        assert_true(sz > 0);
        unlink(path);
    }

    test_harness_sdl_shutdown(&sdl);
}

/*
 * fb_golden_compare: generate a golden PNG from a render, then compare
 * the same render against it — should pass.  A different render should
 * fail.
 */
static void
test_fb_golden_compare(void **state)
{
    (void)state;
    TestSdlState sdl;
    assert_int_equal(test_harness_sdl_init(&sdl), 0);

    /* Render a known scene. */
    clear_bg(sdl.renderer);
    SDL_Rect r = { 40, 50, 60, 70 };
    fill_rect(sdl.renderer, &r);
    SDL_RenderPresent(sdl.renderer);

    uint8_t buf[FB_W * FB_H * 4];
    assert_int_equal(fb_read_pixels(sdl.renderer, NULL, buf, sizeof(buf)), 0);

    /* Save as golden baseline. */
    const char *golden_path = "/tmp/cbx_test_golden.png";
    assert_int_equal(fb_save_png(buf, FB_W, FB_H, golden_path), 0);

    /* Compare the same buffer against the golden — should pass. */
    assert_true(fb_golden_compare(buf, FB_W, FB_H, golden_path, 3, 2));

    /* Now render a different scene and compare — should fail. */
    clear_bg(sdl.renderer);
    SDL_RenderPresent(sdl.renderer);
    uint8_t buf2[FB_W * FB_H * 4];
    assert_int_equal(fb_read_pixels(sdl.renderer, NULL, buf2, sizeof(buf2)), 0);
    assert_false(fb_golden_compare(buf2, FB_W, FB_H, golden_path, 3, 2));

    /* Clean up. */
    unlink(golden_path);

    test_harness_sdl_shutdown(&sdl);
}

/*
 * fb_save_diff writes a valid diff PNG between two frames.
 */
static void
test_fb_save_diff(void **state)
{
    (void)state;
    TestSdlState sdl;
    assert_int_equal(test_harness_sdl_init(&sdl), 0);

    /* Frame A: background only. */
    clear_bg(sdl.renderer);
    SDL_RenderPresent(sdl.renderer);
    uint8_t buf_a[FB_W * FB_H * 4];
    assert_int_equal(fb_read_pixels(sdl.renderer, NULL, buf_a, sizeof(buf_a)), 0);

    /* Frame B: background + rect. */
    clear_bg(sdl.renderer);
    SDL_Rect r = { 10, 10, 50, 50 };
    fill_rect(sdl.renderer, &r);
    SDL_RenderPresent(sdl.renderer);
    uint8_t buf_b[FB_W * FB_H * 4];
    assert_int_equal(fb_read_pixels(sdl.renderer, NULL, buf_b, sizeof(buf_b)), 0);

    const char *diff_path = "/tmp/cbx_test_diff.png";
    assert_int_equal(fb_save_diff(buf_a, buf_b, FB_W, FB_H, diff_path), 0);

    /* Verify the file exists and is non-empty. */
    FILE *f = fopen(diff_path, "rb");
    assert_non_null(f);
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        assert_true(sz > 0);
        unlink(diff_path);
    }

    test_harness_sdl_shutdown(&sdl);
}

/* ── Test runner ──────────────────────────────────────────────────── */

static const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_fb_read_pixels_basic),
    cmocka_unit_test(test_fb_read_pixels_subrect),
    cmocka_unit_test(test_fb_read_pixels_bad_args),
    cmocka_unit_test(test_fb_region_has_content),
    cmocka_unit_test(test_fb_region_has_color),
    cmocka_unit_test(test_fb_frames_differ),
    cmocka_unit_test(test_fb_save_png),
    cmocka_unit_test(test_fb_golden_compare),
    cmocka_unit_test(test_fb_save_diff),
};

int
main(void)
{
    return cmocka_run_group_tests(tests, NULL, NULL);
}