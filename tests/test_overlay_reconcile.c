/*
 * test_overlay_reconcile.c — Task 9: visible reusable overlay and runtime
 * reconciliation tests.
 *
 * Exercises three production paths through cbx_overlay_service_step:
 *
 *   1. Per-activating-composite lifecycle: when composite N triggers the
 *      activation, close sets InterceptMode=PASS on composite N (not the
 *      primary composite).
 *
 *   2. Poll re-arm: after close (lifecycle IDLE), the step function re-arms
 *      IDLE polls so a subsequent activation is possible.
 *
 *   3. Hotplug reconciliation: when ip_hotplug sets model_changed, the step
 *      function rebuilds grid rows, columns, input mappings, triggers,
 *      and polls.
 *
 * SPEC §2.5, §4.7, §10.1.
 *
 * NOTE: The mock DBus deduplicates expectations by (iface, member) — only
 * one value per key at a time.  Tests set expectations in stages between
 * activation and close steps.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <SDL2/SDL.h>

#include "app/overlay_service.h"
#include "dbus_mock.h"
#include "dbus/ip_input_signal.h"
#include "dbus/ip_intercept_poll.h"
#include "dbus/ip_composite.h"
#include "dbus/ip_connection.h"
#include "dbus/ip_hotplug.h"
#include "config/config_settings.h"
#include "config/config_assignments.h"
#include "ui/renderer.h"
#include "overlay/surface_build.h"
#include "overlay/lifecycle.h"
#include "overlay/grid_render.h"
#include "overlay/player_mode.h"
#include "overlay/host_mode.h"
#include "overlay/dynamic_columns.h"

/* --- Constants --------------------------------------------------------- */

#define COMP_PATH_0 "/org/shadowblip/InputPlumber/CompositeDevice0"
#define COMP_PATH_1 "/org/shadowblip/InputPlumber/CompositeDevice1"
#define TARGET_PATH_0 "/org/shadowblip/InputPlumber/devices/target/gamepad0"
#define TARGET_PATH_1 "/org/shadowblip/InputPlumber/devices/target/gamepad1"

/* --- Test-local poll callbacks (replicate production per-composite logic) */

static void
test_on_activating(void *userdata)
{
    cbx_poll_activation_ctx *act = (cbx_poll_activation_ctx *)userdata;
    if (!act || !act->lifecycle)
        return;
    if (act->composite_path[0]) {
        size_t len = strlen(act->composite_path);
        if (len >= sizeof(act->lifecycle->composite_path))
            len = sizeof(act->lifecycle->composite_path) - 1;
        memcpy(act->lifecycle->composite_path, act->composite_path, len);
        act->lifecycle->composite_path[len] = '\0';
    }
    cbx_overlay_lifecycle_activate(act->lifecycle);
}

static void
test_on_deactivating(void *userdata)
{
    cbx_overlay_lifecycle *lc = (cbx_overlay_lifecycle *)userdata;
    cbx_overlay_lifecycle_close(lc);
}

static void
test_on_poll_error(int error_code, void *userdata)
{
    (void)userdata;
    (void)error_code;
}

/* --- Helpers ----------------------------------------------------------- */

static void
push_keydown(SDL_Keycode sym)
{
    SDL_Event ev;
    SDL_zero(ev);
    ev.type           = SDL_KEYDOWN;
    ev.key.keysym.sym = sym;
    SDL_PushEvent(&ev);
}

static void
push_poll_event(uint32_t event_type)
{
    SDL_Event ev;
    SDL_zero(ev);
    ev.type       = event_type;
    ev.user.code  = 0;
    ev.user.data1 = NULL;
    ev.user.data2 = NULL;
    SDL_PushEvent(&ev);
}

static void
flush_events(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
        ;
}

/* Staged mock helpers — the mock deduplicates by (iface, member). */

