/*
 * test_dynamic_columns.c — Unit tests for dynamic column management (Task 31).
 *
 * Tests:
 *   - Build virtual_controllers from target types
 *   - Needs rebuild detection (target count changed)
 *   - Rebuild grid with new column count
 *   - Rebuild preserves profile list
 *   - Rebuild preserves assignments and positions
 *   - Clamp positions (removed slot → Unassigned)
 *   - Extract types from device model
 *   - NULL safety
 */
#include "overlay/dynamic_columns.h"
#include "overlay/grid_render.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "dbus/ip_device_model.h"
#include "identify/assign.h"  /* cbx_assign_make_default */

/* --- Helpers ---------------------------------------------------------- */

static cbx_grid_composite_info
make_comp(const char *id, const char *name, const char *path)
{
    cbx_grid_composite_info c;
    memset(&c, 0, sizeof(c));
    if (id) snprintf(c.id, CBX_MAX_ID_LEN, "%s", id);
    if (name) snprintf(c.model_name, CBX_MAX_NAME_LEN, "%s", name);
    if (path) snprintf(c.composite_path, CBX_MAX_PATH_LEN, "%s", path);
    return c;
}

static void
build_grid_with_types(cbx_select_grid *g, int rows,
                       const char (*types)[CBX_MAX_TYPE_LEN], int type_count)
{
    cbx_grid_composite_info comps[CBX_GRID_MAX_ROWS];
    for (int i = 0; i < rows; i++) {
        char id[32], name[32], path[64];
        snprintf(id, sizeof(id), "ORDER:%d", i);
        snprintf(name, sizeof(name), "Ctrl %d", i);
        snprintf(path, sizeof(path),
                 "/org/shadowblip/InputPlumber/CompositeDevice%d", i);
        comps[i] = make_comp(id, name, path);
    }

    cbx_settings s;
    memset(&s, 0, sizeof(s));
    s.virtual_controllers.count = type_count;
    for (int i = 0; i < type_count; i++)
        snprintf(s.virtual_controllers.types[i], CBX_MAX_TYPE_LEN,
                 "%s", types[i]);

    cbx_assignments a;
    cbx_assignments_init(&a);

    cbx_select_grid_build(g, comps, rows, &s, &a);

    /* Add profiles. */
    cbx_select_grid_add_profile(g, "default");
    cbx_select_grid_add_profile(g, "fighting");
}

/* --- Build VCS tests ------------------------------------------------- */

static void
test_build_vcs(void **state)
{
    (void)state;
    const char types[][CBX_MAX_TYPE_LEN] = {"xb360", "ds5", "deck"};
    cbx_virtual_controllers vc;

    int rc = cbx_dynamic_columns_build_vcs(types, 3, &vc);
    assert_int_equal(rc, 0);
    assert_int_equal(vc.count, 3);
    assert_string_equal(vc.types[0], "xb360");
    assert_string_equal(vc.types[1], "ds5");
    assert_string_equal(vc.types[2], "deck");
}

static void
test_build_vcs_single(void **state)
{
    (void)state;
    const char types[][CBX_MAX_TYPE_LEN] = {"xb360"};
    cbx_virtual_controllers vc;

    int rc = cbx_dynamic_columns_build_vcs(types, 1, &vc);
    assert_int_equal(rc, 0);
    assert_int_equal(vc.count, 1);
    assert_string_equal(vc.types[0], "xb360");
}

static void
test_build_vcs_null(void **state)
{
    (void)state;
    cbx_virtual_controllers vc;
    int rc = cbx_dynamic_columns_build_vcs(NULL, 3, &vc);
    assert_int_equal(rc, -EINVAL);

    const char types[][CBX_MAX_TYPE_LEN] = {"xb360"};
    rc = cbx_dynamic_columns_build_vcs(types, 3, NULL);
    assert_int_equal(rc, -EINVAL);
}

