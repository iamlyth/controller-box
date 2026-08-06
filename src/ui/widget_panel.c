/*
 * widget_panel.c — Panel container widget implementation.
 *
 * A panel holds up to CBX_PANEL_MAX_CHILDREN child widgets.  It optionally
 * draws a background fill and/or border, then draws all visible children.
 * Events are forwarded to the focused child (if any).
 *
 * The panel does NOT own its children — the caller is responsible for
 * destroying child widgets.  This avoids double-free when children are
 * stack-allocated or shared.
 *
 * Task 20 — Widget base and concrete widgets.
 */
#include "widget.h"

#include <errno.h>
#include <string.h>

/* --- vtable -------------------------------------------------------- */

static void
panel_draw(cbx_widget *w, SDL_Renderer *r)
{
    cbx_panel *pnl = (cbx_panel *)w;
    if (!r)
        return;

    /* Draw background. */
    if (pnl->draw_bg && pnl->theme) {
        SDL_SetRenderDrawColor(r, pnl->theme->panel_bg.r,
                               pnl->theme->panel_bg.g,
                               pnl->theme->panel_bg.b,
                               pnl->theme->panel_bg.a);
        SDL_RenderFillRect(r, &pnl->base.rect);
    }

    /* Draw border. */
    if (pnl->draw_border && pnl->theme) {
        SDL_SetRenderDrawColor(r, pnl->theme->border.r,
                               pnl->theme->border.g,
                               pnl->theme->border.b,
                               pnl->theme->border.a);
        SDL_RenderDrawRect(r, &pnl->base.rect);
    }

    /* Draw all visible children. */
    for (int i = 0; i < pnl->child_count; i++) {
        cbx_widget_draw(pnl->children[i], r);
    }
}

static bool
panel_handle_event(cbx_widget *w, const SDL_Event *ev)
{
    cbx_panel *pnl = (cbx_panel *)w;
    if (!ev)
        return false;

    /* Forward to focused child if any. */
    if (pnl->focused_child >= 0 && pnl->focused_child < pnl->child_count) {
        cbx_widget *child = pnl->children[pnl->focused_child];
        if (child)
            return cbx_widget_handle_event(child, ev);
    }
    return false;
}

static void
panel_focus(cbx_widget *w)
{
    cbx_panel *pnl = (cbx_panel *)w;
    pnl->base.focused = true;
    /* Focus first child if none focused. */
    if (pnl->focused_child < 0 && pnl->child_count > 0) {
        cbx_panel_focus_first(pnl);
    }
}

static void
panel_blur(cbx_widget *w)
{
    cbx_panel *pnl = (cbx_panel *)w;
    pnl->base.focused = false;
    /* Blur focused child. */
    if (pnl->focused_child >= 0 && pnl->focused_child < pnl->child_count) {
        cbx_widget_blur(pnl->children[pnl->focused_child]);
    }
}

static void
panel_get_rect(const cbx_widget *w, SDL_Rect *out)
{
    const cbx_panel *pnl = (const cbx_panel *)w;
    *out = pnl->base.rect;
}

static void
panel_set_rect(cbx_widget *w, const SDL_Rect *rect)
{
    cbx_panel *pnl = (cbx_panel *)w;
    pnl->base.rect = *rect;
}

static void
panel_destroy(cbx_widget *w)
{
    (void)w;
    /* Panel does NOT own children — caller destroys them. */
}

static const cbx_widget_vtable panel_vt = {
    .draw         = panel_draw,
    .handle_event = panel_handle_event,
    .focus        = panel_focus,
    .blur         = panel_blur,
    .get_rect     = panel_get_rect,
    .set_rect     = panel_set_rect,
    .destroy      = panel_destroy,
};

/* --- public API ---------------------------------------------------- */

int
cbx_panel_init(cbx_panel *panel, const cbx_theme *theme)
{
    if (!panel)
        return -EINVAL;
    memset(panel, 0, sizeof(*panel));
    panel->base.vt = &panel_vt;
    panel->base.visible = true;
    panel->base.focused = false;
    panel->base.rect = (SDL_Rect){0, 0, 0, 0};
    panel->theme = theme;
    panel->child_count = 0;
    panel->focused_child = -1;
    panel->padding = 8;
    panel->draw_bg = true;
    panel->draw_border = true;
    return 0;
}

int
cbx_panel_add_child(cbx_panel *panel, cbx_widget *child)
{
    if (!panel || !child)
        return -EINVAL;
    if (panel->child_count >= CBX_PANEL_MAX_CHILDREN)
        return -ENOMEM;
    panel->children[panel->child_count++] = child;
    return 0;
}

int
cbx_panel_remove_child(cbx_panel *panel, cbx_widget *child)
{
    if (!panel || !child)
        return -EINVAL;
    for (int i = 0; i < panel->child_count; i++) {
        if (panel->children[i] == child) {
            /* Shift remaining children down. */
            for (int j = i; j < panel->child_count - 1; j++)
                panel->children[j] = panel->children[j + 1];
            panel->child_count--;
            /* Adjust focused_child index. */
            if (panel->focused_child == i) {
                panel->focused_child = -1;
            } else if (panel->focused_child > i) {
                panel->focused_child--;
            }
            return 0;
        }
    }
    return -ENOENT;
}

int
cbx_panel_child_count(const cbx_panel *panel)
{
    return panel ? panel->child_count : 0;
}

cbx_widget *
cbx_panel_get_child(cbx_panel *panel, int index)
{
    if (!panel || index < 0 || index >= panel->child_count)
        return NULL;
    return panel->children[index];
}

cbx_widget *
cbx_panel_get_focused_child(cbx_panel *panel)
{
    if (!panel || panel->focused_child < 0 ||
        panel->focused_child >= panel->child_count)
        return NULL;
    return panel->children[panel->focused_child];
}

static void
set_focus(cbx_panel *panel, int index)
{
    /* Blur old. */
    if (panel->focused_child >= 0 && panel->focused_child < panel->child_count)
        cbx_widget_blur(panel->children[panel->focused_child]);
    panel->focused_child = index;
    if (index >= 0 && index < panel->child_count)
        cbx_widget_focus(panel->children[index]);
}

int
cbx_panel_focus_first(cbx_panel *panel)
{
    if (!panel || panel->child_count == 0)
        return -1;
    set_focus(panel, 0);
    return 0;
}

int
cbx_panel_focus_next(cbx_panel *panel)
{
    if (!panel || panel->child_count == 0)
        return -1;
    int next = (panel->focused_child + 1) % panel->child_count;
    set_focus(panel, next);
    return next;
}

int
cbx_panel_focus_prev(cbx_panel *panel)
{
    if (!panel || panel->child_count == 0)
        return -1;
    int prev = panel->focused_child <= 0
        ? panel->child_count - 1
        : panel->focused_child - 1;
    set_focus(panel, prev);
    return prev;
}

void
cbx_panel_clear_focus(cbx_panel *panel)
{
    if (!panel)
        return;
    if (panel->focused_child >= 0 && panel->focused_child < panel->child_count)
        cbx_widget_blur(panel->children[panel->focused_child]);
    panel->focused_child = -1;
}

void
cbx_panel_set_padding(cbx_panel *panel, int padding)
{
    if (!panel)
        return;
    panel->padding = padding;
}

void
cbx_panel_set_draw_bg(cbx_panel *panel, bool draw_bg)
{
    if (!panel)
        return;
    panel->draw_bg = draw_bg;
}

void
cbx_panel_set_draw_border(cbx_panel *panel, bool draw_border)
{
    if (!panel)
        return;
    panel->draw_border = draw_border;
}