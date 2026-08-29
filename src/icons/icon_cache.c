/*
 * icon_cache.c — SVG-to-SDL2 texture rasterization cache (Task 17).
 *
 * Rasterizes SVG icons into SDL2 textures using nanosvg and caches them
 * in a hash map keyed by icon name.  A single NSVGrasterizer is reused
 * across all icons (SPEC §8.3).  Textures use SDL_BLENDMODE_BLEND for
 * alpha compositing and SDL_SetTextureColorMod() for theme recolouring.
 *
 * The hash map uses open addressing with linear probing.  The hash
 * function is djb2 applied to the icon name.  CBX_ICON_CACHE_HASH_SIZE
 * (256) is a power of two so the modulo is a bitmask.
 */
#include "icons/icon_cache.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <nanosvg.h>
#include <nanosvgrast.h>

#include <errno.h>
#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Hash map internals                                                */
/* ------------------------------------------------------------------ */

static unsigned int icon_hash(const char *name)
{
    /* djb2 — simple, fast, good distribution for short strings. */
    unsigned int h = 5381;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        h = ((h << 5) + h) + *p;
    return h;
}

static int insert_entry(cbx_icon_cache *cache, const char *name,
                         SDL_Texture *tex, int w, int h)
{
    if (cache->count >= CBX_ICON_CACHE_MAX)
        return -ENOMEM;

    unsigned int start = icon_hash(name) & (CBX_ICON_CACHE_HASH_SIZE - 1);
    for (int i = 0; i < CBX_ICON_CACHE_HASH_SIZE; i++) {
        int slot = (int)((start + (unsigned int)i) & (CBX_ICON_CACHE_HASH_SIZE - 1));
        if (cache->entries[slot].texture == NULL) {
            /* Empty or tombstone slot — claim it. */
            snprintf(cache->entries[slot].name, sizeof(cache->entries[slot].name),
                     "%s", name);
            cache->entries[slot].texture = tex;
            cache->entries[slot].width  = w;
            cache->entries[slot].height = h;
            cache->count++;
            return 0;
        }
        /* If already present with the same name, replace. */
        if (strcmp(cache->entries[slot].name, name) == 0) {
            if (cache->entries[slot].texture)
                SDL_DestroyTexture(cache->entries[slot].texture);
            cache->entries[slot].texture = tex;
            cache->entries[slot].width  = w;
            cache->entries[slot].height = h;
            return 0;
        }
    }
    return -ENOMEM;
}

/* ------------------------------------------------------------------ */
/*  SVG rasterization                                                 */
/* ------------------------------------------------------------------ */

