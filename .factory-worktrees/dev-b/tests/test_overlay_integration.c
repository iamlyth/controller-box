/*
 * test_overlay_integration.c — Full overlay lifecycle integration test.
 *
 * Task 33 — Overlay integration test.
 *
 * Exercises the complete overlay workflow with mock DBus + SDL2 dummy driver:
 *   1. Trigger registration (SetInterceptActivation + InterceptMode=PASS)
 *   2. Lifecycle activation (IDLE → ACTIVATING → VISIBLE)
 *   3. Grid build with composites, settings, assignments, profiles
 *   4. Player Mode navigation (Left/Right changes active column per controller,
 *      Up/Down cycles profile)
 *   5. Profile change applied to InputPlumber (LoadProfilePath via mock DBus)
 *   6. Conflict detection (two controllers on same P-slot)
 *   7. Host Mode (R3 enters, host navigates rows, edits other row's slot,
 *      R3 exits)
 *   8. Close: conflict resolve → assignment sync → save → InterceptMode=PASS → IDLE
 *
 * Uses SDL2 dummy driver + mock DBus backend.  No real DBus or display needed.
 */
#include "overlay/close.h"
#include "overlay/conflict.h"
#include "overlay/grid_render.h"
#include "overlay/host_mode.h"
#include "overlay/lifecycle.h"
#include "overlay/player_mode.h"
#include "overlay/profile_cycle.h"
#include "overlay/trigger.h"

#include "config/config_assignments.h"
#include "config/config_settings.h"
#include "config/config_profile_list.h"

#include "dbus/ip_composite.h"
#include "dbus/ip_connection.h"
#include "dbus/ip_device_model.h"

#include "dbus_mock.h"
#include "test_harness.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include <cmocka.h>

/* --- Constants --------------------------------------------------------- */

#define COMP_PATH_0 "/org/shadowblip/InputPlumber/CompositeDevice0"
#define COMP_PATH_1 "/org/shadowblip/InputPlumber/CompositeDevice1"
#define COMP_PATH_2 "/org/shadowblip/InputPlumber/CompositeDevice2"

/* --- Temp HOME management --------------------------------------------- */

static char test_home[PATH_MAX];

