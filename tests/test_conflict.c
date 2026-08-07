/*
 * test_conflict.c — Unit tests for conflict detection and resolution.
 *
 * Task 30 — Host Mode and conflict detection/resolution.
 *
 * Tests:
 *   - List init
 *   - Detect: no conflicts (all different columns)
 *   - Detect: two controllers same column (second arrival conflicted)
 *   - Detect: multiple conflicts
 *   - Detect: Unassigned (col 0) is never a conflict
 *   - Detect: all on same column
 *   - Detect: empty grid, single row
 *   - Detect: NULL safety
 *   - is_row_conflicted
 *   - find_lowest_free_slot (all free, some occupied, all occupied, excluded row)
 *   - count_occupied
 *   - Resolve: move to lowest free slot
 *   - Resolve: all occupied (leave in place)
 *   - Resolve: conflicted on Unassigned (no conflict, skipped)
 *   - Resolve: multiple conflicts (sequential)
 *   - Resolve: NULL safety
 *   - Resolve: resolution eliminates further conflict
 */
#include "overlay/conflict.h"
#include "overlay/grid_render.h"
#include "overlay/surface_build.h"
#include "identify/assign.h"
#include "config/config_settings.h"
#include "fb_assert.h"
#include "test_harness.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include <SDL2/SDL.h>
#include <cmocka.h>

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
}

/* Move a row to a specific column by repeated moves */
static void
move_to_col(cbx_select_grid *g, int row_idx, int target_col)
{
    int cur = cbx_select_grid_get_cur_col(g, row_idx);
    while (cur < target_col) {
        cbx_select_grid_move_right(g, row_idx);
        cur++;
    }
    while (cur > target_col) {
        cbx_select_grid_move_left(g, row_idx);
        cur--;
    }
}

/* --- List init ------------------------------------------------------- */

static void
test_list_init(void **state)
{
    (void)state;
    cbx_conflict_list list;
    cbx_conflict_list_init(&list);
    assert_int_equal(list.count, 0);
}

static void
test_list_init_null(void **state)
{
    (void)state;
    cbx_conflict_list_init(NULL); /* should not crash */
}

/* --- Detect: no conflicts ------------------------------------------- */

static void
test_detect_no_conflicts(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    /* All rows start on col 0 (Unassigned) — no conflicts */
    cbx_conflict_list list;
    int rc = cbx_conflict_detect(&g, &list);
    assert_int_equal(rc, 0);
    assert_int_equal(list.count, 0);
}

static void
test_detect_all_different(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    /* Row 0 → col 1 (P1), Row 1 → col 2 (P2), Row 2 → col 3 (P3) */
    move_to_col(&g, 0, 1);
    move_to_col(&g, 1, 2);
    move_to_col(&g, 2, 3);

    cbx_conflict_list list;
    cbx_conflict_detect(&g, &list);
    assert_int_equal(list.count, 0);
}

/* --- Detect: conflicts ---------------------------------------------- */

static void
test_detect_two_same_column(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    /* Row 0 → col 1, Row 1 → col 1 (conflict: row 1 is second arrival) */
    move_to_col(&g, 0, 1);
    move_to_col(&g, 1, 1);

    cbx_conflict_list list;
    cbx_conflict_detect(&g, &list);
    assert_int_equal(list.count, 1);
    assert_int_equal(list.conflicts[0].row_idx, 1);
    assert_int_equal(list.conflicts[0].conflicting_col, 1);
}

static void
test_detect_multiple_conflicts(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 4);
    /* Row 0 → col 1, Row 1 → col 1 (conflict), Row 2 → col 2, Row 3 → col 2 (conflict) */
    move_to_col(&g, 0, 1);
    move_to_col(&g, 1, 1);
    move_to_col(&g, 2, 2);
    move_to_col(&g, 3, 2);

    cbx_conflict_list list;
    cbx_conflict_detect(&g, &list);
    assert_int_equal(list.count, 2);
    assert_int_equal(list.conflicts[0].row_idx, 1);
    assert_int_equal(list.conflicts[0].conflicting_col, 1);
    assert_int_equal(list.conflicts[1].row_idx, 3);
    assert_int_equal(list.conflicts[1].conflicting_col, 2);
}

static void
test_detect_three_same_column(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    move_to_col(&g, 0, 1);
    move_to_col(&g, 1, 1);
    move_to_col(&g, 2, 1);

    cbx_conflict_list list;
    cbx_conflict_detect(&g, &list);
    /* Row 0 is first, rows 1 and 2 are second arrivals */
    assert_int_equal(list.count, 2);
    assert_int_equal(list.conflicts[0].row_idx, 1);
    assert_int_equal(list.conflicts[1].row_idx, 2);
}

