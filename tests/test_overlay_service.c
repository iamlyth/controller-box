/*
 * test_overlay_service.c — cmocka tests for run_overlay_service (Tasks 3–4, 6).
 *
 * Verifies:
 *   (a) run_overlay_service(0) with SDL_VIDEODRIVER=nonexistent returns
 *       non-zero (SDL init failure, no crash).
 *   (b) run_overlay_service(1) (dry-run) returns 0.
 *   (c) SIGTERM handler sets the shutdown flag (clean exit mechanism).
 *   (d) SIGINT handler sets the shutdown flag (clean exit mechanism).
 *   (e) Multi-controller input: two DBus InputEvent signals from different
 *       device paths move their own rows independently.
 *   (f) Unknown device path is dropped (no row movement).
 *   (g) Wrong sender is dropped (fail-closed security).
 *   (h) ip_input_events_process is a no-op on the mock backend.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include <SDL2/SDL.h>

#include "app/overlay_service.h"
#include "dbus_mock.h"
#include "dbus/ip_objectmanager.h"
#include "dbus/ip_manager.h"
#include "dbus/ip_target.h"
#include "dbus/ip_device_model.h"
#include "dbus/ip_input_signal.h"
#include "config/config_settings.h"
#include "config/config_assignments.h"
#include "ui/renderer.h"
#include "overlay/surface_build.h"

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
/*  Test (c): SIGTERM handler sets shutdown flag                      */
/* ------------------------------------------------------------------ */
static void test_sigterm_sets_shutdown_flag(void **state)
{
    (void)state;

    /* Reset and install handlers. */
    cbx_overlay_service_reset_shutdown();
    assert_int_equal(cbx_overlay_service_install_signal_handlers(), 0);

    /* Before signal: shutdown not requested. */
    assert_false(cbx_overlay_service_shutdown_requested());

    /* Send SIGTERM to self — handler sets g_running = 0. */
    raise(SIGTERM);

    /* After signal: shutdown requested. */
    assert_true(cbx_overlay_service_shutdown_requested());

    /* Reset for other tests. */
    cbx_overlay_service_reset_shutdown();
}

/* ------------------------------------------------------------------ */
/*  Test (d): SIGINT handler sets shutdown flag                       */
/* ------------------------------------------------------------------ */
static void test_sigint_sets_shutdown_flag(void **state)
{
    (void)state;

    cbx_overlay_service_reset_shutdown();
    assert_int_equal(cbx_overlay_service_install_signal_handlers(), 0);

    assert_false(cbx_overlay_service_shutdown_requested());

    raise(SIGINT);

    assert_true(cbx_overlay_service_shutdown_requested());

    cbx_overlay_service_reset_shutdown();
}

/* ================================================================== */
/*  Multi-controller input tests (Task 6)                             */
/* ================================================================== */

#define EXP_SENDER ":1.42"
#define DEV_PATH_0 "/org/shadowblip/InputPlumber/devices/dbus0"
#define DEV_PATH_1 "/org/shadowblip/InputPlumber/devices/dbus1"
#define DEV_PATH_UNKNOWN "/org/shadowblip/InputPlumber/devices/unknown"

/* Fixture for multi-controller input tests. */
typedef struct {
    ip_dbus_mock          mock;
    const ip_dbus_backend *backend;
    ip_input_events        ie;
    cbx_select_grid        grid;
    cbx_player_mode        pm;
    cbx_host_mode          hm;
    cbx_overlay_lifecycle  lifecycle;
    cbx_overlay_input_ctx  input_ctx;
    int                    slot_change_row;
    int                    slot_change_count;
} overlay_input_fixture;

/* Track slot changes to verify row dispatch. */
static int
track_slot_change(int row_idx, int new_slot, void *userdata)
{
    overlay_input_fixture *f = (overlay_input_fixture *)userdata;
    f->slot_change_row  = row_idx;
    f->slot_change_count++;
    (void)new_slot;
    return 0;
}

