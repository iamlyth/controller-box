/*
 * lifecycle.c — Overlay state machine and lifecycle management.
 *
 * Task 28 — Overlay state machine and lifecycle.
 *
 * Implements the IDLE → ACTIVATING → VISIBLE → CLOSING → IDLE state
 * machine described in lifecycle.h.  The module is a pure state machine
 * with optional rendering side effects (surface show/hide) and optional
 * animation (fade in/out).  DBus interaction is limited to setting
 * InterceptMode=PASS on close, via the injectable backend vtable.
 */
#include "overlay/lifecycle.h"

#include <errno.h>
#include <string.h>

#include "dbus/ip_composite.h"

/* --- Helpers ---------------------------------------------------------- */

/*
 * Start fade-in: if fade_in_ms > 0 and surface exists, start the animation
 * from 0.0 to target_opacity.  Otherwise, transition directly to VISIBLE.
 */
static void
begin_fade_in(cbx_overlay_lifecycle *lc)
{
    if (lc->fade_in_ms > 0) {
        cbx_anim_fade_in(&lc->fade, lc->target_opacity, lc->fade_in_ms);
        if (lc->surface)
            cbx_overlay_surface_set_opacity(lc->surface, 0.0);
        /* Mark surface dirty so the render callback redraws. */
        if (lc->surface)
            cbx_overlay_surface_mark_dirty_all(lc->surface);
    }
    /* If fade_in_ms == 0, tick() or activate() will
     * immediately transition to VISIBLE. */
}

/*
 * Start fade-out: if fade_out_ms > 0 and surface exists, start the
 * animation from current alpha to 0.0.  Otherwise, transition directly
 * to IDLE on the next tick (or immediately).
 */
static void
begin_fade_out(cbx_overlay_lifecycle *lc)
{
    if (lc->fade_out_ms > 0) {
        double cur = cbx_anim_alpha(&lc->fade);
        if (cur <= 0.0)
            cur = lc->target_opacity; /* safety: use target if alpha is 0 */
        cbx_anim_fade_out(&lc->fade, lc->fade_out_ms);
        /* If fade was not running, fade_out sets from=current_alpha which
         * is 0 for a fresh anim.  Fix: set from explicitly. */
        lc->fade.from_alpha = cur;
    }
}

/*
 * Show the surface: mark it dirty and present the pre-built texture.
 * The function never renders — it marks the surface so
 * cbx_overlay_service_step()'s active+dirty branch re-renders from current
 * state on the next step, while the already-built texture is presented
 * immediately for zero-latency visibility (SPEC §4.9/§11).
 * Called when entering VISIBLE state.
 */
static void
show_surface(cbx_overlay_lifecycle *lc)
{
    if (!lc->surface || !lc->renderer)
        return;
    cbx_overlay_surface_mark_dirty_all(lc->surface);
    cbx_overlay_surface_show(lc->surface, lc->renderer);
}

/*
 * Hide the surface (not destroy — SPEC §11).
 */
static void
hide_surface(cbx_overlay_lifecycle *lc)
{
    if (!lc->surface)
        return;
    cbx_overlay_surface_hide(lc->surface);
}

/*
 * Set InterceptMode=PASS via DBus.  Returns 0 on success, negative on
 * error.  Tracks consecutive failures.
 */
static int
set_intercept_pass(cbx_overlay_lifecycle *lc)
{
    if (!lc->backend || !lc->bus || !lc->composite_path[0])
        return -EINVAL;

    int rc = ip_composite_set_intercept_mode(lc->backend, lc->bus,
                                               lc->composite_path, "1");
    if (rc < 0) {
        lc->error_count++;
        if (lc->on_error)
            lc->on_error(rc, lc->on_error_data);
        return rc;
    }
    lc->error_count = 0;
    return 0;
}

/*
 * Transition to VISIBLE: show surface, fire on_visible callback.
 */
static void
enter_visible(cbx_overlay_lifecycle *lc)
{
    lc->state = CBX_OVERLAY_VISIBLE;
    lc->visible_ticks = 0;
    show_surface(lc);
    if (lc->on_visible)
        lc->on_visible(lc->on_visible_data);
}

/*
 * Transition to IDLE: hide surface, fire on_closed callback.
 */
static void
enter_idle(cbx_overlay_lifecycle *lc)
{
    hide_surface(lc);
    cbx_anim_stop(&lc->fade);
    lc->state = CBX_OVERLAY_IDLE;
    lc->visible_ticks = 0;
    if (lc->on_closed)
        lc->on_closed(lc->on_closed_data);
}