/* --- Detect: Unassigned never a conflict ---------------------------- */

static void
test_detect_unassigned_no_conflict(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    /* All on col 0 (Unassigned) — not a conflict */
    cbx_conflict_list list;
    cbx_conflict_detect(&g, &list);
    assert_int_equal(list.count, 0);
}

static void
test_detect_unassigned_and_occupied(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    /* Row 0 → col 0 (Unassigned), Row 1 → col 0, Row 2 → col 1 */
    /* Rows 0 and 1 on Unassigned — no conflict. Row 2 alone on col 1. */
    cbx_conflict_list list;
    cbx_conflict_detect(&g, &list);
    assert_int_equal(list.count, 0);
}

/* --- Detect: edge cases --------------------------------------------- */

static void
test_detect_empty_grid(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_select_grid_init(&g);
    cbx_conflict_list list;
    int rc = cbx_conflict_detect(&g, &list);
    assert_int_equal(rc, 0);
    assert_int_equal(list.count, 0);
}

static void
test_detect_single_row(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 1);
    move_to_col(&g, 0, 2);
    cbx_conflict_list list;
    cbx_conflict_detect(&g, &list);
    assert_int_equal(list.count, 0);
}

static void
test_detect_null(void **state)
{
    (void)state;
    cbx_conflict_list list;
    int rc = cbx_conflict_detect(NULL, &list);
    assert_int_equal(rc, -EINVAL);
}

static void
test_detect_null_out(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 2);
    int rc = cbx_conflict_detect(&g, NULL);
    assert_int_equal(rc, -EINVAL);
}

/* --- is_row_conflicted ---------------------------------------------- */

static void
test_is_row_conflicted(void **state)
{
    (void)state;
    cbx_conflict_list list;
    cbx_conflict_list_init(&list);
    list.conflicts[0].row_idx = 1;
    list.conflicts[0].conflicting_col = 2;
    list.count = 1;

    assert_true(cbx_conflict_is_row_conflicted(&list, 1));
    assert_false(cbx_conflict_is_row_conflicted(&list, 0));
    assert_false(cbx_conflict_is_row_conflicted(&list, 2));
}

static void
test_is_row_conflicted_null(void **state)
{
    (void)state;
    assert_false(cbx_conflict_is_row_conflicted(NULL, 0));
}

static void
test_is_row_conflicted_empty(void **state)
{
    (void)state;
    cbx_conflict_list list;
    cbx_conflict_list_init(&list);
    assert_false(cbx_conflict_is_row_conflicted(&list, 0));
}

/* --- find_lowest_free_slot ----------------------------------------- */

static void
test_find_lowest_free_all_free(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 2);
    /* All rows on Unassigned — all P-slots free */
    int slot = cbx_conflict_find_lowest_free_slot(&g, 0);
    assert_int_equal(slot, 0); /* P1 (slot 0) is free */
}

static void
test_find_lowest_free_some_occupied(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    move_to_col(&g, 0, 1); /* Row 0 on P1 */
    move_to_col(&g, 1, 2); /* Row 1 on P2 */

    /* Excluding row 2 (on Unassigned), P1 and P2 are occupied, P3 is free */
    int slot = cbx_conflict_find_lowest_free_slot(&g, 2);
    assert_int_equal(slot, 2); /* P3 (slot 2) */
}

static void
test_find_lowest_free_all_occupied(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 4);
    /* 4 slots, 3 occupied by rows 0-2, row 3 excluded */
    move_to_col(&g, 0, 1); /* P1 */
    move_to_col(&g, 1, 2); /* P2 */
    move_to_col(&g, 2, 3); /* P3 */
    /* Row 3 on Unassigned (excluded) */
    /* P4 (slot 3) is free */
    int slot = cbx_conflict_find_lowest_free_slot(&g, 3);
    assert_int_equal(slot, 3);
}

static void
test_find_lowest_free_all_occupied_no_slot(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 5);
    /* 4 slots, all occupied by rows 0-3, row 4 excluded */
    move_to_col(&g, 0, 1);
    move_to_col(&g, 1, 2);
    move_to_col(&g, 2, 3);
    move_to_col(&g, 3, 4);
    int slot = cbx_conflict_find_lowest_free_slot(&g, 4);
    assert_int_equal(slot, -1); /* all occupied */
}

static void
test_find_lowest_free_null(void **state)
{
    (void)state;
    int slot = cbx_conflict_find_lowest_free_slot(NULL, 0);
    assert_int_equal(slot, -1);
}

/* --- count_occupied -------------------------------------------------- */

