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
#include "ui/input_map.h"

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
static cbx_widget *cbx_manager_hit_test(cbx_manager *mgr,
                                          const SDL_Point *p);
static void cbx_manager_update_hover(cbx_manager *mgr,
                                      cbx_widget *hovered);
static bool cbx_manager_handle_mouse_event(cbx_manager *mgr,
                                             const SDL_Event *ev);
static bool cbx_manager_tab_handle_key(cbx_manager *mgr,
                                          const SDL_Event *ev);
static bool cbx_manager_tab_activate(cbx_manager *mgr);
static bool cbx_manager_tab_cancel(cbx_manager *mgr);
static void cbx_manager_check_mode_change(cbx_manager *mgr,
                                            int prev_mode);
static void cbx_manager_open_gamecontroller(cbx_manager *mgr,
                                              int device_index);
static void cbx_manager_close_gamecontroller(cbx_manager *mgr,
                                               SDL_JoystickID instance_id);
static bool cbx_manager_controller_to_key(const SDL_Event *ev,
                                            SDL_Event *key_event);
void cbx_manager_backend_ready(void *userdata);
void cbx_manager_backend_degraded(const char *reason, void *userdata);

static void
cbx_manager_open_gamecontroller(cbx_manager *mgr, int device_index)
{
    if (!mgr || mgr->gamecontroller_count >= CBX_MGR_MAX_GAMECONTROLLERS ||
        !SDL_IsGameController(device_index))
        return;
    SDL_GameController *controller = SDL_GameControllerOpen(device_index);
    if (!controller)
        return;
    mgr->gamecontrollers[mgr->gamecontroller_count++] = controller;
}

static void
cbx_manager_close_gamecontroller(cbx_manager *mgr,
                                  SDL_JoystickID instance_id)
{
    if (!mgr)
        return;
    for (int i = 0; i < mgr->gamecontroller_count; i++) {
        SDL_Joystick *joystick =
            SDL_GameControllerGetJoystick(mgr->gamecontrollers[i]);
        if (joystick && SDL_JoystickInstanceID(joystick) == instance_id) {
            SDL_GameControllerClose(mgr->gamecontrollers[i]);
            for (int j = i; j + 1 < mgr->gamecontroller_count; j++)
                mgr->gamecontrollers[j] = mgr->gamecontrollers[j + 1];
            mgr->gamecontrollers[--mgr->gamecontroller_count] = NULL;
            return;
        }
    }
}

void
cbx_manager_backend_ready(void *userdata)
{
    cbx_manager *mgr = userdata;
    if (!mgr)
        return;
    mgr->dbus_connected = true;
    mgr->ct.backend = mgr->dbus_backend;
    mgr->ct.bus = mgr->dbus_bus;
    if (cbx_controllers_tab_refresh(&mgr->ct) == 0)
        cbx_controllers_tab_set_available(&mgr->ct, true, NULL);
    else
        cbx_controllers_tab_set_available(&mgr->ct, false,
                                           "InputPlumber enumeration failed");
    cbx_profiles_tab_set_context(&mgr->pt, mgr->rend.renderer,
                                  mgr->dbus_backend, mgr->dbus_bus);
}

void
cbx_manager_backend_degraded(const char *reason, void *userdata)
{
    cbx_manager *mgr = userdata;
    if (!mgr)
        return;
    fprintf(stderr, "controller-box: %s\n",
            reason ? reason : "InputPlumber unavailable");
    mgr->dbus_connected = false;
    mgr->ct.backend = NULL;
    cbx_device_model_init(&mgr->ct.model);
    cbx_controllers_tab_set_available(&mgr->ct, false, reason);
    cbx_profiles_tab_set_context(&mgr->pt, mgr->rend.renderer, NULL, NULL);
}

