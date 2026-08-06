/*
 * host_mode.h — Host Mode state machine for the overlay.
 *
 * Task 30 — Host Mode and conflict detection/resolution.
 *
 * When a controller presses R3, it becomes the exclusive host. All other
 * controllers freeze. The host can navigate to any row (Up/Down) and edit
 * the slot within that row (Left/Right). R3 again exits back to Player Mode.
 */
#ifndef CBX_OVERLAY_HOST_MODE_H
#define CBX_OVERLAY_HOST_MODE_H

#include <stdbool.h>
#include "overlay/grid_render.h"

/* --- Result codes ----------------------------------------------------- */

#define CBX_HM_RESULT_NONE    0   /* no action taken (boundary, frozen) */
#define CBX_HM_RESULT_MOVED   1   /* selected row changed (Up/Down)     */
#define CBX_HM_RESULT_SLOT    2   /* slot changed within selected row   */
#define CBX_HM_RESULT_EXIT    3   /* R3 pressed — exiting host mode     */
#define CBX_HM_RESULT_CLOSE   4   /* B pressed — close overlay         */
#define CBX_HM_RESULT_FROZEN  5   /* non-host controller input (ignored) */
#define CBX_HM_RESULT_ERROR   (-1)

/* --- Input enum (mirrors player_mode) --------------------------------- */

typedef enum {
    CBX_HM_LEFT,
    CBX_HM_RIGHT,
    CBX_HM_UP,
    CBX_HM_DOWN,
    CBX_HM_B,
    CBX_HM_R3,
} cbx_hm_input;

/* --- Visual state for rendering -------------------------------------- */

typedef enum {
    CBX_ROW_NORMAL   = 0,  /* normal row (player mode, or not host mode) */
    CBX_ROW_HOST     = 1,  /* host controller's own row                   */
    CBX_ROW_SELECTED = 2,  /* row the host is currently editing           */
    CBX_ROW_FROZEN   = 3,  /* frozen row (not host, not selected)          */
} cbx_row_visual_state;

/* --- Callbacks -------------------------------------------------------- */

typedef int (*cbx_hm_slot_change_cb)(int row_idx, int new_slot, void *userdata);

/* --- Host Mode state -------------------------------------------------- */

typedef struct {
    bool active;
    int  host_row;       /* row_idx of the host controller      */
    int  selected_row;   /* row the host is currently editing    */

    cbx_hm_slot_change_cb on_slot_change;
    void                 *slot_change_data;
} cbx_host_mode;

/* --- API -------------------------------------------------------------- */

void cbx_host_mode_init(cbx_host_mode *hm);

/*
 * Enter host mode. The given row_idx becomes the host.
 * selected_row is initialized to host_row.
 * Returns 0, -EINVAL.
 */
int cbx_host_mode_enter(cbx_host_mode *hm, int row_idx);

/*
 * Exit host mode. Resets to inactive.
 * Returns 0, -EINVAL.
 */
int cbx_host_mode_exit(cbx_host_mode *hm);

/*
 * Toggle host mode. If inactive, enters with row_idx as host.
 * If active and row_idx == host_row, exits. If active and row_idx
 * != host_row, the input is from a frozen controller (ignored).
 *
 * Returns:
 *   1  = entered host mode
 *   0  = exited host mode
 *  -1  = frozen controller (ignored)
 *  -2  = invalid args
 */
int cbx_host_mode_toggle(cbx_host_mode *hm, int row_idx);

/*
 * Handle an input in host mode. Only the host controller can act.
 * Non-host controllers get CBX_HM_RESULT_FROZEN.
 *
 * Up/Down: change selected_row (navigate between rows, clamped)
 * Left/Right: move within selected row's columns
 * R3: exit host mode
 * B: close overlay
 *
 * on_slot_change is fired when Left/Right changes the selected row's
 * column, with the selected_row's row_idx and new slot.
 *
 * Returns CBX_HM_RESULT_* code.
 */
int cbx_host_mode_handle(cbx_host_mode *hm, int row_idx,
                         cbx_hm_input input, cbx_select_grid *grid);

/* --- Accessors -------------------------------------------------------- */

bool cbx_host_mode_is_active(const cbx_host_mode *hm);
int  cbx_host_mode_get_host_row(const cbx_host_mode *hm);
int  cbx_host_mode_get_selected_row(const cbx_host_mode *hm);

/*
 * Get the visual state of a row for rendering.
 * When host mode is not active, all rows are CBX_ROW_NORMAL.
 * When active:
 *   - The selected row is CBX_ROW_SELECTED
 *   - The host's own row (if different from selected) is CBX_ROW_HOST
 *   - All other rows are CBX_ROW_FROZEN
 */
cbx_row_visual_state cbx_host_mode_row_state(const cbx_host_mode *hm,
                                              int row_idx);

/*
 * Check if a controller (row_idx) is frozen (cannot act).
 * True when host mode is active and row_idx != host_row.
 */
bool cbx_host_mode_is_frozen(const cbx_host_mode *hm, int row_idx);

#endif /* CBX_OVERLAY_HOST_MODE_H */