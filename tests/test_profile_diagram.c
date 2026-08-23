/*
 * test_profile_diagram.c — Tests for the controller diagram widget
 * (Task 37).
 *
 * Tests button position lookup, name mapping, highlight state, and
 * rendering with the SDL2 dummy driver.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <SDL2/SDL.h>
#include <string.h>
#include <errno.h>

#include "manager/profile_diagram.h"
#include "config/config_paths.h"
#include "icons/icon_cache.h"
#include "icons/icon_map.h"
#include "icons/icon_lookup.h"
#include "test_harness.h"
#include "fb_assert.h"

/* ------------------------------------------------------------------ */
/*  Fixture                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    TestSdlState sdl;
    cbx_profile_diagram diag;
    cbx_theme theme;
} pd_fixture;

static int setup(void **state)
{
    pd_fixture *f = malloc(sizeof(*f));
    memset(f, 0, sizeof(*f));

    int rc = test_harness_sdl_init(&f->sdl);
    assert_int_equal(rc, 0);

    cbx_theme_default(&f->theme);

    rc = cbx_profile_diagram_init(&f->diag, f->sdl.renderer, NULL, &f->theme);
    assert_int_equal(rc, 0);

    *state = f;
    return 0;
}

static int teardown(void **state)
{
    pd_fixture *f = *state;
    cbx_profile_diagram_shutdown(&f->diag);
    test_harness_sdl_shutdown(&f->sdl);
    free(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Framebuffer helpers (real pixel readback, not "no-crash" stubs)    */
/* ------------------------------------------------------------------ */

/* Read the full renderer framebuffer into a fresh heap buffer. */
static uint8_t *
diag_read_fb(SDL_Renderer *r, int *w, int *h)
{
    assert_int_equal(SDL_GetRendererOutputSize(r, w, h), 0);
    uint8_t *buf = malloc((size_t)(*w) * (*h) * 4);
    assert_non_null(buf);
    assert_int_equal(fb_read_pixels(r, NULL, buf, (size_t)(*w) * (*h) * 4), 0);
    return buf;
}

/* Clear the renderer to a known background distinct from the panel fill. */
static void
diag_clear(SDL_Renderer *r, uint8_t c)
{
    SDL_SetRenderDrawColor(r, c, c, c, 255);
    SDL_RenderClear(r);
}

/* Compute the on-screen rect of a highlighted button within the diagram. */
static SDL_Rect
diag_button_rect(const cbx_diag_button_pos *pos, const SDL_Rect *diag)
{
    SDL_Rect r;
    r.x = diag->x + (int)(pos->x * (float)diag->w);
    r.y = diag->y + (int)(pos->y * (float)diag->h);
    r.w = (int)(pos->w * (float)diag->w);
    r.h = (int)(pos->h * (float)diag->h);
    if (r.w < 1) r.w = 1;
    if (r.h < 1) r.h = 1;
    return r;
}

/*
 * Assert that the highlighted button region is painted over the panel
 * background (semantic highlight outcome).  A non-highlighted button
 * region is exactly panel_bg, so `expected=false` proves no highlight.
 */
static void
diag_assert_highlight(SDL_Renderer *r, const SDL_Rect *diag,
                      cbx_diag_button btn, bool expected)
{
    const cbx_diag_button_pos *pos = cbx_profile_diagram_get_button_pos(btn);
    assert_non_null(pos);
    SDL_Rect br = diag_button_rect(pos, diag);
    int w, h;
    uint8_t *buf = diag_read_fb(r, &w, &h);
    /* panel_bg (theme) is what a non-highlighted button region looks like. */
    uint8_t panel_bg[3] = {30, 30, 42};
    bool has = fb_region_has_content(buf, w, h, &br, panel_bg, 15);
    free(buf);
    assert_int_equal(has, expected);
}

/* ------------------------------------------------------------------ */
/*  Tests: button position lookup                                      */
/* ------------------------------------------------------------------ */

static void test_button_count(void **state)
{
    (void)state;
    assert_int_equal(cbx_profile_diagram_button_count(), CBX_DIAG_BTN_COUNT);
    assert_int_equal(CBX_DIAG_BTN_COUNT, 17);
}

static void test_button_pos_valid(void **state)
{
    (void)state;
    const cbx_diag_button_pos *pos;

    pos = cbx_profile_diagram_get_button_pos(CBX_DIAG_BTN_A);
    assert_non_null(pos);
    assert_string_equal(pos->name, "A");
    assert_true(pos->x >= 0.0f && pos->x <= 1.0f);
    assert_true(pos->y >= 0.0f && pos->y <= 1.0f);
    assert_true(pos->w > 0.0f && pos->w <= 1.0f);
    assert_true(pos->h > 0.0f && pos->h <= 1.0f);
}

