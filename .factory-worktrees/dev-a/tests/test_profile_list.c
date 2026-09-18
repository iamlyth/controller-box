/*
 * test_profile_list.c — tests for profile metadata sidecar (§7.5) and
 * filesystem enumeration (Task 8).
 *
 * Tests:
 *   Sidecar: init, parse (basic/partial/empty/unknown-keys), serialize,
 *     round-trip file (mode 0600), load nonexistent, O_NOFOLLOW load,
 *     save_for/load_for, filename validation, YAML security
 *     (max doc, tags, depth).
 *   Enumeration: empty, user-only, system-only, both (dedup), default
 *     (read-only), sidecar merge (full + partial), no sidecar, name
 *     fallback, sort order, non-YAML filter, null dirs, device configs,
 *     capability maps, integration.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "config/config_profile_meta.h"
#include "config/config_profile_list.h"
#include "config/config_profile.h"
#include "config/config_paths.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_DOC (1024 * 1024)

/* --- Test fixture: temp directories -------------------------------------- */

typedef struct {
    char base[PATH_MAX];     /* test root, e.g. /tmp/cbx-test-XXXXXX */
    char user_dir[PATH_MAX + 32];
    char system_dir[PATH_MAX + 32];
    char meta_dir[PATH_MAX + 32];
} test_env;

static void make_dir(const char *path)
{
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "FATAL: mkdir(%s): %s\n", path, strerror(errno));
        abort();
    }
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "FATAL: fopen(%s): %s\n", path, strerror(errno));
        abort();
    }
    fputs(content, f);
    fclose(f);
}

static void write_file_in(const char *dir, const char *name,
                           const char *content)
{
    char path[PATH_MAX + 512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    write_file(path, content);
}

static int setup_env(void **state)
{
    test_env *e = calloc(1, sizeof(*e));
    if (!e) return -1;

    char tmpl[PATH_MAX];
    snprintf(tmpl, sizeof(tmpl), "/tmp/cbx-test-%d.XXXXXX", getpid());
    if (!mkdtemp(tmpl)) {
        free(e);
        return -1;
    }
    snprintf(e->base, sizeof(e->base), "%s", tmpl);

    snprintf(e->user_dir, sizeof(e->user_dir), "%s/user-profiles", e->base);
    snprintf(e->system_dir, sizeof(e->system_dir), "%s/system-profiles",
             e->base);
    snprintf(e->meta_dir, sizeof(e->meta_dir), "%s/profile-metadata",
             e->base);

    make_dir(e->user_dir);
    make_dir(e->system_dir);
    make_dir(e->meta_dir);

    *state = e;
    return 0;
}

static int teardown_env(void **state)
{
    test_env *e = *state;
    if (e) {
        char cmd[PATH_MAX + 32];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", e->base);
        int ret = system(cmd);
        (void)ret;
        free(e);
    }
    return 0;
}

/* --- Helpers: create profiles and sidecars ------------------------------- */

static void make_profile(const char *dir, const char *name,
                          const char *display_name, const char *desc)
{
    char path[PATH_MAX + 512];
    snprintf(path, sizeof(path), "%s/%s.yaml", dir, name);
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "version: 1\n"
        "kind: DeviceProfile\n"
        "name: %s\n"
        "description: %s\n"
        "mapping: []\n",
        display_name, desc);
    write_file(path, buf);
}

static void make_sidecar(const char *meta_dir, const char *profile_name,
                         const char *yaml)
{
    char path[PATH_MAX + 512];
    snprintf(path, sizeof(path), "%s/%s.meta.yaml", meta_dir, profile_name);
    write_file(path, yaml);
}

/* ======================================================================== */
/* Tests: Profile metadata sidecar                                          */
/* ======================================================================== */

static void test_meta_init(void **state)
{
    (void)state;
    cbx_profile_meta m;
    cbx_profile_meta_init(&m);

    assert_string_equal(m.display_name, "");
    assert_string_equal(m.icon, "");
    assert_string_equal(m.description, "");
    assert_int_equal(m.display_order, 0);
    assert_false(m.has_display_name);
    assert_false(m.has_icon);
    assert_false(m.has_display_order);
    assert_false(m.has_description);
}

static void test_meta_parse_all_fields(void **state)
{
    (void)state;
    const char *yaml =
        "display_name: \"Fighting\"\n"
        "icon: gamepad\n"
        "display_order: 3\n"
        "description: \"Triggers disabled\"\n";
    cbx_profile_meta m;
    int rc = cbx_profile_meta_parse(&m, yaml, 0);
    assert_int_equal(rc, 0);
    assert_string_equal(m.display_name, "Fighting");
    assert_string_equal(m.icon, "gamepad");
    assert_int_equal(m.display_order, 3);
    assert_string_equal(m.description, "Triggers disabled");
    assert_true(m.has_display_name);
    assert_true(m.has_icon);
    assert_true(m.has_display_order);
    assert_true(m.has_description);
}