static void
test_build_vcs_bad_count(void **state)
{
    (void)state;
    const char types[][CBX_MAX_TYPE_LEN] = {"xb360"};
    cbx_virtual_controllers vc;

    int rc = cbx_dynamic_columns_build_vcs(types, 0, &vc);
    assert_int_equal(rc, -EINVAL);

    rc = cbx_dynamic_columns_build_vcs(types, CBX_MAX_CONTROLLERS + 1, &vc);
    assert_int_equal(rc, -EINVAL);
}

/* --- Needs rebuild tests --------------------------------------------- */

static void
test_needs_rebuild_same(void **state)
{
    (void)state;
    cbx_select_grid g;
    const char types[][CBX_MAX_TYPE_LEN] = {"xb360", "ds5"};
    build_grid_with_types(&g, 1, types, 2);
    /* Grid has 2 targets → col_count = 3. target_count = 2 → needs rebuild? */
    bool needs = cbx_dynamic_columns_needs_rebuild(&g, 2);
    assert_false(needs);
}

static void
test_needs_rebuild_more(void **state)
{
    (void)state;
    cbx_select_grid g;
    const char types[][CBX_MAX_TYPE_LEN] = {"xb360"};
    build_grid_with_types(&g, 1, types, 1);
    /* Grid has 1 target → col_count = 2. Now 3 targets. */
    bool needs = cbx_dynamic_columns_needs_rebuild(&g, 3);
    assert_true(needs);
}

static void
test_needs_rebuild_fewer(void **state)
{
    (void)state;
    cbx_select_grid g;
    const char types[][CBX_MAX_TYPE_LEN] = {"xb360", "ds5", "deck"};
    build_grid_with_types(&g, 1, types, 3);
    /* Grid has 3 targets → col_count = 4. Now 1 target. */
    bool needs = cbx_dynamic_columns_needs_rebuild(&g, 1);
    assert_true(needs);
}

static void
test_needs_rebuild_null(void **state)
{
    (void)state;
    bool needs = cbx_dynamic_columns_needs_rebuild(NULL, 2);
    assert_false(needs);
}

/* --- Clamp positions tests -------------------------------------------- */

static void
test_clamp_positions(void **state)
{
    (void)state;
    cbx_select_grid g;
    const char types[][CBX_MAX_TYPE_LEN] = {"xb360", "ds5", "deck", "gamepad"};
    build_grid_with_types(&g, 3, types, 4);
    /* Set positions: row 0 at col 4 (P4), row 1 at col 2 (P2), row 2 at col 1 (P1). */
    g.rows[0].cur_col = 4;
    g.rows[1].cur_col = 2;
    g.rows[2].cur_col = 1;

    /* New col_count = 3 (2 targets + Unassigned). Col 4 is gone. */
    int clamped = cbx_dynamic_columns_clamp_positions(&g, 3);
    assert_int_equal(clamped, 1);  /* row 0 clamped */
    assert_int_equal(g.rows[0].cur_col, 0);  /* Unassigned */
    assert_int_equal(g.rows[1].cur_col, 2);  /* unchanged */
    assert_int_equal(g.rows[2].cur_col, 1);  /* unchanged */
}

static void
test_clamp_positions_none(void **state)
{
    (void)state;
    cbx_select_grid g;
    const char types[][CBX_MAX_TYPE_LEN] = {"xb360", "ds5"};
    build_grid_with_types(&g, 2, types, 2);
    g.rows[0].cur_col = 1;
    g.rows[1].cur_col = 2;

    int clamped = cbx_dynamic_columns_clamp_positions(&g, 3);
    assert_int_equal(clamped, 0);  /* nothing clamped */
}

static void
test_clamp_positions_null(void **state)
{
    (void)state;
    int rc = cbx_dynamic_columns_clamp_positions(NULL, 3);
    assert_int_equal(rc, -EINVAL);
}

/* --- Rebuild tests --------------------------------------------------- */

