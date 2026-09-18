/*
 * ip_objectmanager.c — ObjectManager enumeration (Task 10).
 *
 * Parses GetManagedObjects() replies into the in-memory device model.
 *
 * Text format (shared by production sd-bus backend and mock backend):
 *   <object_path>\t<iface1>,<iface2>,...
 *   # comment lines start with '#'
 *   (empty lines are ignored)
 *
 * Classification:
 *   - Interface list contains org.shadowblip.InputManager      → Manager
 *   - Interface list contains org.shadowblip.Input.CompositeDevice → Composite
 *   - Path prefix "/devices/source/"                     → Source
 *   - Path prefix "/devices/target/"                     → Target
 *
 * Security:
 *   - All object paths must start with "/org/shadowblip/InputPlumber/"
 *   - Invalid paths are silently skipped (logged in production)
 */
#include "ip_objectmanager.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Internal helpers ---------------------------------------------------- */

/* Check whether a comma-separated interface list contains `target`.
 * Does not allocate; splits in place. */
static bool
iface_list_contains(const char *list, const char *target)
{
    if (!list || !target)
        return false;

    size_t tlen = strlen(target);
    const char *p = list;

    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);

        if (len == tlen && strncmp(p, target, tlen) == 0)
            return true;

        if (!comma)
            break;
        p = comma + 1;
    }
    return false;
}

/* Validate that `path` starts with the InputPlumber root prefix. */
static bool
path_is_valid(const char *path)
{
    if (!path)
        return false;

    static const char prefix[] = IP_DBUS_PATH "/";
    size_t plen = sizeof(prefix) - 1;   /* exclude trailing '\0' */

    return strncmp(path, prefix, plen) == 0;
}

/* Extract the last component of a DBus path (after the final '/').
 * e.g. "/org/.../devices/source/event0" → "event0". */
static void
extract_name(const char *path, char *out, size_t out_size)
{
    if (!path || !out || out_size == 0)
        return;

    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;

    snprintf(out, out_size, "%s", name);
}

/* Parse the trailing integer from a CompositeDevice path.
 * e.g. "/org/.../CompositeDevice3" → 3.  Returns -1 on parse failure. */
static int
parse_composite_index(const char *path)
{
    if (!path)
        return -1;

    const char *slash = strrchr(path, '/');
    if (!slash)
        return -1;

    /* Skip non-digit characters after the last '/' to find the number. */
    const char *p = slash + 1;
    while (*p && !(*p >= '0' && *p <= '9'))
        p++;

    if (!*p)
        return -1;

    /* Parse the trailing number with bounds checking (strtol, not atoi).
     * The path component is attacker-influenced DBus data; atoi on an
     * out-of-range digit string is undefined behavior and wraps, and the
     * result feeds fallback labels.  Reject any value outside the signed
     * int range rather than silently wrapping. */
    errno = 0;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p || errno == ERANGE || v < INT_MIN || v > INT_MAX)
        return -1;
    return (int)v;
}

/* Add a composite entry to the model.  Returns true if added, false if full. */
static bool
add_composite(cbx_device_model *model, const char *path)
{
    if (model->composite_count >= CBX_MAX_COMPOSITES)
        return false;

    cbx_composite_entry *e = &model->composites[model->composite_count];
    snprintf(e->path, sizeof(e->path), "%s", path);
    e->index = parse_composite_index(path);
    model->composite_count++;
    return true;
}

/* Add a source device entry to the model. */
static bool
add_source(cbx_device_model *model, const char *path)
{
    if (model->source_count >= CBX_MAX_DEVICES)
        return false;

    cbx_device_entry *e = &model->sources[model->source_count];
    snprintf(e->path, sizeof(e->path), "%s", path);
    extract_name(path, e->name, sizeof(e->name));
    model->source_count++;
    return true;
}

/* Add a target device entry to the model. */
static bool
add_target(cbx_device_model *model, const char *path)
{
    if (model->target_count >= CBX_MAX_DEVICES)
        return false;

    cbx_device_entry *e = &model->targets[model->target_count];
    snprintf(e->path, sizeof(e->path), "%s", path);
    extract_name(path, e->name, sizeof(e->name));
    model->target_count++;
    return true;
}

