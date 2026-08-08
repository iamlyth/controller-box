/*
 * overlay_service.c — overlay service startup and poll loop (Tasks 3–4).
 *
 * Implements run_overlay_service(): the production init path for the
 * overlay service mode.  The function initialises SDL, connects to
 * InputPlumber via the system DBus, enumerates composite devices,
 * pre-builds the overlay surface, registers intercept triggers, and
 * enters the poll loop with full InterceptMode polling, signal handling,
 * and input processing.
 *
 * SPEC §2.3–§2.5, §4.3–§4.5, §10.2–§10.3.
 */
#include "config.h"
#include "app/overlay_service.h"

#include "config/config_paths.h"
#include "config/config_settings.h"
#include "config/config_assignments.h"
#include "ui/renderer.h"
#include "ui/theme.h"
#include "ui/text.h"
#include "icons/icon_cache.h"
#include "icons/icon_map.h"
#include "dbus/ip_connection.h"
#include "dbus_mock.h"          /* ip_dbus_backend, ip_bus_handle, ip_dbus_sd_backend */
#include "dbus/ip_objectmanager.h"
#include "dbus/ip_device_model.h"
#include "dbus/ip_composite.h"
#include "dbus/ip_input_signal.h"
#include "dbus/ip_intercept_poll.h"
#include "overlay/surface_build.h"
#include "overlay/lifecycle.h"
#include "overlay/grid_render.h"
#include "overlay/trigger.h"
#include "overlay/player_mode.h"
#include "overlay/host_mode.h"
#include "overlay/conflict.h"
#include "overlay/profile_cycle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

/* ================================================================== */
/*  Signal handling (SIGTERM / SIGINT)                                 */
/* ================================================================== */

static volatile sig_atomic_t g_running = 1;

static void
signal_handler(int signo)
{
    (void)signo;
    g_running = 0;
}

int
cbx_overlay_service_install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;            /* no SA_RESTART — interrupt SDL_PollEvent */

    if (sigaction(SIGTERM, &sa, NULL) != 0)
        return -1;
    if (sigaction(SIGINT, &sa, NULL) != 0)
        return -1;
    return 0;
}

bool
cbx_overlay_service_shutdown_requested(void)
{
    return g_running == 0;
}

void
cbx_overlay_service_reset_shutdown(void)
{
    g_running = 1;
}

/* ================================================================== */
/*  Poll-loop context (shared with callbacks)                         */
/* ================================================================== */

typedef struct {
    cbx_select_grid       *grid;
    cbx_assignments       *assignments;
    cbx_overlay_surface   *surface;
    SDL_Renderer          *renderer;
    cbx_grid_render_ctx   *render_ctx;
    const ip_dbus_backend *backend;
    ip_bus_handle          bus;
    cbx_player_mode        pm;
    cbx_host_mode          hm;
    cbx_conflict_list      conflicts;
    int                    comp_count;
    /* Overlay input event handling (Task 6). */
    cbx_overlay_input_ctx  input_ctx;
    ip_input_events        input_events;
    char                   expected_sender[128];
    bool                   input_events_ready;
} overlay_ctx;

/* ================================================================== */
/*  InterceptMode poll callbacks                                       */
/* ================================================================== */

static void
on_intercept_activating(void *userdata)
{
    cbx_overlay_lifecycle *lc = (cbx_overlay_lifecycle *)userdata;
    cbx_overlay_lifecycle_activate(lc);
}

static void
on_intercept_deactivating(void *userdata)
{
    cbx_overlay_lifecycle *lc = (cbx_overlay_lifecycle *)userdata;
    /* If already closing/closed, close() returns -EPERM — that's fine. */
    cbx_overlay_lifecycle_close(lc);
}

static void
on_intercept_error(int error_code, void *userdata)
{
    (void)userdata;
    fprintf(stderr,
            "controller-box: intercept poll error: %d\n", error_code);
}

/* ================================================================== */
/*  Lifecycle on_save callback: conflict resolution + assignment save */
/* ================================================================== */

