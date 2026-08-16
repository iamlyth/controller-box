/*
 * test_profile_diagram.c — Tests for the controller diagram widget
 * (Task 37).
 *
 * Tests button position lookup, name mapping, highlight state, and
 * rendering with the SDL2 dummy driver.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <SDL2/SDL.h>
#include <string.h>
#include <errno.h>

#include "manager/profile_diagram.h"
#include "test_harness.h"

/* ------------------------------------------------------------------ */
/*  Fixture                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    TestSdlState sdl;
    cbx_profile_diagram diag;
    cbx_theme theme;
} pd_fixture;

static int setup(void **state)
{
    pd_fixture *f = malloc(sizeof(*f));
    memset(f, 0, sizeof(*f));

    int rc = test_harness_sdl_init(&f->sdl);
    assert_int_equal(rc, 0);

    cbx_theme_default(&f->theme);

    rc = cbx_profile_diagram_init(&f->diag, f->sdl.renderer, NULL, &f->theme);
    assert_int_equal(rc, 0);

    *state = f;
    return 0;
}

static int teardown(void **state)
{
    pd_fixture *f = *state;
    cbx_profile_diagram_shutdown(&f->diag);
    test_harness_sdl_shutdown(&f->sdl);
    free(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Tests: button position lookup                                      */
/* ------------------------------------------------------------------ */

static void test_button_count(void **state)
{
    (void)state;
    assert_int_equal(cbx_profile_diagram_button_count(), CBX_DIAG_BTN_COUNT);
    assert_int_equal(CBX_DIAG_BTN_COUNT, 17);
}

static void test_button_pos_valid(void **state)
{
    (void)state;
    const cbx_diag_button_pos *pos;

    pos = cbx_profile_diagram_get_button_pos(CBX_DIAG_BTN_A);
    assert_non_null(pos);
    assert_string_equal(pos->name, "A");
    assert_true(pos->x >= 0.0f && pos->x <= 1.0f);
    assert_true(pos->y >= 0.0f && pos->y <= 1.0f);
    assert_true(pos->w > 0.0f && pos->w <= 1.0f);
    assert_true(pos->h > 0.0f && pos->h <= 1.0f);
}

static void test_button_pos_all_valid(void **state)
{
    (void)state;
    for (int i = 0; i < CBX_DIAG_BTN_COUNT; i++) {
        const cbx_diag_button_pos *pos =
            cbx_profile_diagram_get_button_pos((cbx_diag_button)i);
        assert_non_null(pos);
        assert_non_null(pos->name);
        assert_true(pos->name[0] != '\0');
        assert_true(pos->x >= 0.0f && pos->x <= 1.0f);
        assert_true(pos->y >= 0.0f && pos->y <= 1.0f);
        assert_true(pos->w > 0.0f && pos->w <= 1.0f);
        assert_true(pos->h > 0.0f && pos->h <= 1.0f);
    }
}

static void test_button_pos_invalid(void **state)
{
    (void)state;
    assert_null(cbx_profile_diagram_get_button_pos(CBX_DIAG_BTN_NONE));
    assert_null(cbx_profile_diagram_get_button_pos(-1));
    assert_null(cbx_profile_diagram_get_button_pos(999));
}

/* ------------------------------------------------------------------ */
/*  Tests: name mapping                                                */
/* ------------------------------------------------------------------ */

static void test_button_from_name_known(void **state)
{
    (void)state;
    assert_int_equal(cbx_profile_diagram_button_from_name("A"),
                       CBX_DIAG_BTN_A);
    assert_int_equal(cbx_profile_diagram_button_from_name("B"),
                       CBX_DIAG_BTN_B);
    assert_int_equal(cbx_profile_diagram_button_from_name("X"),
                       CBX_DIAG_BTN_X);
    assert_int_equal(cbx_profile_diagram_button_from_name("Y"),
                       CBX_DIAG_BTN_Y);
    assert_int_equal(cbx_profile_diagram_button_from_name("Up"),
                       CBX_DIAG_BTN_UP);
    assert_int_equal(cbx_profile_diagram_button_from_name("Down"),
                       CBX_DIAG_BTN_DOWN);
    assert_int_equal(cbx_profile_diagram_button_from_name("Left"),
                       CBX_DIAG_BTN_LEFT);
    assert_int_equal(cbx_profile_diagram_button_from_name("Right"),
                       CBX_DIAG_BTN_RIGHT);
    assert_int_equal(cbx_profile_diagram_button_from_name("Start"),
                       CBX_DIAG_BTN_START);
    assert_int_equal(cbx_profile_diagram_button_from_name("Select"),
                       CBX_DIAG_BTN_SELECT);
    assert_int_equal(cbx_profile_diagram_button_from_name("Guide"),
                       CBX_DIAG_BTN_GUIDE);
    assert_int_equal(cbx_profile_diagram_button_from_name("L1"),
                       CBX_DIAG_BTN_L1);
    assert_int_equal(cbx_profile_diagram_button_from_name("R1"),
                       CBX_DIAG_BTN_R1);
    assert_int_equal(cbx_profile_diagram_button_from_name("L2"),
                       CBX_DIAG_BTN_L2);
    assert_int_equal(cbx_profile_diagram_button_from_name("R2"),
                       CBX_DIAG_BTN_R2);
    assert_int_equal(cbx_profile_diagram_button_from_name("L3"),
                       CBX_DIAG_BTN_L3);
    assert_int_equal(cbx_profile_diagram_button_from_name("R3"),
                       CBX_DIAG_BTN_R3);
}

