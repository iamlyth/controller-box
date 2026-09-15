/*
 * test_overlay_native.c — Overlay interaction acceptance with native DBus.
 *
 * Exercises overlay actions O01–O11 and O13 through the production dispatch
 * path (cbx_overlay_service_step) using a real sd-bus backend connected to a
 * private InputPlumber-compatible DBus server with native type signatures
 * (u, b, as, s, sd).  No ip_dbus_mock is used.
 *
 * SPEC §5.7, §4.2–4.5, §2.5, §11.2 item 3.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <systemd/sd-bus.h>

#include <SDL2/SDL.h>

#include "app/overlay_service.h"
#include "dbus/dbus_client.h"
#include "dbus/ip_connection.h"
#include "dbus/ip_composite.h"
#include "dbus/ip_intercept_poll.h"
#include "dbus/ip_input_signal.h"
#include "dbus/ip_hotplug.h"
#include "dbus/ip_objectmanager.h"
#include "dbus/ip_device_model.h"
#include "dbus/ip_manager.h"
#include "dbus/ip_target.h"
#include "dbus/dbus_interface.h"             /* IP_DBUS_NAME, IP_IFACE_*, constants only */
#include "config/config_settings.h"
#include "interaction_inventory.h"
#include "config/config_assignments.h"
#include "config/config_profile_list.h"
#include "config/config_paths.h"
#include "ui/renderer.h"
#include "overlay/surface_build.h"
#include "overlay/lifecycle.h"
#include "overlay/grid_render.h"
#include "overlay/player_mode.h"
#include "overlay/host_mode.h"
#include "overlay/conflict.h"
#include "overlay/profile_cycle.h"
#include "overlay/trigger.h"
#include "native_ip_server.h"

#ifndef DBUS_SESSION_CONFIG
#error "DBUS_SESSION_CONFIG must be defined (path to dbus session.conf)"
#endif

/* --- Constants --------------------------------------------------------- */

#define COMP_PATH_0 "/org/shadowblip/InputPlumber/CompositeDevice0"
#define COMP_PATH_1 "/org/shadowblip/InputPlumber/CompositeDevice1"
#define MGR_PATH    "/org/shadowblip/InputPlumber/Manager"

/* --- Fixture ----------------------------------------------------------- */

typedef struct {
    cbx_overlay_service_ctx *svc;
    char    bus_address[512];
    pid_t   daemon_pid;
    pid_t   server_pid;
    char    tmp_home[PATH_MAX];
    const ip_dbus_backend *backend;
    ip_bus_handle bus;       /* independent verification connection */
    char    expected_sender[128];
} native_fixture;

/* --- Helpers ----------------------------------------------------------- */

static void drain_bus(const ip_dbus_backend *backend, ip_bus_handle bus, int ms)
{
    for (int i = 0; i < ms / 10; i++) {
        int processed = backend->process(bus);
        if (processed <= 0)
            usleep(10000);
    }
}

static int wait_for_server(const ip_dbus_backend *backend, ip_bus_handle bus,
                            const char *version)
{
    char *value = NULL;
    int rc = -1;
    for (int i = 0; i < 200 && rc != 0; i++) {
        free(value); value = NULL;
        rc = backend->get_property(bus, IP_DBUS_NAME,
            MGR_PATH, IP_IFACE_MANAGER, "Version", &value);
        if (rc != 0) usleep(10000);
    }
    if (rc == 0 && version)
        rc = (value && strcmp(value, version) == 0) ? 0 : -1;
    free(value);
    return rc;
}

static void push_keydown(SDL_Keycode sym)
{
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = sym;
    SDL_PushEvent(&ev);
}

static void push_poll_event(uint32_t event_type, ip_intercept_poll *owner)
{
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = event_type;
    ev.user.code = 0;
    /* Match the production SDL timer callback: the owning poll travels in
     * event.user.data1, and the step loop ticks only that poll. */
    ev.user.data1 = owner;
    ev.user.data2 = NULL;
    SDL_PushEvent(&ev);
}

/* Activate the overlay through the production InterceptMode poll path,
 * exercising the full activation lifecycle (IDLE → PASS_WAIT → ACTIVE →
 * VISIBLE) including the on_intercept_activating callback,
 * cbx_overlay_lifecycle_activate, surface show, and on_visible callback.
 *
 * The IDLE→PASS_WAIT transition is driven by ip_intercept_poll_start()
 * (the same function the production loop uses to arm a poll) rather than
 * by directly mutating poll->state.  The subsequent PASS_WAIT→ACTIVE
 * transition is driven by ip_intercept_poll_tick() reading InterceptMode. */
static void activate_overlay(native_fixture *f)
{
    cbx_overlay_service_ctx *svc = f->svc;
    assert_int_equal(svc->lifecycle.state, CBX_OVERLAY_IDLE);
    assert_int_equal(ip_intercept_poll_start(&svc->polls[0],
                      IP_INTERCEPT_POLL_INTERVAL_MS,
                      svc->poll_event_type), 0);
    assert_int_equal(svc->polls[0].state, IP_POLL_PASS_WAIT);
    assert_int_equal(ip_composite_set_intercept_mode(
        svc->conn.backend, svc->conn.bus, COMP_PATH_0, "2"), 0);
    push_poll_event(svc->poll_event_type, &svc->polls[0]);
    cbx_overlay_service_step(svc);
    assert_int_equal(svc->lifecycle.state, CBX_OVERLAY_VISIBLE);
}

/* Emit an InputEvent signal on the native server by calling the
 * EmitInputEvent(ss) method. The server converts to double and emits
 * the real InputEvent(sd) signal. */
static void emit_input_event(const ip_dbus_backend *backend, ip_bus_handle bus,
                              const char *comp_path, const char *event, double value)
{
    char val_str[32];
    snprintf(val_str, sizeof(val_str), "%.1f", value);
    int rc = backend->call_method(bus, IP_DBUS_NAME, comp_path,
                                   IP_IFACE_DBUS_DEVICE, "EmitInputEvent",
                                   "ss", event, val_str, NULL);
    assert_int_equal(rc, 0);
}

/* Flush remaining SDL events. */
static void flush_events(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {}
}

/* Create a minimal profile YAML file. */
static void create_profile_file(const char *dir, const char *filename,
                                  const char *profile_name)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, filename);
    FILE *fp = fopen(path, "w");
    assert_non_null(fp);
    if (profile_name)
        fprintf(fp, "name: %s\ndescription: Test profile\n", profile_name);
    else
        fprintf(fp, "name: %s\ndescription: Test profile\n", filename);
    fclose(fp);
}

/* --- Poll callbacks (production paths) -------------------------------- */
/*
 * The native test registers the production on_intercept_activating /
 * on_intercept_deactivating / on_intercept_error callbacks (exposed under
 * CBX_TESTING from overlay_service.c) rather than re-implementing local
 * copies.  This keeps the test wiring identical to production so the
 * lifecycle behaviour under test cannot drift from the shipped code.
 */

/* --- Setup / Teardown -------------------------------------------------- */

