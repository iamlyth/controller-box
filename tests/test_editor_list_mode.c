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
#include "config/config_paths.h"
#include "ui/theme.h"
#include "ui/widget.h"
#include "test_harness.h"
#include "fb_assert.h"
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
/*  Device-mapped diagram resolution (BUG-0018)                       */
/* ------------------------------------------------------------------ */
/*
 * The editor resolves its diagram base image + marker layout through the
 * production icon mapping utilities (cbx_icon_map + cbx_icon_cache +
 * cbx_icon_lookup) keyed by device type, not a hardcoded
 * `.../svg/generic-gamepad.svg` path.  These tests assert that resolution
 * path from the editor state: the diagram base is a cache-owned texture
 * (borrowed, not owned) and set_device re-resolves it.
 */

static void test_editor_diagram_resolved_via_production_cache(void **state)
{
    pe_fixture *f = *state;

    /* Default device (NULL -> generic) resolves a real base image through
     * the production icon cache, so the editor never shows a blank diagram
     * and the texture is cache-owned (borrowed, not owned). */
    assert_non_null(f->ed.diagram.base_texture);
    assert_false(f->ed.diagram.owns_base_texture);

    /* The diagram marker layout matches the default generic table. */
    assert_ptr_equal(
        cbx_profile_diagram_active_button_pos(&f->ed.diagram, CBX_DIAG_BTN_A),
        cbx_profile_diagram_get_button_pos(CBX_DIAG_BTN_A));
}