static int
overlay_input_setup(void **state)
{
    overlay_input_fixture *f = malloc(sizeof(*f));
    memset(f, 0, sizeof(*f));

    /* Mock DBus. */
    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);

    /* Build a grid with 2 composites (2 rows). */
    cbx_grid_composite_info comps[2];
    memset(comps, 0, sizeof(comps));
    snprintf(comps[0].id, sizeof(comps[0].id), "ORDER:0");
    snprintf(comps[0].model_name, sizeof(comps[0].model_name), "Controller 0");
    snprintf(comps[0].composite_path, sizeof(comps[0].composite_path),
             "/org/shadowblip/InputPlumber/CompositeDevice0");
    snprintf(comps[1].id, sizeof(comps[1].id), "ORDER:1");
    snprintf(comps[1].model_name, sizeof(comps[1].model_name), "Controller 1");
    snprintf(comps[1].composite_path, sizeof(comps[1].composite_path),
             "/org/shadowblip/InputPlumber/CompositeDevice1");

    cbx_settings s;
    memset(&s, 0, sizeof(s));
    s.virtual_controllers.count = 4;
    for (int i = 0; i < 4; i++)
        snprintf(s.virtual_controllers.types[i], CBX_MAX_TYPE_LEN, "xb360");

    cbx_assignments a;
    cbx_assignments_init(&a);

    cbx_select_grid_init(&f->grid);
    cbx_select_grid_build(&f->grid, comps, 2, &s, &a);

    /* Player mode on the grid. */
    cbx_player_mode_init(&f->pm, &f->grid);
    f->pm.on_slot_change    = track_slot_change;
    f->pm.slot_change_data  = f;

    /* Host mode (inactive). */
    cbx_host_mode_init(&f->hm);

    /* Lifecycle (zero-initialized — not used for player mode dispatch). */
    memset(&f->lifecycle, 0, sizeof(f->lifecycle));

    /* Overlay input context: map device paths to rows. */
    memset(&f->input_ctx, 0, sizeof(f->input_ctx));
    f->input_ctx.pm       = &f->pm;
    f->input_ctx.hm       = &f->hm;
    f->input_ctx.grid     = &f->grid;
    f->input_ctx.lifecycle = &f->lifecycle;
    cbx_overlay_input_add_mapping(&f->input_ctx, DEV_PATH_0, 0);
    cbx_overlay_input_add_mapping(&f->input_ctx, DEV_PATH_1, 1);

    /* ip_input_events: subscribe to InputEvent signals on mock bus. */
    ip_input_events_init(&f->ie, f->backend, f->mock.bus,
                          EXP_SENDER, cbx_overlay_input_cb, &f->input_ctx);
    int rc = ip_input_events_subscribe(&f->ie);
    assert_int_equal(rc, 0);

    *state = f;
    return 0;
}

static int
overlay_input_teardown(void **state)
{
    overlay_input_fixture *f = *state;
    if (f) {
        ip_dbus_mock_reset(&f->mock);
        free(f);
    }
    return 0;
}

/* Helper: inject an InputEvent signal into the mock bus. */
static void
inject_input(overlay_input_fixture *f, const char *sender,
             const char *path, const char *event, double value)
{
    ip_input_event_payload p = {
        .sender = sender,
        .path   = path,
        .event  = event,
        .value  = value,
    };
    f->backend->inject_signal(f->mock.bus,
                               IP_IFACE_DBUS_DEVICE, "InputEvent", &p);
}

