/*
 * test_assignments.c — cmocka tests for assignments.yaml read/write (Task 6).
 *
 * Tests cover:
 *   - Empty/no-file → empty struct
 *   - Round-trip (save → load → compare)
 *   - ID validation (BT:MAC, USB:serial, USB:phys:path, ORDER:n, invalid)
 *   - Profile validation
 *   - Slot validation
 *   - Gamepad order persistence
 *   - Atomic write (file mode 0600)
 *   - YAML security constraints (max doc size, custom tags, tag directives)
 *   - Multiple assignments
 *   - Edge cases (empty profile, zero slot)
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "config/config_assignments.h"

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

static char test_home[PATH_MAX];

static int setup_home(void **state)
{
    (void)state;
    snprintf(test_home, sizeof(test_home), "/tmp/cbx-asgn-XXXXXX");
    if (!mkdtemp(test_home))
        return -1;
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
    int rc = system(cmd);
    (void)rc;
    return 0;
}

/* Build the assignments.yaml path for the current test HOME. */
static void assignments_path(char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "%s/.config/controller-box/assignments.yaml",
             test_home);
}

/* Write raw YAML text to the assignments file (bypasses the save function). */
static void write_raw_assignments(const char *yaml)
{
    char dir[PATH_MAX + 64];
    snprintf(dir, sizeof(dir), "%s/.config/controller-box", test_home);

    /* Create the directory */
    char cmd[PATH_MAX * 2 + 32];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
    int rc = system(cmd);
    (void)rc;

    char path[PATH_MAX + 64];
    assignments_path(path, sizeof(path));

    FILE *f = fopen(path, "w");
    assert_non_null(f);
    fprintf(f, "%s", yaml);
    fclose(f);
}

/* --- ID validation tests ------------------------------------------------- */

static void test_validate_id_bt_mac(void **state)
{
    (void)state;
    assert_true(cbx_validate_id("BT:AB:CD:01:EF:23:45"));
    assert_true(cbx_validate_id("BT:ab:cd:01:ef:23:45"));
    assert_true(cbx_validate_id("BT:00:00:00:00:00:00"));
}

static void test_validate_id_bt_invalid(void **state)
{
    (void)state;
    assert_false(cbx_validate_id("BT:AB:CD:01:EF"));          /* too few octets */
    assert_false(cbx_validate_id("BT:AB:CD:01:EF:23:45:67")); /* too many */
    assert_false(cbx_validate_id("BT:GG:CD:01:EF:23:45"));    /* non-hex */
    assert_false(cbx_validate_id("BT:"));                     /* empty */
    assert_false(cbx_validate_id("BT:AB:CD:01:EF:23:4"));    /* too few octets */
}

static void test_validate_id_usb_serial(void **state)
{
    (void)state;
    assert_true(cbx_validate_id("USB:SN12345"));
    assert_true(cbx_validate_id("USB:abc-123_def"));
    assert_true(cbx_validate_id("USB:0"));
}

static void test_validate_id_usb_serial_invalid(void **state)
{
    (void)state;
    assert_false(cbx_validate_id("USB:"));                     /* empty */
    assert_false(cbx_validate_id("USB:SN 123"));              /* space */
    assert_false(cbx_validate_id("USB:SN123!"));              /* special char */
}

static void test_validate_id_usb_phys(void **state)
{
    (void)state;
    assert_true(cbx_validate_id("USB:phys:usb-3-2"));
    assert_true(cbx_validate_id("USB:phys:0000:00:1d.0/usb1"));
}

static void test_validate_id_usb_phys_invalid(void **state)
{
    (void)state;
    assert_false(cbx_validate_id("USB:phys:"));                /* empty */
    assert_false(cbx_validate_id("USB:phys:has space"));       /* space */
}

static void test_validate_id_order(void **state)
{
    (void)state;
    assert_true(cbx_validate_id("ORDER:0"));
    assert_true(cbx_validate_id("ORDER:1"));
    assert_true(cbx_validate_id("ORDER:999"));
}

static void test_validate_id_order_invalid(void **state)
{
    (void)state;
    assert_false(cbx_validate_id("ORDER:"));                   /* empty */
    assert_false(cbx_validate_id("ORDER:-1"));                /* negative */
    assert_false(cbx_validate_id("ORDER:abc"));                /* non-numeric */
}