static void
test_rebuild_more_columns(void **state)
{
    (void)state;
    cbx_select_grid g;
    const char initial_types[][CBX_MAX_TYPE_LEN] = {"xb360", "ds5"};
    build_grid_with_types(&g, 2, initial_types, 2);
    assert_int_equal(g.col_count, 3);

    /* Add profiles before rebuild. */
    cbx_select_grid_add_profile(&g, "test_profile");
    /* build_grid_with_types adds "default" and "fighting", then we add "test_profile" = 3. */
    int saved_count = g.profile_count;
    assert_int_equal(saved_count, 3);

    /* Rebuild with 4 targets. */
    const char new_types[][CBX_MAX_TYPE_LEN] = {"xb360", "ds5", "deck", "gamepad"};
    cbx_grid_composite_info comps[2];
    comps[0] = make_comp("ORDER:0", "Ctrl 0", "/org/shadowblip/InputPlumber/CompositeDevice0");
    comps[1] = make_comp("ORDER:1", "Ctrl 1", "/org/shadowblip/InputPlumber/CompositeDevice1");

    cbx_assignments a;
    cbx_assignments_init(&a);

    int rc = cbx_dynamic_columns_rebuild(&g, new_types, 4, comps, 2, &a);
    assert_int_equal(rc, 0);
    assert_int_equal(g.col_count, 5);  /* 4 targets + Unassigned */
    assert_string_equal(g.cols[1].device_type, "xb360");
    assert_string_equal(g.cols[2].device_type, "ds5");
    assert_string_equal(g.cols[3].device_type, "deck");
    assert_string_equal(g.cols[4].device_type, "gamepad");

    /* Profile list should be preserved. */
    assert_int_equal(g.profile_count, saved_count);
}

static void
test_rebuild_fewer_columns(void **state)
{
    (void)state;
    cbx_select_grid g;
    const char initial_types[][CBX_MAX_TYPE_LEN] = {"xb360", "ds5", "deck"};
    build_grid_with_types(&g, 2, initial_types, 3);
    assert_int_equal(g.col_count, 4);

    /* Set position for row 0 at col 3 (P3). */
    g.rows[0].cur_col = 3;

    /* Rebuild with 1 target — P2 and P3 columns removed. */
    const char new_types[][CBX_MAX_TYPE_LEN] = {"xb360"};
    cbx_grid_composite_info comps[2];
    comps[0] = make_comp("ORDER:0", "Ctrl 0", "/org/shadowblip/InputPlumber/CompositeDevice0");
    comps[1] = make_comp("ORDER:1", "Ctrl 1", "/org/shadowblip/InputPlumber/CompositeDevice1");

    cbx_assignments a;
    cbx_assignments_init(&a);

    int rc = cbx_dynamic_columns_rebuild(&g, new_types, 1, comps, 2, &a);
    assert_int_equal(rc, 0);
    assert_int_equal(g.col_count, 2);  /* 1 target + Unassigned */
    /* Row 0 had no assignment, so default = Unassigned. */
    assert_int_equal(g.rows[0].cur_col, 0);
}

static void
test_rebuild_preserves_profiles(void **state)
{
    (void)state;
    cbx_select_grid g;
    const char types[][CBX_MAX_TYPE_LEN] = {"xb360"};
    build_grid_with_types(&g, 1, types, 1);

    /* Add custom profiles. */
    cbx_select_grid_clear_profiles(&g);
    cbx_select_grid_add_profile(&g, "custom1");
    cbx_select_grid_add_profile(&g, "custom2");
    assert_int_equal(g.profile_count, 2);

    /* Rebuild with same target count. */
    cbx_grid_composite_info comps[1];
    comps[0] = make_comp("ORDER:0", "Ctrl 0", "/org/shadowblip/InputPlumber/CompositeDevice0");

    cbx_assignments a;
    cbx_assignments_init(&a);

    int rc = cbx_dynamic_columns_rebuild(&g, types, 1, comps, 1, &a);
    assert_int_equal(rc, 0);
    /* Profiles should be preserved. */
    assert_int_equal(g.profile_count, 2);
    assert_string_equal(g.profiles[0], "custom1");
    assert_string_equal(g.profiles[1], "custom2");
}