/* --- Public API -------------------------------------------------------- */

void
cbx_overlay_lifecycle_init(cbx_overlay_lifecycle *lc,
                             const ip_dbus_backend *backend,
                             ip_bus_handle bus,
                             const char *composite_path,
                             cbx_overlay_surface *surface,
                             SDL_Renderer *renderer)
{
    if (!lc)
        return;

    memset(lc, 0, sizeof(*lc));
    lc->state = CBX_OVERLAY_IDLE;
    lc->backend = backend;
    lc->bus = bus;
    if (composite_path)
        strncpy(lc->composite_path, composite_path,
                sizeof(lc->composite_path) - 1);
    lc->surface = surface;
    lc->renderer = renderer;
    lc->fade_in_ms = CBX_OVERLAY_DEFAULT_FADE_IN_MS;
    lc->fade_out_ms = CBX_OVERLAY_DEFAULT_FADE_OUT_MS;
    lc->target_opacity = 1.0;
    lc->max_visible_ticks = CBX_OVERLAY_DEFAULT_MAX_VISIBLE_TICKS;
    lc->max_errors = CBX_OVERLAY_DEFAULT_MAX_ERRORS;
    lc->require_pass_for_close = false;
    lc->close_blocked = false;
    cbx_anim_init(&lc->fade);
}

int
cbx_overlay_lifecycle_activate(cbx_overlay_lifecycle *lc)
{
    if (!lc)
        return -EINVAL;
    if (lc->state != CBX_OVERLAY_IDLE)
        return -EPERM;

    lc->close_blocked = false;
    lc->state = CBX_OVERLAY_ACTIVATING;

    if (lc->fade_in_ms == 0) {
        /* Instant activation: skip directly to VISIBLE. */
        enter_visible(lc);
    } else {
        begin_fade_in(lc);
    }

    return 0;
}

int
cbx_overlay_lifecycle_close(cbx_overlay_lifecycle *lc)
{
    if (!lc)
        return -EINVAL;

    /* Allow close from ACTIVATING (cancel activation) or VISIBLE. */
    if (lc->state != CBX_OVERLAY_VISIBLE && lc->state != CBX_OVERLAY_ACTIVATING)
        return -EPERM;

    lc->close_blocked = false;
    lc->state = CBX_OVERLAY_CLOSING;

    /* Set InterceptMode back to PASS FIRST so input flows to the game in
     * <1 ms (SPEC §11: "single DBus property set").  The save callback runs
     * conflict resolution, a full engine apply (per-row clear/attach with
     * attachment waits) and persistence — dozens of synchronous bus round
     * trips.  Running it before PASS kept gameplay input intercepted for
     * that whole window, one to two orders of magnitude over the budget on a
     * healthy bus and unbounded on a stalled one.  PASS must be the first
     * work the close performs. */
    int pass_rc = set_intercept_pass(lc);
    if (pass_rc != 0 && lc->require_pass_for_close) {
        /* A failed release must remain visible and actionable; hiding here
         * would falsely report restored gameplay while input is intercepted. */
        lc->state = CBX_OVERLAY_VISIBLE;
        lc->visible_ticks = 0;
        lc->close_blocked = true;
        show_surface(lc);
        return pass_rc;
    }

    /* Fire save callback (caller handles persistence + conflict resolution).
     * The caller bounds this callback's synchronous DBus chain with the same
     * wall-clock deadline used by readiness/recovery so a wedged bus cannot
     * hold the UI thread (or the fade-out/hide) indefinitely. */
    int save_rc = 0;
    if (lc->on_save) {
        save_rc = lc->on_save(lc->on_save_data);
        if (save_rc < 0 && lc->on_error)
            lc->on_error(save_rc, lc->on_error_data);
    }

    /* An unresolved conflict is not a close.  Returning to VISIBLE keeps
     * the red/safe state actionable and prevents duplicate routing from being
     * reported as a successful resolution.  PASS has already been requested
     * above, so a later retry starts from a known input mode.  Production
     * enables the same fail-closed behavior for every save error; the
     * state-machine-only compatibility mode keeps best-effort close semantics. */
    if (save_rc == -ENOSPC ||
        (save_rc < 0 && lc->require_pass_for_close)) {
        lc->state = CBX_OVERLAY_VISIBLE;
        lc->visible_ticks = 0;
        lc->close_blocked = true;
        show_surface(lc);
        return save_rc;
    }

    if (lc->fade_out_ms == 0) {
        /* Instant close: immediately transition to IDLE. */
        enter_idle(lc);
    } else {
        begin_fade_out(lc);
    }

    return 0;
}

