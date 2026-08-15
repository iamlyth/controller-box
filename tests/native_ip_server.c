/*
 * native_ip_server.c — Reusable private InputPlumber-compatible DBus server.
 *
 * See native_ip_server.h for documentation.
 */
#include "native_ip_server.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <systemd/sd-bus.h>

#include "dbus_mock.h"  /* IP_DBUS_NAME, IP_IFACE_* */

/* --- Global state (visible to parent before fork) --- */

char   g_nip_profile_path[NIP_MAX_COMPOSITES][256];
char   g_nip_profile_name[NIP_MAX_COMPOSITES][64];
char   g_nip_gamepad_order[NIP_MAX_COMPOSITES][256];
int    g_nip_gamepad_order_count = 0;

char   g_nip_target_paths[NIP_MAX_TARGETS][256];
char   g_nip_target_types[NIP_MAX_TARGETS][32];
int    g_nip_target_count = 0;

char   g_nip_attached[NIP_MAX_COMPOSITES][NIP_MAX_ATTACHED][256];
int    g_nip_attached_counts[NIP_MAX_COMPOSITES];

uint32_t g_nip_intercept_mode[NIP_MAX_COMPOSITES];
char   g_nip_dbus_devices[NIP_MAX_COMPOSITES][256];
char   g_nip_comp_names[NIP_MAX_COMPOSITES][64];
char   g_nip_persistent_ids[NIP_MAX_COMPOSITES][32];

/* Server configuration (set by parent before fork, read by child) */
static int      s_num_composites = 1;
static char     s_version[32] = "9.8.7";
static volatile sig_atomic_t s_service_running = 1;

/* ================================================================== */
/*  Helpers                                                            */
/* ================================================================== */

static int composite_idx_from_path(const char *path)
{
    const char *p = strstr(path, "CompositeDevice");
    if (p) return atoi(p + strlen("CompositeDevice"));
    return -1;
}

static void stop_service(int signo)
{
    (void)signo;
    s_service_running = 0;
}

/* ================================================================== */
/*  Manager property getter/setter                                     */
/* ================================================================== */

static int
manager_property_get(sd_bus *bus, const char *path, const char *interface,
                      const char *property, sd_bus_message *reply,
                      void *userdata, sd_bus_error *error)
{
    (void)bus; (void)path; (void)interface; (void)userdata; (void)error;

    if (strcmp(property, "Version") == 0)
        return sd_bus_message_append(reply, "s", s_version);
    if (strcmp(property, "SupportedTargetDeviceIds") == 0) {
        int rc = sd_bus_message_open_container(reply, 'a', "s");
        if (rc < 0) return rc;
        rc = sd_bus_message_append(reply, "s", "xb360");
        if (rc >= 0) rc = sd_bus_message_append(reply, "s", "ds5");
        if (rc >= 0) rc = sd_bus_message_append(reply, "s", "gamepad");
        if (rc >= 0) rc = sd_bus_message_close_container(reply);
        return rc;
    }
    if (strcmp(property, "InterceptMode") == 0)
        return sd_bus_message_append(reply, "u", (uint32_t)2);
    if (strcmp(property, "Enabled") == 0)
        return sd_bus_message_append(reply, "b", 1);
    if (strcmp(property, "GamepadOrder") == 0) {
        int rc = sd_bus_message_open_container(reply, 'a', "s");
        if (rc < 0) return rc;
        for (int i = 0; i < g_nip_gamepad_order_count; i++) {
            rc = sd_bus_message_append(reply, "s", g_nip_gamepad_order[i]);
            if (rc < 0) return rc;
        }
        return sd_bus_message_close_container(reply);
    }
    return -ENOENT;
}

