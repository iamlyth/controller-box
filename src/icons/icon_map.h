/*
 * icon_map.h — Runtime icon mapping table (SPEC §8.4).
 *
 * Loads controller-icons.yaml at runtime and provides lookup from
 * InputPlumber DeviceType strings to icon name + display name.
 *
 * The YAML file is installed at DATA_DIR/controller-icons.yaml
 * (e.g. /usr/share/controller-box/controller-icons.yaml).
 *
 * Unknown DeviceType strings fall back to "generic-gamepad" with the
 * raw type string as the display name (SPEC §8.4).
 */
#ifndef CBX_ICON_MAP_H
#define CBX_ICON_MAP_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum field lengths for icon mapping entries. */
#define CBX_ICON_TYPE_LEN 64
#define CBX_ICON_NAME_LEN 128
#define CBX_ICON_ICON_LEN 256

/* Maximum number of virtual type entries in the mapping. */
#define CBX_ICON_MAP_MAX_ENTRIES 64

/* Default icon for unknown device types (SPEC §8.4). */
#define CBX_ICON_DEFAULT_ICON  "generic-gamepad"

/* A single icon mapping entry: DeviceType → icon + display name. */
typedef struct {
    char type[CBX_ICON_TYPE_LEN];   /* InputPlumber DeviceType string */
    char icon[CBX_ICON_ICON_LEN];   /* Icon name (SVG filename without .svg) */
    char name[CBX_ICON_NAME_LEN];   /* Human-readable display name */
} cbx_icon_entry;

/* The loaded icon mapping table. */
typedef struct {
    cbx_icon_entry entries[CBX_ICON_MAP_MAX_ENTRIES];
    int count;
    char loaded;   /* 0 = not loaded, 1 = loaded */
    char yaml_path[512];
} cbx_icon_map;

/*
 * Initialize an icon map struct to empty.
 * Must be called before cbx_icon_map_load.
 */
void cbx_icon_map_init(cbx_icon_map *map);

/*
 * Load the icon mapping from controller-icons.yaml at the given path.
 * Uses libyaml event-based parsing with security constraints:
 *   - max depth 50
 *   - max doc size 1 MB
 *   - no custom tags (reject YAML tags)
 *
 * @param map  Initialized icon map.
 * @param path Path to controller-icons.yaml (absolute or relative).
 * @return 0 on success; negative errno on error.
 */
int cbx_icon_map_load(cbx_icon_map *map, const char *path);

/*
 * Parse icon mapping from a YAML string in memory.
 * Initializes the map first.
 *
 * @param map  Icon map to populate.
 * @param yaml NUL-terminated YAML string.
 * @param len  Length (or 0 to use strlen).
 * @return 0 on success; negative errno on error.
 */
int cbx_icon_map_parse(cbx_icon_map *map, const char *yaml, size_t len);

/*
 * Look up the icon name and display name for a given DeviceType.
 *
 * If the type is found in the mapping, the icon and name from the
 * mapping are used.  If the type is not found, the default icon
 * (generic-gamepad) is returned with the raw type string as the name.
 * (SPEC §8.4: "Unknown types: generic gamepad silhouette + the raw
 * DeviceType string as label.")
 *
 * @param map      Loaded icon map (may be NULL → use defaults).
 * @param type     InputPlumber DeviceType string (e.g. "xb360").
 * @param out_icon Output buffer for icon name (may be NULL).
 * @param icon_size Size of out_icon.
 * @param out_name  Output buffer for display name (may be NULL).
 * @param name_size Size of out_name.
 * @return 0 on success (always succeeds — unknown types get defaults).
 */
int cbx_icon_map_lookup(const cbx_icon_map *map, const char *type,
                         char *out_icon, size_t icon_size,
                         char *out_name, size_t name_size);

/*
 * Get the default install path for controller-icons.yaml.
 * Uses the compile-time DATA_DIR constant.
 *
 * @param out_path  Output buffer for the path.
 * @param path_size Size of out_path.
 * @return 0 on success; negative errno on error.
 */
int cbx_icon_map_default_path(char *out_path, size_t path_size);

#ifdef __cplusplus
}
#endif

#endif /* CBX_ICON_MAP_H */