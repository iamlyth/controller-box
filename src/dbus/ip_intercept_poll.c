/*
 * ip_intercept_poll.c — InterceptMode polling state machine (Task 13).
 *
 * Implements the poll state machine declared in ip_intercept_poll.h.
 *
 * The state machine is driven by ip_intercept_poll_tick(), which reads
 * the InterceptMode property and transitions states.  In production, tick
 * is called from the main SDL event loop when a custom timer event fires.
 * In tests, tick is called directly with mock expectations.
 *
 * SPEC §2.5, §10.3, §11; IMPLEMENTATION_PLAN DEC-002 (gap #1 workaround).
 */
#include "ip_intercept_poll.h"
#include "ip_composite.h"     /* IP_INTERCEPT_* constants */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* --- Internal helpers --------------------------------------------------- */

/* SDL timer callback: pushes a custom user event onto the queue. */
static uint32_t
 sdl_timer_cb(uint32_t interval, void *userdata)
{
    ip_intercept_poll *poll = (ip_intercept_poll *)userdata;
    if (!poll || !poll->sdl_event_type)
        return 0;  /* stop timer */

    SDL_Event event;
    SDL_zero(event);
    event.type = poll->sdl_event_type;
    event.user.code = 0;
    event.user.data1 = poll;
    event.user.data2 = NULL;
    SDL_PushEvent(&event);

    return interval;  /* keep timer firing */
}

/* Parse InterceptMode string to uint.  Returns -1 on parse failure. */
static int
parse_intercept_mode(const char *str)
{
    if (!str || !str[0])
        return -1;

    /* The property is uint32; sd-bus returns it as a string. */
    char *end = NULL;
    long val = strtol(str, &end, 10);
    if (!end || *end != '\0' || val < 0 || val > 255)
        return -1;

    return (int)val;
}

/*
 * Fire the error callback and reset to IDLE.
 * `code` is the error code passed to the callback.
 */
static void
poll_error_reset(ip_intercept_poll *poll, int code)
{
    if (poll->error_cb)
        poll->error_cb(code, poll->error_data);

    poll->state = IP_POLL_IDLE;
    poll->error_count = 0;
    poll->timeout_ticks = 0;
}

/* --- Lifecycle ----------------------------------------------------------- */

void
ip_intercept_poll_init(ip_intercept_poll *poll,
                         const ip_dbus_backend *backend,
                         ip_bus_handle bus,
                         const char *composite_path,
                         ip_poll_activating_cb activating_cb,
                         void *activating_data,
                         ip_poll_deactivating_cb deactivating_cb,
                         void *deactivating_data,
                         ip_poll_error_cb error_cb,
                         void *error_data)
{
    if (!poll)
        return;

    memset(poll, 0, sizeof(*poll));
    poll->backend  = backend;
    poll->bus      = bus;
    if (composite_path) {
        strncpy(poll->composite_path, composite_path,
                sizeof(poll->composite_path) - 1);
        poll->composite_path[sizeof(poll->composite_path) - 1] = '\0';
    }
    poll->activating_cb     = activating_cb;
    poll->activating_data   = activating_data;
    poll->deactivating_cb   = deactivating_cb;
    poll->deactivating_data = deactivating_data;
    poll->error_cb          = error_cb;
    poll->error_data        = error_data;
    poll->max_errors        = IP_INTERCEPT_POLL_MAX_ERRORS;
    poll->max_timeout_ticks = IP_INTERCEPT_POLL_TIMEOUT_TICKS;
}

int
ip_intercept_poll_start(ip_intercept_poll *poll,
                         uint32_t interval_ms,
                         uint32_t sdl_event_type)
{
    if (!poll || !poll->backend || !poll->composite_path[0])
        return -EINVAL;

    /* If already polling, stop the existing timer first. */
    if (poll->timer_id)
        ip_intercept_poll_stop(poll);

    poll->sdl_event_type = sdl_event_type;
    poll->state          = IP_POLL_PASS_WAIT;
    poll->error_count    = 0;
    poll->timeout_ticks  = 0;

    poll->timer_id = SDL_AddTimer(interval_ms, sdl_timer_cb, poll);
    if (!poll->timer_id)
        return -errno;

    return 0;
}

