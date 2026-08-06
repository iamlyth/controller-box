/*
 * conflict.c — Conflict detection and resolution implementation.
 *
 * Task 30 — Host Mode and conflict detection/resolution.
 */
#include "overlay/conflict.h"

#include <errno.h>
#include <string.h>

void
cbx_conflict_list_init(cbx_conflict_list *list)
{
    if (!list)
        return;
    memset(list, 0, sizeof(*list));
}

int
cbx_conflict_detect(const cbx_select_grid *grid, cbx_conflict_list *out)
{
    if (!grid || !out)
        return -EINVAL;

    cbx_conflict_list_init(out);

    int row_count = cbx_select_grid_get_row_count(grid);
    if (row_count <= 0)
        return 0;

    /*
     * For each row, check if its column (> 0) is also used by an
     * earlier row. If so, this row is a "second arrival" → conflicted.
     */
    for (int i = 0; i < row_count; i++) {
        int col = cbx_select_grid_get_cur_col(grid, i);
        if (col <= 0)
            continue; /* Unassigned — never a conflict */

        for (int j = 0; j < i; j++) {
            int jcol = cbx_select_grid_get_cur_col(grid, j);
            if (jcol == col) {
                /* Row i is the second arrival on this column */
                if (out->count < CBX_GRID_MAX_ROWS) {
                    out->conflicts[out->count].row_idx       = i;
                    out->conflicts[out->count].conflicting_col = col;
                    out->count++;
                }
                break; /* only record once */
            }
        }
    }

    return 0;
}

bool
cbx_conflict_is_row_conflicted(const cbx_conflict_list *list, int row_idx)
{
    if (!list)
        return false;
    for (int i = 0; i < list->count; i++) {
        if (list->conflicts[i].row_idx == row_idx)
            return true;
    }
    return false;
}

int
cbx_conflict_find_lowest_free_slot(const cbx_select_grid *grid,
                                    int exclude_row_idx)
{
    if (!grid)
        return -1;

    int col_count = cbx_select_grid_get_col_count(grid);
    if (col_count <= 1)
        return -1; /* no P-slots (only Unassigned) */

    int row_count = cbx_select_grid_get_row_count(grid);

    /* Slots are 0..col_count-2 (col 1..col_count-1) */
    int max_slots = col_count - 1;

    for (int slot = 0; slot < max_slots; slot++) {
        int target_col = slot + 1;
        bool occupied  = false;

        for (int r = 0; r < row_count; r++) {
            if (r == exclude_row_idx)
                continue;
            if (cbx_select_grid_get_cur_col(grid, r) == target_col) {
                occupied = true;
                break;
            }
        }

        if (!occupied)
            return slot;
    }

    return -1; /* all occupied */
}

int
cbx_conflict_count_occupied(const cbx_select_grid *grid, int exclude_row_idx)
{
    if (!grid)
        return 0;

    int row_count = cbx_select_grid_get_row_count(grid);
    int count     = 0;

    for (int r = 0; r < row_count; r++) {
        if (r == exclude_row_idx)
            continue;
        if (cbx_select_grid_get_cur_col(grid, r) > 0)
            count++;
    }

    return count;
}

int
cbx_conflict_resolve(cbx_select_grid *grid, cbx_conflict_list *list)
{
    if (!grid || !list)
        return -EINVAL;

    int moved = 0;

    for (int i = 0; i < list->count; i++) {
        int row_idx = list->conflicts[i].row_idx;

        /* Re-check: the row may have been resolved by a prior move */
        int cur_col = cbx_select_grid_get_cur_col(grid, row_idx);
        if (cur_col <= 0)
            continue; /* now on Unassigned, no conflict */

        /* Check if this row is still conflicted (someone else on same col) */
        bool still_conflicted = false;
        int  row_count        = cbx_select_grid_get_row_count(grid);
        for (int r = 0; r < row_count; r++) {
            if (r == row_idx)
                continue;
            if (cbx_select_grid_get_cur_col(grid, r) == cur_col) {
                still_conflicted = true;
                break;
            }
        }
        if (!still_conflicted)
            continue;

        /* Find the lowest free P-slot */
        int free_slot = cbx_conflict_find_lowest_free_slot(grid, row_idx);
        if (free_slot < 0)
            continue; /* all occupied — leave in place */

        /* Move the conflicted row to the free slot */
        /* Move left/right until we reach the target column */
        int cur_slot = cbx_select_grid_col_to_slot(cur_col);
        if (cur_slot < free_slot) {
            /* Move right */
            for (int s = cur_slot; s < free_slot; s++)
                cbx_select_grid_move_right(grid, row_idx);
        } else if (cur_slot > free_slot) {
            /* Move left */
            for (int s = cur_slot; s > free_slot; s--)
                cbx_select_grid_move_left(grid, row_idx);
        }
        moved++;
    }

    return moved;
}