static void test_button_pos_all_valid(void **state)
{
    (void)state;
    for (int i = 0; i < CBX_DIAG_BTN_COUNT; i++) {
        const cbx_diag_button_pos *pos =
            cbx_profile_diagram_get_button_pos((cbx_diag_button)i);
        assert_non_null(pos);
        assert_non_null(pos->name);
        assert_true(pos->name[0] != '\0');
        assert_true(pos->x >= 0.0f && pos->x <= 1.0f);
        assert_true(pos->y >= 0.0f && pos->y <= 1.0f);
        assert_true(pos->w > 0.0f && pos->w <= 1.0f);
        assert_true(pos->h > 0.0f && pos->h <= 1.0f);
    }
}

static void test_button_pos_invalid(void **state)
{
    (void)state;
    assert_null(cbx_profile_diagram_get_button_pos(CBX_DIAG_BTN_NONE));
    assert_null(cbx_profile_diagram_get_button_pos(-1));
    assert_null(cbx_profile_diagram_get_button_pos(999));
}

/* ------------------------------------------------------------------ */
/*  Tests: name mapping                                                */
/* ------------------------------------------------------------------ */

static void test_button_from_name_known(void **state)
{
    (void)state;
    assert_int_equal(cbx_profile_diagram_button_from_name("A"),
                       CBX_DIAG_BTN_A);
    assert_int_equal(cbx_profile_diagram_button_from_name("B"),
                       CBX_DIAG_BTN_B);
    assert_int_equal(cbx_profile_diagram_button_from_name("X"),
                       CBX_DIAG_BTN_X);
    assert_int_equal(cbx_profile_diagram_button_from_name("Y"),
                       CBX_DIAG_BTN_Y);
    assert_int_equal(cbx_profile_diagram_button_from_name("Up"),
                       CBX_DIAG_BTN_UP);
    assert_int_equal(cbx_profile_diagram_button_from_name("Down"),
                       CBX_DIAG_BTN_DOWN);
    assert_int_equal(cbx_profile_diagram_button_from_name("Left"),
                       CBX_DIAG_BTN_LEFT);
    assert_int_equal(cbx_profile_diagram_button_from_name("Right"),
                       CBX_DIAG_BTN_RIGHT);
    assert_int_equal(cbx_profile_diagram_button_from_name("Start"),
                       CBX_DIAG_BTN_START);
    assert_int_equal(cbx_profile_diagram_button_from_name("Select"),
                       CBX_DIAG_BTN_SELECT);
    assert_int_equal(cbx_profile_diagram_button_from_name("Guide"),
                       CBX_DIAG_BTN_GUIDE);
    assert_int_equal(cbx_profile_diagram_button_from_name("L1"),
                       CBX_DIAG_BTN_L1);
    assert_int_equal(cbx_profile_diagram_button_from_name("R1"),
                       CBX_DIAG_BTN_R1);
    assert_int_equal(cbx_profile_diagram_button_from_name("L2"),
                       CBX_DIAG_BTN_L2);
    assert_int_equal(cbx_profile_diagram_button_from_name("R2"),
                       CBX_DIAG_BTN_R2);
    assert_int_equal(cbx_profile_diagram_button_from_name("L3"),
                       CBX_DIAG_BTN_L3);
    assert_int_equal(cbx_profile_diagram_button_from_name("R3"),
                       CBX_DIAG_BTN_R3);
}

static void test_button_from_name_unknown(void **state)
{
    (void)state;
    assert_int_equal(cbx_profile_diagram_button_from_name("Foo"),
                       CBX_DIAG_BTN_NONE);
    assert_int_equal(cbx_profile_diagram_button_from_name(""),
                       CBX_DIAG_BTN_NONE);
    assert_int_equal(cbx_profile_diagram_button_from_name(NULL),
                       CBX_DIAG_BTN_NONE);
}

static void test_button_name_roundtrip(void **state)
{
    (void)state;
    for (int i = 0; i < CBX_DIAG_BTN_COUNT; i++) {
        cbx_diag_button btn = (cbx_diag_button)i;
        const char *name = cbx_profile_diagram_button_name(btn);
        assert_non_null(name);
        cbx_diag_button back = cbx_profile_diagram_button_from_name(name);
        assert_int_equal(back, btn);
    }
}

static void test_button_name_invalid(void **state)
{
    (void)state;
    assert_null(cbx_profile_diagram_button_name(CBX_DIAG_BTN_NONE));
    assert_null(cbx_profile_diagram_button_name(-1));
    assert_null(cbx_profile_diagram_button_name(999));
}

