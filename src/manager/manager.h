/*
 * manager.h — Manager application.
 *
 * The manager is a separate SDL2 window mode (SPEC §5.1) providing a
 * console-style settings menu navigated entirely by controller:
 *   - Left/Right switches tabs (Controllers / Profiles / Settings)
 *   - Up/Down navigates within the active panel
 *
 * The manager owns its own cbx_renderer (a separate window from the
 * overlay service), text cache, theme, and focus chain.  It also owns
 * the complete lifecycle of all three tab modules — cbx_controllers_tab,
 * cbx_profiles_tab, and cbx_settings_tab — which are initialised during
 * cbx_manager_init() and torn down during cbx_manager_shutdown().
 * Each tab populates its corresponding cbx_panel; only the active tab's
 * panel is visible.
 */
#ifndef CBX_MANAGER_H
#define CBX_MANAGER_H

#include <SDL2/SDL.h>
#include <stdbool.h>

#include "ui/renderer.h"
#include "ui/widget.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/focus.h"
#include "config/config_settings.h"
#include "manager/controllers_tab.h"
#include "manager/profiles_tab.h"
#include "manager/settings_tab.h"
#include "dbus_mock.h"  /* ip_dbus_backend, ip_bus_handle, ip_dbus_sd_backend */

/* ------------------------------------------------------------------ */
/*  Tab identifiers                                                   */
/* ------------------------------------------------------------------ */

typedef enum {
    CBX_MGR_TAB_CONTROLLERS = 0,
    CBX_MGR_TAB_PROFILES    = 1,
    CBX_MGR_TAB_SETTINGS    = 2,
    CBX_MGR_TAB_COUNT       = 3,
} cbx_mgr_tab;

/* Layout constants. */
#define CBX_MGR_WINDOW_W  1280
#define CBX_MGR_WINDOW_H  720
#define CBX_MGR_TABBAR_H  48
#define CBX_MGR_FONT_SIZE 18

/* ------------------------------------------------------------------ */
/*  Manager                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    /* Rendering — manager owns its own window (SPEC §5.1). */
    cbx_renderer  rend;
    cbx_text_cache text_cache;
    cbx_theme     theme;
    cbx_settings  settings;
    int           font_id;       /* -1 = no font loaded */

    /* UI widgets. */
    cbx_tabbar    tabbar;
    cbx_panel     panels[CBX_MGR_TAB_COUNT];  /* one per tab */
    int           active_tab;

    /* Focus navigation. */
    cbx_focus_chain focus;

    /* Tab module state — manager owns the full lifecycle. */
    cbx_controllers_tab ct;   /* Controllers tab (DBus-backed)  */
    cbx_profiles_tab    pt;   /* Profiles tab (filesystem)      */
    cbx_settings_tab    st;   /* Settings tab (local settings)  */

    /* DBus connection (for controllers tab). */
    const ip_dbus_backend *dbus_backend;  /* NULL if no bus available  */
    ip_bus_handle          dbus_bus;      /* NULL if not connected      */
    bool                   dbus_connected;

    /* Running flag. */
    bool          running;
} cbx_manager;

/*
 * Initialise the manager: create the SDL2 window, load theme/settings,
 * build the tab bar with 3 tabs, create empty panels, set up the focus
 * chain, and show the window.
 *
 * @param mgr       Output struct (overwritten).
 * @param font_path Path to a TTF font, or NULL to skip font loading.
 * @return 0 on success, negative errno on error.
 */
int  cbx_manager_init(cbx_manager *mgr, const char *font_path);

/*
 * Run the main event loop.  Polls SDL events, dispatches them, renders
 * the frame, and presents.  Returns when the window is closed or
 * cbx_manager_stop() is called.
 *
 * @return 0 on normal exit, negative errno on fatal error.
 */
int  cbx_manager_run(cbx_manager *mgr);

/*
 * Signal the main loop to stop.  Safe to call from event handlers.
 */
void cbx_manager_stop(cbx_manager *mgr);

/*
 * Process a single SDL event.  Public for testing.  Returns true if
 * the event was consumed.
 */
bool cbx_manager_handle_event(cbx_manager *mgr, const SDL_Event *ev);

/*
 * Render one frame.  Clears the screen, draws the tab bar and the
 * active panel.  Public for testing.
 */
void cbx_manager_render(cbx_manager *mgr);

/*
 * Shut down and free all resources.  Safe to call on a zeroed struct.
 */
void cbx_manager_shutdown(cbx_manager *mgr);

/* --- Accessors (for testing) -------------------------------------- */

int  cbx_manager_active_tab(const cbx_manager *mgr);
int  cbx_manager_tab_count(const cbx_manager *mgr);
const cbx_tabbar *cbx_manager_tabbar(const cbx_manager *mgr);
const cbx_panel  *cbx_manager_panel(const cbx_manager *mgr, int tab);
const cbx_focus_chain *cbx_manager_focus(const cbx_manager *mgr);

/* --- Tab module accessors (for testing) --------------------------- */

/*
 * Return a pointer to the manager-owned controllers/profiles/settings
 * tab instance.  These are populated by cbx_manager_init() and allow
 * tests to inspect or configure the tabs without manual init.
 */
cbx_controllers_tab *cbx_manager_controllers_tab(cbx_manager *mgr);
cbx_profiles_tab    *cbx_manager_profiles_tab(cbx_manager *mgr);
cbx_settings_tab    *cbx_manager_settings_tab(cbx_manager *mgr);

#endif /* CBX_MANAGER_H */