static int native_setup(void **state)
{
    native_fixture *f = calloc(1, sizeof(*f));
    assert_non_null(f);

    /* 1. Create isolated HOME with profile files. */
    snprintf(f->tmp_home, sizeof(f->tmp_home),
             "/tmp/cbx_overlay_native_%d", (int)getpid());
    assert_int_equal(mkdir(f->tmp_home, 0700), 0);
    setenv("HOME", f->tmp_home, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");
    unsetenv("FLATPAK_ID");

    char prof_dir[PATH_MAX + 64];
    snprintf(prof_dir, sizeof(prof_dir), "%s/.local/share/inputplumber/profiles",
             f->tmp_home);
    /* Create nested directories. */
    char cmd[PATH_MAX + 128];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", prof_dir);
    assert_int_equal(system(cmd), 0);

    /* Create at least 2 profiles for cycling. */
    create_profile_file(prof_dir, "default.yaml", "Default");
    create_profile_file(prof_dir, "custom.yaml", "Custom");
    create_profile_file(prof_dir, "gamepad.yaml", "Gamepad");

    /* 2. Configure and start the native server. */
    nip_reset_server_state(2);
    for (int i = 0; i < 2; i++) {
        snprintf(g_nip_comp_names[i], sizeof(g_nip_comp_names[i]),
                 "TestController%d", i);
        snprintf(g_nip_persistent_ids[i], sizeof(g_nip_persistent_ids[i]),
                 "ORDER:%d", i);
        /* DbusDevices points to the composite path so InputEvent signals
         * emitted on that path map to the correct row. */
        snprintf(g_nip_dbus_devices[i], sizeof(g_nip_dbus_devices[i]),
                 "/org/shadowblip/InputPlumber/CompositeDevice%d", i);
        g_nip_intercept_mode[i] = IP_INTERCEPT_PASS; /* start in PASS */
    }

    const nip_server_config cfg = { .num_composites = 2, .version = "0.78.0" };
    nip_server_handle sh;
    assert_int_equal(nip_start_server(&sh, &cfg), 0);
    snprintf(f->bus_address, sizeof(f->bus_address), "%s", sh.address);
    f->daemon_pid = sh.daemon_pid;
    f->server_pid = sh.server_pid;

    /* 3. Init SDL (dummy video). */
    SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "dummy", SDL_HINT_OVERRIDE);
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER);
    SDL_VideoInit("dummy");

    /* 4. Allocate overlay service context. */
    f->svc = calloc(1, sizeof(cbx_overlay_service_ctx));
    assert_non_null(f->svc);

    /* 5. Renderer. */
    assert_int_equal(cbx_renderer_init(&f->svc->rend, "test-overlay",
                                         CBX_RENDERER_DEFAULT_W,
                                         CBX_RENDERER_DEFAULT_H, false), 0);

    /* 6. Connect to DBus via real sd-bus. */
    f->backend = ip_dbus_sd_backend();
    ip_connection_init(&f->svc->conn, f->backend);
    int conn_rc = ip_connection_connect(&f->svc->conn);
    assert_int_equal(conn_rc, 0);
    f->svc->backend_ready = ip_connection_is_connected(&f->svc->conn);
    assert_true(f->svc->backend_ready);

    /* Wait for server readiness. */
    assert_int_equal(wait_for_server(f->svc->conn.backend, f->svc->conn.bus,
                                      "0.78.0"), 0);

    /* 7. Enumerate devices. */
    cbx_device_model_init(&f->svc->model);
    assert_int_equal(cbx_objectmanager_enumerate(f->svc->conn.backend,
                                                   f->svc->conn.bus,
                                                   &f->svc->model), 0);
    assert_int_equal(f->svc->model.composite_count, 2);

    /* 8. Settings + assignments. */
    cbx_settings_defaults(&f->svc->settings);
    /* Ensure 4 virtual controllers for P1–P4 columns. */
    f->svc->settings.virtual_controllers.count = 4;
    cbx_assignments_init(&f->svc->assignments);
    /* Use a temp assignments file in the isolated HOME. */
    /* cbx_assignments_load will look in $HOME/.config/controller-box/. */

    /* 9. Create the exact P1-P4 topology used by assignment tests. */
    static const char *types[] = { "xb360", "ds5", "xb360", "ds5" };
    for (int i = 0; i < 4; i++) {
        char *target_path = NULL;
        assert_int_equal(ip_manager_create_target_device(f->svc->conn.backend,
            f->svc->conn.bus, types[i], &target_path), 0);
        assert_non_null(target_path);
        free(target_path);
    }

    /* Re-enumerate to pick up the new targets. */
    assert_int_equal(cbx_objectmanager_enumerate(f->svc->conn.backend,
                                                   f->svc->conn.bus,
                                                   &f->svc->model), 0);
    assert_int_equal(f->svc->model.target_count, 4);

    /* 10. Build composite info from device model. */
    f->svc->comp_count = f->svc->model.composite_count;
    for (int i = 0; i < f->svc->comp_count; i++) {
        char *id = NULL;
        ip_composite_get_persistent_id(f->svc->conn.backend, f->svc->conn.bus,
                                         f->svc->model.composites[i].path, &id);
        char tmp_path[CBX_MAX_PATH_LEN];
        snprintf(tmp_path, sizeof(tmp_path), "%s", f->svc->model.composites[i].path);
        snprintf(f->svc->composites[i].composite_path,
                 sizeof(f->svc->composites[i].composite_path),
                 "%s", tmp_path);
        if (id) {
            snprintf(f->svc->composites[i].id,
                     sizeof(f->svc->composites[i].id),
                     "%s", id);
        } else {
            snprintf(f->svc->composites[i].id,
                     sizeof(f->svc->composites[i].id),
                     "ORDER:%d", i);
        }
        free(id);
        char *name = NULL;
        ip_composite_get_name(f->svc->conn.backend, f->svc->conn.bus,
                               f->svc->model.composites[i].path, &name);
        snprintf(f->svc->composites[i].model_name,
                 sizeof(f->svc->composites[i].model_name),
                 "%s", name ? name : "Controller");
        free(name);
    }

    /* 11. Build grid. */
    cbx_select_grid_init(&f->svc->grid);
    assert_int_equal(cbx_select_grid_build(&f->svc->grid, f->svc->composites,
                                            f->svc->comp_count,
                                            &f->svc->settings,
                                            &f->svc->assignments), 0);

    /* 12. Enumerate profiles and load into grid. */
    assert_int_equal(cbx_profile_list_enumerate(&f->svc->profiles), 0);
    assert_int_equal(cbx_profile_cycle_load_profiles(&f->svc->grid,
                                                        &f->svc->profiles), 0);

    /* Set row 0's profile to the first profile (for cycling tests). */
    if (f->svc->grid.profile_count > 0) {
        char tmp_prof[CBX_MAX_PROFILE_LEN];
        snprintf(tmp_prof, sizeof(tmp_prof), "%s", f->svc->grid.profiles[0]);
        snprintf(f->svc->grid.rows[0].profile,
                 sizeof(f->svc->grid.rows[0].profile),
                 "%s", tmp_prof);
    }

    /* 13. Init profile cycle (backend, bus, assignments, profiles). */
    cbx_profile_cycle_init(&f->svc->profile_cycle, f->svc->conn.backend,
                            f->svc->conn.bus, &f->svc->assignments,
                            &f->svc->profiles);

    /* 14. Overlay surface. */
    assert_int_equal(cbx_overlay_surface_init(&f->svc->surface,
                                                f->svc->rend.renderer,
                                                CBX_RENDERER_DEFAULT_W,
                                                CBX_RENDERER_DEFAULT_H,
                                                1.0), 0);

    /* 15. Render context (minimal — no fonts/icons/theme for interaction tests). */
    f->svc->render_ctx.grid = &f->svc->grid;
    f->svc->render_ctx.icon_cache = NULL;
    f->svc->render_ctx.icon_map = NULL;
    f->svc->render_ctx.theme = NULL;
    f->svc->render_ctx.text_cache = NULL;
    f->svc->render_ctx.font_id = -1;
    f->svc->render_ctx.settings = &f->svc->settings;

    /* 16. Lifecycle (fade = 0 for deterministic instant transitions). */
    cbx_overlay_lifecycle_init(&f->svc->lifecycle, f->svc->conn.backend,
                                f->svc->conn.bus, COMP_PATH_0,
                                &f->svc->surface, f->svc->rend.renderer);
    f->svc->lifecycle.fade_in_ms = 0;
    f->svc->lifecycle.fade_out_ms = 0;
    f->svc->lifecycle.state = CBX_OVERLAY_IDLE;

    /* 17. Wire on_save. */
    f->svc->lifecycle.on_save = cbx_overlay_on_save;
    f->svc->lifecycle.on_save_data = f->svc;

    /* 17b. End Host Mode when the overlay closes (SPEC §4.4) — the exact
     * production wiring, so close/reopen is exercised on the real DBus
     * transport. */
    f->svc->lifecycle.on_closed = cbx_overlay_on_lifecycle_closed;
    f->svc->lifecycle.on_closed_data = f->svc;

    /* 18. Player mode + callbacks. */
    cbx_player_mode_init(&f->svc->pm, &f->svc->grid);
    f->svc->pm.on_slot_change = cbx_overlay_on_slot_change;
    f->svc->pm.slot_change_data = f->svc;
    f->svc->pm.on_profile_change = cbx_overlay_on_profile_change;
    f->svc->pm.profile_change_data = f->svc;

    /* 19. Host mode + callbacks. */
    cbx_host_mode_init(&f->svc->hm);
    f->svc->hm.on_slot_change = cbx_overlay_on_slot_change;
    f->svc->hm.slot_change_data = f->svc;
    /* Host Mode edits the selected row's profile too (L1/R1); reuse the
     * production profile-change callback exactly as Player Mode does. */
    f->svc->hm.on_profile_change = cbx_overlay_on_profile_change;
    f->svc->hm.profile_change_data = f->svc;
    /* Host-mode enter/exit marks the surface dirty (W1). */
    f->svc->hm.on_state_change = cbx_overlay_on_host_mode_change;
    f->svc->hm.state_change_data = f->svc;

    /* 20. Build input map from real DBus (queries DbusDevices property). */
    f->svc->input_ctx.pm = &f->svc->pm;
    f->svc->input_ctx.hm = &f->svc->hm;
    f->svc->input_ctx.grid = &f->svc->grid;
    f->svc->input_ctx.lifecycle = &f->svc->lifecycle;
    f->svc->input_ctx.path_count = 0;
    cbx_overlay_input_build_map(f->svc->conn.backend, f->svc->conn.bus,
                                  f->svc->composites, f->svc->comp_count,
                                  &f->svc->input_ctx);
    assert_int_equal(f->svc->input_ctx.path_count, 2);

    /* 21. Input event subscription. */
    const char *uniq = ip_connection_get_unique_name(&f->svc->conn);
    snprintf(f->svc->expected_sender, sizeof(f->svc->expected_sender),
             "%s", uniq ? uniq : "");
    snprintf(f->expected_sender, sizeof(f->expected_sender), "%s",
             f->svc->expected_sender);
    ip_input_events_init(&f->svc->input_events, f->svc->conn.backend,
                          f->svc->conn.bus, f->svc->expected_sender,
                          cbx_overlay_input_cb, &f->svc->input_ctx);
    f->svc->input_events_ready = false;
    if (ip_input_events_subscribe(&f->svc->input_events) == 0)
        f->svc->input_events_ready = true;
    assert_true(f->svc->input_events_ready);

    /* 22. InterceptMode polling (init only — no timer for determinism). */
    f->svc->poll_event_type = SDL_RegisterEvents(1);
    f->svc->poll_count = 0;
    for (int i = 0; i < f->svc->comp_count && i < CBX_MAX_COMPOSITES; i++) {
        f->svc->poll_acts[i].lifecycle = &f->svc->lifecycle;
        {
            char tmp_cp[CBX_MAX_PATH_LEN];
            snprintf(tmp_cp, sizeof(tmp_cp), "%s",
                     f->svc->composites[i].composite_path);
            snprintf(f->svc->poll_acts[i].composite_path,
                     sizeof(f->svc->poll_acts[i].composite_path),
                     "%s", tmp_cp);
        }
        ip_intercept_poll_init(&f->svc->polls[i],
                                f->svc->conn.backend, f->svc->conn.bus,
                                f->svc->composites[i].composite_path,
                                on_intercept_activating, &f->svc->poll_acts[i],
                                on_intercept_deactivating, &f->svc->lifecycle,
                                on_intercept_error, NULL);
        f->svc->poll_count++;
    }

    /* 23. Register triggers (SetInterceptActivation + InterceptMode=PASS). */
    {
        const char *paths[CBX_MAX_COMPOSITES];
        for (int i = 0; i < f->svc->comp_count; i++)
            paths[i] = f->svc->composites[i].composite_path;
        cbx_trigger_register_all(f->svc->conn.backend, f->svc->conn.bus,
                                    paths, f->svc->comp_count,
                                    f->svc->settings.overlay_trigger);
    }

    /* 24. Independent verification connection. */
    f->bus = NULL;
    assert_int_equal(f->backend->connect(&f->bus), 0);

    /* 25. Finalize. */
    f->svc->initialized = true;
    cbx_overlay_service_reset_shutdown();

    *state = f;
    return 0;
}

