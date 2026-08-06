/*
 * manager.h — Manager application skeleton with tab bar.
 *
 * The manager is a separate SDL2 window mode (SPEC §5.1) providing a
 * console-style settings menu navigated entirely by controller:
 *   - Left/Right switches tabs (Controllers / Profiles / Settings)
 *   - Up/Down navigates within the active panel
 *
 * The manager owns its own cbx_renderer (a separate window from the
 * overlay service), text cache, theme, and focus chain.  Each tab has
 * a cbx_panel container; only the active tab's panel is visible.
 *
 * Subsequent tasks (35–39) populate the panel contents (controllers list,
 * profiles browser, settings editor, profile editor).  This skeleton
 * provides the tab bar, event dispatch, focus management, and rendering
 * loop.
 *
 * Task 34 — Manager skeleton and tab bar.
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

#endif /* CBX_MANAGER_H */