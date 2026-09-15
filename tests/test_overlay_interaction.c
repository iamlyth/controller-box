/*
 * test_overlay_interaction.c — Overlay production-dispatch interaction tests.
 *
 * Exercises overlay actions O01–O12 from the interaction acceptance
 * inventory through the production dispatch path (cbx_overlay_service_step),
 * verifying semantic outcomes: grid state, profile changes, assignment
 * sync, lifecycle transitions, and DBus mock expectations.  O13 (Player
 * Mode conflict) is covered by test_overlay_native.c with native DBus.
 * This file also covers the W1/W2 host-mode dirty triggers (surface
 * re-render on host-mode entry/exit) — these are NOT the O13 inventory ID
 * (see note at the Host-mode dirty-trigger section below).
 *
 * SPEC §5.7, §4.3–4.5, §11.2 item 3.
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
#include "config/config_paths.h"
#include "ui/renderer.h"
#include "overlay/surface_build.h"
#include "overlay/lifecycle.h"
#include "overlay/grid_render.h"
#include "overlay/player_mode.h"
#include "overlay/host_mode.h"
#include "overlay/conflict.h"
#include "overlay/dynamic_columns.h"

/* --- Constants --------------------------------------------------------- */

#define EXP_SENDER  ":1.42"
#define DEV_PATH_0  "/org/shadowblip/InputPlumber/devices/dbus0"
#define DEV_PATH_1  "/org/shadowblip/InputPlumber/devices/dbus1"

/* --- Fixture ----------------------------------------------------------- */

typedef struct {
    cbx_overlay_service_ctx *svc;
    ip_dbus_mock             mock;
    const ip_dbus_backend   *backend;

    /* Tracking counters for callbacks. */
    int slot_change_count;
    int slot_change_row;
    int profile_change_count;
    int profile_change_row;
    char profile_change_name[64];
    int save_count;
} interaction_fixture;

/* --- Test-local poll callbacks (trivial wrappers over production API) -- */

static void
test_on_activating(void *userdata)
{
    cbx_overlay_lifecycle *lc = (cbx_overlay_lifecycle *)userdata;
    cbx_overlay_lifecycle_activate(lc);
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
    ev.type        = SDL_KEYDOWN;
    ev.key.keysym.sym = sym;
    SDL_PushEvent(&ev);
}

static void
push_poll_event(uint32_t event_type, ip_intercept_poll *owner)
{
    SDL_Event ev;
    SDL_zero(ev);
    ev.type       = event_type;
    ev.user.code  = 0;
    /* The production SDL timer callback carries the owning poll in
     * event.user.data1; the step loop ticks only that poll. */
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

static void
make_visible(interaction_fixture *f)
{
    f->svc->lifecycle.state = CBX_OVERLAY_VISIBLE;
}


/* --- Setup / Teardown -------------------------------------------------- */

static int
interaction_setup(void **state)
{
    interaction_fixture *f = malloc(sizeof(*f));
    memset(f, 0, sizeof(*f));

    /* SDL with dummy video driver + timer for poll re-arm during reconcile. */
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER);
    SDL_VideoInit("dummy");

    /* Allocate service context. */
    f->svc = calloc(1, sizeof(cbx_overlay_service_ctx));
    assert_non_null(f->svc);

    /* Renderer. */
    int rc = cbx_renderer_init(&f->svc->rend, "test",
                               CBX_RENDERER_DEFAULT_W,
                               CBX_RENDERER_DEFAULT_H, false);
    assert_int_equal(rc, 0);

    /* Mock DBus. */
    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);

    /* Connection (for on_profile_change DBus calls). */
    ip_connection_init(&f->svc->conn, f->backend);
    ip_connection_set_bus(&f->svc->conn, f->mock.bus);

    /* 2 composites (needed for multi-controller and host-mode tests). */
    cbx_grid_composite_info comps[2];
    memset(comps, 0, sizeof(comps));
    snprintf(comps[0].id,           sizeof(comps[0].id),           "TEST:0");
    snprintf(comps[0].model_name,   sizeof(comps[0].model_name),   "TestPad0");
    snprintf(comps[0].composite_path,sizeof(comps[0].composite_path),
             "/org/shadowblip/InputPlumber/CompositeDevice0");
    snprintf(comps[1].id,           sizeof(comps[1].id),           "TEST:1");
    snprintf(comps[1].model_name,   sizeof(comps[1].model_name),   "TestPad1");
    snprintf(comps[1].composite_path,sizeof(comps[1].composite_path),
             "/org/shadowblip/InputPlumber/CompositeDevice1");
    f->svc->comp_count = 2;
    memcpy(f->svc->composites, comps, sizeof(comps));

    /* Settings: 4 virtual controllers. */
    cbx_settings s;
    memset(&s, 0, sizeof(s));
    s.virtual_controllers.count = 4;
    for (int i = 0; i < 4; i++)
        snprintf(s.virtual_controllers.types[i],
                 CBX_MAX_TYPE_LEN, "xb360");

    /* Assignments (empty — will be synced by on_save). */
    cbx_assignments_init(&f->svc->assignments);

    /* Grid. */
    cbx_select_grid_init(&f->svc->grid);
    cbx_select_grid_build(&f->svc->grid, comps, 2, &s, &f->svc->assignments);

    /* Add 2 profiles for cycle-profile tests (O04/O05). */
    snprintf(f->svc->grid.profiles[0], CBX_GRID_PROFILE_LEN, "Default");
    snprintf(f->svc->grid.profiles[1], CBX_GRID_PROFILE_LEN, "Custom");
    f->svc->grid.profile_count = 2;
    /* Row 0 starts with "Default". */
    snprintf(f->svc->grid.rows[0].profile,
             CBX_GRID_PROFILE_LEN, "Default");

    /* Device model: 1 target + 1 composite (for on_save backend calls). */
    f->svc->model.target_count = 1;
    snprintf(f->svc->model.targets[0].path,
             sizeof(f->svc->model.targets[0].path),
             "/org/shadowblip/InputPlumber/devices/target/gamepad0");
    f->svc->model.composite_count = 1;
    snprintf(f->svc->model.composites[0].path,
             sizeof(f->svc->model.composites[0].path),
             "%s", comps[0].composite_path);

    /* Render context (minimal). */
    f->svc->render_ctx = (cbx_grid_render_ctx){
        .grid       = &f->svc->grid,
        .icon_cache = NULL,
        .icon_map   = NULL,
        .theme      = NULL,
        .text_cache = NULL,
        .font_id    = -1,
    };

    /* Overlay surface. */
    rc = cbx_overlay_surface_init(&f->svc->surface, f->svc->rend.renderer,
                                    CBX_RENDERER_DEFAULT_W,
                                    CBX_RENDERER_DEFAULT_H,
                                    1.0);
    assert_int_equal(rc, 0);

    /* Player mode + production callbacks. */
    cbx_player_mode_init(&f->svc->pm, &f->svc->grid);
    f->svc->pm.on_slot_change      = cbx_overlay_on_slot_change;
    f->svc->pm.slot_change_data    = f->svc;
    f->svc->pm.on_profile_change   = cbx_overlay_on_profile_change;
    f->svc->pm.profile_change_data = f->svc;

    /* Host mode. */
    cbx_host_mode_init(&f->svc->hm);
    f->svc->hm.on_slot_change     = cbx_overlay_on_slot_change;
    f->svc->hm.slot_change_data   = f->svc;
    /* Host Mode edits the selected row's profile too (SPEC §4.4, L1/R1);
     * reuse the production profile-change callback. */
    f->svc->hm.on_profile_change   = cbx_overlay_on_profile_change;
    f->svc->hm.profile_change_data = f->svc;
    /* Mirror production wiring (run_overlay_service): host-mode enter/exit
     * marks the surface dirty so the frame reflects the transition (W1). */
    f->svc->hm.on_state_change    = cbx_overlay_on_host_mode_change;
    f->svc->hm.state_change_data  = f->svc;

    /* Lifecycle — use instant transitions (fade = 0) for deterministic tests. */
    cbx_overlay_lifecycle_init(&f->svc->lifecycle, f->backend, f->mock.bus,
                                comps[0].composite_path,
                                &f->svc->surface, f->svc->rend.renderer);
    f->svc->lifecycle.fade_in_ms  = 0;
    f->svc->lifecycle.fade_out_ms = 0;
    f->svc->lifecycle.state      = CBX_OVERLAY_IDLE;

    /* Wire on_save. */
    f->svc->lifecycle.on_save      = cbx_overlay_on_save;
    f->svc->lifecycle.on_save_data = f->svc;

    /* Wire on_closed to end Host Mode with the overlay (SPEC §4.4) — the
     * exact production wiring, so the close/reopen path is exercised. */
    f->svc->lifecycle.on_closed      = cbx_overlay_on_lifecycle_closed;
    f->svc->lifecycle.on_closed_data = f->svc;

    /* Input context: device path → row mappings. */
    f->svc->input_ctx.pm       = &f->svc->pm;
    f->svc->input_ctx.hm       = &f->svc->hm;
    f->svc->input_ctx.grid     = &f->svc->grid;
    f->svc->input_ctx.lifecycle = &f->svc->lifecycle;
    cbx_overlay_input_add_mapping(&f->svc->input_ctx, DEV_PATH_0, 0);
    cbx_overlay_input_add_mapping(&f->svc->input_ctx, DEV_PATH_1, 1);

    /* ip_input_events subscription. */
    snprintf(f->svc->expected_sender, sizeof(f->svc->expected_sender),
             "%s", EXP_SENDER);
    ip_input_events_init(&f->svc->input_events, f->backend, f->mock.bus,
                          EXP_SENDER, cbx_overlay_input_cb,
                          &f->svc->input_ctx);
    rc = ip_input_events_subscribe(&f->svc->input_events);
    assert_int_equal(rc, 0);
    f->svc->input_events_ready = true;

    /* Intercept poll for O01 (activation test). */
    f->svc->poll_event_type = SDL_RegisterEvents(1);
    if (f->svc->poll_event_type != (uint32_t)-1) {
        ip_intercept_poll_init(&f->svc->polls[0],
                                f->backend, f->mock.bus,
                                comps[0].composite_path,
                                test_on_activating,  &f->svc->lifecycle,
                                test_on_deactivating, &f->svc->lifecycle,
                                test_on_poll_error,   NULL);
        f->svc->poll_count = 1;
        /* Leave poll in IDLE state; O01 test will set PASS_WAIT. */
    } else {
        f->svc->poll_count = 0;
    }

    /* Mock expectations: InterceptMode (for poll get + close set),
     * LoadProfilePath (for profile change), AttachTargetDevice + GamepadOrder
     * (for on_save backend apply). */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "InterceptMode", "2");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "LoadProfilePath", "");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                            "AttachTargetDevice", NULL);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                            "GamepadOrder", NULL);

    f->svc->initialized = true;
    cbx_overlay_service_reset_shutdown();

    *state = f;
    return 0;
}

