/*
 * ip_gamepad_order.h — GamepadOrder persistence layer (Task 15, gap #2).
 *
 * InputPlumber's Manager.GamepadOrder property is in-memory only and
 * resets to empty on daemon restart (gap #2, SPEC §10.3).  This module
 * provides the persistence layer:
 *
 *   - ip_gamepad_order_save(): Takes the current GamepadOrder from DBus
 *     (comma-separated composite device paths), queries PersistentId for
 *     each composite, and saves the IDs to assignments.yaml.
 *   - ip_gamepad_order_load(): Reads the saved gamepad order from
 *     assignments.yaml and returns the IDs as a CSV string.
 *
 * Stale device paths (composite paths not in the device model) are
 * skipped during save.  Invalid IDs are skipped during load.
 *
 * Orchestration of restart re-application (mapping IDs back to composite
 * paths and calling the DBus setter) is deferred to Task 27.
 */
#ifndef CBX_IP_GAMEPAD_ORDER_H
#define CBX_IP_GAMEPAD_ORDER_H

#include "dbus_mock.h"          /* ip_dbus_backend, ip_bus_handle */
#include "dbus/ip_device_model.h"
#include "config/config_assignments.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Save the current GamepadOrder to assignments.yaml.
 *
 * Iterates the comma-separated composite device paths, verifies each
 * path exists in the device model (skips stale paths), queries
 * PersistentId via the DBus backend, and stores the resulting IDs in
 * assignments.yaml's gamepad_order array.  Existing assignment entries
 * are preserved.
 *
 * @param backend     DBus backend vtable (for PersistentId queries).
 * @param bus         DBus bus handle.
 * @param model       Device model (to verify composite paths exist).
 * @param paths_csv   Comma-separated composite device paths from the
 *                    GamepadOrder property (may be empty to clear).
 * @return 0 on success; negative errno on error.
 */
int ip_gamepad_order_save(const ip_dbus_backend *backend,
                           ip_bus_handle bus,
                           const cbx_device_model *model,
                           const char *paths_csv);

/*
 * Load the saved gamepad order from assignments.yaml.
 *
 * Reads assignments.yaml and returns the gamepad_order entries as a
 * comma-separated string of IDs.  Entries that fail ID validation are
 * skipped.
 *
 * @param out_csv  On success, receives a heap-allocated CSV string of
 *                 gamepad order IDs (caller frees).  Empty string if no
 *                 saved order.  Set to NULL on entry.
 * @return 0 on success; negative errno on error.
 */
int ip_gamepad_order_load(char **out_csv);

#ifdef __cplusplus
}
#endif

#endif /* CBX_IP_GAMEPAD_ORDER_H */