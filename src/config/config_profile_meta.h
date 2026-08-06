/*
 * config_profile_meta.h — Profile metadata sidecar (§7.5) read/write.
 *
 * Each profile can have an optional *.meta.yaml sidecar file stored in
 * ~/.config/controller-box/profile-metadata/.  The sidecar overrides display
 * properties used by the GUI:
 *
 *   display_name:  "Fighting — No Triggers"
 *   icon:          gamepad            # built-in icon name or absolute path
 *   display_order: 2
 *   description:   "Triggers disabled, L/R bumpers only"
 *
 * If a sidecar is absent, the GUI falls back to the name/description from the
 * profile YAML and the default icon.
 *
 * Security constraints (from plan security review):
 *   - Filename validated against ^[a-zA-Z0-9_-]+$
 *   - Paths canonicalized with realpath() and verified within expected base dir
 *   - File opens use O_NOFOLLOW to reject symlink attacks
 *   - Atomic write: temp file (mkstemp) + rename, mode 0600
 *   - YAML parsed via libyaml: max depth 50, max doc size 1 MB, no custom tags
 */
#ifndef CBX_CONFIG_PROFILE_META_H
#define CBX_CONFIG_PROFILE_META_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum length of sidecar string fields. */
#define CBX_META_NAME_LEN 256
#define CBX_META_ICON_LEN 256

/* Profile metadata sidecar (§7.5).
 * has_* flags distinguish "field not present" from "field set to empty/zero". */
typedef struct {
    char display_name[CBX_META_NAME_LEN];
    char icon[CBX_META_ICON_LEN];
    int  display_order;
    char description[CBX_META_NAME_LEN];

    bool has_display_name;
    bool has_icon;
    bool has_display_order;
    bool has_description;
} cbx_profile_meta;

/* Initialize a metadata struct to all-zero / unset. */
void cbx_profile_meta_init(cbx_profile_meta *m);

/*
 * Load a sidecar from a file path (opened with O_NOFOLLOW).
 * Initializes the struct first.  If the file does not exist, returns 0
 * with all has_* flags false.
 *
 * @return 0 on success; negative errno on error.
 */
int cbx_profile_meta_load(cbx_profile_meta *m, const char *path);

/*
 * Parse a sidecar from a YAML string in memory.
 * Initializes the struct first.
 *
 * @param yaml NUL-terminated YAML string.
 * @param len  Length (or 0 to use strlen).
 * @return 0 on success; negative errno on error.
 */
int cbx_profile_meta_parse(cbx_profile_meta *m, const char *yaml, size_t len);

/*
 * Save a sidecar to a file path (atomic: mkstemp + rename, mode 0600).
 * Opens the output with O_NOFOLLOW | O_CREAT.
 *
 * @return 0 on success; negative errno on error.
 */
int cbx_profile_meta_save(const cbx_profile_meta *m, const char *path);

/*
 * Serialize a sidecar to a YAML string (libyaml document-based emitter).
 *
 * @param buf Output: malloc'd buffer (caller frees).
 * @param len Output: length excluding NUL.
 * @return 0 on success; negative errno on error.
 */
int cbx_profile_meta_serialize(const cbx_profile_meta *m,
                                char **buf, size_t *len);

/*
 * Load the sidecar for a named profile.
 * Constructs path: <config_dir>/profile-metadata/<name>.meta.yaml
 * and calls cbx_profile_meta_load.
 *
 * @param m            Output metadata.
 * @param profile_name Profile base name (e.g. "fighting"), validated
 *                     against ^[a-zA-Z0-9_-]+$.
 * @return 0 on success; -EINVAL if name is invalid; negative errno on error.
 */
int cbx_profile_meta_load_for(cbx_profile_meta *m, const char *profile_name);

/*
 * Save a sidecar for a named profile.
 * Validates the profile name, constructs path, creates the
 * profile-metadata directory if needed, and writes atomically.
 *
 * @return 0 on success; -EINVAL if name is invalid; negative errno on error.
 */
int cbx_profile_meta_save_for(const cbx_profile_meta *m,
                               const char *profile_name);

/*
 * Validate that a filename matches ^[a-zA-Z0-9_-]+$.
 * @return true if valid, false otherwise.
 */
bool cbx_validate_filename(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* CBX_CONFIG_PROFILE_META_H */