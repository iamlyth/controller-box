/*
 * renderer.h — SDL2 renderer initialisation for Controller-Box (Task 19).
 *
 * Provides a high-level renderer initialisation that:
 *   - Creates an SDL2 window + renderer with the correct flags for
 *     pre-built overlay rendering (SDL_RENDERER_ACCELERATED |
 *     SDL_RENDERER_TARGETTEXTURE).
 *   - Verifies SDL_RENDERER_TARGETTEXTURE is available (required for
 *     rendering to off-screen target textures — the pre-built overlay
 *     surface, SPEC §4.9).
 *   - Falls back to a software renderer on Pi 4 / GLES environments
 *     where the accelerated renderer may not support target textures.
 *     This ensures the overlay works on all target hardware.
 *   - Exposes renderer info (flags, vsync, GLES mode) for diagnostics.
 *
 * The renderer struct holds the window + renderer pair and is the
 * central rendering context for the entire application.  Both the
 * overlay service and the manager use this struct.
 */
#ifndef CBX_UI_RENDERER_H
#define CBX_UI_RENDERER_H

#include <SDL2/SDL.h>
#include <stdbool.h>

/* Renderer flags used for the overlay surface. */
#define CBX_RENDERER_FLAGS_ACCEL \
    (SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE)

/* Window default size (used when not fullscreen). */
#define CBX_RENDERER_DEFAULT_W 1280
#define CBX_RENDERER_DEFAULT_H 720

typedef struct {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    Uint32        renderer_flags;   /* actual flags from SDL_GetRendererInfo */
    bool          is_gles;          /* true if using OpenGL ES backend       */
    bool          has_target_texture;/* true if SDL_RENDERER_TARGETTEXTURE    */
    bool          vsync_enabled;    /* true if SDL_RENDERER_PRESENTVSYNC      */
    int           window_w;
    int           window_h;
} cbx_renderer;

/*
 * Initialise SDL2 video subsystem and create a window + renderer.
 *
 * Calls SDL_Init(SDL_INIT_VIDEO), creates a hidden window, and attempts
 * to create an accelerated renderer with TARGETTEXTURE support.  If the
 * accelerated renderer does not support target textures (common on Pi 4
 * GLES), falls back to a software renderer and sets is_gles = true.
 *
 * After init, the caller should call cbx_renderer_show() to make the
 * window visible (overlay service typically keeps it hidden until the
 * overlay is activated).
 *
 * @param r       Renderer struct to initialise.
 * @param title   Window title (may be NULL for a default).
 * @param w       Window width (0 = default 1280).
 * @param h       Window height (0 = default 720).
 * @param fullscreen  If true, create a fullscreen window.
 * @return 0 on success, negative errno on error.
 */
int cbx_renderer_init(cbx_renderer *r, const char *title,
                      int w, int h, bool fullscreen);

/*
 * Check whether the renderer supports SDL_RENDERER_TARGETTEXTURE.
 * Called internally by cbx_renderer_init; exposed for testing.
 * Returns true if the flag is set in the renderer's info.
 */
bool cbx_renderer_check_target_texture(SDL_Renderer *renderer);

/*
 * Verify alpha blending works correctly on the given renderer.
 * Creates a small target texture, draws a semi-transparent rectangle,
 * reads back the pixels, and checks the alpha channel is blended.
 * Used by the GLES fallback path to verify the software renderer can
 * handle the overlay's semi-transparent background.
 *
 * @return 0 if blending works, -ENOTSUP if blending fails.
 */
int cbx_renderer_verify_blending(SDL_Renderer *renderer);

/*
 * Make the window visible (unhide).  No-op if already visible.
 */
void cbx_renderer_show(cbx_renderer *r);

/*
 * Hide the window.  No-op if already hidden.
 */
void cbx_renderer_hide(cbx_renderer *r);

/*
 * Present the back buffer (swap).  Calls SDL_RenderPresent().
 */
void cbx_renderer_present(cbx_renderer *r);

/*
 * Clear the back buffer with the given colour.
 */
void cbx_renderer_clear(cbx_renderer *r, SDL_Color color);

/*
 * Shut down and free all resources.  Safe to call on a zeroed struct.
 * After shutdown the struct can be re-initialised with cbx_renderer_init().
 */
void cbx_renderer_shutdown(cbx_renderer *r);

#endif /* CBX_UI_RENDERER_H */