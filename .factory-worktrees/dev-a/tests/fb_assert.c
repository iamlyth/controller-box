/*
 * fb_assert.c — Framebuffer pixel assertion utilities (Task 1).
 *
 * All pixel comparisons use integer-only arithmetic — no floating-point.
 */
#include "fb_assert.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Pixel readback ────────────────────────────────────────────────── */

int
fb_read_pixels(SDL_Renderer *renderer, const SDL_Rect *rect,
               uint8_t *buf, size_t buf_len)
{
    if (!renderer || !buf)
        return -1;

    int w, h;
    if (SDL_GetRendererOutputSize(renderer, &w, &h) != 0)
        return -1;

    SDL_Rect full;
    if (!rect) {
        full.x = 0;
        full.y = 0;
        full.w = w;
        full.h = h;
        rect = &full;
    }

    /* Validate rect is within bounds. */
    if (rect->w <= 0 || rect->h <= 0)
        return -1;
    if (rect->x < 0 || rect->y < 0)
        return -1;
    if (rect->x + rect->w > w || rect->y + rect->h > h)
        return -1;

    size_t needed = (size_t)rect->w * (size_t)rect->h * 4;
    if (buf_len < needed)
        return -1;

    int pitch = rect->w * 4;
    if (SDL_RenderReadPixels(renderer, rect,
                             SDL_PIXELFORMAT_ABGR8888,
                             buf, pitch) != 0)
        return -1;

    return 0;
}

/* ── Region assertions ─────────────────────────────────────────────── */

/*
 * Clamp rect to buffer bounds.  Returns false if the rect is entirely
 * outside the buffer (nothing to check).
 */
static bool
clamp_rect(const SDL_Rect *rect, int w, int h, SDL_Rect *out)
{
    if (!rect) {
        out->x = 0;
        out->y = 0;
        out->w = w;
        out->h = h;
        return true;
    }
    out->x = rect->x;
    out->y = rect->y;
    out->w = rect->w;
    out->h = rect->h;
    if (out->x < 0) { out->w += out->x; out->x = 0; }
    if (out->y < 0) { out->h += out->y; out->y = 0; }
    if (out->x + out->w > w) out->w = w - out->x;
    if (out->y + out->h > h) out->h = h - out->y;
    return out->w > 0 && out->h > 0;
}

static inline int
iabs(int v)
{
    return v < 0 ? -v : v;
}

bool
fb_region_has_content(const uint8_t *buf, int w, int h,
                      const SDL_Rect *rect,
                      const uint8_t bg_color[3], int tolerance)
{
    if (!buf || !bg_color || w <= 0 || h <= 0)
        return false;

    SDL_Rect r;
    if (!clamp_rect(rect, w, h, &r))
        return false;

    for (int y = r.y; y < r.y + r.h; y++) {
        for (int x = r.x; x < r.x + r.w; x++) {
            const uint8_t *px = buf + (y * w + x) * 4;
            if (iabs((int)px[0] - (int)bg_color[0]) > tolerance ||
                iabs((int)px[1] - (int)bg_color[1]) > tolerance ||
                iabs((int)px[2] - (int)bg_color[2]) > tolerance)
                return true;
    }
    }
    return false;
}

bool
fb_region_has_color(const uint8_t *buf, int w, int h,
                    const SDL_Rect *rect,
                    const uint8_t target_rgb[3], int tolerance)
{
    if (!buf || !target_rgb || w <= 0 || h <= 0)
        return false;

    SDL_Rect r;
    if (!clamp_rect(rect, w, h, &r))
        return false;

    for (int y = r.y; y < r.y + r.h; y++) {
        for (int x = r.x; x < r.x + r.w; x++) {
            const uint8_t *px = buf + (y * w + x) * 4;
            if (iabs((int)px[0] - (int)target_rgb[0]) <= tolerance &&
                iabs((int)px[1] - (int)target_rgb[1]) <= tolerance &&
                iabs((int)px[2] - (int)target_rgb[2]) <= tolerance)
                return true;
        }
    }
    return false;
}

bool
fb_frames_differ(const uint8_t *buf_a, const uint8_t *buf_b,
                 int w, int h, int threshold_pct)
{
    if (!buf_a || !buf_b || w <= 0 || h <= 0)
        return false;

    long total = (long)w * (long)h;
    long differ = 0;

    for (int i = 0; i < w * h; i++) {
        const uint8_t *a = buf_a + i * 4;
        const uint8_t *b = buf_b + i * 4;
        if (a[0] != b[0] || a[1] != b[1] ||
            a[2] != b[2] || a[3] != b[3])
            differ++;
    }

    /* Integer percentage: differ * 100 > total * threshold_pct */
    return differ * 100 > total * (long)threshold_pct;
}

