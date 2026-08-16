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
#include "dbus/ip_input_signal.h"
#include "dbus_mock.h"             /* IP_DBUS_PATH, IP_IFACE_* */
#include "config/config_settings.h"
#include "app/overlay_service.h"   /* cbx_reconcile_startup_targets */
#include "native_ip_server.h"

/* --- Helpers --- */

static void drain_bus(const ip_dbus_backend *backend, ip_bus_handle bus, int ms)
{
    for (int i = 0; i < ms / 10; i++) {
        int processed = backend->process(bus);
        if (processed <= 0)
            usleep(10000);
    }
}

/* Wait for server to come up by polling for Version property. */
static int wait_for_server(const ip_dbus_backend *backend, ip_bus_handle bus,
                           const char *version)
{
    char *value = NULL;
    int rc = -1;
    for (int i = 0; i < 100 && rc != 0; i++) {
        free(value); value = NULL;
        rc = backend->get_property(bus, IP_DBUS_NAME,
            IP_DBUS_MANAGER_PATH, IP_IFACE_MANAGER, "Version", &value);
        if (rc != 0) usleep(10000);
    }
    if (rc == 0 && version && value) {
        if (strcmp(value, version) != 0)
            rc = -1;
    }
    free(value);
    return rc;
}

/* ================================================================== */
/*  Test 1: Manager property round-trip with native type signatures      */
/* ================================================================== */

static void test_production_backend_native_roundtrip(void **state)
{
    (void)state;
    nip_server_handle sh;
    const nip_server_config cfg = { .num_composites = 1, .version = "9.8.7" };
    assert_int_equal(nip_start_server(&sh, &cfg), 0);

    const ip_dbus_backend *backend = ip_dbus_sd_backend();
    ip_bus_handle bus = NULL;
    assert_int_equal(backend->connect(&bus), 0);

    assert_int_equal(wait_for_server(backend, bus, "9.8.7"), 0);

    char *value = NULL;

    /* Version (s) */
    assert_int_equal(backend->get_property(bus, IP_DBUS_NAME,
        IP_DBUS_MANAGER_PATH, IP_IFACE_MANAGER, "Version", &value), 0);
    assert_string_equal(value, "9.8.7"); free(value);

    /* SupportedTargetDeviceIds (as) */
    assert_int_equal(backend->get_property(bus, IP_DBUS_NAME,
        IP_DBUS_MANAGER_PATH, IP_IFACE_MANAGER,
        "SupportedTargetDeviceIds", &value), 0);
    assert_string_equal(value, "xb360,ds5,gamepad"); free(value);

    /* SupportedTargetDevices (as) — human-readable names */
    assert_int_equal(backend->get_property(bus, IP_DBUS_NAME,
        IP_DBUS_MANAGER_PATH, IP_IFACE_MANAGER,
        "SupportedTargetDevices", &value), 0);
    assert_string_equal(value, "Xbox 360 Controller,DualSense,Generic Gamepad");
    free(value);

    /* InterceptMode (u) — Manager-level, read-only */
    assert_int_equal(backend->get_property(bus, IP_DBUS_NAME,
        IP_DBUS_MANAGER_PATH, IP_IFACE_MANAGER, "InterceptMode", &value), 0);
    assert_string_equal(value, "2"); free(value);

    /* Enabled (b) */
    assert_int_equal(backend->get_property(bus, IP_DBUS_NAME,
        IP_DBUS_MANAGER_PATH, IP_IFACE_MANAGER, "Enabled", &value), 0);
    assert_string_equal(value, "1"); free(value);

    backend->disconnect(bus);
    nip_stop_server(&sh);
}

/* ================================================================== */
/*  Test 2: Owner loss and reacquisition via real sd-bus                 */
/* ================================================================== */

static int s_native_reenum_called = 0;
static int s_native_degraded_called = 0;
static char s_native_degraded_reason[256] = {0};

static void native_reenumerate_cb(void *ud)
{
    (void)ud;
    s_native_reenum_called++;
}

static void native_degraded_cb(const char *reason, void *ud)
{
    (void)ud;
    s_native_degraded_called++;
    if (reason)
        snprintf(s_native_degraded_reason,
                 sizeof(s_native_degraded_reason), "%s", reason);
}

