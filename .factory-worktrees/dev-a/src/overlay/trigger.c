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
    if (!backend || !composite_path || !trigger_str)
        return -EINVAL;

    /* Parse the trigger string into events CSV + target. */
    char events_csv[CBX_TRIGGER_CSV_LEN];
    char target[CBX_TRIGGER_TARGET_LEN];
    int rc = cbx_trigger_parse(trigger_str, events_csv, sizeof(events_csv),
                               target, sizeof(target));
    if (rc < 0)
        return rc;

    /* Call SetInterceptActivation on the composite device. */
    rc = ip_composite_set_intercept_activation(backend, bus,
                                                  composite_path,
                                                  events_csv, target);
    if (rc < 0)
        return rc;

    /* Set InterceptMode = 1 (PASS). */
    rc = ip_composite_set_intercept_mode(backend, bus, composite_path, "1");
    if (rc < 0)
        return rc;

    return 0;
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