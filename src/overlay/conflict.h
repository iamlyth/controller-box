/*
 * conflict.h — Conflict detection and resolution for the overlay grid.
 *
 * Task 30 — Host Mode and conflict detection/resolution.
 *
 * A conflict occurs when two or more controllers (rows) occupy the same
 * P-slot column (col > 0). The "second arrival" (the row that is not the
 * first to claim the column) is shown in red. On overlay exit, conflicted
 * controllers are moved to the lowest unoccupied P-slot (deterministic,
 * automatic). Controllers on Unassigned (col 0) are never in conflict.
 */
#ifndef CBX_OVERLAY_CONFLICT_H
#define CBX_OVERLAY_CONFLICT_H

#include <stdbool.h>
#include "overlay/grid_render.h"

/* --- Conflict info ---------------------------------------------------- */

typedef struct {
    int row_idx;           /* which row is conflicted (second arrival)  */
    int conflicting_col;   /* the column where the conflict occurs      */
} cbx_conflict_info;

typedef struct {
    cbx_conflict_info conflicts[CBX_GRID_MAX_ROWS];
    int               count;
} cbx_conflict_list;

/* --- API -------------------------------------------------------------- */

void cbx_conflict_list_init(cbx_conflict_list *list);

/*
 * Detect all conflicts in the grid. A conflict is when 2+ rows share
 * the same column > 0 (P-slot). The first row (by index) on a column
 * is the "owner"; subsequent rows are "second arrivals" (conflicted).
 *
 * Col 0 (Unassigned) is never a conflict — any number of controllers
 * can be unassigned.
 *
 * Returns 0 on success, -EINVAL on bad args.
 */
int cbx_conflict_detect(const cbx_select_grid *grid,
                         cbx_conflict_list *out);

/*
 * Check if a specific row is conflicted (is a second arrival).
 */
bool cbx_conflict_is_row_conflicted(const cbx_conflict_list *list,
                                     int row_idx);

/*
 * Resolve all conflicts: move each conflicted row to the lowest
 * unoccupied P-slot.
 *
 * Processing is in row order (deterministic). After each move, the
 * grid state is updated so subsequent conflicts see the new layout.
 *
 * Edge cases:
 *   - All P-slots occupied: conflicted controller stays in place
 *   - Conflicted controller on Unassigned: not a conflict (skipped)
 *
 * Returns the number of controllers moved, or -EINVAL on bad args.
 */
int cbx_conflict_resolve(cbx_select_grid *grid, cbx_conflict_list *list);

/*
 * Find the lowest unoccupied P-slot for a given row, excluding that
 * row's own current position from the occupancy check.
 *
 * Returns slot index (0-based) or -1 if all slots are occupied.
 */
int cbx_conflict_find_lowest_free_slot(const cbx_select_grid *grid,
                                        int exclude_row_idx);

/*
 * Count how many rows occupy P-slots (col > 0), excluding the given row.
 */
int cbx_conflict_count_occupied(const cbx_select_grid *grid,
                                 int exclude_row_idx);

#endif /* CBX_OVERLAY_CONFLICT_H */