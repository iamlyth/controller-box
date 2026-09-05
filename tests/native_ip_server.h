/*
 * native_ip_server.h — Reusable private InputPlumber-compatible DBus server
 * for native-signature testing.
 *
 * Provides a forked sd-bus server that emulates InputPlumber's DBus
 * interfaces with correct native type signatures (u, b, as, s, sd).
 * Supports: Manager, CompositeDevice (writable InterceptMode +
 * SetInterceptActivation), Target (fallback vtable), ObjectManager,
 * and org.shadowblip.Input.DBusDevice (InputEvent signal emission).
 *
 * Shared by test_native_dbus.c, test_installed_functional.c, and
 * future overlay interaction tests.
 */
#ifndef NATIVE_IP_SERVER_H
#define NATIVE_IP_SERVER_H

#include <stdbool.h>
#include <sys/types.h>
#include <systemd/sd-bus.h>

/* --- Limits --- */
#define NIP_MAX_COMPOSITES  16
#define NIP_MAX_TARGETS     16
#define NIP_MAX_ATTACHED    16

/* --- Server configuration --- */
typedef struct {
    int         num_composites;
    const char *version;          /* Version string (e.g. "9.8.7", "0.78.0") */
    unsigned    publication_delay_ms; /* Create reply precedes OM publication */
    unsigned    removal_delay_ms;     /* Stop reply precedes OM disappearance */
    bool        reverse_object_order; /* exercise unordered OM dictionaries */
    bool        fail_stop;
    bool        fail_attach;
    int         hide_attachment_for_composite; /* 1-based; 0 = expose all */
} nip_server_config;

/* --- Server handle --- */
typedef struct {
    char    address[512];
    pid_t   daemon_pid;
    pid_t   server_pid;
} nip_server_handle;

/* --- Global server state (accessible to parent before fork) --- */

extern char   g_nip_profile_path[NIP_MAX_COMPOSITES][256];
extern char   g_nip_profile_name[NIP_MAX_COMPOSITES][64];
extern char   g_nip_gamepad_order[NIP_MAX_COMPOSITES][256];
extern int    g_nip_gamepad_order_count;

extern char   g_nip_target_paths[NIP_MAX_TARGETS][256];
extern char   g_nip_target_types[NIP_MAX_TARGETS][32];
extern int    g_nip_target_count;

extern char   g_nip_attached[NIP_MAX_COMPOSITES][NIP_MAX_ATTACHED][256];
extern int    g_nip_attached_counts[NIP_MAX_COMPOSITES];

extern uint32_t g_nip_intercept_mode[NIP_MAX_COMPOSITES];
extern char   g_nip_dbus_devices[NIP_MAX_COMPOSITES][256];
extern char   g_nip_comp_names[NIP_MAX_COMPOSITES][64];
extern char   g_nip_persistent_ids[NIP_MAX_COMPOSITES][32];

/* ManageAllDevices is a writable boolean on the Manager interface. */
extern int    g_nip_manage_all_devices;

/* When non-zero, the next CreateTargetDevice call returns a DBus error
 * and auto-resets to 0.  Set before nip_fork_server/nip_start_server
 * to simulate a transient backend failure. */
extern volatile sig_atomic_t g_nip_fail_next_create;

/* --- API --- */

/*
 * Reset all server state to defaults.  Call before nip_start_server
 * to ensure the forked child starts with clean state.
 * If num_composites > 0, initializes composite names and persistent IDs
 * for that many composites.
 */
void nip_reset_server_state(int num_composites);

/*
 * Fork the InputPlumber-compatible server onto an existing private bus.
 * Use after nip_start_private_bus() for tests that need to restart the
 * server without restarting the daemon.
 * Sets DBUS_SYSTEM_BUS_ADDRESS env var if not already set.
 * Returns server PID (>0) on success, negative errno on failure.
 */
pid_t nip_fork_server(const char *address, const nip_server_config *cfg);

/*
 * Start a private dbus-daemon and fork the InputPlumber-compatible
 * server as a child process.  Sets DBUS_SYSTEM_BUS_ADDRESS env var.
 * Returns 0 on success, negative errno on failure.
 */
int nip_start_server(nip_server_handle *h, const nip_server_config *cfg);

/*
 * Stop the server and daemon, clean up child processes.
 * Unsets DBUS_SYSTEM_BUS_ADDRESS.
 */
void nip_stop_server(nip_server_handle *h);

/*
 * Start a private dbus-daemon only (no InputPlumber server).
 * Returns 0 on success, negative errno on failure.
 * Sets *bus_pid to the daemon's PID.
 */
int nip_start_private_bus(char *address, size_t address_len, pid_t *bus_pid);

#endif /* NATIVE_IP_SERVER_H */