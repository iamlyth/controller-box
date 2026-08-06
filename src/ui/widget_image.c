/*
 * widget_image.c — Image widget implementation.
 *
 * Renders an SDL_Texture with three scaling modes: fit (preserve aspect),
 * fill (stretch), and center (1:1).  Optionally owns the texture.
 *
 * Task 20 — Widget base and concrete widgets.
 */
#include "widget.h"

#include <errno.h>

/* --- vtable -------------------------------------------------------- */

static void
image_draw(cbx_widget *w, SDL_Renderer *r)
{
    cbx_image *img = (cbx_image *)w;
    if (!r || !img->texture)
        return;

    SDL_Rect dst = img->base.rect;

    switch (img->scale_mode) {
    case CBX_IMAGE_SCALE_FILL:
        /* Stretch to fill — dst is already the full rect. */
        break;
    case CBX_IMAGE_SCALE_CENTER:
        /* 1:1 centred, but don't overflow rect. */
        dst.w = img->tex_w;
        dst.h = img->tex_h;
        dst.x = img->base.rect.x + (img->base.rect.w - img->tex_w) / 2;
        dst.y = img->base.rect.y + (img->base.rect.h - img->tex_h) / 2;
        break;
    case CBX_IMAGE_SCALE_FIT:
    default: {
        /* Preserve aspect ratio, fit within rect. */
        if (img->tex_w <= 0 || img->tex_h <= 0)
            return;
        float scale_x = (float)img->base.rect.w / img->tex_w;
        float scale_y = (float)img->base.rect.h / img->tex_h;
        float scale = scale_x < scale_y ? scale_x : scale_y;
        dst.w = (int)(img->tex_w * scale);
        dst.h = (int)(img->tex_h * scale);
        dst.x = img->base.rect.x + (img->base.rect.w - dst.w) / 2;
        dst.y = img->base.rect.y + (img->base.rect.h - dst.h) / 2;
        break;
    }
    }

    SDL_RenderCopy(r, img->texture, NULL, &dst);
}

static bool
image_handle_event(cbx_widget *w, const SDL_Event *ev)
{
    (void)w;
    (void)ev;
    return false;
}

static void
image_focus(cbx_widget *w)
{
    (void)w;
}

static void
image_blur(cbx_widget *w)
{
    (void)w;
}

static void
image_get_rect(const cbx_widget *w, SDL_Rect *out)
{
    const cbx_image *img = (const cbx_image *)w;
    *out = img->base.rect;
}

static void
image_set_rect(cbx_widget *w, const SDL_Rect *rect)
{
    cbx_image *img = (cbx_image *)w;
    img->base.rect = *rect;
}

static void
image_destroy(cbx_widget *w)
{
    cbx_image *img = (cbx_image *)w;
    if (img->owns_texture && img->texture) {
        SDL_DestroyTexture(img->texture);
        img->texture = NULL;
    }
}

static const cbx_widget_vtable image_vt = {
    .draw         = image_draw,
    .handle_event = image_handle_event,
    .focus        = image_focus,
    .blur         = image_blur,
    .get_rect     = image_get_rect,
    .set_rect     = image_set_rect,
    .destroy      = image_destroy,
};

/* --- public API ---------------------------------------------------- */

int
cbx_image_init(cbx_image *img, SDL_Texture *texture, bool owns_texture)
{
    if (!img)
        return -EINVAL;
    memset(img, 0, sizeof(*img));
    img->base.vt = &image_vt;
    img->base.visible = true;
    img->base.focused = false;
    img->base.rect = (SDL_Rect){0, 0, 0, 0};
    img->texture = texture;
    img->owns_texture = owns_texture;
    img->scale_mode = CBX_IMAGE_SCALE_FIT;

    if (texture) {
        if (SDL_QueryTexture(texture, NULL, NULL,
                              &img->tex_w, &img->tex_h) != 0) {
            img->tex_w = 0;
            img->tex_h = 0;
        }
    }
    return 0;
}

void
cbx_image_set_texture(cbx_image *img, SDL_Texture *texture,
                       bool owns_texture)
{
    if (!img)
        return;
    /* Free previous owned texture. */
    if (img->owns_texture && img->texture)
        SDL_DestroyTexture(img->texture);
    img->texture = texture;
    img->owns_texture = owns_texture;
    if (texture) {
        if (SDL_QueryTexture(texture, NULL, NULL,
                              &img->tex_w, &img->tex_h) != 0) {
            img->tex_w = 0;
            img->tex_h = 0;
        }
    } else {
        img->tex_w = 0;
        img->tex_h = 0;
    }
}

void
cbx_image_set_scale_mode(cbx_image *img, cbx_image_scale_mode mode)
{
    if (!img)
        return;
    img->scale_mode = mode;
}

int
cbx_image_get_natural_dims(const cbx_image *img, int *w, int *h)
{
    if (!img)
        return -EINVAL;
    if (w) *w = img->tex_w;
    if (h) *h = img->tex_h;
    return 0;
}