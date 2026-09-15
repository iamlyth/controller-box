/*
 * config_profile.h — InputPlumber device_profile_v1 YAML parse and generate.
 *
 * Parses and serializes InputPlumber device profile YAML documents (SPEC §7.6).
 * Profiles are standard InputPlumber DeviceProfile YAML — the GUI's editor
 * generates `version: 1, kind: DeviceProfile, name, description, mapping`
 * documents.  There is no duplicate profile format; GUI metadata lives in
 * optional sidecar files (handled by Task 8).
 *
 * Security constraints (from plan security review):
 *   - YAML parsed via libyaml with max depth 50, max document size 1 MB,
 *     no custom tags.
 *   - Atomic write: temp file (mkstemp) + rename.
 */
#ifndef CBX_CONFIG_PROFILE_H
#define CBX_CONFIG_PROFILE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of mapping entries in a profile. */
#define CBX_MAX_MAPPINGS 128

/* Maximum number of target events per mapping entry. */
#define CBX_MAX_TARGET_EVENTS 16

/* Maximum number of source-event properties (key-value pairs). */
#define CBX_MAX_EVENT_PROPS 8

/* Maximum length of an event key (device class or property name). */
#define CBX_MAX_EVENT_KEY_LEN 64

/* Maximum length of an event value (event specifier string). */
#define CBX_MAX_EVENT_VAL_LEN 128

/* Maximum length of profile name / kind / description strings. */
#define CBX_MAX_PROFILE_NAME_LEN 256

/* A key-value pair for source-event properties (e.g. "button" = "Start"). */
typedef struct {
    char key[CBX_MAX_EVENT_KEY_LEN];
    char value[CBX_MAX_EVENT_VAL_LEN];
} cbx_event_prop;

/* Source event: a device-class key with a set of property key-value pairs.
 * Example YAML:
 *   gamepad:
 *     button: Start
 */
typedef struct {
    char device_class[CBX_MAX_EVENT_KEY_LEN];  /* e.g. "gamepad" */
    cbx_event_prop props[CBX_MAX_EVENT_PROPS];
    int prop_count;
} cbx_source_event;

/* Target event: a device-class key with a scalar value.
 * Example YAML:
 *   keyboard: KeyEsc
 */
typedef struct {
    char device_class[CBX_MAX_EVENT_KEY_LEN];  /* e.g. "keyboard" */
    char value[CBX_MAX_EVENT_VAL_LEN];         /* e.g. "KeyEsc" */
} cbx_target_event;

/* A single mapping entry in a profile. */
typedef struct {
    char name[CBX_MAX_PROFILE_NAME_LEN];
    cbx_source_event source_event;
    cbx_target_event target_events[CBX_MAX_TARGET_EVENTS];
    int target_event_count;
} cbx_profile_mapping;

/* A complete InputPlumber device_profile_v1 document.
 *
 * `has_unsupported_content` is set by the parser when the source YAML held
 * structures the simple v1 editor model cannot represent (for example an
 * advanced `chord`/`delayed_chord` target event, or an unknown key that the
 * serializer would silently drop).  Such profiles are still loadable and
 * displayable, but they must never be re-serialized: doing so would destroy
 * the unsupported content.  Every write path (`cbx_profile_save`,
 * `cbx_profile_save_to_dir`, `cbx_profile_serialize`) rejects them with
 * -ENOTSUP, and the UI refuses to open or clone such a profile. */
typedef struct {
    int version;                                   /* must be 1 */
    char kind[CBX_MAX_PROFILE_NAME_LEN];            /* "DeviceProfile" */
    char name[CBX_MAX_PROFILE_NAME_LEN];
    char description[CBX_MAX_PROFILE_NAME_LEN];
    cbx_profile_mapping mappings[CBX_MAX_MAPPINGS];
    int mapping_count;
    bool has_unsupported_content;
} cbx_profile;

/*
 * Initialize a profile with default values:
 *   version=1, kind="DeviceProfile", empty name/description, no mappings.
 */
void cbx_profile_init(cbx_profile *p);

/*
 * Load a profile from a file path.
 * Initializes the profile first, then overwrites with parsed YAML fields.
 * If the file does not exist, returns 0 with defaults.
 *
 * @param p    Output profile.
 * @param path File path to the device_profile_v1 YAML file.
 * @return 0 on success; negative errno on error.
 */
int cbx_profile_load(cbx_profile *p, const char *path);

/*
 * Parse a profile from a YAML string in memory.
 * Initializes the profile first, then overwrites with parsed YAML fields.
 *
 * @param p   Output profile.
 * @param yaml NUL-terminated YAML string.
 * @param len  Length of YAML content (or 0 to use strlen).
 * @return 0 on success; negative errno on error.
 */
int cbx_profile_parse(cbx_profile *p, const char *yaml, size_t len);

/*
 * Validate a profile's in-memory structure before it is serialized or
 * written: version must be 1, kind must be "DeviceProfile", mapping_count
 * and every mapping's source-property/target-event counts must be within
 * their fixed capacities, and the profile must not carry unsupported
 * content that serialization would silently drop.
 *
 * @return 0 if valid; -EINVAL if malformed; -ENOTSUP if the profile can
 *         only be loaded, not re-serialized.
 */
int cbx_profile_validate(const cbx_profile *p);

/*
 * Returns true when the profile has no unsupported structures recorded
 * from parsing and can therefore be re-serialized without losing meaning.
 */
bool cbx_profile_is_lossless(const cbx_profile *p);

/*
 * Save a profile to a file path (atomic write: temp file + rename, mode 0644).
 * Validates first (returns -EINVAL if invalid).
 *
 * @param p    Profile to save.
 * @param path Destination file path.
 * @return 0 on success; negative errno on error.
 */
int cbx_profile_save(const cbx_profile *p, const char *path);

/*
 * Serialize a profile to a YAML string (device_profile_v1 format).
 * Uses libyaml document-based emitter (not string concatenation).
 *
 * @param p   Profile to serialize.
 * @param buf Output: malloc'd buffer containing YAML (caller frees).
 * @param len Output: length of YAML content (excluding NUL).
 * @return 0 on success; negative errno on error.
 */
int cbx_profile_serialize(const cbx_profile *p, char **buf, size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* CBX_CONFIG_PROFILE_H */