/*
 * test_identity_downgrade.c — Unit tests for identity downgrade detection
 *                               (Task 27, SPEC §6.3).
 *
 * Tests:
 *   - cbx_downgrade_check: old vs new identity layer comparison.
 *   - cbx_downgrade_find_stronger: scan assignments for stronger IDs.
 *   - cbx_downgrade_resolve: high-level downgrade detection + fallback.
 *
 * No I/O — pure functions operating on in-memory structs.
 */
#include "identify/identity_downgrade.h"
#include "identify/identity.h"
#include "identify/assign.h"
#include "config/config_assignments.h"

#include <errno.h>
#include <stdarg.h>
#include <string.h>

#include <cmocka.h>

/* --- Helpers -------------------------------------------------------------- */

static cbx_identity
make_ident(const char *id, cbx_identity_layer layer)
{
    cbx_identity ident;
    cbx_identity_init(&ident);
    if (id) {
        strncpy(ident.id, id, sizeof(ident.id) - 1);
        ident.id[sizeof(ident.id) - 1] = '\0';
    }
    ident.layer = layer;
    return ident;
}

static cbx_assignment
make_assignment(const char *id, int slot, const char *profile)
{
    cbx_assignment a;
    memset(&a, 0, sizeof(a));
    strncpy(a.id, id, sizeof(a.id) - 1);
    a.slot = slot;
    if (profile)
        strncpy(a.profile, profile, sizeof(a.profile) - 1);
    return a;
}

static cbx_assignments
make_assignments(int count, ...)
{
    cbx_assignments a;
    cbx_assignments_init(&a);
    va_list ap;
    va_start(ap, count);
    for (int i = 0; i < count && i < CBX_MAX_ASSIGNMENTS; i++) {
        const char *id = va_arg(ap, const char *);
        int slot = va_arg(ap, int);
        const char *prof = va_arg(ap, const char *);
        a.assignments[i] = make_assignment(id, slot, prof);
        a.assignment_count++;
    }
    va_end(ap);
    return a;
}

/* --- cbx_downgrade_check tests ------------------------------------------- */

static void
test_check_no_downgrade_same_layer(void **state)
{
    (void)state;
    cbx_identity new_id = make_ident("USB:SN12345", CBX_IDENTITY_LAYER_USB_SERIAL);
    cbx_identity out;
    int rc = cbx_downgrade_check("USB:SN99999", &new_id, 5, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out.id, "USB:SN12345");
    assert_int_equal(out.layer, CBX_IDENTITY_LAYER_USB_SERIAL);
}

static void
test_check_no_downgrade_upgrade(void **state)
{
    (void)state;
    /* Old was USB:phys (layer 3), new is USB:SN (layer 2) — upgrade. */
    cbx_identity new_id = make_ident("USB:SN12345", CBX_IDENTITY_LAYER_USB_SERIAL);
    cbx_identity out;
    int rc = cbx_downgrade_check("USB:phys:usb-3-2", &new_id, 5, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out.id, "USB:SN12345");
}

static void
test_check_downgrade_bt_to_usb_serial(void **state)
{
    (void)state;
    /* Old was BT: (layer 1), new is USB:SN (layer 2) — downgrade. */
    cbx_identity new_id = make_ident("USB:SN12345", CBX_IDENTITY_LAYER_USB_SERIAL);
    cbx_identity out;
    int rc = cbx_downgrade_check("BT:AB:CD:01:02:03:04", &new_id, 5, &out);
    assert_int_equal(rc, 1);
    assert_string_equal(out.id, "ORDER:5");
    assert_int_equal(out.layer, CBX_IDENTITY_LAYER_ORDER);
}

