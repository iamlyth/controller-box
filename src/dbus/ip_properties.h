/*
 * ip_properties.h — PropertiesChanged signal handling (Task 11).
 *
 * Subscribes to org.freedesktop.DBus.Properties.PropertiesChanged and
 * dispatches validated property changes to a user-provided callback.
 * Only the properties the GUI cares about are processed:
 *
 *   GamepadOrder       (as) — on Manager interface
 *   ProfileName        (s)  — on CompositeDevice interface
 *   ProfilePath        (s)  — on CompositeDevice interface
 *   TargetDevices      (as) — on CompositeDevice interface
 *   SourceDevicePaths  (as) — on CompositeDevice interface
 *
 * SPEC §10.1 — Property changes:
 *   org.freedesktop.DBus.Properties.PropertiesChanged is emitted for
 *   GamepadOrder, ProfileName, ProfilePath, TargetDevices,
 *   SourceDevicePaths — but not for InterceptMode (gap #1, polled separately).
 */
#ifndef CBX_IP_PROPERTIES_H
#define CBX_IP_PROPERTIES_H

#include "dbus_interface.h"      /* ip_dbus_backend, ip_bus_handle, ip_prop_type */

#include <stdbool.h>

/* --- Limits --------------------------------------------------------------- */

#define IP_PROP_MAX_NAME_LEN     256   /* max length for name-type strings */
#define IP_PROP_MAX_PATH_LEN    4096   /* max length for path-type strings */
#define IP_PROP_MAX_ARRAY_ELEMS   256   /* max elements in array properties */

/* --- Callback type -------------------------------------------------------- */

/* Called for each validated property change.  `prop_name` is one of the
 * tracked property names.  `type` indicates the value type.  `value` is
 * the string value (for s) or comma-separated values (for as).  `count`
 * is the array element count (for as), 0 for strings, -1 for invalidated. */
typedef void (*ip_prop_changed_cb)(const char *prop_name,
                                    ip_prop_type type,
                                    const char *value,
                                    int count,
                                    void *userdata);

/* --- Properties handler -------------------------------------------------- */

typedef struct {
    const ip_dbus_backend *backend;
    ip_bus_handle          bus;
    const char            *expected_sender;  /* InputPlumber's unique bus name */
    ip_prop_changed_cb     cb;                /* user callback for valid changes */
    void                  *cb_userdata;       /* userdata passed to callback */
} ip_properties;

/* Initialise the properties handler.  Does not subscribe yet. */
void ip_properties_init(ip_properties *props, const ip_dbus_backend *backend,
                        ip_bus_handle bus, const char *expected_sender,
                        ip_prop_changed_cb cb, void *cb_userdata);

/* Subscribe to PropertiesChanged signals.
 * Returns 0 on success, negative errno on failure. */
int ip_properties_subscribe(ip_properties *props);

/* Process a PropertiesChanged payload (single property change).
 * Validates sender, property name, type, and value limits.
 * On success, fires the user callback.  On any validation failure,
 * silently drops the change. */
void ip_properties_handle_changed(ip_properties *props,
                                    const ip_properties_changed_payload *payload);

#endif /* CBX_IP_PROPERTIES_H */