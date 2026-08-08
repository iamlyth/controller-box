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

static int property_get(sd_bus *bus, const char *path, const char *interface,
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
    return -ENOENT;
}

/* --- Target state for native fixture test --- */

#define NATIVE_MAX_TARGETS 16
static char g_target_paths[NATIVE_MAX_TARGETS][256];
static char g_target_types[NATIVE_MAX_TARGETS][32];
static int  g_target_count = 0;

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
    SD_BUS_METHOD("CreateTargetDevice", "s", "s", method_create_target, 0),
    SD_BUS_METHOD("StopTargetDevice", "s", "", method_stop_target, 0),
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

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_teardown(test_production_backend_native_roundtrip,
                                  cleanup_processes),
        cmocka_unit_test_teardown(test_native_owner_loss_and_reacquisition,
                                  cleanup_processes),
        cmocka_unit_test_teardown(test_native_target_operations,
                                  cleanup_processes),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
