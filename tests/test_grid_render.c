/*
 * test_grid_render.c — Unit tests for character select grid data model.
 *
 * Task 29 — Character select grid rendering and Player Mode navigation.
 *
 * Tests:
 *   - Init (zero state, NULL safe)
 *   - Build (basic, with assignments, without assignments, multiple composites,
 *     no composites, null args, invalid settings)
 *   - Profile management (add, find, clear, duplicates, overflow, null args)
 *   - Navigation (move_left/right, boundaries, cycle_profile_up/down,
 *     no profiles, profile not in list, null args, invalid row)
 *   - Accessors (row_count, col_count, get_row, get_col, cur_col, profile)
 *   - Slot/column conversion (col_to_slot, slot_to_col)
 *   - Render (NULL safe, with dummy SDL renderer)
 */
#include "overlay/grid_render.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <cmocka.h>

/* --- Helpers ---------------------------------------------------------- */

static cbx_settings
make_settings(int count, const char *t0, const char *t1,
              const char *t2, const char *t3)
{
    cbx_settings s;
    memset(&s, 0, sizeof(s));
    s.virtual_controllers.count = count;
    if (t0) strncpy(s.virtual_controllers.types[0], t0, CBX_MAX_TYPE_LEN - 1);
    if (t1) strncpy(s.virtual_controllers.types[1], t1, CBX_MAX_TYPE_LEN - 1);
    if (t2) strncpy(s.virtual_controllers.types[2], t2, CBX_MAX_TYPE_LEN - 1);
    if (t3) strncpy(s.virtual_controllers.types[3], t3, CBX_MAX_TYPE_LEN - 1);
    return s;
}


static void
make_composite(cbx_grid_composite_info *c, const char *id,
               const char *name, const char *path)
{
    memset(c, 0, sizeof(*c));
    if (id) strncpy(c->id, id, CBX_MAX_ID_LEN - 1);
    if (name) strncpy(c->model_name, name, CBX_MAX_NAME_LEN - 1);
    if (path) strncpy(c->composite_path, path, CBX_MAX_PATH_LEN - 1);
}

static cbx_assignments
make_assignments(void)
{
    cbx_assignments a;
    cbx_assignments_init(&a);
    return a;
}

static void
add_assignment(cbx_assignments *a, const char *id, int slot,
               const char *profile)
{
    int i = a->assignment_count++;
    strncpy(a->assignments[i].id, id, CBX_MAX_ID_LEN - 1);
    a->assignments[i].slot = slot;
    strncpy(a->assignments[i].profile, profile, CBX_MAX_PROFILE_LEN - 1);
}

/* --- Init tests ------------------------------------------------------ */

static void
test_init_zero(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_select_grid_init(&g);
    assert_int_equal(g.row_count, 0);
    assert_int_equal(g.col_count, 0);
    assert_int_equal(g.profile_count, 0);
}

static void
test_init_null(void **state)
{
    (void)state;
    cbx_select_grid_init(NULL);  /* should not crash */
}

/* --- Build tests ----------------------------------------------------- */

static void
test_build_basic(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_grid_composite_info comps[2];
    make_composite(&comps[0], "BT:AB:CD:01:02:03:04", "8BitDo Ultimate", "/org/shadowblip/InputPlumber/CompositeDevice0");
    make_composite(&comps[1], "USB:SN12345", "Xbox Series", "/org/shadowblip/InputPlumber/CompositeDevice1");
    cbx_settings s = make_settings(4, "xb360", "xb360", "xb360", "xb360");
    cbx_assignments a = make_assignments();

    int rc = cbx_select_grid_build(&g, comps, 2, &s, &a);
    assert_int_equal(rc, 0);
    assert_int_equal(g.row_count, 2);
    assert_int_equal(g.col_count, 5);  /* 4 virtual + 1 Unassigned */

    /* Row 0: no assignment → Unassigned, default profile. */
    assert_int_equal(g.rows[0].cur_col, 0);
    assert_string_equal(g.rows[0].profile, "default");
    assert_string_equal(g.rows[0].model_name, "8BitDo Ultimate");

    /* Row 1: no assignment → Unassigned, default profile. */
    assert_int_equal(g.rows[1].cur_col, 0);
    assert_string_equal(g.rows[1].profile, "default");
}

