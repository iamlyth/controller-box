/*
 * widget_tabbar.c — Horizontal tab bar widget implementation.
 *
 * Left/Right switches the active tab, callback on change.  The active
 * tab is rendered with the accent color; inactive tabs use secondary
 * text color.  A horizontal underline highlights the active tab.
 *
 * Task 21 — List, Grid, TabBar, and ProgressBar widgets.
 */
#include "widget.h"

#include <errno.h>
#include <string.h>

/* --- vtable -------------------------------------------------------- */

static void
tabbar_draw(cbx_widget *w, SDL_Renderer *r)
{
    cbx_tabbar *tb = (cbx_tabbar *)w;
    if (!r)
        return;

    /* Background. */
    if (tb->theme) {
        SDL_SetRenderDrawColor(r, tb->theme->panel_bg.r,
                               tb->theme->panel_bg.g,
                               tb->theme->panel_bg.b,
                               tb->theme->panel_bg.a);
        SDL_RenderFillRect(r, &tb->base.rect);
    }

    if (tb->tab_count == 0)
        return;

    int tab_w = tb->base.rect.w / tb->tab_count;

    for (int i = 0; i < tb->tab_count; i++) {
        SDL_Rect tab_rect = {
            .x = tb->base.rect.x + i * tab_w,
            .y = tb->base.rect.y,
            .w = tab_w,
            .h = tb->base.rect.h,
        };

        /* Active tab background. */
        if (i == tb->active_tab && tb->theme) {
            SDL_SetRenderDrawColor(r, tb->theme->panel_bg_hover.r,
                                   tb->theme->panel_bg_hover.g,
                                   tb->theme->panel_bg_hover.b,
                                   tb->theme->panel_bg_hover.a);
            SDL_RenderFillRect(r, &tab_rect);
        }

        /* Draw label. */
        if (tb->text_cache && tb->tabs[i].label[0]) {
            SDL_Color col = (i == tb->active_tab)
                ? (tb->base.focused ? tb->theme->text_accent
                                     : tb->theme->text_primary)
                : tb->theme->text_secondary;
            SDL_Texture *tex = cbx_text_render(tb->text_cache,
                                                tb->font_id,
                                                tb->tabs[i].label, col);
            if (tex) {
                int tw, th;
                if (SDL_QueryTexture(tex, NULL, NULL, &tw, &th) == 0) {
                    SDL_Rect dst = {
                        .x = tab_rect.x + (tab_rect.w - tw) / 2,
                        .y = tab_rect.y + (tab_rect.h - th) / 2,
                        .w = tw,
                        .h = th,
                    };
                    SDL_RenderCopy(r, tex, NULL, &dst);
                }
            }
        }

        /* Underline for active tab. */
        if (i == tb->active_tab && tb->theme) {
            SDL_Rect underline = {
                .x = tab_rect.x,
                .y = tab_rect.y + tab_rect.h - 3,
                .w = tab_rect.w,
                .h = 3,
            };
            SDL_SetRenderDrawColor(r, tb->theme->text_accent.r,
                                   tb->theme->text_accent.g,
                                   tb->theme->text_accent.b,
                                   tb->theme->text_accent.a);
            SDL_RenderFillRect(r, &underline);
        }
    }

    /* Border. */
    if (tb->theme) {
        SDL_SetRenderDrawColor(r, tb->theme->border.r,
                               tb->theme->border.g,
                               tb->theme->border.b,
                               tb->theme->border.a);
        SDL_RenderDrawRect(r, &tb->base.rect);
    }
}

static bool
tabbar_handle_event(cbx_widget *w, const SDL_Event *ev)
{
    cbx_tabbar *tb = (cbx_tabbar *)w;
    if (!ev)
        return false;

    switch (ev->type) {
    case SDL_KEYDOWN:
        switch (ev->key.keysym.sym) {
        case SDLK_LEFT:
            cbx_tabbar_move_left(tb);  /* returns -1 at boundary but still consumed */
            return true;
        case SDLK_RIGHT:
            cbx_tabbar_move_right(tb);
            return true;
        case SDLK_RETURN:
        case SDLK_SPACE:
            return true;  /* consume but no action */
        default:
            break;
        }
        break;
    case SDL_MOUSEBUTTONDOWN:
        if (ev->button.button == SDL_BUTTON_LEFT && tb->tab_count > 0) {
            SDL_Point p = { ev->button.x, ev->button.y };
            if (SDL_PointInRect(&p, &tb->base.rect)) {
                int tab_w = tb->base.rect.w / tb->tab_count;
                if (tab_w <= 0)
                    return false;
                int clicked = (p.x - tb->base.rect.x) / tab_w;
                if (clicked >= 0 && clicked < tb->tab_count) {
                    cbx_tabbar_set_active(tb, clicked);
                    return true;
                }
            }
        }
        break;
    default:
        break;
    }
    return false;
}

