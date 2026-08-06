/*
 * ip_target.c — Target device interface wrappers (Task 14).
 *
 * Implements the wrappers declared in ip_target.h.  All wrappers go
 * through the ip_dbus_backend vtable using backend->get_property.
 * Target device properties use IP_IFACE_TARGET as the interface.
 *
 * SPEC §10.2 — Target device interfaces.
 */
#include "ip_target.h"

#include <errno.h>
#include <string.h>

int
ip_target_get_name(const ip_dbus_backend *backend,
                     ip_bus_handle bus,
                     const char *target_path,
                     char **out_value)
{
    if (!backend || !bus || !target_path || !out_value)
        return -EINVAL;

    *out_value = NULL;

    return backend->get_property(bus, IP_DBUS_NAME,
                                   target_path,
                                   IP_IFACE_TARGET,
                                   "Name", out_value);
}

int
ip_target_get_device_type(const ip_dbus_backend *backend,
                            ip_bus_handle bus,
                            const char *target_path,
                            char **out_value)
{
    if (!backend || !bus || !target_path || !out_value)
        return -EINVAL;

    *out_value = NULL;

    return backend->get_property(bus, IP_DBUS_NAME,
                                   target_path,
                                   IP_IFACE_TARGET,
                                   "DeviceType", out_value);
}