/*
 * test_assign_persist.c — cmocka tests for atomic assignment persistence (Task 26).
 *
 * Tests cover:
 *   - persist_set (new, update, full table, invalid args)
 *   - persist_set_slot (existing, not found, invalid)
 *   - persist_set_profile (existing, not found, invalid, empty profile)
 *   - persist_remove (existing, not found, idempotent, null)
 *   - persist_auto_assign (new, existing, all full, null, atomic)
 *   - round-trip persistence (save → reload → verify)
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "identify/assign_persist.h"
#include "identify/assign.h"
#include "config/config_assignments.h"
#include "config/config_settings.h" /* CBX_MAX_CONTROLLERS */

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

/* --- Test fixture --------------------------------------------------------- */

static char test_home[PATH_MAX];

static int setup_home(void **state)
{
    (void)state;
    snprintf(test_home, sizeof(test_home), "/tmp/cbx-asgn-persist-XXXXXX");
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

static void assignments_path(char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "%s/.config/controller-box/assignments.yaml",
             test_home);
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void write_raw_assignments(const char *yaml)
{
    char dir[PATH_MAX + 64];
    char path[PATH_MAX + 128];
    snprintf(dir, sizeof(dir), "%s/.config/controller-box", test_home);
    snprintf(path, sizeof(path), "%s/assignments.yaml", dir);

    /* Create config dir */
    char cmd[PATH_MAX * 2 + 32];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
    int rc = system(cmd);
    (void)rc;

    FILE *f = fopen(path, "w");
    assert_non_null(f);
    fputs(yaml, f);
    fclose(f);
}

/* --- persist_set tests ---------------------------------------------------- */

static void test_set_new(void **state)
{
    (void)state;
    int rc = cbx_assign_persist_set("ORDER:0", 0, "default");
    assert_int_equal(rc, 0);

    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assignments_load(&a), 0);
    assert_int_equal(a.assignment_count, 1);
    assert_string_equal(a.assignments[0].id, "ORDER:0");
    assert_int_equal(a.assignments[0].slot, 0);
    assert_string_equal(a.assignments[0].profile, "default");
}

static void test_set_update_existing(void **state)
{
    (void)state;
    assert_int_equal(cbx_assign_persist_set("ORDER:0", 0, "default"), 0);
    assert_int_equal(cbx_assign_persist_set("ORDER:0", 2, "fighting"), 0);

    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assignments_load(&a), 0);
    assert_int_equal(a.assignment_count, 1); /* still 1, not 2 */
    assert_int_equal(a.assignments[0].slot, 2);
    assert_string_equal(a.assignments[0].profile, "fighting");
}

static void test_set_multiple(void **state)
{
    (void)state;
    assert_int_equal(cbx_assign_persist_set("ORDER:0", 0, "default"), 0);
    assert_int_equal(cbx_assign_persist_set("ORDER:1", 1, "fighting"), 0);
    assert_int_equal(cbx_assign_persist_set("BT:AB:CD:01:02:03:04", 2, "default"), 0);

    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assignments_load(&a), 0);
    assert_int_equal(a.assignment_count, 3);
}

static void test_set_empty_profile(void **state)
{
    (void)state;
    int rc = cbx_assign_persist_set("ORDER:0", 0, "");
    assert_int_equal(rc, 0);

    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assignments_load(&a), 0);
    assert_string_equal(a.assignments[0].profile, "");
}

static void test_set_null_profile(void **state)
{
    (void)state;
    int rc = cbx_assign_persist_set("ORDER:0", 0, NULL);
    assert_int_equal(rc, 0);

    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assignments_load(&a), 0);
    assert_string_equal(a.assignments[0].profile, "");
}

static void test_set_invalid_id(void **state)
{
    (void)state;
    assert_int_equal(cbx_assign_persist_set("INVALID", 0, "default"), -EINVAL);
    assert_int_equal(cbx_assign_persist_set("", 0, "default"), -EINVAL);
}

static void test_set_negative_slot(void **state)
{
    (void)state;
    assert_int_equal(cbx_assign_persist_set("ORDER:0", -1, "default"), -EINVAL);
}

