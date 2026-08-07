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
#define IP_IFACE_SOURCE_EVENT "org.shadowblip.Input.Source.EventDevice"
#define IP_IFACE_SOURCE_UDEV "org.shadowblip.Input.Source.UdevDevice"
#define IP_IFACE_SOURCE_HIDRAW "org.shadowblip.Input.Source.HIDRawDevice"
#define IP_IFACE_TARGET "org.shadowblip.Input.Target"
#define IP_IFACE_DBUS_DEVICE "org.shadowblip.Input.DBusDevice"

/*
 * Opaque handle representing the bus connection in the vtable.  In
 * production this wraps an `sd_bus *`; in tests it wraps the mock state.
 */
typedef void *ip_bus_handle;

/*
 * Callback type for signal subscriptions (production and mock).
 * `member` is the signal name (e.g. "PropertiesChanged"); `payload` is a
 * signal-specific struct (see ip_owner_changed_payload below).
 */
typedef void (*ip_signal_cb)(const char *interface, const char *member,
                            const void *payload, void *userdata);

/*
 * NameOwnerChanged signal payload (Task 9).
 * Shared by production (sd-bus backend parses the message into this struct)
 * and tests (test constructs and injects it via inject_signal).
 */
typedef struct {
    const char *name;       /* well-known name (e.g. IP_DBUS_NAME) */
    const char *old_owner;  /* previous unique name ("" if just acquired) */
    const char *new_owner;  /* new unique name ("" if just lost) */
} ip_owner_changed_payload;

/*
 * InterfacesAdded / InterfacesRemoved signal payload (Task 11).
 * `interfaces` is a comma-separated list of interface names
 * (e.g. "org.shadowblip.Input.CompositeDevice,org.shadowblip.Input.DBusDevice").
 * In production, the sd-bus callback builds this string from the message;
 * in tests, the test constructs and injects it directly.
 */
typedef struct {
    const char *sender;       /* unique bus name of the signal sender */
    const char *path;         /* object path of the added/removed object */
    const char *interfaces;   /* comma-separated interface names */
} ip_interfaces_changed_payload;

/*
 * PropertiesChanged signal payload (Task 11).
 * Represents a single property change within a PropertiesChanged signal.
 * The production callback calls the handler once per changed property we
 * care about; tests construct and inject individual payloads.
 */
typedef enum {
    IP_PROP_TYPE_STRING     = 0,  /* 's' — string value in `value` */
    IP_PROP_TYPE_ARRAY      = 1,  /* 'as' — comma-separated values in `value`, count in `array_count` */
    IP_PROP_TYPE_INVALIDATED = 2, /* property invalidated (value is NULL) */
} ip_prop_type;

typedef struct {
    const char  *sender;       /* unique bus name of the signal sender */
    const char  *iface_name;   /* interface that emitted the change */
    const char  *prop_name;    /* property name */
    ip_prop_type prop_type;    /* value type */
    const char  *value;        /* string (for s), comma-separated (for as), NULL (invalidated) */
    int          array_count;  /* number of elements (for as), 0 otherwise */
} ip_properties_changed_payload;

/*
 * InputEvent signal payload (Task 14).
 * DBusDevice interface emits InputEvent(event: s, value: d) during
 * intercept mode.  The production callback parses the sd-bus message
 * into this struct; tests construct and inject it directly.
 */
typedef struct {
    const char *sender;   /* unique bus name of the signal sender */
    const char *path;     /* DBusDevice object path */
    const char *event;    /* raw event string (e.g. "A", "Up", "LeftStickX") */
    double      value;    /* event value (buttons: 0.0 or 1.0; axes: -1.0..1.0) */
} ip_input_event_payload;

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

/*
 * Signal subscription table (Task 9 extension).
 * Stores registered callbacks for (iface, member) pairs so that
 * inject_signal can dispatch to the correct callback.
 */
#define IP_MOCK_MAX_SUBSCRIPTIONS 8

typedef struct {
    const char   *iface;
    const char   *member;
    ip_signal_cb  cb;
    void         *userdata;
} ip_mock_subscription;

typedef struct {
    ip_mock_expectation  expectations[IP_MOCK_MAX_EXPECTATIONS];
    int                  count;
    ip_bus_handle        bus;   /* opaque, points back to this struct */
    ip_mock_subscription subscriptions[IP_MOCK_MAX_SUBSCRIPTIONS];
    int                  sub_count;
    int                  subscribe_fail_rc;  /* 0 = normal, <0 = fail subscribe */
} ip_dbus_mock;

/* --- Mock lifecycle -------------------------------------------------------- */

/* Initialise an empty mock state. */
void ip_dbus_mock_init(ip_dbus_mock *mock);

/* Free any heap-owned canned values. */
void ip_dbus_mock_free(ip_dbus_mock *mock);

/* Return a backend vtable backed by `mock`.  The vtable pointers remain
 * valid as long as `mock` is alive. */
const ip_dbus_backend *ip_dbus_mock_backend(ip_dbus_mock *mock);

/* Return the production sd-bus backend vtable.  Defined in
 * src/dbus/dbus_client.c; declared here so production code (manager.c)
 * can obtain it without a dedicated header. */
const ip_dbus_backend *ip_dbus_sd_backend(void);

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