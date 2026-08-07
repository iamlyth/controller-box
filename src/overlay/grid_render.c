/*
 * grid_render.c — Character select grid data model and rendering.
 *
 * Task 29 — Character select grid rendering and Player Mode navigation.
 *
 * Implements the pure data model (build, navigation, profile cycling)
 * and the SDL render function for the character select grid.
 */
#include "overlay/grid_render.h"

#include <errno.h>
#include <string.h>

#include "icons/icon_lookup.h"
#include "identify/assign.h"  /* CBX_DEFAULT_PROFILE, cbx_assign_lookup */
#include "overlay/conflict.h"  /* cbx_conflict_is_row_conflicted */

/* --- Helpers ---------------------------------------------------------- */

static int
clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* --- Lifecycle -------------------------------------------------------- */

void
cbx_select_grid_init(cbx_select_grid *g)
{
    if (!g)
        return;
    memset(g, 0, sizeof(*g));
}

/* --- Build ----------------------------------------------------------- */

int
cbx_select_grid_build(cbx_select_grid *g,
                      const cbx_grid_composite_info *composites,
                      int composite_count,
                      const cbx_settings *settings,
                      const cbx_assignments *assignments)
{
    if (!g || !settings || !assignments)
        return -EINVAL;
    if (composite_count < 0 || composite_count > CBX_GRID_MAX_ROWS)
        return -EINVAL;
    if (composite_count > 0 && !composites)
        return -EINVAL;
    if (settings->virtual_controllers.count < 1 ||
        settings->virtual_controllers.count > CBX_MAX_CONTROLLERS)
        return -EINVAL;

    cbx_select_grid_init(g);

    /* Build columns: col 0 = Unassigned, cols 1..N = player slots. */
    g->col_count = settings->virtual_controllers.count + 1;
    for (int i = 0; i < settings->virtual_controllers.count &&
                    i < CBX_MAX_CONTROLLERS; i++) {
        /* col index = i + 1 (col 0 is Unassigned) */
        strncpy(g->cols[i + 1].device_type,
                settings->virtual_controllers.types[i],
                CBX_MAX_TYPE_LEN - 1);
        g->cols[i + 1].device_type[CBX_MAX_TYPE_LEN - 1] = '\0';
    }
    /* col 0 (Unassigned) has empty device type. */
    g->cols[0].device_type[0] = '\0';

    /* Build rows: one per composite. */
    g->row_count = clamp_int(composite_count, 0, CBX_GRID_MAX_ROWS);
    for (int i = 0; i < g->row_count; i++) {
        cbx_grid_row *row = &g->rows[i];

        strncpy(row->id, composites[i].id, CBX_MAX_ID_LEN - 1);
        row->id[CBX_MAX_ID_LEN - 1] = '\0';

        strncpy(row->model_name, composites[i].model_name,
                CBX_MAX_NAME_LEN - 1);
        row->model_name[CBX_MAX_NAME_LEN - 1] = '\0';

        strncpy(row->composite_path, composites[i].composite_path,
                CBX_MAX_PATH_LEN - 1);
        row->composite_path[CBX_MAX_PATH_LEN - 1] = '\0';

        /* Look up assignment by identity ID. */
        row->cur_col = CBX_GRID_UNASSIGNED_COL;  /* default: Unassigned */
        strncpy(row->profile, CBX_DEFAULT_PROFILE, CBX_GRID_PROFILE_LEN - 1);
        row->profile[CBX_GRID_PROFILE_LEN - 1] = '\0';

        if (row->id[0] != '\0') {
            cbx_assignment found;
            int rc = cbx_assign_lookup(assignments, row->id, &found);
            if (rc == 0 && found.slot >= 0) {
                /* slot 0 = P1 = col 1, slot 1 = P2 = col 2, etc. */
                int col = found.slot + 1;
                if (col < g->col_count)
                    row->cur_col = col;
                if (found.profile[0] != '\0') {
                    snprintf(row->profile, CBX_GRID_PROFILE_LEN,
                             "%s", found.profile);
                }
            }
        }
    }

    return 0;
}

/* --- Profile list management ----------------------------------------- */