/* ── Golden image comparison ───────────────────────────────────────── */

bool
fb_golden_compare(const uint8_t *capture, int w, int h,
                  const char *golden_path,
                  int per_pixel_tol, int image_tol_pct)
{
    if (!capture || !golden_path || w <= 0 || h <= 0)
        return false;

    SDL_Surface *golden = IMG_Load(golden_path);
    if (!golden) {
        fprintf(stderr, "fb_golden_compare: cannot load %s: %s\n",
                golden_path, IMG_GetError());
        return false;
    }

    /* Convert golden to ABGR8888 for direct comparison. */
    SDL_Surface *conv = SDL_ConvertSurfaceFormat(golden,
                            SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(golden);
    if (!conv) {
        fprintf(stderr, "fb_golden_compare: format conversion failed: %s\n",
                SDL_GetError());
        return false;
    }

    /* Dimensions must match. */
    if (conv->w != w || conv->h != h) {
        fprintf(stderr, "fb_golden_compare: dimension mismatch "
                "(golden %dx%d vs capture %dx%d)\n",
                conv->w, conv->h, w, h);
        SDL_FreeSurface(conv);
        return false;
    }

    const uint8_t *gbuf = (const uint8_t *)conv->pixels;
    long total = (long)w * (long)h;
    long differ = 0;

    for (int i = 0; i < w * h; i++) {
        const uint8_t *cap = capture + i * 4;
        const uint8_t *gold = gbuf + i * 4;
        if (iabs((int)cap[0] - (int)gold[0]) > per_pixel_tol ||
            iabs((int)cap[1] - (int)gold[1]) > per_pixel_tol ||
            iabs((int)cap[2] - (int)gold[2]) > per_pixel_tol)
            differ++;
    }

    SDL_FreeSurface(conv);

    /* Pass if differing pixels are below image_tol_pct of total. */
    return differ * 100 < total * (long)image_tol_pct;
}

/* ── PNG output ────────────────────────────────────────────────────── */

/*
 * Create an SDL_Surface from an RGBA buffer.  The surface does NOT own
 * the buffer (no SDL_FREEBUF); caller must keep buf alive until the
 * surface is freed.
 */
static SDL_Surface *
rgba_to_surface(const uint8_t *buf, int w, int h)
{
    int depth = 32;
    int pitch = w * 4;
    Uint32 rmask = 0x000000FF;
    Uint32 gmask = 0x0000FF00;
    Uint32 bmask = 0x00FF0000;
    Uint32 amask = 0xFF000000;

    return SDL_CreateRGBSurfaceFrom((void *)buf, w, h, depth, pitch,
                                    rmask, gmask, bmask, amask);
}

int
fb_save_png(const uint8_t *buf, int w, int h, const char *path)
{
    if (!buf || !path || w <= 0 || h <= 0)
        return -1;

    SDL_Surface *surf = rgba_to_surface(buf, w, h);
    if (!surf)
        return -1;

    int ret = IMG_SavePNG(surf, path);
    SDL_FreeSurface(surf);
    return ret == 0 ? 0 : -1;
}

int
fb_save_diff(const uint8_t *actual, const uint8_t *expected,
             int w, int h, const char *path)
{
    if (!actual || !expected || !path || w <= 0 || h <= 0)
        return -1;

    /* Build a diff buffer: red for differing pixels, dimmed for matches. */
    size_t sz = (size_t)w * (size_t)h * 4;
    uint8_t *diff = malloc(sz);
    if (!diff)
        return -1;

    for (int i = 0; i < w * h; i++) {
        const uint8_t *a = actual + i * 4;
        const uint8_t *e = expected + i * 4;
        if (a[0] != e[0] || a[1] != e[1] || a[2] != e[2] || a[3] != e[3]) {
            /* Highlight differing pixel in bright red. */
            diff[i * 4 + 0] = 255;  /* R */
            diff[i * 4 + 1] = 0;    /* G */
            diff[i * 4 + 2] = 0;    /* B */
            diff[i * 4 + 3] = 255;  /* A */
        } else {
            /* Dim the matching pixel to 50%. */
            diff[i * 4 + 0] = a[0] / 2;
            diff[i * 4 + 1] = a[1] / 2;
            diff[i * 4 + 2] = a[2] / 2;
            diff[i * 4 + 3] = a[3];
        }
    }

    int ret = fb_save_png(diff, w, h, path);
    free(diff);
    return ret;
}