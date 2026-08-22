/*
 * dbus_mock.c — DBus mock interface abstraction (Task 3).
 *
 * Implements the mock backend vtable and the canned-response store.  The
 * mock replays expectations registered by tests; calls to unregistered
 * (iface, member) pairs return -ENXIO (no such entry), which tests can
 * assert against.
 */
#define _POSIX_C_SOURCE 200809L

#include "dbus_mock.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Mock store helpers ---------------------------------------------------- */

void ip_dbus_mock_init(ip_dbus_mock *mock) {
    if (!mock) return;
    memset(mock, 0, sizeof(*mock));
    mock->bus = mock;  /* opaque handle points back to the mock struct */
    mock->creds_pid = 4242;
    mock->creds_uid = 0;   /* root — trusted by the anti-squatting policy */
    mock->creds_rc  = 0;
}

void ip_dbus_mock_free(ip_dbus_mock *mock) {
    ip_dbus_mock_reset(mock);
}

void ip_dbus_mock_reset(ip_dbus_mock *mock) {
    if (!mock) return;
    for (int i = 0; i < mock->count; i++) {
        free(mock->expectations[i].value);
        mock->expectations[i].value = NULL;
    }
    mock->count = 0;
    mock->sub_count = 0;
    mock->subscribe_fail_rc = 0;
    mock->queued_signal_count = 0;
    mock->creds_pid = 4242;
    mock->creds_uid = 0;
    mock->creds_rc  = 0;
    mock->unique_name_rc = 0;
    memset(&mock->last_call, 0, sizeof(mock->last_call));
}

void ip_dbus_mock_set_creds(ip_dbus_mock *mock, uint32_t pid, uint32_t uid) {
    if (!mock) return;
    mock->creds_pid = pid;
    mock->creds_uid = uid;
    mock->creds_rc  = 0;
}

void ip_dbus_mock_set_creds_fail(ip_dbus_mock *mock, int rc) {
    if (!mock) return;
    mock->creds_rc = rc;
}

void ip_dbus_mock_set_unique_name_fail(ip_dbus_mock *mock, int rc) {
    if (!mock) return;
    mock->unique_name_rc = rc;
}

/* --- Queued signal helper ----------------------------------------------- */

int ip_dbus_mock_queue_noc(ip_dbus_mock *mock,
                            const char *name,
                            const char *old_owner,
                            const char *new_owner)
{
    if (!mock || mock->queued_signal_count >= IP_MOCK_MAX_QUEUED_SIGNALS)
        return -1;
    ip_mock_queued_signal *qs = &mock->queued_signals[mock->queued_signal_count++];
    snprintf(qs->iface, sizeof(qs->iface), "%s", "org.freedesktop.DBus");
    snprintf(qs->member, sizeof(qs->member), "%s", "NameOwnerChanged");
    snprintf(qs->noc_name, sizeof(qs->noc_name), "%s", name ? name : "");
    snprintf(qs->noc_old,  sizeof(qs->noc_old),  "%s", old_owner ? old_owner : "");
    snprintf(qs->noc_new,  sizeof(qs->noc_new),  "%s", new_owner ? new_owner : "");
    qs->noc_payload.name      = qs->noc_name;
    qs->noc_payload.old_owner = qs->noc_old;
    qs->noc_payload.new_owner = qs->noc_new;
    return 0;
}

int ip_dbus_mock_expect(ip_dbus_mock *mock, const char *iface,
                        const char *member, int rc, const char *value) {
    if (!mock || !iface || !member) return -1;
    if (mock->count >= IP_MOCK_MAX_EXPECTATIONS) {
        fprintf(stderr, "dbus_mock: expectation table full (%d)\n",
                IP_MOCK_MAX_EXPECTATIONS);
        return -1;
    }

    /* If an expectation for this key already exists, overwrite it. */
    for (int i = 0; i < mock->count; i++) {
        if (strcmp(mock->expectations[i].iface, iface) == 0 &&
            strcmp(mock->expectations[i].member, member) == 0) {
            free(mock->expectations[i].value);
            mock->expectations[i].rc    = rc;
            mock->expectations[i].value = value ? strdup(value) : NULL;
            return 0;
        }
    }

    ip_mock_expectation *e = &mock->expectations[mock->count++];
    e->iface  = iface;
    e->member = member;
    e->rc     = rc;
    e->value  = value ? strdup(value) : NULL;
    return 0;
}

int ip_dbus_mock_expect_ok(ip_dbus_mock *mock, const char *iface,
                            const char *member, const char *value) {
    return ip_dbus_mock_expect(mock, iface, member, 0, value);
}

int ip_dbus_mock_expect_error(ip_dbus_mock *mock, const char *iface,
                              const char *member, int rc) {
    return ip_dbus_mock_expect(mock, iface, member, rc, NULL);
}

