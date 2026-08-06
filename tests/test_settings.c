/*
 * test_settings.c — cmocka tests for settings.yaml read/write (Task 5).
 *
 * Tests cover:
 *   - Default generation (no file → all defaults)
 *   - Round-trip (save → load → compare all fields)
 *   - Validation (opacity, count, types)
 *   - Missing fields (partial YAML → defaults filled)
 *   - Out-of-range values clamped on load
 *   - Atomic write (file mode 0600)
 *   - YAML security constraints (max doc size, custom tags)
 *   - Known-good type list check
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "config/config_settings.h"

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

static char *make_temp_dir(char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "/tmp/cbx-test-XXXXXX");
    return mkdtemp(buf);
}

static void rmrf(const char *path)
{
    char cmd[PATH_MAX * 2 + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    int rc = system(cmd);
    (void)rc;
}

/* Fixture: create a temp HOME and unset XDG vars for isolation. */
static char test_home[PATH_MAX];

static int setup_home(void **state)
{
    (void)state;
    if (!make_temp_dir(test_home, sizeof(test_home)))
        return -1;
    setenv("HOME", test_home, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");
    return 0;
}

static int teardown_home(void **state)
{
    (void)state;
    rmrf(test_home);
    return 0;
}

/* Build the settings.yaml path for the current test HOME. */
static void settings_path(char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "%s/.config/controller-box/settings.yaml", test_home);
}

/* Write raw YAML text to the settings file (bypasses the save function). */
static void write_raw_settings(const char *yaml)
{
    /* test_home is PATH_MAX; callers use PATH_MAX+64 buffers to avoid
     * format-truncation warnings (mem-1785994416-3022). */
    char dir[PATH_MAX + 64];
    snprintf(dir, sizeof(dir), "%s/.config/controller-box", test_home);
    /* Create the directory (mimic what config_dir does). */
    char cmd[PATH_MAX * 2 + 32];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
    int r1 = system(cmd);
    (void)r1;

    char path[PATH_MAX + 64];
    settings_path(path, sizeof(path));

    FILE *f = fopen(path, "w");
    assert_non_null(f);
    fputs(yaml, f);
    fclose(f);
}

/* --- Defaults test ------------------------------------------------------- */

static void test_defaults_no_file(void **state)
{
    (void)state;
    cbx_settings s;
    int rc = cbx_settings_load(&s);
    assert_int_equal(rc, 0);
    assert_string_equal(s.overlay_trigger, "Select+A");
    assert_true(s.launch_at_boot);
    assert_string_equal(s.theme, "default");
    assert_float_equal(s.overlay_opacity, 0.85, 0.001);
    assert_int_equal(s.virtual_controllers.count, 4);
    for (int i = 0; i < 4; i++)
        assert_string_equal(s.virtual_controllers.types[i], "xb360");
}

/* --- Defaults function directly ----------------------------------------- */

static void test_defaults_function(void **state)
{
    (void)state;
    cbx_settings s;
    cbx_settings_defaults(&s);
    assert_string_equal(s.overlay_trigger, "Select+A");
    assert_true(s.launch_at_boot);
    assert_string_equal(s.theme, "default");
    assert_float_equal(s.overlay_opacity, 0.85, 0.001);
    assert_int_equal(s.virtual_controllers.count, 4);
    assert_string_equal(s.virtual_controllers.types[0], "xb360");
}

/* --- Round-trip save/load ------------------------------------------------ */

