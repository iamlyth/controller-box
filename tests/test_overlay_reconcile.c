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
#include <errno.h>
#include <unistd.h>

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
#define SOURCE_PATH_0 "/org/shadowblip/InputPlumber/devices/source/event0"
#define SOURCE_PATH_1 "/org/shadowblip/InputPlumber/devices/source/event1"
#define PHYSICAL_ID_0 "USB:phys:usb-test-0"
#define PHYSICAL_ID_1 "USB:phys:usb-test-1"

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
push_poll_event(uint32_t event_type, ip_intercept_poll *owner)
{
    SDL_Event ev;
    SDL_zero(ev);
    ev.type       = event_type;
    /* Match the production SDL timer callback: the owning poll travels in
     * event.user.data1 and the arm generation in event.user.code, and the
     * step loop ticks only that live poll. */
    ev.user.code  = (Sint32)owner->generation;
    ev.user.data1 = owner;
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
    ip_dbus_backend         physical_backend;
    char                    temp_config[64];
    char                   *saved_config;
} reconcile_fixture;

/* The shared mock has one value per property.  This fixture adds a second
 * physical device while still delegating reads/errors/call accounting to
 * the mock.  Only successful identity replies for device 1 differ. */
static int
physical_get_property(ip_bus_handle bus, const char *dest, const char *path,
                      const char *iface, const char *prop, char **out)
{
    const ip_dbus_backend *mock_backend = ip_dbus_mock_backend(bus);
    int rc = mock_backend->get_property(bus, dest, path, iface, prop, out);
    if (rc != 0)
        return rc;
    const char *value = NULL;
    if (strcmp(path, COMP_PATH_1) == 0 &&
        strcmp(iface, IP_IFACE_COMPOSITE) == 0 &&
        strcmp(prop, "SourceDevicePaths") == 0)
        value = SOURCE_PATH_1;
    if (strcmp(path, SOURCE_PATH_1) == 0 &&
        strcmp(iface, IP_IFACE_SOURCE_EVENT) == 0 &&
        strcmp(prop, "PhysPath") == 0)
        value = "usb-test-1";
    if (value) {
        free(*out);
        *out = strdup(value);
        if (!*out)
            return -ENOMEM;
    }
    return 0;
}

static void
stage_composite_reconcile_expectations(ip_dbus_mock *mock)
{
    ip_dbus_mock_reset(mock);
    ip_dbus_mock_expect_ok(mock, IP_IFACE_COMPOSITE, "SourceDevicePaths", "");
    ip_dbus_mock_expect_ok(mock, IP_IFACE_COMPOSITE, "DbusDevices", "");
    ip_dbus_mock_expect_ok(mock, IP_IFACE_COMPOSITE, "TargetDevices", "");
    ip_dbus_mock_expect_ok(mock, IP_IFACE_MANAGER, "GamepadOrder", NULL);
    ip_dbus_mock_expect_ok(mock, IP_IFACE_TARGET, "DeviceType", "xb360");
    ip_dbus_mock_expect_ok(mock, IP_IFACE_COMPOSITE, "InterceptMode", "1");
    ip_dbus_mock_expect_ok(mock, IP_IFACE_COMPOSITE,
                            "SetInterceptActivation", NULL);
}

static void
stage_physical_reconcile(reconcile_fixture *f)
{
    stage_composite_reconcile_expectations(&f->mock);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "SourceDevicePaths", SOURCE_PATH_0);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_EVENT, "UniqueId", "");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_EVENT,
                            "PhysPath", "usb-test-0");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_EVENT, "IdBustype", "3");
    f->physical_backend = *f->backend;
    f->physical_backend.get_property = physical_get_property;
    f->svc->conn.backend = &f->physical_backend;
}