static void
test_rebuild_preserves_assignments(void **state)
{
    (void)state;
    cbx_select_grid g;
    const char types[][CBX_MAX_TYPE_LEN] = {"xb360", "ds5"};
    build_grid_with_types(&g, 2, types, 2);

    /* Create assignment for controller 0 at slot 1 (P2). */
    cbx_assignments a;
    cbx_assignments_init(&a);
    cbx_assignment def;
    cbx_assign_make_default("ORDER:0", 1, &def);
    snprintf(def.profile, CBX_MAX_PROFILE_LEN, "fighting");
    a.assignments[a.assignment_count++] = def;

    /* Rebuild with same target count. */
    cbx_grid_composite_info comps[2];
    comps[0] = make_comp("ORDER:0", "Ctrl 0", "/org/shadowblip/InputPlumber/CompositeDevice0");
    comps[1] = make_comp("ORDER:1", "Ctrl 1", "/org/shadowblip/InputPlumber/CompositeDevice1");

    int rc = cbx_dynamic_columns_rebuild(&g, types, 2, comps, 2, &a);
    assert_int_equal(rc, 0);
    /* Row 0 should be at col 2 (slot 1 + 1 = P2). */
    assert_int_equal(g.rows[0].cur_col, 2);
    assert_string_equal(g.rows[0].profile, "fighting");
}

static void
test_rebuild_null(void **state)
{
    (void)state;
    const char types[][CBX_MAX_TYPE_LEN] = {"xb360"};
    cbx_grid_composite_info comps[1];
    comps[0] = make_comp("ORDER:0", "Ctrl 0", "/path");
    cbx_assignments a;
    cbx_assignments_init(&a);

    int rc = cbx_dynamic_columns_rebuild(NULL, types, 1, comps, 1, &a);
    assert_int_equal(rc, -EINVAL);

    cbx_select_grid g;
    build_grid_with_types(&g, 1, types, 1);
    rc = cbx_dynamic_columns_rebuild(&g, NULL, 1, comps, 1, &a);
    assert_int_equal(rc, -EINVAL);

    rc = cbx_dynamic_columns_rebuild(&g, types, 1, NULL, 1, &a);
    assert_int_equal(rc, -EINVAL);

    rc = cbx_dynamic_columns_rebuild(&g, types, 1, comps, 1, NULL);
    assert_int_equal(rc, -EINVAL);
}

static void
test_rebuild_bad_count(void **state)
{
    (void)state;
    const char types[][CBX_MAX_TYPE_LEN] = {"xb360"};
    cbx_grid_composite_info comps[1];
    comps[0] = make_comp("ID", "Name", "/path");
    cbx_assignments a;
    cbx_assignments_init(&a);
    cbx_select_grid g;
    cbx_select_grid_init(&g);

    int rc = cbx_dynamic_columns_rebuild(&g, types, 0, comps, 1, &a);
    assert_int_equal(rc, -EINVAL);

    rc = cbx_dynamic_columns_rebuild(&g, types, CBX_MAX_CONTROLLERS + 1, comps, 1, &a);
    assert_int_equal(rc, -EINVAL);
}

/* --- Extract types tests --------------------------------------------- */

static int
mock_query_type(const char *target_path, char *out, size_t out_len)
{
    (void)target_path;
    snprintf(out, out_len, "xb360");
    return 0;
}

static int
mock_query_type_fail(const char *target_path, char *out, size_t out_len)
{
    (void)target_path; (void)out; (void)out_len;
    return -EIO;
}

static void
test_extract_types(void **state)
{
    (void)state;
    cbx_device_model model;
    cbx_device_model_init(&model);
    cbx_device_model_add_target(&model, "/org/shadowblip/InputPlumber/devices/target/gamepad0");
    cbx_device_model_add_target(&model, "/org/shadowblip/InputPlumber/devices/target/gamepad1");

    char types[CBX_MAX_CONTROLLERS][CBX_MAX_TYPE_LEN];
    int count = 0;

    int rc = cbx_dynamic_columns_extract_types(&model, mock_query_type,
                                                  types, &count,
                                                  CBX_MAX_CONTROLLERS);
    assert_int_equal(rc, 0);
    assert_int_equal(count, 2);
    assert_string_equal(types[0], "xb360");
    assert_string_equal(types[1], "xb360");
}

