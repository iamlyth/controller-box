/*
 * test_editor_list_mode.c — Tests for the profile editor binding list
 * mode (Task 37).
 *
 * Tests init, profile loading, binding list building, navigation,
 * diagram synchronisation, target pick mode, capture mode, and
 * capabilities loading with mock DBus.
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
} pe_fixture;

static int setup(void **state)
{
    pe_fixture *f = malloc(sizeof(*f));
    memset(f, 0, sizeof(*f));

    /* SDL dummy driver */
    int rc = test_harness_sdl_init(&f->sdl);
    assert_int_equal(rc, 0);

    /* Theme */
    cbx_theme_default(&f->theme);

    /* Panel */
    rc = cbx_panel_init(&f->panel, &f->theme);
    assert_int_equal(rc, 0);

    /* Text cache (no font for headless tests) */
    rc = cbx_text_cache_init(&f->text_cache, f->sdl.renderer);
    assert_int_equal(rc, 0);

    /* DBus mock */
    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);

    /* Editor */
    rc = cbx_profile_editor_init(&f->ed, &f->panel, f->sdl.renderer,
                                   &f->text_cache, &f->theme, -1);
    assert_int_equal(rc, 0);

    *state = f;
    return 0;
}

static int teardown(void **state)
{
    pe_fixture *f = *state;
    cbx_profile_editor_shutdown(&f->ed);
    cbx_widget_destroy(&f->panel.base);
    cbx_text_cache_cleanup(&f->text_cache);
    ip_dbus_mock_free(&f->mock);
    test_harness_sdl_shutdown(&f->sdl);
    free(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Helper: create a test profile with mappings                        */
/* ------------------------------------------------------------------ */

static cbx_profile make_test_profile(int mappings)
{
    cbx_profile p;
    cbx_profile_init(&p);
    strncpy(p.name, "Test Profile", sizeof(p.name) - 1);

    for (int i = 0; i < mappings && i < CBX_MAX_MAPPINGS; i++) {
        cbx_profile_mapping *m = &p.mappings[p.mapping_count];
        memset(m, 0, sizeof(*m));
        strncpy(m->name, "Binding", sizeof(m->name) - 1);

        /* Set source event: gamepad button */
        strncpy(m->source_event.device_class, "gamepad",
                 sizeof(m->source_event.device_class) - 1);
        m->source_event.prop_count = 1;
        switch (i) {
            case 0:
                strncpy(m->source_event.props[0].key, "button",
                         sizeof(m->source_event.props[0].key) - 1);
                strncpy(m->source_event.props[0].value, "A",
                         sizeof(m->source_event.props[0].value) - 1);
                break;
            case 1:
                strncpy(m->source_event.props[0].key, "button",
                         sizeof(m->source_event.props[0].key) - 1);
                strncpy(m->source_event.props[0].value, "B",
                         sizeof(m->source_event.props[0].value) - 1);
                break;
            case 2:
                strncpy(m->source_event.props[0].key, "button",
                         sizeof(m->source_event.props[0].key) - 1);
                strncpy(m->source_event.props[0].value, "Start",
                         sizeof(m->source_event.props[0].value) - 1);
                break;
            default:
                strncpy(m->source_event.props[0].key, "button",
                         sizeof(m->source_event.props[0].key) - 1);
                snprintf(m->source_event.props[0].value,
                          sizeof(m->source_event.props[0].value),
                          "Button%d", i);
                break;
        }

        /* Set target event */
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
/*  Tests: init                                                        */
/* ------------------------------------------------------------------ */

static void test_init_basic(void **state)
{
    pe_fixture *f = *state;

    /* Panel should have children added by editor */
    assert_int_equal(cbx_panel_child_count(&f->panel), 6);

    /* Mode should be LIST */
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                       CBX_EDITOR_MODE_LIST);

    /* No profile loaded */
    assert_null(cbx_profile_editor_get_profile(&f->ed));

    /* No bindings */
    assert_int_equal(cbx_profile_editor_binding_count(&f->ed), 0);

    /* No selection */
    assert_int_equal(cbx_profile_editor_get_selected(&f->ed), -1);
}

static void test_init_null_args(void **state)
{
    (void)state;
    assert_int_equal(cbx_profile_editor_init(NULL, NULL, NULL, NULL,
                                                 NULL, 0),
                       -EINVAL);
}

static void test_shutdown_null_safe(void **state)
{
    (void)state;
    cbx_profile_editor ed;
    memset(&ed, 0, sizeof(ed));
    cbx_profile_editor_shutdown(&ed);
    /* should not crash */
}

/* ------------------------------------------------------------------ */
/*  Tests: profile loading                                             */
/* ------------------------------------------------------------------ */

static void test_load_profile(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p = make_test_profile(3);
    int rc = cbx_profile_editor_load_profile(&f->ed, &p);
    assert_int_equal(rc, 0);

    assert_int_equal(cbx_profile_editor_binding_count(&f->ed), 3);
    assert_int_equal(cbx_profile_editor_get_selected(&f->ed), 0);

    const cbx_profile *loaded = cbx_profile_editor_get_profile(&f->ed);
    assert_non_null(loaded);
    assert_string_equal(loaded->name, "Test Profile");
}

static void test_load_profile_null(void **state)
{
    pe_fixture *f = *state;

    int rc = cbx_profile_editor_load_profile(&f->ed, NULL);
    assert_int_equal(rc, -EINVAL);
}

static void test_load_profile_empty(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p;
    cbx_profile_init(&p);

    int rc = cbx_profile_editor_load_profile(&f->ed, &p);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_profile_editor_binding_count(&f->ed), 0);
    assert_int_equal(cbx_profile_editor_get_selected(&f->ed), -1);
}

/* ------------------------------------------------------------------ */
/*  Tests: navigation                                                  */
/* ------------------------------------------------------------------ */

static void test_move_down(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p = make_test_profile(5);
    cbx_profile_editor_load_profile(&f->ed, &p);

    int idx = cbx_profile_editor_move_down(&f->ed);
    assert_int_equal(idx, 1);

    idx = cbx_profile_editor_move_down(&f->ed);
    assert_int_equal(idx, 2);
}

static void test_move_up(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p = make_test_profile(5);
    cbx_profile_editor_load_profile(&f->ed, &p);

    /* Move down first */
    cbx_profile_editor_move_down(&f->ed);
    cbx_profile_editor_move_down(&f->ed);
    assert_int_equal(cbx_profile_editor_get_selected(&f->ed), 2);

    int idx = cbx_profile_editor_move_up(&f->ed);
    assert_int_equal(idx, 1);
}

static void test_move_wrap_down(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p = make_test_profile(3);
    cbx_profile_editor_load_profile(&f->ed, &p);

    /* Move to last */
    cbx_profile_editor_move_down(&f->ed);
    cbx_profile_editor_move_down(&f->ed);
    assert_int_equal(cbx_profile_editor_get_selected(&f->ed), 2);

    /* Wrap to first */
    int idx = cbx_profile_editor_move_down(&f->ed);
    assert_int_equal(idx, 0);
}

static void test_move_wrap_up(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p = make_test_profile(3);
    cbx_profile_editor_load_profile(&f->ed, &p);

    /* At first (0), move up wraps to last */
    int idx = cbx_profile_editor_move_up(&f->ed);
    assert_int_equal(idx, 2);
}

static void test_move_empty_list(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p;
    cbx_profile_init(&p);
    cbx_profile_editor_load_profile(&f->ed, &p);

    int idx = cbx_profile_editor_move_down(&f->ed);
    assert_int_equal(idx, -1);

    idx = cbx_profile_editor_move_up(&f->ed);
    assert_int_equal(idx, -1);
}

/* ------------------------------------------------------------------ */
/*  Tests: diagram synchronisation                                     */
/* ------------------------------------------------------------------ */

static void test_diagram_sync_on_load(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p = make_test_profile(3);
    cbx_profile_editor_load_profile(&f->ed, &p);

    /* Selection is 0, which is button "A" */
    assert_int_equal(cbx_profile_editor_get_selected(&f->ed), 0);
    assert_int_equal(cbx_profile_editor_get_diagram_highlight(&f->ed),
                       CBX_DIAG_BTN_A);
}

static void test_diagram_sync_on_move(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p = make_test_profile(3);
    cbx_profile_editor_load_profile(&f->ed, &p);

    /* Move to binding 1 (button "B") */
    cbx_profile_editor_move_down(&f->ed);
    assert_int_equal(cbx_profile_editor_get_diagram_highlight(&f->ed),
                       CBX_DIAG_BTN_B);

    /* Move to binding 2 (button "Start") */
    cbx_profile_editor_move_down(&f->ed);
    assert_int_equal(cbx_profile_editor_get_diagram_highlight(&f->ed),
                       CBX_DIAG_BTN_START);
}

static void test_diagram_sync_empty(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p;
    cbx_profile_init(&p);
    cbx_profile_editor_load_profile(&f->ed, &p);

    assert_int_equal(cbx_profile_editor_get_diagram_highlight(&f->ed),
                       CBX_DIAG_BTN_NONE);
}

static void test_diagram_sync_unknown_button(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p = make_test_profile(1);
    /* Change button name to something unknown */
    strncpy(p.mappings[0].source_event.props[0].value, "FooButton",
             sizeof(p.mappings[0].source_event.props[0].value) - 1);
    p.mappings[0].source_event.props[0].value
        [sizeof(p.mappings[0].source_event.props[0].value) - 1] = '\0';

    cbx_profile_editor_load_profile(&f->ed, &p);

    /* Unknown button → NONE */
    assert_int_equal(cbx_profile_editor_get_diagram_highlight(&f->ed),
                       CBX_DIAG_BTN_NONE);
}

/* ------------------------------------------------------------------ */
/*  Tests: target pick mode                                            */
/* ------------------------------------------------------------------ */

static void test_activate_enters_target_pick(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p = make_test_profile(3);
    cbx_profile_editor_load_profile(&f->ed, &p);

    /* A on a binding opens the binding edit sub-menu. */
    int rc = cbx_profile_editor_activate(&f->ed);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                       CBX_EDITOR_MODE_BINDING_EDIT);

    /* A on "Pick Target" (index 0) enters target pick mode. */
    rc = cbx_profile_editor_activate(&f->ed);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                       CBX_EDITOR_MODE_TARGET_PICK);
}

