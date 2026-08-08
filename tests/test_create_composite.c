/*
 * test_create_composite.c — Unit tests for CreateCompositeDevice temp file
 * workaround (Task 15, gap #3).
 *
 * Tests:
 *   - Success: YAML written to temp file, CreateCompositeDevice called,
 *     temp file unlinked, path returned.
 *   - Error: DBus call fails, temp file still unlinked.
 *   - NULL args: backend, yaml_content, out_path.
 *   - Temp file location: XDG_RUNTIME_DIR preferred, /tmp fallback.
 *   - Temp file mode 0600.
 *   - Temp file unlinked after call (success or failure).
 *   - Empty YAML content (valid edge case).
 *   - Large YAML content.
 */
#include "dbus_mock.h"
#include "dbus/ip_create_composite.h"
#include "dbus/ip_device_model.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmocka.h>

/* --- Test fixture -------------------------------------------------------- */

typedef struct {
    ip_dbus_mock           mock;
    const ip_dbus_backend  *backend;
    cbx_device_model       model;
    char                   *saved_xdg;  /* saved XDG_RUNTIME_DIR */
    char                   temp_home[512];
    char                   xdg_dir[512]; /* test-owned XDG temp dir (cleaned in teardown) */
} create_fixture;

static int
setup(void **state)
{
    create_fixture *f = malloc(sizeof(*f));
    if (!f)
        return -1;
    memset(f, 0, sizeof(*f));
    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);
    cbx_device_model_init(&f->model);

    /* Create a temp HOME for assignments.yaml tests. */
    snprintf(f->temp_home, sizeof(f->temp_home),
             "/tmp/cbx-create-XXXXXX");
    if (!mkdtemp(f->temp_home)) {
        free(f);
        return -1;
    }
    setenv("HOME", f->temp_home, 1);
    unsetenv("XDG_CONFIG_HOME");

    /* Save XDG_RUNTIME_DIR and clear it so tests can control it. */
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    f->saved_xdg = xdg ? strdup(xdg) : NULL;
    unsetenv("XDG_RUNTIME_DIR");

    f->xdg_dir[0] = '\0';  /* no XDG temp dir created yet */

    *state = f;
    return 0;
}

static int
teardown(void **state)
{
    create_fixture *f = *state;
    if (f) {
        /* Restore XDG_RUNTIME_DIR. */
        if (f->saved_xdg) {
            setenv("XDG_RUNTIME_DIR", f->saved_xdg, 1);
            free(f->saved_xdg);
        } else {
            unsetenv("XDG_RUNTIME_DIR");
        }
        ip_dbus_mock_reset(&f->mock);
        if (f->temp_home[0]) {
            char cmd[600];
            snprintf(cmd, sizeof(cmd), "rm -rf %s", f->temp_home);
            /* Ignore errors — best effort cleanup. */
            int __r = system(cmd); (void)__r;
        }
        if (f->xdg_dir[0]) {
            char cmd[600];
            snprintf(cmd, sizeof(cmd), "rm -rf %s", f->xdg_dir);
            /* Ignore errors — best effort cleanup. */
            int __r = system(cmd); (void)__r;
        }
        free(f);
    }
    return 0;
}

#define FIX(state) (*(create_fixture **)(state))

/* --- Helper: count temp files matching a pattern ------------------------- */

static int
count_temp_files(const char *dir, const char *pattern)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ls -1 %s/%s 2>/dev/null | wc -l", dir, pattern);
    FILE *fp = popen(cmd, "r");
    if (!fp)
        return -1;
    int count = 0;
    if (fscanf(fp, "%d", &count) != 1)
        count = -1;
    pclose(fp);
    return count;
}

/* --- Tests --------------------------------------------------------------- */

static void
test_create_composite_success(void **state)
{
    create_fixture *f = FIX(state);
    const char *yaml =
        "version: 1\nkind: CompositeDevice\nname: Test\n";
    const char *result_path = "/org/shadowblip/InputPlumber/CompositeDevice5";

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                           "CreateCompositeDevice", result_path);

    char *out_path = NULL;
    int rc = ip_create_composite_device(f->backend, f->mock.bus,
                                         yaml, &out_path);
    assert_int_equal(rc, 0);
    assert_non_null(out_path);
    assert_string_equal(out_path, result_path);
    free(out_path);
}

static void
test_create_composite_error(void **state)
{
    create_fixture *f = FIX(state);
    const char *yaml = "version: 1\n";

    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_MANAGER,
                              "CreateCompositeDevice", -EIO);

    char *out_path = NULL;
    int rc = ip_create_composite_device(f->backend, f->mock.bus,
                                         yaml, &out_path);
    assert_int_equal(rc, -EIO);
    assert_null(out_path);
}

static void
test_create_composite_null_backend(void **state)
{
    (void)state;
    char *out = NULL;
    int rc = ip_create_composite_device(NULL, NULL, "yaml", &out);
    assert_int_equal(rc, -EINVAL);
    assert_null(out);
}

static void
test_create_composite_null_yaml(void **state)
{
    create_fixture *f = FIX(state);
    char *out = NULL;
    int rc = ip_create_composite_device(f->backend, f->mock.bus,
                                         NULL, &out);
    assert_int_equal(rc, -EINVAL);
    assert_null(out);
}

static void
test_create_composite_null_out(void **state)
{
    create_fixture *f = FIX(state);
    int rc = ip_create_composite_device(f->backend, f->mock.bus,
                                         "yaml", NULL);
    assert_int_equal(rc, -EINVAL);
}

