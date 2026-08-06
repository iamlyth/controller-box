/*
 * test_identity.c — Unit tests for identity extraction (Task 25, SPEC §6.2–6.3).
 *
 * Tests all 4 identity layers, edge cases (empty uniq for BT, HIDRaw
 * serial fallback, MAC-like strings on USB bus, invalid characters),
 * parse_layer, downgrade detection, and helper functions.
 */
#include "identify/identity.h"

#include <errno.h>
#include <string.h>

#include <cmocka.h>

/* --- Helpers ------------------------------------------------------------- */

/* Build source props with sensible defaults. */
static cbx_source_props
make_props(cbx_source_iface iface,
           const char *unique_id,
           const char *phys_path,
           const char *serial_number,
           const char *id_bustype)
{
    cbx_source_props p;
    p.iface = iface;
    p.unique_id = unique_id;
    p.phys_path = phys_path;
    p.serial_number = serial_number;
    p.id_bustype = id_bustype;
    return p;
}

/* --- Init tests ---------------------------------------------------------- */

static void
test_identity_init(void **state)
{
    (void)state;
    cbx_identity ident;
    cbx_identity_init(&ident);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_NONE);
    assert_string_equal(ident.id, "");
}

static void
test_identity_init_null(void **state)
{
    (void)state;
    /* Should not crash */
    cbx_identity_init(NULL);
}

/* --- MAC address helper tests ------------------------------------------- */

static void
test_is_mac_valid(void **state)
{
    (void)state;
    assert_true(cbx_identity_is_mac_address("AB:CD:01:EF:23:45"));
    assert_true(cbx_identity_is_mac_address("ab:cd:01:ef:23:45"));
    assert_true(cbx_identity_is_mac_address("01:23:45:67:89:AB"));
}

static void
test_is_mac_invalid(void **state)
{
    (void)state;
    assert_false(cbx_identity_is_mac_address(NULL));
    assert_false(cbx_identity_is_mac_address(""));
    assert_false(cbx_identity_is_mac_address("AB:CD:01:EF:23"));      /* 5 octets */
    assert_false(cbx_identity_is_mac_address("AB:CD:01:EF:23:45:67")); /* 7 octets */
    assert_false(cbx_identity_is_mac_address("AB-CD-01-EF-23-45"));    /* dashes */
    assert_false(cbx_identity_is_mac_address("ZZ:CD:01:EF:23:45"));    /* non-hex */
    assert_false(cbx_identity_is_mac_address("AB:CD:01:EF:23:4"));     /* short octet */
    assert_false(cbx_identity_is_mac_address("SN12345"));             /* not MAC */
    assert_false(cbx_identity_is_mac_address("usb-3-2"));              /* not MAC */
}

/* --- Bustype parser tests ------------------------------------------------ */

static void
test_parse_bustype_valid(void **state)
{
    (void)state;
    assert_int_equal(cbx_identity_parse_bustype("3"), 3);
    assert_int_equal(cbx_identity_parse_bustype("5"), 5);
    assert_int_equal(cbx_identity_parse_bustype("0"), 0);
    assert_int_equal(cbx_identity_parse_bustype("65535"), 65535);
}

static void
test_parse_bustype_invalid(void **state)
{
    (void)state;
    assert_int_equal(cbx_identity_parse_bustype(NULL), -1);
    assert_int_equal(cbx_identity_parse_bustype(""), -1);
    assert_int_equal(cbx_identity_parse_bustype("abc"), -1);
    assert_int_equal(cbx_identity_parse_bustype("3.5"), -1);
    assert_int_equal(cbx_identity_parse_bustype("-1"), -1);
    assert_int_equal(cbx_identity_parse_bustype("70000"), -1); /* > 0xFFFF */
}

/* --- Layer 1: Bluetooth MAC tests ---------------------------------------- */

static void
test_extract_bt_mac(void **state)
{
    (void)state;
    cbx_source_props p = make_props(CBX_SOURCE_IFACE_EVDEV,
                                     "ab:cd:01:ef:23:45",
                                     NULL, NULL, "5");
    cbx_identity ident;
    int rc = cbx_identity_extract(&p, 0, &ident);
    assert_int_equal(rc, 0);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_BT_MAC);
    /* MAC is uppercased */
    assert_string_equal(ident.id, "BT:AB:CD:01:EF:23:45");
}

