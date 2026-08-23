/*
 * profile_editor_list.h — Binding list mode editor (Task 37, right panel).
 *
 * The profile editor's "list mode" provides a two-panel layout:
 *   Left:  cbx_profile_diagram  — controller diagram with highlightable buttons
 *   Right: cbx_list             — scrollable binding list (source → targets)
 *
 * Navigation:
 *   Up/Down  — scroll binding list, highlight corresponding button on diagram
 *   A        — edit selected binding (enter target-pick or capture mode)
 *   B        — cancel / go back
 *
 * Modes:
 *   LIST        — browsing bindings (default)
 *   TARGET_PICK — choosing a target event from a list populated from
 *                 capabilities (DBus CompositeDevice properties + filesystem
 *                 capability maps)
 *   CAPTURE     — waiting for a physical button press (InputEvent signal)
 *
 * Diagram and binding list are always synchronised: moving the list
 * selection immediately updates the diagram highlight.
 *
 * Task 37 — Profile editor — controller diagram and binding list mode.
 */
#ifndef CBX_PROFILE_EDITOR_LIST_H
#define CBX_PROFILE_EDITOR_LIST_H

#include <SDL2/SDL.h>
#include <stdbool.h>

#include "dbus/dbus_interface.h"           /* ip_dbus_backend, ip_bus_handle */
#include "ui/widget.h"           /* cbx_panel, cbx_list, cbx_label */
#include "ui/text.h"             /* cbx_text_cache */
#include "ui/theme.h"             /* cbx_theme */
#include "config/config_profile.h"      /* cbx_profile */
#include "config/config_profile_list.h" /* cbx_file_list */
#include "dbus/ip_input_signal.h"        /* ip_input_events, ip_input_id */
#include "icons/icon_map.h"              /* cbx_icon_map (device->icon) */
#include "icons/icon_cache.h"            /* cbx_icon_cache (SVG textures) */
#include "manager/profile_diagram.h"     /* cbx_profile_diagram */

/* ------------------------------------------------------------------ */
/*  Limits                                                            */
/* ------------------------------------------------------------------ */

#define CBX_PE_MAX_TARGETS  128   /* max target entries in picker */
#define CBX_PE_LABEL_LEN   256

/* ------------------------------------------------------------------ */
/*  Editor modes                                                      */
/* ------------------------------------------------------------------ */

typedef enum {
    CBX_EDITOR_MODE_LIST = 0,      /* browsing bindings */
    CBX_EDITOR_MODE_TARGET_PICK,  /* choosing target event */
    CBX_EDITOR_MODE_CAPTURE,      /* waiting for physical button press */
    CBX_EDITOR_MODE_SEQUENTIAL,   /* sequential binding mode (Task 38) */
    CBX_EDITOR_MODE_BINDING_EDIT, /* choosing action: target-pick / capture / sequential */
} cbx_editor_mode;

/* ------------------------------------------------------------------ */
/*  Target entry (for target picker list)                             */
/* ------------------------------------------------------------------ */

typedef struct {
    char device_class[64];   /* e.g. "keyboard", "mouse", "gamepad" */
    char value[128];         /* e.g. "KeyEsc", "ButtonLeft" */
    char label[CBX_PE_LABEL_LEN];  /* display label */
} cbx_pe_target;

/* ------------------------------------------------------------------ */
/*  Profile editor state                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    /* --- Profile data (owned) ------------------------------------- */
    cbx_profile profile;            /* profile being edited */
    bool         profile_loaded;

    /* --- Widgets (owned) ------------------------------------------ */
    cbx_profile_diagram diagram;    /* left panel: controller diagram */
    cbx_list   binding_list;        /* right panel: source → targets list */
    cbx_list   target_list;         /* target picker (hidden in LIST mode) */
    cbx_label  status_lbl;          /* status / prompt text */
    cbx_label  title_lbl;           /* profile name title */

    /* --- Panel (borrowed) ----------------------------------------- */
    cbx_panel *panel;

    /* --- Rendering deps (borrowed) -------------------------------- */
    cbx_text_cache *text_cache;
    const cbx_theme *theme;
    int             font_id;
    SDL_Renderer   *renderer;

    /* --- Device-mapped diagram resolution (BUG-0018) -------------- */
    /* Production icon mapping utilities: cbx_icon_map + cbx_icon_cache
     * resolve the diagram's base SVG and its marker layout by device type
     * instead of a hardcoded generic-gamepad path.  Owned by the editor. */
    cbx_icon_map      icon_map;
    cbx_icon_cache    icon_cache;
    char              device_type[CBX_ICON_TYPE_LEN]; /* resolved DeviceType */

    /* --- DBus deps (borrowed, optional) --------------------------- */
    const ip_dbus_backend *backend;
    ip_bus_handle          bus;
    char                   composite_path[512];

    /* --- Target capabilities ------------------------------------- */
    cbx_pe_target targets[CBX_PE_MAX_TARGETS];
    int            target_count;

    /* --- Filesystem capability maps (optional) -------------------- */
    cbx_file_list cap_maps;

    /* --- UI state ------------------------------------------------- */
    cbx_editor_mode mode;
    int             selected_index;  /* selected binding in list, -1 = none */
    int             editing_index;  /* binding being edited, -1 = none */

    /* --- Capture mode state -------------------------------------- */
    ip_input_events input_events;
    bool            capture_active;
    cbx_diag_button captured_button;

    /* --- Sequential binding mode state (Task 38) ------------------ */
    cbx_progress progress_bar;     /* completion progress bar */
    int           seq_step;        /* current button index in sequence */
    bool          seq_active;      /* sequential mode in progress */

    /* --- Expected sender for InputEvent verification (Task 5) ----- */
    char          expected_sender[128]; /* unique bus name (e.g. ":1.42"), not well-known */

    /* --- Dirty flag: true when the profile has unsaved edits ------- */
    bool          dirty;
} cbx_profile_editor;

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                         */
/* ------------------------------------------------------------------ */

