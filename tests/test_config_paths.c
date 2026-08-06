/*
 * test_config_paths.c — cmocka tests for XDG config directory resolution.
 *
 * Tests cover:
 *   - Path resolution with XDG_CONFIG_HOME / XDG_DATA_HOME set (absolute)
 *   - Path resolution with XDG vars unset, HOME set (fallback paths)
 *   - Path resolution with both unset (error: -ENOENT)
 *   - Relative and empty XDG values treated as unset
 *   - Buffer-too-small error
 *   - Directory creation (mode 0700) via temp dirs
 *   - Recursive parent creation
 *   - System path accessors
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "config/config_paths.h"

#include <errno.h>
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

/* --- Test helpers -------------------------------------------------------- */

/* Create a unique temp directory; returns NULL on failure. */
static char *make_temp_dir(char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "/tmp/cbx-test-XXXXXX");
    return mkdtemp(buf);
}

/* Recursively delete a directory tree (test-only, no user input in path). */
static void rmrf(const char *path)
{
    char cmd[PATH_MAX * 2];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    /* Safe: path comes from mkdtemp, not user input. */
    int rc = system(cmd);
    (void)rc;
}

/* Read the current process umask without permanently changing it. */
static mode_t get_umask(void)
{
    mode_t mask = umask(0);
    umask(mask);
    return mask;
}

/* --- Config dir resolution tests ---------------------------------------- */

static void test_config_dir_xdg_absolute(void **state)
{
    (void)state;
    char buf[PATH_MAX];

    setenv("XDG_CONFIG_HOME", "/tmp/cbx-xdg-abs-test", 1);
    unsetenv("HOME");

    assert_int_equal(cbx_resolve_config_dir(buf, sizeof(buf)), 0);
    assert_string_equal(buf, "/tmp/cbx-xdg-abs-test/controller-box");

    unsetenv("XDG_CONFIG_HOME");
}

static void test_config_dir_xdg_unset_home_set(void **state)
{
    (void)state;
    char buf[PATH_MAX];

    unsetenv("XDG_CONFIG_HOME");
    setenv("HOME", "/tmp/cbx-home-fb", 1);

    assert_int_equal(cbx_resolve_config_dir(buf, sizeof(buf)), 0);
    assert_string_equal(buf, "/tmp/cbx-home-fb/.config/controller-box");

    unsetenv("HOME");
}

static void test_config_dir_both_unset(void **state)
{
    (void)state;
    char buf[PATH_MAX];

    unsetenv("XDG_CONFIG_HOME");
    unsetenv("HOME");

    assert_int_equal(cbx_resolve_config_dir(buf, sizeof(buf)), -ENOENT);
}

static void test_config_dir_xdg_relative_ignored(void **state)
{
    (void)state;
    char buf[PATH_MAX];

    /* Per XDG spec, relative values are ignored → fall back to HOME. */
    setenv("XDG_CONFIG_HOME", "relative/path", 1);
    setenv("HOME", "/tmp/cbx-rel-test", 1);

    assert_int_equal(cbx_resolve_config_dir(buf, sizeof(buf)), 0);
    assert_string_equal(buf, "/tmp/cbx-rel-test/.config/controller-box");

    unsetenv("XDG_CONFIG_HOME");
    unsetenv("HOME");
}

static void test_config_dir_xdg_empty_ignored(void **state)
{
    (void)state;
    char buf[PATH_MAX];

    setenv("XDG_CONFIG_HOME", "", 1);
    setenv("HOME", "/tmp/cbx-empty-test", 1);

    assert_int_equal(cbx_resolve_config_dir(buf, sizeof(buf)), 0);
    assert_string_equal(buf, "/tmp/cbx-empty-test/.config/controller-box");

    unsetenv("XDG_CONFIG_HOME");
    unsetenv("HOME");
}

static void test_config_dir_buf_too_small(void **state)
{
    (void)state;
    char buf[4];

    setenv("HOME", "/tmp/cbx-tiny", 1);
    unsetenv("XDG_CONFIG_HOME");

    assert_int_equal(cbx_resolve_config_dir(buf, sizeof(buf)), -ENAMETOOLONG);

    unsetenv("HOME");
}

