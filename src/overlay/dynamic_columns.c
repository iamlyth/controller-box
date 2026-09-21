/*
 * dynamic_columns.c — Dynamic column management from device model (Task 31).
 *
 * Builds grid columns from target device types and rebuilds the grid
 * when target devices change (hotplug / manager add-remove).
 */
#include "overlay/dynamic_columns.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* --- Build virtual_controllers from target types ------------------------ */

int
cbx_dynamic_columns_build_vcs(const char (*types)[CBX_MAX_TYPE_LEN],
                                 int type_count,
                                 cbx_virtual_controllers *out)
{
    if (!out || !types)
        return -EINVAL;
    if (type_count < 1 || type_count > CBX_MAX_CONTROLLERS)
        return -EINVAL;

    memset(out, 0, sizeof(*out));
    out->count = type_count;

    for (int i = 0; i < type_count && i < CBX_MAX_CONTROLLERS; i++) {
        snprintf(out->types[i], CBX_MAX_TYPE_LEN, "%s", types[i]);
    }

    return 0;
}

/* --- Needs rebuild check ------------------------------------------------ */

bool
cbx_dynamic_columns_needs_rebuild(const cbx_select_grid *grid,
                                      int target_count)
{
    if (!grid || target_count < 0 || target_count > CBX_MAX_CONTROLLERS)
        return false;

    int expected_cols = target_count + 1;  /* +1 for Unassigned */
    return grid->col_count != expected_cols;
}

bool
cbx_dynamic_columns_types_changed(const cbx_select_grid *grid,
                                   const char (*target_types)[CBX_MAX_TYPE_LEN],
                                   int target_type_count)
{
    if (!grid || target_type_count < 0 ||
        target_type_count > CBX_MAX_CONTROLLERS ||
        (target_type_count > 0 && !target_types))
        return false;
    if (grid->col_count != target_type_count + 1)
        return true;
    for (int i = 0; i < target_type_count; i++)
        if (strcmp(grid->cols[i + 1].device_type, target_types[i]) != 0)
            return true;
    return false;
}

/* --- Clamp positions ---------------------------------------------------- */

int
cbx_dynamic_columns_clamp_positions(cbx_select_grid *grid,
                                      int new_col_count)
{
    if (!grid || new_col_count < 1 || new_col_count > CBX_GRID_MAX_COLS)
        return -EINVAL;
    if (grid->row_count < 0 || grid->row_count > CBX_GRID_MAX_ROWS)
        return -EINVAL;

    int clamped = 0;

    for (int i = 0; i < grid->row_count; i++) {
        if (grid->rows[i].cur_col >= new_col_count) {
            grid->rows[i].cur_col = CBX_GRID_UNASSIGNED_COL;
            clamped++;
        }
    }

    return clamped;
}

/* --- Rebuild grid ------------------------------------------------------- */

