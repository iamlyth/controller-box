/*
 * icon_map.c — Runtime icon mapping table (SPEC §8.4).
 *
 * Parses controller-icons.yaml using libyaml event-based parsing.
 * The YAML structure is:
 *
 *   virtual_types:
 *     - type: "xb360"
 *       icon: "cc-xbox-360"
 *       name: "Xbox 360 Controller"
 *     - ...
 *   custom_icons:
 *     - icon: "arcade-stick"
 *       name: "Arcade Stick"
 *     - ...
 *
 * Only the virtual_types entries are used for DeviceType→icon lookup.
 * custom_icons are informational (available for profile overrides).
 *
 * Security:
 *   - libyaml parser: max depth 50, max doc size 1 MB, no custom tags
 *   - Field lengths bounded (CBX_ICON_TYPE_LEN, CBX_ICON_NAME_LEN, etc.)
 *   - Entry count bounded (CBX_ICON_MAP_MAX_ENTRIES)
 */
#include "icons/icon_map.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <yaml.h>

#include "config.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* --- Constants ----------------------------------------------------------- */

#define MAX_DOC_SIZE  (1024 * 1024)   /* 1 MB */
#define MAX_YAML_DEPTH 50

/* Parser context states. */
typedef enum {
    MAP_STATE_TOP,            /* top level, expecting key */
    MAP_STATE_VIRTUAL_TYPES,  /* inside virtual_types: sequence */
    MAP_STATE_CUSTOM_ICONS,  /* inside custom_icons: sequence */
    MAP_STATE_ENTRY,          /* inside a mapping entry (type/icon/name) */
} map_state;

/* --- Init ---------------------------------------------------------------- */

void cbx_icon_map_init(cbx_icon_map *map)
{
    if (!map)
        return;
    memset(map, 0, sizeof(*map));
}

/* --- Helpers ------------------------------------------------------------- */

static void safe_copy(char *dst, size_t dst_size, const char *src)
{
    if (!src || dst_size == 0) {
        if (dst && dst_size > 0)
            dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

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

/* --- YAML parsing (event-based) ------------------------------------------ */

static int parse_icon_map_events(cbx_icon_map *map, yaml_parser_t *parser)
{
    yaml_event_t ev;
    int rc = 0;
    int depth = 0;
    map_state state = MAP_STATE_TOP;

    /* Current entry being built. */
    cbx_icon_entry cur_entry;
    memset(&cur_entry, 0, sizeof(cur_entry));
    bool have_key = false;
    char current_key[32] = {0};
    bool got_stream_end = false;

    while (yaml_parser_parse(parser, &ev)) {
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
                rc = -EFBIG;
                yaml_event_delete(&ev);
                goto done;
            }
            /* A mapping start inside a sequence = new entry. */
            if (state == MAP_STATE_VIRTUAL_TYPES || state == MAP_STATE_CUSTOM_ICONS) {
                state = MAP_STATE_ENTRY;
                memset(&cur_entry, 0, sizeof(cur_entry));
            }
            break;

        case YAML_SEQUENCE_START_EVENT:
            depth++;
            if (depth > MAX_YAML_DEPTH) {
                rc = -EFBIG;
                yaml_event_delete(&ev);
                goto done;
            }
            /* If we're at top level with a key, the sequence is the value. */
            if (state == MAP_STATE_TOP && have_key) {
                if (strcmp(current_key, "virtual_types") == 0)
                    state = MAP_STATE_VIRTUAL_TYPES;
                else if (strcmp(current_key, "custom_icons") == 0)
                    state = MAP_STATE_CUSTOM_ICONS;
                have_key = false;
            }
            break;

        case YAML_SCALAR_EVENT: {
            const char *val = (const char *)ev.data.scalar.value;
            if (!val)
                val = "";

            if (state == MAP_STATE_TOP && !have_key) {
                /* Top-level key. */
                safe_copy(current_key, sizeof(current_key), val);
                have_key = true;
            } else if (state == MAP_STATE_TOP && have_key) {
                /* Top-level value — should be a sequence, but handle scalar gracefully. */
                if (strcmp(current_key, "virtual_types") == 0)
                    state = MAP_STATE_VIRTUAL_TYPES;
                else if (strcmp(current_key, "custom_icons") == 0)
                    state = MAP_STATE_CUSTOM_ICONS;
                have_key = false;
            } else if (state == MAP_STATE_ENTRY && !have_key) {
                /* Key within an entry. */
                safe_copy(current_key, sizeof(current_key), val);
                have_key = true;
            } else if (state == MAP_STATE_ENTRY && have_key) {
                /* Value within an entry. */
                if (strcmp(current_key, "type") == 0)
                    safe_copy(cur_entry.type, sizeof(cur_entry.type), val);
                else if (strcmp(current_key, "icon") == 0)
                    safe_copy(cur_entry.icon, sizeof(cur_entry.icon), val);
                else if (strcmp(current_key, "name") == 0)
                    safe_copy(cur_entry.name, sizeof(cur_entry.name), val);
                /* Unknown keys: silently ignore. */
                have_key = false;
            }
            break;
        }

        case YAML_MAPPING_END_EVENT:
            depth--;
            if (state == MAP_STATE_ENTRY) {
                /* Entry complete — store it if it has a type field. */
                if (cur_entry.type[0] != '\0' &&
                    map->count < CBX_ICON_MAP_MAX_ENTRIES) {
                    /* Only store entries from virtual_types (those with type field). */
                    memcpy(&map->entries[map->count], &cur_entry,
                           sizeof(cur_entry));
                    map->count++;
                }
                /* Return to the sequence state. */
                state = MAP_STATE_VIRTUAL_TYPES;
                memset(&cur_entry, 0, sizeof(cur_entry));
            }
            break;

        case YAML_SEQUENCE_END_EVENT:
            depth--;
            if (state == MAP_STATE_VIRTUAL_TYPES || state == MAP_STATE_CUSTOM_ICONS)
                state = MAP_STATE_TOP;
            have_key = false;
            break;

        case YAML_DOCUMENT_END_EVENT:
            break;

        case YAML_STREAM_END_EVENT:
            got_stream_end = true;
            yaml_event_delete(&ev);
            goto done;

        case YAML_NO_EVENT:
            rc = -EIO;
            yaml_event_delete(&ev);
            goto done;

        default:
            rc = -EINVAL;
            yaml_event_delete(&ev);
            goto done;
        }

        yaml_event_delete(&ev);
    }

    if (!got_stream_end && rc == 0)
        rc = -EIO;

done:
    return rc;
}

