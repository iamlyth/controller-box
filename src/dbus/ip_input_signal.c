/*
 * ip_input_signal.c — InputEvent signal handling (Task 14).
 *
 * Implements the InputEvent signal handler declared in ip_input_signal.h.
 *
 * The handler subscribes to InputEvent signals on the DBusDevice interface,
 * validates incoming events, rate-limits them, and dispatches to the user
 * callback.
 *
 * SPEC §10.2 — DBusDevice interface, InputEvent(event: s, value: d).
 *
 * Security:
 *   - Sender verification: payload->sender must match expected_sender.
 *   - Event string is parsed against a known set; unknown events are dropped.
 *   - Value validation: buttons must be 0.0 or 1.0; axes must be in [-1, 1].
 *   - Rate limiting: max 200 events/second per device path.
 */
#include "ip_input_signal.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* --- Event string → input ID mapping table ------------------------------- */

typedef struct {
    const char   *name;      /* InputPlumber event string */
    ip_input_id   id;         /* normalized input ID */
    ip_input_category category;
} input_entry;

/* Known InputPlumber gamepad event strings → normalized IDs.
 *
 * InputPlumber uses standard gamepad button/axis names.  We include
 * common variants for robustness (e.g. "Back" as alias for "Select").
 */
static const input_entry s_input_table[] = {
    /* D-pad buttons */
    { "Up",       IP_INPUT_UP,       IP_INPUT_CAT_BUTTON },
    { "Down",     IP_INPUT_DOWN,     IP_INPUT_CAT_BUTTON },
    { "Left",     IP_INPUT_LEFT,     IP_INPUT_CAT_BUTTON },
    { "Right",    IP_INPUT_RIGHT,    IP_INPUT_CAT_BUTTON },

    /* Face buttons */
    { "A",        IP_INPUT_A,        IP_INPUT_CAT_BUTTON },
    { "B",        IP_INPUT_B,        IP_INPUT_CAT_BUTTON },
    { "X",        IP_INPUT_X,        IP_INPUT_CAT_BUTTON },
    { "Y",        IP_INPUT_Y,        IP_INPUT_CAT_BUTTON },

    /* Center buttons */
    { "Start",    IP_INPUT_START,    IP_INPUT_CAT_BUTTON },
    { "Select",   IP_INPUT_SELECT,   IP_INPUT_CAT_BUTTON },
    { "Back",     IP_INPUT_SELECT,   IP_INPUT_CAT_BUTTON },  /* alias */
    { "Guide",    IP_INPUT_GUIDE,    IP_INPUT_CAT_BUTTON },
    { "Home",     IP_INPUT_GUIDE,    IP_INPUT_CAT_BUTTON },  /* alias */

    /* Shoulders / triggers */
    { "L1",       IP_INPUT_L1,       IP_INPUT_CAT_BUTTON },
    { "R1",       IP_INPUT_R1,       IP_INPUT_CAT_BUTTON },
    { "LeftBumper",  IP_INPUT_L1,    IP_INPUT_CAT_BUTTON },  /* alias */
    { "RightBumper", IP_INPUT_R1,    IP_INPUT_CAT_BUTTON },  /* alias */
    { "L2",       IP_INPUT_L2,       IP_INPUT_CAT_BUTTON },  /* trigger as button */
    { "R2",       IP_INPUT_R2,       IP_INPUT_CAT_BUTTON },  /* trigger as button */
    { "LeftTrigger",  IP_INPUT_L2,   IP_INPUT_CAT_BUTTON },  /* alias */
    { "RightTrigger", IP_INPUT_R2,   IP_INPUT_CAT_BUTTON },  /* alias */

    /* Stick clicks */
    { "L3",       IP_INPUT_L3,       IP_INPUT_CAT_BUTTON },
    { "R3",       IP_INPUT_R3,       IP_INPUT_CAT_BUTTON },
    { "LeftStick",  IP_INPUT_L3,    IP_INPUT_CAT_BUTTON },  /* alias */
    { "RightStick", IP_INPUT_R3,    IP_INPUT_CAT_BUTTON },  /* alias */

    /* Stick axes */
    { "LeftStickX",  IP_INPUT_LEFT_STICK_X,  IP_INPUT_CAT_AXIS },
    { "LeftStickY",  IP_INPUT_LEFT_STICK_Y,  IP_INPUT_CAT_AXIS },
    { "RightStickX", IP_INPUT_RIGHT_STICK_X, IP_INPUT_CAT_AXIS },
    { "RightStickY", IP_INPUT_RIGHT_STICK_Y, IP_INPUT_CAT_AXIS },
};

#define NUM_INPUT_ENTRIES \
    (sizeof(s_input_table) / sizeof(s_input_table[0]))

/* --- Parsing helpers ----------------------------------------------------- */

ip_input_id
ip_input_parse(const char *event)
{
    if (!event)
        return IP_INPUT_UNKNOWN;

    for (size_t i = 0; i < NUM_INPUT_ENTRIES; i++) {
        if (strcmp(s_input_table[i].name, event) == 0)
            return s_input_table[i].id;
    }

    return IP_INPUT_UNKNOWN;
}

ip_input_category
ip_input_category_of(ip_input_id input)
{
    for (size_t i = 0; i < NUM_INPUT_ENTRIES; i++) {
        if (s_input_table[i].id == input)
            return s_input_table[i].category;
    }
    return IP_INPUT_CAT_BUTTON;  /* default */
}

