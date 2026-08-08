/*
 * test_interaction_inventory.c — Task 7 enumeration test.
 *
 * Verifies that the interaction acceptance inventory:
 *   - compiles and is accessible via cbx_interaction_inventory_get()
 *   - has the expected number of entries (M01–M38, O01–O12, D01–D08)
 *   - every entry has non-NULL required fields (id, context, controller_path,
 *     semantic_outcome, dispatch_path, evidence_task)
 *   - the inventory covers all required control IDs
 *   - pointer-path-availability is correctly marked (n/a vs available)
 *   - lookup by ID works (find)
 *   - all three prefix families (M, O, D) are present
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include "interaction_inventory.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>

/* ---- Expected counts ---- */
#define EXPECTED_MANAGER_COUNT  38  /* M01–M38 */
#define EXPECTED_OVERLAY_COUNT  12  /* O01–O12 */
#define EXPECTED_DISABLED_COUNT   8  /* D01–D08 */
#define EXPECTED_TOTAL         (EXPECTED_MANAGER_COUNT + EXPECTED_OVERLAY_COUNT + EXPECTED_DISABLED_COUNT)

/* ---- Tests ---- */

static void test_inventory_count(void **state)
{
    (void)state;
    size_t count = cbx_interaction_inventory_count();
    assert_int_equal(count, EXPECTED_TOTAL);
}

static void test_inventory_all_fields_populated(void **state)
{
    (void)state;
    const cbx_interaction_entry *inv = cbx_interaction_inventory_get();
    for (size_t i = 0; inv[i].id != NULL; i++) {
        assert_non_null(inv[i].id);
        assert_non_null(inv[i].context);
        assert_non_null(inv[i].controller_path);
        assert_non_null(inv[i].pointer_path);
        assert_non_null(inv[i].semantic_outcome);
        assert_non_null(inv[i].dispatch_path);
        assert_non_null(inv[i].evidence_task);
    }
}

static void test_inventory_has_all_manager_controls(void **state)
{
    (void)state;
    /* M01 through M38 must all be present */
    for (int n = 1; n <= 38; n++) {
        char id[8];
        snprintf(id, sizeof(id), "M%02d", n);
        const cbx_interaction_entry *e = cbx_interaction_inventory_find(id);
        assert_non_null(e);
        assert_string_equal(e->id, id);
    }
}

static void test_inventory_has_all_overlay_actions(void **state)
{
    (void)state;
    /* O01 through O12 must all be present */
    for (int n = 1; n <= 12; n++) {
        char id[8];
        snprintf(id, sizeof(id), "O%02d", n);
        const cbx_interaction_entry *e = cbx_interaction_inventory_find(id);
        assert_non_null(e);
        assert_string_equal(e->id, id);
    }
}

static void test_inventory_has_all_disabled_scenarios(void **state)
{
    (void)state;
    /* D01 through D08 must all be present */
    for (int n = 1; n <= 8; n++) {
        char id[8];
        snprintf(id, sizeof(id), "D%02d", n);
        const cbx_interaction_entry *e = cbx_interaction_inventory_find(id);
        assert_non_null(e);
        assert_string_equal(e->id, id);
    }
}

static void test_inventory_find_returns_null_for_unknown(void **state)
{
    (void)state;
    assert_null(cbx_interaction_inventory_find("M99"));
    assert_null(cbx_interaction_inventory_find("X01"));
    assert_null(cbx_interaction_inventory_find(""));
    assert_null(cbx_interaction_inventory_find(NULL));
}

static void test_inventory_pointer_path_availability(void **state)
{
    (void)state;
    const cbx_interaction_entry *inv = cbx_interaction_inventory_get();

    /* Controller-only controls must have pointer_path == "n/a" and
       pointer_path_avail == CBX_PATH_NA */
    int na_count = 0;
    int avail_count = 0;
    for (size_t i = 0; inv[i].id != NULL; i++) {
        if (inv[i].pointer_path_avail == CBX_PATH_NA) {
            /* Verify the pointer path string starts with "n/a" */
            assert_true(strncmp(inv[i].pointer_path, "n/a", 3) == 0);
            na_count++;
        } else {
            /* Available pointer path must NOT start with "n/a" */
            assert_true(strncmp(inv[i].pointer_path, "n/a", 3) != 0);
            avail_count++;
        }
    }
    /* We expect at least some of each type */
    assert_true(na_count > 0);
    assert_true(avail_count > 0);
}

static void test_inventory_categories(void **state)
{
    (void)state;
    const cbx_interaction_entry *inv = cbx_interaction_inventory_get();

    int manager_count = 0;
    int overlay_count = 0;
    int disabled_count = 0;

    for (size_t i = 0; inv[i].id != NULL; i++) {
        const char c = inv[i].id[0];
        switch (c) {
        case 'M':
            assert_true(inv[i].category >= CBX_CAT_MANAGER_TABBAR &&
                        inv[i].category <= CBX_CAT_MANAGER_EDITOR);
            manager_count++;
            break;
        case 'O':
            assert_int_equal(inv[i].category, CBX_CAT_OVERLAY);
            overlay_count++;
            break;
        case 'D':
            assert_int_equal(inv[i].category, CBX_CAT_DISABLED);
            disabled_count++;
            break;
        default:
            fail_msg("Unexpected ID prefix: %s", inv[i].id);
        }
    }

    assert_int_equal(manager_count, EXPECTED_MANAGER_COUNT);
    assert_int_equal(overlay_count, EXPECTED_OVERLAY_COUNT);
    assert_int_equal(disabled_count, EXPECTED_DISABLED_COUNT);
}

