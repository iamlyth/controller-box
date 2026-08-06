/*
 * surface_build.c — Pre-built overlay surface infrastructure.
 *
 * Task 24 — Pre-built overlay surface infrastructure (render-to-texture).
 *
 * Implements the render-to-texture overlay surface described in SPEC
 * §4.9 and §11.  The overlay texture is created once at init time and
 * reused for the lifetime of the daemon.  Show/hide is a single
 * SDL_RenderCopy + SDL_RenderPresent — no texture allocation.
 *
 * Dirty-rect incremental re-render uses the cbx_dirty_rect system from
 * Task 23.  cbx_overlay_surface_render() switches the SDL render target
 * to the overlay texture, clips per dirty rect, invokes the caller's
 * render callback, then restores the default target.
 */
#include "overlay/surface_build.h"

#include <errno.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static uint8_t
opacity_to_u8(double opacity)
{
    if (opacity < 0.0) opacity = 0.0;
    if (opacity > 1.0) opacity = 1.0;
    return (uint8_t)(opacity * 255.0 + 0.5);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

int
cbx_overlay_surface_init(cbx_overlay_surface *s,
                         SDL_Renderer *renderer,
                         int width, int height,
                         double opacity)
{
    if (!s || !renderer || width <= 0 || height <= 0)
        return -EINVAL;

    memset(s, 0, sizeof(*s));

    s->texture = SDL_CreateTexture(renderer,
                                   SDL_PIXELFORMAT_RGBA8888,
                                   SDL_TEXTUREACCESS_TARGET,
                                   width, height);
    if (!s->texture)
        return -ENOMEM;

    s->width   = width;
    s->height  = height;
    s->visible = false;
    s->opacity = opacity_to_u8(opacity);
    s->built   = true;

    cbx_dirty_rect_init(&s->dirty, width, height);

    /* Apply initial opacity */
    SDL_SetTextureAlphaMod(s->texture, s->opacity);

    /* Enable blending so alpha modulation composites correctly */
    SDL_SetTextureBlendMode(s->texture, SDL_BLENDMODE_BLEND);

    return 0;
}

void
cbx_overlay_surface_destroy(cbx_overlay_surface *s)
{
    if (!s)
        return;
    if (s->texture)
        SDL_DestroyTexture(s->texture);
    memset(s, 0, sizeof(*s));
}

/* ------------------------------------------------------------------ */
/* Opacity                                                             */
/* ------------------------------------------------------------------ */

int
cbx_overlay_surface_set_opacity(cbx_overlay_surface *s, double opacity)
{
    if (!s || !s->built)
        return -EINVAL;
    s->opacity = opacity_to_u8(opacity);
    SDL_SetTextureAlphaMod(s->texture, s->opacity);
    return 0;
}

uint8_t
cbx_overlay_surface_get_opacity(const cbx_overlay_surface *s)
{
    if (!s)
        return 0;
    return s->opacity;
}

/* ------------------------------------------------------------------ */
/* Visibility (no texture creation in show/hide)                      */
/* ------------------------------------------------------------------ */

int
cbx_overlay_surface_show(cbx_overlay_surface *s, SDL_Renderer *r)
{
    if (!s || !r || !s->built)
        return -EINVAL;

    /* Ensure we're rendering to the screen, not a target texture */
    SDL_SetRenderTarget(r, NULL);

    /* Single render copy of the pre-built texture — no allocation */
    if (SDL_RenderCopy(r, s->texture, NULL, NULL) != 0)
        return -1;

    SDL_RenderPresent(r);
    s->visible = true;
    return 0;
}

void
cbx_overlay_surface_hide(cbx_overlay_surface *s)
{
    if (!s)
        return;
    s->visible = false;
}

bool
cbx_overlay_surface_is_visible(const cbx_overlay_surface *s)
{
    return s ? s->visible : false;
}

/* ------------------------------------------------------------------ */
/* Dirty-rect management                                             */
/* ------------------------------------------------------------------ */

void
cbx_overlay_surface_mark_dirty(cbx_overlay_surface *s, const SDL_Rect *rect)
{
    if (!s || !s->built)
        return;
    cbx_dirty_rect_add(&s->dirty, rect);
}

void
cbx_overlay_surface_mark_dirty_all(cbx_overlay_surface *s)
{
    if (!s || !s->built)
        return;
    cbx_dirty_rect_add_all(&s->dirty);
}

void
cbx_overlay_surface_clear_dirty(cbx_overlay_surface *s)
{
    if (!s)
        return;
    cbx_dirty_rect_clear(&s->dirty);
}

bool
cbx_overlay_surface_is_dirty(const cbx_overlay_surface *s)
{
    if (!s)
        return false;
    return cbx_dirty_rect_is_dirty(&s->dirty);
}

int
cbx_overlay_surface_dirty_count(const cbx_overlay_surface *s)
{
    if (!s)
        return 0;
    return cbx_dirty_rect_count(&s->dirty);
}

/* ------------------------------------------------------------------ */
/* Render content into target texture                                  */
/* ------------------------------------------------------------------ */

int
cbx_overlay_surface_render(cbx_overlay_surface *s,
                           SDL_Renderer *r,
                           cbx_overlay_render_fn fn,
                           void *userdata)
{
    if (!s || !r || !s->built || !fn)
        return -EINVAL;

    /* Switch render target to the overlay texture */
    if (SDL_SetRenderTarget(r, s->texture) != 0)
        return -1;

    /* Merge dirty rects before rendering to reduce overdraw */
    cbx_dirty_rect_merge(&s->dirty);

    /* Delegate to dirty_rect_render which handles clip rects and
     * calls fn per dirty region (or full-screen if none dirty). */
    int rc = cbx_dirty_rect_render(&s->dirty, r,
                                    (cbx_dirty_render_fn)fn, userdata);

    /* Restore default render target (screen) */
    SDL_SetRenderTarget(r, NULL);

    /* Clear dirty rects after a successful render */
    if (rc == 0)
        cbx_dirty_rect_clear(&s->dirty);

    return rc;
}

/* ------------------------------------------------------------------ */
/* Accessors                                                           */
/* ------------------------------------------------------------------ */

SDL_Texture *
cbx_overlay_surface_get_texture(const cbx_overlay_surface *s)
{
    return s ? s->texture : NULL;
}

void
cbx_overlay_surface_get_size(const cbx_overlay_surface *s,
                              int *w, int *h)
{
    if (w) *w = s ? s->width  : 0;
    if (h) *h = s ? s->height : 0;
}

bool
cbx_overlay_surface_is_built(const cbx_overlay_surface *s)
{
    return s ? s->built : false;
}