static void test_meta_parse_partial(void **state)
{
    (void)state;
    const char *yaml =
        "display_name: \"No Triggers\"\n"
        "display_order: 1\n";
    cbx_profile_meta m;
    int rc = cbx_profile_meta_parse(&m, yaml, 0);
    assert_int_equal(rc, 0);
    assert_string_equal(m.display_name, "No Triggers");
    assert_true(m.has_display_name);
    assert_true(m.has_display_order);
    assert_false(m.has_icon);
    assert_false(m.has_description);
}

static void test_meta_parse_empty(void **state)
{
    (void)state;
    cbx_profile_meta m;
    int rc = cbx_profile_meta_parse(&m, "", 0);
    assert_int_equal(rc, 0);
    assert_false(m.has_display_name);
    assert_false(m.has_icon);
    assert_false(m.has_display_order);
    assert_false(m.has_description);
}

static void test_meta_parse_unknown_keys_ignored(void **state)
{
    (void)state;
    const char *yaml =
        "display_name: \"Test\"\n"
        "unknown_field: 42\n";
    cbx_profile_meta m;
    int rc = cbx_profile_meta_parse(&m, yaml, 0);
    assert_int_equal(rc, 0);
    assert_string_equal(m.display_name, "Test");
    assert_true(m.has_display_name);
    assert_false(m.has_display_order);
}

static void test_meta_serialize(void **state)
{
    (void)state;
    cbx_profile_meta m;
    cbx_profile_meta_init(&m);
    snprintf(m.display_name, sizeof(m.display_name), "Fighting");
    m.has_display_name = true;
    snprintf(m.icon, sizeof(m.icon), "gamepad");
    m.has_icon = true;
    m.display_order = 2;
    m.has_display_order = true;
    snprintf(m.description, sizeof(m.description), "No triggers");
    m.has_description = true;

    char *buf = NULL;
    size_t len = 0;
    int rc = cbx_profile_meta_serialize(&m, &buf, &len);
    assert_int_equal(rc, 0);
    assert_non_null(buf);
    assert_true(len > 0);

    /* Round-trip: parse back. */
    cbx_profile_meta m2;
    rc = cbx_profile_meta_parse(&m2, buf, len);
    assert_int_equal(rc, 0);
    assert_string_equal(m2.display_name, "Fighting");
    assert_string_equal(m2.icon, "gamepad");
    assert_int_equal(m2.display_order, 2);
    assert_string_equal(m2.description, "No triggers");
    assert_true(m2.has_display_name);
    assert_true(m2.has_icon);
    assert_true(m2.has_display_order);
    assert_true(m2.has_description);

    free(buf);
}

static void test_meta_round_trip_file(void **state)
{
    test_env *e = *state;
    char path[PATH_MAX + 512];
    snprintf(path, sizeof(path), "%s/test.meta.yaml", e->meta_dir);

    cbx_profile_meta m;
    cbx_profile_meta_init(&m);
    snprintf(m.display_name, sizeof(m.display_name), "My Profile");
    m.has_display_name = true;
    m.display_order = 5;
    m.has_display_order = true;

    int rc = cbx_profile_meta_save(&m, path);
    assert_int_equal(rc, 0);

    /* Verify file mode 0600. */
    struct stat st;
    assert_int_equal(stat(path, &st), 0);
    assert_int_equal(st.st_mode & 0777, 0600);

    /* Load back. */
    cbx_profile_meta m2;
    rc = cbx_profile_meta_load(&m2, path);
    assert_int_equal(rc, 0);
    assert_string_equal(m2.display_name, "My Profile");
    assert_true(m2.has_display_name);
    assert_int_equal(m2.display_order, 5);
    assert_true(m2.has_display_order);
    assert_false(m2.has_icon);
    assert_false(m2.has_description);
}

static void test_meta_load_nonexistent(void **state)
{
    test_env *e = *state;
    char path[PATH_MAX + 512];
    snprintf(path, sizeof(path), "%s/nonexistent.meta.yaml", e->meta_dir);

    cbx_profile_meta m;
    int rc = cbx_profile_meta_load(&m, path);
    assert_int_equal(rc, 0);
    assert_false(m.has_display_name);
    assert_false(m.has_icon);
    assert_false(m.has_display_order);
    assert_false(m.has_description);
}