static int rasterize_svg(cbx_icon_cache *cache, const char *icon_name)
{
    char path[640];
    int plen;

    /* Build full path: icon_dir + "/svg/" + file_name + ".svg"
     * The CMake install layout places SVGs in ${ICON_DIR}/svg/.
     * Strip the "cc-" prefix used by Controllercons icon names in the
     * YAML mapping — the actual SVG files on disk do not have it. */
    const char *file_name = icon_name;
    if (strncmp(icon_name, "cc-", 3) == 0)
        file_name = icon_name + 3;

    /* Reject path traversal — icon names must be simple file names without
     * directory components or parent-directory sequences.  This prevents a
     * malicious profile-metadata sidecar from escaping the icon directory
     * via an icon_override like "../../etc/something". */
    if (strchr(file_name, '/') != NULL ||
        strstr(file_name, "..") != NULL ||
        file_name[0] == '.') {
        fprintf(stderr, "icon_cache: rejecting path traversal in icon name '%s'\n",
                icon_name);
        return -EINVAL;
    }

    plen = snprintf(path, sizeof(path), "%s/svg/%s.svg", cache->icon_dir, file_name);
    if (plen < 0 || (size_t)plen >= sizeof(path))
        return -ENAMETOOLONG;

    /* Parse the SVG. */
    NSVGimage *image = nsvgParseFromFile(path, "px", 96.0f);
    if (!image) {
        fprintf(stderr, "icon_cache: failed to parse %s\n", path);
        return -ENOENT;
    }
    if (!isfinite(image->width) || !isfinite(image->height) ||
        image->width <= 0.0f || image->height <= 0.0f) {
        fprintf(stderr, "icon_cache: invalid dimensions for %s (%.0fx%.0f)\n",
                path, image->width, image->height);
        nsvgDelete(image);
        return -EINVAL;
    }

    /* Scale to fit within target_size, preserving aspect ratio. */
    float scale = (float)cache->target_size /
                  (image->width > image->height ? image->width : image->height);
    int tex_w = (int)(image->width  * scale + 0.5f);
    int tex_h = (int)(image->height * scale + 0.5f);
    if (tex_w < 1) tex_w = 1;
    if (tex_h < 1) tex_h = 1;

    /* Allocate pixel buffer (RGBA, 4 bytes/pixel). */
    size_t buf_size = (size_t)tex_w * (size_t)tex_h * 4;
    unsigned char *pixels = malloc(buf_size);
    if (!pixels) {
        nsvgDelete(image);
        return -ENOMEM;
    }
    memset(pixels, 0, buf_size);

    /* Rasterize.  nanosvg outputs RGBA (non-premultiplied alpha). */
    nsvgRasterize(cache->rasterizer, image, 0.0f, 0.0f, scale,
                  pixels, tex_w, tex_h, tex_w * 4);
    nsvgDelete(image);

    /* Do not cache an apparently valid but completely transparent asset.
     * The profile editor adopts icon-cache textures directly for its diagram;
     * accepting such an asset would produce a blank production diagram while
     * all texture metadata (dimensions and non-NULL pointer) still looked
     * healthy.  nanosvg writes RGBA bytes, so alpha is byte three. */
    bool has_visible_pixel = false;
    for (size_t i = 3; i < buf_size; i += 4) {
        if (pixels[i] != 0) {
            has_visible_pixel = true;
            break;
        }
    }
    if (!has_visible_pixel) {
        fprintf(stderr, "icon_cache: rejecting transparent asset %s\n", path);
        free(pixels);
        return -EINVAL;
    }

    /* Create SDL2 texture. */
    SDL_Texture *tex = SDL_CreateTexture(
        cache->renderer,
        SDL_PIXELFORMAT_ABGR8888,
        SDL_TEXTUREACCESS_STATIC,
        tex_w, tex_h);
    if (!tex) {
        fprintf(stderr, "icon_cache: SDL_CreateTexture failed for %s: %s\n",
                icon_name, SDL_GetError());
        free(pixels);
        return -ENOMEM;
    }

    /* Upload pixel data to the texture. */
    if (SDL_UpdateTexture(tex, NULL, pixels, tex_w * 4) != 0) {
        fprintf(stderr, "icon_cache: SDL_UpdateTexture failed for %s: %s\n",
                icon_name, SDL_GetError());
        SDL_DestroyTexture(tex);
        free(pixels);
        return -ENOMEM;
    }

    /* Enable alpha blending for compositing. */
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

    free(pixels);

    /* Store in the hash map. */
    int rc = insert_entry(cache, icon_name, tex, tex_w, tex_h);
    if (rc != 0) {
        fprintf(stderr, "icon_cache: hash map full, cannot cache %s\n",
                icon_name);
        SDL_DestroyTexture(tex);
        return rc;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

int cbx_icon_cache_init(cbx_icon_cache *cache, SDL_Renderer *renderer,
                         const char *icon_dir, int target_size)
{
    if (!cache || !renderer || !icon_dir || target_size <= 0)
        return -EINVAL;

    /* Clean up any existing state to avoid leaking the rasterizer / textures
     * when init is called on an already-initialised cache. */
    if (cache->rasterizer || cache->count > 0)
        cbx_icon_cache_cleanup(cache);

    memset(cache, 0, sizeof(*cache));
    cache->renderer = renderer;
    cache->rasterizer = nsvgCreateRasterizer();
    if (!cache->rasterizer)
        return -ENOMEM;

    snprintf(cache->icon_dir, sizeof(cache->icon_dir), "%s", icon_dir);
    cache->target_size = target_size;
    cache->count = 0;
    return 0;
}

int cbx_icon_cache_load(cbx_icon_cache *cache, const cbx_icon_map *map)
{
    if (!cache || !cache->rasterizer || !map)
        return -EINVAL;

    for (int i = 0; i < map->count; i++) {
        const char *icon_name = map->entries[i].icon;
        if (!icon_name || icon_name[0] == '\0')
            continue;

        /* Skip if already cached (shared icons like generic-gamepad). */
        if (cbx_icon_cache_get(cache, icon_name) != NULL)
            continue;

        int rc = rasterize_svg(cache, icon_name);
        if (rc != 0) {
            /* Log but continue — a single missing icon should not abort. */
            fprintf(stderr, "icon_cache: warning: could not load '%s' (rc=%d)\n",
                    icon_name, rc);
        }
    }
    return 0;
}

SDL_Texture *cbx_icon_cache_get(const cbx_icon_cache *cache, const char *icon_name)
{
    if (!cache || !icon_name)
        return NULL;

    unsigned int start = icon_hash(icon_name) & (CBX_ICON_CACHE_HASH_SIZE - 1);
    for (int i = 0; i < CBX_ICON_CACHE_HASH_SIZE; i++) {
        int slot = (int)((start + (unsigned int)i) & (CBX_ICON_CACHE_HASH_SIZE - 1));
        if (cache->entries[slot].texture == NULL) {
            if (cache->entries[slot].name[0] == '\0')
                return NULL;     /* empty — not found */
            continue;            /* tombstone — keep probing */
        }
        if (strcmp(cache->entries[slot].name, icon_name) == 0)
            return cache->entries[slot].texture;
    }
    return NULL;
}

int cbx_icon_cache_get_dims(const cbx_icon_cache *cache, const char *icon_name,
                             int *out_w, int *out_h)
{
    if (!cache || !icon_name)
        return -EINVAL;

    unsigned int start = icon_hash(icon_name) & (CBX_ICON_CACHE_HASH_SIZE - 1);
    for (int i = 0; i < CBX_ICON_CACHE_HASH_SIZE; i++) {
        int slot = (int)((start + (unsigned int)i) & (CBX_ICON_CACHE_HASH_SIZE - 1));
        if (cache->entries[slot].texture == NULL) {
            if (cache->entries[slot].name[0] == '\0')
                return -ENOENT;  /* empty — not found */
            continue;           /* tombstone — keep probing */
        }
        if (strcmp(cache->entries[slot].name, icon_name) == 0) {
            if (out_w) *out_w = cache->entries[slot].width;
            if (out_h) *out_h = cache->entries[slot].height;
            return 0;
        }
    }
    return -ENOENT;
}

int cbx_icon_cache_load_one(cbx_icon_cache *cache, const char *icon_name)
{
    if (!cache || !cache->rasterizer || !icon_name || icon_name[0] == '\0')
        return -EINVAL;

    /* Already cached? */
    if (cbx_icon_cache_get(cache, icon_name) != NULL)
        return 0;

    return rasterize_svg(cache, icon_name);
}

int cbx_icon_cache_insert(cbx_icon_cache *cache, const char *key,
                           SDL_Texture *tex, int w, int h)
{
    if (!cache || !key || key[0] == '\0' || !tex || w <= 0 || h <= 0)
        return -EINVAL;

    return insert_entry(cache, key, tex, w, h);
}

void cbx_icon_cache_cleanup(cbx_icon_cache *cache)
{
    if (!cache)
        return;

    for (int i = 0; i < CBX_ICON_CACHE_HASH_SIZE; i++) {
        if (cache->entries[i].texture) {
            SDL_DestroyTexture(cache->entries[i].texture);
            cache->entries[i].texture = NULL;
        }
        cache->entries[i].name[0] = '\0';
        cache->entries[i].width  = 0;
        cache->entries[i].height = 0;
    }
    cache->count = 0;

    if (cache->rasterizer) {
        nsvgDeleteRasterizer(cache->rasterizer);
        cache->rasterizer = NULL;
    }
    cache->renderer = NULL;
    cache->icon_dir[0] = '\0';
    cache->target_size = 0;
}