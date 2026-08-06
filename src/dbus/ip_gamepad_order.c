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

/* --- Internal helpers --------------------------------------------------- */

/*
 * Append an ID string to the gamepad_order array in cbx_assignments.
 * Returns 0 on success, -ENOSPC if the array is full.
 */
static int
append_order_id(cbx_assignments *a, const char *id)
{
    if (a->gamepad_order_count >= CBX_MAX_GAMEPAD_ORDER)
        return -ENOSPC;

    /* Validate the ID — skip invalid ones. */
    if (!cbx_validate_id(id))
        return -EINVAL;

    /* Check for duplicates (idempotent — don't add the same ID twice). */
    for (int i = 0; i < a->gamepad_order_count; i++) {
        if (strcmp(a->gamepad_order[i], id) == 0)
            return 0;  /* already present, not an error */
    }

    strncpy(a->gamepad_order[a->gamepad_order_count], id,
            CBX_MAX_ID_LEN - 1);
    a->gamepad_order[a->gamepad_order_count][CBX_MAX_ID_LEN - 1] = '\0';
    a->gamepad_order_count++;
    return 0;
}

/* --- Public API --------------------------------------------------------- */

int
ip_gamepad_order_save(const ip_dbus_backend *backend,
                       ip_bus_handle bus,
                       const cbx_device_model *model,
                       const char *paths_csv)
{
    if (!backend || !model || !paths_csv)
        return -EINVAL;

    /* Load existing assignments to preserve the assignment entries. */
    cbx_assignments a;
    cbx_assignments_init(&a);
    int rc = cbx_assignments_load(&a);
    if (rc != 0)
        return rc;

    /* Clear the existing gamepad_order — we replace it entirely. */
    a.gamepad_order_count = 0;

    /* Empty CSV = clear the order. */
    if (paths_csv[0] == '\0') {
        return cbx_assignments_save(&a);
    }

    /* Iterate the comma-separated composite paths. */
    const char *p = paths_csv;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);

        /* Copy the path fragment for lookup. */
        char path[CBX_MAX_PATH_LEN];
        if (len >= sizeof(path)) {
            /* Path too long — skip (stale/invalid). */
            if (!comma)
                break;
            p = comma + 1;
            continue;
        }
        memcpy(path, p, len);
        path[len] = '\0';

        /* Verify the path exists in the device model (skip stale). */
        if (!cbx_device_model_find_composite(model, path)) {
            if (!comma)
                break;
            p = comma + 1;
            continue;
        }

        /* Query PersistentId for this composite. */
        char *persistent_id = NULL;
        rc = ip_composite_get_persistent_id(backend, bus, path,
                                              &persistent_id);
        if (rc != 0 || !persistent_id) {
            /* Failed to get PersistentId — skip this entry. */
            free(persistent_id);
            if (!comma)
                break;
            p = comma + 1;
            continue;
        }

        /* Append the ID to the gamepad order. */
        (void)append_order_id(&a, persistent_id);
        free(persistent_id);

        if (!comma)
            break;
        p = comma + 1;
    }

    /* Save the updated assignments. */
    return cbx_assignments_save(&a);
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

    /* Build a CSV string from the gamepad_order entries. */
    /* Calculate the total length needed. */
    size_t total_len = 1;  /* NUL terminator */
    for (int i = 0; i < a.gamepad_order_count; i++) {
        if (!cbx_validate_id(a.gamepad_order[i]))
            continue;  /* skip invalid IDs */
        total_len += strlen(a.gamepad_order[i]);
        if (total_len > 1)
            total_len++;  /* comma separator */
    }

    char *csv = malloc(total_len);
    if (!csv)
        return -ENOMEM;

    csv[0] = '\0';
    size_t offset = 0;
    for (int i = 0; i < a.gamepad_order_count; i++) {
        if (!cbx_validate_id(a.gamepad_order[i]))
            continue;  /* skip invalid IDs */
        if (offset > 0) {
            csv[offset] = ',';
            offset++;
        }
        size_t id_len = strlen(a.gamepad_order[i]);
        memcpy(csv + offset, a.gamepad_order[i], id_len);
        offset += id_len;
    }
    csv[offset] = '\0';

    *out_csv = csv;
    return 0;
}