static int
setup_home(void **state)
{
    (void)state;
    snprintf(test_home, sizeof(test_home), "/tmp/cbx-integ-XXXXXX");
    if (!mkdtemp(test_home))
        return -1;
    setenv("HOME", test_home, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");
    return 0;
}

static int
teardown_home(void **state)
{
    (void)state;
    char cmd[PATH_MAX * 2 + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", test_home);
    int rc = system(cmd);
    (void)rc;
    return 0;
}

/* --- Grid helpers ------------------------------------------------------ */

static cbx_grid_composite_info
make_comp(const char *id, const char *name, const char *path)
{
    cbx_grid_composite_info c;
    memset(&c, 0, sizeof(c));
    if (id)   snprintf(c.id, sizeof(c.id), "%s", id);
    if (name) snprintf(c.model_name, sizeof(c.model_name), "%s", name);
    if (path) snprintf(c.composite_path, sizeof(c.composite_path), "%s", path);
    return c;
}

static void
build_test_grid(cbx_select_grid *g, int rows)
{
    cbx_grid_composite_info comps[CBX_GRID_MAX_ROWS];
    for (int i = 0; i < rows; i++) {
        char id[32], name[32], path[64];
        snprintf(id, sizeof(id), "ORDER:%d", i);
        snprintf(name, sizeof(name), "Controller %d", i);
        snprintf(path, sizeof(path),
                 "/org/shadowblip/InputPlumber/CompositeDevice%d", i);
        comps[i] = make_comp(id, name, path);
    }

    cbx_settings s;
    memset(&s, 0, sizeof(s));
    s.virtual_controllers.count = 4;
    for (int i = 0; i < 4; i++)
        snprintf(s.virtual_controllers.types[i],
                 CBX_MAX_TYPE_LEN, "xb360");

    cbx_assignments a;
    cbx_assignments_init(&a);

    cbx_select_grid_build(g, comps, rows, &s, &a);

    /* Add profiles for cycling. */
    cbx_select_grid_add_profile(g, "default");
    cbx_select_grid_add_profile(g, "fighting");
    cbx_select_grid_add_profile(g, "fps");
}

static void
move_to_col(cbx_select_grid *g, int row_idx, int target_col)
{
    int cur = cbx_select_grid_get_cur_col(g, row_idx);
    while (cur < target_col) {
        cbx_select_grid_move_right(g, row_idx);
        cur++;
    }
    while (cur > target_col) {
        cbx_select_grid_move_left(g, row_idx);
        cur--;
    }
}

/* --- Callback tracking ------------------------------------------------- */

typedef struct {
    int slot_changes;
    int last_row;
    int last_slot;
    int profile_changes;
    char last_profile[CBX_GRID_PROFILE_LEN];
    char last_composite_path[CBX_MAX_PATH_LEN];
} pm_callbacks;

static int
on_slot_change(int row_idx, int new_slot, void *userdata)
{
    pm_callbacks *cb = userdata;
    if (!cb) return 0;
    cb->slot_changes++;
    cb->last_row = row_idx;
    cb->last_slot = new_slot;
    return 0;
}

static int
on_profile_change(int row_idx, const char *profile,
                   const char *composite_path, void *userdata)
{
    pm_callbacks *cb = userdata;
    if (!cb) return 0;
    cb->profile_changes++;
    cb->last_row = row_idx;
    if (profile) snprintf(cb->last_profile, sizeof(cb->last_profile), "%s", profile);
    if (composite_path)
        snprintf(cb->last_composite_path,
                 sizeof(cb->last_composite_path), "%s", composite_path);
    return 0;
}

static int
hm_on_slot_change(int row_idx, int new_slot, void *userdata)
{
    pm_callbacks *cb = userdata;
    if (!cb) return 0;
    cb->slot_changes++;
    cb->last_row = row_idx;
    cb->last_slot = new_slot;
    return 0;
}

/* --- Integration fixture ----------------------------------------------- */

typedef struct {
    ip_dbus_mock          mock;
    const ip_dbus_backend *backend;
    cbx_overlay_lifecycle lc;
    TestSdlState           sdl;
    cbx_select_grid        grid;
    cbx_player_mode        pm;
    cbx_host_mode          hm;
    cbx_profile_cycle      pc;
    cbx_profile_list       profiles;
    cbx_assignments        assignments;
    pm_callbacks           cb_track;
} integ_fixture;

static int
setup_integ(void **state)
{
    if (setup_home(state) != 0)
        return -1;

    integ_fixture *f = malloc(sizeof(*f));
    if (!f) return -1;
    memset(f, 0, sizeof(*f));

    /* SDL2 dummy driver for rendering. */
    test_harness_sdl_init(&f->sdl);

    /* Mock DBus. */
    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);

    /* Lifecycle: state machine + SDL renderer (dummy). */
    cbx_overlay_lifecycle_init(&f->lc, f->backend, f->mock.bus,
                               COMP_PATH_0, NULL, f->sdl.renderer);
    f->lc.fade_in_ms = 0;
    f->lc.fade_out_ms = 0;

    /* Build a 3-controller grid. */
    build_test_grid(&f->grid, 3);

    /* Player mode. */
    cbx_player_mode_init(&f->pm, &f->grid);
    f->pm.on_slot_change = on_slot_change;
    f->pm.slot_change_data = &f->cb_track;
    f->pm.on_profile_change = on_profile_change;
    f->pm.profile_change_data = &f->cb_track;

    /* Host mode. */
    cbx_host_mode_init(&f->hm);
    f->hm.on_slot_change = hm_on_slot_change;
    f->hm.slot_change_data = &f->cb_track;

    /* Profile list (in-memory). */
    memset(&f->profiles, 0, sizeof(f->profiles));
    {
        cbx_profile_entry *e;
        e = &f->profiles.entries[0];
        snprintf(e->filename, sizeof(e->filename), "default");
        snprintf(e->path, sizeof(e->path),
                 "/usr/share/inputplumber/profiles/default.yaml");
        e->is_system = true;
        e->is_default = true;
        e->read_only = true;

        e = &f->profiles.entries[1];
        snprintf(e->filename, sizeof(e->filename), "fighting");
        snprintf(e->path, sizeof(e->path),
                 "/usr/share/inputplumber/profiles/fighting.yaml");

        e = &f->profiles.entries[2];
        snprintf(e->filename, sizeof(e->filename), "fps");
        snprintf(e->path, sizeof(e->path),
                 "/usr/share/inputplumber/profiles/fps.yaml");

        f->profiles.count = 3;
    }

    /* Profile cycle context. */
    cbx_profile_cycle_init(&f->pc, f->backend, f->mock.bus,
                           &f->assignments, &f->profiles);

    /* Assignments. */
    cbx_assignments_init(&f->assignments);

    /* Callback tracking. */
    memset(&f->cb_track, 0, sizeof(f->cb_track));

    *state = f;
    return 0;
}

static int
teardown_integ(void **state)
{
    integ_fixture *f = *state;
    if (f) {
        ip_dbus_mock_reset(&f->mock);
        test_harness_sdl_shutdown(&f->sdl);
        free(f);
    }
    teardown_home(state);
    return 0;
}

/* ======================================================================== */
/*  Test 1: Trigger registration on a single composite device.               */
/* ======================================================================== */

static void
test_trigger_registration(void **state)
{
    (void)state;
    ip_dbus_mock mock;
    const ip_dbus_backend *backend;

    ip_dbus_mock_init(&mock);
    backend = ip_dbus_mock_backend(&mock);

    /* Expect SetInterceptActivation + InterceptMode set (PASS). */
    ip_dbus_mock_expect_ok(&mock, IP_IFACE_COMPOSITE,
                           "SetInterceptActivation", NULL);
    ip_dbus_mock_expect_ok(&mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", NULL);

    int rc = cbx_trigger_register(backend, mock.bus, COMP_PATH_0,
                                   "Select+A");
    assert_int_equal(rc, 0);

    ip_dbus_mock_reset(&mock);
}

/* ======================================================================== */
/*  Test 2: Trigger registration on multiple composites.                    */
/* ======================================================================== */

static void
test_trigger_registration_all(void **state)
{
    (void)state;
    ip_dbus_mock mock;
    const ip_dbus_backend *backend;

    ip_dbus_mock_init(&mock);
    backend = ip_dbus_mock_backend(&mock);

    /* Both methods are looked up by (iface, member) and the mock returns
     * the first match, so a single expectation covers all devices. */
    ip_dbus_mock_expect_ok(&mock, IP_IFACE_COMPOSITE,
                           "SetInterceptActivation", NULL);
    ip_dbus_mock_expect_ok(&mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", NULL);

    const char *paths[] = { COMP_PATH_0, COMP_PATH_1, COMP_PATH_2 };
    int rc = cbx_trigger_register_all(backend, mock.bus, paths, 3,
                                        "Select+A");
    assert_int_equal(rc, 0);

    ip_dbus_mock_reset(&mock);
}

/* ======================================================================== */
/*  Test 3: Full lifecycle — activate, navigate in Player Mode, close.      */
/* ======================================================================== */

static void
test_full_lifecycle_player_mode(void **state)
{
    integ_fixture *f = *state;

    /* --- Activate overlay --- */
    assert_int_equal(cbx_overlay_lifecycle_get_state(&f->lc),
                     CBX_OVERLAY_IDLE);

    int rc = cbx_overlay_lifecycle_activate(&f->lc);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_overlay_lifecycle_get_state(&f->lc),
                     CBX_OVERLAY_VISIBLE);

    /* --- Player Mode: controller 0 moves right to P1 --- */
    int result = cbx_player_mode_handle(&f->pm, 0, CBX_PM_RIGHT);
    assert_int_equal(result, CBX_PM_RESULT_MOVED);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->grid, 0), 1);

    /* Slot change callback fired. */
    assert_int_equal(f->cb_track.slot_changes, 1);
    assert_int_equal(f->cb_track.last_row, 0);
    assert_int_equal(f->cb_track.last_slot, 0);  /* col 1 → slot 0 (P1) */

    /* --- Player Mode: controller 1 moves right to P2 --- */
    result = cbx_player_mode_handle(&f->pm, 1, CBX_PM_RIGHT);
    assert_int_equal(result, CBX_PM_RESULT_MOVED);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->grid, 1), 1);

    result = cbx_player_mode_handle(&f->pm, 1, CBX_PM_RIGHT);
    assert_int_equal(result, CBX_PM_RESULT_MOVED);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->grid, 1), 2);

    /* Slot change callback fired for row 1, slot 1 (P2). */
    assert_int_equal(f->cb_track.slot_changes, 3);
    assert_int_equal(f->cb_track.last_row, 1);
    assert_int_equal(f->cb_track.last_slot, 1);

    /* --- Player Mode: controller 0 cycles profile down (forward) --- */
    result = cbx_player_mode_handle(&f->pm, 0, CBX_PM_DOWN);
    assert_int_equal(result, CBX_PM_RESULT_PROFILE);

    /* Profile should have changed from "default" to "fighting". */
    const char *prof = cbx_select_grid_get_profile(&f->grid, 0);
    assert_non_null(prof);
    assert_string_equal(prof, "fighting");

    /* Profile change callback fired. */
    assert_int_equal(f->cb_track.profile_changes, 1);
    assert_int_equal(f->cb_track.last_row, 0);
    assert_string_equal(f->cb_track.last_profile, "fighting");

    /* --- Player Mode: B closes overlay --- */
    result = cbx_player_mode_handle(&f->pm, 0, CBX_PM_B);
    assert_int_equal(result, CBX_PM_RESULT_CLOSE);

    /* Wire close: expect InterceptMode=PASS on close. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", NULL);

    rc = cbx_overlay_request_close(&f->lc, &f->grid, &f->assignments);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_overlay_lifecycle_get_state(&f->lc),
                     CBX_OVERLAY_IDLE);

    /* Assignments saved: controller 0 at slot 0 (P1), controller 1 at slot 1 (P2). */
    assert_int_equal(f->assignments.assignment_count, 2);

    /* Find and verify. */
    bool found0 = false, found1 = false;
    for (int i = 0; i < f->assignments.assignment_count; i++) {
        if (strcmp(f->assignments.assignments[i].id, "ORDER:0") == 0) {
            found0 = true;
            assert_int_equal(f->assignments.assignments[i].slot, 0);
            assert_string_equal(f->assignments.assignments[i].profile, "fighting");
        }
        if (strcmp(f->assignments.assignments[i].id, "ORDER:1") == 0) {
            found1 = true;
            assert_int_equal(f->assignments.assignments[i].slot, 1);
        }
    }
    assert_true(found0);
    assert_true(found1);
}