static int
manager_property_set(sd_bus *bus, const char *path, const char *interface,
                      const char *property, sd_bus_message *value,
                      void *userdata, sd_bus_error *error)
{
    (void)bus; (void)path; (void)interface; (void)userdata; (void)error;
    if (strcmp(property, "GamepadOrder") == 0) {
        int rc = sd_bus_message_enter_container(value, 'a', "s");
        if (rc < 0) return rc;
        g_nip_gamepad_order_count = 0;
        const char *p = NULL;
        while ((rc = sd_bus_message_read_basic(value, 's', &p)) > 0) {
            if (g_nip_gamepad_order_count < NIP_MAX_COMPOSITES) {
                snprintf(g_nip_gamepad_order[g_nip_gamepad_order_count],
                         sizeof(g_nip_gamepad_order[g_nip_gamepad_order_count]),
                         "%s", p);
                g_nip_gamepad_order_count++;
            }
        }
        if (rc < 0) return rc;
        return sd_bus_message_exit_container(value);
    }
    return -ENOENT;
}

/* ================================================================== */
/*  Target property (fallback vtable)                                   */
/* ================================================================== */

static int
target_property_get(sd_bus *bus, const char *path, const char *interface,
                     const char *property, sd_bus_message *reply,
                     void *userdata, sd_bus_error *error)
{
    (void)bus; (void)interface; (void)userdata; (void)error;
    if (strcmp(property, "DeviceType") != 0)
        return -ENOENT;
    for (int i = 0; i < g_nip_target_count; i++) {
        if (strcmp(g_nip_target_paths[i], path) == 0)
            return sd_bus_message_append(reply, "s", g_nip_target_types[i]);
    }
    return -ENOENT;
}

static const sd_bus_vtable target_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("DeviceType", "s", target_property_get, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END
};

static int
target_find(sd_bus *bus, const char *path, const char *interface,
             void *userdata, void **ret_found, sd_bus_error *error)
{
    (void)bus; (void)interface; (void)userdata; (void)error;
    for (int i = 0; i < g_nip_target_count; i++) {
        if (strcmp(g_nip_target_paths[i], path) == 0) {
            *ret_found = (void *)(intptr_t)(i + 1);
            return 1;
        }
    }
    return 0;
}

/* ================================================================== */
/*  Composite property getter/setter                                    */
/* ================================================================== */

static int
composite_property_get(sd_bus *bus, const char *path, const char *interface,
                        const char *property, sd_bus_message *reply,
                        void *userdata, sd_bus_error *error)
{
    (void)bus; (void)interface; (void)userdata; (void)error;
    int ci = composite_idx_from_path(path);
    if (ci < 0 || ci >= NIP_MAX_COMPOSITES) return -ENOENT;

    if (strcmp(property, "TargetDevices") == 0) {
        int rc = sd_bus_message_open_container(reply, 'a', "s");
        if (rc < 0) return rc;
        for (int j = 0; j < g_nip_attached_counts[ci]; j++) {
            rc = sd_bus_message_append(reply, "s", g_nip_attached[ci][j]);
            if (rc < 0) break;
        }
        if (rc >= 0) rc = sd_bus_message_close_container(reply);
        return rc;
    }
    if (strcmp(property, "ProfileName") == 0)
        return sd_bus_message_append(reply, "s", g_nip_profile_name[ci]);
    if (strcmp(property, "ProfilePath") == 0)
        return sd_bus_message_append(reply, "s", g_nip_profile_path[ci]);
    if (strcmp(property, "PersistentId") == 0)
        return sd_bus_message_append(reply, "s", g_nip_persistent_ids[ci]);
    if (strcmp(property, "InterceptMode") == 0)
        return sd_bus_message_append(reply, "u", g_nip_intercept_mode[ci]);
    if (strcmp(property, "DbusDevices") == 0) {
        int rc = sd_bus_message_open_container(reply, 'a', "s");
        if (rc < 0) return rc;
        if (g_nip_dbus_devices[ci][0]) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s", g_nip_dbus_devices[ci]);
            char *tok = strtok(buf, ",");
            while (tok) {
                rc = sd_bus_message_append(reply, "s", tok);
                if (rc < 0) break;
                tok = strtok(NULL, ",");
            }
        }
        if (rc >= 0) rc = sd_bus_message_close_container(reply);
        return rc;
    }
    if (strcmp(property, "Name") == 0)
        return sd_bus_message_append(reply, "s", g_nip_comp_names[ci]);
    return -ENOENT;
}