static int
reconcile_setup(void **state)
{
    reconcile_fixture *f = malloc(sizeof(*f));
    memset(f, 0, sizeof(*f));

    /* Saved order is read during every successful reconcile. */
    const char *config = getenv("XDG_CONFIG_HOME");
    f->saved_config = config ? strdup(config) : NULL;
    snprintf(f->temp_config, sizeof(f->temp_config), "/tmp/cbx-or-XXXXXX");
    assert_non_null(mkdtemp(f->temp_config));
    assert_int_equal(setenv("XDG_CONFIG_HOME", f->temp_config, 1), 0);

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
    f->svc->model.composites[0].index = 0;
    snprintf(f->svc->model.composites[1].path,
             sizeof(f->svc->model.composites[1].path), "%s", COMP_PATH_1);
    f->svc->model.composites[1].index = 1;

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
        char path[256];
        snprintf(path, sizeof(path), "%s/controller-box/assignments.yaml",
                 f->temp_config);
        unlink(path);
        snprintf(path, sizeof(path), "%s/controller-box/.controller-box.lock",
                 f->temp_config);
        unlink(path);
        snprintf(path, sizeof(path), "%s/controller-box", f->temp_config);
        rmdir(path);
        assert_int_equal(rmdir(f->temp_config), 0);
        if (f->saved_config)
            setenv("XDG_CONFIG_HOME", f->saved_config, 1);
        else
            unsetenv("XDG_CONFIG_HOME");
        free(f->saved_config);
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

    push_poll_event(svc->poll_event_type, &svc->polls[1]);
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

    push_poll_event(svc->poll_event_type, &svc->polls[0]);
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
    push_poll_event(svc->poll_event_type, &svc->polls[0]);
    cbx_overlay_service_step(svc);

    /* Poll 0 should already be re-armed to PASS_WAIT (deactivation +
     * re-arm happened in the same step). */
    assert_int_equal(svc->polls[0].state, IP_POLL_PASS_WAIT);

    /* Verify re-activation works. */
    expect_activation(&f->mock);

    flush_events();
    push_poll_event(svc->poll_event_type, &svc->polls[0]);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_overlay_lifecycle_get_state(&svc->lifecycle),
                      CBX_OVERLAY_VISIBLE);
}

/* ====================================================================== */
/*  Test 2b: Rearm timer ownership and rollback                           */
/* ====================================================================== */

/* A successful rearm arms exactly one timer per composite and covers the
 * whole managed poll range, so cleanup/dispatch can never miss an armed
 * poll. */
static void
test_rearm_success_arms_every_composite(void **state)
{
    reconcile_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    int rc = cbx_overlay_rearm_polls(svc);
    assert_int_equal(rc, 0);
    assert_int_equal(svc->poll_count, svc->comp_count);
    for (int i = 0; i < svc->comp_count; i++) {
        assert_int_not_equal(svc->polls[i].timer_id, 0);
        assert_int_equal(svc->polls[i].state, IP_POLL_PASS_WAIT);
    }
}

/* If one composite cannot arm its timer, the earlier successful arm must be
 * rolled back: a sparse partial arming cannot leave an active poll outside
 * poll_count/cleanup bounds.  Composite 1 is made unstartable with an empty
 * composite path (start() returns -EINVAL) after composite 0 would have
 * armed successfully. */
static void
test_rearm_partial_start_failure_stops_all_timers(void **state)
{
    reconcile_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    snprintf(svc->composites[1].composite_path,
             sizeof(svc->composites[1].composite_path), "%s", "");

    int rc = cbx_overlay_rearm_polls(svc);
    assert_int_equal(rc, -EIO);
    assert_int_equal(svc->poll_count, 0);

    for (int i = 0; i < CBX_MAX_COMPOSITES; i++) {
        assert_int_equal(svc->polls[i].timer_id, 0);
        assert_int_equal(svc->polls[i].state, IP_POLL_IDLE);
    }
}

/* Regression for the generation reset across rebuilds: the production
 * rebuild path (stop_all → init → start, exactly what cbx_overlay_rearm_polls
 * runs for hotplug/recovery) must not reset a slot's arm generation to a
 * value already carried by events queued before the rebuild, or the step
 * loop accepts a stale pre-rebuild event and ticks a poll that now belongs
 * to a different composite.  The older generation+7 test could not catch
 * this because it never re-initialized the slot between arms. */