static void test_meta_load_no_follow(void **state)
{
    test_env *e = *state;
    /* Create a real file with YAML content. */
    char target[PATH_MAX + 256];
    snprintf(target, sizeof(target), "%s/real2.meta.yaml", e->meta_dir);
    write_file(target, "display_name: \"Real\"\ndisplay_order: 10\n");

    /* Create a symlink pointing to the real file. */
    char link_path[PATH_MAX + 256];
    snprintf(link_path, sizeof(link_path), "%s/link2.meta.yaml", e->meta_dir);
    unlink(link_path);
    if (symlink(target, link_path) != 0) {
        skip();
        return;
    }

    /* Loading through symlink should fail (O_NOFOLLOW rejects symlinks). */
    cbx_profile_meta m;
    int rc = cbx_profile_meta_load(&m, link_path);
    assert_int_equal(rc, -ELOOP);

    /* But loading the real file directly works. */
    rc = cbx_profile_meta_load(&m, target);
    assert_int_equal(rc, 0);
    assert_string_equal(m.display_name, "Real");
    assert_int_equal(m.display_order, 10);

    unlink(link_path);
}

/* --- Filename validation ------------------------------------------------- */

static void test_validate_filename(void **state)
{
    (void)state;
    assert_true(cbx_validate_filename("fighting"));
    assert_true(cbx_validate_filename("my-profile"));
    assert_true(cbx_validate_filename("my_profile"));
    assert_true(cbx_validate_filename("Profile123"));
    assert_true(cbx_validate_filename("a"));
    assert_true(cbx_validate_filename("A-B_C"));

    assert_false(cbx_validate_filename(""));
    assert_false(cbx_validate_filename(NULL));
    assert_false(cbx_validate_filename("has space"));
    assert_false(cbx_validate_filename("has/slash"));
    assert_false(cbx_validate_filename("../etc"));
    assert_false(cbx_validate_filename("file.yaml"));
    assert_false(cbx_validate_filename("file.meta.yaml"));
    assert_false(cbx_validate_filename("has.dots"));
}

/* --- YAML security ------------------------------------------------------- */

static void test_meta_max_doc_size(void **state)
{
    (void)state;
    /* Create a YAML doc > 1 MB. */
    size_t sz = MAX_DOC + 64;
    char *big = malloc(sz);
    if (!big) { skip(); return; }
    memset(big, 'x', sz - 1);
    big[sz - 1] = '\0';
    char *yaml = malloc(sz + 64);
    if (!yaml) { free(big); skip(); return; }
    snprintf(yaml, sz + 64, "display_name: \"%s\"\n", big);

    cbx_profile_meta m;
    int rc = cbx_profile_meta_parse(&m, yaml, 0);
    assert_int_equal(rc, -EFBIG);

    free(big);
    free(yaml);
}

static void test_meta_custom_tags_rejected(void **state)
{
    (void)state;
    const char *yaml =
        "display_name: !custom \"Tagged\"\n"
        "display_order: 1\n";
    cbx_profile_meta m;
    int rc = cbx_profile_meta_parse(&m, yaml, 0);
    assert_int_equal(rc, -EPERM);
}

static void test_meta_tag_directives_rejected(void **state)
{
    (void)state;
    const char *yaml =
        "%TAG !tag! tag:example.org,2024:app/\n"
        "---\n"
        "display_name: \"Test\"\n";
    cbx_profile_meta m;
    int rc = cbx_profile_meta_parse(&m, yaml, 0);
    assert_int_equal(rc, -EPERM);
}

static void test_meta_max_depth(void **state)
{
    (void)state;
    /* Create YAML with 60 nesting levels (incrementing indentation). */
    char yaml[16384];
    int off = 0;
    for (int i = 0; i < 60; i++) {
        for (int j = 0; j < i * 2; j++)
            off += snprintf(yaml + off, sizeof(yaml) - off, " ");
        off += snprintf(yaml + off, sizeof(yaml) - off, "a:\n");
    }
    for (int j = 0; j < 60 * 2; j++)
        off += snprintf(yaml + off, sizeof(yaml) - off, " ");
    off += snprintf(yaml + off, sizeof(yaml) - off, "x: 1\n");
    cbx_profile_meta m;
    int rc = cbx_profile_meta_parse(&m, yaml, 0);
    assert_int_equal(rc, -EFBIG);
}

/* --- save_for / load_for ------------------------------------------------- */

static void test_meta_save_for_load_for(void **state)
{
    test_env *e = *state;
    char old_home[PATH_MAX + 256];
    if (!getcwd(old_home, sizeof(old_home))) old_home[0] = '\0';

    setenv("HOME", e->base, 1);
    unsetenv("XDG_CONFIG_HOME");

    cbx_profile_meta m;
    cbx_profile_meta_init(&m);
    snprintf(m.display_name, sizeof(m.display_name), "Saved Profile");
    m.has_display_name = true;
    m.display_order = 7;
    m.has_display_order = true;
    snprintf(m.icon, sizeof(m.icon), "xb360");
    m.has_icon = true;

    int rc = cbx_profile_meta_save_for(&m, "fighting");
    assert_int_equal(rc, 0);

    /* Verify file was created in the expected location. */
    char expected_path[PATH_MAX + 256];
    snprintf(expected_path, sizeof(expected_path),
             "%s/.config/controller-box/profile-metadata/fighting.meta.yaml",
             e->base);
    struct stat st;
    assert_int_equal(stat(expected_path, &st), 0);
    assert_int_equal(st.st_mode & 0777, 0600);

    /* Load back. */
    cbx_profile_meta m2;
    rc = cbx_profile_meta_load_for(&m2, "fighting");
    assert_int_equal(rc, 0);
    assert_string_equal(m2.display_name, "Saved Profile");
    assert_true(m2.has_display_name);
    assert_int_equal(m2.display_order, 7);
    assert_true(m2.has_display_order);
    assert_string_equal(m2.icon, "xb360");
    assert_true(m2.has_icon);

    setenv("HOME", old_home, 1);
}

