/*
 * test_assign.c — cmocka tests for assignment lookup (Task 26).
 *
 * Tests cover:
 *   - find_index (found, not found, null args, multiple entries)
 *   - lookup (found, not found, null args)
 *   - slot_occupied (yes, no, null, negative)
 *   - lowest_free_slot (empty, partially filled, all filled, max_slots edge)
 *   - make_default (valid, invalid id, negative slot, null)
 *   - resolve (existing found, default created, all slots full, null args)
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "identify/assign.h"
#include "config/config_assignments.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* --- Helpers -------------------------------------------------------------- */

static void fill_assignments(cbx_assignments *a, int count)
{
    cbx_assignments_init(a);
    for (int i = 0; i < count && i < CBX_MAX_ASSIGNMENTS; i++) {
        snprintf(a->assignments[i].id, CBX_MAX_ID_LEN, "ORDER:%d", i);
        a->assignments[i].slot = i;
        strncpy(a->assignments[i].profile, "default", CBX_MAX_PROFILE_LEN - 1);
    }
    a->assignment_count = count;
}

/* --- find_index tests ----------------------------------------------------- */

static void test_find_index_found(void **state)
{
    (void)state;
    cbx_assignments a;
    fill_assignments(&a, 3);
    assert_int_equal(cbx_assign_find_index(&a, "ORDER:1"), 1);
    assert_int_equal(cbx_assign_find_index(&a, "ORDER:0"), 0);
    assert_int_equal(cbx_assign_find_index(&a, "ORDER:2"), 2);
}

static void test_find_index_not_found(void **state)
{
    (void)state;
    cbx_assignments a;
    fill_assignments(&a, 3);
    assert_int_equal(cbx_assign_find_index(&a, "ORDER:99"), -1);
}

static void test_find_index_empty(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assign_find_index(&a, "ORDER:0"), -1);
}

static void test_find_index_null_a(void **state)
{
    (void)state;
    assert_int_equal(cbx_assign_find_index(NULL, "ORDER:0"), -1);
}

static void test_find_index_null_id(void **state)
{
    (void)state;
    cbx_assignments a;
    fill_assignments(&a, 2);
    assert_int_equal(cbx_assign_find_index(&a, NULL), -1);
}

static void test_find_index_multiple_entries(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_assignments_init(&a);
    strncpy(a.assignments[0].id, "BT:AB:CD:01:02:03:04", CBX_MAX_ID_LEN - 1);
    a.assignments[0].slot = 0;
    strncpy(a.assignments[1].id, "USB:SN12345", CBX_MAX_ID_LEN - 1);
    a.assignments[1].slot = 2;
    strncpy(a.assignments[2].id, "ORDER:0", CBX_MAX_ID_LEN - 1);
    a.assignments[2].slot = 1;
    a.assignment_count = 3;
    assert_int_equal(cbx_assign_find_index(&a, "USB:SN12345"), 1);
    assert_int_equal(cbx_assign_find_index(&a, "ORDER:0"), 2);
}

/* --- lookup tests --------------------------------------------------------- */

static void test_lookup_found(void **state)
{
    (void)state;
    cbx_assignments a;
    fill_assignments(&a, 3);
    cbx_assignment out;
    int rc = cbx_assign_lookup(&a, "ORDER:1", &out);
    assert_int_equal(rc, 0);
    assert_int_equal(out.slot, 1);
    assert_string_equal(out.profile, "default");
    assert_string_equal(out.id, "ORDER:1");
}

static void test_lookup_not_found(void **state)
{
    (void)state;
    cbx_assignments a;
    fill_assignments(&a, 2);
    cbx_assignment out;
    assert_int_equal(cbx_assign_lookup(&a, "ORDER:99", &out), -ENOENT);
}

static void test_lookup_null_args(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_assignment out;
    assert_int_equal(cbx_assign_lookup(NULL, "ORDER:0", &out), -EINVAL);
    assert_int_equal(cbx_assign_lookup(&a, NULL, &out), -EINVAL);
    assert_int_equal(cbx_assign_lookup(&a, "ORDER:0", NULL), -EINVAL);
}

/* --- slot_occupied tests -------------------------------------------------- */

static void test_slot_occupied_yes(void **state)
{
    (void)state;
    cbx_assignments a;
    fill_assignments(&a, 3);
    assert_true(cbx_assign_slot_occupied(&a, 0));
    assert_true(cbx_assign_slot_occupied(&a, 1));
    assert_true(cbx_assign_slot_occupied(&a, 2));
}

static void test_slot_occupied_no(void **state)
{
    (void)state;
    cbx_assignments a;
    fill_assignments(&a, 2);
    assert_false(cbx_assign_slot_occupied(&a, 2));
    assert_false(cbx_assign_slot_occupied(&a, 99));
}

static void test_slot_occupied_null(void **state)
{
    (void)state;
    assert_false(cbx_assign_slot_occupied(NULL, 0));
}