static void
test_extract_bt_mac_uppercase_input(void **state)
{
    (void)state;
    cbx_source_props p = make_props(CBX_SOURCE_IFACE_EVDEV,
                                     "AB:CD:01:EF:23:45",
                                     NULL, NULL, "5");
    cbx_identity ident;
    int rc = cbx_identity_extract(&p, 0, &ident);
    assert_int_equal(rc, 0);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_BT_MAC);
    assert_string_equal(ident.id, "BT:AB:CD:01:EF:23:45");
}

static void
test_extract_bt_mac_hidraw_fallback(void **state)
{
    (void)state;
    /* BT device with empty uniq, serial_number available as MAC */
    cbx_source_props p = make_props(CBX_SOURCE_IFACE_EVDEV,
                                     NULL, NULL,
                                     "ab:cd:01:ef:23:45", "5");
    cbx_identity ident;
    int rc = cbx_identity_extract(&p, 0, &ident);
    assert_int_equal(rc, 0);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_BT_MAC);
    assert_string_equal(ident.id, "BT:AB:CD:01:EF:23:45");
}

static void
test_extract_bt_empty_uniq_falls_through(void **state)
{
    (void)state;
    /* BT device with empty uniq and no serial — falls to phys or order */
    cbx_source_props p = make_props(CBX_SOURCE_IFACE_EVDEV,
                                     NULL,
                                     "usb-3-2", NULL, "5");
    cbx_identity ident;
    int rc = cbx_identity_extract(&p, 2, &ident);
    assert_int_equal(rc, 0);
    /* No MAC available, no serial → falls to phys (layer 3) */
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_USB_PORT);
    assert_string_equal(ident.id, "USB:phys:usb-3-2");
}

static void
test_extract_bt_invalid_mac_falls_through(void **state)
{
    (void)state;
    /* BT bus type but unique_id is not a MAC — falls through to serial */
    cbx_source_props p = make_props(CBX_SOURCE_IFACE_EVDEV,
                                     "SN12345",
                                     "usb-3-2", NULL, "5");
    cbx_identity ident;
    int rc = cbx_identity_extract(&p, 2, &ident);
    assert_int_equal(rc, 0);
    /* Not a MAC → treated as USB serial (layer 2) */
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_USB_SERIAL);
    assert_string_equal(ident.id, "USB:SN12345");
}

/* --- Layer 2: USB serial tests ------------------------------------------- */

static void
test_extract_usb_serial_evdev(void **state)
{
    (void)state;
    cbx_source_props p = make_props(CBX_SOURCE_IFACE_EVDEV,
                                     "SN12345", NULL, NULL, "3");
    cbx_identity ident;
    int rc = cbx_identity_extract(&p, 0, &ident);
    assert_int_equal(rc, 0);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_USB_SERIAL);
    assert_string_equal(ident.id, "USB:SN12345");
}

static void
test_extract_usb_serial_hidraw(void **state)
{
    (void)state;
    /* HIDRaw device: serial from SerialNumber property */
    cbx_source_props p = make_props(CBX_SOURCE_IFACE_HIDRAW,
                                     NULL, NULL,
                                     "SN67890", "3");
    cbx_identity ident;
    int rc = cbx_identity_extract(&p, 0, &ident);
    assert_int_equal(rc, 0);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_USB_SERIAL);
    assert_string_equal(ident.id, "USB:SN67890");
}

static void
test_extract_usb_serial_hidraw_fallback_to_unique_id(void **state)
{
    (void)state;
    /* HIDRaw device with empty SerialNumber, fallback to UniqueId */
    cbx_source_props p = make_props(CBX_SOURCE_IFACE_HIDRAW,
                                     "SN99999", NULL,
                                     "", "3");
    cbx_identity ident;
    int rc = cbx_identity_extract(&p, 0, &ident);
    assert_int_equal(rc, 0);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_USB_SERIAL);
    assert_string_equal(ident.id, "USB:SN99999");
}

