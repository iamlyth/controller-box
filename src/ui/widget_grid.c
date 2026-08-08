/*
 * widget_grid.c — Grid widget implementation.
 *
 * N rows × M columns, each cell is a widget.  Independent row/column
 * navigation with current-position highlight.  The grid does NOT own
 * its cell widgets (caller manages lifetime, same as Panel).
 *
 * Task 21 — List, Grid, TabBar, and ProgressBar widgets.
 */
#include "widget.h"

#include <errno.h>
#include <string.h>

/* --- helpers ------------------------------------------------------- */

static int
idx(cbx_grid *grid, int row, int col)
{
    if (row < 0 || row >= grid->rows || col < 0 || col >= grid->cols)
        return -1;
    return row * grid->cols + col;
}

static void
compute_cell_dims(cbx_grid *grid)
{
    if (grid->cols > 0 && grid->base.rect.w > 0)
        grid->cell_w = grid->base.rect.w / grid->cols;
    else
        grid->cell_w = 0;
    if (grid->rows > 0 && grid->base.rect.h > 0)
        grid->cell_h = grid->base.rect.h / grid->rows;
    else
        grid->cell_h = 0;
}

static void
focus_current(cbx_grid *grid, bool focus)
{
    int i = idx(grid, grid->cur_row, grid->cur_col);
    if (i >= 0 && i < grid->cell_count && grid->cells[i]) {
        if (focus)
            cbx_widget_focus(grid->cells[i]);
        else
            cbx_widget_blur(grid->cells[i]);
    }
}

/* --- vtable -------------------------------------------------------- */

static void
grid_draw(cbx_widget *w, SDL_Renderer *r)
{
    cbx_grid *grid = (cbx_grid *)w;
    if (!r)
        return;
    compute_cell_dims(grid);

    /* Draw each cell. */
    for (int row = 0; row < grid->rows; row++) {
        for (int col = 0; col < grid->cols; col++) {
            int i = row * grid->cols + col;
            if (i >= grid->cell_count || !grid->cells[i])
                continue;
            SDL_Rect cell_rect = {
                .x = grid->base.rect.x + col * grid->cell_w,
                .y = grid->base.rect.y + row * grid->cell_h,
                .w = grid->cell_w,
                .h = grid->cell_h,
            };
            cbx_widget_set_rect(grid->cells[i], &cell_rect);
            cbx_widget_draw(grid->cells[i], r);
        }
    }

    /* Draw highlight rectangle around current cell. */
    if (grid->theme && grid->base.focused) {
        SDL_Rect hl = {
            .x = grid->base.rect.x + grid->cur_col * grid->cell_w,
            .y = grid->base.rect.y + grid->cur_row * grid->cell_h,
            .w = grid->cell_w,
            .h = grid->cell_h,
        };
        SDL_SetRenderDrawColor(r, grid->theme->border_focus.r,
                               grid->theme->border_focus.g,
                               grid->theme->border_focus.b,
                               grid->theme->border_focus.a);
        SDL_RenderDrawRect(r, &hl);
    }
}

static bool
grid_handle_event(cbx_widget *w, const SDL_Event *ev)
{
    cbx_grid *grid = (cbx_grid *)w;
    if (!ev)
        return false;

    switch (ev->type) {
    case SDL_KEYDOWN:
        switch (ev->key.keysym.sym) {
        case SDLK_UP:
            return cbx_grid_move_up(grid) == 0;
        case SDLK_DOWN:
            return cbx_grid_move_down(grid) == 0;
        case SDLK_LEFT:
            return cbx_grid_move_left(grid) == 0;
        case SDLK_RIGHT:
            return cbx_grid_move_right(grid) == 0;
        case SDLK_RETURN:
        case SDLK_SPACE: {
            int i = idx(grid, grid->cur_row, grid->cur_col);
            if (i >= 0 && i < grid->cell_count && grid->cells[i]) {
                /* Forward the event to the cell. */
                return cbx_widget_handle_event(grid->cells[i], ev);
            }
            break;
        }
        default:
            break;
        }
        break;
    default:
        break;
    }
    return false;
}

static void
grid_focus(cbx_widget *w)
{
    cbx_grid *grid = (cbx_grid *)w;
    grid->base.focused = true;
    focus_current(grid, true);
}

static void
grid_blur(cbx_widget *w)
{
    cbx_grid *grid = (cbx_grid *)w;
    grid->base.focused = false;
    focus_current(grid, false);
}