static void
test_build_with_assignments(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_grid_composite_info comps[2];
    make_composite(&comps[0], "BT:AB:CD:01:02:03:04", "8BitDo", "/comp/0");
    make_composite(&comps[1], "USB:SN12345", "Xbox", "/comp/1");
    cbx_settings s = make_settings(4, "xb360", "ds5", "deck", "gamepad");
    cbx_assignments a = make_assignments();
    add_assignment(&a, "BT:AB:CD:01:02:03:04", 0, "fighting");
    add_assignment(&a, "USB:SN12345", 2, "racing");

    int rc = cbx_select_grid_build(&g, comps, 2, &s, &a);
    assert_int_equal(rc, 0);

    /* Row 0: slot 0 → col 1 (P1), profile "fighting". */
    assert_int_equal(g.rows[0].cur_col, 1);
    assert_string_equal(g.rows[0].profile, "fighting");

    /* Row 1: slot 2 → col 3 (P3), profile "racing". */
    assert_int_equal(g.rows[1].cur_col, 3);
    assert_string_equal(g.rows[1].profile, "racing");
}

static void
test_build_no_composites(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_settings s = make_settings(2, "xb360", "ds5", NULL, NULL);
    cbx_assignments a = make_assignments();

    int rc = cbx_select_grid_build(&g, NULL, 0, &s, &a);
    assert_int_equal(rc, 0);
    assert_int_equal(g.row_count, 0);
    assert_int_equal(g.col_count, 3);  /* 2 virtual + 1 Unassigned */
}

static void
test_build_null_args(void **state)
{
    (void)state;
    cbx_grid_composite_info comps[1];
    make_composite(&comps[0], "ORDER:0", "Ctrl", "/c/0");
    cbx_settings s = make_settings(1, "xb360", NULL, NULL, NULL);
    cbx_assignments a = make_assignments();

    cbx_select_grid g;
    assert_int_equal(cbx_select_grid_build(NULL, comps, 1, &s, &a), -EINVAL);
    assert_int_equal(cbx_select_grid_build(&g, NULL, 1, &s, &a), -EINVAL);
    assert_int_equal(cbx_select_grid_build(&g, comps, 1, NULL, &a), -EINVAL);
    assert_int_equal(cbx_select_grid_build(&g, comps, 1, &s, NULL), -EINVAL);
}

static void
test_build_invalid_count(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_grid_composite_info comps[1];
    make_composite(&comps[0], "ORDER:0", "Ctrl", "/c/0");
    cbx_settings s = make_settings(2, "xb360", "ds5", NULL, NULL);
    cbx_assignments a = make_assignments();

    assert_int_equal(cbx_select_grid_build(&g, comps, -1, &s, &a), -EINVAL);
}

static void
test_build_column_types(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_grid_composite_info comps[1];
    make_composite(&comps[0], "ORDER:0", "Ctrl", "/c/0");
    cbx_settings s = make_settings(3, "xb360", "ds5", "deck", NULL);
    cbx_assignments a = make_assignments();

    int rc = cbx_select_grid_build(&g, comps, 1, &s, &a);
    assert_int_equal(rc, 0);
    assert_int_equal(g.col_count, 4);

    /* Col 0 = Unassigned (empty type). */
    assert_string_equal(g.cols[0].device_type, "");
    /* Col 1 = P1 = xb360. */
    assert_string_equal(g.cols[1].device_type, "xb360");
    /* Col 2 = P2 = ds5. */
    assert_string_equal(g.cols[2].device_type, "ds5");
    /* Col 3 = P3 = deck. */
    assert_string_equal(g.cols[3].device_type, "deck");
}

static void
test_build_slot_out_of_range(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_grid_composite_info comps[1];
    make_composite(&comps[0], "ORDER:0", "Ctrl", "/c/0");
    cbx_settings s = make_settings(2, "xb360", "ds5", NULL, NULL);  /* 2 virtual → 3 cols */
    cbx_assignments a = make_assignments();
    add_assignment(&a, "ORDER:0", 5, "fighting");  /* slot 5, but only 2 virtual */

    int rc = cbx_select_grid_build(&g, comps, 1, &s, &a);
    assert_int_equal(rc, 0);
    /* Slot 5 → col 6, but col_count is 3, so stays Unassigned. */
    assert_int_equal(g.rows[0].cur_col, 0);
    /* But profile is still set from assignment. */
    assert_string_equal(g.rows[0].profile, "fighting");
}