static void test_slot_occupied_negative(void **state)
{
    (void)state;
    cbx_assignments a;
    fill_assignments(&a, 2);
    assert_false(cbx_assign_slot_occupied(&a, -1));
}

static void test_slot_occupied_gaps(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_assignments_init(&a);
    /* Slots 0 and 2 occupied, 1 free */
    strncpy(a.assignments[0].id, "ORDER:0", CBX_MAX_ID_LEN - 1);
    a.assignments[0].slot = 0;
    strncpy(a.assignments[1].id, "ORDER:1", CBX_MAX_ID_LEN - 1);
    a.assignments[1].slot = 2;
    a.assignment_count = 2;
    assert_true(cbx_assign_slot_occupied(&a, 0));
    assert_false(cbx_assign_slot_occupied(&a, 1));
    assert_true(cbx_assign_slot_occupied(&a, 2));
}

/* --- lowest_free_slot tests ----------------------------------------------- */

static void test_lowest_free_empty(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assign_lowest_free_slot(&a, 4), 0);
}

static void test_lowest_free_partial(void **state)
{
    (void)state;
    cbx_assignments a;
    fill_assignments(&a, 2);
    assert_int_equal(cbx_assign_lowest_free_slot(&a, 4), 2);
}

static void test_lowest_free_with_gap(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_assignments_init(&a);
    /* Slots 0 and 2 occupied, 1 free */
    strncpy(a.assignments[0].id, "ORDER:0", CBX_MAX_ID_LEN - 1);
    a.assignments[0].slot = 0;
    strncpy(a.assignments[1].id, "ORDER:1", CBX_MAX_ID_LEN - 1);
    a.assignments[1].slot = 2;
    a.assignment_count = 2;
    assert_int_equal(cbx_assign_lowest_free_slot(&a, 4), 1);
}

static void test_lowest_free_all_filled(void **state)
{
    (void)state;
    cbx_assignments a;
    fill_assignments(&a, 4);
    assert_int_equal(cbx_assign_lowest_free_slot(&a, 4), -1);
}

static void test_lowest_free_more_than_max(void **state)
{
    (void)state;
    cbx_assignments a;
    fill_assignments(&a, 3);
    assert_int_equal(cbx_assign_lowest_free_slot(&a, 3), -1);
    assert_int_equal(cbx_assign_lowest_free_slot(&a, 4), 3);
}

static void test_lowest_free_null(void **state)
{
    (void)state;
    assert_int_equal(cbx_assign_lowest_free_slot(NULL, 4), -1);
}

static void test_lowest_free_zero_max(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assign_lowest_free_slot(&a, 0), -1);
}

static void test_lowest_free_negative_max(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_int_equal(cbx_assign_lowest_free_slot(&a, -1), -1);
}

/* --- make_default tests --------------------------------------------------- */

static void test_make_default_valid(void **state)
{
    (void)state;
    cbx_assignment out;
    int rc = cbx_assign_make_default("BT:AB:CD:01:02:03:04", 2, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out.id, "BT:AB:CD:01:02:03:04");
    assert_int_equal(out.slot, 2);
    assert_string_equal(out.profile, "default");
}

static void test_make_default_order_id(void **state)
{
    (void)state;
    cbx_assignment out;
    int rc = cbx_assign_make_default("ORDER:5", 0, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out.id, "ORDER:5");
    assert_int_equal(out.slot, 0);
    assert_string_equal(out.profile, "default");
}

static void test_make_default_invalid_id(void **state)
{
    (void)state;
    cbx_assignment out;
    assert_int_equal(cbx_assign_make_default("INVALID", 0, &out), -EINVAL);
    assert_int_equal(cbx_assign_make_default("", 0, &out), -EINVAL);
}

static void test_make_default_negative_slot(void **state)
{
    (void)state;
    cbx_assignment out;
    assert_int_equal(cbx_assign_make_default("ORDER:0", -1, &out), -EINVAL);
}

static void test_make_default_null(void **state)
{
    (void)state;
    cbx_assignment out;
    assert_int_equal(cbx_assign_make_default(NULL, 0, &out), -EINVAL);
    assert_int_equal(cbx_assign_make_default("ORDER:0", 0, NULL), -EINVAL);
}

/* --- resolve tests -------------------------------------------------------- */

static void test_resolve_existing(void **state)
{
    (void)state;
    cbx_assignments a;
    fill_assignments(&a, 3);
    cbx_assignment out;
    int rc = cbx_assign_resolve(&a, "ORDER:1", 4, &out);
    assert_int_equal(rc, 0); /* existing */
    assert_int_equal(out.slot, 1);
    assert_string_equal(out.profile, "default");
}

