/*
 * ip_composite.h — CompositeDevice interface wrappers (Task 13).
 *
 * Thin wrappers around the InputPlumber CompositeDevice interface
 * (org.shadowblip.Input.CompositeDevice) at per-device paths
 * /org/shadowblip/InputPlumber/CompositeDevice{N}.  All wrappers go
 * through the ip_dbus_backend vtable so they are fully unit-testable
 * with the mock backend.
 *
 * SPEC §10.2 — CompositeDevice interface:
 *   SetInterceptActivation(events: as, target: s)  — register trigger combo
 *   InterceptMode: u (rw)                         — 0=NONE, 1=PASS, 2=ALL, 3=GAMEPAD_ONLY
 *   LoadProfilePath(path: s)                       — load profile from file
 *   LoadProfileFromYaml(yaml: s)                   — load profile from string
 *   GetProfileYaml() → s                           — dump current profile as YAML
 *   SetTargetDevices(types: as)                    — replace all target devices
 *   TargetDevices: as (r)                          — current target device paths
 *   SourceDevicePaths: as (r)                     — physical source device paths
 *   PersistentId: s (r)                           — persistent identifier
 *   Name: s (r)                                    — display name
 *   Capabilities: as (r)                          — input capabilities
 *   OutputCapabilities: as (r)                    — output capabilities
 *   TargetCapabilities: as (r)                    — target device capabilities
 *   DbusDevices: as (r)                           — DBusDevice object paths
 *   Stop()                                         — stop the composite device
 *
 * Security:
 *   - All wrappers return categorized error codes (IP_ERR_* or negative
 *     errno) on failure.
 *   - InterceptMode does NOT emit PropertiesChanged (gap #1) — see
 *     ip_intercept_poll.h for the polling state machine.
 */
#ifndef CBX_IP_COMPOSITE_H
#define CBX_IP_COMPOSITE_H

#include "dbus_mock.h"          /* ip_dbus_backend, ip_bus_handle, constants */

/* --- InterceptMode values (SPEC §10.2) ----------------------------------- */

#define IP_INTERCEPT_NONE         0u
#define IP_INTERCEPT_PASS         1u
#define IP_INTERCEPT_ALL           2u
#define IP_INTERCEPT_GAMEPAD_ONLY  3u

/* --- Method wrappers ----------------------------------------------------- */

/*
 * Register the overlay trigger combo on a composite device.
 * `composite_path` is the DBus path of the composite device.
 * `events_csv` is a comma-separated list of activation event names
 * (e.g. "Select,A").
 * `target_event` is the target event string (e.g. "Select+A").
 * Returns 0 on success, categorized error code on failure.
 */
int ip_composite_set_intercept_activation(const ip_dbus_backend *backend,
                                            ip_bus_handle bus,
                                            const char *composite_path,
                                            const char *events_csv,
                                            const char *target_event);

/*
 * Load a profile from a file path.
 * `composite_path` is the DBus path of the composite device.
 * `profile_path` is the filesystem path to the profile YAML.
 * Returns 0 on success, categorized error code on failure.
 */
int ip_composite_load_profile_path(const ip_dbus_backend *backend,
                                    ip_bus_handle bus,
                                    const char *composite_path,
                                    const char *profile_path);

/*
 * Load a profile from a YAML string.
 * `composite_path` is the DBus path of the composite device.
 * `yaml` is the profile YAML content.
 * Returns 0 on success, categorized error code on failure.
 */
int ip_composite_load_profile_from_yaml(const ip_dbus_backend *backend,
                                          ip_bus_handle bus,
                                          const char *composite_path,
                                          const char *yaml);

/*
 * Get the current profile as a YAML string.
 * On success, *out_yaml is heap-allocated.  Caller must free.
 * Returns 0 on success, categorized error code on failure.
 */
int ip_composite_get_profile_yaml(const ip_dbus_backend *backend,
                                    ip_bus_handle bus,
                                    const char *composite_path,
                                    char **out_yaml);

/*
 * Replace all target devices on a composite device.
 * `composite_path` is the DBus path of the composite device.
 * `types_csv` is a comma-separated list of device type strings
 * (e.g. "xb360,ds5").
 * Returns 0 on success, categorized error code on failure.
 */
int ip_composite_set_target_devices(const ip_dbus_backend *backend,
                                      ip_bus_handle bus,
                                      const char *composite_path,
                                      const char *types_csv);

/*
 * Stop the composite device.
 * Returns 0 on success, categorized error code on failure.
 */
int ip_composite_stop(const ip_dbus_backend *backend,
                       ip_bus_handle bus,
                       const char *composite_path);