static int
interaction_teardown(void **state)
{
    interaction_fixture *f = *state;
    if (f) {
        if (f->svc) {
            /* Stop poll timer if started. */
            for (int i = 0; i < f->svc->poll_count; i++)
                ip_intercept_poll_stop(&f->svc->polls[i]);
            cbx_overlay_surface_destroy(&f->svc->surface);
            cbx_renderer_shutdown(&f->svc->rend);
            free(f->svc);
        }
        ip_dbus_mock_reset(&f->mock);
        free(f);
    }
    flush_events();
    return 0;
}

/* --- Input injection helper for O11 ------------------------------------ */

static void
inject_input(interaction_fixture *f, const char *sender,
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

/* ================================================================== */
/*  O01 — Open (trigger activation)                                    */
/* ================================================================== */

static void
test_o01_open_lifecycle_activates(void **state)
{
    interaction_fixture *f = *state;

    /* Lifecycle starts IDLE. */
    assert_int_equal(f->svc->lifecycle.state, CBX_OVERLAY_IDLE);

    /* Put poll in PASS_WAIT so it will detect InterceptMode = ALL. */
    f->svc->polls[0].state = IP_POLL_PASS_WAIT;

    /* Push poll-timer event; mock will return InterceptMode "2" = ALL. */
    push_poll_event(f->svc->poll_event_type, &f->svc->polls[0]);

    /* Run one step. */
    cbx_overlay_service_step(f->svc);

    /* The poll should detect ALL → activate → VISIBLE (fade_in_ms=0). */
    assert_int_equal(f->svc->lifecycle.state, CBX_OVERLAY_VISIBLE);
}

/* ================================================================== */
/*  O02 — Move left (Player Mode)                                      */
/* ================================================================== */

static void
test_o02_move_left(void **state)
{
    interaction_fixture *f = *state;
    make_visible(f);

    /* Move right first so LEFT is not at boundary. */
    push_keydown(SDLK_RIGHT);
    cbx_overlay_service_step(f->svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 0), 1);

    /* Now move left. */
    push_keydown(SDLK_LEFT);
    cbx_overlay_service_step(f->svc);

    /* Row 0 should have moved back to col 0 (Unassigned). */
    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 0), 0);
}

/* ================================================================== */
/*  O03 — Move right (Player Mode)                                     */
/* ================================================================== */

static void
test_o03_move_right(void **state)
{
    interaction_fixture *f = *state;
    make_visible(f);

    /* Row 0 starts at col 0 (Unassigned). */
    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 0), 0);

    /* Move right. */
    push_keydown(SDLK_RIGHT);
    cbx_overlay_service_step(f->svc);

    /* Row 0 should have moved to col 1 (P1). */
    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 0), 1);
}

/* ================================================================== */
/*  O04 — Cycle profile up                                             */
/* ================================================================== */

static void
test_o04_cycle_profile_up(void **state)
{
    interaction_fixture *f = *state;
    make_visible(f);

    /* Row 0 profile starts as "Default". */
    assert_string_equal(cbx_select_grid_get_profile(&f->svc->grid, 0),
                        "Default");

    /* Cycle profile up. */
    push_keydown(SDLK_UP);
    cbx_overlay_service_step(f->svc);

    /* Profile should have changed (cycled). */
    const char *new_profile = cbx_select_grid_get_profile(&f->svc->grid, 0);
    assert_non_null(new_profile);
    assert_true(strcmp(new_profile, "Default") != 0);
}

/* ================================================================== */
/*  O05 — Cycle profile down                                           */
/* ================================================================== */

