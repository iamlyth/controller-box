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
        if (rc >= 0) rc = sd_bus_message_close_container(reply);
        return rc;
    }
    if (strcmp(property, "InterceptMode") == 0)
        return sd_bus_message_append(reply, "u", (uint32_t)2);
    if (strcmp(property, "Enabled") == 0)
        return sd_bus_message_append(reply, "b", 1);
    return -ENOENT;
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
    assert_string_equal(value, "xb360,ds5"); free(value);

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

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_teardown(test_production_backend_native_roundtrip,
                                  cleanup_processes),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
