/*
 * config_profile_list.c — Profile enumeration and filesystem listing.
 *
 * Enumerates profiles from user and system directories, merges optional
 * sidecar metadata, and produces a sorted list.  Also enumerates device
 * configs and capability maps from the filesystem (Gap #4).
 *
 * Security:
 *   - Only *.yaml files enumerated
 *   - Paths constructed from directory + dirent d_name (safe by construction)
 *   - Profile and sidecar files opened with O_NOFOLLOW
 *   - Sidecar YAML parsed with libyaml: max depth 50, max doc 1 MB, no tags
 */
#include "config_profile_list.h"
#include "config_profile_meta.h"
#include "config_profile.h"
#include "config_paths.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* --- Helpers ------------------------------------------------------------- */

/* Check if a filename ends with ".yaml". */
static bool has_yaml_ext(const char *name)
{
    size_t len = strlen(name);
    if (len < 6)  /* ".yaml" = 5 chars, need at least 1 char base */
        return false;
    return strcmp(name + len - 5, ".yaml") == 0;
}

/* Strip ".yaml" extension from a filename.  Returns the base name length. */
static size_t strip_yaml_ext(const char *filename, char *out, size_t out_size)
{
    size_t len = strlen(filename);
    size_t base_len = len - 5;  /* remove ".yaml" */
    if (base_len >= out_size)
        base_len = out_size - 1;
    memcpy(out, filename, base_len);
    out[base_len] = '\0';
    return base_len;
}

