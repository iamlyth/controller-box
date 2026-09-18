/*
 * identity.c — Controller identity extraction (Task 25, SPEC §6.2–6.3).
 *
 * Implements the multi-layered identity extraction described in SPEC §6.2.
 * The caller gathers source device properties (via ip_source_get_* from
 * Task 14) and passes them to cbx_identity_extract(), which selects the
 * strongest available identity layer and formats the prefixed ID string.
 *
 * Linux input subsystem bus type constant (from linux/input.h):
 *   BUS_BLUETOOTH = 0x05
 * Other bus types need no constant here: the layer ladder keys Bluetooth
 * off BUS_BLUETOOTH and treats every other bus by its property values.
 */
#include "identity.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Constants ----------------------------------------------------------- */

/* Linux input subsystem bus types. */
#define BUS_BLUETOOTH 0x05

/* --- Init ---------------------------------------------------------------- */

void
cbx_identity_init(cbx_identity *ident)
{
    if (!ident)
        return;
    memset(ident->id, 0, sizeof(ident->id));
    ident->layer = CBX_IDENTITY_LAYER_NONE;
}

/* --- Helpers ------------------------------------------------------------- */

/*
 * Check if a character is a hex digit.
 */
static bool
is_hex_digit(char c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

bool
cbx_identity_is_mac_address(const char *s)
{
    if (!s || !*s)
        return false;

    /* MAC address format: xx:xx:xx:xx:xx:xx (6 hex octets, colon-separated) */
    const char *p = s;
    for (int i = 0; i < 6; i++) {
        if (!is_hex_digit(p[0]) || !is_hex_digit(p[1]))
            return false;
        p += 2;
        if (i < 5) {
            if (*p != ':')
                return false;
            p++;
        }
    }
    /* Must be end of string */
    return *p == '\0';
}

int
cbx_identity_parse_bustype(const char *bustype_str)
{
    if (!bustype_str || !*bustype_str)
        return -1;

    /* evdev IdBustype is a u16.  InputPlumber has been observed to report it
     * both as a decimal string ("3") and as a hexadecimal one ("0x0003");
     * accept either so a Bluetooth controller is not misclassified as USB
     * merely because of the property's textual encoding. */
    const char *digits = bustype_str;
    int base = 10;
    if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        base = 16;
        digits += 2;
        if (*digits == '\0')
            return -1;
    }

    char *end = NULL;
    long val = strtol(digits, &end, base);
    if (end == digits || *end != '\0' || val < 0 || val > 0xFFFF)
        return -1;
    return (int)val;
}

/*
 * Check if a character is valid in a USB serial string.
 * Same rules as cbx_validate_profile: alnum, underscore, dash.
 */