static void
test_o05_cycle_profile_down(void **state)
{
    interaction_fixture *f = *state;
    make_visible(f);

    /* Row 0 profile starts as "Default". */
    assert_string_equal(cbx_select_grid_get_profile(&f->svc->grid, 0),
                        "Default");

    /* Cycle profile down. */
    push_keydown(SDLK_DOWN);
    cbx_overlay_service_step(f->svc);

    /* Profile should have changed (cycled in the other direction). */
    const char *new_profile = cbx_select_grid_get_profile(&f->svc->grid, 0);
    assert_non_null(new_profile);
    assert_true(strcmp(new_profile, "Default") != 0);
}

/* ================================================================== */
/*  O06 — Enter Host Mode (R3)                                         */
/* ================================================================== */

static void
test_o06_enter_host_mode(void **state)
{
    interaction_fixture *f = *state;
    make_visible(f);

    /* Host mode is not active initially. */
    assert_false(cbx_host_mode_is_active(&f->svc->hm));

    /* Press R3 to toggle host mode. */
    push_keydown(SDLK_r);
    cbx_overlay_service_step(f->svc);

    /* Host mode should be active; host_row = 0 (keyboard = row 0). */
    assert_true(cbx_host_mode_is_active(&f->svc->hm));
    assert_int_equal(cbx_host_mode_get_host_row(&f->svc->hm), 0);
}

/* ================================================================== */
/*  O07 — Host: navigate rows                                          */
/* ================================================================== */

static void
test_o07_host_navigate_rows(void **state)
{
    interaction_fixture *f = *state;
    make_visible(f);

    /* Enter host mode. */
    push_keydown(SDLK_r);
    cbx_overlay_service_step(f->svc);
    assert_true(cbx_host_mode_is_active(&f->svc->hm));

    /* selected_row starts at host_row = 0. */
    assert_int_equal(cbx_host_mode_get_selected_row(&f->svc->hm), 0);

    /* Navigate down to row 1. */
    push_keydown(SDLK_DOWN);
    cbx_overlay_service_step(f->svc);

    assert_int_equal(cbx_host_mode_get_selected_row(&f->svc->hm), 1);

    /* Navigate back up to row 0. */
    push_keydown(SDLK_UP);
    cbx_overlay_service_step(f->svc);

    assert_int_equal(cbx_host_mode_get_selected_row(&f->svc->hm), 0);
}

/* ================================================================== */
/*  O08 — Host: move slot                                              */
/* ================================================================== */

static void
test_o08_host_move_slot(void **state)
{
    interaction_fixture *f = *state;
    make_visible(f);

    /* Enter host mode. */
    push_keydown(SDLK_r);
    cbx_overlay_service_step(f->svc);
    assert_true(cbx_host_mode_is_active(&f->svc->hm));

    /* selected_row = 0; col starts at 0 (Unassigned). */
    assert_int_equal(cbx_host_mode_get_selected_row(&f->svc->hm), 0);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 0), 0);

    /* Move slot right on selected row. */
    push_keydown(SDLK_RIGHT);
    cbx_overlay_service_step(f->svc);

    /* Row 0's column should have changed to 1 (P1). */
    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 0), 1);

    /* Move slot left. */
    push_keydown(SDLK_LEFT);
    cbx_overlay_service_step(f->svc);

    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 0), 0);
}

/* ================================================================== */
/*  O09 — Exit Host Mode (R3)                                          */
/* ================================================================== */

static void
test_o09_exit_host_mode(void **state)
{
    interaction_fixture *f = *state;
    make_visible(f);

    /* Enter host mode. */
    push_keydown(SDLK_r);
    cbx_overlay_service_step(f->svc);
    assert_true(cbx_host_mode_is_active(&f->svc->hm));

    /* Press R3 again to exit host mode. */
    push_keydown(SDLK_r);
    cbx_overlay_service_step(f->svc);

    /* Host mode should be inactive. */
    assert_false(cbx_host_mode_is_active(&f->svc->hm));
}

/* ================================================================== */
/*  Host-mode dirty triggers W1/W2 (surface re-render on enter/exit)    */
/*      Entering/exiting host mode must set the pre-built surface    */
/*      dirty so the presented frame reflects the HOST/SELECTED/     */
/*      FROZEN row visuals on entry (SPEC §4.4/§4.9).  This drives    */
/*      the toggle through the production DBus dispatch path          */
/*      (cbx_overlay_input_cb) with the lifecycle inactive so the    */
/*      step's re-render does not consume the flag before we check    */
/*      it — isolating the dirty-trigger itself.                      */
/*      NOTE: these are W1/W2 dirty-trigger tests.  They are NOT the   */
/*      O13 inventory ID (Player Mode conflict), which lives in        */
/*      test_overlay_native.c.                                       */
/* ================================================================== */

static void
test_hm_entry_marks_dirty_dbus(void **state)
{
    interaction_fixture *f = *state;

    /* Lifecycle IDLE: the step must not consume the dirty flag via the
     * active-only re-render, so we observe the trigger directly. */
    assert_int_equal(f->svc->lifecycle.state, CBX_OVERLAY_IDLE);

    /* Start from a clean (not dirty) surface. */
    cbx_overlay_surface_clear_dirty(&f->svc->surface);
    assert_false(cbx_overlay_surface_is_dirty(&f->svc->surface));

    /* Controller 0 presses R3 via DBus → player_mode toggles host mode. */
    inject_input(f, EXP_SENDER, DEV_PATH_0, "R3", 1.0);
    cbx_overlay_service_step(f->svc);

    assert_true(cbx_host_mode_is_active(&f->svc->hm));
    assert_int_equal(cbx_host_mode_get_host_row(&f->svc->hm), 0);
    /* Host-mode entry must have marked the surface dirty (W1). */
    assert_true(cbx_overlay_surface_is_dirty(&f->svc->surface));
}

static void
test_hm_exit_marks_dirty_dbus(void **state)
{
    interaction_fixture *f = *state;

    assert_int_equal(f->svc->lifecycle.state, CBX_OVERLAY_IDLE);

    /* Enter host mode via DBus (dirties the surface). */
    inject_input(f, EXP_SENDER, DEV_PATH_0, "R3", 1.0);
    cbx_overlay_service_step(f->svc);
    assert_true(cbx_host_mode_is_active(&f->svc->hm));
    assert_true(cbx_overlay_surface_is_dirty(&f->svc->surface));

    /* Clean the flag so exit is the only dirtying event we observe. */
    cbx_overlay_surface_clear_dirty(&f->svc->surface);
    assert_false(cbx_overlay_surface_is_dirty(&f->svc->surface));

    /* Host (row 0) presses R3 again → host_mode_handle routes to the
     * host path and exits via cbx_host_mode_exit → marks surface dirty. */
    inject_input(f, EXP_SENDER, DEV_PATH_0, "R3", 1.0);
    cbx_overlay_service_step(f->svc);

    assert_false(cbx_host_mode_is_active(&f->svc->hm));
    /* Host-mode exit must have marked the surface dirty (W1). */
    assert_true(cbx_overlay_surface_is_dirty(&f->svc->surface));
}

/* --- Single-fire dirty-trigger counter (W2 regression) ------------- */

typedef struct {
    int transitions;                    /* host-mode state-change fires    */
    cbx_overlay_service_ctx *svc;       /* forwarded to production callback */
} hm_transition_counter;

