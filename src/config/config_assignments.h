/*
 * config_assignments.h — assignments.yaml read/write for Controller-Box.
 *
 * Loads and saves the controller-to-slot assignment table and persisted
 * gamepad order (SPEC §7.4, gap #2 workaround).
 *
 * File location: ~/.config/controller-box/assignments.yaml
 *
 * Security constraints (from plan security review):
 *   - YAML parsed via libyaml with max depth 50, max document size 1 MB,
 *     no custom tags.
 *   - Atomic write: temp file (mkstemp) + rename, file mode 0600.
 *   - Profile names validated against ^[a-zA-Z0-9_-]+$.
 *   - ID format validated (BT:xx:xx:xx:xx:xx:xx, USB:xxxxx,
 *     USB:phys:xxxxx, ORDER:n).
 */
#ifndef CBX_CONFIG_ASSIGNMENTS_H
#define CBX_CONFIG_ASSIGNMENTS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of assignment entries. */
#define CBX_MAX_ASSIGNMENTS 32

/* Maximum number of gamepad order entries. */
#define CBX_MAX_GAMEPAD_ORDER 16

/* Maximum length of an assignment id string (NUL-terminated). */
#define CBX_MAX_ID_LEN 128

/* Maximum length of a profile name (NUL-terminated). */
#define CBX_MAX_PROFILE_LEN 64

/* A single controller-to-slot assignment (SPEC §7.4). */
typedef struct {
    char id[CBX_MAX_ID_LEN];       /* e.g. "BT:AB:CD:01:EF:23" */
    int  slot;                      /* 0-based player slot, >= 0 */
    char profile[CBX_MAX_PROFILE_LEN]; /* profile filename, no extension */
} cbx_assignment;

/* The full assignments file content (SPEC §7.4). */
typedef struct {
    cbx_assignment assignments[CBX_MAX_ASSIGNMENTS];
    int             assignment_count;

    /* Persisted gamepad order (gap #2 workaround, §10.3).
     * Each entry is an assignment id; the order is the desired GamepadOrder. */
    char gamepad_order[CBX_MAX_GAMEPAD_ORDER][CBX_MAX_ID_LEN];
    int  gamepad_order_count;
} cbx_assignments;

/*
 * Initialize an empty assignments struct (zero assignments, empty order).
 */
void cbx_assignments_init(cbx_assignments *a);

/*
 * Load assignments from ~/.config/controller-box/assignments.yaml.
 * If the file does not exist, returns an empty struct and 0.
 * Missing fields are left as defaults (empty/zero).
 *
 * @param a  Output struct (overwritten).
 * @return 0 on success; negative errno on error.
 */
int cbx_assignments_load(cbx_assignments *a);

/*
 * Validate assignments strictly:
 *   - Each id matches the allowed format (BT:MAC, USB:serial,
 *     USB:phys:path, ORDER:n).
 *   - Each slot is >= 0.
 *   - Each profile matches ^[a-zA-Z0-9_-]+$ or is empty (empty = no profile).
 *   - Counts within bounds.
 *
 * @return 0 if valid; -EINVAL if invalid.
 */
int cbx_assignments_validate(const cbx_assignments *a);

/*
 * Save assignments to ~/.config/controller-box/assignments.yaml.
 * Validates first (returns -EINVAL if invalid).
 * Writes atomically: temp file + rename, mode 0600.
 *
 * @return 0 on success; negative errno on error.
 */
int cbx_assignments_save(const cbx_assignments *a);

/*
 * Validate an id string format.
 * Allowed formats:
 *   BT:xx:xx:xx:xx:xx:xx   (hex pairs, case-insensitive)
 *   USB:xxxxx               (serial: non-empty alnum/underscore/dash)
 *   USB:phys:xxxxx          (phys path)
 *   ORDER:n                  (n is a non-negative integer)
 *
 * @return true if valid, false otherwise.
 */
bool cbx_validate_id(const char *id);

/*
 * Validate a profile name (must match ^[a-zA-Z0-9_-]+$ or be empty).
 *
 * @return true if valid, false otherwise.
 */
bool cbx_validate_profile(const char *profile);

#ifdef __cplusplus
}
#endif

#endif /* CBX_CONFIG_ASSIGNMENTS_H */