/*
 * test_composite_identity.c — Tests for source-derived composite identity
 *                             (SPEC §6.2–6.3, task 6).
 *
 * Unit tests drive the extraction module through the mock DBus backend,
 * proving that the identity is collected from the proper source-device
 * interface (UniqueId/PhysPath/IdBustype on evdev/udev; SerialNumber on
 * HIDRaw) and that a transient read failure is not reported as absence.
 */
#include "dbus_mock.h"
#include "dbus/ip_connection.h"   /* IP_ERR_UNKNOWN_INTERFACE */
#include "identify/composite_identity.h"
#include "dbus/ip_device_model.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#define COMP_PATH "/org/shadowblip/InputPlumber/CompositeDevice0"

typedef struct {
    ip_dbus_mock          mock;
    const ip_dbus_backend *backend;
} ci_fixture;

static int
setup(void **state)
{
    ci_fixture *f = malloc(sizeof(*f));
    if (!f)
        return -1;
    memset(f, 0, sizeof(*f));
    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);
    *state = f;
    return 0;
}

static int
teardown(void **state)
{
    ci_fixture *f = *state;
    if (f) {
        ip_dbus_mock_reset(&f->mock);
        free(f);
    }
    return 0;
}

#define FIX(state) (*(ci_fixture **)(state))

static void
expect_evdev(ci_fixture *f, const char *source,
             const char *unique_id, const char *phys, const char *bustype)
{
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "SourceDevicePaths", source);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_EVENT, "UniqueId",
                           unique_id);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_EVENT, "PhysPath", phys);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_EVENT, "IdBustype",
                           bustype);
}

static void
test_source_iface_for_path(void **state)
{
    (void)state;
    assert_int_equal(cbx_source_iface_for_path(
        "/org/shadowblip/InputPlumber/devices/source/hidraw0"),
        CBX_SOURCE_IFACE_HIDRAW);
    assert_int_equal(cbx_source_iface_for_path(
        "/org/shadowblip/InputPlumber/devices/source/event3"),
        CBX_SOURCE_IFACE_EVDEV);
    assert_int_equal(cbx_source_iface_for_path(
        "/org/shadowblip/InputPlumber/devices/source/iio:device0"),
        CBX_SOURCE_IFACE_EVDEV);
    assert_int_equal(cbx_source_iface_for_path(NULL), CBX_SOURCE_IFACE_EVDEV);
}

static void
test_extract_usb_serial(void **state)
{
    ci_fixture *f = FIX(state);
    expect_evdev(f,
        "/org/shadowblip/InputPlumber/devices/source/event0",
        "SN12345", "", "3");

    cbx_identity ident;
    cbx_composite_identity_status status = CBX_COMPOSITE_IDENTITY_OK;
    int rc = cbx_composite_identity_extract(f->backend, f->mock.bus,
                                            COMP_PATH, 0, &ident, &status);
    assert_int_equal(rc, 0);
    assert_int_equal(status, CBX_COMPOSITE_IDENTITY_OK);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_USB_SERIAL);
    assert_string_equal(ident.id, "USB:SN12345");
}

static void
test_extract_bt_mac(void **state)
{
    ci_fixture *f = FIX(state);
    expect_evdev(f,
        "/org/shadowblip/InputPlumber/devices/source/event0",
        "ab:cd:01:ef:23:45", "", "5");

    cbx_identity ident;
    int rc = cbx_composite_identity_extract(f->backend, f->mock.bus,
                                            COMP_PATH, 0, &ident, NULL);
    assert_int_equal(rc, 0);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_BT_MAC);
    assert_string_equal(ident.id, "BT:AB:CD:01:EF:23:45");
}

static void
test_extract_bt_mac_hex_bustype(void **state)
{
    ci_fixture *f = FIX(state);
    /* InputPlumber may report IdBustype as "0x0005". */
    expect_evdev(f,
        "/org/shadowblip/InputPlumber/devices/source/event0",
        "ab:cd:01:ef:23:45", "", "0x0005");

    cbx_identity ident;
    int rc = cbx_composite_identity_extract(f->backend, f->mock.bus,
                                            COMP_PATH, 0, &ident, NULL);
    assert_int_equal(rc, 0);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_BT_MAC);
}

