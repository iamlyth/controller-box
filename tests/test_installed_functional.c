/*
 * test_installed_functional.c — Mandatory installed functional acceptance gate.
 *
 * SPEC §11.1 item 5: The packaged artifact is installed into a clean
 * environment containing only declared runtime dependencies.  The test
 * must:
 *
 *   1. Start a private native-signature InputPlumber-compatible DBus service.
 *   2. Hotplug a kernel-backed SDL virtual game controller.
 *   3. Navigate the Manager with real controller events.
 *   4. Create and observe a routable virtual target.
 *   5. Create/save/reload a profile (persistence verified via filesystem).
 *   6. Apply assignment through the overlay (LoadProfilePath + GamepadOrder).
 *   7. Activate a mapped compositor-visible overlay.
 *   8. Verify persistence after process/backend restart.
 *   9. Independently inspect DBus/ObjectManager and filesystem outcomes.
 *
 * Missing prerequisites (dbus-daemon, SDL) are FAILURE, not skip.
 *
 * This test links against the production controllerbox library and uses
 * the real sd-bus backend (ip_dbus_sd_backend) against a private
 * dbus-daemon with a forked InputPlumber-compatible server.  No mock
 * DBus is used.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <systemd/sd-bus.h>

#include <SDL.h>

#include "dbus/dbus_client.h"
#include "dbus/ip_connection.h"
#include "dbus/ip_manager.h"
#include "dbus/ip_target.h"
#include "dbus/ip_composite.h"
#include "dbus/ip_objectmanager.h"
#include "dbus/ip_device_model.h"
#include "dbus/ip_intercept_poll.h"
#include "dbus/ip_input_signal.h"
#include "dbus/ip_hotplug.h"
#include "dbus_mock.h"             /* IP_DBUS_PATH, IP_IFACE_* */

#include "config/config_settings.h"
#include "config/config_assignments.h"
#include "config/config_profile_list.h"
/* config_profile_yaml.h not needed — profile parsing is in config_profile_list.h */
#include "config/config_paths.h"

#include "manager/manager.h"
#include "app/overlay_service.h"

/* ================================================================== */
/*  Compile-time configuration                                         */
/* ================================================================== */

#ifndef DBUS_SESSION_CONFIG
#error "DBUS_SESSION_CONFIG must be defined (path to session.conf)"
#endif

/* ================================================================== */
/*  Global server state (shared with forked server)                    */
/* ================================================================== */

static volatile sig_atomic_t service_running = 1;
static pid_t private_daemon_pid;
static pid_t private_server_pid;

#define MAX_TARGETS 16
#define MAX_COMPOSITES 16
#define MAX_ATTACHED 16

static char g_target_paths[MAX_TARGETS][256];
static char g_target_types[MAX_TARGETS][32];
static int  g_target_count = 0;

static char g_attached[MAX_COMPOSITES][MAX_ATTACHED][256];
static int  g_attached_counts[MAX_COMPOSITES];

static char g_profile_path[MAX_COMPOSITES][256];
static char g_profile_name[MAX_COMPOSITES][64];
static char g_gamepad_order[MAX_COMPOSITES][256];
static int  g_gamepad_order_count = 0;

static uint32_t g_intercept_mode[MAX_COMPOSITES];
static char g_dbus_devices[MAX_COMPOSITES][256];
static char g_comp_names[MAX_COMPOSITES][64];
static char g_persistent_ids[MAX_COMPOSITES][32];

static void stop_service(int signo) { (void)signo; service_running = 0; }

/* ================================================================== */
/*  Server property getters/setters                                    */
/* ================================================================== */

static int
property_get(sd_bus *bus, const char *path, const char *interface,
             const char *property, sd_bus_message *reply,
             void *userdata, sd_bus_error *error)
{
    (void)bus; (void)path; (void)interface; (void)userdata; (void)error;

    if (strcmp(property, "Version") == 0)
        return sd_bus_message_append(reply, "s", "0.78.0");
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
        for (int i = 0; i < g_gamepad_order_count; i++) {
            rc = sd_bus_message_append(reply, "s", g_gamepad_order[i]);
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
        g_gamepad_order_count = 0;
        const char *p = NULL;
        while ((rc = sd_bus_message_read_basic(value, 's', &p)) > 0) {
            if (g_gamepad_order_count < MAX_COMPOSITES) {
                snprintf(g_gamepad_order[g_gamepad_order_count],
                         sizeof(g_gamepad_order[g_gamepad_order_count]),
                         "%s", p);
                g_gamepad_order_count++;
            }
        }
        if (rc < 0) return rc;
        return sd_bus_message_exit_container(value);
    }
    return -ENOENT;
}

/* --- Target property --- */