/*
 * Wraps the production dirty-trigger callback with a counter so a test can
 * assert that entering/exiting host mode fires the state-change transition
 * exactly once through the production dispatch path (W2: deliberate,
 * consistent triggers — no double-fire on the R3-exit path).
 */
static int
hm_count_and_mark(bool active, void *userdata)
{
    hm_transition_counter *c = (hm_transition_counter *)userdata;
    if (!c)
        return -EINVAL;
    c->transitions++;
    return cbx_overlay_on_host_mode_change(active, c->svc);
}

static void
test_hm_transition_fires_once(void **state)
{
    interaction_fixture *f = *state;

    /* Install the counting wrapper over the production dirty-trigger so we
     * can prove single-fire on enter/exit (and no-op on redundant exit). */
    hm_transition_counter c = {0, f->svc};
    f->svc->hm.on_state_change   = hm_count_and_mark;
    f->svc->hm.state_change_data = &c;

    assert_int_equal(f->svc->lifecycle.state, CBX_OVERLAY_IDLE);

    /* Enter host mode: R3 → player mode → toggle → enter → one transition. */
    inject_input(f, EXP_SENDER, DEV_PATH_0, "R3", 1.0);
    cbx_overlay_service_step(f->svc);
    assert_true(cbx_host_mode_is_active(&f->svc->hm));
    assert_int_equal(c.transitions, 1);

    /* Exit host mode: R3 → host_mode_handle → cbx_host_mode_exit fires
     * exactly once; the production wrapper must NOT re-exit/double-fire. */
    inject_input(f, EXP_SENDER, DEV_PATH_0, "R3", 1.0);
    cbx_overlay_service_step(f->svc);
    assert_false(cbx_host_mode_is_active(&f->svc->hm));
    assert_int_equal(c.transitions, 2);

    /* A redundant exit on the now-idle object is a no-op transition: the
     * dirty trigger must NOT fire on it (W2: dirt only on actual state
     * transitions). */
    cbx_host_mode_exit(&f->svc->hm);
    assert_int_equal(c.transitions, 2);

    /* Sanity: the production dirty-trigger still works through the wrapper. */
    assert_true(cbx_overlay_surface_is_dirty(&f->svc->surface));
}

/* ================================================================== */
/*  Host Mode must not leak across overlay close/reopen (SPEC §4.4)     */
/*      Host Mode is scoped to one overlay session: the first           */
/*      controller to press R3 becomes the exclusive host.  Closing the  */
/*      overlay (B, deactivation, timeout, force-close) must end Host    */
/*      Mode; otherwise reopening presents every non-host controller     */
/*      frozen even though no controller pressed R3 in the new session,  */
/*      and only the stale host can exit.                                */
/* ================================================================== */

static void
assert_all_rows_player_mode(interaction_fixture *f)
{
    /* Substantive, not tautological: this is asserted while the previous
     * session had left rows frozen, so it proves the leak is gone. */
    assert_false(cbx_host_mode_is_active(&f->svc->hm));
    for (int i = 0; i < f->svc->grid.row_count; i++) {
        assert_int_equal(cbx_host_mode_row_state(&f->svc->hm, i),
                         CBX_ROW_NORMAL);
        assert_false(cbx_host_mode_is_frozen(&f->svc->hm, i));
    }
}

static void
test_host_mode_exits_on_close_and_reopen(void **state)
{
    interaction_fixture *f = *state;
    make_visible(f);

    /* Enter Host Mode from controller 0 through the production DBus path. */
    inject_input(f, EXP_SENDER, DEV_PATH_0, "R3", 1.0);
    cbx_overlay_service_step(f->svc);
    assert_true(cbx_host_mode_is_active(&f->svc->hm));
    assert_int_equal(cbx_host_mode_get_host_row(&f->svc->hm), 0);
    /* Host Mode visuals are live: host/selected row 0, frozen row 1. */
    assert_int_equal(cbx_host_mode_row_state(&f->svc->hm, 0),
                     CBX_ROW_SELECTED);
    assert_int_equal(cbx_host_mode_row_state(&f->svc->hm, 1),
                     CBX_ROW_FROZEN);
    assert_true(cbx_host_mode_is_frozen(&f->svc->hm, 1));

    /* The host presses B: the production close path runs (save, PASS,
     * IDLE).  fade_out_ms == 0, so IDLE — and on_closed — happen in-step. */
    inject_input(f, EXP_SENDER, DEV_PATH_0, "B", 1.0);
    cbx_overlay_service_step(f->svc);
    assert_int_equal(f->svc->lifecycle.state, CBX_OVERLAY_IDLE);

    /* Host Mode ended with the overlay; no frozen row survives the close. */
    assert_all_rows_player_mode(f);

    /* Re-activate the overlay through the production lifecycle API.  The
     * new session must start in Player Mode: no controller is frozen and
     * every row renders normal. */
    assert_int_equal(cbx_overlay_lifecycle_activate(&f->svc->lifecycle), 0);
    cbx_overlay_service_step(f->svc);
    assert_int_equal(f->svc->lifecycle.state, CBX_OVERLAY_VISIBLE);
    assert_all_rows_player_mode(f);

    /* A controller that was frozen in the previous session is not frozen
     * now: controller 1's R3 makes it the new exclusive host. */
    inject_input(f, EXP_SENDER, DEV_PATH_1, "R3", 1.0);
    cbx_overlay_service_step(f->svc);
    assert_true(cbx_host_mode_is_active(&f->svc->hm));
    assert_int_equal(cbx_host_mode_get_host_row(&f->svc->hm), 1);
}

/* ================================================================== */
/*  O12 — Host cycles the selected row's profile (L1/R1, DBus path)   */
/*      SPEC §4.4: the exclusive host can navigate to any row and     */
/*      edit both slot and profile.  Documented affordance: L1 =      */
/*      previous profile, R1 = next profile for the selected row.     */
/* ================================================================== */

static void
test_o12_host_profile_cycle_dbus(void **state)
{
    interaction_fixture *f = *state;

    /* Row 1 starts on a known profile in the list ("Default"). */
    snprintf(f->svc->grid.rows[1].profile, CBX_GRID_PROFILE_LEN, "Default");

    /* Enter host mode from controller 0 via DBus. */
    inject_input(f, EXP_SENDER, DEV_PATH_0, "R3", 1.0);
    cbx_overlay_service_step(f->svc);
    assert_true(cbx_host_mode_is_active(&f->svc->hm));
    assert_int_equal(cbx_host_mode_get_host_row(&f->svc->hm), 0);

    /* Host navigates down to row 1 (not its own row). */
    inject_input(f, EXP_SENDER, DEV_PATH_0, "Down", 1.0);
    cbx_overlay_service_step(f->svc);
    assert_int_equal(cbx_host_mode_get_selected_row(&f->svc->hm), 1);

    char row0_before[CBX_GRID_PROFILE_LEN];
    snprintf(row0_before, sizeof(row0_before), "%s",
             cbx_select_grid_get_profile(&f->svc->grid, 0));

    /* R1 (right bumper) cycles the selected row's profile to the next. */
    inject_input(f, EXP_SENDER, DEV_PATH_0, "R1", 1.0);
    cbx_overlay_service_step(f->svc);
    assert_string_equal(cbx_select_grid_get_profile(&f->svc->grid, 1),
                        "Custom");
    /* Only the selected row changed; the host's own row is untouched. */
    assert_string_equal(cbx_select_grid_get_profile(&f->svc->grid, 0),
                        row0_before);

    /* L1 (left bumper) cycles back to the previous profile. */
    inject_input(f, EXP_SENDER, DEV_PATH_0, "L1", 1.0);
    cbx_overlay_service_step(f->svc);
    assert_string_equal(cbx_select_grid_get_profile(&f->svc->grid, 1),
                        "Default");
}