/* ------------------------------------------------------------------ */
/*  Tests: highlight state                                             */
/* ------------------------------------------------------------------ */

static void test_highlight_set_get(void **state)
{
    pd_fixture *f = *state;

    cbx_profile_diagram_highlight(&f->diag, CBX_DIAG_BTN_A);
    assert_int_equal(cbx_profile_diagram_get_highlight(&f->diag),
                       CBX_DIAG_BTN_A);
}

static void test_highlight_clear(void **state)
{
    pd_fixture *f = *state;

    cbx_profile_diagram_highlight(&f->diag, CBX_DIAG_BTN_START);
    assert_int_equal(cbx_profile_diagram_get_highlight(&f->diag),
                       CBX_DIAG_BTN_START);

    cbx_profile_diagram_clear_highlight(&f->diag);
    assert_int_equal(cbx_profile_diagram_get_highlight(&f->diag),
                       CBX_DIAG_BTN_NONE);
}

static void test_highlight_none(void **state)
{
    pd_fixture *f = *state;

    /* Initially no highlight */
    assert_int_equal(cbx_profile_diagram_get_highlight(&f->diag),
                       CBX_DIAG_BTN_NONE);

    /* Setting to NONE clears */
    cbx_profile_diagram_highlight(&f->diag, CBX_DIAG_BTN_B);
    cbx_profile_diagram_highlight(&f->diag, CBX_DIAG_BTN_NONE);
    assert_int_equal(cbx_profile_diagram_get_highlight(&f->diag),
                       CBX_DIAG_BTN_NONE);
}

static void test_highlight_all_buttons(void **state)
{
    pd_fixture *f = *state;

    for (int i = 0; i < CBX_DIAG_BTN_COUNT; i++) {
        cbx_diag_button btn = (cbx_diag_button)i;
        cbx_profile_diagram_highlight(&f->diag, btn);
        assert_int_equal(cbx_profile_diagram_get_highlight(&f->diag), btn);
    }
}

static void test_highlight_invalid(void **state)
{
    pd_fixture *f = *state;

    /* Invalid button IDs should be ignored */
    cbx_profile_diagram_highlight(&f->diag, CBX_DIAG_BTN_A);
    cbx_profile_diagram_highlight(&f->diag, (cbx_diag_button)999);
    assert_int_equal(cbx_profile_diagram_get_highlight(&f->diag),
                       CBX_DIAG_BTN_A);
}

static void test_highlight_null_safe(void **state)
{
    (void)state;
    /* NULL-safe operations */
    cbx_profile_diagram_highlight(NULL, CBX_DIAG_BTN_A);
    cbx_profile_diagram_clear_highlight(NULL);
    assert_int_equal(cbx_profile_diagram_get_highlight(NULL),
                       CBX_DIAG_BTN_NONE);
}

/* ------------------------------------------------------------------ */
/*  Tests: lifecycle                                                   */
/* ------------------------------------------------------------------ */

static void test_init_basic(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);

    cbx_profile_diagram diag;
    int rc = cbx_profile_diagram_init(&diag, NULL, NULL, &theme);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_profile_diagram_get_highlight(&diag),
                       CBX_DIAG_BTN_NONE);
    assert_true(diag.base.visible);
    cbx_profile_diagram_shutdown(&diag);
}

static void test_init_null_args(void **state)
{
    (void)state;
    assert_int_equal(cbx_profile_diagram_init(NULL, NULL, NULL, NULL),
                       -EINVAL);
}

static void test_shutdown_null_safe(void **state)
{
    (void)state;
    cbx_profile_diagram diag;
    memset(&diag, 0, sizeof(diag));
    cbx_profile_diagram_shutdown(&diag);
    /* should not crash */
}

static void test_shutdown_cleans_up(void **state)
{
    pd_fixture *f = *state;

    cbx_profile_diagram_highlight(&f->diag, CBX_DIAG_BTN_X);
    cbx_profile_diagram_shutdown(&f->diag);
    /* Shutdown releases owned resources and zeroes the struct: no owned
     * texture remains, and the widget vtable is gone. */
    assert_false(f->diag.owns_base_texture);
    assert_null(f->diag.base_texture);
    assert_null(f->diag.base.vt);
    /* Shutdown is idempotent — calling it again on the released struct is
     * safe and leaves the struct in the same released state. */
    cbx_profile_diagram_shutdown(&f->diag);
    assert_false(f->diag.owns_base_texture);
    assert_null(f->diag.base_texture);
    assert_null(f->diag.base.vt);
}

/* ------------------------------------------------------------------ */
/*  Tests: rendering                                                   */
/* ------------------------------------------------------------------ */