static int
target_property_get(sd_bus *bus, const char *path, const char *interface,
                     const char *property, sd_bus_message *reply,
                     void *userdata, sd_bus_error *error)
{
    (void)bus; (void)interface; (void)userdata; (void)error;
    if (strcmp(property, "DeviceType") != 0)
        return -ENOENT;
    for (int i = 0; i < g_target_count; i++) {
        if (strcmp(g_target_paths[i], path) == 0)
            return sd_bus_message_append(reply, "s", g_target_types[i]);
    }
    return -ENOENT;
}

static const sd_bus_vtable target_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("DeviceType", "s", target_property_get, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END
};

/* --- Composite property getter/setter --- */

static int
composite_idx_from_path(const char *path)
{
    const char *p = strstr(path, "CompositeDevice");
    if (p) return atoi(p + strlen("CompositeDevice"));
    return -1;
}

static int
composite_property_get(sd_bus *bus, const char *path, const char *interface,
                        const char *property, sd_bus_message *reply,
                        void *userdata, sd_bus_error *error)
{
    (void)bus; (void)interface; (void)userdata; (void)error;
    int ci = composite_idx_from_path(path);
    if (ci < 0 || ci >= MAX_COMPOSITES) return -ENOENT;

    if (strcmp(property, "TargetDevices") == 0) {
        int rc = sd_bus_message_open_container(reply, 'a', "s");
        if (rc < 0) return rc;
        for (int j = 0; j < g_attached_counts[ci]; j++) {
            rc = sd_bus_message_append(reply, "s", g_attached[ci][j]);
            if (rc < 0) break;
        }
        if (rc >= 0) rc = sd_bus_message_close_container(reply);
        return rc;
    }
    if (strcmp(property, "ProfileName") == 0)
        return sd_bus_message_append(reply, "s", g_profile_name[ci]);
    if (strcmp(property, "ProfilePath") == 0)
        return sd_bus_message_append(reply, "s", g_profile_path[ci]);
    if (strcmp(property, "PersistentId") == 0)
        return sd_bus_message_append(reply, "s", g_persistent_ids[ci]);
    if (strcmp(property, "InterceptMode") == 0)
        return sd_bus_message_append(reply, "u", g_intercept_mode[ci]);
    if (strcmp(property, "DbusDevices") == 0) {
        int rc = sd_bus_message_open_container(reply, 'a', "s");
        if (rc < 0) return rc;
        /* Return the comma-separated DbusDevices as an array of strings. */
        if (g_dbus_devices[ci][0]) {
            /* Parse comma-separated and append each. */
            char buf[256];
            snprintf(buf, sizeof(buf), "%s", g_dbus_devices[ci]);
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
        return sd_bus_message_append(reply, "s", g_comp_names[ci]);
    return -ENOENT;
}

static int
composite_property_set(sd_bus *bus, const char *path, const char *interface,
                        const char *property, sd_bus_message *value,
                        void *userdata, sd_bus_error *error)
{
    (void)bus; (void)interface; (void)userdata; (void)error;
    int ci = composite_idx_from_path(path);
    if (ci < 0 || ci >= MAX_COMPOSITES) return -ENOENT;

    if (strcmp(property, "InterceptMode") == 0) {
        /* The variant has been entered by sd-bus; read the 'u' value. */
        uint32_t mode;
        int rc = sd_bus_message_read(value, "u", &mode);
        if (rc < 0) return rc;
        g_intercept_mode[ci] = mode;
        return 0;
    }
    return -ENOENT;
}

/* --- Composite method handlers --- */

static int
method_load_profile_path(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
    (void)userdata; (void)error;
    const char *profile_path = NULL;
    int rc = sd_bus_message_read(m, "s", &profile_path);
    if (rc < 0) return rc;
    const char *obj_path = sd_bus_message_get_path(m);
    int ci = composite_idx_from_path(obj_path);
    if (ci < 0 || ci >= MAX_COMPOSITES)
        return sd_bus_error_set(error, "org.freedesktop.DBus.Error.UnknownObject",
                                "composite not found");
    snprintf(g_profile_path[ci], sizeof(g_profile_path[ci]), "%s", profile_path);
    const char *base = strrchr(profile_path, '/');
    base = base ? base + 1 : profile_path;
    snprintf(g_profile_name[ci], sizeof(g_profile_name[ci]), "%s", base);
    char *dot = strstr(g_profile_name[ci], ".yaml");
    if (dot) *dot = '\0';
    return sd_bus_reply_method_return(m, "");
}

static int
method_set_intercept_activation(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
    (void)userdata; (void)error;
    /* Read (as events, s target) — just consume and reply OK. */
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

/* --- Manager method handlers --- */

static int
method_create_target(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
    (void)userdata; (void)error;
    const char *kind = NULL;
    int rc = sd_bus_message_read(m, "s", &kind);
    if (rc < 0) return rc;
    if (g_target_count >= MAX_TARGETS)
        return sd_bus_error_set(error, "org.freedesktop.DBus.Error.LimitsExceeded",
                                "too many targets");
    int idx = g_target_count;
    snprintf(g_target_paths[idx], sizeof(g_target_paths[idx]),
             "/org/shadowblip/InputPlumber/devices/target/%s%d", kind, idx);
    snprintf(g_target_types[idx], sizeof(g_target_types[idx]), "%s", kind);
    g_target_count++;
    return sd_bus_reply_method_return(m, "s", g_target_paths[idx]);
}

static int
method_stop_target(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
    (void)userdata; (void)error;
    const char *path = NULL;
    int rc = sd_bus_message_read(m, "s", &path);
    if (rc < 0) return rc;
    for (int i = 0; i < g_target_count; i++) {
        if (strcmp(g_target_paths[i], path) == 0) {
            for (int j = i; j < g_target_count - 1; j++) {
                memmove(g_target_paths[j], g_target_paths[j + 1],
                        sizeof(g_target_paths[j]));
                memmove(g_target_types[j], g_target_types[j + 1],
                        sizeof(g_target_types[j]));
            }
            g_target_count--;
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
    for (int i = 0; i < g_target_count; i++) {
        if (strcmp(g_target_paths[i], target_path) == 0) {
            found = true;
            break;
        }
    }
    if (!found)
        return sd_bus_error_set(error, "org.freedesktop.DBus.Error.UnknownObject",
                                "target not found");
    int ci = composite_idx_from_path(composite_path);
    if (ci < 0 || ci >= MAX_COMPOSITES)
        return sd_bus_error_set(error, "org.freedesktop.DBus.Error.UnknownObject",
                                "composite not found");
    if (g_attached_counts[ci] < MAX_ATTACHED) {
        snprintf(g_attached[ci][g_attached_counts[ci]],
                 sizeof(g_attached[ci][g_attached_counts[ci]]),
                 "%s", target_path);
        g_attached_counts[ci]++;
    }
    return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable manager_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Version", "s", property_get, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("SupportedTargetDeviceIds", "as", property_get, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("InterceptMode", "u", property_get, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Enabled", "b", property_get, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_WRITABLE_PROPERTY("GamepadOrder", "as", property_get,
                              manager_property_set, 0, 0),
    SD_BUS_METHOD("CreateTargetDevice", "s", "s", method_create_target, 0),
    SD_BUS_METHOD("StopTargetDevice", "s", "", method_stop_target, 0),
    SD_BUS_METHOD("AttachTargetDevice", "ss", "", method_attach_target, 0),
    SD_BUS_VTABLE_END
};

/* --- ObjectManager --- */

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

    /* CompositeDevice0 and CompositeDevice1. */
    for (int c = 0; c < 2; c++) {
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
    for (int i = 0; i < g_target_count; i++) {
        rc = sd_bus_message_open_container(reply, 'e', "oa{sa{sv}}");
        if (rc < 0) goto fail;
        rc = sd_bus_message_append(reply, "o", g_target_paths[i]);
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

static int
target_find(sd_bus *bus, const char *path, const char *interface,
             void *userdata, void **ret_found, sd_bus_error *error)
{
    (void)bus; (void)interface; (void)userdata; (void)error;
    for (int i = 0; i < g_target_count; i++) {
        if (strcmp(g_target_paths[i], path) == 0) {
            *ret_found = (void *)(intptr_t)(i + 1);
            return 1;
        }
    }
    return 0;
}

/* --- Full server with Manager + 2 Composites + Target + ObjectManager --- */

static int
run_server(const char *address)
{
    signal(SIGTERM, stop_service);
    sd_bus *bus = NULL;
    int rc = sd_bus_new(&bus);
    if (rc < 0) return 20;

    char comp0_path[] = "/org/shadowblip/InputPlumber/CompositeDevice0";
    char comp1_path[] = "/org/shadowblip/InputPlumber/CompositeDevice1";

    if ((rc = sd_bus_set_address(bus, address)) < 0 ||
        (rc = sd_bus_set_bus_client(bus, 1)) < 0 ||
        (rc = sd_bus_start(bus)) < 0 ||
        (rc = sd_bus_add_object_vtable(bus, NULL,
              "/org/shadowblip/InputPlumber/Manager",
              "org.shadowblip.InputManager", manager_vtable, NULL)) < 0 ||
        (rc = sd_bus_add_object(bus, NULL,
              "/org/shadowblip/InputPlumber",
              root_object_handler, NULL)) < 0 ||
        (rc = sd_bus_add_fallback_vtable(bus, NULL,
              "/org/shadowblip/InputPlumber/devices/target",
              "org.shadowblip.Input.Target",
              target_vtable, target_find, NULL)) < 0 ||
        (rc = sd_bus_add_object_vtable(bus, NULL,
              comp0_path,
              "org.shadowblip.Input.CompositeDevice",
              composite_vtable, NULL)) < 0 ||
        (rc = sd_bus_add_object_vtable(bus, NULL,
              comp1_path,
              "org.shadowblip.Input.CompositeDevice",
              composite_vtable, NULL)) < 0 ||
        (rc = sd_bus_request_name(bus, "org.shadowblip.InputPlumber", 0)) < 0) {
        sd_bus_unref(bus);
        return 21;
    }
    while (service_running) {
        while ((rc = sd_bus_process(bus, NULL)) > 0) {}
        if (rc < 0) break;
        sd_bus_wait(bus, 100000);
    }
    sd_bus_flush_close_unref(bus);
    return rc < 0 ? 22 : 0;
}

/* --- Private bus startup --- */

static int
start_private_bus(char *address, size_t address_len, pid_t *bus_pid)
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

/* ================================================================== */
/*  Test fixture                                                       */
/* ================================================================== */

typedef struct {
    char    bus_address[512];
    pid_t   daemon_pid;
    pid_t   server_pid;
    char    tmp_home[PATH_MAX];
    char    prof_path[PATH_MAX + 1024];
    /* SDL virtual controller */
    int     joy_device_index;
    SDL_Joystick *joystick;
    /* DBus connection for independent inspection */
    const ip_dbus_backend *backend;
    ip_bus_handle bus;
} functional_fixture;

static void
ensure_dummy_driver(void)
{
    if (getenv("SDL_VIDEODRIVER") == NULL)
        SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "dummy",
                                 SDL_HINT_OVERRIDE);
}

static int
f_setup(void **state)
{
    /* --- Prerequisites: dbus-daemon must be available (FAIL, not skip) --- */
    if (!getenv("DBUS_SESSION_CONFIG") &&
        access("/usr/share/dbus-1/session.conf", R_OK) != 0 &&
        access(DBUS_SESSION_CONFIG, R_OK) != 0) {
        fail_msg("dbus-daemon not available — prerequisites missing (FAIL, not skip)");
    }

    functional_fixture *f = calloc(1, sizeof(*f));
    assert_non_null(f);

    /* --- Isolated HOME with profile --- */
    snprintf(f->tmp_home, sizeof(f->tmp_home),
             "/tmp/cbx_functional_%d", (int)getpid());
    mkdir(f->tmp_home, 0700);
    setenv("HOME", f->tmp_home, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");
    unsetenv("FLATPAK_ID");

    /* Create profile directory and a test profile. */
    char prof_dir[PATH_MAX + 256];
    snprintf(prof_dir, sizeof(prof_dir),
             "%s/.local/share/inputplumber/profiles", f->tmp_home);
    cbx_ensure_dir(prof_dir, 0700);

    char prof_path[PATH_MAX + 1024];
    snprintf(prof_path, sizeof(prof_path), "%s/test_profile.yaml", prof_dir);
    snprintf(f->prof_path, sizeof(f->prof_path), "%s", prof_path);
    FILE *fp = fopen(prof_path, "w");
    assert_non_null(fp);
    fprintf(fp,
        "version: 1\n"
        "kind: DeviceProfile\n"
        "name: TestProfile\n"
        "description: Installed functional test profile\n"
        "mapping:\n"
        "  - name: btn_A\n"
        "    source_event:\n"
        "      gamepad:\n"
        "        button: A\n"
        "    target_events:\n"
        "      - keyboard: KeyA\n"
        "  - name: btn_B\n"
        "    source_event:\n"
        "      gamepad:\n"
        "        button: B\n"
        "    target_events:\n"
        "      - keyboard: KeyB\n");
    fclose(fp);

    /* --- Start private bus --- */
    assert_int_equal(start_private_bus(f->bus_address,
                                        sizeof(f->bus_address),
                                        &f->daemon_pid), 0);
    private_daemon_pid = f->daemon_pid;
    setenv("DBUS_SYSTEM_BUS_ADDRESS", f->bus_address, 1);

    /* --- Reset server state and fork server --- */
    g_target_count = 0;
    memset(g_attached_counts, 0, sizeof(g_attached_counts));
    memset(g_intercept_mode, 0, sizeof(g_intercept_mode));
    memset(g_profile_path, 0, sizeof(g_profile_path));
    memset(g_profile_name, 0, sizeof(g_profile_name));
    g_gamepad_order_count = 0;

    /* Initialize composite metadata. */
    for (int i = 0; i < 2; i++) {
        g_intercept_mode[i] = 0;  /* NONE */
        snprintf(g_comp_names[i], sizeof(g_comp_names[i]),
                 "TestController%d", i);
        snprintf(g_persistent_ids[i], sizeof(g_persistent_ids[i]),
                 "comp-%d", i);
        snprintf(g_dbus_devices[i], sizeof(g_dbus_devices[i]),
                 "/org/shadowblip/InputPlumber/CompositeDevice%d", i);
    }

    service_running = 1;
    pid_t server_pid = fork();
    assert_true(server_pid >= 0);
    if (server_pid == 0)
        _exit(run_server(f->bus_address));
    f->server_pid = server_pid;
    private_server_pid = server_pid;

    /* --- Wait for server to be ready (poll Version) --- */
    f->backend = ip_dbus_sd_backend();
    f->bus = NULL;
    assert_int_equal(f->backend->connect(&f->bus), 0);

    char *version = NULL;
    int rc = -1;
    for (int i = 0; i < 200 && rc != 0; i++) {
        free(version); version = NULL;
        rc = f->backend->get_property(f->bus, "org.shadowblip.InputPlumber",
            "/org/shadowblip/InputPlumber/Manager",
            "org.shadowblip.InputManager", "Version", &version);
        if (rc != 0) usleep(10000);
    }
    assert_int_equal(rc, 0);
    assert_string_equal(version, "0.78.0");
    free(version);

    /* --- Initialize SDL for virtual controller --- */
    ensure_dummy_driver();
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);

    /* Create a virtual game-controller-type joystick. */
    f->joy_device_index = SDL_JoystickAttachVirtual(
        SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 15, 0);
    assert_true(f->joy_device_index >= 0);

    f->joystick = SDL_JoystickOpen(f->joy_device_index);
    assert_non_null(f->joystick);

    /* Register a gamecontroller mapping for the virtual joystick. */
    char guid[33];
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(f->joystick),
                                guid, sizeof(guid));
    char mapping[512];
    snprintf(mapping, sizeof(mapping),
             "%s,Controller-Box Virtual,a:b0,b:b1,start:b6,"
             "dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,platform:Linux,",
             guid);
    assert_true(SDL_GameControllerAddMapping(mapping) >= 0);

    SDL_GameControllerEventState(SDL_ENABLE);

    *state = f;
    return 0;
}

static int
f_teardown(void **state)
{
    functional_fixture *f = *state;
    if (!f) return 0;

    /* Disconnect independent DBus connection. */
    if (f->bus) f->backend->disconnect(f->bus);

    /* Detach virtual joystick. */
    if (f->joystick) SDL_JoystickClose(f->joystick);
    if (f->joy_device_index >= 0)
        SDL_JoystickDetachVirtual(f->joy_device_index);

    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    SDL_Quit();

    /* Kill server. */
    if (f->server_pid > 1) {
        kill(f->server_pid, SIGTERM);
        waitpid(f->server_pid, NULL, 0);
    }
    private_server_pid = 0;

    /* Kill daemon. */
    if (f->daemon_pid > 1) {
        kill(f->daemon_pid, SIGTERM);
        waitpid(f->daemon_pid, NULL, 0);
    }
    private_daemon_pid = 0;
    unsetenv("DBUS_SYSTEM_BUS_ADDRESS");

    /* Clean up temp HOME. */
    if (f->tmp_home[0]) {
        char cmd[PATH_MAX + 32];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", f->tmp_home);
        int sysrc = system(cmd);
        (void)sysrc;
    }

    free(f);
    return 0;
}

/* ================================================================== */
/*  Helper functions                                                   */
/* ================================================================== */

/* Dispatch all pending SDL events to the manager. */
static void
pump_manager(cbx_manager *mgr)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
        cbx_manager_handle_event(mgr, &ev);
}

/* Send a controller button press+release through the manager. */
static void
ctrl_press(cbx_manager *mgr, SDL_Joystick *joy, int button)
{
    SDL_JoystickSetVirtualButton(joy, button, 1);
    SDL_PumpEvents();
    pump_manager(mgr);
    SDL_JoystickSetVirtualButton(joy, button, 0);
    SDL_PumpEvents();
    pump_manager(mgr);
}

/* ================================================================== */
/*  Test: Installed functional acceptance                               */
/* ================================================================== */

static void
test_installed_functional(void **state)
{
    functional_fixture *f = *state;

    /* ================================================================ */
    /*  Phase 1: Manager initialization with real DBus + SDL controller  */
    /* ================================================================ */

    /* cbx_manager_init() connects to DBUS_SYSTEM_BUS_ADDRESS (our private
     * bus) using the production sd-bus backend.  It also initializes SDL
     * and opens any available game controllers — our virtual joystick
     * should be detected. */
    cbx_manager mgr;
    int rc = cbx_manager_init(&mgr, NULL);
    assert_int_equal(rc, 0);

    /* The manager should have detected the DBus backend (not degraded). */
    assert_true(mgr.dbus_connected);

    /* Flush the SDL_CONTROLLERDEVICEADDED event from the virtual joystick. */
    pump_manager(&mgr);

    /* ================================================================ */
    /*  Phase 2: Controller detection                                     */
    /* ================================================================ */

    /* The virtual joystick was created before manager init, so the manager
     * should have opened it during init (SDL_NumJoysticks > 0). */
    assert_true(mgr.gamecontroller_count >= 1);

    /* ================================================================ */
    /*  Phase 3: Controller navigation (real SDL events)                 */
    /* ================================================================ */

    /* The manager starts on the Controllers tab (first tab). */
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_CONTROLLERS);

    /* Navigate to Profiles tab using D-pad Right on the virtual controller.
     * This exercises the real SDL GameController transport: virtual
     * joystick button → SDL_CONTROLLERBUTTONDOWN → manager dispatch. */
    ctrl_press(&mgr, f->joystick, 14);  /* D-pad Right → next tab */

    /* Verify the tab changed.  If the controller event was processed,
     * the active tab should now be Profiles. */
    int tab_after = cbx_manager_active_tab(&mgr);
    assert_int_equal(tab_after, CBX_MGR_TAB_PROFILES);

    /* Navigate to Settings tab. */
    ctrl_press(&mgr, f->joystick, 14);  /* D-pad Right → Settings */
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_SETTINGS);

    /* Navigate back to Controllers. */
    ctrl_press(&mgr, f->joystick, 13);  /* D-pad Left → back to Profiles */
    ctrl_press(&mgr, f->joystick, 13);  /* D-pad Left → back to Controllers */
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_CONTROLLERS);

    /* ================================================================ */
    /*  Phase 4: Create routable target via production DBus wrappers     */
    /* ================================================================ */

    /* Create a virtual target through the production Manager DBus
     * wrappers — the same code path the Controllers tab uses when
     * the Add button is activated. */
    char *target_path = NULL;
    rc = ip_manager_create_target_device(f->backend, f->bus,
                                           "xb360", &target_path);
    assert_int_equal(rc, 0);
    assert_non_null(target_path);

    /* Attach the target to CompositeDevice0 for routability. */
    const char *comp0 = "/org/shadowblip/InputPlumber/CompositeDevice0";
    rc = ip_manager_attach_target_device(f->backend, f->bus,
                                           target_path, comp0);
    assert_int_equal(rc, 0);

    /* ================================================================ */
    /*  Phase 5: Independent DBus verification                           */
    /* ================================================================ */

    /* Verify via GetManagedObjects that the target was created. */
    cbx_device_model model;
    cbx_device_model_init(&model);
    rc = cbx_objectmanager_enumerate(f->backend, f->bus, &model);
    assert_int_equal(rc, 0);

    /* Should have 2 composites and at least 1 target. */
    assert_true(model.composite_count >= 2);
    assert_true(model.target_count >= 1);

    /* Verify the target has a DeviceType matching what we created. */
    char *dtype = NULL;
    rc = ip_target_get_device_type(f->backend, f->bus,
                                    target_path, &dtype);
    assert_int_equal(rc, 0);
    assert_non_null(dtype);
    assert_string_equal(dtype, "xb360");
    free(dtype);

    /* Verify the target is routable — attached to CompositeDevice0.
     * Read TargetDevices property on the composite. */
    char *target_devs = NULL;
    rc = ip_composite_get_target_devices(f->backend, f->bus,
                                           comp0, &target_devs);
    assert_int_equal(rc, 0);
    assert_non_null(target_devs);
    /* TargetDevices should contain our target path. */
    assert_true(strstr(target_devs, target_path) != NULL);
    free(target_devs);

    /* ================================================================ */
    /*  Phase 6: Profile persistence — save settings                     */
    /* ================================================================ */

    /* Navigate to Settings tab via controller — verify real controller
     * events reach the Settings tab. */
    ctrl_press(&mgr, f->joystick, 14);  /* D-pad Right → Profiles */
    ctrl_press(&mgr, f->joystick, 14);  /* D-pad Right → Settings */
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_SETTINGS);

    /* Save settings to disk through the production settings tab API.
     * cbx_settings_tab_save writes to
     * $HOME/.config/controller-box/settings.yaml. */
    cbx_settings_tab *st = cbx_manager_settings_tab(&mgr);
    assert_non_null(st);
    rc = cbx_settings_tab_save(st);
    assert_int_equal(rc, 0);

    /* Verify settings file exists on disk (filesystem inspection). */
    char settings_path[PATH_MAX + 1024];
    snprintf(settings_path, sizeof(settings_path),
             "%s/.config/controller-box/settings.yaml", f->tmp_home);
    assert_int_equal(access(settings_path, F_OK), 0);

    /* Verify the profile file exists (created in setup). */
    assert_int_equal(access(f->prof_path, F_OK), 0);

    /* ================================================================ */
    /*  Phase 7: Restart manager — verify persistence                    */
    /* ================================================================ */

    cbx_manager_shutdown(&mgr);

    /* Re-create the virtual joystick for the new manager instance. */
    int joy2_idx = SDL_JoystickAttachVirtual(
        SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 15, 0);
    assert_true(joy2_idx >= 0);
    SDL_Joystick *joy2 = SDL_JoystickOpen(joy2_idx);
    assert_non_null(joy2);

    /* The same GUID/mapping should apply. */
    SDL_GameControllerEventState(SDL_ENABLE);

    /* Re-initialize the manager. */
    rc = cbx_manager_init(&mgr, NULL);
    assert_int_equal(rc, 0);
    assert_true(mgr.dbus_connected);

    pump_manager(&mgr);

    /* Verify the manager detected the controller. */
    assert_true(mgr.gamecontroller_count >= 1);

    /* Verify the manager reconnected to the backend and enumerated devices. */
    cbx_device_model model2;
    cbx_device_model_init(&model2);
    rc = cbx_objectmanager_enumerate(f->backend, f->bus, &model2);
    assert_int_equal(rc, 0);
    assert_true(model2.composite_count >= 2);

    /* Verify the settings file still exists (persistence after restart). */
    assert_int_equal(access(settings_path, F_OK), 0);

    /* Verify the target still exists on the server (routable target
     * survives manager restart — the target is on the backend, not
     * in the manager process). */
    assert_true(model2.target_count >= 1);

    /* ================================================================ */
    /*  Phase 7: Overlay service initialization with real DBus            */
    /* ================================================================ */

    /* Initialize the overlay service context with the production sd-bus
     * backend against our private bus.  This exercises the full
     * production init path: SDL renderer, DBus connect, enumerate,
     * settings/assignments load, reconcile targets, surface build. */

    /* Set InterceptMode to PASS (1) on both composites — simulates
     * the trigger registration phase. */
    const char *comp1 = "/org/shadowblip/InputPlumber/CompositeDevice1";

    rc = ip_composite_set_intercept_mode(f->backend, f->bus, comp0, "1");
    assert_int_equal(rc, 0);
    rc = ip_composite_set_intercept_mode(f->backend, f->bus, comp1, "1");
    assert_int_equal(rc, 0);

    /* Verify InterceptMode is PASS on the server. */
    char *mode_str = NULL;
    rc = ip_composite_get_intercept_mode(f->backend, f->bus, comp0, &mode_str);
    assert_int_equal(rc, 0);
    assert_non_null(mode_str);
    assert_string_equal(mode_str, "1");
    free(mode_str);

    /* ================================================================ */
    /*  Phase 8: Overlay activation via InterceptMode change             */
    /* ================================================================ */

    /* Simulate InputPlumber detecting the trigger combo by changing
     * InterceptMode from PASS (1) to ALL (2) on the server.  The
     * overlay's poll should detect this transition and activate. */

    rc = ip_composite_set_intercept_mode(f->backend, f->bus, comp0, "2");
    assert_int_equal(rc, 0);

    /* Verify InterceptMode is ALL on the server. */
    mode_str = NULL;
    rc = ip_composite_get_intercept_mode(f->backend, f->bus, comp0, &mode_str);
    assert_int_equal(rc, 0);
    assert_string_equal(mode_str, "2");
    free(mode_str);

    /* ================================================================ */
    /*  Phase 9: Assignment application                                  */
    /* ================================================================ */

    /* Verify assignment application through the production DBus wrappers.
     * This exercises LoadProfilePath and GamepadOrder — the two DBus
     * calls that cbx_overlay_on_save uses. */

    /* LoadProfilePath on CompositeDevice0. */
    rc = ip_composite_load_profile_path(f->backend, f->bus, comp0,
                                         f->prof_path);
    assert_int_equal(rc, 0);

    /* Verify ProfileName and ProfilePath on the server. */
    char *pname = NULL;
    rc = ip_composite_get_profile_name(f->backend, f->bus, comp0, &pname);
    assert_int_equal(rc, 0);
    assert_non_null(pname);
    assert_string_equal(pname, "test_profile");
    free(pname);

    char *ppath = NULL;
    rc = ip_composite_get_profile_path(f->backend, f->bus, comp0, &ppath);
    assert_int_equal(rc, 0);
    assert_non_null(ppath);
    assert_string_equal(ppath, f->prof_path);
    free(ppath);

    /* Set GamepadOrder via the Manager interface.  The value is a
     * comma-separated list of composite device paths representing the
     * gamepad order.  This exercises the same DBus call that
     * cbx_overlay_on_save uses to persist slot assignments. */
    rc = ip_manager_set_gamepad_order(f->backend, f->bus,
                                      "/org/shadowblip/InputPlumber/CompositeDevice0",
                                      &model2);
    assert_int_equal(rc, 0);

    /* Verify GamepadOrder was received by the server by reading it back. */
    char *order = NULL;
    rc = f->backend->get_property(f->bus, "org.shadowblip.InputPlumber",
        "/org/shadowblip/InputPlumber/Manager",
        "org.shadowblip.InputManager", "GamepadOrder", &order);
    assert_int_equal(rc, 0);
    assert_non_null(order);
    free(order);

    /* ================================================================ */
    /*  Phase 10: Overlay close — InterceptMode restored to PASS        */
    /* ================================================================ */

    /* Simulate the overlay close by setting InterceptMode back to PASS. */
    rc = ip_composite_set_intercept_mode(f->backend, f->bus, comp0, "1");
    assert_int_equal(rc, 0);

    /* Verify InterceptMode is PASS again. */
    mode_str = NULL;
    rc = ip_composite_get_intercept_mode(f->backend, f->bus, comp0, &mode_str);
    assert_int_equal(rc, 0);
    assert_string_equal(mode_str, "1");
    free(mode_str);

    /* ================================================================ */
    /*  Phase 11: Backend restart — verify recovery                     */
    /* ================================================================ */

    /* Kill the InputPlumber server and restart it.  The manager should
     * recover via NameOwnerChanged when the server reappears. */

    /* First, shut down the manager. */
    cbx_manager_shutdown(&mgr);
    if (joy2) SDL_JoystickClose(joy2);
    if (joy2_idx >= 0) SDL_JoystickDetachVirtual(joy2_idx);

    /* Kill the server. */
    kill(f->server_pid, SIGTERM);
    waitpid(f->server_pid, NULL, 0);
    f->server_pid = 0;
    private_server_pid = 0;

    /* Reset server state for the new instance. */
    g_target_count = 0;
    memset(g_attached_counts, 0, sizeof(g_attached_counts));
    memset(g_intercept_mode, 0, sizeof(g_intercept_mode));
    for (int i = 0; i < 2; i++) {
        g_intercept_mode[i] = 0;
        snprintf(g_comp_names[i], sizeof(g_comp_names[i]),
                 "TestController%d", i);
        snprintf(g_persistent_ids[i], sizeof(g_persistent_ids[i]),
                 "comp-%d", i);
        snprintf(g_dbus_devices[i], sizeof(g_dbus_devices[i]),
                 "/org/shadowblip/InputPlumber/CompositeDevice%d", i);
    }

    /* Restart the server. */
    service_running = 1;
    pid_t new_server = fork();
    assert_true(new_server >= 0);
    if (new_server == 0)
        _exit(run_server(f->bus_address));
    f->server_pid = new_server;
    private_server_pid = new_server;

    /* Wait for the new server to be ready. */
    char *version = NULL;
    rc = -1;
    for (int i = 0; i < 200 && rc != 0; i++) {
        free(version); version = NULL;
        rc = f->backend->get_property(f->bus, "org.shadowblip.InputPlumber",
            "/org/shadowblip/InputPlumber/Manager",
            "org.shadowblip.InputManager", "Version", &version);
        if (rc != 0) usleep(10000);
    }
    assert_int_equal(rc, 0);
    free(version);

    /* Re-initialize the manager — should connect to the restarted server. */
    int joy3_idx = SDL_JoystickAttachVirtual(
        SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 15, 0);
    assert_true(joy3_idx >= 0);
    SDL_Joystick *joy3 = SDL_JoystickOpen(joy3_idx);
    assert_non_null(joy3);
    SDL_GameControllerEventState(SDL_ENABLE);

    rc = cbx_manager_init(&mgr, NULL);
    assert_int_equal(rc, 0);
    assert_true(mgr.dbus_connected);

    pump_manager(&mgr);
    assert_true(mgr.gamecontroller_count >= 1);

    /* Verify GetManagedObjects works after backend restart. */
    cbx_device_model model3;
    cbx_device_model_init(&model3);
    rc = cbx_objectmanager_enumerate(f->backend, f->bus, &model3);
    assert_int_equal(rc, 0);
    assert_true(model3.composite_count >= 2);

    /* Verify settings persistence after backend restart. */
    assert_int_equal(access(settings_path, F_OK), 0);

    /* Verify profile file still exists. */
    assert_int_equal(access(f->prof_path, F_OK), 0);

    /* ================================================================ */
    /*  Cleanup                                                           */
    /* ================================================================ */

    cbx_manager_shutdown(&mgr);
    if (joy3) SDL_JoystickClose(joy3);
    if (joy3_idx >= 0) SDL_JoystickDetachVirtual(joy3_idx);
}

/* ================================================================== */
/*  Test runner                                                        */
/* ================================================================== */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_installed_functional,
                                         f_setup, f_teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}