static int
on_overlay_save(void *userdata)
{
    overlay_ctx *ctx = (overlay_ctx *)userdata;

    /* Detect and resolve conflicts (SPEC §4.5). */
    cbx_conflict_list_init(&ctx->conflicts);
    cbx_conflict_detect(ctx->grid, &ctx->conflicts);
    cbx_conflict_resolve(ctx->grid, &ctx->conflicts);

    /* Sync grid state back to assignments and save. */
    for (int i = 0; i < ctx->grid->row_count; i++) {
        const cbx_grid_row *row = &ctx->grid->rows[i];
        int slot = cbx_select_grid_col_to_slot(row->cur_col);

        /* Find existing assignment by id. */
        int found = -1;
        for (int j = 0; j < ctx->assignments->assignment_count; j++) {
            if (strcmp(ctx->assignments->assignments[j].id,
                       row->id) == 0) {
                found = j;
                break;
            }
        }

        if (slot >= 0) {
            /* Assigned: update or add. */
            if (found >= 0) {
                ctx->assignments->assignments[found].slot = slot;
                snprintf(ctx->assignments->assignments[found].profile,
                         sizeof(ctx->assignments->assignments[found].profile),
                         "%s", row->profile);
            } else if (ctx->assignments->assignment_count <
                       CBX_MAX_ASSIGNMENTS) {
                cbx_assignment *a =
                    &ctx->assignments->assignments[
                        ctx->assignments->assignment_count++];
                snprintf(a->id, sizeof(a->id), "%s", row->id);
                a->slot = slot;
                snprintf(a->profile, sizeof(a->profile),
                         "%s", row->profile);
            }
        } else {
            /* Unassigned: remove entry if present. */
            if (found >= 0) {
                ctx->assignments->assignments[found] =
                    ctx->assignments->assignments[
                        --ctx->assignments->assignment_count];
            }
        }
    }

    cbx_assignments_save(ctx->assignments);  /* best-effort */

    /* Mark surface dirty so it's re-rendered before next activation. */
    cbx_overlay_surface_mark_dirty_all(ctx->surface);
    return 0;
}

/* ================================================================== */
/*  Player Mode callbacks (slot/profile change side effects)          */
/* ================================================================== */

static int
on_slot_change(int row_idx, int new_slot, void *userdata)
{
    overlay_ctx *ctx = (overlay_ctx *)userdata;
    (void)row_idx;
    (void)new_slot;
    /* Grid is already updated by player_mode_handle.
     * Mark surface dirty for re-render. */
    cbx_overlay_surface_mark_dirty_all(ctx->surface);
    return 0;
}

static int
on_profile_change(int row_idx, const char *profile,
                   const char *composite_path, void *userdata)
{
    overlay_ctx *ctx = (overlay_ctx *)userdata;
    (void)row_idx;

    /* Load the profile on InputPlumber via DBus (best-effort). */
    if (ctx->backend && composite_path && profile && profile[0]) {
        char profiles_dir[512];
        char profile_path[576];

        if (cbx_resolve_user_profiles_dir(profiles_dir,
                                           sizeof(profiles_dir)) == 0) {
            snprintf(profile_path, sizeof(profile_path),
                     "%s/%s.yaml", profiles_dir, profile);
            ip_composite_load_profile_path(ctx->backend, ctx->bus,
                                            composite_path, profile_path);
        }
    }

    /* Mark surface dirty for re-render. */
    cbx_overlay_surface_mark_dirty_all(ctx->surface);
    return 0;
}

/* ================================================================== */
/*  Host Mode slot change callback                                     */
/* ================================================================== */

static int
on_host_slot_change(int row_idx, int new_slot, void *userdata)
{
    return on_slot_change(row_idx, new_slot, userdata);
}

/* ================================================================== */
/*  Helper: fill cbx_grid_composite_info from the device model + DBus  */
/* ================================================================== */
static void fill_composite_info(cbx_grid_composite_info *info,
                                 const cbx_composite_entry *entry,
                                 const ip_dbus_backend *backend,
                                 ip_bus_handle bus)
{
    /* Path */
    snprintf(info->composite_path, sizeof(info->composite_path),
             "%s", entry->path);

    /* Persistent ID (best-effort) */
    char *id = NULL;
    if (ip_composite_get_persistent_id(backend, bus, entry->path, &id) == 0
        && id) {
        snprintf(info->id, sizeof(info->id), "%s", id);
        free(id);
    } else {
        snprintf(info->id, sizeof(info->id), "composite-%d", entry->index);
    }

    /* Model name (best-effort) */
    char *name = NULL;
    if (ip_composite_get_name(backend, bus, entry->path, &name) == 0
        && name) {
        snprintf(info->model_name, sizeof(info->model_name), "%s", name);
        free(name);
    } else {
        snprintf(info->model_name, sizeof(info->model_name),
                 "Controller %d", entry->index);
    }
}

