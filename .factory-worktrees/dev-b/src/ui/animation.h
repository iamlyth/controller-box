/*
 * animation.h — Alpha tween and easing primitives for overlay fade in/out.
 *
 * Task 23 — Animation primitives and dirty rect optimization.
 *
 * Provides a lightweight, stateless-per-call animation system built on
 * SDL_GetTicks().  The caller owns the cbx_anim struct and polls
 * cbx_anim_update() each frame to obtain the current alpha value.
 *
 * The system is intentionally simple: one tween at a time, linear or
 * quadratic easing, millisecond resolution.  This matches SPEC §11
 * (overlay visible in <10 ms) — the animation is a visual nicety layered
 * on top of the pre-built overlay surface, not a critical-path requirement.
 */
#ifndef CBX_ANIMATION_H
#define CBX_ANIMATION_H

#include <SDL2/SDL.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Easing                                                             */
/* ------------------------------------------------------------------ */

typedef enum {
    CBX_EASE_LINEAR = 0,   /* y = t                          */
    CBX_EASE_IN,           /* y = t²  (quadratic ease-in)    */
    CBX_EASE_OUT,           /* y = 1-(1-t)²  (quadratic ease-out) */
    CBX_EASE_IN_OUT,        /* y = t² for t<0.5, else 1-(-2t+2)²/2  */
} cbx_ease_type;

/*
 * Compute the eased progress factor (0.0 – 1.0) for a raw linear
 * progress value.  Values outside [0,1] are clamped.  This is a pure
 * function — no state, no side effects.
 */
double cbx_ease_eval(cbx_ease_type type, double linear_t);

/* ------------------------------------------------------------------ */
/* Animation state machine                                            */
/* ------------------------------------------------------------------ */

typedef enum {
    CBX_ANIM_IDLE    = 0,   /* not started                     */
    CBX_ANIM_RUNNING,       /* in progress                     */
    CBX_ANIM_COMPLETE,      /* finished, final value returned   */
} cbx_anim_state;

/*
 * Single alpha tween.  The caller is expected to poll
 * cbx_anim_update() every frame.
 */
typedef struct {
    cbx_anim_state state;
    uint32_t        start_ticks;   /* SDL_GetTicks() captured at start */
    uint32_t        duration_ms;   /* tween length (0 = instant)       */
    double          from_alpha;    /* start alpha 0.0–1.0              */
    double          to_alpha;      /* end alpha 0.0–1.0               */
    cbx_ease_type   easing;       /* easing curve                     */
    double          cur_alpha;     /* last computed alpha               */
} cbx_anim;

/* Zero-initialise the animation (state = IDLE). */
void cbx_anim_init(cbx_anim *a);

/*
 * Start (or restart) a tween.  Captures SDL_GetTicks() as the start
 * time.  If duration_ms is 0 the animation completes immediately on the
 * first cbx_anim_update() call.
 */
void cbx_anim_start(cbx_anim *a, double from, double to,
                    uint32_t duration_ms, cbx_ease_type easing);

/*
 * Poll the animation.  Computes the current alpha based on elapsed
 * SDL_GetTicks() time, applies easing, and stores it in cur_alpha.
 * When the elapsed time >= duration_ms, state transitions to COMPLETE
 * and cur_alpha is set to to_alpha.  Returns cur_alpha.
 *
 * Calling update on an IDLE animation returns from_alpha without
 * starting it.  Calling update on a COMPLETE animation returns
 * to_alpha (idempotent).
 */
double cbx_anim_update(cbx_anim *a);

/* Query helpers (no state change). */
bool   cbx_anim_is_running(const cbx_anim *a);
bool   cbx_anim_is_complete(const cbx_anim *a);
double cbx_anim_alpha(const cbx_anim *a);

/* Stop the animation and reset to IDLE.  cur_alpha is left as-is. */
void cbx_anim_stop(cbx_anim *a);

/*
 * Convenience: start a fade-in tween from 0.0 to target_alpha over
 * duration_ms milliseconds with linear easing.
 */
void cbx_anim_fade_in(cbx_anim *a, double target_alpha, uint32_t duration_ms);

/*
 * Convenience: start a fade-out tween from current alpha to 0.0 over
 * duration_ms milliseconds with linear easing.
 */
void cbx_anim_fade_out(cbx_anim *a, uint32_t duration_ms);

#endif /* CBX_ANIMATION_H */