static int native_teardown(void **state)
{
    native_fixture *f = *state;

    /* Stop polls. */
    for (int i = 0; i < f->svc->poll_count; i++)
        ip_intercept_poll_stop(&f->svc->polls[i]);

    /* Destroy surface + shutdown renderer. */
    cbx_overlay_surface_destroy(&f->svc->surface);
    cbx_renderer_shutdown(&f->svc->rend);

    /* Disconnect DBus. */
    if (f->bus)
        f->backend->disconnect(f->bus);
    ip_connection_disconnect(&f->svc->conn);

    free(f->svc);

    /* Kill server + daemon. */
    if (f->server_pid > 1) {
        kill(f->server_pid, SIGTERM);
        waitpid(f->server_pid, NULL, 0);
    }
    if (f->daemon_pid > 1) {
        kill(f->daemon_pid, SIGTERM);
        waitpid(f->daemon_pid, NULL, 0);
    }
    unsetenv("DBUS_SYSTEM_BUS_ADDRESS");

    SDL_Quit();

    /* Clean up temp HOME. */
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", f->tmp_home);
    int _rc = system(cmd);
    (void)_rc;

    flush_events();
    free(f);
    return 0;
}

/* ================================================================== */
/*  Tests                                                              */
/* ================================================================== */

/* --- O01: Open (activation via InterceptMode poll) --- */

static void test_o01_open_activates(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    /* Verify starting state. */
    assert_int_equal(svc->lifecycle.state, CBX_OVERLAY_IDLE);

    /* Arm the poll via the production IDLE→PASS_WAIT transition. */
    assert_int_equal(ip_intercept_poll_start(&svc->polls[0],
                      IP_INTERCEPT_POLL_INTERVAL_MS,
                      svc->poll_event_type), 0);
    assert_int_equal(svc->polls[0].state, IP_POLL_PASS_WAIT);

    /* Set InterceptMode = ALL (2) on server — native u type on wire. */
    assert_int_equal(ip_composite_set_intercept_mode(
        svc->conn.backend, svc->conn.bus, COMP_PATH_0, "2"), 0);

    /* Verify InterceptMode was set as u on the wire by reading it back. */
    char *mode_str = NULL;
    assert_int_equal(ip_composite_get_intercept_mode(
        svc->conn.backend, svc->conn.bus, COMP_PATH_0, &mode_str), 0);
    assert_string_equal(mode_str, "2");
    free(mode_str);

    /* Push poll event and step. */
    push_poll_event(svc->poll_event_type, &svc->polls[0]);
    cbx_overlay_service_step(svc);

    /* Overlay should be visible. */
    assert_int_equal(svc->lifecycle.state, CBX_OVERLAY_VISIBLE);
}

/* --- O01b: Deactivation via poll --- */

static void test_o01b_deactivation_closes(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    activate_overlay(f);

    /* activate_overlay() leaves the poll in ACTIVE; make the state explicit
     * for the deactivation step below (no manual state mutation). */
    assert_int_equal(svc->polls[0].state, IP_POLL_ACTIVE);

    /* Set InterceptMode = PASS (1) on server. */
    assert_int_equal(ip_composite_set_intercept_mode(
        svc->conn.backend, svc->conn.bus, COMP_PATH_0, "1"), 0);

    push_poll_event(svc->poll_event_type, &svc->polls[0]);
    cbx_overlay_service_step(svc);

    assert_int_equal(svc->lifecycle.state, CBX_OVERLAY_IDLE);
}

/* --- O02: Move left (Player Mode) --- */

static void test_o02_move_left(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    activate_overlay(f);

    /* Move right first so LEFT is not at boundary. */
    push_keydown(SDLK_RIGHT);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 0), 1);

    /* Move left back to Unassigned. */
    push_keydown(SDLK_LEFT);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 0), 0);
}

/* --- O03: Move right (Player Mode) --- */

static void test_o03_move_right(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    activate_overlay(f);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 0), 0);

    push_keydown(SDLK_RIGHT);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 0), 1);
}

/* --- O04: Cycle profile up --- */

static void test_o04_cycle_profile_up(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    activate_overlay(f);

    /* Verify we have at least 2 profiles. */
    assert_true(svc->grid.profile_count >= 2);

    /* Capture current profile (must copy — cycle modifies in place). */
    char prof_before[CBX_GRID_PROFILE_LEN];
    snprintf(prof_before, sizeof(prof_before), "%s",
             cbx_select_grid_get_profile(&svc->grid, 0));
    assert_true(prof_before[0] != '\0');

    push_keydown(SDLK_UP);
    cbx_overlay_service_step(svc);

    /* Profile should have changed. */
    const char *prof_after = cbx_select_grid_get_profile(&svc->grid, 0);
    assert_non_null(prof_after);
    assert_string_not_equal(prof_after, prof_before);

    /* Verify LoadProfilePath DBus call was actually made on the wire —
     * read back the ProfilePath property on the composite device and
     * confirm it matches the new profile's path.  Checking only the
     * in-memory grid name doesn't prove the backend was called. */
    char *engine_path = NULL;
    int rc = ip_composite_get_profile_path(svc->conn.backend,
                                             svc->conn.bus,
                                             COMP_PATH_0, &engine_path);
    assert_int_equal(rc, 0);
    assert_non_null(engine_path);
    /* The engine path should contain the new profile name. */
    assert_non_null(strstr(engine_path, prof_after));
    free(engine_path);
}

/* --- O05: Cycle profile down --- */

static void test_o05_cycle_profile_down(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    activate_overlay(f);
    assert_true(svc->grid.profile_count >= 2);

    /* Capture current profile (must copy — cycle modifies in place). */
    char prof_before[CBX_GRID_PROFILE_LEN];
    snprintf(prof_before, sizeof(prof_before), "%s",
             cbx_select_grid_get_profile(&svc->grid, 0));
    assert_true(prof_before[0] != '\0');

    push_keydown(SDLK_DOWN);
    cbx_overlay_service_step(svc);

    const char *prof_after = cbx_select_grid_get_profile(&svc->grid, 0);
    assert_non_null(prof_after);
    assert_string_not_equal(prof_after, prof_before);

    /* Verify LoadProfilePath DBus call was actually made on the wire —
     * read back the ProfilePath property and confirm it matches the
     * new profile. */
    char *engine_path = NULL;
    int rc = ip_composite_get_profile_path(svc->conn.backend,
                                             svc->conn.bus,
                                             COMP_PATH_0, &engine_path);
    assert_int_equal(rc, 0);
    assert_non_null(engine_path);
    assert_non_null(strstr(engine_path, prof_after));
    free(engine_path);
}

/* --- O06: Enter Host Mode (R3) --- */