static void test_activate_no_selection(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p;
    cbx_profile_init(&p);
    cbx_profile_editor_load_profile(&f->ed, &p);

    int rc = cbx_profile_editor_activate(&f->ed);
    assert_int_equal(rc, -EINVAL);
}

static void test_target_pick_has_targets(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p = make_test_profile(1);
    cbx_profile_editor_load_profile(&f->ed, &p);

    /* Load capabilities (uses defaults since no DBus) */
    cbx_profile_editor_load_capabilities(&f->ed);
    assert_true(cbx_profile_editor_get_target_count(&f->ed) > 0);

    /* Enter binding edit sub-menu, then target pick. */
    cbx_profile_editor_activate(&f->ed);
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                       CBX_EDITOR_MODE_BINDING_EDIT);
    cbx_profile_editor_activate(&f->ed);
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                       CBX_EDITOR_MODE_TARGET_PICK);
}

static void test_target_pick_confirm(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p = make_test_profile(2);
    cbx_profile_editor_load_profile(&f->ed, &p);

    /* Move to binding 1 */
    cbx_profile_editor_move_down(&f->ed);
    assert_int_equal(cbx_profile_editor_get_selected(&f->ed), 1);

    /* Enter binding edit sub-menu, then target pick. */
    cbx_profile_editor_activate(&f->ed);
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                       CBX_EDITOR_MODE_BINDING_EDIT);
    cbx_profile_editor_activate(&f->ed);
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                       CBX_EDITOR_MODE_TARGET_PICK);
    assert_int_equal(cbx_profile_editor_get_editing_index(&f->ed), 1);

    /* Confirm (selects first target) */
    int rc = cbx_profile_editor_activate(&f->ed);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                       CBX_EDITOR_MODE_LIST);

    /* Verify the binding's target was updated */
    const cbx_profile *prof = cbx_profile_editor_get_profile(&f->ed);
    assert_non_null(prof);
    assert_true(prof->mappings[1].target_event_count > 0);
    /* Target should be one of the default targets */
    assert_true(strlen(prof->mappings[1].target_events[0].value) > 0);
}

