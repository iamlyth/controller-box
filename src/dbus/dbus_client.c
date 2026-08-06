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

#include <systemd/sd-bus.h>

#include <errno.h>
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
    } else {
        /* Generic signal subscription — later tasks add specific parsers. */
        char match[512];
        snprintf(match, sizeof(match),
                 "type='signal',interface='%s',member='%s'",
                 iface, member);
        r = sd_bus_add_match(w->bus, &slot, match, sd_noc_callback, data);
        /* Note: for non-NOC signals, sd_noc_callback will try to read
         * "sss" which will fail silently. Later tasks replace this with
         * signal-specific callbacks. */
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

/* --- Vtable: stubs (implemented in later tasks) -------------------------- */

static int
sd_call_method(ip_bus_handle bus, const char *dest,
               const char *path, const char *iface,
               const char *method, const char *sig, ...)
{
    (void)bus; (void)dest; (void)path; (void)iface; (void)method; (void)sig;
    return -ENOSYS;  /* Task 12 */
}

static int
sd_set_property(ip_bus_handle bus, const char *dest,
                const char *path, const char *iface,
                const char *prop, const char *value)
{
    (void)bus; (void)dest; (void)path; (void)iface; (void)prop; (void)value;
    return -ENOSYS;  /* Task 12 */
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
};

const ip_dbus_backend *
ip_dbus_sd_backend(void)
{
    return &s_sd_backend;
}