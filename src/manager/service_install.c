/*
 * service_install.c — Systemd user service installation (SPEC §2.4, §9.1).
 *
 * Implements the first-run service installation: writes the systemd user
 * unit file atomically, enables it via systemctl, verifies it's running,
 * and checks the user's group membership for polkit authorization.
 *
 * The unit template is static — only the binary path (from CMake) or the
 * Flatpak app ID is compiled in.  No user-supplied values are
 * interpolated at runtime (security: no injection through unit files).
 *
 * Task 40 — Systemd service installation and manager integration test.
 */
#include "manager/service_install.h"

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ------------------------------------------------------------------ */
/*  Compile-time binary path                                            */
/* ------------------------------------------------------------------ */

/*
 * The binary path is determined at CMake configure time.  In a normal
 * install it's /usr/bin/controller-box.  For Flatpak, the ExecStart
 * line uses `flatpak run <app-id>` instead — we detect Flatpak at
 * runtime via the FLATPAK_ID env var (set by the Flatpak runtime).
 *
 * The service template (SPEC §2.4):
 *
 *   [Unit]
 *   Description=Controller-Box Overlay Service
 *   After=graphical-session.target
 *   PartOf=graphical-session.target
 *
 *   [Service]
 *   ExecStart=<binary> --overlay-service
 *   Restart=on-failure
 *   RestartSec=2s
 *
 *   [Install]
 *   WantedBy=graphical-session.target
 */

/* ------------------------------------------------------------------ */
/*  Test overrides (static state)                                      */
/* ------------------------------------------------------------------ */

static const char *mock_systemctl  = NULL;  /* mock systemctl command   */
static const char *mock_group_file  = NULL;  /* mock /etc/group path      */
static const char *mock_username    = NULL;  /* mock current username     */

void
cbx_service_set_mock_systemctl(const char *cmd)
{
    mock_systemctl = cmd;
}

void
cbx_service_set_mock_group_file(const char *path)
{
    mock_group_file = path;
}

void
cbx_service_set_mock_username(const char *username)
{
    mock_username = username;
}

/* ------------------------------------------------------------------ */
/*  Path resolution                                                    */
/* ------------------------------------------------------------------ */

int
cbx_service_unit_path(char *buf, size_t buflen)
{
    if (!buf || buflen == 0)
        return -EINVAL;

    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    const char *base = NULL;
    size_t need;

    if (xdg && xdg[0] == '/') {
        base = xdg;
    } else if (home) {
        base = home;
    } else {
        return -ENOENT;
    }

    /* base + "/.config/systemd/user/controller-box.service" or
     * base + "/systemd/user/controller-box.service" */
    need = strlen(base) + strlen("/.config/systemd/user/controller-box.service") + 1;
    if (xdg && xdg[0] == '/')
        need = strlen(base) + strlen("/systemd/user/controller-box.service") + 1;

    if (need > buflen)
        return -ENAMETOOLONG;

    if (xdg && xdg[0] == '/')
        snprintf(buf, buflen, "%s/systemd/user/controller-box.service", base);
    else
        snprintf(buf, buflen, "%s/.config/systemd/user/controller-box.service", base);

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Unit file content                                                  */
/* ------------------------------------------------------------------ */

/* Validate that a Flatpak app ID contains only safe characters.
 * Flatpak app IDs are reverse-DNS names (e.g. org.shadowblip.ControllerBox)
 * and must match [a-zA-Z0-9._-]+.  Rejecting unsafe characters prevents
 * injection of shell metacharacters into the systemd unit ExecStart line. */
static bool
flatpak_id_is_valid(const char *id)
{
    if (!id || !id[0])
        return false;
    for (const char *p = id; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') ||
              (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') ||
              *p == '.' || *p == '_' || *p == '-'))
            return false;
    }
    return true;
}

