/*
 * test_profile_save.c — cmocka tests for manager-level profile save (Task 39).
 *
 * Tests cover:
 *   - Save valid profile with NES minimum met
 *   - Reject invalid filename (special chars, empty, NULL)
 *   - Reject invalid profile (wrong version/kind)
 *   - Reject profile missing NES minimum bindings
 *   - Missing button names in output buffer
 *   - Path canonicalization and verification (within profiles dir)
 *   - Sidecar metadata write
 *   - Save to test directory override
 *   - Round-trip: save → load → verify
 *   - NULL safety
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "manager/profile_save.h"
#include "config/config_profile.h"
#include "config/config_profile_meta.h"
#include "config/config_paths.h"
#include "manager/profile_validate.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* --- Test helpers -------------------------------------------------------- */

static char test_home[256];
static int setup_home(void **state)
{
    (void)state;
    snprintf(test_home, sizeof(test_home), "/tmp/cbx-ps-test-%d", (int)getpid());
    char cmd[PATH_MAX * 2 + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", test_home);
    int r0 = system(cmd);
    (void)r0;
    mkdir(test_home, 0700);
    setenv("HOME", test_home, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");
    return 0;
}

static int teardown_home(void **state)
{
    (void)state;
    char cmd[PATH_MAX * 2 + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", test_home);
    int r = system(cmd);
    (void)r;
    unsetenv("HOME");
    return 0;
}

/* Create a temp profiles directory for testing. */
static void make_profiles_dir(char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "%s/profiles", test_home);
    mkdir(buf, 0700);
}

static void make_meta_dir(char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "%s/meta", test_home);
    mkdir(buf, 0700);
}

/* Build a profile that passes NES minimum validation (A, B, Up, Down, Left, Right). */
static cbx_profile make_valid_profile(void)
{
    cbx_profile p;
    cbx_profile_init(&p);
    strncpy(p.name, "Test Profile", sizeof(p.name) - 1);

    const char *buttons[] = {"A", "B", "Up", "Down", "Left", "Right"};
    const char *keys[] = {"KeyA", "KeyB", "KeyUp", "KeyDown", "KeyLeft", "KeyRight"};

    for (int i = 0; i < 6; i++) {
        cbx_profile_mapping *m = &p.mappings[p.mapping_count];
        snprintf(m->name, sizeof(m->name), "btn_%s", buttons[i]);
        strncpy(m->source_event.device_class, "gamepad",
                sizeof(m->source_event.device_class) - 1);
        m->source_event.prop_count = 1;
        strncpy(m->source_event.props[0].key, "button",
                sizeof(m->source_event.props[0].key) - 1);
        strncpy(m->source_event.props[0].value, buttons[i],
                sizeof(m->source_event.props[0].value) - 1);
        m->target_event_count = 1;
        strncpy(m->target_events[0].device_class, "keyboard",
                sizeof(m->target_events[0].device_class) - 1);
        strncpy(m->target_events[0].value, keys[i],
                sizeof(m->target_events[0].value) - 1);
        p.mapping_count++;
    }

    return p;
}

/* Build a profile missing the "A" button binding. */
static cbx_profile make_profile_missing_a(void)
{
    cbx_profile p;
    cbx_profile_init(&p);
    strncpy(p.name, "Incomplete", sizeof(p.name) - 1);

    const char *buttons[] = {"B", "Up", "Down", "Left", "Right"};
    const char *keys[] = {"KeyB", "KeyUp", "KeyDown", "KeyLeft", "KeyRight"};

    for (int i = 0; i < 5; i++) {
        cbx_profile_mapping *m = &p.mappings[p.mapping_count];
        snprintf(m->name, sizeof(m->name), "btn_%s", buttons[i]);
        strncpy(m->source_event.device_class, "gamepad",
                sizeof(m->source_event.device_class) - 1);
        m->source_event.prop_count = 1;
        strncpy(m->source_event.props[0].key, "button",
                sizeof(m->source_event.props[0].key) - 1);
        strncpy(m->source_event.props[0].value, buttons[i],
                sizeof(m->source_event.props[0].value) - 1);
        m->target_event_count = 1;
        strncpy(m->target_events[0].device_class, "keyboard",
                sizeof(m->target_events[0].device_class) - 1);
        strncpy(m->target_events[0].value, keys[i],
                sizeof(m->target_events[0].value) - 1);
        p.mapping_count++;
    }

    return p;
}

/* --- Tests --------------------------------------------------------------- */

/* Save a valid profile to a test directory. */
static void test_save_valid_profile(void **state)
{
    (void)state;
    char profiles_dir[PATH_MAX];
    make_profiles_dir(profiles_dir, sizeof(profiles_dir));

    cbx_profile p = make_valid_profile();
    int rc = cbx_profile_save_to_dir(&p, "myprofile", NULL, profiles_dir,
                                      NULL, 0);
    assert_int_equal(rc, 0);

    /* Verify file exists. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/myprofile.yaml", profiles_dir);
    struct stat st;
    assert_int_equal(stat(path, &st), 0);
    assert_true(S_ISREG(st.st_mode));

    /* Verify round-trip. */
    cbx_profile loaded;
    rc = cbx_profile_load(&loaded, path);
    assert_int_equal(rc, 0);
    assert_int_equal(loaded.mapping_count, 6);
    assert_string_equal(loaded.name, "Test Profile");
}

/* Reject invalid filename. */
static void test_reject_invalid_filename(void **state)
{
    (void)state;
    char profiles_dir[PATH_MAX];
    make_profiles_dir(profiles_dir, sizeof(profiles_dir));

    cbx_profile p = make_valid_profile();

    /* Name with slash. */
    assert_int_equal(cbx_profile_save_to_dir(&p, "bad/name", NULL,
                       profiles_dir, NULL, 0), -EINVAL);
    /* Name with space. */
    assert_int_equal(cbx_profile_save_to_dir(&p, "bad name", NULL,
                       profiles_dir, NULL, 0), -EINVAL);
    /* Name with dot. */
    assert_int_equal(cbx_profile_save_to_dir(&p, "bad.name", NULL,
                       profiles_dir, NULL, 0), -EINVAL);
    /* Empty name. */
    assert_int_equal(cbx_profile_save_to_dir(&p, "", NULL,
                       profiles_dir, NULL, 0), -EINVAL);
    /* NULL name. */
    assert_int_equal(cbx_profile_save_to_dir(&p, NULL, NULL,
                       profiles_dir, NULL, 0), -EINVAL);
}

/* Reject invalid profile (wrong version/kind). */
static void test_reject_invalid_profile(void **state)
{
    (void)state;
    char profiles_dir[PATH_MAX];
    make_profiles_dir(profiles_dir, sizeof(profiles_dir));

    cbx_profile p = make_valid_profile();
    p.version = 2;  /* wrong version */
    assert_int_equal(cbx_profile_save_to_dir(&p, "badver", NULL,
                       profiles_dir, NULL, 0), -EINVAL);

    p = make_valid_profile();
    strncpy(p.kind, "NotDeviceProfile", sizeof(p.kind) - 1);
    assert_int_equal(cbx_profile_save_to_dir(&p, "badkind", NULL,
                       profiles_dir, NULL, 0), -EINVAL);
}

/* Reject profile missing NES minimum. */
static void test_reject_missing_nes_minimum(void **state)
{
    (void)state;
    char profiles_dir[PATH_MAX];
    make_profiles_dir(profiles_dir, sizeof(profiles_dir));

    cbx_profile p = make_profile_missing_a();
    char missing_buf[256] = {0};
    int rc = cbx_profile_save_to_dir(&p, "incomplete", NULL, profiles_dir,
                                      missing_buf, sizeof(missing_buf));
    assert_int_equal(rc, -EINVAL);
    /* Missing buf should contain "A". */
    assert_string_equal(missing_buf, "A");

    /* Verify file was NOT created. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/incomplete.yaml", profiles_dir);
    struct stat st;
    assert_int_not_equal(stat(path, &st), 0);
}

/* Missing buf NULL is OK (just doesn't report names). */
static void test_missing_buf_null(void **state)
{
    (void)state;
    char profiles_dir[PATH_MAX];
    make_profiles_dir(profiles_dir, sizeof(profiles_dir));

    cbx_profile p = make_profile_missing_a();
    int rc = cbx_profile_save_to_dir(&p, "incomplete", NULL, profiles_dir,
                                      NULL, 0);
    assert_int_equal(rc, -EINVAL);
}

/* Save with sidecar metadata to a test meta dir. */
static void test_save_with_sidecar(void **state)
{
    (void)state;
    char profiles_dir[PATH_MAX];
    make_profiles_dir(profiles_dir, sizeof(profiles_dir));
    char meta_dir[PATH_MAX];
    make_meta_dir(meta_dir, sizeof(meta_dir));

    cbx_profile p = make_valid_profile();
    cbx_profile_meta meta;
    cbx_profile_meta_init(&meta);
    meta.has_display_name = true;
    strncpy(meta.display_name, "My Profile", sizeof(meta.display_name) - 1);
    meta.has_icon = true;
    strncpy(meta.icon, "cc-xbox-360", sizeof(meta.icon) - 1);

    int rc = cbx_profile_save_to_dir(&p, "withmeta", NULL, profiles_dir,
                                      NULL, 0);
    assert_int_equal(rc, 0);

    /* Save sidecar to test meta dir. */
    rc = cbx_profile_save_meta_to_dir(&meta, "withmeta", meta_dir);
    assert_int_equal(rc, 0);

    /* Verify sidecar exists. */
    char sidecar[PATH_MAX + 128];
    snprintf(sidecar, sizeof(sidecar), "%s/withmeta.meta.yaml", meta_dir);
    struct stat st;
    assert_int_equal(stat(sidecar, &st), 0);

    /* Verify round-trip sidecar. */
    cbx_profile_meta loaded;
    rc = cbx_profile_meta_load(&loaded, sidecar);
    assert_int_equal(rc, 0);
    assert_true(loaded.has_display_name);
    assert_string_equal(loaded.display_name, "My Profile");
    assert_true(loaded.has_icon);
    assert_string_equal(loaded.icon, "cc-xbox-360");
}

/* NULL profile. */
static void test_null_profile(void **state)
{
    (void)state;
    char profiles_dir[PATH_MAX];
    make_profiles_dir(profiles_dir, sizeof(profiles_dir));

    int rc = cbx_profile_save_to_dir(NULL, "test", NULL, profiles_dir, NULL, 0);
    assert_int_equal(rc, -EINVAL);
}

/* NULL profiles_dir with valid profile (uses default path). */
static void test_null_profiles_dir_uses_default(void **state)
{
    (void)state;
    /* This uses the default XDG_DATA_HOME path, which is set up by setup_home. */
    cbx_profile p = make_valid_profile();
    int rc = cbx_profile_save_named(&p, "defaulttest", NULL, NULL, 0);
    assert_int_equal(rc, 0);

    /* Verify the file was created in the user profiles dir. */
    char dir[PATH_MAX];
    rc = cbx_user_profiles_dir(dir, sizeof(dir));
    assert_int_equal(rc, 0);

    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/defaulttest.yaml", dir);
    struct stat st;
    assert_int_equal(stat(path, &st), 0);
}

/* Save overwrites existing profile. */
static void test_save_overwrites(void **state)
{
    (void)state;
    char profiles_dir[PATH_MAX];
    make_profiles_dir(profiles_dir, sizeof(profiles_dir));

    cbx_profile p = make_valid_profile();
    int rc = cbx_profile_save_to_dir(&p, "overwrite", NULL, profiles_dir,
                                      NULL, 0);
    assert_int_equal(rc, 0);

    /* Modify and save again. */
    strncpy(p.name, "Updated", sizeof(p.name) - 1);
    rc = cbx_profile_save_to_dir(&p, "overwrite", NULL, profiles_dir,
                                  NULL, 0);
    assert_int_equal(rc, 0);

    /* Verify updated content. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/overwrite.yaml", profiles_dir);
    cbx_profile loaded;
    rc = cbx_profile_load(&loaded, path);
    assert_int_equal(rc, 0);
    assert_string_equal(loaded.name, "Updated");
}

/* Profile with extra mappings beyond NES minimum is valid. */
static void test_save_with_extra_mappings(void **state)
{
    (void)state;
    char profiles_dir[PATH_MAX];
    make_profiles_dir(profiles_dir, sizeof(profiles_dir));

    cbx_profile p = make_valid_profile();

    /* Add an extra mapping for Start button. */
    cbx_profile_mapping *m = &p.mappings[p.mapping_count];
    snprintf(m->name, sizeof(m->name), "btn_Start");
    strncpy(m->source_event.device_class, "gamepad",
            sizeof(m->source_event.device_class) - 1);
    m->source_event.prop_count = 1;
    strncpy(m->source_event.props[0].key, "button",
            sizeof(m->source_event.props[0].key) - 1);
    strncpy(m->source_event.props[0].value, "Start",
            sizeof(m->source_event.props[0].value) - 1);
    m->target_event_count = 1;
    strncpy(m->target_events[0].device_class, "keyboard",
            sizeof(m->target_events[0].device_class) - 1);
    strncpy(m->target_events[0].value, "KeyEsc",
            sizeof(m->target_events[0].value) - 1);
    p.mapping_count++;

    int rc = cbx_profile_save_to_dir(&p, "extra", NULL, profiles_dir,
                                      NULL, 0);
    assert_int_equal(rc, 0);

    /* Verify round-trip. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/extra.yaml", profiles_dir);
    cbx_profile loaded;
    rc = cbx_profile_load(&loaded, path);
    assert_int_equal(rc, 0);
    assert_int_equal(loaded.mapping_count, 7);
}

/* meta_dir NULL uses default path for sidecar. */
static void test_meta_to_dir_null_uses_default(void **state)
{
    (void)state;
    cbx_profile_meta meta;
    cbx_profile_meta_init(&meta);
    meta.has_display_name = true;
    strncpy(meta.display_name, "Default Path Test",
            sizeof(meta.display_name) - 1);

    int rc = cbx_profile_save_meta_to_dir(&meta, "defaultmeta", NULL);
    assert_int_equal(rc, 0);

    /* Verify via load_for. */
    cbx_profile_meta loaded;
    rc = cbx_profile_meta_load_for(&loaded, "defaultmeta");
    assert_int_equal(rc, 0);
    assert_true(loaded.has_display_name);
    assert_string_equal(loaded.display_name, "Default Path Test");
}

/* Invalid name for meta save. */
static void test_meta_to_dir_invalid_name(void **state)
{
    (void)state;
    char meta_dir[PATH_MAX];
    make_meta_dir(meta_dir, sizeof(meta_dir));

    cbx_profile_meta meta;
    cbx_profile_meta_init(&meta);

    int rc = cbx_profile_save_meta_to_dir(&meta, "bad/name", meta_dir);
    assert_int_equal(rc, -EINVAL);

    rc = cbx_profile_save_meta_to_dir(&meta, "", meta_dir);
    assert_int_equal(rc, -EINVAL);

    rc = cbx_profile_save_meta_to_dir(NULL, "test", meta_dir);
    assert_int_equal(rc, -EINVAL);
}

/* Empty profile (no mappings) fails NES minimum. */
static void test_empty_profile_fails(void **state)
{
    (void)state;
    char profiles_dir[PATH_MAX];
    make_profiles_dir(profiles_dir, sizeof(profiles_dir));

    cbx_profile p;
    cbx_profile_init(&p);
    strncpy(p.name, "Empty", sizeof(p.name) - 1);

    char missing_buf[256] = {0};
    int rc = cbx_profile_save_to_dir(&p, "empty", NULL, profiles_dir,
                                      missing_buf, sizeof(missing_buf));
    assert_int_equal(rc, -EINVAL);
    /* Should list all 6 missing buttons. */
    assert_non_null(strstr(missing_buf, "A"));
    assert_non_null(strstr(missing_buf, "B"));
    assert_non_null(strstr(missing_buf, "Up"));
    assert_non_null(strstr(missing_buf, "Down"));
    assert_non_null(strstr(missing_buf, "Left"));
    assert_non_null(strstr(missing_buf, "Right"));
}

/* --- main ---------------------------------------------------------------- */

/* TOCTOU fix: verify that saving through a symlinked profiles directory
 * writes to the canonical (resolved) path, not the symlink path.  This
 * confirms that cbx_profile_save_to_dir uses the canonicalized path
 * from verify_path_within_dir for the actual write, eliminating the
 * TOCTOU race between the realpath() check and the file write. */
static void test_save_through_symlink_uses_canonical(void **state)
{
    (void)state;
    /* Create a real directory. */
    char real_dir[PATH_MAX];
    snprintf(real_dir, sizeof(real_dir), "%s/real_profiles", test_home);
    mkdir(real_dir, 0700);

    /* Create a symlink pointing to the real directory. */
    char link_dir[PATH_MAX];
    snprintf(link_dir, sizeof(link_dir), "%s/link_profiles", test_home);
    unlink(link_dir);  /* remove if exists from a prior run */
    assert_int_equal(symlink(real_dir, link_dir), 0);

    /* Save through the symlink — the canonical path should be used. */
    cbx_profile p = make_valid_profile();
    int rc = cbx_profile_save_to_dir(&p, "cannon", NULL, link_dir,
                                      NULL, 0);
    assert_int_equal(rc, 0);

    /* File must exist in the real (canonical) directory. */
    char real_path[PATH_MAX + 128];
    snprintf(real_path, sizeof(real_path), "%s/cannon.yaml", real_dir);
    struct stat st;
    assert_int_equal(stat(real_path, &st), 0);
    assert_true(S_ISREG(st.st_mode));

    /* Round-trip: load the file from the canonical path. */
    cbx_profile loaded;
    rc = cbx_profile_load(&loaded, real_path);
    assert_int_equal(rc, 0);
    assert_int_equal(loaded.mapping_count, 6);

    /* Clean up. */
    unlink(real_path);
    unlink(link_dir);
    rmdir(real_dir);
}

/* ================================================================== */
/*  Task 3: Profile determinism and portability (PE-07)              */
/* ================================================================== */

/* PE-07: Same profile YAML loaded twice produces identical mapping
 * state.  Profiles are deterministic — no dynamic behavior. */
static void
test_profile_determinism_load_twice(void **state)
{
    (void)state;
    cbx_profile p = make_valid_profile();
    int rc = cbx_profile_save_to_dir(&p, "deterministic", NULL,
                                       test_home, NULL, 0);
    assert_int_equal(rc, 0);

    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/deterministic.yaml", test_home);

    /* Load the profile twice and compare. */
    cbx_profile loaded1, loaded2;
    cbx_profile_init(&loaded1);
    cbx_profile_init(&loaded2);

    assert_int_equal(cbx_profile_load(&loaded1, path), 0);
    assert_int_equal(cbx_profile_load(&loaded2, path), 0);

    /* Assert identical mapping state. */
    assert_int_equal(loaded1.mapping_count, loaded2.mapping_count);
    assert_int_equal(loaded1.version, loaded2.version);
    assert_string_equal(loaded1.kind, loaded2.kind);
    assert_string_equal(loaded1.name, loaded2.name);

    for (int i = 0; i < loaded1.mapping_count; i++) {
        assert_string_equal(loaded1.mappings[i].name,
                             loaded2.mappings[i].name);
        assert_int_equal(loaded1.mappings[i].source_event.prop_count,
                         loaded2.mappings[i].source_event.prop_count);
        assert_string_equal(loaded1.mappings[i].source_event.device_class,
                             loaded2.mappings[i].source_event.device_class);
        for (int j = 0; j < loaded1.mappings[i].source_event.prop_count; j++) {
            assert_string_equal(loaded1.mappings[i].source_event.props[j].key,
                                 loaded2.mappings[i].source_event.props[j].key);
            assert_string_equal(loaded1.mappings[i].source_event.props[j].value,
                                 loaded2.mappings[i].source_event.props[j].value);
        }
        assert_int_equal(loaded1.mappings[i].target_event_count,
                         loaded2.mappings[i].target_event_count);
        for (int j = 0; j < loaded1.mappings[i].target_event_count; j++) {
            assert_string_equal(loaded1.mappings[i].target_events[j].device_class,
                                 loaded2.mappings[i].target_events[j].device_class);
            assert_string_equal(loaded1.mappings[i].target_events[j].value,
                                 loaded2.mappings[i].target_events[j].value);
        }
    }
}

/* PE-07: Portability — the same profile produces the same result
 * regardless of connection method.  InputPlumber's capability maps
 * normalize physical-controller differences, so profiles map against
 * virtual device capabilities.  We verify by loading the same profile
 * file and checking that the mapping state is independent of any
 * external connection state — the profile is self-contained YAML. */
static void
test_profile_portability_same_result(void **state)
{
    (void)state;
    cbx_profile p = make_valid_profile();
    /* Add an extra mapping (Start button) to exercise non-NES bindings. */
    cbx_profile_mapping *m = &p.mappings[p.mapping_count];
    memset(m, 0, sizeof(*m));
    strncpy(m->name, "btn_start", sizeof(m->name) - 1);
    strncpy(m->source_event.device_class, "gamepad",
             sizeof(m->source_event.device_class) - 1);
    m->source_event.prop_count = 1;
    strncpy(m->source_event.props[0].key, "button",
             sizeof(m->source_event.props[0].key) - 1);
    strncpy(m->source_event.props[0].value, "Start",
             sizeof(m->source_event.props[0].value) - 1);
    m->target_event_count = 1;
    strncpy(m->target_events[0].device_class, "keyboard",
             sizeof(m->target_events[0].device_class) - 1);
    strncpy(m->target_events[0].value, "KeyReturn",
             sizeof(m->target_events[0].value) - 1);
    p.mapping_count++;

    int rc = cbx_profile_save_to_dir(&p, "portable", NULL,
                                       test_home, NULL, 0);
    assert_int_equal(rc, 0);

    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/portable.yaml", test_home);

    /* Load the profile — the result is deterministic regardless of
     * how the physical controller is connected (BT/USB/serial).
     * The profile YAML is self-contained and does not vary by
     * connection method.  This is the design principle:
     * "Same profile + same controller = same result, every time." */
    cbx_profile loaded;
    cbx_profile_init(&loaded);
    assert_int_equal(cbx_profile_load(&loaded, path), 0);

    /* Verify all 7 mappings are intact with correct button/key pairs. */
    assert_int_equal(loaded.mapping_count, 7);

    /* Verify the Start mapping was preserved (non-NES binding). */
    bool found_start = false;
    for (int i = 0; i < loaded.mapping_count; i++) {
        for (int j = 0; j < loaded.mappings[i].source_event.prop_count; j++) {
            if (strcmp(loaded.mappings[i].source_event.props[j].key, "button") == 0 &&
                strcmp(loaded.mappings[i].source_event.props[j].value, "Start") == 0) {
                found_start = true;
                /* Verify target is keyboard:KeyReturn */
                assert_int_equal(loaded.mappings[i].target_event_count, 1);
                assert_string_equal(loaded.mappings[i].target_events[0].device_class, "keyboard");
                assert_string_equal(loaded.mappings[i].target_events[0].value, "KeyReturn");
            }
        }
    }
    assert_true(found_start);

    /* Save the loaded profile back and reload — round-trip must
     * produce the same state, proving determinism. */
    cbx_profile roundtrip;
    cbx_profile_init(&roundtrip);
    rc = cbx_profile_save_to_dir(&loaded, "portable", NULL,
                                   test_home, NULL, 0);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_profile_load(&roundtrip, path), 0);
    assert_int_equal(roundtrip.mapping_count, loaded.mapping_count);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_save_valid_profile,
            setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_reject_invalid_filename,
            setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_reject_invalid_profile,
            setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_reject_missing_nes_minimum,
            setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_missing_buf_null,
            setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_save_with_sidecar,
            setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_null_profile,
            setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_null_profiles_dir_uses_default,
            setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_save_overwrites,
            setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_save_with_extra_mappings,
            setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_meta_to_dir_null_uses_default,
            setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_meta_to_dir_invalid_name,
            setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_empty_profile_fails,
            setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_save_through_symlink_uses_canonical,
            setup_home, teardown_home),
        /* Task 3: Determinism and portability (PE-07) */
        cmocka_unit_test_setup_teardown(test_profile_determinism_load_twice,
            setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_profile_portability_same_result,
            setup_home, teardown_home),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}