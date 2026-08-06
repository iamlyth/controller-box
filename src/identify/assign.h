/*
 * assign.h — Assignment lookup and default assignment (Task 26, SPEC §6.2).
 *
 * Given a controller's extracted identity (Task 25), looks up the
 * preferred slot + profile in the assignments table (Task 6).
 * If not found, computes the lowest unoccupied slot and creates a
 * default assignment.
 *
 * These are pure functions operating on an in-memory cbx_assignments
 * struct — no file I/O. Persistence is in assign_persist.h.
 */
#ifndef CBX_ASSIGN_H
#define CBX_ASSIGN_H

#include <stdbool.h>
#include <stddef.h>

#include "config/config_assignments.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default profile name used for new controllers (SPEC §5.3 — built-in). */
#define CBX_DEFAULT_PROFILE "default"

/*
 * Find the index of an assignment by ID in the assignments table.
 *
 * @param a  Loaded assignments.
 * @param id Identity ID string (e.g. "BT:AB:CD:01:EF:23").
 * @return Index in a->assignments[], or -1 if not found / null args.
 */
int cbx_assign_find_index(const cbx_assignments *a, const char *id);

/*
 * Look up an assignment by identity ID.
 *
 * @param a    Loaded assignments.
 * @param id   Identity ID string.
 * @param out  Output assignment (filled if found).
 * @return 0 if found; -ENOENT if not found; -EINVAL if null args.
 */
int cbx_assign_lookup(const cbx_assignments *a, const char *id,
                       cbx_assignment *out);

/*
 * Check whether a slot is occupied in the given assignments.
 *
 * @param a    Loaded assignments.
 * @param slot Slot index to check (0-based).
 * @return true if slot is occupied, false otherwise (or null args).
 */
bool cbx_assign_slot_occupied(const cbx_assignments *a, int slot);

/*
 * Compute the lowest unoccupied slot (0-based).
 *
 * Scans all assignments and finds the smallest non-negative integer
 * not used as a slot. This is the "atomic" computation: given a
 * snapshot of assignments, the result is deterministic.
 *
 * @param a         Loaded assignments.
 * @param max_slots Maximum number of slots (from settings).
 * @return Lowest free slot index (0..max_slots-1), or -1 if all occupied.
 */
int cbx_assign_lowest_free_slot(const cbx_assignments *a, int max_slots);

/*
 * Create a default assignment for a new controller.
 * Uses the given slot and CBX_DEFAULT_PROFILE.
 *
 * @param id    Identity ID string (will be copied, validated by cbx_validate_id).
 * @param slot  Slot to assign (>= 0).
 * @param out   Output assignment (filled).
 * @return 0 on success; -EINVAL if null args or invalid id/slot.
 */
int cbx_assign_make_default(const char *id, int slot,
                              cbx_assignment *out);

/*
 * Resolve a controller's assignment on connect (SPEC §6.2).
 *
 * If the ID is found in the assignments table, returns the stored
 * slot + profile (existing preference). Otherwise, computes the
 * lowest free slot and creates a default assignment.
 *
 * @param a         Loaded assignments.
 * @param id        Identity ID string.
 * @param max_slots Maximum number of slots.
 * @param out       Output assignment (filled).
 * @return 0 if existing found; 1 if default created;
 *         -ENOENT if no free slot;
 *         -EINVAL if null args.
 */
int cbx_assign_resolve(const cbx_assignments *a, const char *id,
                        int max_slots, cbx_assignment *out);

#ifdef __cplusplus
}
#endif

#endif /* CBX_ASSIGN_H */