static void
test_count_occupied(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    move_to_col(&g, 0, 1);
    move_to_col(&g, 1, 2);
    /* Row 2 on Unassigned */

    assert_int_equal(cbx_conflict_count_occupied(&g, -1), 2); /* no exclusion */
    assert_int_equal(cbx_conflict_count_occupied(&g, 0), 1); /* exclude row 0 */
    assert_int_equal(cbx_conflict_count_occupied(&g, 2), 2); /* exclude row 2 (Unassigned) */
}

static void
test_count_occupied_null(void **state)
{
    (void)state;
    assert_int_equal(cbx_conflict_count_occupied(NULL, 0), 0);
}

/* --- Resolve --------------------------------------------------------- */

static void
test_resolve_move_to_free(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    /* Row 0 → col 1 (P1), Row 1 → col 1 (conflict), Row 2 → Unassigned */
    move_to_col(&g, 0, 1);
    move_to_col(&g, 1, 1);

    cbx_conflict_list list;
    cbx_conflict_detect(&g, &list);
    assert_int_equal(list.count, 1);

    int moved = cbx_conflict_resolve(&g, &list);
    assert_int_equal(moved, 1);
    /* Row 1 should have moved to P2 (slot 1, col 2) — lowest free */
    assert_int_equal(cbx_select_grid_get_cur_col(&g, 1), 2);
    /* Row 0 stays on P1 */
    assert_int_equal(cbx_select_grid_get_cur_col(&g, 0), 1);
}

static void
test_resolve_all_occupied_leave(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    /* 4 slots available, but 3 rows on P1-P3, and one conflict on P1 */
    move_to_col(&g, 0, 1); /* P1 */
    move_to_col(&g, 1, 1); /* P1 (conflict) */
    move_to_col(&g, 2, 2); /* P2 */
    /* P3 (slot 2) and P4 (slot 3) are still free */

    cbx_conflict_list list;
    cbx_conflict_detect(&g, &list);
    int moved = cbx_conflict_resolve(&g, &list);
    assert_int_equal(moved, 1);
    /* Row 1 moves to P3 (slot 2, col 3) — lowest free */
    assert_int_equal(cbx_select_grid_get_cur_col(&g, 1), 3);
}

static void
test_resolve_all_slots_occupied_stay(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 5);
    /* 4 slots, 4 occupied + 1 conflict → no free slot */
    move_to_col(&g, 0, 1); /* P1 */
    move_to_col(&g, 1, 1); /* P1 (conflict, second arrival) */
    move_to_col(&g, 2, 2); /* P2 */
    move_to_col(&g, 3, 3); /* P3 */
    move_to_col(&g, 4, 4); /* P4 */

    cbx_conflict_list list;
    cbx_conflict_detect(&g, &list);
    int moved = cbx_conflict_resolve(&g, &list);
    assert_int_equal(moved, 0); /* can't move — all slots occupied */
    /* Row 1 stays on P1 (col 1) */
    assert_int_equal(cbx_select_grid_get_cur_col(&g, 1), 1);
}

static void
test_resolve_unassigned_no_conflict(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    /* All on Unassigned — no conflict detected, nothing to resolve */
    cbx_conflict_list list;
    cbx_conflict_detect(&g, &list);
    assert_int_equal(list.count, 0);
    int moved = cbx_conflict_resolve(&g, &list);
    assert_int_equal(moved, 0);
}

static void
test_resolve_multiple_conflicts(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 4);
    /* Row 0 → P1, Row 1 → P1 (conflict), Row 2 → P2, Row 3 → P2 (conflict) */
    move_to_col(&g, 0, 1);
    move_to_col(&g, 1, 1);
    move_to_col(&g, 2, 2);
    move_to_col(&g, 3, 2);

    cbx_conflict_list list;
    cbx_conflict_detect(&g, &list);
    assert_int_equal(list.count, 2);

    int moved = cbx_conflict_resolve(&g, &list);
    assert_int_equal(moved, 2);
    /* Row 1 → P3 (slot 2, col 3) */
    assert_int_equal(cbx_select_grid_get_cur_col(&g, 1), 3);
    /* Row 3 → P4 (slot 3, col 4) */
    assert_int_equal(cbx_select_grid_get_cur_col(&g, 3), 4);
}

static void
test_resolve_no_conflicts(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    move_to_col(&g, 0, 1);
    move_to_col(&g, 1, 2);
    move_to_col(&g, 2, 3);

    cbx_conflict_list list;
    cbx_conflict_detect(&g, &list);
    assert_int_equal(list.count, 0);
    int moved = cbx_conflict_resolve(&g, &list);
    assert_int_equal(moved, 0);
}

