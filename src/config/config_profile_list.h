/*
 * config_profile_list.h — Profile enumeration and filesystem listing.
 *
 * Enumerates InputPlumber profiles from the user profiles directory
 * (~/.local/share/inputplumber/profiles/) and the system profiles directory
 * (/usr/share/inputplumber/profiles/, read-only).  Merges optional sidecar
 * metadata (§7.5) with base profile data.  Produces a sorted list.
 *
 * Also enumerates device configs (/usr/share/inputplumber/devices/) and
 * capability maps (/usr/share/inputplumber/capability_maps/) for the profile
 * editor's target-capability list (Gap #4: filesystem reads, no DBus API).
 *
 * Security:
 *   - Only *.yaml files are enumerated (filename filter)
 *   - Paths constructed by joining known directory + dirent d_name (no
 *     user-controlled path components)
 *   - Sidecar and profile files opened with O_NOFOLLOW
 */
#ifndef CBX_CONFIG_PROFILE_LIST_H
#define CBX_CONFIG_PROFILE_LIST_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Maximum number of profiles in the enumeration list. */
#define CBX_MAX_PROFILES 64

/* Maximum number of files in a generic file listing. */
#define CBX_MAX_FILE_ENTRIES 64

/* Maximum string length for names/paths. */
#define CBX_LIST_NAME_LEN 256

/* --- Generic file listing (device configs, capability maps) ------------- */

typedef struct {
    char name[CBX_LIST_NAME_LEN];   /* filename without .yaml extension */
    char path[PATH_MAX];            /* full filesystem path */
} cbx_file_entry;

typedef struct {
    cbx_file_entry entries[CBX_MAX_FILE_ENTRIES];
    int count;
} cbx_file_list;

/*
 * Enumerate *.yaml files in a directory.
 * Populates the list with entries sorted by name.
 *
 * @param list Output list (cleared first).
 * @param dir  Directory to scan.
 * @return 0 on success; -ENOENT if dir doesn't exist (list is empty);
 *         negative errno on other errors.
 */
int cbx_file_list_enumerate(cbx_file_list *list, const char *dir);

/* --- Profile enumeration with metadata merging --------------------------- */

typedef struct {
    char filename[CBX_LIST_NAME_LEN];   /* base name without .yaml */
    char path[PATH_MAX];                /* full path to profile YAML */
    char display_name[CBX_LIST_NAME_LEN]; /* merged: sidecar → profile name */
    char description[CBX_LIST_NAME_LEN];  /* merged: sidecar → profile desc */
    char icon[CBX_LIST_NAME_LEN];         /* sidecar icon or "" */
    int  display_order;                   /* sidecar display_order or 0 */
    bool is_system;                       /* from system dir (read-only) */
    bool is_default;                      /* filename == "default" */
    bool read_only;                       /* is_system || is_default */
    bool has_meta;                        /* sidecar was found */
} cbx_profile_entry;

typedef struct {
    cbx_profile_entry entries[CBX_MAX_PROFILES];
    int count;
} cbx_profile_list;

/*
 * Enumerate profiles from user and system directories, merge sidecar
 * metadata, and produce a sorted list.
 *
 * Uses default paths from config_paths.h:
 *   - User:   cbx_user_profiles_dir()
 *   - System: cbx_system_profiles_dir()
 *   - Sidecar: <config_dir>/profile-metadata/
 *
 * @param list Output list (cleared first).
 * @return 0 on success; negative errno on error.
 */
int cbx_profile_list_enumerate(cbx_profile_list *list);

/*
 * Enumerate profiles from explicit directories (for testing).
 *
 * @param list       Output list (cleared first).
 * @param user_dir   User profiles directory (read/write). May be NULL.
 * @param system_dir System profiles directory (read-only). May be NULL.
 * @param meta_dir   Sidecar metadata directory. May be NULL (no sidecars).
 * @return 0 on success; negative errno on error.
 */
int cbx_profile_list_enumerate_dirs(cbx_profile_list *list,
                                     const char *user_dir,
                                     const char *system_dir,
                                     const char *meta_dir);

/* --- Convenience: device configs and capability maps -------------------- */

/*
 * Enumerate device configs from /usr/share/inputplumber/devices/ (read-only).
 * @return 0 on success; -ENOENT if dir doesn't exist (list is empty).
 */
int cbx_device_config_list_enumerate(cbx_file_list *list);

/*
 * Enumerate capability maps from /usr/share/inputplumber/capability_maps/
 * (read-only).
 * @return 0 on success; -ENOENT if dir doesn't exist (list is empty).
 */
int cbx_capability_map_list_enumerate(cbx_file_list *list);

#ifdef __cplusplus
}
#endif

#endif /* CBX_CONFIG_PROFILE_LIST_H */