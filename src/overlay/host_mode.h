/*
 * host_mode.h — Host Mode state machine for the overlay.
 *
 * Task 30 — Host Mode and conflict detection/resolution.
 *
 * When a controller presses R3, it becomes the exclusive host. All other
 * controllers freeze. The host can navigate to any row (Up/Down), edit the
 * slot within that row (Left/Right), and cycle the selected row's profile
 * (L1 = previous profile, R1 = next profile). R3 again exits back to Player
 * Mode.
 *
 * Host Mode editing affordance (SPEC §4.4; interior UX deferred by §13):
 *   - Up / Down    : move the selected row
 *   - Left / Right : move the selected row across columns (slot)
 *   - L1 / R1      : cycle the selected row's profile (prev / next)
 *   - R3           : exit Host Mode
 *   - B            : close the overlay
 * The host may apply slot/profile edits to ANY row, not just its own
 * (SPEC §4.4). Non-host controllers are frozen: every input they send is
 * rejected with CBX_HM_RESULT_FROZEN and can never mutate another row.
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
#define CBX_HM_RESULT_PROFILE 6   /* selected row's profile changed     */
#define CBX_HM_RESULT_ERROR   (-1)

/* --- Input enum (mirrors player_mode) --------------------------------- */

typedef enum {
    CBX_HM_LEFT,
    CBX_HM_RIGHT,
    CBX_HM_UP,
    CBX_HM_DOWN,
    CBX_HM_PROFILE_PREV,   /* L1 — previous profile for selected row */
    CBX_HM_PROFILE_NEXT,   /* R1 — next profile for selected row     */
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

/*
 * Fired when the host cycles the selected row's profile.  The new profile
 * name (filename without .yaml) and the row's composite DBus path are
 * passed so a consumer can load the profile via LoadProfilePath and update
 * the persisted assignment — mirroring cbx_pm_profile_change_cb for Player
 * Mode.  The row is the selected row, which may not be the host's own row
 * (SPEC §4.4).
 */
typedef int (*cbx_hm_profile_change_cb)(int row_idx, const char *profile,
                                        const char *composite_path,
                                        void *userdata);

/*
 * Fired whenever host mode transitions between inactive and active (and
 * back).  The new `active` state is passed so a consumer can, for example,
 * mark the overlay surface dirty: entering/exiting host mode materially
 * changes the rendered row visuals (HOST/SELECTED/FROZEN vs. normal Player
 * Mode — SPEC §4.4/§4.9), so the pre-built surface must be re-rendered.
 */
typedef int (*cbx_hm_state_change_cb)(bool active, void *userdata);

/* --- Host Mode state -------------------------------------------------- */

struct cbx_host_mode {
    bool active;
    int  host_row;       /* row_idx of the host controller      */
    int  selected_row;   /* row the host is currently editing    */

    /* Identity of the host and selected rows, recorded when host mode is
     * entered and refreshed as the selection moves.  For a row with a
     * stable source-derived physical identity (BT:/USB:/USB:phys:) the id
     * buffer holds that id and the path buffer is empty; for a degraded row
     * (ORDER:n fallback) the id buffer is empty and the composite path
     * buffer holds the only local identity.  A hotplug grid rebuild
     * re-resolves the row indices by stable id, and only falls back to the
     * path for degraded rows, so one physical controller cannot inherit
     * another's host privileges (SPEC §4.4/§10.1). */
    char host_id[CBX_MAX_ID_LEN];
    char host_composite_path[CBX_MAX_PATH_LEN];
    char selected_id[CBX_MAX_ID_LEN];
    char selected_composite_path[CBX_MAX_PATH_LEN];

    cbx_hm_slot_change_cb on_slot_change;
    void                 *slot_change_data;

    cbx_hm_profile_change_cb on_profile_change;
    void                     *profile_change_data;

    /* Dirty-surface trigger for host-mode state transitions. */
    cbx_hm_state_change_cb on_state_change;
    void                  *state_change_data;
};

/* --- API -------------------------------------------------------------- */

void cbx_host_mode_init(cbx_host_mode *hm);

/*
 * Enter host mode. The given row_idx becomes the host.
 * selected_row is initialized to host_row.
 *
 * W2 no-op contract: entering an already-active host-mode object is not a
 * state transition and does NOT fire on_state_change (the dirty trigger);
 * on_state_change fires only on an actual inactive->active transition.
 *
 * Returns 0, -EINVAL.
 */
int cbx_host_mode_enter(cbx_host_mode *hm, int row_idx);

/*
 * Grid-aware enter: identical to cbx_host_mode_enter, but also records the
 * host controller's persistent identity from `grid` (row id + composite
 * path) so host mode can be re-resolved across a hotplug grid rebuild
 * (cbx_host_mode_reconcile, SPEC §4.4/§10.1).  Production entry MUST use
 * this variant.  A non-NULL grid also bounds-checks row_idx against
 * grid->row_count.
 *
 * Returns 0, -EINVAL.
 */
int cbx_host_mode_enter_with_grid(cbx_host_mode *hm,
                                  const cbx_select_grid *grid, int row_idx);

/*
 * Exit host mode. Resets to inactive.
 *
 * W2 no-op contract: exiting a host-mode object that is already inactive
 * is a no-op transition and does NOT fire on_state_change (the dirty
 * trigger).  on_state_change fires only on an actual active->inactive
 * transition, so a redundant exit never emits a spurious dirty trigger.
 *
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
 * Grid-aware toggle: identical to cbx_host_mode_toggle, but records the
 * host's persistent identity from `grid` when entering.  Production entry
 * MUST use this variant so hotplug reconciliation can re-resolve the host.
 * A non-NULL grid bounds-checks row_idx against grid->row_count.
 *
 * Returns the same codes as cbx_host_mode_toggle.
 */
int cbx_host_mode_toggle_with_grid(cbx_host_mode *hm,
                                   const cbx_select_grid *grid, int row_idx);

/*
 * Handle an input in host mode. Only the host controller can act.
 * Non-host controllers get CBX_HM_RESULT_FROZEN.
 *
 * Up/Down: change selected_row (navigate between rows, clamped)
 * Left/Right: move within selected row's columns
 * L1/R1: cycle the selected row's profile (previous/next)
 * R3: exit host mode
 * B: close overlay
 *
 * on_slot_change is fired when Left/Right changes the selected row's
 * column, with the selected_row's row_idx and new slot.
 * on_profile_change is fired when L1/R1 changes the selected row's profile,
 * with the selected_row's row_idx, new profile name and composite path.
 *
 * Returns CBX_HM_RESULT_* code.
 */
int cbx_host_mode_handle(cbx_host_mode *hm, int row_idx,
                         cbx_hm_input input, cbx_select_grid *grid);

/*
 * Reconcile host mode after the select grid rows were rebuilt by a hotplug
 * event or backend recovery (SPEC §10.1).  The stored host/selected stable
 * source-derived identities are re-resolved against the rebuilt grid:
 *
 *   - Host still present: host_row/selected_row are updated to their new
 *     indices and host mode stays active.  If the edited row disappeared,
 *     the selection falls back to the host row.
 *   - Host removed, or its identity was never recorded, or its stable id no
 *     longer matches any row (even one that reused its composite path):
 *     host mode is exited (firing the state-change dirty trigger once), so
 *     no other controller inherits host privileges and no input stays
 *     frozen.  A degraded (path-only) host is only re-resolved against
 *     another degraded row at the same path; a row that now reports a
 *     stable physical identity is treated as a different physical controller.
 *   - Grid empty: host mode is exited.
 *
 * No-op that returns 0 when host mode is inactive.
 *
 * Returns 1 if host mode remains active, 0 if it is now inactive,
 * -EINVAL on bad args.
 */
int cbx_host_mode_reconcile(cbx_host_mode *hm, const cbx_select_grid *grid);

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