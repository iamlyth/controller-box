/*
 * identity.h — Controller identity extraction (Task 25, SPEC §6.2–6.3).
 *
 * Extracts the strongest available identity from a source device's
 * properties, formatted with a type prefix so downstream code can
 * determine identity strength at runtime.
 *
 * Identity layers (strongest → weakest):
 *   Layer 1: Bluetooth MAC   — BT:xx:xx:xx:xx:xx:xx
 *   Layer 2: USB serial       — USB:SNxxxxx
 *   Layer 3: USB port path    — USB:phys:xxxxx
 *   Layer 4: Connection order  — ORDER:n
 *
 * The caller gathers source device properties via ip_source_get_* (Task 14)
 * and passes them to cbx_identity_extract(), which picks the strongest
 * available layer and formats the prefixed ID string.
 */
#ifndef CBX_IDENTITY_H
#define CBX_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Limits --------------------------------------------------------------- */

/* Maximum length of a formatted identity string (NUL-terminated). */
#define CBX_IDENTITY_MAX_LEN 128

/* --- Identity layer (SPEC §6.2) ------------------------------------------- */

typedef enum {
    CBX_IDENTITY_LAYER_NONE = 0,   /* no identity available */
    CBX_IDENTITY_LAYER_BT_MAC = 1, /* Bluetooth MAC — strongest */
    CBX_IDENTITY_LAYER_USB_SERIAL = 2, /* USB serial */
    CBX_IDENTITY_LAYER_USB_PORT = 3,   /* USB port path — semi-stable */
    CBX_IDENTITY_LAYER_ORDER = 4       /* connection order — fallback */
} cbx_identity_layer;

/* --- Source device properties (inputs) ----------------------------------- */

/*
 * Device interface type — determines which property holds the serial.
 * SPEC §10.2: serial is UniqueId on evdev/udev but SerialNumber on HIDRaw.
 */
typedef enum {
    CBX_SOURCE_IFACE_EVDEV = 0,   /* EventDevice or UdevDevice */
    CBX_SOURCE_IFACE_HIDRAW = 1   /* HIDRawDevice */
} cbx_source_iface;

/*
 * Properties read from a source device via ip_source_get_* (Task 14).
 * Each string field may be NULL or empty if the property is unavailable
 * (e.g. Bluetooth devices may have empty `uniq` for some controllers).
 *
 * `iface` determines whether the serial comes from `unique_id` (evdev)
 * or `serial_number` (HIDRaw).
 */
typedef struct {
    cbx_source_iface iface;     /* evdev or HIDRaw */
    const char *unique_id;      /* evdev `uniq` → UniqueId property */
    const char *phys_path;      /* evdev `phys` → PhysPath property */
    const char *serial_number;  /* HIDRaw SerialNumber property */
    const char *id_bustype;     /* evdev IdBustype property (decimal string) */
} cbx_source_props;

/* --- Identity result ------------------------------------------------------ */

/*
 * The extracted identity. `id` is the formatted prefixed string
 * (e.g. "BT:AB:CD:01:EF:23"). `layer` indicates the strength.
 */
typedef struct {
    char               id[CBX_IDENTITY_MAX_LEN];
    cbx_identity_layer layer;
} cbx_identity;

/* --- API ------------------------------------------------------------------ */

/*
 * Initialize an identity struct (id empty, layer NONE).
 */
void cbx_identity_init(cbx_identity *ident);

/*
 * Extract the strongest available identity from source device properties.
 *
 * Resolution order (SPEC §6.2):
 *   1. If id_bustype indicates Bluetooth (BUS_BLUETOOTH = 0x0005) and
 *      unique_id is a valid MAC address → BT:xx:xx:xx:xx:xx:xx (layer 1)
 *   2. If the serial (unique_id for evdev, serial_number for HIDRaw) is
 *      non-empty and NOT a MAC address → USB:SNxxxxx (layer 2)
 *   3. If phys_path is non-empty → USB:phys:xxxxx (layer 3)
 *   4. If connection_order >= 0 → ORDER:n (layer 4)
 *   5. Otherwise → layer NONE, empty id
 *
 * @param props          Source device properties (may have NULL fields).
 * @param connection_order  Zero-based enumeration order, or -1 if unknown.
 * @param out_ident      Output identity (overwritten).
 * @return 0 on success (identity found, even if layer 4);
 *         -EINVAL if null args;
 *         -ENOENT if no identity could be extracted.
 */
int cbx_identity_extract(const cbx_source_props *props,
                          int connection_order,
                          cbx_identity *out_ident);

/*
 * Determine the identity layer from a prefixed ID string.
 * Parses the prefix and validates the format.
 *
 * @param id  Prefixed ID string (e.g. "BT:AB:CD:01:EF:23").
 * @return    Identity layer (1–4), or CBX_IDENTITY_LAYER_NONE if
 *            the string is invalid or unrecognised.
 */
cbx_identity_layer cbx_identity_parse_layer(const char *id);

/*
 * Check whether two identity layers represent a downgrade
 * (new_layer is weaker than old_layer).
 *
 * @return true if new_layer > old_layer (weaker), false otherwise.
 */
bool cbx_identity_is_downgrade(cbx_identity_layer old_layer,
                                cbx_identity_layer new_layer);

/*
 * Check whether a string looks like a Bluetooth MAC address.
 * Format: xx:xx:xx:xx:xx:xx (6 hex octets, colon-separated).
 *
 * @return true if the string is a valid MAC address.
 */
bool cbx_identity_is_mac_address(const char *s);

/*
 * Parse the IdBustype string to an integer.
 * evdev IdBustype is reported as a string (e.g. "3" for USB, "5" for BT).
 *
 * @return the bus type integer, or -1 if invalid/NULL.
 */
int cbx_identity_parse_bustype(const char *bustype_str);

#ifdef __cplusplus
}
#endif

#endif /* CBX_IDENTITY_H */