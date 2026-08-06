/*
 * test_editor_seq_mode.c — Tests for sequential binding mode
 * (Task 38).
 *
 * Tests: begin sequential, diagram highlight per step, input capture
 * auto-advance, skip (B), cancel (Start), progress bar, validation
 * integration, full workflow.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <SDL2/SDL.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "manager/profile_editor_list.h"
#include "manager/profile_diagram.h"
#include "manager/profile_validate.h"
#include "dbus/ip_input_signal.h"
#include "ui/theme.h"
#include "ui/widget.h"
#include "test_harness.h"
#include "dbus_mock.h"

/* ------------------------------------------------------------------ */
/*  Fixture                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    ip_dbus_mock       mock;
    const ip_dbus_backend *backend;
    TestSdlState       sdl;
    cbx_theme           theme;
    cbx_panel           panel;
    cbx_text_cache      text_cache;
    cbx_profile_editor  ed;
} seq_fixture;

static int setup(void **state)
{
    seq_fixture *f = malloc(sizeof(*f));
    memset(f, 0, sizeof(*f));

    int rc = test_harness_sdl_init(&f->sdl);
    assert_int_equal(rc, 0);

    cbx_theme_default(&f->theme);

    rc = cbx_panel_init(&f->panel, &f->theme);
    assert_int_equal(rc, 0);

    rc = cbx_text_cache_init(&f->text_cache, f->sdl.renderer);
    assert_int_equal(rc, 0);

    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);

    rc = cbx_profile_editor_init(&f->ed, &f->panel, f->sdl.renderer,
                                   &f->text_cache, &f->theme, -1);
    assert_int_equal(rc, 0);

    *state = f;
    return 0;
}

static int teardown(void **state)
{
    seq_fixture *f = *state;
    cbx_profile_editor_shutdown(&f->ed);
    cbx_widget_destroy(&f->panel.base);
    cbx_text_cache_cleanup(&f->text_cache);
    ip_dbus_mock_free(&f->mock);
    test_harness_sdl_shutdown(&f->sdl);
    free(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Helper: create a test profile                                       */
/* ------------------------------------------------------------------ */

static cbx_profile
make_test_profile(int mappings)
{
    cbx_profile p;
    cbx_profile_init(&p);
    strncpy(p.name, "Test Profile", sizeof(p.name) - 1);

    for (int i = 0; i < mappings && i < CBX_MAX_MAPPINGS; i++) {
        cbx_profile_mapping *m = &p.mappings[p.mapping_count];
        memset(m, 0, sizeof(*m));
        strncpy(m->name, "Binding", sizeof(m->name) - 1);
        strncpy(m->source_event.device_class, "gamepad",
                 sizeof(m->source_event.device_class) - 1);
        m->source_event.prop_count = 1;
        strncpy(m->source_event.props[0].key, "button",
                 sizeof(m->source_event.props[0].key) - 1);
        const char *names[] = {"A", "B", "Start", "Up", "Down"};
        strncpy(m->source_event.props[0].value, names[i % 5],
                 sizeof(m->source_event.props[0].value) - 1);
        m->target_event_count = 1;
        strncpy(m->target_events[0].device_class, "keyboard",
                 sizeof(m->target_events[0].device_class) - 1);
        strncpy(m->target_events[0].value, "KeyA",
                 sizeof(m->target_events[0].value) - 1);
        p.mapping_count++;
    }

    return p;
}

/* ------------------------------------------------------------------ */
/*  Tests: begin sequential mode                                        */
/* ------------------------------------------------------------------ */

static void test_begin_sequential(void **state)
{
    seq_fixture *f = *state;

    cbx_profile p = make_test_profile(0);
    cbx_profile_editor_load_profile(&f->ed, &p);

    int rc = cbx_profile_editor_begin_sequential(&f->ed);
    assert_int_equal(rc, 0);

    assert_true(cbx_profile_editor_seq_is_active(&f->ed));
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                       CBX_EDITOR_MODE_SEQUENTIAL);
    assert_int_equal(cbx_profile_editor_seq_get_step(&f->ed), 0);
}