/* ======================================================================== */
/*  Test 4: Profile change applies to InputPlumber via LoadProfilePath.     */
/* ======================================================================== */

static void
test_profile_change_applied(void **state)
{
    integ_fixture *f = *state;

    /* Move controller 0 to P1. */
    move_to_col(&f->grid, 0, 1);

    /* Expect LoadProfilePath call on the composite. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "LoadProfilePath", NULL);
    /* Expect ProfilePath read-back for engine state verification. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "ProfilePath",
                           "/usr/share/inputplumber/profiles/fighting.yaml");

    /* Apply profile change: "fighting" for controller 0. */
    int rc = cbx_profile_cycle_apply(&f->pc, &f->grid, 0, "fighting",
                                      COMP_PATH_0);
    assert_int_equal(rc, 0);

    /* Assignment should be updated with new profile. */
    bool found = false;
    for (int i = 0; i < f->assignments.assignment_count; i++) {
        if (strcmp(f->assignments.assignments[i].id, "ORDER:0") == 0) {
            found = true;
            assert_string_equal(f->assignments.assignments[i].profile,
                                 "fighting");
        }
    }
    assert_true(found);
}

/* ======================================================================== */
/*  Test 4b: LoadProfilePath failure does not update assignment.            */
/* ======================================================================== */

