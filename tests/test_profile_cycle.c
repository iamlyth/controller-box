/*
 * test_profile_cycle.c — Unit tests for profile cycling (Task 31).
 *
 * Tests:
 *   - Init (context fields, defaults)
 *   - Load profiles from enumeration into grid
 *   - Find profile path by name
 *   - Apply profile change (LoadProfilePath via mock backend + assignment update)
 *   - Apply with NULL args / missing profile / backend failure
 *   - Update assignment (existing, new, full assignments)
 *   - Profile follows controller across columns
 *   - Multiple controllers with different profiles
 */
#include "overlay/profile_cycle.h"
#include "overlay/grid_render.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "dbus_mock.h"
#include "dbus/ip_composite.h"
#include "dbus/ip_connection.h"  /* IP_ERR_NO_REPLY */
#include "identify/assign.h"

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
make_profile_list(cbx_profile_list *list, const char *names[], int count)
{
    memset(list, 0, sizeof(*list));
    for (int i = 0; i < count && i < CBX_MAX_PROFILES; i++) {
        snprintf(list->entries[i].filename, CBX_LIST_NAME_LEN, "%s", names[i]);
        snprintf(list->entries[i].path, PATH_MAX,
                 "/home/user/.local/share/inputplumber/profiles/%s.yaml", names[i]);
        list->count++;
    }
}

static void
build_test_grid(cbx_select_grid *g, int rows)
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
    s.virtual_controllers.count = 4;
    for (int i = 0; i < 4; i++)
        snprintf(s.virtual_controllers.types[i], CBX_MAX_TYPE_LEN, "xb360");

    cbx_assignments a;
    cbx_assignments_init(&a);

    cbx_select_grid_build(g, comps, rows, &s, &a);

    /* Add profiles. */
    cbx_select_grid_add_profile(g, "default");
    cbx_select_grid_add_profile(g, "fighting");
    cbx_select_grid_add_profile(g, "racing");
}

/* --- Fixture ---------------------------------------------------------- */

typedef struct {
    ip_dbus_mock          mock;
    const ip_dbus_backend *backend;
    cbx_profile_list      profiles;
    cbx_assignments       assignments;
    cbx_select_grid       grid;
    cbx_profile_cycle     pc;
} pc_fixture;

static int
setup(void **state)
{
    pc_fixture *f = malloc(sizeof(*f));
    if (!f)
        return -1;
    memset(f, 0, sizeof(*f));
    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);
    cbx_assignments_init(&f->assignments);

    const char *names[] = {"default", "fighting", "racing"};
    make_profile_list(&f->profiles, names, 3);

    build_test_grid(&f->grid, 2);

    cbx_profile_cycle_init(&f->pc, f->backend, f->mock.bus,
                            &f->assignments, &f->profiles);

    *state = f;
    return 0;
}

static int
teardown(void **state)
{
    pc_fixture *f = *state;
    if (f) {
        ip_dbus_mock_reset(&f->mock);
        free(f);
    }
    return 0;
}

#define FIX(state) (*(pc_fixture **)(state))

/* --- Init tests ------------------------------------------------------ */

static void
test_init(void **state)
{
    pc_fixture *f = FIX(state);
    assert_ptr_equal(f->pc.backend, f->backend);
    assert_ptr_equal(f->pc.bus, f->mock.bus);
    assert_ptr_equal(f->pc.assignments, &f->assignments);
    assert_ptr_equal(f->pc.profiles, &f->profiles);
}

static void
test_init_null(void **state)
{
    (void)state;
    cbx_profile_cycle pc;
    cbx_profile_cycle_init(&pc, NULL, NULL, NULL, NULL);
    assert_null(pc.backend);
    assert_null(pc.bus);
    assert_null(pc.assignments);
    assert_null(pc.profiles);
}

/* --- Load profiles tests --------------------------------------------- */

