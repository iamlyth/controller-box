/*
 * ip_gamepad_order.c — GamepadOrder persistence layer (Task 15, gap #2).
 *
 * See ip_gamepad_order.h for the gap #2 workaround description.
 */
#include "ip_gamepad_order.h"

#include "dbus/ip_composite.h"  /* ip_composite_get_persistent_id */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* --- Public API --------------------------------------------------------- */

/* Assignment-list gamepad_order replacement applied under the config lock. */
typedef struct {
    char ids[CBX_MAX_GAMEPAD_ORDER][CBX_MAX_ID_LEN];
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
    if (!backend || !model || !paths_csv)
        return -EINVAL;

    /* Resolve composite paths → PersistentId outside the config lock (DBus
     * latency must not serialize config writers).  Stale paths and IDs are
     * skipped and duplicate IDs are collapsed. */
    order_txn_args args;
    memset(&args, 0, sizeof(args));

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

        if (cbx_device_model_find_composite(model, path)) {
            char *persistent_id = NULL;
            int rc = ip_composite_get_persistent_id(backend, bus, path,
                                                    &persistent_id);
            if (rc == 0 && persistent_id) {
                if (cbx_validate_id(persistent_id) &&
                    args.count < CBX_MAX_GAMEPAD_ORDER) {
                    bool duplicate = false;
                    for (int i = 0; i < args.count; i++) {
                        if (strcmp(args.ids[i], persistent_id) == 0) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (!duplicate) {
                        strncpy(args.ids[args.count], persistent_id,
                                CBX_MAX_ID_LEN - 1);
                        args.ids[args.count][CBX_MAX_ID_LEN - 1] = '\0';
                        args.count++;
                    }
                }
            }
            free(persistent_id);
        }

        if (!comma)
            break;
        p = comma + 1;
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