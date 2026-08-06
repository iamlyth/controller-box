/*
 * profile_validate.h — Profile validation: NES minimum binding set
 * (Task 38).
 *
 * A valid profile must bind at least: A, B, D-Pad Up, D-Pad Down,
 * D-Pad Left, D-Pad Right.  All other bindings are optional.
 * Rationale: this is the minimum set that can navigate any menu and
 * play any NES-class game; requiring it makes "broken profile" an
 * impossible state (SPEC §5.4).
 *
 * Validation is a hard gate before saving (Task 39).
 */
#ifndef CBX_PROFILE_VALIDATE_H
#define CBX_PROFILE_VALIDATE_H

#include <stddef.h>
#include <stdbool.h>

#include "config/config_profile.h"     /* cbx_profile */
#include "manager/profile_diagram.h"     /* cbx_diag_button */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  NES minimum required buttons                                       */
/* ------------------------------------------------------------------ */

/* Number of required buttons for NES minimum validation. */
#define CBX_NES_MINIMUM_COUNT 6

/*
 * Get the array of required buttons for NES minimum validation.
 * Returns a pointer to a static array of cbx_diag_button values.
 */
const cbx_diag_button *cbx_nes_minimum_buttons(void);

/* Get the canonical name for a required button at index i (0-based). */
const char *cbx_nes_minimum_button_name(int index);

/* ------------------------------------------------------------------ */
/*  Validation                                                        */
/* ------------------------------------------------------------------ */

/*
 * Check if a specific button is bound in the profile.
 * A button is "bound" if a mapping's source_event has a "button"
 * (or "axis") property whose value matches the button's canonical name.
 *
 * @param p   Profile to check.
 * @param btn Button to look for.
 * @return true if the button is bound, false otherwise.
 */
bool cbx_profile_has_binding(const cbx_profile *p, cbx_diag_button btn);

/*
 * Validate a profile against the NES minimum binding set.
 *
 * @param p           Profile to validate.
 * @param missing_buf  Output buffer for missing button names (comma-separated).
 *                     May be NULL if the caller doesn't need the list.
 * @param buflen       Size of missing_buf.
 * @return 0 if valid (all required buttons bound);
 *         -EINVAL if any required button is missing.
 *         -EINVAL if p is NULL.
 */
int cbx_profile_validate_nes_minimum(const cbx_profile *p,
                                        char *missing_buf, size_t buflen);

/*
 * Get the number of missing required bindings.
 *
 * @param p Profile to check.
 * @return Number of required buttons not bound (0 if valid).
 *         -EINVAL if p is NULL.
 */
int cbx_profile_validate_missing_count(const cbx_profile *p);

#ifdef __cplusplus
}
#endif

#endif /* CBX_PROFILE_VALIDATE_H */