/*
 * profiles_tab.h — Profiles tab for the Manager UI (SPEC §5.3).
 *
 * Lists profiles from filesystem enumeration (Task 8), sorted, with icons.
 * The default profile is shown as read-only.  Users can create new
 * profiles (from Default copy, Empty, or Clone existing) and delete
 * user-created profiles.  Profile editing is handled by Task 37/38.
 *
 * Filesystem operations:
 *   Create  → write <user_profiles_dir>/<name>.yaml (+ optional sidecar)
 *   Delete  → unlink <user_profiles_dir>/<name>.yaml (+ sidecar if present)
 *
 * Task 36 — Profiles tab.
 */
#ifndef CBX_PROFILES_TAB_H
#define CBX_PROFILES_TAB_H

#include <SDL2/SDL.h>
#include <stdbool.h>

#include "ui/widget.h"             /* cbx_panel, cbx_list, cbx_button */
#include "ui/text.h"               /* cbx_text_cache */
#include "ui/theme.h"             /* cbx_theme */
#include "config/config_profile_list.h" /* cbx_profile_list, cbx_profile_entry */

/* ------------------------------------------------------------------ */
/*  Limits                                                            */
/* ------------------------------------------------------------------ */

#define CBX_PT_LABEL_LEN   256
#define CBX_PT_NAME_LEN    128

/* ------------------------------------------------------------------ */
/*  Create sources                                                     */
/* ------------------------------------------------------------------ */

typedef enum {
    CBX_PT_CREATE_DEFAULT_COPY = 0, /* copy the default profile's mappings */
    CBX_PT_CREATE_EMPTY,           /* blank profile (just header)        */
    CBX_PT_CREATE_CLONE,           /* clone a selected existing profile   */
} cbx_pt_create_source;

/* ------------------------------------------------------------------ */
/*  UI modes                                                           */
/* ------------------------------------------------------------------ */

typedef enum {
    CBX_PT_MODE_LIST = 0,          /* browse profile list                */
    CBX_PT_MODE_CONFIRM_DELETE,    /* "Delete <name>?  A=Yes B=No"       */
    CBX_PT_MODE_NAME_INPUT,       /* entering a new profile name         */
    CBX_PT_MODE_CREATE_PICK,      /* pick create source (default/empty/clone) */
} cbx_pt_mode;

/* ------------------------------------------------------------------ */
/*  Profiles tab state                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    /* --- Data (owned) ---------------------------------------------- */
    cbx_profile_list profiles;     /* enumerated profiles                */

    /* --- Widgets (owned) ------------------------------------------- */
    cbx_list   profile_list_w;     /* list of profiles (browse)         */
    cbx_list   create_picker;      /* picker for create source           */
    cbx_button create_btn;        /* "Create Profile"                  */
    cbx_button delete_btn;        /* "Delete Profile"                  */
    cbx_button edit_btn;          /* "Edit Profile" (opens editor)      */
    cbx_label  status_lbl;        /* status / prompt text                */

    /* --- Panel (borrowed) ------------------------------------------ */
    cbx_panel *panel;

    /* --- Rendering deps (borrowed) --------------------------------- */
    cbx_text_cache *text_cache;
    const cbx_theme *theme;
    int             font_id;

    /* --- UI state -------------------------------------------------- */
    cbx_pt_mode mode;
    int         selected_profile;   /* index in profiles.entries, -1 */

    /* --- Name input (for create) ---------------------------------- */
    char        name_buf[CBX_PT_NAME_LEN];
    int         name_len;
    cbx_pt_create_source create_source;

    /* --- Pending delete target ----------------------------------- */
    int         delete_target;      /* index to delete, -1 = none      */

    /* --- Test override dirs (NULL = use default paths) ------------- */
    const char *test_user_dir;      /* override user profiles dir       */
    const char *test_system_dir;    /* override system profiles dir     */
    const char *test_meta_dir;      /* override sidecar metadata dir     */
} cbx_profiles_tab;

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                         */
/* ------------------------------------------------------------------ */

/*
 * Initialise the profiles tab: populate the panel with a profile list,
 * Create / Edit / Delete buttons, and a status label.  Calls refresh to
 * enumerate profiles from the filesystem.
 *
 * @param tab      Output struct (overwritten).
 * @param panel    Panel to populate (borrowed, not freed by tab).
 * @param cache    Text cache (borrowed).
 * @param theme    Theme (borrowed).
 * @param font_id  Font ID from text cache, or -1 for no font.
 * @return 0 on success, negative errno on error.
 */
int cbx_profiles_tab_init(cbx_profiles_tab *tab,
                           cbx_panel *panel,
                           cbx_text_cache *cache,
                           const cbx_theme *theme,
                           int font_id);

/*
 * Set test override directories.  When set, refresh and file operations
 * use these paths instead of the default system paths.  Pass NULL for
 * all three to revert to default paths.
 *
 * @param tab        Profiles tab.
 * @param user_dir   Override for user profiles dir (NULL = default).
 * @param system_dir Override for system profiles dir (NULL = default).
 * @param meta_dir   Override for sidecar metadata dir (NULL = default).
 */
void cbx_profiles_tab_set_test_dirs(cbx_profiles_tab *tab,
                                       const char *user_dir,
                                       const char *system_dir,
                                       const char *meta_dir);