static void
test_check_downgrade_usb_serial_to_phys(void **state)
{
    (void)state;
    /* Old was USB:SN (layer 2), new is USB:phys (layer 3) — downgrade. */
    cbx_identity new_id = make_ident("USB:phys:usb-3-2", CBX_IDENTITY_LAYER_USB_PORT);
    cbx_identity out;
    int rc = cbx_downgrade_check("USB:SN12345", &new_id, 3, &out);
    assert_int_equal(rc, 1);
    assert_string_equal(out.id, "ORDER:3");
    assert_int_equal(out.layer, CBX_IDENTITY_LAYER_ORDER);
}

static void
test_check_downgrade_phys_to_order(void **state)
{
    (void)state;
    /* Old was USB:phys (layer 3), new is ORDER (layer 4) — downgrade. */
    cbx_identity new_id = make_ident("ORDER:2", CBX_IDENTITY_LAYER_ORDER);
    cbx_identity out;
    int rc = cbx_downgrade_check("USB:phys:usb-3-2", &new_id, 2, &out);
    assert_int_equal(rc, 1);
    assert_string_equal(out.id, "ORDER:2");
    assert_int_equal(out.layer, CBX_IDENTITY_LAYER_ORDER);
}

static void
test_check_downgrade_bt_to_order(void **state)
{
    (void)state;
    /* Old was BT (layer 1), new is ORDER (layer 4) — downgrade. */
    cbx_identity new_id = make_ident("ORDER:7", CBX_IDENTITY_LAYER_ORDER);
    cbx_identity out;
    int rc = cbx_downgrade_check("BT:AB:CD:01:02:03:04", &new_id, 7, &out);
    assert_int_equal(rc, 1);
    assert_string_equal(out.id, "ORDER:7");
}

static void
test_check_null_old_id(void **state)
{
    (void)state;
    cbx_identity new_id = make_ident("USB:SN12345", CBX_IDENTITY_LAYER_USB_SERIAL);
    cbx_identity out;
    int rc = cbx_downgrade_check(NULL, &new_id, 5, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out.id, "USB:SN12345");
}

static void
test_check_empty_old_id(void **state)
{
    (void)state;
    cbx_identity new_id = make_ident("USB:SN12345", CBX_IDENTITY_LAYER_USB_SERIAL);
    cbx_identity out;
    int rc = cbx_downgrade_check("", &new_id, 5, &out);
    assert_int_equal(rc, 0);
}

static void
test_check_invalid_old_id(void **state)
{
    (void)state;
    /* Old ID doesn't parse to a valid layer — no downgrade. */
    cbx_identity new_id = make_ident("USB:SN12345", CBX_IDENTITY_LAYER_USB_SERIAL);
    cbx_identity out;
    int rc = cbx_downgrade_check("INVALID_FORMAT", &new_id, 5, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out.id, "USB:SN12345");
}

static void
test_check_new_identity_none(void **state)
{
    (void)state;
    cbx_identity new_id = make_ident("", CBX_IDENTITY_LAYER_NONE);
    cbx_identity out;
    int rc = cbx_downgrade_check("USB:SN12345", &new_id, 5, &out);
    assert_int_equal(rc, -ENOENT);
}

static void
test_check_null_args(void **state)
{
    (void)state;
    cbx_identity new_id = make_ident("USB:SN12345", CBX_IDENTITY_LAYER_USB_SERIAL);
    cbx_identity out;
    assert_int_equal(cbx_downgrade_check("USB:SN12345", NULL, 5, &out), -EINVAL);
    assert_int_equal(cbx_downgrade_check("USB:SN12345", &new_id, 5, NULL), -EINVAL);
}

static void
test_check_downgrade_negative_order(void **state)
{
    (void)state;
    /* Downgrade detected but connection_order < 0 — empty fallback ID. */
    cbx_identity new_id = make_ident("USB:phys:usb-3-2", CBX_IDENTITY_LAYER_USB_PORT);
    cbx_identity out;
    int rc = cbx_downgrade_check("USB:SN12345", &new_id, -1, &out);
    assert_int_equal(rc, 1);
    assert_string_equal(out.id, "");
    assert_int_equal(out.layer, CBX_IDENTITY_LAYER_ORDER);
}