static void test_editor_set_device_reresolves(void **state)
{
    pe_fixture *f = *state;
    static const struct { const char *type, *icon, *asset; } cases[] = {
        {"xb360", "cc-xbox-360", "xbox-360.svg"},
        {"xbox-elite", "cc-xbox-one", "xbox-one.svg"},
        {"xbox-series", "cc-xbox-series-x", "xbox-series-x.svg"},
        {"ds5", "cc-ps5", "ps5.svg"},
        {"deck", "cc-steam-deck", "steam-deck.svg"},
        {"gamepad", "generic-gamepad", "generic-gamepad.svg"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        assert_int_equal(cbx_profile_editor_set_device(&f->ed, cases[i].type), 0);
        assert_non_null(f->ed.diagram.base_texture);
        assert_string_equal(cbx_profile_editor_resolved_icon(&f->ed), cases[i].icon);
        assert_string_equal(cbx_profile_editor_resolved_asset(&f->ed), cases[i].asset);
        assert_int_equal(cbx_profile_editor_diagram_provenance(&f->ed),
            strcmp(cases[i].icon, "generic-gamepad") == 0
            ? CBX_DIAG_PROVENANCE_EXPLICIT_GENERIC
            : CBX_DIAG_PROVENANCE_SUPPORTED_MODEL);
    }

    /* A supported sidecar override wins over the model. */
    assert_int_equal(cbx_profile_editor_set_diagram_selection(
        &f->ed, "xb360", "cc-ps5"), 0);
    assert_string_equal(cbx_profile_editor_resolved_icon(&f->ed), "cc-ps5");
    assert_int_equal(cbx_profile_editor_diagram_provenance(&f->ed),
                     CBX_DIAG_PROVENANCE_PROFILE_OVERRIDE);

    /* Unsupported input alone gets an explicit generic fallback reason. */
    assert_int_equal(cbx_profile_editor_set_device(&f->ed, "mystery-pad"), 0);
    assert_string_equal(cbx_profile_editor_resolved_icon(&f->ed),
                        "generic-gamepad");
    assert_int_equal(cbx_profile_editor_diagram_provenance(&f->ed),
                     CBX_DIAG_PROVENANCE_UNSUPPORTED_FALLBACK);
}

static void test_supported_asset_failure_clears_stale(void **state)
{
    pe_fixture *f = *state;
    assert_int_equal(cbx_profile_editor_set_device(&f->ed, "xb360"), 0);
    assert_non_null(f->ed.diagram.base_texture);
    snprintf(f->ed.icon_cache.icon_dir, sizeof(f->ed.icon_cache.icon_dir),
             "/definitely/missing/controller-icons");
    assert_true(cbx_profile_editor_set_device(&f->ed, "ds5") < 0);
    assert_null(f->ed.diagram.base_texture);
    assert_null(cbx_profile_diagram_active_button_pos(&f->ed.diagram,
                                                       CBX_DIAG_BTN_A));
    assert_string_equal(cbx_profile_editor_resolved_icon(&f->ed), "cc-ps5");
    assert_int_equal(cbx_profile_editor_diagram_provenance(&f->ed),
                     CBX_DIAG_PROVENANCE_SUPPORTED_LOAD_FAILURE);
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
    /* No mappings, but the binding list still enumerates the whole
     * supported-button catalog (BUG-0016), so an empty profile has 17
     * unbound rows and a reachable add/sequential action. */
    assert_int_equal(cbx_profile_editor_binding_count(&f->ed), 0);
    assert_int_equal(cbx_profile_editor_row_count(&f->ed), CBX_DIAG_BTN_COUNT);
    assert_int_equal(cbx_list_item_count(&f->ed.binding_list),
                     CBX_DIAG_BTN_COUNT);
    assert_int_equal(cbx_profile_editor_get_selected(&f->ed), 0);
    for (int r = 0; r < cbx_profile_editor_row_count(&f->ed); r++)
        assert_int_equal(cbx_profile_editor_row_mapping(&f->ed, r), -1);
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

    int rows = cbx_profile_editor_row_count(&f->ed);
    assert_int_equal(rows, CBX_DIAG_BTN_COUNT);

    /* Walk to the last row. */
    for (int i = 1; i < rows; i++)
        assert_int_equal(cbx_profile_editor_move_down(&f->ed), i);

    /* Wrap to first. */
    assert_int_equal(cbx_profile_editor_move_down(&f->ed), 0);
}

static void test_move_wrap_up(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p = make_test_profile(3);
    cbx_profile_editor_load_profile(&f->ed, &p);

    assert_int_equal(cbx_profile_editor_get_selected(&f->ed), 0);

    /* At first row, move up wraps to the last catalog row. */
    int idx = cbx_profile_editor_move_up(&f->ed);
    assert_int_equal(idx, cbx_profile_editor_row_count(&f->ed) - 1);
}

static void test_move_empty_list(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p;
    cbx_profile_init(&p);
    cbx_profile_editor_load_profile(&f->ed, &p);

    /* An empty profile still exposes all 17 catalog rows, so navigation
     * works even though there are no mappings. */
    assert_int_equal(cbx_profile_editor_move_down(&f->ed), 1);
    assert_int_equal(cbx_profile_editor_move_up(&f->ed), 0);
    assert_int_equal(cbx_profile_editor_move_up(&f->ed),
                     CBX_DIAG_BTN_COUNT - 1);
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

    /* Row 1 is button "B". */
    cbx_profile_editor_move_down(&f->ed);
    assert_int_equal(cbx_profile_editor_get_selected(&f->ed), 1);
    assert_int_equal(cbx_profile_editor_get_diagram_highlight(&f->ed),
                       CBX_DIAG_BTN_B);

    /* Move to the "Start" catalog row (mapping 2), wherever it sits in the
     * catalog order. */
    while (cbx_profile_editor_row_button(&f->ed,
              cbx_profile_editor_get_selected(&f->ed)) != CBX_DIAG_BTN_START)
        cbx_profile_editor_move_down(&f->ed);
    assert_int_equal(cbx_profile_editor_get_diagram_highlight(&f->ed),
                       CBX_DIAG_BTN_START);
    assert_int_equal(cbx_profile_editor_row_mapping(&f->ed,
              cbx_profile_editor_get_selected(&f->ed)), 2);
}

static void test_diagram_sync_empty(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p;
    cbx_profile_init(&p);
    cbx_profile_editor_load_profile(&f->ed, &p);

    /* The catalog is never empty; selection lands on the first supported
     * button and lights it up. */
    assert_int_equal(cbx_profile_editor_get_selected(&f->ed), 0);
    assert_int_equal(cbx_profile_editor_row_button(&f->ed, 0), CBX_DIAG_BTN_A);
    assert_int_equal(cbx_profile_editor_get_diagram_highlight(&f->ed),
                       CBX_DIAG_BTN_A);
}

static void test_diagram_sync_unknown_button(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p = make_test_profile(1);
    /* Change button name to something unknown. */
    strncpy(p.mappings[0].source_event.props[0].value, "FooButton",
             sizeof(p.mappings[0].source_event.props[0].value) - 1);
    p.mappings[0].source_event.props[0].value
        [sizeof(p.mappings[0].source_event.props[0].value) - 1] = '\0';

    cbx_profile_editor_load_profile(&f->ed, &p);

    /* Every catalog button still has a row; A is now unbound. */
    assert_int_equal(cbx_profile_editor_row_button(&f->ed, 0), CBX_DIAG_BTN_A);
    assert_int_equal(cbx_profile_editor_row_mapping(&f->ed, 0), -1);
    assert_int_equal(cbx_profile_editor_get_diagram_highlight(&f->ed),
                       CBX_DIAG_BTN_A);

    /* The unknown-source mapping is preserved as a trailing row that
     * cannot light up a diagram button. */
    int extra = CBX_DIAG_BTN_COUNT;
    assert_int_equal(cbx_profile_editor_row_count(&f->ed), extra + 1);
    assert_int_equal(cbx_profile_editor_row_mapping(&f->ed, extra), 0);
    assert_int_equal(cbx_profile_editor_row_button(&f->ed, extra),
                     CBX_DIAG_BTN_NONE);
    for (int i = 0; i < extra; i++)
        cbx_profile_editor_move_down(&f->ed);
    assert_int_equal(cbx_profile_editor_get_selected(&f->ed), extra);
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

static void test_activate_empty_starts_sequential(void **state)
{
    pe_fixture *f = *state;

    cbx_profile p;
    cbx_profile_init(&p);
    cbx_profile_editor_load_profile(&f->ed, &p);

    int rc = cbx_profile_editor_activate(&f->ed);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                     CBX_EDITOR_MODE_SEQUENTIAL);
    assert_true(cbx_widget_is_visible(&f->ed.progress_bar.base));
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

/* Regression (security): the three capability sources append to the same
 * targets[] array, so the bound must be the cumulative count and not this
 * call's additions.  A source returning more than CBX_PE_MAX_TARGETS
 * entries used to fill the array and the next source's first write went
 * out of bounds past targets[CBX_PE_MAX_TARGETS-1]. */
static void test_load_capabilities_multi_source_bounded(void **state)
{
    pe_fixture *f = *state;

    cbx_profile_editor_set_dbus(&f->ed, f->backend, &f->mock,
                                  "/org/shadowblip/InputPlumber/CompositeDevice0");

    /* One source alone exceeds the picker capacity; the other two then
     * each append at least one more token. */
    char csv[8192];
    size_t off = 0;
    for (int i = 0; i < CBX_PE_MAX_TARGETS + 40; i++) {
        int n = snprintf(csv + off, sizeof(csv) - off, "%sgamepad:b%d",
                         i ? "," : "", i);
        assert_true(n > 0 && (size_t)n < sizeof(csv) - off);
        off += (size_t)n;
    }

    assert_int_equal(ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                                             "Capabilities", csv), 0);
    assert_int_equal(ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                                             "OutputCapabilities", "mouse"),
                     0);
    assert_int_equal(ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                                             "TargetCapabilities", "xb360"),
                     0);

    int rc = cbx_profile_editor_load_capabilities(&f->ed);
    assert_int_equal(rc, 0);

    /* Exactly the array capacity — no overflow, no over-capacity count. */
    assert_int_equal(cbx_profile_editor_get_target_count(&f->ed),
                     CBX_PE_MAX_TARGETS);
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

    /* In list mode, status is empty. */
    const char *status = cbx_profile_editor_get_status(&f->ed);
    assert_non_null(status);
    assert_true(strlen(status) == 0);  /* list mode shows no status hint */

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

    /* Clear to a known background distinct from the theme panel fill. */
    SDL_SetRenderDrawColor(f->sdl.renderer, 255, 255, 255, 255);
    SDL_RenderClear(f->sdl.renderer);

    /* Render the panel (includes diagram + list). */
    cbx_widget_draw(&f->panel.base, f->sdl.renderer);

    /* The editor's diagram region must have painted non-background
     * pixels — real rendering, not merely "did not crash". */
    int w, h;
    SDL_GetRendererOutputSize(f->sdl.renderer, &w, &h);
    uint8_t *buf = malloc((size_t)w * h * 4);
    assert_non_null(buf);
    assert_int_equal(fb_read_pixels(f->sdl.renderer, NULL, buf,
                                    (size_t)w * h * 4), 0);
    /* Diagram is laid out at {16, 40, 300, 300}; sample inside the
     * 320x240 window and inside the diagram's painted area. */
    SDL_Rect sample = { 20, 60, 100, 100 };
    uint8_t white[3] = {255, 255, 255};
    assert_true(fb_region_has_content(buf, w, h, &sample, white, 10));
    free(buf);
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

    /* Clear to a known background, then render the panel. */
    SDL_SetRenderDrawColor(f->sdl.renderer, 255, 255, 255, 255);
    SDL_RenderClear(f->sdl.renderer);
    cbx_widget_draw(&f->panel.base, f->sdl.renderer);

    /* The diagram (still visible in target-pick mode) must paint
     * non-background pixels. */
    int w, h;
    SDL_GetRendererOutputSize(f->sdl.renderer, &w, &h);
    uint8_t *buf = malloc((size_t)w * h * 4);
    assert_non_null(buf);
    assert_int_equal(fb_read_pixels(f->sdl.renderer, NULL, buf,
                                    (size_t)w * h * 4), 0);
    SDL_Rect sample = { 20, 60, 100, 100 };
    uint8_t white[3] = {255, 255, 255};
    assert_true(fb_region_has_content(buf, w, h, &sample, white, 10));
    free(buf);
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
/*  Tests: capture interception ownership (Task 16)                   */
/* ------------------------------------------------------------------ */

