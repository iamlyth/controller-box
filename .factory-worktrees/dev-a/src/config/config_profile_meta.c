/*
 * config_profile_meta.c — Profile metadata sidecar (§7.5) read/write.
 *
 * Parses and serializes flat YAML sidecar files with four optional fields:
 * display_name, icon, display_order, description.
 *
 * Security:
 *   - O_NOFOLLOW on all file opens
 *   - realpath() canonicalization + base-dir verification on save_for/load_for
 *   - Atomic write: mkstemp + fchmod 0600 + fsync + rename
 *   - libyaml parser: max depth 50, max doc size 1 MB, no custom tags
 */
#include "config_profile_meta.h"
#include "config_paths.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <yaml.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* --- Constants ----------------------------------------------------------- */

#define MAX_DOC_SIZE  (1024 * 1024)   /* 1 MB */
#define MAX_YAML_DEPTH 50

/* --- Init ---------------------------------------------------------------- */

void cbx_profile_meta_init(cbx_profile_meta *m)
{
    memset(m, 0, sizeof(*m));
}

/* --- Filename validation ------------------------------------------------- */

bool cbx_validate_filename(const char *name)
{
    if (!name || !*name)
        return false;
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            return false;
    }
    return true;
}

/* --- Helpers: O_NOFOLLOW file open --------------------------------------- */

/*
 * Open a file for reading with O_NOFOLLOW.
 * Returns a FILE* on success, NULL on error (errno set).
 */
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

/* --- YAML parsing (event-based, flat mapping) ---------------------------- */

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

static void safe_copy(char *dst, size_t dst_size, const char *src)
{
    if (!src || dst_size == 0) {
        if (dst && dst_size > 0)
            dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

/* Parse flat YAML mapping: key → scalar value.
 * Handles display_name, icon, display_order, description. */
static int parse_meta_events(cbx_profile_meta *m, yaml_parser_t *parser)
{
    yaml_event_t ev;
    int rc = 0;
    int depth = 0;
    bool have_key = false;
    char current_key[64] = {0};
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
            break;

        case YAML_SCALAR_EVENT: {
            const char *val = (const char *)ev.data.scalar.value;
            if (!val)
                val = "";
            if (!have_key) {
                safe_copy(current_key, sizeof(current_key), val);
                have_key = true;
            } else {
                if (strcmp(current_key, "display_name") == 0) {
                    safe_copy(m->display_name,
                              sizeof(m->display_name), val);
                    m->has_display_name = true;
                } else if (strcmp(current_key, "icon") == 0) {
                    safe_copy(m->icon, sizeof(m->icon), val);
                    m->has_icon = true;
                } else if (strcmp(current_key, "display_order") == 0) {
                    m->display_order = atoi(val);
                    m->has_display_order = true;
                } else if (strcmp(current_key, "description") == 0) {
                    safe_copy(m->description,
                              sizeof(m->description), val);
                    m->has_description = true;
                }
                /* Unknown keys: silently ignore (forward-compatible) */
                have_key = false;
            }
            break;
        }

        case YAML_SEQUENCE_START_EVENT:
            depth++;
            if (depth > MAX_YAML_DEPTH) {
                rc = -EFBIG;
                yaml_event_delete(&ev);
                goto done;
            }
            break;

        case YAML_MAPPING_END_EVENT:
            depth--;
            break;

        case YAML_SEQUENCE_END_EVENT:
            depth--;
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

static int parse_meta_from_string(cbx_profile_meta *m, const char *yaml,
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

    rc = parse_meta_events(m, &parser);

    yaml_parser_delete(&parser);
    return rc;
}

/* --- Load ---------------------------------------------------------------- */

int cbx_profile_meta_load(cbx_profile_meta *m, const char *path)
{
    if (!m || !path)
        return -EINVAL;

    cbx_profile_meta_init(m);

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

    rc = parse_meta_events(m, &parser);

    yaml_parser_delete(&parser);
    fclose(f);
    return rc;
}

int cbx_profile_meta_parse(cbx_profile_meta *m, const char *yaml, size_t len)
{
    if (!m || !yaml)
        return -EINVAL;

    cbx_profile_meta_init(m);
    return parse_meta_from_string(m, yaml, len);
}

/* --- Serialization (document-based emitter) ------------------------------- */

static int emit_meta_yaml(const cbx_profile_meta *m, FILE *f)
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

    int root = yaml_document_add_mapping(&doc, NULL,
                                          YAML_BLOCK_MAPPING_STYLE);
    if (root == 0) { rc = -ENOMEM; goto out; }

    if (m->has_display_name) {
        int k = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)"display_name", -1, YAML_PLAIN_SCALAR_STYLE);
        int v = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)m->display_name, -1,
            YAML_DOUBLE_QUOTED_SCALAR_STYLE);
        if (!k || !v) { rc = -ENOMEM; goto out; }
        yaml_document_append_mapping_pair(&doc, root, k, v);
    }

    if (m->has_icon) {
        int k = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)"icon", -1, YAML_PLAIN_SCALAR_STYLE);
        int v = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)m->icon, -1, YAML_PLAIN_SCALAR_STYLE);
        if (!k || !v) { rc = -ENOMEM; goto out; }
        yaml_document_append_mapping_pair(&doc, root, k, v);
    }

    if (m->has_display_order) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", m->display_order);
        int k = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)"display_order", -1, YAML_PLAIN_SCALAR_STYLE);
        int v = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)buf, -1, YAML_PLAIN_SCALAR_STYLE);
        if (!k || !v) { rc = -ENOMEM; goto out; }
        yaml_document_append_mapping_pair(&doc, root, k, v);
    }

    if (m->has_description) {
        int k = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)"description", -1, YAML_PLAIN_SCALAR_STYLE);
        int v = yaml_document_add_scalar(&doc, NULL,
            (yaml_char_t *)m->description, -1,
            YAML_DOUBLE_QUOTED_SCALAR_STYLE);
        if (!k || !v) { rc = -ENOMEM; goto out; }
        yaml_document_append_mapping_pair(&doc, root, k, v);
    }

    if (!yaml_emitter_dump(&emitter, &doc))
        rc = -EIO;