static void test_begin_sequential_no_profile(void **state)
{
    seq_fixture *f = *state;

    /* No profile loaded */
    int rc = cbx_profile_editor_begin_sequential(&f->ed);
    assert_int_equal(rc, -EINVAL);
}

static void test_begin_sequential_null(void **state)
{
    (void)state;
    int rc = cbx_profile_editor_begin_sequential(NULL);
    assert_int_equal(rc, -EINVAL);
}

static void test_begin_sequential_diagram_highlight(void **state)
{
    seq_fixture *f = *state;

    cbx_profile p = make_test_profile(0);
    cbx_profile_editor_load_profile(&f->ed, &p);
    cbx_profile_editor_begin_sequential(&f->ed);

    /* Step 0 = UP, diagram should highlight UP */
    assert_int_equal(cbx_profile_editor_seq_current_button(&f->ed),
                       CBX_DIAG_BTN_UP);
    assert_int_equal(cbx_profile_editor_get_diagram_highlight(&f->ed),
                       CBX_DIAG_BTN_UP);
}

static void test_begin_sequential_progress_zero(void **state)
{
    seq_fixture *f = *state;

    cbx_profile p = make_test_profile(0);
    cbx_profile_editor_load_profile(&f->ed, &p);
    cbx_profile_editor_begin_sequential(&f->ed);

    /* Progress should be 0 at step 0 */
    double prog = cbx_profile_editor_seq_progress(&f->ed);
    assert_true(prog >= 0.0 && prog < 0.01);
}

static void test_begin_sequential_hides_binding_list(void **state)
{
    seq_fixture *f = *state;

    cbx_profile p = make_test_profile(3);
    cbx_profile_editor_load_profile(&f->ed, &p);
    cbx_profile_editor_begin_sequential(&f->ed);

    /* Binding list should be hidden, progress bar visible */
    assert_false(cbx_widget_is_visible(&f->ed.binding_list.base));
    assert_true(cbx_widget_is_visible(&f->ed.progress_bar.base));
}

static void test_begin_panel_has_6_children(void **state)
{
    seq_fixture *f = *state;

    /* After init, panel should have 6 children (title, diagram,
       binding_list, target_list, status, progress_bar) */
    assert_int_equal(cbx_panel_child_count(&f->panel), 6);
}

/* ------------------------------------------------------------------ */
/*  Tests: sequential input capture and auto-advance                   */
/* ------------------------------------------------------------------ */

static void test_seq_capture_auto_advance(void **state)
{
    seq_fixture *f = *state;

    cbx_profile p = make_test_profile(0);
    cbx_profile_editor_load_profile(&f->ed, &p);
    cbx_profile_editor_begin_sequential(&f->ed);

    /* Step 0 = UP, press a button */
    cbx_profile_editor_on_input_event(IP_INPUT_A, IP_INPUT_CAT_BUTTON,
                                        1.0, "A", NULL, &f->ed);

    /* Should have advanced to step 1 */
    assert_int_equal(cbx_profile_editor_seq_get_step(&f->ed), 1);
    assert_true(cbx_profile_editor_seq_is_active(&f->ed));

    /* Should have created a mapping for UP */
    const cbx_profile *prof = cbx_profile_editor_get_profile(&f->ed);
    assert_int_equal(prof->mapping_count, 1);
    /* The mapping should be for UP (the prompted button) */
    assert_string_equal(prof->mappings[0].source_event.props[0].value,
                          "A");
}

