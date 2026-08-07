/*
 * manager.c — Manager application.
 *
 * Implements the manager mode (SPEC §5.1): a separate SDL2 window with a
 * tab bar at the top (Controllers / Profiles / Settings).  Left/Right
 * switches tabs; Up/Down navigates within the active panel via the
 * focus chain.
 *
 * The manager owns the complete lifecycle of all three tab modules:
 * controllers, profiles, and settings.  cbx_manager_init() connects to
 * the system DBus (best-effort) and initialises all three tabs.
 * cbx_manager_shutdown() tears down the tabs and disconnects DBus.
 *
 * Event dispatch flow:
 *   1. Try the currently focused widget — it may consume the event.
 *   2. If not consumed and the event is LEFT/RIGHT, forward to the
 *      tab bar (which switches tabs and fires on_change).
 *   3. If not consumed and the event is UP/DOWN, navigate the focus
 *      chain spatially.
 *   4. SDL_QUIT stops the main loop.
 *
 * Tab switching (on_change callback):
 *   - Update active_tab
 *   - Show only the active panel, hide the others
 *   - Refresh the newly active tab (replace stale data)
 *   - Rebuild the focus chain: tabbar (row 0) + active panel's children
 *   - Focus the tabbar (or first panel child if any)
 */
#include "manager/manager.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Forward declarations                                             */
/* ------------------------------------------------------------------ */

static void cbx_manager_on_tab_change(cbx_widget *w, int new_tab,
                                        void *user_data);
static void cbx_manager_rebuild_focus(cbx_manager *mgr);
static void cbx_manager_layout(cbx_manager *mgr);

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                         */
/* ------------------------------------------------------------------ */

int
cbx_manager_init(cbx_manager *mgr, const char *font_path)
{
    return cbx_manager_init_with_dbus(mgr, font_path, NULL, NULL);
}