/* ------------------------------------------------------------------ */
/*  Test (e): Two controllers move rows independently                  */
/* ------------------------------------------------------------------ */
static void test_multi_controller_independent_rows(void **state)
{
    overlay_input_fixture *f = *state;

    /* Both rows start at cur_col 0 (Unassigned). */
    int col0_before = cbx_select_grid_get_cur_col(&f->grid, 0);
    int col1_before = cbx_select_grid_get_cur_col(&f->grid, 1);
    assert_int_equal(col0_before, 0);
    assert_int_equal(col1_before, 0);

    /* Controller 0 sends Right → row 0 moves. */
    f->slot_change_count = 0;
    f->slot_change_row = -1;
    inject_input(f, EXP_SENDER, DEV_PATH_0, "Right", 1.0);

    int col0_after_0 = cbx_select_grid_get_cur_col(&f->grid, 0);
    int col1_after_0 = cbx_select_grid_get_cur_col(&f->grid, 1);
    assert_int_equal(col0_after_0, 1);  /* row 0 moved right */
    assert_int_equal(col1_after_0, 0);  /* row 1 unchanged */
    assert_int_equal(f->slot_change_row, 0);  /* slot change for row 0 */
    assert_int_equal(f->slot_change_count, 1);

    /* Controller 1 sends Right → row 1 moves (independently). */
    f->slot_change_count = 0;
    f->slot_change_row = -1;
    inject_input(f, EXP_SENDER, DEV_PATH_1, "Right", 1.0);

    int col0_after_1 = cbx_select_grid_get_cur_col(&f->grid, 0);
    int col1_after_1 = cbx_select_grid_get_cur_col(&f->grid, 1);
    assert_int_equal(col0_after_1, 1);  /* row 0 unchanged */
    assert_int_equal(col1_after_1, 1);  /* row 1 moved right */
    assert_int_equal(f->slot_change_row, 1);  /* slot change for row 1 */
    assert_int_equal(f->slot_change_count, 1);

    /* Controller 0 sends Left → row 0 moves back. */
    f->slot_change_count = 0;
    inject_input(f, EXP_SENDER, DEV_PATH_0, "Left", 1.0);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->grid, 0), 0);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->grid, 1), 1);
}

/* ------------------------------------------------------------------ */
/*  Test (f): Unknown device path is dropped                           */
/* ------------------------------------------------------------------ */
static void test_unknown_device_path_dropped(void **state)
{
    overlay_input_fixture *f = *state;

    int col0_before = cbx_select_grid_get_cur_col(&f->grid, 0);
    int col1_before = cbx_select_grid_get_cur_col(&f->grid, 1);

    /* Signal from an unknown device path — should be dropped. */
    f->slot_change_count = 0;
    inject_input(f, EXP_SENDER, DEV_PATH_UNKNOWN, "Right", 1.0);

    /* Neither row should have moved. */
    assert_int_equal(cbx_select_grid_get_cur_col(&f->grid, 0), col0_before);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->grid, 1), col1_before);
    assert_int_equal(f->slot_change_count, 0);
}

/* ------------------------------------------------------------------ */
/*  Test (g): Wrong sender is dropped (fail-closed)                   */
/* ------------------------------------------------------------------ */
static void test_wrong_sender_dropped(void **state)
{
    overlay_input_fixture *f = *state;

    int col0_before = cbx_select_grid_get_cur_col(&f->grid, 0);

    /* Signal from a spoofed sender — should be rejected. */
    f->slot_change_count = 0;
    inject_input(f, ":1.99", DEV_PATH_0, "Right", 1.0);

    assert_int_equal(cbx_select_grid_get_cur_col(&f->grid, 0), col0_before);
    assert_int_equal(f->slot_change_count, 0);
}

/* ------------------------------------------------------------------ */
/*  Test (h): ip_input_events_process is a no-op on mock backend       */
/* ------------------------------------------------------------------ */
static void test_ip_input_events_process_mock_noop(void **state)
{
    overlay_input_fixture *f = *state;

    /* On the mock backend, process() returns 0 (no messages to drain). */
    int rc = ip_input_events_process(&f->ie);
    assert_int_equal(rc, 0);
}

/* ================================================================== */
/*  Step function regression tests (Task 10)                            */
/* ================================================================== */

/* Fixture: minimal service context for step-function tests.
 * Allocates a cbx_overlay_service_ctx and initializes just enough
 * state to exercise the step function: SDL with dummy driver,
 * a 1-row grid, player mode, host mode, and a VISIBLE lifecycle. */
typedef struct {
    cbx_overlay_service_ctx *svc;
    ip_dbus_mock             mock;
    const ip_dbus_backend   *backend;
} step_fixture;

