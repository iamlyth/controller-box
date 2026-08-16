/*
 * config_settings.c — settings.yaml read/write for Controller-Box.
 *
 * Loads and saves the application settings file at
 * ~/.config/controller-box/settings.yaml (SPEC §7.3).
 *
 * Uses libyaml event-based parser (with depth/size/tag security constraints)
 * and document-based emitter for writing.
 */
#include "config_settings.h"

/* For path resolution (cbx_resolve_config_dir, cbx_ensure_dir). */
#include "config_paths.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <yaml.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* --- Constants ----------------------------------------------------------- */

/* Maximum YAML document size: 1 MB (security constraint). */
#define MAX_DOC_SIZE (1024 * 1024)

/* Maximum YAML nesting depth (security constraint). */
#define MAX_YAML_DEPTH 50

/* Settings file name within the config directory. */
#define SETTINGS_FILENAME "settings.yaml"

/* Known-good controller types (SPEC §5.3, §10.2).
 * These are the InputPlumber target device type strings the GUI supports. */
static const char *const known_types[] = {
    "xb360",
    "ds5",
    "deck",
    "gamepad",
    "mouse",
    "keyboard",
    "touchscreen",
    NULL,
};

/* Default controller type used for padding. */
#define DEFAULT_TYPE "xb360"

/* --- Defaults ------------------------------------------------------------ */

void cbx_settings_defaults(cbx_settings *s)
{
    memset(s, 0, sizeof(*s));
    strncpy(s->overlay_trigger, "Select+A", sizeof(s->overlay_trigger) - 1);
    s->launch_at_boot = true;
    strncpy(s->theme, "default", sizeof(s->theme) - 1);
    s->overlay_opacity = 0.85;
    s->virtual_controllers.count = 4;
    for (int i = 0; i < 4; i++)
        strncpy(s->virtual_controllers.types[i], DEFAULT_TYPE,
                sizeof(s->virtual_controllers.types[i]) - 1);
}

/* --- Known-good type check ----------------------------------------------- */

bool cbx_is_known_controller_type(const char *type)
{
    if (!type || type[0] == '\0')
        return false;
    for (int i = 0; known_types[i]; i++) {
        if (strcmp(type, known_types[i]) == 0)
            return true;
    }
    return false;
}

/* --- Icon overrides ----------------------------------------------------- */

const char *cbx_settings_icon_override(const cbx_settings *s,
                                         const char *type)
{
    if (!s || !type)
        return NULL;
    for (int i = 0; i < s->icon_override_count; i++) {
        if (strcmp(s->icon_overrides[i].type, type) == 0)
            return s->icon_overrides[i].icon;
    }
    return NULL;
}

int cbx_settings_set_icon_override(cbx_settings *s, const char *type,
                                    const char *icon)
{
    if (!s || !type || !icon)
        return -EINVAL;

    /* Check if an override for this type already exists. */
    for (int i = 0; i < s->icon_override_count; i++) {
        if (strcmp(s->icon_overrides[i].type, type) == 0) {
            strncpy(s->icon_overrides[i].icon, icon,
                    sizeof(s->icon_overrides[i].icon) - 1);
            s->icon_overrides[i].icon[sizeof(s->icon_overrides[i].icon) - 1] = '\0';
            return 0;
        }
    }

    if (s->icon_override_count >= CBX_MAX_ICON_OVERRIDES)
        return -ENOSPC;

    int idx = s->icon_override_count;
    strncpy(s->icon_overrides[idx].type, type,
            sizeof(s->icon_overrides[idx].type) - 1);
    s->icon_overrides[idx].type[sizeof(s->icon_overrides[idx].type) - 1] = '\0';
    strncpy(s->icon_overrides[idx].icon, icon,
            sizeof(s->icon_overrides[idx].icon) - 1);
    s->icon_overrides[idx].icon[sizeof(s->icon_overrides[idx].icon) - 1] = '\0';
    s->icon_override_count++;
    return 0;
}

int cbx_settings_remove_icon_override(cbx_settings *s, const char *type)
{
    if (!s || !type)
        return -EINVAL;
    for (int i = 0; i < s->icon_override_count; i++) {
        if (strcmp(s->icon_overrides[i].type, type) == 0) {
            /* Shift remaining entries down. */
            for (int j = i; j < s->icon_override_count - 1; j++)
                s->icon_overrides[j] = s->icon_overrides[j + 1];
            s->icon_override_count--;
            memset(&s->icon_overrides[s->icon_override_count], 0,
                   sizeof(s->icon_overrides[s->icon_override_count]));
            return 0;
        }
    }
    return -ENOENT;
}