static void test_set_null_id(void **state)
{
    (void)state;
    assert_int_equal(cbx_assign_persist_set(NULL, 0, "default"), -EINVAL);
}

static void test_set_invalid_profile(void **state)
{
    (void)state;
    assert_int_equal(cbx_assign_persist_set("ORDER:0", 0, "bad profile!"), -EINVAL);
}

static void test_set_preserves_gamepad_order(void **state)
{
    (void)state;
    write_raw_assignments(
        "assignments:\n"
        "  - id: \"ORDER:0\"\n"
        "    slot: 0\n"
        "    profile: \"default\"\n"
        "gamepad_order:\n"
        "  - \"ORDER:0\"\n");

    assert_int_equal(cbx_assign_persist_set("ORDER:1", 1, "default"), 0);

    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assignments_load(&a), 0);
    assert_int_equal(a.assignment_count, 2);
    assert_int_equal(a.gamepad_order_count, 1);
    assert_string_equal(a.gamepad_order[0], "ORDER:0");
}

static void test_set_full_table(void **state)
{
    (void)state;
    /* Fill the table to max capacity.  Slots must be < CBX_MAX_CONTROLLERS
     * (validated by cbx_assignments_validate); wrap to stay in range while
     * exercising all CBX_MAX_ASSIGNMENTS entries. */
    for (int i = 0; i < CBX_MAX_ASSIGNMENTS; i++) {
        char id[32];
        snprintf(id, sizeof(id), "ORDER:%d", i);
        assert_int_equal(cbx_assign_persist_set(id, i % CBX_MAX_CONTROLLERS, "default"), 0);
    }

    /* Adding one more should fail with -ENOSPC */
    int rc = cbx_assign_persist_set("ORDER:999", 0, "default");
    assert_int_equal(rc, -ENOSPC);
}

/* --- persist_set_slot tests ----------------------------------------------- */

static void test_set_slot_existing(void **state)
{
    (void)state;
    assert_int_equal(cbx_assign_persist_set("ORDER:0", 0, "default"), 0);
    assert_int_equal(cbx_assign_persist_set_slot("ORDER:0", 3), 0);

    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assignments_load(&a), 0);
    assert_int_equal(a.assignments[0].slot, 3);
    assert_string_equal(a.assignments[0].profile, "default"); /* unchanged */
}

static void test_set_slot_not_found(void **state)
{
    (void)state;
    assert_int_equal(cbx_assign_persist_set_slot("ORDER:99", 3), -ENOENT);
}

static void test_set_slot_negative(void **state)
{
    (void)state;
    assert_int_equal(cbx_assign_persist_set("ORDER:0", 0, "default"), 0);
    assert_int_equal(cbx_assign_persist_set_slot("ORDER:0", -1), -EINVAL);
}

static void test_set_slot_null(void **state)
{
    (void)state;
    assert_int_equal(cbx_assign_persist_set_slot(NULL, 3), -EINVAL);
}

/* --- persist_set_profile tests -------------------------------------------- */

static void test_set_profile_existing(void **state)
{
    (void)state;
    assert_int_equal(cbx_assign_persist_set("ORDER:0", 0, "default"), 0);
    assert_int_equal(cbx_assign_persist_set_profile("ORDER:0", "fighting"), 0);

    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assignments_load(&a), 0);
    assert_string_equal(a.assignments[0].profile, "fighting");
    assert_int_equal(a.assignments[0].slot, 0); /* unchanged */
}

static void test_set_profile_not_found(void **state)
{
    (void)state;
    assert_int_equal(cbx_assign_persist_set_profile("ORDER:99", "fighting"), -ENOENT);
}

static void test_set_profile_empty(void **state)
{
    (void)state;
    assert_int_equal(cbx_assign_persist_set("ORDER:0", 0, "default"), 0);
    assert_int_equal(cbx_assign_persist_set_profile("ORDER:0", ""), 0);

    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assignments_load(&a), 0);
    assert_string_equal(a.assignments[0].profile, "");
}

