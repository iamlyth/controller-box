/*
 * identity.c — Controller identity extraction (Task 25, SPEC §6.2–6.3).
 *
 * Implements the multi-layered identity extraction described in SPEC §6.2.
 * The caller gathers source device properties (via ip_source_get_* from
 * Task 14) and passes them to cbx_identity_extract(), which selects the
 * strongest available identity layer and formats the prefixed ID string.
 *
 * Linux input subsystem bus type constants (from linux/input.h):
 *   BUS_USB       = 0x03
 *   BUS_BLUETOOTH = 0x05
 *   BUS_I8042     = 0x11
 *   (others are irrelevant for controller identification)
 */
#include "identity.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Constants ----------------------------------------------------------- */

/* Linux input subsystem bus types. */
#define BUS_USB       0x03
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

    /* Parse as decimal integer (evdev IdBustype is a u16) */
    char *end = NULL;
    long val = strtol(bustype_str, &end, 10);
    if (end == bustype_str || *end != '\0' || val < 0 || val > 0xFFFF)
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
        if ((unsigned char)*p <= ' ' || (unsigned char)*p == 127)
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

/*
 * Get the serial string from source properties based on interface type.
 * For evdev: use unique_id (UniqueId property).
 * For HIDRaw: use serial_number (SerialNumber property).
 * Falls back to unique_id if serial_number is NULL.
 */
static const char *
get_serial(const cbx_source_props *props)
{
    if (props->iface == CBX_SOURCE_IFACE_HIDRAW) {
        /* HIDRaw: prefer SerialNumber, fall back to UniqueId */
        if (props->serial_number && *props->serial_number)
            return props->serial_number;
        if (props->unique_id && *props->unique_id)
            return props->unique_id;
    } else {
        /* evdev/udev: use UniqueId */
        if (props->unique_id && *props->unique_id)
            return props->unique_id;
        /* Fall back to HIDRaw serial if available (dual-interface device) */
        if (props->serial_number && *props->serial_number)
            return props->serial_number;
    }
    return NULL;
}

/* --- Main extraction ----------------------------------------------------- */

int
cbx_identity_extract(const cbx_source_props *props,
                      int connection_order,
                      cbx_identity *out_ident)
{
    if (!props || !out_ident)
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
        const char *candidate = NULL;
        if (props->unique_id && *props->unique_id)
            candidate = props->unique_id;
        else if (props->serial_number && *props->serial_number)
            candidate = props->serial_number;

        if (candidate && cbx_identity_is_mac_address(candidate)) {
            if (format_bt_mac(candidate, out_ident) == 0)
                return 0;
        }
        /* BT device but no valid MAC — fall through to lower layers */
    }

    /* Layer 2: USB serial.
     * The serial is unique_id (evdev) or serial_number (HIDRaw).
     * It must be non-empty and NOT a MAC address (MACs are layer 1).
     * Also, if the bus type is USB, we have higher confidence this is
     * a real serial number rather than something else. */
    const char *serial = get_serial(props);
    if (serial && *serial) {
        /* A MAC address is handled by layer 1 (or falls here for non-BT
         * devices). If it IS a MAC and the bus is NOT Bluetooth, we still
         * treat it as layer 2 (USB serial) since it's a valid unique
         * identifier — just format it as USB:SNxxxxx if it passes the
         * serial character check, or skip it if it contains colons. */
        bool is_mac = cbx_identity_is_mac_address(serial);

        if (!is_mac) {
            /* Regular USB serial */
            if (format_usb_serial(serial, out_ident) == 0)
                return 0;
        } else if (bustype != BUS_BLUETOOTH) {
            /* MAC-like string on a non-BT bus: treat as serial.
             * But MAC contains colons which aren't valid serial chars,
             * so format it differently — use the raw string as the serial
             * value. Actually, colons are not in is_serial_char(), so
             * format_usb_serial will reject it. We need to handle this
             * case: a USB device reporting a MAC-like unique ID.
             *
             * Per SPEC §6.2: "USB serial" is from evdev uniq. Some USB
             * devices (e.g. Bluetooth dongles) may report MAC-like strings.
             * For pure USB devices, a MAC-format uniq is unusual but valid
             * as a unique identifier. We'll format it as USB:SNxxxxx only
             * if the characters are valid; otherwise skip to layer 3.
             *
             * In practice, if is_mac is true and bus is USB, this is likely
             * a composite device or mis-reported bustype. Skip to layer 3.
             */
            /* Skip — can't format MAC as USB serial due to colons */
        }
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

/* --- Parse layer from prefixed ID ---------------------------------------- */

cbx_identity_layer
cbx_identity_parse_layer(const char *id)
{
    if (!id || !*id)
        return CBX_IDENTITY_LAYER_NONE;

    if (strncmp(id, "BT:", 3) == 0) {
        /* Validate: must be BT:xx:xx:xx:xx:xx:xx */
        const char *p = id + 3;
        for (int i = 0; i < 6; i++) {
            if (!is_hex_digit(p[0]) || !is_hex_digit(p[1]))
                return CBX_IDENTITY_LAYER_NONE;
            p += 2;
            if (i < 5) {
                if (*p != ':')
                    return CBX_IDENTITY_LAYER_NONE;
                p++;
            }
        }
        if (*p != '\0')
            return CBX_IDENTITY_LAYER_NONE;
        return CBX_IDENTITY_LAYER_BT_MAC;
    }

    if (strncmp(id, "USB:phys:", 9) == 0) {
        const char *p = id + 9;
        if (!*p)
            return CBX_IDENTITY_LAYER_NONE;
        for (; *p; p++) {
            if ((unsigned char)*p <= ' ' || (unsigned char)*p == 127)
                return CBX_IDENTITY_LAYER_NONE;
        }
        return CBX_IDENTITY_LAYER_USB_PORT;
    }

    if (strncmp(id, "USB:", 4) == 0) {
        const char *p = id + 4;
        if (!*p)
            return CBX_IDENTITY_LAYER_NONE;
        for (; *p; p++) {
            if (!is_serial_char(*p))
                return CBX_IDENTITY_LAYER_NONE;
        }
        return CBX_IDENTITY_LAYER_USB_SERIAL;
    }

    if (strncmp(id, "ORDER:", 6) == 0) {
        const char *p = id + 6;
        if (!*p)
            return CBX_IDENTITY_LAYER_NONE;
        for (; *p; p++) {
            if (!isdigit((unsigned char)*p))
                return CBX_IDENTITY_LAYER_NONE;
        }
        return CBX_IDENTITY_LAYER_ORDER;
    }

    return CBX_IDENTITY_LAYER_NONE;
}

/* --- Downgrade detection ------------------------------------------------- */

bool
cbx_identity_is_downgrade(cbx_identity_layer old_layer,
                           cbx_identity_layer new_layer)
{
    /* Higher layer number = weaker identity (SPEC §6.3).
     * A downgrade occurs when the new layer is weaker (higher number)
     * than the old layer. */
    if (old_layer == CBX_IDENTITY_LAYER_NONE ||
        new_layer == CBX_IDENTITY_LAYER_NONE)
        return false;
    return (int)new_layer > (int)old_layer;
}