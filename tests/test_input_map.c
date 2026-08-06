/*
 * test_input_map.c — Tests for the input event mapping module.
 *
 * Tests that ip_input_id + value → SDL_Event conversion works correctly
 * for buttons (press/release), axes (directional keys), and edge cases
 * (unknown inputs, deadzone, NULL out pointer).
 *
 * Task 22 — Focus chain system and input event mapping.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <setjmp.h>
#include <cmocka.h>

#include <SDL2/SDL.h>
#include <string.h>

#include "ui/input_map.h"
#include "dbus/ip_input_signal.h"  /* ip_input_id, ip_input_category */

/* --- keycode mapping tests ----------------------------------------------- */

static void
test_keycode_dpad(void **state)
{
    (void)state;
    assert_int_equal(cbx_input_map_keycode(IP_INPUT_UP),    SDLK_UP);
    assert_int_equal(cbx_input_map_keycode(IP_INPUT_DOWN),  SDLK_DOWN);
    assert_int_equal(cbx_input_map_keycode(IP_INPUT_LEFT),  SDLK_LEFT);
    assert_int_equal(cbx_input_map_keycode(IP_INPUT_RIGHT), SDLK_RIGHT);
}

static void
test_keycode_face_buttons(void **state)
{
    (void)state;
    assert_int_equal(cbx_input_map_keycode(IP_INPUT_A), SDLK_RETURN);
    assert_int_equal(cbx_input_map_keycode(IP_INPUT_B), SDLK_ESCAPE);
    assert_int_equal(cbx_input_map_keycode(IP_INPUT_X), SDLK_x);
    assert_int_equal(cbx_input_map_keycode(IP_INPUT_Y), SDLK_y);
}

static void
test_keycode_center_buttons(void **state)
{
    (void)state;
    assert_int_equal(cbx_input_map_keycode(IP_INPUT_START),  SDLK_TAB);
    assert_int_equal(cbx_input_map_keycode(IP_INPUT_SELECT), SDLK_BACKSPACE);
    assert_int_equal(cbx_input_map_keycode(IP_INPUT_GUIDE), SDLK_MENU);
}

static void
test_keycode_shoulders(void **state)
{
    (void)state;
    assert_int_equal(cbx_input_map_keycode(IP_INPUT_L1), SDLK_PAGEUP);
    assert_int_equal(cbx_input_map_keycode(IP_INPUT_R1), SDLK_PAGEDOWN);
    assert_int_equal(cbx_input_map_keycode(IP_INPUT_L2), SDLK_l);
    assert_int_equal(cbx_input_map_keycode(IP_INPUT_R2), SDLK_r);
}

static void
test_keycode_stick_clicks(void **state)
{
    (void)state;
    assert_int_equal(cbx_input_map_keycode(IP_INPUT_L3), SDLK_F1);
    assert_int_equal(cbx_input_map_keycode(IP_INPUT_R3), SDLK_F2);
}

static void
test_keycode_axes_unknown(void **state)
{
    (void)state;
    /* Axes don't have a single keycode — they map directionally. */
    assert_int_equal(cbx_input_map_keycode(IP_INPUT_LEFT_STICK_X),  SDLK_UNKNOWN);
    assert_int_equal(cbx_input_map_keycode(IP_INPUT_LEFT_STICK_Y),  SDLK_UNKNOWN);
    assert_int_equal(cbx_input_map_keycode(IP_INPUT_RIGHT_STICK_X), SDLK_UNKNOWN);
    assert_int_equal(cbx_input_map_keycode(IP_INPUT_RIGHT_STICK_Y), SDLK_UNKNOWN);
}

static void
test_keycode_unknown(void **state)
{
    (void)state;
    assert_int_equal(cbx_input_map_keycode(IP_INPUT_UNKNOWN), SDLK_UNKNOWN);
}

/* --- button event generation tests --------------------------------------- */

static void
test_button_press(void **state)
{
    (void)state;
    SDL_Event ev;
    memset(&ev, 0xFF, sizeof(ev));   /* poison */
    bool ok = cbx_input_map_to_sdl_event(IP_INPUT_A, IP_INPUT_CAT_BUTTON,
                                          1.0, &ev);
    assert_true(ok);
    assert_int_equal(ev.type, SDL_KEYDOWN);
    assert_int_equal(ev.key.keysym.sym, SDLK_RETURN);
    assert_int_equal(ev.key.state, SDL_PRESSED);
}