static void test_validate_id_other(void **state)
{
    (void)state;
    assert_false(cbx_validate_id(""));
    assert_false(cbx_validate_id(NULL));
    assert_false(cbx_validate_id("invalid"));
    assert_false(cbx_validate_id("WIFI:abc"));
}

/* --- Profile validation tests -------------------------------------------- */

static void test_validate_profile(void **state)
{
    (void)state;
    assert_true(cbx_validate_profile("fighting"));
    assert_true(cbx_validate_profile("default"));
    assert_true(cbx_validate_profile("my-profile_2"));
    assert_true(cbx_validate_profile(""));      /* empty = no profile */
    assert_true(cbx_validate_profile(NULL));    /* NULL treated as empty */
}

static void test_validate_profile_invalid(void **state)
{
    (void)state;
    assert_false(cbx_validate_profile("has space"));
    assert_false(cbx_validate_profile("dot.dot"));
    assert_false(cbx_validate_profile("slash/"));
}

/* --- Load/empty tests ---------------------------------------------------- */

static void test_load_no_file(void **state)
{
    (void)state;
    cbx_assignments a;
    int rc = cbx_assignments_load(&a);
    assert_int_equal(rc, 0);
    assert_int_equal(a.assignment_count, 0);
    assert_int_equal(a.gamepad_order_count, 0);
}

static void test_load_empty_file(void **state)
{
    (void)state;
    write_raw_assignments("");
    cbx_assignments a;
    int rc = cbx_assignments_load(&a);
    assert_int_equal(rc, 0);
    assert_int_equal(a.assignment_count, 0);
    assert_int_equal(a.gamepad_order_count, 0);
}

/* --- Round-trip tests ---------------------------------------------------- */

static void test_round_trip(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_assignments_init(&a);

    /* Add two assignments */
    strncpy(a.assignments[0].id, "BT:AB:CD:01:EF:23:45",
            sizeof(a.assignments[0].id) - 1);
    a.assignments[0].slot = 0;
    strncpy(a.assignments[0].profile, "fighting",
            sizeof(a.assignments[0].profile) - 1);

    strncpy(a.assignments[1].id, "USB:SN12345",
            sizeof(a.assignments[1].id) - 1);
    a.assignments[1].slot = 1;
    strncpy(a.assignments[1].profile, "default",
            sizeof(a.assignments[1].profile) - 1);
    a.assignment_count = 2;

    /* Add gamepad order */
    strncpy(a.gamepad_order[0], "BT:AB:CD:01:EF:23:45",
            sizeof(a.gamepad_order[0]) - 1);
    strncpy(a.gamepad_order[1], "USB:SN12345",
            sizeof(a.gamepad_order[1]) - 1);
    a.gamepad_order_count = 2;

    int rc = cbx_assignments_save(&a);
    assert_int_equal(rc, 0);

    /* Load and verify */
    cbx_assignments loaded;
    rc = cbx_assignments_load(&loaded);
    assert_int_equal(rc, 0);

    assert_int_equal(loaded.assignment_count, 2);
    assert_string_equal(loaded.assignments[0].id, "BT:AB:CD:01:EF:23:45");
    assert_int_equal(loaded.assignments[0].slot, 0);
    assert_string_equal(loaded.assignments[0].profile, "fighting");
    assert_string_equal(loaded.assignments[1].id, "USB:SN12345");
    assert_int_equal(loaded.assignments[1].slot, 1);
    assert_string_equal(loaded.assignments[1].profile, "default");

    assert_int_equal(loaded.gamepad_order_count, 2);
    assert_string_equal(loaded.gamepad_order[0], "BT:AB:CD:01:EF:23:45");
    assert_string_equal(loaded.gamepad_order[1], "USB:SN12345");
}

static void test_round_trip_empty(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_assignments_init(&a);

    int rc = cbx_assignments_save(&a);
    assert_int_equal(rc, 0);

    cbx_assignments loaded;
    rc = cbx_assignments_load(&loaded);
    assert_int_equal(rc, 0);
    assert_int_equal(loaded.assignment_count, 0);
    assert_int_equal(loaded.gamepad_order_count, 0);
}

