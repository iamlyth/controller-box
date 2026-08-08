/*
 * dbus_client.c — Production sd-bus backend vtable implementation.
 *
 * Implements the ip_dbus_backend function-pointer vtable using real
 * sd-bus calls.  This is linked into the production binary; unit tests
 * use the mock backend from tests/dbus_mock.c instead.
 *
 * Task 9 implements: connect, disconnect, get_unique_name, get_property,
 * subscribe_signal, inject_signal (stub).  Later tasks extend with
 * call_method, set_property, get_managed_objects.
 */
#include "dbus_mock.h"      /* vtable interface + constants */
#include "ip_connection.h"  /* ip_owner_changed_payload */
#include "ip_properties.h"  /* ip_prop_type */
#include "ip_input_signal.h" /* ip_input_event_payload */

#include <systemd/sd-bus.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Production bus handle wrapper --------------------------------------- */
/* ip_bus_handle is void *; in production it points to this struct. */

#define MAX_SD_SLOTS 16

typedef struct {
    sd_bus      *bus;
    sd_bus_slot *slots[MAX_SD_SLOTS];
    void        *slot_data[MAX_SD_SLOTS];   /* heap-allocated callback data */
    int          slot_count;
} sd_bus_wrapper;

/* Forward declaration — defined after the vtable stubs section. */
static int translate_sd_error(int rc, const sd_bus_error *error);

/* Callback data for signal subscriptions. */
typedef struct {
    ip_signal_cb  cb;
    void         *userdata;
} sd_signal_data;

/* --- sd-bus signal callback for NameOwnerChanged ------------------------- */

static int
sd_noc_callback(sd_bus_message *msg, void *userdata, sd_bus_error *ret_error)
{
    (void)ret_error;
    sd_signal_data *data = (sd_signal_data *)userdata;
    if (!data || !data->cb)
        return 0;

    const char *name, *old_owner, *new_owner;
    int r = sd_bus_message_read(msg, "sss", &name, &old_owner, &new_owner);
    if (r < 0)
        return 0;  /* ignore parse errors — don't kill the bus */

    ip_owner_changed_payload payload = {
        .name      = name,
        .old_owner = old_owner,
        .new_owner = new_owner,
    };

    data->cb("org.freedesktop.DBus", "NameOwnerChanged",
             &payload, data->userdata);
    return 0;
}

/* --- sd-bus signal callback for InterfacesAdded (Task 11) --------------- */
/* Message signature: oa{sa{sv}}
 * We only need the object path and the interface names (not property
 * values), so we serialise interfaces into a comma-separated string. */
static int
sd_interfaces_added_callback(sd_bus_message *msg, void *userdata,
                               sd_bus_error *ret_error)
{
    (void)ret_error;
    sd_signal_data *data = (sd_signal_data *)userdata;
    if (!data || !data->cb)
        return 0;

    const char *sender = sd_bus_message_get_sender(msg);
    const char *path   = NULL;

    int r = sd_bus_message_read(msg, "o", &path);
    if (r < 0)
        return 0;  /* ignore parse errors */

    /* Read a{sa{sv}} — interface names only. */
    char  *buf = NULL;
    size_t sz  = 0;
    FILE  *fp  = open_memstream(&buf, &sz);
    if (!fp)
        return 0;

    r = sd_bus_message_enter_container(msg, 'a', "{sa{sv}}");
    if (r < 0) {
        fclose(fp);
        free(buf);
        return 0;
    }

    bool first = true;
    while ((r = sd_bus_message_enter_container(msg, 'e', "sa{sv}")) > 0) {
        const char *iface = NULL;
        r = sd_bus_message_read(msg, "s", &iface);
        if (r < 0) {
            fclose(fp);
            free(buf);
            return 0;
        }

        r = sd_bus_message_skip(msg, "a{sv}");
        if (r < 0) {
            fclose(fp);
            free(buf);
            return 0;
        }

        r = sd_bus_message_exit_container(msg);
        if (r < 0) {
            fclose(fp);
            free(buf);
            return 0;
        }

        if (!first)
            fputc(',', fp);
        fputs(iface ? iface : "", fp);
        first = false;
    }
    if (r < 0) {
        fclose(fp);
        free(buf);
        return 0;
    }

    r = sd_bus_message_exit_container(msg);
    fclose(fp);
    if (r < 0) {
        free(buf);
        return 0;
    }

    ip_interfaces_changed_payload payload = {
        .sender     = sender,
        .path       = path,
        .interfaces = buf,
    };
    data->cb(IP_IFACE_OBJECT_MANAGER, "InterfacesAdded",
             &payload, data->userdata);

    free(buf);
    return 0;
}