static void
test_load_profiles(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_select_grid_init(&g);
    cbx_profile_list list;
    const char *names[] = {"default", "fighting", "racing", "fps"};
    make_profile_list(&list, names, 4);

    int rc = cbx_profile_cycle_load_profiles(&g, &list);
    assert_int_equal(rc, 0);
    assert_int_equal(g.profile_count, 4);
    assert_string_equal(g.profiles[0], "default");
    assert_string_equal(g.profiles[1], "fighting");
    assert_string_equal(g.profiles[2], "racing");
    assert_string_equal(g.profiles[3], "fps");
}

static void
test_load_profiles_clears_existing(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_select_grid_init(&g);
    cbx_select_grid_add_profile(&g, "old1");
    cbx_select_grid_add_profile(&g, "old2");
    assert_int_equal(g.profile_count, 2);

    cbx_profile_list list;
    const char *names[] = {"new1"};
    make_profile_list(&list, names, 1);

    int rc = cbx_profile_cycle_load_profiles(&g, &list);
    assert_int_equal(rc, 0);
    assert_int_equal(g.profile_count, 1);
    assert_string_equal(g.profiles[0], "new1");
}

static void
test_load_profiles_null(void **state)
{
    (void)state;
    int rc = cbx_profile_cycle_load_profiles(NULL, NULL);
    assert_int_equal(rc, -EINVAL);
}

static void
test_load_profiles_empty_list(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_select_grid_init(&g);
    cbx_profile_list list;
    memset(&list, 0, sizeof(list));

    int rc = cbx_profile_cycle_load_profiles(&g, &list);
    assert_int_equal(rc, 0);
    assert_int_equal(g.profile_count, 0);
}

/* --- Find path tests ------------------------------------------------- */

static void
test_find_path(void **state)
{
    pc_fixture *f = FIX(state);
    char out[PATH_MAX];
    int rc = cbx_profile_cycle_find_path(&f->profiles, "fighting",
                                          out, sizeof(out));
    assert_int_equal(rc, 0);
    assert_string_equal(out, "/home/user/.local/share/inputplumber/profiles/fighting.yaml");
}

static void
test_find_path_not_found(void **state)
{
    pc_fixture *f = FIX(state);
    char out[PATH_MAX];
    int rc = cbx_profile_cycle_find_path(&f->profiles, "nonexistent",
                                          out, sizeof(out));
    assert_int_equal(rc, -ENOENT);
}

static void
test_find_path_null(void **state)
{
    (void)state;
    char out[PATH_MAX];
    int rc = cbx_profile_cycle_find_path(NULL, "default", out, sizeof(out));
    assert_int_equal(rc, -EINVAL);

    cbx_profile_list list;
    memset(&list, 0, sizeof(list));
    rc = cbx_profile_cycle_find_path(&list, "default", NULL, 0);
    assert_int_equal(rc, -EINVAL);
}

/* --- Apply tests ----------------------------------------------------- */

static void
test_apply_success(void **state)
{
    pc_fixture *f = FIX(state);
    /* Expect LoadProfilePath call on the composite interface. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "LoadProfilePath", NULL);

    int rc = cbx_profile_cycle_apply(&f->pc, &f->grid, 0, "fighting",
        "/org/shadowblip/InputPlumber/CompositeDevice0");
    assert_int_equal(rc, 0);

    /* Assignment should be updated with the new profile. */
    cbx_assignment found;
    int arc = cbx_assign_lookup(&f->assignments, "ORDER:0", &found);
    assert_int_equal(arc, 0);
    assert_string_equal(found.profile, "fighting");
}

static void
test_apply_profile_not_in_list(void **state)
{
    pc_fixture *f = FIX(state);
    int rc = cbx_profile_cycle_apply(&f->pc, &f->grid, 0, "nonexistent",
        "/org/shadowblip/InputPlumber/CompositeDevice0");
    assert_int_equal(rc, -ENOENT);
}