static void test_o06_enter_host_mode(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    activate_overlay(f);
    assert_false(cbx_host_mode_is_active(&svc->hm));

    push_keydown(SDLK_r);
    cbx_overlay_service_step(svc);

    assert_true(cbx_host_mode_is_active(&svc->hm));
    assert_int_equal(cbx_host_mode_get_host_row(&svc->hm), 0);
}

/* --- O06b: Host mode freezes non-host controllers --- */

static void test_o06b_host_freezes_non_host(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    activate_overlay(f);

    /* Before entering host mode, prove the InputEvent signal path works
     * for COMP_PATH_1 by moving row 1 to col 1.  This establishes a
     * non-zero baseline so a frozen-row assertion is meaningful. */
    emit_input_event(svc->conn.backend, svc->conn.bus, COMP_PATH_1, "Right", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 1), 1);

    /* Enter host mode via keyboard (acts as row 0). */
    push_keydown(SDLK_r);
    cbx_overlay_service_step(svc);
    assert_true(cbx_host_mode_is_active(&svc->hm));

    /* Controller 1 (non-host) sends Right via DBus InputEvent. */
    emit_input_event(svc->conn.backend, svc->conn.bus, COMP_PATH_1, "Right", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);

    /* Row 1 should be frozen — column unchanged at col 1 (not col 2). */
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 1), 1);
    assert_int_equal(cbx_host_mode_row_state(&svc->hm, 1), CBX_ROW_FROZEN);
}

/* --- O07: Host: navigate rows --- */

static void test_o07_host_navigate_rows(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    activate_overlay(f);

    /* Enter host mode. */
    push_keydown(SDLK_r);
    cbx_overlay_service_step(svc);
    assert_true(cbx_host_mode_is_active(&svc->hm));
    assert_int_equal(cbx_host_mode_get_selected_row(&svc->hm), 0);

    /* Navigate down to row 1. */
    push_keydown(SDLK_DOWN);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_host_mode_get_selected_row(&svc->hm), 1);

    /* Navigate back up to row 0. */
    push_keydown(SDLK_UP);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_host_mode_get_selected_row(&svc->hm), 0);
}

/* --- O08: Host: move slot --- */

static void test_o08_host_move_slot(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    activate_overlay(f);

    /* Enter host mode. */
    push_keydown(SDLK_r);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_host_mode_get_selected_row(&svc->hm), 0);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 0), 0);

    /* Move selected row right. */
    push_keydown(SDLK_RIGHT);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 0), 1);

    /* Move back left. */
    push_keydown(SDLK_LEFT);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 0), 0);
}

/* --- O09: Exit Host Mode (R3) --- */

static void test_o09_exit_host_mode(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    activate_overlay(f);

    /* Enter host mode. */
    push_keydown(SDLK_r);
    cbx_overlay_service_step(svc);
    assert_true(cbx_host_mode_is_active(&svc->hm));

    /* Exit host mode. */
    push_keydown(SDLK_r);
    cbx_overlay_service_step(svc);
    assert_false(cbx_host_mode_is_active(&svc->hm));
}

/* ================================================================== */
/*  O02–O09 via DBus InputEvent (primary production transport)         */
/* ================================================================== */

/* --- O02d: Move left (Player Mode) via DBus InputEvent --- */

static void test_o02_move_left_dbus(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    activate_overlay(f);

    /* Move right first so LEFT is not at boundary. */
    emit_input_event(svc->conn.backend, svc->conn.bus,
                     COMP_PATH_0, "Right", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 0), 1);

    /* Move left back to Unassigned via DBus InputEvent. */
    emit_input_event(svc->conn.backend, svc->conn.bus,
                     COMP_PATH_0, "Left", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 0), 0);
}

/* --- O03d: Move right (Player Mode) via DBus InputEvent --- */

static void test_o03_move_right_dbus(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    activate_overlay(f);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 0), 0);

    emit_input_event(svc->conn.backend, svc->conn.bus,
                     COMP_PATH_0, "Right", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 0), 1);
}

/* --- O04d: Cycle profile up via DBus InputEvent --- */

static void test_o04_cycle_profile_up_dbus(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    activate_overlay(f);
    assert_true(svc->grid.profile_count >= 2);

    char prof_before[CBX_GRID_PROFILE_LEN];
    snprintf(prof_before, sizeof(prof_before), "%s",
             cbx_select_grid_get_profile(&svc->grid, 0));
    assert_true(prof_before[0] != '\0');

    emit_input_event(svc->conn.backend, svc->conn.bus,
                     COMP_PATH_0, "Up", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);

    const char *prof_after = cbx_select_grid_get_profile(&svc->grid, 0);
    assert_non_null(prof_after);
    assert_string_not_equal(prof_after, prof_before);

    /* Verify LoadProfilePath DBus call was made on the wire. */
    char *engine_path = NULL;
    int rc = ip_composite_get_profile_path(svc->conn.backend,
                                             svc->conn.bus,
                                             COMP_PATH_0, &engine_path);
    assert_int_equal(rc, 0);
    assert_non_null(engine_path);
    assert_non_null(strstr(engine_path, prof_after));
    free(engine_path);
}

/* --- O05d: Cycle profile down via DBus InputEvent --- */

static void test_o05_cycle_profile_down_dbus(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    activate_overlay(f);
    assert_true(svc->grid.profile_count >= 2);

    char prof_before[CBX_GRID_PROFILE_LEN];
    snprintf(prof_before, sizeof(prof_before), "%s",
             cbx_select_grid_get_profile(&svc->grid, 0));
    assert_true(prof_before[0] != '\0');

    emit_input_event(svc->conn.backend, svc->conn.bus,
                     COMP_PATH_0, "Down", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);

    const char *prof_after = cbx_select_grid_get_profile(&svc->grid, 0);
    assert_non_null(prof_after);
    assert_string_not_equal(prof_after, prof_before);

    /* Verify LoadProfilePath DBus call was made on the wire. */
    char *engine_path = NULL;
    int rc = ip_composite_get_profile_path(svc->conn.backend,
                                             svc->conn.bus,
                                             COMP_PATH_0, &engine_path);
    assert_int_equal(rc, 0);
    assert_non_null(engine_path);
    assert_non_null(strstr(engine_path, prof_after));
    free(engine_path);
}

/* --- O06d: Enter Host Mode (R3) via DBus InputEvent --- */

static void test_o06_enter_host_mode_dbus(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    activate_overlay(f);
    assert_false(cbx_host_mode_is_active(&svc->hm));

    emit_input_event(svc->conn.backend, svc->conn.bus,
                     COMP_PATH_0, "R3", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);

    assert_true(cbx_host_mode_is_active(&svc->hm));
    assert_int_equal(cbx_host_mode_get_host_row(&svc->hm), 0);
}

/* --- O07d: Host: navigate rows via DBus InputEvent --- */

static void test_o07_host_navigate_rows_dbus(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    activate_overlay(f);

    /* Enter host mode via DBus InputEvent. */
    emit_input_event(svc->conn.backend, svc->conn.bus,
                     COMP_PATH_0, "R3", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_true(cbx_host_mode_is_active(&svc->hm));
    assert_int_equal(cbx_host_mode_get_selected_row(&svc->hm), 0);

    /* Navigate down to row 1. */
    emit_input_event(svc->conn.backend, svc->conn.bus,
                     COMP_PATH_0, "Down", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_host_mode_get_selected_row(&svc->hm), 1);

    /* Navigate back up to row 0. */
    emit_input_event(svc->conn.backend, svc->conn.bus,
                     COMP_PATH_0, "Up", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_host_mode_get_selected_row(&svc->hm), 0);
}

/* --- O08d: Host: move slot via DBus InputEvent --- */

static void test_o08_host_move_slot_dbus(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    activate_overlay(f);

    /* Enter host mode via DBus InputEvent. */
    emit_input_event(svc->conn.backend, svc->conn.bus,
                     COMP_PATH_0, "R3", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_host_mode_get_selected_row(&svc->hm), 0);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 0), 0);

    /* Move selected row right. */
    emit_input_event(svc->conn.backend, svc->conn.bus,
                     COMP_PATH_0, "Right", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 0), 1);

    /* Move back left. */
    emit_input_event(svc->conn.backend, svc->conn.bus,
                     COMP_PATH_0, "Left", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 0), 0);
}

/* --- O09d: Exit Host Mode (R3) via DBus InputEvent --- */