static void test_seq_capture_multiple(void **state)
{
    seq_fixture *f = *state;

    cbx_profile p = make_test_profile(0);
    cbx_profile_editor_load_profile(&f->ed, &p);
    cbx_profile_editor_begin_sequential(&f->ed);

    /* Capture 3 buttons — use X, Y, L1 to avoid B (skip) and Start (cancel) */
    cbx_profile_editor_on_input_event(IP_INPUT_X, IP_INPUT_CAT_BUTTON,
                                        1.0, "X", NULL, &f->ed);
    cbx_profile_editor_on_input_event(IP_INPUT_Y, IP_INPUT_CAT_BUTTON,
                                        1.0, "Y", NULL, &f->ed);
    cbx_profile_editor_on_input_event(IP_INPUT_L1, IP_INPUT_CAT_BUTTON,
                                        1.0, "L1", NULL, &f->ed);

    assert_int_equal(cbx_profile_editor_seq_get_step(&f->ed), 3);
    assert_true(cbx_profile_editor_seq_is_active(&f->ed));

    const cbx_profile *prof = cbx_profile_editor_get_profile(&f->ed);
    assert_int_equal(prof->mapping_count, 3);
}

static void test_seq_capture_ignores_release(void **state)
{
    seq_fixture *f = *state;

    cbx_profile p = make_test_profile(0);
    cbx_profile_editor_load_profile(&f->ed, &p);
    cbx_profile_editor_begin_sequential(&f->ed);

    /* Button release (value 0.0) should be ignored */
    cbx_profile_editor_on_input_event(IP_INPUT_A, IP_INPUT_CAT_BUTTON,
                                        0.0, "A", NULL, &f->ed);

    assert_int_equal(cbx_profile_editor_seq_get_step(&f->ed), 0);
    assert_true(cbx_profile_editor_seq_is_active(&f->ed));
}

static void test_seq_diagram_highlight_advances(void **state)
{
    seq_fixture *f = *state;

    cbx_profile p = make_test_profile(0);
    cbx_profile_editor_load_profile(&f->ed, &p);
    cbx_profile_editor_begin_sequential(&f->ed);

    /* Step 0 = UP */
    assert_int_equal(cbx_profile_editor_get_diagram_highlight(&f->ed),
                       CBX_DIAG_BTN_UP);

    /* Capture → advance to step 1 = DOWN */
    cbx_profile_editor_on_input_event(IP_INPUT_A, IP_INPUT_CAT_BUTTON,
                                        1.0, "A", NULL, &f->ed);
    assert_int_equal(cbx_profile_editor_get_diagram_highlight(&f->ed),
                       CBX_DIAG_BTN_DOWN);

    /* Capture → advance to step 2 = LEFT */
    cbx_profile_editor_on_input_event(IP_INPUT_B, IP_INPUT_CAT_BUTTON,
                                        1.0, "B", NULL, &f->ed);
    assert_int_equal(cbx_profile_editor_get_diagram_highlight(&f->ed),
                       CBX_DIAG_BTN_LEFT);
}

static void test_seq_progress_increases(void **state)
{
    seq_fixture *f = *state;

    cbx_profile p = make_test_profile(0);
    cbx_profile_editor_load_profile(&f->ed, &p);
    cbx_profile_editor_begin_sequential(&f->ed);

    double prog0 = cbx_profile_editor_seq_progress(&f->ed);

    cbx_profile_editor_on_input_event(IP_INPUT_A, IP_INPUT_CAT_BUTTON,
                                        1.0, "A", NULL, &f->ed);
    double prog1 = cbx_profile_editor_seq_progress(&f->ed);

    assert_true(prog1 > prog0);
}

/* ------------------------------------------------------------------ */
/*  Tests: skip (B button)                                             */
/* ------------------------------------------------------------------ */

static void test_seq_skip(void **state)
{
    seq_fixture *f = *state;

    cbx_profile p = make_test_profile(0);
    cbx_profile_editor_load_profile(&f->ed, &p);
    cbx_profile_editor_begin_sequential(&f->ed);

    int rc = cbx_profile_editor_seq_skip(&f->ed);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_profile_editor_seq_get_step(&f->ed), 1);

    /* Should have advanced to DOWN */
    assert_int_equal(cbx_profile_editor_seq_current_button(&f->ed),
                       CBX_DIAG_BTN_DOWN);
}