static void test_native_owner_loss_and_reacquisition(void **state)
{
    (void)state;
    char address[512];
    pid_t daemon_pid = 0;
    assert_int_equal(nip_start_private_bus(address, sizeof(address),
                                             &daemon_pid), 0);
    setenv("DBUS_SYSTEM_BUS_ADDRESS", address, 1);

    const nip_server_config cfg = { .num_composites = 1, .version = "9.8.7" };
    nip_reset_server_state(1);
    pid_t server_pid = nip_fork_server(address, &cfg);
    assert_true(server_pid > 0);
    usleep(100000);

    const ip_dbus_backend *backend = ip_dbus_sd_backend();
    ip_connection conn;
    ip_connection_init(&conn, backend);
    s_native_reenum_called = 0;
    s_native_degraded_called = 0;

    int rc = ip_connection_connect(&conn);
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

    ip_connection_set_reenumerate_cb(&conn, native_reenumerate_cb, NULL);
    ip_connection_set_degraded_cb(&conn, native_degraded_cb, NULL);

    /* Phase 1: Kill server (name loss). */
    kill(server_pid, SIGTERM);
    waitpid(server_pid, NULL, 0);
    drain_bus(backend, conn.bus, 2000);

    assert_true(ip_connection_is_degraded(&conn));
    assert_int_equal(s_native_degraded_called, 1);
    assert_string_equal(s_native_degraded_reason, "InputPlumber stopped");

    /* Phase 2: Restart server (name acquisition). */
    nip_reset_server_state(1);
    server_pid = nip_fork_server(address, &cfg);
    assert_true(server_pid > 0);
    drain_bus(backend, conn.bus, 2000);

    assert_true(ip_connection_is_connected(&conn));
    assert_int_equal(s_native_reenum_called, 1);

    ip_connection_disconnect(&conn);
    kill(server_pid, SIGTERM);
    waitpid(server_pid, NULL, 0);
    kill(daemon_pid, SIGTERM);
    waitpid(daemon_pid, NULL, 0);
    unsetenv("DBUS_SYSTEM_BUS_ADDRESS");
}

/* ================================================================== */
/*  Test 3: Target operations via real sd-bus                           */
/* ================================================================== */

static void test_native_target_operations(void **state)
{
    (void)state;
    nip_server_handle sh;
    const nip_server_config cfg = { .num_composites = 1, .version = "9.8.7" };
    nip_reset_server_state(1);
    assert_int_equal(nip_start_server(&sh, &cfg), 0);

    const ip_dbus_backend *backend = ip_dbus_sd_backend();
    ip_bus_handle bus = NULL;
    assert_int_equal(backend->connect(&bus), 0);
    assert_int_equal(wait_for_server(backend, bus, "9.8.7"), 0);

    /* SupportedTargetDeviceIds (native `as`). */
    char *types_csv = NULL;
    assert_int_equal(ip_manager_get_supported_target_device_ids(
        backend, bus, &types_csv), 0);
    assert_string_equal(types_csv, "xb360,ds5,gamepad");
    free(types_csv);

    /* SupportedTargetDevices (native `as`) — human-readable names via wrapper. */
    char *devices_csv = NULL;
    assert_int_equal(ip_manager_get_supported_target_devices(
        backend, bus, &devices_csv), 0);
    assert_non_null(devices_csv);
    assert_string_equal(devices_csv, "Xbox 360 Controller,DualSense,Generic Gamepad");
    free(devices_csv);

    /* CreateTargetDevice. */
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

    /* GetManagedObjects lists both targets. */
    cbx_device_model model;
    cbx_device_model_init(&model);
    assert_int_equal(cbx_objectmanager_enumerate(backend, bus, &model), 0);
    assert_int_equal(model.target_count, 2);
    assert_true(model.has_manager);
    assert_int_equal(model.composite_count, 1);

    bool found0 = false, found1 = false;
    for (int i = 0; i < model.target_count; i++) {
        if (strcmp(model.targets[i].path, path0) == 0) found0 = true;
        if (strcmp(model.targets[i].path, path1) == 0) found1 = true;
    }
    assert_true(found0);
    assert_true(found1);

    /* DeviceType property on each target. */
    char *dtype0 = NULL;
    assert_int_equal(ip_target_get_device_type(backend, bus, path0, &dtype0), 0);
    assert_string_equal(dtype0, "gamepad"); free(dtype0);

    char *dtype1 = NULL;
    assert_int_equal(ip_target_get_device_type(backend, bus, path1, &dtype1), 0);
    assert_string_equal(dtype1, "xb360"); free(dtype1);

    /* StopTargetDevice removes from ObjectManager. */
    assert_int_equal(ip_manager_stop_target_device(backend, bus, path0), 0);
    free(path0);

    cbx_device_model model2;
    cbx_device_model_init(&model2);
    assert_int_equal(cbx_objectmanager_enumerate(backend, bus, &model2), 0);
    assert_int_equal(model2.target_count, 1);
    assert_string_equal(model2.targets[0].path, path1);

    /* DeviceType on removed target fails. */
    char *dtype_removed = (char *)0xdeadbeef;
    int rc_removed = ip_target_get_device_type(backend, bus,
        "/org/shadowblip/InputPlumber/devices/target/gamepad0",
        &dtype_removed);
    assert_true(rc_removed < 0);
    assert_null(dtype_removed);

    free(path1);
    backend->disconnect(bus);
    nip_stop_server(&sh);
}