bool
ip_input_validate_value(ip_input_category category, double value)
{
    /* Check for NaN / infinity. */
    if (!isfinite(value))
        return false;

    if (category == IP_INPUT_CAT_BUTTON) {
        /* Buttons must be exactly 0.0 or 1.0. */
        return value == 0.0 || value == 1.0;
    }

    /* Axes: value must be in [-1.0, 1.0]. */
    return value >= -1.0 && value <= 1.0;
}

/* --- Rate limiting ------------------------------------------------------ */

/* Get current monotonic time in milliseconds. */
static long
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Find or create a rate limiter entry for the given device path.
 * Returns NULL if the table is full and the path is not already present. */
static ip_rate_limiter_entry *
find_rate_limiter(ip_input_events *ie, const char *device_path)
{
    int free_slot = -1;

    for (int i = 0; i < IP_INPUT_MAX_DEVICES; i++) {
        if (ie->rate_limiters[i].in_use) {
            if (strcmp(ie->rate_limiters[i].device_path, device_path) == 0)
                return &ie->rate_limiters[i];
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }

    if (free_slot < 0)
        return NULL;  /* table full, unknown device */

    /* Initialise a new slot. */
    ip_rate_limiter_entry *e = &ie->rate_limiters[free_slot];
    snprintf(e->device_path, sizeof(e->device_path), "%s", device_path);
    e->event_count    = 0;
    e->window_start_ms = now_ms();
    e->in_use         = true;
    return e;
}

/*
 * Check if an event from `device_path` is within the rate limit.
 * Returns true if the event should be accepted, false if rate-limited.
 * Updates internal counters.
 */
static bool
rate_limit_check(ip_input_events *ie, const char *device_path)
{
    ip_rate_limiter_entry *e = find_rate_limiter(ie, device_path);
    if (!e)
        return true;  /* can't track — allow the event */

    long now = now_ms();

    /* Reset window if more than 1 second has elapsed. */
    if (now - e->window_start_ms >= 1000) {
        e->window_start_ms = now;
        e->event_count    = 0;
    }

    e->event_count++;

    return e->event_count <= IP_INPUT_MAX_RATE;
}

void
ip_input_events_reset_rate_limiters(ip_input_events *ie)
{
    if (!ie)
        return;
    for (int i = 0; i < IP_INPUT_MAX_DEVICES; i++) {
        ie->rate_limiters[i].in_use = false;
        ie->rate_limiters[i].event_count = 0;
        ie->rate_limiters[i].window_start_ms = 0;
        ie->rate_limiters[i].device_path[0] = '\0';
    }
}

/* --- Signal callback (registered via vtable subscribe_signal) ------------- */

static void
input_event_signal_cb(const char *iface, const char *member,
                       const void *payload, void *userdata)
{
    (void)iface;
    (void)member;
    ip_input_events *ie = (ip_input_events *)userdata;
    const ip_input_event_payload *p =
        (const ip_input_event_payload *)payload;
    if (!ie || !p)
        return;
    ip_input_events_handle(ie, p);
}

/* --- Public API ---------------------------------------------------------- */

void
ip_input_events_init(ip_input_events *ie,
                      const ip_dbus_backend *backend,
                      ip_bus_handle bus,
                      const char *expected_sender,
                      ip_input_event_cb cb,
                      void *cb_userdata)
{
    if (!ie)
        return;
    memset(ie, 0, sizeof(*ie));
    ie->backend         = backend;
    ie->bus             = bus;
    ie->expected_sender = expected_sender;
    ie->cb              = cb;
    ie->cb_userdata      = cb_userdata;
}

int
ip_input_events_subscribe(ip_input_events *ie)
{
    if (!ie || !ie->backend)
        return -EINVAL;

    return ie->backend->subscribe_signal(
        ie->bus, IP_IFACE_DBUS_DEVICE, "InputEvent",
        input_event_signal_cb, ie);
}

void
ip_input_events_handle(ip_input_events *ie,
                        const ip_input_event_payload *payload)
{
    if (!ie || !payload || !ie->cb)
        return;

    /* Sender verification. */
    if (!ie->expected_sender || !payload->sender)
        return;
    if (strcmp(payload->sender, ie->expected_sender) != 0)
        return;

    /* Device path required. */
    if (!payload->path)
        return;

    /* Parse the event string. */
    ip_input_id input = ip_input_parse(payload->event);
    if (input == IP_INPUT_UNKNOWN)
        return;  /* unknown event — silently drop */

    /* Determine category and validate value. */
    ip_input_category cat = ip_input_category_of(input);
    if (!ip_input_validate_value(cat, payload->value))
        return;

    /* Rate limiting (per device path). */
    if (!rate_limit_check(ie, payload->path))
        return;  /* over rate limit — drop */

    /* Dispatch to user callback. */
    ie->cb(input, cat, payload->value, payload->event,
            payload->path, ie->cb_userdata);
}

int
ip_input_events_process(ip_input_events *ie)
{
    if (!ie || !ie->backend || !ie->backend->process)
        return 0;

    int total = 0;
    int rc;
    while ((rc = ie->backend->process(ie->bus)) > 0)
        total += rc;
    return total;
}