static void
test_o12_host_profile_frozen_dbus(void **state)
{
    interaction_fixture *f = *state;

    /* Enter host mode from controller 0. */
    inject_input(f, EXP_SENDER, DEV_PATH_0, "R3", 1.0);
    cbx_overlay_service_step(f->svc);
    assert_true(cbx_host_mode_is_active(&f->svc->hm));

    char row0_before[CBX_GRID_PROFILE_LEN];
    char row1_before[CBX_GRID_PROFILE_LEN];
    snprintf(row0_before, sizeof(row0_before), "%s",
             cbx_select_grid_get_profile(&f->svc->grid, 0));
    snprintf(row1_before, sizeof(row1_before), "%s",
             cbx_select_grid_get_profile(&f->svc->grid, 1));

    /* Frozen controller 1 sends R1/L1: both are rejected; no profile (of
     * the host or the frozen controller) changes, and the host owner and
     * selected row are unchanged. */
    inject_input(f, EXP_SENDER, DEV_PATH_1, "R1", 1.0);
    cbx_overlay_service_step(f->svc);
    inject_input(f, EXP_SENDER, DEV_PATH_1, "L1", 1.0);
    cbx_overlay_service_step(f->svc);

    assert_string_equal(cbx_select_grid_get_profile(&f->svc->grid, 0),
                        row0_before);
    assert_string_equal(cbx_select_grid_get_profile(&f->svc->grid, 1),
                        row1_before);
    assert_int_equal(cbx_host_mode_get_host_row(&f->svc->hm), 0);
    assert_int_equal(cbx_host_mode_get_selected_row(&f->svc->hm), 0);
}

/* ================================================================== */
/*  O12b — Profile apply failure: dirty + engine-consistent rollback    */
/*      SPEC §4.9: a profile-change event must dirty the pre-built      */
/*      surface even when the engine apply fails, so a rejected profile */
/*      never leaves a stale presented frame.  The displayed profile is */
/*      rolled back to the engine's authoritative profile so the grid   */
/*      does not diverge from live engine state.                        */
/* ================================================================== */

static void
test_o12b_profile_apply_failure_rolls_back_and_dirties(void **state)
{
    interaction_fixture *f = *state;

    /* Configure a real profile list so the apply path reaches the engine
     * (LoadProfilePath).  The interaction fixture deliberately leaves
     * profile_cycle unconfigured for the success-path grid tests. */
    static cbx_profile_list list;
    memset(&list, 0, sizeof(list));
    snprintf(list.entries[0].filename, CBX_LIST_NAME_LEN, "Default");
    snprintf(list.entries[0].path, sizeof(list.entries[0].path),
             "/profiles/Default.yaml");
    snprintf(list.entries[1].filename, CBX_LIST_NAME_LEN, "Custom");
    snprintf(list.entries[1].path, sizeof(list.entries[1].path),
             "/profiles/Custom.yaml");
    list.count = 2;
    cbx_profile_cycle_init(&f->svc->profile_cycle, f->backend, f->mock.bus,
                           &f->svc->assignments, &list);

    /* Row 0 is on "Default". */
    snprintf(f->svc->grid.rows[0].profile, CBX_GRID_PROFILE_LEN, "Default");

    /* Lifecycle IDLE so the step's active-only re-render cannot consume the
     * dirty flag before we observe it. */
    assert_int_equal(f->svc->lifecycle.state, CBX_OVERLAY_IDLE);

    /* Engine rejects LoadProfilePath; its current ProfilePath is still the
     * old profile (the rollback read-back below). */
    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                              "LoadProfilePath", -EIO);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE, "ProfilePath",
                           "/profiles/Default.yaml");
    /* ip_dbus_mock_reset clears the mock's signal-subscription table, so
     * re-register the production InputEvent subscription to keep driving
     * the dispatch path through inject_input. */
    assert_int_equal(ip_input_events_subscribe(&f->svc->input_events), 0);

    /* Clean surface so this profile-change event is the only dirtying one. */
    cbx_overlay_surface_clear_dirty(&f->svc->surface);
    assert_false(cbx_overlay_surface_is_dirty(&f->svc->surface));

    /* Player Mode UP cycles row 0's profile (Default -> Custom) through the
     * production DBus dispatch path; the engine apply fails. */
    inject_input(f, EXP_SENDER, DEV_PATH_0, "Up", 1.0);
    cbx_overlay_service_step(f->svc);

    /* Displayed profile rolled back to engine truth (not the rejected
     * cycle), and the surface was dirtied so the frame is never stale. */
    assert_string_equal(cbx_select_grid_get_profile(&f->svc->grid, 0),
                        "Default");
    assert_true(cbx_overlay_surface_is_dirty(&f->svc->surface));
}

/* ================================================================== */
/*  O10 — Close (B): saves, conflict-resolves, sets PASS, hides        */
/* ================================================================== */

static void
test_o10_close_saves_and_sets_pass(void **state)
{
    interaction_fixture *f = *state;
    make_visible(f);

    /* Move row 0 to col 1 (P1) so on_save has something to sync. */
    push_keydown(SDLK_RIGHT);
    cbx_overlay_service_step(f->svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 0), 1);

    /* No assignments yet. */
    assert_int_equal(f->svc->assignments.assignment_count, 0);

    /* Exact post-attach TargetDevices confirmation. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE, "TargetDevices",
        "/org/shadowblip/InputPlumber/devices/target/gamepad0");

    /* Press B to close overlay. */
    push_keydown(SDLK_b);
    cbx_overlay_service_step(f->svc);

    /* Lifecycle should have closed (fade_out_ms=0 → instant IDLE). */
    assert_int_equal(f->svc->lifecycle.state, CBX_OVERLAY_IDLE);

    /* on_save should have synced grid → assignments.
     * Row 0 at col 1 → slot 0 (P1). */
    assert_int_equal(f->svc->assignments.assignment_count, 1);
    assert_int_equal(f->svc->assignments.assignments[0].slot, 0);
    assert_string_equal(f->svc->assignments.assignments[0].id, "TEST:0");
}

/* ================================================================== */
/*  O10b — Close: conflict auto-resolution on save                     */
/* ================================================================== */