int
cbx_manager_init_with_dbus(cbx_manager *mgr, const char *font_path,
                            const ip_dbus_backend *backend,
                            ip_bus_handle bus)
{
    if (!mgr)
        return -EINVAL;

    memset(mgr, 0, sizeof(*mgr));
    mgr->font_id = -1;
    mgr->active_tab = CBX_MGR_TAB_CONTROLLERS;
    mgr->running = false;

    /* --- Renderer + window (SPEC §5.1: separate SDL2 window) ------- */
    int rc = cbx_renderer_init(&mgr->rend, "Controller-Box Manager",
                                CBX_MGR_WINDOW_W, CBX_MGR_WINDOW_H, false);
    if (rc != 0)
        return rc;

    /* --- Text cache + font ----------------------------------------- */
    rc = cbx_text_cache_init(&mgr->text_cache, mgr->rend.renderer);
    if (rc != 0) {
        cbx_renderer_shutdown(&mgr->rend);
        return rc;
    }

    if (font_path && font_path[0] != '\0') {
        mgr->font_id = cbx_text_load_font(&mgr->text_cache, font_path,
                                           CBX_MGR_FONT_SIZE);
        if (mgr->font_id < 0) {
            /* A font path was provided but could not be loaded — the manager
             * is unusable without text, so fail with an actionable error
             * instead of silently degrading to a blank UI. */
            fprintf(stderr,
                    "controller-box: failed to load font '%s' (error %d)\n",
                    font_path, mgr->font_id);
            cbx_text_cache_cleanup(&mgr->text_cache);
            cbx_renderer_shutdown(&mgr->rend);
            return mgr->font_id;
        }
    }

    /* --- Theme + settings ------------------------------------------ */
    cbx_settings_defaults(&mgr->settings);
    cbx_theme_default(&mgr->theme);

    /* --- Tab bar (SPEC §5.1: 3 tabs) ------------------------------- */
    rc = cbx_tabbar_init(&mgr->tabbar, mgr->font_id,
                          &mgr->text_cache, &mgr->theme);
    if (rc != 0) {
        cbx_text_cache_cleanup(&mgr->text_cache);
        cbx_renderer_shutdown(&mgr->rend);
        return rc;
    }

    cbx_tabbar_add_tab(&mgr->tabbar, "Controllers", mgr);
    cbx_tabbar_add_tab(&mgr->tabbar, "Profiles", mgr);
    cbx_tabbar_add_tab(&mgr->tabbar, "Settings", mgr);
    cbx_tabbar_set_change_cb(&mgr->tabbar, cbx_manager_on_tab_change);

    /* --- Panels (one per tab, initially empty) --------------------- */
    for (int i = 0; i < CBX_MGR_TAB_COUNT; i++) {
        rc = cbx_panel_init(&mgr->panels[i], &mgr->theme);
        if (rc != 0) {
            /* Clean up already-initialised panels + tabbar. */
            cbx_widget_destroy(&mgr->tabbar.base);
            for (int j = 0; j < i; j++)
                cbx_widget_destroy(&mgr->panels[j].base);
            cbx_text_cache_cleanup(&mgr->text_cache);
            cbx_renderer_shutdown(&mgr->rend);
            return rc;
        }
        cbx_panel_set_draw_bg(&mgr->panels[i], false);
        /* Only the active panel is visible initially. */
        cbx_widget_set_visible(&mgr->panels[i].base,
                                i == mgr->active_tab);
    }

    /* --- Layout (must precede tab-module init so widgets read the
     *     correct panel rect during their layout phase) ------------- */
    cbx_manager_layout(mgr);

    /* --- Tab modules (manager owns the full lifecycle) ------------- */
    /* DBus connection: use injected backend if provided, otherwise
     * fall back to the production sd-bus backend (best-effort).
     * If InputPlumber is unavailable or there is no system bus, the
     * controllers tab will still initialise with an empty device list
     * and functional buttons — the degraded state. */
    if (backend) {
        /* Caller-provided (mock) backend — use directly, no connect. */
        mgr->dbus_backend = backend;
        mgr->dbus_bus = bus;
        mgr->dbus_connected = true;
    } else {
        /* Production path: get sd-bus backend and connect. */
        mgr->dbus_backend = ip_dbus_sd_backend();
        mgr->dbus_bus = NULL;
        mgr->dbus_connected = false;
        if (mgr->dbus_backend && mgr->dbus_backend->connect) {
            int dbrc = mgr->dbus_backend->connect(&mgr->dbus_bus);
            if (dbrc == 0 && mgr->dbus_bus) {
                mgr->dbus_connected = true;
            } else {
                /* DBus connect failed — proceed in degraded mode. */
                mgr->dbus_backend = NULL;
                mgr->dbus_bus = NULL;
            }
        } else {
            mgr->dbus_backend = NULL;
        }
    }

    /* Controllers tab (DBus-backed; works in degraded mode with NULL). */
    rc = cbx_controllers_tab_init(&mgr->ct, &mgr->panels[0],
                                   mgr->dbus_backend, mgr->dbus_bus,
                                   &mgr->text_cache, &mgr->theme,
                                   mgr->font_id);
    if (rc != 0) {
        /* Clean up DBus + already-initialised resources. */
        if (mgr->dbus_connected && mgr->dbus_backend &&
            mgr->dbus_backend->disconnect)
            mgr->dbus_backend->disconnect(mgr->dbus_bus);
        cbx_widget_destroy(&mgr->tabbar.base);
        for (int i = 0; i < CBX_MGR_TAB_COUNT; i++)
            cbx_widget_destroy(&mgr->panels[i].base);
        cbx_text_cache_cleanup(&mgr->text_cache);
        cbx_renderer_shutdown(&mgr->rend);
        return rc;
    }

    /* Profiles tab (filesystem-backed; does not auto-refresh). */
    rc = cbx_profiles_tab_init(&mgr->pt, &mgr->panels[1],
                                &mgr->text_cache, &mgr->theme,
                                mgr->font_id);
    if (rc != 0) {
        cbx_controllers_tab_shutdown(&mgr->ct);
        if (mgr->dbus_connected && mgr->dbus_backend &&
            mgr->dbus_backend->disconnect)
            mgr->dbus_backend->disconnect(mgr->dbus_bus);
        cbx_widget_destroy(&mgr->tabbar.base);
        for (int i = 0; i < CBX_MGR_TAB_COUNT; i++)
            cbx_widget_destroy(&mgr->panels[i].base);
        cbx_text_cache_cleanup(&mgr->text_cache);
        cbx_renderer_shutdown(&mgr->rend);
        return rc;
    }
    cbx_profiles_tab_refresh(&mgr->pt);

    /* Settings tab (auto-refreshes on init). */
    rc = cbx_settings_tab_init(&mgr->st, &mgr->panels[2],
                                &mgr->text_cache, &mgr->theme,
                                mgr->font_id);
    if (rc != 0) {
        cbx_profiles_tab_shutdown(&mgr->pt);
        cbx_controllers_tab_shutdown(&mgr->ct);
        if (mgr->dbus_connected && mgr->dbus_backend &&
            mgr->dbus_backend->disconnect)
            mgr->dbus_backend->disconnect(mgr->dbus_bus);
        cbx_widget_destroy(&mgr->tabbar.base);
        for (int i = 0; i < CBX_MGR_TAB_COUNT; i++)
            cbx_widget_destroy(&mgr->panels[i].base);
        cbx_text_cache_cleanup(&mgr->text_cache);
        cbx_renderer_shutdown(&mgr->rend);
        return rc;
    }

    /* --- Focus chain (depends on tab modules being initialised) ----- */
    cbx_focus_chain_init(&mgr->focus);
    cbx_focus_chain_set_mode(&mgr->focus, CBX_FOCUS_MODE_HOST);
    cbx_manager_rebuild_focus(mgr);

    /* Focus the tab bar initially. */
    cbx_focus_chain_focus_first(&mgr->focus);

    /* --- Show the window ------------------------------------------- */
    cbx_renderer_show(&mgr->rend);

    return 0;
}

