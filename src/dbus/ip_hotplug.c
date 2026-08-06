/*
 * ip_hotplug.c — Hotplug signal handling (Task 11).
 *
 * Implements InterfacesAdded / InterfacesRemoved subscription and processing.
 * The signal callback (hotplug_signal_cb) is registered via
 * backend->subscribe_signal and dispatched by the mock's inject_signal
 * (tests) or sd-bus process loop (production).
 *
 * Security:
 *   - Sender verification: the payload's `sender` must match `expected_sender`
 *     (InputPlumber's tracked unique bus name).  Mismatched senders are
 *     silently dropped.
 *   - Path validation: object paths must start with IP_DBUS_PATH "/" to be
 *     accepted.
 */
#include "ip_hotplug.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* --- Internal helpers ---------------------------------------------------- */

/* Check whether a comma-separated interface list contains `target`. */
static bool
iface_list_contains(const char *list, const char *target)
{
    if (!list || !target)
        return false;
    size_t tlen = strlen(target);
    const char *p = list;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len == tlen && strncmp(p, target, tlen) == 0)
            return true;
        if (!comma)
            break;
        p = comma + 1;
    }
    return false;
}

/* Validate that `path` starts with the InputPlumber root prefix. */
static bool
path_is_valid(const char *path)
{
    if (!path)
        return false;
    static const char prefix[] = IP_DBUS_PATH "/";
    size_t plen = sizeof(prefix) - 1;
    return strncmp(path, prefix, plen) == 0;
}

/* Verify that the signal sender matches the expected InputPlumber name. */
static bool
sender_ok(const ip_hotplug *hp, const char *sender)
{
    if (!hp || !hp->expected_sender || !sender)
        return false;
    return strcmp(sender, hp->expected_sender) == 0;
}

/* --- Signal callback (registered via vtable subscribe_signal) ------------- */

static void
hotplug_signal_cb(const char *iface, const char *member,
                  const void *payload, void *userdata)
{
    (void)iface;
    ip_hotplug *hp = (ip_hotplug *)userdata;
    const ip_interfaces_changed_payload *p =
        (const ip_interfaces_changed_payload *)payload;
    if (!hp || !p)
        return;

    if (strcmp(member, "InterfacesAdded") == 0)
        ip_hotplug_handle_added(hp, p);
    else if (strcmp(member, "InterfacesRemoved") == 0)
        ip_hotplug_handle_removed(hp, p);
}

/* --- Public API ---------------------------------------------------------- */

void
ip_hotplug_init(ip_hotplug *hp, const ip_dbus_backend *backend,
                ip_bus_handle bus, const char *expected_sender,
                cbx_device_model *model)
{
    if (!hp)
        return;
    memset(hp, 0, sizeof(*hp));
    hp->backend         = backend;
    hp->bus             = bus;
    hp->expected_sender = expected_sender;
    hp->model            = model;
}

int
ip_hotplug_subscribe(ip_hotplug *hp)
{
    if (!hp || !hp->backend)
        return -EINVAL;

    int rc = hp->backend->subscribe_signal(
        hp->bus, IP_IFACE_OBJECT_MANAGER, "InterfacesAdded",
        hotplug_signal_cb, hp);
    if (rc < 0)
        return rc;

    return hp->backend->subscribe_signal(
        hp->bus, IP_IFACE_OBJECT_MANAGER, "InterfacesRemoved",
        hotplug_signal_cb, hp);
}

void
ip_hotplug_handle_added(ip_hotplug *hp,
                         const ip_interfaces_changed_payload *payload)
{
    if (!hp || !payload || !hp->model)
        return;

    /* Sender verification. */
    if (!sender_ok(hp, payload->sender))
        return;

    /* Path validation. */
    if (!path_is_valid(payload->path))
        return;

    cbx_device_model *model = hp->model;
    const char *path   = payload->path;
    const char *ifaces = payload->interfaces ? payload->interfaces : "";

    /* Classify and add. */
    if (iface_list_contains(ifaces, IP_IFACE_MANAGER))
        cbx_device_model_set_manager(model, path);

    if (iface_list_contains(ifaces, IP_IFACE_COMPOSITE))
        cbx_device_model_add_composite(model, path);

    if (strstr(path, "/devices/source/"))
        cbx_device_model_add_source(model, path);
    else if (strstr(path, "/devices/target/"))
        cbx_device_model_add_target(model, path);
}

void
ip_hotplug_handle_removed(ip_hotplug *hp,
                           const ip_interfaces_changed_payload *payload)
{
    if (!hp || !payload || !hp->model)
        return;

    /* Sender verification. */
    if (!sender_ok(hp, payload->sender))
        return;

    /* Path validation. */
    if (!path_is_valid(payload->path))
        return;

    cbx_device_model *model = hp->model;
    const char *path   = payload->path;
    const char *ifaces = payload->interfaces ? payload->interfaces : "";

    /* Remove based on classification. */
    if (iface_list_contains(ifaces, IP_IFACE_MANAGER))
        cbx_device_model_remove_manager(model);

    if (iface_list_contains(ifaces, IP_IFACE_COMPOSITE))
        cbx_device_model_remove_composite(model, path);

    if (strstr(path, "/devices/source/"))
        cbx_device_model_remove_source(model, path);
    else if (strstr(path, "/devices/target/"))
        cbx_device_model_remove_target(model, path);
}