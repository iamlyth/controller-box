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
#include <stdio.h>
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
    case CBX_PM_LEFT:
    case CBX_PM_RIGHT: {
        int old_col = pm->grid->rows[row_idx].cur_col;
        uint64_t old_arrival = pm->grid->row_arrival_seq[row_idx];
        bool old_live_edits = pm->grid->live_edits;
        int rc = input == CBX_PM_LEFT
            ? cbx_select_grid_move_left(pm->grid, row_idx)
            : cbx_select_grid_move_right(pm->grid, row_idx);
        if (rc != 0)
            return rc == -ERANGE ? CBX_PM_RESULT_NONE : CBX_PM_RESULT_ERROR;

        if (pm->on_slot_change) {
            int new_slot = cbx_select_grid_col_to_slot(
                pm->grid->rows[row_idx].cur_col);
            rc = pm->on_slot_change(row_idx, new_slot,
                                    pm->slot_change_data);
            if (rc != 0) {
                /* Navigation is staged in the grid.  A rejected side effect
                 * must not leave the display claiming that the slot changed. */
                pm->grid->rows[row_idx].cur_col = old_col;
                pm->grid->row_arrival_seq[row_idx] = old_arrival;
                pm->grid->live_edits = old_live_edits;
                return rc;
            }
        }
        return CBX_PM_RESULT_MOVED;
    }

    case CBX_PM_UP:
    case CBX_PM_DOWN: {
        char old_profile[CBX_GRID_PROFILE_LEN];
        bool old_live_edits = pm->grid->live_edits;
        snprintf(old_profile, sizeof(old_profile), "%s",
                 pm->grid->rows[row_idx].profile);
        int rc = input == CBX_PM_UP
            ? cbx_select_grid_cycle_profile_up(pm->grid, row_idx)
            : cbx_select_grid_cycle_profile_down(pm->grid, row_idx);
        if (rc != 0)
            return rc == -ENOENT ? CBX_PM_RESULT_NONE : CBX_PM_RESULT_ERROR;

        if (pm->on_profile_change) {
            rc = pm->on_profile_change(row_idx,
                    pm->grid->rows[row_idx].profile,
                    pm->grid->rows[row_idx].composite_path,
                    pm->profile_change_data);
            if (rc != 0) {
                /* The callback is the confirmation boundary for a profile
                 * change.  Restore the last confirmed value on failure. */
                snprintf(pm->grid->rows[row_idx].profile,
                         CBX_GRID_PROFILE_LEN, "%s", old_profile);
                pm->grid->live_edits = old_live_edits;
                return rc;
            }
        }
        return CBX_PM_RESULT_PROFILE;
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