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

#include <errno.h>
#include <string.h>

/* --- Tracked property specifications -------------------------------------- */

typedef struct {
    const char  *name;             /* property name */
    ip_prop_type expected_type;    /* STRING or ARRAY */
    size_t       max_element_len;  /* max length of each string element */
} prop_spec;

/* Properties we care about and their expected types. */
static const prop_spec s_prop_specs[] = {
    /* Manager interface properties */
    { "GamepadOrder",      IP_PROP_TYPE_ARRAY,  IP_PROP_MAX_NAME_LEN },

    /* CompositeDevice interface properties */
    { "ProfileName",       IP_PROP_TYPE_STRING, IP_PROP_MAX_NAME_LEN },
    { "ProfilePath",       IP_PROP_TYPE_STRING, IP_PROP_MAX_PATH_LEN },
    { "TargetDevices",     IP_PROP_TYPE_ARRAY,  IP_PROP_MAX_NAME_LEN },
    { "SourceDevicePaths", IP_PROP_TYPE_ARRAY,  IP_PROP_MAX_PATH_LEN },
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

    /* Handle empty string: zero elements is valid. */
    if (count == 0 && value[0] != '\0')
        count = 1;  /* single element with no comma */

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
    if (!props || !props->backend)
        return -EINVAL;

    return props->backend->subscribe_signal(
        props->bus, IP_IFACE_PROPERTIES, "PropertiesChanged",
        properties_signal_cb, props);
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

    /* Handle invalidated properties. */
    if (payload->prop_type == IP_PROP_TYPE_INVALIDATED) {
        props->cb(payload->prop_name, IP_PROP_TYPE_INVALIDATED,
                  NULL, -1, props->cb_userdata);
        return;
    }

    /* Type validation: must match expected type. */
    if (payload->prop_type != spec->expected_type)
        return;  /* type mismatch — reject */

    if (payload->prop_type == IP_PROP_TYPE_STRING) {
        /* Validate string length limit. */
        if (!string_within_limit(payload->value, spec->max_element_len))
            return;
        props->cb(payload->prop_name, IP_PROP_TYPE_STRING,
                  payload->value, 0, props->cb_userdata);
    } else if (payload->prop_type == IP_PROP_TYPE_ARRAY) {
        /* Validate array size and element lengths. */
        int count = validate_array(payload->value, spec->max_element_len,
                                    IP_PROP_MAX_ARRAY_ELEMS);
        if (count < 0)
            return;
        props->cb(payload->prop_name, IP_PROP_TYPE_ARRAY,
                  payload->value, count, props->cb_userdata);
    }
}