/* --- cbx_downgrade_find_stronger tests ----------------------------------- */

static void
test_find_stronger_basic(void **state)
{
    (void)state;
    cbx_assignments a = make_assignments(2,
        "BT:AB:CD:01:02:03:04", 0, "fighting",
        "ORDER:1", 1, "default");
    char out_id[CBX_IDENTITY_MAX_LEN];
    bool found = cbx_downgrade_find_stronger(&a, CBX_IDENTITY_LAYER_USB_PORT,
                                              out_id, sizeof(out_id));
    assert_true(found);
    assert_string_equal(out_id, "BT:AB:CD:01:02:03:04");
}

static void
test_find_stronger_picks_strongest(void **state)
{
    (void)state;
    /* Both BT (layer 1) and USB:SN (layer 2) are stronger than USB:phys (3).
     * Should return the strongest: BT (layer 1). */
    cbx_assignments a = make_assignments(2,
        "USB:SN12345", 0, "default",
        "BT:AB:CD:01:02:03:04", 1, "fighting");
    char out_id[CBX_IDENTITY_MAX_LEN];
    bool found = cbx_downgrade_find_stronger(&a, CBX_IDENTITY_LAYER_USB_PORT,
                                              out_id, sizeof(out_id));
    assert_true(found);
    assert_string_equal(out_id, "BT:AB:CD:01:02:03:04");
}

static void
test_find_stronger_none_found(void **state)
{
    (void)state;
    /* All assignments are at layer 4 (ORDER) — nothing stronger than layer 3. */
    cbx_assignments a = make_assignments(1,
        "ORDER:0", 0, "default");
    char out_id[CBX_IDENTITY_MAX_LEN];
    bool found = cbx_downgrade_find_stronger(&a, CBX_IDENTITY_LAYER_USB_PORT,
                                              out_id, sizeof(out_id));
    assert_false(found);
}

static void
test_find_stronger_same_layer_not_stronger(void **state)
{
    (void)state;
    /* Assignment at layer 2, new layer 2 — not stronger. */
    cbx_assignments a = make_assignments(1,
        "USB:SN12345", 0, "default");
    char out_id[CBX_IDENTITY_MAX_LEN];
    bool found = cbx_downgrade_find_stronger(&a, CBX_IDENTITY_LAYER_USB_SERIAL,
                                              out_id, sizeof(out_id));
    assert_false(found);
}

static void
test_find_stronger_empty_assignments(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_assignments_init(&a);
    char out_id[CBX_IDENTITY_MAX_LEN];
    bool found = cbx_downgrade_find_stronger(&a, CBX_IDENTITY_LAYER_USB_PORT,
                                              out_id, sizeof(out_id));
    assert_false(found);
}

static void
test_find_stronger_null_args(void **state)
{
    (void)state;
    char out_id[CBX_IDENTITY_MAX_LEN];
    assert_false(cbx_downgrade_find_stronger(NULL, CBX_IDENTITY_LAYER_USB_PORT,
                                               out_id, sizeof(out_id)));
    cbx_assignments a;
    cbx_assignments_init(&a);
    assert_false(cbx_downgrade_find_stronger(&a, CBX_IDENTITY_LAYER_USB_PORT,
                                               NULL, sizeof(out_id)));
    assert_false(cbx_downgrade_find_stronger(&a, CBX_IDENTITY_LAYER_USB_PORT,
                                               out_id, 0));
}

static void
test_find_stronger_none_layer(void **state)
{
    (void)state;
    cbx_assignments a = make_assignments(1,
        "USB:SN12345", 0, "default");
    char out_id[CBX_IDENTITY_MAX_LEN];
    assert_false(cbx_downgrade_find_stronger(&a, CBX_IDENTITY_LAYER_NONE,
                                               out_id, sizeof(out_id)));
}

