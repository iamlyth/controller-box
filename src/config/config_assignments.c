/*
 * config_assignments.c — assignments.yaml read/write for Controller-Box.
 *
 * Loads and saves the controller-to-slot assignment table and persisted
 * gamepad order (SPEC §7.4, gap #2 workaround).
 *
 * Uses libyaml event-based parser (with depth/size/tag security constraints)
 * and document-based emitter for writing.
 */
#include "config_assignments.h"
#include "config_settings.h"

/* For path resolution (cbx_resolve_config_dir, cbx_config_dir). */
#include "config_paths.h"

#include <ctype.h>
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

/* Assignments file name within the config directory. */
#define ASSIGNMENTS_FILENAME "assignments.yaml"

/* --- Init ---------------------------------------------------------------- */

void cbx_assignments_init(cbx_assignments *a)
{
    memset(a, 0, sizeof(*a));
}

/* --- Validation ---------------------------------------------------------- */

/*
 * Check if a character is a valid hex digit.
 */
static bool is_hex(char c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

/*
 * Check if a character is valid in a profile name or USB serial/phys.
 */
static bool is_profile_char(char c)
{
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

bool cbx_validate_id(const char *id)
{
    if (!id || id[0] == '\0')
        return false;

    /* Check maximum length */
    if (strlen(id) >= CBX_MAX_ID_LEN)
        return false;

    /* BT:xx:xx:xx:xx:xx:xx — Bluetooth MAC (6 hex octets, colon-separated) */
    if (strncmp(id, "BT:", 3) == 0) {
        const char *p = id + 3;
        for (int i = 0; i < 6; i++) {
            /* Two hex digits */
            if (!is_hex(p[0]) || !is_hex(p[1]))
                return false;
            p += 2;
            /* Colon between octets (not after the last one) */
            if (i < 5) {
                if (*p != ':')
                    return false;
                p++;
            }
        }
        /* Must be end of string */
        return *p == '\0';
    }

    /* USB:phys:xxxxx — USB port path (semi-stable) */
    if (strncmp(id, "USB:phys:", 9) == 0) {
        const char *p = id + 9;
        if (*p == '\0')
            return false;
        /* Allow non-empty string of printable non-space chars */
        for (; *p; p++) {
            if (*p <= ' ' || *p == 127)
                return false;
        }
        return true;
    }

    /* USB:xxxxx — USB serial (stable) */
    if (strncmp(id, "USB:", 4) == 0) {
        const char *p = id + 4;
        if (*p == '\0')
            return false;
        for (; *p; p++) {
            if (!is_profile_char(*p))
                return false;
        }
        return true;
    }

    /* ORDER:n — ordinal position (gap #2 workaround) */
    if (strncmp(id, "ORDER:", 6) == 0) {
        const char *p = id + 6;
        if (*p == '\0')
            return false;
        /* Must be a non-negative integer */
        for (; *p; p++) {
            if (!isdigit((unsigned char)*p))
                return false;
        }
        return true;
    }

    return false;
}

bool cbx_validate_profile(const char *profile)
{
    /* NULL or empty profile is valid (means "no profile assigned") */
    if (!profile || profile[0] == '\0')
        return true;

    if (strlen(profile) >= CBX_MAX_PROFILE_LEN)
        return false;

    for (const char *p = profile; *p; p++) {
        if (!is_profile_char(*p))
            return false;
    }
    return true;
}

int cbx_assignments_validate(const cbx_assignments *a)
{
    if (!a)
        return -EINVAL;

    if (a->assignment_count < 0 ||
        a->assignment_count > CBX_MAX_ASSIGNMENTS)
        return -EINVAL;

    if (a->gamepad_order_count < 0 ||
        a->gamepad_order_count > CBX_MAX_GAMEPAD_ORDER)
        return -EINVAL;

    for (int i = 0; i < a->assignment_count; i++) {
        if (!cbx_validate_id(a->assignments[i].id))
            return -EINVAL;
        if (a->assignments[i].slot < 0 ||
            a->assignments[i].slot >= CBX_MAX_CONTROLLERS)
            return -EINVAL;
        if (!cbx_validate_profile(a->assignments[i].profile))
            return -EINVAL;
    }

    for (int i = 0; i < a->gamepad_order_count; i++) {
        if (!cbx_validate_id(a->gamepad_order[i]))
            return -EINVAL;
    }

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
    cbx_assignments *a;

    int depth;
    bool root_started;

    /* "assignments" sequence state */
    bool in_assignments_seq;
    bool in_assignment_map;    /* inside a single assignment mapping */
    bool have_key;              /* we have a key awaiting a value */
    char current_key[CBX_MAX_PROFILE_LEN + 1];

    /* Current assignment being built */
    cbx_assignment current;

    /* "gamepad_order" sequence state */
    bool in_gamepad_order_seq;

    bool got_stream_end;
    bool parse_error;
} parse_ctx;

/* Process a scalar value based on the current key and context. */
static void process_scalar(parse_ctx *ctx, const char *val)
{
    if (ctx->in_assignment_map) {
        if (ctx->have_key) {
            if (strcmp(ctx->current_key, "id") == 0) {
                strncpy(ctx->current.id, val,
                        sizeof(ctx->current.id) - 1);
                ctx->current.id[sizeof(ctx->current.id) - 1] = '\0';
            } else if (strcmp(ctx->current_key, "slot") == 0) {
                char *end = NULL;
                long sl = strtol(val, &end, 10);
                if (end == val || *end != '\0' || sl < 0 ||
                    sl >= (long)CBX_MAX_CONTROLLERS) {
                    ctx->parse_error = true;
                    return;
                }
                ctx->current.slot = (int)sl;
            } else if (strcmp(ctx->current_key, "profile") == 0) {
                strncpy(ctx->current.profile, val,
                        sizeof(ctx->current.profile) - 1);
                ctx->current.profile[sizeof(ctx->current.profile) - 1] = '\0';
            }
            ctx->have_key = false;
        } else {
            /* This is a key */
            strncpy(ctx->current_key, val, sizeof(ctx->current_key) - 1);
            ctx->current_key[sizeof(ctx->current_key) - 1] = '\0';
            ctx->have_key = true;
        }
    } else if (ctx->in_gamepad_order_seq) {
        /* This is a gamepad_order entry (an id string) */
        if (ctx->a->gamepad_order_count < CBX_MAX_GAMEPAD_ORDER) {
            strncpy(ctx->a->gamepad_order[ctx->a->gamepad_order_count],
                    val, CBX_MAX_ID_LEN - 1);
            ctx->a->gamepad_order[ctx->a->gamepad_order_count]
                [CBX_MAX_ID_LEN - 1] = '\0';
            ctx->a->gamepad_order_count++;
        }
    } else if (!ctx->root_started) {
        /* Root-level scalar: can't happen in a valid mapping, ignore */
    } else if (!ctx->have_key) {
        /* This is a top-level key */
        strncpy(ctx->current_key, val, sizeof(ctx->current_key) - 1);
        ctx->current_key[sizeof(ctx->current_key) - 1] = '\0';
        ctx->have_key = true;
    } else {
        /* Top-level scalar value (shouldn't happen for this schema) */
        ctx->have_key = false;
    }
}

/*
 * Parse the YAML document from an open file into assignments.
 * Returns 0 on success, negative errno on error.
 */
static int parse_assignments_yaml(cbx_assignments *a, FILE *f)
{
    yaml_parser_t parser;
    yaml_event_t ev;
    int rc = 0;
    parse_ctx ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.a = a;

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
            ctx.depth++;
            if (ctx.depth > MAX_YAML_DEPTH) {
                rc = -EFBIG;
                yaml_event_delete(&ev);
                goto done;
            }
            if (!ctx.root_started) {
                ctx.root_started = true;
            } else if (ctx.have_key) {
                /* Entering a nested mapping; key tells us which one */
                if (strcmp(ctx.current_key, "assignments") == 0) {
                    /* This would be a single mapping under "assignments",
                     * but the spec uses a sequence.  This path handles
                     * malformed YAML gracefully — treat as no-op. */
                }
                ctx.have_key = false;
            } else if (ctx.in_assignments_seq) {
                /* Entering a single assignment mapping */
                ctx.in_assignment_map = true;
                memset(&ctx.current, 0, sizeof(ctx.current));
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
                if (strcmp(ctx.current_key, "assignments") == 0)
                    ctx.in_assignments_seq = true;
                else if (strcmp(ctx.current_key, "gamepad_order") == 0)
                    ctx.in_gamepad_order_seq = true;
                ctx.have_key = false;
            }
            break;

        case YAML_SCALAR_EVENT: {
            const char *val = (const char *)ev.data.scalar.value;
            if (!val)
                val = "";
            process_scalar(&ctx, val);
            if (ctx.parse_error) {
                rc = -EINVAL;
                yaml_event_delete(&ev);
                goto done;
            }
            break;
        }

        case YAML_SEQUENCE_END_EVENT:
            ctx.depth--;
            if (ctx.in_assignments_seq)
                ctx.in_assignments_seq = false;
            if (ctx.in_gamepad_order_seq)
                ctx.in_gamepad_order_seq = false;
            break;

        case YAML_MAPPING_END_EVENT:
            ctx.depth--;
            if (ctx.in_assignment_map) {
                /* Finish current assignment: store if there's room */
                if (ctx.a->assignment_count < CBX_MAX_ASSIGNMENTS) {
                    ctx.a->assignments[ctx.a->assignment_count] =
                        ctx.current;
                    ctx.a->assignment_count++;
                }
                ctx.in_assignment_map = false;
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

        yaml_event_delete(&ev);
    }

    if (!ctx.got_stream_end && rc == 0)
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

int cbx_assignments_load(cbx_assignments *a)
{
    if (!a)
        return -EINVAL;

    cbx_assignments_init(a);

    /* Resolve config directory path (no side effects). */
    char config_dir[PATH_MAX];
    int rc = cbx_resolve_config_dir(config_dir, sizeof(config_dir));
    if (rc < 0)
        return rc;

    /* Build assignments.yaml path. */
    char path[PATH_MAX + 32];
    rc = snprintf(path, sizeof(path), "%s/%s", config_dir,
                  ASSIGNMENTS_FILENAME);
    if (rc < 0 || (size_t)rc >= sizeof(path))
        return -ENAMETOOLONG;

    /* If file doesn't exist, return empty. */
    struct stat st;
    if (stat(path, &st) != 0) {
        if (errno == ENOENT)
            return 0;
        return -errno;
    }

    /* Check file size: must be < 1 MB. */
    if (st.st_size > MAX_DOC_SIZE)
        return -EFBIG;

    FILE *f = open_read_nofollow(path);
    if (!f)
        return -errno;

    rc = parse_assignments_yaml(a, f);
    fclose(f);

    return rc;
}

/* --- Save ---------------------------------------------------------------- */

/*
 * Build a YAML document from assignments and emit it to file.
 * Returns 0 on success, negative errno on error.
 */
static int emit_assignments_yaml(const cbx_assignments *a, FILE *f)
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

    /* Root mapping. */
    int root = yaml_document_add_mapping(&doc, NULL,
                                          YAML_BLOCK_MAPPING_STYLE);
    if (root == 0) { rc = -ENOMEM; goto out; }

    /* assignments (sequence of mappings) */
    {
        int k = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)"assignments", -1, YAML_PLAIN_SCALAR_STYLE);
        int seq = yaml_document_add_sequence(&doc, NULL,
            YAML_BLOCK_SEQUENCE_STYLE);
        if (!k || !seq) { rc = -ENOMEM; goto out; }
        yaml_document_append_mapping_pair(&doc, root, k, seq);

        for (int i = 0; i < a->assignment_count; i++) {
            int item_map = yaml_document_add_mapping(&doc, NULL,
                YAML_BLOCK_MAPPING_STYLE);
            if (!item_map) { rc = -ENOMEM; goto out; }
            yaml_document_append_sequence_item(&doc, seq, item_map);

            const cbx_assignment *asgn = &a->assignments[i];

            /* id */
            {
                int ik = yaml_document_add_scalar(&doc, NULL,
                    (yaml_char_t *)"id", -1, YAML_PLAIN_SCALAR_STYLE);
                int iv = yaml_document_add_scalar(&doc, NULL,
                    (yaml_char_t *)asgn->id, -1, YAML_PLAIN_SCALAR_STYLE);
                if (!ik || !iv) { rc = -ENOMEM; goto out; }
                yaml_document_append_mapping_pair(&doc, item_map, ik, iv);
            }

            /* slot */
            {
                char buf[16];
                snprintf(buf, sizeof(buf), "%d", asgn->slot);
                int sk = yaml_document_add_scalar(&doc, NULL,
                    (yaml_char_t *)"slot", -1, YAML_PLAIN_SCALAR_STYLE);
                int sv = yaml_document_add_scalar(&doc, NULL,
                    (yaml_char_t *)buf, -1, YAML_PLAIN_SCALAR_STYLE);
                if (!sk || !sv) { rc = -ENOMEM; goto out; }
                yaml_document_append_mapping_pair(&doc, item_map, sk, sv);
            }

            /* profile */
            {
                int pk = yaml_document_add_scalar(&doc, NULL,
                    (yaml_char_t *)"profile", -1, YAML_PLAIN_SCALAR_STYLE);
                int pv = yaml_document_add_scalar(&doc, NULL,
                    (yaml_char_t *)asgn->profile, -1,
                    asgn->profile[0] == '\0' ? YAML_PLAIN_SCALAR_STYLE
                                              : YAML_PLAIN_SCALAR_STYLE);
                if (!pk || !pv) { rc = -ENOMEM; goto out; }
                yaml_document_append_mapping_pair(&doc, item_map, pk, pv);
            }
        }
    }

    /* gamepad_order (sequence of id strings) */
    {
        int k = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)"gamepad_order", -1, YAML_PLAIN_SCALAR_STYLE);
        int seq = yaml_document_add_sequence(&doc, NULL,
            YAML_BLOCK_SEQUENCE_STYLE);
        if (!k || !seq) { rc = -ENOMEM; goto out; }
        yaml_document_append_mapping_pair(&doc, root, k, seq);

        for (int i = 0; i < a->gamepad_order_count; i++) {
            int item = yaml_document_add_scalar(&doc, NULL,
                (yaml_char_t *)a->gamepad_order[i], -1,
                YAML_PLAIN_SCALAR_STYLE);
            if (!item) { rc = -ENOMEM; goto out; }
            yaml_document_append_sequence_item(&doc, seq, item);
        }
    }

    if (!yaml_emitter_dump(&emitter, &doc))
        rc = -EIO;