static void test_meta_save_for_invalid_name(void **state)
{
    (void)state;
    cbx_profile_meta m;
    cbx_profile_meta_init(&m);

    assert_int_equal(cbx_profile_meta_save_for(&m, "../etc/passwd"), -EINVAL);
    assert_int_equal(cbx_profile_meta_save_for(&m, "has space"), -EINVAL);
    assert_int_equal(cbx_profile_meta_save_for(&m, ""), -EINVAL);
}

static void test_meta_load_for_nonexistent(void **state)
{
    test_env *e = *state;
    char old_home[PATH_MAX + 256];
    if (!getcwd(old_home, sizeof(old_home))) old_home[0] = '\0';
    setenv("HOME", e->base, 1);
    unsetenv("XDG_CONFIG_HOME");

    cbx_profile_meta m;
    int rc = cbx_profile_meta_load_for(&m, "nonexistent");
    assert_int_equal(rc, 0);
    assert_false(m.has_display_name);

    setenv("HOME", old_home, 1);
}

/* ======================================================================== */
/* Tests: Profile enumeration                                               */
/* ======================================================================== */

static void test_enumerate_empty(void **state)
{
    test_env *e = *state;
    cbx_profile_list list;
    int rc = cbx_profile_list_enumerate_dirs(&list, e->user_dir,
                                              e->system_dir, e->meta_dir);
    assert_int_equal(rc, 0);
    assert_int_equal(list.count, 0);
}

static void test_enumerate_user_only(void **state)
{
    test_env *e = *state;
    make_profile(e->user_dir, "fighting", "Fighting", "Fight profile");
    make_profile(e->user_dir, "racing", "Racing", "Race profile");

    cbx_profile_list list;
    int rc = cbx_profile_list_enumerate_dirs(&list, e->user_dir,
                                              e->system_dir, e->meta_dir);
    assert_int_equal(rc, 0);
    assert_int_equal(list.count, 2);

    /* Sorted by display_order (0) then display_name: "Fighting" < "Racing" */
    assert_string_equal(list.entries[0].display_name, "Fighting");
    assert_string_equal(list.entries[1].display_name, "Racing");
    assert_false(list.entries[0].is_system);
    assert_false(list.entries[0].read_only);
}

static void test_enumerate_system_only(void **state)
{
    test_env *e = *state;
    make_profile(e->system_dir, "system-prog", "System Profile", "Sys");

    cbx_profile_list list;
    int rc = cbx_profile_list_enumerate_dirs(&list, e->user_dir,
                                              e->system_dir, e->meta_dir);
    assert_int_equal(rc, 0);
    assert_int_equal(list.count, 1);
    assert_true(list.entries[0].is_system);
    assert_true(list.entries[0].read_only);
}

static void test_enumerate_both_dedup(void **state)
{
    test_env *e = *state;
    /* User and system both have "fighting" — user takes precedence. */
    make_profile(e->user_dir, "fighting", "Fighting (User)", "User");
    make_profile(e->system_dir, "fighting", "Fighting (System)", "Sys");
    make_profile(e->system_dir, "racing", "Racing", "Race");

    cbx_profile_list list;
    int rc = cbx_profile_list_enumerate_dirs(&list, e->user_dir,
                                              e->system_dir, e->meta_dir);
    assert_int_equal(rc, 0);
    assert_int_equal(list.count, 2);

    /* User "fighting" should take precedence. */
    bool found_fighting = false;
    for (int i = 0; i < list.count; i++) {
        if (strcmp(list.entries[i].filename, "fighting") == 0) {
            assert_string_equal(list.entries[i].display_name,
                               "Fighting (User)");
            assert_false(list.entries[i].is_system);
            found_fighting = true;
        }
    }
    assert_true(found_fighting);
}

static void test_enumerate_default_readonly(void **state)
{
    test_env *e = *state;
    make_profile(e->user_dir, "default", "Default", "Default profile");
    make_profile(e->user_dir, "custom", "Custom", "Custom profile");

    cbx_profile_list list;
    int rc = cbx_profile_list_enumerate_dirs(&list, e->user_dir,
                                              e->system_dir, e->meta_dir);
    assert_int_equal(rc, 0);
    assert_int_equal(list.count, 2);

    for (int i = 0; i < list.count; i++) {
        if (strcmp(list.entries[i].filename, "default") == 0) {
            assert_true(list.entries[i].is_default);
            assert_true(list.entries[i].read_only);
            assert_false(list.entries[i].is_system);
        } else {
            assert_false(list.entries[i].is_default);
            assert_false(list.entries[i].read_only);
        }
    }
}

