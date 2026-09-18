/*
 * ip_create_composite.h — CreateCompositeDevice temp file workaround (Task 15).
 *
 * InputPlumber's Manager.CreateCompositeDevice(config_path: s) → s method
 * requires a YAML file path — there is no string-based variant on DBus
 * (gap #3, SPEC §10.3).  This module implements the workaround:
 *
 *   1. Write the composite-device YAML content to a temp file (mkstemp,
 *      mode 0600).
 *   2. Prefer XDG_RUNTIME_DIR over /tmp for the temp file location.
 *   3. Call CreateCompositeDevice with the temp file path.
 *   4. Unlink the temp file immediately after the call returns, whether
 *      it succeeded or failed.
 *
 * Security:
 *   - The temp file is created with mkstemp() (unique, unpredictable name).
 *   - File mode is 0600 (owner read/write only).
 *   - The temp file is always unlinked, even on failure.
 *   - User-controlled data (the YAML content) is written to the temp file,
 *     but the user never controls the file path.
 *   - No user-controlled paths are passed to CreateCompositeDevice.
 */
#ifndef CBX_IP_CREATE_COMPOSITE_H
#define CBX_IP_CREATE_COMPOSITE_H

#include "dbus_interface.h"          /* ip_dbus_backend, ip_bus_handle */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Create a composite device from YAML content.
 *
 * Writes `yaml_content` to a temp file (mode 0600, in XDG_RUNTIME_DIR or
 * /tmp), calls Manager.CreateCompositeDevice with the temp file path,
 * then unlinks the temp file regardless of outcome.
 *
 * @param backend       DBus backend vtable.
 * @param bus           DBus bus handle.
 * @param yaml_content  NUL-terminated composite-device YAML string.
 * @param out_path      On success, receives the new composite device
 *                      DBus object path (heap-allocated, caller frees).
 *                      Set to NULL on entry; remains NULL on failure.
 * @return 0 on success; negative errno or categorized error on failure.
 */
int ip_create_composite_device(const ip_dbus_backend *backend,
                                 ip_bus_handle bus,
                                 const char *yaml_content,
                                 char **out_path);

#ifdef __cplusplus
}
#endif

#endif /* CBX_IP_CREATE_COMPOSITE_H */