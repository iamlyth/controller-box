/*
 * ip_intercept_poll.h — InterceptMode polling state machine (Task 13).
 *
 * InterceptMode does NOT emit PropertiesChanged (SPEC gap #1).  The GUI
 * must poll the property at 50ms intervals to detect mode transitions.
 *
 * State machine (DEC-002):
 *
 *   IDLE → start() → PASS_WAIT (polling at 50ms, PASS expected)
 *                    → detect ALL → fire activating_cb → ACTIVE
 *   ACTIVE → detect PASS → fire deactivating_cb → IDLE
 *          → timeout (mode stuck at ALL) → fire error_cb → IDLE
 *
 * SDL integration:
 *   start() creates an SDL_AddTimer (50ms interval).  The timer callback
 *   pushes a custom SDL_UserEvent onto the event queue.  The main event
 *   loop processes this event by calling ip_intercept_poll_tick().
 *
 * SPEC §2.5, §10.3, §11; IMPLEMENTATION_PLAN DEC-002.
 */
#ifndef CBX_IP_INTERCEPT_POLL_H
#define CBX_IP_INTERCEPT_POLL_H

#include "dbus_interface.h"          /* ip_dbus_backend, ip_bus_handle */
#include "ip_device_model.h"     /* CBX_MAX_PATH_LEN */

#include <SDL2/SDL.h>
#include <stdbool.h>

/* --- Poll states --------------------------------------------------------- */

typedef enum {
    IP_POLL_IDLE = 0,       /* Not polling */
    IP_POLL_PASS_WAIT,      /* Polling, waiting for ALL (activation) */
    IP_POLL_ACTIVE,         /* Overlay active, waiting for PASS (deactivation) */
} ip_poll_state;

/* --- Callback types ------------------------------------------------------ */

/* Called when InterceptMode transitions PASS → ALL (overlay should show). */
typedef void (*ip_poll_activating_cb)(void *userdata);

/* Called when InterceptMode transitions ALL → PASS (overlay should hide). */
typedef void (*ip_poll_deactivating_cb)(void *userdata);

/* Called on timeout or unrecoverable error (state resets to IDLE). */
typedef void (*ip_poll_error_cb)(int error_code, void *userdata);

/* --- InterceptMode poll state machine ----------------------------------- */

#define IP_INTERCEPT_POLL_INTERVAL_MS  50    /* poll interval (DEC-002) */
#define IP_INTERCEPT_POLL_MAX_ERRORS    5    /* consecutive poll errors before reset */
#define IP_INTERCEPT_POLL_TIMEOUT_TICKS 200  /* ~10s in ACTIVE before timeout (200 * 50ms) */

typedef struct {
    /* DBus connection */
    const ip_dbus_backend *backend;
    ip_bus_handle          bus;
    char                   composite_path[CBX_MAX_PATH_LEN];

    /* State machine */
    ip_poll_state          state;
    int                    error_count;       /* consecutive poll errors */
    int                    max_errors;         /* threshold before error reset */
    int                    timeout_ticks;      /* ticks in current state */
    int                    max_timeout_ticks;  /* max ticks in ACTIVE before timeout */

    /* Callbacks */
    ip_poll_activating_cb   activating_cb;
    void                   *activating_data;
    ip_poll_deactivating_cb deactivating_cb;
    void                   *deactivating_data;
    ip_poll_error_cb        error_cb;
    void                   *error_data;

    /* SDL timer */
    SDL_TimerID             timer_id;          /* 0 = no timer running */
    uint32_t                sdl_event_type;    /* custom SDL event type */
} ip_intercept_poll;

/* --- Lifecycle ----------------------------------------------------------- */

/*
 * Initialise the poll state machine.  Does NOT start polling.
 * `composite_path` is copied into the struct (max CBX_MAX_PATH_LEN).
 * Callbacks may be NULL (events are silently ignored).
 */
void ip_intercept_poll_init(ip_intercept_poll *poll,
                              const ip_dbus_backend *backend,
                              ip_bus_handle bus,
                              const char *composite_path,
                              ip_poll_activating_cb activating_cb,
                              void *activating_data,
                              ip_poll_deactivating_cb deactivating_cb,
                              void *deactivating_data,
                              ip_poll_error_cb error_cb,
                              void *error_data);

/*
 * Start polling.  Sets state to PASS_WAIT and creates an SDL timer.
 * `interval_ms` is the poll interval (default IP_INTERCEPT_POLL_INTERVAL_MS).
 * `sdl_event_type` is a custom SDL event type registered via
 * SDL_RegisterEvents().  The timer callback pushes this event; the main
 * loop calls ip_intercept_poll_tick() when it sees the event.
 * Returns 0 on success, negative errno on failure.
 */
int ip_intercept_poll_start(ip_intercept_poll *poll,
                              uint32_t interval_ms,
                              uint32_t sdl_event_type);

/*
 * Stop polling and reset to IDLE.  Removes the SDL timer.
 * Safe to call when not polling (no-op).
 */
void ip_intercept_poll_stop(ip_intercept_poll *poll);

/* --- State machine tick (call from main loop on timer event) ------------- */

/*
 * Process one poll cycle.  Reads InterceptMode via get_property and
 * transitions the state machine.  Fires callbacks on transitions.
 *
 * This function is the core of the state machine and is fully testable
 * without SDL (call it directly in tests with mock expectations).
 *
 * Returns 0 on success (regardless of state transition), negative errno
 * on internal error (state machine resets to IDLE on unrecoverable errors).
 */
int ip_intercept_poll_tick(ip_intercept_poll *poll);

/* --- Helpers ------------------------------------------------------------- */

/* Get the current state name as a string (for logging). */
const char *ip_intercept_poll_state_name(ip_poll_state state);

#endif /* CBX_IP_INTERCEPT_POLL_H */