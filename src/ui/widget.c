/*
 * widget.c — Base widget dispatcher implementation.
 *
 * Every function is a thin forwarder through the vtable.  All are NULL-safe:
 * passing a NULL widget or a widget whose vtable entry is NULL is a no-op
 * (draw returns void, handle_event returns false).
 *
 * Task 20 — Widget base and concrete widgets.
 */
#include "widget.h"

void
cbx_widget_draw(cbx_widget *w, SDL_Renderer *r)
{
    if (!w || !w->vt || !w->vt->draw || !w->visible)
        return;
    w->vt->draw(w, r);
}

bool
cbx_widget_handle_event(cbx_widget *w, const SDL_Event *ev)
{
    if (!w || !w->vt || !w->vt->handle_event || !w->visible)
        return false;
    return w->vt->handle_event(w, ev);
}

void
cbx_widget_focus(cbx_widget *w)
{
    if (!w || !w->vt || !w->vt->focus)
        return;
    w->vt->focus(w);
}

void
cbx_widget_blur(cbx_widget *w)
{
    if (!w || !w->vt || !w->vt->blur)
        return;
    w->vt->blur(w);
}

void
cbx_widget_get_rect(const cbx_widget *w, SDL_Rect *out)
{
    if (!w || !w->vt || !w->vt->get_rect || !out)
        return;
    w->vt->get_rect(w, out);
}

void
cbx_widget_set_rect(cbx_widget *w, const SDL_Rect *rect)
{
    if (!w || !w->vt || !w->vt->set_rect || !rect)
        return;
    w->vt->set_rect(w, rect);
}

void
cbx_widget_destroy(cbx_widget *w)
{
    if (!w || !w->vt || !w->vt->destroy)
        return;
    w->vt->destroy(w);
}

bool
cbx_widget_is_focused(const cbx_widget *w)
{
    return w ? w->focused : false;
}

bool
cbx_widget_is_visible(const cbx_widget *w)
{
    return w ? w->visible : false;
}

void
cbx_widget_set_visible(cbx_widget *w, bool visible)
{
    if (!w)
        return;
    w->visible = visible;
}