static void
tabbar_focus(cbx_widget *w)
{
    cbx_tabbar *tb = (cbx_tabbar *)w;
    tb->base.focused = true;
}

static void
tabbar_blur(cbx_widget *w)
{
    cbx_tabbar *tb = (cbx_tabbar *)w;
    tb->base.focused = false;
}

static void
tabbar_get_rect(const cbx_widget *w, SDL_Rect *out)
{
    const cbx_tabbar *tb = (const cbx_tabbar *)w;
    *out = tb->base.rect;
}

static void
tabbar_set_rect(cbx_widget *w, const SDL_Rect *rect)
{
    cbx_tabbar *tb = (cbx_tabbar *)w;
    tb->base.rect = *rect;
}

static void
tabbar_destroy(cbx_widget *w)
{
    (void)w;
}

static const cbx_widget_vtable tabbar_vt = {
    .draw         = tabbar_draw,
    .handle_event = tabbar_handle_event,
    .focus        = tabbar_focus,
    .blur         = tabbar_blur,
    .get_rect     = tabbar_get_rect,
    .set_rect     = tabbar_set_rect,
    .destroy      = tabbar_destroy,
};

/* --- public API ---------------------------------------------------- */

int
cbx_tabbar_init(cbx_tabbar *tb, int font_id,
                 cbx_text_cache *cache, const cbx_theme *theme)
{
    if (!tb || !cache || !theme)
        return -EINVAL;
    memset(tb, 0, sizeof(*tb));
    tb->base.vt = &tabbar_vt;
    tb->base.visible = true;
    tb->base.interactive = true;
    tb->base.focused = false;
    tb->base.rect = (SDL_Rect){0, 0, 0, 0};
    tb->text_cache = cache;
    tb->theme = theme;
    tb->font_id = font_id;
    tb->tab_count = 0;
    tb->active_tab = -1;
    tb->on_change = NULL;
    return 0;
}

int
cbx_tabbar_add_tab(cbx_tabbar *tb, const char *label, void *user_data)
{
    if (!tb)
        return -EINVAL;
    if (tb->tab_count >= CBX_TABBAR_MAX_TABS)
        return -ENOMEM;
    cbx_tab *tab = &tb->tabs[tb->tab_count];
    memset(tab, 0, sizeof(*tab));
    if (label) {
        strncpy(tab->label, label, sizeof(tab->label) - 1);
        tab->label[sizeof(tab->label) - 1] = '\0';
    }
    tab->user_data = user_data;
    int idx = tb->tab_count++;
    if (tb->active_tab < 0)
        tb->active_tab = 0;
    return idx;
}

int
cbx_tabbar_tab_count(const cbx_tabbar *tb)
{
    return tb ? tb->tab_count : 0;
}

int
cbx_tabbar_get_active(const cbx_tabbar *tb)
{
    return tb ? tb->active_tab : -1;
}

void
cbx_tabbar_set_active(cbx_tabbar *tb, int index)
{
    if (!tb)
        return;
    if (index < 0 || index >= tb->tab_count)
        return;
    if (index == tb->active_tab)
        return;
    int old = tb->active_tab;
    tb->active_tab = index;
    if (tb->on_change)
        tb->on_change(&tb->base, index, tb->tabs[index].user_data);
    (void)old;
}

int
cbx_tabbar_move_left(cbx_tabbar *tb)
{
    if (!tb || tb->active_tab < 0)
        return -EINVAL;
    if (tb->active_tab > 0) {
        cbx_tabbar_set_active(tb, tb->active_tab - 1);
        return 0;
    }
    return -1;
}

int
cbx_tabbar_move_right(cbx_tabbar *tb)
{
    if (!tb || tb->active_tab < 0)
        return -EINVAL;
    if (tb->active_tab < tb->tab_count - 1) {
        cbx_tabbar_set_active(tb, tb->active_tab + 1);
        return 0;
    }
    return -1;
}

void
cbx_tabbar_set_change_cb(cbx_tabbar *tb, cbx_tabbar_change_cb cb)
{
    if (!tb)
        return;
    tb->on_change = cb;
}