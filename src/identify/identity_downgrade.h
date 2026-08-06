/*
 * identity_downgrade.h — Identity downgrade detection (Task 27, SPEC §6.3).
 *
 * When a controller reconnects with a weaker identity than previously
 * stored (e.g. USB serial becomes unavailable after a port change, so
 * the identity falls from USB:SN to USB:phys), the GUI must detect the
 * downgrade and fall back gracefully to an ORDER:n identity instead of
 * creating a permanent assignment with an unstable identifier.
 *
 * Two API levels:
 *   - cbx_downgrade_check():  Given old + new identity, detect downgrade.
 *   - cbx_downgrade_resolve(): Scan assignments table for stronger-layer
 *     IDs that don't match the new identity, and if found, fall back.
 *
 * These are pure functions — no I/O, no DBus.  The caller loads
 * assignments and extracts the new identity, then calls these functions.
 */
#ifndef CBX_IDENTITY_DOWNGRADE_H
#define CBX_IDENTITY_DOWNGRADE_H

#include <stdbool.h>
#include <stddef.h>

#include "identity.h"            /* cbx_identity, cbx_identity_layer */
#include "config/config_assignments.h" /* cbx_assignments */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Detect identity downgrade and provide fallback.
 *
 * Compares the old stored identity layer against the new identity layer.
 * If the new layer is weaker (higher number), it's a downgrade — the
 * caller should use the ORDER:n fallback identity instead of the new
 * (weak) identity to avoid creating a permanent assignment with an
 * unstable identifier.
 *
 * @param old_id          Previously stored identity ID string (may be NULL
 *                        if unknown — treated as NONE, no downgrade).
 * @param new_ident       Newly extracted identity (must not be NULL).
 * @param connection_order Current connection order (>= 0 for ORDER:n
 *                        fallback; if < 0, fallback ID will be empty).
 * @param out_ident       Output: the identity to use.  If no downgrade,
 *                        this is a copy of *new_ident.  If downgrade,
 *                        this is ORDER:n (or empty if connection_order < 0).
 * @return 0 if no downgrade (out_ident = *new_ident);
 *         1 if downgrade detected (out_ident = ORDER:n fallback);
 *         -EINVAL if null args;
 *         -ENOENT if new_ident layer is NONE.
 */
int cbx_downgrade_check(const char *old_id,
                        const cbx_identity *new_ident,
                        int connection_order,
                        cbx_identity *out_ident);

/*
 * Resolve identity downgrade by scanning the assignments table.
 *
 * Given the loaded assignments and a newly extracted identity, determines
 * whether to use the new identity as-is or fall back to ORDER:n.
 *
 * A downgrade is detected when:
 *   1. The new identity's ID does NOT match any existing assignment.
 *   2. There EXISTS an assignment whose ID is at a STRONGER layer than
 *      the new identity (i.e. lower layer number).
 *
 * The rationale: if a controller reconnects with a weaker identity and
 * no assignment matches, it may be the same physical device that
 * previously had a stronger identity.  Falling back to ORDER:n avoids
 * creating a duplicate permanent assignment with an unstable identifier.
 *
 * @param a               Loaded assignments (may be NULL — treated as empty).
 * @param new_ident       Newly extracted identity.
 * @param connection_order Current connection order.
 * @param out_ident       Output: the identity to use.
 * @return 0 if no downgrade (use new_ident as-is);
 *         1 if downgrade detected (out_ident = ORDER:n);
 *         -EINVAL if null args;
 *         -ENOENT if new_ident layer is NONE.
 */
int cbx_downgrade_resolve(const cbx_assignments *a,
                          const cbx_identity *new_ident,
                          int connection_order,
                          cbx_identity *out_ident);

/*
 * Find the strongest stored identity that doesn't match the new identity.
 *
 * Scans all assignments for IDs at a stronger (lower) layer than the new
 * identity.  Returns the strongest such ID (lowest layer number).
 *
 * @param a              Loaded assignments.
 * @param new_layer      New identity layer.
 * @param out_id         Output buffer for the strongest non-matching ID.
 * @param out_len        Size of out_id buffer.
 * @return true if a stronger-layer assignment was found;
 *         false if none found or null/invalid args.
 */
bool cbx_downgrade_find_stronger(const cbx_assignments *a,
                                  cbx_identity_layer new_layer,
                                  char *out_id, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* CBX_IDENTITY_DOWNGRADE_H */