#define PE_TEST_COMPOSITE "/org/shadowblip/InputPlumber/CompositeDevice0"
#define PE_TEST_DBUS_DEVICE \
    "/org/shadowblip/InputPlumber/CompositeDevice0/dbus0"

static void
pe_set_composite_context(pe_fixture *f, const char *prior_mode)
{
    assert_int_equal(ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                                              "InterceptMode", prior_mode),
                     0);
    assert_int_equal(ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                                              "DbusDevices",
                                              PE_TEST_DBUS_DEVICE),
                     0);
    cbx_profile_editor_set_dbus(&f->ed, f->backend, &f->mock,
                                  PE_TEST_COMPOSITE);
}

/* List capture saves the composite's InterceptMode, switches to
 * GAMEPAD_ONLY for the capture, and restores the prior value on cancel. */
static void test_capture_acquires_and_restores_interception(void **state)
{
    pe_fixture *f = *state;
    cbx_profile p = make_test_profile(1);
    cbx_profile_editor_load_profile(&f->ed, &p);
    pe_set_composite_context(f, "1");

    assert_int_equal(cbx_profile_editor_begin_capture(&f->ed), 0);
    assert_true(cbx_profile_editor_is_capture_active(&f->ed));
    assert_true(cbx_profile_editor_intercept_active(&f->ed));
    assert_string_equal(f->mock.last_set_prop, "InterceptMode");
    assert_string_equal(f->mock.last_set_value, "3");

    /* The composite's DBusDevice list was resolved for authentication. */
    assert_int_equal(cbx_profile_editor_dbus_device_count(&f->ed), 1);
    assert_string_equal(cbx_profile_editor_dbus_device(&f->ed, 0),
                          PE_TEST_DBUS_DEVICE);

    assert_int_equal(cbx_profile_editor_cancel(&f->ed), 0);
    assert_false(cbx_profile_editor_is_capture_active(&f->ed));
    assert_false(cbx_profile_editor_intercept_active(&f->ed));
    assert_string_equal(f->mock.last_set_value, "1");
}

