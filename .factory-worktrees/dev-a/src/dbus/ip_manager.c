/*
 * ip_manager.c — Manager interface method wrappers (Task 12).
 *
 * Implements the wrappers declared in ip_manager.h.  All wrappers go
 * through the ip_dbus_backend vtable:
 *
 *   - Method calls: backend->call_method(bus, dest, path, iface, method, sig, ...)
 *     The last variadic argument is always a char **out_value (NULL for
 *     void methods, a valid pointer for methods that return a string).
 *     The mock backend counts input args from `sig`, skips them, and
 *     fills *out_value from the canned expectation.
 *   - Property gets: backend->get_property(bus, dest, path, iface, prop, &out)
 *   - Property sets: backend->set_property(bus, dest, path, iface, prop, value)
 *
 * SPEC §10.2 — Manager interface.
 */
#include "ip_manager.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* --- Internal helpers --------------------------------------------------- */

/*
 * Validate that every comma-separated path in `value` exists as a
 * composite device in `model`.  Returns true if all paths are found,
 * false if any path is missing or if value/model is NULL.
 */
static bool
validate_gamepad_order_paths(const char *value,
                              const cbx_device_model *model)
{
    if (!value || !model)
        return false;

    /* Empty string is valid (clears the order). */
    if (value[0] == '\0')
        return true;

    const char *p = value;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);

        /* Copy the path fragment to a temp buffer for lookup. */
        char path[CBX_MAX_PATH_LEN];
        if (len >= sizeof(path))
            return false;  /* path too long — can't be valid */
        memcpy(path, p, len);
        path[len] = '\0';

        if (!cbx_device_model_find_composite(model, path))
            return false;

        if (!comma)
            break;
        p = comma + 1;
    }

    return true;
}

/* --- Method wrappers ----------------------------------------------------- */

int
ip_manager_create_target_device(const ip_dbus_backend *backend,
                                 ip_bus_handle bus,
                                 const char *kind,
                                 char **out_path)
{
    if (!backend || !kind || !out_path)
        return -EINVAL;

    *out_path = NULL;

    return backend->call_method(bus, IP_DBUS_NAME,
                                  IP_DBUS_MANAGER_PATH,
                                  IP_IFACE_MANAGER,
                                  "CreateTargetDevice",
                                  "s", kind, out_path);
}

int
ip_manager_stop_target_device(const ip_dbus_backend *backend,
                               ip_bus_handle bus,
                               const char *path)
{
    if (!backend || !path)
        return -EINVAL;

    return backend->call_method(bus, IP_DBUS_NAME,
                                  IP_DBUS_MANAGER_PATH,
                                  IP_IFACE_MANAGER,
                                  "StopTargetDevice",
                                  "s", path, NULL);
}

int
ip_manager_attach_target_device(const ip_dbus_backend *backend,
                                 ip_bus_handle bus,
                                 const char *target_path,
                                 const char *composite_path)
{
    if (!backend || !target_path || !composite_path)
        return -EINVAL;

    return backend->call_method(bus, IP_DBUS_NAME,
                                  IP_DBUS_MANAGER_PATH,
                                  IP_IFACE_MANAGER,
                                  "AttachTargetDevice",
                                  "ss", target_path, composite_path,
                                  NULL);
}

int
ip_manager_set_target_devices(const ip_dbus_backend *backend,
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

/* --- Property wrappers -------------------------------------------------- */

int
ip_manager_get_gamepad_order(const ip_dbus_backend *backend,
                              ip_bus_handle bus,
                              char **out_value)
{
    if (!backend || !out_value)
        return -EINVAL;

    *out_value = NULL;

    return backend->get_property(bus, IP_DBUS_NAME,
                                   IP_DBUS_MANAGER_PATH,
                                   IP_IFACE_MANAGER,
                                   "GamepadOrder", out_value);
}

int
ip_manager_set_gamepad_order(const ip_dbus_backend *backend,
                              ip_bus_handle bus,
                              const char *value,
                              const cbx_device_model *model)
{
    if (!backend || !value)
        return -EINVAL;

    /* Validate all paths exist in the device model before calling. */
    if (!validate_gamepad_order_paths(value, model))
        return -EINVAL;

    return backend->set_property(bus, IP_DBUS_NAME,
                                  IP_DBUS_MANAGER_PATH,
                                  IP_IFACE_MANAGER,
                                  "GamepadOrder", value);
}

int
ip_manager_get_supported_target_device_ids(const ip_dbus_backend *backend,
                                             ip_bus_handle bus,
                                             char **out_value)
{
    if (!backend || !out_value)
        return -EINVAL;

    *out_value = NULL;

    return backend->get_property(bus, IP_DBUS_NAME,
                                   IP_DBUS_MANAGER_PATH,
                                   IP_IFACE_MANAGER,
                                   "SupportedTargetDeviceIds", out_value);
}

int
ip_manager_get_supported_target_devices(const ip_dbus_backend *backend,
                                          ip_bus_handle bus,
                                          char **out_value)
{
    if (!backend || !out_value)
        return -EINVAL;

    *out_value = NULL;

    return backend->get_property(bus, IP_DBUS_NAME,
                                   IP_DBUS_MANAGER_PATH,
                                   IP_IFACE_MANAGER,
                                   "SupportedTargetDevices", out_value);
}