static void
test_o10b_close_conflict_resolution(void **state)
{
    interaction_fixture *f = *state;
    make_visible(f);

    /* Move row 0 to col 1 (P1). */
    push_keydown(SDLK_RIGHT);
    cbx_overlay_service_step(f->svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 0), 1);

    /* Move row 1 to col 1 (P1) — creates a conflict. */
    push_keydown(SDLK_DOWN);
    cbx_overlay_service_step(f->svc);  /* enters host mode! */

    /* Actually, DOWN in player mode cycles profile, not moves row.
     * We need to use host mode to move row 1 to col 1. */
    /* Enter host mode. */
    push_keydown(SDLK_r);
    cbx_overlay_service_step(f->svc);
    assert_true(cbx_host_mode_is_active(&f->svc->hm));

    /* Navigate down to row 1. */
    push_keydown(SDLK_DOWN);
    cbx_overlay_service_step(f->svc);
    assert_int_equal(cbx_host_mode_get_selected_row(&f->svc->hm), 1);

    /* Move row 1 right to col 1 (P1) — same as row 0 → conflict. */
    push_keydown(SDLK_RIGHT);
    cbx_overlay_service_step(f->svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 1), 1);

    /* Exit host mode. */
    push_keydown(SDLK_r);
    cbx_overlay_service_step(f->svc);
    assert_false(cbx_host_mode_is_active(&f->svc->hm));

    /* Close overlay (B). */
    push_keydown(SDLK_b);
    cbx_overlay_service_step(f->svc);

    /* on_save should have resolved the conflict.
     * Row 0 keeps P1 (slot 0); row 1 should have been moved to P2 (slot 1). */
    assert_int_equal(f->svc->grid.rows[0].cur_col, 1);  /* row 0 keeps P1 */
    assert_int_equal(f->svc->grid.rows[1].cur_col, 2);  /* row 1 → P2 */
}

/* ================================================================== */
/*  O11 — Multi-controller independence (DBus InputEvent)             */
/* ================================================================== */

static void
test_o11_multi_controller_independent(void **state)
{
    interaction_fixture *f = *state;
    make_visible(f);

    /* Both rows start at col 0. */
    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 0), 0);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 1), 0);

    /* Controller 0 sends Right → row 0 moves. */
    inject_input(f, EXP_SENDER, DEV_PATH_0, "Right", 1.0);
    cbx_overlay_service_step(f->svc);  /* lifecycle tick + render */

    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 0), 1);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 1), 0);

    /* Controller 1 sends Right → row 1 moves independently. */
    inject_input(f, EXP_SENDER, DEV_PATH_1, "Right", 1.0);
    cbx_overlay_service_step(f->svc);

    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 0), 1);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 1), 1);

    /* Controller 0 sends Left → row 0 moves back, row 1 stays. */
    inject_input(f, EXP_SENDER, DEV_PATH_0, "Left", 1.0);
    cbx_overlay_service_step(f->svc);

    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 0), 0);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 1), 1);
}

/* ================================================================== */
/*  O11b — Multi-controller: host mode via DBus InputEvent             */
/* ================================================================== */

static void
test_o11b_host_mode_via_dbus_input(void **state)
{
    interaction_fixture *f = *state;
    make_visible(f);

    /* Controller 0 presses R3 via DBus → host mode entered. */
    inject_input(f, EXP_SENDER, DEV_PATH_0, "R3", 1.0);
    cbx_overlay_service_step(f->svc);

    assert_true(cbx_host_mode_is_active(&f->svc->hm));
    assert_int_equal(cbx_host_mode_get_host_row(&f->svc->hm), 0);

    /* Controller 1 (frozen) presses Right → no movement. */
    inject_input(f, EXP_SENDER, DEV_PATH_1, "Right", 1.0);
    cbx_overlay_service_step(f->svc);

    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 1), 0);

    /* Host (controller 0) navigates down to row 1. */
    inject_input(f, EXP_SENDER, DEV_PATH_0, "Down", 1.0);
    cbx_overlay_service_step(f->svc);

    assert_int_equal(cbx_host_mode_get_selected_row(&f->svc->hm), 1);

    /* Host moves selected row 1 right. */
    inject_input(f, EXP_SENDER, DEV_PATH_0, "Right", 1.0);
    cbx_overlay_service_step(f->svc);

    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 1), 1);

    /* Host exits via R3. */
    inject_input(f, EXP_SENDER, DEV_PATH_0, "R3", 1.0);
    cbx_overlay_service_step(f->svc);

    assert_false(cbx_host_mode_is_active(&f->svc->hm));
}

/* ================================================================== */
/*  O11e — Non-host controller cannot move the host's selection        */
/* ================================================================== */

static void
test_o11e_non_host_nav_keeps_host_selection(void **state)
{
    interaction_fixture *f = *state;
    make_visible(f);

    /* Establish a baseline host selection: move row 0 (the host) to col 1,
     * then enter host so host_row = 0 / selected_row = 0. */
    inject_input(f, EXP_SENDER, DEV_PATH_0, "Right", 1.0);
    cbx_overlay_service_step(f->svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 0), 1);

    inject_input(f, EXP_SENDER, DEV_PATH_0, "R3", 1.0);
    cbx_overlay_service_step(f->svc);
    assert_true(cbx_host_mode_is_active(&f->svc->hm));
    assert_int_equal(cbx_host_mode_get_host_row(&f->svc->hm), 0);
    assert_int_equal(cbx_host_mode_get_selected_row(&f->svc->hm), 0);

    /* Non-host controller (row 1) navigates: Right, Down, Left.  Under the
     * freeze guard these must all be ignored — the host's selected row and
     * its column are unchanged, and frozen rows are untouched. */
    inject_input(f, EXP_SENDER, DEV_PATH_1, "Right", 1.0);
    cbx_overlay_service_step(f->svc);
    inject_input(f, EXP_SENDER, DEV_PATH_1, "Down", 1.0);
    cbx_overlay_service_step(f->svc);
    inject_input(f, EXP_SENDER, DEV_PATH_1, "Left", 1.0);
    cbx_overlay_service_step(f->svc);

    /* Host selection stable: selected_row still 0, host row 0 still col 1. */
    assert_int_equal(cbx_host_mode_get_selected_row(&f->svc->hm), 0);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 0), 1);
    /* Frozen row 1 was not moved by any of its own inputs. */
    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 1), 0);
}

/* ================================================================== */
/*  O11f — Non-host R3 does not exit host mode                         */
/* ================================================================== */

static void
test_o11f_non_host_r3_keeps_host_mode(void **state)
{
    interaction_fixture *f = *state;
    make_visible(f);

    /* Enter host mode via controller 0 (host). */
    inject_input(f, EXP_SENDER, DEV_PATH_0, "R3", 1.0);
    cbx_overlay_service_step(f->svc);
    assert_true(cbx_host_mode_is_active(&f->svc->hm));
    assert_int_equal(cbx_host_mode_get_host_row(&f->svc->hm), 0);

    /* Non-host controller (row 1) presses R3 — must be frozen; host mode
     * must remain active instead of exiting. */
    inject_input(f, EXP_SENDER, DEV_PATH_1, "R3", 1.0);
    cbx_overlay_service_step(f->svc);

    assert_true(cbx_host_mode_is_active(&f->svc->hm));
    assert_int_equal(cbx_host_mode_get_host_row(&f->svc->hm), 0);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 1), 0);
}

/* ================================================================== */
/*  O11c — Unknown device path dropped via step                        */
/* ================================================================== */

static void
test_o11c_unknown_device_dropped(void **state)
{
    interaction_fixture *f = *state;
    make_visible(f);

    int col0_before = cbx_select_grid_get_cur_col(&f->svc->grid, 0);
    int col1_before = cbx_select_grid_get_cur_col(&f->svc->grid, 1);

    /* Signal from unknown device path → dropped. */
    inject_input(f, EXP_SENDER,
                 "/org/shadowblip/InputPlumber/devices/unknown",
                 "Right", 1.0);
    cbx_overlay_service_step(f->svc);

    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 0),
                     col0_before);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 1),
                     col1_before);
}

