/*
 * profile_save.c — Manager-level profile save with security and validation.
 *
 * Implements cbx_profile_save_named / cbx_profile_save_to_dir which:
 *   1. Validate the profile name against ^[a-zA-Z0-9_-]+$
 *   2. Validate the profile (version=1, kind="DeviceProfile")
 *   3. Validate NES minimum bindings (cannot save invalid profile)
 *   4. Build the target path: <profiles_dir>/<name>.yaml
 *   5. Canonicalize the profiles directory with realpath()
 *   6. Verify the final path is within the profiles directory
 *   7. Write the profile YAML atomically (delegates to cbx_profile_save)
 *   8. Optionally write sidecar metadata
 *
 * Task 39 — Profile save and Settings tab.
 */
#include "profile_save.h"

#include "config/config_paths.h"
#include "config/config_profile_meta.h"
#include "profile_validate.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/*
 * Verify that a target file path is within the given base directory.
 * Canonicalizes both paths with realpath() and checks that the target
 * starts with the base path (with a '/' boundary).
 *
 * If the target file doesn't exist yet, canonicalizes its parent directory
 * and appends the filename.
 *
 * @param base_dir   The expected containing directory.
 * @param target_path The full path to the target file.
 * @return 0 if within base_dir; -EACCES if outside; negative errno on error.
 */
static int verify_path_within_dir(const char *base_dir,
                                   const char *target_path)
{
    char real_base[PATH_MAX];
    char real_parent[PATH_MAX];

    /* Canonicalize the base directory. */
    if (!realpath(base_dir, real_base)) {
        /* If base_dir doesn't exist, we can't canonicalize it.
         * This shouldn't happen since we ensure the dir exists first. */
        return -errno;
    }

    /* Try to canonicalize the target path directly.
     * If the file doesn't exist yet (first save), canonicalize the parent. */
    if (!realpath(target_path, real_parent)) {
        if (errno == ENOENT) {
            /* Extract parent directory from target_path. */
            char parent[PATH_MAX];
            strncpy(parent, target_path, sizeof(parent) - 1);
            parent[sizeof(parent) - 1] = '\0';
            char *slash = strrchr(parent, '/');
            if (!slash)
                return -EINVAL;
            *slash = '\0';

            if (!realpath(parent, real_parent))
                return -errno;

            /* Append the filename back. */
            size_t plen = strlen(real_parent);
            size_t flen = strlen(slash + 1);
            if (plen + 1 + flen + 1 > sizeof(real_parent))
                return -ENAMETOOLONG;
            real_parent[plen] = '/';
            memcpy(real_parent + plen + 1, slash + 1, flen + 1);
        } else {
            return -errno;
        }
    }

    /* Check that real_parent starts with real_base. */
    size_t base_len = strlen(real_base);
    if (strncmp(real_parent, real_base, base_len) != 0)
        return -EACCES;

    /* Ensure boundary: real_parent[base_len] must be '/' or '\0'. */
    if (real_parent[base_len] != '\0' && real_parent[base_len] != '/')
        return -EACCES;

    return 0;
}

int cbx_profile_save_to_dir(const cbx_profile *profile, const char *name,
                             const cbx_profile_meta *meta,
                             const char *profiles_dir,
                             char *missing_buf, size_t buflen)
{
    if (!profile || !name)
        return -EINVAL;

    /* 1. Validate filename. */
    if (!cbx_validate_filename(name))
        return -EINVAL;

    /* 2. Validate profile structure. */
    int rc = cbx_profile_validate(profile);
    if (rc < 0)
        return rc;

    /* 3. NES minimum validation. */
    rc = cbx_profile_validate_nes_minimum(profile, missing_buf, buflen);
    if (rc < 0)
        return rc;  /* -EINVAL with missing buttons in buf */

    /* 4. Resolve profiles directory. */
    char dir_buf[PATH_MAX];
    const char *dir;

    if (profiles_dir) {
        /* Use the provided override (for testing). */
        if (strlen(profiles_dir) >= sizeof(dir_buf))
            return -ENAMETOOLONG;
        strncpy(dir_buf, profiles_dir, sizeof(dir_buf) - 1);
        dir_buf[sizeof(dir_buf) - 1] = '\0';
        dir = dir_buf;
    } else {
        rc = cbx_user_profiles_dir(dir_buf, sizeof(dir_buf));
        if (rc < 0)
            return rc;
        dir = dir_buf;
    }

    /* 5. Build the target path. */
    char path[PATH_MAX + 128];
    rc = snprintf(path, sizeof(path), "%s/%s.yaml", dir, name);
    if (rc < 0 || (size_t)rc >= sizeof(path))
        return -ENAMETOOLONG;

    /* 6. Canonicalize and verify path is within profiles dir. */
    rc = verify_path_within_dir(dir, path);
    if (rc < 0)
        return rc;

    /* 7. Write the profile YAML atomically (delegates to cbx_profile_save
     *    which uses mkstemp + rename, mode 0644). */
    rc = cbx_profile_save(profile, path);
    if (rc < 0)
        return rc;

    /* 8. Optionally write sidecar metadata. */
    if (meta) {
        if (profiles_dir) {
            /* For testing: use the same base for metadata.
             * The sidecar goes to <config_dir>/profile-metadata/<name>.meta.yaml
             * normally, but for tests we use a separate meta_dir.
             * We pass NULL to use the default path resolution. */
            /* When using a test profiles_dir, the sidecar uses the default
             * config dir, which may not be set up for testing. We handle
             * this via cbx_profile_save_meta_to_dir in the caller. */
            /* If meta is non-NULL and we're using a test dir, we skip the
             * sidecar here — the caller should use cbx_profile_save_meta_to_dir. */
        } else {
            rc = cbx_profile_meta_save_for(meta, name);
            if (rc < 0)
                return rc;
        }
    }

    return 0;
}

int cbx_profile_save_named(const cbx_profile *profile, const char *name,
                            const cbx_profile_meta *meta,
                            char *missing_buf, size_t buflen)
{
    return cbx_profile_save_to_dir(profile, name, meta, NULL,
                                    missing_buf, buflen);
}

int cbx_profile_save_meta_to_dir(const cbx_profile_meta *meta,
                                  const char *name,
                                  const char *meta_dir)
{
    if (!meta || !name)
        return -EINVAL;

    if (!cbx_validate_filename(name))
        return -EINVAL;

    if (meta_dir) {
        /* Build path: <meta_dir>/<name>.meta.yaml */
        char path[PATH_MAX + 128];
        int rc = snprintf(path, sizeof(path), "%s/%s.meta.yaml",
                          meta_dir, name);
        if (rc < 0 || (size_t)rc >= sizeof(path))
            return -ENAMETOOLONG;

        /* Ensure the directory exists. */
        rc = cbx_ensure_dir(meta_dir, 0700);
        if (rc < 0)
            return rc;

        return cbx_profile_meta_save(meta, path);
    }

    /* Use the default path resolution. */
    return cbx_profile_meta_save_for(meta, name);
}