static void
test_apply_null(void **state)
{
    pc_fixture *f = FIX(state);
    int rc = cbx_profile_cycle_apply(NULL, &f->grid, 0, "default", "/path");
    assert_int_equal(rc, -EINVAL);

    rc = cbx_profile_cycle_apply(&f->pc, NULL, 0, "default", "/path");
    assert_int_equal(rc, -EINVAL);

    rc = cbx_profile_cycle_apply(&f->pc, &f->grid, -1, "default", "/path");
    assert_int_equal(rc, -EINVAL);

    rc = cbx_profile_cycle_apply(&f->pc, &f->grid, 0, NULL, "/path");
    assert_int_equal(rc, -EINVAL);

    rc = cbx_profile_cycle_apply(&f->pc, &f->grid, 0, "default", NULL);
    assert_int_equal(rc, -EINVAL);
}

static void
test_apply_no_profiles(void **state)
{
    (void)state;
    /* Profile cycle with no profile list should return -ENOENT. */
    ip_dbus_mock mock;
    ip_dbus_mock_init(&mock);
    const ip_dbus_backend *backend = ip_dbus_mock_backend(&mock);

    cbx_assignments a;
    cbx_assignments_init(&a);
    cbx_select_grid g;
    build_test_grid(&g, 1);

    cbx_profile_cycle pc;
    cbx_profile_cycle_init(&pc, backend, mock.bus, &a, NULL);

    int rc = cbx_profile_cycle_apply(&pc, &g, 0, "default", "/path");
    assert_int_equal(rc, -ENOENT);

    ip_dbus_mock_reset(&mock);
}

static void
test_apply_no_backend(void **state)
{
    (void)state;
    /* Apply without backend should still update assignment (skips DBus). */
    cbx_assignments a;
    cbx_assignments_init(&a);
    cbx_profile_list list;
    const char *names[] = {"default", "fighting"};
    make_profile_list(&list, names, 2);
    cbx_select_grid g;
    build_test_grid(&g, 1);

    cbx_profile_cycle pc;
    cbx_profile_cycle_init(&pc, NULL, NULL, &a, &list);

    int rc = cbx_profile_cycle_apply(&pc, &g, 0, "fighting", "/path");
    assert_int_equal(rc, 0);

    /* Assignment should be updated. */
    cbx_assignment found;
    int arc = cbx_assign_lookup(&a, "ORDER:0", &found);
    assert_int_equal(arc, 0);
    assert_string_equal(found.profile, "fighting");
}

static void
test_apply_no_assignments(void **state)
{
    (void)state;
    /* Apply without assignments should still do LoadProfilePath. */
    ip_dbus_mock mock;
    ip_dbus_mock_init(&mock);
    const ip_dbus_backend *backend = ip_dbus_mock_backend(&mock);

    cbx_profile_list list;
    const char *names[] = {"default", "fighting"};
    make_profile_list(&list, names, 2);
    cbx_select_grid g;
    build_test_grid(&g, 1);

    ip_dbus_mock_expect_ok(&mock, IP_IFACE_COMPOSITE,
                            "LoadProfilePath", NULL);

    cbx_profile_cycle pc;
    cbx_profile_cycle_init(&pc, backend, mock.bus, NULL, &list);

    int rc = cbx_profile_cycle_apply(&pc, &g, 0, "fighting", "/path");
    assert_int_equal(rc, 0);

    ip_dbus_mock_reset(&mock);
}

static void
test_apply_backend_error(void **state)
{
    pc_fixture *f = FIX(state);
    /* Expect LoadProfilePath to fail. */
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                               "LoadProfilePath", IP_ERR_NO_REPLY);

    int rc = cbx_profile_cycle_apply(&f->pc, &f->grid, 0, "fighting",
        "/org/shadowblip/InputPlumber/CompositeDevice0");
    assert_int_equal(rc, IP_ERR_NO_REPLY);
}