static void
test_rearm_rejects_pre_rebuild_stale_event(void **state)
{
    reconcile_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    /* Arm slot 0 through the real production path and record the first
     * arm's generation. */
    int rc = cbx_overlay_rearm_polls(svc);
    assert_int_equal(rc, 0);
    uint32_t first_gen = svc->polls[0].generation;

    /* The old timer queued this event before the rebuild. */
    SDL_Event stale;
    SDL_zero(stale);
    stale.user.code  = (Sint32)first_gen;
    stale.user.data1 = &svc->polls[0];

    /* Rebuild through the real production path (stop_all → init → start);
     * slot 0 is now reused for a different composite. */
    snprintf(svc->composites[0].composite_path,
             sizeof(svc->composites[0].composite_path), "%s", COMP_PATH_1);
    rc = cbx_overlay_rearm_polls(svc);
    assert_int_equal(rc, 0);
    assert_int_not_equal(svc->polls[0].generation, first_gen);

    /* Isolate the synthetic stale event from the freshly armed real timers:
     * those timers still push the event type captured by start(), while the
     * step is told to recognize a distinct type.  A live timer event can
     * therefore not tick a poll here, so a non-zero read count can only come
     * from an accepted stale event. */
    svc->poll_event_type = SDL_RegisterEvents(1);
    assert_int_not_equal(svc->poll_event_type, (uint32_t)-1);
    stale.type = svc->poll_event_type;

    ip_dbus_mock_reset(&f->mock);

    SDL_PushEvent(&stale);
    cbx_overlay_service_step(svc);

    /* Stale pre-rebuild event rejected: the rebuilt slot was not ticked. */
    assert_int_equal(f->mock.get_property_count, 0);
    assert_int_equal(svc->polls[0].error_count, 0);
    assert_int_equal(svc->polls[0].state, IP_POLL_PASS_WAIT);
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

    /* Successful source reads distinguish ORDER fallback from read failure. */
    stage_composite_reconcile_expectations(&f->mock);

    flush_events();
    cbx_overlay_service_step(svc);

    assert_false(svc->hp.model_changed);
    assert_int_equal(svc->grid.col_count, 4);
    assert_true(svc->backend_ready);

    /* The hotplug pass bounds its synchronous DBus chain with a nonzero
     * deadline and clears it on exit (efficiency BLOCKER: no unbounded
     * per-call/per-pass chain). */
    assert_true(f->mock.set_deadline_count >= 2);
    assert_int_equal(f->mock.deadline_ms, 0);
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

    /* Rebuild derives positions from saved assignments, not old cur_col.
     * Give this controller a real P2 preference so this proves clamping,
     * rather than merely rebuilding an already-unassigned controller. */
    svc->assignments.assignment_count = 1;
    snprintf(svc->assignments.assignments[0].id, CBX_MAX_ID_LEN,
             "%s", "ORDER:0");
    svc->assignments.assignments[0].slot = 1;
    snprintf(svc->grid.rows[0].id, CBX_MAX_ID_LEN, "%s", "ORDER:0");
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

    stage_composite_reconcile_expectations(&f->mock);

    flush_events();
    cbx_overlay_service_step(svc);

    assert_false(svc->hp.model_changed);
    assert_int_equal(svc->grid.col_count, 2);
    assert_string_equal(svc->grid.rows[0].id, "ORDER:0");
    assert_int_equal(svc->grid.rows[0].cur_col, 0);
    assert_int_equal(svc->assignments.assignments[0].slot, 1);
    assert_true(svc->backend_ready);
    assert_true(f->mock.target_devices_written);
    assert_string_equal(f->mock.target_devices_value, "");
}

/* ====================================================================== */
/*  Test 4b: Hotplug while Host Mode is active (SPEC §4.4/§10.1)          */
/* ====================================================================== */

/* Align host-mode rows with the distinct source-derived physical identities
 * returned by stage_physical_reconcile, never opaque PersistentId values. */
