/*
 * dirty_rect.h — Dirty-rect tracking for incremental overlay re-rendering.
 *
 * Task 23 — Animation primitives and dirty rect optimization.
 *
 * The overlay surface is pre-built (SPEC §4.9) and only re-rendered when
 * device/slot/profile state changes.  This module tracks which screen
 * regions need redrawing so that the renderer can use
 * SDL_RenderSetClipRect to limit work to those areas.
 *
 * Usage:
 *   1. cbx_dirty_rect_init(&dr, screen_w, screen_h);
 *   2. On state change: cbx_dirty_rect_add(&dr, &changed_rect);
 *   3. Before rendering: cbx_dirty_rect_merge(&dr);  // optional
 *   4. For each dirty rect: set clip rect, render, clear clip rect.
 *   5. cbx_dirty_rect_clear(&dr);
 *
 * The merge step combines overlapping or adjacent rects to reduce the
 * number of render passes.  Without merge, each added rect is rendered
 * independently.
 */
#ifndef CBX_DIRTY_RECT_H
#define CBX_DIRTY_RECT_H

#include <SDL2/SDL.h>
#include <stdbool.h>

#define CBX_DIRTY_RECT_MAX 64   /* max tracked dirty regions */

typedef struct {
    SDL_Rect rects[CBX_DIRTY_RECT_MAX];
    int      count;
    int      screen_w;
    int      screen_h;
} cbx_dirty_rect;

/* Initialise with screen dimensions (for clamping).  Count = 0. */
void cbx_dirty_rect_init(cbx_dirty_rect *dr, int screen_w, int screen_h);

/*
 * Add a dirty rect.  The rect is clamped to screen bounds.  NULL or
 * zero-area rects are ignored.  If the array is full, the rect is
 * merged into the first existing one (fallback: union with entry 0).
 * This ensures no dirty region is silently dropped.
 */
void cbx_dirty_rect_add(cbx_dirty_rect *dr, const SDL_Rect *rect);

/* Mark the entire screen dirty (single full-screen rect). */
void cbx_dirty_rect_add_all(cbx_dirty_rect *dr);

/* Clear all dirty rects (count = 0). */
void cbx_dirty_rect_clear(cbx_dirty_rect *dr);

/* Number of dirty rects currently tracked. */
int cbx_dirty_rect_count(const cbx_dirty_rect *dr);

/* True if count > 0. */
bool cbx_dirty_rect_is_dirty(const cbx_dirty_rect *dr);

/*
 * Get a pointer to dirty rect at index.  Returns NULL if index is out
 * of range.  The pointer is valid until the next mutating call.
 */
const SDL_Rect *cbx_dirty_rect_get(const cbx_dirty_rect *dr, int index);

/*
 * Merge overlapping or adjacent dirty rects in-place.  Two rects are
 * merged if they overlap or are directly adjacent (share an edge).
 * The result is a minimal set of non-overlapping rects covering the
 * same total area.  After merge, count <= original count.
 */
void cbx_dirty_rect_merge(cbx_dirty_rect *dr);

/*
 * Render callback type: called once per dirty rect with the clip rect
 * already set on the renderer.  The userdata is passed through.
 * Return 0 on success, non-zero to abort iteration.
 */
typedef int (*cbx_dirty_render_fn)(SDL_Renderer *r, const SDL_Rect *clip,
                                   void *userdata);

/*
 * Iterate dirty rects, set SDL_RenderSetClipRect for each, call fn,
 * then reset clip rect (NULL = no clipping).  Returns 0 if all callbacks
 * succeeded, or the first non-zero return value from fn.
 *
 * If the dirty-rect list is empty, fn is called once with a full-screen
 * clip rect (this ensures a first render paints everything).
 */
int cbx_dirty_rect_render(const cbx_dirty_rect *dr, SDL_Renderer *r,
                           cbx_dirty_render_fn fn, void *userdata);

/*
 * Check whether a given rect intersects any dirty region.  Useful for
 * testing whether a widget needs to be redrawn.
 */
bool cbx_dirty_rect_intersects(const cbx_dirty_rect *dr,
                               const SDL_Rect *rect);

#endif /* CBX_DIRTY_RECT_H */