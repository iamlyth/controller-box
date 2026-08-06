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
    if (!grid)
        return false;

    int expected_cols = target_count + 1;  /* +1 for Unassigned */
    return grid->col_count != expected_cols;
}

/* --- Clamp positions ---------------------------------------------------- */

int
cbx_dynamic_columns_clamp_positions(cbx_select_grid *grid,
                                      int new_col_count)
{
    if (!grid)
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
    if (!grid || !target_types || !composites || !assignments)
        return -EINVAL;
    if (target_type_count < 1 || target_type_count > CBX_MAX_CONTROLLERS)
        return -EINVAL;
    if (composite_count < 0 || composite_count > CBX_GRID_MAX_ROWS)
        return -EINVAL;

    /* Preserve the profile list across rebuilds. */
    char saved_profiles[CBX_GRID_MAX_PROFILES][CBX_GRID_PROFILE_LEN];
    int saved_profile_count = grid->profile_count;
    if (saved_profile_count > CBX_GRID_MAX_PROFILES)
        saved_profile_count = CBX_GRID_MAX_PROFILES;
    memcpy(saved_profiles, grid->profiles, sizeof(saved_profiles));

    /* Build virtual_controllers from target types. */
    cbx_virtual_controllers vc;
    int rc = cbx_dynamic_columns_build_vcs(target_types, target_type_count,
                                             &vc);
    if (rc != 0)
        return rc;

    /* Construct a settings struct with just virtual_controllers.
     * Other fields are defaults — grid_build only uses virtual_controllers. */
    cbx_settings settings;
    memset(&settings, 0, sizeof(settings));
    settings.virtual_controllers = vc;

    /* Rebuild the grid (this re-derives row positions from assignments
     * and clamps any slots that no longer exist to Unassigned). */
    rc = cbx_select_grid_build(grid, composites, composite_count,
                                &settings, assignments);
    if (rc != 0)
        return rc;

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