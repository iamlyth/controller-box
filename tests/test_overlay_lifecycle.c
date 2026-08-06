/*
 * test_overlay_lifecycle.c — Unit tests for overlay state machine lifecycle.
 *
 * Task 28 — Overlay state machine and lifecycle.
 *
 * Tests:
 *   - Init (struct fields, defaults, NULL safe)
 *   - Activate from IDLE (instant → VISIBLE, with fade → ACTIVATING)
 *   - Activate from non-IDLE (-EPERM)
 *   - Close from VISIBLE (instant → IDLE, with fade → CLOSING)
 *   - Close from ACTIVATING (cancel activation)
 *   - Close from IDLE (-EPERM)
 *   - Tick in IDLE (no-op)
 *   - Tick in ACTIVATING (animation → VISIBLE)
 *   - Tick in VISIBLE (timeout → force close)
 *   - Tick in CLOSING (animation → IDLE)
 *   - Force close from any state
 *   - InterceptMode set on close (mock DBus)
 *   - InterceptMode set failure → on_error
 *   - Save callback (fired on close, failure → on_error)
 *   - Timeout handling (VISIBLE for max_ticks → close)
 *   - State name, is_active helpers
 *   - Full lifecycle simulation
 *   - No callbacks (NULL safe)
 *   - Animation path (with SDL_INIT_TIMER)
 *
 * Uses mock DBus backend (cbx_test_support) for InterceptMode set calls.
 * No SDL rendering — surface is NULL for most tests (state machine only).
 * Animation tests use SDL_INIT_TIMER for SDL_GetTicks().
 */
#include "dbus_mock.h"
#include "dbus/ip_composite.h"
#include "dbus/ip_connection.h"   /* IP_ERR_NO_REPLY */
#include "dbus/ip_device_model.h"
#include "overlay/lifecycle.h"
#include "ui/animation.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <cmocka.h>

/* --- Test constants ---------------------------------------------------- */

#define COMP_PATH "/org/shadowblip/InputPlumber/CompositeDevice0"

/* --- Callback tracking ------------------------------------------------ */

typedef struct {
    int visible_fired;
    int closed_fired;
    int error_fired;
    int error_code;
    int save_rc;       /* return value for on_save (0 = success) */
    int save_fired;
} lifecycle_callbacks;

static void
on_visible(void *userdata)
{
    lifecycle_callbacks *cb = (lifecycle_callbacks *)userdata;
    if (cb) cb->visible_fired++;
}

static void
on_closed(void *userdata)
{
    lifecycle_callbacks *cb = (lifecycle_callbacks *)userdata;
    if (cb) cb->closed_fired++;
}

static int
on_save(void *userdata)
{
    lifecycle_callbacks *cb = (lifecycle_callbacks *)userdata;
    if (cb) {
        cb->save_fired++;
        return cb->save_rc;
    }
    return 0;
}

static void
on_error(int code, void *userdata)
{
    lifecycle_callbacks *cb = (lifecycle_callbacks *)userdata;
    if (cb) {
        cb->error_fired++;
        cb->error_code = code;
    }
}

/* --- Basic fixture (no SDL, instant transitions) ---------------------- */

typedef struct {
    ip_dbus_mock          mock;
    const ip_dbus_backend *backend;
    cbx_overlay_lifecycle lc;
    lifecycle_callbacks   callbacks;
} lc_fixture;

static int
setup(void **state)
{
    lc_fixture *f = malloc(sizeof(*f));
    if (!f)
        return -1;
    memset(f, 0, sizeof(*f));
    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);
    memset(&f->callbacks, 0, sizeof(f->callbacks));
    f->callbacks.save_rc = 0;
    cbx_overlay_lifecycle_init(&f->lc, f->backend, f->mock.bus,
                               COMP_PATH, NULL, NULL);
    f->lc.on_visible = on_visible;
    f->lc.on_visible_data = &f->callbacks;
    f->lc.on_closed = on_closed;
    f->lc.on_closed_data = &f->callbacks;
    f->lc.on_save = on_save;
    f->lc.on_save_data = &f->callbacks;
    f->lc.on_error = on_error;
    f->lc.on_error_data = &f->callbacks;
    /* Instant transitions for basic tests. */
    f->lc.fade_in_ms = 0;
    f->lc.fade_out_ms = 0;
    *state = f;
    return 0;
}

