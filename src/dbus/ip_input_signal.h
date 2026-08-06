/*
 * ip_input_signal.h — InputEvent signal handling (Task 14).
 *
 * Subscribes to org.shadowblip.Input.DBusDevice.InputEvent signals and
 * dispatches validated input events to a user-provided callback.
 *
 * SPEC §10.2 — DBusDevice interface:
 *   InputEvent(event: s, value: d)
 *   Emitted during intercept mode (InterceptMode = ALL or GAMEPAD_ONLY).
 *   `event` is the input event name (e.g. "A", "Up", "LeftStickX").
 *   `value` is a double: buttons = 0.0 or 1.0; axes = -1.0..1.0.
 *
 * The handler:
 *   1. Verifies the signal sender matches InputPlumber's unique bus name.
 *   2. Parses the event string into a normalized input enum (ip_input_id).
 *   3. Validates the value (buttons: 0.0 or 1.0; axes: clamped to [-1,1]).
 *   4. Rate-limits to max 200 events/second per device.
 *   5. Fires the user callback with the parsed input, validated value,
 *      raw event string, and device path.
 *
 * Unknown event strings are silently dropped (not passed to the callback).
 */
#ifndef CBX_IP_INPUT_SIGNAL_H
#define CBX_IP_INPUT_SIGNAL_H

#include "dbus_mock.h"          /* ip_dbus_backend, ip_bus_handle, constants */

#include <stdbool.h>

/* --- Input event enum ---------------------------------------------------- */

/*
 * Normalized input identifiers derived from InputPlumber's InputEvent
 * signal strings.  The enum covers the standard gamepad button and axis
 * events.  Unknown events map to IP_INPUT_UNKNOWN.
 */
typedef enum {
    IP_INPUT_UNKNOWN = 0,

    /* D-pad (buttons) */
    IP_INPUT_UP,
    IP_INPUT_DOWN,
    IP_INPUT_LEFT,
    IP_INPUT_RIGHT,

    /* Face buttons */
    IP_INPUT_A,
    IP_INPUT_B,
    IP_INPUT_X,
    IP_INPUT_Y,

    /* Center buttons */
    IP_INPUT_START,
    IP_INPUT_SELECT,
    IP_INPUT_GUIDE,

    /* Shoulders / triggers */
    IP_INPUT_L1,         /* bumper (button) */
    IP_INPUT_R1,         /* bumper (button) */
    IP_INPUT_L2,         /* trigger (button or axis) */
    IP_INPUT_R2,         /* trigger (button or axis) */

    /* Stick clicks (buttons) */
    IP_INPUT_L3,
    IP_INPUT_R3,

    /* Stick axes */
    IP_INPUT_LEFT_STICK_X,
    IP_INPUT_LEFT_STICK_Y,
    IP_INPUT_RIGHT_STICK_X,
    IP_INPUT_RIGHT_STICK_Y,
} ip_input_id;

/* Maximum number of input IDs (for array sizing). */
#define IP_INPUT_ID_MAX (IP_INPUT_RIGHT_STICK_Y + 1)

/* --- Input category ----------------------------------------------------- */

/*
 * Whether an input is a button (value 0.0 or 1.0) or an axis (value -1..1).
 * L2 and R2 can be either depending on the controller; the category is
 * determined by the parsed event string, not the value.
 */
typedef enum {
    IP_INPUT_CAT_BUTTON = 0,
    IP_INPUT_CAT_AXIS   = 1,
} ip_input_category;

/* --- Rate limiting ------------------------------------------------------ */

/* Maximum events per second per device. */
#define IP_INPUT_MAX_RATE 200

/* Maximum number of devices tracked by the rate limiter. */
#define IP_INPUT_MAX_DEVICES 64

/* --- Callback type ------------------------------------------------------ */

/*
 * Called for each validated input event.
 * `input` is the normalized input ID.
 * `category` is BUTTON or AXIS.
 * `value` is the validated event value.
 * `raw_event` is the original event string from the signal.
 * `device_path` is the DBusDevice object path.
 */
typedef void (*ip_input_event_cb)(ip_input_id input,
                                    ip_input_category category,
                                    double value,
                                    const char *raw_event,
                                    const char *device_path,
                                    void *userdata);

/* --- Input event handler ------------------------------------------------ */

/*
 * Per-device rate limiter entry.  Tracks events within a sliding window.
 */
typedef struct {
    char    device_path[256];  /* DBusDevice object path */
    int     event_count;        /* events seen in current window */
    long    window_start_ms;    /* window start time in milliseconds */
    bool    in_use;             /* whether this slot is occupied */
} ip_rate_limiter_entry;

typedef struct {
    const ip_dbus_backend *backend;
    ip_bus_handle          bus;
    const char            *expected_sender;
    ip_input_event_cb      cb;
    void                  *cb_userdata;

    /* Rate limiter state (per-device). */
    ip_rate_limiter_entry  rate_limiters[IP_INPUT_MAX_DEVICES];
} ip_input_events;

/* Initialise the input event handler.  Does not subscribe yet. */
void ip_input_events_init(ip_input_events *ie,
                            const ip_dbus_backend *backend,
                            ip_bus_handle bus,
                            const char *expected_sender,
                            ip_input_event_cb cb,
                            void *cb_userdata);

/* Subscribe to InputEvent signals on the DBusDevice interface.
 * Returns 0 on success, negative errno on failure. */
int ip_input_events_subscribe(ip_input_events *ie);

/*
 * Process an InputEvent payload.
 * Validates sender, parses the event string, validates the value,
 * and applies rate limiting.  On success, fires the user callback.
 * On any validation failure or rate-limit hit, silently drops the event.
 */
void ip_input_events_handle(ip_input_events *ie,
                              const ip_input_event_payload *payload);

/* --- Parsing helpers (exposed for unit testing) ------------------------- */

/*
 * Parse a raw event string into a normalized input ID.
 * Returns IP_INPUT_UNKNOWN for unrecognized strings.
 * Comparison is case-sensitive and matches InputPlumber's event names.
 */
ip_input_id ip_input_parse(const char *event);

/*
 * Get the category (BUTTON or AXIS) for a given input ID.
 * L2 and R2 are treated as BUTTON by default (they can also be axes,
 * but the category is determined by the event string, not the value).
 */
ip_input_category ip_input_category_of(ip_input_id input);

/*
 * Validate an event value against the expected range for the category.
 * Buttons: value must be 0.0 or 1.0.
 * Axes: value clamped to [-1.0, 1.0]; any finite value within range is valid.
 * Returns true if valid, false otherwise.
 */
bool ip_input_validate_value(ip_input_category category, double value);

/* Reset the rate limiter state (exposed for testing). */
void ip_input_events_reset_rate_limiters(ip_input_events *ie);

#endif /* CBX_IP_INPUT_SIGNAL_H */