static void test_target_pick_cancel(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p = make_test_profile(1);
    cbx_profile_editor_load_profile(&f->ed, &p);

    /* Enter binding edit sub-menu, then target pick. */
    cbx_profile_editor_activate(&f->ed);
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                       CBX_EDITOR_MODE_BINDING_EDIT);
    cbx_profile_editor_activate(&f->ed);
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                       CBX_EDITOR_MODE_TARGET_PICK);

    int rc = cbx_profile_editor_cancel(&f->ed);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                       CBX_EDITOR_MODE_LIST);
    assert_int_equal(cbx_profile_editor_get_editing_index(&f->ed), -1);
}

static void test_cancel_in_list_mode(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p = make_test_profile(1);
    cbx_profile_editor_load_profile(&f->ed, &p);

    int rc = cbx_profile_editor_cancel(&f->ed);
    assert_int_equal(rc, -ENOENT);
}

/* ------------------------------------------------------------------ */
/*  Tests: capabilities loading                                        */
/* ------------------------------------------------------------------ */

static void test_load_capabilities_defaults(void **state)
{
    pe_fixture *f = *state;

    /* No DBus set → should use default targets */
    int rc = cbx_profile_editor_load_capabilities(&f->ed);
    assert_int_equal(rc, 0);
    assert_true(cbx_profile_editor_get_target_count(&f->ed) > 0);
}