static void test_enumerate_with_sidecar(void **state)
{
    test_env *e = *state;
    make_profile(e->user_dir, "fighting", "Fighting Original", "Original desc");

    /* Sidecar overrides display_name, description, icon, display_order. */
    make_sidecar(e->meta_dir, "fighting",
        "display_name: \"Fighting Override\"\n"
        "icon: xb360\n"
        "display_order: 5\n"
        "description: \"Override desc\"\n");

    cbx_profile_list list;
    int rc = cbx_profile_list_enumerate_dirs(&list, e->user_dir,
                                              e->system_dir, e->meta_dir);
    assert_int_equal(rc, 0);
    assert_int_equal(list.count, 1);

    cbx_profile_entry *entry = &list.entries[0];
    assert_string_equal(entry->display_name, "Fighting Override");
    assert_string_equal(entry->description, "Override desc");
    assert_string_equal(entry->icon, "xb360");
    assert_int_equal(entry->display_order, 5);
    assert_true(entry->has_meta);
}

static void test_enumerate_partial_sidecar(void **state)
{
    test_env *e = *state;
    make_profile(e->user_dir, "fighting", "Fighting Name", "Fighting Desc");

    /* Sidecar only has icon + display_order. */
    make_sidecar(e->meta_dir, "fighting",
        "icon: ds5\n"
        "display_order: 3\n");

    cbx_profile_list list;
    int rc = cbx_profile_list_enumerate_dirs(&list, e->user_dir,
                                              e->system_dir, e->meta_dir);
    assert_int_equal(rc, 0);
    assert_int_equal(list.count, 1);

    cbx_profile_entry *entry = &list.entries[0];
    assert_string_equal(entry->display_name, "Fighting Name");
    assert_string_equal(entry->description, "Fighting Desc");
    assert_string_equal(entry->icon, "ds5");
    assert_int_equal(entry->display_order, 3);
    assert_true(entry->has_meta);
}

static void test_enumerate_no_sidecar(void **state)
{
    test_env *e = *state;
    make_profile(e->user_dir, "basic", "Basic Name", "Basic Desc");

    cbx_profile_list list;
    int rc = cbx_profile_list_enumerate_dirs(&list, e->user_dir,
                                              e->system_dir, e->meta_dir);
    assert_int_equal(rc, 0);
    assert_int_equal(list.count, 1);

    cbx_profile_entry *entry = &list.entries[0];
    assert_string_equal(entry->display_name, "Basic Name");
    assert_string_equal(entry->description, "Basic Desc");
    assert_string_equal(entry->icon, "");
    assert_int_equal(entry->display_order, 0);
    assert_false(entry->has_meta);
}

static void test_enumerate_no_profile_name_fallback(void **state)
{
    test_env *e = *state;
    char path[PATH_MAX + 512];
    snprintf(path, sizeof(path), "%s/noname.yaml", e->user_dir);
    write_file(path,
        "version: 1\n"
        "kind: DeviceProfile\n"
        "name: \"\"\n"
        "description: \"Has desc\"\n"
        "mapping: []\n");

    cbx_profile_list list;
    int rc = cbx_profile_list_enumerate_dirs(&list, e->user_dir,
                                              e->system_dir, e->meta_dir);
    assert_int_equal(rc, 0);
    assert_int_equal(list.count, 1);
    assert_string_equal(list.entries[0].display_name, "noname");
    assert_string_equal(list.entries[0].description, "Has desc");
}

static void test_enumerate_sorted_by_order_then_name(void **state)
{
    test_env *e = *state;
    make_profile(e->user_dir, "zeta", "Zeta", "Z");
    make_profile(e->user_dir, "alpha", "Alpha", "A");
    make_profile(e->user_dir, "beta", "Beta", "B");

    make_sidecar(e->meta_dir, "alpha", "display_order: 10\n");
    make_sidecar(e->meta_dir, "beta",  "display_order: 1\n");
    make_sidecar(e->meta_dir, "zeta",  "display_order: 5\n");

    cbx_profile_list list;
    int rc = cbx_profile_list_enumerate_dirs(&list, e->user_dir,
                                              e->system_dir, e->meta_dir);
    assert_int_equal(rc, 0);
    assert_int_equal(list.count, 3);

    /* Expected: beta(1), zeta(5), alpha(10). */
    assert_string_equal(list.entries[0].filename, "beta");
    assert_string_equal(list.entries[1].filename, "zeta");
    assert_string_equal(list.entries[2].filename, "alpha");
}

