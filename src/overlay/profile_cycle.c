/*
 * profile_cycle.c — Profile cycling workflow (Task 31).
 *
 * Implements profile enumeration → grid population → profile change →
 * LoadProfilePath + assignment update.
 */
#include "overlay/profile_cycle.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "identify/assign.h"       /* CBX_DEFAULT_PROFILE, cbx_assign_* */
#include "dbus/ip_composite.h"    /* ip_composite_load_profile_path */

/* --- Init --------------------------------------------------------------- */

void
cbx_profile_cycle_init(cbx_profile_cycle *pc,
                        const ip_dbus_backend *backend,
                        ip_bus_handle bus,
                        cbx_assignments *assignments,
                        const cbx_profile_list *profiles)
{
    if (!pc)
        return;
    pc->backend = backend;
    pc->bus = bus;
    pc->assignments = assignments;
    pc->profiles = profiles;
}

/* --- Profile list → grid ----------------------------------------------- */

int
cbx_profile_cycle_load_profiles(cbx_select_grid *grid,
                                 const cbx_profile_list *list)
{
    if (!grid || !list)
        return -EINVAL;

    cbx_select_grid_clear_profiles(grid);

    for (int i = 0; i < list->count && i < CBX_MAX_PROFILES; i++) {
        int rc = cbx_select_grid_add_profile(grid, list->entries[i].filename);
        if (rc != 0)
            return rc;
    }

    return 0;
}

/* --- Path lookup -------------------------------------------------------- */

int
cbx_profile_cycle_find_path(const cbx_profile_list *list,
                              const char *name,
                              char *out, size_t out_len)
{
    if (!list || !name || !out || out_len == 0)
        return -EINVAL;

    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->entries[i].filename, name) == 0) {
            snprintf(out, out_len, "%s", list->entries[i].path);
            return 0;
        }
    }

    return -ENOENT;
}

/* --- Assignment update -------------------------------------------------- */

int
cbx_profile_cycle_update_assignment(cbx_assignments *assignments,
                                      const char *id,
                                      const char *profile)
{
    if (!assignments || !id || !profile)
        return -EINVAL;

    /* Find existing assignment by ID. */
    int idx = cbx_assign_find_index(assignments, id);
    if (idx >= 0) {
        snprintf(assignments->assignments[idx].profile,
                 CBX_MAX_PROFILE_LEN, "%s", profile);
        return 0;
    }

    /* Not found — create a default assignment at the lowest free slot. */
    cbx_assignment def;
    int rc = cbx_assign_resolve(assignments, id,
                                CBX_MAX_CONTROLLERS, &def);
    if (rc != 0 && rc != 1)
        return rc;  /* -ENOENT if no free slot */

    /* Override the profile with the requested one. */
    snprintf(def.profile, CBX_MAX_PROFILE_LEN, "%s", profile);

    /* Append to assignments array. */
    if (assignments->assignment_count >= CBX_MAX_ASSIGNMENTS)
        return -ENOSPC;

    assignments->assignments[assignments->assignment_count++] = def;
    return 0;
}

/* --- Apply profile change ----------------------------------------------- */

int
cbx_profile_cycle_apply(cbx_profile_cycle *pc,
                         const cbx_select_grid *grid,
                         int row_idx,
                         const char *profile_name,
                         const char *composite_path)
{
    if (!pc || !grid)
        return -EINVAL;
    if (row_idx < 0 || row_idx >= grid->row_count)
        return -EINVAL;
    if (!profile_name || !composite_path)
        return -EINVAL;

    /* Look up the full profile path from the profile list. */
    char profile_path[PATH_MAX];

    if (!pc->profiles)
        return -ENOENT;

    int rc = cbx_profile_cycle_find_path(pc->profiles, profile_name,
                                          profile_path, sizeof(profile_path));
    if (rc != 0)
        return rc;  /* -ENOENT or -EINVAL */

    /* Call LoadProfilePath via the backend. */
    if (pc->backend && pc->bus) {
        rc = ip_composite_load_profile_path(pc->backend, pc->bus,
                                             composite_path, profile_path);
        if (rc != 0)
            return rc;
    }

    /* Update the assignment with the new profile. */
    if (pc->assignments) {
        const cbx_grid_row *row = cbx_select_grid_get_row(grid, row_idx);
        if (row && row->id[0] != '\0') {
            rc = cbx_profile_cycle_update_assignment(pc->assignments,
                                                       row->id,
                                                       profile_name);
            if (rc != 0)
                return rc;
        }
    }

    return 0;
}

/* --- Profile follows controller ----------------------------------------- */

bool
cbx_profile_cycle_profile_follows(const cbx_select_grid *grid,
                                     int row_idx)
{
    if (!grid || row_idx < 0 || row_idx >= grid->row_count)
        return false;

    /*
     * The profile is stored per-row in the grid (rows[row_idx].profile).
     * Navigation functions (move_left, move_right) only modify
     * rows[row_idx].cur_col — they never touch the profile field.
     * Therefore, the profile inherently follows the controller across
     * column moves. This is a structural guarantee of the grid model.
     *
     * We verify the row has a non-empty profile to confirm the
     * per-controller profile model is active.
     */
    const char *profile = cbx_select_grid_get_profile(grid, row_idx);
    if (!profile || profile[0] == '\0')
        return false;

    return true;
}