static void
sync_grid_identity_from_model(cbx_overlay_service_ctx *svc)
{
    for (int i = 0; i < svc->grid.row_count &&
                    i < svc->model.composite_count; i++) {
        snprintf(svc->grid.rows[i].id, CBX_MAX_ID_LEN, "%s",
                 i == 0 ? PHYSICAL_ID_0 : PHYSICAL_ID_1);
        svc->grid.rows[i].id_stable = true;
        snprintf(svc->grid.rows[i].composite_path, CBX_MAX_PATH_LEN, "%s",
                 svc->model.composites[i].path);
    }
}

/* Hot-unplugging the host must exit Host Mode, not freeze every input. */
static void
test_hotplug_host_removed_exits_host_mode(void **state)
{
    reconcile_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    sync_grid_identity_from_model(svc);

    /* Controller 0 (composite-0) enters host mode. */
    assert_int_equal(
        cbx_host_mode_toggle_with_grid(&svc->hm, &svc->grid, 0), 1);
    assert_true(cbx_host_mode_is_active(&svc->hm));
    assert_int_equal(cbx_host_mode_get_host_row(&svc->hm), 0);

    /* Hot-unplug the host composite via the production hotplug handler. */
    ip_interfaces_changed_payload payload;
    memset(&payload, 0, sizeof(payload));
    payload.sender     = ":1.42";
    payload.path       = COMP_PATH_0;
    payload.interfaces = IP_IFACE_COMPOSITE;
    ip_hotplug_handle_removed(&svc->hp, &payload);
    assert_true(svc->hp.model_changed);
    assert_int_equal(svc->model.composite_count, 1);
    assert_true(svc->hp.identity_changed);

    stage_physical_reconcile(f);

    flush_events();
    cbx_overlay_service_step(svc);

    assert_false(svc->hp.model_changed);
    assert_int_equal(svc->grid.row_count, 1);
    assert_string_equal(svc->grid.rows[0].id, PHYSICAL_ID_1);
    assert_true(svc->backend_ready);
    assert_false(svc->hp.identity_changed);
    /* Host gone → host mode exited; the surviving controller is not frozen. */
    assert_false(cbx_host_mode_is_active(&svc->hm));
    assert_false(cbx_host_mode_is_frozen(&svc->hm, 0));
}

/*
 * Removing a controller before the host shifts the host's row index.  The
 * reconcile must follow the host's persistent identity so host privilege
 * never lands on a different physical controller.
 */