static void
test_extract_phys_when_no_serial(void **state)
{
    ci_fixture *f = FIX(state);
    expect_evdev(f,
        "/org/shadowblip/InputPlumber/devices/source/event0",
        "", "usb-3-2", "3");

    cbx_identity ident;
    int rc = cbx_composite_identity_extract(f->backend, f->mock.bus,
                                            COMP_PATH, 4, &ident, NULL);
    assert_int_equal(rc, 0);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_USB_PORT);
    assert_string_equal(ident.id, "USB:phys:usb-3-2");
}

static void
test_extract_udev_serial(void **state)
{
    ci_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "SourceDevicePaths",
        "/org/shadowblip/InputPlumber/devices/source/iio:device0");
    /* EventDevice is confirmed absent (UnknownInterface), so the UdevDevice
     * interface must be probed. */
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_SOURCE_EVENT, "UniqueId",
                              IP_ERR_UNKNOWN_INTERFACE);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_SOURCE_EVENT, "PhysPath",
                              IP_ERR_UNKNOWN_INTERFACE);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_SOURCE_EVENT, "IdBustype",
                              IP_ERR_UNKNOWN_INTERFACE);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_UDEV, "UniqueId",
                           "SN-UDEV-1");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_UDEV, "PhysPath", "");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_UDEV, "IdBustype", "3");

    cbx_identity ident;
    int rc = cbx_composite_identity_extract(f->backend, f->mock.bus,
                                            COMP_PATH, 0, &ident, NULL);
    assert_int_equal(rc, 0);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_USB_SERIAL);
    assert_string_equal(ident.id, "USB:SN-UDEV-1");
}

/*
 * A transient EventDevice failure is NOT an absent interface.  When
 * EventDevice exists but its properties cannot be read, borrowing a valid
 * UdevDevice identity would conceal the uncertain snapshot and could bind
 * the composite to the wrong saved assignment.  The result must remain
 * uncertain (QUERY_FAILED) with the ORDER fallback, even though UdevDevice
 * would answer.
 */
static void
test_extract_udev_not_used_on_transient_event_failure(void **state)
{
    ci_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "SourceDevicePaths",
        "/org/shadowblip/InputPlumber/devices/source/event0");
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_SOURCE_EVENT, "UniqueId",
                              -EIO);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_SOURCE_EVENT, "PhysPath",
                              -EIO);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_SOURCE_EVENT, "IdBustype",
                              -EIO);
    /* A valid UdevDevice identity must not be used to paper over the
     * EventDevice failure. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_UDEV, "UniqueId",
                           "SN-UDEV-1");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_UDEV, "PhysPath", "");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_UDEV, "IdBustype", "3");

    cbx_identity ident;
    cbx_composite_identity_status status = CBX_COMPOSITE_IDENTITY_OK;
    int rc = cbx_composite_identity_extract(f->backend, f->mock.bus,
                                            COMP_PATH, 5, &ident, &status);
    assert_int_equal(rc, 0);
    assert_int_equal(status, CBX_COMPOSITE_IDENTITY_QUERY_FAILED);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_ORDER);
    assert_string_equal(ident.id, "ORDER:5");
    /* The UdevDevice probe was never issued. */
    assert_int_equal(ip_dbus_mock_call_count(&f->mock, IP_IFACE_SOURCE_UDEV,
                                             "UniqueId"), 0);
}

/*
 * Both evdev interfaces confirmed absent for a path that names an evdev
 * source is an inconsistent snapshot: neither borrowed nor weak-OK, it must
 * stay uncertain so no saved preference is erased or mismatched.
 */
static void
test_extract_both_evdev_interfaces_absent_is_uncertain(void **state)
{
    ci_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "SourceDevicePaths",
        "/org/shadowblip/InputPlumber/devices/source/event0");
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_SOURCE_EVENT, "UniqueId",
                              IP_ERR_UNKNOWN_INTERFACE);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_SOURCE_EVENT, "PhysPath",
                              IP_ERR_UNKNOWN_INTERFACE);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_SOURCE_EVENT, "IdBustype",
                              IP_ERR_UNKNOWN_INTERFACE);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_SOURCE_UDEV, "UniqueId",
                              IP_ERR_UNKNOWN_INTERFACE);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_SOURCE_UDEV, "PhysPath",
                              IP_ERR_UNKNOWN_INTERFACE);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_SOURCE_UDEV, "IdBustype",
                              IP_ERR_UNKNOWN_INTERFACE);

    cbx_identity ident;
    cbx_composite_identity_status status = CBX_COMPOSITE_IDENTITY_OK;
    int rc = cbx_composite_identity_extract(f->backend, f->mock.bus,
                                            COMP_PATH, 5, &ident, &status);
    assert_int_equal(rc, 0);
    assert_int_equal(status, CBX_COMPOSITE_IDENTITY_QUERY_FAILED);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_ORDER);
    assert_string_equal(ident.id, "ORDER:5");
}

