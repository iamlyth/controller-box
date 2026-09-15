/*
 * ip_properties.c — PropertiesChanged signal handling (Task 11).
 *
 * Validates and dispatches property changes from InputPlumber's
 * PropertiesChanged signals.  Only tracked properties are processed;
 * type mismatches, length violations, and array-size violations are
 * silently dropped.
 *
 * SPEC §10.1 — Property changes (see header for tracked properties).
 *
 * Security:
 *   - Sender verification: payload->sender must match expected_sender.
 *   - Type validation: the prop_type must match the expected type for the
 *     property name (e.g., ProfileName expects STRING, GamepadOrder expects
 *     ARRAY).  Mismatches are rejected.
 *   - String length limits: names max 256 bytes, paths max 4096 bytes.
 *   - Array size limits: max 256 elements.
 */
#include "ip_properties.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* --- Tracked property specifications -------------------------------------- */

/* Object-path class a property is allowed to originate from. */
typedef enum {
    PROP_PATH_MANAGER = 0,   /* exactly IP_DBUS_PATH "/Manager" */
    PROP_PATH_COMPOSITE,     /* IP_DBUS_PATH "/CompositeDevice<N>" */
} prop_path_class;

typedef struct {
    const char     *name;           /* property name */
    const char     *iface;          /* emitting interface */
    prop_path_class path_class;     /* allowed object-path class */
    ip_prop_type    expected_type;  /* STRING or ARRAY */
    size_t          max_element_len;/* max length of each string element */
} prop_spec;

/* Properties we care about and their expected interface/path/type. */
static const prop_spec s_prop_specs[] = {
    /* Manager interface properties — emitted only by the Manager object. */
    { "GamepadOrder",      IP_IFACE_MANAGER,   PROP_PATH_MANAGER,
      IP_PROP_TYPE_ARRAY,  IP_PROP_MAX_NAME_LEN },

    /* CompositeDevice interface properties — emitted by CompositeDeviceN. */
    { "ProfileName",       IP_IFACE_COMPOSITE, PROP_PATH_COMPOSITE,
      IP_PROP_TYPE_STRING, IP_PROP_MAX_NAME_LEN },
    { "ProfilePath",       IP_IFACE_COMPOSITE, PROP_PATH_COMPOSITE,
      IP_PROP_TYPE_STRING, IP_PROP_MAX_PATH_LEN },
    { "TargetDevices",     IP_IFACE_COMPOSITE, PROP_PATH_COMPOSITE,
      IP_PROP_TYPE_ARRAY,  IP_PROP_MAX_NAME_LEN },
    { "SourceDevicePaths", IP_IFACE_COMPOSITE, PROP_PATH_COMPOSITE,
      IP_PROP_TYPE_ARRAY,  IP_PROP_MAX_PATH_LEN },
};

#define NUM_PROP_SPECS \
    (sizeof(s_prop_specs) / sizeof(s_prop_specs[0]))

/* Look up a property spec by name.  Returns NULL if not tracked. */
static const prop_spec *
find_prop_spec(const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < NUM_PROP_SPECS; i++) {
        if (strcmp(s_prop_specs[i].name, name) == 0)
            return &s_prop_specs[i];
    }
    return NULL;
}

/* --- Internal helpers ---------------------------------------------------- */

/* Verify that the signal sender matches the expected InputPlumber name. */
static bool
sender_ok(const ip_properties *props, const char *sender)
{
    if (!props || !props->expected_sender || !sender)
        return false;
    return strcmp(sender, props->expected_sender) == 0;
}

/* Verify the emitting interface matches the property's declared interface. */
static bool
iface_ok(const prop_spec *spec, const char *iface)
{
    return spec && iface && strcmp(iface, spec->iface) == 0;
}

/* Verify that `path` is a well-formed object path of the expected class.
 * Rejecting the wrong path class here means a Manager property spoofed on
 * a composite path (or vice versa) never reaches the per-device dispatch. */
static bool
path_ok(prop_path_class cls, const char *path)
{
    if (!path)
        return false;
    const size_t root_len = sizeof(IP_DBUS_PATH) - 1;
    if (strncmp(path, IP_DBUS_PATH, root_len) != 0)
        return false;
    const char *rest = path + root_len;

    if (cls == PROP_PATH_MANAGER)
        return strcmp(rest, "/Manager") == 0;

    if (cls == PROP_PATH_COMPOSITE) {
        static const char prefix[] = "/CompositeDevice";
        const size_t plen = sizeof(prefix) - 1;
        if (strncmp(rest, prefix, plen) != 0)
            return false;
        const char *digits = rest + plen;
        if (*digits == '\0')
            return false;
        for (const char *p = digits; *p; p++) {
            if (!isdigit((unsigned char)*p))
                return false;
        }
        return true;
    }

    return false;
}

/* Validate a single string value against a max length. */
static bool
string_within_limit(const char *value, size_t max_len)
{
    if (!value)
        return false;
    return strlen(value) <= max_len;
}

/* Validate a comma-separated array: count elements and check each element
 * length.  `value` is the comma-separated string.  Returns the element
 * count, or -1 if any element exceeds `max_element_len` or the total
 * count exceeds `max_elems`. */
