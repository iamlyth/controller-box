/*
 * ip_target.h — Target device interface wrappers (Task 14).
 *
 * Thin wrappers around InputPlumber's target device interface
 * (org.shadowblip.Input.Target) at per-device paths like:
 *   /org/shadowblip/InputPlumber/devices/target/gamepad0
 *
 * SPEC §10.2 — Target device interfaces:
 *   org.shadowblip.Input.Target — Name: s, DeviceType: s
 *     DeviceType is the icon-mapping key (§8.1), e.g. "xb360", "ds5",
 *     "deck", "gamepad".
 *
 * All wrappers go through the ip_dbus_backend vtable for unit testability.
 */
#ifndef CBX_IP_TARGET_H
#define CBX_IP_TARGET_H

#include "dbus_mock.h"          /* ip_dbus_backend, ip_bus_handle, constants */

/*
 * Get the Name property from a target device.
 * On success, *out_value is heap-allocated.  Caller must free.
 * Returns 0 on success, negative errno on failure.
 */
int ip_target_get_name(const ip_dbus_backend *backend,
                         ip_bus_handle bus,
                         const char *target_path,
                         char **out_value);

/*
 * Get the DeviceType property from a target device.
 * DeviceType is the icon-mapping key (e.g. "xb360", "ds5", "deck").
 * On success, *out_value is heap-allocated.  Caller must free.
 * Returns 0 on success, negative errno on failure.
 */
int ip_target_get_device_type(const ip_dbus_backend *backend,
                                 ip_bus_handle bus,
                                 const char *target_path,
                                 char **out_value);

#endif /* CBX_IP_TARGET_H */