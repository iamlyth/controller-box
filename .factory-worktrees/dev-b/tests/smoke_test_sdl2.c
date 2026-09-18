/*
 * smoke_test_sdl2.c — Task 1 smoke test.
 *
 * Opens an SDL2 window, creates a renderer, clears to a solid color,
 * presents one frame, then shuts down. Works headless by falling back
 * to the SDL2 "dummy" video driver when no display is available, so it
 * can run in CI/sandbox environments as well as on a desktop.
 */
#include <SDL2/SDL.h>
#include <stdio.h>

static int try_init_video(void) {
    if (SDL_Init(SDL_INIT_VIDEO) == 0) return 0;
    /* Fall back to the dummy driver for headless environments. */
    SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "dummy", SDL_HINT_OVERRIDE);
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) == 0) return 0;
    return -1;
}

int main(void) {
    if (try_init_video() != 0) {
        fprintf(stderr, "smoke_test_sdl2: SDL_INIT_VIDEO failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow(
        "controller-box smoke test", SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED, 320, 240, SDL_WINDOW_HIDDEN);
    if (!window) {
        fprintf(stderr, "smoke_test_sdl2: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        fprintf(stderr, "smoke_test_sdl2: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_SetRenderDrawColor(renderer, 0x20, 0x20, 0x30, 0xFF);
    SDL_RenderClear(renderer);
    SDL_RenderPresent(renderer);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    printf("smoke_test_sdl2: OK\n");
    return 0;
}