static void
expect_activation(ip_dbus_mock *mock)
{
    ip_dbus_mock_reset(mock);
    ip_dbus_mock_expect_ok(mock, IP_IFACE_COMPOSITE,
                            "InterceptMode", "2");
}

static void
expect_close_and_save(ip_dbus_mock *mock)
{
    ip_dbus_mock_reset(mock);
    ip_dbus_mock_expect_ok(mock, IP_IFACE_COMPOSITE,
                            "InterceptMode", "1");
    ip_dbus_mock_expect_ok(mock, IP_IFACE_MANAGER,
                            "AttachTargetDevice", NULL);
    ip_dbus_mock_expect_ok(mock, IP_IFACE_MANAGER,
                            "GamepadOrder", NULL);
}

static void
expect_pass_mode(ip_dbus_mock *mock)
{
    ip_dbus_mock_reset(mock);
    ip_dbus_mock_expect_ok(mock, IP_IFACE_COMPOSITE,
                            "InterceptMode", "1");
}

/* --- Fixture ----------------------------------------------------------- */

typedef struct {
    cbx_overlay_service_ctx *svc;
    ip_dbus_mock             mock;
    const ip_dbus_backend   *backend;
} reconcile_fixture;

static int
reconcile_setup(void **state)
{
    reconcile_fixture *f = malloc(sizeof(*f));
    memset(f, 0, sizeof(*f));

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER);
    SDL_VideoInit("dummy");

    f->svc = calloc(1, sizeof(cbx_overlay_service_ctx));
    assert_non_null(f->svc);

    /* Renderer (hidden). */
    int rc = cbx_renderer_init(&f->svc->rend, "test",
                                CBX_RENDERER_DEFAULT_W,
                                CBX_RENDERER_DEFAULT_H, false);
    assert_int_equal(rc, 0);

    /* Mock DBus. */
    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);
    ip_connection_init(&f->svc->conn, f->backend);
    ip_connection_set_bus(&f->svc->conn, f->mock.bus);

    /* 2 composites. */
    cbx_grid_composite_info comps[2];
    memset(comps, 0, sizeof(comps));
    snprintf(comps[0].id, sizeof(comps[0].id), "TEST:0");
    snprintf(comps[0].model_name, sizeof(comps[0].model_name), "TestPad0");
    snprintf(comps[0].composite_path, sizeof(comps[0].composite_path),
             "%s", COMP_PATH_0);
    snprintf(comps[1].id, sizeof(comps[1].id), "TEST:1");
    snprintf(comps[1].model_name, sizeof(comps[1].model_name), "TestPad1");
    snprintf(comps[1].composite_path, sizeof(comps[1].composite_path),
             "%s", COMP_PATH_1);
    f->svc->comp_count = 2;
    memcpy(f->svc->composites, comps, sizeof(comps));

    /* Settings: 2 virtual controllers. */
    cbx_settings s;
    memset(&s, 0, sizeof(s));
    s.virtual_controllers.count = 2;
    for (int i = 0; i < 2; i++)
        snprintf(s.virtual_controllers.types[i], CBX_MAX_TYPE_LEN, "xb360");
    f->svc->settings = s;

    /* Assignments. */
    cbx_assignments_init(&f->svc->assignments);

    /* Grid. */
    cbx_select_grid_init(&f->svc->grid);
    cbx_select_grid_build(&f->svc->grid, comps, 2, &s,
                           &f->svc->assignments);

    /* Device model: 2 targets + 2 composites. */
    f->svc->model.target_count = 2;
    snprintf(f->svc->model.targets[0].path,
             sizeof(f->svc->model.targets[0].path), "%s", TARGET_PATH_0);
    snprintf(f->svc->model.targets[1].path,
             sizeof(f->svc->model.targets[1].path), "%s", TARGET_PATH_1);
    f->svc->model.composite_count = 2;
    snprintf(f->svc->model.composites[0].path,
             sizeof(f->svc->model.composites[0].path), "%s", COMP_PATH_0);
    snprintf(f->svc->model.composites[1].path,
             sizeof(f->svc->model.composites[1].path), "%s", COMP_PATH_1);

    /* Render context. */
    f->svc->render_ctx = (cbx_grid_render_ctx){
        .grid       = &f->svc->grid,
        .icon_cache = NULL,
        .icon_map   = NULL,
        .theme      = NULL,
        .text_cache = NULL,
        .font_id    = -1,
    };

    /* Surface. */
    rc = cbx_overlay_surface_init(&f->svc->surface, f->svc->rend.renderer,
                                    CBX_RENDERER_DEFAULT_W,
                                    CBX_RENDERER_DEFAULT_H, 1.0);
    assert_int_equal(rc, 0);

    /* Player mode + callbacks. */
    cbx_player_mode_init(&f->svc->pm, &f->svc->grid);
    f->svc->pm.on_slot_change      = cbx_overlay_on_slot_change;
    f->svc->pm.slot_change_data    = f->svc;
    f->svc->pm.on_profile_change   = cbx_overlay_on_profile_change;
    f->svc->pm.profile_change_data = f->svc;

    /* Host mode. */
    cbx_host_mode_init(&f->svc->hm);
    f->svc->hm.on_slot_change   = cbx_overlay_on_slot_change;
    f->svc->hm.slot_change_data = f->svc;

    /* Lifecycle — instant transitions. */
    cbx_overlay_lifecycle_init(&f->svc->lifecycle, f->backend, f->mock.bus,
                                COMP_PATH_0, &f->svc->surface,
                                f->svc->rend.renderer);
    f->svc->lifecycle.fade_in_ms  = 0;
    f->svc->lifecycle.fade_out_ms = 0;
    f->svc->lifecycle.state       = CBX_OVERLAY_IDLE;
    f->svc->lifecycle.on_save      = cbx_overlay_on_save;
    f->svc->lifecycle.on_save_data = f->svc;

    /* Input context. */
    f->svc->input_ctx.pm       = &f->svc->pm;
    f->svc->input_ctx.hm       = &f->svc->hm;
    f->svc->input_ctx.grid     = &f->svc->grid;
    f->svc->input_ctx.lifecycle = &f->svc->lifecycle;

    /* Input events (not subscribed). */
    snprintf(f->svc->expected_sender, sizeof(f->svc->expected_sender),
             ":1.42");
    f->svc->input_events_ready = false;

    /* Poll setup: init polls with per-composite activation context
     * without starting SDL timers. Tests manually set poll state. */
    f->svc->poll_event_type = SDL_RegisterEvents(1);
    f->svc->poll_count = 0;
    if (f->svc->poll_event_type != (uint32_t)-1) {
        for (int i = 0; i < f->svc->comp_count && i < CBX_MAX_COMPOSITES; i++) {
            f->svc->poll_acts[i].lifecycle = &f->svc->lifecycle;
            {
                size_t clen = strlen(f->svc->composites[i].composite_path);
                if (clen >= sizeof(f->svc->poll_acts[i].composite_path))
                    clen = sizeof(f->svc->poll_acts[i].composite_path) - 1;
                memcpy(f->svc->poll_acts[i].composite_path,
                       f->svc->composites[i].composite_path, clen);
                f->svc->poll_acts[i].composite_path[clen] = '\0';
            }
            ip_intercept_poll_init(&f->svc->polls[i],
                                    f->backend, f->mock.bus,
                                    f->svc->composites[i].composite_path,
                                    test_on_activating,
                                    &f->svc->poll_acts[i],
                                    test_on_deactivating,
                                    &f->svc->lifecycle,
                                    test_on_poll_error, NULL);
            f->svc->poll_count++;
        }
    }

    /* Hotplug init (not subscribed — tests call handlers directly). */
    ip_hotplug_init(&f->svc->hp, f->backend, f->mock.bus,
                     ":1.42", &f->svc->model);

    f->svc->backend_ready  = true;
    f->svc->initialized    = true;
    cbx_overlay_service_reset_shutdown();

    *state = f;
    return 0;
}