/* --- Validation ---------------------------------------------------------- */

int cbx_settings_validate(const cbx_settings *s)
{
    if (!s)
        return -EINVAL;

    if (s->overlay_opacity < 0.0 || s->overlay_opacity > 1.0)
        return -EINVAL;

    if (s->virtual_controllers.count < 1 ||
        s->virtual_controllers.count > CBX_MAX_CONTROLLERS)
        return -EINVAL;

    for (int i = 0; i < s->virtual_controllers.count; i++) {
        if (!cbx_is_known_controller_type(s->virtual_controllers.types[i]))
            return -EINVAL;
    }

    /* Validate icon overrides. */
    if (s->icon_override_count < 0 ||
        s->icon_override_count > CBX_MAX_ICON_OVERRIDES)
        return -EINVAL;
    for (int i = 0; i < s->icon_override_count; i++) {
        if (s->icon_overrides[i].type[0] == '\0' ||
            s->icon_overrides[i].icon[0] == '\0')
            return -EINVAL;
    }

    return 0;
}

/* --- Clamping (internal) ------------------------------------------------- */

static void clamp_settings(cbx_settings *s)
{
    if (s->overlay_opacity < 0.0)
        s->overlay_opacity = 0.0;
    else if (s->overlay_opacity > 1.0)
        s->overlay_opacity = 1.0;

    if (s->virtual_controllers.count < 1)
        s->virtual_controllers.count = 1;
    else if (s->virtual_controllers.count > CBX_MAX_CONTROLLERS)
        s->virtual_controllers.count = CBX_MAX_CONTROLLERS;

    for (int i = 0; i < s->virtual_controllers.count; i++) {
        if (!cbx_is_known_controller_type(s->virtual_controllers.types[i]) ||
            s->virtual_controllers.types[i][0] == '\0') {
            strncpy(s->virtual_controllers.types[i], DEFAULT_TYPE,
                    sizeof(s->virtual_controllers.types[i]) - 1);
            s->virtual_controllers.types[i][sizeof(s->virtual_controllers.types[i]) - 1] = '\0';
        }
    }
}

/* --- YAML parsing (event-based) ------------------------------------------ */

/* Check an event for explicit tags (security: no custom tags).
 * Returns 0 if OK, -EPERM if a tag is found. */
static int check_event_tags(const yaml_event_t *ev)
{
    switch (ev->type) {
    case YAML_SCALAR_EVENT:
        if (ev->data.scalar.tag)
            return -EPERM;
        break;
    case YAML_MAPPING_START_EVENT:
        if (ev->data.mapping_start.tag)
            return -EPERM;
        break;
    case YAML_SEQUENCE_START_EVENT:
        if (ev->data.sequence_start.tag)
            return -EPERM;
        break;
    case YAML_DOCUMENT_START_EVENT: {
        const yaml_tag_directive_t *start =
            ev->data.document_start.tag_directives.start;
        const yaml_tag_directive_t *end =
            ev->data.document_start.tag_directives.end;
        if (start && end && start != end)
            return -EPERM;
        break;
    }
    default:
        break;
    }
    return 0;
}

/* Parse a YAML boolean (YAML 1.1 forms). Returns true/false. */
static bool parse_yaml_bool(const char *val)
{
    if (!val)
        return false;
    /* YAML 1.1 true values */
    if (strcmp(val, "true") == 0 || strcmp(val, "True") == 0 ||
        strcmp(val, "TRUE") == 0 || strcmp(val, "yes") == 0 ||
        strcmp(val, "Yes") == 0 || strcmp(val, "YES") == 0 ||
        strcmp(val, "on") == 0 || strcmp(val, "On") == 0 ||
        strcmp(val, "ON") == 0)
        return true;
    return false;
}

/* Process a scalar value based on the current key and context. */
static void process_scalar_value(cbx_settings *s, const char *key,
                                  const char *val, bool in_vc)
{
    if (in_vc) {
        if (strcmp(key, "count") == 0) {
            s->virtual_controllers.count = atoi(val);
        }
        /* "types" is handled by sequence, not scalar */
    } else {
        if (strcmp(key, "overlay_trigger") == 0) {
            strncpy(s->overlay_trigger, val, sizeof(s->overlay_trigger) - 1);
            s->overlay_trigger[sizeof(s->overlay_trigger) - 1] = '\0';
        } else if (strcmp(key, "launch_at_boot") == 0) {
            s->launch_at_boot = parse_yaml_bool(val);
        } else if (strcmp(key, "theme") == 0) {
            strncpy(s->theme, val, sizeof(s->theme) - 1);
            s->theme[sizeof(s->theme) - 1] = '\0';
        } else if (strcmp(key, "overlay_opacity") == 0) {
            s->overlay_opacity = strtod(val, NULL);
        }
    }
}

