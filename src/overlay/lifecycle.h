/*
 * lifecycle.h — Overlay state machine and lifecycle management.
 *
 * Task 28 — Overlay state machine and lifecycle.
 *
 * The overlay lifecycle is driven by InputPlumber's InterceptMode property
 * (SPEC §2.5).  The state machine transitions are:
 *
 *   IDLE → ACTIVATING  (poll detected InterceptMode = ALL)
 *   ACTIVATING → VISIBLE  (fade-in animation complete, or instant if 0 ms)
 *   VISIBLE → CLOSING  (B button pressed, or deactivation detected, or timeout)
 *   CLOSING → IDLE  (fade-out animation complete, or instant if 0 ms)
 *
 * On close (VISIBLE → CLOSING):
 *   1. Save assignments (via on_save callback — caller handles persistence
 *      and conflict resolution per SPEC §4.5).
 *   2. Set InterceptMode back to PASS (1) via DBus so input flows to the game
 *      in <1 ms (SPEC §11).
 *   3. Start fade-out animation (if configured).
 *   4. On animation complete: hide surface (not destroy — SPEC §11), → IDLE.
 *
 * Timeout handling: if the overlay stays in VISIBLE for max_visible_ticks
 * ticks (0 = disabled), it is force-closed.  This prevents infinite display
 * if the deactivation signal is lost or the B-press is not detected.
 *
 * The surface and renderer are optional (NULL = state machine only, no
 * rendering side effects).  This allows unit-testing the state machine
 * without SDL rendering, using only the mock DBus backend.
 */
#ifndef CBX_OVERLAY_LIFECYCLE_H
#define CBX_OVERLAY_LIFECYCLE_H

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#include "dbus/dbus_interface.h"          /* ip_dbus_backend, ip_bus_handle */
#include "dbus/ip_device_model.h"  /* CBX_MAX_PATH_LEN */
#include "overlay/surface_build.h"
#include "ui/animation.h"

/* --- Defaults --------------------------------------------------------- */

#define CBX_OVERLAY_DEFAULT_FADE_IN_MS      150   /* ms */
#define CBX_OVERLAY_DEFAULT_FADE_OUT_MS     100   /* ms */
#define CBX_OVERLAY_DEFAULT_MAX_VISIBLE_TICKS  0  /* 0 = disabled */
#define CBX_OVERLAY_DEFAULT_MAX_ERRORS       5

/* --- State enum ------------------------------------------------------- */

typedef enum {
    CBX_OVERLAY_IDLE      = 0,  /* waiting for activation              */
    CBX_OVERLAY_ACTIVATING,     /* fade-in in progress                 */
    CBX_OVERLAY_VISIBLE,        /* overlay shown, user navigating      */
    CBX_OVERLAY_CLOSING,        /* closing: InterceptMode=PASS, fade-out */
} cbx_overlay_state;

/* --- Callbacks -------------------------------------------------------- */

/* Fired when ACTIVATING → VISIBLE transition completes. */
typedef void (*cbx_overlay_transition_cb)(void *userdata);

/* Fired on close to persist assignment changes (SPEC §4.5 conflict
 * resolution is the caller's responsibility).  Returns 0 on success,
 * negative errno on failure. */
typedef int (*cbx_overlay_save_cb)(void *userdata);

/* Fired on errors (DBus failures, save failures, etc.). */
typedef void (*cbx_overlay_error_cb)(int code, void *userdata);

/* --- Lifecycle struct ------------------------------------------------- */

typedef struct {
    /* State */
    cbx_overlay_state state;

    /* DBus backend for setting InterceptMode on close. */
    const ip_dbus_backend *backend;
    ip_bus_handle           bus;
    char                    composite_path[CBX_MAX_PATH_LEN];

    /* Pre-built overlay surface + renderer (optional — NULL = no
     * rendering side effects, state machine only). */
    cbx_overlay_surface *surface;
    SDL_Renderer         *renderer;

    /* Fade animation */
    cbx_anim  fade;
    uint32_t  fade_in_ms;    /* 0 = instant activation */
    uint32_t  fade_out_ms;   /* 0 = instant close */
    double    target_opacity; /* from settings.overlay_opacity (0.0–1.0) */

    /* Timeout watchdog for stuck VISIBLE state (0 = disabled). */
    int visible_ticks;
    int max_visible_ticks;

    /* Error tracking for InterceptMode set failures. */
    int error_count;
    int max_errors;

    /* Callbacks (all optional — NULL = skipped). */
    cbx_overlay_transition_cb on_visible;
    void                     *on_visible_data;
    cbx_overlay_transition_cb on_closed;
    void                     *on_closed_data;
    cbx_overlay_save_cb       on_save;
    void                     *on_save_data;
    cbx_overlay_error_cb      on_error;
    void                     *on_error_data;
} cbx_overlay_lifecycle;

/* --- API -------------------------------------------------------------- */

/*
 * Initialise the lifecycle struct.  Sets defaults and stores the DBus
 * backend, bus, composite path, and optional surface/renderer.
 * State is set to IDLE.
 *
 * `surface` and `renderer` may be NULL (state machine only, no rendering).
 */
void cbx_overlay_lifecycle_init(cbx_overlay_lifecycle *lc,
                                 const ip_dbus_backend *backend,
                                 ip_bus_handle bus,
                                 const char *composite_path,
                                 cbx_overlay_surface *surface,
                                 SDL_Renderer *renderer);

/*
 * Request activation: IDLE → ACTIVATING.
 * Starts fade-in animation (if fade_in_ms > 0) or transitions directly
 * to VISIBLE (if fade_in_ms == 0).  Marks the surface dirty and shows
 * it when VISIBLE is reached.
 *
 * Returns 0 on success, -EINVAL if lc is NULL, -EPERM if not in IDLE.
 */
int cbx_overlay_lifecycle_activate(cbx_overlay_lifecycle *lc);

/*
 * Request close: VISIBLE → CLOSING.
 * Fires on_save callback, sets InterceptMode=PASS via DBus, starts
 * fade-out animation (if fade_out_ms > 0) or transitions directly to
 * IDLE (if fade_out_ms == 0).  Hides surface when IDLE is reached.
 *
 * Returns 0 on success, -EINVAL if lc is NULL, -EPERM if not in VISIBLE
 * or ACTIVATING (close during ACTIVATING cancels the activation).
 */
int cbx_overlay_lifecycle_close(cbx_overlay_lifecycle *lc);

/*
 * Process one main-loop tick.  Polls the fade animation and transitions
 * state accordingly.  In VISIBLE, increments the timeout counter and
 * force-closes if max_visible_ticks is exceeded.
 *
 * Returns 0 on success, -EINVAL if lc is NULL.
 */
int cbx_overlay_lifecycle_tick(cbx_overlay_lifecycle *lc);

/*
 * Force close from any state.  Skips the fade-out animation and
 * immediately sets InterceptMode=PASS, hides surface, → IDLE.
 * Does NOT fire on_save (use close() for the normal path).
 */
void cbx_overlay_lifecycle_force_close(cbx_overlay_lifecycle *lc);

/* Query the current state. */
cbx_overlay_state cbx_overlay_lifecycle_get_state(
    const cbx_overlay_lifecycle *lc);

/* Is the overlay in any active state (ACTIVATING, VISIBLE, CLOSING)? */
bool cbx_overlay_lifecycle_is_active(const cbx_overlay_lifecycle *lc);

/* Human-readable state name (for logging). */
const char *cbx_overlay_lifecycle_state_name(cbx_overlay_state state);

#endif /* CBX_OVERLAY_LIFECYCLE_H */