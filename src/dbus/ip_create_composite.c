/*
 * ip_create_composite.c — CreateCompositeDevice temp file workaround (Task 15).
 *
 * See ip_create_composite.h for the gap #3 workaround description.
 */
#include "ip_create_composite.h"
#include "dbus/ip_manager.h"   /* IP_DBUS_MANAGER_PATH */
#include "dbus/ip_device_model.h" /* CBX_MAX_PATH_LEN */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>          /* fchmod */
#include <unistd.h>

/* mkstemps allows a suffix after the XXXXXX, unlike mkstemp which
 * requires XXXXXX at the very end.  If mkstemps is not available,
 * fall back to mkstemp without a suffix. */
#ifdef __GLIBC__
#  define CBX_HAVE_MKSTEMPS 1
#endif

/* Suffix appended to the mkstemp template — must not exceed 6 chars
 * after the XXXXXX to stay portable. */
#define CBX_TEMP_SUFFIX ".yaml"

/*
 * Resolve the temp directory: prefer XDG_RUNTIME_DIR, fall back to /tmp.
 * Returns a pointer to a static string (safe — env vars don't change
 * during a single call).
 */
static const char *
resolve_temp_dir(void)
{
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (dir && dir[0] != '\0')
        return dir;
    return "/tmp";
}

int
ip_create_composite_device(const ip_dbus_backend *backend,
                            ip_bus_handle bus,
                            const char *yaml_content,
                            char **out_path)
{
    if (!backend || !yaml_content || !out_path)
        return -EINVAL;

    *out_path = NULL;

    /* Build the temp file template: <dir>/controller-box-XXXXXX.yaml
     * mkstemps() allows a suffix after XXXXXX; if unavailable, use
     * mkstemp() without the suffix (InputPlumber reads content, not
     * the file extension). */
    const char *dir = resolve_temp_dir();
    char template[CBX_MAX_PATH_LEN + 32];

    int n = snprintf(template, sizeof(template), "%s/controller-box-XXXXXX"
                     CBX_TEMP_SUFFIX, dir);
    if (n < 0 || (size_t)n >= sizeof(template))
        return -ENAMETOOLONG;

    /* Create the temp file. */
    int fd;
#ifdef CBX_HAVE_MKSTEMPS
    fd = mkstemps(template, (int)strlen(CBX_TEMP_SUFFIX));
#else
    /* Strip the suffix for mkstemp — just use XXXXXX at end. */
    n = snprintf(template, sizeof(template), "%s/controller-box-XXXXXX", dir);
    if (n < 0 || (size_t)n >= sizeof(template))
        return -ENAMETOOLONG;
    fd = mkstemp(template);
#endif
    if (fd < 0)
        return -errno;

    /* Set restrictive permissions immediately. */
    if (fchmod(fd, 0600) != 0) {
        int saved = errno;
        close(fd);
        unlink(template);
        return -saved;
    }

    /* Write the YAML content to the temp file. */
    size_t total = strlen(yaml_content);
    const char *p = yaml_content;
    while (total > 0) {
        ssize_t written = write(fd, p, total);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            int saved = errno;
            close(fd);
            unlink(template);
            return -saved;
        }
        p += written;
        total -= (size_t)written;
    }

    /* Ensure data is on disk before the DBus call reads it. */
    if (fsync(fd) != 0) {
        /* Non-fatal — the data is in the page cache; InputPlumber
         * reads via normal I/O.  Log and continue. */
    }

    if (close(fd) != 0) {
        int saved = errno;
        unlink(template);
        return -saved;
    }

    /* Call CreateCompositeDevice via the backend, passing the temp
     * file path (not user-controlled).  The temp file is unlinked
     * below regardless of the call result. */
    int rc = backend->call_method(bus, IP_DBUS_NAME,
                                    IP_DBUS_MANAGER_PATH,
                                    IP_IFACE_MANAGER,
                                    "CreateCompositeDevice",
                                    "s", template, out_path);

    /* Always unlink the temp file — success or failure. */
    unlink(template);

    return rc;
}