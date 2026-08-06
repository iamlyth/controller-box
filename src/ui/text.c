/*
 * text.c — Text rendering cache with SDL2_ttf (Task 19).
 *
 * Implements font loading and a text texture cache using SDL2_ttf.
 * The cache is an open-addressing hash map keyed by a composite hash of
 * (font_id, text, colour).  Multi-line text wrapping splits on spaces
 * and newlines, hard-breaking words longer than max_w.
 *
 * Design notes:
 *   - djb2 hash over (font_id || text || r || g || b) produces the
 *     composite key.  Colour alpha is excluded from the hash because
 *     TTF_RenderUTF8_Blended ignores the alpha channel (it produces a
 *     surface with per-pixel alpha from the font glyph antialiasing).
 *   - Linear probing handles collisions.  Hash table size (512) is
 *     larger than the max entries (256) to keep load factor < 0.5.
 *   - Tombstone entries (hash=1, text="") allow probing to continue
 *     past deleted entries without false stops.  Hash value 0 = empty,
 *     hash value 1 = tombstone.
 *   - The cache does NOT evict entries.  If the cache is full, render
 *     returns NULL (caller should call cbx_text_cache_clear to free
 *     old textures, e.g. on theme change).
 */
#include "ui/text.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Hash function ------------------------------------------------------- */

static uint32_t djb2_hash(int font_id, const char *text, SDL_Color color)
{
    uint32_t h = 5381;
    h = ((h << 5) + h) + (uint32_t)font_id;
    h = ((h << 5) + h) + (uint32_t)color.r;
    h = ((h << 5) + h) + (uint32_t)color.g;
    h = ((h << 5) + h) + (uint32_t)color.b;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        h = ((h << 5) + h) + *p;
    /* Avoid hash values 0 and 1 (reserved for empty/tombstone). */
    if (h <= 1) h += 2;
    return h;
}

static uint32_t hash_slot(uint32_t hash)
{
    return hash & (CBX_TEXT_HASH_SIZE - 1);  /* power-of-two modulo */
}

/* --- Cache internals ----------------------------------------------------- */

/*
 * Find the slot for a given key (font_id, text, color).  Returns the
 * index of the matching entry, or -1 if not found.
 */
static int find_entry(cbx_text_cache *cache, uint32_t hash,
                       int font_id, const char *text, SDL_Color color)
{
    uint32_t start = hash_slot(hash);
    for (int i = 0; i < CBX_TEXT_HASH_SIZE; i++) {
        uint32_t idx = (start + i) & (CBX_TEXT_HASH_SIZE - 1);
        cbx_text_cache_entry *e = &cache->entries[idx];
        if (e->hash == 0)
            return -1;  /* empty — not found */
        if (e->hash == hash &&
            e->font_id == font_id &&
            e->color.r == color.r &&
            e->color.g == color.g &&
            e->color.b == color.b &&
            strcmp(e->text, text) == 0)
            return (int)idx;
        /* tombstone (hash==1) or mismatch — continue probing */
    }
    return -1;  /* table full and not found */
}

/*
 * Find an empty or tombstone slot for insertion.  Returns the index, or
 * -1 if the table is full.
 */
static int find_free_slot(cbx_text_cache *cache, uint32_t hash)
{
    uint32_t start = hash_slot(hash);
    for (int i = 0; i < CBX_TEXT_HASH_SIZE; i++) {
        uint32_t idx = (start + i) & (CBX_TEXT_HASH_SIZE - 1);
        cbx_text_cache_entry *e = &cache->entries[idx];
        if (e->hash == 0 || e->hash == 1)
            return (int)idx;  /* empty or tombstone */
    }
    return -1;  /* full */
}

/* --- Public API ---------------------------------------------------------- */

int cbx_text_cache_init(cbx_text_cache *cache, SDL_Renderer *renderer)
{
    if (!cache || !renderer)
        return -EINVAL;

    memset(cache, 0, sizeof(*cache));
    cache->renderer = renderer;
    cache->font_count = 0;

    /* Initialise SDL_ttf if not already initialised. */
    if (!TTF_WasInit()) {
        if (TTF_Init() != 0) {
            fprintf(stderr, "cbx_text: TTF_Init failed: %s\n", TTF_GetError());
            return -EIO;
        }
    }

    return 0;
}

