/*
 * input_map.c — Input event mapping: InputPlumber InputEvent → SDL events.
 *
 * Task 22 — Focus chain system and input event mapping.
 */
#include "ui/input_map.h"

#include <math.h>
#include <string.h>

/* --- Button → SDL_Keycode mapping table ----------------------------------- */

/*
 * Maps button-type ip_input_id values to SDL_Keycode.
 * These keycodes are chosen to match what the existing widget vtable
 * expects in handle_event (SDLK_UP/DOWN/LEFT/RIGHT for navigation,
 * SDLK_RETURN for activate, SDLK_ESCAPE for close, etc.).
 */
static const struct {
    ip_input_id  input;
    SDL_Keycode   key;
} s_button_map[] = {
    /* D-pad */
    { IP_INPUT_UP,      SDLK_UP },
    { IP_INPUT_DOWN,    SDLK_DOWN },
    { IP_INPUT_LEFT,    SDLK_LEFT },
    { IP_INPUT_RIGHT,   SDLK_RIGHT },

    /* Face buttons */
    { IP_INPUT_A,       SDLK_RETURN },     /* A = activate / select */
    { IP_INPUT_B,       SDLK_ESCAPE },     /* B = close overlay (SPEC §4.1) */
    { IP_INPUT_X,       SDLK_x },
    { IP_INPUT_Y,       SDLK_y },

    /* Center buttons */
    { IP_INPUT_START,   SDLK_TAB },
    { IP_INPUT_SELECT,  SDLK_BACKSPACE },
    { IP_INPUT_GUIDE,   SDLK_MENU },

    /* Shoulders / triggers */
    { IP_INPUT_L1,      SDLK_PAGEUP },
    { IP_INPUT_R1,      SDLK_PAGEDOWN },
    { IP_INPUT_L2,      SDLK_l },
    { IP_INPUT_R2,      SDLK_r },

    /* Stick clicks */
    { IP_INPUT_L3,      SDLK_F1 },
    { IP_INPUT_R3,      SDLK_F2 },          /* R3 = Host Mode toggle (SPEC §4.4) */
};

#define S_BUTTON_MAP_LEN \
    (int)(sizeof(s_button_map) / sizeof(s_button_map[0]))

/* --- Axis → directional input mapping ----------------------------------- */

/*
 * Maps axis-type ip_input_id values to directional ip_input_id values
 * based on the sign of the axis value.
 *
 * InputPlumber convention:
 *   LeftStickY:  positive = up,   negative = down
 *   LeftStickX:  positive = right, negative = left
 *   RightStickY: positive = up,   negative = down
 *   RightStickX: positive = right, negative = left
 */
static const struct axis_map_entry {
    ip_input_id axis;
    ip_input_id positive_dir;
    ip_input_id negative_dir;
} s_axis_map[] = {
    { IP_INPUT_LEFT_STICK_X,  IP_INPUT_RIGHT, IP_INPUT_LEFT },
    { IP_INPUT_LEFT_STICK_Y,  IP_INPUT_UP,    IP_INPUT_DOWN },
    { IP_INPUT_RIGHT_STICK_X, IP_INPUT_RIGHT, IP_INPUT_LEFT },
    { IP_INPUT_RIGHT_STICK_Y, IP_INPUT_UP,    IP_INPUT_DOWN },
};

#define S_AXIS_MAP_LEN \
    (int)(sizeof(s_axis_map) / sizeof(s_axis_map[0]))

/* --- Helpers -------------------------------------------------------------- */

static SDL_Keycode
lookup_button_key(ip_input_id input)
{
    for (int i = 0; i < S_BUTTON_MAP_LEN; i++) {
        if (s_button_map[i].input == input)
            return s_button_map[i].key;
    }
    return SDLK_UNKNOWN;
}

static const struct axis_map_entry *
lookup_axis_entry(ip_input_id axis)
{
    for (int i = 0; i < S_AXIS_MAP_LEN; i++) {
        if (s_axis_map[i].axis == axis)
            return &s_axis_map[i];
    }
    return NULL;
}

/* --- Public API ---------------------------------------------------------- */

SDL_Keycode
cbx_input_map_keycode(ip_input_id input)
{
    return lookup_button_key(input);
}

ip_input_id
cbx_input_map_axis_direction(ip_input_id axis, double value)
{
    const struct axis_map_entry *entry = lookup_axis_entry(axis);
    if (!entry)
        return IP_INPUT_UNKNOWN;

    if (value > CBX_INPUT_AXIS_THRESHOLD)
        return entry->positive_dir;
    if (value < -CBX_INPUT_AXIS_THRESHOLD)
        return entry->negative_dir;
    return IP_INPUT_UNKNOWN;
}

bool
cbx_input_map_to_sdl_event(ip_input_id input,
                            ip_input_category category,
                            double value,
                            SDL_Event *out)
{
    if (!out)
        return false;

    if (input == IP_INPUT_UNKNOWN)
        return false;

    if (category == IP_INPUT_CAT_BUTTON) {
        /* Button: value 1.0 = press, 0.0 = release. */
        SDL_Keycode key = lookup_button_key(input);
        if (key == SDLK_UNKNOWN)
            return false;

        /* Accept exact 0.0 and 1.0; also accept near-values (float tolerance). */
        bool press;
        if (value >= 0.5)
            press = true;
        else if (value < 0.5)
            press = false;
        else
            return false;

        memset(out, 0, sizeof(*out));
        out->type = press ? SDL_KEYDOWN : SDL_KEYUP;
        out->key.windowID = CBX_CONTROLLER_EVENT_WINDOW_ID;
        out->key.keysym.sym = key;
        out->key.keysym.scancode = SDL_GetScancodeFromKey(key);
        out->key.state = press ? SDL_PRESSED : SDL_RELEASED;
        return true;
    }

    if (category == IP_INPUT_CAT_AXIS) {
        /* Axis: convert to directional key events based on threshold. */
        ip_input_id dir = cbx_input_map_axis_direction(input, value);
        if (dir == IP_INPUT_UNKNOWN)
            return false;   /* within deadzone — no event */

        SDL_Keycode key = lookup_button_key(dir);
        if (key == SDLK_UNKNOWN)
            return false;

        memset(out, 0, sizeof(*out));
        out->type = SDL_KEYDOWN;
        out->key.windowID = CBX_CONTROLLER_EVENT_WINDOW_ID;
        out->key.keysym.sym = key;
        out->key.keysym.scancode = SDL_GetScancodeFromKey(key);
        out->key.state = SDL_PRESSED;
        return true;
    }

    return false;
}