static void
test_apply_updates_assignment_existing(void **state)
{
    pc_fixture *f = FIX(state);
    /* Pre-populate assignment. */
    cbx_assignment a;
    cbx_assign_make_default("ORDER:0", 1, &a);
    cbx_assignments_init(&f->assignments);
    f->assignments.assignments[f->assignments.assignment_count++] = a;

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "LoadProfilePath", NULL);

    int rc = cbx_profile_cycle_apply(&f->pc, &f->grid, 0, "racing",
        "/org/shadowblip/InputPlumber/CompositeDevice0");
    assert_int_equal(rc, 0);

    cbx_assignment found;
    int arc = cbx_assign_lookup(&f->assignments, "ORDER:0", &found);
    assert_int_equal(arc, 0);
    assert_int_equal(found.slot, 1);  /* slot unchanged */
    assert_string_equal(found.profile, "racing");  /* profile updated */
}

static void
test_apply_different_controllers(void **state)
{
    pc_fixture *f = FIX(state);

    /* Controller 0 changes to "fighting". */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "LoadProfilePath", NULL);
    int rc = cbx_profile_cycle_apply(&f->pc, &f->grid, 0, "fighting",
        "/org/shadowblip/InputPlumber/CompositeDevice0");
    assert_int_equal(rc, 0);

    /* Controller 1 changes to "racing". */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "LoadProfilePath", NULL);
    rc = cbx_profile_cycle_apply(&f->pc, &f->grid, 1, "racing",
        "/org/shadowblip/InputPlumber/CompositeDevice1");
    assert_int_equal(rc, 0);

    /* Verify each controller has its own profile. */
    cbx_assignment found0, found1;
    cbx_assign_lookup(&f->assignments, "ORDER:0", &found0);
    cbx_assign_lookup(&f->assignments, "ORDER:1", &found1);
    assert_string_equal(found0.profile, "fighting");
    assert_string_equal(found1.profile, "racing");
}

/* --- Update assignment tests ------------------------------------------ */

static void
test_update_assignment_existing(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_assignments_init(&a);
    cbx_assignment def;
    cbx_assign_make_default("ORDER:0", 0, &def);
    a.assignments[a.assignment_count++] = def;

    int rc = cbx_profile_cycle_update_assignment(&a, "ORDER:0", "fps");
    assert_int_equal(rc, 0);
    assert_string_equal(a.assignments[0].profile, "fps");
}

static void
test_update_assignment_new(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_assignments_init(&a);

    int rc = cbx_profile_cycle_update_assignment(&a, "ORDER:5", "racing");
    assert_int_equal(rc, 0);
    assert_int_equal(a.assignment_count, 1);
    assert_string_equal(a.assignments[0].id, "ORDER:5");
    assert_string_equal(a.assignments[0].profile, "racing");
}

static void
test_update_assignment_null(void **state)
{
    (void)state;
    int rc = cbx_profile_cycle_update_assignment(NULL, "id", "profile");
    assert_int_equal(rc, -EINVAL);

    cbx_assignments a;
    cbx_assignments_init(&a);
    rc = cbx_profile_cycle_update_assignment(&a, NULL, "profile");
    assert_int_equal(rc, -EINVAL);

    rc = cbx_profile_cycle_update_assignment(&a, "id", NULL);
    assert_int_equal(rc, -EINVAL);
}

/* --- Profile follows controller tests --------------------------------- */

static void
test_profile_follows(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 1);
    /* Profile should be "default" after build. */
    bool follows = cbx_profile_cycle_profile_follows(&g, 0);
    assert_true(follows);
}

static void
test_profile_follows_null(void **state)
{
    (void)state;
    bool follows = cbx_profile_cycle_profile_follows(NULL, 0);
    assert_false(follows);
}

static void
test_profile_follows_bad_row(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 1);
    bool follows = cbx_profile_cycle_profile_follows(&g, -1);
    assert_false(follows);
    follows = cbx_profile_cycle_profile_follows(&g, 99);
    assert_false(follows);
}

