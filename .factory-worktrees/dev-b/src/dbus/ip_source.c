/*
 * ip_source.c — Source device interface wrappers (Task 14).
 *
 * Implements the wrappers declared in ip_source.h.  All wrappers go
 * through the ip_dbus_backend vtable using backend->get_property.
 *
 * The source device interface varies by device type:
 *   - EventDevice (evdev) — has UniqueId, PhysPath, IdBustype
 *   - UdevDevice (udev)   — has UniqueId, PhysPath, IdBustype
 *   - HIDRawDevice        — has SerialNumber (not UniqueId)
 *
 * The caller passes the appropriate interface; the wrapper simply
 * forwards the property read to the backend.
 *
 * SPEC §10.2 — Source device interfaces (identification layer, §6).
 */
#include "ip_source.h"

#include <errno.h>
#include <string.h>

/* --- Property wrappers --------------------------------------------------- */

int
ip_source_get_name(const ip_dbus_backend *backend,
                    ip_bus_handle bus,
                    const char *source_path,
                    const char *iface,
                    char **out_value)
{
    if (!backend || !bus || !source_path || !iface || !out_value)
        return -EINVAL;

    *out_value = NULL;

    return backend->get_property(bus, IP_DBUS_NAME,
                                   source_path,
                                   iface,
                                   "Name", out_value);
}

int
ip_source_get_unique_id(const ip_dbus_backend *backend,
                         ip_bus_handle bus,
                         const char *source_path,
                         const char *iface,
                         char **out_value)
{
    if (!backend || !bus || !source_path || !iface || !out_value)
        return -EINVAL;

    *out_value = NULL;

    return backend->get_property(bus, IP_DBUS_NAME,
                                   source_path,
                                   iface,
                                   "UniqueId", out_value);
}

int
ip_source_get_phys_path(const ip_dbus_backend *backend,
                         ip_bus_handle bus,
                         const char *source_path,
                         const char *iface,
                         char **out_value)
{
    if (!backend || !bus || !source_path || !iface || !out_value)
        return -EINVAL;

    *out_value = NULL;

    return backend->get_property(bus, IP_DBUS_NAME,
                                   source_path,
                                   iface,
                                   "PhysPath", out_value);
}

int
ip_source_get_id_vendor(const ip_dbus_backend *backend,
                          ip_bus_handle bus,
                          const char *source_path,
                          const char *iface,
                          char **out_value)
{
    if (!backend || !bus || !source_path || !iface || !out_value)
        return -EINVAL;

    *out_value = NULL;

    return backend->get_property(bus, IP_DBUS_NAME,
                                   source_path,
                                   iface,
                                   "IdVendor", out_value);
}

int
ip_source_get_id_product(const ip_dbus_backend *backend,
                           ip_bus_handle bus,
                           const char *source_path,
                           const char *iface,
                           char **out_value)
{
    if (!backend || !bus || !source_path || !iface || !out_value)
        return -EINVAL;

    *out_value = NULL;

    return backend->get_property(bus, IP_DBUS_NAME,
                                   source_path,
                                   iface,
                                   "IdProduct", out_value);
}

int
ip_source_get_id_bustype(const ip_dbus_backend *backend,
                          ip_bus_handle bus,
                          const char *source_path,
                          const char *iface,
                          char **out_value)
{
    if (!backend || !bus || !source_path || !iface || !out_value)
        return -EINVAL;

    *out_value = NULL;

    return backend->get_property(bus, IP_DBUS_NAME,
                                   source_path,
                                   iface,
                                   "IdBustype", out_value);
}

int
ip_source_get_serial_number(const ip_dbus_backend *backend,
                              ip_bus_handle bus,
                              const char *source_path,
                              const char *iface,
                              char **out_value)
{
    if (!backend || !bus || !source_path || !iface || !out_value)
        return -EINVAL;

    *out_value = NULL;

    return backend->get_property(bus, IP_DBUS_NAME,
                                   source_path,
                                   iface,
                                   "SerialNumber", out_value);
}