static void test_o09_exit_host_mode_dbus(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    activate_overlay(f);

    /* Enter host mode via DBus InputEvent. */
    emit_input_event(svc->conn.backend, svc->conn.bus,
                     COMP_PATH_0, "R3", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_true(cbx_host_mode_is_active(&svc->hm));

    /* Exit host mode via DBus InputEvent. */
    emit_input_event(svc->conn.backend, svc->conn.bus,
                     COMP_PATH_0, "R3", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_false(cbx_host_mode_is_active(&svc->hm));
}

/* --- O10: Close (B) saves + sets PASS + hides --- */

static void test_o10_close_saves_and_sets_pass(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    activate_overlay(f);
    assert_int_equal(svc->assignments.assignment_count, 0);

    /* Move row 0 to col 1 (P1 slot). */
    push_keydown(SDLK_RIGHT);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 0), 1);

    /* Close via B through the production DBus InputEvent transport
     * (not the keyboard SDL path). */
    emit_input_event(svc->conn.backend, svc->conn.bus, COMP_PATH_0, "B", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);

    /* Overlay hidden. */
    assert_int_equal(svc->lifecycle.state, CBX_OVERLAY_IDLE);

    /* Assignment saved. */
    assert_int_equal(svc->assignments.assignment_count, 1);
    assert_int_equal(svc->assignments.assignments[0].slot, 0);

    /* Verify InterceptMode was set to PASS (1) on the wire. */
    char *mode_str = NULL;
    assert_int_equal(ip_composite_get_intercept_mode(
        svc->conn.backend, svc->conn.bus, COMP_PATH_0, &mode_str), 0);
    assert_string_equal(mode_str, "1");
    free(mode_str);

    /* Verify assignments were persisted to disk — not just held in
     * memory.  Read back assignments.yaml and confirm the slot and
     * device path match. */
    {
        cbx_assignments loaded;
        memset(&loaded, 0, sizeof(loaded));
        int load_rc = cbx_assignments_load(&loaded);
        assert_int_equal(load_rc, 0);
        assert_int_equal(loaded.assignment_count, 1);
        assert_int_equal(loaded.assignments[0].slot, 0);
    }

    /* All assertions passed — record in the runtime verification ledger. */
    assert_int_equal(cbx_interaction_inventory_mark_verified("O10"), 0);
}

/* --- O10b/O13: Close with conflict resolution --- */

static void test_o10b_close_conflict_resolution(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    activate_overlay(f);

    /* Move row 0 to col 1 (P1). */
    push_keydown(SDLK_RIGHT);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 0), 1);

    /* Enter host mode to move row 1. */
    push_keydown(SDLK_r);
    cbx_overlay_service_step(svc);
    assert_true(cbx_host_mode_is_active(&svc->hm));

    /* Navigate to row 1. */
    push_keydown(SDLK_DOWN);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_host_mode_get_selected_row(&svc->hm), 1);

    /* Move row 1 right to col 1 — same as row 0 = conflict. */
    push_keydown(SDLK_RIGHT);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 1), 1);

    /* Exit host mode. */
    push_keydown(SDLK_r);
    cbx_overlay_service_step(svc);
    assert_false(cbx_host_mode_is_active(&svc->hm));

    /* Close — conflict should be auto-resolved (DBus InputEvent). */
    emit_input_event(svc->conn.backend, svc->conn.bus, COMP_PATH_0, "B", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_int_equal(svc->lifecycle.state, CBX_OVERLAY_IDLE);

    /* Row 0 keeps P1, row 1 auto-moved to P2. */
    assert_int_equal(svc->grid.rows[0].cur_col, 1);
    assert_int_equal(svc->grid.rows[1].cur_col, 2);

    /* All assertions passed — record in the runtime verification ledger.
     * O10b is a close-with-conflict variant of the O10 close entry. */
    assert_int_equal(cbx_interaction_inventory_mark_verified("O10"), 0);
}

/* --- O10c: Host Mode ends with the overlay (SPEC §4.4) --- */
/*
 * Host Mode is scoped to one overlay session: the first controller to press
 * R3 becomes the exclusive host.  Closing the overlay from Host Mode must
 * end it, or the next activation presents every non-host controller frozen
 * even though no controller pressed R3 in the new session.  Drives the real
 * native DBus InputEvent transport for entry and B-close, then re-activates
 * through the production lifecycle API.
 */
static void
test_host_mode_exits_on_close_native(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    activate_overlay(f);

    /* Controller 0 enters Host Mode via the real DBus InputEvent path. */
    emit_input_event(svc->conn.backend, svc->conn.bus, COMP_PATH_0, "R3", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_true(cbx_host_mode_is_active(&svc->hm));
    assert_int_equal(cbx_host_mode_get_host_row(&svc->hm), 0);
    /* Host Mode visuals are live: row 0 selected, row 1 frozen. */
    assert_int_equal(cbx_host_mode_row_state(&svc->hm, 0), CBX_ROW_SELECTED);
    assert_int_equal(cbx_host_mode_row_state(&svc->hm, 1), CBX_ROW_FROZEN);

    /* Host presses B via the real DBus path → close → IDLE.  on_closed ends
     * Host Mode so no frozen row leaks into the next session. */
    emit_input_event(svc->conn.backend, svc->conn.bus, COMP_PATH_0, "B", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_int_equal(svc->lifecycle.state, CBX_OVERLAY_IDLE);
    assert_false(cbx_host_mode_is_active(&svc->hm));
    for (int i = 0; i < svc->grid.row_count; i++) {
        assert_int_equal(cbx_host_mode_row_state(&svc->hm, i),
                         CBX_ROW_NORMAL);
        assert_false(cbx_host_mode_is_frozen(&svc->hm, i));
    }

    /* Re-activate: the new session starts in Player Mode, so controller 1
     * (frozen last session) can become the new host. */
    assert_int_equal(cbx_overlay_lifecycle_activate(&svc->lifecycle), 0);
    cbx_overlay_service_step(svc);
    assert_int_equal(svc->lifecycle.state, CBX_OVERLAY_VISIBLE);
    assert_false(cbx_host_mode_is_active(&svc->hm));

    emit_input_event(svc->conn.backend, svc->conn.bus, COMP_PATH_1, "R3", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_true(cbx_host_mode_is_active(&svc->hm));
    assert_int_equal(cbx_host_mode_get_host_row(&svc->hm), 1);
}

/* --- O11: Multi-controller independence via DBus InputEvent --- */

static void test_o11_multi_controller_independent(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    activate_overlay(f);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 0), 0);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 1), 0);

    /* Controller 0 sends Right via DBus InputEvent. */
    emit_input_event(svc->conn.backend, svc->conn.bus, COMP_PATH_0, "Right", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 0), 1);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 1), 0);

    /* Controller 1 sends Right via DBus InputEvent. */
    emit_input_event(svc->conn.backend, svc->conn.bus, COMP_PATH_1, "Right", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 0), 1);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 1), 1);

    /* Controller 0 sends Left. */
    emit_input_event(svc->conn.backend, svc->conn.bus, COMP_PATH_0, "Left", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 0), 0);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 1), 1);
}

/* --- O11b: Host mode via DBus InputEvent --- */

static void test_o11b_host_mode_via_dbus(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    activate_overlay(f);

    /* Controller 0 enters host mode via R3. */
    emit_input_event(svc->conn.backend, svc->conn.bus, COMP_PATH_0, "R3", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_true(cbx_host_mode_is_active(&svc->hm));
    assert_int_equal(cbx_host_mode_get_host_row(&svc->hm), 0);

    /* Controller 1 (frozen) sends Right. */
    emit_input_event(svc->conn.backend, svc->conn.bus, COMP_PATH_1, "Right", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 1), 0);

    /* Controller 0 navigates down to row 1. */
    emit_input_event(svc->conn.backend, svc->conn.bus, COMP_PATH_0, "Down", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_host_mode_get_selected_row(&svc->hm), 1);

    /* Controller 0 moves row 1 right. */
    emit_input_event(svc->conn.backend, svc->conn.bus, COMP_PATH_0, "Right", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 1), 1);

    /* Controller 0 exits host mode. */
    emit_input_event(svc->conn.backend, svc->conn.bus, COMP_PATH_0, "R3", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_false(cbx_host_mode_is_active(&svc->hm));
}

/* --- O11c: Unknown device path dropped --- */

static void test_o11c_unknown_device_dropped(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    activate_overlay(f);
    int col0_before = cbx_select_grid_get_cur_col(&svc->grid, 0);
    int col1_before = cbx_select_grid_get_cur_col(&svc->grid, 1);

    /* Emit from an unknown device path — the server has no vtable there,
     * so the method call will fail. No signal is emitted, so grid is unchanged. */
    int rc = svc->conn.backend->call_method(
        svc->conn.bus, IP_DBUS_NAME,
        "/org/shadowblip/InputPlumber/CompositeDevice99",
        IP_IFACE_DBUS_DEVICE, "EmitInputEvent",
        "ss", "Right", "1.0", NULL);
    assert_int_not_equal(rc, 0);  /* expected to fail — no such path */
    cbx_overlay_service_step(svc);

    /* No change — unknown device dropped. */
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 0), col0_before);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 1), col1_before);
}

/* --- O12: Host profile cycle (L1/R1) via production DBus InputEvent ---
 *
 * SPEC §4.4: the exclusive host can navigate to any row and edit both the
 * slot and the profile.  Host Mode's documented profile affordance is
 * L1 = previous profile, R1 = next profile for the selected row. */

