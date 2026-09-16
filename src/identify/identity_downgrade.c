/*
 * identity_downgrade.c — Identity downgrade detection (Task 27, SPEC §6.3).
 *
 * Pure functions — no I/O, no DBus.  The caller loads assignments,
 * extracts the new identity, and calls these functions to determine
 * whether to use the new identity as-is or fall back to ORDER:n.
 */
#include "identity_downgrade.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "assign.h"          /* cbx_assign_lookup */

/* --- Helpers -------------------------------------------------------------- */

/*
 * Build an ORDER:n identity string in the given buffer.
 * Returns 0 on success, -EINVAL if buf is NULL, -ENOSPC if too small.
 */
static int
build_order_identity(int connection_order, char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0)
        return -EINVAL;
    if (connection_order < 0) {
        /* No connection order available — empty identity. */
        buf[0] = '\0';
        return 0;
    }
    int n = snprintf(buf, buf_len, "ORDER:%d", connection_order);
    if (n < 0 || (size_t)n >= buf_len)
        return -ENOSPC;
    return 0;
}

/* --- Public API ----------------------------------------------------------- */

int
cbx_downgrade_check(const char *old_id,
                     const cbx_identity *new_ident,
                     int connection_order,
                     cbx_identity *out_ident)
{
    if (!new_ident || !out_ident)
        return -EINVAL;

    /* Copy new identity as default result. */
    cbx_identity_init(out_ident);
    memcpy(out_ident, new_ident, sizeof(*new_ident));

    if (new_ident->layer == CBX_IDENTITY_LAYER_NONE)
        return -ENOENT;

    /* No old ID → no downgrade possible. */
    if (!old_id || old_id[0] == '\0')
        return 0;

    cbx_identity_layer old_layer = cbx_identity_parse_layer(old_id);
    if (old_layer == CBX_IDENTITY_LAYER_NONE)
        return 0;   /* Invalid old ID — can't compare, no downgrade. */

    if (!cbx_identity_is_downgrade(old_layer, new_ident->layer))
        return 0;   /* Not a downgrade. */

    /* Downgrade detected — fall back to ORDER:n. */
    cbx_identity_init(out_ident);
    int rc = build_order_identity(connection_order,
                                   out_ident->id,
                                   sizeof(out_ident->id));
    if (rc != 0)
        return rc;

    out_ident->layer = CBX_IDENTITY_LAYER_ORDER;
    return 1;   /* Downgrade detected. */
}

bool
cbx_downgrade_find_stronger(const cbx_assignments *a,
                              cbx_identity_layer new_layer,
                              char *out_id, size_t out_len)
{
    /* Backwards-compatible wrapper: no claimed set means every stronger id is
     * treated as potentially related.  Production callers use the _unclaimed
     * variant so a stronger id already owned by another live controller is
     * never mistaken for a downgrade of this device. */
    return cbx_downgrade_find_stronger_unclaimed(a, new_layer, NULL,
                                                 out_id, out_len);
}

/* Test membership of `id` in a comma-separated claimed list. */
static bool
csv_contains_id(const char *csv, const char *id)
{
    if (!csv || !id || !*id)
        return false;

    const char *p = csv;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        if (!*p)
            break;
        const char *end = strchr(p, ',');
        if (!end)
            end = p + strlen(p);
        const char *trim_end = end;
        while (trim_end > p && (trim_end[-1] == ' ' || trim_end[-1] == '\t'))
            trim_end--;
        if ((size_t)(trim_end - p) == strlen(id) &&
            strncmp(p, id, (size_t)(trim_end - p)) == 0)
            return true;
        p = *end ? end + 1 : end;
    }
    return false;
}

bool
cbx_downgrade_find_stronger_unclaimed(const cbx_assignments *a,
                                       cbx_identity_layer new_layer,
                                       const char *claimed_ids_csv,
                                       char *out_id, size_t out_len)
{
    if (!a || !out_id || out_len == 0)
        return false;

    if (new_layer == CBX_IDENTITY_LAYER_NONE)
        return false;

    cbx_identity_layer strongest = CBX_IDENTITY_LAYER_ORDER; /* weakest */
    int  strongest_idx = -1;

    for (int i = 0; i < a->assignment_count; i++) {
        cbx_identity_layer layer =
            cbx_identity_parse_layer(a->assignments[i].id);
        if (layer == CBX_IDENTITY_LAYER_NONE)
            continue;
        /* A stronger id already claimed by a live controller is unrelated. */
        if (csv_contains_id(claimed_ids_csv, a->assignments[i].id))
            continue;
        /* Looking for a STRONGER (lower) layer than new_layer. */
        if ((int)layer < (int)new_layer && (int)layer < (int)strongest) {
            strongest = layer;
            strongest_idx = i;
        }
    }

    if (strongest_idx < 0)
        return false;

    /* Copy the strongest id to output. */
    const char *src = a->assignments[strongest_idx].id;
    size_t len = strlen(src);
    if (len >= out_len)
        return false;   /* Buffer too small. */
    memcpy(out_id, src, len + 1);
    return true;
}

int
cbx_downgrade_resolve(const cbx_assignments *a,
                       const char *claimed_ids_csv,
                       const cbx_identity *new_ident,
                       int connection_order,
                       cbx_identity *out_ident)
{
    if (!new_ident || !out_ident)
        return -EINVAL;

    /* Copy new identity as default result. */
    cbx_identity_init(out_ident);
    memcpy(out_ident, new_ident, sizeof(*new_ident));

    if (new_ident->layer == CBX_IDENTITY_LAYER_NONE)
        return -ENOENT;

    /* If the new ID matches an existing assignment, no downgrade. */
    if (a && new_ident->id[0] != '\0') {
        cbx_assignment found;
        int rc = cbx_assign_lookup(a, new_ident->id, &found);
        if (rc == 0)
            return 0;   /* Existing match — no downgrade. */
    }

    /* Scan for a stronger *unclaimed* assignment.  A stronger id already
     * matched to another live controller is unrelated and is not inferred as
     * a downgrade of this device. */
    char stronger_id[CBX_IDENTITY_MAX_LEN];
    bool found = cbx_downgrade_find_stronger_unclaimed(a, new_ident->layer,
                                                        claimed_ids_csv,
                                                        stronger_id,
                                                        sizeof(stronger_id));
    if (!found)
        return 0;   /* No related stronger assignment — no downgrade. */

    /* Downgrade (uncertain) detected — fall back to ORDER:n. */
    cbx_identity_init(out_ident);
    int rc = build_order_identity(connection_order,
                                   out_ident->id,
                                   sizeof(out_ident->id));
    if (rc != 0)
        return rc;

    out_ident->layer = CBX_IDENTITY_LAYER_ORDER;
    return 1;   /* Downgrade detected. */
}