static int
teardown(void **state)
{
    lc_fixture *f = *state;
    if (f) {
        ip_dbus_mock_reset(&f->mock);
        free(f);
    }
    return 0;
}

#define FIX(state) (*(lc_fixture **)(state))

/* --- Animation fixture (SDL_INIT_TIMER for SDL_GetTicks) -------------- */

typedef struct {
    ip_dbus_mock          mock;
    const ip_dbus_backend *backend;
    cbx_overlay_lifecycle lc;
    lifecycle_callbacks   callbacks;
} anim_fixture;

static int
setup_anim(void **state)
{
    anim_fixture *f = malloc(sizeof(*f));
    if (!f)
        return -1;
    memset(f, 0, sizeof(*f));
    if (SDL_Init(SDL_INIT_TIMER) != 0) {
        free(f);
        return -1;
    }
    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);
    memset(&f->callbacks, 0, sizeof(f->callbacks));
    f->callbacks.save_rc = 0;
    cbx_overlay_lifecycle_init(&f->lc, f->backend, f->mock.bus,
                               COMP_PATH, NULL, NULL);
    f->lc.on_visible = on_visible;
    f->lc.on_visible_data = &f->callbacks;
    f->lc.on_closed = on_closed;
    f->lc.on_closed_data = &f->callbacks;
    f->lc.on_save = on_save;
    f->lc.on_save_data = &f->callbacks;
    f->lc.on_error = on_error;
    f->lc.on_error_data = &f->callbacks;
    /* Use short animation durations. */
    f->lc.fade_in_ms = 50;
    f->lc.fade_out_ms = 30;
    *state = f;
    return 0;
}

static int
teardown_anim(void **state)
{
    anim_fixture *f = *state;
    if (f) {
        ip_dbus_mock_reset(&f->mock);
        free(f);
    }
    SDL_Quit();
    return 0;
}

#define AFIX(state) (*(anim_fixture **)(state))

/* --- Init tests -------------------------------------------------------- */

static void
test_init(void **state)
{
    lc_fixture *f = FIX(state);
    assert_int_equal(f->lc.state, CBX_OVERLAY_IDLE);
    assert_non_null(f->lc.backend);
    assert_string_equal(f->lc.composite_path, COMP_PATH);
    assert_int_equal(f->lc.fade_in_ms, 0);
    assert_int_equal(f->lc.fade_out_ms, 0);
    assert_int_equal(f->lc.max_visible_ticks, 0);
    assert_int_equal(f->lc.max_errors, CBX_OVERLAY_DEFAULT_MAX_ERRORS);
    assert_int_equal(f->lc.error_count, 0);
    assert_int_equal(f->lc.visible_ticks, 0);
}

static void
test_init_defaults(void **state)
{
    (void)state;
    cbx_overlay_lifecycle lc;
    cbx_overlay_lifecycle_init(&lc, NULL, NULL, NULL, NULL, NULL);
    assert_int_equal(lc.state, CBX_OVERLAY_IDLE);
    assert_int_equal(lc.fade_in_ms, CBX_OVERLAY_DEFAULT_FADE_IN_MS);
    assert_int_equal(lc.fade_out_ms, CBX_OVERLAY_DEFAULT_FADE_OUT_MS);
    assert_int_equal(lc.max_visible_ticks, CBX_OVERLAY_DEFAULT_MAX_VISIBLE_TICKS);
    assert_int_equal(lc.max_errors, CBX_OVERLAY_DEFAULT_MAX_ERRORS);
    assert_double_equal(lc.target_opacity, 1.0, 0.001);
    assert_null(lc.surface);
    assert_null(lc.renderer);
}

