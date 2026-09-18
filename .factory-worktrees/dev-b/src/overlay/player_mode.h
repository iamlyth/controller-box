/*
 * player_mode.h — Player Mode navigation for the select grid.
 *
 * Task 29 — Character select grid rendering and Player Mode navigation.
 *
 * Player Mode (SPEC §4.3) is the default overlay mode.  All controllers
 * edit simultaneously, like a fighting-game character select.  Each
 * controller moves its own row independently:
 *   - Left / Right: move across columns (slot position)
 *   - Up / Down: cycle through available profiles
 *   - B: close overlay
 *   - R3: enter/exit Host Mode (handled by Task 30)
 *
 * No controller can affect another's row in Player Mode.
 *
 * Side effects (assignment update, LoadProfilePath) are handled via
 * callbacks, keeping this module pure and testable without DBus or
 * file I/O.
 */
#ifndef CBX_OVERLAY_PLAYER_MODE_H
#define CBX_OVERLAY_PLAYER_MODE_H

#include <stdbool.h>

#include "overlay/grid_render.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Input types -------------------------------------------------------- */

typedef enum {
    CBX_PM_LEFT,       /* move across columns (slot) */
    CBX_PM_RIGHT,
    CBX_PM_UP,         /* cycle profile up */
    CBX_PM_DOWN,       /* cycle profile down */
    CBX_PM_B,          /* close overlay */
    CBX_PM_R3,         /* toggle host mode (Task 30) */
} cbx_pm_input;

/* --- Result codes -------------------------------------------------------- */

#define CBX_PM_RESULT_NONE      0  /* no action taken */
#define CBX_PM_RESULT_MOVED     1  /* column position changed */
#define CBX_PM_RESULT_PROFILE   2  /* profile changed */
#define CBX_PM_RESULT_CLOSE     3  /* B pressed -> close overlay */
#define CBX_PM_RESULT_HOST      4  /* R3 pressed -> enter host mode */
#define CBX_PM_RESULT_ERROR   (-1) /* invalid args */

/* --- Callbacks ---------------------------------------------------------- */

/* Called when a controller's slot changes.
 * new_slot: 0-based (0=P1, 1=P2, ...), or -1 for Unassigned.
 * Return 0 on success, negative errno on error. */
typedef int (*cbx_pm_slot_change_cb)(int row_idx, int new_slot,
                                      void *userdata);

/* Called when a controller's profile changes.
 * profile: new profile filename (without .yaml extension)
 * composite_path: DBus path for LoadProfilePath
 * Return 0 on success, negative errno on error. */
typedef int (*cbx_pm_profile_change_cb)(int row_idx, const char *profile,
                                         const char *composite_path,
                                         void *userdata);

/* --- Player Mode state --------------------------------------------------- */

typedef struct {
    cbx_select_grid *grid;

    /* Callbacks (all optional — NULL = skipped). */
    cbx_pm_slot_change_cb    on_slot_change;
    void                    *slot_change_data;
    cbx_pm_profile_change_cb on_profile_change;
    void                    *profile_change_data;
} cbx_player_mode;

/* --- API ---------------------------------------------------------------- */

void cbx_player_mode_init(cbx_player_mode *pm, cbx_select_grid *grid);

/* Handle input for a specific controller (row_idx).
 *
 * In Player Mode, each controller only affects its own row.
 * Navigation:
 *   LEFT/RIGHT → move across columns, fire on_slot_change
 *   UP/DOWN   → cycle profile, fire on_profile_change
 *   B         → return CBX_PM_RESULT_CLOSE
 *   R3        → return CBX_PM_RESULT_HOST (Task 30 handles transition)
 *
 * Returns a CBX_PM_RESULT_* code. */
int cbx_player_mode_handle(cbx_player_mode *pm, int row_idx,
                            cbx_pm_input input);

/* Get the current slot for a controller (0=P1, -1=unassigned). */
int cbx_player_mode_get_slot(const cbx_player_mode *pm, int row_idx);

/* Get the current profile name for a controller.  Returns NULL on bad args. */
const char *cbx_player_mode_get_profile(const cbx_player_mode *pm,
                                          int row_idx);

#ifdef __cplusplus
}
#endif

#endif /* CBX_OVERLAY_PLAYER_MODE_H */