int
cbx_manager_run(cbx_manager *mgr)
{
    if (!mgr)
        return -EINVAL;

    mgr->running = true;
    SDL_Event ev;

    while (mgr->running) {
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                mgr->running = false;
                break;
            }
            cbx_manager_handle_event(mgr, &ev);
        }
        cbx_manager_render(mgr);
        cbx_renderer_present(&mgr->rend);
    }

    return 0;
}

void
cbx_manager_stop(cbx_manager *mgr)
{
    if (mgr)
        mgr->running = false;
}

void
cbx_manager_shutdown(cbx_manager *mgr)
{
    if (!mgr)
        return;

    /* Tear down tab modules first (they remove widgets from panels). */
    cbx_controllers_tab_shutdown(&mgr->ct);
    cbx_profiles_tab_shutdown(&mgr->pt);
    cbx_settings_tab_shutdown(&mgr->st);

    /* Disconnect DBus if connected. */
    if (mgr->dbus_connected && mgr->dbus_backend &&
        mgr->dbus_backend->disconnect)
        mgr->dbus_backend->disconnect(mgr->dbus_bus);

    /* Destroy widgets. */
    cbx_widget_destroy(&mgr->tabbar.base);
    for (int i = 0; i < CBX_MGR_TAB_COUNT; i++)
        cbx_widget_destroy(&mgr->panels[i].base);

    /* Clean up text cache. */
    cbx_text_cache_cleanup(&mgr->text_cache);

    /* Shut down renderer (destroys window + renderer). */
    cbx_renderer_shutdown(&mgr->rend);

    memset(mgr, 0, sizeof(*mgr));
}

/* ------------------------------------------------------------------ */
/*  Event handling                                                     */
/* ------------------------------------------------------------------ */

bool
cbx_manager_handle_event(cbx_manager *mgr, const SDL_Event *ev)
{
    if (!mgr || !ev)
        return false;

    /* 1. Try the focused widget first. */
    cbx_widget *focused = cbx_focus_chain_get_focused_widget(&mgr->focus);
    if (focused && cbx_widget_handle_event(focused, ev))
        return true;

    /* 2. Handle key events for tab/focus navigation. */
    if (ev->type == SDL_KEYDOWN) {
        SDL_Keycode key = ev->key.keysym.sym;

        switch (key) {
        case SDLK_LEFT:
        case SDLK_RIGHT:
            /* Left/Right switches tabs (SPEC §5.1). */
            return cbx_widget_handle_event(&mgr->tabbar.base, ev);

        case SDLK_UP:
            return cbx_focus_chain_navigate(&mgr->focus, CBX_NAV_UP) >= 0;

        case SDLK_DOWN:
            return cbx_focus_chain_navigate(&mgr->focus, CBX_NAV_DOWN) >= 0;

        case SDLK_RETURN:
        case SDLK_SPACE:
        case SDLK_a:
            /* A/Enter: activate the focused widget if it has a callback. */
            return true;

        case SDLK_ESCAPE:
        case SDLK_b:
            /* B/Escape: does nothing special in the manager. */
            return false;

        default:
            return false;
        }
    }

    return false;
}

/* ------------------------------------------------------------------ */
/*  Rendering                                                          */
/* ------------------------------------------------------------------ */

void
cbx_manager_render(cbx_manager *mgr)
{
    if (!mgr)
        return;

    cbx_renderer_clear(&mgr->rend, mgr->theme.bg);

    /* Draw the tab bar. */
    cbx_widget_draw(&mgr->tabbar.base, mgr->rend.renderer);

    /* Draw only the active panel. */
    if (mgr->active_tab >= 0 && mgr->active_tab < CBX_MGR_TAB_COUNT)
        cbx_widget_draw(&mgr->panels[mgr->active_tab].base,
                         mgr->rend.renderer);
}

/* ------------------------------------------------------------------ */
/*  Tab switching callback                                             */
/* ------------------------------------------------------------------ */

