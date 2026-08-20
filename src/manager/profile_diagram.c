/*
 * profile_diagram.c — SVG-based controller diagram with individually
 * highlightable buttons (Task 37, left panel).
 *
 * Implements a custom cbx_widget that renders a controller outline and
 * highlights individual buttons at normalised positions.  The base
 * image is loaded from SVG via nanosvg (if available), or a plain
 * background rectangle is drawn (for headless tests).
 *
 * Task 37 — Profile editor — controller diagram and binding list mode.
 */
#include "manager/profile_diagram.h"

#include <SDL2/SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* nanosvg — vendored (Task 1), headers only (impl in nanosvg_impl.c) */
#include <nanosvg.h>
#include <nanosvgrast.h>

/* ------------------------------------------------------------------ */
/*  Button position table                                             */
/* ------------------------------------------------------------------ */
/*
 * Normalised positions for a generic gamepad layout.  Coordinates are
 * (x, y, w, h) in the range [0.0, 1.0] relative to the diagram rect.
 * Positions are approximate and designed for a landscape controller
 * outline.
 */

static const cbx_diag_button_pos s_button_pos[CBX_DIAG_BTN_COUNT] = {
    /* D-pad (left side, cross shape) */
    [CBX_DIAG_BTN_UP]    = { CBX_DIAG_BTN_UP,    "Up",    0.20f, 0.25f, 0.08f, 0.08f },
    [CBX_DIAG_BTN_DOWN]  = { CBX_DIAG_BTN_DOWN,  "Down",  0.20f, 0.41f, 0.08f, 0.08f },
    [CBX_DIAG_BTN_LEFT]  = { CBX_DIAG_BTN_LEFT,  "Left",  0.10f, 0.33f, 0.08f, 0.08f },
    [CBX_DIAG_BTN_RIGHT] = { CBX_DIAG_BTN_RIGHT, "Right", 0.30f, 0.33f, 0.08f, 0.08f },

    /* Face buttons (right side, diamond) */
    [CBX_DIAG_BTN_A]     = { CBX_DIAG_BTN_A,     "A",     0.72f, 0.41f, 0.08f, 0.08f },
    [CBX_DIAG_BTN_B]     = { CBX_DIAG_BTN_B,     "B",     0.82f, 0.33f, 0.08f, 0.08f },
    [CBX_DIAG_BTN_X]     = { CBX_DIAG_BTN_X,     "X",     0.62f, 0.33f, 0.08f, 0.08f },
    [CBX_DIAG_BTN_Y]     = { CBX_DIAG_BTN_Y,     "Y",     0.72f, 0.25f, 0.08f, 0.08f },

    /* Center buttons */
    [CBX_DIAG_BTN_START]  = { CBX_DIAG_BTN_START,  "Start",  0.58f, 0.17f, 0.08f, 0.06f },
    [CBX_DIAG_BTN_SELECT] = { CBX_DIAG_BTN_SELECT, "Select", 0.38f, 0.17f, 0.08f, 0.06f },
    [CBX_DIAG_BTN_GUIDE]  = { CBX_DIAG_BTN_GUIDE,  "Guide",  0.48f, 0.17f, 0.08f, 0.06f },

    /* Shoulders / triggers (top edge) */
    [CBX_DIAG_BTN_L1] = { CBX_DIAG_BTN_L1, "L1", 0.15f, 0.05f, 0.10f, 0.05f },
    [CBX_DIAG_BTN_R1] = { CBX_DIAG_BTN_R1, "R1", 0.75f, 0.05f, 0.10f, 0.05f },
    [CBX_DIAG_BTN_L2] = { CBX_DIAG_BTN_L2, "L2", 0.15f, 0.00f, 0.10f, 0.04f },
    [CBX_DIAG_BTN_R2] = { CBX_DIAG_BTN_R2, "R2", 0.75f, 0.00f, 0.10f, 0.04f },

    /* Stick clicks (center-left and center-right) */
    [CBX_DIAG_BTN_L3] = { CBX_DIAG_BTN_L3, "L3", 0.40f, 0.55f, 0.10f, 0.10f },
    [CBX_DIAG_BTN_R3] = { CBX_DIAG_BTN_R3, "R3", 0.55f, 0.55f, 0.10f, 0.10f },
};

/* ------------------------------------------------------------------ */
/*  Vtable forward declarations                                       */
/* ------------------------------------------------------------------ */

static void diag_draw(cbx_widget *w, SDL_Renderer *r);
static void diag_get_rect(const cbx_widget *w, SDL_Rect *out);
static void diag_set_rect(cbx_widget *w, const SDL_Rect *rect);
static void diag_destroy(cbx_widget *w);

