/*
 * overlay_service.c — overlay service startup (Task 3, SPEC §2.3–§2.5).
 *
 * Implements run_overlay_service(): the production init path for the
 * overlay service mode.  The function initialises SDL, connects to
 * InputPlumber via the system DBus, enumerates composite devices,
 * pre-builds the overlay surface, registers intercept triggers, and
 * enters a minimal poll loop.  Task 4 replaces the loop body with full
 * InterceptMode polling, signal handling, and input processing.
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
#include "dbus/ip_intercept_poll.h"
#include "overlay/surface_build.h"
#include "overlay/lifecycle.h"
#include "overlay/grid_render.h"
#include "overlay/trigger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Helper: fill cbx_grid_composite_info from the device model + DBus  */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/*  Helper: set InterceptMode = PASS on all composites                 */
/* ------------------------------------------------------------------ */
static void set_all_pass(const ip_dbus_backend *backend, ip_bus_handle bus,
                          const cbx_composite_entry *composites, int count)
{
    for (int i = 0; i < count; i++)
        ip_composite_set_intercept_mode(backend, bus,
                                         composites[i].path, "1");
}

/* ------------------------------------------------------------------ */
/*  Main entry                                                         */
/* ------------------------------------------------------------------ */
int run_overlay_service(int dry_run)
{
    printf("controller-box: overlay-service mode%s\n",
           dry_run ? " (dry-run)" : "");
    if (dry_run)
        return 0;

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

    /* --- 10. Poll loop (skeleton — Task 4 fills in the body) --------- *
     *                                                                    *
     * Task 4 will add:                                                    *
     *   - ip_intercept_poll per composite at 50 ms (DEC-002)             *
     *   - SDL event processing (input navigation)                        *
     *   - SIGTERM / SIGINT handling for clean shutdown                   *
     *   - overlay activate / close lifecycle calls                      *
     *                                                                    *
     * For now a minimal event loop keeps the service alive.             */
    SDL_Event ev;
    int running = 1;
    while (running) {
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT)
                running = 0;
        }
        SDL_Delay(IP_INTERCEPT_POLL_INTERVAL_MS);
    }

    /* --- 11. Clean shutdown ------------------------------------------- */
    cbx_overlay_lifecycle_force_close(&lifecycle);
    cbx_overlay_surface_destroy(&surface);
    cbx_icon_cache_cleanup(&icon_cache);
    cbx_text_cache_cleanup(&text_cache);
    ip_connection_disconnect(&conn);
    cbx_renderer_shutdown(&rend);
    return 0;
}