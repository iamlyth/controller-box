/*
 * test_interaction_inventory.c — Task 1 enumeration test.
 *
 * Verifies that the interaction acceptance inventory:
 *   - compiles and is accessible via cbx_interaction_inventory_get()
 *   - has the expected number of entries (M01–M39, O01–O13, D01–D08)
 *   - every entry has non-NULL required fields (id, context, controller_path,
 *     pointer_path, semantic_outcome, dispatch_path, evidence_task)
 *   - the inventory covers all required control IDs
 *   - pointer-path-availability is correctly marked (n/a vs available)
 *   - lookup by ID works (find)
 *   - all three prefix families (M, O, D) are present
 *   - verify_status is internally consistent (UNVERIFIED entries reference
 *     a task; VERIFIED entries with "—" are fully verified)
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
#define EXPECTED_MANAGER_COUNT  39  /* M01–M39 */
#define EXPECTED_OVERLAY_COUNT  13  /* O01–O13 */
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
    /* M01 through M39 must all be present */
    for (int n = 1; n <= 39; n++) {
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
    /* O01 through O13 must all be present */
    for (int n = 1; n <= 13; n++) {
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

    /* M16 — Name input cancel: pointer path not applicable (keyboard-only
     * action: B/ESC; no cancel button widget exists in name input mode) */
    const cbx_interaction_entry *m16 = cbx_interaction_inventory_find("M16");
    assert_non_null(m16);
    assert_int_equal(m16->pointer_path_avail, CBX_PATH_NA);
    assert_int_equal(m16->verify_status, CBX_VERIFY_NOT_APPLICABLE);

    /* M37 — Save and close editor: pointer path available (Save button) */
    const cbx_interaction_entry *m37 = cbx_interaction_inventory_find("M37");
    assert_non_null(m37);
    assert_int_equal(m37->pointer_path_avail, CBX_PATH_AVAILABLE);
    assert_int_equal(m37->verify_status, CBX_VERIFY_VERIFIED);

    /* M39 — First-run service install: both paths available (Yes/No buttons) */
    const cbx_interaction_entry *m39 = cbx_interaction_inventory_find("M39");
    assert_non_null(m39);
    assert_int_equal(m39->category, CBX_CAT_MANAGER_SETTINGS);
    assert_int_equal(m39->pointer_path_avail, CBX_PATH_AVAILABLE);
    assert_int_equal(m39->verify_status, CBX_VERIFY_VERIFIED);

    /* O12 — Host cycle profile: verification status deferred */
    const cbx_interaction_entry *o12 = cbx_interaction_inventory_find("O12");
    assert_non_null(o12);
    assert_int_equal(o12->verify_status, CBX_VERIFY_DEFERRED);

    /* O01 — Open: pointer path n/a (overlay is controller-driven) */
    const cbx_interaction_entry *o01 = cbx_interaction_inventory_find("O01");
    assert_non_null(o01);
    assert_int_equal(o01->pointer_path_avail, CBX_PATH_NA);

    /* O13 — Player Mode conflict: new entry, verified by Task 3 (test_overlay_native.c) */
    const cbx_interaction_entry *o13 = cbx_interaction_inventory_find("O13");
    assert_non_null(o13);
    assert_int_equal(o13->category, CBX_CAT_OVERLAY);
    assert_int_equal(o13->verify_status, CBX_VERIFY_VERIFIED);

    /* D01 — InputPlumber unavailable */
    const cbx_interaction_entry *d01 = cbx_interaction_inventory_find("D01");
    assert_non_null(d01);
    assert_int_equal(d01->category, CBX_CAT_DISABLED);
    assert_int_equal(d01->widget_type, CBX_WIDGET_SCENARIO);
    /* D01 has native evidence but pointer path pending Task 6 */
    assert_int_equal(d01->verify_status, CBX_VERIFY_VERIFIED);
    assert_int_equal(d01->pointer_path_avail, CBX_PATH_AVAILABLE);

    /* D08 — Empty profile creation: pointer path now available */
    const cbx_interaction_entry *d08 = cbx_interaction_inventory_find("D08");
    assert_non_null(d08);
    assert_int_equal(d08->category, CBX_CAT_DISABLED);
    assert_int_equal(d08->pointer_path_avail, CBX_PATH_AVAILABLE);
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
    bool has_player_mode_conflict = false;
    bool has_first_run_service_install = false;

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
        if (strcmp(inv[i].id, "O13") == 0)
            has_player_mode_conflict = true;
        if (strcmp(inv[i].id, "M39") == 0)
            has_first_run_service_install = true;
    }

    assert_true(has_create_source_picker);
    assert_true(has_name_input_cancel);
    assert_true(has_capture_mode);
    assert_true(has_sequential_mode);
    assert_true(has_save_and_close);
    assert_true(has_cancel_editor);
    assert_true(has_player_mode_conflict);
    assert_true(has_first_run_service_install);
}