int
cbx_select_grid_add_profile(cbx_select_grid *g, const char *name)
{
    if (!g || !name)
        return -EINVAL;
    if (g->profile_count >= CBX_GRID_MAX_PROFILES)
        return -ENOSPC;
    if (name[0] == '\0')
        return -EINVAL;

    /* Avoid duplicates. */
    if (cbx_select_grid_find_profile(g, name) >= 0)
        return 0;  /* already present */

    strncpy(g->profiles[g->profile_count], name, CBX_GRID_PROFILE_LEN - 1);
    g->profiles[g->profile_count][CBX_GRID_PROFILE_LEN - 1] = '\0';
    g->profile_count++;
    return 0;
}

void
cbx_select_grid_clear_profiles(cbx_select_grid *g)
{
    if (!g)
        return;
    g->profile_count = 0;
}

int
cbx_select_grid_find_profile(const cbx_select_grid *g, const char *name)
{
    if (!g || !name)
        return -1;
    for (int i = 0; i < g->profile_count; i++) {
        if (strcmp(g->profiles[i], name) == 0)
            return i;
    }
    return -1;
}

/* --- Navigation ------------------------------------------------------ */

int
cbx_select_grid_move_left(cbx_select_grid *g, int row_idx)
{
    if (!g || row_idx < 0 || row_idx >= g->row_count)
        return -EINVAL;

    if (g->rows[row_idx].cur_col <= 0)
        return -ERANGE;  /* already at Unassigned (leftmost) */

    g->rows[row_idx].cur_col--;
    return 0;
}

int
cbx_select_grid_move_right(cbx_select_grid *g, int row_idx)
{
    if (!g || row_idx < 0 || row_idx >= g->row_count)
        return -EINVAL;

    if (g->rows[row_idx].cur_col >= g->col_count - 1)
        return -ERANGE;  /* already at rightmost column */

    g->rows[row_idx].cur_col++;
    return 0;
}

int
cbx_select_grid_cycle_profile_up(cbx_select_grid *g, int row_idx)
{
    if (!g || row_idx < 0 || row_idx >= g->row_count)
        return -EINVAL;
    if (g->profile_count == 0)
        return -ENOENT;

    cbx_grid_row *row = &g->rows[row_idx];
    int idx = cbx_select_grid_find_profile(g, row->profile);
    if (idx < 0)
        idx = 0;  /* current profile not in list → start from first */
    else
        idx = (idx - 1 + g->profile_count) % g->profile_count;

    strncpy(row->profile, g->profiles[idx], CBX_GRID_PROFILE_LEN - 1);
    row->profile[CBX_GRID_PROFILE_LEN - 1] = '\0';
    return 0;
}

int
cbx_select_grid_cycle_profile_down(cbx_select_grid *g, int row_idx)
{
    if (!g || row_idx < 0 || row_idx >= g->row_count)
        return -EINVAL;
    if (g->profile_count == 0)
        return -ENOENT;

    cbx_grid_row *row = &g->rows[row_idx];
    int idx = cbx_select_grid_find_profile(g, row->profile);
    if (idx < 0)
        idx = 0;  /* current profile not in list → start from first */
    else
        idx = (idx + 1) % g->profile_count;

    strncpy(row->profile, g->profiles[idx], CBX_GRID_PROFILE_LEN - 1);
    row->profile[CBX_GRID_PROFILE_LEN - 1] = '\0';
    return 0;
}

/* --- Accessors -------------------------------------------------------- */

int
cbx_select_grid_get_row_count(const cbx_select_grid *g)
{
    return g ? g->row_count : 0;
}

int
cbx_select_grid_get_col_count(const cbx_select_grid *g)
{
    return g ? g->col_count : 0;
}

const cbx_grid_row *
cbx_select_grid_get_row(const cbx_select_grid *g, int idx)
{
    if (!g || idx < 0 || idx >= g->row_count)
        return NULL;
    return &g->rows[idx];
}

const cbx_grid_col *
cbx_select_grid_get_col(const cbx_select_grid *g, int idx)
{
    if (!g || idx < 0 || idx >= g->col_count)
        return NULL;
    return &g->cols[idx];
}

