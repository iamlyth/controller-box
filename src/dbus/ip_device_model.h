/*
 * ip_device_model.h — In-memory device model from ObjectManager enumeration.
 *
 * Represents the set of InputPlumber objects discovered via
 * GetManagedObjects(): the Manager, composite devices, source devices,
 * and target devices.  Later tasks (12–14) add property reads to enrich
 * this model; for now it stores object paths and basic classification.
 *
 * SPEC §10.1 — Object tree:
 *   /org/shadowblip/InputPlumber
 *   ├── Manager                         (org.shadowblip.InputManager)
 *   ├── CompositeDevice0, …             (org.shadowblip.Input.CompositeDevice)
 *   └── devices/
 *       ├── source/  event0, hidraw0, …
 *       └── target/  gamepad0, keyboard0, …
 */
#ifndef CBX_IP_DEVICE_MODEL_H
#define CBX_IP_DEVICE_MODEL_H

#include <stdbool.h>
#include <stddef.h>

/* --- Limits --------------------------------------------------------------- */

#define CBX_MAX_PATH_LEN     256   /* DBus object paths are short */
#define CBX_MAX_NAME_LEN     128
#define CBX_MAX_COMPOSITES    16
#define CBX_MAX_DEVICES      64   /* source or target devices */

/* --- Entries -------------------------------------------------------------- */

/* Generic device entry (source or target).  `name` is the last path
 * component (e.g. "event0", "gamepad0"). */
typedef struct {
    char path[CBX_MAX_PATH_LEN];
    char name[CBX_MAX_NAME_LEN];
} cbx_device_entry;

/* Composite device entry.  `index` is the trailing integer parsed from
 * the path (CompositeDevice0 → 0). */
typedef struct {
    char path[CBX_MAX_PATH_LEN];
    int  index;
} cbx_composite_entry;

/* --- Device model --------------------------------------------------------- */

typedef struct {
    /* Manager — at most one, always at /org/shadowblip/InputPlumber/Manager */
    bool has_manager;
    char manager_path[CBX_MAX_PATH_LEN];

    /* Composite devices — sorted by index */
    cbx_composite_entry composites[CBX_MAX_COMPOSITES];
    int                 composite_count;

    /* Source devices (under …/devices/source/) */
    cbx_device_entry    sources[CBX_MAX_DEVICES];
    int                 source_count;

    /* Target devices (under …/devices/target/) */
    cbx_device_entry    targets[CBX_MAX_DEVICES];
    int                 target_count;
} cbx_device_model;

/* --- API ------------------------------------------------------------------ */

/* Zero-initialise the model (all counts 0, has_manager false). */
void cbx_device_model_init(cbx_device_model *model);

/* Convenience: look up a composite by object path.  Returns NULL if not found. */
const cbx_composite_entry *
cbx_device_model_find_composite(const cbx_device_model *model,
                                  const char *path);

/* Convenience: look up a source device by object path.  Returns NULL if not found. */
const cbx_device_entry *
cbx_device_model_find_source(const cbx_device_model *model,
                              const char *path);

/* Convenience: look up a target device by object path.  Returns NULL if not found. */
const cbx_device_entry *
cbx_device_model_find_target(const cbx_device_model *model,
                              const char *path);

/* --- Incremental mutation (Task 11 — Hotplug) ----------------------------- */
/* These functions add or remove individual entries from the model.
 * They are used by the hotplug signal handlers to keep the model
 * in sync with InterfacesAdded / InterfacesRemoved signals.
 *
 * All add functions return true on success, false if the model is full
 * or the entry already exists (idempotent — duplicates are silently
 * rejected without error).  All remove functions return true if an
 * entry was removed, false if it was not found.
 *
 * Path validation (must start with IP_DBUS_PATH "/") is the caller's
 * responsibility — these functions trust the caller. */

/* Set the Manager entry (path + has_manager flag). */
bool cbx_device_model_set_manager(cbx_device_model *model, const char *path);

/* Remove the Manager entry. */
bool cbx_device_model_remove_manager(cbx_device_model *model);

/* Add a composite device entry (index parsed from path). */
bool cbx_device_model_add_composite(cbx_device_model *model, const char *path);

/* Remove a composite device entry by path. */
bool cbx_device_model_remove_composite(cbx_device_model *model,
                                         const char *path);

/* Add a source device entry (name extracted from path). */
bool cbx_device_model_add_source(cbx_device_model *model, const char *path);

/* Remove a source device entry by path. */
bool cbx_device_model_remove_source(cbx_device_model *model,
                                     const char *path);

/* Add a target device entry (name extracted from path). */
bool cbx_device_model_add_target(cbx_device_model *model, const char *path);

/* Remove a target device entry by path. */
bool cbx_device_model_remove_target(cbx_device_model *model,
                                     const char *path);

#endif /* CBX_IP_DEVICE_MODEL_H */