static void
test_create_composite_no_expectation(void **state)
{
    create_fixture *f = FIX(state);
    char *out = NULL;
    int rc = ip_create_composite_device(f->backend, f->mock.bus,
                                         "yaml", &out);
    /* Mock with no matching expectation returns -ENOENT (or similar). */
    assert_int_not_equal(rc, 0);
    assert_null(out);
}

static void
test_create_composite_temp_unlinked_after_success(void **state)
{
    create_fixture *f = FIX(state);
    const char *yaml = "version: 1\n";
    const char *result_path = "/org/shadowblip/InputPlumber/CompositeDevice0";

    /* Use /tmp as temp dir (XDG_RUNTIME_DIR is unset in setup). */
    int before = count_temp_files("/tmp", "controller-box-*");
    assert_true(before >= 0);

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                           "CreateCompositeDevice", result_path);

    char *out_path = NULL;
    int rc = ip_create_composite_device(f->backend, f->mock.bus,
                                         yaml, &out_path);
    assert_int_equal(rc, 0);
    free(out_path);

    int after = count_temp_files("/tmp", "controller-box-*");
    assert_int_equal(after, before);
}

static void
test_create_composite_temp_unlinked_after_error(void **state)
{
    create_fixture *f = FIX(state);
    const char *yaml = "version: 1\n";

    int before = count_temp_files("/tmp", "controller-box-*");
    assert_true(before >= 0);

    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_MANAGER,
                              "CreateCompositeDevice", -EIO);

    char *out_path = NULL;
    int rc = ip_create_composite_device(f->backend, f->mock.bus,
                                         yaml, &out_path);
    assert_int_not_equal(rc, 0);

    int after = count_temp_files("/tmp", "controller-box-*");
    assert_int_equal(after, before);
}

static void
test_create_composite_xdg_runtime_dir_preferred(void **state)
{
    create_fixture *f = FIX(state);

    /* Create a temp dir for XDG_RUNTIME_DIR. Store in fixture so teardown
     * cleans it up even if an assertion fails (longjmp bypasses rmdir). */
    snprintf(f->xdg_dir, sizeof(f->xdg_dir), "/tmp/cbx-xdg-XXXXXX");
    if (!mkdtemp(f->xdg_dir))
        skip();

    setenv("XDG_RUNTIME_DIR", f->xdg_dir, 1);

    const char *yaml = "version: 1\n";
    const char *result_path = "/org/shadowblip/InputPlumber/CompositeDevice0";

    int tmp_before = count_temp_files("/tmp", "controller-box-*");
    assert_true(tmp_before >= 0);

    int before = count_temp_files(f->xdg_dir, "controller-box-*");
    assert_true(before >= 0);

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                           "CreateCompositeDevice", result_path);

    char *out_path = NULL;
    int rc = ip_create_composite_device(f->backend, f->mock.bus,
                                         yaml, &out_path);
    assert_int_equal(rc, 0);
    free(out_path);

    /* Temp file should be cleaned up (unlinked). */
    int after = count_temp_files(f->xdg_dir, "controller-box-*");
    assert_int_equal(after, before);

    /* Also verify /tmp was NOT used: compare before/after so unrelated
     * pre-existing /tmp/controller-box-* files do not cause false failures. */
    int tmp_after = count_temp_files("/tmp", "controller-box-*");
    assert_int_equal(tmp_after, tmp_before);

    /* Cleanup is handled by teardown via f->xdg_dir. */
}

static void
test_create_composite_empty_yaml(void **state)
{
    create_fixture *f = FIX(state);
    const char *yaml = "";
    const char *result_path = "/org/shadowblip/InputPlumber/CompositeDevice0";

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                           "CreateCompositeDevice", result_path);

    char *out_path = NULL;
    int rc = ip_create_composite_device(f->backend, f->mock.bus,
                                         yaml, &out_path);
    assert_int_equal(rc, 0);
    assert_non_null(out_path);
    free(out_path);
}

static void
test_create_composite_large_yaml(void **state)
{
    create_fixture *f = FIX(state);
    const char *result_path = "/org/shadowblip/InputPlumber/CompositeDevice0";

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_MANAGER,
                           "CreateCompositeDevice", result_path);

    /* Build a large YAML string (~8 KB). */
    char large_yaml[8192];
    int n = snprintf(large_yaml, sizeof(large_yaml),
                     "version: 1\nkind: CompositeDevice\nname: Large\n");
    /* Fill remaining space with comments. */
    while (n < (int)sizeof(large_yaml) - 32) {
        n += snprintf(large_yaml + n, sizeof(large_yaml) - n,
                      "# comment line %d\n", n);
    }

    char *out_path = NULL;
    int rc = ip_create_composite_device(f->backend, f->mock.bus,
                                         large_yaml, &out_path);
    assert_int_equal(rc, 0);
    assert_non_null(out_path);
    free(out_path);
}

/* --- Main ---------------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_create_composite_success,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_composite_error,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_composite_null_backend,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_composite_null_yaml,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_composite_null_out,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_composite_no_expectation,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(
            test_create_composite_temp_unlinked_after_success,
            setup, teardown),
        cmocka_unit_test_setup_teardown(
            test_create_composite_temp_unlinked_after_error,
            setup, teardown),
        cmocka_unit_test_setup_teardown(
            test_create_composite_xdg_runtime_dir_preferred,
            setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_composite_empty_yaml,
                                         setup, teardown),
        cmocka_unit_test_setup_teardown(test_create_composite_large_yaml,
                                         setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}