static void test_render_no_crash(void **state)
{
    pd_fixture *f = *state;

    /* Render without highlight — the diagram must paint its panel_bg. */
    diag_clear(f->sdl.renderer, 255);
    cbx_widget_draw(&f->diag.base, f->sdl.renderer);
    int w, h;
    uint8_t *buf = diag_read_fb(f->sdl.renderer, &w, &h);
    SDL_Rect diag = f->diag.base.rect;
    SDL_Rect sample = { diag.x + 4, diag.y + 4, 60, 60 };
    uint8_t white[3] = {255, 255, 255};
    assert_true(fb_region_has_content(buf, w, h, &sample, white, 10));
    free(buf);

    /* Render with highlight — the highlighted button must differ from
     * the surrounding panel_bg (semantic highlight outcome). */
    cbx_profile_diagram_highlight(&f->diag, CBX_DIAG_BTN_A);
    cbx_widget_draw(&f->diag.base, f->sdl.renderer);
    diag_assert_highlight(f->sdl.renderer, &f->diag.base.rect,
                          CBX_DIAG_BTN_A, true);
}

static void test_render_all_buttons(void **state)
{
    pd_fixture *f = *state;

    /* Every button's highlight must actually change pixels in the
     * framebuffer (not merely not crash). */
    for (int i = 0; i < CBX_DIAG_BTN_COUNT; i++) {
        cbx_profile_diagram_highlight(&f->diag, (cbx_diag_button)i);
        cbx_widget_draw(&f->diag.base, f->sdl.renderer);
        diag_assert_highlight(f->sdl.renderer, &f->diag.base.rect,
                              (cbx_diag_button)i, true);
    }
}

static void test_render_with_rect(void **state)
{
    pd_fixture *f = *state;

    SDL_Rect r = { 0, 0, 400, 400 };
    cbx_widget_set_rect(&f->diag.base, &r);

    cbx_profile_diagram_highlight(&f->diag, CBX_DIAG_BTN_START);
    diag_clear(f->sdl.renderer, 0);
    cbx_widget_draw(&f->diag.base, f->sdl.renderer);

    SDL_Rect out;
    cbx_widget_get_rect(&f->diag.base, &out);
    assert_int_equal(out.w, 400);
    assert_int_equal(out.h, 400);

    /* The enlarged rect is still painted, and the START highlight is
     * visible in the framebuffer. */
    int w, h;
    uint8_t *buf = diag_read_fb(f->sdl.renderer, &w, &h);
    SDL_Rect sample = { out.x + 4, out.y + 4, 60, 60 };
    uint8_t black[3] = {0, 0, 0};
    assert_true(fb_region_has_content(buf, w, h, &sample, black, 10));
    free(buf);
    diag_assert_highlight(f->sdl.renderer, &out, CBX_DIAG_BTN_START, true);
}

static void test_render_null_safe(void **state)
{
    (void)state;
    /* Drawing a NULL widget should be a no-op */
    cbx_widget_draw(NULL, NULL);
}

/* ------------------------------------------------------------------ */
/*  Tests: SVG loading (optional, may fail gracefully)                 */
/* ------------------------------------------------------------------ */

static void test_init_with_svg_nonexistent(void **state)
{
    pd_fixture *f = *state;

    cbx_profile_diagram diag;
    cbx_theme theme;
    cbx_theme_default(&theme);

    /* Non-existent SVG path should not cause init failure */
    int rc = cbx_profile_diagram_init(&diag, f->sdl.renderer,
                                        "/nonexistent/file.svg", &theme);
    assert_int_equal(rc, 0);
    /* base_texture should be NULL since file doesn't exist */
    assert_null(diag.base_texture);
    cbx_profile_diagram_shutdown(&diag);
}

/* ------------------------------------------------------------------ */
/*  Geometry tests (BUG-0018)                                         */
/* ------------------------------------------------------------------ */
/*
 * The installed diagram was pixelated/stretched and its mapped-button
 * markers did not align with the rendered controls.  These tests load the
 * real production generic-gamepad.svg through the production rasteriser and
 * assert, from real framebuffer readback:
 *
 *   - pixelation guard: the rasterised base texture is not smaller than the
 *     on-screen content rect (i.e. never up-scaled);
 *   - stretch guard: the content rect preserves the texture's aspect ratio;
 *   - marker-to-control alignment: every highlighted button's marker rect
 *     (computed with the same content-rect transform the renderer uses)
 *     overlaps the light-grey control pixels on the rendered controller.
 *
 * The production SVG is drawn so each control sits at the button table's
 * normalised coordinates, so marker geometry == control geometry.  A marker
 * that landed in the letterbox void or on the black body would fail the
 * alignment assertion below.
 */