static void
test_find_stronger_invalid_ids_skipped(void **state)
{
    (void)state;
    /* Invalid IDs should be skipped during the scan. */
    cbx_assignments a = make_assignments(2,
        "INVALID_FORMAT", 0, "default",
        "USB:SN12345", 1, "default");
    char out_id[CBX_IDENTITY_MAX_LEN];
    /* The valid USB:SN (layer 2) should be found as stronger than layer 3. */
    bool found = cbx_downgrade_find_stronger(&a, CBX_IDENTITY_LAYER_USB_PORT,
                                              out_id, sizeof(out_id));
    assert_true(found);
    assert_string_equal(out_id, "USB:SN12345");
}

/* --- cbx_downgrade_resolve tests ----------------------------------------- */

static void
test_resolve_no_downgrade_matching_id(void **state)
{
    (void)state;
    /* New ID matches existing assignment — no downgrade. */
    cbx_assignments a = make_assignments(1,
        "USB:SN12345", 0, "fighting");
    cbx_identity new_id = make_ident("USB:SN12345", CBX_IDENTITY_LAYER_USB_SERIAL);
    cbx_identity out;
    int rc = cbx_downgrade_resolve(&a, &new_id, 5, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out.id, "USB:SN12345");
    assert_int_equal(out.layer, CBX_IDENTITY_LAYER_USB_SERIAL);
}

static void
test_resolve_downgrade_stronger_exists(void **state)
{
    (void)state;
    /* New ID (USB:phys, layer 3) doesn't match any assignment.
     * Existing assignment at USB:SN (layer 2) is stronger → downgrade. */
    cbx_assignments a = make_assignments(1,
        "USB:SN12345", 0, "fighting");
    cbx_identity new_id = make_ident("USB:phys:usb-3-2", CBX_IDENTITY_LAYER_USB_PORT);
    cbx_identity out;
    int rc = cbx_downgrade_resolve(&a, &new_id, 3, &out);
    assert_int_equal(rc, 1);
    assert_string_equal(out.id, "ORDER:3");
    assert_int_equal(out.layer, CBX_IDENTITY_LAYER_ORDER);
}

static void
test_resolve_no_downgrade_no_stronger(void **state)
{
    (void)state;
    /* New ID (USB:phys, layer 3) doesn't match.
     * Existing assignment at ORDER (layer 4) is not stronger → no downgrade. */
    cbx_assignments a = make_assignments(1,
        "ORDER:0", 0, "default");
    cbx_identity new_id = make_ident("USB:phys:usb-3-2", CBX_IDENTITY_LAYER_USB_PORT);
    cbx_identity out;
    int rc = cbx_downgrade_resolve(&a, &new_id, 3, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out.id, "USB:phys:usb-3-2");
}

static void
test_resolve_downgrade_bt_stronger(void **state)
{
    (void)state;
    /* New ID (USB:SN, layer 2) doesn't match.
     * Existing BT (layer 1) is stronger → downgrade. */
    cbx_assignments a = make_assignments(1,
        "BT:AB:CD:01:02:03:04", 0, "fighting");
    cbx_identity new_id = make_ident("USB:SN99999", CBX_IDENTITY_LAYER_USB_SERIAL);
    cbx_identity out;
    int rc = cbx_downgrade_resolve(&a, &new_id, 5, &out);
    assert_int_equal(rc, 1);
    assert_string_equal(out.id, "ORDER:5");
}

static void
test_resolve_no_downgrade_empty_assignments(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_assignments_init(&a);
    cbx_identity new_id = make_ident("USB:SN12345", CBX_IDENTITY_LAYER_USB_SERIAL);
    cbx_identity out;
    int rc = cbx_downgrade_resolve(&a, &new_id, 5, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out.id, "USB:SN12345");
}

static void
test_resolve_null_assignments(void **state)
{
    (void)state;
    cbx_identity new_id = make_ident("USB:SN12345", CBX_IDENTITY_LAYER_USB_SERIAL);
    cbx_identity out;
    int rc = cbx_downgrade_resolve(NULL, &new_id, 5, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out.id, "USB:SN12345");
}