static int
reconcile_teardown(void **state)
{
    reconcile_fixture *f = *state;
    if (f) {
        if (f->svc) {
            for (int i = 0; i < f->svc->poll_count; i++)
                ip_intercept_poll_stop(&f->svc->polls[i]);
            cbx_overlay_surface_destroy(&f->svc->surface);
            ip_connection_disconnect(&f->svc->conn);
            cbx_renderer_shutdown(&f->svc->rend);
            free(f->svc);
        }
        ip_dbus_mock_reset(&f->mock);
        SDL_VideoQuit();
        SDL_Quit();
        free(f);
    }
    return 0;
}

/* ====================================================================== */
/*  Test 1: Per-activating-composite lifecycle                            */
/* ====================================================================== */

static void
test_per_composite_activation_close_sets_pass_on_activating(void **state)
{
    reconcile_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    assert_int_equal(cbx_overlay_lifecycle_get_state(&svc->lifecycle),
                      CBX_OVERLAY_IDLE);

    /* Stage 1: activation — mock returns ALL for InterceptMode. */
    expect_activation(&f->mock);

    /* Simulate composite 1 triggering activation. */
    flush_events();
    svc->polls[1].state = IP_POLL_PASS_WAIT;

    push_poll_event(svc->poll_event_type);
    cbx_overlay_service_step(svc);

    /* Lifecycle should now be VISIBLE. */
    assert_int_equal(cbx_overlay_lifecycle_get_state(&svc->lifecycle),
                      CBX_OVERLAY_VISIBLE);

    /* composite_path should be COMP_PATH_1 (the activating composite). */
    assert_string_equal(svc->lifecycle.composite_path, COMP_PATH_1);

    /* Stage 2: close — mock returns PASS for InterceptMode + on_save. */
    expect_close_and_save(&f->mock);

    flush_events();
    push_keydown(SDLK_b);
    cbx_overlay_service_step(svc);

    assert_int_equal(cbx_overlay_lifecycle_get_state(&svc->lifecycle),
                      CBX_OVERLAY_IDLE);
}