/* --- Property wrappers --------------------------------------------------- */

/*
 * Get the InterceptMode property as a string (uint32 represented as text).
 * On success, *out_value is heap-allocated.  Caller must free.
 * Returns 0 on success, categorized error code on failure.
 */
int ip_composite_get_intercept_mode(const ip_dbus_backend *backend,
                                      ip_bus_handle bus,
                                      const char *composite_path,
                                      char **out_value);

/*
 * Set the InterceptMode property.
 * `value` is the mode string (e.g. "0", "1", "2", "3").
 * Returns 0 on success, categorized error code on failure.
 */
int ip_composite_set_intercept_mode(const ip_dbus_backend *backend,
                                      ip_bus_handle bus,
                                      const char *composite_path,
                                      const char *value);

/*
 * Get the TargetDevices property (comma-separated paths).
 * On success, *out_value is heap-allocated.  Caller must free.
 * Returns 0 on success, categorized error code on failure.
 */
int ip_composite_get_target_devices(const ip_dbus_backend *backend,
                                      ip_bus_handle bus,
                                      const char *composite_path,
                                      char **out_value);

/*
 * Get the SourceDevicePaths property (comma-separated paths).
 * On success, *out_value is heap-allocated.  Caller must free.
 * Returns 0 on success, categorized error code on failure.
 */
int ip_composite_get_source_device_paths(const ip_dbus_backend *backend,
                                           ip_bus_handle bus,
                                           const char *composite_path,
                                           char **out_value);

/*
 * Get the PersistentId property.
 * On success, *out_value is heap-allocated.  Caller must free.
 * Returns 0 on success, categorized error code on failure.
 */
int ip_composite_get_persistent_id(const ip_dbus_backend *backend,
                                     ip_bus_handle bus,
                                     const char *composite_path,
                                     char **out_value);

/*
 * Get the ProfileName property (SPEC §10.2).
 * Returns the display name of the currently loaded profile.
 * On success, *out_value is heap-allocated.  Caller must free.
 * Returns 0 on success, categorized error code on failure.
 */
int ip_composite_get_profile_name(const ip_dbus_backend *backend,
                                     ip_bus_handle bus,
                                     const char *composite_path,
                                     char **out_value);

/*
 * Get the ProfilePath property (SPEC §10.2).
 * Returns the filesystem path of the currently loaded profile.
 * Used to verify that LoadProfilePath actually loaded the expected
 * profile (SPEC §4.1-4.7: slot/profile changes update verified engine
 * state before persistence).
 * On success, *out_value is heap-allocated.  Caller must free.
 * Returns 0 on success, categorized error code on failure.
 */
int ip_composite_get_profile_path(const ip_dbus_backend *backend,
                                     ip_bus_handle bus,
                                     const char *composite_path,
                                     char **out_value);

/*
 * Get the Name property.
 * On success, *out_value is heap-allocated.  Caller must free.
 * Returns 0 on success, categorized error code on failure.
 */
int ip_composite_get_name(const ip_dbus_backend *backend,
                            ip_bus_handle bus,
                            const char *composite_path,
                            char **out_value);

/*
 * Get the Capabilities property (comma-separated capability strings).
 * On success, *out_value is heap-allocated.  Caller must free.
 * Returns 0 on success, categorized error code on failure.
 */
int ip_composite_get_capabilities(const ip_dbus_backend *backend,
                                    ip_bus_handle bus,
                                    const char *composite_path,
                                    char **out_value);

/*
 * Get the OutputCapabilities property (comma-separated strings).
 * On success, *out_value is heap-allocated.  Caller must free.
 * Returns 0 on success, categorized error code on failure.
 */
int ip_composite_get_output_capabilities(const ip_dbus_backend *backend,
                                            ip_bus_handle bus,
                                            const char *composite_path,
                                            char **out_value);

/*
 * Get the TargetCapabilities property (comma-separated strings).
 * On success, *out_value is heap-allocated.  Caller must free.
 * Returns 0 on success, categorized error code on failure.
 */
int ip_composite_get_target_capabilities(const ip_dbus_backend *backend,
                                            ip_bus_handle bus,
                                            const char *composite_path,
                                            char **out_value);

/*
 * Get the DbusDevices property (comma-separated DBusDevice object paths).
 * Used to correlate InputEvent signals to composite devices.
 * On success, *out_value is heap-allocated.  Caller must free.
 * Returns 0 on success, categorized error code on failure.
 */
int ip_composite_get_dbus_devices(const ip_dbus_backend *backend,
                                    ip_bus_handle bus,
                                    const char *composite_path,
                                    char **out_value);

#endif /* CBX_IP_COMPOSITE_H */