/* --- sd-bus signal callback for InterfacesRemoved (Task 11) ------------ */
/* Message signature: oas */
static int
sd_interfaces_removed_callback(sd_bus_message *msg, void *userdata,
                                  sd_bus_error *ret_error)
{
    (void)ret_error;
    sd_signal_data *data = (sd_signal_data *)userdata;
    if (!data || !data->cb)
        return 0;

    const char *sender = sd_bus_message_get_sender(msg);
    const char *path    = NULL;

    int r = sd_bus_message_read(msg, "o", &path);
    if (r < 0)
        return 0;

    /* Read 'as' — array of removed interface names. */
    char  *buf = NULL;
    size_t sz  = 0;
    FILE  *fp  = open_memstream(&buf, &sz);
    if (!fp)
        return 0;

    r = sd_bus_message_enter_container(msg, 'a', "s");
    if (r < 0) {
        fclose(fp);
        free(buf);
        return 0;
    }

    bool first = true;
    const char *iface = NULL;
    while ((r = sd_bus_message_read(msg, "s", &iface)) > 0) {
        if (!first)
            fputc(',', fp);
        fputs(iface ? iface : "", fp);
        first = false;
    }

    sd_bus_message_exit_container(msg);
    fclose(fp);

    ip_interfaces_changed_payload payload = {
        .sender     = sender,
        .path       = path,
        .interfaces = buf,
    };
    data->cb(IP_IFACE_OBJECT_MANAGER, "InterfacesRemoved",
             &payload, data->userdata);

    free(buf);
    return 0;
}

/* --- sd-bus signal callback for PropertiesChanged (Task 11) ------------ */
/* Message signature: sa{sv}as
 * For each changed property we care about, we extract the value and
 * dispatch a separate payload.  For string arrays, we build a
 * comma-separated string (heap-allocated, freed after dispatch). */