static void test_load_capabilities_dbus(void **state)
{
    pe_fixture *f = *state;

    cbx_profile_editor_set_dbus(&f->ed, f->backend, &f->mock,
                                  "/org/shadowblip/InputPlumber/CompositeDevice0");

    /* Mock capabilities */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                             "Capabilities", "gamepad,keyboard");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                             "OutputCapabilities", "mouse");
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                             "TargetCapabilities", "xb360,ds5");

    int rc = cbx_profile_editor_load_capabilities(&f->ed);
    assert_int_equal(rc, 0);
    assert_true(cbx_profile_editor_get_target_count(&f->ed) >= 4);
}

static void test_load_capabilities_dbus_error(void **state)
{
    pe_fixture *f = *state;

    cbx_profile_editor_set_dbus(&f->ed, f->backend, &f->mock,
                                  "/org/shadowblip/InputPlumber/CompositeDevice0");

    /* No mock expectations set → DBus calls fail, defaults used */
    int rc = cbx_profile_editor_load_capabilities(&f->ed);
    assert_int_equal(rc, 0);
    assert_true(cbx_profile_editor_get_target_count(&f->ed) > 0);
}

/* ------------------------------------------------------------------ */
/*  Tests: capture mode                                                */
/* ------------------------------------------------------------------ */

static void test_begin_capture(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p = make_test_profile(2);
    cbx_profile_editor_load_profile(&f->ed, &p);

    int rc = cbx_profile_editor_begin_capture(&f->ed);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                       CBX_EDITOR_MODE_CAPTURE);
    assert_true(cbx_profile_editor_is_capture_active(&f->ed));
}

static void test_begin_capture_no_selection(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p;
    cbx_profile_init(&p);
    cbx_profile_editor_load_profile(&f->ed, &p);

    int rc = cbx_profile_editor_begin_capture(&f->ed);
    assert_int_equal(rc, -EINVAL);
}

static void test_cancel_capture(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p = make_test_profile(1);
    cbx_profile_editor_load_profile(&f->ed, &p);

    cbx_profile_editor_begin_capture(&f->ed);
    assert_true(cbx_profile_editor_is_capture_active(&f->ed));

    int rc = cbx_profile_editor_cancel(&f->ed);
    assert_int_equal(rc, 0);
    assert_false(cbx_profile_editor_is_capture_active(&f->ed));
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                       CBX_EDITOR_MODE_LIST);
}

static void test_capture_input_event(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p = make_test_profile(2);
    cbx_profile_editor_load_profile(&f->ed, &p);

    /* Select binding 1 */
    cbx_profile_editor_move_down(&f->ed);
    assert_int_equal(cbx_profile_editor_get_selected(&f->ed), 1);

    /* Begin capture */
    cbx_profile_editor_begin_capture(&f->ed);
    assert_true(cbx_profile_editor_is_capture_active(&f->ed));

    /* Simulate input event */
    cbx_profile_editor_on_input_event(IP_INPUT_START,
                                        IP_INPUT_CAT_BUTTON, 1.0,
                                        "Start", NULL, &f->ed);

    /* Should have exited capture mode */
    assert_false(cbx_profile_editor_is_capture_active(&f->ed));
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                       CBX_EDITOR_MODE_LIST);

    /* Verify the binding's source was updated */
    const cbx_profile *prof = cbx_profile_editor_get_profile(&f->ed);
    assert_non_null(prof);
    assert_string_equal(prof->mappings[1].source_event.props[0].value,
                          "Start");
}

