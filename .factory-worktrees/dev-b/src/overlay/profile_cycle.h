/*
 * profile_cycle.h — Profile cycling workflow (Task 31).
 *
 * Coordinates profile enumeration → grid population → profile change →
 * LoadProfilePath + assignment update.
 *
 * SPEC §4.6 — Profiles are per-controller, not per-slot.  A controller's
 * profile follows it across columns.  The grid stores profile per-row
 * (per-controller), so moving columns preserves the profile.  Only
 * Up/Down changes the profile (via cycle_profile_up/down).
 *
 * SPEC §10.2 — LoadProfilePath(path: s) loads a profile from a filesystem
 * path.  The profile list (Task 8 enumeration) provides the full path for
 * each profile name.  On profile change, this module looks up the path
 * and calls ip_composite_load_profile_path via the backend vtable.
 *
 * SPEC §7.2 — Profiles live in ~/.local/share/inputplumber/profiles/
 * (user, read/write) and /usr/share/inputplumber/profiles/ (system,
 * read-only).  The profile list provides full paths for both sources.
 *
 * The module is testable with the mock backend (no real DBus) and a
 * cbx_profile_list constructed in-memory.
 */
#ifndef CBX_OVERLAY_PROFILE_CYCLE_H
#define CBX_OVERLAY_PROFILE_CYCLE_H

#include <stdbool.h>
#include <stddef.h>

#include "config/config_assignments.h"
#include "config/config_profile_list.h"
#include "dbus/dbus_interface.h"          /* ip_dbus_backend, ip_bus_handle */
#include "overlay/grid_render.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Profile cycle context ----------------------------------------------- */

typedef struct {
    const ip_dbus_backend    *backend;
    ip_bus_handle              bus;
    cbx_assignments           *assignments;  /* updated on profile change */
    const cbx_profile_list    *profiles;     /* for path lookup */
} cbx_profile_cycle;

/* --- API ---------------------------------------------------------------- */

/*
 * Initialise the profile cycle context.
 * backend/bus may be NULL (skips LoadProfilePath call).
 * assignments may be NULL (skips assignment update).
 * profiles may be NULL (apply returns -ENOENT).
 */
void cbx_profile_cycle_init(cbx_profile_cycle *pc,
                             const ip_dbus_backend *backend,
                             ip_bus_handle bus,
                             cbx_assignments *assignments,
                             const cbx_profile_list *profiles);

/*
 * Populate the grid's profile list from a profile enumeration.
 * Clears existing profiles, then adds each profile filename.
 * Returns 0 on success, -EINVAL on bad args.
 */
int cbx_profile_cycle_load_profiles(cbx_select_grid *grid,
                                     const cbx_profile_list *list);

/*
 * Find the full filesystem path for a profile by name.
 * Searches the profile list entries for a matching filename.
 *
 * @param list   Profile enumeration (from cbx_profile_list_enumerate).
 * @param name   Profile filename (without .yaml extension).
 * @param out    Output buffer for the full path.
 * @param out_len Size of out buffer.
 * @return 0 on success, -ENOENT if profile not found, -EINVAL on bad args.
 */
int cbx_profile_cycle_find_path(const cbx_profile_list *list,
                                  const char *name,
                                  char *out, size_t out_len);

/*
 * Apply a profile change for a controller (full workflow).
 *
 * Called from the player_mode on_profile_change callback.  This function:
 *   1. Looks up the full profile path from the profile list.
 *   2. Calls ip_composite_load_profile_path via the backend (if backend set).
 *   3. Updates the assignment for this controller's identity (if assignments set).
 *
 * @param pc             Profile cycle context.
 * @param grid           The select grid (for identity ID lookup).
 * @param row_idx        Grid row index of the controller.
 * @param profile_name   New profile filename (without .yaml).
 * @param composite_path DBus object path for LoadProfilePath.
 * @return 0 on success, -ENOENT if profile not in list, negative errno otherwise.
 */
int cbx_profile_cycle_apply(cbx_profile_cycle *pc,
                             const cbx_select_grid *grid,
                             int row_idx,
                             const char *profile_name,
                             const char *composite_path);

/*
 * Update the assignment for a controller when its profile changes.
 * Finds the assignment by identity ID and updates the profile field.
 * If the controller is not in the assignments, creates a default
 * assignment (lowest free slot, or slot 0).
 *
 * @param assignments  The assignments struct to update.
 * @param id           Controller identity ID.
 * @param profile      New profile name.
 * @return 0 on success, negative errno on error.
 */
int cbx_profile_cycle_update_assignment(cbx_assignments *assignments,
                                          const char *id,
                                          const char *profile);

/*
 * Verify that a controller's profile follows it across columns.
 *
 * The profile is stored per-row in the grid, so it inherently follows
 * the controller across column moves (Left/Right only changes cur_col).
 * This function checks that the profile field is non-NULL and non-empty
 * for the given row, confirming the per-controller profile model.
 *
 * @param grid    The select grid.
 * @param row_idx  Row index of the controller to check.
 * @return true if the row has a profile (profile follows controller),
 *         false on bad args or empty profile.
 */
bool cbx_profile_cycle_profile_follows(const cbx_select_grid *grid,
                                         int row_idx);

#ifdef __cplusplus
}
#endif

#endif /* CBX_OVERLAY_PROFILE_CYCLE_H */