/*
 * Refresh the profile list: re-enumerate from filesystem and rebuild
 * the list widget.  Preserves selection if possible.
 * Returns 0 on success, negative errno on error.
 */
int cbx_profiles_tab_refresh(cbx_profiles_tab *tab);

/*
 * Shut down and free all widget resources.  Safe on a zeroed struct.
 */
void cbx_profiles_tab_shutdown(cbx_profiles_tab *tab);

/* ------------------------------------------------------------------ */
/*  Actions (testable without UI events)                               */
/* ------------------------------------------------------------------ */

/*
 * Create a new profile with the given name.
 *
 * @param tab     Profiles tab.
 * @param name    Profile name (validated against ^[a-zA-Z0-9_-]+$).
 * @param source  What to start from: copy default, empty, or clone selected.
 * @return 0 on success, negative errno on error.
 *   -EINVAL: invalid name or read-only clone target.
 *   -EEXIST: profile already exists.
 *   -ENOENT: default profile not found (for CREATE_DEFAULT_COPY).
 */
int cbx_profiles_tab_create(cbx_profiles_tab *tab,
                              const char *name,
                              cbx_pt_create_source source);

/*
 * Delete the profile at the given index.
 * Validates that the target is not the default profile and not a
 * system profile (read_only).  Removes the profile YAML and its sidecar.
 *
 * @return 0 on success, negative errno on error.
 *   -EINVAL: bad index or read-only profile.
 */
int cbx_profiles_tab_delete(cbx_profiles_tab *tab, int profile_index);

/*
 * Enter name-input mode for creating a new profile.
 * Resets the name buffer and sets the create source.
 */
int cbx_profiles_tab_begin_create(cbx_profiles_tab *tab,
                                    cbx_pt_create_source source);

/*
 * Append a character to the name input buffer.
 * Returns 0 on success, -ENOSPC if buffer is full, -EINVAL if not in
 * name-input mode.
 */
int cbx_profiles_tab_name_input_char(cbx_profiles_tab *tab, char ch);

/*
 * Backspace one character from the name input buffer.
 */
int cbx_profiles_tab_name_input_backspace(cbx_profiles_tab *tab);

/*
 * Confirm the name input and create the profile.
 * Returns 0 on success, negative errno on error.
 */
int cbx_profiles_tab_name_input_confirm(cbx_profiles_tab *tab);

/*
 * Cancel name input mode and return to list view.
 */
void cbx_profiles_tab_name_input_cancel(cbx_profiles_tab *tab);

/*
 * Enter delete-confirmation mode for the selected profile.
 * Sets the status label to a confirmation prompt.
 */
int cbx_profiles_tab_begin_delete(cbx_profiles_tab *tab, int profile_index);

/*
 * Confirm the pending delete.
 * Returns 0 on success, negative errno on error.
 */
int cbx_profiles_tab_confirm_delete(cbx_profiles_tab *tab);

/*
 * Cancel the pending delete and return to list view.
 */
void cbx_profiles_tab_cancel_delete(cbx_profiles_tab *tab);

/*
 * Get the profile entry at the given index (read-only accessor).
 * Returns NULL if out of bounds.
 */
const cbx_profile_entry *
cbx_profiles_tab_entry(const cbx_profiles_tab *tab, int index);

/*
 * Open the create source picker (Default copy / Empty / Clone).
 * Populates the create_picker list and switches to CBX_PT_MODE_CREATE_PICK.
 */
int cbx_profiles_tab_begin_create_pick(cbx_profiles_tab *tab);

/*
 * Cancel the create source picker and return to list mode.
 */
void cbx_profiles_tab_cancel_create_pick(cbx_profiles_tab *tab);

/*
 * Tab-level activation (called by manager when A-key KEYUP is not
 * consumed by the focused widget).  Checks the tab’s mode and dispatches
 * (name input confirm, delete confirm, create source confirm).
 * Returns 0 on success, negative errno on error.
 */
int cbx_profiles_tab_activate(cbx_profiles_tab *tab);

/*
 * Tab-level cancel (called by manager when B-key is pressed in a modal mode).
 * Returns true if the cancel was handled (mode was modal), false otherwise.
 */
bool cbx_profiles_tab_cancel(cbx_profiles_tab *tab);

/*
 * Tab-level key handler (called by manager before dispatching to the
 * focused widget).  Handles mode-specific keys (letter keys in name
 * input, A/B in confirm delete, B in create pick).  Returns true if handled.
 */
bool cbx_profiles_tab_handle_key(cbx_profiles_tab *tab, const SDL_Event *ev);

/* ------------------------------------------------------------------ */
/*  Accessors (for testing)                                            */
/* ------------------------------------------------------------------ */

int               cbx_profiles_tab_profile_count(const cbx_profiles_tab *tab);
int               cbx_profiles_tab_selected(const cbx_profiles_tab *tab);
cbx_pt_mode        cbx_profiles_tab_mode(const cbx_profiles_tab *tab);
const char       *cbx_profiles_tab_name_buffer(const cbx_profiles_tab *tab);
const cbx_profile_list *cbx_profiles_tab_list(const cbx_profiles_tab *tab);

#endif /* CBX_PROFILES_TAB_H */