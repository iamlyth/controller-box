/*
 * assign_persist.c — Atomic assignment persistence (Task 26, SPEC §6.2).
 *
 * Each function loads assignments.yaml, applies a change, validates,
 * and saves atomically (temp file + rename).
 */
#include "identify/assign_persist.h"
#include "identify/assign.h"

#include <errno.h>
#include <string.h>

/* Internal: set assignment in a loaded struct (no I/O).
 * Returns 0 on update, 1 on insert, -ENOSPC if full, -EINVAL if invalid. */
static int assign_set_in_memory(cbx_assignments *a, const char *id,
                                 int slot, const char *profile)
{
    if (!cbx_validate_id(id) || slot < 0)
        return -EINVAL;

    if (!cbx_validate_profile(profile ? profile : ""))
        return -EINVAL;

    /* Try to find existing entry to update */
    int idx = cbx_assign_find_index(a, id);
    if (idx >= 0) {
        a->assignments[idx].slot = slot;
        strncpy(a->assignments[idx].profile,
                profile ? profile : "",
                CBX_MAX_PROFILE_LEN - 1);
        a->assignments[idx].profile[CBX_MAX_PROFILE_LEN - 1] = '\0';
        return 0; /* updated */
    }

    /* Add new entry */
    if (a->assignment_count >= CBX_MAX_ASSIGNMENTS)
        return -ENOSPC;

    cbx_assignment *e = &a->assignments[a->assignment_count];
    memset(e, 0, sizeof(*e));
    strncpy(e->id, id, CBX_MAX_ID_LEN - 1);
    e->slot = slot;
    strncpy(e->profile, profile ? profile : "", CBX_MAX_PROFILE_LEN - 1);
    a->assignment_count++;
    return 1; /* inserted */
}

int cbx_assign_persist_set(const char *id, int slot, const char *profile)
{
    if (!id)
        return -EINVAL;

    cbx_assignments a;
    cbx_assignments_init(&a);

    int rc = cbx_assignments_load(&a);
    if (rc != 0)
        return rc;

    rc = assign_set_in_memory(&a, id, slot, profile);
    if (rc < 0)
        return rc;

    return cbx_assignments_save(&a);
}

int cbx_assign_persist_set_slot(const char *id, int new_slot)
{
    if (!id || new_slot < 0)
        return -EINVAL;

    cbx_assignments a;
    cbx_assignments_init(&a);

    int rc = cbx_assignments_load(&a);
    if (rc != 0)
        return rc;

    int idx = cbx_assign_find_index(&a, id);
    if (idx < 0)
        return -ENOENT;

    a.assignments[idx].slot = new_slot;
    return cbx_assignments_save(&a);
}

int cbx_assign_persist_set_profile(const char *id, const char *profile)
{
    if (!id)
        return -EINVAL;

    if (profile && !cbx_validate_profile(profile))
        return -EINVAL;

    cbx_assignments a;
    cbx_assignments_init(&a);

    int rc = cbx_assignments_load(&a);
    if (rc != 0)
        return rc;

    int idx = cbx_assign_find_index(&a, id);
    if (idx < 0)
        return -ENOENT;

    strncpy(a.assignments[idx].profile,
            profile ? profile : "",
            CBX_MAX_PROFILE_LEN - 1);
    a.assignments[idx].profile[CBX_MAX_PROFILE_LEN - 1] = '\0';
    return cbx_assignments_save(&a);
}

int cbx_assign_persist_remove(const char *id)
{
    if (!id)
        return -EINVAL;

    cbx_assignments a;
    cbx_assignments_init(&a);

    int rc = cbx_assignments_load(&a);
    if (rc != 0)
        return rc;

    int idx = cbx_assign_find_index(&a, id);
    if (idx < 0)
        return 0; /* idempotent */

    /* Shift remaining entries down */
    for (int i = idx; i < a.assignment_count - 1; i++)
        a.assignments[i] = a.assignments[i + 1];
    a.assignment_count--;

    return cbx_assignments_save(&a);
}

int cbx_assign_persist_auto_assign(const char *id, int max_slots,
                                    int *out_slot, char *out_profile,
                                    size_t profile_len)
{
    if (!id || !out_slot || max_slots <= 0)
        return -EINVAL;

    cbx_assignments a;
    cbx_assignments_init(&a);

    int rc = cbx_assignments_load(&a);
    if (rc != 0)
        return rc;

    /* Try existing lookup */
    cbx_assignment existing;
    rc = cbx_assign_lookup(&a, id, &existing);
    if (rc == 0) {
        *out_slot = existing.slot;
        if (out_profile && profile_len > 0) {
            strncpy(out_profile, existing.profile, profile_len - 1);
            out_profile[profile_len - 1] = '\0';
        }
        return 0; /* existing found */
    }

    /* Compute lowest free slot */
    int slot = cbx_assign_lowest_free_slot(&a, max_slots);
    if (slot < 0)
        return -ENOENT; /* all slots occupied */

    /* Create default and insert */
    cbx_assignment def;
    rc = cbx_assign_make_default(id, slot, &def);
    if (rc != 0)
        return rc;

    rc = assign_set_in_memory(&a, id, slot, CBX_DEFAULT_PROFILE);
    if (rc < 0)
        return rc;

    rc = cbx_assignments_save(&a);
    if (rc != 0)
        return rc;

    *out_slot = slot;
    if (out_profile && profile_len > 0) {
        strncpy(out_profile, CBX_DEFAULT_PROFILE, profile_len - 1);
        out_profile[profile_len - 1] = '\0';
    }
    return 1; /* newly assigned */
}