static void test_round_trip_usb_phys(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_assignments_init(&a);

    strncpy(a.assignments[0].id, "USB:phys:usb-3-2",
            sizeof(a.assignments[0].id) - 1);
    a.assignments[0].slot = 0;
    strncpy(a.assignments[0].profile, "default",
            sizeof(a.assignments[0].profile) - 1);
    a.assignment_count = 1;

    int rc = cbx_assignments_save(&a);
    assert_int_equal(rc, 0);

    cbx_assignments loaded;
    rc = cbx_assignments_load(&loaded);
    assert_int_equal(rc, 0);
    assert_int_equal(loaded.assignment_count, 1);
    assert_string_equal(loaded.assignments[0].id, "USB:phys:usb-3-2");
    assert_int_equal(loaded.assignments[0].slot, 0);
    assert_string_equal(loaded.assignments[0].profile, "default");
}

static void test_round_trip_empty_profile(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_assignments_init(&a);

    strncpy(a.assignments[0].id, "USB:SN12345",
            sizeof(a.assignments[0].id) - 1);
    a.assignments[0].slot = 2;
    a.assignments[0].profile[0] = '\0';  /* empty profile */
    a.assignment_count = 1;

    int rc = cbx_assignments_save(&a);
    assert_int_equal(rc, 0);

    cbx_assignments loaded;
    rc = cbx_assignments_load(&loaded);
    assert_int_equal(rc, 0);
    assert_int_equal(loaded.assignment_count, 1);
    assert_string_equal(loaded.assignments[0].id, "USB:SN12345");
    assert_int_equal(loaded.assignments[0].slot, 2);
    assert_string_equal(loaded.assignments[0].profile, "");
}

static void test_round_trip_order_id(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_assignments_init(&a);

    strncpy(a.assignments[0].id, "ORDER:0",
            sizeof(a.assignments[0].id) - 1);
    a.assignments[0].slot = 0;
    strncpy(a.assignments[0].profile, "default",
            sizeof(a.assignments[0].profile) - 1);
    a.assignment_count = 1;

    strncpy(a.gamepad_order[0], "ORDER:0", sizeof(a.gamepad_order[0]) - 1);
    a.gamepad_order_count = 1;

    int rc = cbx_assignments_save(&a);
    assert_int_equal(rc, 0);

    cbx_assignments loaded;
    rc = cbx_assignments_load(&loaded);
    assert_int_equal(rc, 0);
    assert_int_equal(loaded.assignment_count, 1);
    assert_string_equal(loaded.assignments[0].id, "ORDER:0");
    assert_int_equal(loaded.gamepad_order_count, 1);
    assert_string_equal(loaded.gamepad_order[0], "ORDER:0");
}

/* --- Validation in save tests -------------------------------------------- */

static void test_save_rejects_invalid_id(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_assignments_init(&a);

    strncpy(a.assignments[0].id, "INVALID",
            sizeof(a.assignments[0].id) - 1);
    a.assignments[0].slot = 0;
    strncpy(a.assignments[0].profile, "default",
            sizeof(a.assignments[0].profile) - 1);
    a.assignment_count = 1;

    int rc = cbx_assignments_save(&a);
    assert_int_equal(rc, -EINVAL);
}

static void test_save_rejects_negative_slot(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_assignments_init(&a);

    strncpy(a.assignments[0].id, "USB:SN12345",
            sizeof(a.assignments[0].id) - 1);
    a.assignments[0].slot = -1;
    strncpy(a.assignments[0].profile, "default",
            sizeof(a.assignments[0].profile) - 1);
    a.assignment_count = 1;

    int rc = cbx_assignments_save(&a);
    assert_int_equal(rc, -EINVAL);
}

static void test_save_rejects_invalid_profile(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_assignments_init(&a);

    strncpy(a.assignments[0].id, "USB:SN12345",
            sizeof(a.assignments[0].id) - 1);
    a.assignments[0].slot = 0;
    strncpy(a.assignments[0].profile, "bad profile!",
            sizeof(a.assignments[0].profile) - 1);
    a.assignment_count = 1;

    int rc = cbx_assignments_save(&a);
    assert_int_equal(rc, -EINVAL);
}

