/*
 * host_mode.c — Host Mode state machine implementation.
 *
 * Task 30 — Host Mode and conflict detection/resolution.
 */
#include "overlay/host_mode.h"

#include <errno.h>
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

int
cbx_host_mode_enter(cbx_host_mode *hm, int row_idx)
{
    if (!hm || row_idx < 0)
        return -EINVAL;
    hm->active       = true;
    hm->host_row     = row_idx;
    hm->selected_row = row_idx;
    return 0;
}

int
cbx_host_mode_exit(cbx_host_mode *hm)
{
    if (!hm)
        return -EINVAL;
    hm->active       = false;
    hm->host_row     = -1;
    hm->selected_row = -1;
    return 0;
}

int
cbx_host_mode_toggle(cbx_host_mode *hm, int row_idx)
{
    if (!hm || row_idx < 0)
        return -2;

    if (!hm->active) {
        cbx_host_mode_enter(hm, row_idx);
        return 1; /* entered */
    }

    if (hm->host_row == row_idx) {
        cbx_host_mode_exit(hm);
        return 0; /* exited */
    }

    return -1; /* frozen controller, ignored */
}

int
cbx_host_mode_handle(cbx_host_mode *hm, int row_idx,
                     cbx_hm_input input, cbx_select_grid *grid)
{
    if (!hm || !grid)
        return CBX_HM_RESULT_ERROR;

    if (!hm->active)
        return CBX_HM_RESULT_ERROR;

    /* Only the host controller can act. */
    if (row_idx != hm->host_row)
        return CBX_HM_RESULT_FROZEN;

    int row_count = cbx_select_grid_get_row_count(grid);
    if (row_count <= 0)
        return CBX_HM_RESULT_ERROR;

    switch (input) {
    case CBX_HM_UP:
        if (hm->selected_row > 0) {
            hm->selected_row--;
            return CBX_HM_RESULT_MOVED;
        }
        return CBX_HM_RESULT_NONE;

    case CBX_HM_DOWN:
        if (hm->selected_row < row_count - 1) {
            hm->selected_row++;
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