void
ip_intercept_poll_stop(ip_intercept_poll *poll)
{
    if (!poll)
        return;

    if (poll->timer_id) {
        SDL_RemoveTimer(poll->timer_id);
        poll->timer_id = 0;
    }

    poll->state = IP_POLL_IDLE;
    poll->error_count = 0;
    poll->timeout_ticks = 0;
}

/* --- State machine tick ------------------------------------------------- */

int
ip_intercept_poll_tick(ip_intercept_poll *poll)
{
    if (!poll || !poll->backend || !poll->composite_path[0])
        return -EINVAL;

    /* If not polling, nothing to do. */
    if (poll->state == IP_POLL_IDLE)
        return 0;

    /* Read InterceptMode property. */
    char *mode_str = NULL;
    int rc = poll->backend->get_property(poll->bus, IP_DBUS_NAME,
                                           poll->composite_path,
                                           IP_IFACE_COMPOSITE,
                                           "InterceptMode", &mode_str);

    if (rc < 0) {
        /* Poll failed — increment error count. */
        poll->error_count++;
        free(mode_str);

        if (poll->error_count >= poll->max_errors) {
            poll_error_reset(poll, rc);
            return rc;
        }
        return 0;  /* transient error, keep polling */
    }

    if (!mode_str) {
        /* No value returned — treat as error. */
        poll->error_count++;
        if (poll->error_count >= poll->max_errors)
            poll_error_reset(poll, -EIO);
        return 0;
    }

    int mode = parse_intercept_mode(mode_str);
    free(mode_str);

    if (mode < 0) {
        /* Parse failure — treat as transient error.  Don't reset
         * error_count here: the property read succeeded but the value
         * is garbage, so the error is persistent until a valid read. */
        poll->error_count++;
        if (poll->error_count >= poll->max_errors)
            poll_error_reset(poll, -EIO);
        return 0;
    }

    /* Valid mode read — reset error count. */
    poll->error_count = 0;

    /* State transitions. */
    switch (poll->state) {
    case IP_POLL_PASS_WAIT:
        /*
         * Waiting for activation.  Expect PASS (1).
         * On ALL (2): fire activating callback, move to ACTIVE.
         * On GAMEPAD_ONLY (3): also treat as activation (intercept active).
         * On NONE (0): unexpected — InputPlumber may have reset.
         *   Increment timeout, eventually error out.
         */
        if (mode == IP_INTERCEPT_ALL || mode == IP_INTERCEPT_GAMEPAD_ONLY) {
            /* Activation detected! */
            poll->state = IP_POLL_ACTIVE;
            poll->timeout_ticks = 0;
            if (poll->activating_cb)
                poll->activating_cb(poll->activating_data);
        } else if (mode == IP_INTERCEPT_NONE) {
            /* Unexpected — InputPlumber reset the mode. */
            poll->timeout_ticks++;
            if (poll->timeout_ticks >= poll->max_timeout_ticks)
                poll_error_reset(poll, -EIO);
        }
        /* mode == PASS: keep waiting (normal). */
        break;

    case IP_POLL_ACTIVE:
        /*
         * Overlay is active.  Expect ALL (or GAMEPAD_ONLY).
         * On PASS (1) or NONE (0): deactivation detected.
         * Timeout if mode stays at ALL/GAMEPAD_ONLY for too long
         * (GUI set PASS but InputPlumber didn't switch — gap #1 edge case).
         */
        if (mode == IP_INTERCEPT_PASS || mode == IP_INTERCEPT_NONE) {
            /* Deactivation detected! */
            poll->state = IP_POLL_IDLE;
            poll->timeout_ticks = 0;
            if (poll->deactivating_cb)
                poll->deactivating_cb(poll->deactivating_data);
        } else {
            /* Still active — check timeout. */
            poll->timeout_ticks++;
            if (poll->timeout_ticks >= poll->max_timeout_ticks)
                poll_error_reset(poll, -ETIMEDOUT);
        }
        break;

    default:
        /* Should not happen. */
        poll->state = IP_POLL_IDLE;
        break;
    }

    return 0;
}

/* --- Helpers ------------------------------------------------------------- */

const char *
ip_intercept_poll_state_name(ip_poll_state state)
{
    switch (state) {
    case IP_POLL_IDLE:      return "IDLE";
    case IP_POLL_PASS_WAIT: return "PASS_WAIT";
    case IP_POLL_ACTIVE:    return "ACTIVE";
    default:                return "UNKNOWN";
    }
}