static int
sd_properties_changed_callback(sd_bus_message *msg, void *userdata,
                                 sd_bus_error *ret_error)
{
    (void)ret_error;
    sd_signal_data *data = (sd_signal_data *)userdata;
    if (!data || !data->cb)
        return 0;

    const char *sender = sd_bus_message_get_sender(msg);
    const char *iface_name = NULL;

    int r = sd_bus_message_read(msg, "s", &iface_name);
    if (r < 0)
        return 0;

    /* Read a{sv} — changed properties dict. */
    r = sd_bus_message_enter_container(msg, 'a', "{sv}");
    if (r < 0)
        return 0;

    while ((r = sd_bus_message_enter_container(msg, 'e', "sv")) > 0) {
        const char *prop_name = NULL;
        r = sd_bus_message_read(msg, "s", &prop_name);
        if (r < 0)
            break;

        /* Peek at the variant to determine the inner type. */
        const char *contents_ptr = NULL;
        char vtype = sd_bus_message_peek_type(msg, NULL, &contents_ptr);
        if (vtype < 0)
            break;

        if (vtype == 'v' && contents_ptr) {
            /* Enter the variant container. */
            r = sd_bus_message_enter_container(msg, 'v', contents_ptr);
            if (r < 0)
                break;

            /* Peek inside the variant to get the actual type. */
            char inner_type = 0;
            const char *inner_sig = NULL;
            r = sd_bus_message_peek_type(msg, &inner_type, &inner_sig);
            if (r < 0) {
                sd_bus_message_exit_container(msg);
                break;
            }

            if (inner_type == 's') {
                /* String property. */
                const char *value = NULL;
                r = sd_bus_message_read(msg, "s", &value);
                if (r >= 0) {
                    ip_properties_changed_payload payload = {
                        .sender      = sender,
                        .iface_name  = iface_name,
                        .prop_name   = prop_name,
                        .prop_type   = IP_PROP_TYPE_STRING,
                        .value       = value,
                        .array_count = 0,
                    };
                    data->cb(IP_IFACE_PROPERTIES, "PropertiesChanged",
                             &payload, data->userdata);
                }
            } else if (inner_type == 'a' && inner_sig &&
                       strcmp(inner_sig, "s") == 0) {
                /* String array property: build comma-separated string. */
                r = sd_bus_message_enter_container(msg, 'a', "s");
                if (r >= 0) {
                    char  *buf = NULL;
                    size_t sz  = 0;
                    FILE  *fp  = open_memstream(&buf, &sz);
                    if (fp) {
                        bool first = true;
                        const char *elem = NULL;
                        int count = 0;
                        while ((r = sd_bus_message_read(msg, "s", &elem)) > 0) {
                            if (!first)
                                fputc(',', fp);
                            fputs(elem ? elem : "", fp);
                            first = false;
                            count++;
                        }
                        fclose(fp);

                        ip_properties_changed_payload payload = {
                            .sender      = sender,
                            .iface_name  = iface_name,
                            .prop_name   = prop_name,
                            .prop_type   = IP_PROP_TYPE_ARRAY,
                            .value       = buf,
                            .array_count = count,
                        };
                        data->cb(IP_IFACE_PROPERTIES, "PropertiesChanged",
                                 &payload, data->userdata);
                        free(buf);
                    }
                    sd_bus_message_exit_container(msg);
                }
            } else {
                /* Unknown variant type — skip it. */
                sd_bus_message_skip(msg, NULL);
            }

            sd_bus_message_exit_container(msg);  /* exit variant */
        } else {
            /* Not a variant — skip. */
            sd_bus_message_skip(msg, NULL);
        }

        r = sd_bus_message_exit_container(msg);  /* exit {sv} */
        if (r < 0)
            break;
    }

    sd_bus_message_exit_container(msg);  /* exit a{sv} */

    /* Read 'as' — invalidated properties. */
    r = sd_bus_message_enter_container(msg, 'a', "s");
    if (r >= 0) {
        const char *inv_name = NULL;
        while ((r = sd_bus_message_read(msg, "s", &inv_name)) > 0) {
            ip_properties_changed_payload payload = {
                .sender      = sender,
                .iface_name  = iface_name,
                .prop_name   = inv_name,
                .prop_type   = IP_PROP_TYPE_INVALIDATED,
                .value       = NULL,
                .array_count = 0,
            };
            data->cb(IP_IFACE_PROPERTIES, "PropertiesChanged",
                     &payload, data->userdata);
        }
        sd_bus_message_exit_container(msg);
    }

    return 0;
}

/* --- sd-bus signal callback for InputEvent (Task 14) --------------------- */

static int
sd_input_event_callback(sd_bus_message *msg, void *userdata,
                         sd_bus_error *ret_error)
{
    (void)ret_error;
    sd_signal_data *data = (sd_signal_data *)userdata;
    if (!data || !data->cb)
        return 0;

    const char *sender = sd_bus_message_get_sender(msg);
    const char *path   = sd_bus_message_get_path(msg);

    const char *event = NULL;
    double      value  = 0.0;

    int r = sd_bus_message_read(msg, "sd", &event, &value);
    if (r < 0)
        return 0;

    ip_input_event_payload payload = {
        .sender = sender,
        .path   = path,
        .event  = event,
        .value  = value,
    };

    data->cb(IP_IFACE_DBUS_DEVICE, "InputEvent",
             &payload, data->userdata);
    return 0;
}

/* --- Vtable: connect ----------------------------------------------------- */

static int
sd_connect(ip_bus_handle *bus)
{
    if (!bus)
        return -EINVAL;

    sd_bus_wrapper *w = calloc(1, sizeof(*w));
    if (!w)
        return -ENOMEM;

    int r = sd_bus_open_system(&w->bus);
    if (r < 0) {
        free(w);
        return r;
    }

    *bus = w;
    return 0;
}

/* --- Vtable: disconnect -------------------------------------------------- */

