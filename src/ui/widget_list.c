/*
 * widget_list.c — Scrollable list widget implementation.
 *
 * Up/down navigation, highlight, optional icon per item.  Handles
 * keyboard Up/Down and mouse wheel for scrolling.  Activating an item
 * (Return/Space) fires the select callback.
 *
 * Task 21 — List, Grid, TabBar, and ProgressBar widgets.
 */
#include "widget.h"

#include <errno.h>
#include <string.h>

#define DEFAULT_ITEM_H 32
#define DEFAULT_ICON_SIZE 24

/* --- helpers ------------------------------------------------------- */

static void
compute_visible(cbx_list *lst)
{
    if (lst->item_h <= 0 || lst->base.rect.h <= 0) {
        lst->visible_count = 0;
        return;
    }
    lst->visible_count = lst->base.rect.h / lst->item_h;
    if (lst->visible_count < 1)
        lst->visible_count = 1;
}

static void
ensure_scroll_visible(cbx_list *lst)
{
    if (lst->selected < 0)
        return;
    if (lst->selected < lst->scroll_offset)
        lst->scroll_offset = lst->selected;
    else if (lst->selected >= lst->scroll_offset + lst->visible_count)
        lst->scroll_offset = lst->selected - lst->visible_count + 1;
    if (lst->scroll_offset < 0)
        lst->scroll_offset = 0;
}

/* --- vtable -------------------------------------------------------- */

static void
list_draw(cbx_widget *w, SDL_Renderer *r)
{
    cbx_list *lst = (cbx_list *)w;
    if (!r)
        return;
    compute_visible(lst);

    /* Background. */
    if (lst->theme) {
        SDL_SetRenderDrawColor(r, lst->theme->panel_bg.r,
                               lst->theme->panel_bg.g,
                               lst->theme->panel_bg.b,
                               lst->theme->panel_bg.a);
        SDL_RenderFillRect(r, &lst->base.rect);
    }

    int y = lst->base.rect.y;
    int max = lst->scroll_offset + lst->visible_count;
    if (max > lst->item_count)
        max = lst->item_count;

    for (int i = lst->scroll_offset; i < max; i++) {
        SDL_Rect row = {
            .x = lst->base.rect.x,
            .y = y,
            .w = lst->base.rect.w,
            .h = lst->item_h,
        };

        /* Highlight selected row. */
        if (i == lst->selected && lst->theme) {
            SDL_SetRenderDrawColor(r, lst->theme->panel_bg_hover.r,
                                   lst->theme->panel_bg_hover.g,
                                   lst->theme->panel_bg_hover.b,
                                   lst->theme->panel_bg_hover.a);
            SDL_RenderFillRect(r, &row);
            /* Focus border. */
            if (lst->base.focused) {
                SDL_SetRenderDrawColor(r, lst->theme->border_focus.r,
                                       lst->theme->border_focus.g,
                                       lst->theme->border_focus.b,
                                       lst->theme->border_focus.a);
                SDL_RenderDrawRect(r, &row);
            }
        }

        /* Draw icon if present. */
        int text_x = row.x + 4;
        if (lst->items[i].icon) {
            SDL_Rect icon_dst = {
                .x = row.x + 4,
                .y = row.y + (row.h - lst->icon_size) / 2,
                .w = lst->icon_size,
                .h = lst->icon_size,
            };
            SDL_RenderCopy(r, lst->items[i].icon, NULL, &icon_dst);
            text_x = icon_dst.x + icon_dst.w + 6;
        }

        /* Draw label. */
        if (lst->text_cache && lst->items[i].label[0]) {
            SDL_Color col = (i == lst->selected && lst->base.focused)
                ? lst->theme->text_accent
                : lst->theme->text_primary;
            SDL_Texture *tex = cbx_text_render(lst->text_cache,
                                               lst->font_id,
                                               lst->items[i].label, col);
            if (tex) {
                int tw, th;
                if (SDL_QueryTexture(tex, NULL, NULL, &tw, &th) == 0) {
                    SDL_Rect dst = {
                        .x = text_x,
                        .y = row.y + (row.h - th) / 2,
                        .w = tw,
                        .h = th,
                    };
                    SDL_RenderCopy(r, tex, NULL, &dst);
                }
            }
        }

        y += lst->item_h;
    }

    /* Border. */
    if (lst->theme) {
        SDL_SetRenderDrawColor(r, lst->theme->border.r,
                               lst->theme->border.g,
                               lst->theme->border.b,
                               lst->theme->border.a);
        SDL_RenderDrawRect(r, &lst->base.rect);
    }
}