static void test_seq_skip_creates_no_mapping(void **state)
{
    seq_fixture *f = *state;

    cbx_profile p = make_test_profile(0);
    cbx_profile_editor_load_profile(&f->ed, &p);
    cbx_profile_editor_begin_sequential(&f->ed);

    cbx_profile_editor_seq_skip(&f->ed);

    const cbx_profile *prof = cbx_profile_editor_get_profile(&f->ed);
    /* Skipped button should not create a mapping */
    assert_int_equal(prof->mapping_count, 0);
}

static void test_seq_skip_via_b_input(void **state)
{
    seq_fixture *f = *state;

    cbx_profile p = make_test_profile(0);
    cbx_profile_editor_load_profile(&f->ed, &p);
    cbx_profile_editor_begin_sequential(&f->ed);

    /* Press B during sequential mode should skip, not create a mapping */
    cbx_profile_editor_on_input_event(IP_INPUT_B, IP_INPUT_CAT_BUTTON,
                                        1.0, "B", NULL, &f->ed);

    assert_int_equal(cbx_profile_editor_seq_get_step(&f->ed), 1);
    const cbx_profile *prof = cbx_profile_editor_get_profile(&f->ed);
    assert_int_equal(prof->mapping_count, 0);
}

static void test_seq_skip_not_active(void **state)
{
    seq_fixture *f = *state;

    int rc = cbx_profile_editor_seq_skip(&f->ed);
    assert_int_equal(rc, -ENOENT);
}

/* ------------------------------------------------------------------ */
/*  Tests: cancel (Start button)                                       */
/* ------------------------------------------------------------------ */

static void test_seq_cancel_via_start(void **state)
{
    seq_fixture *f = *state;

    cbx_profile p = make_test_profile(0);
    cbx_profile_editor_load_profile(&f->ed, &p);
    cbx_profile_editor_begin_sequential(&f->ed);

    /* Press Start → should cancel sequential mode */
    cbx_profile_editor_on_input_event(IP_INPUT_START, IP_INPUT_CAT_BUTTON,
                                        1.0, "Start", NULL, &f->ed);

    assert_false(cbx_profile_editor_seq_is_active(&f->ed));
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                       CBX_EDITOR_MODE_LIST);
}

static void test_seq_cancel_explicit(void **state)
{
    seq_fixture *f = *state;

    cbx_profile p = make_test_profile(0);
    cbx_profile_editor_load_profile(&f->ed, &p);
    cbx_profile_editor_begin_sequential(&f->ed);

    cbx_profile_editor_cancel_sequential(&f->ed);

    assert_false(cbx_profile_editor_seq_is_active(&f->ed));
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                       CBX_EDITOR_MODE_LIST);
}

static void test_seq_cancel_via_editor_cancel(void **state)
{
    seq_fixture *f = *state;

    cbx_profile p = make_test_profile(0);
    cbx_profile_editor_load_profile(&f->ed, &p);
    cbx_profile_editor_begin_sequential(&f->ed);

    int rc = cbx_profile_editor_cancel(&f->ed);
    assert_int_equal(rc, 0);
    assert_false(cbx_profile_editor_seq_is_active(&f->ed));
}

static void test_seq_cancel_shows_binding_list(void **state)
{
    seq_fixture *f = *state;

    cbx_profile p = make_test_profile(3);
    cbx_profile_editor_load_profile(&f->ed, &p);
    cbx_profile_editor_begin_sequential(&f->ed);

    assert_false(cbx_widget_is_visible(&f->ed.binding_list.base));

    cbx_profile_editor_cancel_sequential(&f->ed);

    assert_true(cbx_widget_is_visible(&f->ed.binding_list.base));
    assert_false(cbx_widget_is_visible(&f->ed.progress_bar.base));
}

static void test_seq_cancel_null_safe(void **state)
{
    (void)state;
    cbx_profile_editor_cancel_sequential(NULL);
    /* should not crash */
}

/* ------------------------------------------------------------------ */
/*  Tests: complete all steps                                          */
/* ------------------------------------------------------------------ */

