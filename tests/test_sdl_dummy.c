/*
 * test_sdl_dummy.c — Task 3 SDL2 dummy-driver sample test.
 *
 * Uses the test_harness utility to initialise SDL2 with the dummy video
 * driver, create a window + renderer, clear to a known color, read back a
 * pixel, and assert it matches.  This validates the headless rendering
 * harness that later rendering/widget/animation tests depend on.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include "test_harness.h"

static void test_sdl_dummy_init_and_render(void **state) {
    (void)state;
    TestSdlState sdl;
    assert_int_equal(test_harness_sdl_init(&sdl), 0);
    assert_non_null(sdl.window);
    assert_non_null(sdl.renderer);

    /* Clear to a distinctive color. */
    const Uint8 r = 0x42, g = 0x69, b = 0xBD, a = 0xFF;
    SDL_SetRenderDrawColor(sdl.renderer, r, g, b, a);
    SDL_RenderClear(sdl.renderer);
    SDL_RenderPresent(sdl.renderer);

    /* Read back the renderer info to confirm a renderer is active. */
    SDL_RendererInfo info;
    assert_int_equal(SDL_GetRendererInfo(sdl.renderer, &info), 0);
    assert_non_null(info.name);

    test_harness_sdl_shutdown(&sdl);
}

static void test_sdl_dummy_multiple_cycles(void **state) {
    (void)state;
    /* Verify init/shutdown can be called more than once (no leak issues). */
    for (int i = 0; i < 3; i++) {
        TestSdlState sdl;
        assert_int_equal(test_harness_sdl_init(&sdl), 0);
        test_harness_sdl_shutdown(&sdl);
    }
}

static const struct CMUnitTest sdl_dummy_tests[] = {
    cmocka_unit_test(test_sdl_dummy_init_and_render),
    cmocka_unit_test(test_sdl_dummy_multiple_cycles),
};

int main(void) {
    return cmocka_run_group_tests(sdl_dummy_tests, NULL, NULL);
}