static void
test_extract_usb_serial_evdev_fallback_to_hidraw(void **state)
{
    (void)state;
    /* evdev device with empty UniqueId, fallback to HIDRaw serial */
    cbx_source_props p = make_props(CBX_SOURCE_IFACE_EVDEV,
                                     "", NULL,
                                     "SN42424", "3");
    cbx_identity ident;
    int rc = cbx_identity_extract(&p, 0, &ident);
    assert_int_equal(rc, 0);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_USB_SERIAL);
    assert_string_equal(ident.id, "USB:SN42424");
}

static void
test_extract_usb_serial_with_underscores_dashes(void **state)
{
    (void)state;
    cbx_source_props p = make_props(CBX_SOURCE_IFACE_EVDEV,
                                     "SN_12-34", NULL, NULL, "3");
    cbx_identity ident;
    int rc = cbx_identity_extract(&p, 0, &ident);
    assert_int_equal(rc, 0);
    assert_string_equal(ident.id, "USB:SN_12-34");
}

static void
test_extract_usb_serial_invalid_chars_falls_through(void **state)
{
    (void)state;
    /* Serial with spaces/special chars → invalid serial, falls to phys */
    cbx_source_props p = make_props(CBX_SOURCE_IFACE_EVDEV,
                                     "SN 123 45",
                                     "usb-3-2", NULL, "3");
    cbx_identity ident;
    int rc = cbx_identity_extract(&p, 2, &ident);
    assert_int_equal(rc, 0);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_USB_PORT);
    assert_string_equal(ident.id, "USB:phys:usb-3-2");
}

static void
test_extract_usb_serial_too_long_falls_through(void **state)
{
    (void)state;
    /* Serial longer than max → falls through to phys */
    char long_serial[200];
    memset(long_serial, 'A', sizeof(long_serial) - 1);
    long_serial[sizeof(long_serial) - 1] = '\0';
    cbx_source_props p = make_props(CBX_SOURCE_IFACE_EVDEV,
                                     long_serial,
                                     "usb-3-2", NULL, "3");
    cbx_identity ident;
    int rc = cbx_identity_extract(&p, 2, &ident);
    assert_int_equal(rc, 0);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_USB_PORT);
}

static void
test_extract_mac_on_usb_bus_skips_to_phys(void **state)
{
    (void)state;
    /* USB bus type but unique_id is MAC-like — can't be USB serial (colons),
     * falls through to phys path */
    cbx_source_props p = make_props(CBX_SOURCE_IFACE_EVDEV,
                                     "AB:CD:01:EF:23:45",
                                     "usb-3-2", NULL, "3");
    cbx_identity ident;
    int rc = cbx_identity_extract(&p, 2, &ident);
    assert_int_equal(rc, 0);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_USB_PORT);
    assert_string_equal(ident.id, "USB:phys:usb-3-2");
}

/* --- Layer 3: USB port path tests ---------------------------------------- */

static void
test_extract_usb_phys(void **state)
{
    (void)state;
    /* No serial, has phys path */
    cbx_source_props p = make_props(CBX_SOURCE_IFACE_EVDEV,
                                     NULL, "usb-3-2", NULL, "3");
    cbx_identity ident;
    int rc = cbx_identity_extract(&p, 0, &ident);
    assert_int_equal(rc, 0);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_USB_PORT);
    assert_string_equal(ident.id, "USB:phys:usb-3-2");
}

static void
test_extract_usb_phys_complex(void **state)
{
    (void)state;
    cbx_source_props p = make_props(CBX_SOURCE_IFACE_EVDEV,
                                     NULL,
                                     "usb-1-3.2:1.0", NULL, "3");
    cbx_identity ident;
    int rc = cbx_identity_extract(&p, 0, &ident);
    assert_int_equal(rc, 0);
    assert_string_equal(ident.id, "USB:phys:usb-1-3.2:1.0");
}

static void
test_extract_usb_phys_empty_falls_through(void **state)
{
    (void)state;
    /* Empty phys → falls to order */
    cbx_source_props p = make_props(CBX_SOURCE_IFACE_EVDEV,
                                     NULL, "", NULL, "3");
    cbx_identity ident;
    int rc = cbx_identity_extract(&p, 3, &ident);
    assert_int_equal(rc, 0);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_ORDER);
    assert_string_equal(ident.id, "ORDER:3");
}