static void test_resolve_new_default(void **state)
{
    (void)state;
    cbx_assignments a;
    fill_assignments(&a, 2);
    cbx_assignment out;
    int rc = cbx_assign_resolve(&a, "ORDER:99", 4, &out);
    assert_int_equal(rc, 1); /* newly created default */
    assert_int_equal(out.slot, 2); /* lowest free */
    assert_string_equal(out.profile, "default");
}

static void test_resolve_new_empty_table(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_assignments_init(&a);
    cbx_assignment out;
    int rc = cbx_assign_resolve(&a, "ORDER:0", 4, &out);
    assert_int_equal(rc, 1);
    assert_int_equal(out.slot, 0);
}

static void test_resolve_all_slots_full(void **state)
{
    (void)state;
    cbx_assignments a;
    fill_assignments(&a, 4);
    cbx_assignment out;
    int rc = cbx_assign_resolve(&a, "ORDER:99", 4, &out);
    assert_int_equal(rc, -ENOENT);
}

static void test_resolve_with_gap(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_assignments_init(&a);
    strncpy(a.assignments[0].id, "ORDER:0", CBX_MAX_ID_LEN - 1);
    a.assignments[0].slot = 0;
    strncpy(a.assignments[1].id, "ORDER:1", CBX_MAX_ID_LEN - 1);
    a.assignments[1].slot = 2;
    a.assignment_count = 2;
    cbx_assignment out;
    int rc = cbx_assign_resolve(&a, "ORDER:99", 4, &out);
    assert_int_equal(rc, 1);
    assert_int_equal(out.slot, 1); /* gap at slot 1 */
}

static void test_resolve_null_args(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_assignment out;
    assert_int_equal(cbx_assign_resolve(NULL, "ORDER:0", 4, &out), -EINVAL);
    assert_int_equal(cbx_assign_resolve(&a, NULL, 4, &out), -EINVAL);
    assert_int_equal(cbx_assign_resolve(&a, "ORDER:0", 4, NULL), -EINVAL);
}

static void test_resolve_invalid_max_slots(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_assignments_init(&a);
    cbx_assignment out;
    assert_int_equal(cbx_assign_resolve(&a, "ORDER:0", 0, &out), -ENOENT);
    assert_int_equal(cbx_assign_resolve(&a, "ORDER:0", -1, &out), -ENOENT);
}

static void test_resolve_does_not_mutate(void **state)
{
    (void)state;
    cbx_assignments a;
    fill_assignments(&a, 2);
    cbx_assignment out;
    cbx_assign_resolve(&a, "ORDER:99", 4, &out);
    /* The input struct should not have been modified */
    assert_int_equal(a.assignment_count, 2);
}

/* --- Main ---------------------------------------------------------------- */

int main(void)
{
    const struct CMUnitTest tests[] = {
        /* find_index */
        cmocka_unit_test(test_find_index_found),
        cmocka_unit_test(test_find_index_not_found),
        cmocka_unit_test(test_find_index_empty),
        cmocka_unit_test(test_find_index_null_a),
        cmocka_unit_test(test_find_index_null_id),
        cmocka_unit_test(test_find_index_multiple_entries),

        /* lookup */
        cmocka_unit_test(test_lookup_found),
        cmocka_unit_test(test_lookup_not_found),
        cmocka_unit_test(test_lookup_null_args),

        /* slot_occupied */
        cmocka_unit_test(test_slot_occupied_yes),
        cmocka_unit_test(test_slot_occupied_no),
        cmocka_unit_test(test_slot_occupied_null),
        cmocka_unit_test(test_slot_occupied_negative),
        cmocka_unit_test(test_slot_occupied_gaps),

        /* lowest_free_slot */
        cmocka_unit_test(test_lowest_free_empty),
        cmocka_unit_test(test_lowest_free_partial),
        cmocka_unit_test(test_lowest_free_with_gap),
        cmocka_unit_test(test_lowest_free_all_filled),
        cmocka_unit_test(test_lowest_free_more_than_max),
        cmocka_unit_test(test_lowest_free_null),
        cmocka_unit_test(test_lowest_free_zero_max),
        cmocka_unit_test(test_lowest_free_negative_max),

        /* make_default */
        cmocka_unit_test(test_make_default_valid),
        cmocka_unit_test(test_make_default_order_id),
        cmocka_unit_test(test_make_default_invalid_id),
        cmocka_unit_test(test_make_default_negative_slot),
        cmocka_unit_test(test_make_default_null),

        /* resolve */
        cmocka_unit_test(test_resolve_existing),
        cmocka_unit_test(test_resolve_new_default),
        cmocka_unit_test(test_resolve_new_empty_table),
        cmocka_unit_test(test_resolve_all_slots_full),
        cmocka_unit_test(test_resolve_with_gap),
        cmocka_unit_test(test_resolve_null_args),
        cmocka_unit_test(test_resolve_invalid_max_slots),
        cmocka_unit_test(test_resolve_does_not_mutate),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}