static void
test_profile_change_load_failure(void **state)
{
    integ_fixture *f = *state;

    /* Move controller 0 to P1. */
    move_to_col(&f->grid, 0, 1);

    /* LoadProfilePath returns error — engine state not updated. */
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                              "LoadProfilePath", IP_ERR_NO_REPLY);

    /* Apply should fail. */
    int rc = cbx_profile_cycle_apply(&f->pc, &f->grid, 0, "fighting",
                                      COMP_PATH_0);
    assert_true(rc < 0);

    /* Assignment must NOT be updated — no entry with profile "fighting". */
    for (int i = 0; i < f->assignments.assignment_count; i++) {
        assert_string_not_equal(f->assignments.assignments[i].profile,
                                 "fighting");
    }
}

/* ======================================================================== */
/*  Test 4c: ProfilePath verification mismatch fails apply.                 */
/* ======================================================================== */

static void
test_profile_change_verify_mismatch(void **state)
{
    integ_fixture *f = *state;

    /* Move controller 0 to P1. */
    move_to_col(&f->grid, 0, 1);

    /* LoadProfilePath succeeds, but ProfilePath read-back returns a
     * different path — engine did not load the requested profile. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "LoadProfilePath", NULL);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "ProfilePath",
                           "/some/other/profile.yaml");

    int rc = cbx_profile_cycle_apply(&f->pc, &f->grid, 0, "fighting",
                                      COMP_PATH_0);
    assert_true(rc < 0);

    /* Assignment must NOT be updated. */
    for (int i = 0; i < f->assignments.assignment_count; i++) {
        assert_string_not_equal(f->assignments.assignments[i].profile,
                                 "fighting");
    }
}

/* ======================================================================== */
/*  Test 5: Profile follows controller across column moves.                 */
/* ======================================================================== */

