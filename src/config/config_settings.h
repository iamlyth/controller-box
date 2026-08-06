/*
 * config_settings.h — settings.yaml read/write for Controller-Box.
 *
 * Loads and saves the application settings file at
 * ~/.config/controller-box/settings.yaml (SPEC §7.3).
 *
 * Security constraints (from plan security review):
 *   - YAML parsed via libyaml with max depth 50, max document size 1 MB,
 *     no custom tags.
 *   - Atomic write: temp file (mkstemp) + rename, file mode 0600.
 */
#ifndef CBX_CONFIG_SETTINGS_H
#define CBX_CONFIG_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of virtual controllers (SPEC §5.3, validation range 1–16). */
#define CBX_MAX_CONTROLLERS 16

/* Maximum length of a controller type string (NUL-terminated). */
#define CBX_MAX_TYPE_LEN   32

/* Maximum length of overlay_trigger and theme strings (NUL-terminated). */
#define CBX_MAX_STR_LEN    64

/* Maximum number of per-type icon overrides (SPEC §5.5, §8.4). */
#define CBX_MAX_ICON_OVERRIDES 16

/* Maximum length of an icon override type/icon string. */
#define CBX_ICON_OVR_TYPE_LEN 32
#define CBX_ICON_OVR_ICON_LEN 64

/* Virtual controller configuration (SPEC §7.3). */
typedef struct {
    int count;                                            /* 1–16 */
    char types[CBX_MAX_CONTROLLERS][CBX_MAX_TYPE_LEN];   /* type per slot */
} cbx_virtual_controllers;

/* Per-type icon override entry (SPEC §5.5, §8.4).
 * Overrides the system controller-icons.yaml mapping for the user's
 * session.  Distinct from per-profile icon override in §8.5 sidecar. */
typedef struct {
    char type[CBX_ICON_OVR_TYPE_LEN];   /* InputPlumber DeviceType string */
    char icon[CBX_ICON_OVR_ICON_LEN];   /* built-in icon name or absolute path */
} cbx_icon_override;

/* Application settings (SPEC §7.3, §5.5). */
typedef struct {
    char overlay_trigger[CBX_MAX_STR_LEN];   /* e.g. "Select+A" */
    bool launch_at_boot;
    char theme[CBX_MAX_STR_LEN];            /* e.g. "default" */
    double overlay_opacity;                  /* 0.0–1.0 */
    cbx_virtual_controllers virtual_controllers;
    cbx_icon_override icon_overrides[CBX_MAX_ICON_OVERRIDES]; /* §5.5 */
    int               icon_override_count;
} cbx_settings;

/*
 * Fill settings with default values (SPEC §7.3 defaults):
 *   overlay_trigger="Select+A", launch_at_boot=true, theme="default",
 *   overlay_opacity=0.85, count=4, types=[xb360,xb360,xb360,xb360]
 */
void cbx_settings_defaults(cbx_settings *settings);

/*
 * Load settings from ~/.config/controller-box/settings.yaml.
 * If the file does not exist, fills defaults and returns 0.
 * Missing fields are filled with defaults.
 * Out-of-range values are clamped to valid ranges.
 *
 * @param settings  Output struct (overwritten).
 * @return 0 on success; negative errno on error.
 */
int cbx_settings_load(cbx_settings *settings);

/*
 * Validate settings strictly: opacity 0.0–1.0, count 1–16,
 * each type in the known-good list, types array has exactly count entries.
 *
 * @return 0 if valid; -EINVAL if invalid.
 */
int cbx_settings_validate(const cbx_settings *settings);

/*
 * Save settings to ~/.config/controller-box/settings.yaml.
 * Validates first (returns -EINVAL if invalid).
 * Writes atomically: temp file + rename, mode 0600.
 *
 * @return 0 on success; negative errno on error.
 */
int cbx_settings_save(const cbx_settings *settings);

/*
 * Check whether a controller type string is in the known-good list.
 * Known types: xb360, ds5, deck, gamepad, mouse, keyboard, touchscreen.
 */
bool cbx_is_known_controller_type(const char *type);

/*
 * Look up the icon override for a given controller type.
 * Returns the override icon string, or NULL if no override is set.
 */
const char *cbx_settings_icon_override(const cbx_settings *s,
                                         const char *type);

/*
 * Set or update an icon override for a controller type.
 * If an override for this type already exists, it is updated.
 * If not and there is room, a new entry is added.
 *
 * @return 0 on success; -ENOSPC if no room; -EINVAL if NULL args.
 */
int cbx_settings_set_icon_override(cbx_settings *s, const char *type,
                                    const char *icon);

/*
 * Remove an icon override for a controller type.
 * @return 0 on success; -ENOENT if not found; -EINVAL if NULL args.
 */
int cbx_settings_remove_icon_override(cbx_settings *s, const char *type);

#ifdef __cplusplus
}
#endif

#endif /* CBX_CONFIG_SETTINGS_H */