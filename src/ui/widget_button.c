/*
 * widget_button.c — Button widget implementation.
 *
 * Visual states: normal, focused (brighter bg + focus border), pressed
 * (accent-tinted bg).  Handles mouse click and keyboard Return/Space.
 * Label texture is borrowed from the text cache (not owned by the button).
 *
 * Task 20 — Widget base and concrete widgets.
 */
#include "widget.h"

#include <errno.h>
#include <string.h>

/* --- helpers ------------------------------------------------------- */

static void
render_label(cbx_button *btn)
{
    if (!btn->text_cache || btn->label[0] == '\0') {
        btn->label_tex = NULL;
        btn->label_w = 0;
        btn->label_h = 0;
        return;
    }
    SDL_Color col = btn->base.focused
        ? btn->theme->text_accent
        : btn->theme->text_primary;
    SDL_Texture *tex = cbx_text_render(btn->text_cache, btn->font_id,
                                       btn->label, col);
    if (tex) {
        btn->label_tex = tex;
        int w, h;
        if (SDL_QueryTexture(tex, NULL, NULL, &w, &h) == 0) {
            btn->label_w = w;
            btn->label_h = h;
        }
    }
}

/* --- vtable -------------------------------------------------------- */

static void
button_draw(cbx_widget *w, SDL_Renderer *r)
{
    cbx_button *btn = (cbx_button *)w;
    if (!r)
        return;

    SDL_Color bg = btn->base.focused
        ? btn->theme->panel_bg_hover
        : btn->theme->panel_bg;
    if (btn->pressed)
        bg = btn->theme->text_accent;   /* pressed = accent-tinted */

    SDL_Color border = btn->base.focused
        ? btn->theme->border_focus
        : btn->theme->border;

    /* Fill background. */
    SDL_SetRenderDrawColor(r, bg.r, bg.g, bg.b, bg.a);
    SDL_RenderFillRect(r, &btn->base.rect);

    /* Draw border (4 lines — SDL_RenderDrawRect is 1px, we draw manually). */
    SDL_SetRenderDrawColor(r, border.r, border.g, border.b, border.a);
    SDL_RenderDrawRect(r, &btn->base.rect);

    /* Draw label centred. */
    if (btn->label_tex) {
        SDL_Rect dst;
        dst.w = btn->label_w;
        dst.h = btn->label_h;
        dst.x = btn->base.rect.x + (btn->base.rect.w - dst.w) / 2;
        dst.y = btn->base.rect.y + (btn->base.rect.h - dst.h) / 2;
        /* Clamp to rect — don't overflow if label is wider than button. */
        if (dst.x < btn->base.rect.x)
            dst.x = btn->base.rect.x;
        if (dst.y < btn->base.rect.y)
            dst.y = btn->base.rect.y;
        SDL_RenderCopy(r, btn->label_tex, NULL, &dst);
    }
}

static bool
button_handle_event(cbx_widget *w, const SDL_Event *ev)
{
    cbx_button *btn = (cbx_button *)w;
    if (!ev)
        return false;

    switch (ev->type) {
    case SDL_MOUSEBUTTONDOWN:
        if (ev->button.button == SDL_BUTTON_LEFT) {
            SDL_Point p = { ev->button.x, ev->button.y };
            if (SDL_PointInRect(&p, &btn->base.rect)) {
                btn->pressed = true;
                return true;
            }
        }
        break;
    case SDL_MOUSEBUTTONUP:
        if (ev->button.button == SDL_BUTTON_LEFT && btn->pressed) {
            SDL_Point p = { ev->button.x, ev->button.y };
            bool in_rect = SDL_PointInRect(&p, &btn->base.rect);
            btn->pressed = false;
            if (in_rect && btn->on_press) {
                btn->on_press(&btn->base, btn->user_data);
            }
            return true;
        }
        break;
    case SDL_KEYDOWN:
        if (ev->key.keysym.sym == SDLK_RETURN ||
            ev->key.keysym.sym == SDLK_SPACE) {
            btn->pressed = true;
            return true;
        }
        break;
    case SDL_KEYUP:
        if (ev->key.keysym.sym == SDLK_RETURN ||
            ev->key.keysym.sym == SDLK_SPACE) {
            if (btn->pressed) {
                btn->pressed = false;
                if (btn->on_press)
                    btn->on_press(&btn->base, btn->user_data);
                return true;
            }
        }
        break;
    default:
        break;
    }
    return false;
}

static void
button_focus(cbx_widget *w)
{
    cbx_button *btn = (cbx_button *)w;
    btn->base.focused = true;
    render_label(btn);  /* re-render with focus colour */
}

static void
button_blur(cbx_widget *w)
{
    cbx_button *btn = (cbx_button *)w;
    btn->base.focused = false;
    btn->pressed = false;
    render_label(btn);  /* re-render with normal colour */
}

static void
button_get_rect(const cbx_widget *w, SDL_Rect *out)
{
    const cbx_button *btn = (const cbx_button *)w;
    *out = btn->base.rect;
}

static void
button_set_rect(cbx_widget *w, const SDL_Rect *rect)
{
    cbx_button *btn = (cbx_button *)w;
    btn->base.rect = *rect;
}

static void
button_destroy(cbx_widget *w)
{
    /* Nothing to free — label_tex is owned by the text cache. */
    (void)w;
}

static const cbx_widget_vtable button_vt = {
    .draw         = button_draw,
    .handle_event = button_handle_event,
    .focus        = button_focus,
    .blur         = button_blur,
    .get_rect     = button_get_rect,
    .set_rect     = button_set_rect,
    .destroy      = button_destroy,
};

/* --- public API ---------------------------------------------------- */

int
cbx_button_init(cbx_button *btn, const char *label, int font_id,
                cbx_text_cache *cache, const cbx_theme *theme,
                cbx_button_press_cb cb, void *user_data)
{
    if (!btn || !cache || !theme)
        return -EINVAL;
    memset(btn, 0, sizeof(*btn));
    btn->base.vt = &button_vt;
    btn->base.visible = true;
    btn->base.focused = false;
    btn->base.rect = (SDL_Rect){0, 0, 0, 0};
    btn->text_cache = cache;
    btn->theme = theme;
    btn->font_id = font_id;
    btn->on_press = cb;
    btn->user_data = user_data;
    btn->pressed = false;
    if (label) {
        strncpy(btn->label, label, sizeof(btn->label) - 1);
        btn->label[sizeof(btn->label) - 1] = '\0';
        render_label(btn);
    }
    return 0;
}

void
cbx_button_set_label(cbx_button *btn, const char *label)
{
    if (!btn)
        return;
    if (label) {
        strncpy(btn->label, label, sizeof(btn->label) - 1);
        btn->label[sizeof(btn->label) - 1] = '\0';
    } else {
        btn->label[0] = '\0';
    }
    render_label(btn);
}

void
cbx_button_set_press_cb(cbx_button *btn, cbx_button_press_cb cb,
                         void *user_data)
{
    if (!btn)
        return;
    btn->on_press = cb;
    btn->user_data = user_data;
}

void
cbx_button_set_pressed(cbx_button *btn, bool pressed)
{
    if (!btn)
        return;
    btn->pressed = pressed;
}