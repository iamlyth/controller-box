/*
 * ip_objectmanager.h — ObjectManager enumeration (Task 10).
 *
 * Calls GetManagedObjects() at the InputPlumber root path and parses
 * the response into a cbx_device_model.  The response is represented
 * as a text fixture (one line per managed object: "path\tiface1,iface2,…")
 * so that both the production sd-bus backend and the mock backend use
 * the same parser.
 *
 * SPEC §10.1 — Enumeration:
 *   org.freedesktop.DBus.ObjectManager.GetManagedObjects() at the root
 *   path returns all composite devices, source devices, and target
 *   devices in one call.
 */
#ifndef CBX_IP_OBJECTMANAGER_H
#define CBX_IP_OBJECTMANAGER_H

#include "dbus_mock.h"      /* ip_dbus_backend, ip_bus_handle, constants */
#include "ip_device_model.h"

/*
 * Enumerate all InputPlumber managed objects.
 *
 * Calls backend->get_managed_objects() and parses the reply into `model`.
 * The model is zeroed before parsing.
 *
 * Returns 0 on success (model may be empty if InputPlumber is starting up).
 * Returns negative errno on failure (backend error, or parser error).
 */
int cbx_objectmanager_enumerate(const ip_dbus_backend *backend,
                                 ip_bus_handle bus,
                                 cbx_device_model *model);

/*
 * Parse a GetManagedObjects reply (text format) into `model`.
 *
 * The text format is:
 *   <object_path>\t<iface1>,<iface2>,...
 *   # comment lines start with '#'
 *   (empty lines are ignored)
 *
 * The model is zeroed before parsing.
 *
 * Returns 0 on success (model may be empty).
 * Returns -EINVAL if `reply` is non-NULL but `model` is NULL.
 */
int cbx_objectmanager_parse_reply(const char *reply,
                                   cbx_device_model *model);

#endif /* CBX_IP_OBJECTMANAGER_H */