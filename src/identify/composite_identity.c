/*
 * composite_identity.c — Physical controller identity from source devices
 *                        (SPEC §6.2–6.3, task 6).
 *
 * See composite_identity.h for the contract.  All DBus access goes through
 * the ip_dbus_backend vtable so the extraction path is unit-testable and so
 * the production sd-bus path issues exactly the property reads described in
 * SPEC §10.2.
 */
#include "identify/composite_identity.h"

#include "dbus/ip_composite.h"   /* ip_composite_get_source_device_paths */
#include "dbus/ip_source.h"      /* ip_source_get_*                    */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Small helpers -------------------------------------------------------- */

static const char *
path_basename(const char *path)
{
    if (!path)
        return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

cbx_source_iface
cbx_source_iface_for_path(const char *source_path)
{
    const char *base = path_basename(source_path);
    if (strncmp(base, "hidraw", 6) == 0)
        return CBX_SOURCE_IFACE_HIDRAW;
    /* eventN, iio:deviceN, ledN, ... all use the evdev/udev property set. */
    return CBX_SOURCE_IFACE_EVDEV;
}

/* Copy a trimmed CSV token into `out`.  Returns the number of characters
 * copied (0 for an empty token).  Never writes more than out_size-1 chars
 * plus NUL. */
static size_t
copy_trimmed_token(const char *begin, const char *end, char *out, size_t out_size)
{
    if (out_size == 0)
        return 0;
    while (begin < end && (*begin == ' ' || *begin == '\t'))
        begin++;
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t'))
        end--;
    size_t len = (size_t)(end - begin);
    if (len >= out_size)
        len = out_size - 1;
    memcpy(out, begin, len);
    out[len] = '\0';
    return len;
}

/* --- Per-source extraction ------------------------------------------------ */

/*
 * Probe the shared evdev/udev identification property set
 * (UniqueId/PhysPath/IdBustype, SPEC §10.2) on one interface subtype.
 * `iface` is IP_IFACE_SOURCE_EVENT or IP_IFACE_SOURCE_UDEV.  Returns true
 * when at least one read succeeded, and sets *out_read_failed when any read
 * failed so a partial/transient failure can be distinguished from a
 * confirmed identity-less device.  The three values are heap-allocated on
 * success; the caller owns them.
 */
static bool
probe_evdev_props(const ip_dbus_backend *backend, ip_bus_handle bus,
                  const char *source_path, const char *iface,
                  char **unique_id, char **phys_path, char **id_bustype,
                  bool *out_read_failed)
{
    bool any_ok = false;
    bool failed = false;

    if (ip_source_get_unique_id(backend, bus, source_path, iface,
                                unique_id) != 0)
        failed = true;
    else
        any_ok = true;
    if (ip_source_get_phys_path(backend, bus, source_path, iface,
                                phys_path) != 0)
        failed = true;
    else
        any_ok = true;
    if (ip_source_get_id_bustype(backend, bus, source_path, iface,
                                 id_bustype) != 0)
        failed = true;
    else
        any_ok = true;

    if (out_read_failed)
        *out_read_failed = failed;
    return any_ok;
}

/*
 * Read one source device's interface-appropriate properties and extract its
 * strongest identity.  `connection_order` is -1 here: the composite-level
 * ORDER fallback is applied once, after every source has been considered.
 *
 * `out_read_failed` is set when any attempted property read failed.  With no
 * stable identity available, that distinguishes "device present but without
 * stable identity" (all reads succeeded with empty values) from a transient
 * read failure that must be reported as uncertain rather than matched.
 */
static int
extract_from_source(const ip_dbus_backend *backend, ip_bus_handle bus,
                    const char *source_path, cbx_identity *out_ident,
                    bool *out_read_failed)
{
    cbx_source_iface iface = cbx_source_iface_for_path(source_path);
    char *unique_id = NULL;
    char *phys_path = NULL;
    char *serial_number = NULL;
    char *id_bustype = NULL;
    bool read_failed = false;

    if (iface == CBX_SOURCE_IFACE_HIDRAW) {
        if (ip_source_get_serial_number(backend, bus, source_path,
                                        IP_IFACE_SOURCE_HIDRAW,
                                        &serial_number) != 0)
            read_failed = true;
    } else {
        /* EventDevice and UdevDevice expose the same identification property
         * set (SPEC §10.2); probe EventDevice first and fall back to
         * UdevDevice so a non-event source (e.g. iio:deviceN) is not lost. */
        bool event_ok = probe_evdev_props(backend, bus, source_path,
                                          IP_IFACE_SOURCE_EVENT,
                                          &unique_id, &phys_path,
                                          &id_bustype, &read_failed);
        if (!event_ok) {
            /* No EventDevice property was readable: retry the same set on
             * UdevDevice.  The retry supersedes the EventDevice attempt, so
             * only the retry's failures are reported. */
            (void)probe_evdev_props(backend, bus, source_path,
                                    IP_IFACE_SOURCE_UDEV,
                                    &unique_id, &phys_path,
                                    &id_bustype, &read_failed);
        }
    }

    cbx_source_props props = {
        .iface         = iface,
        .unique_id     = unique_id,
        .phys_path     = phys_path,
        .serial_number = serial_number,
        .id_bustype    = id_bustype,
    };

    int rc = cbx_identity_extract(&props, -1, out_ident);

    free(unique_id);
    free(phys_path);
    free(serial_number);
    free(id_bustype);

    if (out_read_failed)
        *out_read_failed = read_failed;

    return rc;
}

/* --- Public API ----------------------------------------------------------- */

bool
cbx_composite_identity_is_matchable(const cbx_identity *ident,
                                     cbx_composite_identity_status status)
{
    return ident && status != CBX_COMPOSITE_IDENTITY_QUERY_FAILED &&
           ident->layer != CBX_IDENTITY_LAYER_NONE;
}

int
cbx_composite_identity_order(const cbx_composite_entry *entry,
                             int fallback_index)
{
    return (entry && entry->index >= 0) ? entry->index : fallback_index;
}

int
cbx_composite_identity_extract(const ip_dbus_backend *backend,
                               ip_bus_handle bus,
                               const char *composite_path,
                               int connection_order,
                               cbx_identity *out_ident,
                               cbx_composite_identity_status *out_status)
{
    if (!backend || !composite_path || !out_ident)
        return -EINVAL;

    cbx_identity_init(out_ident);
    if (out_status)
        *out_status = CBX_COMPOSITE_IDENTITY_OK;

    /* Fallback used when no stable property is available (or the source list
     * could not be read): connection order is the weakest but always-valid
     * identity layer, and avoids the invalid `composite-N` synthetic ids that
     * cannot be persisted (cbx_validate_id). */
    cbx_identity order_ident;
    bool have_order = false;
    if (connection_order >= 0) {
        cbx_source_props empty;
        memset(&empty, 0, sizeof(empty));
        empty.iface = CBX_SOURCE_IFACE_EVDEV;
        if (cbx_identity_extract(&empty, connection_order, &order_ident) == 0)
            have_order = true;
    }

    char *paths = NULL;
    int rc = ip_composite_get_source_device_paths(backend, bus,
                                                  composite_path, &paths);
    if (rc != 0) {
        /* A transient property-read failure is not absence: report it so
         * order/assignment restoration keeps the saved state and retries
         * instead of applying a misleading empty result. */
        free(paths);
        if (out_status)
            *out_status = CBX_COMPOSITE_IDENTITY_QUERY_FAILED;
        if (have_order) {
            *out_ident = order_ident;
            return 0;
        }
        return rc;
    }

    bool saw_source = false;
    bool read_failed_any = false;
    if (paths && paths[0]) {
        const char *p = paths;
        while (*p) {
            const char *comma = strchr(p, ',');
            const char *end = comma ? comma : p + strlen(p);

            char source_path[CBX_MAX_PATH_LEN];
            if (copy_trimmed_token(p, end, source_path,
                                   sizeof(source_path)) > 0) {
                saw_source = true;
                cbx_identity candidate;
                bool source_read_failed = false;
                if (extract_from_source(backend, bus, source_path,
                                        &candidate, &source_read_failed) == 0) {
                    /* Keep the strongest (lowest layer number). */
                    if (out_ident->layer == CBX_IDENTITY_LAYER_NONE ||
                        (int)candidate.layer < (int)out_ident->layer)
                        *out_ident = candidate;
                }
                if (source_read_failed)
                    read_failed_any = true;
            }

            if (!comma)
                break;
            p = comma + 1;
        }
    }
    free(paths);

    /* A successful identity from one property is not enough to make the
     * composite safe to match when another required property read failed.
     * SourceDevicePaths can describe a composite made from several source
     * interfaces; accepting the first identity here would let a partial
     * DBus snapshot bind the controller to an unrelated saved assignment.
     * Keep the fallback value for display, but mark the whole result
     * uncertain so every restoration caller defers it. */
    if (read_failed_any && out_status)
        *out_status = CBX_COMPOSITE_IDENTITY_QUERY_FAILED;

    if (out_ident->layer != CBX_IDENTITY_LAYER_NONE)
        return 0;

    /* No stable identity was obtained from any source.  A source that was
     * present but whose properties could not all be read is a transient
     * failure, not a confirmed weak identity: report it as uncertain so
     * assignment/order matchers skip the entry instead of letting the
     * ORDER fallback match another controller's saved weak preference. */
    if (!saw_source) {
        if (out_status)
            *out_status = CBX_COMPOSITE_IDENTITY_ABSENT;
    } else if (read_failed_any) {
        if (out_status)
            *out_status = CBX_COMPOSITE_IDENTITY_QUERY_FAILED;
    }

    if (have_order) {
        *out_ident = order_ident;
        return 0;
    }

    return -ENOENT;
}

int
cbx_model_extract_identities(const ip_dbus_backend *backend,
                             ip_bus_handle bus,
                             const cbx_device_model *model,
                             cbx_composite_identity_entry *entries,
                             int *out_count)
{
    if (!backend || !model || !entries || !out_count)
        return -EINVAL;

    *out_count = 0;

    for (int i = 0; i < model->composite_count && i < CBX_MAX_COMPOSITES; i++) {
        const cbx_composite_entry *comp = &model->composites[i];
        cbx_composite_identity_entry *e = &entries[i];

        memset(e, 0, sizeof(*e));
        snprintf(e->path, sizeof(e->path), "%s", comp->path);

        int order = cbx_composite_identity_order(comp, i);
        (void)cbx_composite_identity_extract(backend, bus, comp->path, order,
                                             &e->ident, &e->status);
        (*out_count)++;
    }

    return 0;
}