static int
step_setup(void **state)
{
    step_fixture *f = malloc(sizeof(*f));
    memset(f, 0, sizeof(*f));

    /* Ensure SDL is initialized with the dummy driver. */
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    SDL_VideoInit("dummy");

    /* Allocate and zero the service context. */
    f->svc = calloc(1, sizeof(cbx_overlay_service_ctx));
    assert_non_null(f->svc);

    /* Create a dummy renderer so surface operations don't crash. */
    int rc = cbx_renderer_init(&f->svc->rend, "test",
                               CBX_RENDERER_DEFAULT_W,
                               CBX_RENDERER_DEFAULT_H, false);
    assert_int_equal(rc, 0);

    /* Initialize a 1-composite grid. */
    cbx_grid_composite_info comps[1];
    memset(comps, 0, sizeof(comps));
    snprintf(comps[0].id, sizeof(comps[0].id), "TEST:0");
    snprintf(comps[0].model_name, sizeof(comps[0].model_name), "TestPad");
    snprintf(comps[0].composite_path, sizeof(comps[0].composite_path),
             "/org/shadowblip/InputPlumber/CompositeDevice0");
    f->svc->comp_count = 1;
    memcpy(f->svc->composites, comps, sizeof(comps));

    cbx_settings s;
    memset(&s, 0, sizeof(s));
    s.virtual_controllers.count = 4;
    for (int i = 0; i < 4; i++)
        snprintf(s.virtual_controllers.types[i], CBX_MAX_TYPE_LEN, "xb360");

    cbx_assignments a;
    cbx_assignments_init(&a);

    cbx_select_grid_init(&f->svc->grid);
    cbx_select_grid_build(&f->svc->grid, comps, 1, &s, &a);

    /* Initialize render context (minimal — no icons/text for tests). */
    f->svc->render_ctx = (cbx_grid_render_ctx){
        .grid       = &f->svc->grid,
        .icon_cache = NULL,
        .icon_map   = NULL,
        .theme      = NULL,
        .text_cache = NULL,
        .font_id    = -1,
    };

    /* Initialize overlay surface (needs renderer). */
    rc = cbx_overlay_surface_init(&f->svc->surface, f->svc->rend.renderer,
                                    CBX_RENDERER_DEFAULT_W,
                                    CBX_RENDERER_DEFAULT_H,
                                    1.0);
    assert_int_equal(rc, 0);

    /* Initialize player mode and host mode. */
    cbx_player_mode_init(&f->svc->pm, &f->svc->grid);
    cbx_host_mode_init(&f->svc->hm);

    /* Initialize lifecycle — set to VISIBLE so keydown events are processed. */
    cbx_overlay_lifecycle_init(&f->svc->lifecycle, NULL, NULL,
                                "", &f->svc->surface, f->svc->rend.renderer);
    f->svc->lifecycle.state = CBX_OVERLAY_VISIBLE;

    /* Wire input_ctx pointers. */
    f->svc->input_ctx.pm       = &f->svc->pm;
    f->svc->input_ctx.hm       = &f->svc->hm;
    f->svc->input_ctx.grid     = &f->svc->grid;
    f->svc->input_ctx.lifecycle = &f->svc->lifecycle;
    f->svc->input_ctx.path_count = 0;

    /* No polls, no input events for basic tests. */
    f->svc->poll_count = 0;
    f->svc->poll_event_type = (uint32_t)-1;
    f->svc->input_events_ready = false;
    f->svc->initialized = true;

    /* Reset shutdown flag. */
    cbx_overlay_service_reset_shutdown();

    *state = f;
    return 0;
}

static int
step_teardown(void **state)
{
    step_fixture *f = *state;
    if (f) {
        if (f->svc) {
            cbx_overlay_surface_destroy(&f->svc->surface);
            cbx_renderer_shutdown(&f->svc->rend);
            free(f->svc);
        }
        free(f);
    }
    /* Flush any remaining SDL events. */
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
        ;
    return 0;
}

/* Test (i): SDL_QUIT event sets shutdown flag. */
static void test_step_quit_sets_shutdown(void **state)
{
    step_fixture *f = *state;

    /* Queue a SDL_QUIT event. */
    SDL_Event quit_event = { .type = SDL_QUIT };
    SDL_PushEvent(&quit_event);

    /* Before step: not shutting down. */
    assert_false(cbx_overlay_service_shutdown_requested());

    /* Run one step. */
    cbx_overlay_service_step(f->svc);

    /* After step: shutdown requested. */
    assert_true(cbx_overlay_service_shutdown_requested());
}