static int
validate_array(const char *value, size_t max_element_len, int max_elems)
{
    if (!value)
        return -1;

    int count = 0;
    const char *p = value;

    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);

        if (len > max_element_len)
            return -1;

        count++;
        if (count > max_elems)
            return -1;

        if (!comma)
            break;
        p = comma + 1;
    }

    /* An empty string yields zero elements (valid); any non-empty string
     * yields count >= 1 from the loop above, so no special-casing here. */
    return count;
}

/* --- Signal callback (registered via vtable subscribe_signal) ------------- */

static void
properties_signal_cb(const char *iface, const char *member,
                     const void *payload, void *userdata)
{
    (void)iface;
    (void)member;
    ip_properties *props = (ip_properties *)userdata;
    const ip_properties_changed_payload *p =
        (const ip_properties_changed_payload *)payload;
    if (!props || !p)
        return;
    ip_properties_handle_changed(props, p);
}

/* --- Public API ---------------------------------------------------------- */

void
ip_properties_init(ip_properties *props, const ip_dbus_backend *backend,
                   ip_bus_handle bus, const char *expected_sender,
                   ip_prop_changed_cb cb, void *cb_userdata)
{
    if (!props)
        return;
    memset(props, 0, sizeof(*props));
    props->backend         = backend;
    props->bus             = bus;
    props->expected_sender = expected_sender;
    props->cb              = cb;
    props->cb_userdata      = cb_userdata;
}

int
ip_properties_subscribe(ip_properties *props)
{
    if (!props || !props->backend || !props->backend->subscribe_signal)
        return -EINVAL;

    return props->backend->subscribe_signal(
        props->bus, IP_IFACE_PROPERTIES, "PropertiesChanged",
        properties_signal_cb, props);
}

/* Dispatch a validated value of the declared type to the user callback. */
static void
dispatch_value(ip_properties *props, const ip_properties_changed_payload *p,
               const prop_spec *spec, const char *value)
{
    if (p->prop_type == IP_PROP_TYPE_STRING) {
        if (!string_within_limit(value, spec->max_element_len))
            return;
        props->cb(p->object_path, spec->iface, spec->name,
                  IP_PROP_TYPE_STRING, value, 0, props->cb_userdata);
    } else if (p->prop_type == IP_PROP_TYPE_ARRAY) {
        int count = validate_array(value, spec->max_element_len,
                                    IP_PROP_MAX_ARRAY_ELEMS);
        if (count < 0)
            return;
        props->cb(p->object_path, spec->iface, spec->name,
                  IP_PROP_TYPE_ARRAY, value, count, props->cb_userdata);
    }
}

/* Handle an INVALIDATED change with one bounded authoritative read.  When
 * the read succeeds and validates, dispatch the fresh value so consumers
 * apply real state instead of clearing on an unreliable signal; when it
 * fails, propagate the invalidation so the consumer clears its entry. */
static void
dispatch_invalidated_read(ip_properties *props,
                          const ip_properties_changed_payload *p,
                          const prop_spec *spec)
{
    char *fresh = NULL;
    bool  dispatched = false;

    if (props->backend && props->backend->get_property) {
        int rc = props->backend->get_property(props->bus, IP_DBUS_NAME,
                                               p->object_path, spec->iface,
                                               spec->name, &fresh);
        if (rc == 0 && fresh) {
            if (spec->expected_type == IP_PROP_TYPE_STRING) {
                if (string_within_limit(fresh, spec->max_element_len)) {
                    props->cb(p->object_path, spec->iface, spec->name,
                              IP_PROP_TYPE_STRING, fresh, 0,
                              props->cb_userdata);
                    dispatched = true;
                }
            } else if (spec->expected_type == IP_PROP_TYPE_ARRAY) {
                int count = validate_array(fresh, spec->max_element_len,
                                            IP_PROP_MAX_ARRAY_ELEMS);
                if (count >= 0) {
                    props->cb(p->object_path, spec->iface, spec->name,
                              IP_PROP_TYPE_ARRAY, fresh, count,
                              props->cb_userdata);
                    dispatched = true;
                }
            }
        }
    }
    free(fresh);

    if (!dispatched) {
        props->cb(p->object_path, spec->iface, spec->name,
                  IP_PROP_TYPE_INVALIDATED, NULL, -1, props->cb_userdata);
    }
}

void
ip_properties_handle_changed(ip_properties *props,
                               const ip_properties_changed_payload *payload)
{
    if (!props || !payload || !props->cb)
        return;

    /* Sender verification. */
    if (!sender_ok(props, payload->sender))
        return;

    /* Check if this is a property we care about. */
    const prop_spec *spec = find_prop_spec(payload->prop_name);
    if (!spec)
        return;  /* untracked property — silently ignore */

    /* Interface and object-path validation: a property must arrive on the
     * interface and object-path class that declares it. */
    if (!iface_ok(spec, payload->iface_name))
        return;
    if (!path_ok(spec->path_class, payload->object_path))
        return;

    /* Handle invalidated properties with a bounded authoritative read. */
    if (payload->prop_type == IP_PROP_TYPE_INVALIDATED) {
        dispatch_invalidated_read(props, payload, spec);
        return;
    }

    /* Type validation: must match expected type. */
    if (payload->prop_type != spec->expected_type)
        return;  /* type mismatch — reject */

    dispatch_value(props, payload, spec, payload->value);
}