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

/* The settings list is dynamic: four fixed leading rows (launch, theme,
 * opacity, virtual-controller count), one type selector row for every
 * configured slot (1..CBX_MAX_CONTROLLERS), then three trailing rows
 * (trigger, icon override, save).  The enum values below name the logical
 * kinds; only for the default 4-slot layout do they also equal the row
 * index.  Production code must map rows through the helpers below. */
#define CBX_ST_BASE_ROWS       4
#define CBX_ST_TRAILING_ROWS   3

/* ------------------------------------------------------------------ */
/*  Setting identifiers                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    CBX_ST_SET_LAUNCH_BOOT = 0,   /* toggle: On/Off                    */
    CBX_ST_SET_THEME,            /* display: implemented theme name   */
    CBX_ST_SET_OPACITY,          /* adjust: 0.0–1.0 in 0.05 steps     */
    CBX_ST_SET_VC_COUNT,         /* adjust: 1–16                       */
    CBX_ST_SET_VC_TYPE_0,        /* type-selector row (slot carried)  */
    CBX_ST_SET_VC_TYPE_1,        /* legacy alias (slot 1)             */
    CBX_ST_SET_VC_TYPE_2,        /* legacy alias (slot 2)             */
    CBX_ST_SET_VC_TYPE_3,        /* legacy alias (slot 3)             */
    CBX_ST_SET_TRIGGER,         /* cycle: Select+A, Start+B, L3+R3    */
    CBX_ST_SET_ICON_OVERRIDE,   /* cycle: preset icon overrides (§8.4) */
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
    cbx_settings settings;       /* current working settings             */
    bool          loaded;         /* settings have been loaded            */
    /* Optional authoritative settings owned by the Manager.  When set, the
     * working copy above is edited locally and only published on save, so
     * Controllers-topology changes made on the other tab are never
     * overwritten with a stale copy.  Borrowed, never freed. */
    cbx_settings *external;

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
    int            icon_preset_idx; /* current preset index for icon override edit */
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
/*
 * Reposition all tab widgets relative to the current panel rect.
 * Called during init and on window resize (SPEC §5.1).
 */
void cbx_settings_tab_layout(cbx_settings_tab *tab);

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

/*
 * Tab-level key handler (called by manager before dispatching to the
 * focused widget).  In edit mode, intercepts Up/Down for value adjustment
 * and B for cancel.  Returns true if handled.
 */
bool cbx_settings_tab_handle_key(cbx_settings_tab *tab, const SDL_Event *ev);

/* ------------------------------------------------------------------ */
/*  Accessors (for testing)                                            */
/* ------------------------------------------------------------------ */

const cbx_settings *cbx_settings_tab_settings(const cbx_settings_tab *tab);
cbx_st_mode         cbx_settings_tab_mode(const cbx_settings_tab *tab);
int                 cbx_settings_tab_selected(const cbx_settings_tab *tab);
const char         *cbx_settings_tab_status(const cbx_settings_tab *tab);
int                 cbx_settings_tab_setting_count(const cbx_settings_tab *tab);

/*
 * Dynamic row mapping.  A settings-list row is one of:
 *   [0] launch at boot
 *   [1] theme
 *   [2] overlay opacity
 *   [3] virtual-controller count
 *   [4 .. 3+count]  one type selector per configured slot
 *   [4+count]       overlay trigger
 *   [5+count]       icon override
 *   [6+count]       save
 *
 * The helpers below map between row indices and logical settings so callers
 * never assume the default 4-slot layout.  `type_slot` returns -1 when the
 * row is not a configured type row.
 */
int  cbx_settings_tab_type_row_count(const cbx_settings_tab *tab);
int  cbx_settings_tab_row_for_type(const cbx_settings_tab *tab, int slot);
int  cbx_settings_tab_trigger_row(const cbx_settings_tab *tab);
int  cbx_settings_tab_icon_override_row(const cbx_settings_tab *tab);
int  cbx_settings_tab_save_row(const cbx_settings_tab *tab);
int  cbx_settings_tab_type_slot(const cbx_settings_tab *tab, int row);

/* True when the row is an enabled, adjustable setting.  The theme row is
 * non-editable while only the built-in default theme is implemented
 * (SPEC §5.5, §13): presenting an inert cycle would be dishonest. */
bool cbx_settings_tab_row_editable(const cbx_settings_tab *tab, int row);

/*
 * Bind the tab to the Manager's authoritative settings struct.  The working
 * copy is refreshed from it immediately.  Passing NULL unbinds.
 */
int cbx_settings_tab_set_external(cbx_settings_tab *tab,
                                   cbx_settings *external);

/*
 * Refresh the working copy from the authoritative struct (or disk when
 * unbound) and rebuild the list.  Does not discard an edit in progress.
 */
int cbx_settings_tab_sync(cbx_settings_tab *tab);

/* Known, actually-implemented theme values (SPEC §13).  Only "default" has
 * a palette in cbx_theme_load(); do not list unimplemented names. */
static const char *const cbx_st_themes[] = {"default", NULL};

/* Known trigger combos for cycling. */
static const char *const cbx_st_triggers[] = {"Select+A", "Start+B", "L3+R3", NULL};

#endif /* CBX_SETTINGS_TAB_H */