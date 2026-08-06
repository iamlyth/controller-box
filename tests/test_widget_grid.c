/*
 * test_widget_grid.c — Tests for the Grid widget.
 *
 * Uses the SDL2 dummy driver for headless rendering.
 *
 * Task 21 — List, Grid, TabBar, and ProgressBar widgets.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <setjmp.h>
#include <cmocka.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <errno.h>
#include <string.h>

#include "ui/widget.h"
#include "ui/text.h"
#include "ui/theme.h"

/* --- test context -------------------------------------------------- */

typedef struct {
    SDL_Window   *window;
    SDL_Renderer *renderer;
} TestCtx;

static void
ensure_dummy_driver(void)
{
    if (getenv("SDL_VIDEODRIVER") == NULL)
        SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "dummy",
                                 SDL_HINT_OVERRIDE);
}

static int
test_setup(TestCtx *ctx)
{
    ensure_dummy_driver();
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        return -1;
    ctx->window = SDL_CreateWindow("test", 0, 0, 320, 240,
                                   SDL_WINDOW_HIDDEN);
    if (!ctx->window) { SDL_Quit(); return -1; }
    ctx->renderer = SDL_CreateRenderer(ctx->window, -1,
                                       SDL_RENDERER_SOFTWARE);
    if (!ctx->renderer) {
        SDL_DestroyWindow(ctx->window);
        SDL_Quit();
        return -1;
    }
    return 0;
}

static void
test_teardown(TestCtx *ctx)
{
    if (ctx->renderer) SDL_DestroyRenderer(ctx->renderer);
    if (ctx->window)   SDL_DestroyWindow(ctx->window);
    SDL_Quit();
}

/* --- Grid tests ---------------------------------------------------- */

static void
test_grid_init_basic(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_grid grid;
    assert_int_equal(cbx_grid_init(&grid, &theme), 0);
    assert_true(cbx_widget_is_visible(&grid.base));
    assert_int_equal(grid.rows, 0);
    assert_int_equal(grid.cols, 0);

    cbx_widget_destroy(&grid.base);
}

static void
test_grid_init_null(void **state)
{
    (void)state;
    assert_int_equal(cbx_grid_init(NULL, NULL), -EINVAL);
    cbx_theme theme;
    cbx_theme_default(&theme);
    assert_int_equal(cbx_grid_init(NULL, &theme), -EINVAL);
}

static void
test_grid_set_dims(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_grid grid;
    assert_int_equal(cbx_grid_init(&grid, &theme), 0);

    assert_int_equal(cbx_grid_set_dims(&grid, 3, 4), 0);
    assert_int_equal(grid.rows, 3);
    assert_int_equal(grid.cols, 4);

    /* Setting rect computes cell dims. */
    SDL_Rect r = {0, 0, 240, 180};
    cbx_widget_set_rect(&grid.base, &r);
    assert_int_equal(grid.cell_w, 60);  /* 240/4 */
    assert_int_equal(grid.cell_h, 60);  /* 180/3 */

    cbx_widget_destroy(&grid.base);
}

static void
test_grid_set_dims_invalid(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_grid grid;
    assert_int_equal(cbx_grid_init(&grid, &theme), 0);

    assert_int_equal(cbx_grid_set_dims(&grid, 0, 4), -EINVAL);
    assert_int_equal(cbx_grid_set_dims(&grid, 3, 0), -EINVAL);
    assert_int_equal(cbx_grid_set_dims(&grid, -1, 4), -EINVAL);
    assert_int_equal(cbx_grid_set_dims(NULL, 3, 4), -EINVAL);

    cbx_widget_destroy(&grid.base);
}

static void
test_grid_set_dims_too_large(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_grid grid;
    assert_int_equal(cbx_grid_init(&grid, &theme), 0);

    /* CBX_GRID_MAX_CELLS = 256. 17×17=289 > 256. */
    assert_int_equal(cbx_grid_set_dims(&grid, 17, 17), -ENOMEM);

    /* 16×16=256 — OK. */
    assert_int_equal(cbx_grid_set_dims(&grid, 16, 16), 0);

    cbx_widget_destroy(&grid.base);
}