static int
composite_property_set(sd_bus *bus, const char *path, const char *interface,
                        const char *property, sd_bus_message *value,
                        void *userdata, sd_bus_error *error)
{
    (void)bus; (void)interface; (void)userdata; (void)error;
    int ci = composite_idx_from_path(path);
    if (ci < 0 || ci >= NIP_MAX_COMPOSITES) return -ENOENT;

    if (strcmp(property, "InterceptMode") == 0) {
        uint32_t mode;
        int rc = sd_bus_message_read(value, "u", &mode);
        if (rc < 0) return rc;
        g_nip_intercept_mode[ci] = mode;
        return 0;
    }
    return -ENOENT;
}

/* ================================================================== */
/*  Composite method handlers                                           */
/* ================================================================== */

static int
method_load_profile_path(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
    (void)userdata; (void)error;
    const char *profile_path = NULL;
    int rc = sd_bus_message_read(m, "s", &profile_path);
    if (rc < 0) return rc;
    const char *obj_path = sd_bus_message_get_path(m);
    int ci = composite_idx_from_path(obj_path);
    if (ci < 0 || ci >= NIP_MAX_COMPOSITES)
        return sd_bus_error_set(error, "org.freedesktop.DBus.Error.UnknownObject",
                                "composite not found");
    snprintf(g_nip_profile_path[ci], sizeof(g_nip_profile_path[ci]),
             "%s", profile_path);
    const char *base = strrchr(profile_path, '/');
    base = base ? base + 1 : profile_path;
    snprintf(g_nip_profile_name[ci], sizeof(g_nip_profile_name[ci]),
             "%s", base);
    char *dot = strstr(g_nip_profile_name[ci], ".yaml");
    if (dot) *dot = '\0';
    return sd_bus_reply_method_return(m, "");
}

static int
method_set_intercept_activation(sd_bus_message *m, void *userdata,
                                  sd_bus_error *error)
{
    (void)userdata; (void)error;
    /* Read (as events, s target) — consume and reply OK. */
    int rc = sd_bus_message_enter_container(m, 'a', "s");
    if (rc < 0) return rc;
    const char *s = NULL;
    while ((rc = sd_bus_message_read_basic(m, 's', &s)) > 0)
        ;
    if (rc < 0) return rc;
    rc = sd_bus_message_exit_container(m);
    if (rc < 0) return rc;
    rc = sd_bus_message_read(m, "s", &s);
    if (rc < 0) return rc;
    return sd_bus_reply_method_return(m, "");
}

/* ================================================================== */
/*  DBusDevice interface (InputEvent signal + test trigger)             */
/* ================================================================== */

static int
method_emit_input_event(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
    (void)userdata; (void)error;
    const char *event = NULL;
    const char *value_str = NULL;
    int rc = sd_bus_message_read(m, "ss", &event, &value_str);
    if (rc < 0) return rc;

    /* Parse the value string to double. */
    double value = strtod(value_str, NULL);

    sd_bus *bus = sd_bus_message_get_bus(m);
    const char *path = sd_bus_message_get_path(m);

    /* Emit the InputEvent signal with native (sd) signature. */
    rc = sd_bus_emit_signal(bus, path, IP_IFACE_DBUS_DEVICE,
                            "InputEvent", "sd", event, value);
    if (rc < 0) return rc;

    return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable dbus_device_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_SIGNAL("InputEvent", "sd", 0),
    SD_BUS_METHOD("EmitInputEvent", "ss", "", method_emit_input_event, 0),
    SD_BUS_VTABLE_END
};

/* ================================================================== */
/*  Composite vtable (assembles all composite properties + methods)    */
/* ================================================================== */

