/*
 * assign_persist.h — Atomic assignment persistence (Task 26, SPEC §6.2).
 *
 * Load-modify-save operations on assignments.yaml that are atomic
 * from the caller's perspective. Each function loads the current
 * state, applies the change, validates, and saves atomically
 * (temp file + rename via cbx_assignments_save).
 *
 * The "auto-assign" path is the atomic lowest-free-slot computation:
 * load → find free slot → add → save in one call, minimizing the race
 * window for simultaneous connects.
 */
#ifndef CBX_ASSIGN_PERSIST_H
#define CBX_ASSIGN_PERSIST_H

#include <stddef.h>

#include "config/config_assignments.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Add or update an assignment atomically.
 * If the ID already exists, updates slot + profile in place.
 * Otherwise adds a new entry at the end.
 *
 * @param id      Identity ID string (validated by cbx_validate_id).
 * @param slot    New slot (>= 0).
 * @param profile New profile name (may be empty for "no profile").
 * @return 0 on success; -EINVAL if invalid args (null/invalid id, slot < 0);
 *         -ENOSPC if table full (new ID, no room);
 *         negative errno from load/save on I/O error.
 */
int cbx_assign_persist_set(const char *id, int slot, const char *profile);

/*
 * Update the slot for an existing assignment.
 *
 * @param id       Identity ID string.
 * @param new_slot New slot (>= 0).
 * @return 0 on success; -ENOENT if ID not found; -EINVAL if invalid args.
 */
int cbx_assign_persist_set_slot(const char *id, int new_slot);

/*
 * Update the profile for an existing assignment.
 *
 * @param id      Identity ID string.
 * @param profile New profile name (may be empty).
 * @return 0 on success; -ENOENT if ID not found; -EINVAL if invalid args.
 */
int cbx_assign_persist_set_profile(const char *id, const char *profile);

/*
 * Remove an assignment by ID.
 * If the ID is not found, returns 0 (idempotent).
 *
 * @param id Identity ID string.
 * @return 0 on success; -EINVAL if null.
 */
int cbx_assign_persist_remove(const char *id);

/*
 * Auto-assign a new controller atomically (SPEC §6.2).
 *
 * Loads current assignments, looks up the ID. If found, returns the
 * existing slot + profile. If not found, computes the lowest free slot,
 * adds a default-profile assignment, and saves atomically.
 *
 * This is the atomic lowest-free-slot computation: the entire
 * load → compute → insert → save happens in one call.
 *
 * @param id         Identity ID string.
 * @param max_slots  Maximum number of slots.
 * @param out_slot   Output: assigned slot.
 * @param out_profile Output buffer for profile name (may be NULL).
 * @param profile_len Length of out_profile buffer (if out_profile != NULL).
 * @return 0 if existing found; 1 if newly assigned;
 *         -ENOENT if no free slot;
 *         -EINVAL if invalid args;
 *         negative errno from load/save on I/O error.
 */
int cbx_assign_persist_auto_assign(const char *id, int max_slots,
                                    int *out_slot, char *out_profile,
                                    size_t profile_len);

#ifdef __cplusplus
}
#endif

#endif /* CBX_ASSIGN_PERSIST_H */