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
#include "ip_connection.h"   /* IP_ERR_UNKNOWN_PROPERTY */
#include "ip_manager.h"      /* ip_manager_attach_target_device fallback */

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
ip_composite_set_target_device_paths(const ip_dbus_backend *backend,
                                       ip_bus_handle bus,
                                       const char *composite_path,
                                       const char *paths_csv)
{
    if (!backend || !composite_path || !paths_csv)
        return -EINVAL;

    int rc = backend->set_property(bus, IP_DBUS_NAME,
                                  composite_path,
                                  IP_IFACE_COMPOSITE,
                                  "TargetDevices", paths_csv);
    if (rc != IP_ERR_UNKNOWN_PROPERTY && rc != IP_ERR_UNKNOWN_INTERFACE &&
        rc != IP_ERR_PROPERTY_READ_ONLY)
        return rc;

    /* Live InputPlumber >= 0.78 exposes TargetDevices as a *read-only*
     * property (the object server answers Properties.Set with
     * UnknownProperty), so path-based replacement must use the supported
     * method surface instead:
     *   - SetTargetDevices(target_device_types) replaces the composite's
     *     target devices; an empty type list clears them, and
     *   - Manager.AttachTargetDevice(target, composite) then attaches the
     *     exact pre-created target path for this slot.
     * The clear is per-composite and happens immediately before the attach,
     * so a failure is returned (never a silent partial route) without the
     * save path having to tear every composite down first. */
    if (paths_csv[0] == '\0')
        return ip_composite_set_target_devices(backend, bus, composite_path, "");

    int clear_rc = ip_composite_set_target_devices(backend, bus,
                                                    composite_path, "");
    if (clear_rc != 0)
        return clear_rc;

    return ip_manager_attach_target_device(backend, bus, paths_csv,
                                            composite_path);
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
ip_composite_get_profile_name(const ip_dbus_backend *backend,
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
                                   "ProfileName", out_value);
}

int
ip_composite_get_profile_path(const ip_dbus_backend *backend,
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
                                   "ProfilePath", out_value);
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