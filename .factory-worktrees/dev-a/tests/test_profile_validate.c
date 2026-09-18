/*
 * test_profile_validate.c — Tests for NES minimum profile validation
 * (Task 38).
 *
 * Tests: NES minimum button table, has_binding, validate_nes_minimum,
 * missing_count, edge cases (NULL, empty profile, partial, complete).
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <string.h>
#include <errno.h>

#include "manager/profile_validate.h"
#include "manager/profile_diagram.h"
#include "config/config_profile.h"

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/*
 * Create a profile with the given buttons bound (each as a gamepad
 * source-event button property mapping to keyboard:KeyA).
 */
static cbx_profile
make_profile_with_buttons(const char **buttons, int count)
{
    cbx_profile p;
    cbx_profile_init(&p);
    strncpy(p.name, "Test", sizeof(p.name) - 1);

    for (int i = 0; i < count && i < CBX_MAX_MAPPINGS; i++) {
        cbx_profile_mapping *m = &p.mappings[p.mapping_count];
        memset(m, 0, sizeof(*m));
        strncpy(m->name, buttons[i], sizeof(m->name) - 1);
        strncpy(m->source_event.device_class, "gamepad",
                 sizeof(m->source_event.device_class) - 1);
        m->source_event.prop_count = 1;
        strncpy(m->source_event.props[0].key, "button",
                 sizeof(m->source_event.props[0].key) - 1);
        strncpy(m->source_event.props[0].value, buttons[i],
                 sizeof(m->source_event.props[0].value) - 1);
        m->target_event_count = 1;
        strncpy(m->target_events[0].device_class, "keyboard",
                 sizeof(m->target_events[0].device_class) - 1);
        strncpy(m->target_events[0].value, "KeyA",
                 sizeof(m->target_events[0].value) - 1);
        p.mapping_count++;
    }

    return p;
}

/* ------------------------------------------------------------------ */
/*  Tests: NES minimum button table                                     */
/* ------------------------------------------------------------------ */

static void test_nes_minimum_count(void **state)
{
    (void)state;
    const cbx_diag_button *btns = cbx_nes_minimum_buttons();
    assert_non_null(btns);
    /* Count should be 6: A, B, Up, Down, Left, Right */
    assert_int_equal(CBX_NES_MINIMUM_COUNT, 6);
}

static void test_nes_minimum_button_names(void **state)
{
    (void)state;
    /* Each required button should have a valid name */
    for (int i = 0; i < CBX_NES_MINIMUM_COUNT; i++) {
        const char *name = cbx_nes_minimum_button_name(i);
        assert_non_null(name);
        assert_true(strlen(name) > 0);
    }
}

static void test_nes_minimum_button_name_bad_index(void **state)
{
    (void)state;
    assert_null(cbx_nes_minimum_button_name(-1));
    assert_null(cbx_nes_minimum_button_name(CBX_NES_MINIMUM_COUNT));
}

static void test_nes_minimum_contains_a_b_dpad(void **state)
{
    (void)state;
    /* Check that the required set includes A, B, Up, Down, Left, Right */
    const cbx_diag_button *btns = cbx_nes_minimum_buttons();
    bool found_a = false, found_b = false;
    bool found_up = false, found_down = false, found_left = false, found_right = false;

    for (int i = 0; i < CBX_NES_MINIMUM_COUNT; i++) {
        if (btns[i] == CBX_DIAG_BTN_A) found_a = true;
        if (btns[i] == CBX_DIAG_BTN_B) found_b = true;
        if (btns[i] == CBX_DIAG_BTN_UP) found_up = true;
        if (btns[i] == CBX_DIAG_BTN_DOWN) found_down = true;
        if (btns[i] == CBX_DIAG_BTN_LEFT) found_left = true;
        if (btns[i] == CBX_DIAG_BTN_RIGHT) found_right = true;
    }

    assert_true(found_a);
    assert_true(found_b);
    assert_true(found_up);
    assert_true(found_down);
    assert_true(found_left);
    assert_true(found_right);
}

/* ------------------------------------------------------------------ */
/*  Tests: has_binding                                                 */
/* ------------------------------------------------------------------ */