static void
test_extract_hidraw_serial(void **state)
{
    ci_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "SourceDevicePaths",
        "/org/shadowblip/InputPlumber/devices/source/hidraw0");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_HIDRAW,
                           "SerialNumber", "SN-HID-9");

    cbx_identity ident;
    int rc = cbx_composite_identity_extract(f->backend, f->mock.bus,
                                            COMP_PATH, 0, &ident, NULL);
    assert_int_equal(rc, 0);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_USB_SERIAL);
    assert_string_equal(ident.id, "USB:SN-HID-9");
}

/*
 * InputPlumber's SourceDevicePaths property reports physical device nodes
 * (e.g. "/dev/input/event3"), while the identification properties are
 * exposed on the DBus source object at
 * /org/shadowblip/InputPlumber/devices/source/<sysname>.  The extraction
 * path must map the device node to that object path before querying; a
 * device node passed straight through as an object path would fail against
 * the real service.  This asserts the derived object path, not merely that
 * some property read succeeded.
 */
static void
test_extract_device_node_source_path(void **state)
{
    ci_fixture *f = FIX(state);
    expect_evdev(f, "/dev/input/event3", "SN12345", "", "3");

    cbx_identity ident;
    cbx_composite_identity_status status = CBX_COMPOSITE_IDENTITY_OK;
    int rc = cbx_composite_identity_extract(f->backend, f->mock.bus,
                                            COMP_PATH, 0, &ident, &status);
    assert_int_equal(rc, 0);
    assert_int_equal(status, CBX_COMPOSITE_IDENTITY_OK);
    assert_string_equal(ident.id, "USB:SN12345");
    assert_string_equal(f->mock.last_get_property_path,
                        "/org/shadowblip/InputPlumber/devices/source/event3");
}

/* The same device-node mapping applies to HIDRaw sources. */
static void
test_extract_hidraw_device_node_source_path(void **state)
{
    ci_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "SourceDevicePaths", "/dev/hidraw2");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_HIDRAW,
                           "SerialNumber", "SN-HID-2");

    cbx_identity ident;
    int rc = cbx_composite_identity_extract(f->backend, f->mock.bus,
                                            COMP_PATH, 0, &ident, NULL);
    assert_int_equal(rc, 0);
    assert_string_equal(ident.id, "USB:SN-HID-2");
    assert_string_equal(f->mock.last_get_property_path,
                        "/org/shadowblip/InputPlumber/devices/source/hidraw2");
}

/*
 * Non-evdev/udev source subsystems (iio, leds, tty) register their DBus
 * source object under a sanitized sysname (InputPlumber replaces ':', '-'
 * and '.' with '_').  A composite commonly includes an IMU source, so its
 * path must resolve to the registered object (UdevDevice answers there)
 * instead of being misread as an uncertain evdev source.
 */
static void
test_extract_iio_device_node_source_path(void **state)
{
    ci_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
        "SourceDevicePaths",
        "/sys/bus/iio/devices/iio:device0");
    /* EventDevice is absent on an IIO object; fall back to UdevDevice. */
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_SOURCE_EVENT, "UniqueId",
                              IP_ERR_UNKNOWN_INTERFACE);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_SOURCE_EVENT, "PhysPath",
                              IP_ERR_UNKNOWN_INTERFACE);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_SOURCE_EVENT, "IdBustype",
                              IP_ERR_UNKNOWN_INTERFACE);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_UDEV, "UniqueId", "");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_UDEV, "PhysPath", "");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_UDEV, "IdBustype", "0");

    cbx_identity ident;
    cbx_composite_identity_status status = CBX_COMPOSITE_IDENTITY_OK;
    int rc = cbx_composite_identity_extract(f->backend, f->mock.bus,
                                            COMP_PATH, 4, &ident, &status);
    assert_int_equal(rc, 0);
    assert_int_equal(status, CBX_COMPOSITE_IDENTITY_OK);
    assert_string_equal(ident.id, "ORDER:4");
    assert_string_equal(f->mock.last_get_property_path,
                        "/org/shadowblip/InputPlumber/devices/source/iio_device0");
}