static void
test_grid_set_get_cell(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_grid grid;
    assert_int_equal(cbx_grid_init(&grid, &theme), 0);
    assert_int_equal(cbx_grid_set_dims(&grid, 2, 3), 0);

    /* Use labels as cells. */
    cbx_label cells[6];
    /* Initialize labels minimally without text cache. */
    for (int i = 0; i < 6; i++) {
        memset(&cells[i], 0, sizeof(cells[i]));
        cells[i].base.visible = true;
    }

    for (int r = 0; r < 2; r++)
        for (int c = 0; c < 3; c++) {
            int rc = cbx_grid_set_cell(&grid, r, c, &cells[r * 3 + c].base);
            assert_int_equal(rc, 0);
        }

    assert_ptr_equal(cbx_grid_get_cell(&grid, 0, 0), &cells[0].base);
    assert_ptr_equal(cbx_grid_get_cell(&grid, 1, 2), &cells[5].base);

    /* Out of bounds. */
    assert_null(cbx_grid_get_cell(&grid, 2, 0));
    assert_null(cbx_grid_get_cell(&grid, 0, 3));
    assert_null(cbx_grid_get_cell(&grid, -1, 0));
    assert_null(cbx_grid_get_cell(NULL, 0, 0));

    cbx_widget_destroy(&grid.base);
}

static void
test_grid_set_cell_invalid(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_grid grid;
    assert_int_equal(cbx_grid_init(&grid, &theme), 0);
    assert_int_equal(cbx_grid_set_dims(&grid, 2, 2), 0);

    cbx_label lbl;
    memset(&lbl, 0, sizeof(lbl));
    lbl.base.visible = true;

    assert_int_equal(cbx_grid_set_cell(&grid, 5, 0, &lbl.base), -EINVAL);
    assert_int_equal(cbx_grid_set_cell(&grid, 0, 5, &lbl.base), -EINVAL);
    assert_int_equal(cbx_grid_set_cell(NULL, 0, 0, &lbl.base), -EINVAL);

    cbx_widget_destroy(&grid.base);
}

static void
test_grid_navigation(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_grid grid;
    assert_int_equal(cbx_grid_init(&grid, &theme), 0);
    assert_int_equal(cbx_grid_set_dims(&grid, 3, 3), 0);

    int row, col;
    assert_int_equal(cbx_grid_get_cursor(&grid, &row, &col), 0);
    assert_int_equal(row, 0);
    assert_int_equal(col, 0);

    /* Move down. */
    assert_int_equal(cbx_grid_move_down(&grid), 0);
    cbx_grid_get_cursor(&grid, &row, &col);
    assert_int_equal(row, 1);
    assert_int_equal(col, 0);

    /* Move right. */
    assert_int_equal(cbx_grid_move_right(&grid), 0);
    cbx_grid_get_cursor(&grid, &row, &col);
    assert_int_equal(row, 1);
    assert_int_equal(col, 1);

    /* Move up. */
    assert_int_equal(cbx_grid_move_up(&grid), 0);
    cbx_grid_get_cursor(&grid, &row, &col);
    assert_int_equal(row, 0);
    assert_int_equal(col, 1);

    /* Move left. */
    assert_int_equal(cbx_grid_move_left(&grid), 0);
    cbx_grid_get_cursor(&grid, &row, &col);
    assert_int_equal(row, 0);
    assert_int_equal(col, 0);

    cbx_widget_destroy(&grid.base);
}

static void
test_grid_navigation_bounds(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_grid grid;
    assert_int_equal(cbx_grid_init(&grid, &theme), 0);
    assert_int_equal(cbx_grid_set_dims(&grid, 2, 2), 0);

    /* At (0,0) — can't go up or left. */
    assert_int_equal(cbx_grid_move_up(&grid), -1);
    assert_int_equal(cbx_grid_move_left(&grid), -1);

    /* Move to (1,1). */
    cbx_grid_move_down(&grid);
    cbx_grid_move_right(&grid);

    /* At (1,1) — can't go down or right. */
    assert_int_equal(cbx_grid_move_down(&grid), -1);
    assert_int_equal(cbx_grid_move_right(&grid), -1);

    cbx_widget_destroy(&grid.base);
}

static void
test_grid_navigation_no_dims(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_grid grid;
    assert_int_equal(cbx_grid_init(&grid, &theme), 0);

    assert_int_equal(cbx_grid_move_up(&grid), -EINVAL);
    assert_int_equal(cbx_grid_move_down(&grid), -EINVAL);
    assert_int_equal(cbx_grid_move_left(&grid), -EINVAL);
    assert_int_equal(cbx_grid_move_right(&grid), -EINVAL);

    cbx_widget_destroy(&grid.base);
}