/* ================================================================== */
/*  Test 4: Topology reconciliation via real sd-bus                       */
/* ================================================================== */

static void test_native_topology_reconciliation(void **state)
{
    (void)state;
    nip_server_handle sh;
    const nip_server_config cfg = { .num_composites = 1, .version = "9.8.7" };
    nip_reset_server_state(1);
    assert_int_equal(nip_start_server(&sh, &cfg), 0);

    const ip_dbus_backend *backend = ip_dbus_sd_backend();
    ip_bus_handle bus = NULL;
    assert_int_equal(backend->connect(&bus), 0);
    assert_int_equal(wait_for_server(backend, bus, NULL), 0);

    /* Scenario 1: Clean startup creates ordered topology. */
    cbx_device_model model;
    cbx_device_model_init(&model);
    assert_int_equal(cbx_objectmanager_enumerate(backend, bus, &model), 0);
    assert_int_equal(model.target_count, 0);
    assert_int_equal(model.composite_count, 1);

    char *path0 = NULL, *path1 = NULL, *path2 = NULL;
    assert_int_equal(ip_manager_create_target_device(
        backend, bus, "xb360", &path0), 0);
    assert_int_equal(ip_manager_create_target_device(
        backend, bus, "ds5", &path1), 0);
    assert_int_equal(ip_manager_create_target_device(
        backend, bus, "gamepad", &path2), 0);

    cbx_device_model_init(&model);
    assert_int_equal(cbx_objectmanager_enumerate(backend, bus, &model), 0);
    assert_int_equal(model.target_count, 3);

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

    const char *comp0 = "/org/shadowblip/InputPlumber/CompositeDevice0";
    assert_int_equal(ip_manager_attach_target_device(
        backend, bus, model.targets[0].path, comp0), 0);
    assert_int_equal(ip_manager_attach_target_device(
        backend, bus, model.targets[1].path, comp0), 0);
    assert_int_equal(ip_manager_attach_target_device(
        backend, bus, model.targets[2].path, comp0), 0);

    char *td = NULL;
    assert_int_equal(ip_composite_get_target_devices(
        backend, bus, comp0, &td), 0);
    assert_non_null(td);
    assert_true(strstr(td, model.targets[0].path) != NULL);
    assert_true(strstr(td, model.targets[1].path) != NULL);
    assert_true(strstr(td, model.targets[2].path) != NULL);
    free(td);

    /* Scenario 2: Remove one slot, verify others preserved. */
    char slot1_path[256];
    snprintf(slot1_path, sizeof(slot1_path), "%s", model.targets[1].path);
    assert_int_equal(ip_manager_stop_target_device(backend, bus, slot1_path), 0);

    cbx_device_model model2;
    cbx_device_model_init(&model2);
    assert_int_equal(cbx_objectmanager_enumerate(backend, bus, &model2), 0);
    assert_int_equal(model2.target_count, 2);
    char *dt0 = NULL;
    assert_int_equal(ip_target_get_device_type(backend, bus,
        model2.targets[0].path, &dt0), 0);
    assert_string_equal(dt0, "xb360"); free(dt0);
    char *dt1 = NULL;
    assert_int_equal(ip_target_get_device_type(backend, bus,
        model2.targets[1].path, &dt1), 0);
    assert_string_equal(dt1, "gamepad"); free(dt1);

    /* Scenario 3: Type correction (stop+create). */
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
    char *s3_dt0 = NULL;
    assert_int_equal(ip_target_get_device_type(backend, bus,
        model3.targets[0].path, &s3_dt0), 0);
    assert_string_equal(s3_dt0, "xb360"); free(s3_dt0);
    char *s3_dt1 = NULL;
    assert_int_equal(ip_target_get_device_type(backend, bus,
        model3.targets[1].path, &s3_dt1), 0);
    assert_string_equal(s3_dt1, "ds5"); free(s3_dt1);

    /* Scenario 4: Attach after type correction. */
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

    for (int i = model3.target_count - 1; i >= 0; i--)
        ip_manager_stop_target_device(backend, bus, model3.targets[i].path);

    free(path0); free(path1); free(path2);
    backend->disconnect(bus);
    nip_stop_server(&sh);
}

