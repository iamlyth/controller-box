/*
 * dbus_interface.h — Production DBus interface abstraction.
 *
 * Controller-Box communicates with InputPlumber over the system DBus via
 * sd-bus.  To make the DBus client layer unit-testable without a running
 * InputPlumber or system bus, the production code calls through a
 * function-pointer vtable (`ip_dbus_backend`) instead of sd_bus_*
 * functions directly.
 *
 * This header defines the vtable interface, shared DBus constants, signal
 * payload structs, and the production sd-bus backend accessor.  Test code
 * uses the mock backend defined in tests/dbus_mock.h, which includes this
 * header for the shared definitions.
 *
 * SPEC §2.4, §10.2.
 */
#ifndef CBX_DBUS_INTERFACE_H
#define CBX_DBUS_INTERFACE_H

#include <stddef.h>
#include <stdint.h>

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

    /* Query the DBus daemon for the owning connection's credentials for
     * `unique_name`.  Fills *pid and *uid from GetConnectionCredentials
     * (UnixProcessID / UnixUserID).  Used for sender verification so a
     * name-squatting process that grabs InputPlumber's well-known name
     * is detected by its process/user identity rather than trusted
     * merely because it owns the name (F3, SPEC §10.1).
     *
     * Returns 0 on success, negative errno on failure (including when
     * the name has no owner). */
    int  (*get_connection_creds)(ip_bus_handle bus, const char *unique_name,
                                 uint32_t *pid, uint32_t *uid);

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

    /* Process pending DBus messages (signals, method replies).
     * In production, calls sd_bus_process() to dispatch one message.
     * Returns >0 if a message was processed, 0 if no messages pending,
     * negative errno on error.  Mock implementations return 0 (no-op:
     * signals are injected directly via inject_signal). */
    int  (*process)(ip_bus_handle bus);
} ip_dbus_backend;

/*
 * Production sd-bus backend accessor.
 * Defined in src/dbus/dbus_client.c; declared here so production code
 * (manager.c, overlay_service.c) and tests can obtain it without a
 * dedicated wrapper header.
 */
const ip_dbus_backend *ip_dbus_sd_backend(void);

#endif /* CBX_DBUS_INTERFACE_H */