static int parse_icon_map_from_string(cbx_icon_map *map, const char *yaml,
                                       size_t len)
{
    yaml_parser_t parser;
    int rc;

    if (len == 0)
        len = strlen(yaml);
    if (len > MAX_DOC_SIZE)
        return -EFBIG;

    if (!yaml_parser_initialize(&parser))
        return -ENOMEM;
    yaml_parser_set_input_string(&parser, (const yaml_char_t *)yaml, len);

    rc = parse_icon_map_events(map, &parser);

    yaml_parser_delete(&parser);
    return rc;
}

/* --- Load ---------------------------------------------------------------- */

int cbx_icon_map_load(cbx_icon_map *map, const char *path)
{
    if (!map || !path)
        return -EINVAL;

    cbx_icon_map_init(map);

    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return -errno;
    FILE *f = fdopen(fd, "rb");
    if (!f) {
        int e = errno;
        close(fd);
        errno = e;
        return -e;
    }

    /* Check file size. */
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -EIO;
    }
    long fsize = ftell(f);
    if (fsize < 0) {
        fclose(f);
        return -EIO;
    }
    if (fsize > MAX_DOC_SIZE) {
        fclose(f);
        return -EFBIG;
    }
    rewind(f);

    /* Read entire file. */
    char *buf = malloc((size_t)fsize + 1);
    if (!buf) {
        fclose(f);
        return -ENOMEM;
    }
    size_t nread = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    if (nread != (size_t)fsize) {
        free(buf);
        return -EIO;
    }
    buf[nread] = '\0';

    int rc = parse_icon_map_from_string(map, buf, nread);
    free(buf);

    if (rc == 0) {
        map->loaded = 1;
        snprintf(map->yaml_path, sizeof(map->yaml_path), "%s", path);
    }

    return rc;
}

int cbx_icon_map_parse(cbx_icon_map *map, const char *yaml, size_t len)
{
    if (!map || !yaml)
        return -EINVAL;

    cbx_icon_map_init(map);
    int rc = parse_icon_map_from_string(map, yaml, len);
    if (rc == 0)
        map->loaded = 1;
    return rc;
}

/* --- Lookup -------------------------------------------------------------- */

int cbx_icon_map_lookup(const cbx_icon_map *map, const char *type,
                         char *out_icon, size_t icon_size,
                         char *out_name, size_t name_size)
{
    if (!type)
        return -EINVAL;

    /* Search the loaded mapping. */
    if (map && map->loaded) {
        for (int i = 0; i < map->count; i++) {
            if (strcmp(map->entries[i].type, type) == 0) {
                if (out_icon && icon_size > 0)
                    safe_copy(out_icon, icon_size, map->entries[i].icon);
                if (out_name && name_size > 0)
                    safe_copy(out_name, name_size, map->entries[i].name);
                return 0;
            }
        }
    }

    /* Unknown type: default icon + raw type string as name (SPEC §8.4). */
    if (out_icon && icon_size > 0)
        safe_copy(out_icon, icon_size, CBX_ICON_DEFAULT_ICON);
    if (out_name && name_size > 0)
        safe_copy(out_name, name_size, type);

    return 0;
}

/* --- Default path -------------------------------------------------------- */

int cbx_icon_map_default_path(char *out_path, size_t path_size)
{
    if (!out_path || path_size == 0)
        return -EINVAL;

    int rc = snprintf(out_path, path_size, "%s/controller-icons.yaml",
                      DATA_DIR);
    if (rc < 0 || (size_t)rc >= path_size)
        return -ENAMETOOLONG;

    return 0;
}