/* --- Layer 4: Connection order tests ------------------------------------- */

static void
test_extract_order(void **state)
{
    (void)state;
    cbx_source_props p = make_props(CBX_SOURCE_IFACE_EVDEV,
                                     NULL, NULL, NULL, "3");
    cbx_identity ident;
    int rc = cbx_identity_extract(&p, 7, &ident);
    assert_int_equal(rc, 0);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_ORDER);
    assert_string_equal(ident.id, "ORDER:7");
}

static void
test_extract_order_zero(void **state)
{
    (void)state;
    cbx_source_props p = make_props(CBX_SOURCE_IFACE_EVDEV,
                                     NULL, NULL, NULL, NULL);
    cbx_identity ident;
    int rc = cbx_identity_extract(&p, 0, &ident);
    assert_int_equal(rc, 0);
    assert_string_equal(ident.id, "ORDER:0");
}

static void
test_extract_no_identity(void **state)
{
    (void)state;
    /* No properties, no order */
    cbx_source_props p = make_props(CBX_SOURCE_IFACE_EVDEV,
                                     NULL, NULL, NULL, NULL);
    cbx_identity ident;
    int rc = cbx_identity_extract(&p, -1, &ident);
    assert_int_equal(rc, -ENOENT);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_NONE);
    assert_string_equal(ident.id, "");
}

/* --- Null arg tests ------------------------------------------------------ */

static void
test_extract_null_props(void **state)
{
    (void)state;
    cbx_identity ident;
    int rc = cbx_identity_extract(NULL, 0, &ident);
    assert_int_equal(rc, -EINVAL);
}

static void
test_extract_null_output(void **state)
{
    (void)state;
    cbx_source_props p = make_props(CBX_SOURCE_IFACE_EVDEV,
                                     "SN12345", NULL, NULL, "3");
    int rc = cbx_identity_extract(&p, 0, NULL);
    assert_int_equal(rc, -EINVAL);
}

/* --- Layer precedence tests ---------------------------------------------- */

static void
test_layer_precedence_bt_over_serial(void **state)
{
    (void)state;
    /* BT bus, MAC in unique_id, also has a serial_number → BT wins */
    cbx_source_props p = make_props(CBX_SOURCE_IFACE_EVDEV,
                                     "ab:cd:01:ef:23:45",
                                     NULL,
                                     "SN99999", "5");
    cbx_identity ident;
    int rc = cbx_identity_extract(&p, 0, &ident);
    assert_int_equal(rc, 0);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_BT_MAC);
}

static void
test_layer_precedence_serial_over_phys(void **state)
{
    (void)state;
    /* USB bus, has both serial and phys → serial wins */
    cbx_source_props p = make_props(CBX_SOURCE_IFACE_EVDEV,
                                     "SN12345",
                                     "usb-3-2", NULL, "3");
    cbx_identity ident;
    int rc = cbx_identity_extract(&p, 0, &ident);
    assert_int_equal(rc, 0);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_USB_SERIAL);
}

static void
test_layer_precedence_phys_over_order(void **state)
{
    (void)state;
    /* No serial, has phys and order → phys wins */
    cbx_source_props p = make_props(CBX_SOURCE_IFACE_EVDEV,
                                     NULL,
                                     "usb-3-2", NULL, "3");
    cbx_identity ident;
    int rc = cbx_identity_extract(&p, 5, &ident);
    assert_int_equal(rc, 0);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_USB_PORT);
}

static void
test_layer_precedence_all_present(void **state)
{
    (void)state;
    /* BT bus, MAC, serial, phys, order all present → BT wins */
    cbx_source_props p = make_props(CBX_SOURCE_IFACE_EVDEV,
                                     "ab:cd:01:ef:23:45",
                                     "usb-3-2",
                                     "SN99999", "5");
    cbx_identity ident;
    int rc = cbx_identity_extract(&p, 3, &ident);
    assert_int_equal(rc, 0);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_BT_MAC);
}

/* --- No bustype tests ---------------------------------------------------- */

