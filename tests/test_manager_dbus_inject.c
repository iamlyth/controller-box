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
 * Task 2: Also tests that the manager run-loop drains NameOwnerChanged
 * signals via process(), firing cbx_manager_backend_ready/_degraded.
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
#include "dbus/ip_connection.h"

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

    cbx_controllers_tab *ct = cbx_manager_controllers_tab(&mgr);
    assert_non_null(ct);
    assert_ptr_equal(ct->backend, mock_be);
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
    int rc = cbx_manager_init_with_dbus(&mgr, font, NULL, NULL);
    assert_int_equal(rc, 0);

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
/*  Task 2: Manager run-loop drains NameOwnerChanged via process()     */
/* ------------------------------------------------------------------ */

/* Test: when the manager run loop calls process(), it drains queued
 * NameOwnerChanged signals and fires cbx_manager_backend_ready,
 * transitioning the controllers tab from degraded to available.
 * This proves the loop integrates with ip_connection recovery (SPEC §2.4). */
static void
test_loop_drains_noc_recovers(void **state)
{
    (void)state;
    ensure_dummy_driver();

    ip_dbus_mock mock;
    ip_dbus_mock_init(&mock);
    const ip_dbus_backend *mock_be = ip_dbus_mock_backend(&mock);

    /* Initialize the manager with the mock backend. */
    cbx_manager mgr;
    const char *font = font_available() ? CBX_FONT_PATH : NULL;
    int rc = cbx_manager_init_with_dbus(&mgr, font, mock_be, mock.bus);
    assert_int_equal(rc, 0);

    /* Now set up the ip_connection on mgr.connection directly so the
     * NameOwnerChanged subscription callback points to mgr.connection. */
    ip_dbus_mock_expect_error(&mock, IP_IFACE_MANAGER, "Version",
                              IP_ERR_SERVICE_UNKNOWN);
    ip_connection_init(&mgr.connection, mock_be);
    ip_connection_set_bus(&mgr.connection, mock.bus);
    int crc = ip_connection_connect(&mgr.connection);
    assert_int_equal(crc, IP_ERR_SERVICE_UNKNOWN);
    assert_true(ip_connection_is_degraded(&mgr.connection));
    assert_non_null(mgr.connection.bus);
    mgr.owns_dbus_connection = false;  /* don't double-free */

    /* Register the production callbacks on the connection. */
    ip_connection_set_reenumerate_cb(&mgr.connection,
                                       cbx_manager_backend_ready, &mgr);
    ip_connection_set_degraded_cb(&mgr.connection,
                                    cbx_manager_backend_degraded, &mgr);

    /* Simulate degraded state: InputPlumber not available. */
    mgr.dbus_connected = false;
    cbx_controllers_tab_set_available(&mgr.ct, false,
        "InputPlumber unavailable \xe2\x80\x94 waiting for service");

    /* Verify degraded state before recovery. */
    assert_false(mgr.dbus_connected);
    assert_false(mgr.ct.add_btn.base.interactive);

    /* Queue a NameOwnerChanged (acquired) signal. */
    ip_dbus_mock_expect_ok(&mock, IP_IFACE_MANAGER, "Version", "1.0.0");
    /* Provide a minimal GetManagedObjects so refresh succeeds. */
    ip_dbus_mock_expect_ok(&mock, IP_IFACE_OBJECT_MANAGER,
                            "GetManagedObjects",
                            "/org/shadowblip/InputPlumber/Manager\t"
                            "org.shadowblip.InputManager\n");
    ip_dbus_mock_queue_noc(&mock, IP_DBUS_NAME, "", ":1.42");

    /* Simulate one iteration of the manager run loop's DBus drain.
     * This is exactly what cbx_manager_run() does. */
    assert_non_null(mgr.dbus_backend);
    assert_non_null(mgr.dbus_bus);
    assert_non_null(mgr.dbus_backend->process);
    for (int i = 0; i < 64; i++) {
        int processed = mgr.dbus_backend->process(mgr.dbus_bus);
        if (processed <= 0)
            break;
    }

    /* The loop should have drained the NOC signal, firing
     * cbx_manager_backend_ready, which sets dbus_connected and
     * re-enables the controllers tab. */
    assert_true(mgr.dbus_connected);
    assert_true(mgr.ct.add_btn.base.interactive);

    cbx_manager_shutdown(&mgr);
    ip_dbus_mock_free(&mock);
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
        cmocka_unit_test(test_loop_drains_noc_recovers),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}