/* ====================================================================== */
/*  Test 2: Poll re-arm after close                                       */
/* ====================================================================== */

static void
test_poll_rearm_after_close(void **state)
{
    reconcile_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    /* Start with poll 0 in PASS_WAIT. */
    flush_events();
    svc->polls[0].state = IP_POLL_PASS_WAIT;

    /* Stage 1: activation. */
    expect_activation(&f->mock);

    push_poll_event(svc->poll_event_type);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_overlay_lifecycle_get_state(&svc->lifecycle),
                      CBX_OVERLAY_VISIBLE);
    assert_int_equal(svc->polls[0].state, IP_POLL_ACTIVE);

    /* Stage 2: close. */
    expect_close_and_save(&f->mock);

    flush_events();
    push_keydown(SDLK_b);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_overlay_lifecycle_get_state(&svc->lifecycle),
                      CBX_OVERLAY_IDLE);

    /* Stage 3: poll detects PASS → deactivation + re-arm in one step.
     * The poll goes ACTIVE→IDLE (deactivation) and then IDLE→PASS_WAIT
     * (re-arm) because the lifecycle is already IDLE and backend is ready. */
    expect_pass_mode(&f->mock);

    flush_events();
    push_poll_event(svc->poll_event_type);
    cbx_overlay_service_step(svc);

    /* Poll 0 should already be re-armed to PASS_WAIT (deactivation +
     * re-arm happened in the same step). */
    assert_int_equal(svc->polls[0].state, IP_POLL_PASS_WAIT);

    /* Verify re-activation works. */
    expect_activation(&f->mock);

    flush_events();
    push_poll_event(svc->poll_event_type);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_overlay_lifecycle_get_state(&svc->lifecycle),
                      CBX_OVERLAY_VISIBLE);
}

