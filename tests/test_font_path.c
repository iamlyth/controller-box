/*
 * test_font_path.c — cmocka tests for runtime font discovery (BUG-0001, Task 1).
 *
 * Tests cover:
 *   - cbx_font_path() returns a readable .ttf path when a font is available
 *   - cbx_font_path() returns NULL when no font is found (no crash)
 *   - cbx_font_dir() returns a non-NULL, non-empty compile-time FONT_DIR
 *   - Custom XDG_DATA_HOME font directory is searched
 *   - Custom HOME font directory is searched
 *
 * Font-dependent assertions use skip() when no system font is available in
 * the test environment, matching the existing test_manager_tabs pattern.
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
    snprintf(buf, buf_size, "/tmp/cbx-font-test-XXXXXX");
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

/* --- Tests ---------------------------------------------------------------- */

/*
 * cbx_font_dir() returns the compile-time FONT_DIR constant.
 * Verify it is non-NULL and non-empty (it is a compile-time string).
 */
static void test_font_dir_constant(void **state)
{
    (void)state;
    const char *fd = cbx_font_dir();
    assert_non_null(fd);
    assert_true(fd[0] != '\0');
}

/*
 * cbx_font_path() returns either NULL (no font found) or a path to a
 * readable .ttf file.  When non-NULL, verify access(R_OK) succeeds and
 * the path ends with "DejaVuSans.ttf".
 */
static void test_font_path_returns_readable_or_null(void **state)
{
    (void)state;
    const char *path = cbx_font_path();
    if (path == NULL) {
        /* No font available in this environment — that's acceptable. */
        skip();
    }

    /* Path must be readable. */
    assert_return_code(access(path, R_OK), 0);

    /* Path should end with DejaVuSans.ttf. */
    const char *suffix = "DejaVuSans.ttf";
    size_t plen = strlen(path);
    size_t slen = strlen(suffix);
    assert_true(plen >= slen);
    assert_string_equal(path + plen - slen, suffix);
}

/*
 * cbx_font_path() with no font available returns NULL without crashing.
 * We set HOME and XDG_DATA_HOME to empty temp dirs so no user font is found.
 * System font dirs may still have a font, so we only assert no crash.
 */
static void test_font_path_no_crash_empty_env(void **state)
{
    (void)state;
    char tmpdir[PATH_MAX];
    if (!make_temp_dir(tmpdir, sizeof(tmpdir))) {
        skip();
    }

    /* Point XDG and HOME to empty temp dirs — no fonts there. */
    setenv("XDG_DATA_HOME", tmpdir, 1);
    setenv("HOME", tmpdir, 1);

    /* Must not crash regardless of result. */
    const char *path = cbx_font_path();
    (void)path;

    unsetenv("XDG_DATA_HOME");
    unsetenv("HOME");
    rmrf(tmpdir);
}

/*
 * cbx_font_path() discovers a font placed in $XDG_DATA_HOME/fonts/.
 * We create a temp dir, place a dummy file named DejaVuSans.ttf, and
 * verify cbx_font_path() finds it.  We skip if the real system already
 * has a font at a higher-priority location (XDG wins).
 */
static void test_font_path_finds_xdg_font(void **state)
{
    (void)state;
    char tmpdir[PATH_MAX];
    char fontsdir[PATH_MAX + 16];
    char fontfile[PATH_MAX + 64];

    if (!make_temp_dir(tmpdir, sizeof(tmpdir))) {
        skip();
    }

    /* Create fonts/ subdirectory under tmpdir. */
    snprintf(fontsdir, sizeof(fontsdir), "%s/fonts", tmpdir);
    if (mkdir(fontsdir, 0700) != 0) {
        rmrf(tmpdir);
        skip();
    }

    /* Create a dummy DejaVuSans.ttf (empty file — access(R_OK) passes). */
    snprintf(fontfile, sizeof(fontfile), "%s/DejaVuSans.ttf", fontsdir);
    FILE *f = fopen(fontfile, "w");
    if (!f) {
        rmrf(tmpdir);
        skip();
    }
    fclose(f);

    setenv("XDG_DATA_HOME", tmpdir, 1);

    const char *path = cbx_font_path();
    /* XDG_DATA_HOME is highest priority, so it should be found. */
    assert_non_null(path);
    assert_string_equal(path, fontfile);

    unsetenv("XDG_DATA_HOME");
    rmrf(tmpdir);
}

/*
 * cbx_font_path() discovers a font placed in $HOME/.local/share/fonts/.
 */
static void test_font_path_finds_home_font(void **state)
{
    (void)state;
    char tmpdir[PATH_MAX];
    char datadir[PATH_MAX + 16];
    char fontsdir[PATH_MAX + 32];
    char share_fonts[PATH_MAX + 64];
    char fontfile[PATH_MAX + 128];

    if (!make_temp_dir(tmpdir, sizeof(tmpdir))) {
        skip();
    }

    /* Create .local/share/fonts under tmpdir (simulating $HOME). */
    snprintf(datadir, sizeof(datadir), "%s/.local", tmpdir);
    if (mkdir(datadir, 0700) != 0) {
        rmrf(tmpdir);
        skip();
    }
    snprintf(fontsdir, sizeof(fontsdir), "%s/.local/share", datadir);
    if (mkdir(fontsdir, 0700) != 0) {
        rmrf(tmpdir);
        skip();
    }
    snprintf(share_fonts, sizeof(share_fonts), "%s/fonts", fontsdir);
    if (mkdir(share_fonts, 0700) != 0) {
        rmrf(tmpdir);
        skip();
    }

    snprintf(fontfile, sizeof(fontfile), "%s/DejaVuSans.ttf", share_fonts);
    FILE *f = fopen(fontfile, "w");
    if (!f) {
        rmrf(tmpdir);
        skip();
    }
    fclose(f);

    /* Unset XDG_DATA_HOME so HOME fallback is used. */
    unsetenv("XDG_DATA_HOME");
    setenv("HOME", tmpdir, 1);

    const char *path = cbx_font_path();
    /* With XDG unset, $HOME/.local/share/fonts is searched. */
    assert_non_null(path);
    assert_string_equal(path, fontfile);

    unsetenv("HOME");
    rmrf(tmpdir);
}

/* --- Test runner ---------------------------------------------------------- */

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_font_dir_constant),
        cmocka_unit_test(test_font_path_returns_readable_or_null),
        cmocka_unit_test(test_font_path_no_crash_empty_env),
        cmocka_unit_test(test_font_path_finds_xdg_font),
        cmocka_unit_test(test_font_path_finds_home_font),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}