/* Test (j): SDL_KEYDOWN event updates grid state. */
static void test_step_keydown_updates_grid(void **state)
{
    step_fixture *f = *state;

    /* Row 0 starts at col 0 (Unassigned). */
    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 0), 0);

    /* Queue a RIGHT keydown event. */
    SDL_Event key_event = { .type = SDL_KEYDOWN };
    key_event.key.keysym.sym = SDLK_RIGHT;
    SDL_PushEvent(&key_event);

    /* Run one step. */
    cbx_overlay_service_step(f->svc);

    /* Row 0 should have moved to col 1 (P1). */
    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 0), 1);
}

/* Test (k): Empty event queue does not crash. */
static void test_step_empty_queue_no_crash(void **state)
{
    step_fixture *f = *state;

    /* Flush any pending events. */
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
        ;

    /* Run one step — should be a no-op, no crash. */
    cbx_overlay_service_step(f->svc);

    /* Grid state unchanged. */
    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 0), 0);
    /* No shutdown requested. */
    assert_false(cbx_overlay_service_shutdown_requested());
}

/* ------------------------------------------------------------------ */
/*  Task 2: Step drains DBus unconditionally (degraded recovery fix)    */
/* ------------------------------------------------------------------ */

typedef struct {
    cbx_overlay_service_ctx *svc;
    ip_dbus_mock             mock;
    const ip_dbus_backend   *backend;
    int                      reenumerate_called;
    int                      degraded_called;
    char                     degraded_reason[256];
} conn_step_fixture;

static void conn_step_reenumerate_cb(void *ud)
{
    conn_step_fixture *f = ud;
    f->reenumerate_called++;
}

static void conn_step_degraded_cb(const char *reason, void *ud)
{
    conn_step_fixture *f = ud;
    f->degraded_called++;
    if (reason)
        snprintf(f->degraded_reason, sizeof(f->degraded_reason), "%s", reason);
}

static int
conn_step_setup(void **state)
{
    conn_step_fixture *f = malloc(sizeof(*f));
    memset(f, 0, sizeof(*f));

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    SDL_VideoInit("dummy");

    f->svc = calloc(1, sizeof(cbx_overlay_service_ctx));
    assert_non_null(f->svc);

    int rc = cbx_renderer_init(&f->svc->rend, "test",
                               CBX_RENDERER_DEFAULT_W,
                               CBX_RENDERER_DEFAULT_H, false);
    assert_int_equal(rc, 0);

    /* Set up mock DBus — InputPlumber unavailable (ServiceUnknown). */
    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);

    ip_connection_init(&f->svc->conn, f->backend);
    ip_connection_set_bus(&f->svc->conn, f->mock.bus);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_MANAGER, "Version",
                              IP_ERR_SERVICE_UNKNOWN);
    int crc = ip_connection_connect(&f->svc->conn);
    assert_int_equal(crc, IP_ERR_SERVICE_UNKNOWN);
    assert_true(ip_connection_is_degraded(&f->svc->conn));
    assert_non_null(f->svc->conn.bus);

    /* Register callbacks. */
    ip_connection_set_reenumerate_cb(&f->svc->conn, conn_step_reenumerate_cb, f);
    ip_connection_set_degraded_cb(&f->svc->conn, conn_step_degraded_cb, f);

    /* Set up minimal grid + surface for rendering. */
    cbx_grid_composite_info comps[1];
    memset(comps, 0, sizeof(comps));
    snprintf(comps[0].id, sizeof(comps[0].id), "TEST:0");
    snprintf(comps[0].model_name, sizeof(comps[0].model_name), "TestPad");
    snprintf(comps[0].composite_path, sizeof(comps[0].composite_path),
             "/org/shadowblip/InputPlumber/CompositeDevice0");
    f->svc->comp_count = 1;
    memcpy(f->svc->composites, comps, sizeof(comps));

    cbx_settings s;
    memset(&s, 0, sizeof(s));
    s.virtual_controllers.count = 4;
    for (int i = 0; i < 4; i++)
        snprintf(s.virtual_controllers.types[i], CBX_MAX_TYPE_LEN, "xb360");
    cbx_assignments a;
    cbx_assignments_init(&a);
    cbx_select_grid_init(&f->svc->grid);
    cbx_select_grid_build(&f->svc->grid, comps, 1, &s, &a);

    f->svc->render_ctx = (cbx_grid_render_ctx){
        .grid       = &f->svc->grid,
        .icon_cache = NULL,
        .icon_map   = NULL,
        .theme      = NULL,
        .text_cache = NULL,
        .font_id    = -1,
    };

    rc = cbx_overlay_surface_init(&f->svc->surface, f->svc->rend.renderer,
                                    CBX_RENDERER_DEFAULT_W,
                                    CBX_RENDERER_DEFAULT_H, 1.0);
    assert_int_equal(rc, 0);

    cbx_player_mode_init(&f->svc->pm, &f->svc->grid);
    cbx_host_mode_init(&f->svc->hm);
    cbx_overlay_lifecycle_init(&f->svc->lifecycle, NULL, NULL,
                                "", &f->svc->surface, f->svc->rend.renderer);

    f->svc->input_ctx.pm       = &f->svc->pm;
    f->svc->input_ctx.hm       = &f->svc->hm;
    f->svc->input_ctx.grid     = &f->svc->grid;
    f->svc->input_ctx.lifecycle = &f->svc->lifecycle;
    f->svc->input_ctx.path_count = 0;

    f->svc->poll_count = 0;
    f->svc->poll_event_type = (uint32_t)-1;
    f->svc->input_events_ready = false;
    f->svc->initialized = true;
    f->svc->backend_ready = false;

    cbx_overlay_service_reset_shutdown();
    *state = f;
    return 0;
}

