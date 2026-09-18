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
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* nanosvg — vendored (Task 1), headers only (impl in nanosvg_impl.c) */
#include <nanosvg.h>
#include <nanosvgrast.h>

/*
 * Rasterisation resolution used for the base texture.  This is higher than
 * the largest diagram widget rect (300px) so the texture is down-scaled
 * (never up-scaled) to the on-screen content rect, avoiding the pixelated,
 * stretched look observed in BUG-0018.  Aspect ratio is preserved.
 */
#define CBX_DIAG_RASTER_SIZE 512

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

/*
 * Strict diagram catalog (BUG-0018).
 *
 * Public icon IDs, installed filenames and complete marker layouts are bound
 * explicitly here.  These initial coordinates are implementation geometry
 * pending exact human visual approval; they are deliberately not described as
 * human calibrated.  The renderer and markers share content_rect(), so the
 * normalized geometry receives exactly the SVG's aspect-fit transform.
 */
#define DEVICE_LAYOUT(name, du, dd, dl, dr, aa, ab, ax, ay, lsx, lsy, rsx, rsy) \
static const cbx_diag_button_pos name[CBX_DIAG_BTN_COUNT] = { \
 [CBX_DIAG_BTN_UP]={CBX_DIAG_BTN_UP,"Up",du,0.28f,0.065f,0.065f}, \
 [CBX_DIAG_BTN_DOWN]={CBX_DIAG_BTN_DOWN,"Down",dd,0.50f,0.065f,0.065f}, \
 [CBX_DIAG_BTN_LEFT]={CBX_DIAG_BTN_LEFT,"Left",dl,0.39f,0.065f,0.065f}, \
 [CBX_DIAG_BTN_RIGHT]={CBX_DIAG_BTN_RIGHT,"Right",dr,0.39f,0.065f,0.065f}, \
 [CBX_DIAG_BTN_A]={CBX_DIAG_BTN_A,"A",aa,0.39f,0.075f,0.075f}, \
 [CBX_DIAG_BTN_B]={CBX_DIAG_BTN_B,"B",ab,0.28f,0.075f,0.075f}, \
 [CBX_DIAG_BTN_X]={CBX_DIAG_BTN_X,"X",ax,0.28f,0.075f,0.075f}, \
 [CBX_DIAG_BTN_Y]={CBX_DIAG_BTN_Y,"Y",ay,0.17f,0.075f,0.075f}, \
 [CBX_DIAG_BTN_START]={CBX_DIAG_BTN_START,"Start",0.57f,0.29f,0.065f,0.05f}, \
 [CBX_DIAG_BTN_SELECT]={CBX_DIAG_BTN_SELECT,"Select",0.39f,0.29f,0.065f,0.05f}, \
 [CBX_DIAG_BTN_GUIDE]={CBX_DIAG_BTN_GUIDE,"Guide",0.47f,0.18f,0.07f,0.07f}, \
 [CBX_DIAG_BTN_L1]={CBX_DIAG_BTN_L1,"L1",0.16f,0.12f,0.12f,0.05f}, \
 [CBX_DIAG_BTN_R1]={CBX_DIAG_BTN_R1,"R1",0.72f,0.12f,0.12f,0.05f}, \
 [CBX_DIAG_BTN_L2]={CBX_DIAG_BTN_L2,"L2",0.12f,0.07f,0.12f,0.05f}, \
 [CBX_DIAG_BTN_R2]={CBX_DIAG_BTN_R2,"R2",0.76f,0.07f,0.12f,0.05f}, \
 [CBX_DIAG_BTN_L3]={CBX_DIAG_BTN_L3,"L3",lsx,lsy,0.10f,0.10f}, \
 [CBX_DIAG_BTN_R3]={CBX_DIAG_BTN_R3,"R3",rsx,rsy,0.10f,0.10f}, \
}

DEVICE_LAYOUT(s_xb360_pos, 0.31f,0.31f,0.25f,0.37f,
              0.76f,0.84f,0.68f,0.76f, 0.15f,0.28f,0.59f,0.46f);
DEVICE_LAYOUT(s_xbone_pos, 0.335f,0.335f,0.28f,0.39f,
              0.755f,0.825f,0.685f,0.755f, 0.19f,0.285f,0.585f,0.445f);
DEVICE_LAYOUT(s_series_pos, 0.34f,0.34f,0.285f,0.395f,
              0.72f,0.79f,0.65f,0.72f, 0.19f,0.29f,0.58f,0.44f);
