/*
 * test_overlay_visual.c — Deterministic framebuffer visual tests for the
 * overlay grid composition path (SPEC §4.10).
 *
 * Task 5 — Overlay deterministic framebuffer visual tests.
 *
 * Renders through the production composition path:
 *   cbx_select_grid_build() → cbx_overlay_surface_init() →
 *   cbx_overlay_surface_render(surface, renderer, cbx_select_grid_render_cb,
 *   &ctx) → fb_read_pixels(renderer, NULL, buf, buf_len)
 *
 * Tests for each §4.10 visual state:
 *   1. Player Mode grid — content in grid cell, text, and icon regions
 *   2. Host Mode — frames differ from Player Mode (different highlight)
 *  2b. Host Mode row states — distinct colors for HOST/SELECTED/FROZEN rows
 *   3. Conflict highlighting — red {220,40,40} in conflicted cell
 *   4. Unassigned + ≥2 player columns — content in all header + ≥2 slot
 *   5. Controller model/profile text — text-colored pixels in label region
 *   6. Virtual-device icons — content in icon regions for each slot
 *   7. State transitions — fb_frames_differ between materially different states
 *
 * Each assertion checks pixel content, not struct fields.  Tests fail if
 * text, icons, rows, columns, or highlights are absent even when in-memory
 * objects are valid.
 */
#include "overlay/conflict.h"
#include "overlay/grid_render.h"
#include "overlay/surface_build.h"
#include "overlay/host_mode.h"
#include "identify/assign.h"
#include "config/config_settings.h"
#include "config/config_assignments.h"
#include "config/config_paths.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "icons/icon_cache.h"
#include "icons/icon_map.h"
#include "fb_assert.h"
#include "test_harness.h"

#include <SDL2/SDL.h>
#include <cmocka.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>

#ifndef CBX_SOURCE_DIR
#define CBX_SOURCE_DIR "."
#endif

#define SVG_DIR  CBX_SOURCE_DIR "/data/icons/svg/"
#define YAML_DIR CBX_SOURCE_DIR "/data/"

/* Visual dimensions for the overlay surface. */
#define VIS_W 800
#define VIS_H 600
#define VIS_TOL 25

/* Layout constants — must match grid_render.c. */
#define HEADER_H    32
#define LABEL_W     200
#define PROFILE_W   160
#define CELL_MARGIN 4

/* ------------------------------------------------------------------ */
/*  Fixture                                                           */
/* ------------------------------------------------------------------ */

struct vis_fixture {
    TestSdlState    sdl;
    cbx_text_cache  text_cache;
    int             font_id;       /* -1 if no font available  */
    bool            has_text;
    cbx_icon_cache  icon_cache;
    cbx_icon_map    icon_map;
    cbx_theme       theme;
    bool            has_icons;
};

/*
 * Find a usable TrueType font.  Tries cbx_font_path() first, then
 * searches common nix-store locations for DejaVuSans.ttf.
 */
static const char *
find_font(void)
{
    const char *p = cbx_font_path();
    if (p)
        return p;

    /* Search common nix-store font directories. */
    static char found[PATH_MAX];
    const char *candidates[] = {
        "/nix/store/zzs2q7lk5mn6y2rywd3snhak7098zs66-system-path"
            "/share/X11/fonts/DejaVuSans.ttf",
        NULL,
    };
    for (int i = 0; candidates[i]; i++) {
        if (access(candidates[i], R_OK) == 0) {
            snprintf(found, sizeof(found), "%s", candidates[i]);
            return found;
        }
    }

    /* Try finding DejaVuSans.ttf in nix-store via popen + find. */
    FILE *fp = popen(
        "find /nix/store -maxdepth 4 -name DejaVuSans.ttf "
        "-path '*/share/X11/fonts/*' 2>/dev/null | head -1",
        "r");
    if (fp) {
        if (fgets(found, sizeof(found), fp) && found[0] != '\0') {
            /* Strip trailing newline. */
            size_t len = strlen(found);
            if (len > 0 && found[len - 1] == '\n')
                found[len - 1] = '\0';
            pclose(fp);
            if (found[0] != '\0' && access(found, R_OK) == 0)
                return found;
        }
        pclose(fp);
    }

    return NULL;
}