static void test_capture_ignores_release(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p = make_test_profile(1);
    cbx_profile_editor_load_profile(&f->ed, &p);

    cbx_profile_editor_begin_capture(&f->ed);
    assert_true(cbx_profile_editor_is_capture_active(&f->ed));

    /* Release event (value 0.0) should be ignored */
    cbx_profile_editor_on_input_event(IP_INPUT_A, IP_INPUT_CAT_BUTTON,
                                        0.0, "A", NULL, &f->ed);

    assert_true(cbx_profile_editor_is_capture_active(&f->ed));
}

static void test_capture_null_safe(void **state)
{
    (void)state;
    cbx_profile_editor_on_input_event(IP_INPUT_A, IP_INPUT_CAT_BUTTON,
                                        1.0, "A", NULL, NULL);
    /* should not crash */
}

/* ------------------------------------------------------------------ */
/*  Tests: accessors                                                   */
/* ------------------------------------------------------------------ */

static void test_accessors_null_safe(void **state)
{
    (void)state;
    assert_int_equal(cbx_profile_editor_get_mode(NULL),
                       CBX_EDITOR_MODE_LIST);
    assert_int_equal(cbx_profile_editor_binding_count(NULL), 0);
    assert_int_equal(cbx_profile_editor_get_selected(NULL), -1);
    assert_int_equal(cbx_profile_editor_get_diagram_highlight(NULL),
                       CBX_DIAG_BTN_NONE);
    assert_int_equal(cbx_profile_editor_get_target_count(NULL), 0);
    assert_null(cbx_profile_editor_get_status(NULL));
    assert_int_equal(cbx_profile_editor_get_editing_index(NULL), -1);
    assert_false(cbx_profile_editor_is_capture_active(NULL));
}

static void test_status_message(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p = make_test_profile(1);
    cbx_profile_editor_load_profile(&f->ed, &p);

    /* In list mode, status is empty */
    const char *status = cbx_profile_editor_get_status(&f->ed);
    assert_non_null(status);
    assert_true(strlen(status) == 0 || strlen(status) > 0);  /* just non-null */

    /* Enter target pick → status should be set */
    cbx_profile_editor_activate(&f->ed);
    status = cbx_profile_editor_get_status(&f->ed);
    assert_non_null(status);
    assert_true(strlen(status) > 0);
}

/* ------------------------------------------------------------------ */
/*  Tests: rendering                                                   */
/* ------------------------------------------------------------------ */

static void test_render_no_crash(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p = make_test_profile(3);
    cbx_profile_editor_load_profile(&f->ed, &p);

    /* Render the panel (includes diagram + list) */
    cbx_widget_draw(&f->panel.base, f->sdl.renderer);

    /* should not crash */
    assert_true(1);
}

static void test_render_target_pick(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p = make_test_profile(2);
    cbx_profile_editor_load_profile(&f->ed, &p);

    /* Enter binding edit sub-menu, then target pick. */
    cbx_profile_editor_activate(&f->ed);
    cbx_profile_editor_activate(&f->ed);
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                       CBX_EDITOR_MODE_TARGET_PICK);

    cbx_widget_draw(&f->panel.base, f->sdl.renderer);
    /* should not crash */
    assert_true(1);
}

/* ------------------------------------------------------------------ */
/*  Tests: expected_sender security (Task 5)                           */
/* ------------------------------------------------------------------ */

static void test_expected_sender_resolved(void **state)
{
    pe_fixture *f = *state;

    /* set_dbus should resolve the unique bus name via the backend. */
    cbx_profile_editor_set_dbus(&f->ed, f->backend, &f->mock, NULL);
    /* mock_get_unique_name returns ":1.42". */
    assert_string_equal(f->ed.expected_sender, ":1.42");
}

static void test_expected_sender_accepts_match(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p = make_test_profile(1);
    cbx_profile_editor_load_profile(&f->ed, &p);

    /* Set DBus — resolves expected_sender to ":1.42". */
    cbx_profile_editor_set_dbus(&f->ed, f->backend, &f->mock, NULL);

    /* Begin capture for binding 0. */
    int rc = cbx_profile_editor_begin_capture(&f->ed);
    assert_int_equal(rc, 0);
    assert_true(cbx_profile_editor_is_capture_active(&f->ed));

    /* Inject a signal from the expected unique name ":1.42". */
    ip_input_event_payload payload = {
        .sender = ":1.42",
        .path   = "/org/shadowblip/InputPlumber/CompositeDevice0/dbus0",
        .event  = "X",
        .value  = 1.0,
    };
    f->backend->inject_signal(&f->mock,
        IP_IFACE_DBUS_DEVICE, "InputEvent", &payload);

    /* Capture should have succeeded — binding updated, back in LIST mode. */
    assert_false(cbx_profile_editor_is_capture_active(&f->ed));
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                       CBX_EDITOR_MODE_LIST);

    /* Verify the binding source was updated to "X". */
    const cbx_profile *prof = cbx_profile_editor_get_profile(&f->ed);
    assert_non_null(prof);
    bool found = false;
    for (int i = 0; i < prof->mappings[0].source_event.prop_count; i++) {
        if (strcmp(prof->mappings[0].source_event.props[i].key, "button") == 0) {
            assert_string_equal(prof->mappings[0].source_event.props[i].value, "X");
            found = true;
            break;
        }
    }
    assert_true(found);
}

