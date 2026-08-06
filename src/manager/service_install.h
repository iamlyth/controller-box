/*
 * service_install.h — Systemd user service installation (SPEC §2.4, §9.1).
 *
 * On first run, the manager prompts the user to enable the overlay service.
 * If they agree, this module:
 *   1. Writes ~/.config/systemd/user/controller-box.service atomically
 *      (mkstemp + rename, mode 0644).
 *   2. Runs `systemctl --user enable --now controller-box` (or
 *      `flatpak-spawn --host systemctl --user ...` in Flatpak).
 *   3. Verifies the service started via `systemctl --user is-active`.
 *   4. Handles systemd not available gracefully (returns -ENOSYS).
 *   5. Checks if the user is in the `inputplumber` group for polkit
 *      authorization; if not, provides guidance.
 *
 * The unit file is a static template — only the binary path (or Flatpak
 * app ID) is compiled in via CMake.  No user-supplied values are
 * interpolated into the template at runtime.
 *
 * Task 40 — Systemd service installation and manager integration test.
 */
#ifndef CBX_SERVICE_INSTALL_H
#define CBX_SERVICE_INSTALL_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Result codes                                                       */
/* ------------------------------------------------------------------ */

enum {
    CBX_SVC_OK              =  0,  /* service installed and started       */
    CBX_SVC_ALREADY_ACTIVE  =  1,  /* service was already active          */
    CBX_SVC_INSTALLED       =  2,  /* unit file written but not started   */
    CBX_SVC_NO_SYSTEMD      = -1,  /* systemd user session not available  */
    CBX_SVC_WRITE_FAILED    = -2,  /* could not write the unit file        */
    CBX_SVC_ENABLE_FAILED    = -3,  /* systemctl enable --now failed        */
    CBX_SVC_VERIFY_FAILED    = -4,  /* service installed but not active    */
    CBX_SVC_NOT_IN_GROUP    = -5,  /* user not in inputplumber group        */
};

/* ------------------------------------------------------------------ */
/*  Service installation                                               */
/* ------------------------------------------------------------------ */

/*
 * Resolve the path where the systemd user unit will be written:
 *   $XDG_CONFIG_HOME/systemd/user/controller-box.service or
 *   $HOME/.config/systemd/user/controller-box.service
 *
 * @param buf    Output buffer for the NUL-terminated path.
 * @param buflen Size of buf.
 * @return 0 on success; -ENAMETOOLONG if path doesn't fit; -ENOENT if
 *         $HOME is unset and $XDG_CONFIG_HOME is not absolute.
 */
int cbx_service_unit_path(char *buf, size_t buflen);

/*
 * Generate the systemd unit file content for the overlay service.
 * The content is a static template with only the compiled-in binary
 * path (or Flatpak app ID) substituted — no user-supplied values.
 *
 * @param buf    Output buffer for the unit file content.
 * @param buflen Size of buf.
 * @return 0 on success; -ENAMETOOLONG if content doesn't fit.
 */
int cbx_service_unit_content(char *buf, size_t buflen);

/*
 * Write the unit file atomically to the resolved systemd user dir.
 * Creates the directory if it does not exist.  Uses mkstemp + rename
 * with mode 0644.
 *
 * @param unit_path   Full path to the target unit file (NULL = auto-resolve).
 * @param content      Unit file content (NULL = use default template).
 * @return 0 on success; negative errno on error.
 */
int cbx_service_write_unit(const char *unit_path, const char *content);

/*
 * Check if the user is in the `inputplumber` group.
 * Reads /etc/group and searches for the inputplumber group, then checks
 * if the current username appears in the member list.
 *
 * @return 1 if in group; 0 if not; -errno on error.
 */
int cbx_service_check_group(void);

/*
 * Check if systemd user session is available.
 * Runs `systemctl --user is-system-running` (or checks for the systemd
 * user bus socket).  Returns 1 if available, 0 if not.
 *
 * @return 1 if available; 0 if not.
 */
int cbx_service_systemd_available(void);

/*
 * Install and enable the overlay service.
 *
 * Steps:
 *   1. Check if systemd user session is available (returns CBX_SVC_NO_SYSTEMD).
 *   2. Check if user is in inputplumber group (returns CBX_SVC_NOT_IN_GROUP
 *      with guidance, but still installs the unit).
 *   3. Write the unit file atomically.
 *   4. Run `systemctl --user enable --now controller-box`.
 *   5. Verify service is active via `systemctl --user is-active`.
 *
 * @param status_buf   Output buffer for a human-readable status message.
 * @param buflen       Size of status_buf.
 * @return CBX_SVC_OK / CBX_SVC_ALREADY_ACTIVE on success; negative
 *         CBX_SVC_* on error.
 */
int cbx_service_install(char *status_buf, size_t buflen);

/*
 * Check if the overlay service is currently active.
 * Runs `systemctl --user is-active controller-box`.
 *
 * @return 1 if active; 0 if inactive; -errno on error.
 */
int cbx_service_is_active(void);

/*
 * Uninstall (disable and remove) the overlay service.
 * Runs `systemctl --user disable controller-box` and unlinks the unit file.
 *
 * @return 0 on success; negative errno on error.
 */
int cbx_service_uninstall(void);

/* ------------------------------------------------------------------ */
/*  Test overrides                                                     */
/* ------------------------------------------------------------------ */

/*
 * Set a test override for the systemctl command.  When set, service
 * install/enable/verify calls use this command prefix instead of the
 * real `systemctl --user`.  Pass NULL to revert to the real systemctl.
 *
 * This allows tests to mock systemctl by pointing to a script that
 * always succeeds (or fails in a controlled way).
 *
 * @param cmd  Mock command prefix (e.g. "/bin/true"), or NULL to reset.
 */
void cbx_service_set_mock_systemctl(const char *cmd);

/*
 * Set a test override for the group check.  When set, the group check
 * uses this file instead of /etc/group.  Pass NULL to revert to the
 * real /etc/group.
 *
 * @param path  Path to a mock group file, or NULL to reset.
 */
void cbx_service_set_mock_group_file(const char *path);

/*
 * Set a test override for the current username in group checks.
 * When set, the group check searches for this username instead of
 * calling getpwuid(getuid()).  Pass NULL to revert to the real lookup.
 *
 * @param username  Mock username, or NULL to reset.
 */
void cbx_service_set_mock_username(const char *username);

#ifdef __cplusplus
}
#endif

#endif /* CBX_SERVICE_INSTALL_H */