static void
test_profile_follows_controller(void **state)
{
    integ_fixture *f = *state;

    /* Move controller 0 to P2 (col 2). */
    move_to_col(&f->grid, 0, 2);

    /* Cycle profile down (forward) to "fighting". */
    cbx_select_grid_cycle_profile_down(&f->grid, 0);
    assert_string_equal(cbx_select_grid_get_profile(&f->grid, 0), "fighting");

    /* Move left to P1 (col 1) — profile should stay "fighting". */
    cbx_select_grid_move_left(&f->grid, 0);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->grid, 0), 1);
    assert_string_equal(cbx_select_grid_get_profile(&f->grid, 0), "fighting");

    /* Move right back to P2 — profile still "fighting". */
    cbx_select_grid_move_right(&f->grid, 0);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->grid, 0), 2);
    assert_string_equal(cbx_select_grid_get_profile(&f->grid, 0), "fighting");

    /* Verify via API. */
    assert_true(cbx_profile_cycle_profile_follows(&f->grid, 0));
}

/* ======================================================================== */
/*  Test 6: Conflict detection — two controllers on same P-slot.            */
/* ======================================================================== */

static void
test_conflict_detection_and_resolution(void **state)
{
    integ_fixture *f = *state;

    /* Put controllers 0 and 1 both on P1 (col 1). */
    move_to_col(&f->grid, 0, 1);
    move_to_col(&f->grid, 1, 1);

    /* Detect conflict. */
    cbx_conflict_list clist;
    cbx_conflict_list_init(&clist);
    int rc = cbx_conflict_detect(&f->grid, &clist);
    assert_int_equal(rc, 0);

    /* Row 1 should be conflicted (second arrival). */
    assert_true(cbx_conflict_is_row_conflicted(&clist, 1));
    assert_false(cbx_conflict_is_row_conflicted(&clist, 0));

    /* Resolve: controller 1 moves to P2 (slot 1, lowest free). */
    rc = cbx_conflict_resolve(&f->grid, &clist);
    assert_int_equal(rc, 1);  /* 1 conflict resolved */

    /* Controller 0 stays at P1, controller 1 moved to P2. */
    assert_int_equal(cbx_select_grid_get_cur_col(&f->grid, 0), 1);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->grid, 1), 2);

    /* No more conflicts. */
    cbx_conflict_list_init(&clist);
    cbx_conflict_detect(&f->grid, &clist);
    assert_false(cbx_conflict_is_row_conflicted(&clist, 0));
    assert_false(cbx_conflict_is_row_conflicted(&clist, 1));
}

/* ======================================================================== */
/*  Test 7: Host Mode — R3 enters, host navigates rows, edits slot, exits.  */
/* ======================================================================== */

