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
    cbx_overlay_service_ctx *svc = (cbx_overlay_service_ctx *)userdata;

    /* Detect and resolve conflicts (SPEC §4.5). */
    cbx_conflict_list_init(&svc->conflicts);
    cbx_conflict_detect(&svc->grid, &svc->conflicts);
    cbx_conflict_resolve(&svc->grid, &svc->conflicts);

    /* Sync grid state back to assignments and save. */
    for (int i = 0; i < svc->grid.row_count; i++) {
        const cbx_grid_row *row = &svc->grid.rows[i];
        int slot = cbx_select_grid_col_to_slot(row->cur_col);

        /* Find existing assignment by id. */
        int found = -1;
        for (int j = 0; j < svc->assignments.assignment_count; j++) {
            if (strcmp(svc->assignments.assignments[j].id,
                       row->id) == 0) {
                found = j;
                break;
            }
        }

        if (slot >= 0) {
            /* Assigned: update or add. */
            if (found >= 0) {
                svc->assignments.assignments[found].slot = slot;
                snprintf(svc->assignments.assignments[found].profile,
                         sizeof(svc->assignments.assignments[found].profile),
                         "%s", row->profile);
            } else if (svc->assignments.assignment_count <
                       CBX_MAX_ASSIGNMENTS) {
                cbx_assignment *a =
                    &svc->assignments.assignments[
                        svc->assignments.assignment_count++];
                snprintf(a->id, sizeof(a->id), "%s", row->id);
                a->slot = slot;
                snprintf(a->profile, sizeof(a->profile),
                         "%s", row->profile);
            }
        } else {
            /* Unassigned: remove entry if present. */
            if (found >= 0) {
                svc->assignments.assignments[found] =
                    svc->assignments.assignments[
                        --svc->assignments.assignment_count];
            }
        }
    }

    cbx_assignments_save(&svc->assignments);  /* best-effort */

    /* Mark surface dirty so it's re-rendered before next activation. */
    cbx_overlay_surface_mark_dirty_all(&svc->surface);
    return 0;
}

/* ================================================================== */
/*  Player Mode callbacks (slot/profile change side effects)          */
/* ================================================================== */

static int
on_slot_change(int row_idx, int new_slot, void *userdata)
{
    cbx_overlay_service_ctx *svc = (cbx_overlay_service_ctx *)userdata;
    (void)row_idx;
    (void)new_slot;
    /* Grid is already updated by player_mode_handle.
     * Mark surface dirty for re-render. */
    cbx_overlay_surface_mark_dirty_all(&svc->surface);
    return 0;
}