static bool
cbx_manager_controller_to_key(const SDL_Event *ev, SDL_Event *key_event)
{
    if (!ev || !key_event ||
        (ev->type != SDL_CONTROLLERBUTTONDOWN &&
         ev->type != SDL_CONTROLLERBUTTONUP))
        return false;

    SDL_Keycode key;
    switch (ev->cbutton.button) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:    key = SDLK_UP; break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  key = SDLK_DOWN; break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  key = SDLK_LEFT; break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: key = SDLK_RIGHT; break;
    case SDL_CONTROLLER_BUTTON_A:          key = SDLK_a; break;
    case SDL_CONTROLLER_BUTTON_B:          key = SDLK_b; break;
    case SDL_CONTROLLER_BUTTON_START:      key = SDLK_TAB; break;
    default: return false;
    }

    memset(key_event, 0, sizeof(*key_event));
    key_event->type = ev->type == SDL_CONTROLLERBUTTONDOWN
                        ? SDL_KEYDOWN : SDL_KEYUP;
    key_event->key.type = key_event->type;
    key_event->key.windowID = CBX_CONTROLLER_EVENT_WINDOW_ID;
    key_event->key.state = ev->type == SDL_CONTROLLERBUTTONDOWN
                             ? SDL_PRESSED : SDL_RELEASED;
    key_event->key.repeat = 0;
    key_event->key.keysym.sym = key;
    return true;
}

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

    /* Real controller input is mandatory for Manager operation. */
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "controller-box: SDL game-controller init failed: %s\n",
                SDL_GetError());
        cbx_renderer_shutdown(&mgr->rend);
        return -EIO;
    }
    SDL_GameControllerEventState(SDL_ENABLE);
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        cbx_manager_open_gamecontroller(mgr, i);

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
        /* Caller-provided fixture is already connected and ready. */
        mgr->dbus_backend = backend;
        mgr->dbus_bus = bus;
        mgr->dbus_connected = true;
    } else {
        mgr->dbus_backend = ip_dbus_sd_backend();
        mgr->owns_dbus_connection = true;
        ip_connection_init(&mgr->connection, mgr->dbus_backend);
        int dbrc = ip_connection_connect(&mgr->connection);
        mgr->dbus_bus = mgr->connection.bus;
        mgr->dbus_connected = dbrc == 0 &&
                              ip_connection_is_connected(&mgr->connection);
        if (!mgr->dbus_bus)
            mgr->dbus_backend = NULL;
        mgr->dbus_init_rc = dbrc;  /* saved for degraded reason */
    }

    /* A live bus is retained in degraded mode for NameOwnerChanged, but
     * backend-changing controls are exposed only after Version readiness. */
    const ip_dbus_backend *ready_backend =
        mgr->dbus_connected ? mgr->dbus_backend : NULL;
    rc = cbx_controllers_tab_init(&mgr->ct, &mgr->panels[0],
                                   ready_backend, mgr->dbus_bus,
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

    /* Override the generic degraded reason with a specific actionable
     * message when the connection failed (SPEC §2.4). */
    if (!mgr->dbus_connected && !backend)
        cbx_controllers_tab_set_available(&mgr->ct, false,
            ip_connection_reason_for_error(mgr->dbus_init_rc));

    /* SPEC §5.2: wire expected target count from settings so the
     * controllers tab can detect orphan-columns (fewer targets than
     * configured) and show an error state. */
    cbx_controllers_tab_set_expected_count(&mgr->ct,
        mgr->settings.virtual_controllers.count);

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
    cbx_profiles_tab_set_context(&mgr->pt, mgr->rend.renderer,
                                   ready_backend, mgr->dbus_bus);
    cbx_profiles_tab_refresh(&mgr->pt);
    if (mgr->owns_dbus_connection) {
        ip_connection_set_reenumerate_cb(&mgr->connection,
                                          cbx_manager_backend_ready, mgr);
        ip_connection_set_degraded_cb(&mgr->connection,
                                       cbx_manager_backend_degraded, mgr);
    }

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
            cbx_manager_handle_event(mgr, &ev);
        }
        /* Dispatch InputPlumber signals used by capture/sequential editing
         * and connection/hotplug recovery.  Unit tests that inject signals
         * synchronously keep process() as a no-op. */
        if (mgr->dbus_backend && mgr->dbus_bus &&
            mgr->dbus_backend->process) {
            for (int i = 0; i < 64; i++) {
                int processed = mgr->dbus_backend->process(mgr->dbus_bus);
                if (processed <= 0)
                    break;
            }
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

    for (int i = 0; i < mgr->gamecontroller_count; i++)
        SDL_GameControllerClose(mgr->gamecontrollers[i]);
    mgr->gamecontroller_count = 0;
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);

    /* Tear down tab modules first (they remove widgets from panels). */
    cbx_controllers_tab_shutdown(&mgr->ct);
    cbx_profiles_tab_shutdown(&mgr->pt);
    cbx_settings_tab_shutdown(&mgr->st);

    if (mgr->owns_dbus_connection)
        ip_connection_disconnect(&mgr->connection);
    else {
        if (mgr->dbus_bus && mgr->dbus_backend &&
             mgr->dbus_backend->disconnect)
            mgr->dbus_backend->disconnect(mgr->dbus_bus);
        /* Still free connection-allocated strings even when the bus
         * handle is externally owned (e.g. injected by tests). */
        free(mgr->connection.unique_name);
        mgr->connection.unique_name = NULL;
        free(mgr->connection.version);
        mgr->connection.version = NULL;
    }

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

    /* SDL_QUIT (window close): if the profile editor has unsaved
     * changes, prompt before quitting instead of silently discarding
     * (SPEC section 5.3).  Otherwise quit immediately. */
    if (ev->type == SDL_QUIT) {
        if (mgr->pt.mode == CBX_PT_MODE_EDITOR &&
            mgr->pt.editor_initialized &&
            cbx_profile_editor_is_dirty(&mgr->pt.editor)) {
            /* Enter confirm-quit mode — user must choose save or discard */
            cbx_profiles_tab_begin_confirm_quit(&mgr->pt);
            return true;
        }
        if (mgr->pt.mode == CBX_PT_MODE_CONFIRM_QUIT) {
            /* Second SDL_QUIT while already prompting: force quit */
            mgr->running = false;
            return true;
        }
        mgr->running = false;
        return true;
    }

    /* SDL_WINDOWEVENT: handle resize to keep widget rects and focus
     * chain in sync with the new window dimensions (SPEC §5.1). */
    if (ev->type == SDL_WINDOWEVENT) {
        if (ev->window.event == SDL_WINDOWEVENT_RESIZED ||
            ev->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            mgr->rend.window_w = ev->window.data1;
            mgr->rend.window_h = ev->window.data2;
            cbx_manager_layout(mgr);
            cbx_manager_rebuild_focus(mgr);
            return true;
        }
        return false;
    }

    if (ev->type == SDL_CONTROLLERDEVICEADDED) {
        cbx_manager_open_gamecontroller(mgr, ev->cdevice.which);
        return true;
    }
    if (ev->type == SDL_CONTROLLERDEVICEREMOVED) {
        cbx_manager_close_gamecontroller(mgr, ev->cdevice.which);
        return true;
    }
    if (ev->type == SDL_CONTROLLERBUTTONDOWN ||
        ev->type == SDL_CONTROLLERBUTTONUP) {
        SDL_Event key_event;
        if (!cbx_manager_controller_to_key(ev, &key_event))
            return false;
        return cbx_manager_handle_event(mgr, &key_event);
    }

    /* Route mouse events via hit-testing of visible widgets (SPEC §5.1:
     * pointer is the secondary input path — every visible enabled
     * control must respond to clicks inside its rendered bounds). */
    if (ev->type == SDL_MOUSEMOTION ||
        ev->type == SDL_MOUSEBUTTONDOWN ||
        ev->type == SDL_MOUSEBUTTONUP) {
        /* Track mode changes from mouse events (button clicks can
         * trigger tab mode changes — e.g. Add button opens type picker). */
        int prev_mode = 0;
        switch (mgr->active_tab) {
        case CBX_MGR_TAB_CONTROLLERS: prev_mode = (int)mgr->ct.mode; break;
        case CBX_MGR_TAB_PROFILES:    prev_mode = (int)mgr->pt.mode * 100
                                         + (mgr->pt.editor_initialized
                                                ? (int)mgr->pt.editor.mode : 0);
                                      break;
        case CBX_MGR_TAB_SETTINGS:    prev_mode = (int)mgr->st.mode; break;
        default: break;
        }
        bool result = cbx_manager_handle_mouse_event(mgr, ev);
        cbx_manager_check_mode_change(mgr, prev_mode);
        return result;
    }

    /* Record the active tab’s mode before processing — used to detect
     * mode changes and trigger focus-chain rebuilds. */
    int prev_mode = 0;
    switch (mgr->active_tab) {
    case CBX_MGR_TAB_CONTROLLERS: prev_mode = (int)mgr->ct.mode; break;
    case CBX_MGR_TAB_PROFILES:    prev_mode = (int)mgr->pt.mode * 100
                                     + (mgr->pt.editor_initialized
                                            ? (int)mgr->pt.editor.mode : 0);
                                  break;
    case CBX_MGR_TAB_SETTINGS:    prev_mode = (int)mgr->st.mode; break;
    default: break;
    }

    /* 0. If the active tab is in a modal mode, let the tab handle the
     *    key first (before the focused widget gets a chance to consume
     *    it for navigation/activation).  This intercepts Up/Down in
     *    settings edit mode, letter keys in name input mode, and B
     *    for canceling modal sub-modes. */
    if (cbx_manager_tab_handle_key(mgr, ev)) {
        cbx_manager_check_mode_change(mgr, prev_mode);
        /* If a confirm-quit action completed, stop the manager. */
        if (mgr->pt.quit_after_action) {
            mgr->running = false;
            mgr->pt.quit_after_action = false;
        }
        return true;
    }

    /* 1. Try the focused widget first. */
    cbx_widget *focused = cbx_focus_chain_get_focused_widget(&mgr->focus);
    if (focused && cbx_widget_handle_event(focused, ev)) {
        cbx_manager_check_mode_change(mgr, prev_mode);
        return true;
    }

    /* 2. Handle key events for tab/focus navigation. */
    if (ev->type == SDL_KEYDOWN) {
        SDL_Keycode key = ev->key.keysym.sym;

        switch (key) {
        case SDLK_LEFT:
        case SDLK_RIGHT:
        {
            /* On the tab bar: Left/Right switches tabs (SPEC §5.1).
             * On a panel child: try horizontal focus navigation first
             * (side-by-side buttons share a row), then fall back to
             * tab switching so the user can always change tabs. */
            cbx_widget *cur =
                cbx_focus_chain_get_focused_widget(&mgr->focus);
            if (cur != &mgr->tabbar.base) {
                cbx_nav_direction dir =
                    (key == SDLK_LEFT) ? CBX_NAV_LEFT : CBX_NAV_RIGHT;
                if (cbx_focus_chain_navigate(&mgr->focus, dir) >= 0) {
                    cbx_manager_check_mode_change(mgr, prev_mode);
                    return true;
                }
            }
            return cbx_widget_handle_event(&mgr->tabbar.base, ev);
        }

        case SDLK_UP:
            return cbx_focus_chain_navigate(&mgr->focus, CBX_NAV_UP) >= 0;

        case SDLK_DOWN:
            return cbx_focus_chain_navigate(&mgr->focus, CBX_NAV_DOWN) >= 0;

        case SDLK_RETURN:
        case SDLK_SPACE:
        case SDLK_a:
            /* A/Enter KEYDOWN is swallowed (visual pressed state is
             * handled by the focused widget in step 1).  Activation
             * fires on KEYUP — see below. */
            return true;

        case SDLK_ESCAPE:
        case SDLK_b:
            /* B/Escape: handled by tab_handle_key in modal modes.
             * In list mode, B is a no-op. */
            return false;

        default:
            return false;
        }
    }

    if (ev->type == SDL_KEYUP) {
        SDL_Keycode key = ev->key.keysym.sym;

        switch (key) {
        case SDLK_a:
        case SDLK_RETURN:
        case SDLK_SPACE:
            /* A/Enter KEYUP: forward to the active tab’s activate
             * function (only reached if the focused widget didn’t
             * consume the KEYUP — e.g. focus is on the tabbar or a
             * list with no on_select).  The activate function checks
             * the tab’s mode and dispatches (type picker confirm,
             * name input confirm, delete confirm, settings activate). */
            {
                bool handled = cbx_manager_tab_activate(mgr);
                cbx_manager_check_mode_change(mgr, prev_mode);
                return handled;
            }

        case SDLK_b:
        case SDLK_ESCAPE:
            /* B KEYUP: try tab cancel (modal modes). */
            {
                bool handled = cbx_manager_tab_cancel(mgr);
                cbx_manager_check_mode_change(mgr, prev_mode);
                return handled;
            }

        default:
            return false;
        }
    }

    return false;
}

