/*
 * dbus_mock.c — DBus mock interface abstraction (Task 3).
 *
 * Implements the mock backend vtable and the canned-response store.  The
 * mock replays expectations registered by tests; calls to unregistered
 * (iface, member) pairs return -ENXIO (no such entry), which tests can
 * assert against.
 */
#include "dbus_mock.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Mock store helpers ---------------------------------------------------- */

void ip_dbus_mock_init(ip_dbus_mock *mock) {
    if (!mock) return;
    memset(mock, 0, sizeof(*mock));
    mock->bus = mock;  /* opaque handle points back to the mock struct */
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
    /* Return a deterministic fake unique name. */
    *out_unique = strdup(":1.42");
    return 0;
}

static int mock_call_method(ip_bus_handle bus, const char *dest,
                            const char *path, const char *iface,
                            const char *method, const char *sig, ...) {
    (void)dest; (void)path; (void)sig;
    ip_dbus_mock *mock = (ip_dbus_mock *)bus;
    const ip_mock_expectation *e = ip_dbus_mock_find(mock, iface, method);
    return e ? e->rc : -ENXIO;
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

/* --- Backend accessor ------------------------------------------------------ */

static ip_dbus_backend s_mock_backend;

const ip_dbus_backend *ip_dbus_mock_backend(ip_dbus_mock *mock) {
    if (!mock) return NULL;
    s_mock_backend.connect              = mock_connect;
    s_mock_backend.disconnect           = mock_disconnect;
    s_mock_backend.get_unique_name      = mock_get_unique_name;
    s_mock_backend.call_method          = mock_call_method;
    s_mock_backend.get_property         = mock_get_property;
    s_mock_backend.set_property         = mock_set_property;
    s_mock_backend.get_managed_objects  = mock_get_managed_objects;
    s_mock_backend.subscribe_signal      = mock_subscribe_signal;
    s_mock_backend.inject_signal         = mock_inject_signal;
    return &s_mock_backend;
}