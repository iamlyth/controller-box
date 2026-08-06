/*
 * test_harness.h — SDL2 dummy-driver test utility (Task 3).
 *
 * Provides helpers that initialise SDL2 with the "dummy" video driver so
 * rendering and widget tests can run headless in CI/sandbox environments
 * without a real display server.  Later tasks (renderer init, widget
 * tests, animation, overlay surface) call these helpers from their cmocka
 * unit tests.
 *
 * Usage pattern (inside a cmocka test):
 *
 *     TestState state;
 *     assert_int_equal(test_harness_sdl_init(&state), 0);
 *     // ... use state.window and state.renderer ...
 *     test_harness_sdl_shutdown(&state);
 */
#ifndef CBX_TEST_HARNESS_H
#define CBX_TEST_HARNESS_H

#include <SDL2/SDL.h>

typedef struct {
    SDL_Window   *window;
    SDL_Renderer *renderer;
} TestSdlState;

/*
 * Initialise SDL2 video with the dummy driver (falls back to dummy if the
 * real driver is unavailable).  Creates a hidden 320x240 window and a
 * renderer.  Returns 0 on success, -1 on failure (SDL_GetError describes
 * the problem).  On success the caller owns state->window/renderer and
 * must call test_harness_sdl_shutdown().
 */
int test_harness_sdl_init(TestSdlState *state);

/* Tear down SDL2 resources allocated by test_harness_sdl_init(). */
void test_harness_sdl_shutdown(TestSdlState *state);

#endif /* CBX_TEST_HARNESS_H */