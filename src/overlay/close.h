/*
 * close.h — Overlay close coordination (Task 32, SPEC §2.5, §4.5, §11).
 *
 * Coordinates the overlay close sequence:
 *   1. Detect conflicts in the grid (two controllers on the same P-slot).
 *   2. Resolve conflicts: move second arrivals to the lowest free P-slot
 *      (deterministic, automatic — SPEC §4.5).
 *   3. Sync grid state back to assignments (slot + profile per controller).
 *   4. Save assignments to ~/.config/controller-box/assignments.yaml.
 *   5. The lifecycle module handles InterceptMode=PASS + hide surface +
 *      state transition to IDLE.
 *
 * The on_save callback (cbx_close_on_save) performs steps 1–4 and is
 * designed to be wired into cbx_overlay_lifecycle's on_save field.
 * The convenience function cbx_overlay_request_close wires the callback
 * and calls cbx_overlay_lifecycle_close.
 *
 * This module is testable with the mock DBus backend and in-memory
 * grid + assignments structs — no file I/O in tests if the assignments
 * save is mocked or the config dir is a temp directory.
 */
#ifndef CBX_OVERLAY_CLOSE_H
#define CBX_OVERLAY_CLOSE_H

#include <stdbool.h>

#include "config/config_assignments.h"
#include "overlay/grid_render.h"
#include "overlay/lifecycle.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Close context — passed as on_save userdata to the lifecycle.
 * Contains the grid and assignments to sync + save on close.
 */
typedef struct {
    cbx_select_grid  *grid;
    cbx_assignments  *assignments;
} cbx_close_ctx;

/*
 * Sync the grid state back to the assignments struct.
 *
 * For each grid row with a valid (non-empty) identity ID:
 *   - If the row is on a P-slot (col > 0): find the existing assignment
 *     by ID and update its slot + profile, or create a new assignment
 *     if none exists.
 *   - If the row is on Unassigned (col 0): remove any existing assignment
 *     for that ID (the controller is no longer assigned to a player slot).
 *
 * Assignments for controllers not in the grid (disconnected) are
 * preserved unchanged.
 *
 * @param grid        The select grid (after conflict resolution).
 * @param assignments  The assignments struct to update.
 * @return 0 on success; -EINVAL on null args.
 */
int cbx_close_sync_assignments(cbx_select_grid *grid,
                                cbx_assignments *assignments);

/*
 * On-save callback for the overlay lifecycle.
 *
 * Performs the full close-time save sequence:
 *   1. Detect conflicts in the grid.
 *   2. Resolve conflicts (move second arrivals to free P-slots).
 *   3. Sync grid state back to assignments.
 *   4. Save assignments to disk.
 *
 * @param userdata  Pointer to cbx_close_ctx (grid + assignments).
 * @return 0 on success; -EINVAL if userdata is null; negative errno
 *         from assignments save on failure.
 */
int cbx_close_on_save(void *userdata);

/*
 * Convenience: wire the on_save callback and request lifecycle close.
 *
 * Sets lc->on_save = cbx_close_on_save and lc->on_save_data = &ctx,
 * then calls cbx_overlay_lifecycle_close(lc).  The lifecycle close
 * fires the on_save callback (conflict resolve + assignment sync +
 * save), sets InterceptMode=PASS, hides the surface, and transitions
 * to IDLE.
 *
 * @param lc      Overlay lifecycle.
 * @param grid    Select grid (will be modified by conflict resolution).
 * @param assignments  Assignments struct (will be updated + saved).
 * @return 0 on success; -EINVAL on null args; -EPERM if lifecycle not
 *         in VISIBLE or ACTIVATING state.
 */
int cbx_overlay_request_close(cbx_overlay_lifecycle *lc,
                                cbx_select_grid *grid,
                                cbx_assignments *assignments);

#ifdef __cplusplus
}
#endif

#endif /* CBX_OVERLAY_CLOSE_H */