static int
compare_composites(const void *a, const void *b)
{
    const cbx_composite_entry *ca = a;
    const cbx_composite_entry *cb = b;
    if (ca->index >= 0 && cb->index >= 0 && ca->index != cb->index)
        return ca->index < cb->index ? -1 : 1;
    return strcmp(ca->path, cb->path);
}

static int
compare_devices(const void *a, const void *b)
{
    const cbx_device_entry *da = a;
    const cbx_device_entry *db = b;
    return strcmp(da->path, db->path);
}

/* --- Public API ---------------------------------------------------------- */

int
cbx_objectmanager_parse_reply(const char *reply, cbx_device_model *model)
{
    if (!model)
        return -EINVAL;

    cbx_device_model_init(model);

    if (!reply)
        return 0;   /* empty reply → empty model (InputPlumber starting up) */

    /* Make a mutable copy for line-by-line strtok_r parsing. */
    char *copy = strdup(reply);
    if (!copy)
        return -ENOMEM;

    char *saveptr = NULL;
    char *line = strtok_r(copy, "\n", &saveptr);

    while (line) {
        /* Skip comments and empty lines. */
        if (*line == '#' || *line == '\0' || *line == '\r') {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;
        }

        /* Strip trailing \r (CRLF line endings). */
        size_t llen = strlen(line);
        if (llen > 0 && line[llen - 1] == '\r')
            line[llen - 1] = '\0';

        /* Split on tab: path \t interfaces */
        char *tab = strchr(line, '\t');
        if (!tab) {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;   /* malformed line — skip */
        }

        *tab = '\0';
        const char *path = line;
        const char *ifaces = tab + 1;

        /* Security: validate object path prefix. */
        if (!path_is_valid(path)) {
            line = strtok_r(NULL, "\n", &saveptr);
            continue;   /* invalid path — skip */
        }

        /* Classify and add to model. */
        if (iface_list_contains(ifaces, IP_IFACE_MANAGER)) {
            model->has_manager = true;
            snprintf(model->manager_path, sizeof(model->manager_path),
                     "%s", path);
        }

        if (iface_list_contains(ifaces, IP_IFACE_COMPOSITE)) {
            add_composite(model, path);
        }

        /* Source/target classification by path prefix (hardened — same
         * approach as ip_hotplug.c classify_device_path).  Uses exact
         * prefix match at the correct path position to prevent
         * misclassification via crafted DBus paths containing the
         * substring at an unexpected position. */
        static const char dev_prefix[] = IP_DBUS_PATH "/devices/";
        size_t dlen = sizeof(dev_prefix) - 1;
        if (strncmp(path, dev_prefix, dlen) == 0) {
            const char *rest = path + dlen;
            if (strncmp(rest, "source/", 7) == 0)
                add_source(model, path);
            else if (strncmp(rest, "target/", 7) == 0 &&
                     iface_list_contains(ifaces, IP_IFACE_TARGET))
                add_target(model, path);
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    /* ObjectManager dictionaries are unordered.  Stable path/index order is
     * required before any slot mapping is derived from the model. */
    qsort(model->composites, (size_t)model->composite_count,
          sizeof(model->composites[0]), compare_composites);
    qsort(model->sources, (size_t)model->source_count,
          sizeof(model->sources[0]), compare_devices);
    qsort(model->targets, (size_t)model->target_count,
          sizeof(model->targets[0]), compare_devices);

    free(copy);
    return 0;
}

int
cbx_objectmanager_enumerate(const ip_dbus_backend *backend,
                              ip_bus_handle bus,
                              cbx_device_model *model)
{
    if (!backend || !model)
        return -EINVAL;

    cbx_device_model_init(model);

    char *reply = NULL;
    int rc = backend->get_managed_objects(bus, IP_DBUS_NAME, IP_DBUS_PATH,
                                            &reply);
    if (rc < 0)
        return rc;

    /* Parse the reply (handles NULL/empty gracefully). */
    rc = cbx_objectmanager_parse_reply(reply, model);

    /* The reply is heap-allocated (strdup'd by the backend). */
    free(reply);

    return rc;
}