/*
 * ip_gamepad_order.c — GamepadOrder persistence layer (Task 15, gap #2).
 *
 * See ip_gamepad_order.h for the gap #2 workaround description.  The order is
 * keyed by the source-derived physical identity (SPEC §6.2), not by the
 * opaque InputPlumber PersistentId (task 6 acceptance).
 */
#include "ip_gamepad_order.h"

#include "identify/composite_identity.h" /* cbx_composite_identity_extract */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Public API --------------------------------------------------------- */

/* Assignment-list gamepad_order replacement applied under the config lock. */
typedef struct {
    char ids[CBX_MAX_GAMEPAD_ORDER][CBX_MAX_ID_LEN];
    char paths[CBX_MAX_GAMEPAD_ORDER][CBX_MAX_PATH_LEN];
    int  count;
} order_txn_args;

static int txn_replace_order(cbx_assignments *a, void *userdata)
{
    order_txn_args *args = userdata;
    a->gamepad_order_count = 0;
    for (int i = 0; i < args->count; i++) {
        if (a->gamepad_order_count >= CBX_MAX_GAMEPAD_ORDER)
            break;
        strncpy(a->gamepad_order[a->gamepad_order_count], args->ids[i],
                CBX_MAX_ID_LEN - 1);
        a->gamepad_order[a->gamepad_order_count][CBX_MAX_ID_LEN - 1] = '\0';
        a->gamepad_order_count++;
    }
    return 0;
}

int
ip_gamepad_order_save(const ip_dbus_backend *backend,
                       ip_bus_handle bus,
                       const cbx_device_model *model,
                       const char *paths_csv)
{
    if (!backend || !bus || !model || !paths_csv)
        return -EINVAL;
    if (model->composite_count < 0 ||
        model->composite_count > CBX_MAX_COMPOSITES)
        return -E2BIG;

    /* Resolve composite paths → source-derived identities outside the config
     * lock (DBus latency must not serialize config writers).  Stale paths and
     * IDs are skipped and duplicate IDs are collapsed.  A transient identity
     * query failure is skipped rather than persisted as a wrong/opaque id. */
    order_txn_args args;
    memset(&args, 0, sizeof(args));
    bool query_failed = false;
    bool ambiguous_identity = false;

    const char *p = paths_csv;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);

        char path[CBX_MAX_PATH_LEN];
        if (len >= sizeof(path)) {
            if (!comma)
                break;
            p = comma + 1;
            continue;
        }
        memcpy(path, p, len);
        path[len] = '\0';

        const cbx_composite_entry *comp =
            cbx_device_model_find_composite(model, path);
        if (comp) {
            int order = -1;
            for (int i = 0; i < model->composite_count; i++) {
                if (strcmp(model->composites[i].path, path) == 0) {
                    order = i;
                    break;
                }
            }
            if (order < 0) {
                /* The model changed between lookup and this pass.  Do not
                 * manufacture ORDER:0 for an unresolved path. */
                query_failed = true;
                if (!comma)
                    break;
                p = comma + 1;
                continue;
            }
            order = cbx_composite_identity_order(comp, order);
            cbx_identity ident;
            cbx_composite_identity_status status = CBX_COMPOSITE_IDENTITY_OK;
            int ident_rc = cbx_composite_identity_extract(backend, bus,
                                                            path, order,
                                                            &ident, &status);
            if (status == CBX_COMPOSITE_IDENTITY_QUERY_FAILED)
                query_failed = true;
            if (ident_rc == 0 &&
                cbx_composite_identity_is_matchable(&ident, status) &&
                cbx_validate_id(ident.id) &&
                args.count < CBX_MAX_GAMEPAD_ORDER) {
                bool duplicate_path = false;
                for (int i = 0; i < args.count; i++) {
                    if (strcmp(args.paths[i], path) == 0) {
                        duplicate_path = true;
                        break;
                    }
                    if (strcmp(args.ids[i], ident.id) == 0)
                        ambiguous_identity = true;
                }
                if (!duplicate_path && args.count < CBX_MAX_GAMEPAD_ORDER) {
                    snprintf(args.ids[args.count], CBX_MAX_ID_LEN, "%s",
                             ident.id);
                    snprintf(args.paths[args.count], CBX_MAX_PATH_LEN, "%s",
                             path);
                    args.count++;
                }
            }
        }

        if (!comma)
            break;
        p = comma + 1;
    }

    /* A partial source-property snapshot is not an authoritative empty
     * order.  Do not run the replacement transaction in that case: a
     * transient DBus failure must preserve the last durable preference.
     * Keep the no-file/empty-order case backward compatible by reporting
     * success without creating or rewriting a file. */
    if (query_failed || ambiguous_identity) {
        /* Never replace a good durable order with a partial or ambiguous
         * snapshot.  Preserve the historical no-file result (there is no
         * durable state to damage), but report uncertainty when an existing
         * order was protected. */
        char *saved = NULL;
        int load_rc = ip_gamepad_order_load(&saved);
        if (load_rc != 0) {
            free(saved);
            return load_rc;
        }
        bool had_saved_order = saved && saved[0] != '\0';
        free(saved);
        return had_saved_order ? -EAGAIN : 0;
    }

    /* Replace the persisted order on top of the current on-disk assignment
     * entries in one serialized transaction. */
    return cbx_assignments_transaction(txn_replace_order, &args, NULL);
}

int
ip_gamepad_order_load(char **out_csv)
{
    if (!out_csv)
        return -EINVAL;

    *out_csv = NULL;

    cbx_assignments a;
    cbx_assignments_init(&a);
    int rc = cbx_assignments_load(&a);
    if (rc != 0)
        return rc;

    /* Build a CSV string from the gamepad_order entries.  The load path
     * already validated every id via cbx_assignments_validate, so this is a
     * single pass with exact worst-case sizing (id + separator each). */
    size_t total_len = 1;  /* NUL terminator */
    for (int i = 0; i < a.gamepad_order_count; i++)
        total_len += strlen(a.gamepad_order[i]) + 1;

    char *csv = malloc(total_len);
    if (!csv)
        return -ENOMEM;

    size_t offset = 0;
    for (int i = 0; i < a.gamepad_order_count; i++) {
        if (offset > 0)
            csv[offset++] = ',';
        size_t id_len = strlen(a.gamepad_order[i]);
        memcpy(csv + offset, a.gamepad_order[i], id_len);
        offset += id_len;
    }
    csv[offset] = '\0';

    *out_csv = csv;
    return 0;
}