int
cbx_service_unit_content(char *buf, size_t buflen)
{
    if (!buf || buflen == 0)
        return -EINVAL;

    /*
     * Detect Flatpak runtime via the FLATPAK_ID environment variable
     * (set by the Flatpak runtime to the app ID).  When running inside
     * Flatpak, ExecStart uses `flatpak run <app-id>` so the service
     * survives reboots without the Flatpak app being manually launched.
     * The app ID is validated to prevent shell metacharacter injection.
     */
    const char *flatpak_id = getenv("FLATPAK_ID");

    const char *exec_start;
    char exec_buf[512];

    if (flatpak_id_is_valid(flatpak_id)) {
        snprintf(exec_buf, sizeof(exec_buf),
                 "flatpak run %s --overlay-service", flatpak_id);
        exec_start = exec_buf;
    } else {
        /*
         * Use the compile-time install prefix.  The CMake install puts
         * the binary at ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/
         * controller-box.  We hardcode /usr/bin/controller-box as the
         * default (matching the SPEC §9.3 install layout), with a
         * fallback to the configured binary path.
         */
#ifdef CBX_BINARY_PATH
        snprintf(exec_buf, sizeof(exec_buf),
                 "%s --overlay-service", CBX_BINARY_PATH);
        exec_start = exec_buf;
#else
        exec_start = "/usr/bin/controller-box --overlay-service";
#endif
    }

    const char *template =
        "[Unit]\n"
        "Description=Controller-Box Overlay Service\n"
        "After=graphical-session.target\n"
        "PartOf=graphical-session.target\n"
        "\n"
        "[Service]\n"
        "ExecStart=%s\n"
        "Restart=on-failure\n"
        "RestartSec=2s\n"
        "\n"
        "[Install]\n"
        "WantedBy=graphical-session.target\n";

    int len = snprintf(buf, buflen, template, exec_start);
    if (len < 0 || (size_t)len >= buflen)
        return -ENAMETOOLONG;

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Atomic file write                                                  */
/* ------------------------------------------------------------------ */

static int
write_atomic(const char *path, const char *content, size_t len, mode_t mode)
{
    if (!path || !content)
        return -EINVAL;

    /* Create parent directory if needed. */
    char dir[PATH_MAX];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        /* Recursively create parent dirs. */
        char *p = dir;
        if (*p == '/')
            p++;  /* skip leading / */
        while (p && *p) {
            char *next = strchr(p, '/');
            if (next)
                *next = '\0';
            if (dir[0])
                mkdir(dir, 0755);
            if (next) {
                *next = '/';
                p = next + 1;
            } else {
                break;
            }
        }
        if (dir[0])
            mkdir(dir, 0755);
    }

    /* Create temp file in the same directory (for atomic rename). */
    char tmppath[PATH_MAX + 8];
    snprintf(tmppath, sizeof(tmppath), "%s.XXXXXX", path);

    int fd = mkstemp(tmppath);
    if (fd < 0)
        return -errno;

    if (fchmod(fd, mode) < 0) {
        int saved = errno;
        close(fd);
        unlink(tmppath);
        return -saved;
    }

    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, content + written, len - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            int saved = errno;
            close(fd);
            unlink(tmppath);
            return -saved;
        }
        written += (size_t)n;
    }

    if (close(fd) < 0) {
        int saved = errno;
        unlink(tmppath);
        return -saved;
    }

    if (rename(tmppath, path) < 0) {
        int saved = errno;
        unlink(tmppath);
        return -saved;
    }

    return 0;
}

int
cbx_service_write_unit(const char *unit_path, const char *content)
{
    char path_buf[PATH_MAX];
    char content_buf[2048];
    int rc;

    if (!unit_path) {
        rc = cbx_service_unit_path(path_buf, sizeof(path_buf));
        if (rc != 0)
            return rc;
        unit_path = path_buf;
    }

    if (!content) {
        rc = cbx_service_unit_content(content_buf, sizeof(content_buf));
        if (rc != 0)
            return rc;
        content = content_buf;
    }

    return write_atomic(unit_path, content, strlen(content), 0644);
}

/* ------------------------------------------------------------------ */
/*  Group membership check                                             */
/* ------------------------------------------------------------------ */

static const char *
get_current_username(void)
{
    if (mock_username)
        return mock_username;

    struct passwd *pw = getpwuid(getuid());
    return pw ? pw->pw_name : NULL;
}

int
cbx_service_check_group(void)
{
    const char *username = get_current_username();
    if (!username)
        return -ENOENT;

    const char *group_path = mock_group_file ? mock_group_file : "/etc/group";
    FILE *f = fopen(group_path, "r");
    if (!f) {
        /* If /etc/group is not readable, assume not in group. */
        return 0;
    }

    char line[1024];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        /* Parse: groupname:passwd:gid:member1,member2,... */
        char *saveptr = NULL;
        char *grp = strtok_r(line, ":", &saveptr);
        if (!grp)
            continue;
        if (strcmp(grp, "inputplumber") != 0)
            continue;

        /* Skip passwd and gid fields. */
        strtok_r(NULL, ":", &saveptr);  /* passwd */
        strtok_r(NULL, ":", &saveptr);  /* gid */

        /* Members field. */
        char *members = strtok_r(NULL, "\n", &saveptr);
        if (members) {
            char *m_save = NULL;
            char *m = strtok_r(members, ",", &m_save);
            while (m) {
                if (strcmp(m, username) == 0) {
                    found = 1;
                    break;
                }
                m = strtok_r(NULL, ",", &m_save);
            }
        }
        break;  /* only one inputplumber line */
    }

    fclose(f);
    return found;
}

/* ------------------------------------------------------------------ */
/*  systemd availability                                               */
/* ------------------------------------------------------------------ */

static const char *
systemctl_prefix(void)
{
    if (mock_systemctl)
        return mock_systemctl;

    /* Detect Flatpak — use flatpak-spawn --host for systemctl. */
    if (flatpak_id_is_valid(getenv("FLATPAK_ID")))
        return "flatpak-spawn --host systemctl --user";

    return "systemctl --user";
}

