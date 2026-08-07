/*
 * config_paths.h — XDG base directory resolution for Controller-Box.
 *
 * Provides runtime resolution of user config and data directories per the
 * XDG Base Directory Specification, plus accessors for compile-time system
 * paths (read-only data directories).
 *
 * SPEC §7.2 file layout:
 *   ~/.config/controller-box/                  ← GUI-only config
 *   ~/.local/share/inputplumber/profiles/      ← InputPlumber + GUI read/write
 *   /usr/share/inputplumber/                   ← system data (read-only)
 *   /usr/share/controller-box/                ← icons, icon mapping (read-only)
 */
#ifndef CBX_CONFIG_PATHS_H
#define CBX_CONFIG_PATHS_H

#include <stddef.h>
#include <sys/types.h> /* mode_t */

#ifdef __cplusplus
extern "C" {
#endif

/* --- Path resolution (no side effects) ----------------------------------- */

/*
 * Resolve the user config directory path: $XDG_CONFIG_HOME/controller-box
 * or $HOME/.config/controller-box.  Does NOT create the directory.
 *
 * @param buf       Output buffer for the NUL-terminated path.
 * @param buf_size  Size of buf in bytes.
 * @return 0 on success; -ENAMETOOLONG if the path doesn't fit;
 *         -ENOENT if $HOME is unset and $XDG_CONFIG_HOME is not absolute.
 */
int cbx_resolve_config_dir(char *buf, size_t buf_size);

/*
 * Resolve the user InputPlumber profiles directory path:
 * $XDG_DATA_HOME/inputplumber/profiles or
 * $HOME/.local/share/inputplumber/profiles.  Does NOT create the directory.
 *
 * @return 0 on success; -ENAMETOOLONG; -ENOENT if $HOME unset.
 */
int cbx_resolve_user_profiles_dir(char *buf, size_t buf_size);

/* --- Directory creation -------------------------------------------------- */

/*
 * Ensure a directory exists, creating parent directories as needed.
 * If the directory already exists and is a directory, returns 0.
 *
 * @param path  NUL-terminated directory path.
 * @param mode  File mode for newly created directories (e.g. 0700).
 *              Modified by the process umask.
 * @return 0 on success; negative errno on failure.
 */
int cbx_ensure_dir(const char *path, mode_t mode);

/*
 * Resolve and create the user config directory (mode 0700).
 * @return 0 on success; negative errno on failure.
 */
int cbx_config_dir(char *buf, size_t buf_size);

/*
 * Resolve and create the user InputPlumber profiles directory (mode 0700).
 * Creates parent directories (~/.local/share/inputplumber/) as needed.
 * @return 0 on success; negative errno on failure.
 */
int cbx_user_profiles_dir(char *buf, size_t buf_size);

/* --- System paths (compile-time, read-only) ------------------------------- */

/* System InputPlumber data directory: /usr/share/inputplumber */
const char *cbx_system_inputplumber_dir(void);

/* System InputPlumber profiles directory (read-only). */
const char *cbx_system_profiles_dir(void);

/* System InputPlumber devices directory (read-only). */
const char *cbx_system_devices_dir(void);

/* System InputPlumber capability maps directory (read-only). */
const char *cbx_system_capability_maps_dir(void);

/* Controller-Box data directory: /usr/share/controller-box (icons, mapping). */
const char *cbx_data_dir(void);

/* Controller-Box icon directory: /usr/share/controller-box/icons. */
const char *cbx_icon_dir(void);

/* Controller-Box bundled font directory: ${DATA_DIR}/fonts (read-only).
 * Intended for future bundled fonts; runtime discovery also checks
 * system font directories via cbx_font_path(). */
const char *cbx_font_dir(void);

/*
 * Discover a usable TTF font at runtime by searching common system font
 * directories in priority order.  DejaVuSans.ttf is preferred; the first
 * readable candidate found is returned.
 *
 * Search order (first match wins):
 *   $XDG_DATA_HOME/fonts/DejaVuSans.ttf
 *   $HOME/.local/share/fonts/DejaVuSans.ttf
 *   $HOME/.fonts/DejaVuSans.ttf
 *   /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf
 *   /usr/share/fonts/dejavu/DejaVuSans.ttf
 *   /usr/share/fonts/TTF/DejaVuSans.ttf
 *   /run/current-system/sw/share/X11/fonts/DejaVuSans.ttf   (NixOS)
 *   /nix/var/nix/profiles/default/share/X11/fonts/DejaVuSans.ttf
 *
 * @return Pointer to a static buffer holding the NUL-terminated font path,
 *         or NULL if no readable DejaVuSans.ttf was found.  The returned
 *         pointer is valid until the next call to cbx_font_path().
 */
const char *cbx_font_path(void);

#ifdef __cplusplus
}
#endif

#endif /* CBX_CONFIG_PATHS_H */