static void test_has_binding_yes(void **state)
{
    (void)state;
    const char *btns[] = {"A"};
    cbx_profile p = make_profile_with_buttons(btns, 1);
    assert_true(cbx_profile_has_binding(&p, CBX_DIAG_BTN_A));
}

static void test_has_binding_no(void **state)
{
    (void)state;
    const char *btns[] = {"A"};
    cbx_profile p = make_profile_with_buttons(btns, 1);
    assert_false(cbx_profile_has_binding(&p, CBX_DIAG_BTN_B));
}

static void test_has_binding_empty_profile(void **state)
{
    (void)state;
    cbx_profile p;
    cbx_profile_init(&p);
    assert_false(cbx_profile_has_binding(&p, CBX_DIAG_BTN_A));
}

static void test_has_binding_null_profile(void **state)
{
    (void)state;
    assert_false(cbx_profile_has_binding(NULL, CBX_DIAG_BTN_A));
}

static void test_has_binding_none_button(void **state)
{
    (void)state;
    const char *btns[] = {"A"};
    cbx_profile p = make_profile_with_buttons(btns, 1);
    assert_false(cbx_profile_has_binding(&p, CBX_DIAG_BTN_NONE));
}

static void test_has_binding_dpad(void **state)
{
    (void)state;
    const char *btns[] = {"Up", "Down", "Left", "Right"};
    cbx_profile p = make_profile_with_buttons(btns, 4);
    assert_true(cbx_profile_has_binding(&p, CBX_DIAG_BTN_UP));
    assert_true(cbx_profile_has_binding(&p, CBX_DIAG_BTN_DOWN));
    assert_true(cbx_profile_has_binding(&p, CBX_DIAG_BTN_LEFT));
    assert_true(cbx_profile_has_binding(&p, CBX_DIAG_BTN_RIGHT));
}

static void test_has_binding_axis_prop(void **state)
{
    (void)state;
    /* A mapping with "axis" prop instead of "button" should also match */
    cbx_profile p;
    cbx_profile_init(&p);
    cbx_profile_mapping *m = &p.mappings[p.mapping_count];
    memset(m, 0, sizeof(*m));
    strncpy(m->source_event.device_class, "gamepad",
             sizeof(m->source_event.device_class) - 1);
    m->source_event.prop_count = 1;
    strncpy(m->source_event.props[0].key, "axis",
             sizeof(m->source_event.props[0].key) - 1);
    strncpy(m->source_event.props[0].value, "A",
             sizeof(m->source_event.props[0].value) - 1);
    p.mapping_count++;

    assert_true(cbx_profile_has_binding(&p, CBX_DIAG_BTN_A));
}

/* ------------------------------------------------------------------ */
/*  Tests: validate_nes_minimum                                        */
/* ------------------------------------------------------------------ */

static void test_validate_complete(void **state)
{
    (void)state;
    const char *btns[] = {"A", "B", "Up", "Down", "Left", "Right"};
    cbx_profile p = make_profile_with_buttons(btns, 6);
    int rc = cbx_profile_validate_nes_minimum(&p, NULL, 0);
    assert_int_equal(rc, 0);
}

static void test_validate_complete_with_extras(void **state)
{
    (void)state;
    const char *btns[] = {"A", "B", "Up", "Down", "Left", "Right",
                           "Start", "Select", "L1", "R1"};
    cbx_profile p = make_profile_with_buttons(btns, 10);
    int rc = cbx_profile_validate_nes_minimum(&p, NULL, 0);
    assert_int_equal(rc, 0);
}

static void test_validate_missing_a(void **state)
{
    (void)state;
    const char *btns[] = {"B", "Up", "Down", "Left", "Right"};
    cbx_profile p = make_profile_with_buttons(btns, 5);
    char missing[256] = "";
    int rc = cbx_profile_validate_nes_minimum(&p, missing, sizeof(missing));
    assert_int_equal(rc, -EINVAL);
    assert_true(strlen(missing) > 0);
    assert_non_null(strstr(missing, "A"));
}

static void test_validate_missing_multiple(void **state)
{
    (void)state;
    const char *btns[] = {"Up", "Down"};
    cbx_profile p = make_profile_with_buttons(btns, 2);
    char missing[256] = "";
    int rc = cbx_profile_validate_nes_minimum(&p, missing, sizeof(missing));
    assert_int_equal(rc, -EINVAL);
    /* Should mention A, B, Left, Right */
    assert_non_null(strstr(missing, "A"));
    assert_non_null(strstr(missing, "B"));
    assert_non_null(strstr(missing, "Left"));
    assert_non_null(strstr(missing, "Right"));
}