int
cbx_select_grid_get_cur_col(const cbx_select_grid *g, int row_idx)
{
    if (!g || row_idx < 0 || row_idx >= g->row_count)
        return -EINVAL;
    return g->rows[row_idx].cur_col;
}

const char *
cbx_select_grid_get_profile(const cbx_select_grid *g, int row_idx)
{
    if (!g || row_idx < 0 || row_idx >= g->row_count)
        return NULL;
    return g->rows[row_idx].profile;
}

int
cbx_select_grid_col_to_slot(int col)
{
    if (col <= 0)
        return -1;  /* Unassigned */
    return col - 1;
}

int
cbx_select_grid_slot_to_col(int slot)
{
    if (slot < 0)
        return 0;  /* Unassigned */
    return slot + 1;
}

/* --- Rendering -------------------------------------------------------- */

/*
 * Layout constants (pixels).  These are simple defaults; the overlay
 * surface is at screen resolution and the grid fills the clip rect.
 */
#define HEADER_H       32
#define LABEL_W        200
#define PROFILE_W      160
#define CELL_MARGIN      4
#define INDICATOR_R      6   /* radius of position indicator circle */

static void
draw_filled_circle(SDL_Renderer *r, int cx, int cy, int radius)
{
    /* Simple filled circle using rectangles (no trig needed). */
    for (int dy = -radius; dy <= radius; dy++) {
        int half_w = (int)(radius * 0.7071);
        SDL_Rect row = {
            .x = cx - half_w,
            .y = cy + dy,
            .w = half_w * 2 + 1,
            .h = 1,
        };
        if (dy * dy + half_w * half_w <= radius * radius)
            SDL_RenderFillRect(r, &row);
    }
}

static void
draw_hollow_circle(SDL_Renderer *r, int cx, int cy, int radius)
{
    /* Draw circle as a ring of points. */
    for (int dy = -radius; dy <= radius; dy++) {
        int half_w = (int)(radius * 0.7071);
        if (dy * dy + half_w * half_w <= radius * radius) {
            SDL_Rect left  = { .x = cx - half_w, .y = cy + dy, .w = 1, .h = 1 };
            SDL_Rect right = { .x = cx + half_w, .y = cy + dy, .w = 1, .h = 1 };
            SDL_RenderFillRect(r, &left);
            SDL_RenderFillRect(r, &right);
        }
    }
}