/* --- Profile management tests ---------------------------------------- */

static void
test_add_profile(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_select_grid_init(&g);

    assert_int_equal(cbx_select_grid_add_profile(&g, "default"), 0);
    assert_int_equal(cbx_select_grid_add_profile(&g, "fighting"), 0);
    assert_int_equal(g.profile_count, 2);
    assert_int_equal(cbx_select_grid_find_profile(&g, "default"), 0);
    assert_int_equal(cbx_select_grid_find_profile(&g, "fighting"), 1);
}

static void
test_add_profile_duplicate(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_select_grid_init(&g);

    cbx_select_grid_add_profile(&g, "default");
    int rc = cbx_select_grid_add_profile(&g, "default");
    assert_int_equal(rc, 0);  /* idempotent */
    assert_int_equal(g.profile_count, 1);
}

static void
test_add_profile_null_args(void **state)
{
    (void)state;
    assert_int_equal(cbx_select_grid_add_profile(NULL, "p"), -EINVAL);
    cbx_select_grid g;
    cbx_select_grid_init(&g);
    assert_int_equal(cbx_select_grid_add_profile(&g, NULL), -EINVAL);
    assert_int_equal(cbx_select_grid_add_profile(&g, ""), -EINVAL);
}

static void
test_clear_profiles(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_select_grid_init(&g);
    cbx_select_grid_add_profile(&g, "a");
    cbx_select_grid_add_profile(&g, "b");
    assert_int_equal(g.profile_count, 2);

    cbx_select_grid_clear_profiles(&g);
    assert_int_equal(g.profile_count, 0);
}

static void
test_find_profile_not_found(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_select_grid_init(&g);
    assert_int_equal(cbx_select_grid_find_profile(&g, "nope"), -1);
}

/* --- Navigation tests ------------------------------------------------ */

static void
test_move_left(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_grid_composite_info comps[1];
    make_composite(&comps[0], "ORDER:0", "Ctrl", "/c/0");
    cbx_settings s = make_settings(4, "xb360", "xb360", "xb360", "xb360");
    cbx_assignments a = make_assignments();
    add_assignment(&a, "ORDER:0", 2, "default");  /* slot 2 → col 3 */
    cbx_select_grid_build(&g, comps, 1, &s, &a);

    assert_int_equal(g.rows[0].cur_col, 3);

    int rc = cbx_select_grid_move_left(&g, 0);
    assert_int_equal(rc, 0);
    assert_int_equal(g.rows[0].cur_col, 2);

    cbx_select_grid_move_left(&g, 0);
    cbx_select_grid_move_left(&g, 0);
    assert_int_equal(g.rows[0].cur_col, 0);

    /* Can't go past Unassigned. */
    rc = cbx_select_grid_move_left(&g, 0);
    assert_int_equal(rc, -ERANGE);
    assert_int_equal(g.rows[0].cur_col, 0);
}

static void
test_move_right(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_grid_composite_info comps[1];
    make_composite(&comps[0], "ORDER:0", "Ctrl", "/c/0");
    cbx_settings s = make_settings(2, "xb360", "ds5", NULL, NULL);  /* 3 cols (0,1,2) */
    cbx_assignments a = make_assignments();
    cbx_select_grid_build(&g, comps, 1, &s, &a);

    assert_int_equal(g.rows[0].cur_col, 0);

    int rc = cbx_select_grid_move_right(&g, 0);
    assert_int_equal(rc, 0);
    assert_int_equal(g.rows[0].cur_col, 1);

    rc = cbx_select_grid_move_right(&g, 0);
    assert_int_equal(rc, 0);
    assert_int_equal(g.rows[0].cur_col, 2);

    /* Can't go past last column. */
    rc = cbx_select_grid_move_right(&g, 0);
    assert_int_equal(rc, -ERANGE);
    assert_int_equal(g.rows[0].cur_col, 2);
}