static void
test_extract_no_bustype_with_serial(void **state)
{
    (void)state;
    /* No bustype but has serial → USB serial (layer 2) */
    cbx_source_props p = make_props(CBX_SOURCE_IFACE_EVDEV,
                                     "SN12345", NULL, NULL, NULL);
    cbx_identity ident;
    int rc = cbx_identity_extract(&p, 0, &ident);
    assert_int_equal(rc, 0);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_USB_SERIAL);
}

static void
test_extract_no_bustype_with_mac(void **state)
{
    (void)state;
    /* No bustype but unique_id is MAC → can't be BT (bus unknown),
     * MAC can't be USB serial (colons), falls to phys */
    cbx_source_props p = make_props(CBX_SOURCE_IFACE_EVDEV,
                                     "ab:cd:01:ef:23:45",
                                     "usb-3-2", NULL, NULL);
    cbx_identity ident;
    int rc = cbx_identity_extract(&p, 2, &ident);
    assert_int_equal(rc, 0);
    assert_int_equal(ident.layer, CBX_IDENTITY_LAYER_USB_PORT);
}

/* --- parse_layer tests --------------------------------------------------- */

static void
test_parse_layer_bt(void **state)
{
    (void)state;
    assert_int_equal(cbx_identity_parse_layer("BT:AB:CD:01:EF:23:45"),
                     CBX_IDENTITY_LAYER_BT_MAC);
    assert_int_equal(cbx_identity_parse_layer("BT:ab:cd:01:ef:23:45"),
                     CBX_IDENTITY_LAYER_BT_MAC);
}

static void
test_parse_layer_usb_serial(void **state)
{
    (void)state;
    assert_int_equal(cbx_identity_parse_layer("USB:SN12345"),
                     CBX_IDENTITY_LAYER_USB_SERIAL);
    assert_int_equal(cbx_identity_parse_layer("USB:SN_12-34"),
                     CBX_IDENTITY_LAYER_USB_SERIAL);
}

static void
test_parse_layer_usb_phys(void **state)
{
    (void)state;
    assert_int_equal(cbx_identity_parse_layer("USB:phys:usb-3-2"),
                     CBX_IDENTITY_LAYER_USB_PORT);
    assert_int_equal(cbx_identity_parse_layer("USB:phys:usb-1-3.2:1.0"),
                     CBX_IDENTITY_LAYER_USB_PORT);
}

static void
test_parse_layer_order(void **state)
{
    (void)state;
    assert_int_equal(cbx_identity_parse_layer("ORDER:0"),
                     CBX_IDENTITY_LAYER_ORDER);
    assert_int_equal(cbx_identity_parse_layer("ORDER:42"),
                     CBX_IDENTITY_LAYER_ORDER);
}

static void
test_parse_layer_invalid(void **state)
{
    (void)state;
    assert_int_equal(cbx_identity_parse_layer(NULL), CBX_IDENTITY_LAYER_NONE);
    assert_int_equal(cbx_identity_parse_layer(""), CBX_IDENTITY_LAYER_NONE);
    assert_int_equal(cbx_identity_parse_layer("BT:"), CBX_IDENTITY_LAYER_NONE);
    assert_int_equal(cbx_identity_parse_layer("USB:"), CBX_IDENTITY_LAYER_NONE);
    assert_int_equal(cbx_identity_parse_layer("USB:phys:"), CBX_IDENTITY_LAYER_NONE);
    assert_int_equal(cbx_identity_parse_layer("ORDER:"), CBX_IDENTITY_LAYER_NONE);
    assert_int_equal(cbx_identity_parse_layer("UNKNOWN:foo"), CBX_IDENTITY_LAYER_NONE);
    assert_int_equal(cbx_identity_parse_layer("BT:AB:CD:01:EF:23"), CBX_IDENTITY_LAYER_NONE);
    assert_int_equal(cbx_identity_parse_layer("USB:SN 123"), CBX_IDENTITY_LAYER_NONE);
    assert_int_equal(cbx_identity_parse_layer("USB:phys:has space"), CBX_IDENTITY_LAYER_NONE);
    assert_int_equal(cbx_identity_parse_layer("ORDER:-1"), CBX_IDENTITY_LAYER_NONE);
    assert_int_equal(cbx_identity_parse_layer("ORDER:abc"), CBX_IDENTITY_LAYER_NONE);
}