static void
test_extract_types_empty(void **state)
{
    (void)state;
    cbx_device_model model;
    cbx_device_model_init(&model);

    char types[CBX_MAX_CONTROLLERS][CBX_MAX_TYPE_LEN];
    int count = -1;

    int rc = cbx_dynamic_columns_extract_types(&model, mock_query_type,
                                                  types, &count,
                                                  CBX_MAX_CONTROLLERS);
    assert_int_equal(rc, 0);
    assert_int_equal(count, 0);
}

static void
test_extract_types_fail(void **state)
{
    (void)state;
    cbx_device_model model;
    cbx_device_model_init(&model);
    cbx_device_model_add_target(&model, "/org/shadowblip/InputPlumber/devices/target/gamepad0");

    char types[CBX_MAX_CONTROLLERS][CBX_MAX_TYPE_LEN];
    int count = 0;

    int rc = cbx_dynamic_columns_extract_types(&model, mock_query_type_fail,
                                                  types, &count,
                                                  CBX_MAX_CONTROLLERS);
    assert_int_equal(rc, -EIO);
}

static void
test_extract_types_null(void **state)
{
    (void)state;
    char types[CBX_MAX_CONTROLLERS][CBX_MAX_TYPE_LEN];
    int count = 0;

    int rc = cbx_dynamic_columns_extract_types(NULL, mock_query_type,
                                                  types, &count,
                                                  CBX_MAX_CONTROLLERS);
    assert_int_equal(rc, -EINVAL);

    cbx_device_model model;
    cbx_device_model_init(&model);
    rc = cbx_dynamic_columns_extract_types(&model, mock_query_type,
                                              types, NULL,
                                              CBX_MAX_CONTROLLERS);
    assert_int_equal(rc, -EINVAL);
}

/* --- Hotplug simulation tests ---------------------------------------- */

static void
test_hotplug_add_target(void **state)
{
    (void)state;
    /* Start with 2 targets. */
    cbx_select_grid g;
    const char initial_types[][CBX_MAX_TYPE_LEN] = {"xb360", "ds5"};
    build_grid_with_types(&g, 2, initial_types, 2);
    assert_int_equal(g.col_count, 3);

    /* Simulate hotplug: a new target device appears. */
    bool needs = cbx_dynamic_columns_needs_rebuild(&g, 3);
    assert_true(needs);

    /* Rebuild with 3 targets. */
    const char new_types[][CBX_MAX_TYPE_LEN] = {"xb360", "ds5", "deck"};
    cbx_grid_composite_info comps[2];
    comps[0] = make_comp("ORDER:0", "Ctrl 0", "/org/shadowblip/InputPlumber/CompositeDevice0");
    comps[1] = make_comp("ORDER:1", "Ctrl 1", "/org/shadowblip/InputPlumber/CompositeDevice1");

    cbx_assignments a;
    cbx_assignments_init(&a);

    int rc = cbx_dynamic_columns_rebuild(&g, new_types, 3, comps, 2, &a);
    assert_int_equal(rc, 0);
    assert_int_equal(g.col_count, 4);

    needs = cbx_dynamic_columns_needs_rebuild(&g, 3);
    assert_false(needs);
}

