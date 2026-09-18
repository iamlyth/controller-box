/*
 * ip_source.h — Source device interface wrappers (Task 14).
 *
 * Thin wrappers around InputPlumber source device interfaces:
 *   - org.shadowblip.Input.Source.EventDevice  (evdev source devices)
 *   - org.shadowblip.Input.Source.UdevDevice   (udev source devices)
 *   - org.shadowblip.Input.Source.HIDRawDevice (HID raw source devices)
 *
 * Source device paths look like:
 *   /org/shadowblip/InputPlumber/devices/source/event0
 *   /org/shadowblip/InputPlumber/devices/source/hidraw0
 *
 * SPEC §10.2 — Source device interfaces (identification layer, §6):
 *   EventDevice / UdevDevice (all read):
 *     Name, PhysPath, IdVendor, IdProduct, UniqueId, SysfsPath,
 *     DevicePath, IdBustype, IdVersion, plus capability arrays.
 *   HIDRawDevice (all read):
 *     SerialNumber, Manufacturer, Product, IdVendor, IdProduct,
 *     InterfaceNumber.
 *
 * Note: serial is UniqueId on evdev/udev but SerialNumber on HIDRaw —
 * the caller must pass the correct interface.
 *
 * All wrappers go through the ip_dbus_backend vtable for unit testability.
 */
#ifndef CBX_IP_SOURCE_H
#define CBX_IP_SOURCE_H

#include "dbus_interface.h"          /* ip_dbus_backend, ip_bus_handle, constants */

/*
 * Get the Name property from a source device.
 * `iface` must be IP_IFACE_SOURCE_EVENT, IP_IFACE_SOURCE_UDEV, or
 * IP_IFACE_SOURCE_HIDRAW.
 * On success, *out_value is heap-allocated.  Caller must free.
 * Returns 0 on success, negative errno on failure.
 */
int ip_source_get_name(const ip_dbus_backend *backend,
                         ip_bus_handle bus,
                         const char *source_path,
                         const char *iface,
                         char **out_value);

/*
 * Get the UniqueId property from a source device (evdev/udev only).
 * On success, *out_value is heap-allocated.  Caller must free.
 * Returns 0 on success, negative errno on failure.
 */
int ip_source_get_unique_id(const ip_dbus_backend *backend,
                              ip_bus_handle bus,
                              const char *source_path,
                              const char *iface,
                              char **out_value);

/*
 * Get the PhysPath property from a source device (evdev/udev only).
 * On success, *out_value is heap-allocated.  Caller must free.
 * Returns 0 on success, negative errno on failure.
 */
int ip_source_get_phys_path(const ip_dbus_backend *backend,
                              ip_bus_handle bus,
                              const char *source_path,
                              const char *iface,
                              char **out_value);

/*
 * Get the IdVendor property from a source device.
 * On success, *out_value is heap-allocated.  Caller must free.
 * Returns 0 on success, negative errno on failure.
 */
int ip_source_get_id_vendor(const ip_dbus_backend *backend,
                              ip_bus_handle bus,
                              const char *source_path,
                              const char *iface,
                              char **out_value);

/*
 * Get the IdProduct property from a source device.
 * On success, *out_value is heap-allocated.  Caller must free.
 * Returns 0 on success, negative errno on failure.
 */
int ip_source_get_id_product(const ip_dbus_backend *backend,
                               ip_bus_handle bus,
                               const char *source_path,
                               const char *iface,
                               char **out_value);

/*
 * Get the IdBustype property from a source device (evdev/udev only).
 * On success, *out_value is heap-allocated.  Caller must free.
 * Returns 0 on success, negative errno on failure.
 */
int ip_source_get_id_bustype(const ip_dbus_backend *backend,
                               ip_bus_handle bus,
                               const char *source_path,
                               const char *iface,
                               char **out_value);

/*
 * Get the SerialNumber property from a source device (HIDRaw only).
 * On success, *out_value is heap-allocated.  Caller must free.
 * Returns 0 on success, negative errno on failure.
 */
int ip_source_get_serial_number(const ip_dbus_backend *backend,
                                  ip_bus_handle bus,
                                  const char *source_path,
                                  const char *iface,
                                  char **out_value);

#endif /* CBX_IP_SOURCE_H */