static void
test_parse_layer_roundtrip(void **state)
{
    (void)state;
    /* Extract then parse should give the same layer */
    cbx_source_props p = make_props(CBX_SOURCE_IFACE_EVDEV,
                                     "ab:cd:01:ef:23:45",
                                     NULL, NULL, "5");
    cbx_identity ident;
    cbx_identity_extract(&p, 0, &ident);
    assert_int_equal(cbx_identity_parse_layer(ident.id), ident.layer);

    p = make_props(CBX_SOURCE_IFACE_EVDEV, "SN12345", NULL, NULL, "3");
    cbx_identity_extract(&p, 0, &ident);
    assert_int_equal(cbx_identity_parse_layer(ident.id), ident.layer);

    p = make_props(CBX_SOURCE_IFACE_EVDEV, NULL, "usb-3-2", NULL, "3");
    cbx_identity_extract(&p, 0, &ident);
    assert_int_equal(cbx_identity_parse_layer(ident.id), ident.layer);

    p = make_props(CBX_SOURCE_IFACE_EVDEV, NULL, NULL, NULL, NULL);
    cbx_identity_extract(&p, 9, &ident);
    assert_int_equal(cbx_identity_parse_layer(ident.id), ident.layer);
}

/* --- Downgrade detection tests ------------------------------------------- */

static void
test_is_downgrade_yes(void **state)
{
    (void)state;
    /* Layer 1 → 2 is a downgrade */
    assert_true(cbx_identity_is_downgrade(CBX_IDENTITY_LAYER_BT_MAC,
                                          CBX_IDENTITY_LAYER_USB_SERIAL));
    /* Layer 2 → 3 is a downgrade */
    assert_true(cbx_identity_is_downgrade(CBX_IDENTITY_LAYER_USB_SERIAL,
                                          CBX_IDENTITY_LAYER_USB_PORT));
    /* Layer 1 → 4 is a downgrade */
    assert_true(cbx_identity_is_downgrade(CBX_IDENTITY_LAYER_BT_MAC,
                                          CBX_IDENTITY_LAYER_ORDER));
}

static void
test_is_downgrade_no(void **state)
{
    (void)state;
    /* Same layer → not a downgrade */
    assert_false(cbx_identity_is_downgrade(CBX_IDENTITY_LAYER_BT_MAC,
                                           CBX_IDENTITY_LAYER_BT_MAC));
    /* Upgrade (weaker → stronger) → not a downgrade */
    assert_false(cbx_identity_is_downgrade(CBX_IDENTITY_LAYER_ORDER,
                                           CBX_IDENTITY_LAYER_BT_MAC));
    assert_false(cbx_identity_is_downgrade(CBX_IDENTITY_LAYER_USB_PORT,
                                           CBX_IDENTITY_LAYER_USB_SERIAL));
    /* NONE is not a valid comparison */
    assert_false(cbx_identity_is_downgrade(CBX_IDENTITY_LAYER_NONE,
                                           CBX_IDENTITY_LAYER_BT_MAC));
    assert_false(cbx_identity_is_downgrade(CBX_IDENTITY_LAYER_BT_MAC,
                                           CBX_IDENTITY_LAYER_NONE));
}

/* --- Integration: extract + validate with cbx_validate_id ---------------- */

/*
 * Verify that all extracted IDs pass the existing cbx_validate_id validator.
 * This requires linking with config_assignments.  We test via parse_layer
 * which mirrors the same validation logic.
 */