/* Light-grey control fill used by the production generic-gamepad.svg. */
static const uint8_t s_control_rgb[3] = {221, 221, 221};

/* On-screen marker rect for `btn` within the aspect-fitted content rect, */
/* replicating diag_draw() exactly (BUG-0018). */
static SDL_Rect
geo_marker_rect(const cbx_diag_button_pos *pos, const SDL_Rect *content)
{
    SDL_Rect r;
    r.x = content->x + (int)(pos->x * (float)content->w);
    r.y = content->y + (int)(pos->y * (float)content->h);
    r.w = (int)(pos->w * (float)content->w);
    r.h = (int)(pos->h * (float)content->h);
    if (r.w < 1) r.w = 1;
    if (r.h < 1) r.h = 1;
    return r;
}

static void test_geometry_pixelation_and_stretch(void **state)
{
    pd_fixture *f = *state;

    /* Load the production diagram SVG through the production rasteriser. */
    char svg_path[PATH_MAX];
    snprintf(svg_path, sizeof(svg_path), "%s/svg/generic-gamepad.svg",
             cbx_icon_dir());

    cbx_profile_diagram diag;
    cbx_theme theme;
    cbx_theme_default(&theme);
    assert_int_equal(cbx_profile_diagram_init(&diag, f->sdl.renderer,
                                              svg_path, &theme), 0);
    assert_non_null(diag.base_texture);

    /* Landscape content box that matches the SVG's 5:3 aspect ratio. */
    SDL_Rect rect = {0, 0, 300, 180};
    cbx_widget_set_rect(&diag.base, &rect);

    SDL_Rect content;
    assert_true(cbx_profile_diagram_content_rect(&diag, &rect, &content));
    assert_int_equal(content.x, 0);
    assert_int_equal(content.y, 0);
    assert_int_equal(content.w, 300);
    assert_int_equal(content.h, 180);

    int tw, th;
    assert_true(cbx_profile_diagram_base_texture_size(&diag, &tw, &th));
    fprintf(stderr, "DBG tw=%d th=%d content=%dx%d\n", tw, th, content.w, content.h);

    /* Pixelation guard: raster resolution >= displayed content size, so the
     * texture is never up-scaled (BUG-0018). */
    assert_true(tw >= content.w);
    assert_true(th >= content.h);

    /* Stretch guard: content rect preserves the texture's aspect ratio. */
    /* Compare cross-multiplied integers to avoid float rounding. */
    long long tw_th = (long long)tw * content.h;
    long long th_tw = (long long)th * content.w;
    assert_true(llabs(tw_th - th_tw) <= content.w); /* within 1 row of px */

    cbx_profile_diagram_shutdown(&diag);
}