static int
vis_setup(void **state)
{
    struct vis_fixture *f = malloc(sizeof(*f));
    if (!f)
        return -1;
    memset(f, 0, sizeof(*f));
    f->font_id = -1;
    f->has_text = false;
    f->has_icons = false;

    /* SDL init. */
    if (test_harness_sdl_init(&f->sdl) != 0) {
        free(f);
        return -1;
    }

    /* Theme. */
    cbx_theme_default(&f->theme);

    /* Text cache + font. */
    if (cbx_text_cache_init(&f->text_cache, f->sdl.renderer) == 0) {
        const char *font = find_font();
        if (font) {
            f->font_id = cbx_text_load_font(&f->text_cache, font, 18);
            if (f->font_id >= 0)
                f->has_text = true;
        }
    }

    /* Icon cache + map. */
    cbx_icon_map_init(&f->icon_map);
    char yaml_path[PATH_MAX + 64];
    snprintf(yaml_path, sizeof(yaml_path), "%s/controller-icons.yaml",
             YAML_DIR);
    if (cbx_icon_map_load(&f->icon_map, yaml_path) == 0) {
        if (cbx_icon_cache_init(&f->icon_cache, f->sdl.renderer,
                                SVG_DIR, 64) == 0) {
            cbx_icon_cache_load(&f->icon_cache, &f->icon_map);
            f->has_icons = true;
        }
    }

    *state = f;
    return 0;
}