int
cbx_service_systemd_available(void)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s is-system-running 2>/dev/null",
             systemctl_prefix());

    FILE *p = popen(cmd, "r");
    if (!p)
        return 0;

    char buf[128];
    int got = fgets(buf, sizeof(buf), p) != NULL;
    int status = pclose(p);

    (void)got;

    /* is-system-running returns non-zero for degraded states, but
     * that still means systemd is available.  Only return 0 if the
     * command itself failed to run (status == -1 or 127). */
    if (status == -1 || WIFEXITED(status) == 0)
        return 0;

    int exit_code = WEXITSTATUS(status);
    /* 0 = running, 1 = degraded, 2 = maintenance, etc. All mean systemd is present. */
    /* 127 = command not found */
    if (exit_code == 127)
        return 0;

    return 1;
}

/* ------------------------------------------------------------------ */
/*  Service active check                                               */
/* ------------------------------------------------------------------ */

int
cbx_service_is_active(void)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s is-active controller-box 2>/dev/null",
             systemctl_prefix());

    FILE *p = popen(cmd, "r");
    if (!p)
        return -errno;

    char buf[128];
    int got = (fgets(buf, sizeof(buf), p) != NULL);
    int status = pclose(p);
    (void)status;

    if (!got)
        return 0;

    /* Trim trailing whitespace. */
    char *nl = strchr(buf, '\n');
    if (nl)
        *nl = '\0';

    if (strcmp(buf, "active") == 0)
        return 1;

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Full install                                                       */
/* ------------------------------------------------------------------ */

int
cbx_service_install(char *status_buf, size_t buflen)
{
    if (status_buf && buflen > 0)
        status_buf[0] = '\0';

    /* 1. Check systemd availability. */
    if (!cbx_service_systemd_available()) {
        if (status_buf && buflen > 0)
            snprintf(status_buf, buflen,
                     "systemd user session not available");
        return CBX_SVC_NO_SYSTEMD;
    }

    /* 2. Check group membership (advisory — still install the unit). */
    int in_group = cbx_service_check_group();

    /* 3. Check if already active. */
    int active = cbx_service_is_active();
    if (active == 1) {
        if (status_buf && buflen > 0)
            snprintf(status_buf, buflen,
                     "Overlay service is already active");
        return CBX_SVC_ALREADY_ACTIVE;
    }

    /* 4. Write the unit file. */
    int rc = cbx_service_write_unit(NULL, NULL);
    if (rc != 0) {
        if (status_buf && buflen > 0)
            snprintf(status_buf, buflen,
                     "Failed to write unit file: %s", strerror(-rc));
        return CBX_SVC_WRITE_FAILED;
    }

    /* 5. Enable and start the service. */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s enable --now controller-box 2>&1",
             systemctl_prefix());

    FILE *p = popen(cmd, "r");
    if (!p) {
        if (status_buf && buflen > 0)
            snprintf(status_buf, buflen,
                     "Failed to run systemctl enable");
        return CBX_SVC_ENABLE_FAILED;
    }

    char out[256];
    if (fgets(out, sizeof(out), p))
        out[sizeof(out) - 1] = '\0';
    else
        out[0] = '\0';

    int status = pclose(p);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    if (exit_code != 0) {
        if (status_buf && buflen > 0)
            snprintf(status_buf, buflen,
                     "systemctl enable failed (exit %d): %s",
                     exit_code, out);
        return CBX_SVC_ENABLE_FAILED;
    }

    /* 6. Verify the service is active. */
    active = cbx_service_is_active();
    if (active != 1) {
        if (status_buf && buflen > 0)
            snprintf(status_buf, buflen,
                     "Service installed but not active");
        return CBX_SVC_VERIFY_FAILED;
    }

    /* 7. Report success (with group warning if needed). */
    if (status_buf && buflen > 0) {
        if (in_group == 0) {
            snprintf(status_buf, buflen,
                     "Overlay service installed and started. "
                     "WARNING: Add yourself to the 'inputplumber' group "
                     "for DBus access: sudo usermod -aG inputplumber $USER");
        } else {
            snprintf(status_buf, buflen,
                     "Overlay service installed and started");
        }
    }

    return CBX_SVC_OK;
}

/* ------------------------------------------------------------------ */
/*  Uninstall                                                          */
/* ------------------------------------------------------------------ */

int
cbx_service_uninstall(void)
{
    /* Disable the service. */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s disable controller-box 2>/dev/null",
             systemctl_prefix());
    int rc = system(cmd);
    (void)rc;

    /* Remove the unit file. */
    char path[PATH_MAX];
    rc = cbx_service_unit_path(path, sizeof(path));
    if (rc != 0)
        return rc;

    if (unlink(path) < 0 && errno != ENOENT)
        return -errno;

    return 0;
}