/* ================================================================== */
/*  O11d — Wrong sender rejected via step                              */
/* ================================================================== */

static void
test_o11d_wrong_sender_rejected(void **state)
{
    interaction_fixture *f = *state;
    make_visible(f);

    int col0_before = cbx_select_grid_get_cur_col(&f->svc->grid, 0);

    /* Signal from spoofed sender → rejected (fail-closed). */
    inject_input(f, ":1.99", DEV_PATH_0, "Right", 1.0);
    cbx_overlay_service_step(f->svc);

    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 0),
                     col0_before);
}

/* ================================================================== */
/*  O12 — Host: cycle the selected row's profile (SDL keyboard path)   */
/*      Keyboard equivalents of the documented L1/R1 affordance:       */
/*      Q = previous profile, E = next profile.                        */
/* ================================================================== */

static void
test_o12_host_profile_cycle_sdl(void **state)
{
    interaction_fixture *f = *state;
    make_visible(f);

    /* Row 0 starts on "Default" (profile list: Default, Custom). */
    snprintf(f->svc->grid.rows[0].profile, CBX_GRID_PROFILE_LEN, "Default");

    /* Enter host mode. */
    push_keydown(SDLK_r);
    cbx_overlay_service_step(f->svc);
    assert_true(cbx_host_mode_is_active(&f->svc->hm));

    const char *before = cbx_select_grid_get_profile(&f->svc->grid, 0);
    char before_copy[CBX_GRID_PROFILE_LEN];
    snprintf(before_copy, sizeof(before_copy), "%s", before);

    /* E (R1 equivalent) cycles to the next profile. */
    push_keydown(SDLK_e);
    cbx_overlay_service_step(f->svc);
    const char *after = cbx_select_grid_get_profile(&f->svc->grid, 0);
    assert_non_null(after);
    assert_string_not_equal(after, before_copy);

    /* Q (L1 equivalent) cycles back to the previous profile. */
    push_keydown(SDLK_q);
    cbx_overlay_service_step(f->svc);
    assert_string_equal(cbx_select_grid_get_profile(&f->svc->grid, 0),
                        before_copy);
}

/* ================================================================== */
/*  O10c — Close via DBus InputEvent (B button)                        */
/* ================================================================== */

static void
test_o10c_close_via_dbus_b(void **state)
{
    interaction_fixture *f = *state;
    make_visible(f);

    /* Move row 0 to col 1 via DBus InputEvent. */
    inject_input(f, EXP_SENDER, DEV_PATH_0, "Right", 1.0);
    cbx_overlay_service_step(f->svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 0), 1);

    /* Exact post-attach TargetDevices confirmation. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE, "TargetDevices",
        "/org/shadowblip/InputPlumber/devices/target/gamepad0");

    /* Press B via DBus → close overlay. */
    inject_input(f, EXP_SENDER, DEV_PATH_0, "B", 1.0);
    cbx_overlay_service_step(f->svc);

    /* Lifecycle should have closed. */
    assert_int_equal(f->svc->lifecycle.state, CBX_OVERLAY_IDLE);

    /* on_save should have synced assignments. */
    assert_int_equal(f->svc->assignments.assignment_count, 1);
    assert_int_equal(f->svc->assignments.assignments[0].slot, 0);
}

/* ================================================================== */
/*  O01b — Deactivation via poll (InterceptMode → PASS)                */
/* ================================================================== */

static void
test_o01b_deactivation_closes_overlay(void **state)
{
    interaction_fixture *f = *state;

    /* Start in VISIBLE with poll in ACTIVE state. */
    make_visible(f);
    f->svc->polls[0].state = IP_POLL_ACTIVE;

    /* Mock expectation: InterceptMode = "1" (PASS) triggers deactivation.
     * The fixture set up "InterceptMode" expectation with value "2".
     * We need to change it to "1" for this test. */
    ip_dbus_mock_reset(&f->mock);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "InterceptMode", "1");

    push_poll_event(f->svc->poll_event_type, &f->svc->polls[0]);
    cbx_overlay_service_step(f->svc);

    /* Deactivation should close the overlay (instant with fade_out=0). */
    assert_int_equal(f->svc->lifecycle.state, CBX_OVERLAY_IDLE);
}

/* ================================================================== */
/*  O06b — Host mode freezes non-host controllers                      */
/* ================================================================== */

static void
test_o06b_host_mode_freezes_non_host(void **state)
{
    interaction_fixture *f = *state;
    make_visible(f);

    /* Move row 1 to col 1 first (via host mode). */
    push_keydown(SDLK_r);
    cbx_overlay_service_step(f->svc);
    assert_true(cbx_host_mode_is_active(&f->svc->hm));

    /* Controller 1 (frozen) tries to move right via DBus. */
    inject_input(f, EXP_SENDER, DEV_PATH_1, "Right", 1.0);
    cbx_overlay_service_step(f->svc);

    /* Row 1 should not move (frozen). */
    assert_int_equal(cbx_select_grid_get_cur_col(&f->svc->grid, 1), 0);

    /* Host visual state for row 1 should be FROZEN. */
    assert_int_equal(cbx_host_mode_row_state(&f->svc->hm, 1),
                     CBX_ROW_FROZEN);
}

/* ================================================================== */
/*  Hotplug — Dynamic columns rebuild through production dispatch       */
/*  (SPEC §4.7, §10.1; plan Task 7)                                   */
/* ================================================================== */

/* Helper: inject an InterfacesAdded/Removed signal through the mock
 * DBus backend's subscription dispatch (the same path production code
 * uses when sd-bus delivers the signal).
 */
static void
inject_hotplug(interaction_fixture *f, const char *member,
               const char *path, const char *interfaces)
{
    ip_interfaces_changed_payload p = {
        .sender     = EXP_SENDER,
        .path       = path,
        .interfaces = interfaces,
    };
    f->backend->inject_signal(f->mock.bus,
                               IP_IFACE_OBJECT_MANAGER, member, &p);
}

/* Stage mock expectations for cbx_overlay_reconcile_hotplug.
 * NOTE: ip_dbus_mock_reset clears subscriptions too, so we must
 * re-subscribe hotplug signals after resetting. */
static void
expect_reconcile(ip_dbus_mock *mock, ip_hotplug *hp)
{
    ip_dbus_mock_reset(mock);
    ip_dbus_mock_expect_ok(mock, IP_IFACE_TARGET,
                            "DeviceType", "xb360");
    ip_dbus_mock_expect_ok(mock, IP_IFACE_COMPOSITE,
                            "InterceptMode", "1");
    ip_dbus_mock_expect_ok(mock, IP_IFACE_COMPOSITE,
                            "SetInterceptActivation", NULL);
    /* Re-subscribe hotplug signals (reset cleared them). */
    ip_hotplug_subscribe(hp);
}