/* The restore target is the exact prior mode, not a hardcoded PASS: an
 * overlay that had the composite in ALL is returned to ALL. */
static void test_capture_restores_exact_prior_mode(void **state)
{
    pe_fixture *f = *state;
    cbx_profile p = make_test_profile(1);
    cbx_profile_editor_load_profile(&f->ed, &p);
    pe_set_composite_context(f, "2");

    assert_int_equal(cbx_profile_editor_begin_capture(&f->ed), 0);
    assert_string_equal(f->mock.last_set_value, "3");
    cbx_profile_editor_cancel_capture(&f->ed);
    assert_string_equal(f->mock.last_set_value, "2");
}

/* InputEvents from a device path outside the selected composite's
 * DbusDevices are rejected (BUG-0017: reject other devices' events). */
static void test_capture_rejects_foreign_device_path(void **state)
{
    pe_fixture *f = *state;
    cbx_profile p = make_test_profile(1);
    cbx_profile_editor_load_profile(&f->ed, &p);
    pe_set_composite_context(f, "1");
    assert_int_equal(cbx_profile_editor_begin_capture(&f->ed), 0);

    cbx_profile_editor_on_input_event(IP_INPUT_A, IP_INPUT_CAT_BUTTON, 1.0,
                                        "A",
                                        "/org/shadowblip/InputPlumber/CompositeDevice1/dbus0",
                                        &f->ed);
    assert_true(cbx_profile_editor_is_capture_active(&f->ed));

    /* A matching device path still captures. */
    cbx_profile_editor_on_input_event(IP_INPUT_A, IP_INPUT_CAT_BUTTON, 1.0,
                                        "A", PE_TEST_DBUS_DEVICE, &f->ed);
    assert_false(cbx_profile_editor_is_capture_active(&f->ed));
}

/* A subscription failure aborts capture before mode is changed, so no
 * interception is left owned by a broken editor. */
static void test_capture_subscription_failure_aborts(void **state)
{
    pe_fixture *f = *state;
    cbx_profile p = make_test_profile(1);
    cbx_profile_editor_load_profile(&f->ed, &p);
    pe_set_composite_context(f, "1");
    f->mock.subscribe_fail_rc = -EIO;

    assert_int_equal(cbx_profile_editor_begin_capture(&f->ed), -EIO);
    assert_false(cbx_profile_editor_is_capture_active(&f->ed));
    assert_false(cbx_profile_editor_intercept_active(&f->ed));
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                       CBX_EDITOR_MODE_LIST);
    /* InterceptMode was never written. */
    assert_int_not_equal(strcmp(f->mock.last_set_prop, "InterceptMode"), 0);
}

/* Sequential capture also owns interception and restores it on cancel. */
static void test_sequential_acquires_and_restores_interception(void **state)
{
    pe_fixture *f = *state;
    cbx_profile p = make_test_profile(1);
    cbx_profile_editor_load_profile(&f->ed, &p);
    pe_set_composite_context(f, "1");

    assert_int_equal(cbx_profile_editor_begin_sequential(&f->ed), 0);
    assert_true(cbx_profile_editor_intercept_active(&f->ed));
    assert_string_equal(f->mock.last_set_value, "3");

    cbx_profile_editor_cancel_sequential(&f->ed);
    assert_false(cbx_profile_editor_intercept_active(&f->ed));
    assert_string_equal(f->mock.last_set_value, "1");
}

/* A backend replacement while capture is open restores the old owner and
 * drops the previous composite's device filter. */