static void
sd_disconnect(ip_bus_handle bus)
{
    if (!bus)
        return;
    sd_bus_wrapper *w = (sd_bus_wrapper *)bus;

    for (int i = 0; i < w->slot_count; i++) {
        if (w->slots[i])
            w->slots[i] = sd_bus_slot_unref(w->slots[i]);
        free(w->slot_data[i]);
        w->slot_data[i] = NULL;
    }

    if (w->bus) {
        sd_bus_close(w->bus);
        w->bus = sd_bus_unref(w->bus);
    }
    free(w);
}

/* --- Vtable: get_unique_name -------------------------------------------- */

static int
sd_get_unique_name(ip_bus_handle bus, const char *well_known,
                   char **out_unique)
{
    sd_bus_wrapper *w = (sd_bus_wrapper *)bus;
    if (!w || !well_known || !out_unique)
        return -EINVAL;

    sd_bus_error   error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;

    int r = sd_bus_call_method(w->bus,
        "org.freedesktop.DBus",      /* destination */
        "/org/freedesktop/DBus",     /* path */
        "org.freedesktop.DBus",      /* interface */
        "GetNameOwner",              /* method */
        &error, &reply,
        "s", well_known);

    if (r < 0) {
        /* NameHasNoOwner → InputPlumber not running yet. */
        if (sd_bus_error_has_name(&error,
                "org.freedesktop.DBus.Error.NameHasNoOwner")) {
            r = IP_ERR_SERVICE_UNKNOWN;
        }
        sd_bus_error_free(&error);
        return r;
    }

    const char *unique = NULL;
    r = sd_bus_message_read(reply, "s", &unique);
    if (r < 0) {
        sd_bus_message_unref(reply);
        sd_bus_error_free(&error);
        return r;
    }

    *out_unique = strdup(unique);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);

    return *out_unique ? 0 : -ENOMEM;
}

/* --- Vtable: get_property ------------------------------------------------ */

static int
sd_get_property(ip_bus_handle bus, const char *dest,
                const char *path, const char *iface,
                const char *prop, char **out_value)
{
    sd_bus_wrapper *w = (sd_bus_wrapper *)bus;
    if (!w || !dest || !path || !iface || !prop)
        return -EINVAL;

    sd_bus_error error = SD_BUS_ERROR_NULL;
    char        *value = NULL;

    int r = sd_bus_get_property_string(w->bus, dest, path, iface, prop,
                                        &error, &value);

    if (r < 0) {
        /* Translate known DBus error names to categorized errno codes. */
        if (sd_bus_error_has_name(&error,
                "org.freedesktop.DBus.Error.ServiceUnknown"))
            r = IP_ERR_SERVICE_UNKNOWN;
        else if (sd_bus_error_has_name(&error,
                "org.freedesktop.DBus.Error.NameHasNoOwner"))
            r = IP_ERR_SERVICE_UNKNOWN;
        else if (sd_bus_error_has_name(&error,
                "org.freedesktop.DBus.Error.AccessDenied"))
            r = IP_ERR_ACCESS_DENIED;
        else if (sd_bus_error_has_name(&error,
                "org.freedesktop.DBus.Error.NoReply"))
            r = IP_ERR_NO_REPLY;
        else if (sd_bus_error_has_name(&error,
                "org.freedesktop.DBus.Error.InvalidArgs"))
            r = IP_ERR_INVALID_ARGS;
        /* Otherwise r is already the negative errno from sd-bus. */

        sd_bus_error_free(&error);
        free(value);
        return r;
    }

    if (out_value)
        *out_value = value;   /* value is heap-allocated by sd-bus */
    else
        free(value);

    sd_bus_error_free(&error);
    return 0;
}

/* --- Vtable: subscribe_signal -------------------------------------------- */