/* Add a type entry from the types sequence. */
static void add_type_entry(cbx_settings *s, const char *val, int *type_count)
{
    if (*type_count < CBX_MAX_CONTROLLERS) {
        strncpy(s->virtual_controllers.types[*type_count], val,
                sizeof(s->virtual_controllers.types[*type_count]) - 1);
        s->virtual_controllers.types[*type_count]
            [sizeof(s->virtual_controllers.types[*type_count]) - 1] = '\0';
        (*type_count)++;
    }
}

/*
 * Parse the YAML document from an open file into settings.
 * Caller has already applied defaults; this only overwrites fields
 * present in the YAML.
 *
 * Returns 0 on success, negative errno on error.
 */
static int parse_settings_yaml(cbx_settings *s, FILE *f)
{
    yaml_parser_t parser;
    yaml_event_t ev;
    int rc = 0;
    int depth = 0;
    bool root_started = false;
    bool in_vc_map = false;
    bool in_types_seq = false;
    bool in_icon_ovr_seq = false;
    bool in_icon_ovr_item = false;
    bool have_key = false;
    char current_key[CBX_MAX_STR_LEN] = "";
    int type_count = 0;
    int ovr_type_idx = -1;  /* current override item index */
    char ovr_key[CBX_ICON_OVR_TYPE_LEN] = "";  /* current override key */
    bool got_stream_end = false;

    if (!yaml_parser_initialize(&parser))
        return -ENOMEM;
    yaml_parser_set_input_file(&parser, f);

    while (yaml_parser_parse(&parser, &ev)) {
        /* Security: check for custom tags */
        int tag_rc = check_event_tags(&ev);
        if (tag_rc < 0) {
            rc = tag_rc;
            yaml_event_delete(&ev);
            break;
        }

        switch (ev.type) {
        case YAML_STREAM_START_EVENT:
            break;

        case YAML_DOCUMENT_START_EVENT:
            break;

        case YAML_MAPPING_START_EVENT:
            depth++;
            if (depth > MAX_YAML_DEPTH) {
                rc = -EFBIG; /* too deep */
                yaml_event_delete(&ev);
                goto done;
            }
            if (!root_started) {
                root_started = true;
            } else if (have_key) {
                if (strcmp(current_key, "virtual_controllers") == 0)
                    in_vc_map = true;
                have_key = false;
            }
            /* Entering an icon_override item mapping (inside the seq). */
            if (in_icon_ovr_seq && !in_icon_ovr_item) {
                in_icon_ovr_item = true;
                ovr_type_idx = -1;
                ovr_key[0] = '\0';
            }
            break;

        case YAML_SEQUENCE_START_EVENT:
            depth++;
            if (depth > MAX_YAML_DEPTH) {
                rc = -EFBIG;
                yaml_event_delete(&ev);
                goto done;
            }
            if (have_key) {
                if (strcmp(current_key, "types") == 0)
                    in_types_seq = true;
                else if (strcmp(current_key, "icon_overrides") == 0)
                    in_icon_ovr_seq = true;
                have_key = false;
            }
            break;

        case YAML_SCALAR_EVENT: {
            const char *val = (const char *)ev.data.scalar.value;
            if (!val)
                val = "";
            if (in_types_seq) {
                add_type_entry(s, val, &type_count);
            } else if (in_icon_ovr_item) {
                /* Inside an icon_override item: key-value pairs (type, icon). */
                if (ovr_type_idx < 0) {
                    /* This is a key. */
                    strncpy(ovr_key, val, sizeof(ovr_key) - 1);
                    ovr_key[sizeof(ovr_key) - 1] = '\0';
                    ovr_type_idx = 0; /* mark expecting value next */
                } else {
                    /* This is a value for the key in ovr_key. */
                    if (s->icon_override_count < CBX_MAX_ICON_OVERRIDES) {
                        int idx = s->icon_override_count;
                        if (strcmp(ovr_key, "type") == 0) {
                            strncpy(s->icon_overrides[idx].type, val,
                                    sizeof(s->icon_overrides[idx].type) - 1);
                            s->icon_overrides[idx].type[sizeof(s->icon_overrides[idx].type) - 1] = '\0';
                        } else if (strcmp(ovr_key, "icon") == 0) {
                            strncpy(s->icon_overrides[idx].icon, val,
                                    sizeof(s->icon_overrides[idx].icon) - 1);
                            s->icon_overrides[idx].icon[sizeof(s->icon_overrides[idx].icon) - 1] = '\0';
                        }
                        /* When we have both type and icon, commit the entry. */
                        if (s->icon_overrides[idx].type[0] != '\0' &&
                            s->icon_overrides[idx].icon[0] != '\0')
                            s->icon_override_count++;
                    }
                    ovr_type_idx = -1; /* expect key next */
                }
            } else if (!have_key) {
                strncpy(current_key, val, sizeof(current_key) - 1);
                current_key[sizeof(current_key) - 1] = '\0';
                have_key = true;
            } else {
                process_scalar_value(s, current_key, val, in_vc_map);
                have_key = false;
            }
            break;
        }

        case YAML_SEQUENCE_END_EVENT:
            depth--;
            if (in_types_seq)
                in_types_seq = false;
            else if (in_icon_ovr_seq)
                in_icon_ovr_seq = false;
            break;

        case YAML_MAPPING_END_EVENT:
            depth--;
            if (in_icon_ovr_item) {
                in_icon_ovr_item = false;
            } else if (in_vc_map) {
                in_vc_map = false;
            }
            break;

        case YAML_DOCUMENT_END_EVENT:
            break;

        case YAML_STREAM_END_EVENT:
            got_stream_end = true;
            yaml_event_delete(&ev);
            goto done;

        case YAML_NO_EVENT:
            /* Should not happen after successful parse */
            rc = -EIO;
            yaml_event_delete(&ev);
            goto done;

        default:
            /* Unexpected alias event or other */
            rc = -EINVAL;
            yaml_event_delete(&ev);
            goto done;
        }

        yaml_event_delete(&ev);
    }

    /* If we exited the loop without STREAM_END, the parser hit an error */
    if (!got_stream_end && rc == 0)
        rc = -EIO;

done:
    yaml_parser_delete(&parser);
    return rc;
}