int cbx_text_load_font(cbx_text_cache *cache, const char *path, int pt_size)
{
    if (!cache || !path || pt_size <= 0)
        return -EINVAL;
    if (cache->font_count >= CBX_TEXT_MAX_FONTS)
        return -ENOMEM;

    int id = cache->font_count;
    TTF_Font *font = TTF_OpenFont(path, pt_size);
    if (!font) {
        fprintf(stderr, "cbx_text: TTF_OpenFont('%s', %d) failed: %s\n",
                path, pt_size, TTF_GetError());
        return -EIO;
    }

    cbx_font_entry *fe = &cache->fonts[id];
    fe->font = font;
    fe->pt_size = pt_size;
    strncpy(fe->path, path, sizeof(fe->path) - 1);
    fe->path[sizeof(fe->path) - 1] = '\0';

    cache->font_count++;
    return id;
}

int cbx_text_default_font(const cbx_text_cache *cache)
{
    if (!cache || cache->font_count == 0)
        return -1;
    return 0;
}

static TTF_Font *get_font(cbx_text_cache *cache, int font_id)
{
    if (!cache || font_id < 0 || font_id >= cache->font_count)
        return NULL;
    return cache->fonts[font_id].font;
}

static SDL_Texture *render_to_texture(cbx_text_cache *cache,
                                       TTF_Font *font,
                                       const char *text,
                                       SDL_Color color)
{
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, color);
    if (!surf) {
        fprintf(stderr, "cbx_text: TTF_RenderUTF8_Blended failed: %s\n",
                TTF_GetError());
        return NULL;
    }

    SDL_Texture *tex = SDL_CreateTextureFromSurface(cache->renderer, surf);
    SDL_FreeSurface(surf);
    if (!tex) {
        fprintf(stderr, "cbx_text: SDL_CreateTextureFromSurface failed: %s\n",
                SDL_GetError());
        return NULL;
    }

    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    return tex;
}

SDL_Texture *cbx_text_render(cbx_text_cache *cache, int font_id,
                               const char *text, SDL_Color color)
{
    if (!cache || !text || text[0] == '\0')
        return NULL;

    TTF_Font *font = get_font(cache, font_id);
    if (!font)
        return NULL;

    /* Check the cache first. */
    uint32_t h = djb2_hash(font_id, text, color);
    int idx = find_entry(cache, h, font_id, text, color);
    if (idx >= 0)
        return cache->entries[idx].texture;

    /* Check if text is too long for the cache entry. */
    if (strlen(text) >= CBX_TEXT_MAX_LEN) {
        /* Render without caching. */
        return render_to_texture(cache, font, text, color);
    }

    /* Find a free slot. */
    idx = find_free_slot(cache, h);
    if (idx < 0) {
        /* Cache full — render without caching. */
        return render_to_texture(cache, font, text, color);
    }

    /* Render and cache. */
    SDL_Texture *tex = render_to_texture(cache, font, text, color);
    if (!tex)
        return NULL;

    cbx_text_cache_entry *e = &cache->entries[idx];
    e->hash = h;
    e->font_id = font_id;
    e->color = color;
    strncpy(e->text, text, CBX_TEXT_MAX_LEN - 1);
    e->text[CBX_TEXT_MAX_LEN - 1] = '\0';
    e->texture = tex;

    /* Query texture dimensions. */
    SDL_QueryTexture(tex, NULL, NULL, &e->width, &e->height);

    cache->entry_count++;
    return tex;
}

