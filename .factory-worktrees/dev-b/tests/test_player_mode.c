/*
 * test_player_mode.c — Unit tests for Player Mode navigation.
 *
 * Task 29 — Character select grid rendering and Player Mode navigation.
 *
 * Tests:
 *   - Init (grid pointer, defaults)
 *   - Handle LEFT (moves left, fires slot change callback)
 *   - Handle RIGHT (moves right, fires slot change callback)
 *   - Handle UP/DOWN (cycles profile, fires profile change callback)
 *   - Handle B (returns CLOSE)
 *   - Handle R3 (returns HOST)
 *   - Handle invalid args (NULL, bad row_idx)
 *   - No callbacks (NULL safe)
 *   - Independence: controller 0 can't affect controller 1's row
 *   - Boundary handling (can't move past edges)
 *   - Get slot / get profile accessors
 */
#include "overlay/player_mode.h"
#include "overlay/grid_render.h"

#include <errno.h>
#include <string.h>

#include <cmocka.h>

/* --- Callback tracking ------------------------------------------------ */

typedef struct {
    int slot_change_count;
    int slot_change_row;
    int slot_change_slot;
    int profile_change_count;
    int profile_change_row;
    char profile_change_profile[CBX_GRID_PROFILE_LEN];
    char profile_change_path[CBX_MAX_PATH_LEN];
} pm_callbacks;

static int
on_slot_change(int row_idx, int new_slot, void *userdata)
{
    pm_callbacks *cb = (pm_callbacks *)userdata;
    if (cb) {
        cb->slot_change_count++;
        cb->slot_change_row = row_idx;
        cb->slot_change_slot = new_slot;
    }
    return 0;
}

static int
on_profile_change(int row_idx, const char *profile,
                  const char *composite_path, void *userdata)
{
    pm_callbacks *cb = (pm_callbacks *)userdata;
    if (cb) {
        cb->profile_change_count++;
        cb->profile_change_row = row_idx;
        strncpy(cb->profile_change_profile, profile,
                 CBX_GRID_PROFILE_LEN - 1);
        cb->profile_change_profile[CBX_GRID_PROFILE_LEN - 1] = '\0';
        strncpy(cb->profile_change_path, composite_path,
                 CBX_MAX_PATH_LEN - 1);
        cb->profile_change_path[CBX_MAX_PATH_LEN - 1] = '\0';
    }
    return 0;
}

/* --- Helpers ---------------------------------------------------------- */

static cbx_grid_composite_info
make_comp(const char *id, const char *name, const char *path)
{
    cbx_grid_composite_info c;
    memset(&c, 0, sizeof(c));
    if (id) strncpy(c.id, id, CBX_MAX_ID_LEN - 1);
    if (name) strncpy(c.model_name, name, CBX_MAX_NAME_LEN - 1);
    if (path) strncpy(c.composite_path, path, CBX_MAX_PATH_LEN - 1);
    return c;
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
        strncpy(s.virtual_controllers.types[i], "xb360", CBX_MAX_TYPE_LEN - 1);

    cbx_assignments a;
    cbx_assignments_init(&a);

    cbx_select_grid_build(g, comps, rows, &s, &a);

    /* Add profiles. */
    cbx_select_grid_add_profile(g, "default");
    cbx_select_grid_add_profile(g, "fighting");
    cbx_select_grid_add_profile(g, "racing");
}

/* --- Init tests ------------------------------------------------------ */

static void
test_init(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_select_grid_init(&g);
    cbx_player_mode pm;
    cbx_player_mode_init(&pm, &g);
    assert_ptr_equal(pm.grid, &g);
    assert_null(pm.on_slot_change);
    assert_null(pm.on_profile_change);
}

static void
test_init_null(void **state)
{
    (void)state;
    cbx_player_mode pm;
    cbx_player_mode_init(&pm, NULL);
    assert_null(pm.grid);
}

/* --- LEFT tests ------------------------------------------------------ */