static void
test_host_mode_integration(void **state)
{
    integ_fixture *f = *state;

    /* Activate overlay. */
    cbx_overlay_lifecycle_activate(&f->lc);
    assert_int_equal(cbx_overlay_lifecycle_get_state(&f->lc),
                     CBX_OVERLAY_VISIBLE);

    /* Controller 0 and 1 at initial positions (both Unassigned). */
    /* Move controller 0 to P1. */
    cbx_player_mode_handle(&f->pm, 0, CBX_PM_RIGHT);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->grid, 0), 1);

    /* Controller 1 presses R3 → enters Host Mode as host. */
    int result = cbx_player_mode_handle(&f->pm, 1, CBX_PM_R3);
    assert_int_equal(result, CBX_PM_RESULT_HOST);

    int rc = cbx_host_mode_toggle(&f->hm, 1);
    assert_int_equal(rc, 1);  /* entered host mode */
    assert_true(cbx_host_mode_is_active(&f->hm));
    assert_int_equal(cbx_host_mode_get_host_row(&f->hm), 1);
    assert_int_equal(cbx_host_mode_get_selected_row(&f->hm), 1);

    /* Controller 0 is frozen. */
    assert_true(cbx_host_mode_is_frozen(&f->hm, 0));
    /* Controller 1 (host) is not frozen. */
    assert_false(cbx_host_mode_is_frozen(&f->hm, 1));

    /* Visual states. */
    assert_int_equal(cbx_host_mode_row_state(&f->hm, 0), CBX_ROW_FROZEN);
    assert_int_equal(cbx_host_mode_row_state(&f->hm, 1), CBX_ROW_SELECTED);
    assert_int_equal(cbx_host_mode_row_state(&f->hm, 2), CBX_ROW_FROZEN);

    /* Host navigates up to row 0. */
    result = cbx_host_mode_handle(&f->hm, 1, CBX_HM_UP, &f->grid);
    assert_int_equal(result, CBX_HM_RESULT_MOVED);
    assert_int_equal(cbx_host_mode_get_selected_row(&f->hm), 0);

    /* Host edits selected row 0's slot: move right. */
    /* Row 0 is already at col 1 (P1).  Move right → col 2 (P2). */
    int prev_slot_changes = f->cb_track.slot_changes;
    result = cbx_host_mode_handle(&f->hm, 1, CBX_HM_RIGHT, &f->grid);
    assert_int_equal(result, CBX_HM_RESULT_SLOT);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->grid, 0), 2);

    /* Slot change callback fired for row 0 (selected row), slot 1 (P2). */
    assert_int_equal(f->cb_track.slot_changes, prev_slot_changes + 1);
    assert_int_equal(f->cb_track.last_row, 0);
    assert_int_equal(f->cb_track.last_slot, 1);

    /* Host exits (R3). */
    result = cbx_host_mode_handle(&f->hm, 1, CBX_HM_R3, &f->grid);
    assert_int_equal(result, CBX_HM_RESULT_EXIT);
    assert_false(cbx_host_mode_is_active(&f->hm));

    /* All rows back to normal. */
    assert_int_equal(cbx_host_mode_row_state(&f->hm, 0), CBX_ROW_NORMAL);
    assert_int_equal(cbx_host_mode_row_state(&f->hm, 1), CBX_ROW_NORMAL);
    assert_int_equal(cbx_host_mode_row_state(&f->hm, 2), CBX_ROW_NORMAL);

    /* Close overlay. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", NULL);
    rc = cbx_overlay_request_close(&f->lc, &f->grid, &f->assignments);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_overlay_lifecycle_get_state(&f->lc),
                     CBX_OVERLAY_IDLE);
}

/* ======================================================================== */
/*  Test 8: Host Mode — B closes overlay.                                   */
/* ======================================================================== */

