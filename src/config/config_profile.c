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

    /* Validate the nested counts before any serializer walks the arrays.
     * A malformed in-memory count (for example a corrupted prop_count or
     * target_event_count) must never reach emit_mapping()/emit_profile_yaml()
     * where it would read past the fixed arrays. */
    for (int i = 0; i < p->mapping_count; i++) {
        const cbx_profile_mapping *m = &p->mappings[i];
        if (m->source_event.prop_count < 0 ||
            m->source_event.prop_count > CBX_MAX_EVENT_PROPS)
            return -EINVAL;
        if (m->target_event_count < 0 ||
            m->target_event_count > CBX_MAX_TARGET_EVENTS)
            return -EINVAL;
    }

    /* A profile that parsed unsupported structures may be loaded and
     * displayed, but re-serializing it would destroy content.  Reject it
     * here so every write path fails closed before touching the originals. */
    if (p->has_unsupported_content)
        return -ENOTSUP;

    return 0;
}

bool cbx_profile_is_lossless(const cbx_profile *p)
{
    return p != NULL && !p->has_unsupported_content;
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

/* Check an event for the full set of YAML security constraints that apply to
 * every event, including content inside containers that are otherwise
 * skipped: no custom tags and no aliases.  Aliases are rejected because the
 * event-based parser would otherwise let a document reference arbitrary
 * earlier nodes, defeating the bounded fixed-field model.  A standalone
 * anchor is harmless (it cannot be referenced once aliases are refused).
 * Returns 0 if OK, -EPERM if a forbidden construct is found. */
static int check_event_security(const yaml_event_t *ev)
{
    int rc = check_event_tags(ev);
    if (rc < 0)
        return rc;

    if (ev->type == YAML_ALIAS_EVENT)
        return -EPERM;

    return 0;
}

/*
 * Parser state machine.
 *
 * The parser walks the flat libyaml event stream with an explicit stack of
 * container frames.  Tracking the container kind (rather than a handful of
 * booleans) lets depth, shape, document boundaries and field capacities be
 * enforced uniformly, including inside unsupported ("advanced") content
 * that the simple v1 model cannot represent and therefore discards.
 */
typedef enum {
    PST_ROOT_MAP = 0,   /* the document's root mapping */
    PST_MAPPING_SEQ,    /* the top-level "mapping" sequence */
    PST_MAPPING_ITEM,   /* one entry inside the "mapping" sequence */
    PST_SOURCE_EVENT,   /* the "source_event" mapping */
    PST_SOURCE_PROPS,   /* the device-class property mapping */
    PST_TARGET_SEQ,     /* the "target_events" sequence */
    PST_TARGET_ITEM,    /* one entry inside "target_events" */
    PST_SKIP            /* unsupported/unknown container (content discarded) */
} parse_state;

#define PST_STACK_MAX (MAX_YAML_DEPTH + 4)

typedef struct {
    cbx_profile *p;

    int  depth;
    bool root_started;
    bool document_started;
    bool document_ended;
    bool got_stream_end;

    bool have_key;
    char current_key[CBX_MAX_EVENT_KEY_LEN];

    parse_state stack[PST_STACK_MAX];
    int stack_top;

    /* Current mapping entry being built. */
    cbx_profile_mapping current_mapping;
} parse_ctx;

static parse_state ctx_top(const parse_ctx *ctx)
{
    if (ctx->stack_top <= 0)
        return PST_ROOT_MAP;
    return ctx->stack[ctx->stack_top - 1];
}

static void ctx_push(parse_ctx *ctx, parse_state st)
{
    if (ctx->stack_top < PST_STACK_MAX)
        ctx->stack[ctx->stack_top] = st;
    ctx->stack_top++;
}

static void ctx_pop(parse_ctx *ctx)
{
    if (ctx->stack_top > 0)
        ctx->stack_top--;
}

/* Record that the document contains structures the simple v1 model cannot
 * represent.  Such a profile may still be loaded and displayed, but every
 * write path refuses to re-serialize it so the original content survives. */
static void mark_unsupported(parse_ctx *ctx)
{
    if (ctx->p)
        ctx->p->has_unsupported_content = true;
}

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

/* Add a property to the current mapping's source event.
 * Returns 0, or -E2BIG when the fixed field capacity is exceeded (never a
 * silent drop, which would corrupt the profile meaning). */
static int add_source_prop(parse_ctx *ctx, const char *key, const char *val)
{
    cbx_source_event *se = &ctx->current_mapping.source_event;
    if (se->prop_count < 0 || se->prop_count >= CBX_MAX_EVENT_PROPS)
        return -E2BIG;
    safe_copy(se->props[se->prop_count].key,
              sizeof(se->props[se->prop_count].key), key);
    safe_copy(se->props[se->prop_count].value,
              sizeof(se->props[se->prop_count].value), val);
    se->prop_count++;
    return 0;
}

/* Add a target event to the current mapping.
 * Returns 0, or -E2BIG when the fixed field capacity is exceeded. */
static int add_target_event(parse_ctx *ctx, const char *dev_class,
                              const char *val)
{
    if (ctx->current_mapping.target_event_count < 0 ||
        ctx->current_mapping.target_event_count >= CBX_MAX_TARGET_EVENTS)
        return -E2BIG;
    int idx = ctx->current_mapping.target_event_count;
    safe_copy(ctx->current_mapping.target_events[idx].device_class,
              sizeof(ctx->current_mapping.target_events[idx].device_class),
              dev_class);
    safe_copy(ctx->current_mapping.target_events[idx].value,
              sizeof(ctx->current_mapping.target_events[idx].value), val);
    ctx->current_mapping.target_event_count++;
    return 0;
}

/* Process a scalar based on the current container.
 * Returns 0 on success, negative errno on malformed shape or capacity
 * overflow. */
static int process_scalar(parse_ctx *ctx, const char *val)
{
    if (!val)
        val = "";

    switch (ctx_top(ctx)) {
    case PST_ROOT_MAP:
        if (!ctx->have_key) {
            safe_copy(ctx->current_key, sizeof(ctx->current_key), val);
            ctx->have_key = true;
        } else {
            if (strcmp(ctx->current_key, "version") == 0) {
                ctx->p->version = atoi(val);
            } else if (strcmp(ctx->current_key, "kind") == 0) {
                safe_copy(ctx->p->kind, sizeof(ctx->p->kind), val);
            } else if (strcmp(ctx->current_key, "name") == 0) {
                safe_copy(ctx->p->name, sizeof(ctx->p->name), val);
            } else if (strcmp(ctx->current_key, "description") == 0) {
                safe_copy(ctx->p->description, sizeof(ctx->p->description),
                          val);
            } else {
                /* Unknown top-level field: serializer would drop it. */
                mark_unsupported(ctx);
            }
            ctx->have_key = false;
        }
        return 0;

    case PST_MAPPING_ITEM:
        if (!ctx->have_key) {
            safe_copy(ctx->current_key, sizeof(ctx->current_key), val);
            ctx->have_key = true;
        } else {
            if (strcmp(ctx->current_key, "name") == 0) {
                safe_copy(ctx->current_mapping.name,
                          sizeof(ctx->current_mapping.name), val);
            } else {
                /* Unknown mapping field: serializer would drop it. */
                mark_unsupported(ctx);
            }
            ctx->have_key = false;
        }
        return 0;

    case PST_SOURCE_EVENT:
        if (!ctx->have_key) {
            safe_copy(ctx->current_key, sizeof(ctx->current_key), val);
            ctx->have_key = true;
        } else {
            /* Scalar source: <device_class>: <specifier> */
            safe_copy(ctx->current_mapping.source_event.device_class,
                      sizeof(ctx->current_mapping.source_event.device_class),
                      ctx->current_key);
            int rc = add_source_prop(ctx, "value", val);
            ctx->have_key = false;
            return rc;
        }
        return 0;

    case PST_SOURCE_PROPS:
        if (!ctx->have_key) {
            safe_copy(ctx->current_key, sizeof(ctx->current_key), val);
            ctx->have_key = true;
        } else {
            int rc = add_source_prop(ctx, ctx->current_key, val);
            ctx->have_key = false;
            return rc;
        }
        return 0;

    case PST_TARGET_ITEM:
        if (!ctx->have_key) {
            safe_copy(ctx->current_key, sizeof(ctx->current_key), val);
            ctx->have_key = true;
        } else {
            int rc = add_target_event(ctx, ctx->current_key, val);
            ctx->have_key = false;
            return rc;
        }
        return 0;

    case PST_MAPPING_SEQ:
    case PST_TARGET_SEQ:
        /* A bare scalar where a mapping item is required is malformed. */
        return -EINVAL;

    case PST_SKIP:
        return 0;

    default:
        return -EINVAL;
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
    ctx_push(&ctx, PST_ROOT_MAP);

    while (yaml_parser_parse(parser, &ev)) {
        /* Reject tags and aliases on every event, including content inside
         * containers that are otherwise skipped. */
        rc = check_event_security(&ev);
        if (rc < 0) {
            yaml_event_delete(&ev);
            break;
        }

        /* Enforce the single-document boundary: nothing may follow the
         * document end except the stream end. */
        if (ctx.document_ended && ev.type != YAML_STREAM_END_EVENT) {
            rc = -EINVAL;
            yaml_event_delete(&ev);
            break;
        }

        parse_state top = ctx_top(&ctx);

        switch (ev.type) {
        case YAML_STREAM_START_EVENT:
            break;

        case YAML_DOCUMENT_START_EVENT:
            /* Exactly one document per profile. */
            if (ctx.document_started) {
                rc = -EINVAL;
                goto fail_event;
            }
            ctx.document_started = true;
            break;

        case YAML_DOCUMENT_END_EVENT:
            ctx.document_ended = true;
            break;

        case YAML_MAPPING_START_EVENT: {
            ctx.depth++;
            if (ctx.depth > MAX_YAML_DEPTH) {
                rc = -EFBIG;
                goto fail_event;
            }

            parse_state child = PST_SKIP;
            bool push = true;

            if (top == PST_ROOT_MAP && !ctx.have_key && !ctx.root_started) {
                /* The document's root mapping; the sentinel frame already
                 * represents it, so do not push another frame. */
                ctx.root_started = true;
                push = false;
            } else if (top == PST_MAPPING_SEQ) {
                /* A new mapping entry. */
                child = PST_MAPPING_ITEM;
                memset(&ctx.current_mapping, 0, sizeof(ctx.current_mapping));
            } else if (top == PST_TARGET_SEQ) {
                /* A new target-event entry. */
                child = PST_TARGET_ITEM;
                ctx.have_key = false;
            } else if (top == PST_SOURCE_EVENT && ctx.have_key) {
                /* <device_class>: { props } */
                safe_copy(ctx.current_mapping.source_event.device_class,
                          sizeof(ctx.current_mapping.source_event.device_class),
                          ctx.current_key);
                child = PST_SOURCE_PROPS;
                ctx.have_key = false;
            } else if (top == PST_MAPPING_ITEM && ctx.have_key &&
                       strcmp(ctx.current_key, "source_event") == 0) {
                child = PST_SOURCE_EVENT;
                ctx.have_key = false;
            } else if (top == PST_TARGET_ITEM && ctx.have_key) {
                /* Advanced/complex target event (chord, delayed_chord,
                 * gamepad->mouse, ...).  It remains loadable, but the v1
                 * model stores only the device class and marks the profile
                 * unsupported so it is never destructively re-serialized. */
                rc = add_target_event(&ctx, ctx.current_key, "");
                if (rc < 0)
                    goto fail_event;
                mark_unsupported(&ctx);
                ctx.have_key = false;
            } else if (ctx.have_key) {
                /* Unknown key with a mapping value (root, mapping item or a
                 * source property): the serializer cannot represent it. */
                mark_unsupported(&ctx);
                ctx.have_key = false;
            } else {
                /* A mapping where none is allowed. */
                mark_unsupported(&ctx);
            }

            if (push)
                ctx_push(&ctx, child);
            break;
        }

        case YAML_SEQUENCE_START_EVENT: {
            ctx.depth++;
            if (ctx.depth > MAX_YAML_DEPTH) {
                rc = -EFBIG;
                goto fail_event;
            }

            parse_state child = PST_SKIP;
            if (top == PST_ROOT_MAP && ctx.have_key &&
                strcmp(ctx.current_key, "mapping") == 0) {
                child = PST_MAPPING_SEQ;
                ctx.have_key = false;
            } else if (top == PST_MAPPING_ITEM && ctx.have_key &&
                       strcmp(ctx.current_key, "target_events") == 0) {
                child = PST_TARGET_SEQ;
                ctx.have_key = false;
            } else if (ctx.have_key) {
                /* A sequence value the model cannot represent. */
                mark_unsupported(&ctx);
                ctx.have_key = false;
            } else {
                mark_unsupported(&ctx);
            }
            ctx_push(&ctx, child);
            break;
        }

        case YAML_SCALAR_EVENT: {
            const char *val = (const char *)ev.data.scalar.value;
            if (!val)
                val = "";
            rc = process_scalar(&ctx, val);
            if (rc < 0)
                goto fail_event;
            break;
        }

        case YAML_SEQUENCE_END_EVENT:
            if (ctx.depth <= 0) {
                rc = -EINVAL;
                goto fail_event;
            }
            ctx.depth--;
            if (top == PST_MAPPING_SEQ || top == PST_TARGET_SEQ ||
                top == PST_SKIP) {
                ctx_pop(&ctx);
            } else {
                rc = -EINVAL;
                goto fail_event;
            }
            break;

        case YAML_MAPPING_END_EVENT:
            if (ctx.depth <= 0) {
                rc = -EINVAL;
                goto fail_event;
            }
            ctx.depth--;
            if (top == PST_MAPPING_ITEM) {
                /* Commit the completed entry, refusing to silently drop
                 * entries beyond the fixed capacity. */
                if (ctx.p->mapping_count < 0 ||
                    ctx.p->mapping_count >= CBX_MAX_MAPPINGS) {
                    rc = -E2BIG;
                    goto fail_event;
                }
                ctx.p->mappings[ctx.p->mapping_count] = ctx.current_mapping;
                ctx.p->mapping_count++;
                memset(&ctx.current_mapping, 0, sizeof(ctx.current_mapping));
                ctx_pop(&ctx);
            } else if (top == PST_ROOT_MAP) {
                ctx.root_started = false;
                ctx_pop(&ctx);
            } else if (top == PST_TARGET_ITEM || top == PST_SOURCE_EVENT ||
                       top == PST_SOURCE_PROPS || top == PST_SKIP) {
                ctx_pop(&ctx);
                ctx.have_key = false;
            } else {
                rc = -EINVAL;
                goto fail_event;
            }
            break;

        case YAML_STREAM_END_EVENT:
            ctx.got_stream_end = true;
            yaml_event_delete(&ev);
            goto done;

        case YAML_NO_EVENT:
            rc = -EIO;
            goto fail_event;

        default:
            rc = -EINVAL;
            goto fail_event;
        }

        yaml_event_delete(&ev);
        continue;

fail_event:
        yaml_event_delete(&ev);
        break;
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

    /* Parse into a scratch profile and publish it only on success, so a
     * malformed document never leaves the caller holding partial data. */
    cbx_profile tmp;
    cbx_profile_init(&tmp);
    rc = parse_profile_events(&tmp, &parser);
    if (rc == 0)
        *p = tmp;

    yaml_parser_delete(&parser);
    fclose(f);

    return rc;
}

int cbx_profile_parse(cbx_profile *p, const char *yaml, size_t len)
{
    if (!p || !yaml)
        return -EINVAL;

    cbx_profile_init(p);

    /* Same fail-closed publish contract as cbx_profile_load(): parse into
     * a scratch profile, copy out only on success. */
    cbx_profile tmp;
    cbx_profile_init(&tmp);
    int rc = parse_profile_from_string(&tmp, yaml, len);
    if (rc == 0)
        *p = tmp;
    return rc;
}

/* --- Serialization (document-based emitter) ------------------------------- */

/*
 * Emit a single mapping entry to a YAML document.
 * Returns 0 on success, -ENOMEM on allocation failure.
 */
static int emit_mapping(yaml_document_t *doc, int parent_seq,
                         const cbx_profile_mapping *m)
{
    /* Defense-in-depth: never walk the fixed arrays with a malformed count,
     * even if a future caller bypasses cbx_profile_validate(). */
    if (m->source_event.prop_count < 0 ||
        m->source_event.prop_count > CBX_MAX_EVENT_PROPS ||
        m->target_event_count < 0 ||
        m->target_event_count > CBX_MAX_TARGET_EVENTS)
        return -EINVAL;

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
    /* Validate the whole in-memory structure (including nested source/target
     * counts) before any serializer access. */
    int vrc = cbx_profile_validate(p);
    if (vrc < 0)
        return vrc;

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

    /* Validate before touching the serializer: rejects malformed nested
     * counts and profiles that cannot be round-tripped losslessly. */
    int vrc = cbx_profile_validate(p);
    if (vrc < 0)
        return vrc;

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