static int
on_profile_change(int row_idx, const char *profile,
                   const char *composite_path, void *userdata)
{
    cbx_overlay_service_ctx *svc = (cbx_overlay_service_ctx *)userdata;
    (void)row_idx;

    /* Load the profile on InputPlumber via DBus (best-effort). */
    if (svc->conn.backend && composite_path && profile && profile[0]) {
        char profiles_dir[512];
        char profile_path[576];

        if (cbx_resolve_user_profiles_dir(profiles_dir,
                                           sizeof(profiles_dir)) == 0) {
            snprintf(profile_path, sizeof(profile_path),
                     "%s/%s.yaml", profiles_dir, profile);
            ip_composite_load_profile_path(svc->conn.backend, svc->conn.bus,
                                            composite_path, profile_path);
        }
    }

    /* Mark surface dirty for re-render. */
    cbx_overlay_surface_mark_dirty_all(&svc->surface);
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
/*  Step function: process one iteration of the poll loop (Task 10)   */
/* ================================================================== */

void
cbx_overlay_service_step(cbx_overlay_service_ctx *svc)
{
    SDL_Event ev;

    /* 1. Process all pending SDL events. */
    while (SDL_PollEvent(&ev)) {
        if (ev.type == svc->poll_event_type) {
            /* InterceptMode poll timer fired — tick all polls. */
            for (int i = 0; i < svc->poll_count; i++)
                ip_intercept_poll_tick(&svc->polls[i]);
        } else if (ev.type == SDL_QUIT) {
            g_running = 0;
        } else if (ev.type == SDL_KEYDOWN &&
                   cbx_overlay_lifecycle_is_active(&svc->lifecycle)) {
            /* Process input only when overlay is visible. */
            SDL_Keycode key = ev.key.keysym.sym;

            if (cbx_host_mode_is_active(&svc->hm)) {
                /* Host Mode: host navigates rows + slots. */
                cbx_hm_input hm_in;
                if (sdl_key_to_hm_input(key, &hm_in)) {
                    int host_row = cbx_host_mode_get_host_row(&svc->hm);
                    int result = cbx_host_mode_handle(
                        &svc->hm, host_row, hm_in, &svc->grid);

                    if (result == CBX_HM_RESULT_EXIT) {
                        cbx_host_mode_exit(&svc->hm);
                    } else if (result == CBX_HM_RESULT_CLOSE) {
                        cbx_overlay_lifecycle_close(&svc->lifecycle);
                    } else if (result == CBX_HM_RESULT_MOVED ||
                               result == CBX_HM_RESULT_SLOT) {
                        cbx_overlay_surface_mark_dirty_all(&svc->surface);
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
                        &svc->pm, 0, pm_in);

                    if (result == CBX_PM_RESULT_CLOSE) {
                        cbx_overlay_lifecycle_close(&svc->lifecycle);
                    } else if (result == CBX_PM_RESULT_HOST) {
                        cbx_host_mode_toggle(&svc->hm, 0);
                    }
                    /* Surface dirty flag is set by callbacks. */
                }
            }
        }
    }

    /* 2. Process pending DBus InputEvent signals (Task 6). */
    if (svc->input_events_ready)
        ip_input_events_process(&svc->input_events);

    /* 3. Advance lifecycle (fade animation, timeout). */
    cbx_overlay_lifecycle_tick(&svc->lifecycle);

    /* 4. Re-render dirty surface (when overlay is active). */
    if (cbx_overlay_lifecycle_is_active(&svc->lifecycle) &&
        cbx_overlay_surface_is_dirty(&svc->surface)) {
        cbx_overlay_surface_render(&svc->surface, svc->rend.renderer,
                                    cbx_select_grid_render_cb,
                                    &svc->render_ctx);
        cbx_overlay_surface_show(&svc->surface, svc->rend.renderer);
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

    /* Allocate the service context on the heap — it contains large
     * arrays (device model, text cache, icon cache) that together
     * exceed 200 KB. */
    cbx_overlay_service_ctx *svc = calloc(1, sizeof(*svc));
    if (!svc) {
        fprintf(stderr, "controller-box: out of memory\n");
        return 1;
    }

    int rc;

    /* --- 1. SDL video + hidden renderer ----------------------------- */
    rc = cbx_renderer_init(&svc->rend, "Controller-Box Overlay",
                               CBX_RENDERER_DEFAULT_W,
                               CBX_RENDERER_DEFAULT_H, false);
    if (rc != 0) {
        fprintf(stderr,
                "controller-box: failed to initialise SDL renderer: %s\n",
                SDL_GetError());
        free(svc);
        return 1;
    }

    /* --- 2. Connect to InputPlumber via system DBus ------------------ */
    ip_connection_init(&svc->conn, ip_dbus_sd_backend());
    rc = ip_connection_connect(&svc->conn);
    if (!ip_connection_is_connected(&svc->conn)) {
        fprintf(stderr,
                "controller-box: InputPlumber not found on system DBus\n");
        ip_connection_disconnect(&svc->conn);
        cbx_renderer_shutdown(&svc->rend);
        free(svc);
        return 1;
    }

    /* --- 3. Enumerate composite devices ------------------------------ */
    cbx_device_model_init(&svc->model);
    rc = cbx_objectmanager_enumerate(svc->conn.backend, svc->conn.bus,
                                      &svc->model);
    if (rc != 0) {
        fprintf(stderr,
                "controller-box: failed to enumerate devices: %d\n", rc);
        ip_connection_disconnect(&svc->conn);
        cbx_renderer_shutdown(&svc->rend);
        free(svc);
        return 1;
    }

    /* --- 4. Load settings + assignments (best-effort) ---------------- */
    cbx_settings_defaults(&svc->settings);
    cbx_settings_load(&svc->settings);   /* best-effort — defaults are OK */

    cbx_assignments_init(&svc->assignments);
    cbx_assignments_load(&svc->assignments);  /* best-effort */

    /* --- 5. Text cache, theme, icon cache ---------------------------- */
    cbx_text_cache_init(&svc->text_cache, svc->rend.renderer);
    svc->font_id = -1;
    const char *font_path = cbx_font_path();
    if (font_path)
        svc->font_id = cbx_text_load_font(&svc->text_cache, font_path, 16);

    cbx_theme_default(&svc->theme);

    cbx_icon_map_init(&svc->icon_map);
    char icon_map_path[512];
    if (cbx_icon_map_default_path(icon_map_path, sizeof(icon_map_path)) == 0)
        cbx_icon_map_load(&svc->icon_map, icon_map_path);  /* best-effort */

    cbx_icon_cache_init(&svc->icon_cache, svc->rend.renderer,
                         cbx_icon_dir(), 48);
    cbx_icon_cache_load(&svc->icon_cache, &svc->icon_map);  /* best-effort */

    /* --- 6. Build the selection grid --------------------------------- */
    svc->comp_count = svc->model.composite_count;
    if (svc->comp_count > CBX_MAX_COMPOSITES)
        svc->comp_count = CBX_MAX_COMPOSITES;
    for (int i = 0; i < svc->comp_count; i++)
        fill_composite_info(&svc->composites[i], &svc->model.composites[i],
                             svc->conn.backend, svc->conn.bus);

    cbx_select_grid_init(&svc->grid);
    cbx_select_grid_build(&svc->grid, svc->composites, svc->comp_count,
                           &svc->settings, &svc->assignments);

    /* --- 7. Initialise and pre-render the overlay surface ------------ */
    rc = cbx_overlay_surface_init(&svc->surface, svc->rend.renderer,
                                   CBX_RENDERER_DEFAULT_W,
                                   CBX_RENDERER_DEFAULT_H,
                                   svc->settings.overlay_opacity);
    if (rc != 0) {
        fprintf(stderr,
                "controller-box: failed to create overlay surface: %d\n",
                rc);
        cbx_icon_cache_cleanup(&svc->icon_cache);
        cbx_text_cache_cleanup(&svc->text_cache);
        ip_connection_disconnect(&svc->conn);
        cbx_renderer_shutdown(&svc->rend);
        free(svc);
        return 1;
    }

    svc->render_ctx = (cbx_grid_render_ctx){
        .grid       = &svc->grid,
        .icon_cache = &svc->icon_cache,
        .icon_map   = &svc->icon_map,
        .theme      = &svc->theme,
        .text_cache = svc->font_id >= 0 ? &svc->text_cache : NULL,
        .font_id    = svc->font_id,
    };
    cbx_overlay_surface_mark_dirty_all(&svc->surface);
    cbx_overlay_surface_render(&svc->surface, svc->rend.renderer,
                                cbx_select_grid_render_cb, &svc->render_ctx);

    /* --- 8. Register overlay triggers + set PASS --------------------- */
    if (svc->comp_count > 0) {
        const char *paths[CBX_MAX_COMPOSITES];
        for (int i = 0; i < svc->comp_count; i++)
            paths[i] = svc->composites[i].composite_path;
        cbx_trigger_register_all(svc->conn.backend, svc->conn.bus,
                                  paths, svc->comp_count,
                                  svc->settings.overlay_trigger);
    }
    set_all_pass(svc->conn.backend, svc->conn.bus,
                  svc->model.composites, svc->comp_count);

    /* --- 9. Initialise overlay lifecycle ----------------------------- */
    const char *primary_path =
        svc->comp_count > 0 ? svc->composites[0].composite_path : "";
    cbx_overlay_lifecycle_init(&svc->lifecycle, svc->conn.backend, svc->conn.bus,
                               primary_path, &svc->surface, svc->rend.renderer);

    /* --- 10. Set up mode state + callbacks ---------------------------- */
    /* Player Mode (SPEC §4.3). */
    cbx_player_mode_init(&svc->pm, &svc->grid);
    svc->pm.on_slot_change     = on_slot_change;
    svc->pm.slot_change_data   = svc;
    svc->pm.on_profile_change  = on_profile_change;
    svc->pm.profile_change_data = svc;

    /* Host Mode (SPEC §4.4). */
    cbx_host_mode_init(&svc->hm);
    svc->hm.on_slot_change    = on_host_slot_change;
    svc->hm.slot_change_data  = svc;

    /* Lifecycle on_save callback: conflict resolution + assignment save. */
    svc->lifecycle.on_save       = on_overlay_save;
    svc->lifecycle.on_save_data  = svc;

    /* --- 10b. Set up DBus InputEvent signal handling (Task 6) ------- */
    svc->input_ctx.pm        = &svc->pm;
    svc->input_ctx.hm        = &svc->hm;
    svc->input_ctx.grid      = &svc->grid;
    svc->input_ctx.lifecycle  = &svc->lifecycle;
    svc->input_ctx.path_count = 0;
    cbx_overlay_input_build_map(svc->conn.backend, svc->conn.bus,
                                  svc->composites, svc->comp_count,
                                  &svc->input_ctx);

    const char *uniq = ip_connection_get_unique_name(&svc->conn);
    snprintf(svc->expected_sender, sizeof(svc->expected_sender),
             "%s", uniq ? uniq : "");
    ip_input_events_init(&svc->input_events, svc->conn.backend, svc->conn.bus,
                          svc->expected_sender,
                          cbx_overlay_input_cb, &svc->input_ctx);
    svc->input_events_ready = false;
    if (ip_input_events_subscribe(&svc->input_events) == 0)
        svc->input_events_ready = true;

    /* --- 11. Set up InterceptMode polling --------------------------- */
    svc->poll_event_type = SDL_RegisterEvents(1);
    svc->poll_count = 0;

    if (svc->poll_event_type != (uint32_t)-1) {
        for (int i = 0; i < svc->comp_count && i < CBX_MAX_COMPOSITES; i++) {
            ip_intercept_poll_init(&svc->polls[i],
                                    svc->conn.backend, svc->conn.bus,
                                    svc->composites[i].composite_path,
                                    on_intercept_activating, &svc->lifecycle,
                                    on_intercept_deactivating, &svc->lifecycle,
                                    on_intercept_error, NULL);
            if (ip_intercept_poll_start(&svc->polls[i],
                                         IP_INTERCEPT_POLL_INTERVAL_MS,
                                         svc->poll_event_type) == 0) {
                svc->poll_count++;
            }
        }
    }

    svc->initialized = true;

    /* --- 12. Poll loop (Task 4) -------------------------------------- */
    while (g_running) {
        cbx_overlay_service_step(svc);
        SDL_Delay(10);
    }

    /* --- 13. Clean shutdown ------------------------------------------- */
    /* Stop all InterceptMode poll timers. */
    for (int i = 0; i < svc->poll_count; i++)
        ip_intercept_poll_stop(&svc->polls[i]);

    /* Force-close the overlay if still visible. */
    cbx_overlay_lifecycle_force_close(&svc->lifecycle);

    /* Destroy surface, cleanup caches, disconnect, shutdown renderer. */
    cbx_overlay_surface_destroy(&svc->surface);
    cbx_icon_cache_cleanup(&svc->icon_cache);
    cbx_text_cache_cleanup(&svc->text_cache);
    ip_connection_disconnect(&svc->conn);
    cbx_renderer_shutdown(&svc->rend);
    free(svc);
    return 0;
}