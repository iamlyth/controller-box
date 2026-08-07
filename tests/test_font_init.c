/*
 * test_font_init.c — Regression test for font initialization and text
 * rendering (BUG-0001, Task 3).
 *
 * Exercises the full production font path:
 *   1. Discover a font via cbx_font_path() (runtime discovery) or
 *      CBX_FONT_PATH (compile-time fallback used by the test harness).
 *   2. Call cbx_manager_init() with the discovered path and assert
 *      mgr.font_id >= 0.
 *   3. Call cbx_text_render() on the manager's text cache with the
 *      loaded font_id and assert the returned SDL_Texture* is non-NULL.
 *   4. Test the failure path: cbx_manager_init() with a non-NULL but
 *      invalid font path returns a non-zero error code.
 *
 * Font-dependent assertions use skip() when no system font is available
 * in the test environment, matching the existing test_manager_tabs
 * pattern.  The invalid-path failure assertion always runs.
 *
 * Headless-safe: SDL_VIDEODRIVER=dummy is set via CMake
 * set_tests_properties.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "manager/manager.h"
#include "ui/text.h"
#include "config/config_paths.h"

#ifndef CBX_FONT_PATH
#define CBX_FONT_PATH ""
#endif

/* --- Helpers -------------------------------------------------------- */

static void
ensure_dummy_driver(void)
{
    if (getenv("SDL_VIDEODRIVER") == NULL)
        SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "dummy",
                                 SDL_HINT_OVERRIDE);
}

/*
 * Resolve a usable font path.  Prefer the compile-time CBX_FONT_PATH
 * (discovered by CMake at configure time) if it exists and is readable.
 * Fall back to runtime cbx_font_path() discovery.  Returns NULL if no
 * font is available.
 */
static const char *
resolve_font_path(void)
{
    if (CBX_FONT_PATH[0] != '\0' && access(CBX_FONT_PATH, R_OK) == 0)
        return CBX_FONT_PATH;

    return cbx_font_path();
}

/* --- Tests ---------------------------------------------------------- */

/*
 * Test (a): discovered font → font_id >= 0 → cbx_text_render() non-NULL.
 *
 * This exercises the full production code path: font discovery,
 * cbx_manager_init() with a real font, and actual text rendering to an
 * SDL_Texture via the dummy video driver.
 */
static void
test_font_init_and_render(void **state)
{
    (void)state;
    const char *font_path = resolve_font_path();
    if (font_path == NULL)
        skip();

    ensure_dummy_driver();

    cbx_manager mgr;
    int rc = cbx_manager_init(&mgr, font_path);
    assert_return_code(rc, 0);

    /* Font must be loaded — font_id >= 0. */
    assert_true(mgr.font_id >= 0);

    /* Render text using the manager's text cache and loaded font.
     * The returned SDL_Texture* must be non-NULL. */
    SDL_Color white = {255, 255, 255, 255};
    SDL_Texture *tex = cbx_text_render(&mgr.text_cache, mgr.font_id,
                                         "Hello", white);
    assert_non_null(tex);

    cbx_manager_shutdown(&mgr);
}

/*
 * Test (b): invalid font path → cbx_manager_init() returns non-zero.
 *
 * This test always runs — it does not depend on a system font being
 * available.  It verifies that passing a non-NULL but nonexistent font
 * path causes cbx_manager_init() to fail with a non-zero error code
 * rather than silently continuing with font_id = -1.
 */
static void
test_invalid_font_path_fails(void **state)
{
    (void)state;
    ensure_dummy_driver();

    cbx_manager mgr;
    int rc = cbx_manager_init(&mgr, "/nonexistent/font.ttf");
    assert_true(rc != 0);

    /* After a failed init, the manager should be in a clean state.
     * shutdown is safe on a partially-initialised struct. */
    cbx_manager_shutdown(&mgr);
}

/*
 * Test (c): NULL font path → init succeeds, font_id stays -1.
 *
 * This verifies the existing behaviour that passing NULL to
 * cbx_manager_init() skips font loading (font_id = -1) but still
 * succeeds.  This is the historical test path used by test_manager_tabs.
 */
static void
test_null_font_path_succeeds(void **state)
{
    (void)state;
    ensure_dummy_driver();

    cbx_manager mgr;
    int rc = cbx_manager_init(&mgr, NULL);
    assert_return_code(rc, 0);
    assert_int_equal(mgr.font_id, -1);

    cbx_manager_shutdown(&mgr);
}

/*
 * Test (d): empty string font path → treated like NULL, init succeeds.
 *
 * An empty string should be treated the same as NULL (the condition in
 * manager.c checks font_path[0] != '\0').
 */
static void
test_empty_font_path_succeeds(void **state)
{
    (void)state;
    ensure_dummy_driver();

    cbx_manager mgr;
    int rc = cbx_manager_init(&mgr, "");
    assert_return_code(rc, 0);
    assert_int_equal(mgr.font_id, -1);

    cbx_manager_shutdown(&mgr);
}

/* --- Test runner ---------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_font_init_and_render),
        cmocka_unit_test(test_invalid_font_path_fails),
        cmocka_unit_test(test_null_font_path_succeeds),
        cmocka_unit_test(test_empty_font_path_succeeds),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}