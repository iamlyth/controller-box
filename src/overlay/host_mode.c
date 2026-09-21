/*
 * host_mode.c — Host Mode state machine implementation.
 *
 * Task 30 — Host Mode and conflict detection/resolution.
 */
#include "overlay/host_mode.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

void
cbx_host_mode_init(cbx_host_mode *hm)
{
    if (!hm)
        return;
    memset(hm, 0, sizeof(*hm));
    hm->active       = false;
    hm->host_row     = -1;
    hm->selected_row = -1;
}

static void
fire_state_change(cbx_host_mode *hm, bool active)
{
    if (hm->on_state_change)
        hm->on_state_change(active, hm->state_change_data);
}

/* Copy a grid row's persistent identity into the given buffers.  When the
 * row has a stable source-derived physical identity (SPEC §6.2: BT MAC,
 * USB serial, USB port path), only `id` is recorded — the composite path
 * is NOT an identity and is deliberately left empty so resolve_row_identity
 * can never re-grant host privileges through a path that a different
 * physical controller may now occupy (SPEC §4.4).  For a degraded row (no
 * stable source-derived identity) the composite path is the only local
 * identity, so it is recorded and `id` is left empty.  Buffers are cleared
 * when the row is out of range or the grid is absent. */
static void
record_row_identity(char *id, size_t id_size,
                    char *path, size_t path_size,
                    const cbx_select_grid *grid, int row_idx)
{
    if (id && id_size)
        id[0] = '\0';
    if (path && path_size)
        path[0] = '\0';
    if (!grid || row_idx < 0 || row_idx >= grid->row_count)
        return;
    if (grid->rows[row_idx].id_stable) {
        if (id && id_size)
            snprintf(id, id_size, "%s", grid->rows[row_idx].id);
    } else if (path && path_size) {
        snprintf(path, path_size, "%s",
                 grid->rows[row_idx].composite_path);
    }
}

/* Re-resolve a persisted row identity against a rebuilt grid.  The stable
 * source-derived physical identity is authoritative (SPEC §4.4/§6.2); the
 * composite path is only a degraded fallback when no stable id was
 * recorded.  Returns the row index or -1.
 *
 * No-privilege-inheritance invariant: when a stable id was recorded but no
 * longer matches a row, the host is gone and we must NOT fall back to the
 * composite path — a different controller may have reused it.  Likewise a
 * degraded (path-only) identity may only match a row that is itself
 * degraded; a controller that now reports a stable source-derived identity
 * is a different physical device and must not inherit host privileges. */
static int
resolve_row_identity(const cbx_select_grid *grid,
                     const char *id, const char *path)
{
    if (!grid || grid->row_count <= 0)
        return -1;
    if (id && id[0]) {
        for (int i = 0; i < grid->row_count; i++)
            if (grid->rows[i].id_stable &&
                strcmp(grid->rows[i].id, id) == 0)
                return i;
        return -1;
    }
    if (path && path[0]) {
        for (int i = 0; i < grid->row_count; i++) {
            if (grid->rows[i].id_stable)
                continue;
            if (strcmp(grid->rows[i].composite_path, path) == 0)
                return i;
        }
    }
    return -1;
}

int
cbx_host_mode_enter_with_grid(cbx_host_mode *hm,
                              const cbx_select_grid *grid, int row_idx)
{
    if (!hm || row_idx < 0)
        return -EINVAL;
    /* Explicit bounds-check when a grid is supplied (hardening): the host
     * row must name a real controller row. */
    if (grid && row_idx >= grid->row_count)
        return -EINVAL;

    bool was_active  = hm->active;
    hm->active       = true;
    hm->host_row     = row_idx;
    hm->selected_row = row_idx;
    record_row_identity(hm->host_id, sizeof(hm->host_id),
                        hm->host_composite_path,
                        sizeof(hm->host_composite_path), grid, row_idx);
    record_row_identity(hm->selected_id, sizeof(hm->selected_id),
                        hm->selected_composite_path,
                        sizeof(hm->selected_composite_path), grid, row_idx);
    /* Fire the dirty trigger only on a real inactive->active transition.
     * Re-entering an already-active host-mode object is a no-op transition
     * and must not emit a spurious state change (W2: dirty only on an
     * actual host-mode state transition). */
    if (!was_active)
        fire_state_change(hm, true);
    return 0;
}