static void
test_init_null(void **state)
{
    (void)state;
    cbx_overlay_lifecycle_init(NULL, NULL, NULL, NULL, NULL, NULL);
    /* Should not crash. */
}

/* --- Activate tests --------------------------------------------------- */

static void
test_activate_instant(void **state)
{
    lc_fixture *f = FIX(state);
    /* fade_in_ms == 0 → instant transition to VISIBLE. */
    int rc = cbx_overlay_lifecycle_activate(&f->lc);
    assert_int_equal(rc, 0);
    assert_int_equal(f->lc.state, CBX_OVERLAY_VISIBLE);
    assert_int_equal(f->callbacks.visible_fired, 1);
}

static void
test_activate_not_idle(void **state)
{
    lc_fixture *f = FIX(state);
    /* Activate once (instant → VISIBLE). */
    cbx_overlay_lifecycle_activate(&f->lc);
    assert_int_equal(f->lc.state, CBX_OVERLAY_VISIBLE);

    /* Second activate should fail. */
    int rc = cbx_overlay_lifecycle_activate(&f->lc);
    assert_int_equal(rc, -EPERM);
    assert_int_equal(f->lc.state, CBX_OVERLAY_VISIBLE);
}

static void
test_activate_null(void **state)
{
    (void)state;
    int rc = cbx_overlay_lifecycle_activate(NULL);
    assert_int_equal(rc, -EINVAL);
}

/* --- Close tests ------------------------------------------------------ */

static void
test_close_instant(void **state)
{
    lc_fixture *f = FIX(state);
    cbx_overlay_lifecycle_activate(&f->lc);
    assert_int_equal(f->lc.state, CBX_OVERLAY_VISIBLE);

    /* Expect InterceptMode set to PASS. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "InterceptMode", "1");

    int rc = cbx_overlay_lifecycle_close(&f->lc);
    assert_int_equal(rc, 0);
    /* fade_out_ms == 0 → instant transition to IDLE. */
    assert_int_equal(f->lc.state, CBX_OVERLAY_IDLE);
    assert_int_equal(f->callbacks.save_fired, 1);
    assert_int_equal(f->callbacks.closed_fired, 1);
}

static void
test_close_from_activating(void **state)
{
    lc_fixture *f = FIX(state);
    /* Manually set to ACTIVATING (cancel activation scenario). */
    f->lc.state = CBX_OVERLAY_ACTIVATING;

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "InterceptMode", "1");

    int rc = cbx_overlay_lifecycle_close(&f->lc);
    assert_int_equal(rc, 0);
    /* fade_out_ms == 0 → instant IDLE. */
    assert_int_equal(f->lc.state, CBX_OVERLAY_IDLE);
    assert_int_equal(f->callbacks.closed_fired, 1);
}

static void
test_close_from_idle(void **state)
{
    lc_fixture *f = FIX(state);
    int rc = cbx_overlay_lifecycle_close(&f->lc);
    assert_int_equal(rc, -EPERM);
    assert_int_equal(f->lc.state, CBX_OVERLAY_IDLE);
}

static void
test_close_null(void **state)
{
    (void)state;
    int rc = cbx_overlay_lifecycle_close(NULL);
    assert_int_equal(rc, -EINVAL);
}

/* --- Tick tests ------------------------------------------------------- */

static void
test_tick_idle(void **state)
{
    lc_fixture *f = FIX(state);
    int rc = cbx_overlay_lifecycle_tick(&f->lc);
    assert_int_equal(rc, 0);
    assert_int_equal(f->lc.state, CBX_OVERLAY_IDLE);
}

static void
test_tick_null(void **state)
{
    (void)state;
    int rc = cbx_overlay_lifecycle_tick(NULL);
    assert_int_equal(rc, -EINVAL);
}