static void
test_hotplug_host_row_shift_preserves_host(void **state)
{
    reconcile_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    sync_grid_identity_from_model(svc);

    /* Controller 1 (composite-1) is the host. */
    assert_int_equal(
        cbx_host_mode_toggle_with_grid(&svc->hm, &svc->grid, 1), 1);
    assert_int_equal(cbx_host_mode_get_host_row(&svc->hm), 1);

    /* Hot-unplug composite-0 (not the host). */
    ip_interfaces_changed_payload payload;
    memset(&payload, 0, sizeof(payload));
    payload.sender     = ":1.42";
    payload.path       = COMP_PATH_0;
    payload.interfaces = IP_IFACE_COMPOSITE;
    ip_hotplug_handle_removed(&svc->hp, &payload);
    assert_true(svc->hp.model_changed);
    assert_true(svc->hp.identity_changed);

    stage_physical_reconcile(f);

    flush_events();
    cbx_overlay_service_step(svc);

    assert_false(svc->hp.model_changed);
    assert_int_equal(svc->grid.row_count, 1);
    /* Host followed its persistent id to the new row 0; privilege did not
     * shift to any other controller and the host is not frozen. */
    assert_true(cbx_host_mode_is_active(&svc->hm));
    assert_int_equal(cbx_host_mode_get_host_row(&svc->hm), 0);
    assert_string_equal(svc->grid.rows[0].id, PHYSICAL_ID_1);
    assert_true(svc->backend_ready);
    assert_false(svc->hp.identity_changed);
    assert_false(cbx_host_mode_is_frozen(&svc->hm, 0));
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
    push_poll_event(svc->poll_event_type, &svc->polls[0]);
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
    push_poll_event(svc->poll_event_type, &svc->polls[0]);
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
    push_poll_event(svc->poll_event_type, &svc->polls[0]);
    cbx_overlay_service_step(svc);

    /* Cycle 2: activate again. */
    expect_activation(&f->mock);
    flush_events();
    push_poll_event(svc->poll_event_type, &svc->polls[0]);
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

    push_poll_event(svc->poll_event_type, &svc->polls[0]);
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

/* ====================================================================== */
/*  Test 8: Hotplug composite add restores the saved assignment on engine  */
/* ====================================================================== */

/*
 * A controller that reconnects mid-session with a saved slot/profile must
 * become effective on the live engine immediately, not merely appear in the
 * grid.  This drives a composite InterfacesAdded through the production step
 * loop (cbx_overlay_service_step -> cbx_overlay_reconcile_hotplug) with a
 * saved phys-path assignment and asserts the exact TargetDevices, the
 * LoadProfilePath, and the GamepadOrder all reach the engine (SPEC §6.2,
 * task 6 acceptance: "production startup, hotplug and owner reacquisition").
 */
static void
test_hotplug_composite_add_restores_saved_assignment(void **state)
{
    reconcile_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    /* Start from an empty physical topology; the reconnecting composite will
     * derive its identity from its source devices. */
    svc->model.composite_count = 0;
    svc->comp_count = 0;

    /* Saved preference: phys-path identity -> P2 (slot 1) with a profile. */
    cbx_assignments_init(&svc->assignments);
    cbx_assignment saved;
    memset(&saved, 0, sizeof(saved));
    snprintf(saved.id, sizeof(saved.id), "%s", "USB:phys:usb-3-1");
    saved.slot = 1;
    snprintf(saved.profile, sizeof(saved.profile), "%s", "custom");
    svc->assignments.assignments[0] = saved;
    svc->assignments.assignment_count = 1;

    /* Profile list so the saved profile resolves to a real path. */
    memset(&svc->profiles, 0, sizeof(svc->profiles));
    snprintf(svc->profiles.entries[0].filename,
             sizeof(svc->profiles.entries[0].filename), "%s", "custom");
    snprintf(svc->profiles.entries[0].path,
             sizeof(svc->profiles.entries[0].path), "%s",
             "/tmp/cbx/custom.yaml");
    svc->profiles.count = 1;
    cbx_profile_cycle_init(&svc->profile_cycle, f->backend, f->mock.bus,
                            &svc->assignments, &svc->profiles);

    /* Source-derived identity for the reconnecting composite. */
    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "SourceDevicePaths",
                            "/org/shadowblip/InputPlumber/devices/source/event0");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_EVENT, "UniqueId", "");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_EVENT, "PhysPath",
                            "usb-3-1");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_EVENT, "IdBustype", "3");

    /* Engine apply expectations: profile load + verified read-back, exact
     * TargetDevices set + read-back, and GamepadOrder. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE, "LoadProfilePath",
                            NULL);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE, "ProfilePath",
                            "/tmp/cbx/custom.yaml");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE, "TargetDevices", "");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER, "GamepadOrder", NULL);

    /* Let the rest of the reconcile complete normally. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE, "DbusDevices", "");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE, "InterceptMode", "1");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "SetInterceptActivation", NULL);

    /* Drive the production hotplug handler for a composite add. */
    ip_interfaces_changed_payload payload;
    memset(&payload, 0, sizeof(payload));
    payload.sender     = ":1.42";
    payload.path       = COMP_PATH_0;
    payload.interfaces = IP_IFACE_COMPOSITE;
    ip_hotplug_handle_added(&svc->hp, &payload);
    assert_true(svc->hp.model_changed);
    assert_int_equal(svc->model.composite_count, 1);

    flush_events();
    cbx_overlay_service_step(svc);

    /* The rebuilt row holds the saved slot/profile... */
    assert_int_equal(svc->grid.row_count, 1);
    assert_string_equal(svc->grid.rows[0].id, "USB:phys:usb-3-1");
    assert_int_equal(svc->grid.rows[0].cur_col, 2);   /* slot 1 -> P2 */
    assert_string_equal(svc->grid.rows[0].profile, "custom");

    /* ...and the engine actually received the restored routing state. */
    assert_string_equal(f->mock.target_devices_value, TARGET_PATH_1);
    assert_int_equal(ip_dbus_mock_call_count(&f->mock, IP_IFACE_COMPOSITE,
                                              "LoadProfilePath"), 1);
    assert_true(f->mock.gamepad_order_written);
    assert_string_equal(f->mock.gamepad_order_value, COMP_PATH_0);
}