/* --- User profiles dir resolution tests ---------------------------------- */

static void test_profiles_dir_xdg_absolute(void **state)
{
    (void)state;
    char buf[PATH_MAX];

    setenv("XDG_DATA_HOME", "/tmp/cbx-xdg-data-abs", 1);
    unsetenv("HOME");

    assert_int_equal(cbx_resolve_user_profiles_dir(buf, sizeof(buf)), 0);
    assert_string_equal(buf, "/tmp/cbx-xdg-data-abs/inputplumber/profiles");

    unsetenv("XDG_DATA_HOME");
}

static void test_profiles_dir_xdg_unset_home_set(void **state)
{
    (void)state;
    char buf[PATH_MAX];

    unsetenv("XDG_DATA_HOME");
    setenv("HOME", "/tmp/cbx-prof-home", 1);

    assert_int_equal(cbx_resolve_user_profiles_dir(buf, sizeof(buf)), 0);
    assert_string_equal(buf,
        "/tmp/cbx-prof-home/.local/share/inputplumber/profiles");

    unsetenv("HOME");
}

static void test_profiles_dir_both_unset(void **state)
{
    (void)state;
    char buf[PATH_MAX];

    unsetenv("XDG_DATA_HOME");
    unsetenv("HOME");

    assert_int_equal(cbx_resolve_user_profiles_dir(buf, sizeof(buf)), -ENOENT);
}

static void test_profiles_dir_xdg_relative_ignored(void **state)
{
    (void)state;
    char buf[PATH_MAX];

    setenv("XDG_DATA_HOME", "rel/data", 1);
    setenv("HOME", "/tmp/cbx-prof-rel", 1);

    assert_int_equal(cbx_resolve_user_profiles_dir(buf, sizeof(buf)), 0);
    assert_string_equal(buf,
        "/tmp/cbx-prof-rel/.local/share/inputplumber/profiles");

    unsetenv("XDG_DATA_HOME");
    unsetenv("HOME");
}

/* --- Directory creation tests -------------------------------------------- */

static void test_config_dir_creates(void **state)
{
    (void)state;
    char tmpdir[PATH_MAX];
    char buf[PATH_MAX];

    assert_non_null(make_temp_dir(tmpdir, sizeof(tmpdir)));

    setenv("HOME", tmpdir, 1);
    unsetenv("XDG_CONFIG_HOME");

    assert_int_equal(cbx_config_dir(buf, sizeof(buf)), 0);

    struct stat st;
    assert_int_equal(stat(buf, &st), 0);
    assert_true(S_ISDIR(st.st_mode));

    /* Mode should be 0700 adjusted by umask. */
    mode_t mask = get_umask();
    assert_int_equal(st.st_mode & 0777, 0700 & ~mask);

    rmrf(tmpdir);
    unsetenv("HOME");
}

static void test_profiles_dir_creates_recursive(void **state)
{
    (void)state;
    char tmpdir[PATH_MAX];
    char buf[PATH_MAX];

    assert_non_null(make_temp_dir(tmpdir, sizeof(tmpdir)));

    setenv("HOME", tmpdir, 1);
    unsetenv("XDG_DATA_HOME");

    /* This creates ~/.local/share/inputplumber/profiles/ recursively. */
    assert_int_equal(cbx_user_profiles_dir(buf, sizeof(buf)), 0);

    struct stat st;
    assert_int_equal(stat(buf, &st), 0);
    assert_true(S_ISDIR(st.st_mode));

    mode_t mask = get_umask();
    assert_int_equal(st.st_mode & 0777, 0700 & ~mask);

    /* Verify intermediate dir also created. */
    char intermediate[PATH_MAX + 32];
    snprintf(intermediate, sizeof(intermediate), "%s/.local/share/inputplumber",
             tmpdir);
    assert_int_equal(stat(intermediate, &st), 0);
    assert_true(S_ISDIR(st.st_mode));

    rmrf(tmpdir);
    unsetenv("HOME");
}

