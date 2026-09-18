/*
 * assign_persist.c — Atomic assignment persistence (Task 26, SPEC §6.2).
 *
 * Each function runs a serialized read-modify-write transaction on
 * assignments.yaml through cbx_assignments_transaction(): the shared
 * cross-process config lock is held across load → mutate → save so two
 * independent writers (Manager, overlay, gamepad-order sync) cannot interleave
 * and erase each other's updates.
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

/* --- Transaction callbacks ------------------------------------------------ */

typedef struct {
    const char *id;
    int         slot;
    const char *profile;
} set_args;

static int txn_set(cbx_assignments *a, void *userdata)
{
    set_args *args = userdata;
    int rc = assign_set_in_memory(a, args->id, args->slot, args->profile);
    return rc < 0 ? rc : 0;  /* 0 commits (insert or update) */
}

typedef struct {
    const char *id;
    int         new_slot;
} slot_args;

static int txn_set_slot(cbx_assignments *a, void *userdata)
{
    slot_args *args = userdata;
    int idx = cbx_assign_find_index(a, args->id);
    if (idx < 0)
        return -ENOENT;
    a->assignments[idx].slot = args->new_slot;
    return 0;
}

typedef struct {
    const char *id;
    const char *profile;
} profile_args;

static int txn_set_profile(cbx_assignments *a, void *userdata)
{
    profile_args *args = userdata;
    int idx = cbx_assign_find_index(a, args->id);
    if (idx < 0)
        return -ENOENT;
    strncpy(a->assignments[idx].profile,
            args->profile ? args->profile : "",
            CBX_MAX_PROFILE_LEN - 1);
    a->assignments[idx].profile[CBX_MAX_PROFILE_LEN - 1] = '\0';
    return 0;
}

static int txn_remove(cbx_assignments *a, void *userdata)
{
    const char *id = userdata;
    int idx = cbx_assign_find_index(a, id);
    if (idx < 0)
        return 1;  /* idempotent: nothing to change, skip the save */

    for (int i = idx; i < a->assignment_count - 1; i++)
        a->assignments[i] = a->assignments[i + 1];
    a->assignment_count--;
    return 0;
}

typedef struct {
    const char *id;
    int         max_slots;
    int        *out_slot;
    char       *out_profile;
    size_t      profile_len;
    int         result;      /* public 0 = existing, 1 = newly assigned */
} auto_args;

static void store_profile(auto_args *args, const char *profile)
{
    if (args->out_profile && args->profile_len > 0) {
        strncpy(args->out_profile, profile, args->profile_len - 1);
        args->out_profile[args->profile_len - 1] = '\0';
    }
}

static int txn_auto_assign(cbx_assignments *a, void *userdata)
{
    auto_args *args = userdata;

    /* Existing lookup */
    cbx_assignment existing;
    int rc = cbx_assign_lookup(a, args->id, &existing);
    if (rc == 0) {
        *args->out_slot = existing.slot;
        store_profile(args, existing.profile);
        args->result = 0;
        return 1;  /* existing found — no write */
    }

    /* Compute lowest free slot */
    int slot = cbx_assign_lowest_free_slot(a, args->max_slots);
    if (slot < 0)
        return -ENOENT;  /* all slots occupied */

    rc = assign_set_in_memory(a, args->id, slot, CBX_DEFAULT_PROFILE);
    if (rc < 0)
        return rc;

    *args->out_slot = slot;
    store_profile(args, CBX_DEFAULT_PROFILE);
    args->result = 1;
    return 0;  /* newly assigned — commit */
}

/* --- Public API ----------------------------------------------------------- */

int cbx_assign_persist_set(const char *id, int slot, const char *profile)
{
    if (!id)
        return -EINVAL;
    if (!cbx_validate_id(id) || slot < 0)
        return -EINVAL;
    if (!cbx_validate_profile(profile ? profile : ""))
        return -EINVAL;

    set_args args = { id, slot, profile };
    return cbx_assignments_transaction(txn_set, &args, NULL);
}

int cbx_assign_persist_set_slot(const char *id, int new_slot)
{
    if (!id || new_slot < 0)
        return -EINVAL;

    slot_args args = { id, new_slot };
    return cbx_assignments_transaction(txn_set_slot, &args, NULL);
}

int cbx_assign_persist_set_profile(const char *id, const char *profile)
{
    if (!id)
        return -EINVAL;

    if (profile && !cbx_validate_profile(profile))
        return -EINVAL;

    profile_args args = { id, profile };
    return cbx_assignments_transaction(txn_set_profile, &args, NULL);
}

int cbx_assign_persist_remove(const char *id)
{
    if (!id)
        return -EINVAL;

    int rc = cbx_assignments_transaction(txn_remove, (void *)id, NULL);
    return rc < 0 ? rc : 0;
}

int cbx_assign_persist_auto_assign(const char *id, int max_slots,
                                    int *out_slot, char *out_profile,
                                    size_t profile_len)
{
    if (!id || !out_slot || max_slots <= 0)
        return -EINVAL;

    auto_args args = { id, max_slots, out_slot, out_profile, profile_len, 0 };
    int rc = cbx_assignments_transaction(txn_auto_assign, &args, NULL);
    if (rc < 0)
        return rc;
    return args.result;
}