/*
 * A persisted profile that no longer exists on disk must not block
 * restoration forever when a valid alternative exists (task 6 acceptance:
 * invalid preferred properties with valid alternatives).  The hotplug
 * engine apply falls back to the built-in default, loads it, and updates
 * the row so the UI/persisted table reflects the profile the engine holds.
 */
static void
test_hotplug_composite_add_stale_profile_falls_back(void **state)
{
    reconcile_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    svc->model.composite_count = 0;
    svc->comp_count = 0;

    cbx_assignments_init(&svc->assignments);
    cbx_assignment saved;
    memset(&saved, 0, sizeof(saved));
    snprintf(saved.id, sizeof(saved.id), "%s", "USB:phys:usb-3-1");
    saved.slot = 1;
    snprintf(saved.profile, sizeof(saved.profile), "%s", "missing");
    svc->assignments.assignments[0] = saved;
    svc->assignments.assignment_count = 1;

    /* The saved profile is not present; default is a valid alternative. */
    memset(&svc->profiles, 0, sizeof(svc->profiles));
    snprintf(svc->profiles.entries[0].filename,
             sizeof(svc->profiles.entries[0].filename), "%s", "default");
    snprintf(svc->profiles.entries[0].path,
             sizeof(svc->profiles.entries[0].path), "%s",
             "/tmp/cbx/default.yaml");
    snprintf(svc->profiles.entries[1].filename,
             sizeof(svc->profiles.entries[1].filename), "%s", "custom");
    snprintf(svc->profiles.entries[1].path,
             sizeof(svc->profiles.entries[1].path), "%s",
             "/tmp/cbx/custom.yaml");
    svc->profiles.count = 2;
    cbx_profile_cycle_init(&svc->profile_cycle, f->backend, f->mock.bus,
                            &svc->assignments, &svc->profiles);

    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "SourceDevicePaths",
                            "/org/shadowblip/InputPlumber/devices/source/event0");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_EVENT, "UniqueId", "");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_EVENT, "PhysPath",
                            "usb-3-1");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_EVENT, "IdBustype", "3");

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE, "LoadProfilePath",
                            NULL);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE, "ProfilePath",
                            "/tmp/cbx/default.yaml");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE, "TargetDevices", "");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER, "GamepadOrder", NULL);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE, "DbusDevices", "");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE, "InterceptMode", "1");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "SetInterceptActivation", NULL);

    ip_interfaces_changed_payload payload;
    memset(&payload, 0, sizeof(payload));
    payload.sender     = ":1.42";
    payload.path       = COMP_PATH_0;
    payload.interfaces = IP_IFACE_COMPOSITE;
    ip_hotplug_handle_added(&svc->hp, &payload);
    assert_true(svc->hp.model_changed);

    flush_events();
    cbx_overlay_service_step(svc);

    /* Restoration succeeded with the fallback profile, not a hard failure. */
    assert_int_equal(svc->backend_ready, true);
    assert_int_equal(svc->grid.row_count, 1);
    assert_string_equal(svc->grid.rows[0].profile, "default");
    assert_string_equal(f->mock.target_devices_value, TARGET_PATH_1);
    assert_int_equal(ip_dbus_mock_call_count(&f->mock, IP_IFACE_COMPOSITE,
                                              "LoadProfilePath"), 1);
    assert_string_equal(f->mock.gamepad_order_value, COMP_PATH_0);
}