static void test_set_profile_null(void **state)
{
    (void)state;
    assert_int_equal(cbx_assign_persist_set("ORDER:0", 0, "default"), 0);
    assert_int_equal(cbx_assign_persist_set_profile("ORDER:0", NULL), 0);

    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assignments_load(&a), 0);
    assert_string_equal(a.assignments[0].profile, "");
}

static void test_set_profile_invalid(void **state)
{
    (void)state;
    assert_int_equal(cbx_assign_persist_set("ORDER:0", 0, "default"), 0);
    assert_int_equal(cbx_assign_persist_set_profile("ORDER:0", "bad!"), -EINVAL);
}

static void test_set_profile_null_id(void **state)
{
    (void)state;
    assert_int_equal(cbx_assign_persist_set_profile(NULL, "fighting"), -EINVAL);
}

/* --- persist_remove tests ------------------------------------------------- */

static void test_remove_existing(void **state)
{
    (void)state;
    assert_int_equal(cbx_assign_persist_set("ORDER:0", 0, "default"), 0);
    assert_int_equal(cbx_assign_persist_set("ORDER:1", 1, "default"), 0);
    assert_int_equal(cbx_assign_persist_remove("ORDER:0"), 0);

    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assignments_load(&a), 0);
    assert_int_equal(a.assignment_count, 1);
    assert_string_equal(a.assignments[0].id, "ORDER:1");
}

static void test_remove_not_found(void **state)
{
    (void)state;
    assert_int_equal(cbx_assign_persist_remove("ORDER:99"), 0); /* idempotent */
}

static void test_remove_null(void **state)
{
    (void)state;
    assert_int_equal(cbx_assign_persist_remove(NULL), -EINVAL);
}

static void test_remove_preserves_gamepad_order(void **state)
{
    (void)state;
    write_raw_assignments(
        "assignments:\n"
        "  - id: \"ORDER:0\"\n"
        "    slot: 0\n"
        "    profile: \"default\"\n"
        "  - id: \"ORDER:1\"\n"
        "    slot: 1\n"
        "    profile: \"default\"\n"
        "gamepad_order:\n"
        "  - \"ORDER:0\"\n"
        "  - \"ORDER:1\"\n");

    assert_int_equal(cbx_assign_persist_remove("ORDER:0"), 0);

    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assignments_load(&a), 0);
    assert_int_equal(a.assignment_count, 1);
    assert_string_equal(a.assignments[0].id, "ORDER:1");
    assert_int_equal(a.gamepad_order_count, 2);
}

static void test_remove_all(void **state)
{
    (void)state;
    assert_int_equal(cbx_assign_persist_set("ORDER:0", 0, "default"), 0);
    assert_int_equal(cbx_assign_persist_set("ORDER:1", 1, "default"), 0);
    assert_int_equal(cbx_assign_persist_remove("ORDER:0"), 0);
    assert_int_equal(cbx_assign_persist_remove("ORDER:1"), 0);

    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assignments_load(&a), 0);
    assert_int_equal(a.assignment_count, 0);
}

/* --- persist_auto_assign tests -------------------------------------------- */

static void test_auto_assign_new(void **state)
{
    (void)state;
    int slot = -1;
    char profile[CBX_MAX_PROFILE_LEN];
    int rc = cbx_assign_persist_auto_assign("ORDER:0", 4, &slot, profile, sizeof(profile));
    assert_int_equal(rc, 1); /* newly assigned */
    assert_int_equal(slot, 0);
    assert_string_equal(profile, "default");

    /* Verify persisted */
    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assignments_load(&a), 0);
    assert_int_equal(a.assignment_count, 1);
    assert_string_equal(a.assignments[0].id, "ORDER:0");
    assert_int_equal(a.assignments[0].slot, 0);
}

static void test_auto_assign_existing(void **state)
{
    (void)state;
    assert_int_equal(cbx_assign_persist_set("ORDER:0", 2, "fighting"), 0);

    int slot = -1;
    char profile[CBX_MAX_PROFILE_LEN];
    int rc = cbx_assign_persist_auto_assign("ORDER:0", 4, &slot, profile, sizeof(profile));
    assert_int_equal(rc, 0); /* existing found */
    assert_int_equal(slot, 2);
    assert_string_equal(profile, "fighting");

    /* Verify not duplicated */
    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assignments_load(&a), 0);
    assert_int_equal(a.assignment_count, 1);
}