/*
 * Initialise the profile editor: create the diagram, binding list,
 * target picker, and labels, and add them to the panel.
 *
 * @param ed       Output struct (overwritten).
 * @param panel    Panel to populate (borrowed).
 * @param renderer SDL renderer for SVG texture creation (borrowed).
 * @param cache    Text cache (borrowed).
 * @param theme    Theme (borrowed).
 * @param font_id  Font ID from text cache, or -1 for no font.
 * @return 0 on success, negative errno on error.
 */
int cbx_profile_editor_init(cbx_profile_editor *ed,
                              cbx_panel *panel,
                              SDL_Renderer *renderer,
                              cbx_text_cache *cache,
                              const cbx_theme *theme,
                              int font_id);

/*
 * Set the device type whose controller diagram the editor should show.
 * Re-resolves the diagram base SVG + marker layout through the production
 * icon mapping utilities (cbx_icon_map / cbx_icon_cache).  A NULL/empty
 * device type resolves to the default generic-gamepad device.  Returns 0.
 */
int cbx_profile_editor_set_device(cbx_profile_editor *ed,
                                   const char *device_type);

/*
 * Shut down and free all resources.  Safe on a zeroed struct.
 */
void cbx_profile_editor_shutdown(cbx_profile_editor *ed);

/* ------------------------------------------------------------------ */
/*  Profile loading                                                    */
/* ------------------------------------------------------------------ */

/*
 * Load a profile into the editor.  Copies the profile struct and
 * refreshes the binding list.  Any previous edits are discarded.
 *
 * @return 0 on success, negative errno on error.
 */
int cbx_profile_editor_load_profile(cbx_profile_editor *ed,
                                       const cbx_profile *profile);

/*
 * Get the current profile being edited (read-only accessor).
 * Returns NULL if no profile is loaded.
 */
const cbx_profile *cbx_profile_editor_get_profile(
    const cbx_profile_editor *ed);

/* ------------------------------------------------------------------ */
/*  DBus / capabilities                                                */
/* ------------------------------------------------------------------ */

/*
 * Set DBus connection info for reading capabilities.
 * After calling this, load_capabilities() can query the composite
 * device's Capabilities / OutputCapabilities / TargetCapabilities.
 */
void cbx_profile_editor_set_dbus(cbx_profile_editor *ed,
                                    const ip_dbus_backend *backend,
                                    ip_bus_handle bus,
                                    const char *composite_path);

/*
 * Load target capabilities from DBus (if backend is set) and from
 * filesystem capability maps.  Populates the targets[] array.
 * Returns 0 on success, negative errno on error.
 */
int cbx_profile_editor_load_capabilities(cbx_profile_editor *ed);

/* ------------------------------------------------------------------ */
/*  Refresh: rebuild binding list from profile                        */
/* ------------------------------------------------------------------ */

/*
 * Rebuild the binding list from the loaded profile's mappings.
 * Each list item shows "source → target" text.  Resets selection to 0
 * (or -1 if no mappings).  Updates the diagram highlight.
 *
 * @return 0 on success, negative errno on error.
 */
int cbx_profile_editor_refresh(cbx_profile_editor *ed);

/* ------------------------------------------------------------------ */
/*  Navigation                                                         */
/* ------------------------------------------------------------------ */

/* Move selection up.  Returns new index, or -1 if no bindings. */
int cbx_profile_editor_move_up(cbx_profile_editor *ed);

/* Move selection down.  Returns new index, or -1 if no bindings. */
int cbx_profile_editor_move_down(cbx_profile_editor *ed);

/*
 * Activate (A button): enter target-pick mode for the selected binding.
 * Returns 0 on success, -EINVAL if no selection, -ENODATA if no targets.
 */
