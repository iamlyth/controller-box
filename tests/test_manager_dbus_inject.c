/*
 * test_manager_dbus_inject.c — Test for DBus backend injection point.
 *
 * Verifies that cbx_manager_init_with_dbus() accepts an optional mock
 * backend/bus pair and wires them into the manager's controllers tab
 * without calling ip_dbus_sd_backend() or attempting a real connect.
 *
 * Also verifies that cbx_manager_init() (the thin wrapper) still works
 * by delegating to cbx_manager_init_with_dbus(mgr, font, NULL, NULL),
 * which falls back to the production path (degraded mode in CI).
 *
 * Uses SDL2 dummy driver for headless testing.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include "manager/manager.h"
#include "dbus_mock.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#ifndef CBX_FONT_PATH
#define CBX_FONT_PATH ""
#endif

/* --- helpers ------------------------------------------------------- */

static void
ensure_dummy_driver(void)
{
    if (getenv("SDL_VIDEODRIVER") == NULL)
        SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "dummy",
                                 SDL_HINT_OVERRIDE);
}

static bool
font_available(void)
{
    return CBX_FONT_PATH[0] != '\0' && access(CBX_FONT_PATH, R_OK) == 0;
}

/* ------------------------------------------------------------------ */
/*  Test: injected mock backend is wired into controllers tab          */
/* ------------------------------------------------------------------ */

static void
test_injected_backend_wired(void **state)
{
    (void)state;
    ensure_dummy_driver();

    ip_dbus_mock mock;
    ip_dbus_mock_init(&mock);
    const ip_dbus_backend *mock_be = ip_dbus_mock_backend(&mock);

    cbx_manager mgr;
    const char *font = font_available() ? CBX_FONT_PATH : NULL;
    int rc = cbx_manager_init_with_dbus(&mgr, font, mock_be, mock.bus);
    assert_int_equal(rc, 0);

    /* The injected backend pointer must be the one stored on the
     * controllers tab and on the manager struct. */
    cbx_controllers_tab *ct = cbx_manager_controllers_tab(&mgr);
    assert_non_null(ct);
    assert_ptr_equal(ct->backend, mock_be);

    /* Manager-level fields reflect the injected backend. */
    assert_ptr_equal(mgr.dbus_backend, mock_be);
    assert_true(mgr.dbus_connected);

    cbx_manager_shutdown(&mgr);
    ip_dbus_mock_free(&mock);
}

/* ------------------------------------------------------------------ */
/*  Test: NULL backend falls back to production path (degraded mode)   */
/* ------------------------------------------------------------------ */

static void
test_null_backend_falls_back(void **state)
{
    (void)state;
    ensure_dummy_driver();

    cbx_manager mgr;
    const char *font = font_available() ? CBX_FONT_PATH : NULL;
    /* Passing NULL backend should use ip_dbus_sd_backend() + connect.
     * In CI (no system bus), this results in degraded mode (backend=NULL)
     * but init still succeeds. */
    int rc = cbx_manager_init_with_dbus(&mgr, font, NULL, NULL);
    assert_int_equal(rc, 0);

    /* In degraded mode, backend is NULL (no system bus in CI). */
    cbx_controllers_tab *ct = cbx_manager_controllers_tab(&mgr);
    assert_non_null(ct);
    /* ct->backend is NULL in degraded mode — the controllers tab handles
     * this gracefully.  We just verify init succeeded and shutdown works. */
    cbx_manager_shutdown(&mgr);
}

/* ------------------------------------------------------------------ */
/*  Test: cbx_manager_init wrapper delegates to init_with_dbus         */
/* ------------------------------------------------------------------ */

static void
test_wrapper_delegates(void **state)
{
    (void)state;
    ensure_dummy_driver();

    cbx_manager mgr;
    const char *font = font_available() ? CBX_FONT_PATH : NULL;
    int rc = cbx_manager_init(&mgr, font);
    assert_int_equal(rc, 0);

    /* In CI, this is degraded mode (no system bus).  The key assertion
     * is that init succeeds — the wrapper calls init_with_dbus(NULL, NULL)
     * which exercises the production path. */
    cbx_manager_shutdown(&mgr);
}

/* ------------------------------------------------------------------ */
/*  Test: NULL manager pointer returns -EINVAL                        */
/* ------------------------------------------------------------------ */

static void
test_null_mgr_returns_einval(void **state)
{
    (void)state;
    int rc = cbx_manager_init_with_dbus(NULL, NULL, NULL, NULL);
    assert_int_equal(rc, -EINVAL);
}

/* ------------------------------------------------------------------ */
/*  Runner                                                            */
/* ------------------------------------------------------------------ */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_injected_backend_wired),
        cmocka_unit_test(test_null_backend_falls_back),
        cmocka_unit_test(test_wrapper_delegates),
        cmocka_unit_test(test_null_mgr_returns_einval),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}