static void test_auto_assign_lowest_free(void **state)
{
    (void)state;
    /* Pre-occupy slots 0 and 2 */
    assert_int_equal(cbx_assign_persist_set("ORDER:0", 0, "default"), 0);
    assert_int_equal(cbx_assign_persist_set("ORDER:1", 2, "default"), 0);

    int slot = -1;
    int rc = cbx_assign_persist_auto_assign("ORDER:99", 4, &slot, NULL, 0);
    assert_int_equal(rc, 1);
    assert_int_equal(slot, 1); /* gap at slot 1 */
}

static void test_auto_assign_all_full(void **state)
{
    (void)state;
    for (int i = 0; i < 4; i++) {
        char id[32];
        snprintf(id, sizeof(id), "ORDER:%d", i);
        assert_int_equal(cbx_assign_persist_set(id, i, "default"), 0);
    }

    int slot = -1;
    int rc = cbx_assign_persist_auto_assign("ORDER:99", 4, &slot, NULL, 0);
    assert_int_equal(rc, -ENOENT);
}

static void test_auto_assign_null_args(void **state)
{
    (void)state;
    int slot;
    assert_int_equal(cbx_assign_persist_auto_assign(NULL, 4, &slot, NULL, 0), -EINVAL);
    assert_int_equal(cbx_assign_persist_auto_assign("ORDER:0", 4, NULL, NULL, 0), -EINVAL);
    assert_int_equal(cbx_assign_persist_auto_assign("ORDER:0", 0, &slot, NULL, 0), -EINVAL);
}

static void test_auto_assign_no_profile_buf(void **state)
{
    (void)state;
    int slot = -1;
    int rc = cbx_assign_persist_auto_assign("ORDER:0", 4, &slot, NULL, 0);
    assert_int_equal(rc, 1);
    assert_int_equal(slot, 0);
}

static void test_auto_assign_small_profile_buf(void **state)
{
    (void)state;
    int slot = -1;
    char profile[4]; /* smaller than "default" */
    int rc = cbx_assign_persist_auto_assign("ORDER:0", 4, &slot, profile, sizeof(profile));
    assert_int_equal(rc, 1);
    assert_int_equal(slot, 0);
    /* Should be truncated safely */
    assert_string_equal(profile, "def");
}

static void test_auto_assign_creates_file(void **state)
{
    (void)state;
    char path[PATH_MAX + 64];
    assignments_path(path, sizeof(path));
    assert_false(file_exists(path));

    int slot;
    int rc = cbx_assign_persist_auto_assign("ORDER:0", 4, &slot, NULL, 0);
    assert_int_equal(rc, 1);
    assert_true(file_exists(path));
}

static void test_auto_assign_preserves_gamepad_order(void **state)
{
    (void)state;
    write_raw_assignments(
        "assignments:\n"
        "  - id: \"ORDER:0\"\n"
        "    slot: 0\n"
        "    profile: \"default\"\n"
        "gamepad_order:\n"
        "  - \"ORDER:0\"\n");

    int slot;
    int rc = cbx_assign_persist_auto_assign("ORDER:1", 4, &slot, NULL, 0);
    assert_int_equal(rc, 1);
    assert_int_equal(slot, 1);

    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assignments_load(&a), 0);
    assert_int_equal(a.assignment_count, 2);
    assert_int_equal(a.gamepad_order_count, 1);
    assert_string_equal(a.gamepad_order[0], "ORDER:0");
}

/* --- Atomic write tests --------------------------------------------------- */

static void test_set_atomic_file_mode(void **state)
{
    (void)state;
    assert_int_equal(cbx_assign_persist_set("ORDER:0", 0, "default"), 0);

    char path[PATH_MAX + 64];
    assignments_path(path, sizeof(path));

    struct stat st;
    assert_int_equal(stat(path, &st), 0);
    assert_int_equal(st.st_mode & 0777, 0600);
}