/* ------------------------------------------------------------------ */
/*  Pointer event routing (SPEC §5.1: mouse as secondary path)       */
/* ------------------------------------------------------------------ */

static cbx_widget *
cbx_manager_hit_test(cbx_manager *mgr, const SDL_Point *p)
{
    /* Check the tab bar first (always visible). */
    if (mgr->tabbar.base.visible &&
        SDL_PointInRect(p, &mgr->tabbar.base.rect))
        return &mgr->tabbar.base;

    /* Check the active panel's visible children. */
    cbx_panel *panel = &mgr->panels[mgr->active_tab];
    for (int i = 0; i < panel->child_count; i++) {
        cbx_widget *child = panel->children[i];
        if (!child || !child->visible)
            continue;
        if (SDL_PointInRect(p, &child->rect))
            return child;
    }

    return NULL;
}

static void
cbx_manager_update_hover(cbx_manager *mgr, cbx_widget *hovered)
{
    /* Clear hover on the tabbar. */
    mgr->tabbar.base.hover = false;

    /* Clear hover on all active panel children. */
    cbx_panel *panel = &mgr->panels[mgr->active_tab];
    for (int i = 0; i < panel->child_count; i++) {
        cbx_widget *child = panel->children[i];
        if (child)
            child->hover = false;
    }

    /* Set hover on the widget under the cursor. */
    if (hovered)
        hovered->hover = true;
}

