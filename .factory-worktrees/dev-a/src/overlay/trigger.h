/*
 * trigger.h — Overlay trigger registration (Task 32, SPEC §2.5, §4.2).
 *
 * At startup the GUI registers the overlay trigger combo on each
 * composite device via SetInterceptActivation and sets InterceptMode = 1
 * (PASS).  InputPlumber watches for the combo at kernel level (~1 ms
 * overhead).  When the combo is detected, InputPlumber auto-switches to
 * InterceptMode = 2 (ALL) and the GUI's poll detects the transition.
 *
 * The trigger string is stored in settings.overlay_trigger (e.g.
 * "Select+A").  The '+' delimiter separates individual event names.
 *
 * This module is pure logic + DBus calls — no file I/O, no SDL.
 * It is fully unit-testable with the mock DBus backend.
 */
#ifndef CBX_OVERLAY_TRIGGER_H
#define CBX_OVERLAY_TRIGGER_H

#include <stddef.h>

#include "dbus/dbus_interface.h"          /* ip_dbus_backend, ip_bus_handle */

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum length of the parsed events CSV string. */
#define CBX_TRIGGER_CSV_LEN  128

/* Maximum length of the target event string. */
#define CBX_TRIGGER_TARGET_LEN 128

/*
 * Parse a trigger combo string (e.g. "Select+A") into the events CSV
 * (e.g. "Select,A") and the target event (the original combo string).
 *
 * The '+' delimiter separates individual button/event names.  Each
 * event name is trimmed of surrounding whitespace.  At least one event
 * is required.
 *
 * @param trigger     NUL-terminated trigger string (e.g. "Select+A").
 * @param events_csv  Output buffer for comma-separated events.
 * @param csv_len     Size of events_csv buffer.
 * @param target      Output buffer for the target event string.
 * @param target_len  Size of target buffer.
 * @return 0 on success; -EINVAL if null args or empty trigger;
 *         -ENAMETOOLONG if output buffer too small.
 */
int cbx_trigger_parse(const char *trigger,
                      char *events_csv, size_t csv_len,
                      char *target, size_t target_len);

/*
 * Register the trigger combo on a single composite device.
 *
 * Calls SetInterceptActivation with the parsed events and target,
 * then sets InterceptMode = 1 (PASS) so InputPlumber watches for the
 * combo but passes input through to the game.
 *
 * @param backend        DBus backend vtable.
 * @param bus            DBus bus handle.
 * @param composite_path Composite device DBus path.
 * @param trigger_str    Trigger combo string (e.g. "Select+A").
 * @return 0 on success; -EINVAL on null args; negative errno from
 *         SetInterceptActivation or set_intercept_mode on failure.
 */
int cbx_trigger_register(const ip_dbus_backend *backend,
                          ip_bus_handle bus,
                          const char *composite_path,
                          const char *trigger_str);

/*
 * Register the trigger combo on multiple composite devices.
 *
 * Calls cbx_trigger_register for each path.  If a registration fails
 * for one device, the function continues with the remaining devices
 * and returns the count of failures as a negative number (e.g. -2
 * means 2 devices failed).  Returns 0 if all succeeded.
 *
 * @param backend  DBus backend vtable.
 * @param bus       DBus bus handle.
 * @param paths     Array of composite device DBus paths.
 * @param count     Number of paths.
 * @param trigger_str  Trigger combo string.
 * @return 0 if all succeeded; negative = -(failures) if some failed;
 *         -EINVAL on null args.
 */
int cbx_trigger_register_all(const ip_dbus_backend *backend,
                               ip_bus_handle bus,
                               const char *paths[], int count,
                               const char *trigger_str);

#ifdef __cplusplus
}
#endif

#endif /* CBX_OVERLAY_TRIGGER_H */