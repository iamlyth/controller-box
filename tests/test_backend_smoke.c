/*
 * test_backend_smoke.c — Accelerated backend smoke test (Task 10,
 * SPEC §11.1.6).
 *
 * Verifies that rendering through an accelerated SDL2 backend (OpenGL
 * or OpenGL ES) produces correct pixel output — not just successful
 * draw calls, but actual framebuffer content verified via fb_read_pixels.
 *
 * The test:
 *   1. Creates an SDL2 renderer with SDL_RENDERER_ACCELERATED |
 *      SDL_RENDERER_TARGETTEXTURE.
 *   2. Detects the backend name via SDL_GetRendererInfo.
 *   3. If no accelerated backend is available, exits with code 77
 *      (ctest SKIP_RETURN_CODE) so CI does not fail.
 *   4. If an accelerated backend is available:
 *      a. Renders a representative overlay grid frame via
 *         cbx_overlay_surface_render + cbx_select_grid_render_cb.
 *      b. Renders a manager tab frame via cbx_manager_render().
 *      c. Reads back pixels via fb_read_pixels.
 *      d. Asserts fb_region_has_content in expected regions (grid
 *         cells, tab bar, body).
 *      e. Asserts no all-black or all-background frames.
 *      f. Asserts the accelerated output is broadly consistent with
 *         the software-renderer golden baselines (fb_golden_compare
 *         with ±3 per-channel, <2% image tolerance).
 *
 * This test does NOT set SDL_VIDEODRIVER=dummy — it requires a real
 * display with GPU acceleration.  In headless CI it skips (exit 77).
 */
#include "overlay/conflict.h"
#include "overlay/grid_render.h"
#include "overlay/surface_build.h"
#include "identify/assign.h"
#include "config/config_settings.h"
#include "config/config_assignments.h"
#include "config/config_paths.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "icons/icon_cache.h"
#include "icons/icon_map.h"
#include "manager/manager.h"
#include "fb_assert.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>

#ifndef CBX_SOURCE_DIR
#define CBX_SOURCE_DIR "."
#endif

#define SVG_DIR    CBX_SOURCE_DIR "/data/icons"
#define YAML_DIR   CBX_SOURCE_DIR "/data/"
#define GOLDEN_DIR CBX_SOURCE_DIR "/tests/golden/"

/* Overlay rendering dimensions. */
#define VIS_W 800
#define VIS_H 600

/* Manager rendering dimensions. */
#define MGR_W 1280
#define MGR_H  720

/* Tolerance for content detection. */
#define CONTENT_TOL 10

/* Layout constants — must match grid_render.c. */
#define HEADER_H    32
#define LABEL_W     200
#define PROFILE_W   160
#define CELL_MARGIN 4

/* Manager layout constants. */
#define TABBAR_H  48

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