static void
grid_get_rect(const cbx_widget *w, SDL_Rect *out)
{
    const cbx_grid *grid = (const cbx_grid *)w;
    *out = grid->base.rect;
}

static void
grid_set_rect(cbx_widget *w, const SDL_Rect *rect)
{
    cbx_grid *grid = (cbx_grid *)w;
    grid->base.rect = *rect;
    compute_cell_dims(grid);
}

static void
grid_destroy(cbx_widget *w)
{
    (void)w;
    /* Grid does NOT own cells — caller destroys them. */
}

static const cbx_widget_vtable grid_vt = {
    .draw         = grid_draw,
    .handle_event = grid_handle_event,
    .focus        = grid_focus,
    .blur         = grid_blur,
    .get_rect     = grid_get_rect,
    .set_rect     = grid_set_rect,
    .destroy      = grid_destroy,
};

/* --- public API ---------------------------------------------------- */

int
cbx_grid_init(cbx_grid *grid, const cbx_theme *theme)
{
    if (!grid)
        return -EINVAL;
    memset(grid, 0, sizeof(*grid));
    grid->base.vt = &grid_vt;
    grid->base.visible = true;
    grid->base.interactive = true;
    grid->base.focused = false;
    grid->base.rect = (SDL_Rect){0, 0, 0, 0};
    grid->theme = theme;
    grid->cell_count = 0;
    grid->rows = 0;
    grid->cols = 0;
    grid->cur_row = 0;
    grid->cur_col = 0;
    grid->cell_w = 0;
    grid->cell_h = 0;
    return 0;
}

int
cbx_grid_set_dims(cbx_grid *grid, int rows, int cols)
{
    if (!grid || rows <= 0 || cols <= 0)
        return -EINVAL;
    if (rows * cols > CBX_GRID_MAX_CELLS)
        return -ENOMEM;
    /* Clear existing cells if dims change. */
    if (grid->rows != rows || grid->cols != cols) {
        memset(grid->cells, 0, sizeof(grid->cells));
        grid->cell_count = 0;
    }
    grid->rows = rows;
    grid->cols = cols;
    grid->cur_row = 0;
    grid->cur_col = 0;
    compute_cell_dims(grid);
    return 0;
}

int
cbx_grid_set_cell(cbx_grid *grid, int row, int col, cbx_widget *cell)
{
    if (!grid)
        return -EINVAL;
    int i = idx(grid, row, col);
    if (i < 0)
        return -EINVAL;
    grid->cells[i] = cell;
    if (i >= grid->cell_count)
        grid->cell_count = i + 1;
    return 0;
}

cbx_widget *
cbx_grid_get_cell(cbx_grid *grid, int row, int col)
{
    if (!grid)
        return NULL;
    int i = idx(grid, row, col);
    if (i < 0 || i >= grid->cell_count)
        return NULL;
    return grid->cells[i];
}

int
cbx_grid_get_cursor(const cbx_grid *grid, int *row, int *col)
{
    if (!grid)
        return -EINVAL;
    if (row) *row = grid->cur_row;
    if (col) *col = grid->cur_col;
    return 0;
}

int
cbx_grid_move_up(cbx_grid *grid)
{
    if (!grid || grid->rows == 0)
        return -EINVAL;
    if (grid->cur_row > 0) {
        focus_current(grid, false);
        grid->cur_row--;
        focus_current(grid, true);
        return 0;
    }
    return -1;
}

int
cbx_grid_move_down(cbx_grid *grid)
{
    if (!grid || grid->rows == 0)
        return -EINVAL;
    if (grid->cur_row < grid->rows - 1) {
        focus_current(grid, false);
        grid->cur_row++;
        focus_current(grid, true);
        return 0;
    }
    return -1;
}

int
cbx_grid_move_left(cbx_grid *grid)
{
    if (!grid || grid->cols == 0)
        return -EINVAL;
    if (grid->cur_col > 0) {
        focus_current(grid, false);
        grid->cur_col--;
        focus_current(grid, true);
        return 0;
    }
    return -1;
}

int
cbx_grid_move_right(cbx_grid *grid)
{
    if (!grid || grid->cols == 0)
        return -EINVAL;
    if (grid->cur_col < grid->cols - 1) {
        focus_current(grid, false);
        grid->cur_col++;
        focus_current(grid, true);
        return 0;
    }
    return -1;
}

void
cbx_grid_clear(cbx_grid *grid)
{
    if (!grid)
        return;
    memset(grid->cells, 0, sizeof(grid->cells));
    grid->cell_count = 0;
    grid->cur_row = 0;
    grid->cur_col = 0;
}