int cbx_profile_editor_activate(cbx_profile_editor *ed);

/*
 * Cancel (B button): cancels target-pick or capture mode, returns to
 * list mode.  In list mode, does nothing (returns -ENOENT).
 */
int cbx_profile_editor_cancel(cbx_profile_editor *ed);

/* ------------------------------------------------------------------ */
/*  Target pick mode                                                   */
/* ------------------------------------------------------------------ */

/*
 * Enter target-pick mode for the selected binding.
 * Populates the target list from loaded capabilities and switches mode.
 * Returns 0 on success, negative errno on error.
 */
int cbx_profile_editor_begin_target_pick(cbx_profile_editor *ed);

/*
 * Confirm the target pick: applies the selected target to the binding
 * being edited, then returns to list mode.
 * Returns 0 on success, negative errno on error.
 */
int cbx_profile_editor_confirm_target_pick(cbx_profile_editor *ed);

/* Cancel target pick and return to list mode. */
void cbx_profile_editor_cancel_target_pick(cbx_profile_editor *ed);

/* ------------------------------------------------------------------ */
/*  Capture mode                                                       */
/* ------------------------------------------------------------------ */

/*
 * Enter capture mode for the selected binding.
 * Subscribes to InputEvent signals; the next button press sets the
 * source event for the binding.  Returns 0 on success, negative errno.
 */
int cbx_profile_editor_begin_capture(cbx_profile_editor *ed);

/* Cancel capture mode and return to list mode. */
void cbx_profile_editor_cancel_capture(cbx_profile_editor *ed);

/*
 * InputEvent callback — called when a physical button is pressed during
 * capture mode.  Sets the source event for the editing binding and
 * returns to list mode.
 */
void cbx_profile_editor_on_input_event(ip_input_id input,
                                         ip_input_category category,
                                         double value,
                                         const char *raw_event,
                                         const char *device_path,
                                         void *userdata);

/* ------------------------------------------------------------------ */
/*  Accessors (for testing)                                             */
/* ------------------------------------------------------------------ */

cbx_editor_mode cbx_profile_editor_get_mode(const cbx_profile_editor *ed);
int             cbx_profile_editor_binding_count(const cbx_profile_editor *ed);
int             cbx_profile_editor_get_selected(const cbx_profile_editor *ed);
cbx_diag_button cbx_profile_editor_get_diagram_highlight(
    const cbx_profile_editor *ed);
int             cbx_profile_editor_get_target_count(
    const cbx_profile_editor *ed);
const char     *cbx_profile_editor_get_status(const cbx_profile_editor *ed);
int             cbx_profile_editor_get_editing_index(
    const cbx_profile_editor *ed);
bool            cbx_profile_editor_is_capture_active(
    const cbx_profile_editor *ed);

/* Check if the profile has unsaved edits (dirty flag). */
bool            cbx_profile_editor_is_dirty(const cbx_profile_editor *ed);

/* ------------------------------------------------------------------ */
/*  Sequential binding mode (Task 38)                                */
/* ------------------------------------------------------------------ */

/*
 * Begin sequential binding mode: the editor prompts for each button
 * in order (UP, DOWN, LEFT, RIGHT, A, B, X, Y, START, SELECT, GUIDE,
 * L1, R1, L2, R2, L3, R3).  The diagram lights up the current button.
 * Press a physical button -> captured -> auto-advance.  B skips,
 * Start cancels.
 *
 * Returns 0 on success, negative errno on error.
 */
int cbx_profile_editor_begin_sequential(cbx_profile_editor *ed);

/* Cancel sequential mode and return to list mode. */
void cbx_profile_editor_cancel_sequential(cbx_profile_editor *ed);

/*
 * Skip the current button in sequential mode (B button action).
 * Returns 0 on success, -ENOENT if not in sequential mode.
 */
int cbx_profile_editor_seq_skip(cbx_profile_editor *ed);

/*
 * InputEvent handler for sequential mode — called when a physical
 * button is pressed.  Captures the button, advances to the next step.
 */
void cbx_profile_editor_seq_on_input(ip_input_id input,
                                        ip_input_category category,
                                        double value,
                                        const char *raw_event,
                                        const char *device_path,
                                        void *userdata);

/* Get sequential mode progress (0.0 to 1.0). */
double cbx_profile_editor_seq_progress(const cbx_profile_editor *ed);

/* Get the current button being prompted in sequential mode. */
cbx_diag_button cbx_profile_editor_seq_current_button(
    const cbx_profile_editor *ed);

/* Get the current step index (0-based) in sequential mode. */
int cbx_profile_editor_seq_get_step(const cbx_profile_editor *ed);

/* Check if sequential mode is active. */
bool cbx_profile_editor_seq_is_active(const cbx_profile_editor *ed);

#endif /* CBX_PROFILE_EDITOR_LIST_H */