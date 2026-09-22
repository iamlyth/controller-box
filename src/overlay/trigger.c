/*
 * trigger.c — Overlay trigger registration (Task 32, SPEC §2.5, §4.2).
 *
 * Implements trigger string parsing and SetInterceptActivation +
 * InterceptMode=PASS registration on composite devices.
 */
#include "overlay/trigger.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "dbus/ip_composite.h"

/* --- Helpers ---------------------------------------------------------- */

/*
 * Skip leading/trailing whitespace in a token.  Returns pointer to the
 * first non-whitespace character and writes the trimmed length to *out_len.
 */
static const char *
trim_token(const char *s, size_t *out_len)
{
    /* Skip leading whitespace. */
    while (*s && isspace((unsigned char)*s))
        s++;
    size_t len = strlen(s);
    /* Trim trailing whitespace. */
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        len--;
    *out_len = len;
    return s;
}

/* Map a controller-box display name to the InputPlumber Capability string
 * that the live engine accepts.  InputPlumber's SetInterceptActivation
 * rejects every activation/target event that is not a Button capability
 * (it filters on the literal "Button" substring), so the settings-level
 * names ("A", "Select", ...) must be translated before the DBus call.
 *
 * A token that is already a capability string (contains ':') or a Keyboard
 * capability is returned unchanged, and an unknown short name is passed
 * through so a forward-compatible event name is not silently dropped. */
static const char *
trigger_token_to_capability(const char *token)
{
    static const struct {
        const char *name;
        const char *capability;
    } map[] = {
        { "A",           "Gamepad:Button:South" },
        { "B",           "Gamepad:Button:East" },
        { "X",           "Gamepad:Button:North" },
        { "Y",           "Gamepad:Button:West" },
        { "Start",       "Gamepad:Button:Start" },
        { "Select",      "Gamepad:Button:Select" },
        { "Back",        "Gamepad:Button:Select" },
        { "Guide",       "Gamepad:Button:Guide" },
        { "Home",        "Gamepad:Button:Guide" },
        { "Up",          "Gamepad:Button:DPadUp" },
        { "Down",        "Gamepad:Button:DPadDown" },
        { "Left",        "Gamepad:Button:DPadLeft" },
        { "Right",       "Gamepad:Button:DPadRight" },
        { "L1",          "Gamepad:Button:LeftBumper" },
        { "R1",          "Gamepad:Button:RightBumper" },
        { "LeftBumper",  "Gamepad:Button:LeftBumper" },
        { "RightBumper", "Gamepad:Button:RightBumper" },
        { "L2",          "Gamepad:Button:LeftTrigger" },
        { "R2",          "Gamepad:Button:RightTrigger" },
        { "LeftTrigger", "Gamepad:Button:LeftTrigger" },
        { "RightTrigger","Gamepad:Button:RightTrigger" },
        { "L3",          "Gamepad:Button:LeftStick" },
        { "R3",          "Gamepad:Button:RightStick" },
        { "LeftStick",   "Gamepad:Button:LeftStick" },
        { "RightStick",  "Gamepad:Button:RightStick" },
    };

    if (!token)
        return NULL;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(token, map[i].name) == 0)
            return map[i].capability;
    }
    return token;
}

/* Translate the parsed events CSV into the Capability-string CSV the real
 * engine accepts.  Returns 0 on success, -ENAMETOOLONG if the output does
 * not fit. */
static int
trigger_translate_to_capabilities(const char *events_csv,
                                  char *out_csv, size_t out_csv_len)
{
    if (!events_csv || !out_csv)
        return -EINVAL;

    out_csv[0] = '\0';
    size_t pos = 0;
    bool first = true;

    const char *p = events_csv;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        char token[128];
        if (len >= sizeof(token))
            return -ENAMETOOLONG;
        memcpy(token, p, len);
        token[len] = '\0';

        const char *cap = trigger_token_to_capability(token);
        if (!cap)
            return -EINVAL;

        size_t cap_len = strlen(cap);
        if (!first) {
            if (pos + 1 >= out_csv_len)
                return -ENAMETOOLONG;
            out_csv[pos++] = ',';
        }
        if (pos + cap_len >= out_csv_len)
            return -ENAMETOOLONG;
        memcpy(out_csv + pos, cap, cap_len);
        pos += cap_len;
        first = false;

        if (!comma)
            break;
        p = comma + 1;
    }

    out_csv[pos] = '\0';
    return 0;
}

/* --- Public API -------------------------------------------------------- */

