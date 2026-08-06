/*
 * ip_composite.c — CompositeDevice interface wrappers (Task 13).
 *
 * Implements the wrappers declared in ip_composite.h.  All wrappers go
 * through the ip_dbus_backend vtable:
 *
 *   - Method calls: backend->call_method(bus, dest, path, iface, method, sig, ...)
 *     The last variadic argument is always a char **out_value (NULL for
 *     void methods, a valid pointer for methods that return a string).
 *   - Property gets: backend->get_property(bus, dest, path, iface, prop, &out)
 *   - Property sets: backend->set_property(bus, dest, path, iface, prop, value)
 *
 * SPEC §10.2 — CompositeDevice interface.
 */
#include "ip_composite.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* --- Method wrappers ----------------------------------------------------- */

int
ip_composite_set_intercept_activation(const ip_dbus_backend *backend,
                                        ip_bus_handle bus,
                                        const char *composite_path,
                                        const char *events_csv,
                                        const char *target_event)
{
    if (!backend || !composite_path || !events_csv || !target_event)
        return -EINVAL;

    return backend->call_method(bus, IP_DBUS_NAME,
                                  composite_path,
                                  IP_IFACE_COMPOSITE,
                                  "SetInterceptActivation",
                                  "ass", events_csv, target_event,
                                  NULL);
}

int
ip_composite_load_profile_path(const ip_dbus_backend *backend,
                                  ip_bus_handle bus,
                                  const char *composite_path,
                                  const char *profile_path)
{
    if (!backend || !composite_path || !profile_path)
        return -EINVAL;

    return backend->call_method(bus, IP_DBUS_NAME,
                                  composite_path,
                                  IP_IFACE_COMPOSITE,
                                  "LoadProfilePath",
                                  "s", profile_path, NULL);
}

int
ip_composite_load_profile_from_yaml(const ip_dbus_backend *backend,
                                        ip_bus_handle bus,
                                        const char *composite_path,
                                        const char *yaml)
{
    if (!backend || !composite_path || !yaml)
        return -EINVAL;

    return backend->call_method(bus, IP_DBUS_NAME,
                                  composite_path,
                                  IP_IFACE_COMPOSITE,
                                  "LoadProfileFromYaml",
                                  "s", yaml, NULL);
}

int
ip_composite_get_profile_yaml(const ip_dbus_backend *backend,
                                ip_bus_handle bus,
                                const char *composite_path,
                                char **out_yaml)
{
    if (!backend || !composite_path || !out_yaml)
        return -EINVAL;

    *out_yaml = NULL;

    return backend->call_method(bus, IP_DBUS_NAME,
                                  composite_path,
                                  IP_IFACE_COMPOSITE,
                                  "GetProfileYaml",
                                  "", out_yaml);
}

int
ip_composite_set_target_devices(const ip_dbus_backend *backend,
                                  ip_bus_handle bus,
                                  const char *composite_path,
                                  const char *types_csv)
{
    if (!backend || !composite_path || !types_csv)
        return -EINVAL;

    return backend->call_method(bus, IP_DBUS_NAME,
                                  composite_path,
                                  IP_IFACE_COMPOSITE,
                                  "SetTargetDevices",
                                  "as", types_csv, NULL);
}

int
ip_composite_stop(const ip_dbus_backend *backend,
                   ip_bus_handle bus,
                   const char *composite_path)
{
    if (!backend || !composite_path)
        return -EINVAL;

    return backend->call_method(bus, IP_DBUS_NAME,
                                  composite_path,
                                  IP_IFACE_COMPOSITE,
                                  "Stop",
                                  "", NULL);
}

/* --- Property wrappers -------------------------------------------------- */

int
ip_composite_get_intercept_mode(const ip_dbus_backend *backend,
                                   ip_bus_handle bus,
                                   const char *composite_path,
                                   char **out_value)
{
    if (!backend || !composite_path || !out_value)
        return -EINVAL;

    *out_value = NULL;

    return backend->get_property(bus, IP_DBUS_NAME,
                                   composite_path,
                                   IP_IFACE_COMPOSITE,
                                   "InterceptMode", out_value);
}

