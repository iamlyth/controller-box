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
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <systemd/sd-bus.h>

#include "dbus/dbus_client.h"
#include "dbus/ip_connection.h"
#include "dbus/ip_manager.h"
#include "dbus/ip_target.h"
#include "dbus/ip_composite.h"
#include "dbus/ip_objectmanager.h"
#include "dbus/ip_device_model.h"
#include "dbus_mock.h"             /* IP_DBUS_PATH, IP_IFACE_* */

static volatile sig_atomic_t service_running = 1;
static pid_t private_daemon_pid;
static pid_t private_server_pid;
static void stop_service(int signo) { (void)signo; service_running = 0; }

static int cleanup_processes(void **state)
{
    (void)state;
    if (private_server_pid > 1) {
        kill(private_server_pid, SIGTERM);
        waitpid(private_server_pid, NULL, 0);
        private_server_pid = 0;
    }
    if (private_daemon_pid > 1) {
        kill(private_daemon_pid, SIGTERM);
        waitpid(private_daemon_pid, NULL, 0);
        private_daemon_pid = 0;
    }
    unsetenv("DBUS_SYSTEM_BUS_ADDRESS");
    return 0;
}

/* --- Profile / GamepadOrder state for assignment test --- */

#define NATIVE_MAX_COMPOSITES 16

static char g_profile_path[NATIVE_MAX_COMPOSITES][256];
static char g_profile_name[NATIVE_MAX_COMPOSITES][64];
static char g_gamepad_order[NATIVE_MAX_COMPOSITES][256];
static int  g_gamepad_order_count = 0;