static void
test_host_mode_close(void **state)
{
    integ_fixture *f = *state;

    cbx_overlay_lifecycle_activate(&f->lc);

    /* Enter host mode. */
    cbx_host_mode_toggle(&f->hm, 0);
    assert_true(cbx_host_mode_is_active(&f->hm));

    /* Host presses B → close. */
    int result = cbx_host_mode_handle(&f->hm, 0, CBX_HM_B, &f->grid);
    assert_int_equal(result, CBX_HM_RESULT_CLOSE);

    /* Close overlay. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", NULL);
    int rc = cbx_overlay_request_close(&f->lc, &f->grid, &f->assignments);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_overlay_lifecycle_get_state(&f->lc),
                     CBX_OVERLAY_IDLE);
}

/* ======================================================================== */
/*  Test 9: Full workflow — trigger → activate → navigate → conflict →      */
/*          host mode → resolve → close → verify assignments saved.         */
/* ======================================================================== */

static void
test_full_workflow(void **state)
{
    integ_fixture *f = *state;

    /* --- Step 1: Register trigger --- */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "SetInterceptActivation", NULL);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", NULL);
    int rc = cbx_trigger_register(f->backend, f->mock.bus, COMP_PATH_0,
                                   "Select+A");
    assert_int_equal(rc, 0);

    /* --- Step 2: Activate overlay --- */
    rc = cbx_overlay_lifecycle_activate(&f->lc);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_overlay_lifecycle_get_state(&f->lc),
                     CBX_OVERLAY_VISIBLE);

    /* --- Step 3: Navigate in Player Mode --- */
    /* Controller 0 → P1 (col 1). */
    cbx_player_mode_handle(&f->pm, 0, CBX_PM_RIGHT);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->grid, 0), 1);

    /* Controller 1 → P1 (col 1) — conflict! */
    cbx_player_mode_handle(&f->pm, 1, CBX_PM_RIGHT);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->grid, 1), 1);

    /* Controller 2 → P3 (col 3). */
    cbx_player_mode_handle(&f->pm, 2, CBX_PM_RIGHT);
    cbx_player_mode_handle(&f->pm, 2, CBX_PM_RIGHT);
    cbx_player_mode_handle(&f->pm, 2, CBX_PM_RIGHT);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->grid, 2), 3);

    /* --- Step 4: Cycle profile for controller 0 (down = forward) --- */
    cbx_player_mode_handle(&f->pm, 0, CBX_PM_DOWN);
    assert_string_equal(cbx_select_grid_get_profile(&f->grid, 0), "fighting");

    /* --- Step 5: Verify conflict exists --- */
    cbx_conflict_list clist;
    cbx_conflict_list_init(&clist);
    cbx_conflict_detect(&f->grid, &clist);
    assert_true(cbx_conflict_is_row_conflicted(&clist, 1));

    /* --- Step 6: Enter Host Mode, resolve conflict, exit --- */
    cbx_host_mode_toggle(&f->hm, 0);
    assert_true(cbx_host_mode_is_active(&f->hm));

    /* Host (row 0) navigates to row 1 (the conflicted controller). */
    cbx_host_mode_handle(&f->hm, 0, CBX_HM_DOWN, &f->grid);
    assert_int_equal(cbx_host_mode_get_selected_row(&f->hm), 1);

    /* Move row 1 right to P2 (col 2) — resolves the conflict. */
    cbx_host_mode_handle(&f->hm, 0, CBX_HM_RIGHT, &f->grid);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->grid, 1), 2);

    /* Exit host mode. */
    cbx_host_mode_handle(&f->hm, 0, CBX_HM_R3, &f->grid);
    assert_false(cbx_host_mode_is_active(&f->hm));

    /* --- Step 7: Verify conflict resolved --- */
    cbx_conflict_list_init(&clist);
    cbx_conflict_detect(&f->grid, &clist);
    assert_false(cbx_conflict_is_row_conflicted(&clist, 0));
    assert_false(cbx_conflict_is_row_conflicted(&clist, 1));
    assert_false(cbx_conflict_is_row_conflicted(&clist, 2));

    /* Grid state: ctrl 0 @ P1, ctrl 1 @ P2, ctrl 2 @ P3. */
    assert_int_equal(cbx_select_grid_get_cur_col(&f->grid, 0), 1);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->grid, 1), 2);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->grid, 2), 3);

    /* --- Step 8: Close overlay --- */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", NULL);
    rc = cbx_overlay_request_close(&f->lc, &f->grid, &f->assignments);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_overlay_lifecycle_get_state(&f->lc),
                     CBX_OVERLAY_IDLE);

    /* --- Step 9: Verify assignments saved correctly --- */
    assert_int_equal(f->assignments.assignment_count, 3);

    bool found0 = false, found1 = false, found2 = false;
    for (int i = 0; i < f->assignments.assignment_count; i++) {
        if (strcmp(f->assignments.assignments[i].id, "ORDER:0") == 0) {
            found0 = true;
            assert_int_equal(f->assignments.assignments[i].slot, 0);
            assert_string_equal(f->assignments.assignments[i].profile, "fighting");
        }
        if (strcmp(f->assignments.assignments[i].id, "ORDER:1") == 0) {
            found1 = true;
            assert_int_equal(f->assignments.assignments[i].slot, 1);
        }
        if (strcmp(f->assignments.assignments[i].id, "ORDER:2") == 0) {
            found2 = true;
            assert_int_equal(f->assignments.assignments[i].slot, 2);
        }
    }
    assert_true(found0);
    assert_true(found1);
    assert_true(found2);
}

/* ======================================================================== */
/*  Test 10: Close with conflict auto-resolution (no host mode needed).     */
/* ======================================================================== */

static void
test_close_auto_resolves_conflict(void **state)
{
    integ_fixture *f = *state;

    /* Activate. */
    cbx_overlay_lifecycle_activate(&f->lc);

    /* Put controllers 0, 1, 2 all on P1 (col 1) — triple conflict. */
    move_to_col(&f->grid, 0, 1);
    move_to_col(&f->grid, 1, 1);
    move_to_col(&f->grid, 2, 1);

    /* Close — on_save should detect and auto-resolve conflicts. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", NULL);

    int rc = cbx_overlay_request_close(&f->lc, &f->grid, &f->assignments);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_overlay_lifecycle_get_state(&f->lc),
                     CBX_OVERLAY_IDLE);

    /* Conflicts auto-resolved: ctrl 0 @ P1, ctrl 1 @ P2, ctrl 2 @ P3. */
    assert_int_equal(cbx_select_grid_get_cur_col(&f->grid, 0), 1);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->grid, 1), 2);
    assert_int_equal(cbx_select_grid_get_cur_col(&f->grid, 2), 3);

    /* All 3 assignments saved. */
    assert_int_equal(f->assignments.assignment_count, 3);
}

/* ======================================================================== */
/*  Test 11: Grid rendering with SDL2 dummy driver.                        */
/* ======================================================================== */