static const char *
find_font(void)
{
    const char *p = cbx_font_path();
    if (p)
        return p;

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

    FILE *fp = popen(
        "find /nix/store -maxdepth 4 -name DejaVuSans.ttf "
        "-path '*/share/X11/fonts/*' 2>/dev/null | head -1",
        "r");
    if (fp) {
        if (fgets(found, sizeof(found), fp) && found[0] != '\0') {
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

/*
 * Check that a buffer is not all-black (all pixels = {0,0,0}).
 * Returns true if at least one pixel differs from black.
 */
static bool
frame_not_all_black(const uint8_t *buf, int w, int h)
{
    const uint8_t black[3] = {0, 0, 0};
    SDL_Rect full = {0, 0, w, h};
    return fb_region_has_content(buf, w, h, &full, black, 0);
}

/*
 * Check that a buffer is not all-background.
 * Returns true if at least one pixel differs from the given bg color.
 */
static bool
frame_not_all_bg(const uint8_t *buf, int w, int h,
                 const uint8_t bg[3])
{
    SDL_Rect full = {0, 0, w, h};
    return fb_region_has_content(buf, w, h, &full, bg, CONTENT_TOL);
}

/*
 * Simple assertion helper — prints [ OK ] or [FAIL] and returns -1
 * on failure so the caller can jump to cleanup.
 */
static int
check(bool cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "  [FAIL] %s\n", msg);
        return -1;
    }
    printf("  [ OK ] %s\n", msg);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Overlay backend smoke test                                        */
/* ------------------------------------------------------------------ */

static int
test_overlay_accelerated(SDL_Renderer *renderer)
{
    int ret = 1;
    uint8_t *buf = NULL;
    bool surface_inited = false;
    bool caches_inited = false;

    /* Theme. */
    cbx_theme theme;
    cbx_theme_default(&theme);

    /* Text cache + font. */
    cbx_text_cache text_cache;
    int font_id = -1;
    bool has_text = false;

    if (cbx_text_cache_init(&text_cache, renderer) == 0) {
        const char *font = find_font();
        if (font) {
            font_id = cbx_text_load_font(&text_cache, font, 18);
            if (font_id >= 0)
                has_text = true;
        }
    }

    /* Icon cache + map. */
    cbx_icon_cache icon_cache;
    cbx_icon_map icon_map;
    bool has_icons = false;

    cbx_icon_map_init(&icon_map);
    char yaml_path[PATH_MAX + 64];
    snprintf(yaml_path, sizeof(yaml_path), "%s/controller-icons.yaml",
             YAML_DIR);
    if (cbx_icon_map_load(&icon_map, yaml_path) == 0) {
        if (cbx_icon_cache_init(&icon_cache, renderer, SVG_DIR, 64) == 0) {
            cbx_icon_cache_load(&icon_cache, &icon_map);
            has_icons = true;
        }
    }
    caches_inited = true;

    /* Build grid: 3 controllers on P1, P2, P3 (same as golden baseline). */
    cbx_select_grid g;
    build_grid(&g, 3);
    move_to_col(&g, 0, 1);
    move_to_col(&g, 1, 2);
    move_to_col(&g, 2, 3);

    cbx_conflict_list conflicts;
    cbx_conflict_detect(&g, &conflicts);

    /* Set up render context. */
    cbx_grid_render_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.grid = &g;
    ctx.conflicts = &conflicts;
    ctx.theme = &theme;
    if (has_text) {
        ctx.text_cache = &text_cache;
        ctx.font_id = font_id;
    }
    if (has_icons) {
        ctx.icon_cache = &icon_cache;
        ctx.icon_map = &icon_map;
    }

    /* Render to overlay surface. */
    cbx_overlay_surface surface;
    memset(&surface, 0, sizeof(surface));
    if (cbx_overlay_surface_init(&surface, renderer, VIS_W, VIS_H, 1.0) != 0) {
        fprintf(stderr, "  [FAIL] overlay surface init\n");
        goto cleanup;
    }
    surface_inited = true;

    cbx_overlay_surface_mark_dirty_all(&surface);
    if (cbx_overlay_surface_render(&surface, renderer,
                                    cbx_select_grid_render_cb, &ctx) != 0) {
        fprintf(stderr, "  [FAIL] overlay surface render\n");
        goto cleanup;
    }

    /* Read back pixels. */
    buf = malloc(VIS_W * VIS_H * 4);
    if (!buf) {
        fprintf(stderr, "  [FAIL] pixel buffer alloc\n");
        goto cleanup;
    }

    SDL_SetRenderTarget(renderer, cbx_overlay_surface_get_texture(&surface));
    int rc = fb_read_pixels(renderer, NULL, buf, VIS_W * VIS_H * 4);
    SDL_SetRenderTarget(renderer, NULL);

    if (rc != 0) {
        fprintf(stderr, "  [FAIL] fb_read_pixels\n");
        goto cleanup;
    }

    /* Assert no all-black frame. */
    if (check(frame_not_all_black(buf, VIS_W, VIS_H),
              "overlay: frame is not all-black") != 0)
        goto cleanup;

    /* Assert no all-background frame. */
    if (check(frame_not_all_bg(buf, VIS_W, VIS_H,
                               (uint8_t[]){18, 18, 28}),
              "overlay: frame is not all-background") != 0)
        goto cleanup;

    /* Assert content in grid cell regions. */
    for (int row = 0; row < 3; row++) {
        SDL_Rect cell = cell_rect(row, 1, 3, 5);
        char msg[64];
        snprintf(msg, sizeof(msg),
                 "overlay: grid cell content (row %d)", row);
        if (check(fb_region_has_content(buf, VIS_W, VIS_H, &cell,
                                        (uint8_t[]){18, 18, 28},
                                        CONTENT_TOL), msg) != 0)
            goto cleanup;
    }

    /* Assert content in label region (left column). */
    {
        SDL_Rect label_region = {0, HEADER_H, LABEL_W, VIS_H - HEADER_H};
        if (check(fb_region_has_content(buf, VIS_W, VIS_H, &label_region,
                                        (uint8_t[]){18, 18, 28},
                                        CONTENT_TOL),
                  "overlay: label region content") != 0)
            goto cleanup;
    }

    /* Compare with golden baseline (software renderer output). */
    {
        char golden_path[PATH_MAX];
        snprintf(golden_path, sizeof(golden_path),
                 "%soverlay_player_mode.png", GOLDEN_DIR);

        if (access(golden_path, R_OK) == 0) {
            if (check(fb_golden_compare(buf, VIS_W, VIS_H,
                                        golden_path, 3, 2),
                      "overlay: accelerated output matches golden baseline") != 0)
                goto cleanup;
        } else {
            printf("  [SKIP] overlay: golden baseline not found at %s\n",
                   golden_path);
        }
    }

    ret = 0;  /* success */

cleanup:
    free(buf);
    if (surface_inited)
        cbx_overlay_surface_destroy(&surface);
    if (caches_inited) {
        if (has_icons)
            cbx_icon_cache_cleanup(&icon_cache);
        if (text_cache.renderer)
            cbx_text_cache_cleanup(&text_cache);
    }
    return ret;
}

/* ------------------------------------------------------------------ */
/*  Manager backend smoke test                                        */
/* ------------------------------------------------------------------ */

static int
test_manager_accelerated(void)
{
    int ret = 1;
    uint8_t *buf = NULL;
    bool mgr_inited = false;
    char tmp[256];
    char saved_home[256];
    bool saved_home_set = false;

    /* Isolated HOME so profiles tab refresh succeeds. */
    const char *home = getenv("HOME");
    if (home) {
        snprintf(saved_home, sizeof(saved_home), "%s", home);
        saved_home_set = true;
    }
    snprintf(tmp, sizeof(tmp), "/tmp/cbx_bsmoke_%d", (int)getpid());
    {
        char rmcmd[PATH_MAX * 2 + 32];
        snprintf(rmcmd, sizeof(rmcmd), "rm -rf '%s'", tmp);
        int r0 = system(rmcmd);
        (void)r0;
    }
    mkdir(tmp, 0700);
    setenv("HOME", tmp, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");
    unsetenv("FLATPAK_ID");

    char profiles_dir[PATH_MAX + 64];
    snprintf(profiles_dir, sizeof(profiles_dir),
             "%s/.local/share/inputplumber/profiles", tmp);
    cbx_ensure_dir(profiles_dir, 0700);

    /* Init manager — cbx_renderer_init tries accelerated first. */
    cbx_manager mgr;
    memset(&mgr, 0, sizeof(mgr));
    const char *font = find_font();
    if (cbx_manager_init(&mgr, font) != 0) {
        fprintf(stderr, "  [FAIL] cbx_manager_init\n");
        goto cleanup_home;
    }
    mgr_inited = true;

    /* Check that the manager's renderer is accelerated. */
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(mgr.rend.renderer, &info) != 0) {
        fprintf(stderr, "  [FAIL] SDL_GetRendererInfo on manager renderer\n");
        goto cleanup;
    }

    printf("  manager renderer: %s (flags=0x%x)\n",
           info.name ? info.name : "(null)", (unsigned)info.flags);

    if (!(info.flags & SDL_RENDERER_ACCELERATED)) {
        fprintf(stderr, "  [FAIL] manager renderer is not accelerated\n");
        goto cleanup;
    }

    /* Render the controllers tab (default tab, degraded mode). */
    cbx_manager_render(&mgr);

    /* Read back pixels. */
    buf = malloc(MGR_W * MGR_H * 4);
    if (!buf) {
        fprintf(stderr, "  [FAIL] pixel buffer alloc\n");
        goto cleanup;
    }

    int rc = fb_read_pixels(mgr.rend.renderer, NULL, buf, MGR_W * MGR_H * 4);
    if (rc != 0) {
        fprintf(stderr, "  [FAIL] fb_read_pixels on manager\n");
        goto cleanup;
    }

    /* Assert no all-black frame. */
    if (check(frame_not_all_black(buf, MGR_W, MGR_H),
              "manager: frame is not all-black") != 0)
        goto cleanup;

    /* Assert no all-background frame. */
    if (check(frame_not_all_bg(buf, MGR_W, MGR_H,
                               (uint8_t[]){18, 18, 28}),
              "manager: frame is not all-background") != 0)
        goto cleanup;

    /* Assert content in tab bar region. */
    {
        SDL_Rect tabbar = {0, 0, MGR_W, TABBAR_H};
        if (check(fb_region_has_content(buf, MGR_W, MGR_H, &tabbar,
                                        (uint8_t[]){18, 18, 28},
                                        CONTENT_TOL),
                  "manager: tab bar content") != 0)
            goto cleanup;
    }

    /* Assert content in body region (device list area). */
    {
        SDL_Rect body = {16, 64, 1248, 400};
        if (check(fb_region_has_content(buf, MGR_W, MGR_H, &body,
                                        (uint8_t[]){18, 18, 28},
                                        CONTENT_TOL),
                  "manager: body content") != 0)
            goto cleanup;
    }

    /* Assert content in button region. */
    {
        SDL_Rect buttons = {16, 480, 200, 44};
        if (check(fb_region_has_content(buf, MGR_W, MGR_H, &buttons,
                                        (uint8_t[]){18, 18, 28},
                                        CONTENT_TOL),
                  "manager: button region content") != 0)
            goto cleanup;
    }

    /* Compare with golden baseline (software renderer output). */
    {
        char golden_path[PATH_MAX];
        snprintf(golden_path, sizeof(golden_path),
                 "%smanager_controllers_degraded.png", GOLDEN_DIR);

        if (access(golden_path, R_OK) == 0) {
            if (check(fb_golden_compare(buf, MGR_W, MGR_H,
                                        golden_path, 3, 2),
                      "manager: accelerated output matches golden baseline") != 0)
                goto cleanup;
        } else {
            printf("  [SKIP] manager: golden baseline not found at %s\n",
                   golden_path);
        }
    }

    ret = 0;  /* success */

cleanup:
    free(buf);
    if (mgr_inited)
        cbx_manager_shutdown(&mgr);

cleanup_home:
    if (saved_home_set) setenv("HOME", saved_home, 1);
    else unsetenv("HOME");

    {
        char rmcmd[PATH_MAX * 2 + 32];
        snprintf(rmcmd, sizeof(rmcmd), "rm -rf '%s'", tmp);
        int r = system(rmcmd);
        (void)r;
    }

    return ret;
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */

int
main(void)
{
    /* Do NOT set SDL_VIDEODRIVER=dummy — we need a real display. */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "test_backend_smoke: SDL_Init failed: %s\n",
                SDL_GetError());
        /* No video subsystem → no accelerated backend → skip. */
        return 77;
    }

    /* Create a window with an accelerated renderer. */
    SDL_Window *window = SDL_CreateWindow(
        "controller-box backend smoke test",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        VIS_W, VIS_H, SDL_WINDOW_HIDDEN);
    if (!window) {
        fprintf(stderr, "test_backend_smoke: SDL_CreateWindow failed: %s\n",
                SDL_GetError());
        SDL_Quit();
        return 77;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    if (!renderer) {
        fprintf(stderr, "test_backend_smoke: no accelerated renderer: %s\n",
                SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 77;  /* skip — no accelerated backend */
    }

    /* Verify the renderer is actually accelerated. */
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(renderer, &info) != 0) {
        fprintf(stderr, "test_backend_smoke: SDL_GetRendererInfo failed\n");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 77;
    }

    printf("test_backend_smoke: backend = %s (flags=0x%x)\n",
           info.name ? info.name : "(null)", (unsigned)info.flags);

    if (!(info.flags & SDL_RENDERER_ACCELERATED)) {
        printf("test_backend_smoke: backend is not accelerated — skipping\n");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 77;
    }

    /* Check for TARGETTEXTURE support (required for overlay rendering). */
    if (!(info.flags & SDL_RENDERER_TARGETTEXTURE)) {
        fprintf(stderr, "test_backend_smoke: accelerated backend lacks "
                "TARGETTEXTURE — skipping\n");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 77;
    }

    printf("test_backend_smoke: accelerated backend available (%s)\n",
           info.name ? info.name : "unknown");

    /* ── Overlay test ─────────────────────────────────────────── */
    printf("[overlay]\n");
    if (test_overlay_accelerated(renderer) != 0) {
        fprintf(stderr, "test_backend_smoke: overlay test FAILED\n");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    /* Clean up overlay renderer before manager test (manager creates
     * its own window + renderer). */
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    /* ── Manager test ─────────────────────────────────────────── */
    printf("[manager]\n");
    if (test_manager_accelerated() != 0) {
        fprintf(stderr, "test_backend_smoke: manager test FAILED\n");
        SDL_Quit();
        return 1;
    }

    SDL_Quit();
    printf("test_backend_smoke: ALL PASS\n");
    return 0;
}