static void
test_move_null_args(void **state)
{
    (void)state;
    assert_int_equal(cbx_select_grid_move_left(NULL, 0), -EINVAL);
    assert_int_equal(cbx_select_grid_move_right(NULL, 0), -EINVAL);

    cbx_select_grid g;
    cbx_select_grid_init(&g);
    assert_int_equal(cbx_select_grid_move_left(&g, 0), -EINVAL);
    assert_int_equal(cbx_select_grid_move_left(&g, -1), -EINVAL);
}

static void
test_cycle_profile_up(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_grid_composite_info comps[1];
    make_composite(&comps[0], "ORDER:0", "Ctrl", "/c/0");
    cbx_settings s = make_settings(1, "xb360", NULL, NULL, NULL);
    cbx_assignments a = make_assignments();
    cbx_select_grid_build(&g, comps, 1, &s, &a);
    cbx_select_grid_add_profile(&g, "default");
    cbx_select_grid_add_profile(&g, "fighting");
    cbx_select_grid_add_profile(&g, "racing");

    assert_string_equal(g.rows[0].profile, "default");

    int rc = cbx_select_grid_cycle_profile_up(&g, 0);
    assert_int_equal(rc, 0);
    assert_string_equal(g.rows[0].profile, "racing");  /* wraps around */

    cbx_select_grid_cycle_profile_up(&g, 0);
    assert_string_equal(g.rows[0].profile, "fighting");

    cbx_select_grid_cycle_profile_up(&g, 0);
    assert_string_equal(g.rows[0].profile, "default");
}

static void
test_cycle_profile_down(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_grid_composite_info comps[1];
    make_composite(&comps[0], "ORDER:0", "Ctrl", "/c/0");
    cbx_settings s = make_settings(1, "xb360", NULL, NULL, NULL);
    cbx_assignments a = make_assignments();
    cbx_select_grid_build(&g, comps, 1, &s, &a);
    cbx_select_grid_add_profile(&g, "default");
    cbx_select_grid_add_profile(&g, "fighting");
    cbx_select_grid_add_profile(&g, "racing");

    assert_string_equal(g.rows[0].profile, "default");

    int rc = cbx_select_grid_cycle_profile_down(&g, 0);
    assert_int_equal(rc, 0);
    assert_string_equal(g.rows[0].profile, "fighting");

    cbx_select_grid_cycle_profile_down(&g, 0);
    assert_string_equal(g.rows[0].profile, "racing");

    cbx_select_grid_cycle_profile_down(&g, 0);
    assert_string_equal(g.rows[0].profile, "default");  /* wraps around */
}

static void
test_cycle_profile_no_profiles(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_grid_composite_info comps[1];
    make_composite(&comps[0], "ORDER:0", "Ctrl", "/c/0");
    cbx_settings s = make_settings(1, "xb360", NULL, NULL, NULL);
    cbx_assignments a = make_assignments();
    cbx_select_grid_build(&g, comps, 1, &s, &a);
    /* No profiles added. */

    assert_int_equal(cbx_select_grid_cycle_profile_up(&g, 0), -ENOENT);
    assert_int_equal(cbx_select_grid_cycle_profile_down(&g, 0), -ENOENT);
}

static void
test_cycle_profile_not_in_list(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_grid_composite_info comps[1];
    make_composite(&comps[0], "ORDER:0", "Ctrl", "/c/0");
    cbx_settings s = make_settings(1, "xb360", NULL, NULL, NULL);
    cbx_assignments a = make_assignments();
    add_assignment(&a, "ORDER:0", 0, "custom");  /* "custom" not in profile list */
    cbx_select_grid_build(&g, comps, 1, &s, &a);
    cbx_select_grid_add_profile(&g, "default");
    cbx_select_grid_add_profile(&g, "fighting");

    /* Current profile is "custom" which is not in the list. */
    assert_string_equal(g.rows[0].profile, "custom");

    /* Cycling down should start from first profile. */
    int rc = cbx_select_grid_cycle_profile_down(&g, 0);
    assert_int_equal(rc, 0);
    assert_string_equal(g.rows[0].profile, "default");
}

static void
test_cycle_profile_null_args(void **state)
{
    (void)state;
    assert_int_equal(cbx_select_grid_cycle_profile_up(NULL, 0), -EINVAL);
    assert_int_equal(cbx_select_grid_cycle_profile_down(NULL, 0), -EINVAL);
}