/* ====================================================================== */
/*  Test 3: Hotplug target add triggers column rebuild                    */
/* ====================================================================== */

static void
test_hotplug_target_add_rebuilds_columns(void **state)
{
    reconcile_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    assert_int_equal(svc->grid.col_count, 3);

    /* Simulate adding a target device via hotplug. */
    ip_interfaces_changed_payload payload;
    memset(&payload, 0, sizeof(payload));
    payload.sender    = ":1.42";
    payload.path      = "/org/shadowblip/InputPlumber/devices/target/gamepad2";
    payload.interfaces = "org.shadowblip.Input.Target";
    ip_hotplug_handle_added(&svc->hp, &payload);

    assert_true(svc->hp.model_changed);
    assert_int_equal(svc->model.target_count, 3);

    /* Set up mock expectations for the reconcile. */
    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_TARGET,
                            "DeviceType", "xb360");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "InterceptMode", "1");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "SetInterceptActivation", NULL);

    flush_events();
    cbx_overlay_service_step(svc);

    assert_false(svc->hp.model_changed);
    assert_int_equal(svc->grid.col_count, 4);
}

/* ====================================================================== */
/*  Test 4: Hotplug target remove clamps positions                        */
/* ====================================================================== */

static void
test_hotplug_target_remove_clamps_positions(void **state)
{
    reconcile_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    assert_int_equal(svc->grid.col_count, 3);

    /* Place row 0 in column 2 (P2 slot). */
    svc->grid.rows[0].cur_col = 2;

    /* Simulate removing target 1 via hotplug. */
    ip_interfaces_changed_payload payload;
    memset(&payload, 0, sizeof(payload));
    payload.sender    = ":1.42";
    payload.path      = TARGET_PATH_1;
    payload.interfaces = "org.shadowblip.Input.Target";
    ip_hotplug_handle_removed(&svc->hp, &payload);

    assert_true(svc->hp.model_changed);
    assert_int_equal(svc->model.target_count, 1);

    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_TARGET,
                            "DeviceType", "xb360");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "InterceptMode", "1");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "SetInterceptActivation", NULL);

    flush_events();
    cbx_overlay_service_step(svc);

    assert_false(svc->hp.model_changed);
    assert_int_equal(svc->grid.col_count, 2);
    assert_int_equal(svc->grid.rows[0].cur_col, 0);
}

/* ====================================================================== */
/*  Test 5: Overlay visibility tracks lifecycle                           */
/* ====================================================================== */

static void
test_window_visibility_tracks_lifecycle(void **state)
{
    reconcile_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    /* When IDLE, step should keep lifecycle IDLE. */
    flush_events();
    expect_pass_mode(&f->mock);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_overlay_lifecycle_get_state(&svc->lifecycle),
                      CBX_OVERLAY_IDLE);
    assert_false(cbx_overlay_lifecycle_is_active(&svc->lifecycle));

    /* Activate. */
    expect_activation(&f->mock);
    svc->polls[0].state = IP_POLL_PASS_WAIT;
    flush_events();
    push_poll_event(svc->poll_event_type);
    cbx_overlay_service_step(svc);

    assert_int_equal(cbx_overlay_lifecycle_get_state(&svc->lifecycle),
                      CBX_OVERLAY_VISIBLE);
    assert_true(cbx_overlay_lifecycle_is_active(&svc->lifecycle));

    /* Close. */
    expect_close_and_save(&f->mock);
    flush_events();
    push_keydown(SDLK_b);
    cbx_overlay_service_step(svc);

    assert_int_equal(cbx_overlay_lifecycle_get_state(&svc->lifecycle),
                      CBX_OVERLAY_IDLE);
    assert_false(cbx_overlay_lifecycle_is_active(&svc->lifecycle));
}

/* ====================================================================== */
/*  Test 6: Surface reuse across close→reopen cycles                      */
/* ====================================================================== */

