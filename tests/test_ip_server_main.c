/*
 * test_ip_server_main.c — Standalone InputPlumber-compatible DBus server.
 *
 * Used by test_installed_binary.sh to provide a native-signature
 * InputPlumber-compatible server on a private dbus-daemon, so the
 * installed `controller-box` binary can connect to a real DBus backend
 * without a system-wide InputPlumber installation.
 *
 * This binary does NOT link against libcontrollerbox.  It only uses
 * native_ip_server.c (sd-bus) and the DBus interface constants from
 * dbus_mock.h.
 *
 * Usage: test_ip_server [address-file-path]
 *
 * Writes the private bus address to the specified file (default:
 * /tmp/cbx_test_bus_addr) so the test script can set
 * DBUS_SYSTEM_BUS_ADDRESS for the controller-box subprocess.
 *
 * On SIGTERM/SIGINT: kills child processes (server + daemon) and exits.
 */
#include "native_ip_server.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static volatile sig_atomic_t s_running = 1;

static void handle_signal(int sig)
{
    (void)sig;
    s_running = 0;
}

int main(int argc, char **argv)
{
    const char *addr_file = (argc > 1) ? argv[1] : "/tmp/cbx_test_bus_addr";

    signal(SIGTERM, handle_signal);
    signal(SIGINT,  handle_signal);

    char  address[512];
    pid_t daemon_pid = 0;

    int rc = nip_start_private_bus(address, sizeof(address), &daemon_pid);
    if (rc < 0) {
        fprintf(stderr, "test_ip_server: failed to start private bus: %d\n", rc);
        return 1;
    }

    /* Write bus address to file so the test script can read it. */
    FILE *fp = fopen(addr_file, "w");
    if (!fp) {
        fprintf(stderr, "test_ip_server: cannot write address file: %s\n", addr_file);
        kill(daemon_pid, SIGTERM);
        waitpid(daemon_pid, NULL, 0);
        return 1;
    }
    fprintf(fp, "%s\n", address);
    fclose(fp);

    /* Set env var so nip_fork_server's child inherits it. */
    setenv("DBUS_SYSTEM_BUS_ADDRESS", address, 1);

    /* Reset and fork the InputPlumber-compatible server. */
    nip_reset_server_state(2);
    nip_server_config cfg = { .num_composites = 2, .version = "0.78.0" };
    pid_t server_pid = nip_fork_server(address, &cfg);
    if (server_pid < 0) {
        fprintf(stderr, "test_ip_server: failed to fork server: %d\n",
                (int)server_pid);
        kill(daemon_pid, SIGTERM);
        waitpid(daemon_pid, NULL, 0);
        unlink(addr_file);
        return 1;
    }

    fprintf(stderr, "test_ip_server: ready (bus=%s server=%d daemon=%d)\n",
            address, (int)server_pid, (int)daemon_pid);

    /* Wait for termination signal. */
    while (s_running)
        sleep(1);

    /* Cleanup: kill server first, then daemon. */
    kill(server_pid, SIGTERM);
    waitpid(server_pid, NULL, 0);
    kill(daemon_pid, SIGTERM);
    waitpid(daemon_pid, NULL, 0);
    unlink(addr_file);

    fprintf(stderr, "test_ip_server: stopped\n");
    return 0;
}