/* --- Helpers: O_NOFOLLOW file open --------------------------------------- */

static FILE *open_read_nofollow(const char *path)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return NULL;
    FILE *f = fdopen(fd, "r");
    if (!f) {
        int e = errno;
        close(fd);
        errno = e;
        return NULL;
    }
    return f;
}

/* --- Load ---------------------------------------------------------------- */

int cbx_settings_load(cbx_settings *settings)
{
    if (!settings)
        return -EINVAL;

    /* Always start with defaults; YAML overwrites what it contains. */
    cbx_settings_defaults(settings);

    /* Resolve config directory path (no side effects). */
    char config_dir[PATH_MAX];
    int rc = cbx_resolve_config_dir(config_dir, sizeof(config_dir));
    if (rc < 0)
        return rc;

    /* Build settings.yaml path. */
    char path[PATH_MAX + 32];
    rc = snprintf(path, sizeof(path), "%s/%s", config_dir, SETTINGS_FILENAME);
    if (rc < 0 || (size_t)rc >= sizeof(path))
        return -ENAMETOOLONG;

    /* If file doesn't exist, return with defaults. */
    struct stat st;
    if (stat(path, &st) != 0) {
        if (errno == ENOENT)
            return 0; /* defaults */
        return -errno;
    }

    /* Check file size: must be < 1 MB. */
    if (st.st_size > MAX_DOC_SIZE)
        return -EFBIG;

    /* Open and parse. */
    FILE *f = open_read_nofollow(path);
    if (!f)
        return -errno;

    rc = parse_settings_yaml(settings, f);
    fclose(f);

    if (rc < 0)
        return rc;

    /* Clamp out-of-range values and pad types to count. */
    clamp_settings(settings);

    /* Ensure types array has exactly count entries (pad with default). */
    for (int i = 0; i < settings->virtual_controllers.count; i++) {
        if (settings->virtual_controllers.types[i][0] == '\0') {
            strncpy(settings->virtual_controllers.types[i], DEFAULT_TYPE,
                    sizeof(settings->virtual_controllers.types[i]) - 1);
        }
    }

    return 0;
}

/* --- Save ---------------------------------------------------------------- */

