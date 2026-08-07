/*
 * test_overlay_service.c — cmocka tests for run_overlay_service (Task 3).
 *
 * Verifies:
 *   (a) run_overlay_service(0) with SDL_VIDEODRIVER=nonexistent returns
 *       non-zero (SDL init failure, no crash).
 *   (b) run_overlay_service(1) (dry-run) returns 0.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "app/overlay_service.h"

/* ------------------------------------------------------------------ */
/*  Test (a): SDL init failure returns non-zero                       */
/* ------------------------------------------------------------------ */
static void test_sdl_init_failure_returns_nonzero(void **state)
{
    (void)state;

    /* Ensure SDL is fully uninitialized so the env var is re-read. */
    SDL_Quit();

    /* Force a non-existent video driver. */
    setenv("SDL_VIDEODRIVER", "nonexistent", 1);

    int rc = run_overlay_service(0);

    /* Restore environment. */
    unsetenv("SDL_VIDEODRIVER");
    SDL_Quit();

    assert_int_not_equal(rc, 0);
}

/* ------------------------------------------------------------------ */
/*  Test (b): dry-run returns 0                                       */
/* ------------------------------------------------------------------ */
static void test_dry_run_returns_zero(void **state)
{
    (void)state;

    int rc = run_overlay_service(1);
    assert_int_equal(rc, 0);
}

/* ------------------------------------------------------------------ */
/*  Test runner                                                        */
/* ------------------------------------------------------------------ */
static const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_sdl_init_failure_returns_nonzero),
    cmocka_unit_test(test_dry_run_returns_zero),
};

int main(void)
{
    return cmocka_run_group_tests(tests, NULL, NULL);
}