static void test_save_rejects_invalid_gamepad_order(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_assignments_init(&a);

    strncpy(a.gamepad_order[0], "INVALID",
            sizeof(a.gamepad_order[0]) - 1);
    a.gamepad_order_count = 1;

    int rc = cbx_assignments_save(&a);
    assert_int_equal(rc, -EINVAL);
}

/* --- Atomic write / file mode tests -------------------------------------- */

static void test_file_mode_0600(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_assignments_init(&a);

    strncpy(a.assignments[0].id, "USB:SN12345",
            sizeof(a.assignments[0].id) - 1);
    a.assignments[0].slot = 0;
    strncpy(a.assignments[0].profile, "default",
            sizeof(a.assignments[0].profile) - 1);
    a.assignment_count = 1;

    int rc = cbx_assignments_save(&a);
    assert_int_equal(rc, 0);

    char path[PATH_MAX + 64];
    assignments_path(path, sizeof(path));

    struct stat st;
    assert_int_equal(stat(path, &st), 0);
    assert_int_equal(st.st_mode & 0777, 0600);
}

/* --- YAML security tests ------------------------------------------------- */

static void test_max_doc_size(void **state)
{
    (void)state;
    /* Create a file larger than 1MB */
    char dir[PATH_MAX + 64];
    snprintf(dir, sizeof(dir), "%s/.config/controller-box", test_home);
    char cmd[PATH_MAX * 2 + 32];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
    int rc = system(cmd);
    (void)rc;

    char path[PATH_MAX + 64];
    assignments_path(path, sizeof(path));

    FILE *f = fopen(path, "w");
    assert_non_null(f);
    fprintf(f, "assignments:\n");
    /* Write more than 1MB of content */
    for (int i = 0; i < 60000; i++) {
        fprintf(f, "  - id: \"USB:SN%05d\"\n    slot: 0\n    profile: default\n", i);
    }
    fclose(f);

    cbx_assignments a;
    rc = cbx_assignments_load(&a);
    assert_int_equal(rc, -EFBIG);
}

static void test_custom_tags_rejected(void **state)
{
    (void)state;
    write_raw_assignments(
        "assignments:\n"
        "  - id: !!str \"USB:SN12345\"\n"
        "    slot: 0\n"
        "    profile: default\n");
    cbx_assignments a;
    int rc = cbx_assignments_load(&a);
    assert_int_equal(rc, -EPERM);
}

static void test_tag_directives_rejected(void **state)
{
    (void)state;
    write_raw_assignments(
        "%TAG ! tag:example.com,2002:app/\n"
        "---\n"
        "assignments: []\n");
    cbx_assignments a;
    int rc = cbx_assignments_load(&a);
    assert_int_equal(rc, -EPERM);
}

/* --- Parse from YAML tests ----------------------------------------------- */

static void test_parse_from_yaml(void **state)
{
    (void)state;
    write_raw_assignments(
        "assignments:\n"
        "  - id: \"BT:AB:CD:01:EF:23:45\"\n"
        "    slot: 0\n"
        "    profile: fighting\n"
        "  - id: \"USB:SN12345\"\n"
        "    slot: 1\n"
        "    profile: default\n"
        "  - id: \"USB:phys:usb-3-2\"\n"
        "    slot: 0\n"
        "    profile: default\n"
        "gamepad_order:\n"
        "  - \"BT:AB:CD:01:EF:23:45\"\n"
        "  - \"USB:SN12345\"\n");

    cbx_assignments a;
    int rc = cbx_assignments_load(&a);
    assert_int_equal(rc, 0);

    assert_int_equal(a.assignment_count, 3);
    assert_string_equal(a.assignments[0].id, "BT:AB:CD:01:EF:23:45");
    assert_int_equal(a.assignments[0].slot, 0);
    assert_string_equal(a.assignments[0].profile, "fighting");
    assert_string_equal(a.assignments[1].id, "USB:SN12345");
    assert_int_equal(a.assignments[1].slot, 1);
    assert_string_equal(a.assignments[1].profile, "default");
    assert_string_equal(a.assignments[2].id, "USB:phys:usb-3-2");
    assert_int_equal(a.assignments[2].slot, 0);
    assert_string_equal(a.assignments[2].profile, "default");

    assert_int_equal(a.gamepad_order_count, 2);
    assert_string_equal(a.gamepad_order[0], "BT:AB:CD:01:EF:23:45");
    assert_string_equal(a.gamepad_order[1], "USB:SN12345");
}