int
cbx_trigger_parse(const char *trigger,
                  char *events_csv, size_t csv_len,
                  char *target, size_t target_len)
{
    if (!trigger || !events_csv || !target)
        return -EINVAL;
    if (csv_len == 0 || target_len == 0)
        return -EINVAL;

    events_csv[0] = '\0';
    size_t csv_pos = 0;
    bool first = true;

    /* Copy the full trigger string as the target event. */
    if (strlen(trigger) >= target_len)
        return -ENAMETOOLONG;
    strncpy(target, trigger, target_len - 1);
    target[target_len - 1] = '\0';

    /* Parse events: split on '+', trim each, join with ','. */
    const char *p = trigger;
    while (*p) {
        /* Find the next '+' delimiter. */
        const char *plus = strchr(p, '+');
        size_t tok_len;
        const char *tok_start;
        /* tmp must outlive the if-block: tok_start may point into it
         * after trim_token returns, and is used in the memcpy below. */
        char tmp[128];
        if (plus) {
            /* Extract substring p..plus */
            size_t seg = (size_t)(plus - p);
            if (seg >= sizeof(tmp))
                seg = sizeof(tmp) - 1;
            memcpy(tmp, p, seg);
            tmp[seg] = '\0';
            tok_start = trim_token(tmp, &tok_len);
        } else {
            /* Last token (no '+'). */
            tok_start = trim_token(p, &tok_len);
        }

        if (tok_len > 0) {
            /* Append comma separator if not first. */
            if (!first) {
                if (csv_pos + 1 >= csv_len)
                    return -ENAMETOOLONG;
                events_csv[csv_pos++] = ',';
            }
            if (csv_pos + tok_len >= csv_len)
                return -ENAMETOOLONG;
            memcpy(events_csv + csv_pos, tok_start, tok_len);
            csv_pos += tok_len;
            first = false;
        }

        if (!plus)
            break;
        p = plus + 1;
    }

    events_csv[csv_pos] = '\0';

    /* At least one event is required. */
    if (csv_pos == 0)
        return -EINVAL;

    return 0;
}

int
cbx_trigger_register(const ip_dbus_backend *backend,
                      ip_bus_handle bus,
                      const char *composite_path,
                      const char *trigger_str)
{
    int rc = cbx_trigger_register_only(backend, bus, composite_path,
                                        trigger_str);
    if (rc < 0)
        return rc;

    /* Set InterceptMode = 1 (PASS). */
    rc = ip_composite_set_intercept_mode(backend, bus, composite_path, "1");
    if (rc < 0)
        return rc;

    return 0;
}

int
cbx_trigger_register_only(const ip_dbus_backend *backend,
                          ip_bus_handle bus,
                          const char *composite_path,
                          const char *trigger_str)
{
    if (!backend || !composite_path || !trigger_str)
        return -EINVAL;

    /* Parse the trigger string into events CSV + target. */
    char events_csv[CBX_TRIGGER_CSV_LEN];
    char target[CBX_TRIGGER_TARGET_LEN];
    int rc = cbx_trigger_parse(trigger_str, events_csv, sizeof(events_csv),
                               target, sizeof(target));
    if (rc < 0)
        return rc;

    /* Translate the display names into InputPlumber Capability strings.
     * SetInterceptActivation rejects non-Button events on the live engine. */
    char caps_csv[CBX_TRIGGER_CSV_LEN];
    rc = trigger_translate_to_capabilities(events_csv, caps_csv,
                                           sizeof(caps_csv));
    if (rc < 0)
        return rc;

    /* No capability target: the engine would emit the target event over DBus
     * after switching to intercept mode Always, which would echo a real
     * button back into the overlay as spurious input.  Passing the original
     * combo (not a Button capability) makes the engine select
     * Capability::None, consuming the chord without emitting anything. */
    return ip_composite_set_intercept_activation(backend, bus,
                                                  composite_path,
                                                  caps_csv, target);
}

int
cbx_trigger_register_all(const ip_dbus_backend *backend,
                           ip_bus_handle bus,
                           const char *paths[], int count,
                           const char *trigger_str)
{
    if (!backend || !paths || !trigger_str)
        return -EINVAL;
    if (count <= 0)
        return -EINVAL;

    int failures = 0;
    for (int i = 0; i < count; i++) {
        if (!paths[i]) {
            failures++;
            continue;
        }
        int rc = cbx_trigger_register(backend, bus, paths[i], trigger_str);
        if (rc < 0)
            failures++;
    }

    return failures > 0 ? -failures : 0;
}