/* Task 1: Verify that the inventory's verify_status is internally
 * consistent.  Entries marked UNVERIFIED, NOT_APPLICABLE, or DEFERRED
 * must reference a task ("Task N").  VERIFIED entries may have "—"
 * (fully verified) or "Task N" (partially verified, pending pointer path). */
static void test_inventory_verify_status_consistency(void **state)
{
    (void)state;
    const cbx_interaction_entry *inv = cbx_interaction_inventory_get();
    for (size_t i = 0; inv[i].id != NULL; i++) {
        switch (inv[i].verify_status) {
        case CBX_VERIFY_UNVERIFIED:
        case CBX_VERIFY_NOT_APPLICABLE:
        case CBX_VERIFY_DEFERRED:
            /* Must reference a task */
            if (strncmp(inv[i].evidence_task, "Task ", 5) != 0) {
                fail_msg("Entry %s verify_status=%d should reference a task, got '%s'",
                         inv[i].id, (int)inv[i].verify_status, inv[i].evidence_task);
            }
            break;
        case CBX_VERIFY_VERIFIED:
            /* May be "—" (fully verified) or "Task N" (partially verified) */
            break;
        }
    }
}

/* Task 11: The inventory ledger ties "verified" flags to actual test pass
 * status.  The static table's verify_status is a DECLARATION of intent —
 * not runtime truth.  A control is only considered verified after a
 * passing production-dispatch test calls
 * cbx_interaction_inventory_mark_verified().  Prove the ledger is runtime
 * driven and starts unseeded (no static claim is treated as verified). */
static void test_inventory_verified_flags_are_runtime_marks(void **state)
{
    (void)state;

    /* Reset the runtime ledger — it must start empty (unseeded). */
    cbx_interaction_inventory_reset();

    const char *declared_verified[] = {
        "M01", "M02", "M03", "M04", "M05", "M06", "M07", "M08",
        "M09", "M10", "M11", "M12", "M15", "M17", "M18",
        "M19", "M20", "M21", "M22", "M23", "M24", "M25", "M26",
        "M27", "M28", "M29", "M30", "M31", "M33", "M39",
        "O01", "O02", "O03", "O04", "O05", "O06", "O07", "O08",
        "O09", "O10", "O11", "O13",
        "D01", "D02", "D03", "D04", "D05", "D06", "D07", "D08"
    };
    for (size_t i = 0; i < sizeof(declared_verified)/sizeof(declared_verified[0]); i++) {
        /* The static table declares VERIFIED as intent ... */
        const cbx_interaction_entry *e = cbx_interaction_inventory_find(declared_verified[i]);
        assert_non_null(e);
        assert_int_equal(e->verify_status, CBX_VERIFY_VERIFIED);
        /* ... but the runtime ledger is NOT verified until a passing
         * dispatch test marks it.  A hardcoded claim must never read
         * back as verified. */
        assert_int_equal(cbx_interaction_inventory_is_verified(declared_verified[i]), 0);
    }

    /* A passing dispatch test marks its control; the ledger flips. */
    assert_int_equal(cbx_interaction_inventory_mark_verified("M30"), 0);
    assert_int_equal(cbx_interaction_inventory_is_verified("M30"), 1);

    /* A control that was not marked stays unverified. */
    assert_int_equal(cbx_interaction_inventory_is_verified("M13"), 0);

    /* Reset clears the runtime mark (proving it is a mark, not a claim). */
    cbx_interaction_inventory_reset();
    assert_int_equal(cbx_interaction_inventory_is_verified("M30"), 0);
}

/* Task 1: Verify that dialog/disabled entries now have pointer_path_avail
 * == AVAILABLE (per §5.1/§5.7). */
static void test_inventory_dialog_pointer_paths_available(void **state)
{
    (void)state;
    const char *avail_ids[] = {
        "M09", "M15", "M19", "M20", "M24", "M25", "M26", "M39",
        "D01", "D02", "D03", "D04", "D05", "D06", "D07", "D08"
    };
    for (size_t i = 0; i < sizeof(avail_ids)/sizeof(avail_ids[0]); i++) {
        const cbx_interaction_entry *e = cbx_interaction_inventory_find(avail_ids[i]);
        assert_non_null(e);
        assert_int_equal(e->pointer_path_avail, CBX_PATH_AVAILABLE);
        /* pointer_path string must NOT start with "n/a" */
        if (strncmp(e->pointer_path, "n/a", 3) == 0) {
            fail_msg("Entry %s has AVAILABLE path but string starts with n/a", e->id);
        }
    }
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
        /* Task 1: verify_status ledger checks */
        cmocka_unit_test(test_inventory_verify_status_consistency),
        cmocka_unit_test(test_inventory_dialog_pointer_paths_available),
        /* Task 11: verified flags are runtime marks, not static claims */
        cmocka_unit_test(test_inventory_verified_flags_are_runtime_marks),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}