static void test_set_then_reload_matches(void **state)
{
    (void)state;
    assert_int_equal(cbx_assign_persist_set("BT:AB:CD:01:02:03:04", 1, "fighting"), 0);
    assert_int_equal(cbx_assign_persist_set("USB:SN12345", 0, "default"), 0);
    assert_int_equal(cbx_assign_persist_set("ORDER:0", 2, ""), 0);

    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assignments_load(&a), 0);
    assert_int_equal(a.assignment_count, 3);

    /* Find each by ID */
    cbx_assignment out;
    assert_int_equal(cbx_assign_lookup(&a, "BT:AB:CD:01:02:03:04", &out), 0);
    assert_int_equal(out.slot, 1);
    assert_string_equal(out.profile, "fighting");

    assert_int_equal(cbx_assign_lookup(&a, "USB:SN12345", &out), 0);
    assert_int_equal(out.slot, 0);
    assert_string_equal(out.profile, "default");

    assert_int_equal(cbx_assign_lookup(&a, "ORDER:0", &out), 0);
    assert_int_equal(out.slot, 2);
    assert_string_equal(out.profile, "");
}

static void test_set_no_file_load_ok(void **state)
{
    (void)state;
    /* Setting on a non-existent file should work — load returns empty */
    int rc = cbx_assign_persist_set("ORDER:0", 0, "default");
    assert_int_equal(rc, 0);
}

static void test_auto_assign_then_update_slot(void **state)
{
    (void)state;
    int slot;
    cbx_assign_persist_auto_assign("ORDER:0", 4, &slot, NULL, 0);
    assert_int_equal(slot, 0);

    /* Now update slot via overlay change */
    assert_int_equal(cbx_assign_persist_set_slot("ORDER:0", 3), 0);

    /* Re-verify */
    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assignments_load(&a), 0);
    assert_int_equal(a.assignments[0].slot, 3);
}

/* --- Main ---------------------------------------------------------------- */

int main(void)
{
    const struct CMUnitTest tests[] = {
        /* set */
        cmocka_unit_test_setup_teardown(test_set_new, setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_set_update_existing, setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_set_multiple, setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_set_empty_profile, setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_set_null_profile, setup_home, teardown_home),
        cmocka_unit_test(test_set_invalid_id),
        cmocka_unit_test(test_set_negative_slot),
        cmocka_unit_test(test_set_null_id),
        cmocka_unit_test(test_set_invalid_profile),
        cmocka_unit_test_setup_teardown(test_set_preserves_gamepad_order, setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_set_full_table, setup_home, teardown_home),

        /* set_slot */
        cmocka_unit_test_setup_teardown(test_set_slot_existing, setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_set_slot_not_found, setup_home, teardown_home),
        cmocka_unit_test(test_set_slot_negative),
        cmocka_unit_test(test_set_slot_null),

        /* set_profile */
        cmocka_unit_test_setup_teardown(test_set_profile_existing, setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_set_profile_not_found, setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_set_profile_empty, setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_set_profile_null, setup_home, teardown_home),
        cmocka_unit_test(test_set_profile_invalid),
        cmocka_unit_test(test_set_profile_null_id),

        /* remove */
        cmocka_unit_test_setup_teardown(test_remove_existing, setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_remove_not_found, setup_home, teardown_home),
        cmocka_unit_test(test_remove_null),
        cmocka_unit_test_setup_teardown(test_remove_preserves_gamepad_order, setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_remove_all, setup_home, teardown_home),

        /* auto_assign */
        cmocka_unit_test_setup_teardown(test_auto_assign_new, setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_auto_assign_existing, setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_auto_assign_lowest_free, setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_auto_assign_all_full, setup_home, teardown_home),
        cmocka_unit_test(test_auto_assign_null_args),
        cmocka_unit_test_setup_teardown(test_auto_assign_no_profile_buf, setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_auto_assign_small_profile_buf, setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_auto_assign_creates_file, setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_auto_assign_preserves_gamepad_order, setup_home, teardown_home),

        /* atomic / round-trip */
        cmocka_unit_test_setup_teardown(test_set_atomic_file_mode, setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_set_then_reload_matches, setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_set_no_file_load_ok, setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_auto_assign_then_update_slot, setup_home, teardown_home),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}