static int
sd_subscribe_signal(ip_bus_handle bus, const char *iface,
                    const char *member, ip_signal_cb cb,
                    void *userdata)
{
    sd_bus_wrapper *w = (sd_bus_wrapper *)bus;
    if (!w || !iface || !member || !cb)
        return -EINVAL;
    if (w->slot_count >= MAX_SD_SLOTS)
        return -ENOMEM;

    sd_signal_data *data = malloc(sizeof(*data));
    if (!data)
        return -ENOMEM;
    data->cb       = cb;
    data->userdata = userdata;

    sd_bus_slot *slot = NULL;
    int r;

    if (strcmp(iface, "org.freedesktop.DBus") == 0 &&
        strcmp(member, "NameOwnerChanged") == 0) {
        /* NameOwnerChanged — add arg0 filter for InputPlumber name. */
        const char *match =
            "type='signal',"
            "sender='org.freedesktop.DBus',"
            "interface='org.freedesktop.DBus',"
            "member='NameOwnerChanged',"
            "arg0='org.shadowblip.InputPlumber'";
        r = sd_bus_add_match(w->bus, &slot, match, sd_noc_callback, data);
    } else if (strcmp(iface, IP_IFACE_OBJECT_MANAGER) == 0 &&
               strcmp(member, "InterfacesAdded") == 0) {
        char match[512];
        snprintf(match, sizeof(match),
                 "type='signal',interface='%s',member='%s',path='%s'",
                 iface, member, IP_DBUS_PATH);
        r = sd_bus_add_match(w->bus, &slot, match,
                             sd_interfaces_added_callback, data);
    } else if (strcmp(iface, IP_IFACE_OBJECT_MANAGER) == 0 &&
               strcmp(member, "InterfacesRemoved") == 0) {
        char match[512];
        snprintf(match, sizeof(match),
                 "type='signal',interface='%s',member='%s',path='%s'",
                 iface, member, IP_DBUS_PATH);
        r = sd_bus_add_match(w->bus, &slot, match,
                             sd_interfaces_removed_callback, data);
    } else if (strcmp(iface, IP_IFACE_PROPERTIES) == 0 &&
               strcmp(member, "PropertiesChanged") == 0) {
        char match[512];
        snprintf(match, sizeof(match),
                 "type='signal',interface='%s',member='%s'",
                 iface, member);
        r = sd_bus_add_match(w->bus, &slot, match,
                             sd_properties_changed_callback, data);
    } else if (strcmp(iface, IP_IFACE_DBUS_DEVICE) == 0 &&
               strcmp(member, "InputEvent") == 0) {
        /* InputEvent signal from DBusDevice interface. */
        char match[512];
        snprintf(match, sizeof(match),
                 "type='signal',interface='%s',member='%s'",
                 iface, member);
        r = sd_bus_add_match(w->bus, &slot, match,
                             sd_input_event_callback, data);
    } else {
        /* Generic signal subscription (fallback for future signal types). */
        char match[512];
        snprintf(match, sizeof(match),
                 "type='signal',interface='%s',member='%s'",
                 iface, member);
        r = sd_bus_add_match(w->bus, &slot, match, sd_noc_callback, data);
    }

    if (r < 0) {
        free(data);
        return r;
    }

    w->slots[w->slot_count]     = slot;
    w->slot_data[w->slot_count] = data;
    w->slot_count++;
    return 0;
}

/* --- Vtable: call_method (Task 12) --------------------------------------- */
/*
 * Production sd-bus method call.  Creates a method-call message, appends
 * input args based on `sig`, calls the method, and optionally reads a
 * string reply.
 *
 * Calling convention (shared with the mock backend):
 *   - `sig` encodes the input argument types.  Each 's' is one string
 *     arg; 'as' is one array-of-strings arg (passed as a comma-separated
 *     string, split here into a DBus array).
 *   - The last variadic argument is always a char **out_value: NULL for
 *     void methods, a valid pointer for methods that return a string.
 *   - On success with a non-NULL out_value, *out_value is set to a
 *     heap-allocated copy of the reply string.
 */

/* Append a comma-separated string as a DBus string array ('as'). */
static int
sd_append_string_array(sd_bus_message *m, const char *csv)
{
    int r = sd_bus_message_open_container(m, 'a', "s");
    if (r < 0)
        return r;

    if (csv && *csv) {
        const char *p = csv;
        while (*p) {
            const char *comma = strchr(p, ',');
            size_t len = comma ? (size_t)(comma - p) : strlen(p);

            char *elem = malloc(len + 1);
            if (!elem) {
                sd_bus_message_close_container(m);
                return -ENOMEM;
            }
            memcpy(elem, p, len);
            elem[len] = '\0';

            r = sd_bus_message_append_basic(m, 's', elem);
            free(elem);
            if (r < 0) {
                sd_bus_message_close_container(m);
                return r;
            }

            if (!comma)
                break;
            p = comma + 1;
        }
    }

    return sd_bus_message_close_container(m);
}

