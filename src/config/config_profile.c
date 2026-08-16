/*
 * config_profile.c — InputPlumber device_profile_v1 YAML parse and generate.
 *
 * Parses and serializes InputPlumber device profile YAML documents (SPEC §7.6).
 *
 * Uses libyaml event-based parser (with depth/size/tag security constraints)
 * and document-based emitter for writing.
 *
 * Profile YAML structure (device_profile_v1):
 *
 *   version: 1
 *   kind: DeviceProfile
 *   name: Start Button to Escape Key
 *   description: Profile to map a gamepad's start button to the Escape keyboard key
 *   mapping:
 *     - name: Menu
 *       source_event:
 *         gamepad:
 *           button: Start
 *       target_events:
 *         - keyboard: KeyEsc
 */
#include "config_profile.h"

#include <errno.h>
#include <stdbool.h>
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

/* --- Init ---------------------------------------------------------------- */

void cbx_profile_init(cbx_profile *p)
{
    memset(p, 0, sizeof(*p));
    p->version = 1;
    strncpy(p->kind, "DeviceProfile", sizeof(p->kind) - 1);
}

/* --- Validation ---------------------------------------------------------- */

int cbx_profile_validate(const cbx_profile *p)
{
    if (!p)
        return -EINVAL;

    if (p->version != 1)
        return -EINVAL;

    if (strcmp(p->kind, "DeviceProfile") != 0)
        return -EINVAL;

    if (p->mapping_count < 0 || p->mapping_count > CBX_MAX_MAPPINGS)
        return -EINVAL;

    return 0;
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

/* Parser state machine context. */
typedef struct {
    cbx_profile *p;

    int depth;
    bool root_started;
    bool have_key;
    char current_key[CBX_MAX_EVENT_KEY_LEN];

    /* Context flags — set when entering, cleared when leaving */
    bool in_mapping_seq;      /* inside the "mapping" sequence (depth 2) */
    bool in_mapping_item;     /* inside a single mapping item (depth 3) */
    bool in_source_event;     /* inside source_event mapping (depth 4) */
    bool in_source_props;     /* inside device-class props mapping (depth 5) */
    bool in_target_seq;       /* inside target_events sequence (depth 4) */
    bool in_target_item;      /* inside a single target_event mapping (depth 5) */

    /* Current mapping entry being built */
    cbx_profile_mapping current_mapping;

    /* Source-event device class (stored when entering props) */
    char source_class[CBX_MAX_EVENT_KEY_LEN];

    /* Skip mode: when > 0, ignore content inside complex target-event values
     * (mapping instead of scalar).  Advanced InputPlumber mappings (chord,
     * delayed_chord) are accepted but not fully parsed by the v1 GUI. */
    int skip_depth;

    bool got_stream_end;
} parse_ctx;

/* Copy a string safely into a fixed-size buffer. */
static void safe_copy(char *dst, size_t dst_size, const char *src)
{
    if (!src || dst_size == 0) {
        if (dst && dst_size > 0)
            dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

/* Add a property to the current mapping's source event. */
static void add_source_prop(parse_ctx *ctx, const char *key, const char *val)
{
    cbx_source_event *se = &ctx->current_mapping.source_event;
    if (se->prop_count < CBX_MAX_EVENT_PROPS) {
        safe_copy(se->props[se->prop_count].key,
                  sizeof(se->props[se->prop_count].key), key);
        safe_copy(se->props[se->prop_count].value,
                  sizeof(se->props[se->prop_count].value), val);
        se->prop_count++;
    }
}

/* Add a target event to the current mapping. */
static void add_target_event(parse_ctx *ctx, const char *dev_class,
                              const char *val)
{
    if (ctx->current_mapping.target_event_count < CBX_MAX_TARGET_EVENTS) {
        int idx = ctx->current_mapping.target_event_count;
        safe_copy(ctx->current_mapping.target_events[idx].device_class,
                  sizeof(ctx->current_mapping.target_events[idx].device_class),
                  dev_class);
        safe_copy(ctx->current_mapping.target_events[idx].value,
                  sizeof(ctx->current_mapping.target_events[idx].value), val);
        ctx->current_mapping.target_event_count++;
    }
}

/* Process a scalar based on current context. */
static void process_scalar(parse_ctx *ctx, const char *val)
{
    if (!val)
        val = "";

    if (ctx->in_source_props) {
        /* Inside device-class props: key-value pairs */
        if (!ctx->have_key) {
            safe_copy(ctx->current_key, sizeof(ctx->current_key), val);
            ctx->have_key = true;
        } else {
            add_source_prop(ctx, ctx->current_key, val);
            ctx->have_key = false;
        }
    } else if (ctx->in_source_event) {
        /* Inside source_event mapping: first scalar is device class key */
        if (!ctx->have_key) {
            safe_copy(ctx->current_key, sizeof(ctx->current_key), val);
            ctx->have_key = true;
        } else {
            /* Scalar value for device class (not a mapping) */
            safe_copy(ctx->current_mapping.source_event.device_class,
                      sizeof(ctx->current_mapping.source_event.device_class),
                      ctx->current_key);
            add_source_prop(ctx, "value", val);
            ctx->have_key = false;
        }
    } else if (ctx->in_target_item) {
        /* Inside a target_event mapping: key=device class, value=specifier */
        if (!ctx->have_key) {
            safe_copy(ctx->current_key, sizeof(ctx->current_key), val);
            ctx->have_key = true;
        } else {
            add_target_event(ctx, ctx->current_key, val);
            ctx->have_key = false;
        }
    } else if (ctx->in_mapping_item) {
        /* Inside a mapping item: name, source_event, target_events keys */
        if (!ctx->have_key) {
            safe_copy(ctx->current_key, sizeof(ctx->current_key), val);
            ctx->have_key = true;
        } else {
            if (strcmp(ctx->current_key, "name") == 0) {
                safe_copy(ctx->current_mapping.name,
                          sizeof(ctx->current_mapping.name), val);
            }
            ctx->have_key = false;
        }
    } else if (!ctx->root_started) {
        /* Root-level scalar before root mapping: ignore */
    } else if (!ctx->have_key) {
        /* Top-level key */
        safe_copy(ctx->current_key, sizeof(ctx->current_key), val);
        ctx->have_key = true;
    } else {
        /* Top-level scalar value */
        if (strcmp(ctx->current_key, "version") == 0) {
            ctx->p->version = atoi(val);
        } else if (strcmp(ctx->current_key, "kind") == 0) {
            safe_copy(ctx->p->kind, sizeof(ctx->p->kind), val);
        } else if (strcmp(ctx->current_key, "name") == 0) {
            safe_copy(ctx->p->name, sizeof(ctx->p->name), val);
        } else if (strcmp(ctx->current_key, "description") == 0) {
            safe_copy(ctx->p->description, sizeof(ctx->p->description), val);
        }
        ctx->have_key = false;
    }
}

/*
 * Parse YAML events from a parser into a profile.
 * The profile has already been initialized by the caller.
 * Returns 0 on success, negative errno on error.
 */
static int parse_profile_events(cbx_profile *p, yaml_parser_t *parser)
{
    yaml_event_t ev;
    int rc = 0;
    parse_ctx ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.p = p;

    while (yaml_parser_parse(parser, &ev)) {
        int tag_rc = check_event_tags(&ev);
        if (tag_rc < 0) {
            rc = tag_rc;
            yaml_event_delete(&ev);
            break;
        }

        /* Skip mode: ignore content inside complex target-event values. */
        if (ctx.skip_depth > 0) {
            if (ev.type == YAML_MAPPING_START_EVENT ||
                ev.type == YAML_SEQUENCE_START_EVENT) {
                ctx.depth++;
            } else if (ev.type == YAML_MAPPING_END_EVENT ||
                       ev.type == YAML_SEQUENCE_END_EVENT) {
                ctx.depth--;
                if (ctx.depth < ctx.skip_depth)
                    ctx.skip_depth = 0;
            }
            yaml_event_delete(&ev);
            continue;
        }

        switch (ev.type) {
        case YAML_STREAM_START_EVENT:
            break;

        case YAML_DOCUMENT_START_EVENT:
            break;

        case YAML_MAPPING_START_EVENT:
            ctx.depth++;
            if (ctx.depth > MAX_YAML_DEPTH) {
                rc = -EFBIG;
                yaml_event_delete(&ev);
                goto done;
            }
            if (!ctx.root_started) {
                ctx.root_started = true;
            } else if (ctx.in_source_event && ctx.have_key) {
                /* Entering device-class props mapping */
                safe_copy(ctx.source_class,
                          sizeof(ctx.source_class), ctx.current_key);
                safe_copy(ctx.current_mapping.source_event.device_class,
                          sizeof(ctx.current_mapping.source_event.device_class),
                          ctx.current_key);
                ctx.in_source_props = true;
                ctx.have_key = false;
            } else if (ctx.in_mapping_item && ctx.have_key) {
                /* Entering source_event mapping */
                if (strcmp(ctx.current_key, "source_event") == 0) {
                    ctx.in_source_event = true;
                    ctx.have_key = false;
                }
            }
            /* Complex target event: value is a mapping (not a scalar).
             * Store device class with empty value and skip the nested content. */
            if (ctx.in_target_item && ctx.have_key) {
                add_target_event(&ctx, ctx.current_key, "");
                ctx.have_key = false;
                ctx.skip_depth = ctx.depth;
            }
            break;

        case YAML_SEQUENCE_START_EVENT:
            ctx.depth++;
            if (ctx.depth > MAX_YAML_DEPTH) {
                rc = -EFBIG;
                yaml_event_delete(&ev);
                goto done;
            }
            if (ctx.have_key) {
                if (!ctx.in_mapping_item) {
                    /* Top-level "mapping" key */
                    if (strcmp(ctx.current_key, "mapping") == 0)
                        ctx.in_mapping_seq = true;
                } else {
                    /* "target_events" inside a mapping item */
                    if (strcmp(ctx.current_key, "target_events") == 0)
                        ctx.in_target_seq = true;
                }
                ctx.have_key = false;
            }
            break;

        case YAML_SCALAR_EVENT: {
            const char *val = (const char *)ev.data.scalar.value;
            if (!val)
                val = "";
            process_scalar(&ctx, val);
            break;
        }

        case YAML_SEQUENCE_END_EVENT:
            ctx.depth--;
            if (ctx.in_target_seq)
                ctx.in_target_seq = false;
            else if (ctx.in_mapping_seq)
                ctx.in_mapping_seq = false;
            break;

        case YAML_MAPPING_END_EVENT:
            ctx.depth--;
            if (ctx.in_target_item) {
                ctx.in_target_item = false;
            } else if (ctx.in_source_props) {
                ctx.in_source_props = false;
            } else if (ctx.in_source_event) {
                ctx.in_source_event = false;
            } else if (ctx.in_mapping_item) {
                /* Commit the current mapping entry */
                if (ctx.p->mapping_count < CBX_MAX_MAPPINGS) {
                    ctx.p->mappings[ctx.p->mapping_count] =
                        ctx.current_mapping;
                    ctx.p->mapping_count++;
                }
                memset(&ctx.current_mapping, 0, sizeof(ctx.current_mapping));
                ctx.in_mapping_item = false;
            }
            break;

        case YAML_DOCUMENT_END_EVENT:
            break;

        case YAML_STREAM_END_EVENT:
            ctx.got_stream_end = true;
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

        /* Handle mapping items in the "mapping" sequence: detect entry */
        if (ev.type == YAML_MAPPING_START_EVENT && ctx.in_mapping_seq &&
            !ctx.in_mapping_item && !ctx.have_key) {
            /* Entering a new mapping item */
            ctx.in_mapping_item = true;
            memset(&ctx.current_mapping, 0, sizeof(ctx.current_mapping));
        }

        /* Handle target event items: detect entry from sequence */
        if (ev.type == YAML_MAPPING_START_EVENT && ctx.in_target_seq &&
            !ctx.in_target_item) {
            ctx.in_target_item = true;
            ctx.have_key = false;
        }

        yaml_event_delete(&ev);
    }

    if (!ctx.got_stream_end && rc == 0)
        rc = -EIO;

done:
    return rc;
}

/*
 * Parse a profile from a YAML string in memory.
 * The profile has already been initialized by the caller.
 */
static int parse_profile_from_string(cbx_profile *p, const char *yaml,
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

    rc = parse_profile_events(p, &parser);

    yaml_parser_delete(&parser);
    return rc;
}

/* --- Helpers: O_NOFOLLOW file open --------------------------------------- */

/* Open a file for reading with O_NOFOLLOW to prevent symlink attacks.
 * Returns a FILE* on success, NULL on error (errno set). */
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

int cbx_profile_load(cbx_profile *p, const char *path)
{
    if (!p || !path)
        return -EINVAL;

    cbx_profile_init(p);

    struct stat st;
    if (stat(path, &st) != 0) {
        if (errno == ENOENT)
            return 0;
        return -errno;
    }

    if (st.st_size > MAX_DOC_SIZE)
        return -EFBIG;

    FILE *f = open_read_nofollow(path);
    if (!f)
        return -errno;

    yaml_parser_t parser;
    int rc;

    if (!yaml_parser_initialize(&parser)) {
        fclose(f);
        return -ENOMEM;
    }
    yaml_parser_set_input_file(&parser, f);

    rc = parse_profile_events(p, &parser);

    yaml_parser_delete(&parser);
    fclose(f);

    return rc;
}

int cbx_profile_parse(cbx_profile *p, const char *yaml, size_t len)
{
    if (!p || !yaml)
        return -EINVAL;

    cbx_profile_init(p);
    return parse_profile_from_string(p, yaml, len);
}

/* --- Serialization (document-based emitter) ------------------------------- */

/*
 * Emit a single mapping entry to a YAML document.
 * Returns 0 on success, -ENOMEM on allocation failure.
 */
static int emit_mapping(yaml_document_t *doc, int parent_seq,
                         const cbx_profile_mapping *m)
{
    int item_map = yaml_document_add_mapping(doc, NULL,
        YAML_BLOCK_MAPPING_STYLE);
    if (!item_map)
        return -ENOMEM;
    yaml_document_append_sequence_item(doc, parent_seq, item_map);

    /* name */
    {
        int k = yaml_document_add_scalar(doc, NULL,
            (yaml_char_t *)"name", -1, YAML_PLAIN_SCALAR_STYLE);
        int v = yaml_document_add_scalar(doc, NULL,
            (yaml_char_t *)m->name, -1, YAML_DOUBLE_QUOTED_SCALAR_STYLE);
        if (!k || !v)
            return -ENOMEM;
        yaml_document_append_mapping_pair(doc, item_map, k, v);
    }

    /* source_event */
    {
        int k = yaml_document_add_scalar(doc, NULL,
            (yaml_char_t *)"source_event", -1, YAML_PLAIN_SCALAR_STYLE);
        int se_map = yaml_document_add_mapping(doc, NULL,
            YAML_BLOCK_MAPPING_STYLE);
        if (!k || !se_map)
            return -ENOMEM;
        yaml_document_append_mapping_pair(doc, item_map, k, se_map);

        /* Device class → props mapping */
        {
            int dc_key = yaml_document_add_scalar(doc, NULL,
                (yaml_char_t *)m->source_event.device_class, -1,
                YAML_PLAIN_SCALAR_STYLE);
            int props_map = yaml_document_add_mapping(doc, NULL,
                YAML_BLOCK_MAPPING_STYLE);
            if (!dc_key || !props_map)
                return -ENOMEM;
            yaml_document_append_mapping_pair(doc, se_map, dc_key, props_map);

            for (int i = 0; i < m->source_event.prop_count; i++) {
                int pk = yaml_document_add_scalar(doc, NULL,
                    (yaml_char_t *)m->source_event.props[i].key, -1,
                    YAML_PLAIN_SCALAR_STYLE);
                int pv = yaml_document_add_scalar(doc, NULL,
                    (yaml_char_t *)m->source_event.props[i].value, -1,
                    YAML_PLAIN_SCALAR_STYLE);
                if (!pk || !pv)
                    return -ENOMEM;
                yaml_document_append_mapping_pair(doc, props_map, pk, pv);
            }
        }
    }

    /* target_events */
    {
        int k = yaml_document_add_scalar(doc, NULL,
            (yaml_char_t *)"target_events", -1, YAML_PLAIN_SCALAR_STYLE);
        int te_seq = yaml_document_add_sequence(doc, NULL,
            YAML_BLOCK_SEQUENCE_STYLE);
        if (!k || !te_seq)
            return -ENOMEM;
        yaml_document_append_mapping_pair(doc, item_map, k, te_seq);

        for (int i = 0; i < m->target_event_count; i++) {
            int te_map = yaml_document_add_mapping(doc, NULL,
                YAML_BLOCK_MAPPING_STYLE);
            if (!te_map)
                return -ENOMEM;
            yaml_document_append_sequence_item(doc, te_seq, te_map);

            int dk = yaml_document_add_scalar(doc, NULL,
                (yaml_char_t *)m->target_events[i].device_class, -1,
                YAML_PLAIN_SCALAR_STYLE);
            int dv = yaml_document_add_scalar(doc, NULL,
                (yaml_char_t *)m->target_events[i].value, -1,
                YAML_PLAIN_SCALAR_STYLE);
            if (!dk || !dv)
                return -ENOMEM;
            yaml_document_append_mapping_pair(doc, te_map, dk, dv);
        }
    }

    return 0;
}

/*
 * Build a YAML document from a profile and emit it to a FILE*.
 * Returns 0 on success, negative errno on error.
 */
static int emit_profile_yaml(const cbx_profile *p, FILE *f)
{
    yaml_emitter_t emitter;
    yaml_document_t doc;
    int rc = 0;

    if (!yaml_emitter_initialize(&emitter))
        return -ENOMEM;
    yaml_emitter_set_output_file(&emitter, f);
    yaml_emitter_set_indent(&emitter, 2);

    if (!yaml_document_initialize(&doc, NULL, NULL, NULL, 0, 0)) {
        yaml_emitter_delete(&emitter);
        return -ENOMEM;
    }

    /* Root mapping */
    int root = yaml_document_add_mapping(&doc, NULL,
                                          YAML_BLOCK_MAPPING_STYLE);
    if (root == 0) { rc = -ENOMEM; goto out; }

    /* version */
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", p->version);
        int k = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)"version", -1, YAML_PLAIN_SCALAR_STYLE);
        int v = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)buf, -1, YAML_PLAIN_SCALAR_STYLE);
        if (!k || !v) { rc = -ENOMEM; goto out; }
        yaml_document_append_mapping_pair(&doc, root, k, v);
    }

    /* kind */
    {
        int k = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)"kind", -1, YAML_PLAIN_SCALAR_STYLE);
        int v = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)p->kind, -1, YAML_PLAIN_SCALAR_STYLE);
        if (!k || !v) { rc = -ENOMEM; goto out; }
        yaml_document_append_mapping_pair(&doc, root, k, v);
    }

    /* name */
    {
        int k = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)"name", -1, YAML_PLAIN_SCALAR_STYLE);
        int v = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)p->name, -1, YAML_DOUBLE_QUOTED_SCALAR_STYLE);
        if (!k || !v) { rc = -ENOMEM; goto out; }
        yaml_document_append_mapping_pair(&doc, root, k, v);
    }

    /* description */
    {
        int k = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)"description", -1, YAML_PLAIN_SCALAR_STYLE);
        int v = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)p->description, -1,
            YAML_DOUBLE_QUOTED_SCALAR_STYLE);
        if (!k || !v) { rc = -ENOMEM; goto out; }
        yaml_document_append_mapping_pair(&doc, root, k, v);
    }

    /* mapping (sequence) */
    {
        int k = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)"mapping", -1, YAML_PLAIN_SCALAR_STYLE);
        int seq = yaml_document_add_sequence(&doc, NULL,
            YAML_BLOCK_SEQUENCE_STYLE);
        if (!k || !seq) { rc = -ENOMEM; goto out; }
        yaml_document_append_mapping_pair(&doc, root, k, seq);

        for (int i = 0; i < p->mapping_count; i++) {
            rc = emit_mapping(&doc, seq, &p->mappings[i]);
            if (rc < 0)
                goto out;
        }
    }

    if (!yaml_emitter_dump(&emitter, &doc))
        rc = -EIO;