int
cbx_overlay_lifecycle_tick(cbx_overlay_lifecycle *lc)
{
    if (!lc)
        return -EINVAL;

    switch (lc->state) {
    case CBX_OVERLAY_IDLE:
        /* Nothing to do. */
        break;

    case CBX_OVERLAY_ACTIVATING: {
        if (!cbx_anim_is_running(&lc->fade)) {
            /* Animation not started or already complete → VISIBLE. */
            enter_visible(lc);
        } else {
            double alpha = cbx_anim_update(&lc->fade);
            if (lc->surface)
                cbx_overlay_surface_set_opacity(lc->surface, alpha);
            if (cbx_anim_is_complete(&lc->fade)) {
                enter_visible(lc);
            } else if (lc->surface && lc->renderer) {
                /* Present this fade frame.  set_opacity only writes the
                 * texture's alpha mod; without a present the intermediate
                 * opacity work is discarded and the overlay stays invisible
                 * until the fade completes (SPEC §11: first
                 * compositor-visible frame < 10 ms from ALL detection). */
                cbx_overlay_surface_show(lc->surface, lc->renderer);
            }
        }
        break;
    }

    case CBX_OVERLAY_VISIBLE:
        /* Increment timeout counter. */
        lc->visible_ticks++;
        if (lc->max_visible_ticks > 0 &&
            lc->visible_ticks >= lc->max_visible_ticks) {
            /* Timeout: force close. */
            cbx_overlay_lifecycle_close(lc);
        }
        break;

    case CBX_OVERLAY_CLOSING: {
        if (!cbx_anim_is_running(&lc->fade)) {
            /* Animation not started or already complete → IDLE. */
            enter_idle(lc);
        } else {
            double alpha = cbx_anim_update(&lc->fade);
            if (lc->surface) {
                cbx_overlay_surface_set_opacity(lc->surface, alpha);
                /* Present each fade-out frame so the close is an actual
                 * fade rather than a hard cut (the opacity write alone is
                 * never presented otherwise). */
                if (lc->renderer)
                    cbx_overlay_surface_show(lc->surface, lc->renderer);
            }
            if (cbx_anim_is_complete(&lc->fade)) {
                enter_idle(lc);
            }
        }
        break;
    }

    default:
        /* Unknown state — reset to IDLE. */
        lc->state = CBX_OVERLAY_IDLE;
        break;
    }

    return 0;
}

void
cbx_overlay_lifecycle_force_close(cbx_overlay_lifecycle *lc)
{
    if (!lc)
        return;

    /* Set InterceptMode=PASS regardless of current state.  In the
     * production fail-closed mode, do not claim IDLE if release failed:
     * hidden/IDLE is a gameplay-restored state, not merely a rendering
     * decision. */
    if (lc->state != CBX_OVERLAY_IDLE) {
        int rc = set_intercept_pass(lc);
        if (rc != 0 && lc->require_pass_for_close) {
            lc->state = CBX_OVERLAY_VISIBLE;
            lc->visible_ticks = 0;
            lc->close_blocked = true;
            show_surface(lc);
            return;
        }
    }

    enter_idle(lc);
}

void
cbx_overlay_lifecycle_abandon(cbx_overlay_lifecycle *lc)
{
    if (!lc)
        return;

    /* The backend is gone, so there is no intercept mode to release and no
     * truthful PASS to confirm.  Hiding is safe because the engine that
     * owned the interception no longer exists.  Return to IDLE so a later
     * reconnect re-registers triggers and resumes normally. */
    lc->close_blocked = false;
    enter_idle(lc);
}

cbx_overlay_state
cbx_overlay_lifecycle_get_state(const cbx_overlay_lifecycle *lc)
{
    if (!lc)
        return CBX_OVERLAY_IDLE;
    return lc->state;
}

bool
cbx_overlay_lifecycle_is_active(const cbx_overlay_lifecycle *lc)
{
    if (!lc)
        return false;
    return lc->state != CBX_OVERLAY_IDLE;
}

const char *
cbx_overlay_lifecycle_state_name(cbx_overlay_state state)
{
    switch (state) {
    case CBX_OVERLAY_IDLE:       return "IDLE";
    case CBX_OVERLAY_ACTIVATING: return "ACTIVATING";
    case CBX_OVERLAY_VISIBLE:    return "VISIBLE";
    case CBX_OVERLAY_CLOSING:     return "CLOSING";
    default:                      return "UNKNOWN";
    }
}