static bool
is_serial_char(char c)
{
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

/*
 * Format a USB serial ID: USB:SNxxxxx
 * Validates that the serial contains only safe characters.
 * Returns 0 on success, -EINVAL on invalid characters.
 */
static int
format_usb_serial(const char *serial, cbx_identity *out)
{
    if (!serial || !*serial)
        return -EINVAL;

    /* Validate characters */
    for (const char *p = serial; *p; p++) {
        if (!is_serial_char(*p))
            return -EINVAL;
    }

    /* Check length: "USB:" (4) + serial + NUL */
    size_t serial_len = strlen(serial);
    if (serial_len + 5 > CBX_IDENTITY_MAX_LEN)
        return -EINVAL;

    snprintf(out->id, CBX_IDENTITY_MAX_LEN, "USB:%s", serial);
    out->layer = CBX_IDENTITY_LAYER_USB_SERIAL;
    return 0;
}

/*
 * Format a Bluetooth MAC ID: BT:xx:xx:xx:xx:xx:xx
 * Normalizes to uppercase hex.
 * Returns 0 on success, -EINVAL if not a valid MAC.
 */
static int
format_bt_mac(const char *mac, cbx_identity *out)
{
    if (!cbx_identity_is_mac_address(mac))
        return -EINVAL;

    /* "BT:" (3) + 17 chars (xx:xx:xx:xx:xx:xx) + NUL = 21 */
    snprintf(out->id, CBX_IDENTITY_MAX_LEN, "BT:%s", mac);
    /* Uppercase the hex digits for consistency */
    for (char *p = out->id + 3; *p; p++) {
        *p = (char)toupper((unsigned char)*p);
    }
    out->layer = CBX_IDENTITY_LAYER_BT_MAC;
    return 0;
}

/*
 * Format a USB port path ID: USB:phys:xxxxx
 * Allows non-empty printable non-space characters (same as cbx_validate_id).
 * Returns 0 on success, -EINVAL on invalid input.
 */
static int
format_usb_phys(const char *phys, cbx_identity *out)
{
    if (!phys || !*phys)
        return -EINVAL;

    /* Validate characters: printable non-space */
    for (const char *p = phys; *p; p++) {
        /* Physical paths are later serialized in comma-separated
         * GamepadOrder values.  A comma would make an otherwise valid path
         * ambiguous and could restore the wrong controller. */
        if ((unsigned char)*p <= ' ' || (unsigned char)*p == 127 ||
            *p == ',')
            return -EINVAL;
    }

    /* "USB:phys:" (9) + phys + NUL */
    size_t phys_len = strlen(phys);
    if (phys_len + 10 > CBX_IDENTITY_MAX_LEN)
        return -EINVAL;

    snprintf(out->id, CBX_IDENTITY_MAX_LEN, "USB:phys:%s", phys);
    out->layer = CBX_IDENTITY_LAYER_USB_PORT;
    return 0;
}

/*
 * Format a connection order ID: ORDER:n
 * Returns 0 on success, -EINVAL on invalid input.
 */
static int
format_order(int connection_order, cbx_identity *out)
{
    if (connection_order < 0)
        return -EINVAL;

    snprintf(out->id, CBX_IDENTITY_MAX_LEN, "ORDER:%d", connection_order);
    out->layer = CBX_IDENTITY_LAYER_ORDER;
    return 0;
}

/* Return the two possible serial sources in their interface-defined order.
 * Keep both candidates available: a present but malformed preferred property
 * must not hide a valid alternative from a dual-interface device. */
static void
serial_candidates(const cbx_source_props *props,
                  const char *out_candidates[2])
{
    if (props->iface == CBX_SOURCE_IFACE_HIDRAW) {
        out_candidates[0] = props->serial_number;
        out_candidates[1] = props->unique_id;
    } else {
        out_candidates[0] = props->unique_id;
        out_candidates[1] = props->serial_number;
    }
}

/* --- Main extraction ----------------------------------------------------- */

int
cbx_identity_extract(const cbx_source_props *props,
                      int connection_order,
                      cbx_identity *out_ident)
{
    if (!props || !out_ident)
        return -EINVAL;
    if (props->iface != CBX_SOURCE_IFACE_EVDEV &&
        props->iface != CBX_SOURCE_IFACE_HIDRAW)
        return -EINVAL;

    cbx_identity_init(out_ident);

    int bustype = cbx_identity_parse_bustype(props->id_bustype);

    /* Layer 1: Bluetooth MAC.
     * If the bus type is Bluetooth and unique_id is a MAC address,
     * this is the strongest identity. */
    if (bustype == BUS_BLUETOOTH) {
        /* Bluetooth devices: uniq should be the MAC address.
         * Some controllers may have empty uniq — check both unique_id
         * and serial_number as fallback (SPEC §6.2: "handles empty uniq
         * for Bluetooth devices"). */
        const char *candidates[2];
        serial_candidates(props, candidates);
        for (size_t i = 0; i < 2; i++) {
            const char *candidate = candidates[i];
            if (candidate && *candidate &&
                cbx_identity_is_mac_address(candidate) &&
                format_bt_mac(candidate, out_ident) == 0)
                return 0;
        }
        /* BT device but no valid MAC — fall through to lower layers. */
    }

    /* Layer 2: USB serial.  Try both interface-defined candidates rather
     * than stopping at the first non-empty value.  Real devices can expose
     * a stale/malformed UniqueId while HIDRaw still supplies a valid
     * SerialNumber (and vice versa). */
    const char *serials[2];
    serial_candidates(props, serials);
    for (size_t i = 0; i < 2; i++) {
        const char *serial = serials[i];
        if (serial && *serial && !cbx_identity_is_mac_address(serial) &&
            format_usb_serial(serial, out_ident) == 0)
            return 0;
    }

    /* Layer 3: USB port path (phys). */
    if (props->phys_path && *props->phys_path) {
        if (format_usb_phys(props->phys_path, out_ident) == 0)
            return 0;
    }

    /* Layer 4: Connection order (fallback). */
    if (connection_order >= 0) {
        if (format_order(connection_order, out_ident) == 0)
            return 0;
    }

    /* No identity could be extracted. */
    out_ident->layer = CBX_IDENTITY_LAYER_NONE;
    out_ident->id[0] = '\0';
    return -ENOENT;
}