/* ================================================================== */
/*  Helper: set InterceptMode = PASS on all composites                 */
/* ================================================================== */
static void set_all_pass(const ip_dbus_backend *backend, ip_bus_handle bus,
                          const cbx_composite_entry *composites, int count)
{
    for (int i = 0; i < count; i++)
        ip_composite_set_intercept_mode(backend, bus,
                                         composites[i].path, "1");
}

/* ================================================================== */
/*  Helper: map SDL key to player_mode / host_mode input               */
/* ================================================================== */
static int
sdl_key_to_pm_input(SDL_Keycode key, cbx_pm_input *out)
{
    switch (key) {
    case SDLK_LEFT:   *out = CBX_PM_LEFT;  return 1;
    case SDLK_RIGHT:  *out = CBX_PM_RIGHT; return 1;
    case SDLK_UP:     *out = CBX_PM_UP;    return 1;
    case SDLK_DOWN:   *out = CBX_PM_DOWN;  return 1;
    case SDLK_b:      *out = CBX_PM_B;     return 1;
    case SDLK_r:      *out = CBX_PM_R3;    return 1;
    default:          return 0;
    }
}

static int
sdl_key_to_hm_input(SDL_Keycode key, cbx_hm_input *out)
{
    switch (key) {
    case SDLK_LEFT:   *out = CBX_HM_LEFT;  return 1;
    case SDLK_RIGHT:  *out = CBX_HM_RIGHT; return 1;
    case SDLK_UP:     *out = CBX_HM_UP;    return 1;
    case SDLK_DOWN:   *out = CBX_HM_DOWN;  return 1;
    case SDLK_b:      *out = CBX_HM_B;     return 1;
    case SDLK_r:      *out = CBX_HM_R3;    return 1;
    default:          return 0;
    }
}

/* ================================================================== */
/*  Overlay input event handling (Task 6)                              */
/*  Maps DBus InputEvent signals to grid rows and dispatches to        */
/*  player_mode or host_mode with the correct row index.               */
/* ================================================================== */

void
cbx_overlay_input_add_mapping(cbx_overlay_input_ctx *ctx,
                                const char *device_path,
                                int row_idx)
{
    if (!ctx || !device_path || ctx->path_count >= CBX_MAX_DBUS_DEVICES)
        return;
    snprintf(ctx->device_paths[ctx->path_count],
             sizeof(ctx->device_paths[ctx->path_count]),
             "%s", device_path);
    ctx->row_indices[ctx->path_count] = row_idx;
    ctx->path_count++;
}

int
cbx_overlay_input_find_row(const char *device_path,
                              char paths[][256],
                              const int *rows, int count)
{
    if (!device_path || !paths || !rows)
        return -1;
    for (int i = 0; i < count; i++) {
        if (strcmp(paths[i], device_path) == 0)
            return rows[i];
    }
    return -1;
}

int
cbx_overlay_input_build_map(const ip_dbus_backend *backend,
                              ip_bus_handle bus,
                              cbx_grid_composite_info *composites,
                              int comp_count,
                              cbx_overlay_input_ctx *ctx)
{
    if (!backend || !composites || !ctx)
        return -EINVAL;

    ctx->path_count = 0;

    for (int i = 0; i < comp_count; i++) {
        char *dbus_devices = NULL;
        int rc = ip_composite_get_dbus_devices(backend, bus,
                                                composites[i].composite_path,
                                                &dbus_devices);
        if (rc != 0 || !dbus_devices)
            continue;  /* best-effort */

        /* Parse comma-separated DBusDevice paths. */
        char *saveptr = NULL;
        char *token = strtok_r(dbus_devices, ",", &saveptr);
        while (token) {
            /* Trim leading whitespace. */
            while (*token == ' ')
                token++;
            if (*token)
                cbx_overlay_input_add_mapping(ctx, token, i);
            token = strtok_r(NULL, ",", &saveptr);
        }

        free(dbus_devices);
    }

    return 0;
}

int
cbx_ip_input_to_pm(ip_input_id input, cbx_pm_input *out)
{
    switch (input) {
    case IP_INPUT_LEFT:  *out = CBX_PM_LEFT;  return 1;
    case IP_INPUT_RIGHT: *out = CBX_PM_RIGHT; return 1;
    case IP_INPUT_UP:    *out = CBX_PM_UP;    return 1;
    case IP_INPUT_DOWN:  *out = CBX_PM_DOWN;  return 1;
    case IP_INPUT_B:     *out = CBX_PM_B;     return 1;
    case IP_INPUT_R3:    *out = CBX_PM_R3;    return 1;
    default:             return 0;
    }
}

