/*
 * icon_lookup.h — Runtime icon lookup API (SPEC §8.4, §8.5).
 *
 * Provides a single entry point that resolves a device type string to an
 * SDL2 texture + human-readable label, applying profile icon overrides
 * from the metadata sidecar (§7.5, §8.5).
 *
 * Resolution order (SPEC §8.5):
 *   1. Profile icon override (from sidecar metadata):
 *      a. Absolute path → load custom PNG via SDL2_image (with security
 *         validation: no `..` traversal, realpath() within safe dirs).
 *      b. Built-in icon name (e.g. "cc-ps5") → look up in icon cache.
 *   2. Icon map lookup by DeviceType string → icon name + display name.
 *   3. Unknown DeviceType → "generic-gamepad" icon + raw type string.
 *
 * The caller provides the icon cache (for SVG lookups) and the renderer
 * (for PNG loads).  PNG textures are cached in the icon cache for
 * subsequent lookups (keyed by the absolute path).
 */
#ifndef CBX_ICON_LOOKUP_H
#define CBX_ICON_LOOKUP_H

#include <SDL2/SDL.h>
#include "icons/icon_cache.h"
#include "icons/icon_map.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum label length returned by icon_lookup. */
#define CBX_ICON_LABEL_LEN 256

/* Result of an icon lookup: texture + dimensions + label. */
typedef struct {
    SDL_Texture *texture;      /* SDL2 texture (owned by the icon cache)  */
    int          width;         /* texture pixel width                      */
    int          height;        /* texture pixel height                      */
    char         label[CBX_ICON_LABEL_LEN]; /* human-readable display name */
} cbx_icon_result;

/*
 * Look up the icon + label for a device type, applying an optional
 * profile icon override.
 *
 * Resolution order (SPEC §8.5):
 *   1. If icon_override is non-NULL and non-empty:
 *      a. Starts with '/' → absolute path → PNG load via SDL2_image.
 *         The path is validated: no `..` traversal, realpath() must be
 *         within a safe directory (user config dir, user data dir, or
 *         system data dir).  If validation fails, falls back to step 2.
 *      b. Otherwise → treated as a built-in icon name → looked up in the
 *         icon cache (loaded on demand via cbx_icon_cache_load_one if
 *         not already cached).
 *      The label is set to the display name from the icon map for the
 *      given device_type (or the raw type string if unknown).
 *   2. If no override or override failed:
 *      Look up device_type in the icon map → icon name + display name.
 *      Look up the icon name in the cache (load on demand if needed).
 *   3. If the device_type is unknown:
 *      "generic-gamepad" icon + raw type string as label.
 *
 * The returned texture is owned by the icon cache — the caller must NOT
 * destroy it.  If the texture could not be resolved (e.g. SVG missing),
 * texture is NULL and width/height are 0, but the label is still set.
 *
 * @param cache         Initialized icon cache (must outlive the result).
 * @param map           Loaded icon map (may be NULL → defaults only).
 * @param device_type   InputPlumber DeviceType string (e.g. "xb360").
 *                      May be NULL → treated as unknown.
 * @param icon_override Profile icon override from sidecar metadata.
 *                      May be NULL or empty → no override.
 * @param result        Output: filled with texture + dims + label.
 * @return 0 on success (texture may still be NULL if icon missing);
 *         -EINVAL on bad args (NULL cache/result).
 */
int cbx_icon_lookup(cbx_icon_cache *cache, const cbx_icon_map *map,
                    const char *device_type, const char *icon_override,
                    cbx_icon_result *result);

/*
 * Validate that an absolute icon path is safe to load.
 *
 * Security checks:
 *   - Path must be absolute (start with '/').
 *   - No ".." path components (traversal prevention).
 *   - realpath() must resolve successfully.
 *   - Resolved path must be within one of the safe directories:
 *     user config dir, user data dir, or system data dir.
 *
 * @param abs_path  Absolute path to validate.
 * @param resolved  Output buffer for the canonical path (may be NULL).
 * @param resolved_size Size of resolved buffer.
 * @return 0 if safe; -EINVAL if not absolute or contains "..";
 *         -EACCES if realpath fails or path is outside safe dirs;
 *         -ENAMETOOLONG if path is too long.
 */
int cbx_icon_validate_path(const char *abs_path,
                            char *resolved, size_t resolved_size);

#ifdef __cplusplus
}
#endif

#endif /* CBX_ICON_LOOKUP_H */