static void
test_button_release(void **state)
{
    (void)state;
    SDL_Event ev;
    memset(&ev, 0xFF, sizeof(ev));
    bool ok = cbx_input_map_to_sdl_event(IP_INPUT_B, IP_INPUT_CAT_BUTTON,
                                          0.0, &ev);
    assert_true(ok);
    assert_int_equal(ev.type, SDL_KEYUP);
    assert_int_equal(ev.key.keysym.sym, SDLK_ESCAPE);
    assert_int_equal(ev.key.state, SDL_RELEASED);
}

static void
test_button_dpad_press(void **state)
{
    (void)state;
    SDL_Event ev;
    bool ok = cbx_input_map_to_sdl_event(IP_INPUT_UP, IP_INPUT_CAT_BUTTON,
                                          1.0, &ev);
    assert_true(ok);
    assert_int_equal(ev.type, SDL_KEYDOWN);
    assert_int_equal(ev.key.keysym.sym, SDLK_UP);
}

static void
test_button_r3_press(void **state)
{
    (void)state;
    SDL_Event ev;
    bool ok = cbx_input_map_to_sdl_event(IP_INPUT_R3, IP_INPUT_CAT_BUTTON,
                                          1.0, &ev);
    assert_true(ok);
    assert_int_equal(ev.type, SDL_KEYDOWN);
    assert_int_equal(ev.key.keysym.sym, SDLK_F2);
}

static void
test_button_release_value_zero(void **state)
{
    (void)state;
    SDL_Event ev;
    bool ok = cbx_input_map_to_sdl_event(IP_INPUT_LEFT, IP_INPUT_CAT_BUTTON,
                                          0.0, &ev);
    assert_true(ok);
    assert_int_equal(ev.type, SDL_KEYUP);
    assert_int_equal(ev.key.keysym.sym, SDLK_LEFT);
}

static void
test_button_threshold_press(void **state)
{
    (void)state;
    /* value >= 0.5 is treated as press. */
    SDL_Event ev;
    bool ok = cbx_input_map_to_sdl_event(IP_INPUT_A, IP_INPUT_CAT_BUTTON,
                                          0.6, &ev);
    assert_true(ok);
    assert_int_equal(ev.type, SDL_KEYDOWN);
}

static void
test_button_threshold_release(void **state)
{
    (void)state;
    /* value < 0.5 is treated as release. */
    SDL_Event ev;
    bool ok = cbx_input_map_to_sdl_event(IP_INPUT_A, IP_INPUT_CAT_BUTTON,
                                          0.4, &ev);
    assert_true(ok);
    assert_int_equal(ev.type, SDL_KEYUP);
}

/* --- axis event generation tests ----------------------------------------- */

static void
test_axis_left_stick_up(void **state)
{
    (void)state;
    SDL_Event ev;
    bool ok = cbx_input_map_to_sdl_event(IP_INPUT_LEFT_STICK_Y,
                                          IP_INPUT_CAT_AXIS, 1.0, &ev);
    assert_true(ok);
    assert_int_equal(ev.type, SDL_KEYDOWN);
    assert_int_equal(ev.key.keysym.sym, SDLK_UP);
}

static void
test_axis_left_stick_down(void **state)
{
    (void)state;
    SDL_Event ev;
    bool ok = cbx_input_map_to_sdl_event(IP_INPUT_LEFT_STICK_Y,
                                          IP_INPUT_CAT_AXIS, -1.0, &ev);
    assert_true(ok);
    assert_int_equal(ev.type, SDL_KEYDOWN);
    assert_int_equal(ev.key.keysym.sym, SDLK_DOWN);
}

static void
test_axis_left_stick_right(void **state)
{
    (void)state;
    SDL_Event ev;
    bool ok = cbx_input_map_to_sdl_event(IP_INPUT_LEFT_STICK_X,
                                          IP_INPUT_CAT_AXIS, 0.8, &ev);
    assert_true(ok);
    assert_int_equal(ev.type, SDL_KEYDOWN);
    assert_int_equal(ev.key.keysym.sym, SDLK_RIGHT);
}

static void
test_axis_left_stick_left(void **state)
{
    (void)state;
    SDL_Event ev;
    bool ok = cbx_input_map_to_sdl_event(IP_INPUT_LEFT_STICK_X,
                                          IP_INPUT_CAT_AXIS, -0.7, &ev);
    assert_true(ok);
    assert_int_equal(ev.type, SDL_KEYDOWN);
    assert_int_equal(ev.key.keysym.sym, SDLK_LEFT);
}

