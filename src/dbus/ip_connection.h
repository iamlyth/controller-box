/*
 * ip_connection.h — InputPlumber DBus connection management (Task 9).
 *
 * Manages the system-bus connection to InputPlumber: connecting, reading
 * the Version property for compatibility checks, subscribing to
 * NameOwnerChanged to detect daemon start/stop, and tracking connection
 * state (connected / degraded).
 *
 * All DBus operations go through the ip_dbus_backend vtable so that the
 * connection logic is fully unit-testable with the mock backend.
 *
 * SPEC §10.1 — Connection model.
 */
#ifndef CBX_IP_CONNECTION_H
#define CBX_IP_CONNECTION_H

#include "dbus_interface.h"  /* ip_dbus_backend, ip_bus_handle, constants */

#include <stdbool.h>
#include <stddef.h>

/* --- Categorized error codes ---------------------------------------------- */
/* Negative errno values that map to specific DBus error conditions.
 * The production sd-bus backend translates sd_bus_error names to these
 * codes; the mock backend returns them directly. */

#include <errno.h>

#define IP_ERR_SERVICE_UNKNOWN  (-EUNATCH)   /* org.freedesktop.DBus.Error.ServiceUnknown  */
#define IP_ERR_ACCESS_DENIED    (-EACCES)   /* org.freedesktop.DBus.Error.AccessDenied    */
#define IP_ERR_NO_REPLY         (-ETIMEDOUT) /* org.freedesktop.DBus.Error.NoReply        */
#define IP_ERR_INVALID_ARGS     (-EINVAL)    /* org.freedesktop.DBus.Error.InvalidArgs     */
#define IP_ERR_NOT_CONNECTED    (-ENOTCONN)  /* no bus connection                          */
#define IP_ERR_INTERNAL         (-EIO)       /* other / unexpected sd-bus failure          */
#define IP_ERR_INCOMPATIBLE      (-ENOSYS)    /* version too old / incompatible              */

/* Minimum compatible InputPlumber version (§2.4, plan compat ref 0.78.0). */
#define IP_COMPAT_MIN_MAJOR  0
#define IP_COMPAT_MIN_MINOR  78
#define IP_COMPAT_MIN_PATCH  0

/* --- Connection state ----------------------------------------------------- */

typedef enum {
    IP_CONN_DISCONNECTED = 0,  /* never connected or disconnected */
    IP_CONN_CONNECTED,         /* InputPlumber running, version read */
    IP_CONN_DEGRADED,          /* InputPlumber not running (ServiceUnknown) or name lost */
} ip_conn_state;

/* --- Callback types ------------------------------------------------------ */

/* Called when InputPlumber's bus name is (re-)acquired — triggers re-enumeration. */
typedef void (*ip_reenumerate_cb)(void *userdata);

/* Called when InputPlumber's bus name is lost — enter user-facing degraded mode. */
typedef void (*ip_degraded_cb)(const char *reason, void *userdata);

/* --- Connection handle --------------------------------------------------- */

typedef struct {
    const ip_dbus_backend *backend;
    ip_bus_handle           bus;
    ip_conn_state           state;
    char                   *unique_name;  /* InputPlumber's unique bus name (e.g. ":1.42") */
    char                   *version;      /* InputPlumber version string (e.g. "0.1.0") */
    ip_reenumerate_cb       reenumerate_cb;
    void                   *reenumerate_ud;
    ip_degraded_cb          degraded_cb;
    void                   *degraded_ud;
} ip_connection;

/* --- API ----------------------------------------------------------------- */

/* Initialise with a backend vtable.  Bus handle is NULL until connect or
 * set_bus is called. */
void ip_connection_init(ip_connection *conn, const ip_dbus_backend *backend);

/* Set the bus handle explicitly (for mock backends that require a pre-set
 * handle).  Production backends allocate the handle inside connect(). */
void ip_connection_set_bus(ip_connection *conn, ip_bus_handle bus);

/* Connect to the system bus, read Version, subscribe to NameOwnerChanged.
 *
 * Returns 0 on success (state = CONNECTED).
 * Returns IP_ERR_SERVICE_UNKNOWN if InputPlumber is not running (state = DEGRADED,
 *   but the bus is connected and NameOwnerChanged is subscribed).
 * Returns other IP_ERR_* on failure (state = DISCONNECTED, bus is released). */
int ip_connection_connect(ip_connection *conn);

/* Disconnect and free all resources (bus handle, strings). */
void ip_connection_disconnect(ip_connection *conn);

/* Get the current connection state. */
ip_conn_state ip_connection_get_state(const ip_connection *conn);

/* Get the InputPlumber version string (NULL if not available). */
const char *ip_connection_get_version(const ip_connection *conn);

/* Get InputPlumber's unique bus name (NULL if not connected). */
const char *ip_connection_get_unique_name(const ip_connection *conn);

/* Convenience: state == CONNECTED. */
bool ip_connection_is_connected(const ip_connection *conn);

/* Convenience: state == DEGRADED. */
bool ip_connection_is_degraded(const ip_connection *conn);

/* Set the re-enumeration callback (called when InputPlumber name acquired). */
void ip_connection_set_reenumerate_cb(ip_connection *conn,
                                       ip_reenumerate_cb cb, void *userdata);

/* Set the degraded-mode callback (called when InputPlumber name lost). */
void ip_connection_set_degraded_cb(ip_connection *conn,
                                    ip_degraded_cb cb, void *userdata);

/* Handle a NameOwnerChanged event.  Called from the signal callback.
 * old_owner: previous unique name ("" if just acquired).
 * new_owner: new unique name ("" if just lost). */
void ip_connection_handle_name_changed(ip_connection *conn,
                                        const char *old_owner,
                                        const char *new_owner);

/* Map a negative errno return code to a human-readable, actionable reason
 * string suitable for display in the degraded UI (SPEC §2.4).
 * Returns a static string — no allocation. */
const char *ip_connection_reason_for_error(int rc);

/* Check whether a version string satisfies the minimum compatibility.
 * Returns true if compatible, false otherwise. */
bool ip_version_is_compatible(const char *version);

#endif /* CBX_IP_CONNECTION_H */