int
cbx_host_mode_enter(cbx_host_mode *hm, int row_idx)
{
    return cbx_host_mode_enter_with_grid(hm, NULL, row_idx);
}

int
cbx_host_mode_exit(cbx_host_mode *hm)
{
    if (!hm)
        return -EINVAL;
    bool was_active = hm->active;
    hm->active       = false;
    hm->host_row     = -1;
    hm->selected_row = -1;
    hm->host_id[0] = '\0';
    hm->host_composite_path[0] = '\0';
    hm->selected_id[0] = '\0';
    hm->selected_composite_path[0] = '\0';
    /* Fire the dirty trigger only on a real active→inactive transition.
     * A no-op exit on an already-idle host-mode object must not emit a
     * spurious state change (W2: deliberate/consistent triggers — dirty
     * only on an actual host-mode state transition). */
    if (was_active)
        fire_state_change(hm, false);
    return 0;
}

int
cbx_host_mode_toggle_with_grid(cbx_host_mode *hm,
                               const cbx_select_grid *grid, int row_idx)
{
    if (!hm || row_idx < 0)
        return -2;
    /* Explicit bounds-check when a grid is supplied (hardening). */
    if (grid && row_idx >= grid->row_count)
        return -2;

    if (!hm->active) {
        cbx_host_mode_enter_with_grid(hm, grid, row_idx);
        return 1; /* entered */
    }

    if (hm->host_row == row_idx) {
        cbx_host_mode_exit(hm);
        return 0; /* exited */
    }

    return -1; /* frozen controller, ignored */
}

int
cbx_host_mode_toggle(cbx_host_mode *hm, int row_idx)
{
    return cbx_host_mode_toggle_with_grid(hm, NULL, row_idx);
}

int
cbx_host_mode_handle(cbx_host_mode *hm, int row_idx,
                     cbx_hm_input input, cbx_select_grid *grid)
{
    if (!hm || !grid)
        return CBX_HM_RESULT_ERROR;

    if (!hm->active)
        return CBX_HM_RESULT_ERROR;

    int row_count = cbx_select_grid_get_row_count(grid);
    if (row_count <= 0)
        return CBX_HM_RESULT_ERROR;

    /* Explicit sender bounds-check (hardening): a row index that does not
     * name a controller row cannot be the host. */
    if (row_idx < 0 || row_idx >= row_count)
        return CBX_HM_RESULT_ERROR;

    /* Only the host controller can act. */
    if (row_idx != hm->host_row)
        return CBX_HM_RESULT_FROZEN;

    switch (input) {
    case CBX_HM_UP:
        if (hm->selected_row > 0) {
            hm->selected_row--;
            record_row_identity(hm->selected_id, sizeof(hm->selected_id),
                                hm->selected_composite_path,
                                sizeof(hm->selected_composite_path),
                                grid, hm->selected_row);
            return CBX_HM_RESULT_MOVED;
        }
        return CBX_HM_RESULT_NONE;

    case CBX_HM_DOWN:
        if (hm->selected_row < row_count - 1) {
            hm->selected_row++;
            record_row_identity(hm->selected_id, sizeof(hm->selected_id),
                                hm->selected_composite_path,
                                sizeof(hm->selected_composite_path),
                                grid, hm->selected_row);
            return CBX_HM_RESULT_MOVED;
        }
        return CBX_HM_RESULT_NONE;

    case CBX_HM_LEFT: {
        int rc = cbx_select_grid_move_left(grid, hm->selected_row);
        if (rc == 0) {
            int slot = cbx_select_grid_col_to_slot(
                cbx_select_grid_get_cur_col(grid, hm->selected_row));
            if (hm->on_slot_change)
                hm->on_slot_change(hm->selected_row, slot,
                                   hm->slot_change_data);
            return CBX_HM_RESULT_SLOT;
        }
        return CBX_HM_RESULT_NONE;
    }

    case CBX_HM_RIGHT: {
        int rc = cbx_select_grid_move_right(grid, hm->selected_row);
        if (rc == 0) {
            int slot = cbx_select_grid_col_to_slot(
                cbx_select_grid_get_cur_col(grid, hm->selected_row));
            if (hm->on_slot_change)
                hm->on_slot_change(hm->selected_row, slot,
                                   hm->slot_change_data);
            return CBX_HM_RESULT_SLOT;
        }
        return CBX_HM_RESULT_NONE;
    }

    case CBX_HM_PROFILE_PREV: {
        int rc = cbx_select_grid_cycle_profile_up(grid, hm->selected_row);
        if (rc == 0) {
            if (hm->on_profile_change)
                hm->on_profile_change(hm->selected_row,
                    grid->rows[hm->selected_row].profile,
                    grid->rows[hm->selected_row].composite_path,
                    hm->profile_change_data);
            return CBX_HM_RESULT_PROFILE;
        }
        return CBX_HM_RESULT_NONE;  /* no profiles or error */
    }

    case CBX_HM_PROFILE_NEXT: {
        int rc = cbx_select_grid_cycle_profile_down(grid, hm->selected_row);
        if (rc == 0) {
            if (hm->on_profile_change)
                hm->on_profile_change(hm->selected_row,
                    grid->rows[hm->selected_row].profile,
                    grid->rows[hm->selected_row].composite_path,
                    hm->profile_change_data);
            return CBX_HM_RESULT_PROFILE;
        }
        return CBX_HM_RESULT_NONE;  /* no profiles or error */
    }

    case CBX_HM_R3:
        cbx_host_mode_exit(hm);
        return CBX_HM_RESULT_EXIT;

    case CBX_HM_B:
        return CBX_HM_RESULT_CLOSE;

    default:
        return CBX_HM_RESULT_NONE;
    }
}