/* --- Accessor tests --------------------------------------------------- */

static void
test_accessors(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_grid_composite_info comps[2];
    make_composite(&comps[0], "ORDER:0", "Ctrl A", "/c/0");
    make_composite(&comps[1], "ORDER:1", "Ctrl B", "/c/1");
    cbx_settings s = make_settings(2, "xb360", "ds5", NULL, NULL);
    cbx_assignments a = make_assignments();
    add_assignment(&a, "ORDER:0", 1, "fighting");
    cbx_select_grid_build(&g, comps, 2, &s, &a);

    assert_int_equal(cbx_select_grid_get_row_count(&g), 2);
    assert_int_equal(cbx_select_grid_get_col_count(&g), 3);

    const cbx_grid_row *row0 = cbx_select_grid_get_row(&g, 0);
    assert_non_null(row0);
    assert_string_equal(row0->model_name, "Ctrl A");
    assert_int_equal(row0->cur_col, 2);  /* slot 1 → col 2 */
    assert_string_equal(row0->profile, "fighting");

    const cbx_grid_row *row1 = cbx_select_grid_get_row(&g, 1);
    assert_non_null(row1);
    assert_int_equal(row1->cur_col, 0);  /* no assignment → Unassigned */

    const cbx_grid_col *col0 = cbx_select_grid_get_col(&g, 0);
    assert_non_null(col0);
    assert_string_equal(col0->device_type, "");

    const cbx_grid_col *col1 = cbx_select_grid_get_col(&g, 1);
    assert_non_null(col1);
    assert_string_equal(col1->device_type, "xb360");

    assert_int_equal(cbx_select_grid_get_cur_col(&g, 0), 2);
    assert_int_equal(cbx_select_grid_get_cur_col(&g, 1), 0);

    assert_string_equal(cbx_select_grid_get_profile(&g, 0), "fighting");
    assert_string_equal(cbx_select_grid_get_profile(&g, 1), "default");
}

static void
test_accessors_null(void **state)
{
    (void)state;
    assert_int_equal(cbx_select_grid_get_row_count(NULL), 0);
    assert_int_equal(cbx_select_grid_get_col_count(NULL), 0);
    assert_null(cbx_select_grid_get_row(NULL, 0));
    assert_null(cbx_select_grid_get_col(NULL, 0));
    assert_int_equal(cbx_select_grid_get_cur_col(NULL, 0), -EINVAL);
    assert_null(cbx_select_grid_get_profile(NULL, 0));
}

static void
test_accessors_out_of_range(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_select_grid_init(&g);
    assert_null(cbx_select_grid_get_row(&g, 0));
    assert_null(cbx_select_grid_get_col(&g, 0));
    assert_int_equal(cbx_select_grid_get_cur_col(&g, 0), -EINVAL);
    assert_null(cbx_select_grid_get_profile(&g, 0));
}

/* --- Slot/column conversion tests ----------------------------------- */

static void
test_col_slot_conversion(void **state)
{
    (void)state;
    /* col 0 = Unassigned → slot -1. */
    assert_int_equal(cbx_select_grid_col_to_slot(0), -1);
    /* col 1 → slot 0 (P1). */
    assert_int_equal(cbx_select_grid_col_to_slot(1), 0);
    /* col 2 → slot 1 (P2). */
    assert_int_equal(cbx_select_grid_col_to_slot(2), 1);

    /* slot -1 → col 0 (Unassigned). */
    assert_int_equal(cbx_select_grid_slot_to_col(-1), 0);
    /* slot 0 → col 1 (P1). */
    assert_int_equal(cbx_select_grid_slot_to_col(0), 1);
    /* slot 3 → col 4 (P4). */
    assert_int_equal(cbx_select_grid_slot_to_col(3), 4);
}

/* --- Render tests ---------------------------------------------------- */

static void
test_render_null_safe(void **state)
{
    (void)state;
    /* NULL renderer → no-op. */
    cbx_grid_render_ctx ctx = {0};
    assert_int_equal(cbx_select_grid_render(NULL, NULL, &ctx), 0);

    /* NULL ctx → no-op. */
    assert_int_equal(cbx_select_grid_render(NULL, NULL, NULL), 0);

    /* Grid but no renderer → no-op. */
    cbx_select_grid g;
    cbx_select_grid_init(&g);
    ctx.grid = &g;
    assert_int_equal(cbx_select_grid_render(NULL, NULL, &ctx), 0);
}