static void
test_hotplug_target_add_remove_through_dispatch(void **state)
{
    interaction_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    /* Align device-model target_count with the grid column count.
     * The fixture builds the grid with 4 virtual controllers (col_count=5:
     * Unassigned + P1-P4) but sets model.target_count=1.  For hotplug
     * reconciliation to work we need target_count to match so that
     * cbx_dynamic_columns_needs_rebuild returns false initially. */
    svc->model.target_count = 4;
    snprintf(svc->model.targets[1].path,
             sizeof(svc->model.targets[1].path),
             "/org/shadowblip/InputPlumber/devices/target/gamepad1");
    snprintf(svc->model.targets[2].path,
             sizeof(svc->model.targets[2].path),
             "/org/shadowblip/InputPlumber/devices/target/gamepad2");
    snprintf(svc->model.targets[3].path,
             sizeof(svc->model.targets[3].path),
             "/org/shadowblip/InputPlumber/devices/target/gamepad3");

    /* Initialize and subscribe hotplug handler (production path). */
    ip_hotplug_init(&svc->hp, f->backend, f->mock.bus,
                     EXP_SENDER, &svc->model);
    assert_int_equal(ip_hotplug_subscribe(&svc->hp), 0);

    /* Sanity: grid has 5 columns (Unassigned + 4 VCs), 4 targets. */
    assert_int_equal(svc->grid.col_count, 5);
    assert_int_equal(svc->model.target_count, 4);
    assert_false(svc->hp.model_changed);

    /* --- Phase 1: Hotplug ADD a 5th target via signal dispatch --- */
    inject_hotplug(f, "InterfacesAdded",
                   "/org/shadowblip/InputPlumber/devices/target/gamepad4",
                   "org.shadowblip.Input.Target");

    /* The subscription callback (hotplug_signal_cb) should have called
     * ip_hotplug_handle_added, which increments target_count and sets
     * model_changed.  Sender verification and path validation were
     * exercised by the production handler. */
    assert_true(svc->hp.model_changed);
    assert_int_equal(svc->model.target_count, 5);

    /* Stage mock expectations for the reconcile. */
    expect_reconcile(&f->mock, &svc->hp);

    flush_events();
    cbx_overlay_service_step(svc);

    /* Reconcile should have rebuilt columns: 5 targets → 6 columns. */
    assert_false(svc->hp.model_changed);
    assert_int_equal(svc->grid.col_count, 6);

    /* --- Phase 2: Hotplug REMOVE the 5th target, clamp positions --- */
    /* Place row 0 in column 5 (P5).  After removing target gamepad4,
     * col_count drops to 5 and column 5 is out of range — the row must
     * be clamped to Unassigned (col 0). */
    svc->grid.rows[0].cur_col = 5;

    inject_hotplug(f, "InterfacesRemoved",
                   "/org/shadowblip/InputPlumber/devices/target/gamepad4",
                   "org.shadowblip.Input.Target");

    assert_true(svc->hp.model_changed);
    assert_int_equal(svc->model.target_count, 4);

    expect_reconcile(&f->mock, &svc->hp);

    flush_events();
    cbx_overlay_service_step(svc);

    /* Column count back to 5; row 0 clamped from col 5 to col 0
     * (Unassigned) by cbx_dynamic_columns_clamp_positions. */
    assert_false(svc->hp.model_changed);
    assert_int_equal(svc->grid.col_count, 5);
    assert_int_equal(svc->grid.rows[0].cur_col, 0);
}

/* --- Test runner ------------------------------------------------------- */

static const struct CMUnitTest tests[] = {
    /* O01 — Open */
    cmocka_unit_test_setup_teardown(test_o01_open_lifecycle_activates,
                                     interaction_setup, interaction_teardown),
    cmocka_unit_test_setup_teardown(test_o01b_deactivation_closes_overlay,
                                     interaction_setup, interaction_teardown),

    /* O02–O03 — Move left/right (Player Mode) */
    cmocka_unit_test_setup_teardown(test_o02_move_left,
                                     interaction_setup, interaction_teardown),
    cmocka_unit_test_setup_teardown(test_o03_move_right,
                                     interaction_setup, interaction_teardown),

    /* O04–O05 — Cycle profile up/down */
    cmocka_unit_test_setup_teardown(test_o04_cycle_profile_up,
                                     interaction_setup, interaction_teardown),
    cmocka_unit_test_setup_teardown(test_o05_cycle_profile_down,
                                     interaction_setup, interaction_teardown),

    /* O06–O09 — Host Mode */
    cmocka_unit_test_setup_teardown(test_o06_enter_host_mode,
                                     interaction_setup, interaction_teardown),
    cmocka_unit_test_setup_teardown(test_o06b_host_mode_freezes_non_host,
                                     interaction_setup, interaction_teardown),
    cmocka_unit_test_setup_teardown(test_o07_host_navigate_rows,
                                     interaction_setup, interaction_teardown),
    cmocka_unit_test_setup_teardown(test_o08_host_move_slot,
                                     interaction_setup, interaction_teardown),
    cmocka_unit_test_setup_teardown(test_o09_exit_host_mode,
                                     interaction_setup, interaction_teardown),
    cmocka_unit_test_setup_teardown(test_hm_entry_marks_dirty_dbus,
                                     interaction_setup, interaction_teardown),
    cmocka_unit_test_setup_teardown(test_hm_exit_marks_dirty_dbus,
                                     interaction_setup, interaction_teardown),
    cmocka_unit_test_setup_teardown(test_hm_transition_fires_once,
                                     interaction_setup, interaction_teardown),
    cmocka_unit_test_setup_teardown(
        test_host_mode_exits_on_close_and_reopen,
        interaction_setup, interaction_teardown),

    /* O10 — Close */
    cmocka_unit_test_setup_teardown(test_o10_close_saves_and_sets_pass,
                                     interaction_setup, interaction_teardown),
    cmocka_unit_test_setup_teardown(test_o10b_close_conflict_resolution,
                                     interaction_setup, interaction_teardown),
    cmocka_unit_test_setup_teardown(test_o10c_close_via_dbus_b,
                                     interaction_setup, interaction_teardown),

    /* O11 — Multi-controller independence */
    cmocka_unit_test_setup_teardown(test_o11_multi_controller_independent,
                                     interaction_setup, interaction_teardown),
    cmocka_unit_test_setup_teardown(test_o11b_host_mode_via_dbus_input,
                                     interaction_setup, interaction_teardown),
    cmocka_unit_test_setup_teardown(test_o11e_non_host_nav_keeps_host_selection,
                                     interaction_setup, interaction_teardown),
    cmocka_unit_test_setup_teardown(test_o11f_non_host_r3_keeps_host_mode,
                                     interaction_setup, interaction_teardown),
    cmocka_unit_test_setup_teardown(test_o11c_unknown_device_dropped,
                                     interaction_setup, interaction_teardown),
    cmocka_unit_test_setup_teardown(test_o11d_wrong_sender_rejected,
                                     interaction_setup, interaction_teardown),

    cmocka_unit_test_setup_teardown(test_o12_host_profile_cycle_dbus,
                                     interaction_setup, interaction_teardown),
    cmocka_unit_test_setup_teardown(test_o12_host_profile_frozen_dbus,
                                     interaction_setup, interaction_teardown),
    cmocka_unit_test_setup_teardown(test_o12_host_profile_cycle_sdl,
                                     interaction_setup, interaction_teardown),
    cmocka_unit_test_setup_teardown(
        test_o12b_profile_apply_failure_rolls_back_and_dirties,
        interaction_setup, interaction_teardown),

    /* Hotplug — Dynamic columns rebuild through production dispatch */
    cmocka_unit_test_setup_teardown(
        test_hotplug_target_add_remove_through_dispatch,
        interaction_setup, interaction_teardown),
};

int
main(void)
{
    return cmocka_run_group_tests(tests, NULL, NULL);
}