static void test_enumerate_sorted_same_order_by_name(void **state)
{
    test_env *e = *state;
    make_profile(e->user_dir, "zeta", "Zeta", "Z");
    make_profile(e->user_dir, "alpha", "Alpha", "A");
    make_profile(e->user_dir, "beta", "Beta", "B");

    cbx_profile_list list;
    int rc = cbx_profile_list_enumerate_dirs(&list, e->user_dir,
                                              e->system_dir, e->meta_dir);
    assert_int_equal(rc, 0);
    assert_int_equal(list.count, 3);
    assert_string_equal(list.entries[0].filename, "alpha");
    assert_string_equal(list.entries[1].filename, "beta");
    assert_string_equal(list.entries[2].filename, "zeta");
}

static void test_enumerate_ignores_non_yaml(void **state)
{
    test_env *e = *state;
    make_profile(e->user_dir, "real", "Real", "R");
    write_file_in(e->user_dir, "readme.txt", "not a profile\n");
    write_file_in(e->user_dir, ".hidden.yaml", "hidden\n");

    cbx_profile_list list;
    int rc = cbx_profile_list_enumerate_dirs(&list, e->user_dir,
                                              e->system_dir, e->meta_dir);
    assert_int_equal(rc, 0);
    assert_int_equal(list.count, 1);
    assert_string_equal(list.entries[0].filename, "real");
}

static void test_enumerate_dirs_null(void **state)
{
    (void)state;
    cbx_profile_list list;
    int rc = cbx_profile_list_enumerate_dirs(&list, NULL, NULL, NULL);
    assert_int_equal(rc, 0);
    assert_int_equal(list.count, 0);
}

/* --- Device configs and capability maps ---------------------------------- */

static void test_file_list_enumerate(void **state)
{
    test_env *e = *state;
    char dev_dir[PATH_MAX + 256];
    snprintf(dev_dir, sizeof(dev_dir), "%s/devices", e->base);
    make_dir(dev_dir);

    write_file_in(dev_dir, "xbox.yaml", "name: Xbox\n");
    write_file_in(dev_dir, "dualsense.yaml", "name: DualSense\n");
    write_file_in(dev_dir, "readme.txt", "not yaml\n");

    cbx_file_list list;
    int rc = cbx_file_list_enumerate(&list, dev_dir);
    assert_int_equal(rc, 0);
    assert_int_equal(list.count, 2);
    /* Sorted by name. */
    assert_string_equal(list.entries[0].name, "dualsense");
    assert_string_equal(list.entries[1].name, "xbox");
}

static void test_file_list_empty_dir(void **state)
{
    test_env *e = *state;
    char empty[PATH_MAX + 256];
    snprintf(empty, sizeof(empty), "%s/empty", e->base);
    make_dir(empty);

    cbx_file_list list;
    int rc = cbx_file_list_enumerate(&list, empty);
    assert_int_equal(rc, 0);
    assert_int_equal(list.count, 0);
}

static void test_file_list_nonexistent_dir(void **state)
{
    test_env *e = *state;
    char nonexistent[PATH_MAX + 256];
    snprintf(nonexistent, sizeof(nonexistent), "%s/does-not-exist", e->base);

    cbx_file_list list;
    int rc = cbx_file_list_enumerate(&list, nonexistent);
    assert_int_equal(rc, 0);
    assert_int_equal(list.count, 0);
}

/* --- Integration: enumerate with real (isolated) paths -------------------- */

