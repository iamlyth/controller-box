/*
 * ip_manager.h — Manager interface method wrappers (Task 12).
 *
 * Thin wrappers around the InputPlumber Manager interface
 * (org.shadowblip.InputManager) at /org/shadowblip/InputPlumber/Manager.
 * All wrappers go through the ip_dbus_backend vtable so they are fully
 * unit-testable with the mock backend.
 *
 * SPEC §10.2 — Manager interface:
 *   CreateTargetDevice(kind: s) → s    — add virtual controller, returns path
 *   StopTargetDevice(path: s)           — remove virtual controller
 *   AttachTargetDevice(target, comp)    — attach standalone target to composite
 *   SetTargetDevices(types: as)         — replace all target devices (CompositeDevice iface)
 *   GamepadOrder: as (rw)               — player ordering (array of paths)
 *   SupportedTargetDeviceIds: as (r)    — device type IDs for picker
 *   SupportedTargetDevices: as (r)     — human-readable device type names
 *
 * Security:
 *   - GamepadOrder setter validates every path in the value exists in the
 *     device model before calling the DBus setter, preventing the GUI from
 *     sending invalid paths to InputPlumber.
 *   - All wrappers return categorized error codes (IP_ERR_* or negative
 *     errno) on failure.
 */
#ifndef CBX_IP_MANAGER_H
#define CBX_IP_MANAGER_H

#include "dbus_mock.h"          /* ip_dbus_backend, ip_bus_handle, constants */
#include "ip_device_model.h"     /* cbx_device_model for GamepadOrder validation */

/* --- Manager path -------------------------------------------------------- */

#define IP_DBUS_MANAGER_PATH IP_DBUS_PATH "/Manager"

/* --- Method wrappers ----------------------------------------------------- */

/*
 * Create a target device (virtual controller).  `kind` is a device type
 * string (e.g. "xb360", "ds5", "deck").  On success, *out_path is set to
 * the new device's object path (heap-allocated, caller must free).
 * Returns 0 on success, categorized error code on failure.
 */
int ip_manager_create_target_device(const ip_dbus_backend *backend,
                                     ip_bus_handle bus,
                                     const char *kind,
                                     char **out_path);

/*
 * Stop (remove) a target device by object path.
 * Returns 0 on success, categorized error code on failure.
 */
int ip_manager_stop_target_device(const ip_dbus_backend *backend,
                                   ip_bus_handle bus,
                                   const char *path);

/*
 * Attach a standalone target device to a composite device.
 * `target_path` and `composite_path` are full DBus object paths.
 * Returns 0 on success, categorized error code on failure.
 */
int ip_manager_attach_target_device(const ip_dbus_backend *backend,
                                     ip_bus_handle bus,
                                     const char *target_path,
                                     const char *composite_path);

/*
 * Replace all target devices on a composite device.
 * `composite_path` is the DBus path of the composite device.
 * `types_csv` is a comma-separated list of target device type strings
 * (e.g. "xb360,ds5").  This calls the CompositeDevice interface's
 * SetTargetDevices method.
 * Returns 0 on success, categorized error code on failure.
 */
int ip_manager_set_target_devices(const ip_dbus_backend *backend,
                                   ip_bus_handle bus,
                                   const char *composite_path,
                                   const char *types_csv);

/* --- Property wrappers --------------------------------------------------- */

/*
 * Get the GamepadOrder property (comma-separated composite paths).
 * On success, *out_value is set to a heap-allocated comma-separated
 * string of composite device paths.  Caller must free.
 * Returns 0 on success, categorized error code on failure.
 */
int ip_manager_get_gamepad_order(const ip_dbus_backend *backend,
                                  ip_bus_handle bus,
                                  char **out_value);

/*
 * Set the GamepadOrder property.  `value` is a comma-separated list of
 * composite device paths.  Each path is validated against `model` before
 * the DBus call is made — if any path does not correspond to a known
 * composite device, -EINVAL is returned without calling InputPlumber.
 * Returns 0 on success, categorized error code on failure.
 */
int ip_manager_set_gamepad_order(const ip_dbus_backend *backend,
                                  ip_bus_handle bus,
                                  const char *value,
                                  const cbx_device_model *model);

/*
 * Get the SupportedTargetDeviceIds property (comma-separated IDs).
 * On success, *out_value is heap-allocated.  Caller must free.
 * Returns 0 on success, categorized error code on failure.
 */
int ip_manager_get_supported_target_device_ids(const ip_dbus_backend *backend,
                                                ip_bus_handle bus,
                                                char **out_value);

/*
 * Get the SupportedTargetDevices property (comma-readable names).
 * On success, *out_value is heap-allocated.  Caller must free.
 * Returns 0 on success, categorized error code on failure.
 */
int ip_manager_get_supported_target_devices(const ip_dbus_backend *backend,
                                              ip_bus_handle bus,
                                              char **out_value);

#endif /* CBX_IP_MANAGER_H */