/* --- Accessors -------------------------------------------------------- */

int
cbx_host_mode_reconcile(cbx_host_mode *hm, const cbx_select_grid *grid)
{
    if (!hm || !grid)
        return -EINVAL;
    if (!hm->active)
        return 0;

    if (grid->row_count <= 0) {
        cbx_host_mode_exit(hm);
        return 0;
    }

    int new_host = resolve_row_identity(grid, hm->host_id,
                                        hm->host_composite_path);
    if (new_host < 0) {
        /* The host controller is no longer present: exit host mode so no
         * other controller inherits host privileges and no input remains
         * permanently frozen (SPEC §4.4). */
        cbx_host_mode_exit(hm);
        return 0;
    }

    int new_selected = resolve_row_identity(grid, hm->selected_id,
                                            hm->selected_composite_path);
    if (new_selected < 0)
        new_selected = new_host; /* edited row gone → edit the host row */

    hm->host_row     = new_host;
    hm->selected_row = new_selected;
    record_row_identity(hm->host_id, sizeof(hm->host_id),
                        hm->host_composite_path,
                        sizeof(hm->host_composite_path), grid, new_host);
    record_row_identity(hm->selected_id, sizeof(hm->selected_id),
                        hm->selected_composite_path,
                        sizeof(hm->selected_composite_path), grid,
                        new_selected);
    return 1;
}

bool
cbx_host_mode_is_active(const cbx_host_mode *hm)
{
    return hm ? hm->active : false;
}

int
cbx_host_mode_get_host_row(const cbx_host_mode *hm)
{
    return hm ? hm->host_row : -1;
}

int
cbx_host_mode_get_selected_row(const cbx_host_mode *hm)
{
    return hm ? hm->selected_row : -1;
}

cbx_row_visual_state
cbx_host_mode_row_state(const cbx_host_mode *hm, int row_idx)
{
    if (!hm || !hm->active || row_idx < 0)
        return CBX_ROW_NORMAL;

    if (row_idx == hm->selected_row)
        return CBX_ROW_SELECTED;

    if (row_idx == hm->host_row)
        return CBX_ROW_HOST;

    return CBX_ROW_FROZEN;
}

bool
cbx_host_mode_is_frozen(const cbx_host_mode *hm, int row_idx)
{
    if (!hm || !hm->active)
        return false;
    return row_idx != hm->host_row;
}