static bool
list_handle_event(cbx_widget *w, const SDL_Event *ev)
{
    cbx_list *lst = (cbx_list *)w;
    if (!ev)
        return false;

    switch (ev->type) {
    case SDL_KEYDOWN:
        switch (ev->key.keysym.sym) {
        case SDLK_UP:
            if (lst->selected > 0) {
                lst->selected--;
                ensure_scroll_visible(lst);
                return true;
            }
            return false;  /* at top boundary: let focus chain navigate */
        case SDLK_DOWN:
            if (lst->selected < lst->item_count - 1) {
                lst->selected++;
                ensure_scroll_visible(lst);
                return true;
            }
            return false;  /* at bottom boundary: let focus chain navigate */
        case SDLK_RETURN:
        case SDLK_SPACE:
            if (lst->on_select && lst->selected >= 0) {
                lst->on_select(&lst->base, lst->selected,
                              lst->items[lst->selected].user_data);
            }
            return true;
        default:
            break;
        }
        break;
    case SDL_MOUSEWHEEL:
        /* SDL: wheel.y > 0 = scroll up (earlier items), < 0 = scroll down. */
        lst->scroll_offset -= ev->wheel.y;
        if (lst->scroll_offset < 0)
            lst->scroll_offset = 0;
        if (lst->scroll_offset > lst->item_count - lst->visible_count)
            lst->scroll_offset = lst->item_count - lst->visible_count;
        return true;
    case SDL_MOUSEBUTTONDOWN:
        if (ev->button.button == SDL_BUTTON_LEFT) {
            SDL_Point p = { ev->button.x, ev->button.y };
            if (SDL_PointInRect(&p, &lst->base.rect)) {
                int rel_y = p.y - lst->base.rect.y;
                int idx = lst->scroll_offset + rel_y / lst->item_h;
                if (idx >= 0 && idx < lst->item_count) {
                    lst->selected = idx;
                    if (lst->on_select)
                        lst->on_select(&lst->base, idx,
                                        lst->items[idx].user_data);
                }
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
list_focus(cbx_widget *w)
{
    cbx_list *lst = (cbx_list *)w;
    lst->base.focused = true;
    if (lst->selected < 0 && lst->item_count > 0)
        lst->selected = 0;
}

static void
list_blur(cbx_widget *w)
{
    cbx_list *lst = (cbx_list *)w;
    lst->base.focused = false;
}

static void
list_get_rect(const cbx_widget *w, SDL_Rect *out)
{
    const cbx_list *lst = (const cbx_list *)w;
    *out = lst->base.rect;
}

static void
list_set_rect(cbx_widget *w, const SDL_Rect *rect)
{
    cbx_list *lst = (cbx_list *)w;
    lst->base.rect = *rect;
    compute_visible(lst);
}

static void
list_destroy(cbx_widget *w)
{
    (void)w;
    /* Icons are borrowed — not freed. */
}

static const cbx_widget_vtable list_vt = {
    .draw         = list_draw,
    .handle_event = list_handle_event,
    .focus        = list_focus,
    .blur         = list_blur,
    .get_rect     = list_get_rect,
    .set_rect     = list_set_rect,
    .destroy      = list_destroy,
};

/* --- public API ---------------------------------------------------- */

int
cbx_list_init(cbx_list *lst, int font_id,
               cbx_text_cache *cache, const cbx_theme *theme)
{
    if (!lst || !cache || !theme)
        return -EINVAL;
    memset(lst, 0, sizeof(*lst));
    lst->base.vt = &list_vt;
    lst->base.visible = true;
    lst->base.focused = false;
    lst->base.rect = (SDL_Rect){0, 0, 0, 0};
    lst->text_cache = cache;
    lst->theme = theme;
    lst->font_id = font_id;
    lst->item_count = 0;
    lst->selected = -1;
    lst->scroll_offset = 0;
    lst->visible_count = 0;
    lst->item_h = DEFAULT_ITEM_H;
    lst->icon_size = DEFAULT_ICON_SIZE;
    lst->on_select = NULL;
    return 0;
}

int
cbx_list_add_item(cbx_list *lst, const char *label,
                   SDL_Texture *icon, void *user_data)
{
    if (!lst)
        return -EINVAL;
    if (lst->item_count >= CBX_LIST_MAX_ITEMS)
        return -ENOMEM;
    cbx_list_item *item = &lst->items[lst->item_count];
    memset(item, 0, sizeof(*item));
    if (label) {
        strncpy(item->label, label, sizeof(item->label) - 1);
        item->label[sizeof(item->label) - 1] = '\0';
    }
    item->icon = icon;
    item->user_data = user_data;
    lst->item_count++;
    if (lst->selected < 0)
        lst->selected = 0;
    return lst->item_count - 1;
}

void
cbx_list_clear(cbx_list *lst)
{
    if (!lst)
        return;
    lst->item_count = 0;
    lst->selected = -1;
    lst->scroll_offset = 0;
}

int
cbx_list_item_count(const cbx_list *lst)
{
    return lst ? lst->item_count : 0;
}

int
cbx_list_get_selected(const cbx_list *lst)
{
    return lst ? lst->selected : -1;
}

void
cbx_list_set_selected(cbx_list *lst, int index)
{
    if (!lst)
        return;
    if (index < 0 || index >= lst->item_count) {
        lst->selected = -1;
        return;
    }
    lst->selected = index;
    ensure_scroll_visible(lst);
}

int
cbx_list_scroll_up(cbx_list *lst)
{
    if (!lst)
        return -EINVAL;
    if (lst->scroll_offset > 0) {
        lst->scroll_offset--;
        return 0;
    }
    return -1;
}

int
cbx_list_scroll_down(cbx_list *lst)
{
    if (!lst)
        return -EINVAL;
    compute_visible(lst);
    int max_scroll = lst->item_count - lst->visible_count;
    if (max_scroll < 0)
        max_scroll = 0;
    if (lst->scroll_offset < max_scroll) {
        lst->scroll_offset++;
        return 0;
    }
    return -1;
}

void
cbx_list_set_select_cb(cbx_list *lst, cbx_list_select_cb cb)
{
    if (!lst)
        return;
    lst->on_select = cb;
}