int
cbx_ip_input_to_hm(ip_input_id input, cbx_hm_input *out)
{
    switch (input) {
    case IP_INPUT_LEFT:  *out = CBX_HM_LEFT;  return 1;
    case IP_INPUT_RIGHT: *out = CBX_HM_RIGHT; return 1;
    case IP_INPUT_UP:    *out = CBX_HM_UP;    return 1;
    case IP_INPUT_DOWN:  *out = CBX_HM_DOWN;  return 1;
    case IP_INPUT_B:     *out = CBX_HM_B;     return 1;
    case IP_INPUT_R3:    *out = CBX_HM_R3;    return 1;
    default:             return 0;
    }
}

void
cbx_overlay_input_cb(ip_input_id input,
                       ip_input_category category,
                       double value,
                       const char *raw_event,
                       const char *device_path,
                       void *userdata)
{
    (void)raw_event;
    cbx_overlay_input_ctx *ctx = (cbx_overlay_input_ctx *)userdata;
    if (!ctx || !ctx->pm || !ctx->hm || !ctx->grid || !ctx->lifecycle)
        return;

    /* Only process button press events (value == 1.0).
     * Button releases (0.0) and axis events are ignored — the overlay
     * only needs directional navigation. */
    if (category != IP_INPUT_CAT_BUTTON || value != 1.0)
        return;

    /* Map device_path→row.  Unknown device paths are dropped. */
    int row_idx = cbx_overlay_input_find_row(device_path,
                                               ctx->device_paths,
                                               ctx->row_indices,
                                               ctx->path_count);
    if (row_idx < 0 || row_idx >= ctx->grid->row_count)
        return;  /* unknown device path or out of bounds */

    if (cbx_host_mode_is_active(ctx->hm)) {
        /* Host Mode: host navigates rows + slots. */
        cbx_hm_input hm_in;
        if (!cbx_ip_input_to_hm(input, &hm_in))
            return;

        int host_row = cbx_host_mode_get_host_row(ctx->hm);
        int result = cbx_host_mode_handle(ctx->hm, host_row, hm_in,
                                            ctx->grid);
        if (result == CBX_HM_RESULT_EXIT) {
            cbx_host_mode_exit(ctx->hm);
        } else if (result == CBX_HM_RESULT_CLOSE) {
            cbx_overlay_lifecycle_close(ctx->lifecycle);
        } else if (result == CBX_HM_RESULT_MOVED ||
                   result == CBX_HM_RESULT_SLOT) {
            cbx_overlay_surface_mark_dirty_all(ctx->lifecycle->surface);
        }
    } else {
        /* Player Mode: each controller edits its own row. */
        cbx_pm_input pm_in;
        if (!cbx_ip_input_to_pm(input, &pm_in))
            return;

        int result = cbx_player_mode_handle(ctx->pm, row_idx, pm_in);
        if (result == CBX_PM_RESULT_CLOSE) {
            cbx_overlay_lifecycle_close(ctx->lifecycle);
        } else if (result == CBX_PM_RESULT_HOST) {
            cbx_host_mode_toggle(ctx->hm, row_idx);
        }
        /* Surface dirty flag is set by callbacks. */
    }
}