static void test_parse_gamepad_order_only(void **state)
{
    (void)state;
    write_raw_assignments(
        "assignments: []\n"
        "gamepad_order:\n"
        "  - \"ORDER:0\"\n"
        "  - \"ORDER:1\"\n");

    cbx_assignments a;
    int rc = cbx_assignments_load(&a);
    assert_int_equal(rc, 0);
    assert_int_equal(a.assignment_count, 0);
    assert_int_equal(a.gamepad_order_count, 2);
    assert_string_equal(a.gamepad_order[0], "ORDER:0");
    assert_string_equal(a.gamepad_order[1], "ORDER:1");
}

static void test_parse_no_gamepad_order(void **state)
{
    (void)state;
    write_raw_assignments(
        "assignments:\n"
        "  - id: \"USB:SN12345\"\n"
        "    slot: 3\n"
        "    profile: fighting\n");

    cbx_assignments a;
    int rc = cbx_assignments_load(&a);
    assert_int_equal(rc, 0);
    assert_int_equal(a.assignment_count, 1);
    assert_int_equal(a.gamepad_order_count, 0);
}

/* --- Validate struct tests ----------------------------------------------- */

static void test_validate_empty_ok(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assignments_validate(&a), 0);
}

static void test_validate_null(void **state)
{
    (void)state;
    assert_int_equal(cbx_assignments_validate(NULL), -EINVAL);
}

/* --- Main ---------------------------------------------------------------- */

int main(void)
{
    const struct CMUnitTest tests[] = {
        /* ID validation */
        cmocka_unit_test(test_validate_id_bt_mac),
        cmocka_unit_test(test_validate_id_bt_invalid),
        cmocka_unit_test(test_validate_id_usb_serial),
        cmocka_unit_test(test_validate_id_usb_serial_invalid),
        cmocka_unit_test(test_validate_id_usb_phys),
        cmocka_unit_test(test_validate_id_usb_phys_invalid),
        cmocka_unit_test(test_validate_id_order),
        cmocka_unit_test(test_validate_id_order_invalid),
        cmocka_unit_test(test_validate_id_other),

        /* Profile validation */
        cmocka_unit_test(test_validate_profile),
        cmocka_unit_test(test_validate_profile_invalid),

        /* Load / empty */
        cmocka_unit_test(test_load_no_file),
        cmocka_unit_test(test_load_empty_file),

        /* Round-trip */
        cmocka_unit_test(test_round_trip),
        cmocka_unit_test(test_round_trip_empty),
        cmocka_unit_test(test_round_trip_usb_phys),
        cmocka_unit_test(test_round_trip_empty_profile),
        cmocka_unit_test(test_round_trip_order_id),

        /* Validation in save */
        cmocka_unit_test(test_save_rejects_invalid_id),
        cmocka_unit_test(test_save_rejects_negative_slot),
        cmocka_unit_test(test_save_rejects_invalid_profile),
        cmocka_unit_test(test_save_rejects_invalid_gamepad_order),

        /* File mode */
        cmocka_unit_test(test_file_mode_0600),

        /* YAML security */
        cmocka_unit_test(test_max_doc_size),
        cmocka_unit_test(test_custom_tags_rejected),
        cmocka_unit_test(test_tag_directives_rejected),

        /* Parse from YAML */
        cmocka_unit_test(test_parse_from_yaml),
        cmocka_unit_test(test_parse_gamepad_order_only),
        cmocka_unit_test(test_parse_no_gamepad_order),

        /* Validate struct */
        cmocka_unit_test(test_validate_empty_ok),
        cmocka_unit_test(test_validate_null),
    };

    return cmocka_run_group_tests(tests, setup_home, teardown_home);
}