static void
test_handle_left(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 1);
    /* Set initial position to col 3 (P3). */
    g.rows[0].cur_col = 3;

    pm_callbacks cbs = {0};
    cbx_player_mode pm;
    cbx_player_mode_init(&pm, &g);
    pm.on_slot_change = on_slot_change;
    pm.slot_change_data = &cbs;

    int rc = cbx_player_mode_handle(&pm, 0, CBX_PM_LEFT);
    assert_int_equal(rc, CBX_PM_RESULT_MOVED);
    assert_int_equal(g.rows[0].cur_col, 2);
    assert_int_equal(cbs.slot_change_count, 1);
    assert_int_equal(cbs.slot_change_row, 0);
    assert_int_equal(cbs.slot_change_slot, 1);  /* col 2 → slot 1 (P2) */
}

static void
test_handle_left_at_boundary(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 1);
    g.rows[0].cur_col = 0;  /* already at Unassigned */

    pm_callbacks cbs = {0};
    cbx_player_mode pm;
    cbx_player_mode_init(&pm, &g);
    pm.on_slot_change = on_slot_change;
    pm.slot_change_data = &cbs;

    int rc = cbx_player_mode_handle(&pm, 0, CBX_PM_LEFT);
    assert_int_equal(rc, CBX_PM_RESULT_NONE);
    assert_int_equal(g.rows[0].cur_col, 0);
    assert_int_equal(cbs.slot_change_count, 0);  /* no callback fired */
}

/* --- RIGHT tests ----------------------------------------------------- */

static void
test_handle_right(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 1);
    g.rows[0].cur_col = 0;  /* at Unassigned */

    pm_callbacks cbs = {0};
    cbx_player_mode pm;
    cbx_player_mode_init(&pm, &g);
    pm.on_slot_change = on_slot_change;
    pm.slot_change_data = &cbs;

    int rc = cbx_player_mode_handle(&pm, 0, CBX_PM_RIGHT);
    assert_int_equal(rc, CBX_PM_RESULT_MOVED);
    assert_int_equal(g.rows[0].cur_col, 1);
    assert_int_equal(cbs.slot_change_count, 1);
    assert_int_equal(cbs.slot_change_slot, 0);  /* col 1 → slot 0 (P1) */
}

static void
test_handle_right_at_boundary(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 1);
    g.rows[0].cur_col = 4;  /* at last column (col_count = 5) */

    pm_callbacks cbs = {0};
    cbx_player_mode pm;
    cbx_player_mode_init(&pm, &g);
    pm.on_slot_change = on_slot_change;
    pm.slot_change_data = &cbs;

    int rc = cbx_player_mode_handle(&pm, 0, CBX_PM_RIGHT);
    assert_int_equal(rc, CBX_PM_RESULT_NONE);
    assert_int_equal(g.rows[0].cur_col, 4);
    assert_int_equal(cbs.slot_change_count, 0);
}

/* --- UP/DOWN tests --------------------------------------------------- */

static void
test_handle_up(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 1);
    /* Default profile is "default". */

    pm_callbacks cbs = {0};
    cbx_player_mode pm;
    cbx_player_mode_init(&pm, &g);
    pm.on_profile_change = on_profile_change;
    pm.profile_change_data = &cbs;

    int rc = cbx_player_mode_handle(&pm, 0, CBX_PM_UP);
    assert_int_equal(rc, CBX_PM_RESULT_PROFILE);
    /* Up wraps: default → racing (last in list). */
    assert_string_equal(g.rows[0].profile, "racing");
    assert_int_equal(cbs.profile_change_count, 1);
    assert_int_equal(cbs.profile_change_row, 0);
    assert_string_equal(cbs.profile_change_profile, "racing");
}

static void
test_handle_down(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 1);

    pm_callbacks cbs = {0};
    cbx_player_mode pm;
    cbx_player_mode_init(&pm, &g);
    pm.on_profile_change = on_profile_change;
    pm.profile_change_data = &cbs;

    int rc = cbx_player_mode_handle(&pm, 0, CBX_PM_DOWN);
    assert_int_equal(rc, CBX_PM_RESULT_PROFILE);
    /* Down: default → fighting. */
    assert_string_equal(g.rows[0].profile, "fighting");
    assert_int_equal(cbs.profile_change_count, 1);
    assert_string_equal(cbs.profile_change_profile, "fighting");
}