static bool
cbx_manager_handle_mouse_event(cbx_manager *mgr, const SDL_Event *ev)
{
    SDL_Point p;

    if (ev->type == SDL_MOUSEMOTION) {
        p.x = ev->motion.x;
        p.y = ev->motion.y;
    } else {
        p.x = ev->button.x;
        p.y = ev->button.y;
    }

    /* Find the visible widget under the cursor. */
    cbx_widget *hit = cbx_manager_hit_test(mgr, &p);

    /* Update hover state on mouse motion. */
    if (ev->type == SDL_MOUSEMOTION)
        cbx_manager_update_hover(mgr, hit);

    /* Focus the widget on left-button down so keyboard focus
     * follows the pointer (SPEC §5.7: controller and pointer
     * activation invoke the same behavior). */
    if (ev->type == SDL_MOUSEBUTTONDOWN &&
        ev->button.button == SDL_BUTTON_LEFT && hit) {
        cbx_focus_chain_focus_widget(&mgr->focus, hit);
    }

    /* Dispatch the event to the hit widget. */
    if (hit)
        return cbx_widget_handle_event(hit, ev);

    /* Click on empty space — no side effect. */
    return false;
}

/* ------------------------------------------------------------------ */
/*  Tab-level key handling, activation, and mode-change tracking       */
/* ------------------------------------------------------------------ */