static int
sd_call_method(ip_bus_handle bus, const char *dest,
               const char *path, const char *iface,
               const char *method, const char *sig, ...)
{
    sd_bus_wrapper *w = (sd_bus_wrapper *)bus;
    if (!w || !dest || !path || !iface || !method)
        return -EINVAL;

    sd_bus_error   error = SD_BUS_ERROR_NULL;
    sd_bus_message *m    = NULL;
    sd_bus_message *reply = NULL;
    int r;

    r = sd_bus_message_new_method_call(w->bus, &m, dest, path, iface, method);
    if (r < 0)
        return r;

    /* Append input args based on sig. */
    va_list ap;
    va_start(ap, sig);

    if (sig) {
        for (const char *p = sig; *p; p++) {
            if (*p == 'a' && p[1] == 's') {
                p++;  /* skip element type */
                const char *csv = va_arg(ap, const char *);
                r = sd_append_string_array(m, csv);
                if (r < 0)
                    goto fail_va;
            } else if (*p == 's') {
                const char *str = va_arg(ap, const char *);
                r = sd_bus_message_append_basic(m, 's', str);
                if (r < 0)
                    goto fail_va;
            } else {
                /* Unsupported type — skip the corresponding arg. */
                (void)va_arg(ap, const char *);
            }
        }
    }

    /* Read the output pointer (always present as the last variadic arg). */
    char **out = va_arg(ap, char **);
    va_end(ap);

    r = sd_bus_call(w->bus, m, 0, &error, &reply);
    if (r < 0) {
        r = translate_sd_error(r, &error);
        goto fail;
    }

    if (out) {
        const char *str = NULL;
        r = sd_bus_message_read(reply, "s", &str);
        if (r < 0)
            goto fail;
        *out = strdup(str);
        if (!*out) {
            r = -ENOMEM;
            goto fail;
        }
    }

    sd_bus_message_unref(m);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    return 0;

fail_va:
    va_end(ap);
fail:
    sd_bus_message_unref(m);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    return r;
}

/* --- Vtable: set_property (Task 12) ------------------------------------- */
/*
 * Production sd-bus property setter.  Uses the Properties.Set method call
 * with signature ssv (interface, property, variant).  For array
 * properties, the variant contains an 'as' built from the comma-separated
 * value string.  For string properties, the variant contains an 's'.
 */

/* Properties that are string arrays (as) — need variant "as". */
static bool
sd_is_array_property(const char *prop)
{
    return strcmp(prop, "GamepadOrder") == 0 ||
           strcmp(prop, "TargetDevices") == 0 ||
           strcmp(prop, "SourceDevicePaths") == 0 ||
           strcmp(prop, "SupportedTargetDeviceIds") == 0 ||
           strcmp(prop, "SupportedTargetDevices") == 0 ||
           strcmp(prop, "Capabilities") == 0 ||
           strcmp(prop, "OutputCapabilities") == 0 ||
           strcmp(prop, "TargetCapabilities") == 0 ||
           strcmp(prop, "DbusDevices") == 0;
}

/* Properties that are uint32 (u) — need variant "u". */
static bool
sd_is_uint_property(const char *prop)
{
    return strcmp(prop, "InterceptMode") == 0;
}

