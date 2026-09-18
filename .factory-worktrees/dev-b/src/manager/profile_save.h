/*
 * profile_save.h — Manager-level profile save with security and validation.
 *
 * Wraps cbx_profile_save with:
 *   - Filename validation against ^[a-zA-Z0-9_-]+$
 *   - Path canonicalization (realpath) and verification within profiles dir
 *   - NES minimum validation gate (cannot save invalid profile)
 *   - Optional sidecar metadata write
 *
 * Task 39 — Profile save and Settings tab.
 */
#ifndef CBX_PROFILE_SAVE_H
#define CBX_PROFILE_SAVE_H

#include <stddef.h>

#include "config/config_profile.h"
#include "config/config_profile_meta.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Save a profile to the InputPlumber user profiles directory.
 *
 * Security and validation:
 *   - Name validated against ^[a-zA-Z0-9_-]+$
 *   - Profile validated (version=1, kind="DeviceProfile")
 *   - NES minimum validation enforced (cannot save invalid profile)
 *   - Path canonicalized with realpath() and verified within profiles dir
 *   - Atomic write (mkstemp + rename, mode 0644)
 *
 * @param profile     Profile to save (must pass cbx_profile_validate).
 * @param name        Profile filename (without .yaml extension).
 * @param meta        Optional sidecar metadata (NULL = no sidecar written).
 * @param missing_buf Output buffer for missing NES button names (may be NULL).
 * @param buflen      Size of missing_buf.
 * @return 0 on success; -EINVAL if name invalid or NES minimum not met;
 *         negative errno on other errors.
 */
int cbx_profile_save_named(const cbx_profile *profile, const char *name,
                            const cbx_profile_meta *meta,
                            char *missing_buf, size_t buflen);

/*
 * Save a profile to a specific profiles directory (for testing).
 *
 * Same validation and security as cbx_profile_save_named, but writes to
 * the given directory instead of the default user profiles dir.
 *
 * @param profiles_dir Directory to write to (NULL = use default).
 */
int cbx_profile_save_to_dir(const cbx_profile *profile, const char *name,
                             const cbx_profile_meta *meta,
                             const char *profiles_dir,
                             char *missing_buf, size_t buflen);

/*
 * Save sidecar metadata for a named profile to a specific metadata dir.
 * When meta_dir is NULL, uses the default config dir/profile-metadata/.
 *
 * @param meta     Metadata to save.
 * @param name     Profile name (validated against ^[a-zA-Z0-9_-]+$).
 * @param meta_dir Override metadata directory (NULL = default).
 * @return 0 on success; negative errno on error.
 */
int cbx_profile_save_meta_to_dir(const cbx_profile_meta *meta,
                                  const char *name,
                                  const char *meta_dir);

#ifdef __cplusplus
}
#endif

#endif /* CBX_PROFILE_SAVE_H */