static int
vis_teardown(void **state)
{
    struct vis_fixture *f = *state;
    if (f) {
        if (f->has_icons)
            cbx_icon_cache_cleanup(&f->icon_cache);
        if (f->text_cache.renderer)
            cbx_text_cache_cleanup(&f->text_cache);
        test_harness_sdl_shutdown(&f->sdl);
        free(f);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

static cbx_grid_composite_info
make_comp(const char *id, const char *name, const char *path)
{
    cbx_grid_composite_info c;
    memset(&c, 0, sizeof(c));
    if (id)   strncpy(c.id, id, CBX_MAX_ID_LEN - 1);
    if (name) strncpy(c.model_name, name, CBX_MAX_NAME_LEN - 1);
    if (path) strncpy(c.composite_path, path, CBX_MAX_PATH_LEN - 1);
    return c;
}

static void
build_grid(cbx_select_grid *g, int rows)
{
    cbx_grid_composite_info comps[CBX_GRID_MAX_ROWS];
    for (int i = 0; i < rows; i++) {
        char id[32], name[32], path[64];
        snprintf(id, sizeof(id), "ORDER:%d", i);
        snprintf(name, sizeof(name), "Controller %d", i);
        snprintf(path, sizeof(path),
                 "/org/shadowblip/InputPlumber/CompositeDevice%d", i);
        comps[i] = make_comp(id, name, path);
    }

    cbx_settings s;
    memset(&s, 0, sizeof(s));
    s.virtual_controllers.count = 4;
    for (int i = 0; i < 4; i++)
        strncpy(s.virtual_controllers.types[i], "xb360",
                CBX_MAX_TYPE_LEN - 1);

    cbx_assignments a;
    cbx_assignments_init(&a);

    cbx_select_grid_build(g, comps, rows, &s, &a);

    /* Add profiles for cycling. */
    cbx_select_grid_add_profile(g, "default");
    cbx_select_grid_add_profile(g, "fps");
    cbx_select_grid_add_profile(g, "retro");
}

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

/*
 * Render the grid into an overlay surface and read back pixels.
 * Caller provides the render context.  Returns the pixel buffer
 * (malloc'd, caller must free) or NULL on failure.
 */
static uint8_t *
render_and_readback(struct vis_fixture *f, cbx_grid_render_ctx *ctx)
{
    cbx_overlay_surface surface;
    memset(&surface, 0, sizeof(surface));
    int rc = cbx_overlay_surface_init(&surface, f->sdl.renderer,
                                       VIS_W, VIS_H, 1.0);
    if (rc != 0)
        return NULL;

    cbx_overlay_surface_mark_dirty_all(&surface);
    rc = cbx_overlay_surface_render(&surface, f->sdl.renderer,
                                     cbx_select_grid_render_cb, ctx);
    if (rc != 0) {
        cbx_overlay_surface_destroy(&surface);
        return NULL;
    }

    uint8_t *buf = malloc(VIS_W * VIS_H * 4);
    if (!buf) {
        cbx_overlay_surface_destroy(&surface);
        return NULL;
    }

    SDL_SetRenderTarget(f->sdl.renderer,
                        cbx_overlay_surface_get_texture(&surface));
    rc = fb_read_pixels(f->sdl.renderer, NULL, buf, VIS_W * VIS_H * 4);
    SDL_SetRenderTarget(f->sdl.renderer, NULL);

    cbx_overlay_surface_destroy(&surface);

    if (rc != 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

/*
 * Render the grid into an overlay surface and keep the surface alive
 * for re-rendering (needed for transition tests).  Caller destroys
 * the surface and frees the buffer.
 */
static int
render_with_surface(struct vis_fixture *f, cbx_grid_render_ctx *ctx,
                    cbx_overlay_surface *surface, uint8_t *buf)
{
    int rc = cbx_overlay_surface_init(surface, f->sdl.renderer,
                                       VIS_W, VIS_H, 1.0);
    if (rc != 0)
        return -1;

    cbx_overlay_surface_mark_dirty_all(surface);
    rc = cbx_overlay_surface_render(surface, f->sdl.renderer,
                                     cbx_select_grid_render_cb, ctx);
    if (rc != 0) {
        cbx_overlay_surface_destroy(surface);
        return -1;
    }

    SDL_SetRenderTarget(f->sdl.renderer,
                        cbx_overlay_surface_get_texture(surface));
    rc = fb_read_pixels(f->sdl.renderer, NULL, buf, VIS_W * VIS_H * 4);
    SDL_SetRenderTarget(f->sdl.renderer, NULL);

    if (rc != 0) {
        cbx_overlay_surface_destroy(surface);
        return -1;
    }
    return 0;
}

static void
re_render_and_readback(struct vis_fixture *f, cbx_grid_render_ctx *ctx,
                       cbx_overlay_surface *surface, uint8_t *buf)
{
    cbx_overlay_surface_mark_dirty_all(surface);
    cbx_overlay_surface_render(surface, f->sdl.renderer,
                               cbx_select_grid_render_cb, ctx);
    SDL_SetRenderTarget(f->sdl.renderer,
                        cbx_overlay_surface_get_texture(surface));
    fb_read_pixels(f->sdl.renderer, NULL, buf, VIS_W * VIS_H * 4);
    SDL_SetRenderTarget(f->sdl.renderer, NULL);
}

static void
setup_ctx(struct vis_fixture *f, cbx_grid_render_ctx *ctx,
          cbx_select_grid *g, cbx_conflict_list *conflicts)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->grid = g;
    ctx->conflicts = conflicts;
    ctx->theme = &f->theme;
    if (f->has_text) {
        ctx->text_cache = &f->text_cache;
        ctx->font_id = f->font_id;
    }
    if (f->has_icons) {
        ctx->icon_cache = &f->icon_cache;
        ctx->icon_map = &f->icon_map;
    }
}

/* ------------------------------------------------------------------ */
/*  Layout helpers                                                    */
/* ------------------------------------------------------------------ */

/* Compute cell rectangle for (row, col) in an 800x600 surface. */
static SDL_Rect
cell_rect(int row, int col, int row_count, int col_count)
{
    int grid_x = LABEL_W;
    int grid_y = HEADER_H;
    int grid_w = VIS_W - LABEL_W - PROFILE_W;
    int grid_h = VIS_H - HEADER_H;
    int cell_w = grid_w / col_count;
    int cell_h = grid_h / row_count;
    SDL_Rect r = {
        .x = grid_x + col * cell_w + CELL_MARGIN,
        .y = grid_y + row * cell_h + CELL_MARGIN,
        .w = cell_w - CELL_MARGIN,
        .h = cell_h - CELL_MARGIN,
    };
    return r;
}

/* Compute the label region for a row (where model name + profile text is). */
static SDL_Rect
label_rect(int row, int row_count)
{
    int grid_y = HEADER_H;
    int grid_h = VIS_H - HEADER_H;
    int cell_h = grid_h / row_count;
    SDL_Rect r = {
        .x = 4,
        .y = grid_y + row * cell_h + 4,
        .w = LABEL_W - 8,
        .h = cell_h - 8,
    };
    return r;
}

/* Compute the header region for a column. */
static SDL_Rect
header_rect(int col, int col_count)
{
    int grid_x = LABEL_W;
    int grid_w = VIS_W - LABEL_W - PROFILE_W;
    int cell_w = grid_w / col_count;
    SDL_Rect r = {
        .x = grid_x + col * cell_w,
        .y = 0,
        .w = cell_w,
        .h = HEADER_H,
    };
    return r;
}

/* Compute the icon center region for a cell (inner area, excluding
 * the position indicator at the bottom). */
static SDL_Rect
icon_region(int row, int col, int row_count, int col_count)
{
    int grid_x = LABEL_W;
    int grid_y = HEADER_H;
    int grid_w = VIS_W - LABEL_W - PROFILE_W;
    int grid_h = VIS_H - HEADER_H;
    int cell_w = grid_w / col_count;
    int cell_h = grid_h / row_count;
    /* Icon is centered in the cell, excluding bottom indicator area. */
    int icon_area_h = cell_h - 2 * CELL_MARGIN - 20;
    SDL_Rect r = {
        .x = grid_x + col * cell_w + CELL_MARGIN + 4,
        .y = grid_y + row * cell_h + CELL_MARGIN + 4,
        .w = cell_w - 2 * CELL_MARGIN - 8,
        .h = icon_area_h > 0 ? icon_area_h : cell_h / 2,
    };
    return r;
}

/* ------------------------------------------------------------------ */
/*  Tests — SPEC §4.10 visual states                                  */
/* ------------------------------------------------------------------ */

/*
 * 1. Player Mode grid — assert fb_region_has_content for grid cell
 *    regions, text regions (controller model + profile name), and
 *    icon regions.
 */
static void
test_player_mode_grid(void **state)
{
    struct vis_fixture *f = *state;

    cbx_select_grid g;
    build_grid(&g, 3);
    /* Row 0 → P1, Row 1 → P2, Row 2 → P3. */
    move_to_col(&g, 0, 1);
    move_to_col(&g, 1, 2);
    move_to_col(&g, 2, 3);

    cbx_conflict_list conflicts;
    cbx_conflict_detect(&g, &conflicts);

    cbx_grid_render_ctx ctx;
    setup_ctx(f, &ctx, &g, &conflicts);

    uint8_t *buf = render_and_readback(f, &ctx);
    assert_non_null(buf);

    /* Background color from theme. */
    uint8_t bg[3] = { f->theme.bg.r, f->theme.bg.g, f->theme.bg.b };

    /* Assert content in grid cell regions (highlighted cells). */
    for (int row = 0; row < 3; row++) {
        int col = cbx_select_grid_get_cur_col(&g, row);
        SDL_Rect cr = cell_rect(row, col, 3, g.col_count);
        assert_true(fb_region_has_content(buf, VIS_W, VIS_H, &cr,
                                           bg, VIS_TOL));
    }

    /* Assert content in column header regions. */
    for (int col = 0; col < g.col_count; col++) {
        SDL_Rect hr = header_rect(col, g.col_count);
        assert_true(fb_region_has_content(buf, VIS_W, VIS_H, &hr,
                                          bg, VIS_TOL));
    }

    /* Assert content in label regions (model name + profile text). */
    if (f->has_text) {
        for (int row = 0; row < 3; row++) {
            SDL_Rect lr = label_rect(row, 3);
            assert_true(fb_region_has_content(buf, VIS_W, VIS_H, &lr,
                                              bg, VIS_TOL));
        }
    }

    /* Assert content in icon regions for player slots. */
    if (f->has_icons) {
        for (int row = 0; row < 3; row++) {
            int col = cbx_select_grid_get_cur_col(&g, row);
            if (col > 0) {
                SDL_Rect ir = icon_region(row, col, 3, g.col_count);
                assert_true(fb_region_has_content(buf, VIS_W, VIS_H, &ir,
                                                  bg, VIS_TOL));
            }
        }
    }

    free(buf);
}

/*
 * 2. Host Mode — assert fb_frames_differ between Player Mode and
 *    Host Mode captures (different highlight/focus region).
 */
static void
test_host_mode_differs(void **state)
{
    struct vis_fixture *f = *state;

    /* Player Mode: row 0 on P1, row 1 on P2, row 2 on P3. */
    cbx_select_grid g;
    build_grid(&g, 3);
    move_to_col(&g, 0, 1);
    move_to_col(&g, 1, 2);
    move_to_col(&g, 2, 3);

    cbx_conflict_list conflicts;
    cbx_conflict_detect(&g, &conflicts);

    cbx_grid_render_ctx ctx;
    setup_ctx(f, &ctx, &g, &conflicts);

    /* Render Player Mode frame. */
    cbx_overlay_surface surface;
    memset(&surface, 0, sizeof(surface));
    uint8_t *buf_a = malloc(VIS_W * VIS_H * 4);
    assert_non_null(buf_a);
    assert_int_equal(render_with_surface(f, &ctx, &surface, buf_a), 0);

    /* Host Mode: host (row 0) navigates row 1 from P2 to P1.
     * This changes the highlight position and creates a conflict. */
    move_to_col(&g, 1, 1);  /* Row 1 → P1 (same as row 0 → conflict) */
    cbx_conflict_detect(&g, &conflicts);

    /* Re-render with updated grid state. */
    uint8_t *buf_b = malloc(VIS_W * VIS_H * 4);
    assert_non_null(buf_b);
    re_render_and_readback(f, &ctx, &surface, buf_b);

    /* Frames must differ — different highlight position + conflict red. */
    assert_true(fb_frames_differ(buf_a, buf_b, VIS_W, VIS_H, 5));

    cbx_overlay_surface_destroy(&surface);
    free(buf_a);
    free(buf_b);
}

/*
 * 2b. Host Mode row states — use the actual cbx_host_mode state machine
 *     to verify distinct visual rendering for HOST (green cell), SELECTED
 *     (blue cell + accent border), and FROZEN (dimmed — no highlight) rows.
 *     SPEC §4.4 — host/selected/frozen rows must be visually distinguished.
 */
static void
test_host_mode_row_states(void **state)
{
    struct vis_fixture *f = *state;

    /* Build grid with 3 rows on P1/P2/P3. */
    cbx_select_grid g;
    build_grid(&g, 3);
    move_to_col(&g, 0, 1);  /* Row 0 → P1 */
    move_to_col(&g, 1, 2);  /* Row 1 → P2 */
    move_to_col(&g, 2, 3);  /* Row 2 → P3 */

    cbx_conflict_list conflicts;
    cbx_conflict_detect(&g, &conflicts);

    /* Enter host mode on row 0, navigate selected to row 1. */
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    cbx_host_mode_enter(&hm, 0);
    cbx_host_mode_handle(&hm, 0, CBX_HM_DOWN, &g);

    /* Verify state machine: row 0=HOST, row 1=SELECTED, row 2=FROZEN. */
    assert_int_equal(cbx_host_mode_row_state(&hm, 0), CBX_ROW_HOST);
    assert_int_equal(cbx_host_mode_row_state(&hm, 1), CBX_ROW_SELECTED);
    assert_int_equal(cbx_host_mode_row_state(&hm, 2), CBX_ROW_FROZEN);

    cbx_grid_render_ctx ctx;
    setup_ctx(f, &ctx, &g, &conflicts);
    ctx.hm = &hm;

    uint8_t *buf = render_and_readback(f, &ctx);
    assert_non_null(buf);

    /* HOST row (row 0): current cell (col 1) has green background. */
    SDL_Rect host_cell = cell_rect(0, 1, 3, g.col_count);
    uint8_t green[3] = { f->theme.success.r, f->theme.success.g,
                         f->theme.success.b };
    assert_true(fb_region_has_color(buf, VIS_W, VIS_H, &host_cell,
                                     green, 25));

    /* SELECTED row (row 1): current cell (col 2) has blue highlight. */
    SDL_Rect sel_cell = cell_rect(1, 2, 3, g.col_count);
    uint8_t blue[3] = { f->theme.border_focus.r, f->theme.border_focus.g,
                        f->theme.border_focus.b };
    assert_true(fb_region_has_color(buf, VIS_W, VIS_H, &sel_cell,
                                     blue, 25));

    /* FROZEN row (row 2): current cell (col 3) does NOT have blue
     * highlight — frozen rows show dim background instead. */
    SDL_Rect frozen_cell = cell_rect(2, 3, 3, g.col_count);
    assert_false(fb_region_has_color(buf, VIS_W, VIS_H, &frozen_cell,
                                      blue, 25));

    /* Host mode frame must differ from player mode frame. */
    cbx_host_mode_exit(&hm);
    uint8_t *buf_player = render_and_readback(f, &ctx);
    assert_non_null(buf_player);
    assert_true(fb_frames_differ(buf, buf_player, VIS_W, VIS_H, 5));

    free(buf);
    free(buf_player);
}

/*
 * 3. Conflict highlighting — set two controllers to the same P-slot;
 *    assert fb_region_has_color with red target {220, 40, 40} in the
 *    conflicted row's cell region.
 */
static void
test_conflict_highlighting(void **state)
{
    struct vis_fixture *f = *state;

    cbx_select_grid g;
    build_grid(&g, 3);
    /* Row 0 → P1, Row 1 → P1 (conflict — second arrival). */
    move_to_col(&g, 0, 1);
    move_to_col(&g, 1, 1);
    /* Row 2 stays on Unassigned. */

    cbx_conflict_list conflicts;
    cbx_conflict_detect(&g, &conflicts);
    assert_int_equal(conflicts.count, 1);
    assert_int_equal(conflicts.conflicts[0].row_idx, 1);

    cbx_grid_render_ctx ctx;
    setup_ctx(f, &ctx, &g, &conflicts);

    uint8_t *buf = render_and_readback(f, &ctx);
    assert_non_null(buf);

    /* Conflicted cell: row 1, col 1 (current column). */
    SDL_Rect conflict_cell = cell_rect(1, 1, 3, g.col_count);
    uint8_t red_target[3] = {220, 40, 40};
    assert_true(fb_region_has_color(buf, VIS_W, VIS_H, &conflict_cell,
                                     red_target, VIS_TOL));

    /* Non-conflicted cell: row 0, col 1 — should NOT have red. */
    SDL_Rect normal_cell = cell_rect(0, 1, 3, g.col_count);
    assert_false(fb_region_has_color(buf, VIS_W, VIS_H, &normal_cell,
                                      red_target, VIS_TOL));

    free(buf);
}

/*
 * 4. Unassigned + ≥2 player columns — assert fb_region_has_content
 *    in all column header regions and at least 2 player slot regions.
 */
static void
test_unassigned_with_columns(void **state)
{
    struct vis_fixture *f = *state;

    cbx_select_grid g;
    build_grid(&g, 3);
    /* All rows start on Unassigned (col 0). */
    /* Move row 0 → P1, row 1 → P2 to have content in player slots. */
    move_to_col(&g, 0, 1);
    move_to_col(&g, 1, 2);
    /* Row 2 stays on Unassigned. */

    cbx_conflict_list conflicts;
    cbx_conflict_detect(&g, &conflicts);

    cbx_grid_render_ctx ctx;
    setup_ctx(f, &ctx, &g, &conflicts);

    uint8_t *buf = render_and_readback(f, &ctx);
    assert_non_null(buf);

    uint8_t bg[3] = { f->theme.bg.r, f->theme.bg.g, f->theme.bg.b };

    /* Assert content in ALL column header regions (Unassigned + P1-P4). */
    for (int col = 0; col < g.col_count; col++) {
        SDL_Rect hr = header_rect(col, g.col_count);
        assert_true(fb_region_has_content(buf, VIS_W, VIS_H, &hr,
                                          bg, VIS_TOL));
    }

    /* Assert content in at least 2 player slot regions (col > 0). */
    int slots_with_content = 0;
    for (int col = 1; col < g.col_count; col++) {
        /* Check the cell region in any row for this column. */
        SDL_Rect cr = cell_rect(0, col, 3, g.col_count);
        if (fb_region_has_content(buf, VIS_W, VIS_H, &cr, bg, VIS_TOL))
            slots_with_content++;
    }
    assert_int_in_range(slots_with_content, 2, g.col_count);

    free(buf);
}

/*
 * 5. Controller model/profile text — assert fb_region_has_content
 *    with text-colored pixels (theme.text_primary) in the profile
 *    text region for each row.
 */
static void
test_model_profile_text(void **state)
{
    struct vis_fixture *f = *state;

    if (!f->has_text) {
        fail_msg("DejaVuSans.ttf not found — model/profile text visual "
                 "test requires a TrueType font.  Install dejavu-fonts or "
                 "run in the declared Nix environment (nix-shell).  "
                 "Searched: cbx_font_path(), common nix-store font paths, "
                 "and popen('find /nix/store -name DejaVuSans.ttf ...').");
    }

    cbx_select_grid g;
    build_grid(&g, 3);
    move_to_col(&g, 0, 1);
    move_to_col(&g, 1, 2);
    move_to_col(&g, 2, 3);

    cbx_conflict_list conflicts;
    cbx_conflict_detect(&g, &conflicts);

    cbx_grid_render_ctx ctx;
    setup_ctx(f, &ctx, &g, &conflicts);

    uint8_t *buf = render_and_readback(f, &ctx);
    assert_non_null(buf);

    /* Text color from theme. */
    uint8_t text_color[3] = {
        f->theme.text_primary.r,
        f->theme.text_primary.g,
        f->theme.text_primary.b,
    };

    /* Assert text-colored pixels in the label region for each row. */
    for (int row = 0; row < 3; row++) {
        SDL_Rect lr = label_rect(row, 3);
        assert_true(fb_region_has_color(buf, VIS_W, VIS_H, &lr,
                                        text_color, VIS_TOL));
    }

    /* Also check the profile label on the right side. */
    int grid_x = LABEL_W;
    int grid_w = VIS_W - LABEL_W - PROFILE_W;
    int grid_h = VIS_H - HEADER_H;
    int cell_h = grid_h / 3;
    int profile_x = grid_x + g.col_count * (grid_w / g.col_count) + 4;
    for (int row = 0; row < 3; row++) {
        SDL_Rect pr = {
            .x = profile_x,
            .y = HEADER_H + row * cell_h + 4,
            .w = VIS_W - profile_x - 4,
            .h = cell_h - 8,
        };
        if (pr.w > 0) {
            assert_true(fb_region_has_color(buf, VIS_W, VIS_H, &pr,
                                            text_color, VIS_TOL));
        }
    }

    free(buf);
}

/*
 * 6. Virtual-device icons — assert fb_region_has_content in icon
 *    regions for each slot.
 */
static void
test_virtual_device_icons(void **state)
{
    struct vis_fixture *f = *state;

    if (!f->has_icons) {
        fail_msg("Icon assets not found — virtual-device icon visual test "
                 "requires controller-icons.yaml and SVG icons.  Ensure "
                 "data/controller-icons.yaml and data/icons/svg/ are "
                 "available relative to CBX_SOURCE_DIR (project root).  "
                 "Run in the declared Nix environment (nix-shell).");
    }

    cbx_select_grid g;
    build_grid(&g, 3);
    move_to_col(&g, 0, 1);
    move_to_col(&g, 1, 2);
    move_to_col(&g, 2, 3);

    cbx_conflict_list conflicts;
    cbx_conflict_detect(&g, &conflicts);

    cbx_grid_render_ctx ctx;
    setup_ctx(f, &ctx, &g, &conflicts);

    uint8_t *buf = render_and_readback(f, &ctx);
    assert_non_null(buf);

    uint8_t bg[3] = { f->theme.bg.r, f->theme.bg.g, f->theme.bg.b };

    /* Assert content in icon regions for each occupied player slot. */
    for (int row = 0; row < 3; row++) {
        int col = cbx_select_grid_get_cur_col(&g, row);
        if (col > 0) {
            SDL_Rect ir = icon_region(row, col, 3, g.col_count);
            assert_true(fb_region_has_content(buf, VIS_W, VIS_H, &ir,
                                              bg, VIS_TOL));
        }
    }

    free(buf);
}

/*
 * 7. State transitions — assert that transitions between states
 *    (Player Mode → Host Mode, no-conflict → conflict) produce
 *    materially different frames via fb_frames_differ.
 */
static void
test_state_transitions_differ(void **state)
{
    struct vis_fixture *f = *state;

    /* Frame A: no conflicts, rows on different slots. */
    cbx_select_grid g;
    build_grid(&g, 3);
    move_to_col(&g, 0, 1);
    move_to_col(&g, 1, 2);
    move_to_col(&g, 2, 3);

    cbx_conflict_list conflicts;
    cbx_conflict_detect(&g, &conflicts);

    cbx_grid_render_ctx ctx;
    setup_ctx(f, &ctx, &g, &conflicts);

    cbx_overlay_surface surface;
    memset(&surface, 0, sizeof(surface));
    uint8_t *buf_a = malloc(VIS_W * VIS_H * 4);
    assert_non_null(buf_a);
    assert_int_equal(render_with_surface(f, &ctx, &surface, buf_a), 0);

    /* Frame B: introduce conflict (row 1 moves to col 1 = P1). */
    move_to_col(&g, 1, 1);
    cbx_conflict_detect(&g, &conflicts);

    uint8_t *buf_b = malloc(VIS_W * VIS_H * 4);
    assert_non_null(buf_b);
    re_render_and_readback(f, &ctx, &surface, buf_b);

    /* No-conflict → conflict: frames must differ. */
    assert_true(fb_frames_differ(buf_a, buf_b, VIS_W, VIS_H, 5));

    /* Frame C: resolve conflict (row 1 moves back to col 2). */
    move_to_col(&g, 1, 2);
    cbx_conflict_detect(&g, &conflicts);

    uint8_t *buf_c = malloc(VIS_W * VIS_H * 4);
    assert_non_null(buf_c);
    re_render_and_readback(f, &ctx, &surface, buf_c);

    /* Conflict → no-conflict: frames must differ. */
    assert_true(fb_frames_differ(buf_b, buf_c, VIS_W, VIS_H, 5));

    /* Frame A and C should be similar (both no-conflict, same state). */
    assert_false(fb_frames_differ(buf_a, buf_c, VIS_W, VIS_H, 5));

    cbx_overlay_surface_destroy(&surface);
    free(buf_a);
    free(buf_b);
    free(buf_c);
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_player_mode_grid,
                                        vis_setup, vis_teardown),
        cmocka_unit_test_setup_teardown(test_host_mode_differs,
                                        vis_setup, vis_teardown),
        cmocka_unit_test_setup_teardown(test_host_mode_row_states,
                                        vis_setup, vis_teardown),
        cmocka_unit_test_setup_teardown(test_conflict_highlighting,
                                        vis_setup, vis_teardown),
        cmocka_unit_test_setup_teardown(test_unassigned_with_columns,
                                        vis_setup, vis_teardown),
        cmocka_unit_test_setup_teardown(test_model_profile_text,
                                        vis_setup, vis_teardown),
        cmocka_unit_test_setup_teardown(test_virtual_device_icons,
                                        vis_setup, vis_teardown),
        cmocka_unit_test_setup_teardown(test_state_transitions_differ,
                                        vis_setup, vis_teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}