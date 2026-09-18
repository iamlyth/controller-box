/*
 * dirty_rect.c — Dirty-rect tracking for incremental re-rendering.
 *
 * Task 23 — Animation primitives and dirty rect optimization.
 */
#include "ui/dirty_rect.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static bool
rects_overlap_or_adjacent(const SDL_Rect *a, const SDL_Rect *b)
{
    /* Adjacent = share an edge (touch but don't overlap interior).
     * Overlap or adjacent means merging them won't create a rect with
     * significantly more area than the union of both. */
    int a_right  = a->x + a->w;
    int a_bottom = a->y + a->h;
    int b_right  = b->x + b->w;
    int b_bottom = b->y + b->h;

    /* They overlap or touch if they share at least one point in both
     * axes. */
    return (a->x <= b_right  && b->x <= a_right) &&
           (a->y <= b_bottom && b->y <= a_bottom);
}

static SDL_Rect
rect_union(const SDL_Rect *a, const SDL_Rect *b)
{
    SDL_Rect u;
    int a_right  = a->x + a->w;
    int a_bottom = a->y + a->h;
    int b_right  = b->x + b->w;
    int b_bottom = b->y + b->h;

    u.x = (a->x < b->x) ? a->x : b->x;
    u.y = (a->y < b->y) ? a->y : b->y;
    int ur  = (a_right  > b_right)  ? a_right  : b_right;
    int ub  = (a_bottom > b_bottom) ? a_bottom : b_bottom;
    u.w = ur - u.x;
    u.h = ub - u.y;
    return u;
}

static bool
rect_intersects(const SDL_Rect *a, const SDL_Rect *b)
{
    return (a->x < b->x + b->w) && (a->x + a->w > b->x) &&
           (a->y < b->y + b->h) && (a->y + a->h > b->y);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void
cbx_dirty_rect_init(cbx_dirty_rect *dr, int screen_w, int screen_h)
{
    memset(dr, 0, sizeof(*dr));
    dr->screen_w = screen_w;
    dr->screen_h = screen_h;
}

void
cbx_dirty_rect_add(cbx_dirty_rect *dr, const SDL_Rect *rect)
{
    if (!rect || rect->w <= 0 || rect->h <= 0)
        return;

    /* Clamp to screen bounds */
    SDL_Rect r = *rect;
    if (dr->screen_w > 0 && dr->screen_h > 0) {
        if (r.x < 0) { r.w += r.x; r.x = 0; }
        if (r.y < 0) { r.h += r.y; r.y = 0; }
        if (r.x + r.w > dr->screen_w) r.w = dr->screen_w - r.x;
        if (r.y + r.h > dr->screen_h) r.h = dr->screen_h - r.y;
    }
    if (r.w <= 0 || r.h <= 0)
        return;

    if (dr->count < CBX_DIRTY_RECT_MAX) {
        dr->rects[dr->count++] = r;
    } else {
        /* Full: merge into entry 0 as fallback */
        dr->rects[0] = rect_union(&dr->rects[0], &r);
    }
}

void
cbx_dirty_rect_add_all(cbx_dirty_rect *dr)
{
    dr->count = 0;
    SDL_Rect full = { 0, 0, dr->screen_w, dr->screen_h };
    dr->rects[dr->count++] = full;
}

void
cbx_dirty_rect_clear(cbx_dirty_rect *dr)
{
    dr->count = 0;
}

int
cbx_dirty_rect_count(const cbx_dirty_rect *dr)
{
    return dr->count;
}

bool
cbx_dirty_rect_is_dirty(const cbx_dirty_rect *dr)
{
    return dr->count > 0;
}

const SDL_Rect *
cbx_dirty_rect_get(const cbx_dirty_rect *dr, int index)
{
    if (index < 0 || index >= dr->count)
        return NULL;
    return &dr->rects[index];
}

void
cbx_dirty_rect_merge(cbx_dirty_rect *dr)
{
    if (dr->count <= 1)
        return;

    bool changed;
    do {
        changed = false;
        for (int i = 0; i < dr->count && !changed; i++) {
            for (int j = i + 1; j < dr->count; j++) {
                if (rects_overlap_or_adjacent(&dr->rects[i],
                                              &dr->rects[j])) {
                    dr->rects[i] = rect_union(&dr->rects[i],
                                              &dr->rects[j]);
                    /* Shift last element into the gap at j */
                    dr->rects[j] = dr->rects[dr->count - 1];
                    dr->count--;
                    changed = true;
                    break;
                }
            }
        }
    } while (changed);
}

int
cbx_dirty_rect_render(const cbx_dirty_rect *dr, SDL_Renderer *r,
                       cbx_dirty_render_fn fn, void *userdata)
{
    if (!fn || !r)
        return -1;

    if (dr->count == 0) {
        /* First render: paint everything */
        SDL_Rect full = { 0, 0, dr->screen_w, dr->screen_h };
        SDL_RenderSetClipRect(r, &full);
        int rc = fn(r, &full, userdata);
        SDL_RenderSetClipRect(r, NULL);
        return rc;
    }

    int rc = 0;
    for (int i = 0; i < dr->count; i++) {
        SDL_RenderSetClipRect(r, &dr->rects[i]);
        rc = fn(r, &dr->rects[i], userdata);
        if (rc != 0)
            break;
    }
    SDL_RenderSetClipRect(r, NULL);
    return rc;
}

bool
cbx_dirty_rect_intersects(const cbx_dirty_rect *dr, const SDL_Rect *rect)
{
    if (!rect)
        return false;
    for (int i = 0; i < dr->count; i++) {
        if (rect_intersects(&dr->rects[i], rect))
            return true;
    }
    return false;
}