static void test_button_from_name_unknown(void **state)
{
    (void)state;
    assert_int_equal(cbx_profile_diagram_button_from_name("Foo"),
                       CBX_DIAG_BTN_NONE);
    assert_int_equal(cbx_profile_diagram_button_from_name(""),
                       CBX_DIAG_BTN_NONE);
    assert_int_equal(cbx_profile_diagram_button_from_name(NULL),
                       CBX_DIAG_BTN_NONE);
}

static void test_button_name_roundtrip(void **state)
{
    (void)state;
    for (int i = 0; i < CBX_DIAG_BTN_COUNT; i++) {
        cbx_diag_button btn = (cbx_diag_button)i;
        const char *name = cbx_profile_diagram_button_name(btn);
        assert_non_null(name);
        cbx_diag_button back = cbx_profile_diagram_button_from_name(name);
        assert_int_equal(back, btn);
    }
}

static void test_button_name_invalid(void **state)
{
    (void)state;
    assert_null(cbx_profile_diagram_button_name(CBX_DIAG_BTN_NONE));
    assert_null(cbx_profile_diagram_button_name(-1));
    assert_null(cbx_profile_diagram_button_name(999));
}

/* ------------------------------------------------------------------ */
/*  Tests: highlight state                                             */
/* ------------------------------------------------------------------ */

static void test_highlight_set_get(void **state)
{
    pd_fixture *f = *state;

    cbx_profile_diagram_highlight(&f->diag, CBX_DIAG_BTN_A);
    assert_int_equal(cbx_profile_diagram_get_highlight(&f->diag),
                       CBX_DIAG_BTN_A);
}

static void test_highlight_clear(void **state)
{
    pd_fixture *f = *state;

    cbx_profile_diagram_highlight(&f->diag, CBX_DIAG_BTN_START);
    assert_int_equal(cbx_profile_diagram_get_highlight(&f->diag),
                       CBX_DIAG_BTN_START);

    cbx_profile_diagram_clear_highlight(&f->diag);
    assert_int_equal(cbx_profile_diagram_get_highlight(&f->diag),
                       CBX_DIAG_BTN_NONE);
}

static void test_highlight_none(void **state)
{
    pd_fixture *f = *state;

    /* Initially no highlight */
    assert_int_equal(cbx_profile_diagram_get_highlight(&f->diag),
                       CBX_DIAG_BTN_NONE);

    /* Setting to NONE clears */
    cbx_profile_diagram_highlight(&f->diag, CBX_DIAG_BTN_B);
    cbx_profile_diagram_highlight(&f->diag, CBX_DIAG_BTN_NONE);
    assert_int_equal(cbx_profile_diagram_get_highlight(&f->diag),
                       CBX_DIAG_BTN_NONE);
}

static void test_highlight_all_buttons(void **state)
{
    pd_fixture *f = *state;

    for (int i = 0; i < CBX_DIAG_BTN_COUNT; i++) {
        cbx_diag_button btn = (cbx_diag_button)i;
        cbx_profile_diagram_highlight(&f->diag, btn);
        assert_int_equal(cbx_profile_diagram_get_highlight(&f->diag), btn);
    }
}

static void test_highlight_invalid(void **state)
{
    pd_fixture *f = *state;

    /* Invalid button IDs should be ignored */
    cbx_profile_diagram_highlight(&f->diag, CBX_DIAG_BTN_A);
    cbx_profile_diagram_highlight(&f->diag, (cbx_diag_button)999);
    assert_int_equal(cbx_profile_diagram_get_highlight(&f->diag),
                       CBX_DIAG_BTN_A);
}

static void test_highlight_null_safe(void **state)
{
    (void)state;
    /* NULL-safe operations */
    cbx_profile_diagram_highlight(NULL, CBX_DIAG_BTN_A);
    cbx_profile_diagram_clear_highlight(NULL);
    assert_int_equal(cbx_profile_diagram_get_highlight(NULL),
                       CBX_DIAG_BTN_NONE);
}

/* ------------------------------------------------------------------ */
/*  Tests: lifecycle                                                   */
/* ------------------------------------------------------------------ */

static void test_init_basic(void **state)
{
    (void)state;
    cbx_theme theme;
    cbx_theme_default(&theme);

    cbx_profile_diagram diag;
    int rc = cbx_profile_diagram_init(&diag, NULL, NULL, &theme);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_profile_diagram_get_highlight(&diag),
                       CBX_DIAG_BTN_NONE);
    assert_true(diag.base.visible);
    cbx_profile_diagram_shutdown(&diag);
}