static void test_round_trip(void **state)
{
    (void)state;
    cbx_settings s;
    cbx_settings_defaults(&s);
    /* Modify some values. */
    strncpy(s.overlay_trigger, "Start+B", sizeof(s.overlay_trigger) - 1);
    s.launch_at_boot = false;
    strncpy(s.theme, "dark", sizeof(s.theme) - 1);
    s.overlay_opacity = 0.50;
    s.virtual_controllers.count = 2;
    strncpy(s.virtual_controllers.types[0], "ds5", sizeof(s.virtual_controllers.types[0]) - 1);
    strncpy(s.virtual_controllers.types[1], "deck", sizeof(s.virtual_controllers.types[1]) - 1);

    int rc = cbx_settings_save(&s);
    assert_int_equal(rc, 0);

    cbx_settings loaded;
    rc = cbx_settings_load(&loaded);
    assert_int_equal(rc, 0);

    assert_string_equal(loaded.overlay_trigger, "Start+B");
    assert_false(loaded.launch_at_boot);
    assert_string_equal(loaded.theme, "dark");
    assert_float_equal(loaded.overlay_opacity, 0.50, 0.001);
    assert_int_equal(loaded.virtual_controllers.count, 2);
    assert_string_equal(loaded.virtual_controllers.types[0], "ds5");
    assert_string_equal(loaded.virtual_controllers.types[1], "deck");
}

/* --- Round-trip with default values -------------------------------------- */

static void test_round_trip_defaults(void **state)
{
    (void)state;
    cbx_settings s;
    cbx_settings_defaults(&s);

    int rc = cbx_settings_save(&s);
    assert_int_equal(rc, 0);

    cbx_settings loaded;
    rc = cbx_settings_load(&loaded);
    assert_int_equal(rc, 0);

    assert_string_equal(loaded.overlay_trigger, "Select+A");
    assert_true(loaded.launch_at_boot);
    assert_string_equal(loaded.theme, "default");
    assert_float_equal(loaded.overlay_opacity, 0.85, 0.001);
    assert_int_equal(loaded.virtual_controllers.count, 4);
}

/* --- Validation: opacity out of range ----------------------------------- */

static void test_validate_opacity_out_of_range(void **state)
{
    (void)state;
    cbx_settings s;
    cbx_settings_defaults(&s);

    s.overlay_opacity = -0.1;
    assert_int_equal(cbx_settings_validate(&s), -EINVAL);

    s.overlay_opacity = 1.1;
    assert_int_equal(cbx_settings_validate(&s), -EINVAL);

    s.overlay_opacity = 0.0;
    assert_int_equal(cbx_settings_validate(&s), 0);

    s.overlay_opacity = 1.0;
    assert_int_equal(cbx_settings_validate(&s), 0);
}

/* --- Validation: count out of range -------------------------------------- */

static void test_validate_count_out_of_range(void **state)
{
    (void)state;
    cbx_settings s;
    cbx_settings_defaults(&s);

    s.virtual_controllers.count = 0;
    assert_int_equal(cbx_settings_validate(&s), -EINVAL);

    s.virtual_controllers.count = 17;
    assert_int_equal(cbx_settings_validate(&s), -EINVAL);

    s.virtual_controllers.count = 1;
    assert_int_equal(cbx_settings_validate(&s), 0);

    s.virtual_controllers.count = 16;
    /* Need 16 valid types */
    for (int i = 0; i < 16; i++)
        strncpy(s.virtual_controllers.types[i], "xb360",
                sizeof(s.virtual_controllers.types[i]) - 1);
    assert_int_equal(cbx_settings_validate(&s), 0);
}

/* --- Validation: unknown type -------------------------------------------- */

static void test_validate_unknown_type(void **state)
{
    (void)state;
    cbx_settings s;
    cbx_settings_defaults(&s);

    strncpy(s.virtual_controllers.types[0], "unknown_type",
            sizeof(s.virtual_controllers.types[0]) - 1);
    assert_int_equal(cbx_settings_validate(&s), -EINVAL);

    strncpy(s.virtual_controllers.types[0], "xb360",
            sizeof(s.virtual_controllers.types[0]) - 1);
    assert_int_equal(cbx_settings_validate(&s), 0);
}

/* --- Save rejects invalid settings --------------------------------------- */

static void test_save_rejects_invalid(void **state)
{
    (void)state;
    cbx_settings s;
    cbx_settings_defaults(&s);
    s.overlay_opacity = 2.0;
    assert_int_equal(cbx_settings_save(&s), -EINVAL);
}

/* --- Missing fields → defaults filled ------------------------------------ */

