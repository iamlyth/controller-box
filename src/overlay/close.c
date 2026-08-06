/*
 * close.c — Overlay close coordination (Task 32, SPEC §2.5, §4.5, §11).
 *
 * Implements the close-time save sequence: conflict detection +
 * resolution, assignment sync from grid state, and assignment
 * persistence.  The lifecycle module handles InterceptMode=PASS and
 * surface hide/transition.
 */
#include "overlay/close.h"

#include <errno.h>
#include <string.h>

#include "identify/assign.h"      /* cbx_assign_find_index */
#include "overlay/conflict.h"     /* cbx_conflict_detect, cbx_conflict_resolve */

/* --- Helpers ---------------------------------------------------------- */

/*
 * Remove the assignment at the given index by shifting subsequent
 * entries down.  Decrements assignment_count.
 */
static void
remove_assignment_at(cbx_assignments *a, int idx)
{
    if (idx < 0 || idx >= a->assignment_count)
        return;
    for (int i = idx; i < a->assignment_count - 1; i++)
        a->assignments[i] = a->assignments[i + 1];
    a->assignment_count--;
}

/*
 * Add a new assignment at the end of the array.
 * Returns 0 on success, -ENOSPC if the array is full.
 */
static int
add_assignment(cbx_assignments *a, const char *id, int slot,
               const char *profile)
{
    if (a->assignment_count >= CBX_MAX_ASSIGNMENTS)
        return -ENOSPC;

    cbx_assignment *entry = &a->assignments[a->assignment_count];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->id, CBX_MAX_ID_LEN, "%s", id);
    entry->slot = slot;
    if (profile && profile[0])
        snprintf(entry->profile, CBX_MAX_PROFILE_LEN, "%s", profile);
    a->assignment_count++;
    return 0;
}

/* --- Public API -------------------------------------------------------- */

int
cbx_close_sync_assignments(cbx_select_grid *grid,
                            cbx_assignments *assignments)
{
    if (!grid || !assignments)
        return -EINVAL;

    int row_count = cbx_select_grid_get_row_count(grid);

    for (int i = 0; i < row_count; i++) {
        const cbx_grid_row *row = cbx_select_grid_get_row(grid, i);
        if (!row || !row->id[0])
            continue;  /* skip rows without identity */

        int col = row->cur_col;
        int slot = cbx_select_grid_col_to_slot(col);  /* col 0 → -1 */

        int idx = cbx_assign_find_index(assignments, row->id);

        if (slot >= 0) {
            /* Assigned to a P-slot: update or create. */
            if (idx >= 0) {
                assignments->assignments[idx].slot = slot;
                /* Update profile. */
                if (row->profile[0])
                    snprintf(assignments->assignments[idx].profile,
                             CBX_MAX_PROFILE_LEN, "%s", row->profile);
                else
                    assignments->assignments[idx].profile[0] = '\0';
            } else {
                int rc = add_assignment(assignments, row->id, slot,
                                          row->profile);
                if (rc < 0)
                    return rc;
            }
        } else {
            /* Unassigned (col 0): remove existing assignment if any. */
            if (idx >= 0)
                remove_assignment_at(assignments, idx);
        }
    }

    return 0;
}

int
cbx_close_on_save(void *userdata)
{
    if (!userdata)
        return -EINVAL;

    cbx_close_ctx *ctx = (cbx_close_ctx *)userdata;
    if (!ctx->grid || !ctx->assignments)
        return -EINVAL;

    /* 1. Detect conflicts. */
    cbx_conflict_list conflicts;
    cbx_conflict_list_init(&conflicts);
    int rc = cbx_conflict_detect(ctx->grid, &conflicts);
    if (rc < 0)
        return rc;

    /* 2. Resolve conflicts (move second arrivals to free P-slots). */
    if (conflicts.count > 0)
        cbx_conflict_resolve(ctx->grid, &conflicts);

    /* 3. Sync grid state back to assignments. */
    rc = cbx_close_sync_assignments(ctx->grid, ctx->assignments);
    if (rc < 0)
        return rc;

    /* 4. Save assignments to disk. */
    rc = cbx_assignments_save(ctx->assignments);
    if (rc < 0)
        return rc;

    return 0;
}

int
cbx_overlay_request_close(cbx_overlay_lifecycle *lc,
                            cbx_select_grid *grid,
                            cbx_assignments *assignments)
{
    if (!lc || !grid || !assignments)
        return -EINVAL;

    /* Wire the on_save callback with a stack-local context.  This is
     * safe because cbx_overlay_lifecycle_close calls on_save
     * synchronously before returning — the context is valid throughout
     * the callback invocation.  After close returns, on_save_data is
     * never used again (the lifecycle never calls on_save twice). */
    cbx_close_ctx ctx = { .grid = grid, .assignments = assignments };
    lc->on_save = cbx_close_on_save;
    lc->on_save_data = &ctx;

    return cbx_overlay_lifecycle_close(lc);
}