const ip_mock_expectation *ip_dbus_mock_find(ip_dbus_mock *mock,
                                             const char *iface,
                                             const char *member) {
    if (!mock || !iface || !member) return NULL;
    for (int i = 0; i < mock->count; i++) {
        if (strcmp(mock->expectations[i].iface, iface) == 0 &&
            strcmp(mock->expectations[i].member, member) == 0) {
            return &mock->expectations[i];
        }
    }
    return NULL;
}

/* --- Mock vtable callbacks ------------------------------------------------- */

static int mock_connect(ip_bus_handle *bus) {
    /* bus is set by the test before calling; we just return success. */
    if (!bus || !*bus) return -EINVAL;
    return 0;
}

static void mock_disconnect(ip_bus_handle bus) {
    (void)bus;  /* nothing to free at the handle level */
}

static int mock_get_unique_name(ip_bus_handle bus, const char *well_known,
                                char **out_unique) {
    (void)well_known;
    ip_dbus_mock *mock = (ip_dbus_mock *)bus;
    if (!mock || !out_unique) return -EINVAL;
    if (mock->unique_name_rc < 0)
        return mock->unique_name_rc;
    /* Return a deterministic fake unique name. */
    *out_unique = strdup(":1.42");
    return 0;
}

/*
 * Count the number of input arguments encoded in a DBus type signature.
 * Convention: each 's' is one string arg; 'a' is a container prefix that
 * pairs with the following element type (e.g. "as" = one array arg).
 * All other characters count as one arg each.
 */
static int
mock_count_sig_args(const char *sig)
{
    int count = 0;
    for (const char *p = sig; *p; p++) {
        if (*p == 'a') {
            p++;            /* skip element type char */
            count++;
        } else {
            count++;
        }
    }
    return count;
}

/*
 * Mock call_method: looks up by (iface, method) and returns the canned
 * rc.  If the expectation has a value string and the last variadic arg
 * (a char **) is non-NULL, fills it with a strdup'd copy of the value.
 *
 * Calling convention: the last variadic argument is always a char **out
 * (NULL for void methods, a valid pointer for methods that return a
 * string).  The mock counts input args from `sig`, skips them, and reads
 * the trailing char **.
 */
static int mock_call_method(ip_bus_handle bus, const char *dest,
                            const char *path, const char *iface,
                            const char *method, const char *sig, ...) {
    (void)dest; (void)path;
    ip_dbus_mock *mock = (ip_dbus_mock *)bus;
    const ip_mock_expectation *e = ip_dbus_mock_find(mock, iface, method);
    int rc = e ? e->rc : -ENXIO;

    /* Process variadic args: capture input args, then read output ptr. */
    va_list ap;
    va_start(ap, sig);
    int nargs = mock_count_sig_args(sig ? sig : "");

    /* Record the string input args for later assertion (last call wins). */
    if (mock) {
        mock->last_call.has_call = true;
        snprintf(mock->last_call.iface, sizeof(mock->last_call.iface),
                 "%s", iface ? iface : "");
        snprintf(mock->last_call.member, sizeof(mock->last_call.member),
                 "%s", method ? method : "");
        mock->last_call.args[0] = '\0';
        size_t used = 0;
        for (int i = 0; i < nargs; i++) {
            const char *arg = va_arg(ap, const char *);
            if (!arg)
                arg = "";
            size_t want = strlen(arg) + (used ? 1 : 0);
            if (used + want < sizeof(mock->last_call.args)) {
                if (used) {
                    mock->last_call.args[used++] = ',';
                    mock->last_call.args[used] = '\0';
                }
                size_t alen = strlen(arg);
                memcpy(mock->last_call.args + used, arg, alen);
                used += alen;
                mock->last_call.args[used] = '\0';
            }
        }
    } else {
        for (int i = 0; i < nargs; i++)
            (void)va_arg(ap, const char *);
    }
    char **out = va_arg(ap, char **);
    if (out && e && e->value && rc >= 0)
        *out = strdup(e->value);
    va_end(ap);

    return rc;
}

static int mock_get_property(ip_bus_handle bus, const char *dest,
                             const char *path, const char *iface,
                             const char *prop, char **out_value) {
    (void)dest; (void)path;
    ip_dbus_mock *mock = (ip_dbus_mock *)bus;
    const ip_mock_expectation *e = ip_dbus_mock_find(mock, iface, prop);
    if (!e) return -ENXIO;
    if (out_value) *out_value = e->value ? strdup(e->value) : NULL;
    return e->rc;
}

static int mock_set_property(ip_bus_handle bus, const char *dest,
                             const char *path, const char *iface,
                             const char *prop, const char *value) {
    (void)dest; (void)path; (void)value;
    ip_dbus_mock *mock = (ip_dbus_mock *)bus;
    const ip_mock_expectation *e = ip_dbus_mock_find(mock, iface, prop);
    return e ? e->rc : -ENXIO;
}