/* ================================================================== */
/*  Main entry                                                         */
/* ================================================================== */
int run_overlay_service(int dry_run)
{
    printf("controller-box: overlay-service mode%s\n",
           dry_run ? " (dry-run)" : "");
    if (dry_run)
        return 0;

    /* --- 0. Install signal handlers ------------------------------- */
    g_running = 1;
    if (cbx_overlay_service_install_signal_handlers() != 0) {
        fprintf(stderr,
                "controller-box: warning: signal handler setup failed\n");
        /* Continue anyway — the service still works, just no clean
         * shutdown on SIGTERM. */
    }

    /* --- 1. SDL video + hidden renderer ----------------------------- */
    cbx_renderer rend;
    int rc = cbx_renderer_init(&rend, "Controller-Box Overlay",
                               CBX_RENDERER_DEFAULT_W,
                               CBX_RENDERER_DEFAULT_H, false);
    if (rc != 0) {
        fprintf(stderr,
                "controller-box: failed to initialise SDL renderer: %s\n",
                SDL_GetError());
        return 1;
    }

    /* --- 2. Connect to InputPlumber via system DBus ------------------ */
    ip_connection conn;
    ip_connection_init(&conn, ip_dbus_sd_backend());
    rc = ip_connection_connect(&conn);
    if (!ip_connection_is_connected(&conn)) {
        fprintf(stderr,
                "controller-box: InputPlumber not found on system DBus\n");
        ip_connection_disconnect(&conn);
        cbx_renderer_shutdown(&rend);
        return 1;
    }

    /* --- 3. Enumerate composite devices ------------------------------ */
    cbx_device_model model;
    cbx_device_model_init(&model);
    rc = cbx_objectmanager_enumerate(conn.backend, conn.bus, &model);
    if (rc != 0) {
        fprintf(stderr,
                "controller-box: failed to enumerate devices: %d\n", rc);
        ip_connection_disconnect(&conn);
        cbx_renderer_shutdown(&rend);
        return 1;
    }

    /* --- 4. Load settings + assignments (best-effort) ---------------- */
    cbx_settings settings;
    cbx_settings_defaults(&settings);
    cbx_settings_load(&settings);   /* best-effort — defaults are OK */

    cbx_assignments assignments;
    cbx_assignments_init(&assignments);
    cbx_assignments_load(&assignments);  /* best-effort */

    /* --- 5. Text cache, theme, icon cache ---------------------------- */
    cbx_text_cache text_cache;
    cbx_text_cache_init(&text_cache, rend.renderer);
    int font_id = -1;
    const char *font_path = cbx_font_path();
    if (font_path)
        font_id = cbx_text_load_font(&text_cache, font_path, 16);

    cbx_theme theme;
    cbx_theme_default(&theme);

    cbx_icon_map icon_map;
    cbx_icon_map_init(&icon_map);
    char icon_map_path[512];
    if (cbx_icon_map_default_path(icon_map_path, sizeof(icon_map_path)) == 0)
        cbx_icon_map_load(&icon_map, icon_map_path);  /* best-effort */

    cbx_icon_cache icon_cache;
    cbx_icon_cache_init(&icon_cache, rend.renderer,
                         cbx_icon_dir(), 48);
    cbx_icon_cache_load(&icon_cache, &icon_map);  /* best-effort */

    /* --- 6. Build the selection grid --------------------------------- */
    cbx_grid_composite_info composites[CBX_MAX_COMPOSITES];
    int comp_count = model.composite_count;
    if (comp_count > CBX_MAX_COMPOSITES)
        comp_count = CBX_MAX_COMPOSITES;
    for (int i = 0; i < comp_count; i++)
        fill_composite_info(&composites[i], &model.composites[i],
                             conn.backend, conn.bus);

    cbx_select_grid grid;
    cbx_select_grid_init(&grid);
    cbx_select_grid_build(&grid, composites, comp_count,
                           &settings, &assignments);

    /* --- 7. Initialise and pre-render the overlay surface ------------ */
    cbx_overlay_surface surface;
    rc = cbx_overlay_surface_init(&surface, rend.renderer,
                                   CBX_RENDERER_DEFAULT_W,
                                   CBX_RENDERER_DEFAULT_H,
                                   settings.overlay_opacity);
    if (rc != 0) {
        fprintf(stderr,
                "controller-box: failed to create overlay surface: %d\n",
                rc);
        cbx_icon_cache_cleanup(&icon_cache);
        cbx_text_cache_cleanup(&text_cache);
        ip_connection_disconnect(&conn);
        cbx_renderer_shutdown(&rend);
        return 1;
    }

    cbx_grid_render_ctx render_ctx = {
        .grid       = &grid,
        .icon_cache = &icon_cache,
        .icon_map   = &icon_map,
        .theme      = &theme,
        .text_cache = font_id >= 0 ? &text_cache : NULL,
        .font_id    = font_id,
    };
    cbx_overlay_surface_mark_dirty_all(&surface);
    cbx_overlay_surface_render(&surface, rend.renderer,
                                cbx_select_grid_render_cb, &render_ctx);

    /* --- 8. Register overlay triggers + set PASS --------------------- */
    if (comp_count > 0) {
        const char *paths[CBX_MAX_COMPOSITES];
        for (int i = 0; i < comp_count; i++)
            paths[i] = composites[i].composite_path;
        cbx_trigger_register_all(conn.backend, conn.bus,
                                  paths, comp_count,
                                  settings.overlay_trigger);
    }
    set_all_pass(conn.backend, conn.bus, model.composites, comp_count);

    /* --- 9. Initialise overlay lifecycle ----------------------------- */
    cbx_overlay_lifecycle lifecycle;
    const char *primary_path =
        comp_count > 0 ? composites[0].composite_path : "";
    cbx_overlay_lifecycle_init(&lifecycle, conn.backend, conn.bus,
                               primary_path, &surface, rend.renderer);

    /* --- 10. Set up poll-loop context -------------------------------- */
    overlay_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.grid         = &grid;
    ctx.assignments  = &assignments;
    ctx.surface      = &surface;
    ctx.renderer     = rend.renderer;
    ctx.render_ctx   = &render_ctx;
    ctx.backend      = conn.backend;
    ctx.bus          = conn.bus;
    ctx.comp_count   = comp_count;

    /* Player Mode (SPEC §4.3). */
    cbx_player_mode_init(&ctx.pm, &grid);
    ctx.pm.on_slot_change     = on_slot_change;
    ctx.pm.slot_change_data   = &ctx;
    ctx.pm.on_profile_change  = on_profile_change;
    ctx.pm.profile_change_data = &ctx;

    /* Host Mode (SPEC §4.4). */
    cbx_host_mode_init(&ctx.hm);
    ctx.hm.on_slot_change    = on_host_slot_change;
    ctx.hm.slot_change_data  = &ctx;

    /* Lifecycle on_save callback: conflict resolution + assignment save. */
    lifecycle.on_save       = on_overlay_save;
    lifecycle.on_save_data  = &ctx;

    /* --- 10b. Set up DBus InputEvent signal handling (Task 6) ------- */
    /* Build the device_path→row mapping from composite DbusDevices. */
    ctx.input_ctx.pm        = &ctx.pm;
    ctx.input_ctx.hm        = &ctx.hm;
    ctx.input_ctx.grid      = &grid;
    ctx.input_ctx.lifecycle  = &lifecycle;
    ctx.input_ctx.path_count = 0;
    cbx_overlay_input_build_map(conn.backend, conn.bus,
                                  composites, comp_count, &ctx.input_ctx);

    /* Initialise ip_input_events with InputPlumber's unique bus name
     * as expected_sender (fail-closed: if unique name is unavailable,
     * expected_sender is empty and all signals are rejected). */
    const char *uniq = ip_connection_get_unique_name(&conn);
    snprintf(ctx.expected_sender, sizeof(ctx.expected_sender),
             "%s", uniq ? uniq : "");
    ip_input_events_init(&ctx.input_events, conn.backend, conn.bus,
                          ctx.expected_sender,
                          cbx_overlay_input_cb, &ctx.input_ctx);
    ctx.input_events_ready = false;
    if (ip_input_events_subscribe(&ctx.input_events) == 0)
        ctx.input_events_ready = true;

    /* --- 11. Set up InterceptMode polling --------------------------- */
    /* Register a custom SDL event type for the poll timer. */
    uint32_t poll_event_type = SDL_RegisterEvents(1);
    ip_intercept_poll polls[CBX_MAX_COMPOSITES];
    int poll_count = 0;

    if (poll_event_type != (uint32_t)-1) {
        for (int i = 0; i < comp_count && i < CBX_MAX_COMPOSITES; i++) {
            ip_intercept_poll_init(&polls[i],
                                    conn.backend, conn.bus,
                                    composites[i].composite_path,
                                    on_intercept_activating, &lifecycle,
                                    on_intercept_deactivating, &lifecycle,
                                    on_intercept_error, NULL);
            if (ip_intercept_poll_start(&polls[i],
                                         IP_INTERCEPT_POLL_INTERVAL_MS,
                                         poll_event_type) == 0) {
                poll_count++;
            }
        }
    }

    /* --- 12. Poll loop (Task 4) -------------------------------------- */
    /*
     * Main loop:
     *   1. SDL_PollEvent processes:
     *      - Custom poll timer events → ip_intercept_poll_tick
     *      - SDL_KEYDOWN → grid navigation (player/host mode)
     *      - SDL_QUIT → shutdown
     *   2. Process pending DBus InputEvent signals (Task 6).
     *   3. cbx_overlay_lifecycle_tick advances fade animation / timeout.
     *   4. If surface is dirty, re-render + re-show (when visible).
     *   5. Check g_running flag (set by SIGTERM/SIGINT handler).
     */
    SDL_Event ev;
    while (g_running) {
        /* Process all pending SDL events. */
        while (SDL_PollEvent(&ev)) {
            if (ev.type == poll_event_type) {
                /* InterceptMode poll timer fired — tick all polls. */
                for (int i = 0; i < poll_count; i++)
                    ip_intercept_poll_tick(&polls[i]);
            } else if (ev.type == SDL_QUIT) {
                g_running = 0;
            } else if (ev.type == SDL_KEYDOWN &&
                       cbx_overlay_lifecycle_is_active(&lifecycle)) {
                /* Process input only when overlay is visible. */
                SDL_Keycode key = ev.key.keysym.sym;

                if (cbx_host_mode_is_active(&ctx.hm)) {
                    /* Host Mode: host navigates rows + slots. */
                    cbx_hm_input hm_in;
                    if (sdl_key_to_hm_input(key, &hm_in)) {
                        int host_row = cbx_host_mode_get_host_row(&ctx.hm);
                        int result = cbx_host_mode_handle(
                            &ctx.hm, host_row, hm_in, &grid);

                        if (result == CBX_HM_RESULT_EXIT) {
                            cbx_host_mode_exit(&ctx.hm);
                        } else if (result == CBX_HM_RESULT_CLOSE) {
                            cbx_overlay_lifecycle_close(&lifecycle);
                        } else if (result == CBX_HM_RESULT_MOVED ||
                                   result == CBX_HM_RESULT_SLOT) {
                            cbx_overlay_surface_mark_dirty_all(&surface);
                        }
                    }
                } else {
                    /* Player Mode: each controller edits its own row. */
                    cbx_pm_input pm_in;
                    if (sdl_key_to_pm_input(key, &pm_in)) {
                        /* Use row 0 (primary controller) for keyboard
                         * input.  In production, DBus InputEvent signals
                         * carry the device path, which maps to a row. */
                        int result = cbx_player_mode_handle(
                            &ctx.pm, 0, pm_in);

                        if (result == CBX_PM_RESULT_CLOSE) {
                            cbx_overlay_lifecycle_close(&lifecycle);
                        } else if (result == CBX_PM_RESULT_HOST) {
                            cbx_host_mode_toggle(&ctx.hm, 0);
                        }
                        /* Surface dirty flag is set by callbacks. */
                    }
                }
            }
        }

        /* Process pending DBus InputEvent signals (Task 6).
         * In production, this calls sd_bus_process() to dispatch signals
         * registered via ip_input_events_subscribe.  The signal callback
         * validates the sender, parses the event, and fires
         * cbx_overlay_input_cb which maps device_path→row and dispatches
         * to player_mode or host_mode.
         * In tests, this is a no-op (signals injected via inject_signal). */
        if (ctx.input_events_ready)
            ip_input_events_process(&ctx.input_events);

        /* Advance lifecycle (fade animation, timeout). */
        cbx_overlay_lifecycle_tick(&lifecycle);

        /* Re-render dirty surface (when overlay is active). */
        if (cbx_overlay_lifecycle_is_active(&lifecycle) &&
            cbx_overlay_surface_is_dirty(&surface)) {
            cbx_overlay_surface_render(&surface, rend.renderer,
                                        cbx_select_grid_render_cb,
                                        &render_ctx);
            cbx_overlay_surface_show(&surface, rend.renderer);
        }

        /* Small delay to prevent busy-looping (the poll timers drive
         * InterceptMode checks at 50 ms; this just limits the SDL
         * event polling rate). */
        SDL_Delay(10);
    }

    /* --- 13. Clean shutdown ------------------------------------------- */
    /* Stop all InterceptMode poll timers. */
    for (int i = 0; i < poll_count; i++)
        ip_intercept_poll_stop(&polls[i]);

    /* Force-close the overlay if still visible. */
    cbx_overlay_lifecycle_force_close(&lifecycle);

    /* Destroy surface, cleanup caches, disconnect, shutdown renderer. */
    cbx_overlay_surface_destroy(&surface);
    cbx_icon_cache_cleanup(&icon_cache);
    cbx_text_cache_cleanup(&text_cache);
    ip_connection_disconnect(&conn);
    cbx_renderer_shutdown(&rend);
    return 0;
}