int
cbx_dynamic_columns_rebuild(cbx_select_grid *grid,
                              const char (*target_types)[CBX_MAX_TYPE_LEN],
                              int target_type_count,
                              const cbx_grid_composite_info *composites,
                              int composite_count,
                              const cbx_assignments *assignments)
{
    if (!grid || (target_type_count > 0 && !target_types) ||
        (target_type_count == 0 && target_types != NULL) ||
        (composite_count > 0 && !composites) || !assignments)
        return -EINVAL;
    if (target_type_count < 0 || target_type_count > CBX_MAX_CONTROLLERS)
        return -EINVAL;
    if (composite_count < 0 || composite_count > CBX_GRID_MAX_ROWS)
        return -EINVAL;

    /* Snapshot live edits before rebuilding from durable assignments.  The
     * assignment file is intentionally not authoritative while the overlay
     * session is active. */
    cbx_grid_row saved_rows[CBX_GRID_MAX_ROWS];
    uint64_t saved_arrivals[CBX_GRID_MAX_ROWS];
    int saved_row_count = grid->row_count;
    bool preserve_live_edits = grid->live_edits;
    if (saved_row_count > CBX_GRID_MAX_ROWS)
        saved_row_count = CBX_GRID_MAX_ROWS;
    memcpy(saved_rows, grid->rows, sizeof(saved_rows));
    memcpy(saved_arrivals, grid->row_arrival_seq, sizeof(saved_arrivals));

    /* Preserve the profile list across rebuilds. */
    char saved_profiles[CBX_GRID_MAX_PROFILES][CBX_GRID_PROFILE_LEN];
    int saved_profile_count = grid->profile_count;
    if (saved_profile_count > CBX_GRID_MAX_PROFILES)
        saved_profile_count = CBX_GRID_MAX_PROFILES;
    memcpy(saved_profiles, grid->profiles, sizeof(saved_profiles));

    /* Build virtual_controllers from target types. */
    cbx_virtual_controllers vc;
    int rc = 0;
    if (target_type_count > 0) {
        rc = cbx_dynamic_columns_build_vcs(target_types, target_type_count,
                                             &vc);
        if (rc != 0)
            return rc;
    } else {
        memset(&vc, 0, sizeof(vc));
    }

    /* Construct a settings struct with just virtual_controllers.
     * Other fields are defaults — grid_build only uses virtual_controllers. */
    cbx_settings settings;
    memset(&settings, 0, sizeof(settings));
    settings.virtual_controllers = vc;

    /* Rebuild the grid (this re-derives durable positions first). */
    rc = cbx_select_grid_build(grid, composites, composite_count,
                                &settings, assignments);
    if (rc != 0)
        return rc;

    /* Restore each surviving row's unsaved slot/profile by stable identity.
     * A reused path must not inherit a stable physical controller's edits;
     * path matching is reserved for degraded rows. */
    bool used[CBX_GRID_MAX_ROWS] = {false};
    for (int ni = 0; preserve_live_edits && ni < grid->row_count; ni++) {
        cbx_grid_row *current = &grid->rows[ni];
        int found = -1;
        for (int oi = 0; oi < saved_row_count; oi++) {
            if (used[oi])
                continue;
            bool match = false;
            if (saved_rows[oi].id_stable && current->id_stable) {
                match = saved_rows[oi].id[0] && current->id[0] &&
                        strcmp(saved_rows[oi].id, current->id) == 0;
            } else if (!saved_rows[oi].id_stable && !current->id_stable) {
                match = saved_rows[oi].composite_path[0] &&
                        current->composite_path[0] &&
                        strcmp(saved_rows[oi].composite_path,
                               current->composite_path) == 0;
            }
            if (match) {
                found = oi;
                break;
            }
        }
        if (found < 0)
            continue;
        used[found] = true;
        if (saved_rows[found].cur_col >= 0 &&
            saved_rows[found].cur_col < grid->col_count)
            current->cur_col = saved_rows[found].cur_col;
        else
            current->cur_col = CBX_GRID_UNASSIGNED_COL;
        snprintf(current->profile, sizeof(current->profile), "%s",
                 saved_rows[found].profile);
        grid->row_arrival_seq[ni] = saved_arrivals[found];
        if (grid->row_arrival_seq[ni] >= grid->next_arrival_seq)
            grid->next_arrival_seq = grid->row_arrival_seq[ni] + 1;
    }

    grid->live_edits = preserve_live_edits;

    /* Restore the profile list. */
    cbx_select_grid_clear_profiles(grid);
    for (int i = 0; i < saved_profile_count; i++) {
        rc = cbx_select_grid_add_profile(grid, saved_profiles[i]);
        if (rc != 0)
            return rc;
    }

    return 0;
}

/* --- Extract types from device model ------------------------------------ */

int
cbx_dynamic_columns_extract_types(const cbx_device_model *model,
                                      cbx_dc_type_query_fn query_fn,
                                      char (*out_types)[CBX_MAX_TYPE_LEN],
                                      int *out_count,
                                      int max_types)
{
    if (!model || !query_fn || !out_types || !out_count)
        return -EINVAL;
    if (max_types < 1)
        return -EINVAL;

    int count = model->target_count;
    if (count > max_types)
        count = max_types;
    if (count > CBX_MAX_DEVICES)
        count = CBX_MAX_DEVICES;

    for (int i = 0; i < count; i++) {
        int rc = query_fn(model->targets[i].path,
                          out_types[i], CBX_MAX_TYPE_LEN);
        if (rc != 0)
            return rc;
    }

    *out_count = count;
    return 0;
}