static const cbx_widget_vtable s_diag_vt = {
    .draw     = diag_draw,
    .handle_event = NULL,   /* diagram is display-only */
    .focus    = NULL,
    .blur     = NULL,
    .get_rect = diag_get_rect,
    .set_rect = diag_set_rect,
    .destroy  = diag_destroy,
};

/* ------------------------------------------------------------------ */
/*  SVG loading                                                       */
/* ------------------------------------------------------------------ */

/*
 * Load an SVG file, rasterise it to an SDL_Texture, and return it.
 * Returns NULL on any failure (file not found, parse error, texture
 * creation failure).  The caller owns the returned texture.
 */
static SDL_Texture *
load_svg_texture(SDL_Renderer *renderer, const char *svg_path, int size)
{
    if (!renderer || !svg_path)
        return NULL;

    int fd = open(svg_path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return NULL;
    FILE *f = fdopen(fd, "rb");
    if (!f) {
        close(fd);
        return NULL;
    }

    /* Read file contents */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 4 * 1024 * 1024) {  /* 4 MB max */
        fclose(f);
        return NULL;
    }

    char *buf = (char *)malloc((size_t)fsize + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t rd = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    buf[rd] = '\0';

    /* nanosvg mutates its input — buf is malloc'd, so that's fine */
    NSVGimage *image = nsvgParse(buf, "px", 96.0f);
    free(buf);

    if (!image)
        return NULL;

    int w = image->width > 0 ? (int)image->width : size;
    int h = image->height > 0 ? (int)image->height : size;

    /* Scale to fit within `size` while preserving aspect ratio */
    float scale = 1.0f;
    if (w > h) {
        if (w > size)
            scale = (float)size / (float)w;
    } else {
        if (h > size)
            scale = (float)size / (float)h;
    }
    int tw = (int)((float)w * scale);
    int th = (int)((float)h * scale);
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) {
        nsvgDelete(image);
        return NULL;
    }

    unsigned char *pixels = (unsigned char *)malloc((size_t)tw * th * 4);
    if (!pixels) {
        nsvgDeleteRasterizer(rast);
        nsvgDelete(image);
        return NULL;
    }

    nsvgRasterize(rast, image, 0, 0, scale, pixels, tw, th, tw * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);

    /* nsvgRasterize() outputs RGBA byte order; SDL_PIXELFORMAT_ABGR8888
     * is the matching native-endian mapping so opaque pixels keep their
     * alpha (RGBA8888 would be byte-swapped on little-endian, turning the
     * black controller outline fully transparent — BUG-0014). */
    SDL_Texture *tex = SDL_CreateTexture(renderer,
                                            SDL_PIXELFORMAT_ABGR8888,
                                            SDL_TEXTUREACCESS_STATIC,
                                            tw, th);
    if (!tex) {
        free(pixels);
        return NULL;
    }

    SDL_UpdateTexture(tex, NULL, pixels, tw * 4);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    free(pixels);

    return tex;
}

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                         */
/* ------------------------------------------------------------------ */

int
cbx_profile_diagram_init(cbx_profile_diagram *diag,
                           SDL_Renderer *renderer,
                           const char *svg_path,
                           const cbx_theme *theme)
{
    if (!diag)
        return -EINVAL;

    memset(diag, 0, sizeof(*diag));

    diag->base.vt = &s_diag_vt;
    diag->base.visible = true;
    diag->base.rect = (SDL_Rect){ 0, 0, 256, 256 };
    diag->highlighted = CBX_DIAG_BTN_NONE;
    diag->theme = theme;

    if (theme)
        diag->highlight_color = theme->focus;
    else {
        /* Default highlight: yellow-ish */
        diag->highlight_color = (SDL_Color){ 255, 200, 0, 180 };
    }

    /* Try to load the SVG base image */
    if (svg_path && renderer) {
        diag->base_texture = load_svg_texture(renderer, svg_path, 256);
        diag->owns_base_texture = (diag->base_texture != NULL);
    }

    return 0;
}

void
cbx_profile_diagram_shutdown(cbx_profile_diagram *diag)
{
    if (!diag)
        return;

    if (diag->owns_base_texture && diag->base_texture) {
        SDL_DestroyTexture(diag->base_texture);
        diag->base_texture = NULL;
    }
    diag->owns_base_texture = false;
    diag->highlighted = CBX_DIAG_BTN_NONE;

    memset(diag, 0, sizeof(*diag));
}

/* ------------------------------------------------------------------ */
/*  Highlight control                                                  */
/* ------------------------------------------------------------------ */

void
cbx_profile_diagram_highlight(cbx_profile_diagram *diag,
                                cbx_diag_button btn)
{
    if (!diag)
        return;
    if (btn < CBX_DIAG_BTN_NONE || btn >= CBX_DIAG_BTN_COUNT)
        return;
    diag->highlighted = btn;
}