static void test_validate_empty_profile(void **state)
{
    (void)state;
    cbx_profile p;
    cbx_profile_init(&p);
    char missing[256] = "";
    int rc = cbx_profile_validate_nes_minimum(&p, missing, sizeof(missing));
    assert_int_equal(rc, -EINVAL);
    /* All 6 should be missing */
    assert_non_null(strstr(missing, "A"));
    assert_non_null(strstr(missing, "B"));
    assert_non_null(strstr(missing, "Up"));
    assert_non_null(strstr(missing, "Down"));
    assert_non_null(strstr(missing, "Left"));
    assert_non_null(strstr(missing, "Right"));
}

static void test_validate_null_profile(void **state)
{
    (void)state;
    int rc = cbx_profile_validate_nes_minimum(NULL, NULL, 0);
    assert_int_equal(rc, -EINVAL);
}

static void test_validate_no_missing_buf(void **state)
{
    (void)state;
    const char *btns[] = {"B", "Up", "Down", "Left", "Right"};
    cbx_profile p = make_profile_with_buttons(btns, 5);
    /* Should still return -EINVAL even without a buffer */
    int rc = cbx_profile_validate_nes_minimum(&p, NULL, 0);
    assert_int_equal(rc, -EINVAL);
}

/* ------------------------------------------------------------------ */
/*  Tests: missing_count                                                */
/* ------------------------------------------------------------------ */

static void test_missing_count_zero(void **state)
{
    (void)state;
    const char *btns[] = {"A", "B", "Up", "Down", "Left", "Right"};
    cbx_profile p = make_profile_with_buttons(btns, 6);
    int count = cbx_profile_validate_missing_count(&p);
    assert_int_equal(count, 0);
}

static void test_missing_count_some(void **state)
{
    (void)state;
    const char *btns[] = {"A", "B"};
    cbx_profile p = make_profile_with_buttons(btns, 2);
    int count = cbx_profile_validate_missing_count(&p);
    assert_int_equal(count, 4);  /* missing Up, Down, Left, Right */
}

static void test_missing_count_all(void **state)
{
    (void)state;
    cbx_profile p;
    cbx_profile_init(&p);
    int count = cbx_profile_validate_missing_count(&p);
    assert_int_equal(count, 6);
}

static void test_missing_count_null(void **state)
{
    (void)state;
    int count = cbx_profile_validate_missing_count(NULL);
    assert_int_equal(count, -EINVAL);
}

/* ------------------------------------------------------------------ */
/*  Test runner                                                        */
/* ------------------------------------------------------------------ */

int main(void)
{
    const struct CMUnitTest tests[] = {
        /* NES minimum button table */
        cmocka_unit_test(test_nes_minimum_count),
        cmocka_unit_test(test_nes_minimum_button_names),
        cmocka_unit_test(test_nes_minimum_button_name_bad_index),
        cmocka_unit_test(test_nes_minimum_contains_a_b_dpad),

        /* has_binding */
        cmocka_unit_test(test_has_binding_yes),
        cmocka_unit_test(test_has_binding_no),
        cmocka_unit_test(test_has_binding_empty_profile),
        cmocka_unit_test(test_has_binding_null_profile),
        cmocka_unit_test(test_has_binding_none_button),
        cmocka_unit_test(test_has_binding_dpad),
        cmocka_unit_test(test_has_binding_axis_prop),

        /* validate_nes_minimum */
        cmocka_unit_test(test_validate_complete),
        cmocka_unit_test(test_validate_complete_with_extras),
        cmocka_unit_test(test_validate_missing_a),
        cmocka_unit_test(test_validate_missing_multiple),
        cmocka_unit_test(test_validate_empty_profile),
        cmocka_unit_test(test_validate_null_profile),
        cmocka_unit_test(test_validate_no_missing_buf),

        /* missing_count */
        cmocka_unit_test(test_missing_count_zero),
        cmocka_unit_test(test_missing_count_some),
        cmocka_unit_test(test_missing_count_all),
        cmocka_unit_test(test_missing_count_null),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}