static bool
cbx_manager_tab_handle_key(cbx_manager *mgr, const SDL_Event *ev)
{
    if (!mgr || !ev)
        return false;

    switch (mgr->active_tab) {
    case CBX_MGR_TAB_CONTROLLERS:
        return cbx_controllers_tab_handle_key(&mgr->ct, ev);
    case CBX_MGR_TAB_PROFILES:
        return cbx_profiles_tab_handle_key(&mgr->pt, ev);
    case CBX_MGR_TAB_SETTINGS:
        return cbx_settings_tab_handle_key(&mgr->st, ev);
    default:
        return false;
    }
}

static bool
cbx_manager_tab_activate(cbx_manager *mgr)
{
    if (!mgr)
        return false;

    switch (mgr->active_tab) {
    case CBX_MGR_TAB_CONTROLLERS:
        return cbx_controllers_tab_activate(&mgr->ct) == 0;
    case CBX_MGR_TAB_PROFILES:
        return cbx_profiles_tab_activate(&mgr->pt) == 0;
    case CBX_MGR_TAB_SETTINGS:
        return cbx_settings_tab_activate(&mgr->st) == 0;
    default:
        return false;
    }
}

static bool
cbx_manager_tab_cancel(cbx_manager *mgr)
{
    if (!mgr)
        return false;

    switch (mgr->active_tab) {
    case CBX_MGR_TAB_CONTROLLERS:
        return cbx_controllers_tab_cancel(&mgr->ct);
    case CBX_MGR_TAB_PROFILES:
        return cbx_profiles_tab_cancel(&mgr->pt);
    case CBX_MGR_TAB_SETTINGS:
        return mgr->st.mode == CBX_ST_MODE_EDIT
            ? (cbx_settings_tab_cancel_edit(&mgr->st), true)
            : false;
    default:
        return false;
    }
}

