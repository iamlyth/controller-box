/*
 * animation.c — Alpha tween and easing primitives.
 *
 * Task 23 — Animation primitives and dirty rect optimization.
 */
#include "ui/animation.h"

#include <math.h>

/* ------------------------------------------------------------------ */
/* Easing                                                             */
/* ------------------------------------------------------------------ */

double
cbx_ease_eval(cbx_ease_type type, double t)
{
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;

    switch (type) {
    case CBX_EASE_LINEAR:
        return t;

    case CBX_EASE_IN:
        return t * t;

    case CBX_EASE_OUT:
        return 1.0 - (1.0 - t) * (1.0 - t);

    case CBX_EASE_IN_OUT:
        if (t < 0.5)
            return 2.0 * t * t;
        else {
            double f = -2.0 * t + 2.0;
            return 1.0 - f * f / 2.0;
        }

    default:
        return t;   /* unknown → linear */
    }
}

/* ------------------------------------------------------------------ */
/* Animation                                                          */
/* ------------------------------------------------------------------ */

void
cbx_anim_init(cbx_anim *a)
{
    a->state       = CBX_ANIM_IDLE;
    a->start_ticks = 0;
    a->duration_ms = 0;
    a->from_alpha  = 0.0;
    a->to_alpha    = 0.0;
    a->easing      = CBX_EASE_LINEAR;
    a->cur_alpha   = 0.0;
}

void
cbx_anim_start(cbx_anim *a, double from, double to,
               uint32_t duration_ms, cbx_ease_type easing)
{
    a->state       = CBX_ANIM_RUNNING;
    a->start_ticks = SDL_GetTicks();
    a->duration_ms = duration_ms;
    a->from_alpha  = from;
    a->to_alpha    = to;
    a->easing      = easing;
    a->cur_alpha   = from;
}

double
cbx_anim_update(cbx_anim *a)
{
    switch (a->state) {
    case CBX_ANIM_IDLE:
        a->cur_alpha = a->from_alpha;
        return a->cur_alpha;

    case CBX_ANIM_COMPLETE:
        a->cur_alpha = a->to_alpha;
        return a->cur_alpha;

    case CBX_ANIM_RUNNING:
        break;
    }

    /* Running: compute progress from elapsed ticks */
    uint32_t now      = SDL_GetTicks();
    uint32_t elapsed  = now - a->start_ticks;

    /* Handle tick wraparound (SDL_GetTicks wraps ~49 days) */
    if (now < a->start_ticks)
        elapsed = 0;

    /* Instant or completed */
    if (a->duration_ms == 0 || elapsed >= a->duration_ms) {
        a->state     = CBX_ANIM_COMPLETE;
        a->cur_alpha = a->to_alpha;
        return a->cur_alpha;
    }

    /* Compute linear progress, then apply easing */
    double t = (double)elapsed / (double)a->duration_ms;
    double eased = cbx_ease_eval(a->easing, t);

    a->cur_alpha = a->from_alpha + (a->to_alpha - a->from_alpha) * eased;
    return a->cur_alpha;
}

bool
cbx_anim_is_running(const cbx_anim *a)
{
    return a->state == CBX_ANIM_RUNNING;
}

bool
cbx_anim_is_complete(const cbx_anim *a)
{
    return a->state == CBX_ANIM_COMPLETE;
}

double
cbx_anim_alpha(const cbx_anim *a)
{
    return a->cur_alpha;
}

void
cbx_anim_stop(cbx_anim *a)
{
    a->state = CBX_ANIM_IDLE;
}

void
cbx_anim_fade_in(cbx_anim *a, double target_alpha, uint32_t duration_ms)
{
    cbx_anim_start(a, 0.0, target_alpha, duration_ms, CBX_EASE_LINEAR);
}

void
cbx_anim_fade_out(cbx_anim *a, uint32_t duration_ms)
{
    cbx_anim_start(a, a->cur_alpha, 0.0, duration_ms, CBX_EASE_LINEAR);
}