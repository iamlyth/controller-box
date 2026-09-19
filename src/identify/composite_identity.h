/*
 * composite_identity.h — Physical controller identity from source devices
 *                        (SPEC §6.2–6.3, task 6).
 *
 * The authoritative controller identity is derived from the physical source
 * devices that compose a CompositeDevice, not from the opaque, InputPlumber
 * implementation detail `PersistentId`.  For each source device this module
 * collects the interface-appropriate properties (SPEC §10.2):
 *
 *   - EventDevice / UdevDevice: UniqueId, PhysPath, IdBustype
 *   - HIDRawDevice:             SerialNumber
 *
 * and feeds them to cbx_identity_extract() (SPEC §6.2), which chooses the
 * strongest available layer:
 *
 *   BT MAC → USB serial → USB port path → connection order
 *
 * The query result distinguishes a *confirmed absent* source list from a
 * *transient read failure*.  Order/assignment restoration must never treat a
 * failed DBus read as "the controller is gone": doing so would erase saved
 * preferences or apply a misleading empty order (task 6 acceptance).
 */
#ifndef CBX_COMPOSITE_IDENTITY_H
#define CBX_COMPOSITE_IDENTITY_H

#include <stdbool.h>

#include "dbus/dbus_interface.h"     /* ip_dbus_backend, ip_bus_handle */
#include "dbus/ip_device_model.h"    /* cbx_device_model */
#include "identify/identity.h"       /* cbx_identity, cbx_source_iface */

#ifdef __cplusplus
extern "C" {
#endif

/* Result of querying a composite's source devices. */
typedef enum {
    /* The source list was read successfully.  The identity may still be the
     * ORDER:n connection-order fallback when no stable property is present. */
    CBX_COMPOSITE_IDENTITY_OK = 0,
    /* Confirmed: the composite exposes no source devices at all. */
    CBX_COMPOSITE_IDENTITY_ABSENT = 1,
    /* Transient: a required property read failed.  The composite's identity
     * is unknown for this pass and must not be treated as absent/stale. */
    CBX_COMPOSITE_IDENTITY_QUERY_FAILED = 2
} cbx_composite_identity_status;

/* Per-composite extraction result (for whole-model passes). */
typedef struct {
    char                          path[CBX_MAX_PATH_LEN];
    cbx_identity                  ident;
    cbx_composite_identity_status status;
} cbx_composite_identity_entry;

/*
 * Single acceptance predicate for "may this extracted identity be used to
 * match a saved assignment / order entry?".
 *
 * True only when the query completed (a transient QUERY_FAILED read is never
 * matchable — a DBus hiccup must not reroute a controller or erase saved
 * state) and a real layer was extracted (including the ORDER:n fallback).
 * Every assignment/order matcher uses this so the absence-vs-failure rule
 * cannot drift between call sites (task 6 acceptance).
 */
bool cbx_composite_identity_is_matchable(const cbx_identity *ident,
                                         cbx_composite_identity_status status);

/*
 * Connection-order value for one composite's ORDER:n fallback (SPEC §6.2
 * layer 4).  Prefer the composite's parsed model index; use `fallback_index`
 * (the caller's enumeration position) only when the model carries none, so
 * every call site derives the same ORDER:n instead of drifting between 0 and
 * the loop position.
 */
int cbx_composite_identity_order(const cbx_composite_entry *entry,
                                 int fallback_index);

/*
 * Resolve a saved assignment/order identity against an already-extracted
 * snapshot of the current composites.
 *
 * This is the single definition of the match rule shared by assignment
 * restoration (overlay), target routing (manager) and GamepadOrder
 * restoration, so the absence-vs-failure and duplicate-identity policies
 * cannot drift between call sites (task 6 acceptance).
 *
 * Returns:
 *    1  exactly one matchable entry matches; `out_path` (when non-NULL) and
 *       `out_index` (when non-NULL) describe it;
 *    0  no entry matches — a confirmed stale preference;
 *   -1  uncertain or ambiguous: an entry's transient read failed, or more
 *       than one matchable entry shares the identity.  Callers must leave
 *       saved state untouched and report uncertainty rather than choosing
 *       the first ObjectManager path.
 * Null args or an empty saved_id also return -1.
 */
int cbx_composite_identity_resolve_id(
    const cbx_composite_identity_entry *entries, int entry_count,
    const char *saved_id, char *out_path, size_t out_path_size,
    int *out_index);

/*
 * Classify a source device object path by its interface subtype, derived
 * from the last path component (SPEC §10.2):
 *   - "hidrawN"                    → HIDRawDevice
 *   - "eventN", "iio:deviceN", ... → EventDevice / UdevDevice
 * Unknown/missing paths default to the evdev/udev interface (which carries
 * UniqueId/PhysPath/IdBustype); callers still validate the returned
 * properties, so a wrong guess degrades to the ORDER fallback.
 */
cbx_source_iface cbx_source_iface_for_path(const char *source_path);

/*
 * Extract the strongest identity for a single composite.
 *
 * Reads `SourceDevicePaths` from the composite, reads each source device's
 * interface-appropriate properties, extracts the strongest per-source
 * identity, and keeps the strongest across all sources.  Each
 * SourceDevicePaths entry is a physical device node (e.g.
 * "/dev/input/event3", "/dev/hidraw0"); it is mapped to the DBus source
 * object path InputPlumber registers at
 * /org/shadowblip/InputPlumber/devices/source/<sysname> before its
 * properties are read, because a device node is not a DBus object path.
 * When no property
 * yields a stable identity and `connection_order >= 0`, falls back to
 * ORDER:n (SPEC §6.2 layer 4).
 *
 * @param backend          DBus backend vtable.
 * @param bus              DBus bus handle.
 * @param composite_path   CompositeDevice object path.
 * @param connection_order Zero-based fallback order, or -1 for none.
 * @param out_ident        Output identity (initialised to NONE on entry).
 * @param out_status       Optional query status output.
 * @return 0 when an identity was extracted (including ORDER:n);
 *         -ENOENT when no identity could be extracted;
 *         negative errno when required args are NULL or the source list
 *         read failed (out_status is QUERY_FAILED in that case).  When the
 *         source list reads successfully but no stable identity is obtained
 *         from any source whose property reads failed, out_status is also
 *         QUERY_FAILED (the ORDER:n fallback is still produced for display
 *         continuity, but matchers must not treat it as a confirmed
 *         identity).
 */
int cbx_composite_identity_extract(const ip_dbus_backend *backend,
                                   ip_bus_handle bus,
                                   const char *composite_path,
                                   int connection_order,
                                   cbx_identity *out_ident,
                                   cbx_composite_identity_status *out_status);

/*
 * Extract the identity and query status for every composite in `model`.
 *
 * Each composite is queried exactly once (unlike per-assignment scans), and
 * the ORDER fallback uses the composite's model index.  On return
 * `out_entries[0..*out_count)` corresponds to `model->composites[0..count)`.
 *
 * @param entries   Caller-provided array of at least CBX_MAX_COMPOSITES.
 * @param out_count Output: number of populated entries.
 * @return 0 on success; -EINVAL on NULL args.
 */
int cbx_model_extract_identities(const ip_dbus_backend *backend,
                                 ip_bus_handle bus,
                                 const cbx_device_model *model,
                                 cbx_composite_identity_entry *entries,
                                 int *out_count);

#ifdef __cplusplus
}
#endif

#endif /* CBX_COMPOSITE_IDENTITY_H */