static void test_seq_complete_all(void **state)
{
    seq_fixture *f = *state;

    cbx_profile p = make_test_profile(0);
    cbx_profile_editor_load_profile(&f->ed, &p);
    cbx_profile_editor_begin_sequential(&f->ed);

    /* Capture all 17 buttons, skipping B (skip) and Start (cancel) */
    const char *btn_names[] = {
        "Up", "Down", "Left", "Right", "A", "B", "X", "Y",
        "Start", "Select", "Guide", "L1", "R1", "L2", "R2", "L3", "R3"
    };

    for (int i = 0; i < CBX_DIAG_BTN_COUNT; i++) {
        if (strcmp(btn_names[i], "Start") == 0) {
            cbx_profile_editor_seq_skip(&f->ed);
            continue;
        }
        if (strcmp(btn_names[i], "B") == 0) {
            cbx_profile_editor_seq_skip(&f->ed);
            continue;
        }
        cbx_profile_editor_on_input_event(IP_INPUT_A,
                                            IP_INPUT_CAT_BUTTON,
                                            1.0, btn_names[i],
                                            NULL, &f->ed);
    }

    /* After completing (or skipping all), should return to list mode */
    assert_false(cbx_profile_editor_seq_is_active(&f->ed));
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                       CBX_EDITOR_MODE_LIST);
}

/* ------------------------------------------------------------------ */
/*  Tests: accessors                                                   */
/* ------------------------------------------------------------------ */

static void test_seq_accessors_null_safe(void **state)
{
    (void)state;
    assert_false(cbx_profile_editor_seq_is_active(NULL));
    assert_int_equal(cbx_profile_editor_seq_get_step(NULL), -1);
    assert_int_equal(cbx_profile_editor_seq_current_button(NULL),
                       CBX_DIAG_BTN_NONE);
    assert_true(cbx_profile_editor_seq_progress(NULL) == 0.0);
}

static void test_seq_current_button_when_inactive(void **state)
{
    seq_fixture *f = *state;
    cbx_profile p = make_test_profile(0);
    cbx_profile_editor_load_profile(&f->ed, &p);
    /* Not in sequential mode */
    assert_int_equal(cbx_profile_editor_seq_current_button(&f->ed),
                       CBX_DIAG_BTN_NONE);
}

/* ------------------------------------------------------------------ */
/*  Tests: validation integration                                      */
/* ------------------------------------------------------------------ */

static void test_seq_validation_after_capture(void **state)
{
    seq_fixture *f = *state;

    cbx_profile p = make_test_profile(0);
    cbx_profile_editor_load_profile(&f->ed, &p);
    cbx_profile_editor_begin_sequential(&f->ed);

    /* Capture A (step 4 = A) — first skip UP, DOWN, LEFT, RIGHT */
    cbx_profile_editor_seq_skip(&f->ed);  /* skip UP */
    cbx_profile_editor_seq_skip(&f->ed);  /* skip DOWN */
    cbx_profile_editor_seq_skip(&f->ed);  /* skip LEFT */
    cbx_profile_editor_seq_skip(&f->ed);  /* skip RIGHT */

    /* Now at step 4 = A */
    assert_int_equal(cbx_profile_editor_seq_current_button(&f->ed),
                       CBX_DIAG_BTN_A);

    /* Press X to map A */
    cbx_profile_editor_on_input_event(IP_INPUT_X, IP_INPUT_CAT_BUTTON,
                                        1.0, "X", NULL, &f->ed);

    /* Cancel sequential mode */
    cbx_profile_editor_cancel_sequential(&f->ed);

    /* Validate — should be missing A (we captured X for the A slot,
       so the source button prop says "X" not "A" — so A is NOT bound */
    const cbx_profile *prof = cbx_profile_editor_get_profile(&f->ed);
    char missing[256] = "";
    int rc = cbx_profile_validate_nes_minimum(prof, missing, sizeof(missing));
    /* A is missing because we captured X for the A slot */
    assert_int_equal(rc, -EINVAL);
    assert_non_null(strstr(missing, "A"));
}