/* ================================================================== */
/*  Test 5: Assignment/profile application via real sd-bus              */
/* ================================================================== */

static void test_native_assignment_application(void **state)
{
    (void)state;
    nip_server_handle sh;
    const nip_server_config cfg = { .num_composites = 1, .version = "9.8.7" };
    nip_reset_server_state(1);
    assert_int_equal(nip_start_server(&sh, &cfg), 0);

    const ip_dbus_backend *backend = ip_dbus_sd_backend();
    ip_bus_handle bus = NULL;
    assert_int_equal(backend->connect(&bus), 0);
    assert_int_equal(wait_for_server(backend, bus, NULL), 0);

    const char *comp0 = "/org/shadowblip/InputPlumber/CompositeDevice0";

    /* Create 2 targets and attach. */
    char *t0 = NULL, *t1 = NULL;
    assert_int_equal(ip_manager_create_target_device(
        backend, bus, "xb360", &t0), 0);
    assert_int_equal(ip_manager_create_target_device(
        backend, bus, "ds5", &t1), 0);
    assert_int_equal(ip_manager_attach_target_device(
        backend, bus, t0, comp0), 0);
    assert_int_equal(ip_manager_attach_target_device(
        backend, bus, t1, comp0), 0);

    char *td = NULL;
    assert_int_equal(ip_composite_get_target_devices(
        backend, bus, comp0, &td), 0);
    assert_true(strstr(td, t0) != NULL);
    assert_true(strstr(td, t1) != NULL);
    free(td);

    /* LoadProfilePath. */
    const char *test_profile = "/usr/share/inputplumber/profiles/default.yaml";
    assert_int_equal(ip_composite_load_profile_path(
        backend, bus, comp0, test_profile), 0);

    char *pp = NULL;
    assert_int_equal(ip_composite_get_profile_path(
        backend, bus, comp0, &pp), 0);
    assert_string_equal(pp, test_profile); free(pp);

    char *pn = NULL;
    assert_int_equal(ip_composite_get_profile_name(
        backend, bus, comp0, &pn), 0);
    assert_string_equal(pn, "default"); free(pn);

    /* Set/Get GamepadOrder. */
    char order_csv[512];
    snprintf(order_csv, sizeof(order_csv), "%s", comp0);
    cbx_device_model model;
    cbx_device_model_init(&model);
    assert_int_equal(cbx_objectmanager_enumerate(backend, bus, &model), 0);
    assert_int_equal(ip_manager_set_gamepad_order(
        backend, bus, order_csv, &model), 0);
    char *go = NULL;
    assert_int_equal(ip_manager_get_gamepad_order(
        backend, bus, &go), 0);
    assert_true(strstr(go, comp0) != NULL); free(go);

    /* Simulate restart (clear GamepadOrder). */
    assert_int_equal(ip_manager_set_gamepad_order(
        backend, bus, "", &model), 0);
    char *go_empty = NULL;
    assert_int_equal(ip_manager_get_gamepad_order(
        backend, bus, &go_empty), 0);
    assert_true(go_empty[0] == '\0' || strstr(go_empty, comp0) == NULL);
    free(go_empty);

    /* Restore GamepadOrder. */
    assert_int_equal(ip_manager_set_gamepad_order(
        backend, bus, order_csv, &model), 0);
    char *go_restored = NULL;
    assert_int_equal(ip_manager_get_gamepad_order(
        backend, bus, &go_restored), 0);
    assert_true(strstr(go_restored, comp0) != NULL); free(go_restored);

    /* PersistentId. */
    char *pid0 = NULL;
    assert_int_equal(ip_composite_get_persistent_id(
        backend, bus, comp0, &pid0), 0);
    assert_string_equal(pid0, "comp-0"); free(pid0);

    /* Profile switch. */
    const char *fighting = "/usr/share/inputplumber/profiles/fighting.yaml";
    assert_int_equal(ip_composite_load_profile_path(
        backend, bus, comp0, fighting), 0);
    char *pp2 = NULL;
    assert_int_equal(ip_composite_get_profile_path(
        backend, bus, comp0, &pp2), 0);
    assert_string_equal(pp2, fighting); free(pp2);
    char *pn2 = NULL;
    assert_int_equal(ip_composite_get_profile_name(
        backend, bus, comp0, &pn2), 0);
    assert_string_equal(pn2, "fighting"); free(pn2);

    for (int i = model.target_count - 1; i >= 0; i--)
        ip_manager_stop_target_device(backend, bus, model.targets[i].path);
    free(t0); free(t1);
    backend->disconnect(bus);
    nip_stop_server(&sh);
}