static void test_ensure_dir_already_exists(void **state)
{
    (void)state;
    char tmpdir[PATH_MAX];

    assert_non_null(make_temp_dir(tmpdir, sizeof(tmpdir)));

    /* Calling ensure_dir on an existing directory should succeed. */
    assert_int_equal(cbx_ensure_dir(tmpdir, 0700), 0);

    rmrf(tmpdir);
}

static void test_ensure_dir_deep_recursive(void **state)
{
    (void)state;
    char tmpdir[PATH_MAX];
    char deep[PATH_MAX * 2];

    assert_non_null(make_temp_dir(tmpdir, sizeof(tmpdir)));

    snprintf(deep, sizeof(deep), "%s/a/b/c/d", tmpdir);
    assert_int_equal(cbx_ensure_dir(deep, 0700), 0);

    struct stat st;
    assert_int_equal(stat(deep, &st), 0);
    assert_true(S_ISDIR(st.st_mode));

    rmrf(tmpdir);
}

static void test_config_dir_idempotent(void **state)
{
    (void)state;
    char tmpdir[PATH_MAX];
    char buf1[PATH_MAX];
    char buf2[PATH_MAX];

    assert_non_null(make_temp_dir(tmpdir, sizeof(tmpdir)));

    setenv("HOME", tmpdir, 1);
    unsetenv("XDG_CONFIG_HOME");

    /* First call creates the directory. */
    assert_int_equal(cbx_config_dir(buf1, sizeof(buf1)), 0);

    /* Second call should succeed (already exists). */
    assert_int_equal(cbx_config_dir(buf2, sizeof(buf2)), 0);

    /* Both calls should return the same path. */
    assert_string_equal(buf1, buf2);

    rmrf(tmpdir);
    unsetenv("HOME");
}

/* --- System path tests ---------------------------------------------------- */

static void test_system_paths(void **state)
{
    (void)state;

    assert_string_equal(cbx_system_inputplumber_dir(),
        "/usr/share/inputplumber");
    assert_string_equal(cbx_system_profiles_dir(),
        "/usr/share/inputplumber/profiles");
    assert_string_equal(cbx_system_devices_dir(),
        "/usr/share/inputplumber/devices");
    assert_string_equal(cbx_system_capability_maps_dir(),
        "/usr/share/inputplumber/capability_maps");

    /* DATA_DIR / ICON_DIR are compile-time; verify non-NULL and non-empty. */
    const char *dd = cbx_data_dir();
    assert_non_null(dd);
    assert_true(dd[0] != '\0');

    const char *id = cbx_icon_dir();
    assert_non_null(id);
    assert_true(id[0] != '\0');
}

/* --- Test runner ---------------------------------------------------------- */

int main(void)
{
    const struct CMUnitTest tests[] = {
        /* Config dir resolution. */
        cmocka_unit_test(test_config_dir_xdg_absolute),
        cmocka_unit_test(test_config_dir_xdg_unset_home_set),
        cmocka_unit_test(test_config_dir_both_unset),
        cmocka_unit_test(test_config_dir_xdg_relative_ignored),
        cmocka_unit_test(test_config_dir_xdg_empty_ignored),
        cmocka_unit_test(test_config_dir_buf_too_small),

        /* Profiles dir resolution. */
        cmocka_unit_test(test_profiles_dir_xdg_absolute),
        cmocka_unit_test(test_profiles_dir_xdg_unset_home_set),
        cmocka_unit_test(test_profiles_dir_both_unset),
        cmocka_unit_test(test_profiles_dir_xdg_relative_ignored),

        /* Directory creation. */
        cmocka_unit_test(test_config_dir_creates),
        cmocka_unit_test(test_profiles_dir_creates_recursive),
        cmocka_unit_test(test_ensure_dir_already_exists),
        cmocka_unit_test(test_ensure_dir_deep_recursive),
        cmocka_unit_test(test_config_dir_idempotent),

        /* System paths. */
        cmocka_unit_test(test_system_paths),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}