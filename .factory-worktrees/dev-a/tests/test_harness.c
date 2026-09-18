/*
 * test_harness.c — SDL2 dummy-driver test utility (Task 3).
 *
 * Implementation of the headless SDL2 test helpers declared in
 * test_harness.h.
 */
#include "test_harness.h"

#include <stdio.h>
#include <stdlib.h>

/*
 * Ensure SDL2 uses the "dummy" video driver so tests run without a
 * display.  If the environment already sets SDL_VIDEODRIVER we respect it
 * (letting a developer run tests on a real display if desired); otherwise
 * we force the dummy driver before SDL_Init.
 */
static void ensure_dummy_driver(void) {
    if (getenv("SDL_VIDEODRIVER") == NULL) {
        SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "dummy",
                                SDL_HINT_OVERRIDE);
    }
}

int test_harness_sdl_init(TestSdlState *state) {
    if (!state) return -1;
    state->window   = NULL;
    state->renderer = NULL;

    ensure_dummy_driver();

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "test_harness: SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    state->window = SDL_CreateWindow(
        "controller-box test", SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED, 320, 240, SDL_WINDOW_HIDDEN);
    if (!state->window) {
        fprintf(stderr, "test_harness: SDL_CreateWindow failed: %s\n",
                SDL_GetError());
        SDL_Quit();
        return -1;
    }

    /* Prefer software renderer (always available with the dummy driver). */
    state->renderer = SDL_CreateRenderer(state->window, -1,
                                         SDL_RENDERER_SOFTWARE);
    if (!state->renderer) {
        fprintf(stderr, "test_harness: SDL_CreateRenderer failed: %s\n",
                SDL_GetError());
        SDL_DestroyWindow(state->window);
        state->window = NULL;
        SDL_Quit();
        return -1;
    }

    return 0;
}

void test_harness_sdl_shutdown(TestSdlState *state) {
    if (!state) return;
    if (state->renderer) {
        SDL_DestroyRenderer(state->renderer);
        state->renderer = NULL;
    }
    if (state->window) {
        SDL_DestroyWindow(state->window);
        state->window = NULL;
    }
    SDL_Quit();
}