static void
test_tick_visible_timeout(void **state)
{
    lc_fixture *f = FIX(state);
    cbx_overlay_lifecycle_activate(&f->lc);
    assert_int_equal(f->lc.state, CBX_OVERLAY_VISIBLE);
    f->lc.max_visible_ticks = 3;

    /* Expect InterceptMode set to PASS when timeout fires. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "InterceptMode", "1");

    /* Tick 1 and 2: still VISIBLE. */
    cbx_overlay_lifecycle_tick(&f->lc);
    assert_int_equal(f->lc.state, CBX_OVERLAY_VISIBLE);
    cbx_overlay_lifecycle_tick(&f->lc);
    assert_int_equal(f->lc.state, CBX_OVERLAY_VISIBLE);

    /* Tick 3: timeout → close → IDLE (instant fade_out). */
    cbx_overlay_lifecycle_tick(&f->lc);
    assert_int_equal(f->lc.state, CBX_OVERLAY_IDLE);
    assert_int_equal(f->callbacks.closed_fired, 1);
}

static void
test_tick_visible_no_timeout(void **state)
{
    lc_fixture *f = FIX(state);
    cbx_overlay_lifecycle_activate(&f->lc);
    /* max_visible_ticks = 0 → disabled. */
    assert_int_equal(f->lc.max_visible_ticks, 0);

    for (int i = 0; i < 100; i++)
        cbx_overlay_lifecycle_tick(&f->lc);

    assert_int_equal(f->lc.state, CBX_OVERLAY_VISIBLE);
}

/* --- Force close tests ------------------------------------------------ */

static void
test_force_close_from_visible(void **state)
{
    lc_fixture *f = FIX(state);
    cbx_overlay_lifecycle_activate(&f->lc);
    assert_int_equal(f->lc.state, CBX_OVERLAY_VISIBLE);

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "InterceptMode", "1");

    cbx_overlay_lifecycle_force_close(&f->lc);
    assert_int_equal(f->lc.state, CBX_OVERLAY_IDLE);
    /* force_close does NOT fire on_save. */
    assert_int_equal(f->callbacks.save_fired, 0);
    assert_int_equal(f->callbacks.closed_fired, 1);
}

static void
test_force_close_from_idle(void **state)
{
    lc_fixture *f = FIX(state);
    cbx_overlay_lifecycle_force_close(&f->lc);
    assert_int_equal(f->lc.state, CBX_OVERLAY_IDLE);
    /* Should still fire on_closed. */
    assert_int_equal(f->callbacks.closed_fired, 1);
}

static void
test_force_close_null(void **state)
{
    (void)state;
    cbx_overlay_lifecycle_force_close(NULL);
    /* Should not crash. */
}

/* --- Error handling tests --------------------------------------------- */

static void
test_close_intercept_mode_fail(void **state)
{
    lc_fixture *f = FIX(state);
    cbx_overlay_lifecycle_activate(&f->lc);

    /* InterceptMode set fails. */
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                               "InterceptMode", IP_ERR_NO_REPLY);

    int rc = cbx_overlay_lifecycle_close(&f->lc);
    /* close() itself returns 0 (it still transitions); error is reported
     * via on_error callback. */
    assert_int_equal(rc, 0);
    assert_int_equal(f->lc.state, CBX_OVERLAY_IDLE);
    assert_int_equal(f->callbacks.error_fired, 1);
    assert_int_equal(f->callbacks.error_code, IP_ERR_NO_REPLY);
    /* Save still fires even if InterceptMode set fails. */
    assert_int_equal(f->callbacks.save_fired, 1);
}