static void
test_extract_order_fallback_absent(void **state)
{
    ci_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "SourceDevicePaths", "");

    cbx_identity ident;
    cbx_composite_identity_status status = CBX_COMPOSITE_IDENTITY_OK;
    int rc = cbx_composite_identity_extract(f->backend, f->mock.bus,
                                            COMP_PATH, 2, &ident, &status);
    assert_int_equal(rc, 0);
    assert_int_equal(status, CBX_COMPOSITE_IDENTITY_ABSENT);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_ORDER);
    assert_string_equal(ident.id, "ORDER:2");
}

static void
test_extract_malformed_nonempty_source_list_is_uncertain(void **state)
{
    ci_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "SourceDevicePaths", " ,");

    cbx_identity ident;
    cbx_composite_identity_status status = CBX_COMPOSITE_IDENTITY_OK;
    assert_int_equal(cbx_composite_identity_extract(f->backend, f->mock.bus,
        COMP_PATH, 2, &ident, &status), 0);
    assert_int_equal(status, CBX_COMPOSITE_IDENTITY_QUERY_FAILED);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_ORDER);
}

static void
test_extract_query_failure_is_uncertain(void **state)
{
    ci_fixture *f = FIX(state);
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                              "SourceDevicePaths", -EIO);

    cbx_identity ident;
    cbx_composite_identity_status status = CBX_COMPOSITE_IDENTITY_OK;
    int rc = cbx_composite_identity_extract(f->backend, f->mock.bus,
                                            COMP_PATH, 7, &ident, &status);
    /* ORDER fallback is still returned for display continuity, but the
     * status records that the query failed. */
    assert_int_equal(rc, 0);
    assert_int_equal(status, CBX_COMPOSITE_IDENTITY_QUERY_FAILED);
    assert_string_equal(ident.id, "ORDER:7");
}

/* A failed EventDevice property read is not an absent interface.  Even if
 * UdevDevice would provide a plausible identity, retrying it could hide a
 * transient read and bind the wrong saved preference. */
static void
test_extract_property_failure_does_not_fallback(void **state)
{
    ci_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "SourceDevicePaths",
                           "/org/shadowblip/InputPlumber/devices/source/event0");
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_SOURCE_EVENT,
                              "UniqueId", -EIO);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_UDEV,
                           "UniqueId", "SHOULD-NOT-BE-USED");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_UDEV,
                           "PhysPath", "");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_UDEV,
                           "IdBustype", "3");

    cbx_identity ident;
    cbx_composite_identity_status status = CBX_COMPOSITE_IDENTITY_OK;
    assert_int_equal(cbx_composite_identity_extract(f->backend, f->mock.bus,
        COMP_PATH, 4, &ident, &status), 0);
    assert_int_equal(status, CBX_COMPOSITE_IDENTITY_QUERY_FAILED);
    assert_string_equal(ident.id, "ORDER:4");
    assert_int_equal(ip_dbus_mock_call_count(&f->mock, IP_IFACE_SOURCE_UDEV,
                                             "UniqueId"), 0);
}