static int
sd_set_property(ip_bus_handle bus, const char *dest,
                const char *path, const char *iface,
                const char *prop, const char *value)
{
    sd_bus_wrapper *w = (sd_bus_wrapper *)bus;
    if (!w || !dest || !path || !iface || !prop || !value)
        return -EINVAL;

    sd_bus_error   error = SD_BUS_ERROR_NULL;
    sd_bus_message *m    = NULL;
    sd_bus_message *reply = NULL;
    int r;

    r = sd_bus_message_new_method_call(w->bus, &m, dest, path,
                                         IP_IFACE_PROPERTIES, "Set");
    if (r < 0)
        return r;

    /* Append: interface name (s), property name (s). */
    r = sd_bus_message_append_basic(m, 's', iface);
    if (r < 0)
        goto fail;
    r = sd_bus_message_append_basic(m, 's', prop);
    if (r < 0)
        goto fail;

    /* Build the variant value (v). */
    if (sd_is_array_property(prop)) {
        r = sd_bus_message_open_container(m, 'v', "as");
        if (r < 0)
            goto fail;
        r = sd_append_string_array(m, value);
        if (r < 0)
            goto fail;
        r = sd_bus_message_close_container(m);  /* close variant */
        if (r < 0)
            goto fail;
    } else if (sd_is_uint_property(prop)) {
        /* InterceptMode is uint32 — parse the string value. */
        char *end = NULL;
        unsigned long uval = strtoul(value, &end, 10);
        if (!end || *end != '\0') {
            r = -EINVAL;
            goto fail;
        }
        r = sd_bus_message_open_container(m, 'v', "u");
        if (r < 0)
            goto fail;
        r = sd_bus_message_append_basic(m, 'u', &uval);
        if (r < 0)
            goto fail;
        r = sd_bus_message_close_container(m);  /* close variant */
        if (r < 0)
            goto fail;
    } else {
        /* Default: string property. */
        r = sd_bus_message_open_container(m, 'v', "s");
        if (r < 0)
            goto fail;
        r = sd_bus_message_append_basic(m, 's', value);
        if (r < 0)
            goto fail;
        r = sd_bus_message_close_container(m);  /* close variant */
        if (r < 0)
            goto fail;
    }

    r = sd_bus_call(w->bus, m, 0, &error, &reply);
    if (r < 0) {
        r = translate_sd_error(r, &error);
        goto fail;
    }

    sd_bus_message_unref(m);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    return 0;

fail:
    sd_bus_message_unref(m);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    return r;
}

/* --- Vtable: get_managed_objects (Task 10) ------------------------------- */

/* Translate sd-bus errors to categorized codes (same as get_property). */
static int
translate_sd_error(int rc, const sd_bus_error *error)
{
    if (rc >= 0)
        return rc;
    if (sd_bus_error_has_name(error,
            "org.freedesktop.DBus.Error.ServiceUnknown"))
        return IP_ERR_SERVICE_UNKNOWN;
    if (sd_bus_error_has_name(error,
            "org.freedesktop.DBus.Error.NameHasNoOwner"))
        return IP_ERR_SERVICE_UNKNOWN;
    if (sd_bus_error_has_name(error,
            "org.freedesktop.DBus.Error.AccessDenied"))
        return IP_ERR_ACCESS_DENIED;
    if (sd_bus_error_has_name(error,
            "org.freedesktop.DBus.Error.NoReply"))
        return IP_ERR_NO_REPLY;
    if (sd_bus_error_has_name(error,
            "org.freedesktop.DBus.Error.InvalidArgs"))
        return IP_ERR_INVALID_ARGS;
    return rc;  /* already negative errno */
}

/*
 * Calls GetManagedObjects() on the ObjectManager interface at `path`,
 * iterates the a{oa{sa{sv}}} reply, and serialises it into a text
 * representation (one line per object: "path\tiface1,iface2,…").
 *
 * The text format is consumed by cbx_objectmanager_parse_reply(), which
 * is shared by the production and mock backends.
 */
