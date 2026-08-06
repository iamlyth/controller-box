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

#endif /* CBX_IP_DEVICE_MODEL_H */