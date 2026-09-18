/*
 * player_mode.c — Player Mode navigation for the select grid.
 *
 * Task 29 — Character select grid rendering and Player Mode navigation.
 *
 * Implements Player Mode (SPEC §4.3): each controller navigates its own
 * row independently.  Left/Right moves across columns (slot position),
 * Up/Down cycles profiles.  Side effects are handled via callbacks.
 */
#include "overlay/player_mode.h"

#include <errno.h>
#include <string.h>

/* --- API -------------------------------------------------------------- */

void
cbx_player_mode_init(cbx_player_mode *pm, cbx_select_grid *grid)
{
    if (!pm)
        return;
    memset(pm, 0, sizeof(*pm));
    pm->grid = grid;
}

int
cbx_player_mode_handle(cbx_player_mode *pm, int row_idx,
                        cbx_pm_input input)
{
    if (!pm || !pm->grid)
        return CBX_PM_RESULT_ERROR;
    if (row_idx < 0 || row_idx >= pm->grid->row_count)
        return CBX_PM_RESULT_ERROR;

    switch (input) {
    case CBX_PM_LEFT: {
        int rc = cbx_select_grid_move_left(pm->grid, row_idx);
        if (rc == 0) {
            /* Fire slot change callback. */
            if (pm->on_slot_change) {
                int new_slot = cbx_select_grid_col_to_slot(
                    pm->grid->rows[row_idx].cur_col);
                pm->on_slot_change(row_idx, new_slot,
                                    pm->slot_change_data);
            }
            return CBX_PM_RESULT_MOVED;
        }
        return CBX_PM_RESULT_NONE;  /* at boundary, no move */
    }

    case CBX_PM_RIGHT: {
        int rc = cbx_select_grid_move_right(pm->grid, row_idx);
        if (rc == 0) {
            if (pm->on_slot_change) {
                int new_slot = cbx_select_grid_col_to_slot(
                    pm->grid->rows[row_idx].cur_col);
                pm->on_slot_change(row_idx, new_slot,
                                    pm->slot_change_data);
            }
            return CBX_PM_RESULT_MOVED;
        }
        return CBX_PM_RESULT_NONE;
    }

    case CBX_PM_UP: {
        int rc = cbx_select_grid_cycle_profile_up(pm->grid, row_idx);
        if (rc == 0) {
            if (pm->on_profile_change) {
                pm->on_profile_change(row_idx,
                    pm->grid->rows[row_idx].profile,
                    pm->grid->rows[row_idx].composite_path,
                    pm->profile_change_data);
            }
            return CBX_PM_RESULT_PROFILE;
        }
        return CBX_PM_RESULT_NONE;  /* no profiles or error */
    }

    case CBX_PM_DOWN: {
        int rc = cbx_select_grid_cycle_profile_down(pm->grid, row_idx);
        if (rc == 0) {
            if (pm->on_profile_change) {
                pm->on_profile_change(row_idx,
                    pm->grid->rows[row_idx].profile,
                    pm->grid->rows[row_idx].composite_path,
                    pm->profile_change_data);
            }
            return CBX_PM_RESULT_PROFILE;
        }
        return CBX_PM_RESULT_NONE;
    }

    case CBX_PM_B:
        return CBX_PM_RESULT_CLOSE;

    case CBX_PM_R3:
        return CBX_PM_RESULT_HOST;

    default:
        return CBX_PM_RESULT_NONE;
    }
}

int
cbx_player_mode_get_slot(const cbx_player_mode *pm, int row_idx)
{
    if (!pm || !pm->grid)
        return -1;
    if (row_idx < 0 || row_idx >= pm->grid->row_count)
        return -1;
    return cbx_select_grid_col_to_slot(pm->grid->rows[row_idx].cur_col);
}

const char *
cbx_player_mode_get_profile(const cbx_player_mode *pm, int row_idx)
{
    if (!pm || !pm->grid)
        return NULL;
    if (row_idx < 0 || row_idx >= pm->grid->row_count)
        return NULL;
    return pm->grid->rows[row_idx].profile;
}