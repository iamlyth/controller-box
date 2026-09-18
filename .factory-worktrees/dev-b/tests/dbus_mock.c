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
    mock->get_property_count = 0;
    mock->set_property_count = 0;
    mock->gamepad_order_written = false;
    mock->gamepad_order_value[0] = '\0';
    mock->target_devices_written = false;
    mock->target_devices_value[0] = '\0';
    for (int i = 0; i < mock->count; i++)
        mock->expectations[i].calls = 0;
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
            mock->expectations[i].calls = 0;
            mock->expectations[i].has_last_args = false;
            mock->expectations[i].last_args[0] = '\0';
            return 0;
        }
    }

    ip_mock_expectation *e = &mock->expectations[mock->count++];
    e->iface  = iface;
    e->member = member;
    e->rc     = rc;
    e->value  = value ? strdup(value) : NULL;
    e->calls  = 0;
    e->has_last_args = false;
    e->last_args[0] = '\0';
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

/* Mutable lookup used by the vtable callbacks to record consumption. */
static ip_mock_expectation *
mock_find_mut(ip_dbus_mock *mock, const char *iface, const char *member)
{
    if (!mock || !iface || !member) return NULL;
    for (int i = 0; i < mock->count; i++) {
        if (strcmp(mock->expectations[i].iface, iface) == 0 &&
            strcmp(mock->expectations[i].member, member) == 0)
            return &mock->expectations[i];
    }
    return NULL;
}

int ip_dbus_mock_call_count(ip_dbus_mock *mock, const char *iface,
                            const char *member) {
    const ip_mock_expectation *e = ip_dbus_mock_find(mock, iface, member);
    return e ? e->calls : 0;
}

int ip_dbus_mock_last_call_args(ip_dbus_mock *mock, const char *iface,
                                const char *member, char *out,
                                size_t outsz) {
    if (!mock || !iface || !member || !out || outsz == 0)
        return -EINVAL;
    const ip_mock_expectation *e = ip_dbus_mock_find(mock, iface, member);
    if (!e || !e->has_last_args)
        return -ENOENT;
    snprintf(out, outsz, "%s", e->last_args);
    return 0;
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

static int mock_set_deadline(ip_bus_handle bus, uint64_t deadline_ms) {
    ip_dbus_mock *mock = (ip_dbus_mock *)bus;
    if (!mock) return -EINVAL;
    mock->deadline_ms = deadline_ms;
    mock->set_deadline_count++;
    return 0;
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
    ip_mock_expectation *e = mock_find_mut(mock, iface, method);
    if (e) e->calls++;
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
    if (e) {
        snprintf(e->last_args, sizeof(e->last_args), "%s",
                 mock ? mock->last_call.args : "");
        e->has_last_args = true;
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
    if (mock)
        mock->get_property_count++;
    ip_mock_expectation *e = mock_find_mut(mock, iface, prop);
    if (e) e->calls++;
    if (!e) return -ENXIO;
    if (out_value) {
        if (strcmp(iface, IP_IFACE_COMPOSITE) == 0 &&
            strcmp(prop, "TargetDevices") == 0 &&
            mock->target_devices_written)
            *out_value = strdup(mock->target_devices_value);
        else
            *out_value = e->value ? strdup(e->value) : NULL;
    }
    return e->rc;
}

static int mock_set_property(ip_bus_handle bus, const char *dest,
                             const char *path, const char *iface,
                             const char *prop, const char *value) {
    (void)dest; (void)path;
    ip_dbus_mock *mock = (ip_dbus_mock *)bus;
    if (mock)
        mock->set_property_count++;
    ip_mock_expectation *e = mock_find_mut(mock, iface, prop);
    if (e) e->calls++;
    if (mock) {
        snprintf(mock->last_set_iface, sizeof(mock->last_set_iface), "%s",
                 iface ? iface : "");
        snprintf(mock->last_set_prop, sizeof(mock->last_set_prop), "%s",
                 prop ? prop : "");
        snprintf(mock->last_set_value, sizeof(mock->last_set_value), "%s",
                 value ? value : "");
    }
    if (e && e->rc == 0 && strcmp(iface, IP_IFACE_COMPOSITE) == 0 &&
        strcmp(prop, "TargetDevices") == 0) {
        snprintf(mock->target_devices_value,
                 sizeof(mock->target_devices_value), "%s", value ? value : "");
        mock->target_devices_written = true;
    }
    if (e && e->rc == 0 && strcmp(iface, IP_IFACE_MANAGER) == 0 &&
        strcmp(prop, "GamepadOrder") == 0) {
        snprintf(mock->gamepad_order_value,
                 sizeof(mock->gamepad_order_value), "%s", value ? value : "");
        mock->gamepad_order_written = true;
    }
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
    ip_mock_expectation *e = mock_find_mut(
        mock, IP_IFACE_OBJECT_MANAGER, "GetManagedObjects");
    if (e) e->calls++;
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

    /* Idempotent re-subscription: a repeat subscribe for the same
     * (interface, member) refreshes the existing binding instead of
     * appending a duplicate entry.  Mirrors sd_subscribe_signal so the
     * recovery re-wire path (startup + each InputPlumber restart) never
     * grows the subscription array or double-dispatches signals. */
    for (int i = 0; i < mock->sub_count; i++) {
        if (strcmp(mock->subscriptions[i].iface, iface) == 0 &&
            strcmp(mock->subscriptions[i].member, member) == 0) {
            mock->subscriptions[i].cb       = cb;
            mock->subscriptions[i].userdata = userdata;
            return 0;
        }
    }

    if (mock->sub_count >= IP_MOCK_MAX_SUBSCRIPTIONS) return -ENOMEM;

    mock->subscriptions[mock->sub_count].iface    = iface;
    mock->subscriptions[mock->sub_count].member   = member;
    mock->subscriptions[mock->sub_count].cb       = cb;
    mock->subscriptions[mock->sub_count].userdata = userdata;
    mock->sub_count++;
    return 0;
}

static int mock_unsubscribe_signal(ip_bus_handle bus, const char *iface,
                                   const char *member, ip_signal_cb cb,
                                   void *userdata) {
    (void)cb;
    (void)userdata;
    ip_dbus_mock *mock = (ip_dbus_mock *)bus;
    if (!mock || !iface || !member) return -EINVAL;

    /* Clear the matching binding's callback instead of compacting the
     * array: inject_signal/process iterate it live and a re-subscribe
     * refreshes the same (iface, member) slot, so no duplicate is added
     * across acquire/release cycles. */
    for (int i = 0; i < mock->sub_count; i++) {
        if (strcmp(mock->subscriptions[i].iface, iface) == 0 &&
            strcmp(mock->subscriptions[i].member, member) == 0) {
            mock->subscriptions[i].cb       = NULL;
            mock->subscriptions[i].userdata = NULL;
            return 0;
        }
    }
    return 0;   /* no matching subscription: idempotent no-op */
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
    s_mock_backend.unsubscribe_signal    = mock_unsubscribe_signal;
    s_mock_backend.inject_signal         = mock_inject_signal;
    s_mock_backend.process               = mock_process;
    s_mock_backend.set_deadline          = mock_set_deadline;
    return &s_mock_backend;
}