/* ================================================================== */
/*  Test 6: Startup reconciliation via production path                   */
/* ================================================================== */

static void test_native_startup_reconciliation_prod_path(void **state)
{
    (void)state;
    nip_server_handle sh;
    const nip_server_config cfg = { .num_composites = 1, .version = "9.8.7" };
    nip_reset_server_state(1);
    assert_int_equal(nip_start_server(&sh, &cfg), 0);

    const ip_dbus_backend *backend = ip_dbus_sd_backend();
    ip_bus_handle bus = NULL;
    assert_int_equal(backend->connect(&bus), 0);
    assert_int_equal(wait_for_server(backend, bus, NULL), 0);

    /* Scenario 1: Reconcile from empty to 3 targets. */
    cbx_overlay_service_ctx svc;
    memset(&svc, 0, sizeof(svc));
    svc.conn.backend = backend;
    svc.conn.bus = bus;
    cbx_device_model_init(&svc.model);
    cbx_settings_defaults(&svc.settings);
    svc.settings.virtual_controllers.count = 3;
    snprintf(svc.settings.virtual_controllers.types[0],
             sizeof(svc.settings.virtual_controllers.types[0]), "xb360");
    snprintf(svc.settings.virtual_controllers.types[1],
             sizeof(svc.settings.virtual_controllers.types[1]), "ds5");
    snprintf(svc.settings.virtual_controllers.types[2],
             sizeof(svc.settings.virtual_controllers.types[2]), "gamepad");

    assert_int_equal(cbx_objectmanager_enumerate(backend, bus, &svc.model), 0);
    assert_int_equal(svc.model.target_count, 0);
    assert_int_equal(svc.model.composite_count, 1);

    int rc = cbx_reconcile_startup_targets(&svc);
    assert_int_equal(rc, 0);
    assert_int_equal(svc.model.target_count, 3);

    char *dt0 = NULL, *dt1 = NULL, *dt2 = NULL;
    assert_int_equal(ip_target_get_device_type(backend, bus,
        svc.model.targets[0].path, &dt0), 0);
    assert_string_equal(dt0, "xb360"); free(dt0);
    assert_int_equal(ip_target_get_device_type(backend, bus,
        svc.model.targets[1].path, &dt1), 0);
    assert_string_equal(dt1, "ds5"); free(dt1);
    assert_int_equal(ip_target_get_device_type(backend, bus,
        svc.model.targets[2].path, &dt2), 0);
    assert_string_equal(dt2, "gamepad"); free(dt2);

    const char *comp0 = "/org/shadowblip/InputPlumber/CompositeDevice0";
    char *td = NULL;
    assert_int_equal(ip_composite_get_target_devices(
        backend, bus, comp0, &td), 0);
    assert_true(strstr(td, svc.model.targets[0].path) != NULL);
    free(td);

    /* Scenario 2: Shrink to 1 VC. */
    svc.settings.virtual_controllers.count = 1;
    rc = cbx_reconcile_startup_targets(&svc);
    assert_int_equal(rc, 0);
    assert_int_equal(svc.model.target_count, 1);
    char *dt_remain = NULL;
    assert_int_equal(ip_target_get_device_type(backend, bus,
        svc.model.targets[0].path, &dt_remain), 0);
    assert_string_equal(dt_remain, "xb360"); free(dt_remain);

    /* Scenario 3: Type correction. */
    snprintf(svc.settings.virtual_controllers.types[0],
             sizeof(svc.settings.virtual_controllers.types[0]), "ds5");
    rc = cbx_reconcile_startup_targets(&svc);
    assert_int_equal(rc, 0);
    assert_int_equal(svc.model.target_count, 1);
    char *dt_corrected = NULL;
    assert_int_equal(ip_target_get_device_type(backend, bus,
        svc.model.targets[0].path, &dt_corrected), 0);
    assert_string_equal(dt_corrected, "ds5"); free(dt_corrected);

    char *td2 = NULL;
    assert_int_equal(ip_composite_get_target_devices(
        backend, bus, comp0, &td2), 0);
    assert_true(strstr(td2, svc.model.targets[0].path) != NULL);
    free(td2);

    for (int i = svc.model.target_count - 1; i >= 0; i--)
        ip_manager_stop_target_device(backend, bus,
            svc.model.targets[i].path);

    backend->disconnect(bus);
    nip_stop_server(&sh);
}

