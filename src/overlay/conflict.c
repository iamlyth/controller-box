/*
 * conflict.c — Conflict detection and resolution implementation.
 *
 * Task 30 — Host Mode and conflict detection/resolution.
 */
#include "overlay/conflict.h"

#include <errno.h>
#include <string.h>
#include <stdint.h>

void
cbx_conflict_list_init(cbx_conflict_list *list)
{
    if (!list)
        return;
    memset(list, 0, sizeof(*list));
}

static bool
row_arrived_before(const cbx_select_grid *grid, int lhs, int rhs)
{
    uint64_t lseq = grid->row_arrival_seq[lhs];
    uint64_t rseq = grid->row_arrival_seq[rhs];
    if (lseq != 0 && rseq != 0 && lseq != rseq)
        return lseq < rseq;
    if (lseq != 0 && rseq == 0)
        return true;
    if (lseq == 0 && rseq != 0)
        return false;
    return lhs < rhs;
}

int
cbx_conflict_detect(const cbx_select_grid *grid, cbx_conflict_list *out)
{
    if (!grid || !out)
        return -EINVAL;

    cbx_conflict_list_init(out);

    int row_count = cbx_select_grid_get_row_count(grid);
    if (row_count < 0 || row_count > CBX_GRID_MAX_ROWS)
        return -EINVAL;
    if (row_count == 0)
        return 0;

    /* Choose an owner by claim time, not by the current row index. */
    for (int i = 0; i < row_count; i++) {
        int col = cbx_select_grid_get_cur_col(grid, i);
        if (col <= 0)
            continue;

        int owner = i;
        for (int j = 0; j < row_count; j++) {
            if (j == i || cbx_select_grid_get_cur_col(grid, j) != col)
                continue;
            if (row_arrived_before(grid, j, owner))
                owner = j;
        }
        if (i == owner)
            continue;

        bool recorded = false;
        for (int k = 0; k < out->count; k++)
            if (out->conflicts[k].row_idx == i) {
                recorded = true;
                break;
            }
        if (!recorded && out->count < CBX_GRID_MAX_ROWS) {
            out->conflicts[out->count].row_idx = i;
            out->conflicts[out->count].conflicting_col = col;
            out->count++;
        }
    }

    return 0;
}

bool
cbx_conflict_is_row_conflicted(const cbx_conflict_list *list, int row_idx)
{
    if (!list || list->count < 0 || list->count > CBX_GRID_MAX_ROWS)
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

    if (list->count < 0 || list->count > CBX_GRID_MAX_ROWS)
        return -EINVAL;

    int moved = 0;
    list->unresolved_count = 0;

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
        if (free_slot < 0) {
            /* Keep the red row visible; callers must not route a duplicate. */
            list->unresolved_count++;
            continue;
        }

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

    if (list->unresolved_count == 0) {
        cbx_conflict_list remaining;
        cbx_conflict_detect(grid, &remaining);
        list->unresolved_count = remaining.count;
    }
    return moved;
}