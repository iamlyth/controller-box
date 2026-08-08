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
#include "dbus/ip_input_signal.h"
#include "config/config_settings.h"
#include "config/config_assignments.h"

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
};

int main(void)
{
    return cmocka_run_group_tests(tests, NULL, NULL);
}