/*
 * widget_label.c — Label widget implementation.
 *
 * Displays static text, optionally multi-line.  Text is rendered via the
 * shared text cache (textures are cached, not owned by the label).
 * Multi-line mode splits on '\n' and renders each line stacked.
 *
 * Task 20 — Widget base and concrete widgets.
 */
#include "widget.h"

#include <errno.h>
#include <string.h>

/* --- vtable -------------------------------------------------------- */

static void
label_draw(cbx_widget *w, SDL_Renderer *r)
{
    cbx_label *lbl = (cbx_label *)w;
    if (!r || !lbl->text_cache || lbl->text[0] == '\0')
        return;

    if (lbl->multiline) {
        /* Split on '\n' and render each line. */
        char buf[CBX_LABEL_TEXT_LEN];
        strncpy(buf, lbl->text, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        int y = lbl->base.rect.y;
        int line_h = cbx_text_line_height(lbl->text_cache, lbl->font_id);
        if (line_h <= 0)
            line_h = 16;   /* fallback */

        char *save = NULL;
        char *line = strtok_r(buf, "\n", &save);
        while (line) {
            SDL_Texture *tex = cbx_text_render(lbl->text_cache, lbl->font_id,
                                               line, lbl->color);
            if (tex) {
                int tw, th;
                if (SDL_QueryTexture(tex, NULL, NULL, &tw, &th) == 0) {
                    SDL_Rect dst = { lbl->base.rect.x, y, tw, th };
                    SDL_RenderCopy(r, tex, NULL, &dst);
                    y += line_h;
                }
            }
            line = strtok_r(NULL, "\n", &save);
        }
    } else {
        SDL_Texture *tex = cbx_text_render(lbl->text_cache, lbl->font_id,
                                           lbl->text, lbl->color);
        if (tex) {
            int tw, th;
            if (SDL_QueryTexture(tex, NULL, NULL, &tw, &th) == 0) {
                SDL_Rect dst = { lbl->base.rect.x, lbl->base.rect.y, tw, th };
                SDL_RenderCopy(r, tex, NULL, &dst);
            }
        }
    }
}

static bool
label_handle_event(cbx_widget *w, const SDL_Event *ev)
{
    (void)w;
    (void)ev;
    return false;   /* labels are not interactive */
}

static void
label_focus(cbx_widget *w)
{
    (void)w;   /* labels don't take focus */
}

static void
label_blur(cbx_widget *w)
{
    (void)w;
}

static void
label_get_rect(const cbx_widget *w, SDL_Rect *out)
{
    const cbx_label *lbl = (const cbx_label *)w;
    *out = lbl->base.rect;
}

static void
label_set_rect(cbx_widget *w, const SDL_Rect *rect)
{
    cbx_label *lbl = (cbx_label *)w;
    lbl->base.rect = *rect;
}

static void
label_destroy(cbx_widget *w)
{
    (void)w;   /* nothing to free — textures owned by text cache */
}

static const cbx_widget_vtable label_vt = {
    .draw         = label_draw,
    .handle_event = label_handle_event,
    .focus        = label_focus,
    .blur         = label_blur,
    .get_rect     = label_get_rect,
    .set_rect     = label_set_rect,
    .destroy      = label_destroy,
};

/* --- public API ---------------------------------------------------- */

int
cbx_label_init(cbx_label *lbl, const char *text, int font_id,
               cbx_text_cache *cache, const cbx_theme *theme)
{
    if (!lbl || !cache || !theme)
        return -EINVAL;
    memset(lbl, 0, sizeof(*lbl));
    lbl->base.vt = &label_vt;
    lbl->base.visible = true;
    lbl->base.interactive = false;
    lbl->base.focused = false;
    lbl->base.rect = (SDL_Rect){0, 0, 0, 0};
    lbl->text_cache = cache;
    lbl->theme = theme;
    lbl->font_id = font_id;
    lbl->color = theme->text_primary;
    lbl->multiline = false;
    if (text) {
        strncpy(lbl->text, text, sizeof(lbl->text) - 1);
        lbl->text[sizeof(lbl->text) - 1] = '\0';
    }
    return 0;
}

void
cbx_label_set_text(cbx_label *lbl, const char *text)
{
    if (!lbl)
        return;
    if (text) {
        strncpy(lbl->text, text, sizeof(lbl->text) - 1);
        lbl->text[sizeof(lbl->text) - 1] = '\0';
    } else {
        lbl->text[0] = '\0';
    }
}

void
cbx_label_set_color(cbx_label *lbl, SDL_Color color)
{
    if (!lbl)
        return;
    lbl->color = color;
}

void
cbx_label_set_multiline(cbx_label *lbl, bool multiline)
{
    if (!lbl)
        return;
    lbl->multiline = multiline;
}