static int
conn_step_teardown(void **state)
{
    conn_step_fixture *f = *state;
    if (f) {
        if (f->svc) {
            cbx_overlay_surface_destroy(&f->svc->surface);
            ip_connection_disconnect(&f->svc->conn);
            cbx_renderer_shutdown(&f->svc->rend);
            free(f->svc);
        }
        free(f);
    }
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
        ;
    return 0;
}

/* Test: step drains DBus even when input_events_ready is false.
 * This is the core fix for degraded-mode recovery — without unconditional
 * process(), NameOwnerChanged is never dispatched and the service is
 * stuck in degraded mode forever. */
static void
test_step_drains_dbus_when_not_ready(void **state)
{
    conn_step_fixture *f = *state;

    assert_false(f->svc->input_events_ready);
    assert_false(f->svc->backend_ready);

    /* Queue a NameOwnerChanged (acquired) signal for process() to drain. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER, "Version", "2.0.0");
    ip_dbus_mock_queue_noc(&f->mock, IP_DBUS_NAME, "", ":1.42");

    /* Call step — it should process the bus and dispatch the signal. */
    cbx_overlay_service_step(f->svc);

    /* The connection should now be connected (recovery worked). */
    assert_true(ip_connection_is_connected(&f->svc->conn));
    assert_int_equal(f->reenumerate_called, 1);
}

/* Test: step in degraded mode with no queued signals is a safe no-op. */
static void
test_step_degraded_noop(void **state)
{
    conn_step_fixture *f = *state;

    assert_true(ip_connection_is_degraded(&f->svc->conn));
    assert_false(f->svc->backend_ready);

    cbx_overlay_service_step(f->svc);

    /* Still degraded, no crash. */
    assert_true(ip_connection_is_degraded(&f->svc->conn));
    assert_int_equal(f->reenumerate_called, 0);
}


/* ------------------------------------------------------------------ */
/*  Task 5: cbx_reconcile_startup_targets tests                       */
/* ------------------------------------------------------------------ */