out:
    yaml_document_delete(&doc);
    yaml_emitter_delete(&emitter);
    return rc;
}

int cbx_profile_meta_serialize(const cbx_profile_meta *m,
                                char **buf, size_t *len)
{
    if (!m || !buf)
        return -EINVAL;

    FILE *f = open_memstream(buf, len);
    if (!f)
        return -errno;

    int rc = emit_meta_yaml(m, f);
    if (rc < 0) {
        fclose(f);
        if (*buf) { free(*buf); *buf = NULL; }
        if (len) *len = 0;
        return rc;
    }

    if (fflush(f) != 0) {
        rc = -errno;
        fclose(f);
        if (*buf) { free(*buf); *buf = NULL; }
        if (len) *len = 0;
        return rc;
    }

    if (fclose(f) != 0) {
        rc = -errno;
        if (*buf) { free(*buf); *buf = NULL; }
        if (len) *len = 0;
        return rc;
    }

    return 0;
}

/* --- Save ---------------------------------------------------------------- */

static int save_atomic(const cbx_profile_meta *m, const char *path)
{
    /* Extract directory for temp file creation. */
    char dir[PATH_MAX];
    const char *slash = strrchr(path, '/');
    if (slash) {
        size_t dir_len = (size_t)(slash - path);
        if (dir_len >= sizeof(dir))
            return -ENAMETOOLONG;
        memcpy(dir, path, dir_len);
        dir[dir_len] = '\0';
    } else {
        snprintf(dir, sizeof(dir), ".");
    }

    const char *base = slash ? slash + 1 : path;

    /* Build temp template: <dir>/.<base>.XXXXXX */
    char tmpl[PATH_MAX + 64];
    int rc = snprintf(tmpl, sizeof(tmpl), "%s/.%s.XXXXXX", dir, base);
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

    rc = emit_meta_yaml(m, f);
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

int cbx_profile_meta_save(const cbx_profile_meta *m, const char *path)
{
    if (!m || !path)
        return -EINVAL;
    return save_atomic(m, path);
}

/* --- Path construction + realpath verification ---------------------------- */

/*
 * Construct the sidecar path for a named profile:
 *   <config_dir>/profile-metadata/<name>.meta.yaml
 * Also ensures the profile-metadata directory exists (mode 0700).
 *
 * @param profile_name Validated profile base name.
 * @param path_out     Output buffer (PATH_MAX).
 * @param path_size    Size of path_out.
 * @return 0 on success; negative errno on error.
 */
static int build_sidecar_path(const char *profile_name,
                               char *path_out, size_t path_size)
{
    if (!cbx_validate_filename(profile_name))
        return -EINVAL;

    char config_dir[PATH_MAX];
    int rc = cbx_resolve_config_dir(config_dir, sizeof(config_dir));
    if (rc < 0)
        return rc;

    /* Ensure config dir + profile-metadata sub-dir exist. */
    char meta_dir[PATH_MAX + 32];
    rc = snprintf(meta_dir, sizeof(meta_dir), "%s/profile-metadata",
                   config_dir);
    if (rc < 0 || (size_t)rc >= sizeof(meta_dir))
        return -ENAMETOOLONG;

    rc = cbx_ensure_dir(meta_dir, 0700);
    if (rc < 0)
        return rc;

    /* Verify meta_dir is within config_dir via realpath. */
    char real_meta[PATH_MAX];
    char real_config[PATH_MAX];
    if (!realpath(meta_dir, real_meta))
        return -errno;
    if (!realpath(config_dir, real_config)) {
        /* config_dir might not exist yet — resolve parent. */
        /* If config_dir was created by cbx_ensure_dir above (via
         * cbx_resolve_config_dir won't create it, but cbx_ensure_dir for
         * meta_dir will create the parent). */
        return -errno;
    }

    size_t config_len = strlen(real_config);
    if (strncmp(real_meta, real_config, config_len) != 0 ||
        (real_meta[config_len] != '\0' && real_meta[config_len] != '/')) {
        return -EACCES;  /* meta_dir escaped config_dir */
    }

    /* Build full sidecar path. */
    rc = snprintf(path_out, path_size, "%s/%s.meta.yaml",
                   real_meta, profile_name);
    if (rc < 0 || (size_t)rc >= path_size)
        return -ENAMETOOLONG;

    return 0;
}

int cbx_profile_meta_load_for(cbx_profile_meta *m, const char *profile_name)
{
    if (!m || !profile_name)
        return -EINVAL;

    char path[PATH_MAX];
    int rc = build_sidecar_path(profile_name, path, sizeof(path));
    if (rc < 0)
        return rc;

    return cbx_profile_meta_load(m, path);
}

int cbx_profile_meta_save_for(const cbx_profile_meta *m,
                               const char *profile_name)
{
    if (!m || !profile_name)
        return -EINVAL;

    char path[PATH_MAX];
    int rc = build_sidecar_path(profile_name, path, sizeof(path));
    if (rc < 0)
        return rc;

    return save_atomic(m, path);
}