static void test_geometry_marker_control_alignment(void **state)
{
    pd_fixture *f = *state;

    char icon_path[PATH_MAX];
    snprintf(icon_path, sizeof(icon_path), "%s/svg/generic-gamepad.svg",
             cbx_icon_dir());

    cbx_profile_diagram diag;
    cbx_theme theme;
    cbx_theme_default(&theme);
    assert_int_equal(cbx_profile_diagram_init(&diag, f->sdl.renderer,
                                              icon_path, &theme), 0);
    assert_non_null(diag.base_texture);

    SDL_Rect rect = {0, 0, 300, 180};
    cbx_widget_set_rect(&diag.base, &rect);

    SDL_Rect content;
    assert_true(cbx_profile_diagram_content_rect(&diag, &rect, &content));

    /* No highlight: the marker region must show the raw control (grey). */
    cbx_profile_diagram_clear_highlight(&diag);
    cbx_widget_draw(&diag.base, f->sdl.renderer);

    int w, h;
    uint8_t *buf = diag_read_fb(f->sdl.renderer, &w, &h);
    {
        int minx=999,miny=999,maxx=-1,maxy=-1, cnt=0;
        for(int y=0;y<h;y++)for(int x=0;x<w;x++){
            int i=(y*w+x)*4;
            if(buf[i+0]>180&&buf[i+1]>180&&buf[i+2]>180){cnt++; if(x<minx)minx=x; if(y<miny)miny=y; if(x>maxx)maxx=x; if(y>maxy)maxy=y;}
        }
        int bcnt=0,bminx=999,bminy=999,bmaxx=-1,bmaxy=-1;
        for(int y=0;y<h;y++)for(int x=0;x<w;x++){int i=(y*w+x)*4;if(buf[i+0]<20&&buf[i+1]<20&&buf[i+2]<20){bcnt++;if(x<bminx)bminx=x;if(y<bminy)bminy=y;if(x>bmaxx)bmaxx=x;if(y>bmaxy)bmaxy=y;}}
        fprintf(stderr,"DBG gray cnt=%d bbox=%d,%d - %d,%d | black cnt=%d bbox=%d,%d - %d,%d\n",cnt,minx,miny,maxx,maxy,bcnt,bminx,bminy,bmaxx,bmaxy);
    }

    for (int i = 0; i < CBX_DIAG_BTN_COUNT; i++) {
        const cbx_diag_button_pos *pos =
            cbx_profile_diagram_get_button_pos((cbx_diag_button)i);
        assert_non_null(pos);
        SDL_Rect mr = geo_marker_rect(pos, &content);
        /* The marker rect must contain the grey control pixel(s) — this is
         * what proves the marker is anchored on its control, not floating in
         * the letterbox/panel or on the black body. */
        if (!fb_region_has_color(buf, w, h, &mr, s_control_rgb, 40)) {
            fail_msg("button %s (%d): marker rect (%d,%d,%d,%d) does not "
                     "overlap its rendered control",
                     pos->name, i, mr.x, mr.y, mr.w, mr.h);
        }
    }

    /* With a highlight the marker region must change away from the plain
     * control (semantic highlight outcome), proving the overlay is anchored
     * to the same content box. */
    cbx_profile_diagram_highlight(&diag, CBX_DIAG_BTN_A);
    cbx_widget_draw(&diag.base, f->sdl.renderer);
    free(buf);
    buf = diag_read_fb(f->sdl.renderer, &w, &h);
    const cbx_diag_button_pos *apos =
        cbx_profile_diagram_get_button_pos(CBX_DIAG_BTN_A);
    SDL_Rect amr = geo_marker_rect(apos, &content);
    assert_false(fb_region_has_color(buf, w, h, &amr, s_control_rgb, 40));

    free(buf);
    cbx_profile_diagram_shutdown(&diag);
}

/* ------------------------------------------------------------------ */
/*  Tests: device-mapped base image & marker layout (BUG-0018)         */
/* ------------------------------------------------------------------ */
/*
 * The profile editor now resolves its diagram base image + marker layout
 * through the production icon mapping utilities (cbx_icon_map +
 * cbx_icon_cache + cbx_icon_lookup) keyed by device type, instead of a
 * hardcoded `.../svg/generic-gamepad.svg` path build.  These tests prove
 * that production-utility path preserves the BUG-0018 acceptance
 * invariants (adequate raster resolution, aspect preservation, marker-to-
 * control alignment) on the real installed asset.
 */

/* Build a diagram whose base image comes from the production icon cache,
 * mirroring the editor's cbx_profile_editor_set_device() path: resolve an
 * icon name, load it through cbx_icon_cache_load_one(), adopt the cache-
 * owned texture via cbx_profile_diagram_set_base_image(), and select the
 * marker layout via cbx_profile_diagram_set_device().  Returns 0 and fills
 * *cache (caller must cbx_icon_cache_cleanup + diagram_shutdown) on
 * success; nonzero on failure. */
static int
build_cache_resolved_diagram(pd_fixture *f, cbx_icon_cache *cache,
                             cbx_profile_diagram *diag, const char *icon)
{
    cbx_theme theme;
    cbx_theme_default(&theme);

    int rc = cbx_icon_cache_init(cache, f->sdl.renderer, cbx_icon_dir(), 512);
    if (rc != 0)
        return rc;
    rc = cbx_icon_cache_load_one(cache, icon);
    if (rc != 0) {
        cbx_icon_cache_cleanup(cache);
        return rc;
    }
    SDL_Texture *tex = cbx_icon_cache_get(cache, icon);
    if (!tex) {
        cbx_icon_cache_cleanup(cache);
        return -ENOENT;
    }
    rc = cbx_profile_diagram_init(diag, f->sdl.renderer, NULL, &theme);
    if (rc != 0) {
        cbx_icon_cache_cleanup(cache);
        return rc;
    }
    cbx_profile_diagram_set_base_image(diag, tex);
    cbx_profile_diagram_set_device(diag, icon);
    return 0;
}

static void test_device_geometry_known(void **state)
{
    (void)state;
    /* NULL / empty resolve to the default generic device, always verified. */
    assert_true(cbx_profile_diagram_device_geometry_known(NULL));
    assert_true(cbx_profile_diagram_device_geometry_known(""));
    assert_true(cbx_profile_diagram_device_geometry_known("generic-gamepad"));
    /* Unregistered device icons must NOT be shown with markers: their
     * control geometry is not verified, so markers could float off. */
    assert_false(cbx_profile_diagram_device_geometry_known("cc-xbox-360"));
    assert_false(cbx_profile_diagram_device_geometry_known("cc-ps5"));
}