static void test_o12_host_profile_cycle_dbus(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    activate_overlay(f);
    assert_true(svc->grid.profile_count >= 2);

    /* Enter host mode from controller 0 (COMP_PATH_0 -> row 0). */
    emit_input_event(svc->conn.backend, svc->conn.bus,
                     COMP_PATH_0, "R3", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_true(cbx_host_mode_is_active(&svc->hm));
    assert_int_equal(cbx_host_mode_get_selected_row(&svc->hm), 0);

    /* Host navigates to row 1 (not its own row). */
    emit_input_event(svc->conn.backend, svc->conn.bus,
                     COMP_PATH_0, "Down", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_host_mode_get_selected_row(&svc->hm), 1);

    char prof_row0_before[CBX_GRID_PROFILE_LEN];
    char prof_before[CBX_GRID_PROFILE_LEN];
    snprintf(prof_row0_before, sizeof(prof_row0_before), "%s",
             cbx_select_grid_get_profile(&svc->grid, 0));
    snprintf(prof_before, sizeof(prof_before), "%s",
             cbx_select_grid_get_profile(&svc->grid, 1));

    /* R1 (right bumper) cycles the selected row's profile to the next. */
    emit_input_event(svc->conn.backend, svc->conn.bus,
                     COMP_PATH_0, "R1", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);

    const char *after = cbx_select_grid_get_profile(&svc->grid, 1);
    assert_non_null(after);
    assert_string_not_equal(after, prof_before);
    /* The host's own row was not touched — only the selected row changed. */
    assert_string_equal(cbx_select_grid_get_profile(&svc->grid, 0),
                        prof_row0_before);

    /* LoadProfilePath reached the engine for row 1's composite path. */
    char *engine_path = NULL;
    int rc = ip_composite_get_profile_path(svc->conn.backend,
                                             svc->conn.bus,
                                             COMP_PATH_1, &engine_path);
    assert_int_equal(rc, 0);
    assert_non_null(engine_path);
    assert_non_null(strstr(engine_path, after));
    free(engine_path);

    /* L1 cycles back to the previous profile. */
    emit_input_event(svc->conn.backend, svc->conn.bus,
                     COMP_PATH_0, "L1", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_string_equal(cbx_select_grid_get_profile(&svc->grid, 1),
                        prof_before);

    /* The passing production-dispatch test flips the O12 runtime ledger. */
    assert_int_equal(cbx_interaction_inventory_mark_verified("O12"), 0);
}

/* --- O12b: a frozen controller cannot cycle any profile --- */

static void test_o12_host_profile_frozen_dbus(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    activate_overlay(f);

    /* Controller 0 becomes the host. */
    emit_input_event(svc->conn.backend, svc->conn.bus,
                     COMP_PATH_0, "R3", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_true(cbx_host_mode_is_active(&svc->hm));

    char prof_row0[CBX_GRID_PROFILE_LEN];
    char prof_row1[CBX_GRID_PROFILE_LEN];
    snprintf(prof_row0, sizeof(prof_row0), "%s",
             cbx_select_grid_get_profile(&svc->grid, 0));
    snprintf(prof_row1, sizeof(prof_row1), "%s",
             cbx_select_grid_get_profile(&svc->grid, 1));

    /* Frozen controller 1 sends R1/L1: both must be rejected and no row
     * profile (host or frozen) may change. */
    emit_input_event(svc->conn.backend, svc->conn.bus,
                     COMP_PATH_1, "R1", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    emit_input_event(svc->conn.backend, svc->conn.bus,
                     COMP_PATH_1, "L1", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);

    assert_string_equal(cbx_select_grid_get_profile(&svc->grid, 0),
                        prof_row0);
    assert_string_equal(cbx_select_grid_get_profile(&svc->grid, 1),
                        prof_row1);
    /* Host mode is still owned by controller 0. */
    assert_true(cbx_host_mode_is_active(&svc->hm));
    assert_int_equal(cbx_host_mode_get_host_row(&svc->hm), 0);
}

/* --- Exact P1->P4 replacement and Unassigned clear --- */

static int csv_tokens(const char *csv)
{
    int n = 0;
    if (!csv) return 0;
    for (const char *p = csv; *p;) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        n++;
        const char *comma = strchr(p, ',');
        p = comma ? comma + 1 : p + strlen(p);
    }
    return n;
}

static void assert_exact_target_set(cbx_overlay_service_ctx *svc,
                                    const char *composite,
                                    const char *expected)
{
    char *actual = NULL;
    assert_int_equal(ip_composite_get_target_devices(svc->conn.backend,
        svc->conn.bus, composite, &actual), 0);
    if (expected) {
        assert_int_equal(csv_tokens(actual), 1);
        assert_string_equal(actual, expected);
    } else {
        assert_int_equal(csv_tokens(actual), 0);
    }
    free(actual);
}

static void test_assignment_replaces_p1_through_p4_then_clears(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    for (int slot = 0; slot < 4; slot++) {
        activate_overlay(f);
        emit_input_event(svc->conn.backend, svc->conn.bus,
                         COMP_PATH_0, "Right", 1.0);
        drain_bus(svc->conn.backend, svc->conn.bus, 100);
        cbx_overlay_service_step(svc);
        emit_input_event(svc->conn.backend, svc->conn.bus,
                         COMP_PATH_0, "B", 1.0);
        drain_bus(svc->conn.backend, svc->conn.bus, 100);
        cbx_overlay_service_step(svc);
        assert_int_equal(svc->lifecycle.state, CBX_OVERLAY_IDLE);
        assert_exact_target_set(svc, COMP_PATH_0,
                                svc->model.targets[slot].path);
        assert_exact_target_set(svc, COMP_PATH_1, NULL);
    }

    activate_overlay(f);
    for (int i = 0; i < 4; i++) {
        emit_input_event(svc->conn.backend, svc->conn.bus,
                         COMP_PATH_0, "Left", 1.0);
        drain_bus(svc->conn.backend, svc->conn.bus, 100);
        cbx_overlay_service_step(svc);
    }
    emit_input_event(svc->conn.backend, svc->conn.bus, COMP_PATH_0, "B", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_exact_target_set(svc, COMP_PATH_0, NULL);
}

/* --- Reactive PropertiesChanged via the real sd-bus path (Task 5) --------
 * Task 5 wires ip_properties into the overlay backend so that external
 * changes to GamepadOrder/ProfileName/ProfilePath/TargetDevices/
 * SourceDevicePaths (SPEC §10.1) are applied to the per-device model — the
 * matching composite entry, or the model's Manager ordering — instead of a
 * process-global cache.  These tests emit a faithful
 * org.freedesktop.DBus.Properties PropertiesChanged (sa{sv}as) signal from
 * the native server and drive the client's sd-bus process loop so the real
 * production parse path (sd_properties_changed_callback -> ip_properties ->
 * cbx_overlay_on_prop_change) is exercised, not a mock bypass. */

static void test_properties_changed_reactive_string(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    /* Wire the production PropertiesChanged subscription. */
    assert_int_equal(cbx_overlay_props_wire(svc), 0);

    /* Externally emit a ProfileName string change on composite path 1. */
    int rc = svc->conn.backend->call_method(svc->conn.bus, IP_DBUS_NAME,
        COMP_PATH_1, IP_IFACE_DBUS_DEVICE, "EmitStringProp",
        "ss", "ProfileName", "Reactive Name", NULL);
    assert_int_equal(rc, 0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);

    const cbx_composite_entry *e0 =
        cbx_device_model_find_composite(&svc->model, COMP_PATH_0);
    const cbx_composite_entry *e1 =
        cbx_device_model_find_composite(&svc->model, COMP_PATH_1);
    assert_non_null(e0);
    assert_non_null(e1);
    assert_true(e1->has_profile_name);
    assert_string_equal(e1->profile_name, "Reactive Name");
    assert_false(e0->has_profile_name);  /* other device untouched */

    /* And ProfilePath — also a tracked string property. */
    rc = svc->conn.backend->call_method(svc->conn.bus, IP_DBUS_NAME,
        COMP_PATH_1, IP_IFACE_DBUS_DEVICE, "EmitStringProp",
        "ss", "ProfilePath", "/tmp/some/profile.yaml", NULL);
    assert_int_equal(rc, 0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);

    e1 = cbx_device_model_find_composite(&svc->model, COMP_PATH_1);
    e0 = cbx_device_model_find_composite(&svc->model, COMP_PATH_0);
    assert_true(e1->has_profile_path);
    assert_string_equal(e1->profile_path, "/tmp/some/profile.yaml");
    assert_false(e0->has_profile_path);
}

static void test_properties_changed_reactive_array(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    assert_int_equal(cbx_overlay_props_wire(svc), 0);

    /* GamepadOrder — a Manager property: emit on the Manager path so the
     * interface/object-path validation accepts it. */
    int rc = svc->conn.backend->call_method(svc->conn.bus, IP_DBUS_NAME,
        MGR_PATH, IP_IFACE_DBUS_DEVICE, "EmitArrayProp",
        "sas", "GamepadOrder", "gp2,gp3,gp4,gp5", NULL);
    assert_int_equal(rc, 0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);

    assert_true(svc->model.has_gamepad_order);
    assert_string_equal(svc->model.gamepad_order, "gp2,gp3,gp4,gp5");

    /* TargetDevices — a per-composite array property on device 1. */
    rc = svc->conn.backend->call_method(svc->conn.bus, IP_DBUS_NAME,
        COMP_PATH_1, IP_IFACE_DBUS_DEVICE, "EmitArrayProp",
        "sas", "TargetDevices", "gamepad0", NULL);
    assert_int_equal(rc, 0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);

    const cbx_composite_entry *e1 =
        cbx_device_model_find_composite(&svc->model, COMP_PATH_1);
    const cbx_composite_entry *e0 =
        cbx_device_model_find_composite(&svc->model, COMP_PATH_0);
    assert_true(e1->has_target_devices);
    assert_string_equal(e1->target_devices, "gamepad0");
    assert_false(e0->has_target_devices);

    /* SourceDevicePaths — a per-composite array property on device 1. */
    rc = svc->conn.backend->call_method(svc->conn.bus, IP_DBUS_NAME,
        COMP_PATH_1, IP_IFACE_DBUS_DEVICE, "EmitArrayProp",
        "sas", "SourceDevicePaths", "/dev/input/event9", NULL);
    assert_int_equal(rc, 0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);

    e1 = cbx_device_model_find_composite(&svc->model, COMP_PATH_1);
    assert_true(e1->has_source_device_paths);
    assert_string_equal(e1->source_device_paths, "/dev/input/event9");
}

/* Two composites: a change for one device must not overwrite the other. */
static void test_properties_changed_two_devices_isolated(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    assert_int_equal(cbx_overlay_props_wire(svc), 0);

    assert_int_equal(svc->conn.backend->call_method(svc->conn.bus, IP_DBUS_NAME,
        COMP_PATH_0, IP_IFACE_DBUS_DEVICE, "EmitStringProp",
        "ss", "ProfileName", "Alpha", NULL), 0);
    assert_int_equal(svc->conn.backend->call_method(svc->conn.bus, IP_DBUS_NAME,
        COMP_PATH_1, IP_IFACE_DBUS_DEVICE, "EmitStringProp",
        "ss", "ProfileName", "Beta", NULL), 0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);

    const cbx_composite_entry *e0 =
        cbx_device_model_find_composite(&svc->model, COMP_PATH_0);
    const cbx_composite_entry *e1 =
        cbx_device_model_find_composite(&svc->model, COMP_PATH_1);
    assert_string_equal(e0->profile_name, "Alpha");
    assert_string_equal(e1->profile_name, "Beta");

    /* A later change on device 0 still leaves device 1 alone. */
    assert_int_equal(svc->conn.backend->call_method(svc->conn.bus, IP_DBUS_NAME,
        COMP_PATH_0, IP_IFACE_DBUS_DEVICE, "EmitStringProp",
        "ss", "ProfileName", "Alpha2", NULL), 0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);

    e0 = cbx_device_model_find_composite(&svc->model, COMP_PATH_0);
    e1 = cbx_device_model_find_composite(&svc->model, COMP_PATH_1);
    assert_string_equal(e0->profile_name, "Alpha2");
    assert_string_equal(e1->profile_name, "Beta");
}

/* A ProfilePath change updates the rendered grid row for the correct device
 * only, proving the displayed device follows the reported object path. */
static void test_properties_changed_grid_profile(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    assert_int_equal(cbx_overlay_props_wire(svc), 0);

    /* Find the enumerated "custom" profile and the two grid rows. */
    const char *custom_path = NULL;
    for (int i = 0; i < svc->profiles.count; i++)
        if (strcmp(svc->profiles.entries[i].filename, "custom") == 0)
            custom_path = svc->profiles.entries[i].path;
    assert_non_null(custom_path);

    int row0 = -1, row1 = -1;
    for (int i = 0; i < svc->grid.row_count; i++) {
        if (strcmp(svc->grid.rows[i].composite_path, COMP_PATH_0) == 0)
            row0 = i;
        if (strcmp(svc->grid.rows[i].composite_path, COMP_PATH_1) == 0)
            row1 = i;
    }
    assert_true(row0 >= 0);
    assert_true(row1 >= 0);
    char before0[CBX_GRID_PROFILE_LEN];
    snprintf(before0, sizeof(before0), "%s", svc->grid.rows[row0].profile);

    assert_int_equal(svc->conn.backend->call_method(svc->conn.bus, IP_DBUS_NAME,
        COMP_PATH_1, IP_IFACE_DBUS_DEVICE, "EmitStringProp",
        "ss", "ProfilePath", custom_path, NULL), 0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);

    assert_string_equal(svc->grid.rows[row1].profile, "custom");
    assert_string_equal(svc->grid.rows[row0].profile, before0);
}

/* An invalidated property triggers exactly one bounded authoritative read
 * and applies the device's real current value. */
static void test_properties_changed_invalidation_reads(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    assert_int_equal(cbx_overlay_props_wire(svc), 0);

    const char *custom_path = NULL;
    for (int i = 0; i < svc->profiles.count; i++)
        if (strcmp(svc->profiles.entries[i].filename, "custom") == 0)
            custom_path = svc->profiles.entries[i].path;
    assert_non_null(custom_path);

    /* Load a known profile on composite 1 so the server reports a real
     * authoritative ProfileName. */
    assert_int_equal(svc->conn.backend->call_method(svc->conn.bus, IP_DBUS_NAME,
        COMP_PATH_1, IP_IFACE_COMPOSITE, "LoadProfilePath",
        "s", custom_path, NULL), 0);

    assert_int_equal(svc->conn.backend->call_method(svc->conn.bus, IP_DBUS_NAME,
        COMP_PATH_1, IP_IFACE_DBUS_DEVICE, "EmitInvalidatedProp",
        "s", "ProfileName", NULL), 0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);

    const cbx_composite_entry *e1 =
        cbx_device_model_find_composite(&svc->model, COMP_PATH_1);
    const cbx_composite_entry *e0 =
        cbx_device_model_find_composite(&svc->model, COMP_PATH_0);
    assert_true(e1->has_profile_name);
    assert_string_equal(e1->profile_name, "custom");
    assert_false(e0->has_profile_name);  /* authoritative read stayed scoped */
}

/* Subscription replacement must not retain the previous callback. */
static int s_stale_prop_calls = 0;
static int s_fresh_prop_calls = 0;

static void stale_prop_cb(const char *object_path, const char *iface_name,
                          const char *prop_name, ip_prop_type type,
                          const char *value, int count, void *userdata)
{
    (void)object_path; (void)iface_name; (void)prop_name; (void)type;
    (void)value; (void)count; (void)userdata;
    s_stale_prop_calls++;
}

static void fresh_prop_cb(const char *object_path, const char *iface_name,
                          const char *prop_name, ip_prop_type type,
                          const char *value, int count, void *userdata)
{
    (void)object_path; (void)iface_name; (void)prop_name; (void)type;
    (void)value; (void)count; (void)userdata;
    s_fresh_prop_calls++;
}

static void test_props_subscription_replacement_no_stale(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    s_stale_prop_calls = 0;
    s_fresh_prop_calls = 0;

    ip_properties stale, fresh;
    ip_properties_init(&stale, svc->conn.backend, svc->conn.bus,
                       svc->expected_sender, stale_prop_cb, NULL);
    assert_int_equal(ip_properties_subscribe(&stale), 0);

    /* Re-wire (as recovery does) with a different callback. */
    ip_properties_init(&fresh, svc->conn.backend, svc->conn.bus,
                       svc->expected_sender, fresh_prop_cb, NULL);
    assert_int_equal(ip_properties_subscribe(&fresh), 0);

    assert_int_equal(svc->conn.backend->call_method(svc->conn.bus, IP_DBUS_NAME,
        COMP_PATH_0, IP_IFACE_DBUS_DEVICE, "EmitStringProp",
        "ss", "ProfileName", "Once", NULL), 0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);

    assert_int_equal(s_stale_prop_calls, 0);  /* stale callback not retained */
    assert_int_equal(s_fresh_prop_calls, 1);
}

/* --- O13: Conflict detection + auto-resolution on save --- */

static void test_o13_conflict_resolution_on_save(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    activate_overlay(f);

    /* Both controllers move to col 1 (P1) independently via DBus InputEvent. */
    emit_input_event(svc->conn.backend, svc->conn.bus, COMP_PATH_0, "Right", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 0), 1);

    emit_input_event(svc->conn.backend, svc->conn.bus, COMP_PATH_1, "Right", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 1), 1);

    /* Detect conflict before save. */
    cbx_conflict_list conflicts;
    cbx_conflict_list_init(&conflicts);
    cbx_conflict_detect(&svc->grid, &conflicts);
    assert_true(conflicts.count > 0);

    /* Close — auto-resolves conflict (DBus InputEvent). */
    emit_input_event(svc->conn.backend, svc->conn.bus, COMP_PATH_0, "B", 1.0);
    drain_bus(svc->conn.backend, svc->conn.bus, 100);
    cbx_overlay_service_step(svc);
    assert_int_equal(svc->lifecycle.state, CBX_OVERLAY_IDLE);

    /* Row 0 keeps P1, row 1 auto-moved to P2. */
    assert_int_equal(svc->grid.rows[0].cur_col, 1);
    assert_int_equal(svc->grid.rows[1].cur_col, 2);

    /* All assertions passed — record in the runtime verification ledger. */
    assert_int_equal(cbx_interaction_inventory_mark_verified("O13"), 0);
}

/* ================================================================== */
/*  Task 7 — readiness fails closed and recovers within two seconds     */
/* ================================================================== */

/* Restart the InputPlumber-compatible server on the fixture's private bus
 * with the same fixture configuration.  Returns the new server pid. */
static pid_t
restart_native_server(native_fixture *f)
{
    nip_reset_server_state(2);
    for (int i = 0; i < 2; i++) {
        snprintf(g_nip_comp_names[i], sizeof(g_nip_comp_names[i]),
                 "TestController%d", i);
        snprintf(g_nip_persistent_ids[i], sizeof(g_nip_persistent_ids[i]),
                 "ORDER:%d", i);
        snprintf(g_nip_dbus_devices[i], sizeof(g_nip_dbus_devices[i]),
                 "/org/shadowblip/InputPlumber/CompositeDevice%d", i);
        g_nip_intercept_mode[i] = IP_INTERCEPT_PASS;
    }
    const nip_server_config cfg = { .num_composites = 2,
                                    .version = "0.78.0" };
    return nip_fork_server(f->bus_address, &cfg);
}

/* Owner loss must clear readiness (no trusted sender), and a healthy
 * reacquisition must become operational within two seconds.  A transient
 * first recovery failure must be retried (bounded) rather than leaving the
 * overlay permanently disabled. */
static void
test_readiness_fail_closed_and_recovery(void **state)
{
    native_fixture *f = *state;
    cbx_overlay_service_ctx *svc = f->svc;

    /* Use the exact production recovery callbacks. */
    cbx_overlay_install_recovery_callbacks(svc);
    assert_true(svc->backend_ready);
    assert_true(ip_connection_is_sender_verified(&svc->conn));

    /* Phase 1: owner loss → fail closed. */
    kill(f->server_pid, SIGTERM);
    waitpid(f->server_pid, NULL, 0);
    f->server_pid = -1;

    uint32_t t0 = SDL_GetTicks();
    while (svc->backend_ready && SDL_GetTicks() - t0 < 2000) {
        svc->conn.backend->process(svc->conn.bus);
        SDL_Delay(10);
    }
    assert_false(svc->backend_ready);
    assert_false(ip_connection_is_sender_verified(&svc->conn));
    assert_null(ip_connection_get_unique_name(&svc->conn));
    assert_true(svc->readiness_detail[0] != '\0');

    /* Phase 2: restart, but inject a transient reconcile failure so the
     * first recovery attempt fails.  The bounded retry in
     * cbx_overlay_service_step must then bring the overlay ready. */
    g_nip_fail_next_create = 1;
    f->server_pid = restart_native_server(f);
    assert_true(f->server_pid > 0);

    t0 = SDL_GetTicks();
    while (!svc->backend_ready && SDL_GetTicks() - t0 < 2000) {
        cbx_overlay_service_step(svc);
        SDL_Delay(10);
    }
    assert_true(svc->backend_ready);
    assert_true(SDL_GetTicks() - t0 <= 2000);
    assert_true(ip_connection_is_sender_verified(&svc->conn));
    assert_true(svc->input_events_ready);
    assert_true(svc->poll_count == svc->comp_count);
}

/* ================================================================== */
/*  Main                                                               */
/* ================================================================== */

static const struct CMUnitTest tests[] = {
    /* O01 — Open */
    cmocka_unit_test_setup_teardown(test_o01_open_activates,
                                     native_setup, native_teardown),
    cmocka_unit_test_setup_teardown(test_o01b_deactivation_closes,
                                     native_setup, native_teardown),

    /* O02–O03 — Move left/right */
    cmocka_unit_test_setup_teardown(test_o02_move_left,
                                     native_setup, native_teardown),
    cmocka_unit_test_setup_teardown(test_o03_move_right,
                                     native_setup, native_teardown),

    /* O04–O05 — Cycle profile */
    cmocka_unit_test_setup_teardown(test_o04_cycle_profile_up,
                                     native_setup, native_teardown),
    cmocka_unit_test_setup_teardown(test_o05_cycle_profile_down,
                                     native_setup, native_teardown),

    /* O06–O09 — Host Mode */
    cmocka_unit_test_setup_teardown(test_o06_enter_host_mode,
                                     native_setup, native_teardown),
    cmocka_unit_test_setup_teardown(test_o06b_host_freezes_non_host,
                                     native_setup, native_teardown),
    cmocka_unit_test_setup_teardown(test_o07_host_navigate_rows,
                                     native_setup, native_teardown),
    cmocka_unit_test_setup_teardown(test_o08_host_move_slot,
                                     native_setup, native_teardown),
    cmocka_unit_test_setup_teardown(test_o09_exit_host_mode,
                                     native_setup, native_teardown),

    /* O02–O09 via DBus InputEvent (primary production transport) */
    cmocka_unit_test_setup_teardown(test_o02_move_left_dbus,
                                     native_setup, native_teardown),
    cmocka_unit_test_setup_teardown(test_o03_move_right_dbus,
                                     native_setup, native_teardown),
    cmocka_unit_test_setup_teardown(test_o04_cycle_profile_up_dbus,
                                     native_setup, native_teardown),
    cmocka_unit_test_setup_teardown(test_o05_cycle_profile_down_dbus,
                                     native_setup, native_teardown),
    cmocka_unit_test_setup_teardown(test_o06_enter_host_mode_dbus,
                                     native_setup, native_teardown),
    cmocka_unit_test_setup_teardown(test_o07_host_navigate_rows_dbus,
                                     native_setup, native_teardown),
    cmocka_unit_test_setup_teardown(test_o08_host_move_slot_dbus,
                                     native_setup, native_teardown),
    cmocka_unit_test_setup_teardown(test_o09_exit_host_mode_dbus,
                                     native_setup, native_teardown),

    /* O10 — Close */
    cmocka_unit_test_setup_teardown(test_o10_close_saves_and_sets_pass,
                                     native_setup, native_teardown),
    cmocka_unit_test_setup_teardown(test_o10b_close_conflict_resolution,
                                     native_setup, native_teardown),
    cmocka_unit_test_setup_teardown(test_host_mode_exits_on_close_native,
                                     native_setup, native_teardown),

    /* O11 — Multi-controller independence */
    cmocka_unit_test_setup_teardown(test_o11_multi_controller_independent,
                                     native_setup, native_teardown),
    cmocka_unit_test_setup_teardown(test_o11b_host_mode_via_dbus,
                                     native_setup, native_teardown),
    cmocka_unit_test_setup_teardown(test_o11c_unknown_device_dropped,
                                     native_setup, native_teardown),

    /* O12 — Host profile cycle (L1/R1) via production DBus InputEvent */
    cmocka_unit_test_setup_teardown(test_o12_host_profile_cycle_dbus,
                                     native_setup, native_teardown),
    cmocka_unit_test_setup_teardown(test_o12_host_profile_frozen_dbus,
                                     native_setup, native_teardown),

    /* Exact replacement assignment and authoritative Unassigned clear */
    cmocka_unit_test_setup_teardown(
        test_assignment_replaces_p1_through_p4_then_clears,
        native_setup, native_teardown),

    /* Reactive PropertiesChanged via the real sd-bus path (Task 5) */
    cmocka_unit_test_setup_teardown(test_properties_changed_reactive_string,
                                     native_setup, native_teardown),
    cmocka_unit_test_setup_teardown(test_properties_changed_reactive_array,
                                     native_setup, native_teardown),
    cmocka_unit_test_setup_teardown(test_properties_changed_two_devices_isolated,
                                     native_setup, native_teardown),
    cmocka_unit_test_setup_teardown(test_properties_changed_grid_profile,
                                     native_setup, native_teardown),
    cmocka_unit_test_setup_teardown(test_properties_changed_invalidation_reads,
                                     native_setup, native_teardown),
    cmocka_unit_test_setup_teardown(test_props_subscription_replacement_no_stale,
                                     native_setup, native_teardown),

    /* O13 — Conflict resolution on save */
    cmocka_unit_test_setup_teardown(test_o13_conflict_resolution_on_save,
                                     native_setup, native_teardown),

    /* Task 7 — readiness fails closed on owner loss, recovers within 2s,
     * and retries a transient recovery failure. */
    cmocka_unit_test_setup_teardown(test_readiness_fail_closed_and_recovery,
                                     native_setup, native_teardown),
};

int main(void)
{
    int rc = cmocka_run_group_tests(tests, NULL, NULL);
    if (rc == 0) {
        /* All dispatch tests passed.  The overlay close/conflict actions
         * they exercised must now be recorded as verified in the runtime
         * ledger (via mark_verified() inside each passing test), proving
         * the inventory's verified flags reflect actual pass status. */
        assert_int_equal(cbx_interaction_inventory_is_verified("O10"), 1);
        assert_int_equal(cbx_interaction_inventory_is_verified("O13"), 1);
    }
    return rc;
}