static void
cbx_manager_on_tab_change(cbx_widget *w, int new_tab, void *user_data)
{
    (void)w;
    cbx_manager *mgr = (cbx_manager *)user_data;
    if (!mgr || new_tab < 0 || new_tab >= CBX_MGR_TAB_COUNT)
        return;

    mgr->active_tab = new_tab;

    /* Show only the active panel. */
    for (int i = 0; i < CBX_MGR_TAB_COUNT; i++)
        cbx_widget_set_visible(&mgr->panels[i].base, i == new_tab);

    /* Refresh the newly active tab to replace stale data. */
    switch (new_tab) {
    case CBX_MGR_TAB_CONTROLLERS:
        /* Guard against NULL backend (degraded mode). */
        if (mgr->ct.backend)
            cbx_controllers_tab_refresh(&mgr->ct);
        break;
    case CBX_MGR_TAB_PROFILES:
        cbx_profiles_tab_refresh(&mgr->pt);
        break;
    case CBX_MGR_TAB_SETTINGS:
        cbx_settings_tab_refresh(&mgr->st);
        break;
    default:
        break;
    }

    /* Rebuild the focus chain with the new active panel's children. */
    cbx_manager_rebuild_focus(mgr);

    /* Re-focus the tab bar. */
    cbx_focus_chain_focus_widget(&mgr->focus, &mgr->tabbar.base);
}

/* ------------------------------------------------------------------ */
/*  Focus chain management                                             */
/* ------------------------------------------------------------------ */

static void
cbx_manager_rebuild_focus(cbx_manager *mgr)
{
    cbx_focus_chain_clear(&mgr->focus);

    /* Tab bar is always at row 0. */
    SDL_Rect tabbar_rect;
    cbx_widget_get_rect(&mgr->tabbar.base, &tabbar_rect);
    cbx_focus_chain_add(&mgr->focus, &mgr->tabbar.base, &tabbar_rect, 0);

    /* Add the active panel's focusable children (if any). */
    cbx_panel *panel = &mgr->panels[mgr->active_tab];
    for (int i = 0; i < panel->child_count; i++) {
        cbx_widget *child = panel->children[i];
        if (!child)
            continue;
        SDL_Rect child_rect;
        cbx_widget_get_rect(child, &child_rect);
        /* Panel children are at row 1+.  Each child gets its own row
         * so UP/DOWN navigates between them. */
        cbx_focus_chain_add(&mgr->focus, child, &child_rect, 1 + i);
    }
}

/* ------------------------------------------------------------------ */
/*  Layout                                                             */
/* ------------------------------------------------------------------ */

static void
cbx_manager_layout(cbx_manager *mgr)
{
    /* Tab bar: full width at the top. */
    SDL_Rect tabbar_rect = {
        .x = 0,
        .y = 0,
        .w = mgr->rend.window_w,
        .h = CBX_MGR_TABBAR_H,
    };
    cbx_widget_set_rect(&mgr->tabbar.base, &tabbar_rect);

    /* Panels: fill the area below the tab bar. */
    SDL_Rect panel_rect = {
        .x = 0,
        .y = CBX_MGR_TABBAR_H,
        .w = mgr->rend.window_w,
        .h = mgr->rend.window_h - CBX_MGR_TABBAR_H,
    };
    for (int i = 0; i < CBX_MGR_TAB_COUNT; i++)
        cbx_widget_set_rect(&mgr->panels[i].base, &panel_rect);
}

/* ------------------------------------------------------------------ */
/*  Accessors                                                          */
/* ------------------------------------------------------------------ */

int
cbx_manager_active_tab(const cbx_manager *mgr)
{
    return mgr ? mgr->active_tab : -1;
}

int
cbx_manager_tab_count(const cbx_manager *mgr)
{
    return mgr ? cbx_tabbar_tab_count(&mgr->tabbar) : 0;
}

const cbx_tabbar *
cbx_manager_tabbar(const cbx_manager *mgr)
{
    return mgr ? &mgr->tabbar : NULL;
}

const cbx_panel *
cbx_manager_panel(const cbx_manager *mgr, int tab)
{
    if (!mgr || tab < 0 || tab >= CBX_MGR_TAB_COUNT)
        return NULL;
    return &mgr->panels[tab];
}

const cbx_focus_chain *
cbx_manager_focus(const cbx_manager *mgr)
{
    return mgr ? &mgr->focus : NULL;
}

/* --- Tab module accessors ----------------------------------------- */

cbx_controllers_tab *
cbx_manager_controllers_tab(cbx_manager *mgr)
{
    return mgr ? &mgr->ct : NULL;
}

cbx_profiles_tab *
cbx_manager_profiles_tab(cbx_manager *mgr)
{
    return mgr ? &mgr->pt : NULL;
}

cbx_settings_tab *
cbx_manager_settings_tab(cbx_manager *mgr)
{
    return mgr ? &mgr->st : NULL;
}