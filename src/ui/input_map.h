/*
 * input_map.h — Input event mapping: InputPlumber InputEvent → SDL events.
 *
 * Bridges the normalized input enum (ip_input_id, from the DBus layer)
 * to synthetic SDL_Event structures that the widget vtable's handle_event
 * can consume.
 *
 * Task 22 — Focus chain system and input event mapping.
 *
 * SPEC §10.2 — DBusDevice InputEvent(event: s, value: d):
 *   Buttons: value 0.0 (release) or 1.0 (press).
 *   Axes:    value -1.0 .. 1.0 (stick position).
 *
 * Mapping strategy:
 *   - Buttons (press) → SDL_KEYDOWN with a controller-semantic SDLK_* code.
 *   - Buttons (release) → SDL_KEYUP with the same code.
 *   - Axes → SDL_KEYDOWN/SDL_KEYUP based on deadzone threshold (0.5).
 *     Positive axis → directional key; negative axis → opposite directional key.
 *     Return-to-center (|value| <= threshold) → SDL_KEYUP for the last direction.
 *
 * The mapping is stateless: each call produces zero or one SDL_Event.
 * The caller dispatches the event via cbx_widget_handle_event().
 */
#ifndef CBX_INPUT_MAP_H
#define CBX_INPUT_MAP_H

#include <SDL2/SDL.h>
#include <stdbool.h>

#include "dbus/ip_input_signal.h"  /* ip_input_id, ip_input_category */

/*
 * Deadzone threshold for axis-to-key conversion.
 * |value| > threshold generates a directional keydown;
 * |value| <= threshold generates a directional keyup.
 */
#define CBX_INPUT_AXIS_THRESHOLD 0.5

/*
 * Convert a normalized input event to a synthetic SDL_Event.
 *
 * For buttons (category == IP_INPUT_CAT_BUTTON):
 *   value == 1.0 → SDL_KEYDOWN, value == 0.0 → SDL_KEYUP.
 * For axes (category == IP_INPUT_CAT_AXIS):
 *   |value| > threshold → SDL_KEYDOWN in the axis direction.
 *   |value| <= threshold → SDL_KEYUP for the last active direction.
 *
 * `out` must point to a valid SDL_Event.  On success, `out` is populated
 * and the function returns true.  If the input is unknown, the value is
 * within the deadzone (for axes), or the input has no mapping, returns
 * false and `out` is left unchanged.
 */
bool cbx_input_map_to_sdl_event(ip_input_id input,
                                 ip_input_category category,
                                 double value,
                                 SDL_Event *out);

/*
 * Get the SDL_Keycode that an ip_input_id maps to.
 *
 * D-pad, face buttons, center buttons, shoulders, and stick clicks
 * map to semantic SDLK_* codes.  Stick axes (LeftStickX/Y, RightStickX/Y)
 * do not have a single keycode — they map to directional keys based on
 * the value sign — and this function returns SDLK_UNKNOWN for them.
 *
 * Returns SDLK_UNKNOWN for IP_INPUT_UNKNOWN and axis inputs.
 */
SDL_Keycode cbx_input_map_keycode(ip_input_id input);

/*
 * Get the ip_input_id that an axis maps to when the value exceeds the
 * threshold in the positive or negative direction.
 *
 * For example, LeftStickY with value > threshold maps to IP_INPUT_UP
 * (positive Y = up in InputPlumber's convention).
 *
 * Returns IP_INPUT_UNKNOWN if the axis has no directional mapping.
 */
ip_input_id cbx_input_map_axis_direction(ip_input_id axis, double value);

#endif /* CBX_INPUT_MAP_H */