int
ip_composite_set_intercept_mode(const ip_dbus_backend *backend,
                                   ip_bus_handle bus,
                                   const char *composite_path,
                                   const char *value)
{
    if (!backend || !composite_path || !value)
        return -EINVAL;

    return backend->set_property(bus, IP_DBUS_NAME,
                                   composite_path,
                                   IP_IFACE_COMPOSITE,
                                   "InterceptMode", value);
}

int
ip_composite_get_target_devices(const ip_dbus_backend *backend,
                                   ip_bus_handle bus,
                                   const char *composite_path,
                                   char **out_value)
{
    if (!backend || !composite_path || !out_value)
        return -EINVAL;

    *out_value = NULL;

    return backend->get_property(bus, IP_DBUS_NAME,
                                   composite_path,
                                   IP_IFACE_COMPOSITE,
                                   "TargetDevices", out_value);
}

int
ip_composite_get_source_device_paths(const ip_dbus_backend *backend,
                                        ip_bus_handle bus,
                                        const char *composite_path,
                                        char **out_value)
{
    if (!backend || !composite_path || !out_value)
        return -EINVAL;

    *out_value = NULL;

    return backend->get_property(bus, IP_DBUS_NAME,
                                   composite_path,
                                   IP_IFACE_COMPOSITE,
                                   "SourceDevicePaths", out_value);
}

int
ip_composite_get_persistent_id(const ip_dbus_backend *backend,
                                  ip_bus_handle bus,
                                  const char *composite_path,
                                  char **out_value)
{
    if (!backend || !composite_path || !out_value)
        return -EINVAL;

    *out_value = NULL;

    return backend->get_property(bus, IP_DBUS_NAME,
                                   composite_path,
                                   IP_IFACE_COMPOSITE,
                                   "PersistentId", out_value);
}

int
ip_composite_get_name(const ip_dbus_backend *backend,
                        ip_bus_handle bus,
                        const char *composite_path,
                        char **out_value)
{
    if (!backend || !composite_path || !out_value)
        return -EINVAL;

    *out_value = NULL;

    return backend->get_property(bus, IP_DBUS_NAME,
                                   composite_path,
                                   IP_IFACE_COMPOSITE,
                                   "Name", out_value);
}

int
ip_composite_get_capabilities(const ip_dbus_backend *backend,
                                ip_bus_handle bus,
                                const char *composite_path,
                                char **out_value)
{
    if (!backend || !composite_path || !out_value)
        return -EINVAL;

    *out_value = NULL;

    return backend->get_property(bus, IP_DBUS_NAME,
                                   composite_path,
                                   IP_IFACE_COMPOSITE,
                                   "Capabilities", out_value);
}

int
ip_composite_get_output_capabilities(const ip_dbus_backend *backend,
                                         ip_bus_handle bus,
                                         const char *composite_path,
                                         char **out_value)
{
    if (!backend || !composite_path || !out_value)
        return -EINVAL;

    *out_value = NULL;

    return backend->get_property(bus, IP_DBUS_NAME,
                                   composite_path,
                                   IP_IFACE_COMPOSITE,
                                   "OutputCapabilities", out_value);
}

int
ip_composite_get_target_capabilities(const ip_dbus_backend *backend,
                                         ip_bus_handle bus,
                                         const char *composite_path,
                                         char **out_value)
{
    if (!backend || !composite_path || !out_value)
        return -EINVAL;

    *out_value = NULL;

    return backend->get_property(bus, IP_DBUS_NAME,
                                   composite_path,
                                   IP_IFACE_COMPOSITE,
                                   "TargetCapabilities", out_value);
}

int
ip_composite_get_dbus_devices(const ip_dbus_backend *backend,
                                ip_bus_handle bus,
                                const char *composite_path,
                                char **out_value)
{
    if (!backend || !composite_path || !out_value)
        return -EINVAL;

    *out_value = NULL;

    return backend->get_property(bus, IP_DBUS_NAME,
                                   composite_path,
                                   IP_IFACE_COMPOSITE,
                                   "DbusDevices", out_value);
}