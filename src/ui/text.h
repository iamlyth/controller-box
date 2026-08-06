/*
 * text.h — Text rendering cache with SDL2_ttf (Task 19).
 *
 * Provides font loading and a text texture cache keyed by
 * (font_id, text, colour_hash).  Textures are created once and cached
 * for the lifetime of the renderer, avoiding per-frame TTF_Render
 * calls.  Multi-line text wrapping is supported.
 *
 * Font management:
 *   - Fonts are loaded by path at a given point size and assigned an
 *     integer ID (0, 1, 2, ...).  The caller references fonts by ID.
 *   - A default font can be loaded by path (commonly a system TTF).
 *
 * Text cache:
 *   - The cache is a fixed-size open-addressing hash map keyed by a
 *     composite hash of (font_id, text string, colour).  This ensures
 *     the same text rendered in different colours (e.g. focused vs
 *     unfocused) produces separate textures.
 *   - On cache miss, the text is rendered via TTF_RenderUTF8_Blended
 *     and uploaded to an SDL_Texture.  The surface is freed immediately.
 *   - Multi-line text is wrapped to a max pixel width using
 *     TTF_FontHeight for line spacing.
 *
 * Usage:
 *   cbx_text_cache cache;
 *   cbx_text_cache_init(&cache, renderer);
 *   int font_id = cbx_text_load_font(&cache, "/path/to/font.ttf", 24);
 *   SDL_Texture *tex = cbx_text_render(&cache, font_id, "Hello",
 *                                       (SDL_Color){255,255,255,255});
 *   // ... render tex ...
 *   cbx_text_cache_cleanup(&cache);
 */
#ifndef CBX_UI_TEXT_H
#define CBX_UI_TEXT_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdbool.h>

/* Maximum number of simultaneously loaded fonts. */
#define CBX_TEXT_MAX_FONTS 8

/* Maximum number of cached text textures. */
#define CBX_TEXT_CACHE_MAX 256

/* Hash table size (power of two for bitmask modulo). */
#define CBX_TEXT_HASH_SIZE 512

/* Maximum text string length for cache entries. */
#define CBX_TEXT_MAX_LEN 256

/* Font IDs start at 0.  -1 indicates an unused slot. */
#define CBX_TEXT_FONT_NONE (-1)

typedef struct {
    TTF_Font *font;          /* SDL_ttf font handle (NULL = unused)     */
    int pt_size;              /* point size                              */
    char path[512];           /* font file path (for diagnostics)        */
} cbx_font_entry;

typedef struct {
    uint32_t hash;            /* composite hash (0 = empty slot)         */
    int font_id;              /* font used to render this text           */
    SDL_Color color;          /* colour used to render this text          */
    char text[CBX_TEXT_MAX_LEN]; /* the text string (key)                */
    SDL_Texture *texture;     /* cached texture                          */
    int width;                /* texture pixel width                     */
    int height;               /* texture pixel height                    */
} cbx_text_cache_entry;

typedef struct cbx_text_cache {
    SDL_Renderer *renderer;
    cbx_font_entry fonts[CBX_TEXT_MAX_FONTS];
    int font_count;
    cbx_text_cache_entry entries[CBX_TEXT_HASH_SIZE];
    int entry_count;
} cbx_text_cache;

/*
 * Initialise the text cache.  Must be called before any other function.
 * Calls TTF_Init() if not already initialised.  The cache does NOT take
 * ownership of the renderer (caller must keep it alive until cleanup).
 *
 * @return 0 on success, -EINVAL on NULL, -EIO if TTF_Init fails.
 */
int cbx_text_cache_init(cbx_text_cache *cache, SDL_Renderer *renderer);

/*
 * Load a TrueType font from a file path at the given point size.
 * Returns a font ID >= 0 on success, -EINVAL on bad args, -ENOMEM if
 * all font slots are full, -EIO if the file cannot be loaded.
 */
int cbx_text_load_font(cbx_text_cache *cache, const char *path, int pt_size);

/*
 * Get the default font ID (the first loaded font, ID 0).  Returns -1
 * if no fonts are loaded.
 */
int cbx_text_default_font(const cbx_text_cache *cache);

/*
 * Render text to an SDL_Texture.  If the (font_id, text, colour)
 * combination is already cached, returns the cached texture.
 * Otherwise renders via TTF_RenderUTF8_Blended, uploads to a texture,
 * and caches it.
 *
 * @param cache    Text cache.
 * @param font_id  Font ID from cbx_text_load_font.
 * @param text     UTF-8 text string (NULL-terminated).
 * @param color    Text colour (alpha channel is ignored by TTF; the
 *                 surface uses the colour's RGB and full alpha).
 * @return SDL_Texture* on success, NULL on error (including bad font_id,
 *         empty text, or cache full).
 */
SDL_Texture *cbx_text_render(cbx_text_cache *cache, int font_id,
                               const char *text, SDL_Color color);

/*
 * Render multi-line text with word wrapping to a maximum pixel width.
 * Each line is rendered and cached separately.  The lines are returned
 * as an array of SDL_Texture* pointers.  The caller must free the array
 * (but NOT the individual textures — they are owned by the cache).
 *
 * Line height = TTF_FontHeight(font).  Words longer than max_w are
 * hard-broken at the character level.
 *
 * @param cache     Text cache.
 * @param font_id   Font ID.
 * @param text      UTF-8 text (may contain embedded newlines).
 * @param color     Text colour.
 * @param max_w     Maximum pixel width for wrapping (0 = no wrapping).
 * @param out_lines Array of SDL_Texture* pointers (caller frees array,
 *                  not individual textures).  Set to NULL on error.
 * @param out_count Number of lines returned.
 * @param out_total_h Total height of all lines combined (line_height * count).
 * @return 0 on success, negative errno on error.
 */
int cbx_text_render_wrapped(cbx_text_cache *cache, int font_id,
                             const char *text, SDL_Color color,
                             int max_w,
                             SDL_Texture ***out_lines, int *out_count,
                             int *out_total_h);

/*
 * Get the dimensions of a cached text texture without rendering it.
 * Returns 0 on success (fills out_w, out_h), -ENOENT if not cached,
 * -EINVAL on bad args.
 */
int cbx_text_get_dims(const cbx_text_cache *cache, int font_id,
                       const char *text, SDL_Color color,
                       int *out_w, int *out_h);

/*
 * Measure the pixel width of a text string without caching it.
 * Uses TTF_SizeUTF8.  Returns 0 on success (fills out_w and out_h),
 * -EINVAL on bad args, -EIO if the font is invalid.
 */
int cbx_text_measure(const cbx_text_cache *cache, int font_id,
                      const char *text, int *out_w, int *out_h);

/*
 * Get the line height (in pixels) for a given font ID.
 * Returns TTF_FontHeight(font), or 0 if font_id is invalid.
 */
int cbx_text_line_height(const cbx_text_cache *cache, int font_id);

/*
 * Clear all cached text textures (frees textures but keeps fonts loaded).
 * Called when the theme changes and text colours need to be re-rendered.
 */
void cbx_text_cache_clear(cbx_text_cache *cache);

/*
 * Shut down and free all resources including fonts.  Safe to call on a
 * zeroed struct.  After cleanup the struct can be re-initialised.
 */
void cbx_text_cache_cleanup(cbx_text_cache *cache);

#endif /* CBX_UI_TEXT_H */