static void
test_handle_down_wrap(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 1);
    /* Set to last profile. */
    strncpy(g.rows[0].profile, "racing", CBX_GRID_PROFILE_LEN - 1);

    pm_callbacks cbs = {0};
    cbx_player_mode pm;
    cbx_player_mode_init(&pm, &g);
    pm.on_profile_change = on_profile_change;
    pm.profile_change_data = &cbs;

    int rc = cbx_player_mode_handle(&pm, 0, CBX_PM_DOWN);
    assert_int_equal(rc, CBX_PM_RESULT_PROFILE);
    /* Down wraps: racing → default. */
    assert_string_equal(g.rows[0].profile, "default");
}

static void
test_handle_up_no_profiles(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 1);
    cbx_select_grid_clear_profiles(&g);

    cbx_player_mode pm;
    cbx_player_mode_init(&pm, &g);

    int rc = cbx_player_mode_handle(&pm, 0, CBX_PM_UP);
    assert_int_equal(rc, CBX_PM_RESULT_NONE);

    rc = cbx_player_mode_handle(&pm, 0, CBX_PM_DOWN);
    assert_int_equal(rc, CBX_PM_RESULT_NONE);
}

/* --- B / R3 tests ---------------------------------------------------- */

static void
test_handle_b(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 1);
    cbx_player_mode pm;
    cbx_player_mode_init(&pm, &g);

    int rc = cbx_player_mode_handle(&pm, 0, CBX_PM_B);
    assert_int_equal(rc, CBX_PM_RESULT_CLOSE);
}

static void
test_handle_r3(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 1);
    cbx_player_mode pm;
    cbx_player_mode_init(&pm, &g);

    int rc = cbx_player_mode_handle(&pm, 0, CBX_PM_R3);
    assert_int_equal(rc, CBX_PM_RESULT_HOST);
}

/* --- Independence tests ----------------------------------------------- */

static void
test_independence(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    /* Set different positions for each row. */
    g.rows[0].cur_col = 1;
    g.rows[1].cur_col = 3;
    g.rows[2].cur_col = 2;

    cbx_player_mode pm;
    cbx_player_mode_init(&pm, &g);

    /* Move controller 0 right — should only affect row 0. */
    int rc = cbx_player_mode_handle(&pm, 0, CBX_PM_RIGHT);
    assert_int_equal(rc, CBX_PM_RESULT_MOVED);
    assert_int_equal(g.rows[0].cur_col, 2);
    assert_int_equal(g.rows[1].cur_col, 3);  /* unchanged */
    assert_int_equal(g.rows[2].cur_col, 2);  /* unchanged */

    /* Move controller 1 left — should only affect row 1. */
    rc = cbx_player_mode_handle(&pm, 1, CBX_PM_LEFT);
    assert_int_equal(rc, CBX_PM_RESULT_MOVED);
    assert_int_equal(g.rows[0].cur_col, 2);  /* unchanged */
    assert_int_equal(g.rows[1].cur_col, 2);  /* moved */
    assert_int_equal(g.rows[2].cur_col, 2);  /* unchanged */

    /* Cycle profile on controller 2 — should only affect row 2. */
    rc = cbx_player_mode_handle(&pm, 2, CBX_PM_DOWN);
    assert_int_equal(rc, CBX_PM_RESULT_PROFILE);
    assert_string_equal(g.rows[2].profile, "fighting");
    assert_string_equal(g.rows[0].profile, "default");  /* unchanged */
    assert_string_equal(g.rows[1].profile, "default");  /* unchanged */
}

static void
test_slot_change_callback_unassigned(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 1);
    g.rows[0].cur_col = 1;  /* at P1 */

    pm_callbacks cbs = {0};
    cbx_player_mode pm;
    cbx_player_mode_init(&pm, &g);
    pm.on_slot_change = on_slot_change;
    pm.slot_change_data = &cbs;

    /* Move left to Unassigned → slot = -1. */
    int rc = cbx_player_mode_handle(&pm, 0, CBX_PM_LEFT);
    assert_int_equal(rc, CBX_PM_RESULT_MOVED);
    assert_int_equal(g.rows[0].cur_col, 0);
    assert_int_equal(cbs.slot_change_slot, -1);  /* Unassigned */
}

/* --- NULL / error tests --------------------------------------------- */

static void
test_handle_null_pm(void **state)
{
    (void)state;
    assert_int_equal(cbx_player_mode_handle(NULL, 0, CBX_PM_LEFT),
                     CBX_PM_RESULT_ERROR);
}

