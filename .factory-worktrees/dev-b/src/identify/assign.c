/*
 * assign.c — Assignment lookup and default assignment (Task 26, SPEC §6.2).
 *
 * Pure functions operating on in-memory cbx_assignments.
 * No file I/O — persistence is in assign_persist.c.
 */
#include "identify/assign.h"

#include <errno.h>
#include <string.h>

int cbx_assign_find_index(const cbx_assignments *a, const char *id)
{
    if (!a || !id)
        return -1;

    for (int i = 0; i < a->assignment_count; i++) {
        if (strcmp(a->assignments[i].id, id) == 0)
            return i;
    }
    return -1;
}

int cbx_assign_lookup(const cbx_assignments *a, const char *id,
                       cbx_assignment *out)
{
    if (!a || !id || !out)
        return -EINVAL;

    int idx = cbx_assign_find_index(a, id);
    if (idx < 0)
        return -ENOENT;

    *out = a->assignments[idx];
    return 0;
}

bool cbx_assign_slot_occupied(const cbx_assignments *a, int slot)
{
    if (!a || slot < 0)
        return false;

    for (int i = 0; i < a->assignment_count; i++) {
        if (a->assignments[i].slot == slot)
            return true;
    }
    return false;
}

int cbx_assign_lowest_free_slot(const cbx_assignments *a, int max_slots)
{
    if (!a || max_slots <= 0)
        return -1;

    for (int s = 0; s < max_slots; s++) {
        if (!cbx_assign_slot_occupied(a, s))
            return s;
    }
    return -1;
}

int cbx_assign_make_default(const char *id, int slot,
                              cbx_assignment *out)
{
    if (!id || !out || slot < 0)
        return -EINVAL;

    if (!cbx_validate_id(id))
        return -EINVAL;

    memset(out, 0, sizeof(*out));
    strncpy(out->id, id, CBX_MAX_ID_LEN - 1);
    out->slot = slot;
    strncpy(out->profile, CBX_DEFAULT_PROFILE, CBX_MAX_PROFILE_LEN - 1);
    return 0;
}

int cbx_assign_resolve(const cbx_assignments *a, const char *id,
                        int max_slots, cbx_assignment *out)
{
    if (!a || !id || !out)
        return -EINVAL;

    /* Try existing lookup first */
    int rc = cbx_assign_lookup(a, id, out);
    if (rc == 0)
        return 0; /* found existing */

    /* Not found — create default with lowest free slot */
    int slot = cbx_assign_lowest_free_slot(a, max_slots);
    if (slot < 0)
        return -ENOENT; /* all slots occupied */

    rc = cbx_assign_make_default(id, slot, out);
    if (rc != 0)
        return rc;

    return 1; /* default created */
}