static void test_seq_validation_with_correct_capture(void **state)
{
    seq_fixture *f = *state;

    cbx_profile p = make_test_profile(0);
    cbx_profile_editor_load_profile(&f->ed, &p);
    cbx_profile_editor_begin_sequential(&f->ed);

    /* Capture buttons in order — press the same name as prompted */
    /* Step 0 = UP → press Up */
    cbx_profile_editor_on_input_event(IP_INPUT_UP, IP_INPUT_CAT_BUTTON,
                                        1.0, "Up", NULL, &f->ed);
    /* Step 1 = DOWN → press Down */
    cbx_profile_editor_on_input_event(IP_INPUT_DOWN, IP_INPUT_CAT_BUTTON,
                                        1.0, "Down", NULL, &f->ed);
    /* Step 2 = LEFT → press Left */
    cbx_profile_editor_on_input_event(IP_INPUT_LEFT, IP_INPUT_CAT_BUTTON,
                                        1.0, "Left", NULL, &f->ed);
    /* Step 3 = RIGHT → press Right */
    cbx_profile_editor_on_input_event(IP_INPUT_RIGHT, IP_INPUT_CAT_BUTTON,
                                        1.0, "Right", NULL, &f->ed);
    /* Step 4 = A → press A */
    cbx_profile_editor_on_input_event(IP_INPUT_A, IP_INPUT_CAT_BUTTON,
                                        1.0, "A", NULL, &f->ed);
    /* Step 5 = B → B skips, so skip */
    cbx_profile_editor_seq_skip(&f->ed);

    /* Cancel to return to list mode */
    cbx_profile_editor_cancel_sequential(&f->ed);

    /* Validate — we have Up, Down, Left, Right, A but missing B */
    const cbx_profile *prof = cbx_profile_editor_get_profile(&f->ed);
    char missing[256] = "";
    int rc = cbx_profile_validate_nes_minimum(prof, missing, sizeof(missing));
    assert_int_equal(rc, -EINVAL);
    assert_non_null(strstr(missing, "B"));
}

/* ------------------------------------------------------------------ */
/*  Tests: full workflow                                               */
/* ------------------------------------------------------------------ */

static void test_seq_full_workflow(void **state)
{
    seq_fixture *f = *state;

    /* Load an empty profile */
    cbx_profile p = make_test_profile(0);
    cbx_profile_editor_load_profile(&f->ed, &p);

    /* Begin sequential mode */
    cbx_profile_editor_begin_sequential(&f->ed);
    assert_true(cbx_profile_editor_seq_is_active(&f->ed));

    /* Capture 5 buttons (Up, Down, Left, Right, A) */
    cbx_profile_editor_on_input_event(IP_INPUT_UP, IP_INPUT_CAT_BUTTON,
                                        1.0, "Up", NULL, &f->ed);
    cbx_profile_editor_on_input_event(IP_INPUT_DOWN, IP_INPUT_CAT_BUTTON,
                                        1.0, "Down", NULL, &f->ed);
    cbx_profile_editor_on_input_event(IP_INPUT_LEFT, IP_INPUT_CAT_BUTTON,
                                        1.0, "Left", NULL, &f->ed);
    cbx_profile_editor_on_input_event(IP_INPUT_RIGHT, IP_INPUT_CAT_BUTTON,
                                        1.0, "Right", NULL, &f->ed);
    cbx_profile_editor_on_input_event(IP_INPUT_A, IP_INPUT_CAT_BUTTON,
                                        1.0, "A", NULL, &f->ed);

    assert_int_equal(cbx_profile_editor_seq_get_step(&f->ed), 5);

    /* Skip B (step 5) */
    cbx_profile_editor_seq_skip(&f->ed);
    assert_int_equal(cbx_profile_editor_seq_get_step(&f->ed), 6);

    /* Cancel via Start */
    cbx_profile_editor_on_input_event(IP_INPUT_START, IP_INPUT_CAT_BUTTON,
                                        1.0, "Start", NULL, &f->ed);

    assert_false(cbx_profile_editor_seq_is_active(&f->ed));
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                       CBX_EDITOR_MODE_LIST);

    /* Verify bindings were created */
    const cbx_profile *prof = cbx_profile_editor_get_profile(&f->ed);
    assert_int_equal(prof->mapping_count, 5);

    /* Validate — B is missing */
    char missing[256] = "";
    int rc = cbx_profile_validate_nes_minimum(prof, missing, sizeof(missing));
    assert_int_equal(rc, -EINVAL);
    assert_non_null(strstr(missing, "B"));
}

