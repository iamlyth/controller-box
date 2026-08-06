/*
 * ip_device_model.c — In-memory device model helpers.
 *
 * Initialisation and lookup helpers for the device model populated by
 * ObjectManager enumeration (ip_objectmanager.c).
 */
#include "ip_device_model.h"

#include <string.h>

void
cbx_device_model_init(cbx_device_model *model)
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