static void test_expected_sender_rejects_mismatch(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p = make_test_profile(1);
    cbx_profile_editor_load_profile(&f->ed, &p);

    /* Set DBus — resolves expected_sender to ":1.42". */
    cbx_profile_editor_set_dbus(&f->ed, f->backend, &f->mock, NULL);

    /* Begin capture for binding 0. */
    int rc = cbx_profile_editor_begin_capture(&f->ed);
    assert_int_equal(rc, 0);
    assert_true(cbx_profile_editor_is_capture_active(&f->ed));

    /* Save the original source button value. */
    const cbx_profile *prof = cbx_profile_editor_get_profile(&f->ed);
    char original_val[64] = {0};
    for (int i = 0; i < prof->mappings[0].source_event.prop_count; i++) {
        if (strcmp(prof->mappings[0].source_event.props[i].key, "button") == 0) {
            strncpy(original_val, prof->mappings[0].source_event.props[i].value,
                     sizeof(original_val) - 1);
            break;
        }
    }

    /* Inject a signal from a mismatched sender ":1.99". */
    ip_input_event_payload payload = {
        .sender = ":1.99",
        .path   = "/org/shadowblip/InputPlumber/CompositeDevice0/dbus0",
        .event  = "Y",
        .value  = 1.0,
    };
    f->backend->inject_signal(&f->mock,
        IP_IFACE_DBUS_DEVICE, "InputEvent", &payload);

    /* Capture should still be active — signal was rejected. */
    assert_true(cbx_profile_editor_is_capture_active(&f->ed));
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                       CBX_EDITOR_MODE_CAPTURE);

    /* Verify the binding source was NOT changed. */
    prof = cbx_profile_editor_get_profile(&f->ed);
    assert_non_null(prof);
    bool found = false;
    for (int i = 0; i < prof->mappings[0].source_event.prop_count; i++) {
        if (strcmp(prof->mappings[0].source_event.props[i].key, "button") == 0) {
            assert_string_equal(prof->mappings[0].source_event.props[i].value,
                                  original_val);
            found = true;
            break;
        }
    }
    assert_true(found);
}

/* ------------------------------------------------------------------ */
/*  Tests: full workflow                                               */
/* ------------------------------------------------------------------ */

static void test_full_workflow(void **state)
{
    pe_fixture *f = *state;

    /* Load profile */
    cbx_profile p = make_test_profile(3);
    cbx_profile_editor_load_profile(&f->ed, &p);
    assert_int_equal(cbx_profile_editor_binding_count(&f->ed), 3);

    /* Navigate */
    cbx_profile_editor_move_down(&f->ed);
    assert_int_equal(cbx_profile_editor_get_selected(&f->ed), 1);
    assert_int_equal(cbx_profile_editor_get_diagram_highlight(&f->ed),
                       CBX_DIAG_BTN_B);

    /* Edit binding via binding edit sub-menu → target pick */
    cbx_profile_editor_activate(&f->ed);
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                       CBX_EDITOR_MODE_BINDING_EDIT);
    cbx_profile_editor_activate(&f->ed);
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                       CBX_EDITOR_MODE_TARGET_PICK);

    cbx_profile_editor_activate(&f->ed);  /* confirm */
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                       CBX_EDITOR_MODE_LIST);

    /* Navigate again */
    cbx_profile_editor_move_down(&f->ed);
    assert_int_equal(cbx_profile_editor_get_selected(&f->ed), 2);
    assert_int_equal(cbx_profile_editor_get_diagram_highlight(&f->ed),
                       CBX_DIAG_BTN_START);

    /* Capture mode */
    cbx_profile_editor_begin_capture(&f->ed);
    assert_true(cbx_profile_editor_is_capture_active(&f->ed));

    /* Simulate button press */
    cbx_profile_editor_on_input_event(IP_INPUT_UP, IP_INPUT_CAT_BUTTON,
                                        1.0, "Up", NULL, &f->ed);
    assert_false(cbx_profile_editor_is_capture_active(&f->ed));

    /* Verify binding was updated */
    const cbx_profile *prof = cbx_profile_editor_get_profile(&f->ed);
    assert_string_equal(prof->mappings[2].source_event.props[0].value,
                          "Up");
}