/* ------------------------------------------------------------------ */
/*  Tests: rendering (no crash)                                        */
/* ------------------------------------------------------------------ */

static void test_seq_render_no_crash(void **state)
{
    seq_fixture *f = *state;

    cbx_profile p = make_test_profile(2);
    cbx_profile_editor_load_profile(&f->ed, &p);
    cbx_profile_editor_begin_sequential(&f->ed);

    /* Render should not crash */
    cbx_widget_draw(&f->panel.base, f->sdl.renderer);
}

/* ------------------------------------------------------------------ */
/*  Test runner                                                        */
/* ------------------------------------------------------------------ */

int main(void)
{
    const struct CMUnitTest tests[] = {
        /* Begin sequential */
        cmocka_unit_test_setup_teardown(test_begin_sequential, setup, teardown),
        cmocka_unit_test_setup_teardown(test_begin_sequential_no_profile, setup, teardown),
        cmocka_unit_test(test_begin_sequential_null),
        cmocka_unit_test_setup_teardown(test_begin_sequential_diagram_highlight, setup, teardown),
        cmocka_unit_test_setup_teardown(test_begin_sequential_progress_zero, setup, teardown),
        cmocka_unit_test_setup_teardown(test_begin_sequential_hides_binding_list, setup, teardown),
        cmocka_unit_test_setup_teardown(test_begin_panel_has_6_children, setup, teardown),

        /* Input capture and auto-advance */
        cmocka_unit_test_setup_teardown(test_seq_capture_auto_advance, setup, teardown),
        cmocka_unit_test_setup_teardown(test_seq_capture_multiple, setup, teardown),
        cmocka_unit_test_setup_teardown(test_seq_capture_ignores_release, setup, teardown),
        cmocka_unit_test_setup_teardown(test_seq_diagram_highlight_advances, setup, teardown),
        cmocka_unit_test_setup_teardown(test_seq_progress_increases, setup, teardown),

        /* Skip (B) */
        cmocka_unit_test_setup_teardown(test_seq_skip, setup, teardown),
        cmocka_unit_test_setup_teardown(test_seq_skip_creates_no_mapping, setup, teardown),
        cmocka_unit_test_setup_teardown(test_seq_skip_via_b_input, setup, teardown),
        cmocka_unit_test_setup_teardown(test_seq_skip_not_active, setup, teardown),

        /* Cancel (Start) */
        cmocka_unit_test_setup_teardown(test_seq_cancel_via_start, setup, teardown),
        cmocka_unit_test_setup_teardown(test_seq_cancel_explicit, setup, teardown),
        cmocka_unit_test_setup_teardown(test_seq_cancel_via_editor_cancel, setup, teardown),
        cmocka_unit_test_setup_teardown(test_seq_cancel_shows_binding_list, setup, teardown),
        cmocka_unit_test(test_seq_cancel_null_safe),

        /* Complete all steps */
        cmocka_unit_test_setup_teardown(test_seq_complete_all, setup, teardown),

        /* Accessors */
        cmocka_unit_test(test_seq_accessors_null_safe),
        cmocka_unit_test_setup_teardown(test_seq_current_button_when_inactive, setup, teardown),

        /* Validation integration */
        cmocka_unit_test_setup_teardown(test_seq_validation_after_capture, setup, teardown),
        cmocka_unit_test_setup_teardown(test_seq_validation_with_correct_capture, setup, teardown),

        /* Full workflow */
        cmocka_unit_test_setup_teardown(test_seq_full_workflow, setup, teardown),

        /* Rendering */
        cmocka_unit_test_setup_teardown(test_seq_render_no_crash, setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}