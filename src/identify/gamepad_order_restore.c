/*
 * gamepad_order_restore.c — GamepadOrder restoration after restart
 *                           (Task 27, SPEC §10.3 gap #2).
 *
 * After InputPlumber restart and re-enumeration, maps saved gamepad_order
 * IDs back to composite device paths via PersistentId queries, then
 * re-applies the order through ip_manager_set_gamepad_order().
 */
#include "gamepad_order_restore.h"

#include "dbus/ip_gamepad_order.h"  /* ip_gamepad_order_load */
#include "dbus/ip_manager.h"        /* ip_manager_set_gamepad_order */
#include "dbus/ip_composite.h"      /* ip_composite_get_persistent_id */
#include "config/config_assignments.h" /* CBX_MAX_ID_LEN */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Helpers -------------------------------------------------------------- */

/*
 * Split a CSV string into individual tokens, calling `cb` for each.
 * Tokens are trimmed of leading/trailing whitespace.  Empty tokens are
 * skipped.  The token string is NOT modified (we copy into a stack buffer).
 *
 * Returns the number of tokens processed, or -EINVAL if null args.
 */
static int
csv_for_each(const char *csv,
             int (*cb)(const char *token, void *ud),
             void *ud)
{
    if (!csv || !cb)
        return -EINVAL;

    int count = 0;
    const char *p = csv;

    while (*p) {
        /* Skip leading whitespace. */
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;

        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);

        /* Trim trailing whitespace. */
        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t'))
            len--;

        if (len > 0 && len < CBX_MAX_ID_LEN) {
            char token[CBX_MAX_ID_LEN];
            memcpy(token, p, len);
            token[len] = '\0';
            int rc = cb(token, ud);
            if (rc != 0)
                return rc;
            count++;
        }

        if (!comma)
            break;
        p = comma + 1;
    }

    return count;
}

/* --- ID-to-path mapping --------------------------------------------------- */

/*
 * Context for mapping saved IDs to composite paths.
 */
struct map_ctx {
    const ip_dbus_backend *backend;
    ip_bus_handle          bus;
    const cbx_device_model *model;
    char   *paths_csv;          /* output buffer */
    size_t  paths_csv_len;
    size_t  paths_csv_pos;      /* current write position */
    int     restored_count;
    int     skipped_count;
};

/*
 * Try to find a composite whose PersistentId matches `saved_id`.
 * If found, append its path to the output CSV.
 */
static int
map_one_id(const char *saved_id, void *ud)
{
    struct map_ctx *ctx = ud;

    for (int i = 0; i < ctx->model->composite_count; i++) {
        const char *comp_path = ctx->model->composites[i].path;

        char *persistent_id = NULL;
        int rc = ip_composite_get_persistent_id(ctx->backend,
                                                ctx->bus,
                                                comp_path,
                                                &persistent_id);
        if (rc != 0 || !persistent_id) {
            free(persistent_id);
            continue;   /* DBus error — skip this composite. */
        }

        if (strcmp(persistent_id, saved_id) == 0) {
            /* Match found — append path to CSV. */
            free(persistent_id);
            size_t plen = strlen(comp_path);

            /* Need room for path + comma (or NUL). */
            bool need_comma = (ctx->paths_csv_pos > 0);
            size_t need = plen + 1 + (need_comma ? 1 : 0);
            if (ctx->paths_csv_pos + need > ctx->paths_csv_len) {
                return -ENOSPC;   /* Output buffer too small. */
            }

            if (need_comma) {
                ctx->paths_csv[ctx->paths_csv_pos++] = ',';
            }
            memcpy(ctx->paths_csv + ctx->paths_csv_pos, comp_path, plen);
            ctx->paths_csv_pos += plen;
            ctx->paths_csv[ctx->paths_csv_pos] = '\0';

            ctx->restored_count++;
            return 0;   /* Found — stop searching composites. */
        }

        free(persistent_id);
    }

    /* No matching composite found — stale ID. */
    ctx->skipped_count++;
    return 0;
}

/* --- Public API ----------------------------------------------------------- */

int
cbx_gamepad_order_map_ids(const ip_dbus_backend *backend,
                           ip_bus_handle bus,
                           const cbx_device_model *model,
                           const char *saved_ids_csv,
                           char *out_paths_csv,
                           size_t paths_csv_len,
                           int *out_restored_count,
                           int *out_skipped_count)
{
    if (!backend || !bus || !model || !saved_ids_csv || !out_paths_csv)
        return -EINVAL;
    if (paths_csv_len == 0)
        return -EINVAL;

    out_paths_csv[0] = '\0';

    struct map_ctx ctx = {
        .backend         = backend,
        .bus             = bus,
        .model           = model,
        .paths_csv       = out_paths_csv,
        .paths_csv_len   = paths_csv_len,
        .paths_csv_pos   = 0,
        .restored_count  = 0,
        .skipped_count   = 0,
    };

    int rc = csv_for_each(saved_ids_csv, map_one_id, &ctx);
    if (rc < 0)
        return rc;

    if (out_restored_count)
        *out_restored_count = ctx.restored_count;
    if (out_skipped_count)
        *out_skipped_count = ctx.skipped_count;

    return 0;
}

int
cbx_gamepad_order_restore(const ip_dbus_backend *backend,
                           ip_bus_handle bus,
                           const cbx_device_model *model,
                           int *out_restored_count,
                           int *out_skipped_count)
{
    if (!backend || !bus || !model)
        return -EINVAL;

    if (out_restored_count)
        *out_restored_count = 0;
    if (out_skipped_count)
        *out_skipped_count = 0;

    /* 1. Load saved gamepad_order IDs. */
    char *saved_ids_csv = NULL;
    int rc = ip_gamepad_order_load(&saved_ids_csv);
    if (rc != 0)
        return rc;

    if (!saved_ids_csv) {
        return -ENOMEM;
    }

    /* Empty saved order → nothing to restore. */
    if (saved_ids_csv[0] == '\0') {
        free(saved_ids_csv);
        return -ENOENT;
    }

    /* 2. Map saved IDs to composite paths. */
    char paths_csv[CBX_MAX_PATH_LEN * CBX_MAX_GAMEPAD_ORDER];
    int restored = 0;
    int skipped = 0;

    rc = cbx_gamepad_order_map_ids(backend, bus, model,
                                    saved_ids_csv,
                                    paths_csv, sizeof(paths_csv),
                                    &restored, &skipped);
    free(saved_ids_csv);
    if (rc != 0)
        return rc;

    /* 3. Re-apply the order via the DBus setter.
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