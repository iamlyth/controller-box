/*
 * surface_build.h — Pre-built overlay surface infrastructure.
 *
 * Task 24 — Pre-built overlay surface infrastructure (render-to-texture).
 *
 * The overlay surface is an SDL_Texture created with
 * SDL_TEXTUREACCESS_TARGET at screen resolution.  It is pre-built at
 * daemon startup and held in memory for the lifetime of the daemon
 * (SPEC §4.9, §11).  Show/hide is a single SDL_RenderCopy +
 * SDL_RenderPresent — no texture allocation occurs in the show/hide
 * path (SPEC §11: <10 ms from button press to visible).
 *
 * Dirty-rect tracking (built on cbx_dirty_rect from Task 23) enables
 * incremental re-rendering of only changed cells when device/slot/profile
 * state changes (SPEC §4.9).  Task 29 provides the render callback that
 * draws grid content into the target texture; this module manages render
 * target switching and clip-rect scissoring.
 *
 * Opacity from settings.overlay_opacity (0.0–1.0) is applied via
 * SDL_SetTextureAlphaMod so the composited overlay is semi-transparent.
 */
#ifndef CBX_OVERLAY_SURFACE_BUILD_H
#define CBX_OVERLAY_SURFACE_BUILD_H

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#include "ui/dirty_rect.h"

/* Opaque render callback type — same shape as cbx_dirty_render_fn. */
typedef int (*cbx_overlay_render_fn)(SDL_Renderer *r,
                                      const SDL_Rect *clip,
                                      void *userdata);

typedef struct {
    SDL_Texture    *texture;   /* target texture at screen resolution  */
    int             width;     /* texture / screen width               */
    int             height;    /* texture / screen height              */
    bool            visible;   /* overlay currently shown              */
    uint8_t         opacity;   /* 0–255, from settings overlay_opacity */
    cbx_dirty_rect  dirty;     /* dirty-rect tracker                   */
    bool            built;     /* texture successfully created         */
} cbx_overlay_surface;

/*
 * Initialise the overlay surface: create an SDL_TEXTUREACCESS_TARGET
 * texture at the given resolution and apply the initial opacity.
 *
 * @param s        Surface struct to initialise.
 * @param renderer SDL renderer (must support target textures).
 * @param width    Screen width in pixels.
 * @param height   Screen height in pixels.
 * @param opacity  Opacity 0.0–1.0 (clamped, from settings.overlay_opacity).
 * @return 0 on success, -EINVAL on NULL args or zero dims,
 *         -ENOMEM if texture creation fails.
 */
int  cbx_overlay_surface_init(cbx_overlay_surface *s,
                              SDL_Renderer *renderer,
                              int width, int height,
                              double opacity);

/*
 * Destroy the overlay surface: free the SDL_Texture and zero the struct.
 * Safe to call on an uninitialised or failed-init surface.
 */
void cbx_overlay_surface_destroy(cbx_overlay_surface *s);

/*
 * Set the overlay opacity (applies SDL_SetTextureAlphaMod).
 * @param opacity  0.0–1.0 (clamped to [0,1], converted to 0–255).
 */
int  cbx_overlay_surface_set_opacity(cbx_overlay_surface *s, double opacity);

/*
 * Get current opacity as 0–255 integer.
 */
uint8_t cbx_overlay_surface_get_opacity(const cbx_overlay_surface *s);

/*
 * Show the overlay: copy the pre-built texture to the screen and present.
 * No texture creation occurs in this path — just SDL_RenderCopy + present.
 * Sets visible = true.
 */
int  cbx_overlay_surface_show(cbx_overlay_surface *s, SDL_Renderer *r);

/*
 * Hide the overlay: set visible = false.  The texture is NOT destroyed
 * (SPEC §11: "overlay hidden, not destroyed").
 */
void cbx_overlay_surface_hide(cbx_overlay_surface *s);

/*
 * Is the overlay currently visible?
 */
bool cbx_overlay_surface_is_visible(const cbx_overlay_surface *s);

/* --- Dirty-rect management ---------------------------------------- */

/*
 * Mark a screen region as dirty (needs re-render).  The rect is clamped
 * to the surface bounds by cbx_dirty_rect_add.
 */
void cbx_overlay_surface_mark_dirty(cbx_overlay_surface *s,
                                    const SDL_Rect *rect);

/*
 * Mark the entire surface as dirty (full re-render on next render call).
 */
void cbx_overlay_surface_mark_dirty_all(cbx_overlay_surface *s);

/*
 * Clear all dirty regions.
 */
void cbx_overlay_surface_clear_dirty(cbx_overlay_surface *s);

/*
 * Is any region dirty?
 */
bool cbx_overlay_surface_is_dirty(const cbx_overlay_surface *s);

/*
 * Number of dirty regions.
 */
int  cbx_overlay_surface_dirty_count(const cbx_overlay_surface *s);

/* --- Rendering into the target texture ---------------------------- */

/*
 * Render content into the target texture using the dirty-rect mechanism.
 * Sets the render target to the overlay texture, clips to each dirty rect
 * (or full-screen if none dirty), calls fn for each region, then restores
 * the render target to the default (screen).  Merges dirty rects before
 * rendering and clears them afterwards.
 *
 * @param s     Overlay surface (must be built).
 * @param r     SDL renderer.
 * @param fn    Render callback (called per dirty region).
 * @param userdata  Passed through to fn.
 * @return 0 on success, -EINVAL on NULL/invalid args, fn's rc on error.
 */
int  cbx_overlay_surface_render(cbx_overlay_surface *s,
                                SDL_Renderer *r,
                                cbx_overlay_render_fn fn,
                                void *userdata);

/*
 * Get the overlay texture pointer (for Task 29 direct rendering if
 * needed).  Returns NULL if not built.
 */
SDL_Texture *cbx_overlay_surface_get_texture(const cbx_overlay_surface *s);

/*
 * Get surface dimensions.
 */
void cbx_overlay_surface_get_size(const cbx_overlay_surface *s,
                                  int *w, int *h);

/*
 * Is the surface built (texture successfully created)?
 */
bool cbx_overlay_surface_is_built(const cbx_overlay_surface *s);

#endif /* CBX_OVERLAY_SURFACE_BUILD_H */