static void
test_extract_source_with_empty_props_is_confirmed_weak(void **state)
{
    ci_fixture *f = FIX(state);
    /* Source present, all reads succeed but carry no stable identifier: a
     * confirmed weak identity, distinguishable from a transient failure. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "SourceDevicePaths",
                           "/org/shadowblip/InputPlumber/devices/source/event0");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_EVENT, "UniqueId", "");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_EVENT, "PhysPath", "");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_SOURCE_EVENT, "IdBustype", "");

    cbx_identity ident;
    cbx_composite_identity_status status = CBX_COMPOSITE_IDENTITY_OK;
    int rc = cbx_composite_identity_extract(f->backend, f->mock.bus,
                                            COMP_PATH, 3, &ident, &status);
    assert_int_equal(rc, 0);
    assert_int_equal(status, CBX_COMPOSITE_IDENTITY_OK);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_ORDER);
    assert_string_equal(ident.id, "ORDER:3");
}

/*
 * The source list reads, but every per-source property read fails
 * transiently (e.g. the source vanished mid-enumeration).  That is not a
 * confirmed weak identity: the ORDER fallback is returned for display, but
 * the status must be QUERY_FAILED so matchers never bind it to a saved
 * ORDER:n preference and reroute a different controller.
 */
static void
test_extract_per_source_read_failure_is_uncertain(void **state)
{
    ci_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "SourceDevicePaths",
                           "/org/shadowblip/InputPlumber/devices/source/event0");

    cbx_identity ident;
    cbx_composite_identity_status status = CBX_COMPOSITE_IDENTITY_OK;
    int rc = cbx_composite_identity_extract(f->backend, f->mock.bus,
                                            COMP_PATH, 7, &ident, &status);
    assert_int_equal(rc, 0);
    assert_int_equal(status, CBX_COMPOSITE_IDENTITY_QUERY_FAILED);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_ORDER);
    assert_string_equal(ident.id, "ORDER:7");
}

static void
test_extract_no_order_no_identity(void **state)
{
    ci_fixture *f = FIX(state);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "SourceDevicePaths", "");

    cbx_identity ident;
    int rc = cbx_composite_identity_extract(f->backend, f->mock.bus,
                                            COMP_PATH, -1, &ident, NULL);
    assert_int_equal(rc, -ENOENT);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_NONE);
}

static void
test_extract_null_args(void **state)
{
    ci_fixture *f = FIX(state);
    cbx_identity ident;
    assert_int_equal(cbx_composite_identity_extract(NULL, f->mock.bus,
        COMP_PATH, 0, &ident, NULL), -EINVAL);
    assert_int_equal(cbx_composite_identity_extract(f->backend, f->mock.bus,
        NULL, 0, &ident, NULL), -EINVAL);
    assert_int_equal(cbx_composite_identity_extract(f->backend, f->mock.bus,
        COMP_PATH, 0, NULL, NULL), -EINVAL);
}

static void
test_model_extract_identities(void **state)
{
    ci_fixture *f = FIX(state);
    cbx_device_model model;
    cbx_device_model_init(&model);
    assert_true(cbx_device_model_add_composite(&model,
        "/org/shadowblip/InputPlumber/CompositeDevice0"));
    assert_true(cbx_device_model_add_composite(&model,
        "/org/shadowblip/InputPlumber/CompositeDevice1"));

    /* Mock returns the same source for every composite. */
    expect_evdev(f,
        "/org/shadowblip/InputPlumber/devices/source/event0",
        "SN12345", "", "3");

    cbx_composite_identity_entry entries[CBX_MAX_COMPOSITES];
    int count = 0;
    int rc = cbx_model_extract_identities(f->backend, f->mock.bus, &model,
                                          entries, &count);
    assert_int_equal(rc, 0);
    assert_int_equal(count, 2);
    assert_string_equal(entries[0].path,
        "/org/shadowblip/InputPlumber/CompositeDevice0");
    assert_string_equal(entries[0].ident.id, "USB:SN12345");
    assert_string_equal(entries[1].ident.id, "USB:SN12345");
}

/* --- Shared identity resolver (single match rule) ------------------------ */

static void
test_resolve_id_unique_match(void **state)
{
    (void)state;
    cbx_composite_identity_entry entries[2] = {
        {.path = "/c0", .ident = {.id = "USB:SN0",
            .layer = CBX_IDENTITY_LAYER_USB_SERIAL}},
        {.path = "/c1", .ident = {.id = "USB:SN1",
            .layer = CBX_IDENTITY_LAYER_USB_SERIAL}},
    };
    char path[CBX_MAX_PATH_LEN];
    int index = -1;
    assert_int_equal(cbx_composite_identity_resolve_id(entries, 2, "USB:SN1",
                     path, sizeof(path), &index), 1);
    assert_string_equal(path, "/c1");
    assert_int_equal(index, 1);
}