static void test_backend_replacement_releases_and_repoints(void **state)
{
    pe_fixture *f = *state;
    cbx_profile p = make_test_profile(1);
    cbx_profile_editor_load_profile(&f->ed, &p);
    pe_set_composite_context(f, "1");
    assert_int_equal(cbx_profile_editor_begin_capture(&f->ed), 0);
    assert_true(cbx_profile_editor_intercept_active(&f->ed));

    /* Repoint at another composite on the same backend. */
    assert_int_equal(ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                                              "InterceptMode", "1"), 0);
    assert_int_equal(ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                                              "DbusDevices",
                                              PE_TEST_DBUS_DEVICE), 0);
    cbx_profile_editor_set_dbus(&f->ed, f->backend, &f->mock,
        "/org/shadowblip/InputPlumber/CompositeDevice1");
    assert_string_equal(cbx_profile_editor_composite_path(&f->ed),
        "/org/shadowblip/InputPlumber/CompositeDevice1");
    assert_true(cbx_profile_editor_intercept_active(&f->ed));

    cbx_profile_editor_cancel_capture(&f->ed);
    assert_false(cbx_profile_editor_intercept_active(&f->ed));
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
    assert_int_equal(cbx_profile_editor_row_count(&f->ed), CBX_DIAG_BTN_COUNT);

    /* Navigate to row 1 (button "B"). */
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
    assert_int_equal(cbx_profile_editor_get_editing_index(&f->ed), 1);

    cbx_profile_editor_activate(&f->ed);  /* confirm */
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                       CBX_EDITOR_MODE_LIST);

    /* Navigate to the "Start" catalog row (mapping 2). */
    while (cbx_profile_editor_row_button(&f->ed,
              cbx_profile_editor_get_selected(&f->ed)) != CBX_DIAG_BTN_START)
        cbx_profile_editor_move_down(&f->ed);
    assert_int_equal(cbx_profile_editor_get_diagram_highlight(&f->ed),
                       CBX_DIAG_BTN_START);
    assert_int_equal(cbx_profile_editor_row_mapping(&f->ed,
              cbx_profile_editor_get_selected(&f->ed)), 2);

    /* Capture mode */
    cbx_profile_editor_begin_capture(&f->ed);
    assert_true(cbx_profile_editor_is_capture_active(&f->ed));
    assert_int_equal(cbx_profile_editor_get_editing_index(&f->ed), 2);

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
/*  Tests: catalog enumeration and unbound-row activation (BUG-0016)  */
/* ------------------------------------------------------------------ */

/* Build a 6-mapping NES profile (A, B, D-pad) like the shipped Default. */
static cbx_profile make_nes_profile(void)
{
    static const char *btns[6] = {"A", "B", "Up", "Down", "Left", "Right"};
    cbx_profile p;
    cbx_profile_init(&p);
    strncpy(p.name, "NES", sizeof(p.name) - 1);
    for (int i = 0; i < 6; i++) {
        cbx_profile_mapping *m = &p.mappings[p.mapping_count];
        memset(m, 0, sizeof(*m));
        strncpy(m->name, btns[i], sizeof(m->name) - 1);
        strncpy(m->source_event.device_class, "gamepad",
                 sizeof(m->source_event.device_class) - 1);
        m->source_event.prop_count = 1;
        strncpy(m->source_event.props[0].key, "button",
                 sizeof(m->source_event.props[0].key) - 1);
        snprintf(m->source_event.props[0].value,
                  sizeof(m->source_event.props[0].value), "%s", btns[i]);
        m->target_event_count = 1;
        strncpy(m->target_events[0].device_class, "keyboard",
                 sizeof(m->target_events[0].device_class) - 1);
        snprintf(m->target_events[0].value,
                  sizeof(m->target_events[0].value), "Key%s", btns[i]);
        p.mapping_count++;
    }
    return p;
}

/* Index of the catalog row for a supported button, or -1. */
static int
row_for_button(const cbx_profile_editor *ed, cbx_diag_button btn)
{
    for (int r = 0; r < cbx_profile_editor_row_count(ed); r++)
        if (cbx_profile_editor_row_button(ed, r) == btn)
            return r;
    return -1;
}

/* True if any pixel in a region differs between two full frames. */
static bool
pe_region_differs(const uint8_t *a, const uint8_t *b, int w, int h,
                  const SDL_Rect *r)
{
    for (int y = r->y; y < r->y + r->h && y < h; y++) {
        for (int x = r->x; x < r->x + r->w && x < w; x++) {
            int idx = (y * w + x) * 4;
            if (a[idx] != b[idx] || a[idx + 1] != b[idx + 1] ||
                a[idx + 2] != b[idx + 2] || a[idx + 3] != b[idx + 3])
                return true;
        }
    }
    return false;
}

/* The binding list enumerates the whole supported virtual-button catalog
 * (BUG-0016): every one of the 17 buttons is a row, whether the loaded
 * profile binds it or not. */
