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

/* --- Reactive property storage (SPEC §10.1 PropertiesChanged) ------------ */
/* Bounds mirror the ip_properties validation limits: strings up to 4096
 * bytes and arrays up to 256 elements.  Storing them on the per-device
 * model (rather than a process-global cache) is what lets a change for one
 * composite update only that composite's entry. */
#define CBX_MODEL_PROP_NAME_LEN   256
#define CBX_MODEL_PROP_PATH_LEN  4096
#define CBX_MODEL_PROP_ARRAY_LEN 4096

/* --- Entries -------------------------------------------------------------- */

/* Generic device entry (source or target).  `name` is the last path
 * component (e.g. "event0", "gamepad0"). */
typedef struct {
    char path[CBX_MAX_PATH_LEN];
    char name[CBX_MAX_NAME_LEN];
} cbx_device_entry;

/* Composite device entry.  `index` is the trailing integer parsed from
 * the path (CompositeDevice0 → 0).
 *
 * The reactive property fields hold the last validated value delivered by
 * org.freedesktop.DBus.Properties.PropertiesChanged for this exact object
 * path, so a Manager GamepadOrder change and a CompositeDevice
 * ProfileName/ProfilePath/TargetDevices/SourceDevicePaths change land on
 * the device they name (SPEC §10.1).  The `has_*` flags record that a
 * validated value (or an authoritative invalidation) has been applied.
 * `invalidated` is true when the last applied update was an authoritative
 * clear rather than a value. */
typedef struct {
    char path[CBX_MAX_PATH_LEN];
    int  index;

    bool has_profile_name;
    bool has_profile_path;
    bool has_target_devices;
    bool has_source_device_paths;
    bool profile_name_invalidated;
    bool profile_path_invalidated;
    bool target_devices_invalidated;
    bool source_device_paths_invalidated;
    char profile_name[CBX_MODEL_PROP_NAME_LEN];
    char profile_path[CBX_MODEL_PROP_PATH_LEN];
    char target_devices[CBX_MODEL_PROP_ARRAY_LEN];
    char source_device_paths[CBX_MODEL_PROP_ARRAY_LEN];
} cbx_composite_entry;

/* --- Device model --------------------------------------------------------- */

typedef struct {
    /* Manager — at most one, always at /org/shadowblip/InputPlumber/Manager */
    bool has_manager;
    char manager_path[CBX_MAX_PATH_LEN];

    /* Manager GamepadOrder property (SPEC §10.1) — a single global ordering,
     * so it lives on the model rather than on a composite entry. */
    bool has_gamepad_order;
    bool gamepad_order_invalidated;
    char gamepad_order[CBX_MODEL_PROP_ARRAY_LEN];

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

/* --- Reactive property application (SPEC §10.1) --------------------------- */

/* Apply a validated PropertiesChanged value to the per-device model.
 *
 * `object_path`, `iface_name`, and `prop_name` must already have passed
 * ip_properties validation (sender, interface, and path class).  This
 * function additionally rejects data that does not name a known object in
 * this model, so a composite change can never overwrite another device and
 * a Manager property can never land on a composite (and vice versa):
 *
 *   - GamepadOrder on the Manager interface updates model->gamepad_order
 *     only when object_path equals the known manager_path.
 *   - ProfileName/ProfilePath/TargetDevices/SourceDevicePaths on the
 *     CompositeDevice interface update only the matching composite entry.
 *
 * When `invalidated` is true the entry is cleared but the `has_*` flag is
 * still set — an authoritative "no value" was applied.  Returns true when
 * the model was updated (a known object/property), false otherwise. */
bool cbx_device_model_apply_property(cbx_device_model *model,
                                     const char *object_path,
                                     const char *iface_name,
                                     const char *prop_name,
                                     const char *value,
                                     bool invalidated);

/* Copy reactive property state for composites present in both models from
 * `prior` onto `next` by exact object path.  Used after a full
 * re-enumeration so a device that survived the rebuild keeps the property
 * values already observed reactively. */
void cbx_device_model_preserve_props(cbx_device_model *next,
                                     const cbx_device_model *prior);

#endif /* CBX_IP_DEVICE_MODEL_H */