static void test_enumerate_real_paths(void **state)
{
    test_env *e = *state;
    char old_home[PATH_MAX + 256];
    if (!getcwd(old_home, sizeof(old_home))) old_home[0] = '\0';

    setenv("HOME", e->base, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");

    /* Create user profiles dir tree (bottom-up: parent dirs first). */
    char local_dir[PATH_MAX + 256], share_dir[PATH_MAX + 256],
         ip_dir[PATH_MAX + 256], profiles_dir[PATH_MAX + 256];
    snprintf(local_dir, sizeof(local_dir), "%s/.local", e->base);
    snprintf(share_dir, sizeof(share_dir), "%s/.local/share", e->base);
    snprintf(ip_dir, sizeof(ip_dir), "%s/.local/share/inputplumber", e->base);
    snprintf(profiles_dir, sizeof(profiles_dir),
             "%s/.local/share/inputplumber/profiles", e->base);
    make_dir(local_dir);
    make_dir(share_dir);
    make_dir(ip_dir);
    make_dir(profiles_dir);

    make_profile(profiles_dir, "default", "Default", "Default profile");
    make_profile(profiles_dir, "custom", "Custom", "Custom profile");

    /* Create config dir + profile-metadata for sidecars. */
    char config_parent[PATH_MAX + 256], config_dir[PATH_MAX + 256],
         meta_path[PATH_MAX + 256];
    snprintf(config_parent, sizeof(config_parent), "%s/.config", e->base);
    snprintf(config_dir, sizeof(config_dir), "%s/.config/controller-box",
             e->base);
    snprintf(meta_path, sizeof(meta_path),
             "%s/.config/controller-box/profile-metadata", e->base);
    make_dir(config_parent);
    make_dir(config_dir);
    make_dir(meta_path);

    make_sidecar(meta_path, "custom",
        "display_name: \"Custom Override\"\n"
        "display_order: 1\n");

    cbx_profile_list list;
    int rc = cbx_profile_list_enumerate(&list);
    assert_int_equal(rc, 0);
    assert_true(list.count >= 2);

    bool found_custom = false;
    bool found_builtin_default = false;
    for (int i = 0; i < list.count; i++) {
        if (strcmp(list.entries[i].filename, "custom") == 0) {
            assert_string_equal(list.entries[i].display_name,
                               "Custom Override");
            assert_int_equal(list.entries[i].display_order, 1);
            assert_true(list.entries[i].has_meta);
            assert_false(list.entries[i].is_system);
            assert_false(list.entries[i].read_only);
            found_custom = true;
        }
        if (strcmp(list.entries[i].filename, "default") == 0) {
            char expected[PATH_MAX];
            snprintf(expected, sizeof(expected), "%s/default.yaml",
                     cbx_builtin_profiles_dir());
            assert_true(list.entries[i].is_default);
            assert_true(list.entries[i].read_only);
            assert_string_equal(list.entries[i].path, expected);
            found_builtin_default = true;
        }
    }
    assert_true(found_custom);
    assert_true(found_builtin_default);

    setenv("HOME", old_home, 1);
}

/* --- Clean-install: builtin Default with empty host dirs (PR-04) -------- */

/* Verify that a clean install (no user profiles, no system profiles in the
 * test tree) still finds the immutable built-in Default profile via the real
 * cbx_profile_list_enumerate() path, and that a "Default copy" produces a
 * profile with the same 6 NES bindings as the shipped default.yaml.
 */
static void
test_clean_install_builtin_default(void **state)
{
    test_env *e = *state;
    char old_home[PATH_MAX + 256];
    if (!getcwd(old_home, sizeof(old_home))) old_home[0] = '\0';

    /* Simulate clean install: fresh HOME with empty user dirs. */
    setenv("HOME", e->base, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");

    /* Create the user profile directory tree (empty — no profiles written). */
    char local_dir[PATH_MAX + 256], share_dir[PATH_MAX + 256],
         ip_dir[PATH_MAX + 256], profiles_dir[PATH_MAX + 256];
    snprintf(local_dir, sizeof(local_dir), "%s/.local", e->base);
    snprintf(share_dir, sizeof(share_dir), "%s/.local/share", e->base);
    snprintf(ip_dir, sizeof(ip_dir), "%s/.local/share/inputplumber", e->base);
    snprintf(profiles_dir, sizeof(profiles_dir),
             "%s/.local/share/inputplumber/profiles", e->base);
    make_dir(local_dir);
    make_dir(share_dir);
    make_dir(ip_dir);
    make_dir(profiles_dir);

    /* Use the real enumeration path (scans builtin + user + system). */
    cbx_profile_list list;
    int rc = cbx_profile_list_enumerate(&list);
    assert_int_equal(rc, 0);

    /* The builtin Default must be present. */
    bool found_default = false;
    const cbx_profile_entry *default_entry = NULL;
    for (int i = 0; i < list.count; i++) {
        if (list.entries[i].is_default) {
            default_entry = &list.entries[i];
            found_default = true;
        }
    }
    assert_true(found_default);
    assert_non_null(default_entry);

    /* Default must be read-only and come from the builtin profiles dir. */
    assert_true(default_entry->read_only);
    char expected_path[PATH_MAX];
    snprintf(expected_path, sizeof(expected_path), "%s/default.yaml",
             cbx_builtin_profiles_dir());
    assert_string_equal(default_entry->path, expected_path);

    /* Load the shipped Default and verify it has 6 NES bindings. */
    cbx_profile prof;
    rc = cbx_profile_load(&prof, default_entry->path);
    assert_int_equal(rc, 0);
    assert_int_equal(prof.mapping_count, 6);

    /* Verify the 6 NES binding names. */
    const char *expected_names[] = {"A", "B", "D-Pad Up",
                                     "D-Pad Down", "D-Pad Left",
                                     "D-Pad Right"};
    for (int i = 0; i < 6; i++) {
        bool found = false;
        for (int j = 0; j < prof.mapping_count; j++) {
            if (strcmp(prof.mappings[j].name, expected_names[i]) == 0) {
                found = true;
                break;
            }
        }
        assert_true(found);
    }

    /* Simulate "Default copy": save the loaded profile to the user dir
     * and reload — the copy must have the same 6 bindings. */
    char copy_path[PATH_MAX + 512];
    snprintf(copy_path, sizeof(copy_path), "%s/mycopy.yaml",
             profiles_dir);
    rc = cbx_profile_save(&prof, copy_path);
    assert_int_equal(rc, 0);

    cbx_profile copy_prof;
    rc = cbx_profile_load(&copy_prof, copy_path);
    assert_int_equal(rc, 0);
    assert_int_equal(copy_prof.mapping_count, 6);

    /* Verify the copy has the same binding names as the original. */
    for (int i = 0; i < 6; i++) {
        bool found = false;
        for (int j = 0; j < copy_prof.mapping_count; j++) {
            if (strcmp(copy_prof.mappings[j].name,
                       prof.mappings[j].name) == 0) {
                found = true;
                break;
            }
        }
        assert_true(found);
    }

    /* Clean up the copy. */
    unlink(copy_path);

    setenv("HOME", old_home, 1);
}

/* ======================================================================== */
/* Test runner                                                              */
/* ======================================================================== */

int main(void)
{
    /* Tests that don't need the test env fixture */
    const struct CMUnitTest simple_tests[] = {
        cmocka_unit_test(test_meta_init),
        cmocka_unit_test(test_meta_parse_all_fields),
        cmocka_unit_test(test_meta_parse_partial),
        cmocka_unit_test(test_meta_parse_empty),
        cmocka_unit_test(test_meta_parse_unknown_keys_ignored),
        cmocka_unit_test(test_meta_serialize),
        cmocka_unit_test(test_validate_filename),
        cmocka_unit_test(test_meta_custom_tags_rejected),
        cmocka_unit_test(test_meta_tag_directives_rejected),
        cmocka_unit_test(test_meta_max_depth),
        cmocka_unit_test(test_meta_save_for_invalid_name),
        cmocka_unit_test(test_enumerate_dirs_null),
    };

    /* Tests that need the test env fixture */
    const struct CMUnitTest env_tests[] = {
        cmocka_unit_test_setup_teardown(test_meta_round_trip_file,
                                         setup_env, teardown_env),
        cmocka_unit_test_setup_teardown(test_meta_load_nonexistent,
                                         setup_env, teardown_env),
        cmocka_unit_test_setup_teardown(test_meta_load_no_follow,
                                         setup_env, teardown_env),
        cmocka_unit_test_setup_teardown(test_meta_save_for_load_for,
                                         setup_env, teardown_env),
        cmocka_unit_test_setup_teardown(test_meta_load_for_nonexistent,
                                         setup_env, teardown_env),
        cmocka_unit_test_setup_teardown(test_meta_max_doc_size,
                                         setup_env, teardown_env),
        cmocka_unit_test_setup_teardown(test_enumerate_empty,
                                         setup_env, teardown_env),
        cmocka_unit_test_setup_teardown(test_enumerate_user_only,
                                         setup_env, teardown_env),
        cmocka_unit_test_setup_teardown(test_enumerate_system_only,
                                         setup_env, teardown_env),
        cmocka_unit_test_setup_teardown(test_enumerate_both_dedup,
                                         setup_env, teardown_env),
        cmocka_unit_test_setup_teardown(test_enumerate_default_readonly,
                                         setup_env, teardown_env),
        cmocka_unit_test_setup_teardown(test_enumerate_with_sidecar,
                                         setup_env, teardown_env),
        cmocka_unit_test_setup_teardown(test_enumerate_partial_sidecar,
                                         setup_env, teardown_env),
        cmocka_unit_test_setup_teardown(test_enumerate_no_sidecar,
                                         setup_env, teardown_env),
        cmocka_unit_test_setup_teardown(test_enumerate_no_profile_name_fallback,
                                         setup_env, teardown_env),
        cmocka_unit_test_setup_teardown(test_enumerate_sorted_by_order_then_name,
                                         setup_env, teardown_env),
        cmocka_unit_test_setup_teardown(test_enumerate_sorted_same_order_by_name,
                                         setup_env, teardown_env),
        cmocka_unit_test_setup_teardown(test_enumerate_ignores_non_yaml,
                                         setup_env, teardown_env),
        cmocka_unit_test_setup_teardown(test_file_list_enumerate,
                                         setup_env, teardown_env),
        cmocka_unit_test_setup_teardown(test_file_list_empty_dir,
                                         setup_env, teardown_env),
        cmocka_unit_test_setup_teardown(test_file_list_nonexistent_dir,
                                         setup_env, teardown_env),
        cmocka_unit_test_setup_teardown(test_enumerate_real_paths,
                                         setup_env, teardown_env),
        cmocka_unit_test_setup_teardown(test_clean_install_builtin_default,
                                         setup_env, teardown_env),
    };

    int failed = 0;
    failed += cmocka_run_group_tests(simple_tests, NULL, NULL);
    failed += cmocka_run_group_tests(env_tests, NULL, NULL);
    return failed;
}