static void test_catalog_enumerates_all_seventeen(void **state)
{
    pe_fixture *f = *state;
    cbx_profile p = make_nes_profile();
    cbx_profile_editor_load_profile(&f->ed, &p);

    assert_int_equal(cbx_profile_editor_row_count(&f->ed), CBX_DIAG_BTN_COUNT);
    assert_int_equal(cbx_list_item_count(&f->ed.binding_list),
                     CBX_DIAG_BTN_COUNT);

    static const char *nes[6] = {"A", "B", "Up", "Down", "Left", "Right"};
    bool seen[CBX_DIAG_BTN_COUNT] = { false };
    for (int r = 0; r < CBX_DIAG_BTN_COUNT; r++) {
        cbx_diag_button b = cbx_profile_editor_row_button(&f->ed, r);
        assert_true(b >= 0 && b < CBX_DIAG_BTN_COUNT);
        assert_false(seen[b]);
        seen[b] = true;

        const char *name = cbx_profile_diagram_button_name(b);
        bool bound = false;
        for (int i = 0; i < 6; i++)
            if (strcmp(name, nes[i]) == 0)
                bound = true;
        if (bound)
            assert_true(cbx_profile_editor_row_mapping(&f->ed, r) >= 0);
        else
            assert_int_equal(cbx_profile_editor_row_mapping(&f->ed, r), -1);
    }
    for (int b = 0; b < CBX_DIAG_BTN_COUNT; b++)
        assert_true(seen[b]);
}

/* Enumerating the catalog must not mutate a loaded profile: the shipped
 * Default stays exactly as loaded (immutable) while its 11 unbound buttons
 * are still shown as editable rows. */
static void test_default_profile_immutable_under_catalog(void **state)
{
    pe_fixture *f = *state;

    char path[512];
    snprintf(path, sizeof(path), "%s/default.yaml",
             cbx_builtin_profiles_dir());
    cbx_profile def;
    cbx_profile_init(&def);
    int rc = cbx_profile_load(&def, path);
    assert_int_equal(rc, 0);
    assert_int_equal(def.mapping_count, 6);

    cbx_profile_editor_load_profile(&f->ed, &def);
    assert_false(cbx_profile_editor_is_dirty(&f->ed));
    assert_int_equal(cbx_profile_editor_binding_count(&f->ed), 6);
    assert_int_equal(cbx_profile_editor_row_count(&f->ed), CBX_DIAG_BTN_COUNT);

    const cbx_profile *loaded = cbx_profile_editor_get_profile(&f->ed);
    assert_non_null(loaded);
    assert_int_equal(loaded->mapping_count, 6);

    int unbound = 0;
    for (int r = 0; r < cbx_profile_editor_row_count(&f->ed); r++)
        if (cbx_profile_editor_row_mapping(&f->ed, r) < 0)
            unbound++;
    assert_int_equal(unbound, 11);
}

/* Activating an unbound row creates exactly the intended mapping. */
static void test_activate_unbound_row_creates_mapping(void **state)
{
    pe_fixture *f = *state;
    cbx_profile p = make_nes_profile();
    cbx_profile_editor_load_profile(&f->ed, &p);
    assert_int_equal(cbx_profile_editor_binding_count(&f->ed), 6);

    int xrow = row_for_button(&f->ed, CBX_DIAG_BTN_X);
    assert_true(xrow >= 0);
    assert_int_equal(cbx_profile_editor_row_mapping(&f->ed, xrow), -1);
    while (cbx_profile_editor_get_selected(&f->ed) != xrow)
        cbx_profile_editor_move_down(&f->ed);

    assert_int_equal(cbx_profile_editor_activate(&f->ed), 0);
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                     CBX_EDITOR_MODE_BINDING_EDIT);
    assert_int_equal(cbx_profile_editor_binding_count(&f->ed), 7);
    assert_true(cbx_profile_editor_is_dirty(&f->ed));

    const cbx_profile *prof = cbx_profile_editor_get_profile(&f->ed);
    int idx = cbx_profile_editor_get_editing_index(&f->ed);
    assert_true(idx >= 0 && idx < prof->mapping_count);
    assert_string_equal(prof->mappings[idx].source_event.props[0].value, "X");
    /* The X row now points at the new mapping. */
    assert_int_equal(cbx_profile_editor_row_mapping(&f->ed, xrow), idx);

    /* Exactly one X mapping exists (no duplicate). */
    int xcount = 0;
    for (int i = 0; i < prof->mapping_count; i++)
        for (int j = 0; j < prof->mappings[i].source_event.prop_count; j++)
            if ((strcmp(prof->mappings[i].source_event.props[j].key,
                        "button") == 0 ||
                 strcmp(prof->mappings[i].source_event.props[j].key,
                        "axis") == 0) &&
                strcmp(prof->mappings[i].source_event.props[j].value,
                       "X") == 0)
                xcount++;
    assert_int_equal(xcount, 1);
}