int cbx_text_get_dims(const cbx_text_cache *cache, int font_id,
                       const char *text, SDL_Color color,
                       int *out_w, int *out_h)
{
    if (!cache || !text)
        return -EINVAL;

    /* Search cache. */
    uint32_t h = djb2_hash(font_id, text, color);
    uint32_t start = hash_slot(h);
    for (int i = 0; i < CBX_TEXT_HASH_SIZE; i++) {
        uint32_t idx = (start + i) & (CBX_TEXT_HASH_SIZE - 1);
        const cbx_text_cache_entry *e = &cache->entries[idx];
        if (e->hash == 0)
            break;  /* empty — not found */
        if (e->hash == h &&
            e->font_id == font_id &&
            e->color.r == color.r &&
            e->color.g == color.g &&
            e->color.b == color.b &&
            strcmp(e->text, text) == 0) {
            if (out_w) *out_w = e->width;
            if (out_h) *out_h = e->height;
            return 0;
        }
    }
    return -ENOENT;
}

int cbx_text_measure(const cbx_text_cache *cache, int font_id,
                      const char *text, int *out_w, int *out_h)
{
    if (!cache || !text)
        return -EINVAL;

    TTF_Font *font = get_font((cbx_text_cache *)cache, font_id);
    if (!font)
        return -EIO;

    int w = 0, h = 0;
    if (TTF_SizeUTF8(font, text, &w, &h) != 0)
        return -EIO;

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return 0;
}

int cbx_text_line_height(const cbx_text_cache *cache, int font_id)
{
    if (!cache) return 0;
    TTF_Font *font = get_font((cbx_text_cache *)cache, font_id);
    if (!font) return 0;
    return TTF_FontHeight(font);
}

/*
 * Word-wrap a line of text to fit within max_w pixels.
 * Returns an array of substrings (caller must free the array and each
 * string).  *out_count is set to the number of lines.
 */
static int wrap_line(TTF_Font *font, const char *line, int max_w,
                      char ***out_lines, int *out_count)
{
    /* Start with a reasonable capacity. */
    int cap = 4;
    char **lines = malloc(cap * sizeof(char *));
    if (!lines) return -ENOMEM;
    int count = 0;

    if (line[0] == '\0') {
        /* Empty line — keep as an empty string. */
        lines[0] = strdup("");
        if (!lines[0]) { free(lines); return -ENOMEM; }
        *out_lines = lines;
        *out_count = 1;
        return 0;
    }

    /* If max_w <= 0, no wrapping — return the whole line. */
    if (max_w <= 0) {
        lines[0] = strdup(line);
        if (!lines[0]) { free(lines); return -ENOMEM; }
        *out_lines = lines;
        *out_count = 1;
        return 0;
    }

    const char *start = line;
    const char *last_space = NULL;
    const char *cursor = line;

    while (*cursor) {
        if (*cursor == ' ' || *cursor == '\t')
            last_space = cursor;

        /* Measure the substring from 'start' to 'cursor' (inclusive). */
        int len = (int)(cursor - start) + 1;
        char *substr = malloc(len + 1);
        if (!substr) goto fail;
        memcpy(substr, start, len);
        substr[len] = '\0';

        int w = 0, h = 0;
        TTF_SizeUTF8(font, substr, &w, &h);
        free(substr);

        if (w > max_w) {
            /* Need to break. */
            if (last_space && last_space >= start) {
                /* Break at the last space. */
                len = (int)(last_space - start);
                char *segment = malloc(len + 1);
                if (!segment) goto fail;
                memcpy(segment, start, len);
                segment[len] = '\0';

                if (count >= cap) {
                    cap *= 2;
                    char **tmp = realloc(lines, cap * sizeof(char *));
                    if (!tmp) { free(segment); goto fail; }
                    lines = tmp;
                }
                lines[count++] = segment;

                /* Skip the space. */
                start = last_space + 1;
                last_space = NULL;
                cursor = start;
                continue;
            } else if (cursor > start) {
                /* No space found — hard break at the current position
                 * (before the current character). */
                len = (int)(cursor - start);
                char *segment = malloc(len + 1);
                if (!segment) goto fail;
                memcpy(segment, start, len);
                segment[len] = '\0';

                if (count >= cap) {
                    cap *= 2;
                    char **tmp = realloc(lines, cap * sizeof(char *));
                    if (!tmp) { free(segment); goto fail; }
                    lines = tmp;
                }
                lines[count++] = segment;

                start = cursor;
                last_space = NULL;
                continue;
            }
            /* cursor == start: single character is too wide, advance. */
        }
        cursor++;
    }

    /* Add the remaining text. */
    if (*start) {
        if (count >= cap) {
            cap++;
            char **tmp = realloc(lines, cap * sizeof(char *));
            if (!tmp) goto fail;
            lines = tmp;
        }
        lines[count++] = strdup(start);
    }

    *out_lines = lines;
    *out_count = count;
    return 0;

fail:
    for (int i = 0; i < count; i++)
        free(lines[i]);
    free(lines);
    return -ENOMEM;
}

