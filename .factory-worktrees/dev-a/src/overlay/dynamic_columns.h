/*
 * dynamic_columns.h — Dynamic column management from device model (Task 31).
 *
 * SPEC §4.7 — The column count scales with the number of virtual (target)
 * controllers InputPlumber has instantiated: a MAME cabinet with 6 virtual
 * controllers shows 6 columns; a 2-player setup shows 2 (+ Unassigned).
 * When the manager adds/removes a virtual controller, the overlay reflects
 * the new column count the next time it opens.
 *
 * SPEC §5.2 — Removing a slot mid-session: the physical controller in that
 * slot auto-moves to Unassigned; the overlay's column count adjusts
 * dynamically.
 *
 * This module bridges the device model (target devices) and the select grid
 * (columns).  The caller queries each target device's DeviceType (via
 * ip_target_get_device_type) and passes the results as a type array.
 * The module builds a cbx_virtual_controllers-compatible struct and
 * calls cbx_select_grid_build with it, preserving assignments and profiles.
 *
 * The module is pure data (no DBus, no file I/O) for testability.
 */
#ifndef CBX_OVERLAY_DYNAMIC_COLUMNS_H
#define CBX_OVERLAY_DYNAMIC_COLUMNS_H

#include <stdbool.h>

#include "config/config_assignments.h"
#include "config/config_settings.h"
#include "dbus/ip_device_model.h"
#include "overlay/grid_render.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Limits -------------------------------------------------------------- */

/* Maximum target device types we handle (matches CBX_MAX_CONTROLLERS). */
#define CBX_DC_MAX_TYPES CBX_MAX_CONTROLLERS

/* --- API ---------------------------------------------------------------- */

/*
 * Build a cbx_virtual_controllers from target device types.
 *
 * @param types       Array of device type strings (e.g. "xb360", "ds5").
 * @param type_count  Number of type entries (1..CBX_MAX_CONTROLLERS).
 * @param out         Output virtual_controllers struct.
 * @return 0 on success, -EINVAL on bad args, -ENOSPC if type_count > max.
 */
int cbx_dynamic_columns_build_vcs(const char (*types)[CBX_MAX_TYPE_LEN],
                                     int type_count,
                                     cbx_virtual_controllers *out);

/*
 * Check if the grid needs a rebuild because the target count changed.
 *
 * @param grid          The current select grid.
 * @param target_count  Current number of target devices from device model.
 * @return true if col_count != target_count + 1 (needs rebuild),
 *         false otherwise.
 */
bool cbx_dynamic_columns_needs_rebuild(const cbx_select_grid *grid,
                                          int target_count);

/*
 * Rebuild the grid with columns from target device types.
 *
 * Builds a cbx_settings with virtual_controllers from the target types,
 * then calls cbx_select_grid_build.  Row positions are re-derived from
 * assignments (controllers on removed slots move to Unassigned).
 * Profile list is preserved across rebuilds.
 *
 * @param grid              The select grid to rebuild (profile list preserved).
 * @param target_types      Array of device type strings per target.
 * @param target_type_count Number of target types.
 * @param composites        Composite device info for rows.
 * @param composite_count   Number of composite devices.
 * @param assignments       Current assignments (for slot/profile lookup).
 * @return 0 on success, negative errno on error.
 */
int cbx_dynamic_columns_rebuild(cbx_select_grid *grid,
                                  const char (*target_types)[CBX_MAX_TYPE_LEN],
                                  int target_type_count,
                                  const cbx_grid_composite_info *composites,
                                  int composite_count,
                                  const cbx_assignments *assignments);

/*
 * Clamp row positions after a column count decrease.
 *
 * Any row whose cur_col >= new_col_count is moved to Unassigned (col 0).
 * This handles the case where a target device was removed and controllers
 * in the removed slot need to move to Unassigned.
 *
 * @param grid          The select grid.
 * @param new_col_count The new column count (including Unassigned col 0).
 * @return Number of rows that were clamped (moved to Unassigned).
 */
int cbx_dynamic_columns_clamp_positions(cbx_select_grid *grid,
                                          int new_col_count);

/*
 * Extract target device types from the device model.
 *
 * Convenience function that maps model->targets[] to a type array.
 * The caller must provide a callback to query each target's DeviceType
 * (typically ip_target_get_device_type, but mockable for testing).
 *
 * @param model        The device model.
 * @param backend      DBus backend for querying DeviceType.
 * @param bus          DBus bus handle.
 * @param out_types    Output array of type strings.
 * @param out_count    Output number of types.
 * @param max_types    Capacity of out_types array.
 * @return 0 on success, negative errno on error.
 */
typedef int (*cbx_dc_type_query_fn)(const char *target_path, char *out, size_t out_len);

int cbx_dynamic_columns_extract_types(const cbx_device_model *model,
                                        cbx_dc_type_query_fn query_fn,
                                        char (*out_types)[CBX_MAX_TYPE_LEN],
                                        int *out_count,
                                        int max_types);

#ifdef __cplusplus
}
#endif

#endif /* CBX_OVERLAY_DYNAMIC_COLUMNS_H */