out:
    yaml_document_delete(&doc);
    yaml_emitter_delete(&emitter);
    return rc;
}

/* --- Serialize ----------------------------------------------------------- */

int cbx_profile_serialize(const cbx_profile *p, char **buf, size_t *len)
{
    if (!p || !buf)
        return -EINVAL;

    /* Use open_memstream for a growable memory buffer. */
    FILE *f = open_memstream(buf, len);
    if (!f)
        return -errno;

    int rc = emit_profile_yaml(p, f);
    if (rc < 0) {
        fclose(f);
        /* open_memstore allocates *buf even on failure; free it. */
        if (*buf) {
            free(*buf);
            *buf = NULL;
        }
        if (len)
            *len = 0;
        return rc;
    }

    if (fflush(f) != 0) {
        rc = -errno;
        fclose(f);
        if (*buf) {
            free(*buf);
            *buf = NULL;
        }
        if (len)
            *len = 0;
        return rc;
    }

    if (fclose(f) != 0) {
        rc = -errno;
        if (*buf) {
            free(*buf);
            *buf = NULL;
        }
        if (len)
            *len = 0;
        return rc;
    }

    return 0;
}

/* --- Save ---------------------------------------------------------------- */

int cbx_profile_save(const cbx_profile *p, const char *path)
{
    if (!p || !path)
        return -EINVAL;

    /* Validate before writing. */
    int rc = cbx_profile_validate(p);
    if (rc < 0)
        return rc;

    /* Extract directory from path for temp file creation. */
    char dir[PATH_MAX];
    const char *slash = strrchr(path, '/');
    if (slash) {
        size_t dir_len = (size_t)(slash - path);
        if (dir_len >= sizeof(dir))
            return -ENAMETOOLONG;
        memcpy(dir, path, dir_len);
        dir[dir_len] = '\0';
    } else {
        strncpy(dir, ".", sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';
    }

    /* Extract base filename for temp template. */
    const char *base = slash ? slash + 1 : path;

    /* Build temp file template: <dir>/.<base>.XXXXXX */
    char tmpl[PATH_MAX + 64];
    rc = snprintf(tmpl, sizeof(tmpl), "%s/.%s.XXXXXX", dir, base);
    if (rc < 0 || (size_t)rc >= sizeof(tmpl))
        return -ENAMETOOLONG;

    int fd = mkstemp(tmpl);
    if (fd < 0)
        return -errno;

    /* Profile files are InputPlumber format; use 0644 (readable by other tools). */
    if (fchmod(fd, 0644) != 0) {
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

    rc = emit_profile_yaml(p, f);
    if (rc < 0) {
        fclose(f);
        unlink(tmpl);
        return rc;
    }

    if (fflush(f) != 0) {
        rc = -errno;
        fclose(f);
        unlink(tmpl);
        return rc;
    }
    fsync(fileno(f));

    if (fclose(f) != 0) {
        rc = -errno;
        unlink(tmpl);
        return rc;
    }

    if (rename(tmpl, path) != 0) {
        rc = -errno;
        unlink(tmpl);
        return rc;
    }

    return 0;
}