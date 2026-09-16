/*
 * gamepad_order_restore.h — GamepadOrder restoration after restart
 *                           (Task 27, SPEC §10.3 gap #2).
 *
 * InputPlumber's Manager.GamepadOrder is in-memory only and resets to
 * empty on daemon restart (gap #2).  The persistence layer
 * (ip_gamepad_order_save/load, Task 15) saves/loads the order as
 * identity IDs in assignments.yaml.  This module provides the
 * orchestration: after InputPlumber restart and re-enumeration, map the
 * saved IDs back to composite device paths by extracting each composite's
 * source-derived physical identity (composite_identity.h) — never the
 * opaque PersistentId — and re-apply the order via
 * ip_manager_set_gamepad_order().
 *
 * Stale IDs (saved IDs with no matching composite after restart) are
 * skipped.  The caller is informed of restored and skipped counts.
 *
 * This module requires a DBus backend and bus handle — it reads source
 * device properties and calls the GamepadOrder setter.
 */
#ifndef CBX_GAMEPAD_ORDER_RESTORE_H
#define CBX_GAMEPAD_ORDER_RESTORE_H

#include <stdbool.h>
#include <stddef.h>

#include "dbus/dbus_interface.h"            /* ip_dbus_backend, ip_bus_handle */
#include "dbus/ip_device_model.h" /* cbx_device_model */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Restore GamepadOrder after InputPlumber restart.
 *
 * This function should be called after re-enumeration completes (the
 * device model is fully rebuilt from GetManagedObjects).  It:
 *
 *   1. Loads the saved gamepad_order IDs from assignments.yaml.
 *   2. Extracts each composite's physical identity from its source devices
 *      (composite_identity.h) — never from the opaque PersistentId.
 *   3. Matches saved IDs against the extracted identities.
 *   4. Builds a comma-separated list of composite paths in the saved
 *      order (skipping stale IDs that have no matching composite).
 *   5. Calls ip_manager_set_gamepad_order() to re-apply the order.
 *
 * A transient source-property read failure is NOT absence: when any
 * composite's identity query failed, the order is left untouched and
 * -EAGAIN is returned so the caller can retry after recovery instead of
 * applying a misleading (partial or empty) order.
 *
 * @param backend             DBus backend vtable.
 * @param bus                 DBus bus handle.
 * @param model               Freshly re-enumerated device model.
 * @param out_restored_count  Output: number of saved IDs successfully
 *                            matched and restored (may be NULL).
 * @param out_skipped_count   Output: number of saved IDs skipped
 *                            because no matching composite was found
 *                            (may be NULL).
 * @param out_query_failed    Output: true when a transient identity query
 *                            failure made the mapping uncertain (may be
 *                            NULL).
 * @return 0 on success (order restored or empty order applied);
 *         -EINVAL if null args (backend, bus, or model);
 *         -ENOENT if no saved gamepad_order exists (nothing to restore);
 *         -EAGAIN if a transient identity query failure deferred the
 *         restore (saved order untouched);
 *         negative errno from ip_gamepad_order_load or
 *         ip_manager_set_gamepad_order on error.
 */
int cbx_gamepad_order_restore(const ip_dbus_backend *backend,
                               ip_bus_handle bus,
                               const cbx_device_model *model,
                               int *out_restored_count,
                               int *out_skipped_count,
                               bool *out_query_failed);

/*
 * Map saved gamepad_order IDs to composite device paths.
 *
 * For each saved ID, matches against the identities extracted once for the
 * whole model via cbx_model_extract_identities() (BT MAC → USB serial → USB
 * port path → connection order, SPEC §6.2).  Builds a CSV of composite
 * paths in the saved order.  Stale IDs (no match) are counted in
 * *out_skipped_count; a transient identity query failure is reported via
 * *out_query_failed instead of being treated as absence.
 *
 * This is the core mapping logic, exposed for testing.  The caller
 * normally uses cbx_gamepad_order_restore() instead.
 *
 * @param backend           DBus backend vtable.
 * @param bus               DBus bus handle.
 * @param model             Device model with composites.
 * @param saved_ids_csv     Comma-separated saved IDs (from
 *                          ip_gamepad_order_load).
 * @param out_paths_csv     Output buffer for the resulting composite
 *                          path CSV (must be large enough; see
 *                          CBX_MAX_PATH_LEN * CBX_MAX_GAMEPAD_ORDER).
 * @param paths_csv_len     Size of out_paths_csv buffer.
 * @param out_restored_count Output: number of IDs matched.
 * @param out_skipped_count  Output: number of IDs skipped (stale).
 * @param out_query_failed   Output: true when at least one ID could not be
 *                           classified because a composite identity query
 *                           failed transiently (may be NULL).
 * @return 0 on success; -EINVAL null args; -ENOSPC if output buffer
 *         is too small.
 */
int cbx_gamepad_order_map_ids(const ip_dbus_backend *backend,
                               ip_bus_handle bus,
                               const cbx_device_model *model,
                               const char *saved_ids_csv,
                               char *out_paths_csv,
                               size_t paths_csv_len,
                               int *out_restored_count,
                               int *out_skipped_count,
                               bool *out_query_failed);

#ifdef __cplusplus
}
#endif

#endif /* CBX_GAMEPAD_ORDER_RESTORE_H */