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
    /* No EventDevice expectations: the UdevDevice interface must be probed. */
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
        cmocka_unit_test_setup_teardown(test_extract_udev_serial,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_extract_order_fallback_absent,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_extract_query_failure_is_uncertain,
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
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