static int
property_get(sd_bus *bus, const char *path, const char *interface,
             const char *property, sd_bus_message *reply,
             void *userdata, sd_bus_error *error)
{
    (void)bus; (void)path; (void)interface; (void)userdata; (void)error;
    if (strcmp(property, "Version") == 0)
        return sd_bus_message_append(reply, "s", "9.8.7");
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

/* Property setter for Manager.GamepadOrder (writable `as`). */
static int
manager_property_set(sd_bus *bus, const char *path, const char *interface,
                     const char *property, sd_bus_message *value,
                     void *userdata, sd_bus_error *error)
{
    (void)bus; (void)path; (void)interface; (void)userdata; (void)error;
    if (strcmp(property, "GamepadOrder") == 0) {
        /* The variant has already been entered by sd-bus; we read
         * the `as` array contents directly. */
        int rc = sd_bus_message_enter_container(value, 'a', "s");
        if (rc < 0)
            return rc;
        g_gamepad_order_count = 0;
        const char *p = NULL;
        while ((rc = sd_bus_message_read_basic(value, 's', &p)) > 0) {
            if (g_gamepad_order_count < NATIVE_MAX_COMPOSITES) {
                snprintf(g_gamepad_order[g_gamepad_order_count],
                         sizeof(g_gamepad_order[g_gamepad_order_count]),
                         "%s", p);
                g_gamepad_order_count++;
            }
        }
        if (rc < 0)
            return rc;
        return sd_bus_message_exit_container(value);
    }
    return -ENOENT;
}

/* --- Target state for native fixture test --- */

#define NATIVE_MAX_TARGETS 16
static char g_target_paths[NATIVE_MAX_TARGETS][256];
static char g_target_types[NATIVE_MAX_TARGETS][32];
static int  g_target_count = 0;

/* Attachment tracking: for each composite index, list of attached target paths. */
#define NATIVE_MAX_ATTACHED_PER_COMP 16
static char g_attached_targets[NATIVE_MAX_COMPOSITES][NATIVE_MAX_ATTACHED_PER_COMP][256];
static int  g_attached_counts[NATIVE_MAX_COMPOSITES];
static int  g_attached_count = 0;


/* Property getter for target DeviceType (called from fallback vtable). */
static int
target_property_get(sd_bus *bus, const char *path, const char *interface,
                     const char *property, sd_bus_message *reply,
                     void *userdata, sd_bus_error *error)
{
    (void)bus; (void)interface; (void)userdata; (void)error;
    if (strcmp(property, "DeviceType") != 0)
        return -ENOENT;
    /* Look up the target by path. */
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

/* Composite property getter for TargetDevices, ProfileName, ProfilePath,
 * PersistentId (returns attached target paths or profile state). */
static int
composite_property_get(sd_bus *bus, const char *path, const char *interface,
                        const char *property, sd_bus_message *reply,
                        void *userdata, sd_bus_error *error)
{
    (void)bus; (void)interface; (void)userdata; (void)error;

    /* Extract composite index from path (CompositeDevice0 → 0). */
    int comp_idx = -1;
    const char *p = strstr(path, "CompositeDevice");
    if (p)
        comp_idx = atoi(p + strlen("CompositeDevice"));

    if (strcmp(property, "TargetDevices") == 0) {
        int rc = sd_bus_message_open_container(reply, 'a', "s");
        if (rc < 0) return rc;
        if (comp_idx >= 0 && comp_idx < NATIVE_MAX_COMPOSITES) {
            for (int j = 0; j < g_attached_counts[comp_idx]; j++) {
                rc = sd_bus_message_append(reply, "s", g_attached_targets[comp_idx][j]);
                if (rc < 0) break;
            }
        }
        if (rc >= 0) rc = sd_bus_message_close_container(reply);
        return rc;
    }
    if (strcmp(property, "ProfileName") == 0) {
        if (comp_idx < 0 || comp_idx >= NATIVE_MAX_COMPOSITES)
            return -ENOENT;
        return sd_bus_message_append(reply, "s", g_profile_name[comp_idx]);
    }
    if (strcmp(property, "ProfilePath") == 0) {
        if (comp_idx < 0 || comp_idx >= NATIVE_MAX_COMPOSITES)
            return -ENOENT;
        return sd_bus_message_append(reply, "s", g_profile_path[comp_idx]);
    }
    if (strcmp(property, "PersistentId") == 0) {
        if (comp_idx < 0 || comp_idx >= NATIVE_MAX_COMPOSITES)
            return -ENOENT;
        char id[32];
        snprintf(id, sizeof(id), "comp-%d", comp_idx);
        return sd_bus_message_append(reply, "s", id);
    }
    return -ENOENT;
}

/* Method handler: CompositeDevice.LoadProfilePath(path: s) */
static int
method_load_profile_path(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
    (void)userdata; (void)error;
    const char *profile_path = NULL;
    int rc = sd_bus_message_read(m, "s", &profile_path);
    if (rc < 0)
        return rc;
    /* Extract composite index from the object path. */
    const char *obj_path = sd_bus_message_get_path(m);
    int comp_idx = -1;
    if (obj_path) {
        const char *p = strstr(obj_path, "CompositeDevice");
        if (p)
            comp_idx = atoi(p + strlen("CompositeDevice"));
    }
    if (comp_idx < 0 || comp_idx >= NATIVE_MAX_COMPOSITES)
        return sd_bus_error_set(error, "org.freedesktop.DBus.Error.UnknownObject",
                                "composite not found");
    /* Store the profile path and derive a name from the filename. */
    snprintf(g_profile_path[comp_idx], sizeof(g_profile_path[comp_idx]),
             "%s", profile_path);
    /* Derive a simple profile name from the path (strip directory + .yaml). */
    const char *base = strrchr(profile_path, '/');
    base = base ? base + 1 : profile_path;
    snprintf(g_profile_name[comp_idx], sizeof(g_profile_name[comp_idx]),
             "%s", base);
    char *dot = strstr(g_profile_name[comp_idx], ".yaml");
    if (dot)
        *dot = '\0';
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
    SD_BUS_METHOD("LoadProfilePath", "s", "", method_load_profile_path, 0),
    SD_BUS_VTABLE_END
};

/* Target DeviceType property is served by a fallback vtable registered
 * under the /devices/target/ prefix.  The property_get handler receives
 * the object path and looks up the type in g_target_paths/g_target_types. */

static int
target_find(sd_bus *bus, const char *path, const char *interface,
             void *userdata, void **ret_found, sd_bus_error *error)
{
    (void)bus; (void)interface; (void)userdata; (void)error;
    for (int i = 0; i < g_target_count; i++) {
        if (strcmp(g_target_paths[i], path) == 0) {
            *ret_found = (void *)(intptr_t)(i + 1);
            return 1; /* found */
        }
    }
    return 0; /* not found */
}

/* Method handler: Manager.CreateTargetDevice(s) → s */
static int
method_create_target(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
    (void)userdata; (void)error;
    const char *kind = NULL;
    int rc = sd_bus_message_read(m, "s", &kind);
    if (rc < 0)
        return rc;
    if (g_target_count >= NATIVE_MAX_TARGETS)
        return sd_bus_error_set(error, "org.freedesktop.DBus.Error.LimitsExceeded",
                                "too many targets");
    /* Create a unique target name based on the kind and count. */
    int idx = g_target_count;
    snprintf(g_target_paths[idx], sizeof(g_target_paths[idx]),
             "/org/shadowblip/InputPlumber/devices/target/%s%d", kind, idx);
    snprintf(g_target_types[idx], sizeof(g_target_types[idx]), "%s", kind);
    g_target_count++;
    return sd_bus_reply_method_return(m, "s", g_target_paths[idx]);
}

/* Method handler: Manager.StopTargetDevice(s) */
static int
method_stop_target(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
    (void)userdata; (void)error;
    const char *path = NULL;
    int rc = sd_bus_message_read(m, "s", &path);
    if (rc < 0)
        return rc;
    /* Find and remove the target. */
    for (int i = 0; i < g_target_count; i++) {
        if (strcmp(g_target_paths[i], path) == 0) {
            /* Shift remaining targets down. */
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

/* Method handler: Manager.AttachTargetDevice(target:s, composite:s) */
static int
method_attach_target(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
    (void)userdata; (void)error;
    const char *target_path = NULL;
    const char *composite_path = NULL;
    int rc = sd_bus_message_read(m, "ss", &target_path, &composite_path);
    if (rc < 0)
        return rc;
    /* Validate target exists. */
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
    /* Extract composite index from path (CompositeDevice0 → 0). */
    int comp_idx = -1;
    const char *p = strstr(composite_path, "CompositeDevice");
    if (p)
        comp_idx = atoi(p + strlen("CompositeDevice"));
    if (comp_idx < 0 || comp_idx >= NATIVE_MAX_COMPOSITES)
        return sd_bus_error_set(error, "org.freedesktop.DBus.Error.UnknownObject",
                                "composite not found");
    /* Record attachment (append to list for this composite). */
    if (g_attached_counts[comp_idx] < NATIVE_MAX_ATTACHED_PER_COMP) {
        snprintf(g_attached_targets[comp_idx][g_attached_counts[comp_idx]],
                 sizeof(g_attached_targets[comp_idx][g_attached_counts[comp_idx]]),
                 "%s", target_path);
        g_attached_counts[comp_idx]++;
    }
    if (comp_idx + 1 > g_attached_count)
        g_attached_count = comp_idx + 1;
    return sd_bus_reply_method_return(m, "");
}

/* Method handler: ObjectManager.GetManagedObjects() → a{oa{sa{sv}}} */
static int
method_get_managed_objects(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
    (void)userdata; (void)error;
    sd_bus_message *reply = NULL;
    int rc = sd_bus_message_new_method_return(m, &reply);
    if (rc < 0)
        return rc;

    rc = sd_bus_message_open_container(reply, 'a', "{oa{sa{sv}}}");
    if (rc < 0) goto fail;

    /* Manager object. */
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
    rc = sd_bus_message_close_container(reply); /* a{sv} */
    if (rc < 0) goto fail;
    rc = sd_bus_message_close_container(reply); /* {sa{sv}} */
    if (rc < 0) goto fail;
    rc = sd_bus_message_close_container(reply); /* a{sa{sv}} */
    if (rc < 0) goto fail;
    rc = sd_bus_message_close_container(reply); /* {oa{sa{sv}} */
    if (rc < 0) goto fail;

    /* CompositeDevice0 (always present, for change-type tests). */
    rc = sd_bus_message_open_container(reply, 'e', "oa{sa{sv}}");
    if (rc < 0) goto fail;
    rc = sd_bus_message_append(reply, "o",
            "/org/shadowblip/InputPlumber/CompositeDevice0");
    if (rc < 0) goto fail;
    rc = sd_bus_message_open_container(reply, 'a', "{sa{sv}}");
    if (rc < 0) goto fail;
    rc = sd_bus_message_open_container(reply, 'e', "sa{sv}");
    if (rc < 0) goto fail;
    rc = sd_bus_message_append(reply, "s", "org.shadowblip.Input.CompositeDevice");
    if (rc < 0) goto fail;
    rc = sd_bus_message_open_container(reply, 'a', "{sv}");
    if (rc < 0) goto fail;
    rc = sd_bus_message_close_container(reply); /* a{sv} */
    if (rc < 0) goto fail;
    rc = sd_bus_message_close_container(reply); /* {sa{sv}} */
    if (rc < 0) goto fail;
    rc = sd_bus_message_close_container(reply); /* a{sa{sv}} */
    if (rc < 0) goto fail;
    rc = sd_bus_message_close_container(reply); /* {oa{sa{sv}} */
    if (rc < 0) goto fail;

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
        rc = sd_bus_message_close_container(reply); /* a{sv} */
        if (rc < 0) goto fail;
        rc = sd_bus_message_close_container(reply); /* {sa{sv}} */
        if (rc < 0) goto fail;
        rc = sd_bus_message_close_container(reply); /* a{sa{sv}} */
        if (rc < 0) goto fail;
        rc = sd_bus_message_close_container(reply); /* {oa{sa{sv}} */
        if (rc < 0) goto fail;
    }

    rc = sd_bus_message_close_container(reply); /* a{oa{sa{sv}}} */
    if (rc < 0) goto fail;

    return sd_bus_message_send(reply);

fail:
    sd_bus_message_unref(reply);
    return rc;
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

static int run_service(const char *address)
{
    signal(SIGTERM, stop_service);
    sd_bus *bus = NULL;
    int rc = sd_bus_new(&bus);
    if (rc < 0) return 20;
    if ((rc = sd_bus_set_address(bus, address)) < 0 ||
        (rc = sd_bus_set_bus_client(bus, 1)) < 0 ||
        (rc = sd_bus_start(bus)) < 0 ||
        (rc = sd_bus_add_object_vtable(bus, NULL,
              "/org/shadowblip/InputPlumber/Manager",
              "org.shadowblip.InputManager", manager_vtable, NULL)) < 0 ||
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

/* Extended server with ObjectManager + target vtables for target operations test. */

/* Handle all messages to the root path: filter for GetManagedObjects. */
static int
root_object_handler(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
    const char *iface = sd_bus_message_get_interface(m);
    const char *member = sd_bus_message_get_member(m);
    if (iface && member &&
        strcmp(iface, "org.freedesktop.DBus.ObjectManager") == 0 &&
        strcmp(member, "GetManagedObjects") == 0)
        return method_get_managed_objects(m, userdata, error);
    return 0; /* not handled */
}

static int run_service_full(const char *address)
{
    signal(SIGTERM, stop_service);
    sd_bus *bus = NULL;
    int rc = sd_bus_new(&bus);
    if (rc < 0) return 20;
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
              "/org/shadowblip/InputPlumber/CompositeDevice0",
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

static int start_private_bus(char *address, size_t address_len, pid_t *bus_pid)
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

static void test_production_backend_native_roundtrip(void **state)
{
    (void)state;
    char address[512];
    pid_t daemon_pid = 0;
    assert_int_equal(start_private_bus(address, sizeof(address), &daemon_pid), 0);
    private_daemon_pid = daemon_pid;
    setenv("DBUS_SYSTEM_BUS_ADDRESS", address, 1);

    pid_t server_pid = fork();
    assert_true(server_pid >= 0);
    if (server_pid == 0)
        _exit(run_service(address));
    private_server_pid = server_pid;

    const ip_dbus_backend *backend = ip_dbus_sd_backend();
    ip_bus_handle bus = NULL;
    assert_int_equal(backend->connect(&bus), 0);

    char *value = NULL;
    int rc = -1;
    for (int i = 0; i < 100 && rc != 0; i++) {
        free(value); value = NULL;
        rc = backend->get_property(bus, "org.shadowblip.InputPlumber",
            "/org/shadowblip/InputPlumber/Manager",
            "org.shadowblip.InputManager", "Version", &value);
        if (rc != 0) usleep(10000);
    }
    assert_int_equal(rc, 0);
    assert_string_equal(value, "9.8.7"); free(value);

    assert_int_equal(backend->get_property(bus, "org.shadowblip.InputPlumber",
        "/org/shadowblip/InputPlumber/Manager", "org.shadowblip.InputManager",
        "SupportedTargetDeviceIds", &value), 0);
    assert_string_equal(value, "xb360,ds5,gamepad"); free(value);

    assert_int_equal(backend->get_property(bus, "org.shadowblip.InputPlumber",
        "/org/shadowblip/InputPlumber/Manager", "org.shadowblip.InputManager",
        "InterceptMode", &value), 0);
    assert_string_equal(value, "2"); free(value);

    assert_int_equal(backend->get_property(bus, "org.shadowblip.InputPlumber",
        "/org/shadowblip/InputPlumber/Manager", "org.shadowblip.InputManager",
        "Enabled", &value), 0);
    assert_string_equal(value, "1"); free(value);

    backend->disconnect(bus);
    kill(server_pid, SIGTERM);
    int status = 0;
    waitpid(server_pid, &status, 0);
    assert_true(WIFEXITED(status));
    assert_int_equal(WEXITSTATUS(status), 0);
    private_server_pid = 0;
    kill(daemon_pid, SIGTERM);
    waitpid(daemon_pid, NULL, 0);
    private_daemon_pid = 0;
    unsetenv("DBUS_SYSTEM_BUS_ADDRESS");
}

/* --- Native fixture: owner loss / reacquisition via real sd-bus ----- */

static int s_native_reenum_called = 0;
static int s_native_degraded_called = 0;
static char s_native_degraded_reason[256] = {0};

static void
native_reenumerate_cb(void *ud)
{
    (void)ud;
    s_native_reenum_called++;
}

static void
native_degraded_cb(const char *reason, void *ud)
{
    (void)ud;
    s_native_degraded_called++;
    if (reason)
        snprintf(s_native_degraded_reason,
                 sizeof(s_native_degraded_reason), "%s", reason);
}

/* Helper: drain pending sd-bus messages for up to ~2 seconds. */
static void
drain_bus(const ip_dbus_backend *backend, ip_bus_handle bus, int ms)
{
    for (int i = 0; i < ms / 10; i++) {
        int processed = backend->process(bus);
        if (processed <= 0)
            usleep(10000);
    }
}

static void
test_native_owner_loss_and_reacquisition(void **state)
{
    (void)state;
    char address[512];
    pid_t daemon_pid = 0;
    assert_int_equal(start_private_bus(address, sizeof(address),
                                        &daemon_pid), 0);
    private_daemon_pid = daemon_pid;
    setenv("DBUS_SYSTEM_BUS_ADDRESS", address, 1);

    /* Start the InputPlumber server. */
    service_running = 1;
    pid_t server_pid = fork();
    assert_true(server_pid >= 0);
    if (server_pid == 0)
        _exit(run_service(address));
    private_server_pid = server_pid;

    /* Wait for server to come up by polling for the name. */
    usleep(100000);

    const ip_dbus_backend *backend = ip_dbus_sd_backend();

    /* Connect via ip_connection — should succeed (CONNECTED). */
    ip_connection conn;
    ip_connection_init(&conn, backend);
    s_native_reenum_called = 0;
    s_native_degraded_called = 0;

    int rc = ip_connection_connect(&conn);
    /* If connect failed because the server wasn't up yet, retry. */
    for (int i = 0; i < 50 && rc < 0 && rc != IP_ERR_ACCESS_DENIED; i++) {
        if (conn.bus) {
            ip_connection_disconnect(&conn);
            ip_connection_init(&conn, backend);
        }
        usleep(20000);
        rc = ip_connection_connect(&conn);
    }
    assert_int_equal(rc, 0);
    assert_true(ip_connection_is_connected(&conn));
    assert_non_null(conn.bus);

    ip_connection_set_reenumerate_cb(&conn, native_reenumerate_cb, NULL);
    ip_connection_set_degraded_cb(&conn, native_degraded_cb, NULL);

    /* --- Phase 1: Stop the server (name loss) --- */
    kill(server_pid, SIGTERM);
    int wstatus = 0;
    waitpid(server_pid, &wstatus, 0);
    assert_true(WIFEXITED(wstatus));
    private_server_pid = 0;

    /* Drain the bus — NameOwnerChanged should fire degraded callback. */
    drain_bus(backend, conn.bus, 2000);

    assert_true(ip_connection_is_degraded(&conn));
    assert_int_equal(s_native_degraded_called, 1);
    assert_string_equal(s_native_degraded_reason, "InputPlumber stopped");

    /* --- Phase 2: Restart the server (name acquisition) --- */
    service_running = 1;
    server_pid = fork();
    assert_true(server_pid >= 0);
    if (server_pid == 0)
        _exit(run_service(address));
    private_server_pid = server_pid;

    /* Drain the bus — NameOwnerChanged should fire reenumerate callback. */
    drain_bus(backend, conn.bus, 2000);

    assert_true(ip_connection_is_connected(&conn));
    assert_int_equal(s_native_reenum_called, 1);

    /* Clean up. */
    ip_connection_disconnect(&conn);

    kill(server_pid, SIGTERM);
    waitpid(server_pid, NULL, 0);
    private_server_pid = 0;

    kill(daemon_pid, SIGTERM);
    waitpid(daemon_pid, NULL, 0);
    private_daemon_pid = 0;
    unsetenv("DBUS_SYSTEM_BUS_ADDRESS");
}

/* --- Native fixture: target operations via real sd-bus --------------- */

static void
test_native_target_operations(void **state)
{
    (void)state;
    char address[512];
    pid_t daemon_pid = 0;
    assert_int_equal(start_private_bus(address, sizeof(address),
                                        &daemon_pid), 0);
    private_daemon_pid = daemon_pid;
    setenv("DBUS_SYSTEM_BUS_ADDRESS", address, 1);

    /* Reset server state and start it. */
    g_target_count = 0;
    service_running = 1;
    pid_t server_pid = fork();
    assert_true(server_pid >= 0);
    if (server_pid == 0)
        _exit(run_service_full(address));
    private_server_pid = server_pid;

    /* Wait for the server to come up by polling for Version. */
    const ip_dbus_backend *backend = ip_dbus_sd_backend();
    ip_bus_handle bus = NULL;
    assert_int_equal(backend->connect(&bus), 0);

    char *version = NULL;
    int rc = -1;
    for (int i = 0; i < 100 && rc != 0; i++) {
        free(version); version = NULL;
        rc = backend->get_property(bus, IP_DBUS_NAME,
            IP_DBUS_MANAGER_PATH, IP_IFACE_MANAGER, "Version", &version);
        if (rc != 0) usleep(10000);
    }
    assert_int_equal(rc, 0);
    assert_string_equal(version, "9.8.7");
    free(version);

    /* --- Step 1: Read SupportedTargetDeviceIds (native `as`) --- */
    char *types_csv = NULL;
    assert_int_equal(ip_manager_get_supported_target_device_ids(
        backend, bus, &types_csv), 0);
    assert_non_null(types_csv);
    assert_string_equal(types_csv, "xb360,ds5,gamepad");
    free(types_csv);

    /* --- Step 2: CreateTargetDevice --- */
    char *path0 = NULL;
    assert_int_equal(ip_manager_create_target_device(
        backend, bus, "gamepad", &path0), 0);
    assert_non_null(path0);
    assert_true(strstr(path0, "/devices/target/gamepad0") != NULL);

    char *path1 = NULL;
    assert_int_equal(ip_manager_create_target_device(
        backend, bus, "xb360", &path1), 0);
    assert_non_null(path1);
    assert_true(strstr(path1, "/devices/target/xb3601") != NULL);

    /* --- Step 3: GetManagedObjects should list both targets --- */
    cbx_device_model model;
    cbx_device_model_init(&model);
    assert_int_equal(cbx_objectmanager_enumerate(backend, bus, &model), 0);
    assert_int_equal(model.target_count, 2);
    assert_true(model.has_manager);
    assert_int_equal(model.composite_count, 1);
    /* Verify target paths appear in the model. */
    bool found0 = false, found1 = false;
    for (int i = 0; i < model.target_count; i++) {
        if (strcmp(model.targets[i].path, path0) == 0) found0 = true;
        if (strcmp(model.targets[i].path, path1) == 0) found1 = true;
    }
    assert_true(found0);
    assert_true(found1);

    /* --- Step 4: DeviceType property on each target --- */
    char *dtype0 = NULL;
    assert_int_equal(ip_target_get_device_type(backend, bus, path0, &dtype0), 0);
    assert_non_null(dtype0);
    assert_string_equal(dtype0, "gamepad");
    free(dtype0);

    char *dtype1 = NULL;
    assert_int_equal(ip_target_get_device_type(backend, bus, path1, &dtype1), 0);
    assert_non_null(dtype1);
    assert_string_equal(dtype1, "xb360");
    free(dtype1);

    /* --- Step 5: StopTargetDevice removes from ObjectManager --- */
    assert_int_equal(ip_manager_stop_target_device(backend, bus, path0), 0);
    free(path0);

    cbx_device_model model2;
    cbx_device_model_init(&model2);
    assert_int_equal(cbx_objectmanager_enumerate(backend, bus, &model2), 0);
    assert_int_equal(model2.target_count, 1);
    /* Remaining target should be path1 (xb360). */
    assert_string_equal(model2.targets[0].path, path1);

    /* --- Step 6: DeviceType on removed target should fail --- */
    char *dtype_removed = (char *)0xdeadbeef;
    int rc_removed = ip_target_get_device_type(backend, bus,
        "/org/shadowblip/InputPlumber/devices/target/gamepad0",
        &dtype_removed);
    assert_true(rc_removed < 0);
    assert_null(dtype_removed);

    /* Clean up. */
    free(path1);
    backend->disconnect(bus);
    kill(server_pid, SIGTERM);
    waitpid(server_pid, NULL, 0);
    private_server_pid = 0;
    kill(daemon_pid, SIGTERM);
    waitpid(daemon_pid, NULL, 0);
    private_daemon_pid = 0;
    unsetenv("DBUS_SYSTEM_BUS_ADDRESS");
}


/* --- Native fixture: topology reconciliation via real sd-bus ------- */

static void
test_native_topology_reconciliation(void **state)
{
    (void)state;
    char address[512];
    pid_t daemon_pid = 0;
    assert_int_equal(start_private_bus(address, sizeof(address),
                                        &daemon_pid), 0);
    private_daemon_pid = daemon_pid;
    setenv("DBUS_SYSTEM_BUS_ADDRESS", address, 1);

    /* Reset server state and start it. */
    g_target_count = 0;
    g_attached_count = 0;
    memset(g_attached_targets, 0, sizeof(g_attached_targets));
    memset(g_attached_counts, 0, sizeof(g_attached_counts));
    service_running = 1;
    pid_t server_pid = fork();
    assert_true(server_pid >= 0);
    if (server_pid == 0)
        _exit(run_service_full(address));
    private_server_pid = server_pid;

    /* Wait for the server to come up. */
    const ip_dbus_backend *backend = ip_dbus_sd_backend();
    ip_bus_handle bus = NULL;
    assert_int_equal(backend->connect(&bus), 0);

    /* Poll for Version. */
    char *version = NULL;
    int rc = -1;
    for (int i = 0; i < 100 && rc != 0; i++) {
        free(version); version = NULL;
        rc = backend->get_property(bus, IP_DBUS_NAME,
            IP_DBUS_MANAGER_PATH, IP_IFACE_MANAGER, "Version", &version);
        if (rc != 0) usleep(10000);
    }
    assert_int_equal(rc, 0);
    free(version);

    /* --- Scenario 1: Clean startup creates ordered topology ---
     * Desired: 3 targets -- xb360, ds5, gamepad.
     * Start from empty, create them in order, verify count/types/attach. */
    cbx_device_model model;
    cbx_device_model_init(&model);
    assert_int_equal(cbx_objectmanager_enumerate(backend, bus, &model), 0);
    assert_int_equal(model.target_count, 0);
    assert_int_equal(model.composite_count, 1);

    /* Create 3 targets in order. */
    char *path0 = NULL, *path1 = NULL, *path2 = NULL;
    assert_int_equal(ip_manager_create_target_device(
        backend, bus, "xb360", &path0), 0);
    assert_non_null(path0);
    assert_int_equal(ip_manager_create_target_device(
        backend, bus, "ds5", &path1), 0);
    assert_non_null(path1);
    assert_int_equal(ip_manager_create_target_device(
        backend, bus, "gamepad", &path2), 0);
    assert_non_null(path2);

    /* Verify via ObjectManager. */
    cbx_device_model_init(&model);
    assert_int_equal(cbx_objectmanager_enumerate(backend, bus, &model), 0);
    assert_int_equal(model.target_count, 3);

    /* Verify DeviceType per slot. */
    char *dtype0 = NULL, *dtype1 = NULL, *dtype2 = NULL;
    assert_int_equal(ip_target_get_device_type(backend, bus,
        model.targets[0].path, &dtype0), 0);
    assert_string_equal(dtype0, "xb360"); free(dtype0);
    assert_int_equal(ip_target_get_device_type(backend, bus,
        model.targets[1].path, &dtype1), 0);
    assert_string_equal(dtype1, "ds5"); free(dtype1);
    assert_int_equal(ip_target_get_device_type(backend, bus,
        model.targets[2].path, &dtype2), 0);
    assert_string_equal(dtype2, "gamepad"); free(dtype2);

    /* Attach each target to its composite and verify routability
     * via the CompositeDevice TargetDevices property. */
    const char *comp0 = "/org/shadowblip/InputPlumber/CompositeDevice0";
    assert_int_equal(ip_manager_attach_target_device(
        backend, bus, model.targets[0].path, comp0), 0);
    assert_int_equal(ip_manager_attach_target_device(
        backend, bus, model.targets[1].path, comp0), 0);
    assert_int_equal(ip_manager_attach_target_device(
        backend, bus, model.targets[2].path, comp0), 0);

    /* Verify routability: TargetDevices on CompositeDevice0 lists all 3. */
    char *td = NULL;
    assert_int_equal(ip_composite_get_target_devices(
        backend, bus, comp0, &td), 0);
    assert_non_null(td);
    assert_true(strstr(td, model.targets[0].path) != NULL);
    assert_true(strstr(td, model.targets[1].path) != NULL);
    assert_true(strstr(td, model.targets[2].path) != NULL);
    free(td);

    /* --- Scenario 2: Remove one slot, verify others preserved ---
     * Stop target at slot 1 (ds5), verify count=2 and remaining are
     * slot 0 (xb360) and slot 1 (gamepad). */
    char slot1_path[256];
    snprintf(slot1_path, sizeof(slot1_path), "%s", model.targets[1].path);
    assert_int_equal(ip_manager_stop_target_device(backend, bus, slot1_path), 0);

    cbx_device_model model2;
    cbx_device_model_init(&model2);
    assert_int_equal(cbx_objectmanager_enumerate(backend, bus, &model2), 0);
    assert_int_equal(model2.target_count, 2);
    /* Slot 0 should still be xb360. */
    char *dt0 = NULL;
    assert_int_equal(ip_target_get_device_type(backend, bus,
        model2.targets[0].path, &dt0), 0);
    assert_string_equal(dt0, "xb360"); free(dt0);
    /* Slot 1 should now be gamepad (was slot 2 before remove). */
    char *dt1 = NULL;
    assert_int_equal(ip_target_get_device_type(backend, bus,
        model2.targets[1].path, &dt1), 0);
    assert_string_equal(dt1, "gamepad"); free(dt1);

    /* --- Scenario 3: Type correction (stop+create in reverse) ---
     * Current: [xb360, gamepad]. Change slot 1 to ds5.
     * Stop slot 1 (gamepad, last in array), create new ds5. */
    char old_path[256];
    snprintf(old_path, sizeof(old_path), "%s", model2.targets[1].path);
    assert_int_equal(ip_manager_stop_target_device(backend, bus, old_path), 0);
    char *new_path = NULL;
    assert_int_equal(ip_manager_create_target_device(
        backend, bus, "ds5", &new_path), 0);
    assert_non_null(new_path);
    free(new_path);

    cbx_device_model model3;
    cbx_device_model_init(&model3);
    assert_int_equal(cbx_objectmanager_enumerate(backend, bus, &model3), 0);
    assert_int_equal(model3.target_count, 2);
    /* Slot 0 should still be xb360 (preserved). */
    char *s3_dt0 = NULL;
    assert_int_equal(ip_target_get_device_type(backend, bus,
        model3.targets[0].path, &s3_dt0), 0);
    assert_string_equal(s3_dt0, "xb360"); free(s3_dt0);
    /* Slot 1 should now be ds5 (corrected). */
    char *s3_dt1 = NULL;
    assert_int_equal(ip_target_get_device_type(backend, bus,
        model3.targets[1].path, &s3_dt1), 0);
    assert_string_equal(s3_dt1, "ds5"); free(s3_dt1);

    /* --- Scenario 4: Attach after type correction verifies routability --- */
    assert_int_equal(ip_manager_attach_target_device(
        backend, bus, model3.targets[0].path, comp0), 0);
    assert_int_equal(ip_manager_attach_target_device(
        backend, bus, model3.targets[1].path, comp0), 0);
    char *td2 = NULL;
    assert_int_equal(ip_composite_get_target_devices(
        backend, bus, comp0, &td2), 0);
    assert_non_null(td2);
    assert_true(strstr(td2, model3.targets[0].path) != NULL);
    assert_true(strstr(td2, model3.targets[1].path) != NULL);
    free(td2);

    /* Clean up all targets. */
    for (int i = model3.target_count - 1; i >= 0; i--) {
        ip_manager_stop_target_device(backend, bus, model3.targets[i].path);
    }

    /* Clean up. */
    free(path0); free(path1); free(path2);
    backend->disconnect(bus);
    kill(server_pid, SIGTERM);
    waitpid(server_pid, NULL, 0);
    private_server_pid = 0;
    kill(daemon_pid, SIGTERM);
    waitpid(daemon_pid, NULL, 0);
    private_daemon_pid = 0;
    unsetenv("DBUS_SYSTEM_BUS_ADDRESS");
}

/* --- Native fixture: assignment/profile application via real sd-bus -- */

static void
test_native_assignment_application(void **state)
{
    (void)state;
    char address[512];
    pid_t daemon_pid = 0;
    assert_int_equal(start_private_bus(address, sizeof(address),
                                        &daemon_pid), 0);
    private_daemon_pid = daemon_pid;
    setenv("DBUS_SYSTEM_BUS_ADDRESS", address, 1);

    /* Reset server state. */
    g_target_count = 0;
    g_attached_count = 0;
    memset(g_attached_targets, 0, sizeof(g_attached_targets));
    memset(g_attached_counts, 0, sizeof(g_attached_counts));
    memset(g_profile_path, 0, sizeof(g_profile_path));
    memset(g_profile_name, 0, sizeof(g_profile_name));
    g_gamepad_order_count = 0;
    memset(g_gamepad_order, 0, sizeof(g_gamepad_order));
    service_running = 1;
    pid_t server_pid = fork();
    assert_true(server_pid >= 0);
    if (server_pid == 0)
        _exit(run_service_full(address));
    private_server_pid = server_pid;

    /* Wait for the server to come up. */
    const ip_dbus_backend *backend = ip_dbus_sd_backend();
    ip_bus_handle bus = NULL;
    assert_int_equal(backend->connect(&bus), 0);

    char *version = NULL;
    int rc = -1;
    for (int i = 0; i < 100 && rc != 0; i++) {
        free(version); version = NULL;
        rc = backend->get_property(bus, IP_DBUS_NAME,
            IP_DBUS_MANAGER_PATH, IP_IFACE_MANAGER, "Version", &version);
        if (rc != 0) usleep(10000);
    }
    assert_int_equal(rc, 0);
    free(version);

    const char *comp0 = "/org/shadowblip/InputPlumber/CompositeDevice0";

    /* --- Step 1: Create 2 targets --- */
    char *t0 = NULL, *t1 = NULL;
    assert_int_equal(ip_manager_create_target_device(
        backend, bus, "xb360", &t0), 0);
    assert_non_null(t0);
    assert_int_equal(ip_manager_create_target_device(
        backend, bus, "ds5", &t1), 0);
    assert_non_null(t1);

    /* --- Step 2: Attach targets to composite (routability) --- */
    assert_int_equal(ip_manager_attach_target_device(
        backend, bus, t0, comp0), 0);
    assert_int_equal(ip_manager_attach_target_device(
        backend, bus, t1, comp0), 0);

    /* Verify TargetDevices lists both. */
    char *td = NULL;
    assert_int_equal(ip_composite_get_target_devices(
        backend, bus, comp0, &td), 0);
    assert_non_null(td);
    assert_true(strstr(td, t0) != NULL);
    assert_true(strstr(td, t1) != NULL);
    free(td);

    /* --- Step 3: LoadProfilePath on composite --- */
    const char *test_profile = "/usr/share/inputplumber/profiles/default.yaml";
    assert_int_equal(ip_composite_load_profile_path(
        backend, bus, comp0, test_profile), 0);

    /* --- Step 4: Verify ProfilePath matches (engine state verified) --- */
    char *pp = NULL;
    assert_int_equal(ip_composite_get_profile_path(
        backend, bus, comp0, &pp), 0);
    assert_non_null(pp);
    assert_string_equal(pp, test_profile);
    free(pp);

    /* --- Step 5: Verify ProfileName is derived correctly --- */
    char *pn = NULL;
    assert_int_equal(ip_composite_get_profile_name(
        backend, bus, comp0, &pn), 0);
    assert_non_null(pn);
    assert_string_equal(pn, "default");
    free(pn);

    /* --- Step 6: Set GamepadOrder --- */
    char order_csv[512];
    snprintf(order_csv, sizeof(order_csv), "%s", comp0);

    /* Build a device model for validation. */
    cbx_device_model model;
    cbx_device_model_init(&model);
    assert_int_equal(cbx_objectmanager_enumerate(backend, bus, &model), 0);

    /* Set GamepadOrder with proper model validation. */
    assert_int_equal(ip_manager_set_gamepad_order(
        backend, bus, order_csv, &model), 0);

    /* --- Step 7: Read back GamepadOrder --- */
    char *go = NULL;
    assert_int_equal(ip_manager_get_gamepad_order(
        backend, bus, &go), 0);
    assert_non_null(go);
    assert_true(strstr(go, comp0) != NULL);
    free(go);

    /* --- Step 8: Simulate restart (clear in-memory GamepadOrder) ---
     * InputPlumber does not persist GamepadOrder (gap #2).  After a
     * daemon restart, GamepadOrder resets to empty.  The GUI restores
     * it from assignments.yaml.  We simulate this by setting it to
     * empty, then restoring. */
    assert_int_equal(ip_manager_set_gamepad_order(
        backend, bus, "", &model), 0);
    char *go_empty = NULL;
    assert_int_equal(ip_manager_get_gamepad_order(
        backend, bus, &go_empty), 0);
    assert_non_null(go_empty);
    /* GamepadOrder should be empty (or not contain comp0). */
    assert_true(go_empty[0] == '\0' || strstr(go_empty, comp0) == NULL);
    free(go_empty);

    /* --- Step 9: Restore GamepadOrder --- */
    assert_int_equal(ip_manager_set_gamepad_order(
        backend, bus, order_csv, &model), 0);
    char *go_restored = NULL;
    assert_int_equal(ip_manager_get_gamepad_order(
        backend, bus, &go_restored), 0);
    assert_non_null(go_restored);
    assert_true(strstr(go_restored, comp0) != NULL);
    free(go_restored);

    /* --- Step 10: PersistentId is stable per composite --- */
    char *pid0 = NULL;
    assert_int_equal(ip_composite_get_persistent_id(
        backend, bus, comp0, &pid0), 0);
    assert_non_null(pid0);
    assert_string_equal(pid0, "comp-0");
    free(pid0);

    /* --- Step 11: LoadProfilePath with different profile verifies
     * engine state changes before persistence. */
    const char *fighting_profile = "/usr/share/inputplumber/profiles/fighting.yaml";
    assert_int_equal(ip_composite_load_profile_path(
        backend, bus, comp0, fighting_profile), 0);
    char *pp2 = NULL;
    assert_int_equal(ip_composite_get_profile_path(
        backend, bus, comp0, &pp2), 0);
    assert_non_null(pp2);
    assert_string_equal(pp2, fighting_profile);
    free(pp2);
    char *pn2 = NULL;
    assert_int_equal(ip_composite_get_profile_name(
        backend, bus, comp0, &pn2), 0);
    assert_non_null(pn2);
    assert_string_equal(pn2, "fighting");
    free(pn2);

    /* Clean up all targets. */
    for (int i = model.target_count - 1; i >= 0; i--)
        ip_manager_stop_target_device(backend, bus, model.targets[i].path);

    free(t0); free(t1);
    backend->disconnect(bus);
    kill(server_pid, SIGTERM);
    waitpid(server_pid, NULL, 0);
    private_server_pid = 0;
    kill(daemon_pid, SIGTERM);
    waitpid(daemon_pid, NULL, 0);
    private_daemon_pid = 0;
    unsetenv("DBUS_SYSTEM_BUS_ADDRESS");
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_teardown(test_production_backend_native_roundtrip,
                                  cleanup_processes),
        cmocka_unit_test_teardown(test_native_owner_loss_and_reacquisition,
                                  cleanup_processes),
        cmocka_unit_test_teardown(test_native_target_operations,
                                  cleanup_processes),
        cmocka_unit_test_teardown(test_native_topology_reconciliation,
                                  cleanup_processes),
        cmocka_unit_test_teardown(test_native_assignment_application,
                                  cleanup_processes),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