/* Fixture with 1 composite and 1 target (for reconcile tests). */
static const char *FIXTURE_1C1T_RECON =
    "/org/shadowblip/InputPlumber/Manager\t"
        "org.shadowblip.InputManager\n"
    "/org/shadowblip/InputPlumber/CompositeDevice0\t"
        "org.shadowblip.Input.CompositeDevice\n"
    "/org/shadowblip/InputPlumber/devices/target/xb3600\t"
        "org.shadowblip.Input.Target,org.shadowblip.Input.Gamepad\n";

/* Empty fixture (no targets). */
static const char *FIXTURE_EMPTY_RECON = "";

static int
reconcile_setup(void **state)
{
    conn_step_fixture *f = malloc(sizeof(*f));
    memset(f, 0, sizeof(*f));

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    SDL_VideoInit("dummy");

    f->svc = calloc(1, sizeof(cbx_overlay_service_ctx));
    assert_non_null(f->svc);

    int rc = cbx_renderer_init(&f->svc->rend, "test",
                               CBX_RENDERER_DEFAULT_W,
                               CBX_RENDERER_DEFAULT_H, false);
    assert_int_equal(rc, 0);

    /* Mock DBus — connected. */
    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);
    ip_connection_init(&f->svc->conn, f->backend);
    ip_connection_set_bus(&f->svc->conn, f->mock.bus);

    /* Settings: 1 target, type xb360. */
    cbx_settings_defaults(&f->svc->settings);
    f->svc->settings.virtual_controllers.count = 1;
    snprintf(f->svc->settings.virtual_controllers.types[0],
             CBX_MAX_TYPE_LEN, "xb360");

    /* Device model with 1 composite. */
    cbx_device_model_init(&f->svc->model);
    cbx_device_model_set_manager(&f->svc->model,
        "/org/shadowblip/InputPlumber/Manager");
    cbx_device_model_add_composite(&f->svc->model,
        "/org/shadowblip/InputPlumber/CompositeDevice0");

    *state = f;
    return 0;
}

static int
reconcile_teardown(void **state)
{
    conn_step_fixture *f = *state;
    if (f) {
        if (f->svc) {
            ip_connection_disconnect(&f->svc->conn);
            cbx_renderer_shutdown(&f->svc->rend);
            free(f->svc);
        }
        free(f);
    }
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
        ;
    return 0;
}

/* Test: reconcile grows from 0 to 1 target, attaches it to composite.
 * Mock returns CreateTargetDevice path + GetManagedObjects with 1 target. */
static void
test_reconcile_grow_and_attach(void **state)
{
    conn_step_fixture *f = *state;

    /* Start with 0 targets. */
    f->svc->model.target_count = 0;

    /* Expect CreateTargetDevice to return a path. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
        "CreateTargetDevice",
        "/org/shadowblip/InputPlumber/devices/target/xb3600");
    /* Expect GetManagedObjects with 1 target. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_OBJECT_MANAGER,
        "GetManagedObjects", FIXTURE_1C1T_RECON);
    /* Expect AttachTargetDevice to succeed (returns NULL = void). */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
        "AttachTargetDevice", NULL);
    /* Expect DeviceType query to return "xb360" (matches settings). */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_TARGET, "DeviceType", "xb360");

    int rc = cbx_reconcile_startup_targets(f->svc);
    assert_int_equal(rc, 0);
    assert_int_equal(f->svc->model.target_count, 1);
}

/* Test: reconcile returns error when CreateTargetDevice fails. */
static void
test_reconcile_create_fails(void **state)
{
    conn_step_fixture *f = *state;

    f->svc->model.target_count = 0;

    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_MANAGER,
        "CreateTargetDevice", IP_ERR_INVALID_ARGS);

    int rc = cbx_reconcile_startup_targets(f->svc);
    assert_true(rc < 0);
    assert_int_equal(f->svc->model.target_count, 0);
}