/* Persist an order deliberately opposite to the player-slot ordering. */
static void
save_reversed_physical_order(cbx_overlay_service_ctx *svc)
{
    cbx_assignments_init(&svc->assignments);
    svc->assignments.assignment_count = 2;
    svc->assignments.gamepad_order_count = 2;
    for (int i = 0; i < 2; i++) {
        snprintf(svc->assignments.assignments[i].id, CBX_MAX_ID_LEN, "%s",
                 i == 0 ? PHYSICAL_ID_0 : PHYSICAL_ID_1);
        svc->assignments.assignments[i].slot = i;
        snprintf(svc->assignments.gamepad_order[i], CBX_MAX_ID_LEN, "%s",
                 i == 0 ? PHYSICAL_ID_1 : PHYSICAL_ID_0);
    }
    assert_int_equal(cbx_assignments_save(&svc->assignments), 0);
}

static void
assert_saved_physical_order(void)
{
    cbx_assignments saved;
    cbx_assignments_init(&saved);
    assert_int_equal(cbx_assignments_load(&saved), 0);
    assert_int_equal(saved.assignment_count, 2);
    assert_int_equal(saved.gamepad_order_count, 2);
    assert_string_equal(saved.gamepad_order[0], PHYSICAL_ID_1);
    assert_string_equal(saved.gamepad_order[1], PHYSICAL_ID_0);
    for (int i = 0; i < 2; i++) {
        assert_string_equal(saved.assignments[i].id,
                            i == 0 ? PHYSICAL_ID_0 : PHYSICAL_ID_1);
        assert_int_equal(saved.assignments[i].slot, i);
    }
}

/* Source removal leaves composite cardinality unchanged.  Its identity
 * invalidation flag must survive the service-step boundary, and a rebuilt
 * slot grid must not overwrite the separately saved GamepadOrder. */
static void
test_hotplug_saved_order_and_source_removal(void **state)
{
    reconcile_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;
    save_reversed_physical_order(svc);
    /* An auxiliary source comes and goes; the two gamepads' own source
     * properties remain unchanged but must be queried on both passes. */
    ip_interfaces_changed_payload source = {
        .sender = ":1.42",
        .path = "/org/shadowblip/InputPlumber/devices/source/event2",
        .interfaces = IP_IFACE_SOURCE_EVENT,
    };

    for (int removing = 0; removing < 2; removing++) {
        stage_physical_reconcile(f);
        if (removing)
            ip_hotplug_handle_removed(&svc->hp, &source);
        else
            ip_hotplug_handle_added(&svc->hp, &source);
        assert_true(svc->hp.model_changed);
        assert_true(svc->hp.identity_changed);
        assert_int_equal(svc->model.composite_count, 2);
        assert_int_equal(svc->model.source_count, removing ? 0 : 1);

        flush_events();
        cbx_overlay_service_step(svc);

        assert_true(svc->backend_ready);
        assert_true(svc->identities_valid);
        assert_false(svc->hp.model_changed);
        assert_false(svc->hp.identity_changed);
        assert_false(svc->identity_reconcile_pending);
        assert_int_equal(ip_dbus_mock_call_count(&f->mock, IP_IFACE_COMPOSITE,
                                                 "SourceDevicePaths"), 2);
        assert_int_equal(svc->grid.rows[0].cur_col, 1);
        assert_int_equal(svc->grid.rows[1].cur_col, 2);
        assert_string_equal(svc->grid.rows[0].id, PHYSICAL_ID_0);
        assert_string_equal(svc->grid.rows[1].id, PHYSICAL_ID_1);
        assert_true(f->mock.target_devices_written);
        assert_string_equal(f->mock.target_devices_value, TARGET_PATH_1);
        assert_true(f->mock.gamepad_order_written);
        assert_string_equal(f->mock.gamepad_order_value,
                            COMP_PATH_1 "," COMP_PATH_0);
        assert_saved_physical_order();
    }
}