int cbx_text_render_wrapped(cbx_text_cache *cache, int font_id,
                             const char *text, SDL_Color color,
                             int max_w,
                             SDL_Texture ***out_lines, int *out_count,
                             int *out_total_h)
{
    if (!cache || !text || !out_lines || !out_count)
        return -EINVAL;

    *out_lines = NULL;
    *out_count = 0;
    if (out_total_h) *out_total_h = 0;

    TTF_Font *font = get_font(cache, font_id);
    if (!font)
        return -EINVAL;

    /* Split into lines by newline, then word-wrap each line. */
    char *text_copy = strdup(text);
    if (!text_copy) return -ENOMEM;

    int total_lines = 0;
    int line_height = TTF_FontHeight(font);
    int cap = 8;
    SDL_Texture **texs = malloc(cap * sizeof(SDL_Texture *));
    if (!texs) { free(text_copy); return -ENOMEM; }

    char *saveptr = NULL;
    char *line = strtok_r(text_copy, "\n", &saveptr);
    while (line) {
        char **wrapped = NULL;
        int wcount = 0;
        int rc = wrap_line(font, line, max_w, &wrapped, &wcount);
        if (rc != 0) {
            free(texs);
            free(text_copy);
            return rc;
        }

        for (int i = 0; i < wcount; i++) {
            SDL_Texture *tex = cbx_text_render(cache, font_id,
                                                wrapped[i], color);
            if (!tex) {
                /* Render failed for this line — skip it. */
                free(wrapped[i]);
                continue;
            }
            if (total_lines >= cap) {
                cap *= 2;
                SDL_Texture **tmp = realloc(texs, cap * sizeof(SDL_Texture *));
                if (!tmp) {
                    for (int j = 0; j < wcount; j++)
                        free(wrapped[j]);
                    free(wrapped);
                    free(texs);
                    free(text_copy);
                    return -ENOMEM;
                }
                texs = tmp;
            }
            texs[total_lines++] = tex;
            free(wrapped[i]);
        }
        free(wrapped);
        line = strtok_r(NULL, "\n", &saveptr);
    }

    free(text_copy);

    *out_lines = texs;
    *out_count = total_lines;
    if (out_total_h) *out_total_h = total_lines * line_height;
    return 0;
}

void cbx_text_cache_clear(cbx_text_cache *cache)
{
    if (!cache) return;
    for (int i = 0; i < CBX_TEXT_HASH_SIZE; i++) {
        cbx_text_cache_entry *e = &cache->entries[i];
        if (e->texture) {
            SDL_DestroyTexture(e->texture);
            e->texture = NULL;
        }
        e->hash = 0;
        e->text[0] = '\0';
        e->width = 0;
        e->height = 0;
    }
    cache->entry_count = 0;
}

void cbx_text_cache_cleanup(cbx_text_cache *cache)
{
    if (!cache) return;

    /* Destroy all cached textures. */
    cbx_text_cache_clear(cache);

    /* Close all fonts. */
    for (int i = 0; i < cache->font_count; i++) {
        if (cache->fonts[i].font) {
            TTF_CloseFont(cache->fonts[i].font);
            cache->fonts[i].font = NULL;
        }
    }
    cache->font_count = 0;
    cache->renderer = NULL;
}