static void
test_resolve_null(void **state)
{
    (void)state;
    cbx_conflict_list list;
    cbx_conflict_list_init(&list);
    int rc = cbx_conflict_resolve(NULL, &list);
    assert_int_equal(rc, -EINVAL);
}

static void
test_resolve_null_list(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 2);
    int rc = cbx_conflict_resolve(&g, NULL);
    assert_int_equal(rc, -EINVAL);
}

static void
test_resolve_resolves_all_conflicts(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    move_to_col(&g, 0, 1);
    move_to_col(&g, 1, 1);

    cbx_conflict_list list;
    cbx_conflict_detect(&g, &list);
    cbx_conflict_resolve(&g, &list);

    /* After resolution, re-detect should find no conflicts */
    cbx_conflict_list list2;
    cbx_conflict_detect(&g, &list2);
    assert_int_equal(list2.count, 0);
}

/* --- Spec example ---------------------------------------------------- */

static void
test_resolve_spec_example(void **state)
{
    (void)state;
    /*
     * Spec §4.5 example: "P4's controller moves to P2, P2 is occupied,
     * P1 is free → controller becomes P1."
     *
     * Setup: 4 controllers, P1 free, P2 occupied, P4 moves to P2 (conflict).
     */
    cbx_select_grid g;
    build_test_grid(&g, 3); /* 3 composites, 4 slots */
    /* Row 0 → P2 (col 2), Row 1 → P4 (col 4), Row 2 → P2 (conflict) */
    move_to_col(&g, 0, 2); /* P2 */
    move_to_col(&g, 1, 4); /* P4 */
    move_to_col(&g, 2, 2); /* P2 (conflict — second arrival) */

    cbx_conflict_list list;
    cbx_conflict_detect(&g, &list);
    assert_int_equal(list.count, 1);
    assert_int_equal(list.conflicts[0].row_idx, 2);

    int moved = cbx_conflict_resolve(&g, &list);
    assert_int_equal(moved, 1);
    /* Row 2 should move to P1 (slot 0, col 1) — lowest free P-slot */
    assert_int_equal(cbx_select_grid_get_cur_col(&g, 2), 1);
}

/* --- Visual: conflict red rendering (Task 2) ------------------------ */

#define VIS_W 800
#define VIS_H 600
#define VIS_TOL 20

static void
test_conflict_red_rendering(void **state)
{
    (void)state;
    TestSdlState sdl;
    if (test_harness_sdl_init(&sdl) != 0) {
        skip();
        return;
    }

    /* Build a grid with 3 controllers, 4 virtual slots. */
    cbx_select_grid g;
    build_test_grid(&g, 3);
    /* Row 0 → P1 (col 1), Row 1 → P1 (col 1) → conflict (second arrival). */
    move_to_col(&g, 0, 1);
    move_to_col(&g, 1, 1);
    /* Row 2 stays on Unassigned. */

    cbx_conflict_list conflicts;
    cbx_conflict_detect(&g, &conflicts);
    assert_int_equal(conflicts.count, 1);
    assert_int_equal(conflicts.conflicts[0].row_idx, 1);

    /* Set up render context with conflicts. */
    cbx_grid_render_ctx ctx = {0};
    ctx.grid = &g;
    ctx.conflicts = &conflicts;

    /* Render via the production composition path. */
    cbx_overlay_surface surface;
    memset(&surface, 0, sizeof(surface));
    int rc = cbx_overlay_surface_init(&surface, sdl.renderer,
                                       VIS_W, VIS_H, 1.0);
    assert_int_equal(rc, 0);
    cbx_overlay_surface_mark_dirty_all(&surface);
    rc = cbx_overlay_surface_render(&surface, sdl.renderer,
                                     cbx_select_grid_render_cb, &ctx);
    assert_int_equal(rc, 0);

    /* Read back pixels from the overlay texture. */
    uint8_t *buf = malloc(VIS_W * VIS_H * 4);
    assert_non_null(buf);
    SDL_SetRenderTarget(sdl.renderer, cbx_overlay_surface_get_texture(&surface));
    assert_int_equal(fb_read_pixels(sdl.renderer, NULL, buf,
                                     VIS_W * VIS_H * 4), 0);
    SDL_SetRenderTarget(sdl.renderer, NULL);

    /* Compute the cell layout to find the conflicted cell region.
     * This mirrors the layout in cbx_select_grid_render(). */
    int header_h = 32;
    int label_w  = 200;
    int profile_w = 160;
    int grid_x = label_w;
    int grid_y = header_h;
    int grid_w = VIS_W - label_w - profile_w;
    int grid_h = VIS_H - header_h;
    int cell_w = grid_w / g.col_count;
    int cell_h = grid_h / g.row_count;

    /* Row 1 (conflicted), col 1 (current) → the red cell. */
    int cell_x = grid_x + 1 * cell_w + 4;  /* +CELL_MARGIN */
    int cell_y = grid_y + 1 * cell_h + 4;
    int cell_rw = cell_w - 4;  /* -CELL_MARGIN */
    int cell_rh = cell_h - 4;

    SDL_Rect conflict_cell = { .x = cell_x, .y = cell_y,
                               .w = cell_rw, .h = cell_rh };

    /* Assert that the conflicted cell region contains red pixels. */
    uint8_t red_target[3] = {220, 40, 40};
    assert_true(fb_region_has_color(buf, VIS_W, VIS_H, &conflict_cell,
                                     red_target, VIS_TOL));

    /* Assert that a non-conflicted cell (row 0, col 1) does NOT have red. */
    int nc_x = grid_x + 1 * cell_w + 4;
    int nc_y = grid_y + 0 * cell_h + 4;
    SDL_Rect normal_cell = { .x = nc_x, .y = nc_y,
                             .w = cell_w - 4, .h = cell_h - 4 };
    assert_false(fb_region_has_color(buf, VIS_W, VIS_H, &normal_cell,
                                      red_target, VIS_TOL));

    /* Render without conflicts → no red in row 1's cell. */
    cbx_overlay_surface_mark_dirty_all(&surface);
    ctx.conflicts = NULL;
    rc = cbx_overlay_surface_render(&surface, sdl.renderer,
                                     cbx_select_grid_render_cb, &ctx);
    assert_int_equal(rc, 0);
    assert_int_equal(fb_read_pixels(sdl.renderer, NULL, buf,
                                     VIS_W * VIS_H * 4), 0);
    assert_false(fb_region_has_color(buf, VIS_W, VIS_H, &conflict_cell,
                                      red_target, VIS_TOL));

    free(buf);
    cbx_overlay_surface_destroy(&surface);
    test_harness_sdl_shutdown(&sdl);
}