out:
    yaml_document_delete(&doc);
    yaml_emitter_delete(&emitter);
    return rc;
}

int cbx_assignments_save(const cbx_assignments *a)
{
    if (!a)
        return -EINVAL;

    /* Validate before writing. */
    int rc = cbx_assignments_validate(a);
    if (rc < 0)
        return rc;

    /* Resolve and create config directory (mode 0700). */
    char config_dir[PATH_MAX];
    rc = cbx_config_dir(config_dir, sizeof(config_dir));
    if (rc < 0)
        return rc;

    /* Build the target path. */
    char path[PATH_MAX + 32];
    rc = snprintf(path, sizeof(path), "%s/%s", config_dir,
                  ASSIGNMENTS_FILENAME);
    if (rc < 0 || (size_t)rc >= sizeof(path))
        return -ENAMETOOLONG;

    /* Create temp file in the same directory (for atomic rename). */
    char tmpl[PATH_MAX + 48];
    rc = snprintf(tmpl, sizeof(tmpl), "%s/.assignments.yaml.XXXXXX",
                  config_dir);
    if (rc < 0 || (size_t)rc >= sizeof(tmpl))
        return -ENAMETOOLONG;

    int fd = mkstemp(tmpl);
    if (fd < 0)
        return -errno;

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

    rc = emit_assignments_yaml(a, f);
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