static void
cbx_manager_check_mode_change(cbx_manager *mgr, int prev_mode)
{
    if (!mgr)
        return;

    int cur_mode = 0;
    switch (mgr->active_tab) {
    case CBX_MGR_TAB_CONTROLLERS: cur_mode = (int)mgr->ct.mode; break;
    case CBX_MGR_TAB_PROFILES:    cur_mode = (int)mgr->pt.mode * 100
                                     + (mgr->pt.editor_initialized
                                            ? (int)mgr->pt.editor.mode : 0);
                                  break;
    case CBX_MGR_TAB_SETTINGS:    cur_mode = (int)mgr->st.mode; break;
    default: return;
    }

    if (cur_mode == prev_mode)
        return;

    /* Mode changed — rebuild the focus chain (only visible widgets)
     * and focus the first visible panel child, or the tabbar. */
    cbx_manager_rebuild_focus(mgr);

    /* Find the first visible panel child to focus. */
    cbx_panel *panel = &mgr->panels[mgr->active_tab];
    cbx_widget *first_visible = NULL;
    for (int i = 0; i < panel->child_count; i++) {
        if (panel->children[i] && panel->children[i]->visible &&
            panel->children[i]->interactive) {
            first_visible = panel->children[i];
            break;
        }
    }

    if (first_visible)
        cbx_focus_chain_focus_widget(&mgr->focus, first_visible);
    else
        cbx_focus_chain_focus_widget(&mgr->focus, &mgr->tabbar.base);
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

    /* Add the active panel’s focusable children (if any).  Skip invisible
     * widgets so that hidden type pickers, create pickers, etc. don’t
     * appear in the focus chain. */
    cbx_panel *panel = &mgr->panels[mgr->active_tab];
    int row = 1;
    int prev_cy = -1;
    for (int i = 0; i < panel->child_count; i++) {
        cbx_widget *child = panel->children[i];
        if (!child || !child->visible || !child->interactive)
            continue;
        SDL_Rect child_rect;
        cbx_widget_get_rect(child, &child_rect);
        /* Group children by vertical proximity: widgets at the same
         * y level (within 10 px) share a row so LEFT/RIGHT navigates
         * between them (e.g. side-by-side buttons).  Widgets at
         * different y levels get separate rows for UP/DOWN. */
        int cy = child_rect.y + child_rect.h / 2;
        if (prev_cy >= 0) {
            int diff = cy - prev_cy;
            if (diff < 0) diff = -diff;
            if (diff > 10)
                row++;
        }
        cbx_focus_chain_add(&mgr->focus, child, &child_rect, row);
        prev_cy = cy;
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

    /* Reposition tab widgets relative to the new panel rect. */
    cbx_controllers_tab_layout(&mgr->ct);
    cbx_profiles_tab_layout(&mgr->pt);
    cbx_settings_tab_layout(&mgr->st);
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