static void test_missing_fields(void **state)
{
    (void)state;
    /* Partial YAML: only overlay_trigger and theme. */
    write_raw_settings(
        "overlay_trigger: \"L3+R3\"\n"
        "theme: \"neon\"\n");

    cbx_settings s;
    int rc = cbx_settings_load(&s);
    assert_int_equal(rc, 0);

    /* Present fields from YAML. */
    assert_string_equal(s.overlay_trigger, "L3+R3");
    assert_string_equal(s.theme, "neon");

    /* Missing fields → defaults. */
    assert_true(s.launch_at_boot);
    assert_float_equal(s.overlay_opacity, 0.85, 0.001);
    assert_int_equal(s.virtual_controllers.count, 4);
    assert_string_equal(s.virtual_controllers.types[0], "xb360");
}

/* --- Out-of-range values clamped on load --------------------------------- */

static void test_clamp_on_load(void **state)
{
    (void)state;
    write_raw_settings(
        "overlay_opacity: 1.5\n"
        "virtual_controllers:\n"
        "  count: 0\n"
        "  types:\n"
        "    - bogus_type\n");

    cbx_settings s;
    int rc = cbx_settings_load(&s);
    assert_int_equal(rc, 0);

    /* Clamped to valid ranges. */
    assert_float_equal(s.overlay_opacity, 1.0, 0.001);
    assert_int_equal(s.virtual_controllers.count, 1);
    /* Unknown type replaced with default. */
    assert_string_equal(s.virtual_controllers.types[0], "xb360");
}

/* --- Atomic write: file mode 0600 ---------------------------------------- */

static void test_file_mode_0600(void **state)
{
    (void)state;
    cbx_settings s;
    cbx_settings_defaults(&s);

    int rc = cbx_settings_save(&s);
    assert_int_equal(rc, 0);

    char path[PATH_MAX + 64];
    settings_path(path, sizeof(path));

    struct stat st;
    assert_int_equal(stat(path, &st), 0);
    assert_true(S_ISREG(st.st_mode));
    /* Check that only owner has read/write (0600, modulo umask).
     * We explicitly fchmod 0600, so the mode should be exactly 0600. */
    assert_int_equal(st.st_mode & 0777, 0600);
}

/* --- Flow-style types array --------------------------------------------- */

static void test_flow_style_types(void **state)
{
    (void)state;
    write_raw_settings(
        "overlay_trigger: \"Select+A\"\n"
        "launch_at_boot: false\n"
        "theme: \"default\"\n"
        "overlay_opacity: 0.7\n"
        "virtual_controllers:\n"
        "  count: 3\n"
        "  types: [ds5, deck, gamepad]\n");

    cbx_settings s;
    int rc = cbx_settings_load(&s);
    assert_int_equal(rc, 0);

    assert_string_equal(s.overlay_trigger, "Select+A");
    assert_false(s.launch_at_boot);
    assert_float_equal(s.overlay_opacity, 0.7, 0.001);
    assert_int_equal(s.virtual_controllers.count, 3);
    assert_string_equal(s.virtual_controllers.types[0], "ds5");
    assert_string_equal(s.virtual_controllers.types[1], "deck");
    assert_string_equal(s.virtual_controllers.types[2], "gamepad");
}

/* --- Known-good type list ------------------------------------------------ */

static void test_known_controller_types(void **state)
{
    (void)state;
    assert_true(cbx_is_known_controller_type("xb360"));
    assert_true(cbx_is_known_controller_type("ds5"));
    assert_true(cbx_is_known_controller_type("deck"));
    assert_true(cbx_is_known_controller_type("gamepad"));
    assert_true(cbx_is_known_controller_type("mouse"));
    assert_true(cbx_is_known_controller_type("keyboard"));
    assert_true(cbx_is_known_controller_type("touchscreen"));

    assert_false(cbx_is_known_controller_type("bogus"));
    assert_false(cbx_is_known_controller_type(""));
    assert_false(cbx_is_known_controller_type(NULL));
}

/* --- YAML security: max document size ----------------------------------- */