static void
test_grid_key_events(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_grid grid;
    assert_int_equal(cbx_grid_init(&grid, &theme), 0);
    assert_int_equal(cbx_grid_set_dims(&grid, 2, 2), 0);

    SDL_Event ev = {0};
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = SDLK_DOWN;
    assert_true(cbx_widget_handle_event(&grid.base, &ev));

    int row, col;
    cbx_grid_get_cursor(&grid, &row, &col);
    assert_int_equal(row, 1);
    assert_int_equal(col, 0);

    ev.key.keysym.sym = SDLK_RIGHT;
    assert_true(cbx_widget_handle_event(&grid.base, &ev));
    cbx_grid_get_cursor(&grid, &row, &col);
    assert_int_equal(row, 1);
    assert_int_equal(col, 1);

    ev.key.keysym.sym = SDLK_UP;
    assert_true(cbx_widget_handle_event(&grid.base, &ev));
    cbx_grid_get_cursor(&grid, &row, &col);
    assert_int_equal(row, 0);
    assert_int_equal(col, 1);

    ev.key.keysym.sym = SDLK_LEFT;
    assert_true(cbx_widget_handle_event(&grid.base, &ev));
    cbx_grid_get_cursor(&grid, &row, &col);
    assert_int_equal(row, 0);
    assert_int_equal(col, 0);

    cbx_widget_destroy(&grid.base);
}

static void
test_grid_draw(void **state)
{
    (void)state;
    TestCtx ctx = {0};
    assert_int_equal(test_setup(&ctx), 0);
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_grid grid;
    assert_int_equal(cbx_grid_init(&grid, &theme), 0);
    assert_int_equal(cbx_grid_set_dims(&grid, 2, 2), 0);

    SDL_Rect r = {0, 0, 200, 200};
    cbx_widget_set_rect(&grid.base, &r);
    cbx_widget_focus(&grid.base);
    cbx_widget_draw(&grid.base, ctx.renderer);  /* must not crash */

    cbx_widget_blur(&grid.base);
    cbx_widget_draw(&grid.base, ctx.renderer);

    cbx_widget_destroy(&grid.base);
    test_teardown(&ctx);
}

static void
test_grid_clear(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_grid grid;
    assert_int_equal(cbx_grid_init(&grid, &theme), 0);
    assert_int_equal(cbx_grid_set_dims(&grid, 2, 2), 0);

    cbx_label lbl;
    memset(&lbl, 0, sizeof(lbl));
    lbl.base.visible = true;
    cbx_grid_set_cell(&grid, 0, 0, &lbl.base);
    assert_int_equal(grid.cell_count, 1);

    cbx_grid_clear(&grid);
    assert_int_equal(grid.cell_count, 0);
    assert_null(cbx_grid_get_cell(&grid, 0, 0));
    assert_int_equal(grid.cur_row, 0);
    assert_int_equal(grid.cur_col, 0);

    cbx_widget_destroy(&grid.base);
}

static void
test_grid_focus_blur(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_grid grid;
    assert_int_equal(cbx_grid_init(&grid, &theme), 0);
    assert_int_equal(cbx_grid_set_dims(&grid, 2, 2), 0);

    cbx_widget_focus(&grid.base);
    assert_true(cbx_widget_is_focused(&grid.base));

    cbx_widget_blur(&grid.base);
    assert_false(cbx_widget_is_focused(&grid.base));

    cbx_widget_destroy(&grid.base);
}

static void
test_grid_null_args(void **state)
{
    (void)state;
    assert_int_equal(cbx_grid_set_dims(NULL, 2, 2), -EINVAL);
    assert_int_equal(cbx_grid_set_cell(NULL, 0, 0, NULL), -EINVAL);
    assert_null(cbx_grid_get_cell(NULL, 0, 0));
    assert_int_equal(cbx_grid_get_cursor(NULL, NULL, NULL), -EINVAL);
    cbx_grid_clear(NULL);
}

static void
test_grid_unrelated_event(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_grid grid;
    assert_int_equal(cbx_grid_init(&grid, &theme), 0);
    assert_int_equal(cbx_grid_set_dims(&grid, 2, 2), 0);

    SDL_Event ev = {0};
    ev.type = SDL_MOUSEMOTION;
    assert_false(cbx_widget_handle_event(&grid.base, &ev));

    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = SDLK_a;
    assert_false(cbx_widget_handle_event(&grid.base, &ev));

    cbx_widget_destroy(&grid.base);
}

/* --- main ---------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_grid_init_basic),
        cmocka_unit_test(test_grid_init_null),
        cmocka_unit_test(test_grid_set_dims),
        cmocka_unit_test(test_grid_set_dims_invalid),
        cmocka_unit_test(test_grid_set_dims_too_large),
        cmocka_unit_test(test_grid_set_get_cell),
        cmocka_unit_test(test_grid_set_cell_invalid),
        cmocka_unit_test(test_grid_navigation),
        cmocka_unit_test(test_grid_navigation_bounds),
        cmocka_unit_test(test_grid_navigation_no_dims),
        cmocka_unit_test(test_grid_key_events),
        cmocka_unit_test(test_grid_draw),
        cmocka_unit_test(test_grid_clear),
        cmocka_unit_test(test_grid_focus_blur),
        cmocka_unit_test(test_grid_null_args),
        cmocka_unit_test(test_grid_unrelated_event),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}