static void test_set_device_default_layout(void **state)
{
    pd_fixture *f = *state;

    /* Unregistered device falls back to the generic layout table. */
    cbx_profile_diagram_set_device(&f->diag, "cc-xbox-360");
    assert_ptr_equal(
        cbx_profile_diagram_active_button_pos(&f->diag, CBX_DIAG_BTN_A),
        cbx_profile_diagram_get_button_pos(CBX_DIAG_BTN_A));
    /* Generic device uses the generic table. */
    cbx_profile_diagram_set_device(&f->diag, "generic-gamepad");
    assert_ptr_equal(
        cbx_profile_diagram_active_button_pos(&f->diag, CBX_DIAG_BTN_A),
        cbx_profile_diagram_get_button_pos(CBX_DIAG_BTN_A));
    /* NULL device -> generic table. */
    cbx_profile_diagram_set_device(&f->diag, NULL);
    assert_ptr_equal(
        cbx_profile_diagram_active_button_pos(&f->diag, CBX_DIAG_BTN_START),
        cbx_profile_diagram_get_button_pos(CBX_DIAG_BTN_START));
}

static void test_set_base_image_borrowed(void **state)
{
    pd_fixture *f = *state;

    /* Start from an owned-texture diagram (a path-based load) to prove the
     * borrowed adoption frees the previously owned texture and switches
     * ownership to the cache. */
    char svg_path[PATH_MAX];
    snprintf(svg_path, sizeof(svg_path), "%s/svg/generic-gamepad.svg",
             cbx_icon_dir());
    cbx_theme theme;
    cbx_theme_default(&theme);
    cbx_profile_diagram diag;
    assert_int_equal(cbx_profile_diagram_init(&diag, f->sdl.renderer, svg_path,
                                              &theme), 0);
    assert_true(diag.owns_base_texture);
    assert_non_null(diag.base_texture);

    /* Adopt a cache-owned texture: the diagram must NOT own it and must
     * not destroy it on shutdown (the icon cache does). */
    cbx_icon_cache cache;
    assert_int_equal(cbx_icon_cache_init(&cache, f->sdl.renderer, cbx_icon_dir(),
                                         64), 0);
    assert_int_equal(cbx_icon_cache_load_one(&cache, "generic-gamepad"), 0);
    SDL_Texture *cache_tex = cbx_icon_cache_get(&cache, "generic-gamepad");
    assert_non_null(cache_tex);

    cbx_profile_diagram_set_base_image(&diag, cache_tex);
    assert_ptr_equal(diag.base_texture, cache_tex);
    assert_false(diag.owns_base_texture);

    /* Shutdown must not destroy the cache-owned texture (it stays valid for
     * the cache to destroy later). */
    cbx_profile_diagram_shutdown(&diag);
    assert_null(diag.base_texture);
    /* The cache still owns its texture and can query it afterwards. */
    assert_non_null(cbx_icon_cache_get(&cache, "generic-gamepad"));
    cbx_icon_cache_cleanup(&cache);
}

/* Device-mapped base through the production icon cache preserves raster
 * resolution and aspect (pixelation / stretch guards). */
static void test_device_mapped_resolution_geometry(void **state)
{
    pd_fixture *f = *state;

    cbx_icon_cache cache;
    cbx_profile_diagram diag;
    assert_int_equal(build_cache_resolved_diagram(f, &cache, &diag, "generic-gamepad"),
                     0);
    assert_non_null(diag.base_texture);

    /* Landscape content box matching the 5:3 asset. */
    SDL_Rect rect = {0, 0, 300, 180};
    cbx_widget_set_rect(&diag.base, &rect);

    SDL_Rect content;
    assert_true(cbx_profile_diagram_content_rect(&diag, &rect, &content));
    int tw, th;
    assert_true(cbx_profile_diagram_base_texture_size(&diag, &tw, &th));

    /* Pixelation guard: raster >= displayed content. */
    assert_true(tw >= content.w);
    assert_true(th >= content.h);
    /* Stretch guard: aspect preserved. */
    long long tw_th = (long long)tw * content.h;
    long long th_tw = (long long)th * content.w;
    assert_true(llabs(tw_th - th_tw) <= content.w);

    cbx_profile_diagram_shutdown(&diag);
    cbx_icon_cache_cleanup(&cache);
}

/* Device-mapped base through the production icon library still anchors every
 * marker to its control (marker-to-control alignment), the BUG-0018 core. */