static void test_max_doc_size(void **state)
{
    (void)state;
    /* Create a file > 1 MB. */
    char dir[PATH_MAX + 32];
    snprintf(dir, sizeof(dir), "%s/.config/controller-box", test_home);
    char cmd[PATH_MAX * 2 + 32];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
    int r2 = system(cmd);
    (void)r2;

    char path[PATH_MAX + 64];
    settings_path(path, sizeof(path));

    FILE *f = fopen(path, "w");
    assert_non_null(f);
    /* Write 1.1 MB of YAML. */
    char line[512];
    snprintf(line, sizeof(line), "overlay_trigger: \"%s\"\n",
             "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
             "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
             "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
             "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
             "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
             "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    /* ~200 bytes per line; need ~5500 lines for 1.1 MB. */
    for (int i = 0; i < 6000; i++)
        fputs(line, f);
    fclose(f);

    cbx_settings s;
    int rc = cbx_settings_load(&s);
    assert_int_equal(rc, -EFBIG);
}

/* --- YAML security: custom tags rejected -------------------------------- */

static void test_custom_tags_rejected(void **state)
{
    (void)state;
    write_raw_settings(
        "overlay_trigger: !str \"Select+A\"\n");

    cbx_settings s;
    int rc = cbx_settings_load(&s);
    assert_int_equal(rc, -EPERM);
}

/* --- YAML security: tag directives rejected ------------------------------ */

static void test_tag_directives_rejected(void **state)
{
    (void)state;
    write_raw_settings(
        "%TAG !foo! tag:example.org,2024:foo/\n"
        "---\n"
        "overlay_trigger: \"Select+A\"\n");

    cbx_settings s;
    int rc = cbx_settings_load(&s);
    assert_int_equal(rc, -EPERM);
}

/* --- Empty file treated as defaults -------------------------------------- */

static void test_empty_file(void **state)
{
    (void)state;
    write_raw_settings("");

    cbx_settings s;
    int rc = cbx_settings_load(&s);
    assert_int_equal(rc, 0);
    assert_string_equal(s.overlay_trigger, "Select+A");
    assert_int_equal(s.virtual_controllers.count, 4);
}

/* --- Bool variants (YAML 1.1) -------------------------------------------- */

static void test_bool_variants(void **state)
{
    (void)state;
    write_raw_settings(
        "overlay_trigger: \"Select+A\"\n"
        "launch_at_boot: yes\n"
        "theme: \"default\"\n"
        "overlay_opacity: 0.85\n"
        "virtual_controllers:\n"
        "  count: 4\n"
        "  types:\n"
        "    - xb360\n"
        "    - xb360\n"
        "    - xb360\n"
        "    - xb360\n");

    cbx_settings s;
    int rc = cbx_settings_load(&s);
    assert_int_equal(rc, 0);
    assert_true(s.launch_at_boot);

    /* Also test "on". */
    write_raw_settings("launch_at_boot: on\n");
    rc = cbx_settings_load(&s);
    assert_int_equal(rc, 0);
    assert_true(s.launch_at_boot);

    /* "false" */
    write_raw_settings("launch_at_boot: false\n");
    rc = cbx_settings_load(&s);
    assert_int_equal(rc, 0);
    assert_false(s.launch_at_boot);

    /* "no" */
    write_raw_settings("launch_at_boot: no\n");
    rc = cbx_settings_load(&s);
    assert_int_equal(rc, 0);
    assert_false(s.launch_at_boot);
}

/* --- main ---------------------------------------------------------------- */

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_defaults_no_file,
            setup_home, teardown_home),
        cmocka_unit_test(test_defaults_function),
        cmocka_unit_test_setup_teardown(test_round_trip,
            setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_round_trip_defaults,
            setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_validate_opacity_out_of_range,
            setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_validate_count_out_of_range,
            setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_validate_unknown_type,
            setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_save_rejects_invalid,
            setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_missing_fields,
            setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_clamp_on_load,
            setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_file_mode_0600,
            setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_flow_style_types,
            setup_home, teardown_home),
        cmocka_unit_test(test_known_controller_types),
        cmocka_unit_test_setup_teardown(test_max_doc_size,
            setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_custom_tags_rejected,
            setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_tag_directives_rejected,
            setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_empty_file,
            setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_bool_variants,
            setup_home, teardown_home),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}