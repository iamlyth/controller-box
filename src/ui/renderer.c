/*
 * renderer.c — SDL2 renderer initialisation for Controller-Box (Task 19).
 *
 * Implements high-level renderer init with target-texture verification
 * and GLES fallback.  The overlay service requires
 * SDL_RENDERER_TARGETTEXTURE to render the pre-built overlay surface
 * to an off-screen target texture (SPEC §4.9).  On Pi 4 with GLES, the
 * accelerated renderer may not support target textures, so we fall back
 * to a software renderer in that case.
 *
 * The renderer struct is the central rendering context for the entire
 * application.  Both the overlay service and the manager use it.
 */
#include "ui/renderer.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/*
 * Default window title if none is provided.
 */
static const char *default_title = "Controller-Box";

int cbx_renderer_init(cbx_renderer *r, const char *title,
                      int w, int h, bool fullscreen)
{
    if (!r)
        return -EINVAL;

    memset(r, 0, sizeof(*r));

    if (w <= 0) w = CBX_RENDERER_DEFAULT_W;
    if (h <= 0) h = CBX_RENDERER_DEFAULT_H;
    if (!title) title = default_title;

    /* Initialise SDL video subsystem (safe to call multiple times). */
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "cbx_renderer: SDL_InitSubSystem failed: %s\n",
                SDL_GetError());
        return -EIO;
    }

    /* Window flags. */
    Uint32 win_flags = SDL_WINDOW_HIDDEN;
    if (fullscreen)
        win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    r->window = SDL_CreateWindow(title,
                                 SDL_WINDOWPOS_UNDEFINED,
                                 SDL_WINDOWPOS_UNDEFINED,
                                 w, h, win_flags);
    if (!r->window) {
        fprintf(stderr, "cbx_renderer: SDL_CreateWindow failed: %s\n",
                SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return -EIO;
    }
    r->window_w = w;
    r->window_h = h;

    /* Try accelerated renderer with target texture support. */
    r->renderer = SDL_CreateRenderer(r->window, -1, CBX_RENDERER_FLAGS_ACCEL);
    if (r->renderer) {
        r->renderer_flags = CBX_RENDERER_FLAGS_ACCEL;
        if (cbx_renderer_check_target_texture(r->renderer)) {
            r->has_target_texture = true;
        } else {
            /*
             * Accelerated renderer does not support target textures
             * (common on Pi 4 GLES).  Fall back to software renderer.
             */
            SDL_DestroyRenderer(r->renderer);
            r->renderer = NULL;
        }
    }

    /* Fallback: software renderer (always supports target textures). */
    if (!r->renderer) {
        r->renderer = SDL_CreateRenderer(r->window, -1,
                                          SDL_RENDERER_SOFTWARE |
                                          SDL_RENDERER_TARGETTEXTURE);
        if (!r->renderer) {
            fprintf(stderr, "cbx_renderer: SDL_CreateRenderer (software) "
                    "failed: %s\n", SDL_GetError());
            SDL_DestroyWindow(r->window);
            r->window = NULL;
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return -EIO;
        }
        r->renderer_flags = SDL_RENDERER_SOFTWARE | SDL_RENDERER_TARGETTEXTURE;
        r->is_gles = true;  /* software fallback = likely GLES-limited HW */
    }

    /* Query full renderer info. */
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(r->renderer, &info) == 0) {
        if (info.flags & SDL_RENDERER_PRESENTVSYNC)
            r->vsync_enabled = true;
        /* Detect GLES backend by name. */
        if (strstr(info.name, "gles") != NULL ||
            strstr(info.name, "GLES") != NULL)
            r->is_gles = true;
    }

    /* Enable alpha blending (required for overlay transparency). */
    SDL_SetRenderDrawBlendMode(r->renderer, SDL_BLENDMODE_BLEND);

    /* Verify blending works on this renderer. */
    if (cbx_renderer_verify_blending(r->renderer) != 0) {
        fprintf(stderr, "cbx_renderer: warning: alpha blending verification "
                "failed on renderer '%s'\n",
                (SDL_GetRendererInfo(r->renderer, &info) == 0) ?
                 info.name : "unknown");
        /* Non-fatal: continue anyway, overlay may still render correctly. */
    }

    return 0;
}

bool cbx_renderer_check_target_texture(SDL_Renderer *renderer)
{
    if (!renderer) return false;
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(renderer, &info) != 0)
        return false;
    return (info.flags & SDL_RENDERER_TARGETTEXTURE) != 0;
}

int cbx_renderer_verify_blending(SDL_Renderer *renderer)
{
    if (!renderer) return -EINVAL;

    /*
     * Create a small target texture, render a semi-transparent red
     * rectangle over a transparent background, read back the pixels,
     * and verify the alpha channel is non-zero (blending worked).
     */
    SDL_Texture *target = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_TARGET,
        4, 4);
    if (!target)
        return -ENOTSUP;

    int rc = 0;
    SDL_Texture *old_target = SDL_GetRenderTarget(renderer);

    /* Set the target texture and clear to transparent. */
    if (SDL_SetRenderTarget(renderer, target) != 0) {
        rc = -ENOTSUP;
        goto cleanup;
    }
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);

    /* Draw a semi-transparent red rectangle. */
    SDL_SetRenderDrawColor(renderer, 255, 0, 0, 128);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_Rect rect = {0, 0, 4, 4};
    SDL_RenderFillRect(renderer, &rect);

    /* Target textures are generally not lockable.  Read through the active
     * render target instead and decode the requested format with SDL rather
     * than assuming host byte order. */
    Uint32 pixels[16] = {0};
    if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_RGBA8888,
                             pixels, 4 * (int)sizeof(Uint32)) != 0) {
        rc = -ENOTSUP;
        goto restore;
    }
    SDL_PixelFormat *format = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA8888);
    if (!format) {
        rc = -ENOMEM;
        goto restore;
    }
    Uint8 red = 0, green = 0, blue = 0, alpha = 0;
    SDL_GetRGBA(pixels[0], format, &red, &green, &blue, &alpha);
    SDL_FreeFormat(format);
    if (red == 0 || alpha == 0)
        rc = -ENOTSUP;

restore:
    SDL_SetRenderTarget(renderer, old_target);
cleanup:
    SDL_DestroyTexture(target);
    return rc;
}

void cbx_renderer_show(cbx_renderer *r)
{
    if (!r || !r->window) return;
    SDL_ShowWindow(r->window);
}

void cbx_renderer_hide(cbx_renderer *r)
{
    if (!r || !r->window) return;
    SDL_HideWindow(r->window);
}

void cbx_renderer_present(cbx_renderer *r)
{
    if (!r || !r->renderer) return;
    SDL_RenderPresent(r->renderer);
}

void cbx_renderer_clear(cbx_renderer *r, SDL_Color color)
{
    if (!r || !r->renderer) return;
    SDL_SetRenderDrawColor(r->renderer,
                           color.r, color.g, color.b, color.a);
    SDL_SetRenderDrawBlendMode(r->renderer, SDL_BLENDMODE_BLEND);
    SDL_RenderClear(r->renderer);
}

void cbx_renderer_shutdown(cbx_renderer *r)
{
    if (!r) return;
    if (r->renderer) {
        SDL_DestroyRenderer(r->renderer);
        r->renderer = NULL;
    }
    if (r->window) {
        SDL_DestroyWindow(r->window);
        r->window = NULL;
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    memset(r, 0, sizeof(*r));
}