static int
sd_get_managed_objects(ip_bus_handle bus, const char *dest,
                       const char *path, char **out_reply)
{
    sd_bus_wrapper *w = (sd_bus_wrapper *)bus;
    if (!w || !dest || !path || !out_reply)
        return -EINVAL;

    sd_bus_error   error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;

    int r = sd_bus_call_method(w->bus, dest, path,
                               IP_IFACE_OBJECT_MANAGER, "GetManagedObjects",
                               &error, &reply, "");
    if (r < 0) {
        r = translate_sd_error(r, &error);
        goto cleanup;
    }

    /* Build text representation via open_memstream. */
    char  *buf = NULL;
    size_t sz   = 0;
    FILE  *fp   = open_memstream(&buf, &sz);
    if (!fp) {
        r = -ENOMEM;
        goto cleanup;
    }

    /* Iterate the outer array: a{oa{sa{sv}}} */
    r = sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}");
    if (r < 0) {
        fclose(fp);
        free(buf);
        goto cleanup;
    }

    while ((r = sd_bus_message_enter_container(reply, 'e',
                                                "oa{sa{sv}}")) > 0) {
        const char *obj_path = NULL;
        r = sd_bus_message_read(reply, "o", &obj_path);
        if (r < 0) {
            fclose(fp);
            free(buf);
            goto cleanup;
        }

        /* Write path + tab to the stream first. */
        fputs(obj_path ? obj_path : "", fp);
        fputc('\t', fp);

        /* Inner array: a{sa{sv}} — interface → properties */
        r = sd_bus_message_enter_container(reply, 'a', "{sa{sv}}");
        if (r < 0) {
            fclose(fp);
            free(buf);
            goto cleanup;
        }

        /* Iterate interfaces, writing comma-separated names. */
        bool first = true;
        while ((r = sd_bus_message_enter_container(reply, 'e',
                                                    "sa{sv}")) > 0) {
            const char *iface = NULL;
            r = sd_bus_message_read(reply, "s", &iface);
            if (r < 0) {
                fclose(fp);
                free(buf);
                goto cleanup;
            }

            /* Skip the a{sv} property dict — we don't need values. */
            r = sd_bus_message_skip(reply, "a{sv}");
            if (r < 0) {
                fclose(fp);
                free(buf);
                goto cleanup;
            }

            r = sd_bus_message_exit_container(reply);  /* exit {sa{sv}} */
            if (r < 0) {
                fclose(fp);
                free(buf);
                goto cleanup;
            }

            if (!first)
                fputc(',', fp);
            fputs(iface ? iface : "", fp);
            first = false;
        }
        if (r < 0) {
            fclose(fp);
            free(buf);
            goto cleanup;
        }

        r = sd_bus_message_exit_container(reply);  /* exit a{sa{sv}} */
        if (r < 0) {
            fclose(fp);
            free(buf);
            goto cleanup;
        }

        r = sd_bus_message_exit_container(reply);  /* exit {oa{sa{sv}}} */
        if (r < 0) {
            fclose(fp);
            free(buf);
            goto cleanup;
        }

        fputc('\n', fp);
    }
    if (r < 0) {
        fclose(fp);
        free(buf);
        goto cleanup;
    }

    r = sd_bus_message_exit_container(reply);  /* exit a{oa{sa{sv}}} */

    fclose(fp);

    if (r < 0) {
        free(buf);
        goto cleanup;
    }

    *out_reply = buf;
    r = 0;

cleanup:
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    return r;
}

static int
sd_inject_signal(ip_bus_handle bus, const char *iface,
                 const char *member, const void *payload)
{
    (void)bus; (void)iface; (void)member; (void)payload;
    return -ENOSYS;  /* Production does not inject signals */
}

static int
sd_process(ip_bus_handle bus)
{
    sd_bus_wrapper *w = (sd_bus_wrapper *)bus;
    if (!w || !w->bus)
        return -EINVAL;

    /* Process one pending DBus message (non-blocking).
     * Returns 0 if no messages pending, >0 if a message was processed. */
    int r = sd_bus_process(w->bus, NULL);
    if (r < 0)
        return -errno;
    return r;
}

/* --- Backend accessor ---------------------------------------------------- */

static const ip_dbus_backend s_sd_backend = {
    .connect              = sd_connect,
    .disconnect           = sd_disconnect,
    .get_unique_name      = sd_get_unique_name,
    .call_method          = sd_call_method,
    .get_property         = sd_get_property,
    .set_property         = sd_set_property,
    .get_managed_objects  = sd_get_managed_objects,
    .subscribe_signal     = sd_subscribe_signal,
    .inject_signal        = sd_inject_signal,
    .process              = sd_process,
};

const ip_dbus_backend *
ip_dbus_sd_backend(void)
{
    return &s_sd_backend;
}