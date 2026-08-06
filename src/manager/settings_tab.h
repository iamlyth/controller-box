/*
 * settings_tab.h — Settings tab for the Manager UI (SPEC §5.5).
 *
 * Displays and edits app-level settings: launch at boot, theme, overlay
 * opacity, virtual controller startup config (count + types), overlay
 * trigger combo, and controller icon overrides (§8.4).
 *
 * Navigation: Up/Down to select a setting, A to activate/edit.
 * In edit mode: context-dependent (toggle, cycle, adjust).
 * B cancels edit mode. A "Save" button writes settings to disk.
 *
 * Task 39 — Profile save and Settings tab.
 */
#ifndef CBX_SETTINGS_TAB_H
#define CBX_SETTINGS_TAB_H

#include <SDL2/SDL.h>
#include <stdbool.h>

#include "ui/widget.h"             /* cbx_panel, cbx_list, cbx_button */
#include "ui/text.h"               /* cbx_text_cache */
#include "ui/theme.h"             /* cbx_theme */
#include "config/config_settings.h"

/* ------------------------------------------------------------------ */
/*  Limits                                                            */
/* ------------------------------------------------------------------ */

#define CBX_ST_LABEL_LEN   256
#define CBX_ST_SETTING_COUNT 9  /* number of editable setting rows */

/* ------------------------------------------------------------------ */
/*  Setting identifiers                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    CBX_ST_SET_LAUNCH_BOOT = 0,   /* toggle: On/Off                    */
    CBX_ST_SET_THEME,            /* cycle: default, dark, light       */
    CBX_ST_SET_OPACITY,          /* adjust: 0.0–1.0 in 0.05 steps     */
    CBX_ST_SET_VC_COUNT,         /* adjust: 1–16                       */
    CBX_ST_SET_VC_TYPE_0,        /* cycle through known types (slot 0) */
    CBX_ST_SET_VC_TYPE_1,        /* slot 1                             */
    CBX_ST_SET_VC_TYPE_2,        /* slot 2                             */
    CBX_ST_SET_VC_TYPE_3,        /* slot 3                             */
    CBX_ST_SET_TRIGGER,         /* cycle: Select+A, Start+B, L3+R3    */
    CBX_ST_SET_SAVE,            /* Save button                        */
    CBX_ST_SET_COUNT,
} cbx_st_setting;

/* ------------------------------------------------------------------ */
/*  UI modes                                                           */
/* ------------------------------------------------------------------ */

typedef enum {
    CBX_ST_MODE_LIST = 0,         /* browse settings list               */
    CBX_ST_MODE_EDIT,            /* editing the selected setting       */
} cbx_st_mode;

/* ------------------------------------------------------------------ */
/*  Settings tab state                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    /* --- Data (owned) ---------------------------------------------- */
    cbx_settings settings;       /* current settings (loaded from disk) */
    bool          loaded;         /* settings have been loaded            */

    /* --- Widgets (owned) ------------------------------------------- */
    cbx_list   settings_list;    /* list of setting rows                 */
    cbx_button save_btn;         /* "Save Settings"                     */
    cbx_label  status_lbl;       /* status / prompt text                */

    /* --- Panel (borrowed) ------------------------------------------ */
    cbx_panel *panel;

    /* --- Rendering deps (borrowed) --------------------------------- */
    cbx_text_cache *text_cache;
    const cbx_theme *theme;
    int             font_id;

    /* --- UI state -------------------------------------------------- */
    cbx_st_mode    mode;
    int            selected;       /* index in settings_list, 0-based */
} cbx_settings_tab;

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                         */
/* ------------------------------------------------------------------ */

/*
 * Initialise the settings tab: load settings from disk, populate the
 * panel with a settings list, a save button, and a status label.
 *
 * @param tab      Output struct (overwritten).
 * @param panel    Panel to populate (borrowed, not freed by tab).
 * @param cache    Text cache (borrowed).
 * @param theme    Theme (borrowed).
 * @param font_id  Font ID from text cache, or -1 for no font.
 * @return 0 on success, negative errno on error.
 */
int cbx_settings_tab_init(cbx_settings_tab *tab,
                           cbx_panel *panel,
                           cbx_text_cache *cache,
                           const cbx_theme *theme,
                           int font_id);

/*
 * Refresh the settings list: rebuild list items from the settings struct.
 * Returns 0 on success, negative errno on error.
 */
int cbx_settings_tab_refresh(cbx_settings_tab *tab);

/*
 * Shut down and free all widget resources.  Safe on a zeroed struct.
 */
void cbx_settings_tab_shutdown(cbx_settings_tab *tab);

/* ------------------------------------------------------------------ */
/*  Actions (testable without UI events)                               */
/* ------------------------------------------------------------------ */

/*
 * Save current settings to settings.yaml (atomic write).
 * Returns 0 on success, negative errno on error.
 */
int cbx_settings_tab_save(cbx_settings_tab *tab);

/*
 * Move selection up (wraps around).
 */
int cbx_settings_tab_move_up(cbx_settings_tab *tab);

/*
 * Move selection down (wraps around).
 */
int cbx_settings_tab_move_down(cbx_settings_tab *tab);

/*
 * Activate the selected setting (enter edit mode or toggle/save).
 * Returns 0 on success, negative errno on error.
 */
int cbx_settings_tab_activate(cbx_settings_tab *tab);

/*
 * Edit the selected setting: adjust value up.
 * Only valid in edit mode.
 */
int cbx_settings_tab_edit_up(cbx_settings_tab *tab);

/*
 * Edit the selected setting: adjust value down.
 * Only valid in edit mode.
 */
int cbx_settings_tab_edit_down(cbx_settings_tab *tab);

/*
 * Confirm the edit and return to list mode.
 */
int cbx_settings_tab_confirm_edit(cbx_settings_tab *tab);

/*
 * Cancel the edit and return to list mode (revert changes for this setting).
 */
void cbx_settings_tab_cancel_edit(cbx_settings_tab *tab);

/* ------------------------------------------------------------------ */
/*  Accessors (for testing)                                            */
/* ------------------------------------------------------------------ */

const cbx_settings *cbx_settings_tab_settings(const cbx_settings_tab *tab);
cbx_st_mode         cbx_settings_tab_mode(const cbx_settings_tab *tab);
int                 cbx_settings_tab_selected(const cbx_settings_tab *tab);
const char         *cbx_settings_tab_status(const cbx_settings_tab *tab);
int                 cbx_settings_tab_setting_count(const cbx_settings_tab *tab);

/* Known theme values for cycling. */
static const char *const cbx_st_themes[] = {"default", "dark", "light", NULL};

/* Known trigger combos for cycling. */
static const char *const cbx_st_triggers[] = {"Select+A", "Start+B", "L3+R3", NULL};

#endif /* CBX_SETTINGS_TAB_H */