static void
test_resolve_new_identity_none(void **state)
{
    (void)state;
    cbx_assignments a = make_assignments(1,
        "USB:SN12345", 0, "default");
    cbx_identity new_id = make_ident("", CBX_IDENTITY_LAYER_NONE);
    cbx_identity out;
    int rc = cbx_downgrade_resolve(&a, &new_id, 5, &out);
    assert_int_equal(rc, -ENOENT);
}

static void
test_resolve_null_args(void **state)
{
    (void)state;
    cbx_identity new_id = make_ident("USB:SN12345", CBX_IDENTITY_LAYER_USB_SERIAL);
    cbx_identity out;
    assert_int_equal(cbx_downgrade_resolve(NULL, NULL, 5, &out), -EINVAL);
    assert_int_equal(cbx_downgrade_resolve(NULL, &new_id, 5, NULL), -EINVAL);
}

static void
test_resolve_downgrade_negative_order(void **state)
{
    (void)state;
    cbx_assignments a = make_assignments(1,
        "USB:SN12345", 0, "fighting");
    cbx_identity new_id = make_ident("USB:phys:usb-3-2", CBX_IDENTITY_LAYER_USB_PORT);
    cbx_identity out;
    int rc = cbx_downgrade_resolve(&a, &new_id, -1, &out);
    assert_int_equal(rc, 1);
    assert_string_equal(out.id, "");
    assert_int_equal(out.layer, CBX_IDENTITY_LAYER_ORDER);
}

static void
test_resolve_multiple_stronger(void **state)
{
    (void)state;
    /* Multiple stronger assignments — should still detect downgrade. */
    cbx_assignments a = make_assignments(3,
        "BT:AB:CD:01:02:03:04", 0, "fighting",
        "USB:SN12345", 1, "default",
        "USB:phys:usb-1-1", 2, "default");
    cbx_identity new_id = make_ident("ORDER:3", CBX_IDENTITY_LAYER_ORDER);
    cbx_identity out;
    int rc = cbx_downgrade_resolve(&a, &new_id, 3, &out);
    assert_int_equal(rc, 1);
    assert_string_equal(out.id, "ORDER:3");
}

static void
test_resolve_id_matches_no_downgrade(void **state)
{
    (void)state;
    /* Even with stronger assignments, if the new ID matches, no downgrade. */
    cbx_assignments a = make_assignments(2,
        "BT:AB:CD:01:02:03:04", 0, "fighting",
        "ORDER:3", 1, "default");
    cbx_identity new_id = make_ident("ORDER:3", CBX_IDENTITY_LAYER_ORDER);
    cbx_identity out;
    int rc = cbx_downgrade_resolve(&a, &new_id, 3, &out);
    assert_int_equal(rc, 0);
    assert_string_equal(out.id, "ORDER:3");
}

/* --- Integration tests ---------------------------------------------------- */

static void
test_integration_downgrade_flow(void **state)
{
    (void)state;
    /* Simulate full downgrade scenario:
     * 1. Controller was stored as USB:SN12345 (layer 2, slot 0)
     * 2. Reconnects with USB:phys:usb-3-2 (layer 3) — different ID
     * 3. resolve detects downgrade → ORDER:5 fallback
     * 4. ORDER:5 can be auto-assigned to a slot */
    cbx_assignments a = make_assignments(1,
        "USB:SN12345", 0, "fighting");

    cbx_source_props props = {
        .iface = CBX_SOURCE_IFACE_EVDEV,
        .unique_id = "",
        .phys_path = "usb-3-2",
        .serial_number = "",
        .id_bustype = "3",
    };
    cbx_identity new_id;
    int rc = cbx_identity_extract(&props, 5, &new_id);
    assert_int_equal(rc, 0);
    assert_string_equal(new_id.id, "USB:phys:usb-3-2");
    assert_int_equal(new_id.layer, CBX_IDENTITY_LAYER_USB_PORT);

    cbx_identity resolved;
    rc = cbx_downgrade_resolve(&a, &new_id, 5, &resolved);
    assert_int_equal(rc, 1);
    assert_string_equal(resolved.id, "ORDER:5");
    assert_int_equal(resolved.layer, CBX_IDENTITY_LAYER_ORDER);

    /* The ORDER:5 identity can now be resolved to a default assignment. */
    cbx_assignment assign;
    rc = cbx_assign_resolve(&a, resolved.id, 4, &assign);
    assert_int_equal(rc, 1);   /* New default created. */
    assert_int_equal(assign.slot, 1);   /* Slot 0 is taken by USB:SN12345. */
    assert_string_equal(assign.profile, "default");
}

