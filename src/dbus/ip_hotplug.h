/*
 * ip_hotplug.h — Hotplug signal handling (Task 11).
 *
 * Subscribes to ObjectManager InterfacesAdded / InterfacesRemoved signals
 * and applies incremental updates to a cbx_device_model so that the model
 * stays in sync with InputPlumber's object tree without a full
 * re-enumeration on every device change.
 *
 * SPEC §10.1 — Hotplug:
 *   subscribe to ObjectManager InterfacesAdded / InterfacesRemoved —
 *   source, composite, and target devices all register/unregister through
 *   the object server.  No polling for device presence.
 */
#ifndef CBX_IP_HOTPLUG_H
#define CBX_IP_HOTPLUG_H

#include "dbus_mock.h"      /* ip_dbus_backend, ip_bus_handle, constants */
#include "ip_device_model.h"

#include <stdbool.h>

/* --- Hotplug handler ----------------------------------------------------- */

typedef struct {
    const ip_dbus_backend *backend;
    ip_bus_handle          bus;
    const char            *expected_sender;  /* InputPlumber's unique bus name */
    cbx_device_model      *model;            /* device model to update */
    bool                   model_changed;    /* set when a signal modified the model */
} ip_hotplug;

/* Initialise the hotplug handler.  Does not subscribe yet. */
void ip_hotplug_init(ip_hotplug *hp, const ip_dbus_backend *backend,
                     ip_bus_handle bus, const char *expected_sender,
                     cbx_device_model *model);

/* Subscribe to InterfacesAdded and InterfacesRemoved signals.
 * Returns 0 on success, negative errno on failure. */
int ip_hotplug_subscribe(ip_hotplug *hp);

/* Process an InterfacesAdded payload.  Validates sender, path, and
 * classifies the object (Manager / Composite / Source / Target), then
 * adds it to the model.  Silently drops invalid or unrecognised entries. */
void ip_hotplug_handle_added(ip_hotplug *hp,
                              const ip_interfaces_changed_payload *payload);

/* Process an InterfacesRemoved payload.  Validates sender and path,
 * then removes the matching entry from the model. */
void ip_hotplug_handle_removed(ip_hotplug *hp,
                                 const ip_interfaces_changed_payload *payload);

#endif /* CBX_IP_HOTPLUG_H */