/*
 * Build a YAML document from settings and emit it to file.
 * Returns 0 on success, negative errno on error.
 */
static int emit_settings_yaml(const cbx_settings *s, FILE *f)
{
    yaml_emitter_t emitter;
    yaml_document_t doc;
    int rc = 0;

    if (!yaml_emitter_initialize(&emitter))
        return -ENOMEM;
    yaml_emitter_set_output_file(&emitter, f);

    /* Set indentation to 2 spaces. */
    yaml_emitter_set_indent(&emitter, 2);

    if (!yaml_document_initialize(&doc, NULL, NULL, NULL, 0, 0)) {
        yaml_emitter_delete(&emitter);
        return -ENOMEM;
    }

    /* Root mapping. */
    int root = yaml_document_add_mapping(&doc, NULL,
                                          YAML_BLOCK_MAPPING_STYLE);
    if (root == 0) { rc = -ENOMEM; goto out; }

    /* overlay_trigger */
    {
        int k = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)"overlay_trigger", -1, YAML_PLAIN_SCALAR_STYLE);
        int v = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)s->overlay_trigger, -1, YAML_DOUBLE_QUOTED_SCALAR_STYLE);
        if (!k || !v) { rc = -ENOMEM; goto out; }
        yaml_document_append_mapping_pair(&doc, root, k, v);
    }

    /* launch_at_boot */
    {
        const char *bv = s->launch_at_boot ? "true" : "false";
        int k = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)"launch_at_boot", -1, YAML_PLAIN_SCALAR_STYLE);
        int v = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)bv, -1, YAML_PLAIN_SCALAR_STYLE);
        if (!k || !v) { rc = -ENOMEM; goto out; }
        yaml_document_append_mapping_pair(&doc, root, k, v);
    }

    /* theme */
    {
        int k = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)"theme", -1, YAML_PLAIN_SCALAR_STYLE);
        int v = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)s->theme, -1, YAML_DOUBLE_QUOTED_SCALAR_STYLE);
        if (!k || !v) { rc = -ENOMEM; goto out; }
        yaml_document_append_mapping_pair(&doc, root, k, v);
    }

    /* overlay_opacity */
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f", s->overlay_opacity);
        int k = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)"overlay_opacity", -1, YAML_PLAIN_SCALAR_STYLE);
        int v = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)buf, -1, YAML_PLAIN_SCALAR_STYLE);
        if (!k || !v) { rc = -ENOMEM; goto out; }
        yaml_document_append_mapping_pair(&doc, root, k, v);
    }

    /* virtual_controllers (nested mapping) */
    {
        int k = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)"virtual_controllers", -1, YAML_PLAIN_SCALAR_STYLE);
        int vc_map = yaml_document_add_mapping(&doc, NULL,
                                                YAML_BLOCK_MAPPING_STYLE);
        if (!k || !vc_map) { rc = -ENOMEM; goto out; }
        yaml_document_append_mapping_pair(&doc, root, k, vc_map);

        /* count */
        {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", s->virtual_controllers.count);
            int ck = yaml_document_add_scalar(&doc, NULL,
                (yaml_char_t *)"count", -1, YAML_PLAIN_SCALAR_STYLE);
            int cv = yaml_document_add_scalar(&doc, NULL,
                (yaml_char_t *)buf, -1, YAML_PLAIN_SCALAR_STYLE);
            if (!ck || !cv) { rc = -ENOMEM; goto out; }
            yaml_document_append_mapping_pair(&doc, vc_map, ck, cv);
        }

        /* types (sequence) */
        {
            int tk = yaml_document_add_scalar(&doc, NULL,
                (yaml_char_t *)"types", -1, YAML_PLAIN_SCALAR_STYLE);
            int seq = yaml_document_add_sequence(&doc, NULL,
                YAML_BLOCK_SEQUENCE_STYLE);
            if (!tk || !seq) { rc = -ENOMEM; goto out; }
            yaml_document_append_mapping_pair(&doc, vc_map, tk, seq);

            for (int i = 0; i < s->virtual_controllers.count; i++) {
                int item = yaml_document_add_scalar(&doc, NULL,
                    (yaml_char_t *)s->virtual_controllers.types[i], -1,
                    YAML_PLAIN_SCALAR_STYLE);
                if (!item) { rc = -ENOMEM; goto out; }
                yaml_document_append_sequence_item(&doc, seq, item);
            }
        }
    }

    /* icon_overrides (sequence of mappings, only if non-empty) */
    if (s->icon_override_count > 0) {
        int ik = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)"icon_overrides", -1, YAML_PLAIN_SCALAR_STYLE);
        int ovr_seq = yaml_document_add_sequence(&doc, NULL,
            YAML_BLOCK_SEQUENCE_STYLE);
        if (!ik || !ovr_seq) { rc = -ENOMEM; goto out; }
        yaml_document_append_mapping_pair(&doc, root, ik, ovr_seq);

        for (int i = 0; i < s->icon_override_count; i++) {
            int item_map = yaml_document_add_mapping(&doc, NULL,
                YAML_BLOCK_MAPPING_STYLE);
            if (!item_map) { rc = -ENOMEM; goto out; }
            yaml_document_append_sequence_item(&doc, ovr_seq, item_map);

            int tk2 = yaml_document_add_scalar(&doc, NULL,
                (yaml_char_t *)"type", -1, YAML_PLAIN_SCALAR_STYLE);
            int tv2 = yaml_document_add_scalar(&doc, NULL,
                (yaml_char_t *)s->icon_overrides[i].type, -1,
                YAML_PLAIN_SCALAR_STYLE);
            if (!tk2 || !tv2) { rc = -ENOMEM; goto out; }
            yaml_document_append_mapping_pair(&doc, item_map, tk2, tv2);

            int ik2 = yaml_document_add_scalar(&doc, NULL,
                (yaml_char_t *)"icon", -1, YAML_PLAIN_SCALAR_STYLE);
            int iv2 = yaml_document_add_scalar(&doc, NULL,
                (yaml_char_t *)s->icon_overrides[i].icon, -1,
                YAML_PLAIN_SCALAR_STYLE);
            if (!ik2 || !iv2) { rc = -ENOMEM; goto out; }
            yaml_document_append_mapping_pair(&doc, item_map, ik2, iv2);
        }
    }

    if (!yaml_emitter_dump(&emitter, &doc))
        rc = -EIO;