static void
test_save_fail_reports_error(void **state)
{
    lc_fixture *f = FIX(state);
    cbx_overlay_lifecycle_activate(&f->lc);

    /* Save callback returns error. */
    f->callbacks.save_rc = -EIO;

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "InterceptMode", "1");

    cbx_overlay_lifecycle_close(&f->lc);
    assert_int_equal(f->callbacks.error_fired, 1);
    assert_int_equal(f->callbacks.error_code, -EIO);
    /* Close still proceeds. */
    assert_int_equal(f->lc.state, CBX_OVERLAY_IDLE);
}

/* --- No callback tests ------------------------------------------------ */

static void
test_no_callbacks(void **state)
{
    lc_fixture *f = FIX(state);
    /* Re-init with NULL callbacks. */
    cbx_overlay_lifecycle_init(&f->lc, f->backend, f->mock.bus,
                                COMP_PATH, NULL, NULL);
    f->lc.fade_in_ms = 0;
    f->lc.fade_out_ms = 0;

    /* Activate should not crash. */
    int rc = cbx_overlay_lifecycle_activate(&f->lc);
    assert_int_equal(rc, 0);
    assert_int_equal(f->lc.state, CBX_OVERLAY_VISIBLE);

    /* Close should not crash. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "InterceptMode", "1");
    rc = cbx_overlay_lifecycle_close(&f->lc);
    assert_int_equal(rc, 0);
    assert_int_equal(f->lc.state, CBX_OVERLAY_IDLE);
}

static void
test_no_callbacks_intercept_fail(void **state)
{
    lc_fixture *f = FIX(state);
    cbx_overlay_lifecycle_init(&f->lc, f->backend, f->mock.bus,
                                COMP_PATH, NULL, NULL);
    f->lc.fade_in_ms = 0;
    f->lc.fade_out_ms = 0;

    cbx_overlay_lifecycle_activate(&f->lc);

    /* InterceptMode set fails — should not crash with NULL error cb. */
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                               "InterceptMode", IP_ERR_NO_REPLY);
    cbx_overlay_lifecycle_close(&f->lc);
    assert_int_equal(f->lc.state, CBX_OVERLAY_IDLE);
}

/* --- State helpers ---------------------------------------------------- */

static void
test_state_name(void **state)
{
    (void)state;
    assert_string_equal(cbx_overlay_lifecycle_state_name(CBX_OVERLAY_IDLE), "IDLE");
    assert_string_equal(cbx_overlay_lifecycle_state_name(CBX_OVERLAY_ACTIVATING), "ACTIVATING");
    assert_string_equal(cbx_overlay_lifecycle_state_name(CBX_OVERLAY_VISIBLE), "VISIBLE");
    assert_string_equal(cbx_overlay_lifecycle_state_name(CBX_OVERLAY_CLOSING), "CLOSING");
    assert_string_equal(cbx_overlay_lifecycle_state_name(99), "UNKNOWN");
}

static void
test_is_active(void **state)
{
    (void)state;
    cbx_overlay_lifecycle lc;
    cbx_overlay_lifecycle_init(&lc, NULL, NULL, NULL, NULL, NULL);

    assert_false(cbx_overlay_lifecycle_is_active(&lc));
    assert_false(cbx_overlay_lifecycle_is_active(NULL));

    lc.state = CBX_OVERLAY_ACTIVATING;
    assert_true(cbx_overlay_lifecycle_is_active(&lc));

    lc.state = CBX_OVERLAY_VISIBLE;
    assert_true(cbx_overlay_lifecycle_is_active(&lc));

    lc.state = CBX_OVERLAY_CLOSING;
    assert_true(cbx_overlay_lifecycle_is_active(&lc));

    lc.state = CBX_OVERLAY_IDLE;
    assert_false(cbx_overlay_lifecycle_is_active(&lc));
}

static void
test_get_state_null(void **state)
{
    (void)state;
    assert_int_equal(cbx_overlay_lifecycle_get_state(NULL), CBX_OVERLAY_IDLE);
}

/* --- Full lifecycle simulation (instant) ------------------------------ */

