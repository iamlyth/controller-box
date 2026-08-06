/*
 * dbus_mock.h — DBus mock interface abstraction (Task 3).
 *
 * Controller-Box communicates with InputPlumber over the system DBus via
 * sd-bus.  To make the DBus client layer unit-testable without a running
 * InputPlumber or system bus, the production code (src/dbus/dbus_client.c,
 * to be implemented in Task 9+) calls through a function-pointer vtable
 * (`ip_dbus_backend`) instead of sd_bus_* functions directly.
 *
 * This header defines the vtable interface and a simple mock backend
 * suitable for unit tests.  Tests register canned return values keyed by
 * (interface, member, path); the mock backend replays them when the
 * production code calls the corresponding vtable method.
 *
 * Later tasks (9–15) extend the production vtable usage and the mock's
 * canned-response store as needed, but the infrastructure — the struct,
 * the mock lifecycle, and the expectation helpers — is established here.
 */
#ifndef CBX_DBUS_MOCK_H
#define CBX_DBUS_MOCK_H

#include <stddef.h>

/*
 * InputPlumber DBus constants (SPEC §2.4, shared by production and tests).
 */
#define IP_DBUS_NAME "org.shadowblip.InputPlumber"
#define IP_DBUS_PATH "/org/shadowblip/InputPlumber"
#define IP_IFACE_MANAGER "org.shadowblip.InputManager"
#define IP_IFACE_COMPOSITE "org.shadowblip.Input.CompositeDevice"
#define IP_IFACE_PROPERTIES "org.freedesktop.DBus.Properties"
#define IP_IFACE_OBJECT_MANAGER "org.freedesktop.DBus.ObjectManager"

/*
 * Opaque handle representing the bus connection in the vtable.  In
 * production this wraps an `sd_bus *`; in tests it wraps the mock state.
 */
typedef void *ip_bus_handle;

/*
 * Callback type for signal subscriptions (production and mock).
 * `member` is the signal name (e.g. "PropertiesChanged"); `payload` is a
 * test-supplied opaque pointer registered with the expectation.
 */
typedef void (*ip_signal_cb)(const char *interface, const char *member,
                            const void *payload, void *userdata);

/*
 * Function-pointer vtable — the interface abstraction.
 * Production provides a real sd-bus implementation; tests provide the
 * mock implementation from dbus_mock.c.
 */
typedef struct ip_dbus_backend {
    /* Connection lifecycle */
    int  (*connect)(ip_bus_handle *bus);
    void (*disconnect)(ip_bus_handle bus);

    /* Resolve the unique bus name for `well_known` (e.g. IP_DBUS_NAME). */
    int  (*get_unique_name)(ip_bus_handle bus, const char *well_known,
                            char **out_unique);

    /* Generic method call.  `sig` is the sd-bus signature string; variadic
     * arguments are the call parameters.  Returns 0 on success, negative
     * errno on failure. */
    int  (*call_method)(ip_bus_handle bus, const char *dest,
                        const char *path, const char *iface,
                        const char *method, const char *sig, ...);

    /* Property get/set.  `value` is an output/input parameter. */
    int  (*get_property)(ip_bus_handle bus, const char *dest,
                         const char *path, const char *iface,
                         const char *prop, char **out_value);
    int  (*set_property)(ip_bus_handle bus, const char *dest,
                         const char *path, const char *iface,
                         const char *prop, const char *value);

    /* ObjectManager enumeration.  `out_reply` is a test-owned string
     * (captured DBus reply fixture) in the mock, an sd_bus_message in
     * production. */
    int  (*get_managed_objects)(ip_bus_handle bus, const char *dest,
                                const char *path, char **out_reply);

    /* Signal subscription.  `cb` is invoked when a matching signal is
     * delivered (via inject_signal in tests, via sd-bus process in prod). */
    int  (*subscribe_signal)(ip_bus_handle bus, const char *iface,
                             const char *member, ip_signal_cb cb,
                             void *userdata);

    /* Test-only: inject a fake signal message into the mock bus.  This
     * triggers any registered callbacks for (iface, member).  Production
     * implementations set this to NULL. */
    int  (*inject_signal)(ip_bus_handle bus, const char *iface,
                          const char *member, const void *payload);
} ip_dbus_backend;

/*
 * Mock backend — a simple canned-response store.
 *
 * Each expectation maps (iface, member) → a return code and an optional
 * string value.  When the production code calls a vtable method, the
 * mock looks up the expectation, returns the canned rc, and copies out
 * the canned value (if non-NULL).  A fixed-size table keeps the mock
 * dependency-free and simple; later tasks can extend it.
 */
#define IP_MOCK_MAX_EXPECTATIONS 32

typedef struct {
    const char *iface;
    const char *member;   /* method name, property name, or signal name */
    int         rc;       /* canned return code */
    char       *value;   /* canned string value (heap-owned by mock, or NULL) */
} ip_mock_expectation;

typedef struct {
    ip_mock_expectation expectations[IP_MOCK_MAX_EXPECTATIONS];
    int                 count;
    ip_bus_handle       bus;   /* opaque, points back to this struct */
} ip_dbus_mock;

/* --- Mock lifecycle -------------------------------------------------------- */

/* Initialise an empty mock state. */
void ip_dbus_mock_init(ip_dbus_mock *mock);

/* Free any heap-owned canned values. */
void ip_dbus_mock_free(ip_dbus_mock *mock);

/* Return a backend vtable backed by `mock`.  The vtable pointers remain
 * valid as long as `mock` is alive. */
const ip_dbus_backend *ip_dbus_mock_backend(ip_dbus_mock *mock);

/* --- Expectation helpers (test-side) --------------------------------------- */

/* Register a canned return value for (iface, member).  `value` is
 * strdup'd internally (may be NULL).  Returns 0 on success, -1 if the
 * table is full. */
int ip_dbus_mock_expect(ip_dbus_mock *mock, const char *iface,
                        const char *member, int rc, const char *value);

/* Convenience: expect a successful call returning `value`. */
int ip_dbus_mock_expect_ok(ip_dbus_mock *mock, const char *iface,
                            const char *member, const char *value);

/* Convenience: expect a failing call with error code `rc`. */
int ip_dbus_mock_expect_error(ip_dbus_mock *mock, const char *iface,
                              const char *member, int rc);

/* Look up a canned expectation.  Returns NULL if not found. */
const ip_mock_expectation *ip_dbus_mock_find(ip_dbus_mock *mock,
                                             const char *iface,
                                             const char *member);

/* Reset the mock to its initial (empty) state, freeing canned values. */
void ip_dbus_mock_reset(ip_dbus_mock *mock);

#endif /* CBX_DBUS_MOCK_H */