static void
test_grid_renders_with_dummy_driver(void **state)
{
    integ_fixture *f = *state;

    /* The grid should render without crashing even with minimal context. */
    cbx_grid_render_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.grid = &f->grid;
    /* icon_cache, icon_map, theme, text_cache all NULL — basic rendering. */

    SDL_Rect clip = { 0, 0, 320, 240 };
    int rc = cbx_select_grid_render(f->sdl.renderer, &clip, &ctx);
    assert_int_equal(rc, 0);

    /* Also test via render callback wrapper. */
    rc = cbx_select_grid_render_cb(f->sdl.renderer, &clip, &ctx);
    assert_int_equal(rc, 0);
}

/* ======================================================================== */
/*  Test 12: Lifecycle force close from VISIBLE.                            */
/* ======================================================================== */

static void
test_force_close(void **state)
{
    integ_fixture *f = *state;

    cbx_overlay_lifecycle_activate(&f->lc);
    assert_int_equal(cbx_overlay_lifecycle_get_state(&f->lc),
                     CBX_OVERLAY_VISIBLE);

    /* Force close skips on_save and goes straight to IDLE. */
    cbx_overlay_lifecycle_force_close(&f->lc);
    assert_int_equal(cbx_overlay_lifecycle_get_state(&f->lc),
                     CBX_OVERLAY_IDLE);
}

/* ======================================================================== */
/*  Test 13: Overlay tick in VISIBLE state.                                */
/* ======================================================================== */

static void
test_tick_in_visible(void **state)
{
    integ_fixture *f = *state;

    cbx_overlay_lifecycle_activate(&f->lc);
    assert_int_equal(cbx_overlay_lifecycle_get_state(&f->lc),
                     CBX_OVERLAY_VISIBLE);

    /* Tick should succeed and stay in VISIBLE. */
    int rc = cbx_overlay_lifecycle_tick(&f->lc);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_overlay_lifecycle_get_state(&f->lc),
                     CBX_OVERLAY_VISIBLE);
}

/* ======================================================================== */
/*  Main                                                                   */
/* ======================================================================== */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* Trigger registration (no fixture needed — standalone mock). */
        cmocka_unit_test(test_trigger_registration),
        cmocka_unit_test(test_trigger_registration_all),

        /* Full lifecycle + player mode + close (needs fixture + temp HOME). */
        cmocka_unit_test_setup_teardown(test_full_lifecycle_player_mode,
                                         setup_integ, teardown_integ),

        /* Profile change applied via LoadProfilePath (needs fixture). */
        cmocka_unit_test_setup_teardown(test_profile_change_applied,
                                         setup_integ, teardown_integ),

        /* LoadProfilePath failure does not update assignment. */
        cmocka_unit_test_setup_teardown(test_profile_change_load_failure,
                                         setup_integ, teardown_integ),

        /* ProfilePath verification mismatch fails apply. */
        cmocka_unit_test_setup_teardown(test_profile_change_verify_mismatch,
                                         setup_integ, teardown_integ),

        /* Profile follows controller across column moves. */
        cmocka_unit_test_setup_teardown(test_profile_follows_controller,
                                         setup_integ, teardown_integ),

        /* Conflict detection and resolution. */
        cmocka_unit_test_setup_teardown(test_conflict_detection_and_resolution,
                                         setup_integ, teardown_integ),

        /* Host Mode integration: enter, navigate, edit, exit. */
        cmocka_unit_test_setup_teardown(test_host_mode_integration,
                                         setup_integ, teardown_integ),

        /* Host Mode close via B button. */
        cmocka_unit_test_setup_teardown(test_host_mode_close,
                                         setup_integ, teardown_integ),

        /* Full workflow: trigger → activate → navigate → conflict →
         * host mode → resolve → close → verify. */
        cmocka_unit_test_setup_teardown(test_full_workflow,
                                         setup_integ, teardown_integ),

        /* Close auto-resolves conflicts (no host mode needed). */
        cmocka_unit_test_setup_teardown(test_close_auto_resolves_conflict,
                                         setup_integ, teardown_integ),

        /* Grid rendering with SDL2 dummy driver. */
        cmocka_unit_test_setup_teardown(test_grid_renders_with_dummy_driver,
                                         setup_integ, teardown_integ),

        /* Force close from VISIBLE. */
        cmocka_unit_test_setup_teardown(test_force_close,
                                         setup_integ, teardown_integ),

        /* Tick in VISIBLE state. */
        cmocka_unit_test_setup_teardown(test_tick_in_visible,
                                         setup_integ, teardown_integ),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}