/* ================================================================== */
/*  Test 7 (NEW): SetInterceptActivation round-trip                      */
/* ================================================================== */

static void test_native_set_intercept_activation(void **state)
{
    (void)state;
    nip_server_handle sh;
    const nip_server_config cfg = { .num_composites = 1, .version = "9.8.7" };
    nip_reset_server_state(1);
    assert_int_equal(nip_start_server(&sh, &cfg), 0);

    const ip_dbus_backend *backend = ip_dbus_sd_backend();
    ip_bus_handle bus = NULL;
    assert_int_equal(backend->connect(&bus), 0);
    assert_int_equal(wait_for_server(backend, bus, NULL), 0);

    const char *comp0 = "/org/shadowblip/InputPlumber/CompositeDevice0";

    /* SetInterceptActivation(activation_events: as, target_event: s)
     * Call through the production sd-bus backend with native signatures. */
    int rc = ip_composite_set_intercept_activation(
        backend, bus, comp0, "Select+A", "A");
    assert_int_equal(rc, 0);

    /* Verify the call succeeded — the server consumed the arguments
     * and replied OK.  Call again with different events to confirm
     * the as container is parsed correctly. */
    rc = ip_composite_set_intercept_activation(
        backend, bus, comp0, "Select+B,Select+X", "B");
    assert_int_equal(rc, 0);

    backend->disconnect(bus);
    nip_stop_server(&sh);
}

/* ================================================================== */
/*  Test 8 (NEW): InterceptMode writable set/get round-trip as `u`       */
/* ================================================================== */

static void test_native_intercept_mode_writable(void **state)
{
    (void)state;
    nip_server_handle sh;
    const nip_server_config cfg = { .num_composites = 1, .version = "9.8.7" };
    nip_reset_server_state(1);
    assert_int_equal(nip_start_server(&sh, &cfg), 0);

    const ip_dbus_backend *backend = ip_dbus_sd_backend();
    ip_bus_handle bus = NULL;
    assert_int_equal(backend->connect(&bus), 0);
    assert_int_equal(wait_for_server(backend, bus, NULL), 0);

    const char *comp0 = "/org/shadowblip/InputPlumber/CompositeDevice0";

    /* Read initial InterceptMode — should be 0 (NONE) after reset. */
    char *mode_str = NULL;
    assert_int_equal(ip_composite_get_intercept_mode(
        backend, bus, comp0, &mode_str), 0);
    assert_non_null(mode_str);
    assert_string_equal(mode_str, "0");
    free(mode_str);

    /* Set InterceptMode = 1 (PASS) via production path. */
    assert_int_equal(ip_composite_set_intercept_mode(
        backend, bus, comp0, "1"), 0);

    /* Read back — should be 1 (PASS). */
    assert_int_equal(ip_composite_get_intercept_mode(
        backend, bus, comp0, &mode_str), 0);
    assert_non_null(mode_str);
    assert_string_equal(mode_str, "1");
    free(mode_str);

    /* Set InterceptMode = 2 (ALL). */
    assert_int_equal(ip_composite_set_intercept_mode(
        backend, bus, comp0, "2"), 0);

    /* Read back — should be 2 (ALL). */
    assert_int_equal(ip_composite_get_intercept_mode(
        backend, bus, comp0, &mode_str), 0);
    assert_non_null(mode_str);
    assert_string_equal(mode_str, "2");
    free(mode_str);

    /* Set back to PASS (1) — overlay close path. */
    assert_int_equal(ip_composite_set_intercept_mode(
        backend, bus, comp0, "1"), 0);
    assert_int_equal(ip_composite_get_intercept_mode(
        backend, bus, comp0, &mode_str), 0);
    assert_string_equal(mode_str, "1");
    free(mode_str);

    backend->disconnect(bus);
    nip_stop_server(&sh);
}

