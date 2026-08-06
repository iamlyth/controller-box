/*
 * icon_cache.h — SVG-to-SDL2 texture rasterization cache (Task 17).
 *
 * Rasterizes all mapped SVG icons into SDL2 textures at startup using
 * a single reusable NSVGrasterizer.  Textures are stored in a simple
 * open-addressing hash map keyed by icon name and are re-colourable at
 * runtime via SDL_SetTextureColorMod() for theme support (SPEC §8.3).
 *
 * The cache owns the pixel buffers only transiently (they are uploaded
 * to GPU/texture memory and freed immediately).  On shutdown
 * cbx_icon_cache_cleanup() destroys every texture and the rasterizer.
 *
 * Usage:
 *   cbx_icon_cache cache;
 *   cbx_icon_cache_init(&cache, renderer, "/usr/share/controller-box/icons/svg", 128);
 *   cbx_icon_cache_load(&cache, &map);           // rasterise all mapped icons
 *   SDL_Texture *tex = cbx_icon_cache_get(&cache, "cc-ps5");
 *   SDL_SetTextureColorMod(tex, r, g, b);         // theme recolour
 *   cbx_icon_cache_cleanup(&cache);
 */
#ifndef CBX_ICON_CACHE_H
#define CBX_ICON_CACHE_H

#include <SDL2/SDL.h>
#include "icons/icon_map.h"

/*
 * Maximum number of cached icons.  128 covers the 22 mapped types plus
 * profile-overridden custom icons loaded on demand (Task 18).
 */
#define CBX_ICON_CACHE_MAX 128

/*
 * Hash table size (power of two for fast modulo).  Must be >=
 * CBX_ICON_CACHE_MAX to keep the load factor below 1.
 */
#define CBX_ICON_CACHE_HASH_SIZE 256

typedef struct {
    char name[CBX_ICON_ICON_LEN];   /* icon name (key)                  */
    SDL_Texture *texture;           /* cached texture (NULL = empty)    */
    int width;                       /* texture pixel width              */
    int height;                      /* texture pixel height             */
} cbx_icon_cache_entry;

typedef struct cbx_icon_cache {
    SDL_Renderer *renderer;
    struct NSVGrasterizer *rasterizer;   /* reusable, opaque            */
    cbx_icon_cache_entry entries[CBX_ICON_CACHE_HASH_SIZE];
    int count;                           /* number of live entries      */
    char icon_dir[512];                 /* path to SVG directory        */
    int target_size;                     /* max pixel dimension for icons */
} cbx_icon_cache;

/*
 * Initialise the cache.  Creates a reusable NSVGrasterizer and stores
 * the renderer + icon directory for later texture creation.  The cache
 * does NOT take ownership of renderer (caller must keep it alive until
 * cleanup).  target_size is the maximum pixel dimension (width or height)
 * for rasterized icons; the SVG's aspect ratio is preserved.
 * Returns 0 on success, -EINVAL on NULL args, -ENOMEM if rasterizer
 * allocation fails.
 */
int cbx_icon_cache_init(cbx_icon_cache *cache, SDL_Renderer *renderer,
                         const char *icon_dir, int target_size);

/*
 * Rasterize all icons referenced in *map into textures and store them
 * in the cache.  Icons already present in the cache are skipped (so the
 * function is idempotent for shared icon names like "generic-gamepad").
 * Returns 0 on success, negative errno on error (individual icon
 * failures are logged but do not abort the batch).
 */
int cbx_icon_cache_load(cbx_icon_cache *cache, const cbx_icon_map *map);

/*
 * Look up a cached texture by icon name.  Returns the SDL_Texture* or
 * NULL if the icon is not in the cache.
 */
SDL_Texture *cbx_icon_cache_get(const cbx_icon_cache *cache,
                                 const char *icon_name);

/*
 * Look up cached texture dimensions.  Returns 0 on success (fills out_w
 * and out_h), -ENOENT if the icon is not cached, -EINVAL on bad args.
 */
int cbx_icon_cache_get_dims(const cbx_icon_cache *cache,
                             const char *icon_name,
                             int *out_w, int *out_h);

/*
 * Rasterize and cache a single icon by name (e.g. "cc-ps5").  If the
 * icon is already cached, returns 0 without re-rasterizing.  This is
 * used by Task 18 for profile-overridden icons not in the default map.
 * Returns 0 on success, -ENOENT if the SVG file does not exist,
 * -ENOMEM on allocation failure, -EINVAL on bad args.
 */
int cbx_icon_cache_load_one(cbx_icon_cache *cache, const char *icon_name);

/*
 * Insert an externally-created texture into the cache under the given
 * key.  Used by Task 18 (icon_lookup) to cache PNG textures loaded via
 * SDL2_image.  The cache takes ownership of the texture — it will be
 * destroyed by cbx_icon_cache_cleanup().  If the key already exists,
 * the old texture is destroyed and replaced.
 *
 * @param cache  Initialized icon cache.
 * @param key    Cache key (e.g. absolute path of the PNG file).
 * @param tex    SDL_Texture* to store (must not be NULL).
 * @param w      Texture width.
 * @param h      Texture height.
 * @return 0 on success, -EINVAL on bad args, -ENOMEM if cache is full.
 */
int cbx_icon_cache_insert(cbx_icon_cache *cache, const char *key,
                           SDL_Texture *tex, int w, int h);

/*
 * Destroy all cached textures and the rasterizer.  Safe to call on a
 * zeroed/empty cache.  After cleanup the struct can be re-initialised
 * with cbx_icon_cache_init().
 */
void cbx_icon_cache_cleanup(cbx_icon_cache *cache);

#endif /* CBX_ICON_CACHE_H */