static void
test_full_lifecycle_instant(void **state)
{
    lc_fixture *f = FIX(state);

    /* IDLE → activate → VISIBLE. */
    cbx_overlay_lifecycle_activate(&f->lc);
    assert_int_equal(f->lc.state, CBX_OVERLAY_VISIBLE);
    assert_int_equal(f->callbacks.visible_fired, 1);

    /* VISIBLE → close → IDLE. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "InterceptMode", "1");
    cbx_overlay_lifecycle_close(&f->lc);
    assert_int_equal(f->lc.state, CBX_OVERLAY_IDLE);
    assert_int_equal(f->callbacks.save_fired, 1);
    assert_int_equal(f->callbacks.closed_fired, 1);

    /* Can re-activate. */
    cbx_overlay_lifecycle_activate(&f->lc);
    assert_int_equal(f->lc.state, CBX_OVERLAY_VISIBLE);
    assert_int_equal(f->callbacks.visible_fired, 2);
}

/* --- Animation tests (require SDL_INIT_TIMER) ------------------------- */

static void
test_activate_with_fade(void **state)
{
    anim_fixture *f = AFIX(state);

    int rc = cbx_overlay_lifecycle_activate(&f->lc);
    assert_int_equal(rc, 0);
    assert_int_equal(f->lc.state, CBX_OVERLAY_ACTIVATING);
    assert_int_equal(f->callbacks.visible_fired, 0);

    /* Tick: animation should be running. */
    cbx_overlay_lifecycle_tick(&f->lc);
    /* Still activating (50ms animation hasn't completed instantly). */
    assert_int_equal(f->lc.state, CBX_OVERLAY_ACTIVATING);

    /* Wait for animation to complete. */
    SDL_Delay(60);

    cbx_overlay_lifecycle_tick(&f->lc);
    assert_int_equal(f->lc.state, CBX_OVERLAY_VISIBLE);
    assert_int_equal(f->callbacks.visible_fired, 1);
}

static void
test_close_with_fade(void **state)
{
    anim_fixture *f = AFIX(state);

    /* Activate and wait for VISIBLE. */
    cbx_overlay_lifecycle_activate(&f->lc);
    SDL_Delay(60);
    cbx_overlay_lifecycle_tick(&f->lc);
    assert_int_equal(f->lc.state, CBX_OVERLAY_VISIBLE);

    /* Close. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "InterceptMode", "1");
    int rc = cbx_overlay_lifecycle_close(&f->lc);
    assert_int_equal(rc, 0);
    assert_int_equal(f->lc.state, CBX_OVERLAY_CLOSING);
    assert_int_equal(f->callbacks.save_fired, 1);
    assert_int_equal(f->callbacks.closed_fired, 0);

    /* Tick: animation running. */
    cbx_overlay_lifecycle_tick(&f->lc);
    assert_int_equal(f->lc.state, CBX_OVERLAY_CLOSING);

    /* Wait for fade-out. */
    SDL_Delay(40);
    cbx_overlay_lifecycle_tick(&f->lc);
    assert_int_equal(f->lc.state, CBX_OVERLAY_IDLE);
    assert_int_equal(f->callbacks.closed_fired, 1);
}

static void
test_full_lifecycle_with_fade(void **state)
{
    anim_fixture *f = AFIX(state);

    /* Activate. */
    cbx_overlay_lifecycle_activate(&f->lc);
    assert_int_equal(f->lc.state, CBX_OVERLAY_ACTIVATING);

    /* Tick until VISIBLE. */
    SDL_Delay(60);
    cbx_overlay_lifecycle_tick(&f->lc);
    assert_int_equal(f->lc.state, CBX_OVERLAY_VISIBLE);

    /* Close. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "InterceptMode", "1");
    cbx_overlay_lifecycle_close(&f->lc);
    assert_int_equal(f->lc.state, CBX_OVERLAY_CLOSING);

    /* Tick until IDLE. */
    SDL_Delay(40);
    cbx_overlay_lifecycle_tick(&f->lc);
    assert_int_equal(f->lc.state, CBX_OVERLAY_IDLE);

    /* Re-activate. */
    cbx_overlay_lifecycle_activate(&f->lc);
    assert_int_equal(f->lc.state, CBX_OVERLAY_ACTIVATING);
}