/* ================================================================== */
/*  Test 9 (NEW): Boolean property SET round-trip with native type       */
/* ================================================================== */

static void test_native_boolean_property_set(void **state)
{
    (void)state;
    nip_server_handle sh;
    const nip_server_config cfg = { .num_composites = 1, .version = "9.8.7" };
    nip_reset_server_state(1);
    g_nip_manage_all_devices = 0;
    assert_int_equal(nip_start_server(&sh, &cfg), 0);

    const ip_dbus_backend *backend = ip_dbus_sd_backend();
    ip_bus_handle bus = NULL;
    assert_int_equal(backend->connect(&bus), 0);
    assert_int_equal(wait_for_server(backend, bus, NULL), 0);

    char *value = NULL;

    /* Read initial ManageAllDevices — should be 0 (false) after reset. */
    assert_int_equal(backend->get_property(bus, IP_DBUS_NAME,
        IP_DBUS_MANAGER_PATH, IP_IFACE_MANAGER,
        "ManageAllDevices", &value), 0);
    assert_non_null(value);
    assert_string_equal(value, "0");
    free(value);

    /* SET ManageAllDevices = true via production path.
     * This exercises sd_set_property's boolean branch: the variant must
     * carry native type 'b', not 's', or the sd-bus server rejects it
     * with InvalidArgs. */
    assert_int_equal(backend->set_property(bus, IP_DBUS_NAME,
        IP_DBUS_MANAGER_PATH, IP_IFACE_MANAGER,
        "ManageAllDevices", "1"), 0);

    /* Read back — should be 1 (true). */
    assert_int_equal(backend->get_property(bus, IP_DBUS_NAME,
        IP_DBUS_MANAGER_PATH, IP_IFACE_MANAGER,
        "ManageAllDevices", &value), 0);
    assert_non_null(value);
    assert_string_equal(value, "1");
    free(value);

    /* SET ManageAllDevices = false via production path. */
    assert_int_equal(backend->set_property(bus, IP_DBUS_NAME,
        IP_DBUS_MANAGER_PATH, IP_IFACE_MANAGER,
        "ManageAllDevices", "0"), 0);

    /* Read back — should be 0 (false). */
    assert_int_equal(backend->get_property(bus, IP_DBUS_NAME,
        IP_DBUS_MANAGER_PATH, IP_IFACE_MANAGER,
        "ManageAllDevices", &value), 0);
    assert_non_null(value);
    assert_string_equal(value, "0");
    free(value);

    /* Verify the word form also works. */
    assert_int_equal(backend->set_property(bus, IP_DBUS_NAME,
        IP_DBUS_MANAGER_PATH, IP_IFACE_MANAGER,
        "ManageAllDevices", "true"), 0);
    assert_int_equal(backend->get_property(bus, IP_DBUS_NAME,
        IP_DBUS_MANAGER_PATH, IP_IFACE_MANAGER,
        "ManageAllDevices", &value), 0);
    assert_non_null(value);
    assert_string_equal(value, "1");
    free(value);

    /* Invalid boolean value should be rejected. */
    assert_int_not_equal(backend->set_property(bus, IP_DBUS_NAME,
        IP_DBUS_MANAGER_PATH, IP_IFACE_MANAGER,
        "ManageAllDevices", "maybe"), 0);

    backend->disconnect(bus);
    nip_stop_server(&sh);
}

/* ================================================================== */
/*  Test 10 (NEW): InputEvent signal emission and reception               */
/* ================================================================== */

/* Test callback for InputEvent signals. */
static int s_ie_event_received = 0;
static ip_input_id s_ie_input_id = IP_INPUT_UNKNOWN;
static ip_input_category s_ie_category = IP_INPUT_CAT_BUTTON;
static double s_ie_value = 0.0;
static char s_ie_raw_event[64] = {0};
static char s_ie_device_path[256] = {0};