static void
test_render_callback(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_select_grid_init(&g);
    cbx_grid_render_ctx ctx = { .grid = &g };

    /* NULL renderer → no-op via callback. */
    assert_int_equal(cbx_select_grid_render_cb(NULL, NULL, &ctx), 0);
    assert_int_equal(cbx_select_grid_render_cb(NULL, NULL, NULL), 0);
}

static void
test_render_with_dummy(void **state)
{
    (void)state;
    /* Initialize SDL with dummy video driver. */
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        skip();
        return;
    }
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");

    SDL_Window *win = SDL_CreateWindow("test", 0, 0, 800, 600,
                                        SDL_WINDOW_HIDDEN);
    if (!win) {
        SDL_Quit();
        skip();
        return;
    }
    SDL_Renderer *r = SDL_CreateRenderer(win, -1,
                                          SDL_RENDERER_ACCELERATED |
                                          SDL_RENDERER_TARGETTEXTURE);
    if (!r) {
        SDL_DestroyWindow(win);
        SDL_Quit();
        skip();
        return;
    }

    /* Build a minimal grid. */
    cbx_select_grid g;
    cbx_grid_composite_info comps[2];
    make_composite(&comps[0], "ORDER:0", "Ctrl A", "/c/0");
    make_composite(&comps[1], "ORDER:1", "Ctrl B", "/c/1");
    cbx_settings s = make_settings(2, "xb360", "ds5", NULL, NULL);
    cbx_assignments a = make_assignments();
    add_assignment(&a, "ORDER:0", 0, "default");
    cbx_select_grid_build(&g, comps, 2, &s, &a);

    /* Render without icon/text cache (should not crash). */
    cbx_grid_render_ctx ctx = { .grid = &g };
    SDL_Rect clip = {0, 0, 800, 600};
    int rc = cbx_select_grid_render(r, &clip, &ctx);
    assert_int_equal(rc, 0);

    /* Render with clip NULL (uses default area). */
    rc = cbx_select_grid_render(r, NULL, &ctx);
    assert_int_equal(rc, 0);

    /* Render empty grid (no rows/cols). */
    cbx_select_grid empty;
    cbx_select_grid_init(&empty);
    ctx.grid = &empty;
    rc = cbx_select_grid_render(r, &clip, &ctx);
    assert_int_equal(rc, 0);

    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(win);
    SDL_Quit();
}

/* --- Main ------------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* Init */
        cmocka_unit_test(test_init_zero),
        cmocka_unit_test(test_init_null),
        /* Build */
        cmocka_unit_test(test_build_basic),
        cmocka_unit_test(test_build_with_assignments),
        cmocka_unit_test(test_build_no_composites),
        cmocka_unit_test(test_build_null_args),
        cmocka_unit_test(test_build_invalid_count),
        cmocka_unit_test(test_build_column_types),
        cmocka_unit_test(test_build_slot_out_of_range),
        /* Profile management */
        cmocka_unit_test(test_add_profile),
        cmocka_unit_test(test_add_profile_duplicate),
        cmocka_unit_test(test_add_profile_null_args),
        cmocka_unit_test(test_clear_profiles),
        cmocka_unit_test(test_find_profile_not_found),
        /* Navigation */
        cmocka_unit_test(test_move_left),
        cmocka_unit_test(test_move_right),
        cmocka_unit_test(test_move_null_args),
        cmocka_unit_test(test_cycle_profile_up),
        cmocka_unit_test(test_cycle_profile_down),
        cmocka_unit_test(test_cycle_profile_no_profiles),
        cmocka_unit_test(test_cycle_profile_not_in_list),
        cmocka_unit_test(test_cycle_profile_null_args),
        /* Accessors */
        cmocka_unit_test(test_accessors),
        cmocka_unit_test(test_accessors_null),
        cmocka_unit_test(test_accessors_out_of_range),
        /* Conversion */
        cmocka_unit_test(test_col_slot_conversion),
        /* Render */
        cmocka_unit_test(test_render_null_safe),
        cmocka_unit_test(test_render_callback),
        cmocka_unit_test(test_render_with_dummy),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}