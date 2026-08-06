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

/* Virtual controller configuration (SPEC §7.3). */
typedef struct {
    int count;                                            /* 1–16 */
    char types[CBX_MAX_CONTROLLERS][CBX_MAX_TYPE_LEN];   /* type per slot */
} cbx_virtual_controllers;

/* Application settings (SPEC §7.3). */
typedef struct {
    char overlay_trigger[CBX_MAX_STR_LEN];   /* e.g. "Select+A" */
    bool launch_at_boot;
    char theme[CBX_MAX_STR_LEN];            /* e.g. "default" */
    double overlay_opacity;                  /* 0.0–1.0 */
    cbx_virtual_controllers virtual_controllers;
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

#ifdef __cplusplus
}
#endif

#endif /* CBX_CONFIG_SETTINGS_H */