DEVICE_LAYOUT(s_ps5_pos, 0.155f,0.155f,0.10f,0.21f,
              0.79f,0.86f,0.72f,0.79f, 0.285f,0.45f,0.61f,0.45f);
DEVICE_LAYOUT(s_deck_pos, 0.17f,0.17f,0.11f,0.23f,
              0.69f,0.76f,0.62f,0.69f, 0.20f,0.50f,0.62f,0.50f);

#undef DEVICE_LAYOUT

typedef struct {
    const char *icon;
    const char *asset_filename;
    const char *model_label;
    const cbx_diag_button_pos *table;
} cbx_diag_device_layout;

static const cbx_diag_device_layout s_device_layouts[] = {
    { "generic-gamepad", "generic-gamepad.svg", "Generic Gamepad", s_button_pos },
    { "cc-xbox-360", "xbox-360.svg", "Xbox 360 Controller", s_xb360_pos },
    { "cc-xbox-one", "xbox-one.svg", "Xbox One / Elite Controller", s_xbone_pos },
    { "cc-xbox-series-x", "xbox-series-x.svg", "Xbox Series S|X Controller", s_series_pos },
    { "cc-ps5", "ps5.svg", "DualSense", s_ps5_pos },
    { "cc-steam-deck", "steam-deck.svg", "Steam Deck Controller", s_deck_pos },
};

static const cbx_diag_device_layout *
catalog_for_icon(const char *icon_name)
{
    if (!icon_name || icon_name[0] == '\0')
        return NULL;
    for (size_t i = 0; i < sizeof(s_device_layouts) / sizeof(s_device_layouts[0]); i++)
        if (strcmp(s_device_layouts[i].icon, icon_name) == 0)
            return &s_device_layouts[i];
    return NULL;
}