/* --- Main ------------------------------------------------------------ */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* List init */
        cmocka_unit_test(test_list_init),
        cmocka_unit_test(test_list_init_null),
        /* Detect: no conflicts */
        cmocka_unit_test(test_detect_no_conflicts),
        cmocka_unit_test(test_detect_all_different),
        /* Detect: conflicts */
        cmocka_unit_test(test_detect_two_same_column),
        cmocka_unit_test(test_detect_multiple_conflicts),
        cmocka_unit_test(test_detect_three_same_column),
        /* Detect: Unassigned never a conflict */
        cmocka_unit_test(test_detect_unassigned_no_conflict),
        cmocka_unit_test(test_detect_unassigned_and_occupied),
        /* Detect: edge cases */
        cmocka_unit_test(test_detect_empty_grid),
        cmocka_unit_test(test_detect_single_row),
        cmocka_unit_test(test_detect_null),
        cmocka_unit_test(test_detect_null_out),
        /* is_row_conflicted */
        cmocka_unit_test(test_is_row_conflicted),
        cmocka_unit_test(test_is_row_conflicted_null),
        cmocka_unit_test(test_is_row_conflicted_empty),
        /* find_lowest_free_slot */
        cmocka_unit_test(test_find_lowest_free_all_free),
        cmocka_unit_test(test_find_lowest_free_some_occupied),
        cmocka_unit_test(test_find_lowest_free_all_occupied),
        cmocka_unit_test(test_find_lowest_free_all_occupied_no_slot),
        cmocka_unit_test(test_find_lowest_free_null),
        /* count_occupied */
        cmocka_unit_test(test_count_occupied),
        cmocka_unit_test(test_count_occupied_null),
        /* Resolve */
        cmocka_unit_test(test_resolve_move_to_free),
        cmocka_unit_test(test_resolve_all_occupied_leave),
        cmocka_unit_test(test_resolve_all_slots_occupied_stay),
        cmocka_unit_test(test_resolve_unassigned_no_conflict),
        cmocka_unit_test(test_resolve_multiple_conflicts),
        cmocka_unit_test(test_resolve_no_conflicts),
        cmocka_unit_test(test_resolve_null),
        cmocka_unit_test(test_resolve_null_list),
        cmocka_unit_test(test_resolve_resolves_all_conflicts),
        /* Spec example */
        cmocka_unit_test(test_resolve_spec_example),
        /* Visual: conflict red rendering */
        cmocka_unit_test(test_conflict_red_rendering),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}