/* Re-activating an already-created row never duplicates the mapping. */
static void test_activate_existing_row_no_duplicate(void **state)
{
    pe_fixture *f = *state;
    cbx_profile p = make_nes_profile();
    cbx_profile_editor_load_profile(&f->ed, &p);

    int xrow = row_for_button(&f->ed, CBX_DIAG_BTN_X);
    while (cbx_profile_editor_get_selected(&f->ed) != xrow)
        cbx_profile_editor_move_down(&f->ed);

    cbx_profile_editor_activate(&f->ed);       /* creates X */
    assert_int_equal(cbx_profile_editor_binding_count(&f->ed), 7);
    cbx_profile_editor_cancel(&f->ed);          /* back to LIST */
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed), CBX_EDITOR_MODE_LIST);

    /* Selection is preserved on the same row; activating again edits the
     * existing X mapping instead of appending a duplicate. */
    assert_int_equal(cbx_profile_editor_get_selected(&f->ed), xrow);
    assert_int_equal(cbx_profile_editor_activate(&f->ed), 0);
    assert_int_equal(cbx_profile_editor_binding_count(&f->ed), 7);
}

/* Pointer path: a mouse click on an unbound row activates that exact row
 * through the list widget's production on_select callback. */
static void test_activate_unbound_row_pointer_path(void **state)
{
    pe_fixture *f = *state;
    cbx_profile p = make_nes_profile();
    cbx_profile_editor_load_profile(&f->ed, &p);

    int xrow = row_for_button(&f->ed, CBX_DIAG_BTN_X);
    assert_true(xrow >= 0 && xrow < f->ed.binding_list.visible_count);

    SDL_Event ev = {0};
    ev.type = SDL_MOUSEBUTTONDOWN;
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.x = f->ed.binding_list.base.rect.x + 4;
    ev.button.y = f->ed.binding_list.base.rect.y
                  + xrow * f->ed.binding_list.item_h + 1;
    assert_true(cbx_widget_handle_event(&f->ed.binding_list.base, &ev));
    ev.type = SDL_MOUSEBUTTONUP;
    assert_true(cbx_widget_handle_event(&f->ed.binding_list.base, &ev));

    assert_int_equal(cbx_profile_editor_get_selected(&f->ed), xrow);
    assert_int_equal(cbx_profile_editor_get_mode(&f->ed),
                     CBX_EDITOR_MODE_BINDING_EDIT);
    assert_int_equal(cbx_profile_editor_binding_count(&f->ed), 7);
    assert_int_equal(cbx_profile_editor_get_diagram_highlight(&f->ed),
                     CBX_DIAG_BTN_X);
}

/* The 17-row list scrolls so the selected row stays visible. */
static void test_catalog_scrolling(void **state)
{
    pe_fixture *f = *state;
    cbx_profile p;
    cbx_profile_init(&p);
    cbx_profile_editor_load_profile(&f->ed, &p);

    cbx_list *lst = &f->ed.binding_list;
    assert_int_equal(lst->item_count, CBX_DIAG_BTN_COUNT);
    assert_true(lst->visible_count > 0);
    assert_true(lst->visible_count < CBX_DIAG_BTN_COUNT);

    for (int i = 0; i < CBX_DIAG_BTN_COUNT - 1; i++)
        cbx_profile_editor_move_down(&f->ed);
    assert_int_equal(cbx_profile_editor_get_selected(&f->ed),
                     CBX_DIAG_BTN_COUNT - 1);
    assert_true(lst->scroll_offset > 0);
    assert_true(cbx_profile_editor_get_selected(&f->ed) >= lst->scroll_offset);
    assert_true(cbx_profile_editor_get_selected(&f->ed)
                < lst->scroll_offset + lst->visible_count);
}

/* Every catalog selection paints its highlight in that button's diagram
 * region (the always-visible diagram stays synchronised with the list). */