static void
test_hotplug_remove_target(void **state)
{
    (void)state;
    /* Start with 4 targets. */
    cbx_select_grid g;
    const char initial_types[][CBX_MAX_TYPE_LEN] = {"xb360", "ds5", "deck", "gamepad"};
    build_grid_with_types(&g, 2, initial_types, 4);
    assert_int_equal(g.col_count, 5);

    /* Put controller 0 on P4 (col 4). */
    g.rows[0].cur_col = 4;

    /* Simulate hotplug: a target device is removed (4 → 3). */
    bool needs = cbx_dynamic_columns_needs_rebuild(&g, 3);
    assert_true(needs);

    /* Rebuild with 3 targets. */
    const char new_types[][CBX_MAX_TYPE_LEN] = {"xb360", "ds5", "deck"};
    cbx_grid_composite_info comps[2];
    comps[0] = make_comp("ORDER:0", "Ctrl 0", "/org/shadowblip/InputPlumber/CompositeDevice0");
    comps[1] = make_comp("ORDER:1", "Ctrl 1", "/org/shadowblip/InputPlumber/CompositeDevice1");

    cbx_assignments a;
    cbx_assignments_init(&a);

    int rc = cbx_dynamic_columns_rebuild(&g, new_types, 3, comps, 2, &a);
    assert_int_equal(rc, 0);
    assert_int_equal(g.col_count, 4);
    /* Controller 0 had no assignment, so it defaults to Unassigned. */
    assert_int_equal(g.rows[0].cur_col, 0);
}

static void
test_hotplug_remove_with_assignment(void **state)
{
    (void)state;
    /* Start with 3 targets. */
    cbx_select_grid g;
    const char initial_types[][CBX_MAX_TYPE_LEN] = {"xb360", "ds5", "deck"};
    build_grid_with_types(&g, 1, initial_types, 3);
    assert_int_equal(g.col_count, 4);

    /* Create assignment: controller 0 at slot 2 (P3, col 3). */
    cbx_assignments a;
    cbx_assignments_init(&a);
    cbx_assignment def;
    cbx_assign_make_default("ORDER:0", 2, &def);
    a.assignments[a.assignment_count++] = def;

    /* Rebuild with 1 target (P2 and P3 removed). */
    const char new_types[][CBX_MAX_TYPE_LEN] = {"xb360"};
    cbx_grid_composite_info comps[1];
    comps[0] = make_comp("ORDER:0", "Ctrl 0", "/org/shadowblip/InputPlumber/CompositeDevice0");

    int rc = cbx_dynamic_columns_rebuild(&g, new_types, 1, comps, 1, &a);
    assert_int_equal(rc, 0);
    assert_int_equal(g.col_count, 2);
    /* Controller 0 had slot 2, but only 1 slot now → Unassigned. */
    assert_int_equal(g.rows[0].cur_col, 0);
}

/* --- Main ------------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* Build VCS */
        cmocka_unit_test(test_build_vcs),
        cmocka_unit_test(test_build_vcs_single),
        cmocka_unit_test(test_build_vcs_null),
        cmocka_unit_test(test_build_vcs_bad_count),
        /* Needs rebuild */
        cmocka_unit_test(test_needs_rebuild_same),
        cmocka_unit_test(test_needs_rebuild_more),
        cmocka_unit_test(test_needs_rebuild_fewer),
        cmocka_unit_test(test_needs_rebuild_null),
        /* Clamp positions */
        cmocka_unit_test(test_clamp_positions),
        cmocka_unit_test(test_clamp_positions_none),
        cmocka_unit_test(test_clamp_positions_null),
        /* Rebuild */
        cmocka_unit_test(test_rebuild_more_columns),
        cmocka_unit_test(test_rebuild_fewer_columns),
        cmocka_unit_test(test_rebuild_preserves_profiles),
        cmocka_unit_test(test_rebuild_preserves_assignments),
        cmocka_unit_test(test_rebuild_null),
        cmocka_unit_test(test_rebuild_bad_count),
        /* Extract types */
        cmocka_unit_test(test_extract_types),
        cmocka_unit_test(test_extract_types_empty),
        cmocka_unit_test(test_extract_types_fail),
        cmocka_unit_test(test_extract_types_null),
        /* Hotplug */
        cmocka_unit_test(test_hotplug_add_target),
        cmocka_unit_test(test_hotplug_remove_target),
        cmocka_unit_test(test_hotplug_remove_with_assignment),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}