void
cbx_profile_diagram_clear_highlight(cbx_profile_diagram *diag)
{
    if (!diag)
        return;
    diag->highlighted = CBX_DIAG_BTN_NONE;
}

cbx_diag_button
cbx_profile_diagram_get_highlight(const cbx_profile_diagram *diag)
{
    if (!diag)
        return CBX_DIAG_BTN_NONE;
    return diag->highlighted;
}

/* ------------------------------------------------------------------ */
/*  Button position lookup                                            */
/* ------------------------------------------------------------------ */

const cbx_diag_button_pos *
cbx_profile_diagram_get_button_pos(cbx_diag_button btn)
{
    if (btn < 0 || btn >= CBX_DIAG_BTN_COUNT)
        return NULL;
    return &s_button_pos[btn];
}

cbx_diag_button
cbx_profile_diagram_button_from_name(const char *name)
{
    if (!name)
        return CBX_DIAG_BTN_NONE;

    for (int i = 0; i < CBX_DIAG_BTN_COUNT; i++) {
        if (s_button_pos[i].name && strcmp(s_button_pos[i].name, name) == 0)
            return s_button_pos[i].id;
    }
    return CBX_DIAG_BTN_NONE;
}

const char *
cbx_profile_diagram_button_name(cbx_diag_button btn)
{
    if (btn < 0 || btn >= CBX_DIAG_BTN_COUNT)
        return NULL;
    return s_button_pos[btn].name;
}

int
cbx_profile_diagram_button_count(void)
{
    return CBX_DIAG_BTN_COUNT;
}

/* ------------------------------------------------------------------ */
/*  Widget vtable implementation                                      */
/* ------------------------------------------------------------------ */

static void
diag_draw(cbx_widget *w, SDL_Renderer *r)
{
    if (!w || !r)
        return;

    cbx_profile_diagram *diag = (cbx_profile_diagram *)w;
    SDL_Rect rect = w->rect;

    /* Draw background */
    if (diag->theme)
        SDL_SetRenderDrawColor(r,
            diag->theme->panel_bg.r, diag->theme->panel_bg.g,
            diag->theme->panel_bg.b, diag->theme->panel_bg.a);
    else
        SDL_SetRenderDrawColor(r, 30, 30, 30, 255);
    SDL_RenderFillRect(r, &rect);

    /* Draw base texture (if any) — fit within rect preserving aspect */
    if (diag->base_texture) {
        int tw, th;
        SDL_QueryTexture(diag->base_texture, NULL, NULL, &tw, &th);
        SDL_Rect dst = rect;
        /* Preserve aspect ratio: fit within rect */
        float scale = 1.0f;
        if (tw > 0 && th > 0) {
            float sx = (float)rect.w / (float)tw;
            float sy = (float)rect.h / (float)th;
            scale = sx < sy ? sx : sy;
        }
        dst.w = (int)((float)tw * scale);
        dst.h = (int)((float)th * scale);
        dst.x = rect.x + (rect.w - dst.w) / 2;
        dst.y = rect.y + (rect.h - dst.h) / 2;
        SDL_RenderCopy(r, diag->base_texture, NULL, &dst);
    }

    /* Draw highlight overlay for the highlighted button */
    if (diag->highlighted >= 0 && diag->highlighted < CBX_DIAG_BTN_COUNT) {
        const cbx_diag_button_pos *pos =
            &s_button_pos[diag->highlighted];
        if (pos && pos->name) {
            SDL_Rect hr;
            hr.x = rect.x + (int)(pos->x * (float)rect.w);
            hr.y = rect.y + (int)(pos->y * (float)rect.h);
            hr.w = (int)(pos->w * (float)rect.w);
            hr.h = (int)(pos->h * (float)rect.h);

            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r,
                diag->highlight_color.r, diag->highlight_color.g,
                diag->highlight_color.b, diag->highlight_color.a);
            SDL_RenderFillRect(r, &hr);
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        }
    }
}

static void
diag_get_rect(const cbx_widget *w, SDL_Rect *out)
{
    if (!w || !out)
        return;
    *out = w->rect;
}

static void
diag_set_rect(cbx_widget *w, const SDL_Rect *rect)
{
    if (!w || !rect)
        return;
    w->rect = *rect;
}

static void
diag_destroy(cbx_widget *w)
{
    if (!w)
        return;
    cbx_profile_diagram *diag = (cbx_profile_diagram *)w;
    if (diag->owns_base_texture && diag->base_texture) {
        SDL_DestroyTexture(diag->base_texture);
        diag->base_texture = NULL;
    }
    diag->owns_base_texture = false;
}