static void test_inventory_specific_entries(void **state)
{
    (void)state;

    /* M05 — Add button: controller path mentions boundary → A */
    const cbx_interaction_entry *m05 = cbx_interaction_inventory_find("M05");
    assert_non_null(m05);
    assert_string_equal(m05->context, "Controllers tab");
    assert_int_equal(m05->widget_type, CBX_WIDGET_BUTTON);
    assert_true(m05->pointer_path_avail == CBX_PATH_AVAILABLE);

    /* M16 — Name input cancel: pointer path n/a */
    const cbx_interaction_entry *m16 = cbx_interaction_inventory_find("M16");
    assert_non_null(m16);
    assert_int_equal(m16->pointer_path_avail, CBX_PATH_NA);
    assert_true(strncmp(m16->pointer_path, "n/a", 3) == 0);

    /* M37 — Save and close editor: pointer path n/a (controller-only) */
    const cbx_interaction_entry *m37 = cbx_interaction_inventory_find("M37");
    assert_non_null(m37);
    assert_int_equal(m37->pointer_path_avail, CBX_PATH_NA);

    /* O12 — Host cycle profile: verification status deferred */
    const cbx_interaction_entry *o12 = cbx_interaction_inventory_find("O12");
    assert_non_null(o12);
    assert_int_equal(o12->verify_status, CBX_VERIFY_DEFERRED);

    /* O01 — Open: pointer path n/a (overlay is controller-driven) */
    const cbx_interaction_entry *o01 = cbx_interaction_inventory_find("O01");
    assert_non_null(o01);
    assert_int_equal(o01->pointer_path_avail, CBX_PATH_NA);

    /* D01 — InputPlumber unavailable */
    const cbx_interaction_entry *d01 = cbx_interaction_inventory_find("D01");
    assert_non_null(d01);
    assert_int_equal(d01->category, CBX_CAT_DISABLED);
    assert_int_equal(d01->widget_type, CBX_WIDGET_SCENARIO);

    /* D08 — Empty profile creation */
    const cbx_interaction_entry *d08 = cbx_interaction_inventory_find("D08");
    assert_non_null(d08);
    assert_int_equal(d08->category, CBX_CAT_DISABLED);
}

static void test_inventory_all_ids_unique(void **state)
{
    (void)state;
    const cbx_interaction_entry *inv = cbx_interaction_inventory_get();
    size_t count = cbx_interaction_inventory_count();
    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            assert_string_not_equal(inv[i].id, inv[j].id);
        }
    }
}

static void test_inventory_covers_required_scenarios(void **state)
{
    (void)state;
    /* SPEC §5.7 requires end-to-end coverage of specific scenarios.
     * Verify that the inventory includes entries for each required
     * scenario category. */
    bool has_create_source_picker = false;
    bool has_name_input_cancel = false;
    bool has_capture_mode = false;
    bool has_sequential_mode = false;
    bool has_save_and_close = false;
    bool has_cancel_editor = false;

    const cbx_interaction_entry *inv = cbx_interaction_inventory_get();
    for (size_t i = 0; inv[i].id != NULL; i++) {
        if (strstr(inv[i].semantic_outcome, "Create source picker opens"))
            has_create_source_picker = true;
        if (strcmp(inv[i].id, "M16") == 0)
            has_name_input_cancel = true;
        if (strcmp(inv[i].id, "M31") == 0)
            has_capture_mode = true;
        if (strcmp(inv[i].id, "M33") == 0)
            has_sequential_mode = true;
        if (strcmp(inv[i].id, "M37") == 0)
            has_save_and_close = true;
        if (strcmp(inv[i].id, "M38") == 0)
            has_cancel_editor = true;
    }

    assert_true(has_create_source_picker);
    assert_true(has_name_input_cancel);
    assert_true(has_capture_mode);
    assert_true(has_sequential_mode);
    assert_true(has_save_and_close);
    assert_true(has_cancel_editor);
}

/* ---- Test runner ---- */
int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_inventory_count),
        cmocka_unit_test(test_inventory_all_fields_populated),
        cmocka_unit_test(test_inventory_has_all_manager_controls),
        cmocka_unit_test(test_inventory_has_all_overlay_actions),
        cmocka_unit_test(test_inventory_has_all_disabled_scenarios),
        cmocka_unit_test(test_inventory_find_returns_null_for_unknown),
        cmocka_unit_test(test_inventory_pointer_path_availability),
        cmocka_unit_test(test_inventory_categories),
        cmocka_unit_test(test_inventory_specific_entries),
        cmocka_unit_test(test_inventory_all_ids_unique),
        cmocka_unit_test(test_inventory_covers_required_scenarios),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}