static void
test_profile_follows_across_move(void **state)
{
    /* Demonstrates that profile follows controller across column moves. */
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 1);

    /* Set a specific profile. */
    snprintf(g.rows[0].profile, CBX_GRID_PROFILE_LEN, "fighting");
    g.rows[0].cur_col = 2;  /* P2 */

    const char *before = cbx_select_grid_get_profile(&g, 0);
    assert_string_equal(before, "fighting");

    /* Move right to P3 — profile should not change. */
    cbx_select_grid_move_right(&g, 0);
    assert_int_equal(g.rows[0].cur_col, 3);
    const char *after_right = cbx_select_grid_get_profile(&g, 0);
    assert_string_equal(after_right, "fighting");

    /* Move left back to P2 — profile should not change. */
    cbx_select_grid_move_left(&g, 0);
    assert_int_equal(g.rows[0].cur_col, 2);
    const char *after_left = cbx_select_grid_get_profile(&g, 0);
    assert_string_equal(after_left, "fighting");

    /* Profile follows controller. */
    assert_true(cbx_profile_cycle_profile_follows(&g, 0));
}

/* --- Full workflow test ----------------------------------------------- */

static void
test_full_workflow(void **state)
{
    pc_fixture *f = FIX(state);

    /* 1. Load profiles from enumeration into grid. */
    int rc = cbx_profile_cycle_load_profiles(&f->grid, &f->profiles);
    assert_int_equal(rc, 0);
    assert_int_equal(f->grid.profile_count, 3);

    /* 2. Cycle profile up for controller 0. */
    rc = cbx_select_grid_cycle_profile_up(&f->grid, 0);
    assert_int_equal(rc, 0);
    /* "default" → wrapping up: "racing" (index 2). */
    assert_string_equal(cbx_select_grid_get_profile(&f->grid, 0), "racing");

    /* 3. Apply the profile change via the callback handler. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "LoadProfilePath", NULL);
    rc = cbx_profile_cycle_apply(&f->pc, &f->grid, 0, "racing",
        "/org/shadowblip/InputPlumber/CompositeDevice0");
    assert_int_equal(rc, 0);

    /* 4. Verify assignment updated. */
    cbx_assignment found;
    cbx_assign_lookup(&f->assignments, "ORDER:0", &found);
    assert_string_equal(found.profile, "racing");

    /* 5. Verify profile follows controller across column moves. */
    f->grid.rows[0].cur_col = 1;  /* P1 */
    cbx_select_grid_move_right(&f->grid, 0);
    assert_int_equal(f->grid.rows[0].cur_col, 2);  /* P2 */
    assert_string_equal(cbx_select_grid_get_profile(&f->grid, 0), "racing");
    assert_true(cbx_profile_cycle_profile_follows(&f->grid, 0));
}

/* --- Main ------------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* Init */
        cmocka_unit_test(test_init),
        cmocka_unit_test(test_init_null),
        /* Load profiles */
        cmocka_unit_test(test_load_profiles),
        cmocka_unit_test(test_load_profiles_clears_existing),
        cmocka_unit_test(test_load_profiles_null),
        cmocka_unit_test(test_load_profiles_empty_list),
        /* Find path */
        cmocka_unit_test(test_find_path),
        cmocka_unit_test(test_find_path_not_found),
        cmocka_unit_test(test_find_path_null),
        /* Apply */
        cmocka_unit_test(test_apply_success),
        cmocka_unit_test(test_apply_profile_not_in_list),
        cmocka_unit_test(test_apply_null),
        cmocka_unit_test(test_apply_no_profiles),
        cmocka_unit_test(test_apply_no_backend),
        cmocka_unit_test(test_apply_no_assignments),
        cmocka_unit_test(test_apply_backend_error),
        cmocka_unit_test(test_apply_updates_assignment_existing),
        cmocka_unit_test(test_apply_different_controllers),
        /* Update assignment */
        cmocka_unit_test(test_update_assignment_existing),
        cmocka_unit_test(test_update_assignment_new),
        cmocka_unit_test(test_update_assignment_null),
        /* Profile follows controller */
        cmocka_unit_test(test_profile_follows),
        cmocka_unit_test(test_profile_follows_null),
        cmocka_unit_test(test_profile_follows_bad_row),
        cmocka_unit_test(test_profile_follows_across_move),
        /* Full workflow */
        cmocka_unit_test(test_full_workflow),
    };

    return cmocka_run_group_tests(tests, setup, teardown);
}