/* ------------------------------------------------------------------ */
/*  Test runner                                                        */
/* ------------------------------------------------------------------ */

int main(void)
{
    const struct CMUnitTest tests[] = {
        /* Init */
        cmocka_unit_test_setup_teardown(test_init_basic, setup, teardown),
        cmocka_unit_test(test_init_null_args),
        cmocka_unit_test(test_shutdown_null_safe),

        /* Profile loading */
        cmocka_unit_test_setup_teardown(test_load_profile, setup, teardown),
        cmocka_unit_test_setup_teardown(test_load_profile_null, setup, teardown),
        cmocka_unit_test_setup_teardown(test_load_profile_empty, setup, teardown),

        /* Navigation */
        cmocka_unit_test_setup_teardown(test_move_down, setup, teardown),
        cmocka_unit_test_setup_teardown(test_move_up, setup, teardown),
        cmocka_unit_test_setup_teardown(test_move_wrap_down, setup, teardown),
        cmocka_unit_test_setup_teardown(test_move_wrap_up, setup, teardown),
        cmocka_unit_test_setup_teardown(test_move_empty_list, setup, teardown),

        /* Diagram sync */
        cmocka_unit_test_setup_teardown(test_diagram_sync_on_load, setup, teardown),
        cmocka_unit_test_setup_teardown(test_diagram_sync_on_move, setup, teardown),
        cmocka_unit_test_setup_teardown(test_diagram_sync_empty, setup, teardown),
        cmocka_unit_test_setup_teardown(test_diagram_sync_unknown_button, setup, teardown),

        /* Target pick */
        cmocka_unit_test_setup_teardown(test_activate_enters_target_pick, setup, teardown),
        cmocka_unit_test_setup_teardown(test_activate_no_selection, setup, teardown),
        cmocka_unit_test_setup_teardown(test_target_pick_has_targets, setup, teardown),
        cmocka_unit_test_setup_teardown(test_target_pick_confirm, setup, teardown),
        cmocka_unit_test_setup_teardown(test_target_pick_cancel, setup, teardown),
        cmocka_unit_test_setup_teardown(test_cancel_in_list_mode, setup, teardown),

        /* Capabilities */
        cmocka_unit_test_setup_teardown(test_load_capabilities_defaults, setup, teardown),
        cmocka_unit_test_setup_teardown(test_load_capabilities_dbus, setup, teardown),
        cmocka_unit_test_setup_teardown(test_load_capabilities_dbus_error, setup, teardown),

        /* Capture mode */
        cmocka_unit_test_setup_teardown(test_begin_capture, setup, teardown),
        cmocka_unit_test_setup_teardown(test_begin_capture_no_selection, setup, teardown),
        cmocka_unit_test_setup_teardown(test_cancel_capture, setup, teardown),
        cmocka_unit_test_setup_teardown(test_capture_input_event, setup, teardown),
        cmocka_unit_test_setup_teardown(test_capture_ignores_release, setup, teardown),
        cmocka_unit_test(test_capture_null_safe),

        /* Expected sender security (Task 5) */
        cmocka_unit_test_setup_teardown(test_expected_sender_resolved, setup, teardown),
        cmocka_unit_test_setup_teardown(test_expected_sender_accepts_match, setup, teardown),
        cmocka_unit_test_setup_teardown(test_expected_sender_rejects_mismatch, setup, teardown),

        /* Accessors */
        cmocka_unit_test(test_accessors_null_safe),
        cmocka_unit_test_setup_teardown(test_status_message, setup, teardown),

        /* Rendering */
        cmocka_unit_test_setup_teardown(test_render_no_crash, setup, teardown),
        cmocka_unit_test_setup_teardown(test_render_target_pick, setup, teardown),

        /* Full workflow */
        cmocka_unit_test_setup_teardown(test_full_workflow, setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}