/* Test: reconcile returns error when GetManagedObjects fails after create. */
static void
test_reconcile_enumerate_fails(void **state)
{
    conn_step_fixture *f = *state;

    f->svc->model.target_count = 0;

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
        "CreateTargetDevice",
        "/org/shadowblip/InputPlumber/devices/target/xb3600");
    /* GetManagedObjects returns empty (no target visible = unconfirmed). */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_OBJECT_MANAGER,
        "GetManagedObjects", FIXTURE_EMPTY_RECON);
    /* For rollback: StopTargetDevice + GetManagedObjects. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
        "StopTargetDevice", NULL);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_OBJECT_MANAGER,
        "GetManagedObjects", FIXTURE_EMPTY_RECON);

    int rc = cbx_reconcile_startup_targets(f->svc);
    assert_true(rc < 0);
    /* Rollback should leave 0 targets. */
    assert_int_equal(f->svc->model.target_count, 0);
}

/* Test: reconcile shrinks from 2 to 1 target. */
static void
test_reconcile_shrink(void **state)
{
    conn_step_fixture *f = *state;

    /* Start with 2 targets (model has 2, desired is 1). */
    f->svc->model.target_count = 2;
    snprintf(f->svc->model.targets[0].path, CBX_MAX_PATH_LEN,
             "/org/shadowblip/InputPlumber/devices/target/xb3600");
    snprintf(f->svc->model.targets[1].path, CBX_MAX_PATH_LEN,
             "/org/shadowblip/InputPlumber/devices/target/xb3601");

    /* Expect StopTargetDevice to succeed. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
        "StopTargetDevice", NULL);
    /* GetManagedObjects returns 1 target. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_OBJECT_MANAGER,
        "GetManagedObjects", FIXTURE_1C1T_RECON);
    /* DeviceType for the remaining target. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_TARGET, "DeviceType", "xb360");
    /* AttachTargetDevice for the remaining target. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
        "AttachTargetDevice", NULL);

    int rc = cbx_reconcile_startup_targets(f->svc);
    assert_int_equal(rc, 0);
    assert_int_equal(f->svc->model.target_count, 1);
}


/* ------------------------------------------------------------------ */
/*  Test runner                                                        */
/* ------------------------------------------------------------------ */
static const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_sdl_init_failure_returns_nonzero),
    cmocka_unit_test(test_dry_run_returns_zero),
    cmocka_unit_test(test_sigterm_sets_shutdown_flag),
    cmocka_unit_test(test_sigint_sets_shutdown_flag),
    cmocka_unit_test_setup_teardown(test_multi_controller_independent_rows,
                                     overlay_input_setup,
                                     overlay_input_teardown),
    cmocka_unit_test_setup_teardown(test_unknown_device_path_dropped,
                                     overlay_input_setup,
                                     overlay_input_teardown),
    cmocka_unit_test_setup_teardown(test_wrong_sender_dropped,
                                     overlay_input_setup,
                                     overlay_input_teardown),
    cmocka_unit_test_setup_teardown(test_ip_input_events_process_mock_noop,
                                     overlay_input_setup,
                                     overlay_input_teardown),
    /* Step function regression tests (Task 10) */
    cmocka_unit_test_setup_teardown(test_step_quit_sets_shutdown,
                                     step_setup, step_teardown),
    cmocka_unit_test_setup_teardown(test_step_keydown_updates_grid,
                                     step_setup, step_teardown),
    cmocka_unit_test_setup_teardown(test_step_empty_queue_no_crash,
                                     step_setup, step_teardown),
    /* Task 2: unconditional DBus process + degraded recovery via step */
    cmocka_unit_test_setup_teardown(test_step_drains_dbus_when_not_ready,
                                     conn_step_setup, conn_step_teardown),
    cmocka_unit_test_setup_teardown(test_step_degraded_noop,
                                     conn_step_setup, conn_step_teardown),
    /* Task 5: reconcile startup targets */
    cmocka_unit_test_setup_teardown(test_reconcile_grow_and_attach,
                                     reconcile_setup, reconcile_teardown),
    cmocka_unit_test_setup_teardown(test_reconcile_create_fails,
                                     reconcile_setup, reconcile_teardown),
    cmocka_unit_test_setup_teardown(test_reconcile_enumerate_fails,
                                     reconcile_setup, reconcile_teardown),
    cmocka_unit_test_setup_teardown(test_reconcile_shrink,
                                     reconcile_setup, reconcile_teardown),
};

int main(void)
{
    return cmocka_run_group_tests(tests, NULL, NULL);
}