static void test_device_mapped_marker_alignment(void **state)
{
    pd_fixture *f = *state;

    cbx_icon_cache cache;
    cbx_profile_diagram diag;
    assert_int_equal(build_cache_resolved_diagram(f, &cache, &diag, "generic-gamepad"),
                     0);

    SDL_Rect rect = {0, 0, 300, 180};
    cbx_widget_set_rect(&diag.base, &rect);
    SDL_Rect content;
    assert_true(cbx_profile_diagram_content_rect(&diag, &rect, &content));

    cbx_profile_diagram_clear_highlight(&diag);
    cbx_widget_draw(&diag.base, f->sdl.renderer);
    int w, h;
    uint8_t *buf = diag_read_fb(f->sdl.renderer, &w, &h);

    /* Every button marker (active layout table, same content-rect transform
     * as the renderer) must overlap the light-grey control pixels. */
    for (int i = 0; i < CBX_DIAG_BTN_COUNT; i++) {
        const cbx_diag_button_pos *pos =
            cbx_profile_diagram_active_button_pos(&diag, (cbx_diag_button)i);
        assert_non_null(pos);
        SDL_Rect mr = geo_marker_rect(pos, &content);
        if (!fb_region_has_color(buf, w, h, &mr, s_control_rgb, 40)) {
            fail_msg("device-mapped button %s (%d): marker rect (%d,%d,%d,%d) "
                     "does not overlap its rendered control",
                     pos->name, i, mr.x, mr.y, mr.w, mr.h);
        }
    }

    free(buf);
    cbx_profile_diagram_shutdown(&diag);
    cbx_icon_cache_cleanup(&cache);
}

/* The editor-level path (cbx_icon_lookup with a device type) resolves a
 * non-NULL base image for the unknown device through the production icon
 * cache, so the editor never shows a blank diagram. */
static void test_lookup_device_diagram_resolves(void **state)
{
    pd_fixture *f = *state;

    cbx_icon_map map;
    cbx_icon_map_init(&map);
    cbx_icon_cache cache;
    assert_int_equal(cbx_icon_cache_init(&cache, f->sdl.renderer, cbx_icon_dir(),
                                         512), 0);

    /* Unknown device -> generic-gamepad (device-mapped via icon map). */
    cbx_icon_result res;
    assert_int_equal(cbx_icon_lookup(&cache, &map, NULL, NULL, &res), 0);
    assert_non_null(res.texture);
    assert_true(res.width > 0 && res.height > 0);

    cbx_icon_cache_cleanup(&cache);
}

/* ------------------------------------------------------------------ */
/*  Test runner                                                        */
/* ------------------------------------------------------------------ */

int main(void)
{
    const struct CMUnitTest tests[] = {
        /* Button position lookup */
        cmocka_unit_test(test_button_count),
        cmocka_unit_test(test_button_pos_valid),
        cmocka_unit_test(test_button_pos_all_valid),
        cmocka_unit_test(test_button_pos_invalid),

        /* Name mapping */
        cmocka_unit_test(test_button_from_name_known),
        cmocka_unit_test(test_button_from_name_unknown),
        cmocka_unit_test(test_button_name_roundtrip),
        cmocka_unit_test(test_button_name_invalid),

        /* Highlight state */
        cmocka_unit_test_setup_teardown(test_highlight_set_get,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_highlight_clear,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_highlight_none,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_highlight_all_buttons,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_highlight_invalid,
                                          setup, teardown),
        cmocka_unit_test(test_highlight_null_safe),

        /* Lifecycle */
        cmocka_unit_test(test_init_basic),
        cmocka_unit_test(test_init_null_args),
        cmocka_unit_test(test_shutdown_null_safe),
        cmocka_unit_test_setup_teardown(test_shutdown_cleans_up,
                                          setup, teardown),

        /* Rendering */
        cmocka_unit_test_setup_teardown(test_render_no_crash,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_render_all_buttons,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_render_with_rect,
                                          setup, teardown),
        cmocka_unit_test(test_render_null_safe),

        /* SVG loading */
        cmocka_unit_test_setup_teardown(test_init_with_svg_nonexistent,
                                          setup, teardown),

        /* Geometry (BUG-0018) */
        cmocka_unit_test_setup_teardown(test_geometry_pixelation_and_stretch,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_geometry_marker_control_alignment,
                                          setup, teardown),

        /* Device-mapped base & marker layout (BUG-0018) */
        cmocka_unit_test(test_device_geometry_known),
        cmocka_unit_test_setup_teardown(test_set_device_default_layout,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_set_base_image_borrowed,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_device_mapped_resolution_geometry,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_device_mapped_marker_alignment,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_lookup_device_diagram_resolves,
                                          setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}