static void ie_input_event_cb(ip_input_id input, ip_input_category category,
                                double value, const char *raw_event,
                                const char *device_path, void *userdata)
{
    (void)userdata;
    s_ie_event_received++;
    s_ie_input_id = input;
    s_ie_category = category;
    s_ie_value = value;
    if (raw_event)
        snprintf(s_ie_raw_event, sizeof(s_ie_raw_event), "%s", raw_event);
    if (device_path)
        snprintf(s_ie_device_path, sizeof(s_ie_device_path), "%s", device_path);
}

static void test_native_input_event_signal(void **state)
{
    (void)state;
    nip_server_handle sh;
    const nip_server_config cfg = { .num_composites = 1, .version = "9.8.7" };
    nip_reset_server_state(1);
    assert_int_equal(nip_start_server(&sh, &cfg), 0);

    const ip_dbus_backend *backend = ip_dbus_sd_backend();
    ip_bus_handle bus = NULL;
    assert_int_equal(backend->connect(&bus), 0);
    assert_int_equal(wait_for_server(backend, bus, NULL), 0);

    const char *comp0 = "/org/shadowblip/InputPlumber/CompositeDevice0";

    /* Look up the server's unique bus name (needed for sender verification). */
    char *unique_name = NULL;
    assert_int_equal(backend->get_unique_name(bus, IP_DBUS_NAME, &unique_name), 0);
    assert_non_null(unique_name);

    /* Subscribe to InputEvent signals via the production path. */
    ip_input_events ie;
    ip_input_events_init(&ie, backend, bus, unique_name,
                           ie_input_event_cb, NULL);
    s_ie_event_received = 0;
    assert_int_equal(ip_input_events_subscribe(&ie), 0);

    /* Trigger signal emission by calling EmitInputEvent on the
     * DBusDevice interface.  The server emits InputEvent(sd) and
     * replies OK to the method call.  We pass the value as a string
     * because the production call_method vtable supports 's' and 'as'
     * but not 'd'; the server converts to double and emits the real
     * InputEvent(sd) signal. */
    int rc = backend->call_method(bus, IP_DBUS_NAME, comp0,
                                   IP_IFACE_DBUS_DEVICE, "EmitInputEvent",
                                   "ss", "A", "1.0", NULL);
    assert_int_equal(rc, 0);

    /* Drain the bus to receive the signal. */
    drain_bus(backend, bus, 500);

    /* Verify the InputEvent signal was received. */
    assert_int_equal(s_ie_event_received, 1);
    assert_int_equal(s_ie_input_id, IP_INPUT_A);
    assert_int_equal(s_ie_category, IP_INPUT_CAT_BUTTON);
    assert_float_equal(s_ie_value, 1.0, 0.001);
    assert_string_equal(s_ie_raw_event, "A");
    assert_string_equal(s_ie_device_path, comp0);

    /* Emit a second event with an axis value (negative). */
    s_ie_event_received = 0;
    rc = backend->call_method(bus, IP_DBUS_NAME, comp0,
                               IP_IFACE_DBUS_DEVICE, "EmitInputEvent",
                               "ss", "LeftStickX", "-0.5", NULL);
    assert_int_equal(rc, 0);
    drain_bus(backend, bus, 500);

    assert_int_equal(s_ie_event_received, 1);
    assert_int_equal(s_ie_input_id, IP_INPUT_LEFT_STICK_X);
    assert_int_equal(s_ie_category, IP_INPUT_CAT_AXIS);
    assert_float_equal(s_ie_value, -0.5, 0.001);
    assert_string_equal(s_ie_raw_event, "LeftStickX");

    free(unique_name);
    backend->disconnect(bus);
    nip_stop_server(&sh);
}

/* ================================================================== */
/*  Main                                                                */
/* ================================================================== */

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_production_backend_native_roundtrip),
        cmocka_unit_test(test_native_owner_loss_and_reacquisition),
        cmocka_unit_test(test_native_target_operations),
        cmocka_unit_test(test_native_topology_reconciliation),
        cmocka_unit_test(test_native_assignment_application),
        cmocka_unit_test(test_native_startup_reconciliation_prod_path),
        /* New round-trip tests for overlay server capabilities. */
        cmocka_unit_test(test_native_set_intercept_activation),
        cmocka_unit_test(test_native_intercept_mode_writable),
        cmocka_unit_test(test_native_boolean_property_set),
        cmocka_unit_test(test_native_input_event_signal),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}