static void
test_axis_right_stick_up(void **state)
{
    (void)state;
    SDL_Event ev;
    bool ok = cbx_input_map_to_sdl_event(IP_INPUT_RIGHT_STICK_Y,
                                          IP_INPUT_CAT_AXIS, 1.0, &ev);
    assert_true(ok);
    assert_int_equal(ev.key.keysym.sym, SDLK_UP);
}

static void
test_axis_right_stick_left(void **state)
{
    (void)state;
    SDL_Event ev;
    bool ok = cbx_input_map_to_sdl_event(IP_INPUT_RIGHT_STICK_X,
                                          IP_INPUT_CAT_AXIS, -1.0, &ev);
    assert_true(ok);
    assert_int_equal(ev.key.keysym.sym, SDLK_LEFT);
}

static void
test_axis_deadzone_no_event(void **state)
{
    (void)state;
    SDL_Event ev;
    /* Within deadzone (|value| <= 0.5): no event. */
    bool ok = cbx_input_map_to_sdl_event(IP_INPUT_LEFT_STICK_Y,
                                          IP_INPUT_CAT_AXIS, 0.3, &ev);
    assert_false(ok);

    ok = cbx_input_map_to_sdl_event(IP_INPUT_LEFT_STICK_Y,
                                     IP_INPUT_CAT_AXIS, -0.5, &ev);
    assert_false(ok);

    ok = cbx_input_map_to_sdl_event(IP_INPUT_LEFT_STICK_Y,
                                     IP_INPUT_CAT_AXIS, 0.0, &ev);
    assert_false(ok);
}

static void
test_axis_at_threshold(void **state)
{
    (void)state;
    SDL_Event ev;
    /* value > 0.5 → keydown; value == 0.5 → no event (deadzone boundary). */
    bool ok = cbx_input_map_to_sdl_event(IP_INPUT_LEFT_STICK_Y,
                                          IP_INPUT_CAT_AXIS, 0.51, &ev);
    assert_true(ok);
    assert_int_equal(ev.key.keysym.sym, SDLK_UP);

    ok = cbx_input_map_to_sdl_event(IP_INPUT_LEFT_STICK_Y,
                                     IP_INPUT_CAT_AXIS, 0.5, &ev);
    assert_false(ok);
}

/* --- axis direction helper tests ----------------------------------------- */

static void
test_axis_direction_positive(void **state)
{
    (void)state;
    assert_int_equal(cbx_input_map_axis_direction(IP_INPUT_LEFT_STICK_Y, 1.0),
                      IP_INPUT_UP);
    assert_int_equal(cbx_input_map_axis_direction(IP_INPUT_LEFT_STICK_X, 1.0),
                      IP_INPUT_RIGHT);
}

static void
test_axis_direction_negative(void **state)
{
    (void)state;
    assert_int_equal(cbx_input_map_axis_direction(IP_INPUT_LEFT_STICK_Y, -1.0),
                      IP_INPUT_DOWN);
    assert_int_equal(cbx_input_map_axis_direction(IP_INPUT_LEFT_STICK_X, -1.0),
                      IP_INPUT_LEFT);
}

static void
test_axis_direction_deadzone(void **state)
{
    (void)state;
    assert_int_equal(cbx_input_map_axis_direction(IP_INPUT_LEFT_STICK_Y, 0.0),
                      IP_INPUT_UNKNOWN);
    assert_int_equal(cbx_input_map_axis_direction(IP_INPUT_LEFT_STICK_Y, 0.5),
                      IP_INPUT_UNKNOWN);
}

static void
test_axis_direction_unknown_axis(void **state)
{
    (void)state;
    /* Non-axis input passed to axis_direction → unknown. */
    assert_int_equal(cbx_input_map_axis_direction(IP_INPUT_A, 1.0),
                      IP_INPUT_UNKNOWN);
    assert_int_equal(cbx_input_map_axis_direction(IP_INPUT_UP, 1.0),
                      IP_INPUT_UNKNOWN);
}

/* --- edge case tests ----------------------------------------------------- */

static void
test_unknown_input_no_event(void **state)
{
    (void)state;
    SDL_Event ev;
    bool ok = cbx_input_map_to_sdl_event(IP_INPUT_UNKNOWN,
                                          IP_INPUT_CAT_BUTTON, 1.0, &ev);
    assert_false(ok);
}

