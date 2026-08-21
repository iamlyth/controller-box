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
#include "ip_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    if (!conn || !noc || !noc->name ||
        strcmp(noc->name, IP_DBUS_NAME) != 0)
        return;
    ip_connection_handle_name_changed(conn, noc->old_owner, noc->new_owner);
}

/* --- Error reason mapping (SPEC §2.4) ----------------------------------- */

const char *
ip_connection_reason_for_error(int rc)
{
    switch (rc) {
    case IP_ERR_SERVICE_UNKNOWN:
        return "InputPlumber unavailable \xe2\x80\x94 waiting for service";
    case IP_ERR_ACCESS_DENIED:
        return "InputPlumber access denied \xe2\x80\x94 check polkit rules";
    case IP_ERR_NO_REPLY:
        return "InputPlumber not responding \xe2\x80\x94 check daemon status";
    case IP_ERR_INVALID_ARGS:
        return "InputPlumber version incompatible \xe2\x80\x94 update required";
    case IP_ERR_INCOMPATIBLE:
        return "InputPlumber version incompatible \xe2\x80\x94 update required";
    case IP_ERR_NOT_CONNECTED:
        return "System DBus not connected";
    default:
        return "InputPlumber internal error";
    }
}

/* --- Version compatibility check (SPEC §2.4) --------------------------- */

bool
ip_version_is_compatible(const char *version)
{
    if (!version || !version[0])
        return false;
    int major = 0, minor = 0, patch = 0;
    if (sscanf(version, "%d.%d.%d", &major, &minor, &patch) < 1)
        return false;
    if (major != IP_COMPAT_MIN_MAJOR)
        return major > IP_COMPAT_MIN_MAJOR;
    if (minor != IP_COMPAT_MIN_MINOR)
        return minor > IP_COMPAT_MIN_MINOR;
    return patch >= IP_COMPAT_MIN_PATCH;
}

bool
ip_connection_uid_is_trusted(uint32_t uid)
{
    /* InputPlumber runs as a system service (root) or, in a dev/session
     * setup, as the same user as Controller-Box.  A bus owner from any
     * other user is treated as a name-squatting process. */
    if (uid == 0)
        return true;
    return uid == (uint32_t)geteuid();
}

/* Verify the owner of `unique_name` by querying GetConnectionCredentials
 * and applying the anti-squatting UID policy.  On success records the
 * credential fingerprint and marks the sender verified. */
static bool
verify_sender(ip_connection *conn, const char *unique_name)
{
    conn->sender_verified = false;
    conn->expected_pid    = 0;
    conn->expected_uid    = 0;

    if (!conn || !conn->backend || !conn->backend->get_connection_creds ||
        !unique_name)
        return false;

    uint32_t pid = 0, uid = 0;
    if (conn->backend->get_connection_creds(conn->bus, unique_name,
                                            &pid, &uid) != 0)
        return false;
    if (!ip_connection_uid_is_trusted(uid))
        return false;

    conn->expected_pid    = pid;
    conn->expected_uid    = uid;
    conn->sender_verified = true;
    return true;
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
        conn->bus, IP_DBUS_NAME, IP_DBUS_MANAGER_PATH,
        IP_IFACE_MANAGER, "Version", &version);

    if (rc == 0) {
        /* InputPlumber is running — check version compatibility. */
        if (!ip_version_is_compatible(version)) {
            free(version);
            conn->state = IP_CONN_DEGRADED;
            return IP_ERR_INCOMPATIBLE;
        }
        conn->version = version;

        /* Get InputPlumber's unique bus name for sender verification. */
        char *unique = NULL;
        rc = conn->backend->get_unique_name(conn->bus, IP_DBUS_NAME, &unique);
        if (rc == 0 && unique) {
            /* Credential-verify the owner before trusting any signal or
             * method reply from it (F3, prevents name squatting). */
            if (verify_sender(conn, unique)) {
                conn->unique_name = unique;
                conn->state = IP_CONN_CONNECTED;
                return 0;
            }
            /* Owner present but unverified (untrusted UID / creds lookup
             * failed): stay on the bus watching NameOwnerChanged, but do
             * not report readiness and do not advertise a trusted sender. */
            free(unique);
            conn->unique_name = NULL;
            conn->state = IP_CONN_DEGRADED;
            return IP_ERR_ACCESS_DENIED;
        } else {
            /* Non-fatal: unique name is used for signal sender verification
             * in later tasks. If unavailable, we're still connected but the
             * sender cannot be verified. */
            conn->sender_verified = false;
            free(unique);
            conn->state = IP_CONN_CONNECTED;
            return 0;
        }
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
ip_connection_is_sender_verified(const ip_connection *conn)
{
    return conn && conn->sender_verified;
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
        /* InputPlumber's bus name was (re-)acquired.  Credential-verify the
         * new owner before trusting it; if verification fails, treat the
         * owner as untrusted (stay degraded, no trusted sender). */
        char *new_name = strdup(new_owner);
        if (new_name) {
            if (verify_sender(conn, new_owner)) {
                free(conn->unique_name);
                conn->unique_name = new_name;
            } else {
                free(new_name);
                free(conn->unique_name);
                conn->unique_name = NULL;
                conn->state = IP_CONN_DEGRADED;
                if (conn->degraded_cb)
                    conn->degraded_cb("InputPlumber owner could not be verified",
                                      conn->degraded_ud);
                return;
            }
        } else {
            conn->sender_verified = false;
        }

        /* Re-read the Version property. */
        char *version = NULL;
        int rc = conn->backend->get_property(
            conn->bus, IP_DBUS_NAME, IP_DBUS_MANAGER_PATH,
            IP_IFACE_MANAGER, "Version", &version);
        if (rc == 0 && ip_version_is_compatible(version)) {
            free(conn->version);
            conn->version = version;
            conn->state = IP_CONN_CONNECTED;
            if (conn->reenumerate_cb)
                conn->reenumerate_cb(conn->reenumerate_ud);
        } else if (rc == 0) {
            /* Version read succeeded but is incompatible. */
            free(version);
            conn->state = IP_CONN_DEGRADED;
            if (conn->degraded_cb)
                conn->degraded_cb("InputPlumber version incompatible \xe2\x80\x94 update required",
                                  conn->degraded_ud);
        } else {
            free(version);
            conn->state = IP_CONN_DEGRADED;
            if (conn->degraded_cb)
                conn->degraded_cb(ip_connection_reason_for_error(rc),
                                  conn->degraded_ud);
        }
    } else if (lost) {
        /* InputPlumber's bus name was lost — daemon stopped. */
        free(conn->unique_name);
        conn->unique_name = NULL;

        conn->sender_verified = false;
        conn->expected_pid    = 0;
        conn->expected_uid    = 0;

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