int
cbx_select_grid_render(SDL_Renderer *r,
                       const SDL_Rect *clip,
                       cbx_grid_render_ctx *ctx)
{
    if (!r || !ctx || !ctx->grid)
        return 0;  /* NULL-safe no-op */

    cbx_select_grid *g = ctx->grid;
    SDL_Rect area = clip ? *clip : (SDL_Rect){0, 0, 800, 600};

    /* Default colors if no theme. */
    SDL_Color bg       = {30, 30, 40, 255};
    SDL_Color fg       = {220, 220, 220, 255};
    SDL_Color highlight = {100, 200, 255, 255};
    SDL_Color dim      = {60, 60, 70, 255};
    /* Conflict indicator color (SPEC §4.5 — red for second arrivals). */
    SDL_Color conflict_red = {220, 40, 40, 255};

    if (ctx->theme) {
        bg        = ctx->theme->bg;
        fg        = ctx->theme->text_primary;
        highlight = ctx->theme->border_focus;
        dim       = ctx->theme->border;
    }

    /* Clear background. */
    SDL_SetRenderDrawColor(r, bg.r, bg.g, bg.b, bg.a);
    SDL_RenderFillRect(r, &area);

    if (g->row_count == 0 || g->col_count == 0)
        return 0;  /* nothing to render */

    /* Compute layout. */
    int header_h = HEADER_H;
    int label_w  = LABEL_W;
    int profile_w = PROFILE_W;
    int grid_x = area.x + label_w;
    int grid_y = area.y + header_h;
    int grid_w = area.w - label_w - profile_w;
    if (grid_w <= 0) grid_w = area.w - label_w;  /* no profile column if too narrow */
    int grid_h = area.h - header_h;
    int cell_w = grid_w / g->col_count;
    int cell_h = grid_h / g->row_count;

    /* --- Column headers --- */
    for (int col = 0; col < g->col_count; col++) {
        int x = grid_x + col * cell_w;
        const char *header = (col == 0) ? "Unassigned" : NULL;
        char buf[16];
        if (!header) {
            snprintf(buf, sizeof(buf), "P%d", col);
            header = buf;
        }
        /* Draw header background. */
        SDL_Rect hdr_rect = { .x = x, .y = area.y, .w = cell_w, .h = header_h };
        SDL_SetRenderDrawColor(r, dim.r, dim.g, dim.b, dim.a);
        SDL_RenderFillRect(r, &hdr_rect);

        /* Draw header text if text cache available. */
        if (ctx->text_cache) {
            SDL_Texture *tex = cbx_text_render(ctx->text_cache,
                                               ctx->font_id, header, fg);
            if (tex) {
                int tw, th;
                if (SDL_QueryTexture(tex, NULL, NULL, &tw, &th) == 0) {
                    SDL_Rect dst = {
                        .x = x + (cell_w - tw) / 2,
                        .y = area.y + (header_h - th) / 2,
                        .w = tw, .h = th
                    };
                    SDL_RenderCopy(r, tex, NULL, &dst);
                }
            }
        }
    }

    /* --- Rows --- */
    for (int row = 0; row < g->row_count; row++) {
        const cbx_grid_row *gr = &g->rows[row];
        int row_y = grid_y + row * cell_h;

        /* Row label background. */
        SDL_Rect lbl_rect = { .x = area.x, .y = row_y,
                              .w = label_w, .h = cell_h };
        SDL_SetRenderDrawColor(r, bg.r, bg.g, bg.b, bg.a);
        SDL_RenderFillRect(r, &lbl_rect);

        /* Draw model name + profile label. */
        if (ctx->text_cache) {
            char label[CBX_MAX_NAME_LEN + CBX_GRID_PROFILE_LEN + 16];
            snprintf(label, sizeof(label), "%s  |  Profile: %s",
                     gr->model_name[0] ? gr->model_name : "Controller",
                     gr->profile[0] ? gr->profile : CBX_DEFAULT_PROFILE);
            SDL_Texture *tex = cbx_text_render(ctx->text_cache,
                                               ctx->font_id, label, fg);
            if (tex) {
                int tw, th;
                if (SDL_QueryTexture(tex, NULL, NULL, &tw, &th) == 0) {
                    int tx = area.x + 8;
                    int ty = row_y + (cell_h - th) / 2;
                    /* Clip text to label area. */
                    if (tx + tw > area.x + label_w)
                        tw = area.x + label_w - tx;
                    if (tw > 0) {
                        SDL_Rect dst = { tx, ty, tw, th };
                        SDL_RenderCopy(r, tex, NULL, &dst);
                    }
                }
            }
        }

        /* Draw cells. */
        bool is_conflicted = (ctx->conflicts != NULL &&
                             cbx_conflict_is_row_conflicted(ctx->conflicts, row));
        for (int col = 0; col < g->col_count; col++) {
            int x = grid_x + col * cell_w;
            int y = row_y;
            int w = cell_w - CELL_MARGIN;
            int h = cell_h - CELL_MARGIN;

            /* Cell background.
             * - Conflicted row's current column: red indicator (SPEC §4.5).
             * - Current column (non-conflicted): highlight color.
             * - Other columns: dim color. */
            SDL_Rect cell_rect = { .x = x + CELL_MARGIN, .y = y + CELL_MARGIN,
                                   .w = w, .h = h };
            if (is_conflicted && col == gr->cur_col) {
                SDL_SetRenderDrawColor(r, conflict_red.r, conflict_red.g,
                                       conflict_red.b, conflict_red.a);
            } else if (col == gr->cur_col) {
                SDL_SetRenderDrawColor(r, highlight.r, highlight.g,
                                       highlight.b, highlight.a);
            } else {
                SDL_SetRenderDrawColor(r, dim.r, dim.g, dim.b, dim.a);
            }
            SDL_RenderFillRect(r, &cell_rect);

            /* Draw cell border — red for conflicted row's current cell. */
            if (is_conflicted && col == gr->cur_col) {
                SDL_SetRenderDrawColor(r, conflict_red.r, conflict_red.g,
                                       conflict_red.b, conflict_red.a);
            } else {
                SDL_SetRenderDrawColor(r, fg.r, fg.g, fg.b, fg.a / 2);
            }
            SDL_RenderDrawRect(r, &cell_rect);

            /* Draw icon (if icon cache + map available). */
            if (ctx->icon_cache && ctx->icon_map && col > 0) {
                const char *dev_type = g->cols[col].device_type;
                if (dev_type[0] != '\0') {
                    cbx_icon_result icon_res;
                    memset(&icon_res, 0, sizeof(icon_res));
                    int rc = cbx_icon_lookup(ctx->icon_cache,
                                             ctx->icon_map, dev_type,
                                             NULL, &icon_res);
                    if (rc == 0 && icon_res.texture) {
                        SDL_Rect dst = {
                            .x = x + CELL_MARGIN + (w - icon_res.width) / 2,
                            .y = y + CELL_MARGIN + (h - icon_res.height) / 2,
                            .w = icon_res.width,
                            .h = icon_res.height
                        };
                        /* Scale icon to fit if too large. */
                        int max_icon = (w < h ? w : h) - 2 * INDICATOR_R;
                        if (max_icon > 0) {
                            float scale = (float)max_icon /
                                (icon_res.width > icon_res.height ?
                                 icon_res.width : icon_res.height);
                            if (scale < 1.0f) {
                                dst.w = (int)(icon_res.width * scale);
                                dst.h = (int)(icon_res.height * scale);
                                dst.x = x + CELL_MARGIN + (w - dst.w) / 2;
                                dst.y = y + CELL_MARGIN + (h - dst.h) / 2;
                            }
                        }
                        SDL_RenderCopy(r, icon_res.texture, NULL, &dst);
                    }
                }
            }

            /* Draw position indicator.
             * Conflicted row's current cell uses red indicator. */
            int cx = x + cell_w / 2;
            /* Position indicator at bottom of cell. */
            int indicator_y = y + cell_h - INDICATOR_R - 4;
            if (col == gr->cur_col) {
                if (is_conflicted) {
                    SDL_SetRenderDrawColor(r, conflict_red.r, conflict_red.g,
                                           conflict_red.b, conflict_red.a);
                } else {
                    SDL_SetRenderDrawColor(r, fg.r, fg.g, fg.b, fg.a);
                }
                draw_filled_circle(r, cx, indicator_y, INDICATOR_R);
            } else {
                SDL_SetRenderDrawColor(r, dim.r, dim.g, dim.b, dim.a);
                draw_hollow_circle(r, cx, indicator_y, INDICATOR_R);
            }
        }

        /* Draw profile label on the right. */
        if (ctx->text_cache && grid_w < area.w - label_w) {
            int px = grid_x + g->col_count * cell_w + 4;
            if (px < area.x + area.w) {
                char plabel[CBX_GRID_PROFILE_LEN + 16];
                snprintf(plabel, sizeof(plabel), "Profile: %s",
                         gr->profile[0] ? gr->profile : CBX_DEFAULT_PROFILE);
                SDL_Texture *tex = cbx_text_render(ctx->text_cache,
                                                    ctx->font_id,
                                                    plabel, fg);
                if (tex) {
                    int tw, th;
                    if (SDL_QueryTexture(tex, NULL, NULL, &tw, &th) == 0) {
                        int max_w = area.x + area.w - px;
                        if (tw > max_w) tw = max_w;
                        if (tw > 0) {
                            SDL_Rect dst = {
                                .x = px,
                                .y = row_y + (cell_h - th) / 2,
                                .w = tw, .h = th
                            };
                            SDL_RenderCopy(r, tex, NULL, &dst);
                        }
                    }
                }
            }
        }
    }

    return 0;
}

int
cbx_select_grid_render_cb(SDL_Renderer *r,
                           const SDL_Rect *clip,
                           void *userdata)
{
    return cbx_select_grid_render(r, clip,
                                  (cbx_grid_render_ctx *)userdata);
}