static void
check_uncertain_identity_no_writes(reconcile_fixture *f, bool duplicate)
{
    cbx_overlay_service_ctx *svc = f->svc;
    save_reversed_physical_order(svc);
    stage_physical_reconcile(f);
    if (duplicate) {
        /* Both composites expose the same physical source and identity. */
        svc->conn.backend = f->backend;
    } else {
        ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                                  "SourceDevicePaths", IP_ERR_NO_REPLY);
    }
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                            "AttachTargetDevice", NULL);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "LoadProfilePath", NULL);
    ip_interfaces_changed_payload source = {
        .sender = ":1.42", .path = SOURCE_PATH_0,
        .interfaces = IP_IFACE_SOURCE_EVENT,
    };
    ip_hotplug_handle_added(&svc->hp, &source);
    assert_true(svc->hp.identity_changed);
    flush_events();
    cbx_overlay_service_step(svc);

    assert_false(svc->backend_ready);
    assert_false(svc->identities_valid);
    assert_non_null(strstr(svc->readiness_detail, "identity enumeration"));
    assert_int_equal(ip_dbus_mock_call_count(&f->mock, IP_IFACE_COMPOSITE,
                                             "SourceDevicePaths"), 2);
    /* No partially rebuilt grid, routing/profile mutation, or order write. */
    assert_int_equal(svc->grid.row_count, 2);
    assert_string_equal(svc->grid.rows[0].id, "TEST:0");
    assert_string_equal(svc->grid.rows[1].id, "TEST:1");
    assert_false(f->mock.target_devices_written);
    assert_false(f->mock.gamepad_order_written);
    assert_int_equal(f->mock.set_property_count, 0);
    assert_int_equal(ip_dbus_mock_call_count(&f->mock, IP_IFACE_MANAGER,
                                             "AttachTargetDevice"), 0);
    assert_int_equal(ip_dbus_mock_call_count(&f->mock, IP_IFACE_COMPOSITE,
                                             "LoadProfilePath"), 0);
    assert_saved_physical_order();
}

static void
test_hotplug_duplicate_identity_no_writes(void **state)
{
    check_uncertain_identity_no_writes(*state, true);
}

static void
test_hotplug_identity_read_failure_no_writes(void **state)
{
    check_uncertain_identity_no_writes(*state, false);
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
            test_rearm_success_arms_every_composite,
            reconcile_setup, reconcile_teardown),
        cmocka_unit_test_setup_teardown(
            test_rearm_partial_start_failure_stops_all_timers,
            reconcile_setup, reconcile_teardown),
        cmocka_unit_test_setup_teardown(
            test_rearm_rejects_pre_rebuild_stale_event,
            reconcile_setup, reconcile_teardown),
        cmocka_unit_test_setup_teardown(
            test_hotplug_target_add_rebuilds_columns,
            reconcile_setup, reconcile_teardown),
        cmocka_unit_test_setup_teardown(
            test_hotplug_target_remove_clamps_positions,
            reconcile_setup, reconcile_teardown),
        cmocka_unit_test_setup_teardown(
            test_hotplug_host_removed_exits_host_mode,
            reconcile_setup, reconcile_teardown),
        cmocka_unit_test_setup_teardown(
            test_hotplug_host_row_shift_preserves_host,
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
        cmocka_unit_test_setup_teardown(
            test_hotplug_composite_add_restores_saved_assignment,
            reconcile_setup, reconcile_teardown),
        cmocka_unit_test_setup_teardown(
            test_hotplug_composite_add_stale_profile_falls_back,
            reconcile_setup, reconcile_teardown),
        cmocka_unit_test_setup_teardown(
            test_hotplug_saved_order_and_source_removal,
            reconcile_setup, reconcile_teardown),
        cmocka_unit_test_setup_teardown(
            test_hotplug_duplicate_identity_no_writes,
            reconcile_setup, reconcile_teardown),
        cmocka_unit_test_setup_teardown(
            test_hotplug_identity_read_failure_no_writes,
            reconcile_setup, reconcile_teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}