static void test_init_null_args(void **state)
{
    (void)state;
    assert_int_equal(cbx_profile_diagram_init(NULL, NULL, NULL, NULL),
                       -EINVAL);
}

static void test_shutdown_null_safe(void **state)
{
    (void)state;
    cbx_profile_diagram diag;
    memset(&diag, 0, sizeof(diag));
    cbx_profile_diagram_shutdown(&diag);
    /* should not crash */
}

static void test_shutdown_cleans_up(void **state)
{
    pd_fixture *f = *state;

    cbx_profile_diagram_highlight(&f->diag, CBX_DIAG_BTN_X);
    cbx_profile_diagram_shutdown(&f->diag);
    /* After shutdown, struct is zeroed — no crash is the main check */
    assert_true(1);
}

/* ------------------------------------------------------------------ */
/*  Tests: rendering                                                   */
/* ------------------------------------------------------------------ */

static void test_render_no_crash(void **state)
{
    pd_fixture *f = *state;

    /* Render without highlight */
    cbx_widget_draw(&f->diag.base, f->sdl.renderer);

    /* Render with highlight */
    cbx_profile_diagram_highlight(&f->diag, CBX_DIAG_BTN_A);
    cbx_widget_draw(&f->diag.base, f->sdl.renderer);

    /* Should not crash */
    assert_true(1);
}

static void test_render_all_buttons(void **state)
{
    pd_fixture *f = *state;

    for (int i = 0; i < CBX_DIAG_BTN_COUNT; i++) {
        cbx_profile_diagram_highlight(&f->diag, (cbx_diag_button)i);
        cbx_widget_draw(&f->diag.base, f->sdl.renderer);
    }
    /* should not crash for any button */
    assert_true(1);
}

static void test_render_with_rect(void **state)
{
    pd_fixture *f = *state;

    SDL_Rect r = { 0, 0, 400, 400 };
    cbx_widget_set_rect(&f->diag.base, &r);

    cbx_profile_diagram_highlight(&f->diag, CBX_DIAG_BTN_START);
    cbx_widget_draw(&f->diag.base, f->sdl.renderer);

    SDL_Rect out;
    cbx_widget_get_rect(&f->diag.base, &out);
    assert_int_equal(out.w, 400);
    assert_int_equal(out.h, 400);
}

static void test_render_null_safe(void **state)
{
    (void)state;
    /* Drawing a NULL widget should be a no-op */
    cbx_widget_draw(NULL, NULL);
}

/* ------------------------------------------------------------------ */
/*  Tests: SVG loading (optional, may fail gracefully)                 */
/* ------------------------------------------------------------------ */

static void test_init_with_svg_nonexistent(void **state)
{
    pd_fixture *f = *state;

    cbx_profile_diagram diag;
    cbx_theme theme;
    cbx_theme_default(&theme);

    /* Non-existent SVG path should not cause init failure */
    int rc = cbx_profile_diagram_init(&diag, f->sdl.renderer,
                                        "/nonexistent/file.svg", &theme);
    assert_int_equal(rc, 0);
    /* base_texture should be NULL since file doesn't exist */
    assert_null(diag.base_texture);
    cbx_profile_diagram_shutdown(&diag);
}

/* ------------------------------------------------------------------ */
/*  Test runner                                                        */
/* ------------------------------------------------------------------ */

int main(void)
{
    const struct CMUnitTest tests[] = {
        /* Button position lookup */
        cmocka_unit_test(test_button_count),
        cmocka_unit_test(test_button_pos_valid),
        cmocka_unit_test(test_button_pos_all_valid),
        cmocka_unit_test(test_button_pos_invalid),

        /* Name mapping */
        cmocka_unit_test(test_button_from_name_known),
        cmocka_unit_test(test_button_from_name_unknown),
        cmocka_unit_test(test_button_name_roundtrip),
        cmocka_unit_test(test_button_name_invalid),

        /* Highlight state */
        cmocka_unit_test_setup_teardown(test_highlight_set_get,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_highlight_clear,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_highlight_none,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_highlight_all_buttons,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_highlight_invalid,
                                          setup, teardown),
        cmocka_unit_test(test_highlight_null_safe),

        /* Lifecycle */
        cmocka_unit_test(test_init_basic),
        cmocka_unit_test(test_init_null_args),
        cmocka_unit_test(test_shutdown_null_safe),
        cmocka_unit_test_setup_teardown(test_shutdown_cleans_up,
                                          setup, teardown),

        /* Rendering */
        cmocka_unit_test_setup_teardown(test_render_no_crash,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_render_all_buttons,
                                          setup, teardown),
        cmocka_unit_test_setup_teardown(test_render_with_rect,
                                          setup, teardown),
        cmocka_unit_test(test_render_null_safe),

        /* SVG loading */
        cmocka_unit_test_setup_teardown(test_init_with_svg_nonexistent,
                                          setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}