static void
test_resolve_id_no_match_is_stale(void **state)
{
    (void)state;
    cbx_composite_identity_entry entries[1] = {
        {.path = "/c0", .ident = {.id = "USB:SN0",
            .layer = CBX_IDENTITY_LAYER_USB_SERIAL}},
    };
    char path[CBX_MAX_PATH_LEN] = "keep";
    int index = 7;
    assert_int_equal(cbx_composite_identity_resolve_id(entries, 1, "USB:SN9",
                     path, sizeof(path), &index), 0);
    assert_string_equal(path, "");
    assert_int_equal(index, -1);
}

static void
test_resolve_id_duplicate_is_uncertain(void **state)
{
    (void)state;
    cbx_composite_identity_entry entries[2] = {
        {.path = "/c0", .ident = {.id = "USB:SN0",
            .layer = CBX_IDENTITY_LAYER_USB_SERIAL}},
        {.path = "/c1", .ident = {.id = "USB:SN0",
            .layer = CBX_IDENTITY_LAYER_USB_SERIAL}},
    };
    char path[CBX_MAX_PATH_LEN];
    assert_int_equal(cbx_composite_identity_resolve_id(entries, 2, "USB:SN0",
                     path, sizeof(path), NULL), -1);
}

static void
test_resolve_id_query_failure_is_uncertain(void **state)
{
    (void)state;
    cbx_composite_identity_entry entries[2] = {
        {.path = "/c0", .ident = {.id = "USB:SN0",
            .layer = CBX_IDENTITY_LAYER_USB_SERIAL}},
        {.path = "/c1", .status = CBX_COMPOSITE_IDENTITY_QUERY_FAILED},
    };
    char path[CBX_MAX_PATH_LEN];
    assert_int_equal(cbx_composite_identity_resolve_id(entries, 2, "USB:SN0",
                     path, sizeof(path), NULL), -1);
}

static void
test_resolve_id_null_args(void **state)
{
    (void)state;
    cbx_composite_identity_entry entries[1] = {
        {.path = "/c0", .ident = {.id = "USB:SN0",
            .layer = CBX_IDENTITY_LAYER_USB_SERIAL}},
    };
    char path[CBX_MAX_PATH_LEN];
    assert_int_equal(cbx_composite_identity_resolve_id(NULL, 1, "USB:SN0",
                     path, sizeof(path), NULL), -1);
    assert_int_equal(cbx_composite_identity_resolve_id(entries, 1, NULL,
                     path, sizeof(path), NULL), -1);
    assert_int_equal(cbx_composite_identity_resolve_id(entries, 1, "",
                     path, sizeof(path), NULL), -1);
    /* A NULL output path is allowed: callers that only need the verdict. */
    assert_int_equal(cbx_composite_identity_resolve_id(entries, 1, "USB:SN0",
                     NULL, 0, NULL), 1);
}

int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_source_iface_for_path,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_extract_usb_serial,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_extract_bt_mac,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_extract_bt_mac_hex_bustype,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_extract_phys_when_no_serial,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_extract_hidraw_serial,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_extract_device_node_source_path,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(
            test_extract_hidraw_device_node_source_path,
            setup, teardown),
        cmocka_unit_test_setup_teardown(
            test_extract_iio_device_node_source_path,
            setup, teardown),
        cmocka_unit_test_setup_teardown(test_extract_udev_serial,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(
            test_extract_udev_not_used_on_transient_event_failure,
            setup, teardown),
        cmocka_unit_test_setup_teardown(
            test_extract_both_evdev_interfaces_absent_is_uncertain,
            setup, teardown),
        cmocka_unit_test_setup_teardown(test_extract_order_fallback_absent,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(
            test_extract_malformed_nonempty_source_list_is_uncertain,
            setup, teardown),
        cmocka_unit_test_setup_teardown(test_extract_query_failure_is_uncertain,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_extract_property_failure_does_not_fallback,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_extract_source_with_empty_props_is_confirmed_weak,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_extract_per_source_read_failure_is_uncertain,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_extract_no_order_no_identity,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_extract_null_args,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_model_extract_identities,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_resolve_id_unique_match,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_resolve_id_no_match_is_stale,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(
            test_resolve_id_duplicate_is_uncertain, setup, teardown),
        cmocka_unit_test_setup_teardown(
            test_resolve_id_query_failure_is_uncertain, setup, teardown),
        cmocka_unit_test_setup_teardown(test_resolve_id_null_args,
                                         setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