static void
test_integration_no_downgrade_new_controller(void **state)
{
    (void)state;
    /* Brand new controller with USB:SN (layer 2) — no existing assignments. */
    cbx_assignments a;
    cbx_assignments_init(&a);

    cbx_identity new_id = make_ident("USB:SN99999", CBX_IDENTITY_LAYER_USB_SERIAL);
    cbx_identity resolved;
    int rc = cbx_downgrade_resolve(&a, &new_id, 0, &resolved);
    assert_int_equal(rc, 0);
    assert_string_equal(resolved.id, "USB:SN99999");

    /* Can be auto-assigned normally. */
    cbx_assignment assign;
    rc = cbx_assign_resolve(&a, resolved.id, 4, &assign);
    assert_int_equal(rc, 1);
    assert_int_equal(assign.slot, 0);
}

/* --- Main ---------------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* cbx_downgrade_check */
        cmocka_unit_test(test_check_no_downgrade_same_layer),
        cmocka_unit_test(test_check_no_downgrade_upgrade),
        cmocka_unit_test(test_check_downgrade_bt_to_usb_serial),
        cmocka_unit_test(test_check_downgrade_usb_serial_to_phys),
        cmocka_unit_test(test_check_downgrade_phys_to_order),
        cmocka_unit_test(test_check_downgrade_bt_to_order),
        cmocka_unit_test(test_check_null_old_id),
        cmocka_unit_test(test_check_empty_old_id),
        cmocka_unit_test(test_check_invalid_old_id),
        cmocka_unit_test(test_check_new_identity_none),
        cmocka_unit_test(test_check_null_args),
        cmocka_unit_test(test_check_downgrade_negative_order),
        /* cbx_downgrade_find_stronger */
        cmocka_unit_test(test_find_stronger_basic),
        cmocka_unit_test(test_find_stronger_picks_strongest),
        cmocka_unit_test(test_find_stronger_none_found),
        cmocka_unit_test(test_find_stronger_same_layer_not_stronger),
        cmocka_unit_test(test_find_stronger_empty_assignments),
        cmocka_unit_test(test_find_stronger_null_args),
        cmocka_unit_test(test_find_stronger_none_layer),
        cmocka_unit_test(test_find_stronger_invalid_ids_skipped),
        /* cbx_downgrade_resolve */
        cmocka_unit_test(test_resolve_no_downgrade_matching_id),
        cmocka_unit_test(test_resolve_downgrade_stronger_exists),
        cmocka_unit_test(test_resolve_no_downgrade_no_stronger),
        cmocka_unit_test(test_resolve_downgrade_bt_stronger),
        cmocka_unit_test(test_resolve_no_downgrade_empty_assignments),
        cmocka_unit_test(test_resolve_null_assignments),
        cmocka_unit_test(test_resolve_new_identity_none),
        cmocka_unit_test(test_resolve_null_args),
        cmocka_unit_test(test_resolve_downgrade_negative_order),
        cmocka_unit_test(test_resolve_multiple_stronger),
        cmocka_unit_test(test_resolve_id_matches_no_downgrade),
        /* Integration */
        cmocka_unit_test(test_integration_downgrade_flow),
        cmocka_unit_test(test_integration_no_downgrade_new_controller),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}