static void
test_null_out_no_event(void **state)
{
    (void)state;
    bool ok = cbx_input_map_to_sdl_event(IP_INPUT_A, IP_INPUT_CAT_BUTTON,
                                          1.0, NULL);
    assert_false(ok);
}

static void
test_all_buttons_mapped(void **state)
{
    (void)state;
    /* Every button-type input should produce a keydown event. */
    ip_input_id buttons[] = {
        IP_INPUT_UP, IP_INPUT_DOWN, IP_INPUT_LEFT, IP_INPUT_RIGHT,
        IP_INPUT_A, IP_INPUT_B, IP_INPUT_X, IP_INPUT_Y,
        IP_INPUT_START, IP_INPUT_SELECT, IP_INPUT_GUIDE,
        IP_INPUT_L1, IP_INPUT_R1, IP_INPUT_L2, IP_INPUT_R2,
        IP_INPUT_L3, IP_INPUT_R3,
    };
    SDL_Event ev;
    for (size_t i = 0; i < sizeof(buttons)/sizeof(buttons[0]); i++) {
        bool ok = cbx_input_map_to_sdl_event(buttons[i], IP_INPUT_CAT_BUTTON,
                                               1.0, &ev);
        assert_true(ok);
        assert_int_equal(ev.type, SDL_KEYDOWN);
        assert_true(ev.key.keysym.sym != SDLK_UNKNOWN);
    }
}

static void
test_all_axes_mapped(void **state)
{
    (void)state;
    ip_input_id axes[] = {
        IP_INPUT_LEFT_STICK_X, IP_INPUT_LEFT_STICK_Y,
        IP_INPUT_RIGHT_STICK_X, IP_INPUT_RIGHT_STICK_Y,
    };
    SDL_Event ev;
    for (size_t i = 0; i < sizeof(axes)/sizeof(axes[0]); i++) {
        /* Positive direction. */
        bool ok = cbx_input_map_to_sdl_event(axes[i], IP_INPUT_CAT_AXIS,
                                               1.0, &ev);
        assert_true(ok);
        assert_int_equal(ev.type, SDL_KEYDOWN);
        assert_true(ev.key.keysym.sym != SDLK_UNKNOWN);

        /* Negative direction. */
        ok = cbx_input_map_to_sdl_event(axes[i], IP_INPUT_CAT_AXIS,
                                         -1.0, &ev);
        assert_true(ok);
        assert_int_equal(ev.type, SDL_KEYDOWN);
        assert_true(ev.key.keysym.sym != SDLK_UNKNOWN);
    }
}

/* --- main ---------------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* keycode mapping */
        cmocka_unit_test(test_keycode_dpad),
        cmocka_unit_test(test_keycode_face_buttons),
        cmocka_unit_test(test_keycode_center_buttons),
        cmocka_unit_test(test_keycode_shoulders),
        cmocka_unit_test(test_keycode_stick_clicks),
        cmocka_unit_test(test_keycode_axes_unknown),
        cmocka_unit_test(test_keycode_unknown),

        /* button event generation */
        cmocka_unit_test(test_button_press),
        cmocka_unit_test(test_button_release),
        cmocka_unit_test(test_button_dpad_press),
        cmocka_unit_test(test_button_r3_press),
        cmocka_unit_test(test_button_release_value_zero),
        cmocka_unit_test(test_button_threshold_press),
        cmocka_unit_test(test_button_threshold_release),

        /* axis event generation */
        cmocka_unit_test(test_axis_left_stick_up),
        cmocka_unit_test(test_axis_left_stick_down),
        cmocka_unit_test(test_axis_left_stick_right),
        cmocka_unit_test(test_axis_left_stick_left),
        cmocka_unit_test(test_axis_right_stick_up),
        cmocka_unit_test(test_axis_right_stick_left),
        cmocka_unit_test(test_axis_deadzone_no_event),
        cmocka_unit_test(test_axis_at_threshold),

        /* axis direction helper */
        cmocka_unit_test(test_axis_direction_positive),
        cmocka_unit_test(test_axis_direction_negative),
        cmocka_unit_test(test_axis_direction_deadzone),
        cmocka_unit_test(test_axis_direction_unknown_axis),

        /* edge cases */
        cmocka_unit_test(test_unknown_input_no_event),
        cmocka_unit_test(test_null_out_no_event),
        cmocka_unit_test(test_all_buttons_mapped),
        cmocka_unit_test(test_all_axes_mapped),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}