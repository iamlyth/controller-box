/*
 * widget_progress.c — Progress bar widget implementation.
 *
 * Fill bar representing 0.0–1.0 completion.  Configurable fill and
 * background colours.  Non-interactive (handle_event returns false).
 *
 * Task 21 — List, Grid, TabBar, and ProgressBar widgets.
 */
#include "widget.h"

#include <errno.h>

static void
progress_draw(cbx_widget *w, SDL_Renderer *r)
{
    cbx_progress *prog = (cbx_progress *)w;
    if (!r)
        return;

    /* Background. */
    SDL_SetRenderDrawColor(r, prog->bg_color.r, prog->bg_color.g,
                           prog->bg_color.b, prog->bg_color.a);
    SDL_RenderFillRect(r, &prog->base.rect);

    /* Fill. */
    double frac = prog->fraction;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    int fill_w = (int)(prog->base.rect.w * frac);
    if (fill_w > 0) {
        SDL_Rect fill = {
            .x = prog->base.rect.x,
            .y = prog->base.rect.y,
            .w = fill_w,
            .h = prog->base.rect.h,
        };
        SDL_SetRenderDrawColor(r, prog->bar_color.r, prog->bar_color.g,
                               prog->bar_color.b, prog->bar_color.a);
        SDL_RenderFillRect(r, &fill);
    }

    /* Border. */
    if (prog->theme) {
        SDL_SetRenderDrawColor(r, prog->theme->border.r,
                               prog->theme->border.g,
                               prog->theme->border.b,
                               prog->theme->border.a);
        SDL_RenderDrawRect(r, &prog->base.rect);
    }
}

static bool
progress_handle_event(cbx_widget *w, const SDL_Event *ev)
{
    (void)w;
    (void)ev;
    return false;  /* non-interactive */
}

static void
progress_focus(cbx_widget *w)
{
    (void)w;
    /* Progress bar is not focusable. */
}

static void
progress_blur(cbx_widget *w)
{
    (void)w;
}

static void
progress_get_rect(const cbx_widget *w, SDL_Rect *out)
{
    const cbx_progress *prog = (const cbx_progress *)w;
    *out = prog->base.rect;
}

static void
progress_set_rect(cbx_widget *w, const SDL_Rect *rect)
{
    cbx_progress *prog = (cbx_progress *)w;
    prog->base.rect = *rect;
}

static void
progress_destroy(cbx_widget *w)
{
    (void)w;
}

static const cbx_widget_vtable progress_vt = {
    .draw         = progress_draw,
    .handle_event = progress_handle_event,
    .focus        = progress_focus,
    .blur         = progress_blur,
    .get_rect     = progress_get_rect,
    .set_rect     = progress_set_rect,
    .destroy      = progress_destroy,
};

/* --- public API ---------------------------------------------------- */

int
cbx_progress_init(cbx_progress *prog, const cbx_theme *theme)
{
    if (!prog || !theme)
        return -EINVAL;
    memset(prog, 0, sizeof(*prog));
    prog->base.vt = &progress_vt;
    prog->base.visible = true;
    prog->base.interactive = false;
    prog->base.focused = false;
    prog->base.rect = (SDL_Rect){0, 0, 0, 0};
    prog->theme = theme;
    prog->fraction = 0.0;
    prog->bar_color = theme->text_accent;
    prog->bg_color = theme->panel_bg;
    return 0;
}

void
cbx_progress_set_fraction(cbx_progress *prog, double frac)
{
    if (!prog)
        return;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    prog->fraction = frac;
}

double
cbx_progress_get_fraction(const cbx_progress *prog)
{
    return prog ? prog->fraction : 0.0;
}

void
cbx_progress_set_bar_color(cbx_progress *prog, SDL_Color color)
{
    if (!prog)
        return;
    prog->bar_color = color;
}

void
cbx_progress_set_bg_color(cbx_progress *prog, SDL_Color color)
{
    if (!prog)
        return;
    prog->bg_color = color;
}