static const sd_bus_vtable composite_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("TargetDevices", "as", composite_property_get, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ProfileName", "s", composite_property_get, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ProfilePath", "s", composite_property_get, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("PersistentId", "s", composite_property_get, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("DbusDevices", "as", composite_property_get, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Name", "s", composite_property_get, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_WRITABLE_PROPERTY("InterceptMode", "u", composite_property_get,
                              composite_property_set, 0, 0),
    SD_BUS_METHOD("LoadProfilePath", "s", "", method_load_profile_path, 0),
    SD_BUS_METHOD("SetInterceptActivation", "ass", "",
                  method_set_intercept_activation, 0),
    SD_BUS_VTABLE_END
};

/* ================================================================== */
/*  Manager vtable                                                      */
/* ================================================================== */

static int
method_create_target(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
    (void)userdata; (void)error;
    const char *kind = NULL;
    int rc = sd_bus_message_read(m, "s", &kind);
    if (rc < 0) return rc;
    if (g_nip_target_count >= NIP_MAX_TARGETS)
        return sd_bus_error_set(error, "org.freedesktop.DBus.Error.LimitsExceeded",
                                "too many targets");
    int idx = g_nip_target_count;
    snprintf(g_nip_target_paths[idx], sizeof(g_nip_target_paths[idx]),
             "/org/shadowblip/InputPlumber/devices/target/%s%d", kind, idx);
    snprintf(g_nip_target_types[idx], sizeof(g_nip_target_types[idx]), "%s", kind);
    g_nip_target_count++;
    return sd_bus_reply_method_return(m, "s", g_nip_target_paths[idx]);
}

static int
method_stop_target(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
    (void)userdata; (void)error;
    const char *path = NULL;
    int rc = sd_bus_message_read(m, "s", &path);
    if (rc < 0) return rc;
    for (int i = 0; i < g_nip_target_count; i++) {
        if (strcmp(g_nip_target_paths[i], path) == 0) {
            for (int j = i; j < g_nip_target_count - 1; j++) {
                memmove(g_nip_target_paths[j], g_nip_target_paths[j + 1],
                        sizeof(g_nip_target_paths[j]));
                memmove(g_nip_target_types[j], g_nip_target_types[j + 1],
                        sizeof(g_nip_target_types[j]));
            }
            g_nip_target_count--;
            break;
        }
    }
    return sd_bus_reply_method_return(m, "");
}

static int
method_attach_target(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
    (void)userdata; (void)error;
    const char *target_path = NULL;
    const char *composite_path = NULL;
    int rc = sd_bus_message_read(m, "ss", &target_path, &composite_path);
    if (rc < 0) return rc;
    bool found = false;
    for (int i = 0; i < g_nip_target_count; i++) {
        if (strcmp(g_nip_target_paths[i], target_path) == 0) {
            found = true;
            break;
        }
    }
    if (!found)
        return sd_bus_error_set(error, "org.freedesktop.DBus.Error.UnknownObject",
                                "target not found");
    int ci = composite_idx_from_path(composite_path);
    if (ci < 0 || ci >= NIP_MAX_COMPOSITES)
        return sd_bus_error_set(error, "org.freedesktop.DBus.Error.UnknownObject",
                                "composite not found");
    if (g_nip_attached_counts[ci] < NIP_MAX_ATTACHED) {
        snprintf(g_nip_attached[ci][g_nip_attached_counts[ci]],
                 sizeof(g_nip_attached[ci][g_nip_attached_counts[ci]]),
                 "%s", target_path);
        g_nip_attached_counts[ci]++;
    }
    return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable manager_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Version", "s", manager_property_get, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("SupportedTargetDeviceIds", "as", manager_property_get, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("InterceptMode", "u", manager_property_get, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Enabled", "b", manager_property_get, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_WRITABLE_PROPERTY("GamepadOrder", "as", manager_property_get,
                              manager_property_set, 0, 0),
    SD_BUS_METHOD("CreateTargetDevice", "s", "s", method_create_target, 0),
    SD_BUS_METHOD("StopTargetDevice", "s", "", method_stop_target, 0),
    SD_BUS_METHOD("AttachTargetDevice", "ss", "", method_attach_target, 0),
    SD_BUS_VTABLE_END
};

/* ================================================================== */
/*  ObjectManager (GetManagedObjects)                                    */
/* ================================================================== */

static int
method_get_managed_objects(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
    (void)userdata; (void)error;
    sd_bus_message *reply = NULL;
    int rc = sd_bus_message_new_method_return(m, &reply);
    if (rc < 0) return rc;

    rc = sd_bus_message_open_container(reply, 'a', "{oa{sa{sv}}}");
    if (rc < 0) goto fail;

    /* Manager object. */
    {
        rc = sd_bus_message_open_container(reply, 'e', "oa{sa{sv}}");
        if (rc < 0) goto fail;
        rc = sd_bus_message_append(reply, "o",
                "/org/shadowblip/InputPlumber/Manager");
        if (rc < 0) goto fail;
        rc = sd_bus_message_open_container(reply, 'a', "{sa{sv}}");
        if (rc < 0) goto fail;
        rc = sd_bus_message_open_container(reply, 'e', "sa{sv}");
        if (rc < 0) goto fail;
        rc = sd_bus_message_append(reply, "s", "org.shadowblip.InputManager");
        if (rc < 0) goto fail;
        rc = sd_bus_message_open_container(reply, 'a', "{sv}");
        if (rc < 0) goto fail;
        rc = sd_bus_message_close_container(reply);
        if (rc < 0) goto fail;
        rc = sd_bus_message_close_container(reply);
        if (rc < 0) goto fail;
        rc = sd_bus_message_close_container(reply);
        if (rc < 0) goto fail;
        rc = sd_bus_message_close_container(reply);
        if (rc < 0) goto fail;
    }

    /* Composite devices (configurable count). */
    for (int c = 0; c < s_num_composites; c++) {
        char comp_path[128];
        snprintf(comp_path, sizeof(comp_path),
                 "/org/shadowblip/InputPlumber/CompositeDevice%d", c);
        rc = sd_bus_message_open_container(reply, 'e', "oa{sa{sv}}");
        if (rc < 0) goto fail;
        rc = sd_bus_message_append(reply, "o", comp_path);
        if (rc < 0) goto fail;
        rc = sd_bus_message_open_container(reply, 'a', "{sa{sv}}");
        if (rc < 0) goto fail;
        rc = sd_bus_message_open_container(reply, 'e', "sa{sv}");
        if (rc < 0) goto fail;
        rc = sd_bus_message_append(reply, "s", "org.shadowblip.Input.CompositeDevice");
        if (rc < 0) goto fail;
        rc = sd_bus_message_open_container(reply, 'a', "{sv}");
        if (rc < 0) goto fail;
        rc = sd_bus_message_close_container(reply);
        if (rc < 0) goto fail;
        rc = sd_bus_message_close_container(reply);
        if (rc < 0) goto fail;
        rc = sd_bus_message_close_container(reply);
        if (rc < 0) goto fail;
        rc = sd_bus_message_close_container(reply);
        if (rc < 0) goto fail;
    }

    /* Target objects. */
    for (int i = 0; i < g_nip_target_count; i++) {
        rc = sd_bus_message_open_container(reply, 'e', "oa{sa{sv}}");
        if (rc < 0) goto fail;
        rc = sd_bus_message_append(reply, "o", g_nip_target_paths[i]);
        if (rc < 0) goto fail;
        rc = sd_bus_message_open_container(reply, 'a', "{sa{sv}}");
        if (rc < 0) goto fail;
        rc = sd_bus_message_open_container(reply, 'e', "sa{sv}");
        if (rc < 0) goto fail;
        rc = sd_bus_message_append(reply, "s", "org.shadowblip.Input.Target");
        if (rc < 0) goto fail;
        rc = sd_bus_message_open_container(reply, 'a', "{sv}");
        if (rc < 0) goto fail;
        rc = sd_bus_message_close_container(reply);
        if (rc < 0) goto fail;
        rc = sd_bus_message_close_container(reply);
        if (rc < 0) goto fail;
        rc = sd_bus_message_close_container(reply);
        if (rc < 0) goto fail;
        rc = sd_bus_message_close_container(reply);
        if (rc < 0) goto fail;
    }

    rc = sd_bus_message_close_container(reply);
    if (rc < 0) goto fail;
    return sd_bus_message_send(reply);

fail:
    sd_bus_message_unref(reply);
    return rc;
}

static int
root_object_handler(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
    const char *iface = sd_bus_message_get_interface(m);
    const char *member = sd_bus_message_get_member(m);
    if (iface && member &&
        strcmp(iface, "org.freedesktop.DBus.ObjectManager") == 0 &&
        strcmp(member, "GetManagedObjects") == 0)
        return method_get_managed_objects(m, userdata, error);
    return 0;
}

/* ================================================================== */
/*  Server run loop                                                     */
/* ================================================================== */

static int
run_server(const char *address)
{
    signal(SIGTERM, stop_service);
    sd_bus *bus = NULL;
    int rc = sd_bus_new(&bus);
    if (rc < 0) return 20;

    if ((rc = sd_bus_set_address(bus, address)) < 0 ||
        (rc = sd_bus_set_bus_client(bus, 1)) < 0 ||
        (rc = sd_bus_start(bus)) < 0 ||
        /* Manager */
        (rc = sd_bus_add_object_vtable(bus, NULL,
              "/org/shadowblip/InputPlumber/Manager",
              "org.shadowblip.InputManager", manager_vtable, NULL)) < 0 ||
        /* ObjectManager root */
        (rc = sd_bus_add_object(bus, NULL,
              "/org/shadowblip/InputPlumber",
              root_object_handler, NULL)) < 0 ||
        /* Target fallback vtable */
        (rc = sd_bus_add_fallback_vtable(bus, NULL,
              "/org/shadowblip/InputPlumber/devices/target",
              "org.shadowblip.Input.Target",
              target_vtable, target_find, NULL)) < 0) {
        sd_bus_unref(bus);
        return 21;
    }

    /* Composite devices + DBusDevice interface per composite. */
    for (int c = 0; c < s_num_composites; c++) {
        char comp_path[128];
        snprintf(comp_path, sizeof(comp_path),
                 "/org/shadowblip/InputPlumber/CompositeDevice%d", c);

        if ((rc = sd_bus_add_object_vtable(bus, NULL,
              comp_path,
              "org.shadowblip.Input.CompositeDevice",
              composite_vtable, NULL)) < 0) {
            sd_bus_unref(bus);
            return 22;
        }
        /* Register DBusDevice (InputEvent signal) on the same path. */
        if ((rc = sd_bus_add_object_vtable(bus, NULL,
              comp_path,
              IP_IFACE_DBUS_DEVICE,
              dbus_device_vtable, NULL)) < 0) {
            sd_bus_unref(bus);
            return 23;
        }
    }

    if ((rc = sd_bus_request_name(bus, IP_DBUS_NAME, 0)) < 0) {
        sd_bus_unref(bus);
        return 24;
    }

    while (s_service_running) {
        while ((rc = sd_bus_process(bus, NULL)) > 0) {}
        if (rc < 0) break;
        sd_bus_wait(bus, 100000);
    }
    sd_bus_flush_close_unref(bus);
    return rc < 0 ? 25 : 0;
}

/* ================================================================== */
/*  Public API                                                          */
/* ================================================================== */

void nip_reset_server_state(int num_composites)
{
    g_nip_target_count = 0;
    memset(g_nip_target_paths, 0, sizeof(g_nip_target_paths));
    memset(g_nip_target_types, 0, sizeof(g_nip_target_types));

    memset(g_nip_attached, 0, sizeof(g_nip_attached));
    memset(g_nip_attached_counts, 0, sizeof(g_nip_attached_counts));

    memset(g_nip_profile_path, 0, sizeof(g_nip_profile_path));
    memset(g_nip_profile_name, 0, sizeof(g_nip_profile_name));

    g_nip_gamepad_order_count = 0;
    memset(g_nip_gamepad_order, 0, sizeof(g_nip_gamepad_order));

    memset(g_nip_intercept_mode, 0, sizeof(g_nip_intercept_mode));
    memset(g_nip_dbus_devices, 0, sizeof(g_nip_dbus_devices));
    memset(g_nip_comp_names, 0, sizeof(g_nip_comp_names));
    memset(g_nip_persistent_ids, 0, sizeof(g_nip_persistent_ids));

    /* Initialize composite names and persistent IDs. */
    int n = num_composites > 0 ? num_composites : 1;
    if (n > NIP_MAX_COMPOSITES) n = NIP_MAX_COMPOSITES;
    for (int i = 0; i < n; i++) {
        g_nip_intercept_mode[i] = 0;  /* NONE */
        snprintf(g_nip_comp_names[i], sizeof(g_nip_comp_names[i]),
                 "Composite Device %d", i);
        snprintf(g_nip_persistent_ids[i], sizeof(g_nip_persistent_ids[i]),
                 "comp-%d", i);
    }
}

int nip_start_private_bus(char *address, size_t address_len, pid_t *bus_pid)
{
    int pipefd[2];
    if (pipe(pipefd) != 0)
        return -errno;
    pid_t pid = fork();
    if (pid < 0)
        return -errno;
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        execlp("dbus-daemon", "dbus-daemon", "--config-file",
               DBUS_SESSION_CONFIG,
               "--nofork", "--print-address=1", (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    FILE *fp = fdopen(pipefd[0], "r");
    if (!fp || !fgets(address, (int)address_len, fp)) {
        if (fp) fclose(fp); else close(pipefd[0]);
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        return -EIO;
    }
    fclose(fp);
    address[strcspn(address, "\r\n")] = '\0';
    *bus_pid = pid;
    return address[0] ? 0 : -EIO;
}

pid_t nip_fork_server(const char *address, const nip_server_config *cfg)
{
    /* Set server configuration (copied so child inherits via fork). */
    s_num_composites = (cfg && cfg->num_composites > 0) ? cfg->num_composites : 1;
    if (cfg && cfg->version)
        snprintf(s_version, sizeof(s_version), "%s", cfg->version);
    else
        snprintf(s_version, sizeof(s_version), "9.8.7");

    s_service_running = 1;

    pid_t server_pid = fork();
    if (server_pid < 0)
        return -errno;
    if (server_pid == 0)
        _exit(run_server(address));
    return server_pid;
}

int nip_start_server(nip_server_handle *h, const nip_server_config *cfg)
{
    memset(h, 0, sizeof(*h));

    /* Start private dbus-daemon. */
    int rc = nip_start_private_bus(h->address, sizeof(h->address),
                                    &h->daemon_pid);
    if (rc < 0)
        return rc;

    setenv("DBUS_SYSTEM_BUS_ADDRESS", h->address, 1);

    /* Fork the InputPlumber-compatible server. */
    pid_t server_pid = nip_fork_server(h->address, cfg);
    if (server_pid < 0) {
        kill(h->daemon_pid, SIGTERM);
        waitpid(h->daemon_pid, NULL, 0);
        h->daemon_pid = 0;
        return (int)server_pid;
    }

    h->server_pid = server_pid;
    return 0;
}

void nip_stop_server(nip_server_handle *h)
{
    if (h->server_pid > 1) {
        kill(h->server_pid, SIGTERM);
        waitpid(h->server_pid, NULL, 0);
        h->server_pid = 0;
    }
    if (h->daemon_pid > 1) {
        kill(h->daemon_pid, SIGTERM);
        waitpid(h->daemon_pid, NULL, 0);
        h->daemon_pid = 0;
    }
    unsetenv("DBUS_SYSTEM_BUS_ADDRESS");
}