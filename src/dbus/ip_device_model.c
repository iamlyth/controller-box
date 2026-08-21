/*
 * ip_device_model.c — In-memory device model helpers.
 *
 * Initialisation, lookup, and incremental mutation helpers for the device
 * model populated by ObjectManager enumeration (ip_objectmanager.c) and
 * updated by hotplug signals (ip_hotplug.c).
 */
#include "ip_device_model.h"

#include "dbus_interface.h"  /* IP_DBUS_PATH */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Internal helpers ---------------------------------------------------- */

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
    const char *p = slash + 1;
    while (*p && !(*p >= '0' && *p <= '9'))
        p++;
    if (!*p)
        return -1;
    /* Bounds-checked parse (strtol, not atoi): the path component is
     * attacker-influenced DBus data and the value feeds fallback labels;
     * atoi on an out-of-range digit string is undefined behavior. */
    errno = 0;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p || errno == ERANGE || v < INT_MIN || v > INT_MAX)
        return -1;
    return (int)v;
}

/* --- Public API: init + lookups ------------------------------------------ */

void cbx_device_model_init(cbx_device_model *model)
{
    if (!model)
        return;
    memset(model, 0, sizeof(*model));
}

const cbx_composite_entry *
cbx_device_model_find_composite(const cbx_device_model *model,
                                  const char *path)
{
    if (!model || !path)
        return NULL;
    for (int i = 0; i < model->composite_count; i++) {
        if (strcmp(model->composites[i].path, path) == 0)
            return &model->composites[i];
    }
    return NULL;
}

const cbx_device_entry *
cbx_device_model_find_source(const cbx_device_model *model,
                              const char *path)
{
    if (!model || !path)
        return NULL;
    for (int i = 0; i < model->source_count; i++) {
        if (strcmp(model->sources[i].path, path) == 0)
            return &model->sources[i];
    }
    return NULL;
}

const cbx_device_entry *
cbx_device_model_find_target(const cbx_device_model *model,
                              const char *path)
{
    if (!model || !path)
        return NULL;
    for (int i = 0; i < model->target_count; i++) {
        if (strcmp(model->targets[i].path, path) == 0)
            return &model->targets[i];
    }
    return NULL;
}

/* --- Public API: incremental mutation (Task 11) ------------------------- */

bool
cbx_device_model_set_manager(cbx_device_model *model, const char *path)
{
    if (!model || !path)
        return false;
    model->has_manager = true;
    snprintf(model->manager_path, sizeof(model->manager_path), "%s", path);
    return true;
}

bool
cbx_device_model_remove_manager(cbx_device_model *model)
{
    if (!model || !model->has_manager)
        return false;
    model->has_manager = false;
    model->manager_path[0] = '\0';
    return true;
}

bool
cbx_device_model_add_composite(cbx_device_model *model, const char *path)
{
    if (!model || !path)
        return false;
    /* Idempotent: reject duplicates. */
    if (cbx_device_model_find_composite(model, path))
        return false;
    if (model->composite_count >= CBX_MAX_COMPOSITES)
        return false;
    cbx_composite_entry *e = &model->composites[model->composite_count];
    snprintf(e->path, sizeof(e->path), "%s", path);
    e->index = parse_composite_index(path);
    model->composite_count++;
    return true;
}

bool
cbx_device_model_remove_composite(cbx_device_model *model,
                                     const char *path)
{
    if (!model || !path)
        return false;
    for (int i = 0; i < model->composite_count; i++) {
        if (strcmp(model->composites[i].path, path) == 0) {
            /* Shift remaining entries down. */
            for (int j = i; j < model->composite_count - 1; j++)
                model->composites[j] = model->composites[j + 1];
            model->composite_count--;
            memset(&model->composites[model->composite_count], 0,
                   sizeof(model->composites[model->composite_count]));
            return true;
        }
    }
    return false;
}

bool
cbx_device_model_add_source(cbx_device_model *model, const char *path)
{
    if (!model || !path)
        return false;
    if (cbx_device_model_find_source(model, path))
        return false;
    if (model->source_count >= CBX_MAX_DEVICES)
        return false;
    cbx_device_entry *e = &model->sources[model->source_count];
    snprintf(e->path, sizeof(e->path), "%s", path);
    extract_name(path, e->name, sizeof(e->name));
    model->source_count++;
    return true;
}

bool
cbx_device_model_remove_source(cbx_device_model *model, const char *path)
{
    if (!model || !path)
        return false;
    for (int i = 0; i < model->source_count; i++) {
        if (strcmp(model->sources[i].path, path) == 0) {
            for (int j = i; j < model->source_count - 1; j++)
                model->sources[j] = model->sources[j + 1];
            model->source_count--;
            memset(&model->sources[model->source_count], 0,
                   sizeof(model->sources[model->source_count]));
            return true;
        }
    }
    return false;
}

bool
cbx_device_model_add_target(cbx_device_model *model, const char *path)
{
    if (!model || !path)
        return false;
    if (cbx_device_model_find_target(model, path))
        return false;
    if (model->target_count >= CBX_MAX_DEVICES)
        return false;
    cbx_device_entry *e = &model->targets[model->target_count];
    snprintf(e->path, sizeof(e->path), "%s", path);
    extract_name(path, e->name, sizeof(e->name));
    model->target_count++;
    return true;
}

bool
cbx_device_model_remove_target(cbx_device_model *model, const char *path)
{
    if (!model || !path)
        return false;
    for (int i = 0; i < model->target_count; i++) {
        if (strcmp(model->targets[i].path, path) == 0) {
            for (int j = i; j < model->target_count - 1; j++)
                model->targets[j] = model->targets[j + 1];
            model->target_count--;
            memset(&model->targets[model->target_count], 0,
                   sizeof(model->targets[model->target_count]));
            return true;
        }
    }
    return false;
}