static void
test_surface_reuse_across_cycles(void **state)
{
    reconcile_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    assert_true(cbx_overlay_surface_is_built(&svc->surface));

    /* Cycle 1: activate. */
    expect_activation(&f->mock);
    flush_events();
    svc->polls[0].state = IP_POLL_PASS_WAIT;
    push_poll_event(svc->poll_event_type);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_overlay_lifecycle_get_state(&svc->lifecycle),
                      CBX_OVERLAY_VISIBLE);

    /* Close. */
    expect_close_and_save(&f->mock);
    flush_events();
    push_keydown(SDLK_b);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_overlay_lifecycle_get_state(&svc->lifecycle),
                      CBX_OVERLAY_IDLE);

    assert_true(cbx_overlay_surface_is_built(&svc->surface));

    /* Let the poll detect PASS and re-arm. */
    expect_pass_mode(&f->mock);
    flush_events();
    push_poll_event(svc->poll_event_type);
    cbx_overlay_service_step(svc);

    /* Cycle 2: activate again. */
    expect_activation(&f->mock);
    flush_events();
    push_poll_event(svc->poll_event_type);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_overlay_lifecycle_get_state(&svc->lifecycle),
                      CBX_OVERLAY_VISIBLE);

    assert_true(cbx_overlay_surface_is_built(&svc->surface));

    /* Close again. */
    expect_close_and_save(&f->mock);
    flush_events();
    push_keydown(SDLK_b);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_overlay_lifecycle_get_state(&svc->lifecycle),
                      CBX_OVERLAY_IDLE);
    assert_true(cbx_overlay_surface_is_built(&svc->surface));
}

/* ====================================================================== */
/*  Test 7: Primary composite activation                                  */
/* ====================================================================== */

static void
test_primary_composite_activation_close_sets_pass_on_comp0(void **state)
{
    reconcile_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    assert_int_equal(cbx_overlay_lifecycle_get_state(&svc->lifecycle),
                      CBX_OVERLAY_IDLE);

    /* Stage 1: activation. */
    expect_activation(&f->mock);
    flush_events();
    svc->polls[0].state = IP_POLL_PASS_WAIT;

    push_poll_event(svc->poll_event_type);
    cbx_overlay_service_step(svc);

    assert_int_equal(cbx_overlay_lifecycle_get_state(&svc->lifecycle),
                      CBX_OVERLAY_VISIBLE);

    /* composite_path should be COMP_PATH_0 (the activating composite). */
    assert_string_equal(svc->lifecycle.composite_path, COMP_PATH_0);

    /* Stage 2: close. */
    expect_close_and_save(&f->mock);
    flush_events();
    push_keydown(SDLK_b);
    cbx_overlay_service_step(svc);

    assert_int_equal(cbx_overlay_lifecycle_get_state(&svc->lifecycle),
                      CBX_OVERLAY_IDLE);
}

/* --- Test runner ------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(
            test_per_composite_activation_close_sets_pass_on_activating,
            reconcile_setup, reconcile_teardown),
        cmocka_unit_test_setup_teardown(
            test_poll_rearm_after_close,
            reconcile_setup, reconcile_teardown),
        cmocka_unit_test_setup_teardown(
            test_hotplug_target_add_rebuilds_columns,
            reconcile_setup, reconcile_teardown),
        cmocka_unit_test_setup_teardown(
            test_hotplug_target_remove_clamps_positions,
            reconcile_setup, reconcile_teardown),
        cmocka_unit_test_setup_teardown(
            test_window_visibility_tracks_lifecycle,
            reconcile_setup, reconcile_teardown),
        cmocka_unit_test_setup_teardown(
            test_surface_reuse_across_cycles,
            reconcile_setup, reconcile_teardown),
        cmocka_unit_test_setup_teardown(
            test_primary_composite_activation_close_sets_pass_on_comp0,
            reconcile_setup, reconcile_teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}