static const cbx_diag_button_pos *
layout_for_icon(const char *icon_name)
{
    const cbx_diag_device_layout *entry = catalog_for_icon(icon_name);
    return entry ? entry->table : NULL;
}

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
    if (!renderer || !svg_path || size <= 0)
        return NULL;

    int fd = open(svg_path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return NULL;
    FILE *f = fdopen(fd, "rb");
    if (!f) {
        close(fd);
        return NULL;
    }

    /* Read file contents.  Treat positioning failures as a load failure;
     * parsing a partial/unknown-length asset could otherwise produce a
     * seemingly valid but blank production texture. */
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long fsize = ftell(f);
    if (fsize <= 0 || fsize > 4 * 1024 * 1024 ||
        fseek(f, 0, SEEK_SET) != 0) {  /* 4 MB max */
        fclose(f);
        return NULL;
    }

    char *buf = (char *)malloc((size_t)fsize + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t rd = fread(buf, 1, (size_t)fsize, f);
    int read_error = ferror(f);
    fclose(f);
    if (read_error || rd != (size_t)fsize) {
        free(buf);
        return NULL;
    }
    buf[rd] = '\0';

    /* nanosvg mutates its input — buf is malloc'd, so that's fine */
    NSVGimage *image = nsvgParse(buf, "px", 96.0f);
    free(buf);

    if (!image)
        return NULL;

    /* nanosvg accepts SVG dimensions as floats.  Reject non-finite or
     * unreasonably large values before converting to int; otherwise a
     * malformed installed asset could wrap the raster dimensions and either
     * produce a distorted diagram or overflow the pixel allocation. */
    if (!isfinite(image->width) || !isfinite(image->height) ||
        image->width < 1.0f || image->height < 1.0f ||
        image->width > 1000000.0f || image->height > 1000000.0f) {
        nsvgDelete(image);
        return NULL;
    }
    /* Keep the parsed floating-point dimensions until raster sizing.  The
     * SVG viewBox may describe a non-integer size; truncating it before
     * scaling can introduce an avoidable aspect-ratio error and shift the
     * fitted marker box by a pixel. */
    float source_w = image->width;
    float source_h = image->height;

    /* Scale to fit within `size` while preserving aspect ratio.  This scales
     * UP as well as down: a small source SVG (e.g. 100x60) is rasterised at
     * the requested resolution so the on-screen diagram is sharp and
     * aspect-correct rather than blown up from the native pixels (BUG-0018).
     * Round both axes together so rasterisation does not bias one dimension
     * through truncation. */
    float denom = source_w > source_h ? source_w : source_h;
    if (denom <= 0.0f)
        denom = 1.0f;
    float scale = (float)size / denom;
    float raster_w = source_w * scale;
    float raster_h = source_h * scale;
    /* Check the rounded dimensions before converting to int.  The parsed
     * dimensions are bounded above, but the caller-supplied raster size can
     * still be near INT_MAX; converting a non-finite or out-of-range float
     * first would make the subsequent allocation guards ineffective. */
    if (!isfinite(raster_w) || !isfinite(raster_h)) {
        nsvgDelete(image);
        return NULL;
    }
    /* Validate the rounded long result before narrowing it to int.  A
     * floating-point value just below INT_MAX can still round to INT_MAX;
     * checking the unrounded float alone is not sufficient and would make
     * the conversion implementation-defined on a malformed asset. */
    long rounded_w = lroundf(raster_w);
    long rounded_h = lroundf(raster_h);
    if (rounded_w < 1 || rounded_h < 1 ||
        rounded_w > INT_MAX || rounded_h > INT_MAX) {
        nsvgDelete(image);
        return NULL;
    }
    int tw = (int)rounded_w;
    int th = (int)rounded_h;

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) {
        nsvgDelete(image);
        return NULL;
    }

    /* Check the products before evaluating them.  The raster dimensions are
     * derived from an installed SVG and the requested size; a very large
     * caller-provided size must not wrap either the allocation or nanosvg's
     * int stride. */
    if (tw > INT_MAX / 4 || (size_t)th > SIZE_MAX / (size_t)tw) {
        nsvgDeleteRasterizer(rast);
        nsvgDelete(image);
        return NULL;
    }
    size_t pixel_count = (size_t)tw * (size_t)th;
    if (pixel_count > SIZE_MAX / 4) {
        nsvgDeleteRasterizer(rast);
        nsvgDelete(image);
        return NULL;
    }
    unsigned char *pixels = (unsigned char *)malloc(pixel_count * 4);
    if (!pixels) {
        nsvgDeleteRasterizer(rast);
        nsvgDelete(image);
        return NULL;
    }

    nsvgRasterize(rast, image, 0, 0, scale, pixels, tw, th, tw * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);

    /* A syntactically valid SVG can still rasterise to a completely
     * transparent image (for example, an empty or fully clipped installed
     * asset).  Do not expose that as a usable diagram: the manager would
     * otherwise render a blank panel while all texture-level checks pass.
     * nanosvg's output is RGBA, so alpha is byte 3 here, before the SDL
     * pixel-format conversion below. */
    bool has_visible_pixel = false;
    for (size_t i = 3; i < pixel_count * 4; i += 4) {
        if (pixels[i] != 0) {
            has_visible_pixel = true;
            break;
        }
    }
    if (!has_visible_pixel) {
        free(pixels);
        return NULL;
    }

    /* nsvgRasterize() outputs RGBA byte order.  SDL's packed pixel formats
     * describe the native-endian word, so ABGR8888 is the matching format on
     * the little-endian x86_64/aarch64 targets (RGBA8888 would be laid out as
     * A,B,G,R there, turning the opaque outline fully transparent — BUG-0014).
     * Keep the big-endian branch correct as well for portability. */
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
    const Uint32 pixel_format = SDL_PIXELFORMAT_ABGR8888;
#else
    const Uint32 pixel_format = SDL_PIXELFORMAT_RGBA8888;
#endif
    SDL_Texture *tex = SDL_CreateTexture(renderer,
                                            pixel_format,
                                            SDL_TEXTUREACCESS_STATIC,
                                            tw, th);
    if (!tex) {
        free(pixels);
        return NULL;
    }

    /* Treat an upload failure as a load failure.  Returning a non-NULL
     * texture after SDL_UpdateTexture() fails would make callers believe the
     * installed diagram is available while rendering an uninitialised/blank
     * texture, defeating the perceptible-content acceptance. */
    if (SDL_UpdateTexture(tex, NULL, pixels, tw * 4) != 0) {
        SDL_DestroyTexture(tex);
        free(pixels);
        return NULL;
    }
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
    diag->btn_table = s_button_pos;
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
        diag->base_texture = load_svg_texture(renderer, svg_path,
                                              CBX_DIAG_RASTER_SIZE);
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
/*  Device-mapped base image & marker layout (BUG-0018)               */
/* ------------------------------------------------------------------ */

int
cbx_profile_diagram_apply_selection(cbx_profile_diagram *diag,
                                     SDL_Texture *tex,
                                     const char *icon_name,
                                     cbx_diag_provenance provenance)
{
    if (!diag)
        return -EINVAL;
    const cbx_diag_device_layout *entry = catalog_for_icon(icon_name);
    if (!entry)
        return -ENOENT;

    if (diag->owns_base_texture && diag->base_texture &&
        diag->base_texture != tex)
        SDL_DestroyTexture(diag->base_texture);
    diag->base_texture = tex;
    diag->owns_base_texture = false;
    diag->btn_table = tex ? entry->table : NULL;
    snprintf(diag->resolved_icon, sizeof(diag->resolved_icon), "%s", entry->icon);
    snprintf(diag->asset_filename, sizeof(diag->asset_filename), "%s",
             entry->asset_filename);
    snprintf(diag->model_label, sizeof(diag->model_label), "%s",
             entry->model_label);
    diag->provenance = provenance;
    return tex ? 0 : -ENOENT;
}

void
cbx_profile_diagram_set_base_image(cbx_profile_diagram *diag,
                                     SDL_Texture *tex)
{
    if (!diag)
        return;
    if (tex == diag->base_texture)
        return;
    if (diag->owns_base_texture && diag->base_texture)
        SDL_DestroyTexture(diag->base_texture);
    diag->base_texture = tex;
    diag->owns_base_texture = false;
    if (!tex)
        diag->btn_table = NULL;
}

void
cbx_profile_diagram_set_device(cbx_profile_diagram *diag,
                                 const char *icon_name)
{
    if (!diag)
        return;
    diag->btn_table = layout_for_icon(icon_name);
}

bool
cbx_profile_diagram_device_geometry_known(const char *icon_name)
{
    return catalog_for_icon((!icon_name || !icon_name[0])
                            ? "generic-gamepad" : icon_name) != NULL;
}

bool
cbx_profile_diagram_catalog_asset(const char *icon_name,
                                   const char **asset_filename,
                                   const char **model_label)
{
    const cbx_diag_device_layout *entry = catalog_for_icon(icon_name);
    if (!entry)
        return false;
    if (asset_filename) *asset_filename = entry->asset_filename;
    if (model_label) *model_label = entry->model_label;
    return true;
}

bool
cbx_profile_diagram_catalog_valid(void)
{
    for (size_t i = 0; i < sizeof(s_device_layouts) / sizeof(s_device_layouts[0]); i++) {
        const cbx_diag_device_layout *entry = &s_device_layouts[i];
        if (!entry->icon || !entry->asset_filename || !entry->model_label ||
            !entry->table || !entry->icon[0] || !entry->asset_filename[0])
            return false;
        for (int b = 0; b < CBX_DIAG_BTN_COUNT; b++) {
            const cbx_diag_button_pos *p = &entry->table[b];
            if (p->id != b || !p->name || p->x < 0.0f || p->y < 0.0f ||
                p->w <= 0.0f || p->h <= 0.0f || p->x + p->w > 1.0f ||
                p->y + p->h > 1.0f)
                return false;
        }
    }
    return true;
}

const char *cbx_profile_diagram_resolved_icon(const cbx_profile_diagram *diag)
{ return diag ? diag->resolved_icon : NULL; }
const char *cbx_profile_diagram_asset_filename(const cbx_profile_diagram *diag)
{ return diag ? diag->asset_filename : NULL; }
const char *cbx_profile_diagram_model_label(const cbx_profile_diagram *diag)
{ return diag ? diag->model_label : NULL; }
cbx_diag_provenance cbx_profile_diagram_provenance(const cbx_profile_diagram *diag)
{ return diag ? diag->provenance : CBX_DIAG_PROVENANCE_NONE; }
const char *cbx_profile_diagram_provenance_name(cbx_diag_provenance p)
{
    switch (p) {
    case CBX_DIAG_PROVENANCE_SUPPORTED_MODEL: return "supported-model";
    case CBX_DIAG_PROVENANCE_PROFILE_OVERRIDE: return "profile-override";
    case CBX_DIAG_PROVENANCE_EXPLICIT_GENERIC: return "explicit-generic";
    case CBX_DIAG_PROVENANCE_UNSUPPORTED_FALLBACK: return "unsupported-fallback";
    case CBX_DIAG_PROVENANCE_SUPPORTED_LOAD_FAILURE: return "supported-load-failure";
    default: return "none";
    }
}

const cbx_diag_button_pos *
cbx_profile_diagram_active_button_pos(const cbx_profile_diagram *diag,
                                        cbx_diag_button btn)
{
    if (btn < 0 || btn >= CBX_DIAG_BTN_COUNT)
        return NULL;
    const cbx_diag_button_pos *table = diag ? diag->btn_table : s_button_pos;
    return table ? &table[btn] : NULL;
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
/*  Geometry helpers                                                   */
/* ------------------------------------------------------------------ */

bool
cbx_profile_diagram_content_rect(const cbx_profile_diagram *diag,
                                  const SDL_Rect *rect,
                                  SDL_Rect *out)
{
    if (!diag || !diag->base_texture || !rect || !out)
        return false;
    if (rect->w <= 0 || rect->h <= 0)
        return false;

    int tw, th;
    if (SDL_QueryTexture(diag->base_texture, NULL, NULL, &tw, &th) != 0)
        return false;
    if (tw <= 0 || th <= 0)
        return false;

    /* Preserve aspect ratio: fit within rect, centred. */
    float sx = (float)rect->w / (float)tw;
    float sy = (float)rect->h / (float)th;
    float scale = sx < sy ? sx : sy;
    SDL_Rect dst = *rect;
    /* Round to nearest pixel instead of truncating so the fitted box
     * preserves the texture's aspect ratio as closely as the raster
     * resolution allows (BUG-0018): a 512x307 texture in a 300x180 box
     * must yield 300x180, not 300x179, or the highlight/marker overlay
     * is offset by a row and the diagram looks stretched. */
    dst.w = (int)((float)tw * scale + 0.5f);
    dst.h = (int)((float)th * scale + 0.5f);
    /* Rounding must never let the fitted texture escape its widget.  This
     * matters for dimensions whose ideal scale lands within half a pixel of
     * an edge: an escaped row/column would both clip the SVG and invalidate
     * the marker/content-box correspondence. */
    if (dst.w < 1) dst.w = 1;
    if (dst.h < 1) dst.h = 1;
    if (dst.w > rect->w) dst.w = rect->w;
    if (dst.h > rect->h) dst.h = rect->h;
    dst.x = rect->x + (rect->w - dst.w) / 2;
    dst.y = rect->y + (rect->h - dst.h) / 2;
    *out = dst;
    return true;
}

bool
cbx_profile_diagram_base_texture_size(const cbx_profile_diagram *diag,
                                       int *w, int *h)
{
    if (!diag || !diag->base_texture || !w || !h)
        return false;
    if (SDL_QueryTexture(diag->base_texture, NULL, NULL, w, h) != 0)
        return false;
    return (*w > 0 && *h > 0);
}

/* ------------------------------------------------------------------ */
/*  Widget vtable implementation                                      */
/* ------------------------------------------------------------------ */

static void
_diag_draw_content(cbx_profile_diagram *diag, SDL_Renderer *r,
                   const SDL_Rect *rect, SDL_Rect *content_out)
{
    SDL_Rect content = *rect;
    bool base_rendered = false;
    if (diag->base_texture) {
        SDL_Rect dst;
        if (cbx_profile_diagram_content_rect(diag, rect, &dst) &&
            SDL_RenderCopy(r, diag->base_texture, NULL, &dst) == 0) {
            content = dst;
            base_rendered = true;
        }
    }
    if (content_out) {
        /* A headless diagram intentionally has no base texture; retain the
         * historical full-widget marker behavior for that mode.  A texture
         * that exists but cannot be rendered is different: suppress its
         * marker so a failed installed asset cannot masquerade as a
         * perceptible diagram. */
        if (base_rendered || !diag->base_texture)
            *content_out = content;
        else
            *content_out = (SDL_Rect){0, 0, 0, 0};
    }
}

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

    /* Draw base texture (if any) and obtain the aspect-fitted content box. */
    SDL_Rect content;
    _diag_draw_content(diag, r, &rect, &content);

    /* Highlight overlay is anchored inside the same content box as the
     * rendered controller, so a mapped-button marker lands on the control
     * it represents regardless of the widget rect's aspect ratio (BUG-0018).
     * Without a base texture (headless tests) the content box is the full
     * widget rect, preserving the prior behaviour. */
    if (diag->highlighted >= 0 && diag->highlighted < CBX_DIAG_BTN_COUNT) {
        const cbx_diag_button_pos *pos =
            cbx_profile_diagram_active_button_pos(diag, diag->highlighted);
        if (pos && pos->name) {
            SDL_Rect hr;
            hr.x = content.x + (int)(pos->x * (float)content.w);
            hr.y = content.y + (int)(pos->y * (float)content.h);
            hr.w = (int)(pos->w * (float)content.w);
            hr.h = (int)(pos->h * (float)content.h);
            if (hr.w < 1) hr.w = 1;
            if (hr.h < 1) hr.h = 1;

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