static void
test_handle_null_grid(void **state)
{
    (void)state;
    cbx_player_mode pm;
    cbx_player_mode_init(&pm, NULL);
    assert_int_equal(cbx_player_mode_handle(&pm, 0, CBX_PM_LEFT),
                     CBX_PM_RESULT_ERROR);
}

static void
test_handle_bad_row(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 1);
    cbx_player_mode pm;
    cbx_player_mode_init(&pm, &g);

    assert_int_equal(cbx_player_mode_handle(&pm, -1, CBX_PM_LEFT),
                     CBX_PM_RESULT_ERROR);
    assert_int_equal(cbx_player_mode_handle(&pm, 1, CBX_PM_LEFT),
                     CBX_PM_RESULT_ERROR);
}

static void
test_handle_no_callbacks(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 1);
    g.rows[0].cur_col = 1;

    cbx_player_mode pm;
    cbx_player_mode_init(&pm, &g);
    /* No callbacks set. */

    int rc = cbx_player_mode_handle(&pm, 0, CBX_PM_LEFT);
    assert_int_equal(rc, CBX_PM_RESULT_MOVED);
    assert_int_equal(g.rows[0].cur_col, 0);

    rc = cbx_player_mode_handle(&pm, 0, CBX_PM_DOWN);
    assert_int_equal(rc, CBX_PM_RESULT_PROFILE);
}

/* --- Accessor tests --------------------------------------------------- */

static void
test_get_slot(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 2);
    g.rows[0].cur_col = 3;
    g.rows[1].cur_col = 0;

    cbx_player_mode pm;
    cbx_player_mode_init(&pm, &g);

    assert_int_equal(cbx_player_mode_get_slot(&pm, 0), 2);   /* col 3 → slot 2 */
    assert_int_equal(cbx_player_mode_get_slot(&pm, 1), -1);  /* col 0 → unassigned */
}

static void
test_get_profile(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 1);
    strncpy(g.rows[0].profile, "fighting", CBX_GRID_PROFILE_LEN - 1);

    cbx_player_mode pm;
    cbx_player_mode_init(&pm, &g);

    assert_string_equal(cbx_player_mode_get_profile(&pm, 0), "fighting");
}

static void
test_get_slot_null(void **state)
{
    (void)state;
    assert_int_equal(cbx_player_mode_get_slot(NULL, 0), -1);
    cbx_player_mode pm;
    cbx_player_mode_init(&pm, NULL);
    assert_int_equal(cbx_player_mode_get_slot(&pm, 0), -1);
}

static void
test_get_profile_null(void **state)
{
    (void)state;
    assert_null(cbx_player_mode_get_profile(NULL, 0));
    cbx_player_mode pm;
    cbx_player_mode_init(&pm, NULL);
    assert_null(cbx_player_mode_get_profile(&pm, 0));
}

/* --- Main ------------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* Init */
        cmocka_unit_test(test_init),
        cmocka_unit_test(test_init_null),
        /* LEFT */
        cmocka_unit_test(test_handle_left),
        cmocka_unit_test(test_handle_left_at_boundary),
        /* RIGHT */
        cmocka_unit_test(test_handle_right),
        cmocka_unit_test(test_handle_right_at_boundary),
        /* UP/DOWN */
        cmocka_unit_test(test_handle_up),
        cmocka_unit_test(test_handle_down),
        cmocka_unit_test(test_handle_down_wrap),
        cmocka_unit_test(test_handle_up_no_profiles),
        /* B / R3 */
        cmocka_unit_test(test_handle_b),
        cmocka_unit_test(test_handle_r3),
        /* Independence */
        cmocka_unit_test(test_independence),
        cmocka_unit_test(test_slot_change_callback_unassigned),
        /* NULL / error */
        cmocka_unit_test(test_handle_null_pm),
        cmocka_unit_test(test_handle_null_grid),
        cmocka_unit_test(test_handle_bad_row),
        cmocka_unit_test(test_handle_no_callbacks),
        /* Accessors */
        cmocka_unit_test(test_get_slot),
        cmocka_unit_test(test_get_profile),
        cmocka_unit_test(test_get_slot_null),
        cmocka_unit_test(test_get_profile_null),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}