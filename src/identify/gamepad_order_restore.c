/*
 * gamepad_order_restore.c — GamepadOrder restoration after restart
 *                           (Task 27, SPEC §10.3 gap #2).
 *
 * After InputPlumber restart and re-enumeration, maps saved gamepad_order
 * IDs back to composite device paths by extracting each composite's physical
 * identity from its source devices (BT MAC → USB serial → USB port path →
 * connection order, SPEC §6.2), then re-applies the order through
 * ip_manager_set_gamepad_order().
 *
 * The opaque InputPlumber `PersistentId` is deliberately not used as the
 * identity contract: assignments.yaml keys are the prefixed source-derived
 * identities, and PersistentId is an implementation detail that may not
 * share that format (task 6 acceptance).
 *
 * A transient property-read failure is not absence.  When a composite's
 * identity query fails, the mapping is marked uncertain and the order is not
 * applied, so a temporary DBus hiccup can never erase the persisted order or
 * publish a misleading partial/empty one.
 */
#include "gamepad_order_restore.h"

#include "identify/composite_identity.h"
#include "dbus/ip_gamepad_order.h"  /* ip_gamepad_order_load */
#include "dbus/ip_manager.h"        /* ip_manager_set_gamepad_order */
#include "config/config_assignments.h" /* CBX_MAX_ID_LEN */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Helpers -------------------------------------------------------------- */

/* Append `path` to the output CSV at *pos (which is never rolled back on a
 * later error).  Returns 0, or -ENOSPC when the buffer cannot hold it. */
static int
append_path(char *csv, size_t csv_len, size_t *pos, const char *path)
{
    size_t plen = strlen(path);
    bool need_comma = (*pos > 0);
    size_t need = plen + (need_comma ? 1 : 0);
    if (*pos + need + 1 > csv_len)
        return -ENOSPC;

    if (need_comma)
        csv[(*pos)++] = ',';
    memcpy(csv + *pos, path, plen);
    *pos += plen;
    csv[*pos] = '\0';
    return 0;
}

/* --- ID-to-path mapping --------------------------------------------------- */

int
cbx_gamepad_order_map_ids(const ip_dbus_backend *backend,
                           ip_bus_handle bus,
                           const cbx_device_model *model,
                           const char *saved_ids_csv,
                           char *out_paths_csv,
                           size_t paths_csv_len,
                           int *out_restored_count,
                           int *out_skipped_count,
                           bool *out_query_failed)
{
    if (!backend || !model || !saved_ids_csv || !out_paths_csv)
        return -EINVAL;
    if (paths_csv_len == 0)
        return -EINVAL;

    out_paths_csv[0] = '\0';
    if (out_restored_count)
        *out_restored_count = 0;
    if (out_skipped_count)
        *out_skipped_count = 0;
    if (out_query_failed)
        *out_query_failed = false;

    /* Extract every composite's identity exactly once. */
    cbx_composite_identity_entry entries[CBX_MAX_COMPOSITES];
    int entry_count = 0;
    int rc = cbx_model_extract_identities(backend, bus, model, entries,
                                          &entry_count);
    if (rc != 0)
        return rc;

    int restored = 0;
    int skipped = 0;
    bool query_failed = false;
    size_t pos = 0;

    const char *p = saved_ids_csv;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        if (*p == '\0')
            break;

        const char *end = strchr(p, ',');
        if (!end)
            end = p + strlen(p);

        /* Trim and copy the token. */
        while (p < end && (*p == ' ' || *p == '\t'))
            p++;
        const char *trim_end = end;
        while (trim_end > p && (trim_end[-1] == ' ' || trim_end[-1] == '\t'))
            trim_end--;

        size_t len = (size_t)(trim_end - p);
        if (len > 0 && len < CBX_MAX_ID_LEN) {
            char saved_id[CBX_MAX_ID_LEN];
            memcpy(saved_id, p, len);
            saved_id[len] = '\0';

            const char *match = NULL;
            bool any_failed = false;
            for (int i = 0; i < entry_count; i++) {
                if (entries[i].status ==
                    CBX_COMPOSITE_IDENTITY_QUERY_FAILED) {
                    any_failed = true;
                    continue;
                }
                if (entries[i].ident.layer != CBX_IDENTITY_LAYER_NONE &&
                    strcmp(entries[i].ident.id, saved_id) == 0) {
                    match = entries[i].path;
                    break;
                }
            }

            if (match) {
                rc = append_path(out_paths_csv, paths_csv_len, &pos, match);
                if (rc != 0)
                    return rc;
                restored++;
            } else if (any_failed) {
                /* Cannot distinguish stale from a transient read failure. */
                query_failed = true;
            } else {
                skipped++;
            }
        }

        p = *end ? end + 1 : end;
    }

    if (out_restored_count)
        *out_restored_count = restored;
    if (out_skipped_count)
        *out_skipped_count = skipped;
    if (out_query_failed)
        *out_query_failed = query_failed;

    return 0;
}

int
cbx_gamepad_order_restore(const ip_dbus_backend *backend,
                           ip_bus_handle bus,
                           const cbx_device_model *model,
                           int *out_restored_count,
                           int *out_skipped_count,
                           bool *out_query_failed)
{
    if (!backend || !bus || !model)
        return -EINVAL;

    if (out_restored_count)
        *out_restored_count = 0;
    if (out_skipped_count)
        *out_skipped_count = 0;
    if (out_query_failed)
        *out_query_failed = false;

    /* 1. Load saved gamepad_order IDs. */
    char *saved_ids_csv = NULL;
    int rc = ip_gamepad_order_load(&saved_ids_csv);
    if (rc != 0)
        return rc;

    if (!saved_ids_csv)
        return -ENOMEM;

    /* Empty saved order → nothing to restore. */
    if (saved_ids_csv[0] == '\0') {
        free(saved_ids_csv);
        return -ENOENT;
    }

    /* 2. Map saved IDs to composite paths using source-derived identities. */
    char paths_csv[CBX_MAX_PATH_LEN * CBX_MAX_GAMEPAD_ORDER];
    int restored = 0;
    int skipped = 0;
    bool query_failed = false;

    rc = cbx_gamepad_order_map_ids(backend, bus, model,
                                    saved_ids_csv,
                                    paths_csv, sizeof(paths_csv),
                                    &restored, &skipped, &query_failed);
    free(saved_ids_csv);
    if (rc != 0)
        return rc;

    /* 3. A transient identity query failure leaves the saved order untouched
     *    and defers the apply: publishing a partial/empty order here would be
     *    misleading and could disrupt live routing.  The caller retries. */
    if (query_failed) {
        if (out_restored_count)
            *out_restored_count = restored;
        if (out_skipped_count)
            *out_skipped_count = skipped;
        if (out_query_failed)
            *out_query_failed = true;
        return -EAGAIN;
    }

    /* 4. Re-apply the order via the DBus setter.
     *    ip_manager_set_gamepad_order validates all paths exist in model.
     *    If paths_csv is empty (all IDs stale), we set empty order to clear. */
    rc = ip_manager_set_gamepad_order(backend, bus, paths_csv, model);
    if (rc != 0) {
        /* Setter failed — but we still report the counts. */
        if (out_restored_count)
            *out_restored_count = restored;
        if (out_skipped_count)
            *out_skipped_count = skipped;
        return rc;
    }

    if (out_restored_count)
        *out_restored_count = restored;
    if (out_skipped_count)
        *out_skipped_count = skipped;

    return 0;
}