static void test_diagram_regions_all_buttons(void **state)
{
    pe_fixture *f = *state;
    cbx_profile p;
    cbx_profile_init(&p);
    cbx_profile_editor_load_profile(&f->ed, &p);

    SDL_Rect diag_rect;
    cbx_widget_get_rect(&f->ed.diagram.base, &diag_rect);
    /* The renderer anchors markers inside the aspect-fitted content box. */
    SDL_Rect content = diag_rect;
    cbx_profile_diagram_content_rect(&f->ed.diagram, &diag_rect, &content);
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(f->sdl.renderer, &w, &h);
    uint8_t *with_hl = malloc((size_t)w * h * 4);
    uint8_t *no_hl   = malloc((size_t)w * h * 4);
    assert_non_null(with_hl);
    assert_non_null(no_hl);

    for (int b = 0; b < CBX_DIAG_BTN_COUNT; b++) {
        int r = row_for_button(&f->ed, (cbx_diag_button)b);
        assert_true(r >= 0);
        while (cbx_profile_editor_get_selected(&f->ed) != r)
            cbx_profile_editor_move_down(&f->ed);
        assert_int_equal(cbx_profile_editor_get_diagram_highlight(&f->ed), b);

        /* Frame with the row selected (highlight on). */
        SDL_SetRenderDrawColor(f->sdl.renderer, 255, 255, 255, 255);
        SDL_RenderClear(f->sdl.renderer);
        cbx_widget_draw(&f->ed.diagram.base, f->sdl.renderer);
        assert_int_equal(fb_read_pixels(f->sdl.renderer, NULL, with_hl,
                                        (size_t)w * h * 4), 0);

        /* Same diagram with the highlight cleared. */
        cbx_profile_diagram_clear_highlight(&f->ed.diagram);
        SDL_SetRenderDrawColor(f->sdl.renderer, 255, 255, 255, 255);
        SDL_RenderClear(f->sdl.renderer);
        cbx_widget_draw(&f->ed.diagram.base, f->sdl.renderer);
        assert_int_equal(fb_read_pixels(f->sdl.renderer, NULL, no_hl,
                                        (size_t)w * h * 4), 0);

        const cbx_diag_button_pos *pos =
            cbx_profile_diagram_active_button_pos(&f->ed.diagram,
                                                  (cbx_diag_button)b);
        assert_non_null(pos);
        SDL_Rect br;
        br.x = content.x + (int)(pos->x * (float)content.w);
        br.y = content.y + (int)(pos->y * (float)content.h);
        br.w = (int)(pos->w * (float)content.w);
        if (br.w < 1) br.w = 1;
        br.h = (int)(pos->h * (float)content.h);
        if (br.h < 1) br.h = 1;
        assert_true(pe_region_differs(with_hl, no_hl, w, h, &br));

        /* Restore the highlight for the next iteration's bookkeeping. */
        cbx_profile_diagram_highlight(&f->ed.diagram, (cbx_diag_button)b);
    }

    free(with_hl);
    free(no_hl);
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
        cmocka_unit_test_setup_teardown(test_activate_empty_starts_sequential, setup, teardown),
        cmocka_unit_test_setup_teardown(test_target_pick_has_targets, setup, teardown),
        cmocka_unit_test_setup_teardown(test_target_pick_confirm, setup, teardown),
        cmocka_unit_test_setup_teardown(test_target_pick_cancel, setup, teardown),
        cmocka_unit_test_setup_teardown(test_cancel_in_list_mode, setup, teardown),

        /* Capabilities */
        cmocka_unit_test_setup_teardown(test_load_capabilities_defaults, setup, teardown),
        cmocka_unit_test_setup_teardown(test_load_capabilities_dbus, setup, teardown),
        cmocka_unit_test_setup_teardown(test_load_capabilities_dbus_error, setup, teardown),
        cmocka_unit_test_setup_teardown(
            test_load_capabilities_multi_source_bounded, setup, teardown),

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

        /* Capture interception ownership (Task 16) */
        cmocka_unit_test_setup_teardown(
            test_capture_acquires_and_restores_interception, setup, teardown),
        cmocka_unit_test_setup_teardown(
            test_capture_restores_exact_prior_mode, setup, teardown),
        cmocka_unit_test_setup_teardown(
            test_capture_rejects_foreign_device_path, setup, teardown),
        cmocka_unit_test_setup_teardown(
            test_capture_subscription_failure_aborts, setup, teardown),
        cmocka_unit_test_setup_teardown(
            test_sequential_acquires_and_restores_interception, setup, teardown),
        cmocka_unit_test_setup_teardown(
            test_backend_replacement_releases_and_repoints, setup, teardown),

        /* Accessors */
        cmocka_unit_test(test_accessors_null_safe),
        cmocka_unit_test_setup_teardown(test_status_message, setup, teardown),

        /* Rendering */
        cmocka_unit_test_setup_teardown(test_render_no_crash, setup, teardown),
        cmocka_unit_test_setup_teardown(test_render_target_pick, setup, teardown),

        /* Full workflow */
        cmocka_unit_test_setup_teardown(test_full_workflow, setup, teardown),

        /* Catalog enumeration and unbound-row activation (BUG-0016) */
        cmocka_unit_test_setup_teardown(
            test_catalog_enumerates_all_seventeen, setup, teardown),
        cmocka_unit_test_setup_teardown(
            test_default_profile_immutable_under_catalog, setup, teardown),
        cmocka_unit_test_setup_teardown(
            test_activate_unbound_row_creates_mapping, setup, teardown),
        cmocka_unit_test_setup_teardown(
            test_activate_existing_row_no_duplicate, setup, teardown),
        cmocka_unit_test_setup_teardown(
            test_activate_unbound_row_pointer_path, setup, teardown),
        cmocka_unit_test_setup_teardown(test_catalog_scrolling, setup, teardown),
        cmocka_unit_test_setup_teardown(
            test_diagram_regions_all_buttons, setup, teardown),

        /* Device-mapped diagram resolution (BUG-0018) */
        cmocka_unit_test_setup_teardown(
            test_editor_diagram_resolved_via_production_cache, setup, teardown),
        cmocka_unit_test_setup_teardown(
            test_editor_set_device_reresolves, setup, teardown),
        cmocka_unit_test_setup_teardown(
            test_supported_asset_failure_clears_stale, setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}