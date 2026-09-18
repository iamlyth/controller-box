/*
 * fb_assert.h — Framebuffer pixel assertion utilities (Task 1).
 *
 * Provides deterministic, integer-only helpers for reading back SDL2
 * renderer pixels and asserting on region-level visual content.  Used
 * by visual acceptance tests (§4.10, §5.6, §11.1) to verify that
 * rendering produces actual pixel output — not just valid in-memory
 * state.
 *
 * All comparisons use integer tolerance bands (no floating-point).
 *
 * Pixel format: SDL_PIXELFORMAT_ABGR8888 — on little-endian systems
 * the bytes in memory are R, G, B, A, so buf[i*4+0]=R, [1]=G, [2]=B,
 * [3]=A.  All RGB accessors index into this layout.
 */
#ifndef CBX_FB_ASSERT_H
#define CBX_FB_ASSERT_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <stdbool.h>

/* ── Pixel readback ────────────────────────────────────────────────── */

/*
 * Read pixels from the renderer's current target into buf.
 *
 *   renderer  — SDL renderer to read from.
 *   rect      — region to read (NULL = entire output).
 *   buf       — caller-allocated RGBA buffer (w*h*4 bytes minimum).
 *   buf_len   — size of buf in bytes (safety check).
 *
 * Returns 0 on success, -1 on error (bad args, readback failure, or
 * buf too small).
 */
int fb_read_pixels(SDL_Renderer *renderer, const SDL_Rect *rect,
                   uint8_t *buf, size_t buf_len);

/* ── Region assertions ─────────────────────────────────────────────── */

/*
 * Returns true if the region [rect] within the buffer contains at
 * least one pixel whose RGB channels differ from bg_color by more
 * than tolerance on any channel.  This detects "something was drawn"
 * against a known background.
 *
 *   buf        — RGBA pixel buffer.
 *   w, h       — dimensions of the full buffer.
 *   rect       — sub-region to check (clamped to buffer bounds).
 *   bg_color   — 3-element array {R, G, B} of the background color.
 *   tolerance  — max per-channel difference that still counts as
 *                "background" (0 = exact match).
 */
bool fb_region_has_content(const uint8_t *buf, int w, int h,
                           const SDL_Rect *rect,
                           const uint8_t bg_color[3], int tolerance);

/*
 * Returns true if the region [rect] contains at least one pixel
 * matching target_rgb within tolerance on every channel.
 */
bool fb_region_has_color(const uint8_t *buf, int w, int h,
                         const SDL_Rect *rect,
                         const uint8_t target_rgb[3], int tolerance);

/*
 * Returns true if more than threshold_pct percent of pixels differ
 * between buf_a and buf_b.  A pixel "differs" if any RGBA channel
 * differs by any amount.  threshold_pct is 0–100 (integer).
 */
bool fb_frames_differ(const uint8_t *buf_a, const uint8_t *buf_b,
                      int w, int h, int threshold_pct);

/* ── Golden image comparison ───────────────────────────────────────── */

/*
 * Compare a captured RGBA buffer against a golden PNG file.
 *
 *   capture       — live RGBA pixel buffer.
 *   w, h          — dimensions of capture.
 *   golden_path   — filesystem path to the baseline PNG.
 *   per_pixel_tol — max per-channel difference for a pixel to count
 *                   as "matching" (0 = exact).
 *   image_tol_pct — max percentage of pixels that may differ for
 *                   the image to pass (0–100, integer).
 *
 * Returns true if the image passes (differing pixels < image_tol_pct).
 * Returns false on mismatch or if the golden file cannot be loaded.
 */
bool fb_golden_compare(const uint8_t *capture, int w, int h,
                       const char *golden_path,
                       int per_pixel_tol, int image_tol_pct);

/* ── PNG output (failure artifacts) ────────────────────────────────── */

/*
 * Write an RGBA buffer to a PNG file.  Returns 0 on success, -1 on
 * error.
 */
int fb_save_png(const uint8_t *buf, int w, int h, const char *path);

/*
 * Write a visual diff PNG highlighting pixels that differ between
 * actual and expected.  Differing pixels are bright red; matching
 * pixels are dimmed (50% of original).  Returns 0 on success, -1 on
 * error.
 */
int fb_save_diff(const uint8_t *actual, const uint8_t *expected,
                 int w, int h, const char *path);

#endif /* CBX_FB_ASSERT_H */