static void
test_extract_and_parse_all_layers(void **state)
{
    (void)state;
    /* Test all 4 layers extract and parse correctly */
    cbx_source_props p;
    cbx_identity ident;

    /* Layer 1: BT */
    p = make_props(CBX_SOURCE_IFACE_EVDEV, "ab:cd:01:ef:23:45", NULL, NULL, "5");
    assert_int_equal(cbx_identity_extract(&p, 0, &ident), 0);
    assert_int_equal(cbx_identity_parse_layer(ident.id), CBX_IDENTITY_LAYER_BT_MAC);

    /* Layer 2: USB serial */
    p = make_props(CBX_SOURCE_IFACE_EVDEV, "SN12345", NULL, NULL, "3");
    assert_int_equal(cbx_identity_extract(&p, 0, &ident), 0);
    assert_int_equal(cbx_identity_parse_layer(ident.id), CBX_IDENTITY_LAYER_USB_SERIAL);

    /* Layer 3: USB phys */
    p = make_props(CBX_SOURCE_IFACE_EVDEV, NULL, "usb-3-2", NULL, "3");
    assert_int_equal(cbx_identity_extract(&p, 0, &ident), 0);
    assert_int_equal(cbx_identity_parse_layer(ident.id), CBX_IDENTITY_LAYER_USB_PORT);

    /* Layer 4: Order */
    p = make_props(CBX_SOURCE_IFACE_EVDEV, NULL, NULL, NULL, NULL);
    assert_int_equal(cbx_identity_extract(&p, 7, &ident), 0);
    assert_int_equal(cbx_identity_parse_layer(ident.id), CBX_IDENTITY_LAYER_ORDER);
}

/* --- Main ---------------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* Init */
        cmocka_unit_test(test_identity_init),
        cmocka_unit_test(test_identity_init_null),

        /* MAC address helper */
        cmocka_unit_test(test_is_mac_valid),
        cmocka_unit_test(test_is_mac_invalid),

        /* Bustype parser */
        cmocka_unit_test(test_parse_bustype_valid),
        cmocka_unit_test(test_parse_bustype_invalid),

        /* Layer 1: BT MAC */
        cmocka_unit_test(test_extract_bt_mac),
        cmocka_unit_test(test_extract_bt_mac_uppercase_input),
        cmocka_unit_test(test_extract_bt_mac_hidraw_fallback),
        cmocka_unit_test(test_extract_bt_empty_uniq_falls_through),
        cmocka_unit_test(test_extract_bt_invalid_mac_falls_through),

        /* Layer 2: USB serial */
        cmocka_unit_test(test_extract_usb_serial_evdev),
        cmocka_unit_test(test_extract_usb_serial_hidraw),
        cmocka_unit_test(test_extract_usb_serial_hidraw_fallback_to_unique_id),
        cmocka_unit_test(test_extract_usb_serial_evdev_fallback_to_hidraw),
        cmocka_unit_test(test_extract_usb_serial_with_underscores_dashes),
        cmocka_unit_test(test_extract_usb_serial_invalid_chars_falls_through),
        cmocka_unit_test(test_extract_usb_serial_too_long_falls_through),
        cmocka_unit_test(test_extract_mac_on_usb_bus_skips_to_phys),

        /* Layer 3: USB port path */
        cmocka_unit_test(test_extract_usb_phys),
        cmocka_unit_test(test_extract_usb_phys_complex),
        cmocka_unit_test(test_extract_usb_phys_empty_falls_through),

        /* Layer 4: Connection order */
        cmocka_unit_test(test_extract_order),
        cmocka_unit_test(test_extract_order_zero),
        cmocka_unit_test(test_extract_no_identity),

        /* Null args */
        cmocka_unit_test(test_extract_null_props),
        cmocka_unit_test(test_extract_null_output),

        /* Layer precedence */
        cmocka_unit_test(test_layer_precedence_bt_over_serial),
        cmocka_unit_test(test_layer_precedence_serial_over_phys),
        cmocka_unit_test(test_layer_precedence_phys_over_order),
        cmocka_unit_test(test_layer_precedence_all_present),

        /* No bustype */
        cmocka_unit_test(test_extract_no_bustype_with_serial),
        cmocka_unit_test(test_extract_no_bustype_with_mac),

        /* parse_layer */
        cmocka_unit_test(test_parse_layer_bt),
        cmocka_unit_test(test_parse_layer_usb_serial),
        cmocka_unit_test(test_parse_layer_usb_phys),
        cmocka_unit_test(test_parse_layer_order),
        cmocka_unit_test(test_parse_layer_invalid),
        cmocka_unit_test(test_parse_layer_roundtrip),

        /* Downgrade detection */
        cmocka_unit_test(test_is_downgrade_yes),
        cmocka_unit_test(test_is_downgrade_no),

        /* Integration */
        cmocka_unit_test(test_extract_and_parse_all_layers),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}