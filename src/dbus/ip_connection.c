/*
 * ip_connection.c — InputPlumber DBus connection management (Task 9).
 *
 * Implements the connection lifecycle: connect to the system bus, read the
 * Version property from the Manager interface, subscribe to NameOwnerChanged
 * to detect daemon start/stop, and manage degraded-mode transitions.
 *
 * SPEC §10.1 — Connection model.
 */
#include "ip_connection.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* DBus daemon interface for NameOwnerChanged subscription. */
#define DBUS_DAEMON_IFACE  "org.freedesktop.DBus"
#define DBUS_DAEMON_MEMBER "NameOwnerChanged"

/* --- Signal callback (registered via vtable subscribe_signal) ------------- */
/* This is the intermediate callback that receives the parsed payload from
 * the backend (production: sd-bus message parsed into ip_owner_changed_payload;
 * mock: test injects ip_owner_changed_payload directly). */

static void
noc_signal_callback(const char *iface, const char *member,
                    const void *payload, void *userdata)
{
    (void)iface;
    (void)member;
    ip_connection *conn = (ip_connection *)userdata;
    const ip_owner_changed_payload *noc =
        (const ip_owner_changed_payload *)payload;
    if (!conn || !noc)
        return;
    ip_connection_handle_name_changed(conn, noc->old_owner, noc->new_owner);
}

/* --- Public API ---------------------------------------------------------- */

void
ip_connection_init(ip_connection *conn, const ip_dbus_backend *backend)
{
    if (!conn)
        return;
    memset(conn, 0, sizeof(*conn));
    conn->backend = backend;
    conn->state   = IP_CONN_DISCONNECTED;
}

void
ip_connection_set_bus(ip_connection *conn, ip_bus_handle bus)
{
    if (!conn)
        return;
    conn->bus = bus;
}

int
ip_connection_connect(ip_connection *conn)
{
    if (!conn || !conn->backend)
        return IP_ERR_INTERNAL;

    int rc;

    /* 1. Connect to the system bus. */
    rc = conn->backend->connect(&conn->bus);
    if (rc < 0)
        return rc;

    /* 2. Subscribe to NameOwnerChanged for InputPlumber's well-known name.
     *    This subscription must survive a ServiceUnknown on the version
     *    check so we can detect when InputPlumber starts later. */
    rc = conn->backend->subscribe_signal(
        conn->bus, DBUS_DAEMON_IFACE, DBUS_DAEMON_MEMBER,
        noc_signal_callback, conn);
    if (rc < 0) {
        conn->backend->disconnect(conn->bus);
        conn->bus   = NULL;
        conn->state = IP_CONN_DISCONNECTED;
        return rc;
    }

    /* 3. Try to read the Version property from the Manager interface. */
    char *version = NULL;
    rc = conn->backend->get_property(
        conn->bus, IP_DBUS_NAME, IP_DBUS_PATH,
        IP_IFACE_MANAGER, "Version", &version);

    if (rc == 0) {
        /* InputPlumber is running. */
        conn->version = version;

        /* Get InputPlumber's unique bus name for sender verification. */
        char *unique = NULL;
        rc = conn->backend->get_unique_name(conn->bus, IP_DBUS_NAME, &unique);
        if (rc == 0 && unique) {
            conn->unique_name = unique;
        } else {
            /* Non-fatal: unique name is used for signal sender verification
             * in later tasks. If unavailable, we're still connected. */
            free(unique);
        }
        conn->state = IP_CONN_CONNECTED;
        return 0;
    }

    /* Version read failed. */
    free(version);

    if (rc == IP_ERR_SERVICE_UNKNOWN) {
        /* InputPlumber is not running. Enter degraded mode but stay
         * connected to the bus — NameOwnerChanged will tell us when
         * InputPlumber starts. */
        conn->state = IP_CONN_DEGRADED;
        return IP_ERR_SERVICE_UNKNOWN;
    }

    /* Other errors: AccessDenied, NoReply, InvalidArgs, etc. */
    if (rc == IP_ERR_ACCESS_DENIED) {
        fprintf(stderr,
                "ip_connection: AccessDenied — add your user to the "
                "'inputplumber' group and restart the application.\n");
    }

    conn->backend->disconnect(conn->bus);
    conn->bus   = NULL;
    conn->state = IP_CONN_DISCONNECTED;
    return rc;
}

void
ip_connection_disconnect(ip_connection *conn)
{
    if (!conn)
        return;

    if (conn->bus && conn->backend) {
        conn->backend->disconnect(conn->bus);
        conn->bus = NULL;
    }

    free(conn->unique_name);
    conn->unique_name = NULL;

    free(conn->version);
    conn->version = NULL;

    conn->state = IP_CONN_DISCONNECTED;
}

ip_conn_state
ip_connection_get_state(const ip_connection *conn)
{
    return conn ? conn->state : IP_CONN_DISCONNECTED;
}

const char *
ip_connection_get_version(const ip_connection *conn)
{
    return conn ? conn->version : NULL;
}

const char *
ip_connection_get_unique_name(const ip_connection *conn)
{
    return conn ? conn->unique_name : NULL;
}

bool
ip_connection_is_connected(const ip_connection *conn)
{
    return conn && conn->state == IP_CONN_CONNECTED;
}

bool
ip_connection_is_degraded(const ip_connection *conn)
{
    return conn && conn->state == IP_CONN_DEGRADED;
}

void
ip_connection_set_reenumerate_cb(ip_connection *conn,
                                  ip_reenumerate_cb cb, void *userdata)
{
    if (!conn)
        return;
    conn->reenumerate_cb = cb;
    conn->reenumerate_ud = userdata;
}

void
ip_connection_set_degraded_cb(ip_connection *conn,
                                ip_degraded_cb cb, void *userdata)
{
    if (!conn)
        return;
    conn->degraded_cb = cb;
    conn->degraded_ud = userdata;
}

void
ip_connection_handle_name_changed(ip_connection *conn,
                                    const char *old_owner,
                                    const char *new_owner)
{
    if (!conn || !conn->backend)
        return;

    bool acquired = (new_owner && new_owner[0] != '\0');
    bool lost     = (old_owner && old_owner[0] != '\0' &&
                     (!new_owner || new_owner[0] == '\0'));

    if (acquired) {
        /* InputPlumber's bus name was (re-)acquired. */
        free(conn->unique_name);
        conn->unique_name = strdup(new_owner);

        /* Re-read the Version property. */
        char *version = NULL;
        int rc = conn->backend->get_property(
            conn->bus, IP_DBUS_NAME, IP_DBUS_PATH,
            IP_IFACE_MANAGER, "Version", &version);
        if (rc == 0) {
            free(conn->version);
            conn->version = version;
        } else {
            free(version);
        }

        conn->state = IP_CONN_CONNECTED;

        /* Trigger re-enumeration callback. */
        if (conn->reenumerate_cb)
            conn->reenumerate_cb(conn->reenumerate_ud);
    } else if (lost) {
        /* InputPlumber's bus name was lost — daemon stopped. */
        free(conn->unique_name);
        conn->unique_name = NULL;

        free(conn->version);
        conn->version = NULL;

        conn->state = IP_CONN_DEGRADED;

        /* Trigger degraded-mode callback. */
        if (conn->degraded_cb)
            conn->degraded_cb("InputPlumber stopped", conn->degraded_ud);
    }
    /* If both old and new are empty or both non-empty (name transfer
     * without loss), we ignore it — only acquisition and loss matter. */
}