static int mock_get_connection_creds(ip_bus_handle bus,
                                     const char *unique_name,
                                     uint32_t *pid, uint32_t *uid) {
    (void)unique_name;
    ip_dbus_mock *mock = (ip_dbus_mock *)bus;
    if (!mock || !pid || !uid) return -EINVAL;
    if (mock->creds_rc < 0)
        return mock->creds_rc;
    *pid = mock->creds_pid;
    *uid = mock->creds_uid;
    return 0;
}

static int mock_get_managed_objects(ip_bus_handle bus, const char *dest,
                                    const char *path, char **out_reply) {
    (void)dest; (void)path;
    ip_dbus_mock *mock = (ip_dbus_mock *)bus;
    const ip_mock_expectation *e = ip_dbus_mock_find(
        mock, IP_IFACE_OBJECT_MANAGER, "GetManagedObjects");
    if (!e) return -ENXIO;
    if (out_reply) *out_reply = e->value ? strdup(e->value) : NULL;
    return e->rc;
}

static int mock_subscribe_signal(ip_bus_handle bus, const char *iface,
                                 const char *member, ip_signal_cb cb,
                                 void *userdata) {
    ip_dbus_mock *mock = (ip_dbus_mock *)bus;
    if (!mock || !iface || !member) return -EINVAL;

    /* Allow tests to simulate subscribe failure. */
    if (mock->subscribe_fail_rc != 0)
        return mock->subscribe_fail_rc;

    if (mock->sub_count >= IP_MOCK_MAX_SUBSCRIPTIONS) return -ENOMEM;

    mock->subscriptions[mock->sub_count].iface    = iface;
    mock->subscriptions[mock->sub_count].member   = member;
    mock->subscriptions[mock->sub_count].cb       = cb;
    mock->subscriptions[mock->sub_count].userdata = userdata;
    mock->sub_count++;
    return 0;
}

static int mock_inject_signal(ip_bus_handle bus, const char *iface,
                              const char *member, const void *payload) {
    ip_dbus_mock *mock = (ip_dbus_mock *)bus;
    if (!mock || !iface || !member) return -EINVAL;

    for (int i = 0; i < mock->sub_count; i++) {
        if (strcmp(mock->subscriptions[i].iface, iface) == 0 &&
            strcmp(mock->subscriptions[i].member, member) == 0) {
            if (mock->subscriptions[i].cb) {
                mock->subscriptions[i].cb(
                    iface, member, payload,
                    mock->subscriptions[i].userdata);
            }
        }
    }
    return 0;
}

/* --- Mock process (dispatches queued signals) -------------------------- */

static int mock_process(ip_bus_handle bus) {
    ip_dbus_mock *mock = (ip_dbus_mock *)bus;
    if (!mock || mock->queued_signal_count <= 0)
        return 0;

    int dispatched = 0;
    for (int q = 0; q < mock->queued_signal_count; q++) {
        ip_mock_queued_signal *qs = &mock->queued_signals[q];
        for (int i = 0; i < mock->sub_count; i++) {
            if (strcmp(mock->subscriptions[i].iface, qs->iface) == 0 &&
                strcmp(mock->subscriptions[i].member, qs->member) == 0) {
                if (mock->subscriptions[i].cb)
                    mock->subscriptions[i].cb(
                        qs->iface, qs->member, &qs->noc_payload,
                        mock->subscriptions[i].userdata);
                dispatched++;
            }
        }
    }
    mock->queued_signal_count = 0;
    return dispatched;
}

int ip_dbus_mock_last_call(ip_dbus_mock *mock,
                            const char *iface,
                            const char *member,
                            char *out,
                            size_t outsz)
{
    if (!mock || !out || outsz == 0)
        return -EINVAL;
    if (!mock->last_call.has_call ||
        strcmp(mock->last_call.iface, iface ? iface : "") != 0 ||
        strcmp(mock->last_call.member, member ? member : "") != 0)
        return -ENOENT;
    snprintf(out, outsz, "%s", mock->last_call.args);
    return 0;
}

/* --- Backend accessor ------------------------------------------------------ */

static ip_dbus_backend s_mock_backend;

const ip_dbus_backend *ip_dbus_mock_backend(ip_dbus_mock *mock) {
    if (!mock) return NULL;
    s_mock_backend.connect              = mock_connect;
    s_mock_backend.disconnect           = mock_disconnect;
    s_mock_backend.get_unique_name      = mock_get_unique_name;
    s_mock_backend.get_connection_creds = mock_get_connection_creds;
    s_mock_backend.call_method          = mock_call_method;
    s_mock_backend.get_property         = mock_get_property;
    s_mock_backend.set_property         = mock_set_property;
    s_mock_backend.get_managed_objects  = mock_get_managed_objects;
    s_mock_backend.subscribe_signal      = mock_subscribe_signal;
    s_mock_backend.inject_signal         = mock_inject_signal;
    s_mock_backend.process               = mock_process;
    return &s_mock_backend;
}