out:
    /* Document is always initialized before any goto out, so always delete. */
    yaml_document_delete(&doc);
    yaml_emitter_delete(&emitter);
    return rc;
}

/* --- Icon overrides (helper functions are above) ----------------------- */
/* The icon override functions (cbx_settings_icon_override, set, remove)
 * are defined above, before the validation section. */

int cbx_settings_save(const cbx_settings *settings)
{
    if (!settings)
        return -EINVAL;

    /* Validate before writing. */
    int rc = cbx_settings_validate(settings);
    if (rc < 0)
        return rc;

    /* Resolve and create config directory (mode 0700). */
    char config_dir[PATH_MAX];
    rc = cbx_config_dir(config_dir, sizeof(config_dir));
    if (rc < 0)
        return rc;

    /* Build the target path. */
    char path[PATH_MAX + 32];
    rc = snprintf(path, sizeof(path), "%s/%s", config_dir, SETTINGS_FILENAME);
    if (rc < 0 || (size_t)rc >= sizeof(path))
        return -ENAMETOOLONG;

    /* Create temp file in the same directory (for atomic rename). */
    char tmpl[PATH_MAX + 48];
    rc = snprintf(tmpl, sizeof(tmpl), "%s/.settings.yaml.XXXXXX", config_dir);
    if (rc < 0 || (size_t)rc >= sizeof(tmpl))
        return -ENAMETOOLONG;

    int fd = mkstemp(tmpl);
    if (fd < 0)
        return -errno;

    /* Set file permissions to 0600 immediately. */
    if (fchmod(fd, 0600) != 0) {
        rc = -errno;
        close(fd);
        unlink(tmpl);
        return rc;
    }

    FILE *f = fdopen(fd, "w");
    if (!f) {
        rc = -errno;
        close(fd);
        unlink(tmpl);
        return rc;
    }

    rc = emit_settings_yaml(settings, f);
    if (rc < 0) {
        fclose(f); /* also closes fd */
        unlink(tmpl);
        return rc;
    }

    /* Flush and sync to disk before rename. */
    if (fflush(f) != 0) {
        rc = -errno;
        fclose(f);
        unlink(tmpl);
        return rc;
    }
    if (fsync(fileno(f)) != 0) {
        /* Non-fatal on some systems; continue with rename. */
    }

    if (fclose(f) != 0) {
        rc = -errno;
        unlink(tmpl);
        return rc;
    }

    /* Atomic rename. */
    if (rename(tmpl, path) != 0) {
        rc = -errno;
        unlink(tmpl);
        return rc;
    }

    return 0;
}