static void safe_copy(char *dst, size_t dst_size, const char *src)
{
    if (!src || dst_size == 0) {
        if (dst && dst_size > 0)
            dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

/* --- Generic file enumeration -------------------------------------------- */

/*
 * Scan a directory for *.yaml files.  Appends entries to the list.
 * Skips entries that are not regular files or have non-yaml extensions.
 * Returns 0 on success (even if dir is empty), -ENOENT if dir doesn't exist.
 */
static int scan_yaml_files(cbx_file_list *list, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        if (errno == ENOENT || errno == ENOTDIR)
            return -ENOENT;
        return -errno;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (list->count >= CBX_MAX_FILE_ENTRIES)
            break;
        if (ent->d_name[0] == '.')
            continue;
        if (!has_yaml_ext(ent->d_name))
            continue;

        /* Build full path. */
        char path[PATH_MAX];
        int rc = snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if (rc < 0 || (size_t)rc >= sizeof(path))
            continue;

        /* Verify it's a regular file (skip symlinks/dirs). */
        struct stat st;
        if (lstat(path, &st) != 0)
            continue;
        if (!S_ISREG(st.st_mode))
            continue;

        cbx_file_entry *e = &list->entries[list->count];
        strip_yaml_ext(ent->d_name, e->name, sizeof(e->name));
        safe_copy(e->path, sizeof(e->path), path);
        list->count++;
    }

    closedir(d);
    return 0;
}

/* Comparison function for sorting cbx_file_entry by name. */
static int cmp_file_entry(const void *a, const void *b)
{
    const cbx_file_entry *ea = a;
    const cbx_file_entry *eb = b;
    return strcmp(ea->name, eb->name);
}

int cbx_file_list_enumerate(cbx_file_list *list, const char *dir)
{
    if (!list || !dir)
        return -EINVAL;

    memset(list, 0, sizeof(*list));

    int rc = scan_yaml_files(list, dir);
    if (rc == -ENOENT) {
        /* Directory doesn't exist — empty list is fine. */
        return 0;
    }
    if (rc < 0)
        return rc;

    qsort(list->entries, list->count, sizeof(cbx_file_entry),
          cmp_file_entry);
    return 0;
}

int cbx_device_config_list_enumerate(cbx_file_list *list)
{
    return cbx_file_list_enumerate(list, cbx_system_devices_dir());
}

int cbx_capability_map_list_enumerate(cbx_file_list *list)
{
    return cbx_file_list_enumerate(list, cbx_system_capability_maps_dir());
}

/* --- Profile enumeration with metadata merging --------------------------- */

/*
 * Load display_name and description from a profile YAML file.
 * Uses O_NOFOLLOW.  Falls back to filename if profile has no name.
 */
static void load_profile_name_desc(const char *path,
                                    char *name_out, size_t name_size,
                                    char *desc_out, size_t desc_size)
{
    name_out[0] = '\0';
    desc_out[0] = '\0';

    cbx_profile prof;
    int rc = cbx_profile_load(&prof, path);
    if (rc == 0) {
        if (prof.name[0])
            safe_copy(name_out, name_size, prof.name);
        if (prof.description[0])
            safe_copy(desc_out, desc_size, prof.description);
    }
}

/*
 * Scan a directory for profile YAML files and add entries to the list.
 * System profiles are marked read-only.  If a profile with the same filename
 * already exists in the list (from a higher-priority directory), it is
 * skipped.
 */
static int scan_profiles(cbx_profile_list *list, const char *dir,
                          bool is_system, const char *meta_dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        if (errno == ENOENT || errno == ENOTDIR)
            return 0;  /* dir doesn't exist — skip */
        return -errno;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (list->count >= CBX_MAX_PROFILES)
            break;
        if (ent->d_name[0] == '.')
            continue;
        if (!has_yaml_ext(ent->d_name))
            continue;

        char path[PATH_MAX];
        int rc = snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if (rc < 0 || (size_t)rc >= sizeof(path))
            continue;

        struct stat st;
        if (lstat(path, &st) != 0)
            continue;
        if (!S_ISREG(st.st_mode))
            continue;

        /* Extract base filename. */
        char base[CBX_LIST_NAME_LEN];
        strip_yaml_ext(ent->d_name, base, sizeof(base));

        /* Check for duplicates (user dir takes precedence). */
        bool dup = false;
        for (int i = 0; i < list->count; i++) {
            if (strcmp(list->entries[i].filename, base) == 0) {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;

        /* Load profile name/description. */
        char prof_name[CBX_LIST_NAME_LEN] = {0};
        char prof_desc[CBX_LIST_NAME_LEN] = {0};
        load_profile_name_desc(path, prof_name, sizeof(prof_name),
                               prof_desc, sizeof(prof_desc));

        /* Fall back to filename if no name. */
        if (!prof_name[0])
            safe_copy(prof_name, sizeof(prof_name), base);

        /* Load sidecar metadata if meta_dir is provided. */
        cbx_profile_meta meta;
        bool has_meta = false;
        if (meta_dir) {
            char meta_path[PATH_MAX];
            rc = snprintf(meta_path, sizeof(meta_path), "%s/%s.meta.yaml",
                           meta_dir, base);
            if (rc > 0 && (size_t)rc < sizeof(meta_path)) {
                struct stat meta_st;
                if (stat(meta_path, &meta_st) == 0 &&
                    S_ISREG(meta_st.st_mode)) {
                    int mrc = cbx_profile_meta_load(&meta, meta_path);
                    if (mrc == 0)
                        has_meta = true;
                }
            }
        }

        /* Build the entry. */
        cbx_profile_entry *e = &list->entries[list->count];
        safe_copy(e->filename, sizeof(e->filename), base);
        safe_copy(e->path, sizeof(e->path), path);

        /* Merge: sidecar overrides profile data. */
        if (has_meta && meta.has_display_name)
            safe_copy(e->display_name, sizeof(e->display_name),
                      meta.display_name);
        else
            safe_copy(e->display_name, sizeof(e->display_name),
                      prof_name);

        if (has_meta && meta.has_description)
            safe_copy(e->description, sizeof(e->description),
                      meta.description);
        else
            safe_copy(e->description, sizeof(e->description),
                      prof_desc);

        if (has_meta && meta.has_icon)
            safe_copy(e->icon, sizeof(e->icon), meta.icon);
        else
            e->icon[0] = '\0';

        e->display_order = (has_meta && meta.has_display_order)
                           ? meta.display_order : 0;
        e->is_system = is_system;
        e->is_default = (strcmp(base, "default") == 0);
        e->read_only = is_system || e->is_default;
        e->has_meta = has_meta;

        list->count++;
    }

    closedir(d);
    return 0;
}

/* Comparison: sort by display_order (ascending), then by display_name. */
static int cmp_profile_entry(const void *a, const void *b)
{
    const cbx_profile_entry *ea = a;
    const cbx_profile_entry *eb = b;

    if (ea->display_order != eb->display_order)
        return (ea->display_order < eb->display_order) ? -1 : 1;

    return strcmp(ea->display_name, eb->display_name);
}

int cbx_profile_list_enumerate_dirs(cbx_profile_list *list,
                                     const char *user_dir,
                                     const char *system_dir,
                                     const char *meta_dir)
{
    if (!list)
        return -EINVAL;

    memset(list, 0, sizeof(*list));

    /* User profiles first (higher priority — duplicates from system skipped). */
    if (user_dir) {
        int rc = scan_profiles(list, user_dir, false, meta_dir);
        if (rc < 0)
            return rc;
    }

    /* System profiles (read-only). */
    if (system_dir) {
        int rc = scan_profiles(list, system_dir, true, meta_dir);
        if (rc < 0)
            return rc;
    }

    /* Sort by display_order, then display_name. */
    qsort(list->entries, list->count, sizeof(cbx_profile_entry),
          cmp_profile_entry);

    return 0;
}

int cbx_profile_list_enumerate(cbx_profile_list *list)
{
    if (!list)
        return -EINVAL;

    char user_dir[PATH_MAX];
    int rc = cbx_resolve_user_profiles_dir(user_dir, sizeof(user_dir));
    /* If HOME is unset, user_dir may fail — just skip user profiles. */
    const char *user = (rc == 0) ? user_dir : NULL;

    const char *system = cbx_system_profiles_dir();

    /* Sidecar directory: <config_dir>/profile-metadata/ */
    char config_dir[PATH_MAX];
    const char *meta = NULL;
    char meta_dir[PATH_MAX + 32];
    rc = cbx_resolve_config_dir(config_dir, sizeof(config_dir));
    if (rc == 0) {
        int n = snprintf(meta_dir, sizeof(meta_dir), "%s/profile-metadata",
                         config_dir);
        if (n > 0 && (size_t)n < sizeof(meta_dir))
            meta = meta_dir;
    }

    memset(list, 0, sizeof(*list));

    /* The shipped Default is scanned first so neither user nor host-system
     * files can shadow its immutable semantics. */
    rc = scan_profiles(list, cbx_builtin_profiles_dir(), true, NULL);
    if (rc < 0)
        return rc;
    if (user) {
        rc = scan_profiles(list, user, false, meta);
        if (rc < 0)
            return rc;
    }
    if (system) {
        rc = scan_profiles(list, system, true, meta);
        if (rc < 0)
            return rc;
    }
    qsort(list->entries, list->count, sizeof(cbx_profile_entry),
          cmp_profile_entry);
    return 0;
}