static void
test_close_activating_with_fade(void **state)
{
    anim_fixture *f = AFIX(state);

    /* Activate — starts fade-in. */
    cbx_overlay_lifecycle_activate(&f->lc);
    assert_int_equal(f->lc.state, CBX_OVERLAY_ACTIVATING);

    /* Close immediately (cancel activation). */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "InterceptMode", "1");
    int rc = cbx_overlay_lifecycle_close(&f->lc);
    assert_int_equal(rc, 0);
    /* fade_out_ms > 0 → CLOSING state, animation in progress. */
    assert_int_equal(f->lc.state, CBX_OVERLAY_CLOSING);
    assert_int_equal(f->callbacks.closed_fired, 0);

    /* Wait for fade-out to complete. */
    SDL_Delay(40);
    cbx_overlay_lifecycle_tick(&f->lc);
    assert_int_equal(f->lc.state, CBX_OVERLAY_IDLE);
    assert_int_equal(f->callbacks.closed_fired, 1);
}

/* --- Test runner ------------------------------------------------------ */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* Init tests. */
        cmocka_unit_test_setup_teardown(test_init, setup, teardown),
        cmocka_unit_test(test_init_defaults),
        cmocka_unit_test(test_init_null),

        /* Activate tests. */
        cmocka_unit_test_setup_teardown(test_activate_instant, setup, teardown),
        cmocka_unit_test_setup_teardown(test_activate_not_idle, setup, teardown),
        cmocka_unit_test(test_activate_null),

        /* Close tests. */
        cmocka_unit_test_setup_teardown(test_close_instant, setup, teardown),
        cmocka_unit_test_setup_teardown(test_close_from_activating,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_close_from_idle, setup, teardown),
        cmocka_unit_test(test_close_null),

        /* Tick tests. */
        cmocka_unit_test_setup_teardown(test_tick_idle, setup, teardown),
        cmocka_unit_test(test_tick_null),
        cmocka_unit_test_setup_teardown(test_tick_visible_timeout,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_tick_visible_no_timeout,
                                          setup, teardown),

        /* Force close tests. */
        cmocka_unit_test_setup_teardown(test_force_close_from_visible,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_force_close_from_idle,
                                          setup, teardown),
        cmocka_unit_test(test_force_close_null),

        /* Error handling tests. */
        cmocka_unit_test_setup_teardown(test_close_intercept_mode_fail,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_save_fail_reports_error,
                                          setup, teardown),

        /* No callback tests. */
        cmocka_unit_test_setup_teardown(test_no_callbacks, setup, teardown),
        cmocka_unit_test_setup_teardown(test_no_callbacks_intercept_fail,
                                          setup, teardown),

        /* State helpers. */
        cmocka_unit_test(test_state_name),
        cmocka_unit_test(test_is_active),
        cmocka_unit_test(test_get_state_null),

        /* Full lifecycle (instant). */
        cmocka_unit_test_setup_teardown(test_full_lifecycle_instant,
                                          setup, teardown),

        /* Animation tests (SDL_INIT_TIMER). */
        cmocka_unit_test_setup_teardown(test_activate_with_fade,
                                          setup_anim, teardown_anim),
        cmocka_unit_test_setup_teardown(test_close_with_fade,
                                          setup_anim, teardown_anim),
        cmocka_unit_test_setup_teardown(test_full_lifecycle_with_fade,
                                          setup_anim, teardown_anim),
        cmocka_unit_test_setup_teardown(test_close_activating_with_fade,
                                          setup_anim, teardown_anim),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}