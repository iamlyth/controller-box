/*
 * test_close.c — Unit tests for overlay close coordination.
 *
 * Task 32 — Overlay trigger registration and activation/close.
 *
 * Tests:
 *   - sync_assignments: update existing, create new, remove unassigned,
 *     preserve disconnected, empty grid, no-id rows, NULL safety
 *   - on_save: no conflicts, with conflicts (resolved), NULL safety
 *   - request_close: from VISIBLE (saves+sets PASS+IDLE), from IDLE (-EPERM),
 *     NULL args, full lifecycle (activate → close → IDLE)
 *
 * Uses a temp HOME directory for assignments.yaml save.
 */
#include "overlay/close.h"
#include "overlay/conflict.h"
#include "overlay/grid_render.h"
#include "overlay/lifecycle.h"
#include "identify/assign.h"
#include "config/config_assignments.h"
#include "config/config_settings.h"
#include "dbus_mock.h"
#include "dbus/ip_composite.h"
#include "dbus/ip_connection.h"   /* IP_ERR_NO_REPLY */
#include "dbus/ip_device_model.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cmocka.h>

/* --- Test constants ---------------------------------------------------- */

#define COMP_PATH "/org/shadowblip/InputPlumber/CompositeDevice0"

/* --- Temp HOME setup -------------------------------------------------- */

static char test_home[PATH_MAX];

static int
setup_home(void **state)
{
    (void)state;
    snprintf(test_home, sizeof(test_home), "/tmp/cbx-close-XXXXXX");
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

/* --- Grid helpers ----------------------------------------------------- */

static cbx_grid_composite_info
make_comp(const char *id, const char *name, const char *path)
{
    cbx_grid_composite_info c;
    memset(&c, 0, sizeof(c));
    if (id) strncpy(c.id, id, CBX_MAX_ID_LEN - 1);
    if (name) strncpy(c.model_name, name, CBX_MAX_NAME_LEN - 1);
    if (path) strncpy(c.composite_path, path, CBX_MAX_PATH_LEN - 1);
    return c;
}

static void
build_test_grid(cbx_select_grid *g, int rows)
{
    cbx_grid_composite_info comps[CBX_GRID_MAX_ROWS];
    for (int i = 0; i < rows; i++) {
        char id[32], name[32], path[64];
        snprintf(id, sizeof(id), "ORDER:%d", i);
        snprintf(name, sizeof(name), "Ctrl %d", i);
        snprintf(path, sizeof(path),
                 "/org/shadowblip/InputPlumber/CompositeDevice%d", i);
        comps[i] = make_comp(id, name, path);
    }

    cbx_settings s;
    memset(&s, 0, sizeof(s));
    s.virtual_controllers.count = 4;
    for (int i = 0; i < 4; i++)
        strncpy(s.virtual_controllers.types[i], "xb360",
                CBX_MAX_TYPE_LEN - 1);

    cbx_assignments a;
    cbx_assignments_init(&a);

    cbx_select_grid_build(g, comps, rows, &s, &a);
}

/* Move a row to a specific column by repeated moves. */
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

/* --- Sync assignments tests ------------------------------------------- */

static void
test_sync_update_existing(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 2);

    /* Move controller 0 to P2 (col 2, slot 1). */
    move_to_col(&g, 0, 2);
    /* Change profile. */
    strncpy(g.rows[0].profile, "fighting", CBX_GRID_PROFILE_LEN - 1);

    /* Pre-populate assignments: controller 0 at slot 0, profile "default". */
    cbx_assignments a;
    cbx_assignments_init(&a);
    cbx_assignment *entry = &a.assignments[0];
    strncpy(entry->id, "ORDER:0", CBX_MAX_ID_LEN - 1);
    entry->slot = 0;
    strncpy(entry->profile, "default", CBX_MAX_PROFILE_LEN - 1);
    a.assignment_count = 1;

    int rc = cbx_close_sync_assignments(&g, &a);
    assert_int_equal(rc, 0);
    assert_int_equal(a.assignment_count, 1);
    assert_string_equal(a.assignments[0].id, "ORDER:0");
    assert_int_equal(a.assignments[0].slot, 1);   /* updated to slot 1 */
    assert_string_equal(a.assignments[0].profile, "fighting");
}

static void
test_sync_create_new(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 2);

    /* Move controller 1 to P1 (col 1, slot 0). */
    move_to_col(&g, 1, 1);

    /* Empty assignments. */
    cbx_assignments a;
    cbx_assignments_init(&a);

    int rc = cbx_close_sync_assignments(&g, &a);
    assert_int_equal(rc, 0);
    /* Controller 0 was Unassigned (col 0) — no assignment created. */
    /* Controller 1 was at col 1 (slot 0) — assignment created. */
    assert_int_equal(a.assignment_count, 1);
    assert_string_equal(a.assignments[0].id, "ORDER:1");
    assert_int_equal(a.assignments[0].slot, 0);
}

static void
test_sync_remove_unassigned(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 2);

    /* Controller 0 stays at Unassigned (col 0). */

    /* Pre-populate: controller 0 was at slot 0. */
    cbx_assignments a;
    cbx_assignments_init(&a);
    cbx_assignment *entry = &a.assignments[0];
    strncpy(entry->id, "ORDER:0", CBX_MAX_ID_LEN - 1);
    entry->slot = 0;
    strncpy(entry->profile, "default", CBX_MAX_PROFILE_LEN - 1);
    a.assignment_count = 1;

    int rc = cbx_close_sync_assignments(&g, &a);
    assert_int_equal(rc, 0);
    /* Controller 0 was moved to Unassigned → assignment removed. */
    assert_int_equal(a.assignment_count, 0);
}

static void
test_sync_preserve_disconnected(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 1);

    /* Move controller 0 to P1 (col 1, slot 0). */
    move_to_col(&g, 0, 1);

    /* Pre-populate: controller "BT:AB:CD:01:EF:23" at slot 2 (disconnected). */
    cbx_assignments a;
    cbx_assignments_init(&a);
    cbx_assignment *entry = &a.assignments[0];
    strncpy(entry->id, "BT:AB:CD:01:EF:23", CBX_MAX_ID_LEN - 1);
    entry->slot = 2;
    strncpy(entry->profile, "fighting", CBX_MAX_PROFILE_LEN - 1);
    a.assignment_count = 1;

    int rc = cbx_close_sync_assignments(&g, &a);
    assert_int_equal(rc, 0);
    /* Disconnected controller's assignment preserved. */
    /* New controller 0 at slot 0 created. */
    assert_int_equal(a.assignment_count, 2);
    /* Find the disconnected one. */
    bool found_disconnected = false;
    bool found_new = false;
    for (int i = 0; i < a.assignment_count; i++) {
        if (strcmp(a.assignments[i].id, "BT:AB:CD:01:EF:23") == 0) {
            found_disconnected = true;
            assert_int_equal(a.assignments[i].slot, 2);
            assert_string_equal(a.assignments[i].profile, "fighting");
        }
        if (strcmp(a.assignments[i].id, "ORDER:0") == 0) {
            found_new = true;
            assert_int_equal(a.assignments[i].slot, 0);
        }
    }
    assert_true(found_disconnected);
    assert_true(found_new);
}

static void
test_sync_empty_grid(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_select_grid_init(&g);

    cbx_assignments a;
    cbx_assignments_init(&a);
    cbx_assignment *entry = &a.assignments[0];
    strncpy(entry->id, "ORDER:0", CBX_MAX_ID_LEN - 1);
    entry->slot = 0;
    a.assignment_count = 1;

    int rc = cbx_close_sync_assignments(&g, &a);
    assert_int_equal(rc, 0);
    /* No grid rows → no changes to assignments. */
    assert_int_equal(a.assignment_count, 1);
}

static void
test_sync_no_id_row(void **state)
{
    (void)state;
    /* Build a grid with a composite that has no ID. */
    cbx_grid_composite_info comps[1];
    memset(comps, 0, sizeof(comps));
    strncpy(comps[0].model_name, "Unknown", CBX_MAX_NAME_LEN - 1);
    /* id left empty. */

    cbx_settings s;
    memset(&s, 0, sizeof(s));
    s.virtual_controllers.count = 2;
    for (int i = 0; i < 2; i++)
        strncpy(s.virtual_controllers.types[i], "xb360",
                CBX_MAX_TYPE_LEN - 1);

    cbx_assignments a;
    cbx_assignments_init(&a);

    cbx_select_grid g;
    cbx_select_grid_build(&g, comps, 1, &s, &a);

    /* Move to col 1. */
    move_to_col(&g, 0, 1);

    int rc = cbx_close_sync_assignments(&g, &a);
    assert_int_equal(rc, 0);
    /* Row with no ID → skipped → no assignments created. */
    assert_int_equal(a.assignment_count, 0);
}

static void
test_sync_null_args(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_assignments a;
    assert_int_equal(cbx_close_sync_assignments(NULL, &a), -EINVAL);
    assert_int_equal(cbx_close_sync_assignments(&g, NULL), -EINVAL);
}

static void
test_sync_multiple(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);

    /* Controller 0 → P1 (col 1, slot 0). */
    move_to_col(&g, 0, 1);
    /* Controller 1 → P2 (col 2, slot 1). */
    move_to_col(&g, 1, 2);
    /* Controller 2 stays at Unassigned (col 0). */

    /* Pre-populate: controller 2 was at slot 2. */
    cbx_assignments a;
    cbx_assignments_init(&a);
    strncpy(a.assignments[0].id, "ORDER:2", CBX_MAX_ID_LEN - 1);
    a.assignments[0].slot = 2;
    strncpy(a.assignments[0].profile, "default", CBX_MAX_PROFILE_LEN - 1);
    a.assignment_count = 1;

    int rc = cbx_close_sync_assignments(&g, &a);
    assert_int_equal(rc, 0);
    /* Controller 0: created at slot 0. */
    /* Controller 1: created at slot 1. */
    /* Controller 2: removed (was at slot 2, now Unassigned). */
    assert_int_equal(a.assignment_count, 2);

    /* Find each. */
    bool found0 = false, found1 = false;
    for (int i = 0; i < a.assignment_count; i++) {
        if (strcmp(a.assignments[i].id, "ORDER:0") == 0) {
            found0 = true;
            assert_int_equal(a.assignments[i].slot, 0);
        }
        if (strcmp(a.assignments[i].id, "ORDER:1") == 0) {
            found1 = true;
            assert_int_equal(a.assignments[i].slot, 1);
        }
    }
    assert_true(found0);
    assert_true(found1);
}

static void
test_sync_update_profile(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 1);

    /* Move controller to P1 (col 1, slot 0) so the assignment is updated. */
    move_to_col(&g, 0, 1);

    /* Change profile via cycle. */
    cbx_select_grid_add_profile(&g, "fighting");
    cbx_select_grid_cycle_profile_up(&g, 0);

    /* Pre-populate: controller 0 at slot 0, profile "default". */
    cbx_assignments a;
    cbx_assignments_init(&a);
    strncpy(a.assignments[0].id, "ORDER:0", CBX_MAX_ID_LEN - 1);
    a.assignments[0].slot = 0;
    strncpy(a.assignments[0].profile, "default", CBX_MAX_PROFILE_LEN - 1);
    a.assignment_count = 1;

    int rc = cbx_close_sync_assignments(&g, &a);
    assert_int_equal(rc, 0);
    assert_string_equal(a.assignments[0].profile, "fighting");
}

/* --- on_save tests ---------------------------------------------------- */

static void
test_on_save_no_conflicts(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 2);
    move_to_col(&g, 0, 1);  /* P1 */
    move_to_col(&g, 1, 2);  /* P2 */

    cbx_assignments a;
    cbx_assignments_init(&a);

    cbx_close_ctx ctx = { .grid = &g, .assignments = &a };
    int rc = cbx_close_on_save(&ctx);
    assert_int_equal(rc, 0);
    /* Two assignments should be saved. */
    assert_int_equal(a.assignment_count, 2);
}

static void
test_on_save_with_conflicts(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    /* Put controllers 0 and 1 both on P1 (col 1) — conflict. */
    move_to_col(&g, 0, 1);
    move_to_col(&g, 1, 1);

    cbx_assignments a;
    cbx_assignments_init(&a);

    cbx_close_ctx ctx = { .grid = &g, .assignments = &a };
    int rc = cbx_close_on_save(&ctx);
    assert_int_equal(rc, 0);

    /* Conflict resolved: controller 0 stays on P1 (slot 0), controller 1
     * moved to P2 (slot 1).  Both assignments saved. */
    assert_int_equal(a.assignment_count, 2);

    /* Find and verify slots. */
    bool found0 = false, found1 = false;
    for (int i = 0; i < a.assignment_count; i++) {
        if (strcmp(a.assignments[i].id, "ORDER:0") == 0) {
            found0 = true;
            assert_int_equal(a.assignments[i].slot, 0);
        }
        if (strcmp(a.assignments[i].id, "ORDER:1") == 0) {
            found1 = true;
            assert_int_equal(a.assignments[i].slot, 1);
        }
    }
    assert_true(found0);
    assert_true(found1);

    /* Verify grid was actually resolved. */
    assert_int_equal(cbx_select_grid_get_cur_col(&g, 0), 1);
    assert_int_equal(cbx_select_grid_get_cur_col(&g, 1), 2);
}

static void
test_on_save_null_userdata(void **state)
{
    (void)state;
    assert_int_equal(cbx_close_on_save(NULL), -EINVAL);
}

static void
test_on_save_null_grid(void **state)
{
    (void)state;
    cbx_assignments a;
    cbx_close_ctx ctx = { .grid = NULL, .assignments = &a };
    assert_int_equal(cbx_close_on_save(&ctx), -EINVAL);
}

static void
test_on_save_null_assignments(void **state)
{
    (void)state;
    cbx_select_grid g;
    cbx_close_ctx ctx = { .grid = &g, .assignments = NULL };
    assert_int_equal(cbx_close_on_save(&ctx), -EINVAL);
}

static void
test_on_save_all_unassigned(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    /* All controllers stay at Unassigned (col 0). */
    cbx_assignments a;
    cbx_assignments_init(&a);

    cbx_close_ctx ctx = { .grid = &g, .assignments = &a };
    int rc = cbx_close_on_save(&ctx);
    assert_int_equal(rc, 0);
    /* No assignments saved (all unassigned). */
    assert_int_equal(a.assignment_count, 0);
}

/* --- request_close tests ---------------------------------------------- */

typedef struct {
    ip_dbus_mock          mock;
    const ip_dbus_backend *backend;
    cbx_overlay_lifecycle lc;
} close_fixture;

static int
setup_close(void **state)
{
    if (setup_home(state) != 0)
        return -1;

    close_fixture *f = malloc(sizeof(*f));
    if (!f)
        return -1;
    memset(f, 0, sizeof(*f));
    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);
    cbx_overlay_lifecycle_init(&f->lc, f->backend, f->mock.bus,
                               COMP_PATH, NULL, NULL);
    /* Instant transitions. */
    f->lc.fade_in_ms = 0;
    f->lc.fade_out_ms = 0;
    *state = f;
    return 0;
}

static int
teardown_close(void **state)
{
    close_fixture *f = *state;
    if (f) {
        ip_dbus_mock_reset(&f->mock);
        free(f);
    }
    teardown_home(state);
    return 0;
}

static void
test_request_close_from_visible(void **state)
{
    close_fixture *f = *state;
    cbx_select_grid g;
    build_test_grid(&g, 2);
    move_to_col(&g, 0, 1);
    move_to_col(&g, 1, 2);

    cbx_assignments a;
    cbx_assignments_init(&a);

    /* Activate to VISIBLE. */
    cbx_overlay_lifecycle_activate(&f->lc);
    assert_int_equal(cbx_overlay_lifecycle_get_state(&f->lc),
                     CBX_OVERLAY_VISIBLE);

    /* Expect InterceptMode=PASS set on close. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", NULL);

    int rc = cbx_overlay_request_close(&f->lc, &g, &a);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_overlay_lifecycle_get_state(&f->lc),
                     CBX_OVERLAY_IDLE);
    /* Assignments saved. */
    assert_int_equal(a.assignment_count, 2);
}

static void
test_request_close_from_idle(void **state)
{
    close_fixture *f = *state;
    cbx_select_grid g;
    build_test_grid(&g, 1);
    cbx_assignments a;
    cbx_assignments_init(&a);

    /* Lifecycle is in IDLE. */
    int rc = cbx_overlay_request_close(&f->lc, &g, &a);
    assert_int_equal(rc, -EPERM);
}

static void
test_request_close_null_args(void **state)
{
    close_fixture *f = *state;
    cbx_select_grid g;
    cbx_assignments a;
    assert_int_equal(cbx_overlay_request_close(NULL, &g, &a), -EINVAL);
    assert_int_equal(cbx_overlay_request_close(&f->lc, NULL, &a), -EINVAL);
    assert_int_equal(cbx_overlay_request_close(&f->lc, &g, NULL), -EINVAL);
}

static void
test_request_close_full_lifecycle(void **state)
{
    close_fixture *f = *state;
    cbx_select_grid g;
    build_test_grid(&g, 2);
    move_to_col(&g, 0, 1);
    move_to_col(&g, 1, 2);

    cbx_assignments a;
    cbx_assignments_init(&a);

    /* IDLE → activate → VISIBLE → close → IDLE. */
    assert_int_equal(cbx_overlay_lifecycle_get_state(&f->lc),
                     CBX_OVERLAY_IDLE);

    int rc = cbx_overlay_lifecycle_activate(&f->lc);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_overlay_lifecycle_get_state(&f->lc),
                     CBX_OVERLAY_VISIBLE);

    /* Expect InterceptMode=PASS on close. */
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", NULL);

    rc = cbx_overlay_request_close(&f->lc, &g, &a);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_overlay_lifecycle_get_state(&f->lc),
                     CBX_OVERLAY_IDLE);

    /* Verify assignments were saved. */
    assert_int_equal(a.assignment_count, 2);
}

static void
test_request_close_with_conflict(void **state)
{
    close_fixture *f = *state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    /* Controllers 0 and 1 both on P1 (col 1) → conflict. */
    move_to_col(&g, 0, 1);
    move_to_col(&g, 1, 1);

    cbx_assignments a;
    cbx_assignments_init(&a);

    cbx_overlay_lifecycle_activate(&f->lc);

    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                           "InterceptMode", NULL);

    int rc = cbx_overlay_request_close(&f->lc, &g, &a);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_overlay_lifecycle_get_state(&f->lc),
                     CBX_OVERLAY_IDLE);

    /* Conflict resolved: controller 1 moved to P2. */
    assert_int_equal(cbx_select_grid_get_cur_col(&g, 0), 1);
    assert_int_equal(cbx_select_grid_get_cur_col(&g, 1), 2);
    assert_int_equal(a.assignment_count, 2);
}

static void
test_request_close_intercept_mode_fail(void **state)
{
    close_fixture *f = *state;
    cbx_select_grid g;
    build_test_grid(&g, 1);
    move_to_col(&g, 0, 1);

    cbx_assignments a;
    cbx_assignments_init(&a);

    cbx_overlay_lifecycle_activate(&f->lc);

    /* InterceptMode set fails. */
    ip_dbus_mock_expect_error(&f->mock, IP_IFACE_COMPOSITE,
                               "InterceptMode", IP_ERR_NO_REPLY);

    /* Close still succeeds (error is tracked but doesn't block close). */
    int rc = cbx_overlay_request_close(&f->lc, &g, &a);
    /* lifecycle_close returns 0 even if set_intercept_pass fails
     * (it fires on_error but still proceeds to IDLE for instant close). */
    assert_int_equal(rc, 0);
    /* Assignment save still happened (on_save fired before PASS set). */
    assert_int_equal(a.assignment_count, 1);
}

/* --- Main ------------------------------------------------------------- */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* sync_assignments tests (no fixture needed — pure in-memory) */
        cmocka_unit_test(test_sync_update_existing),
        cmocka_unit_test(test_sync_create_new),
        cmocka_unit_test(test_sync_remove_unassigned),
        cmocka_unit_test(test_sync_preserve_disconnected),
        cmocka_unit_test(test_sync_empty_grid),
        cmocka_unit_test(test_sync_no_id_row),
        cmocka_unit_test(test_sync_null_args),
        cmocka_unit_test(test_sync_multiple),
        cmocka_unit_test(test_sync_update_profile),
        /* on_save null-safety tests (no save called — no fixture needed) */
        cmocka_unit_test(test_on_save_null_userdata),
        cmocka_unit_test(test_on_save_null_grid),
        cmocka_unit_test(test_on_save_null_assignments),
        /* on_save tests that call assignments_save (need temp HOME) */
        cmocka_unit_test_setup_teardown(test_on_save_no_conflicts,
                                          setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_on_save_with_conflicts,
                                          setup_home, teardown_home),
        cmocka_unit_test_setup_teardown(test_on_save_all_unassigned,
                                          setup_home, teardown_home),
        /* request_close tests (need close fixture + temp HOME) */
        cmocka_unit_test_setup_teardown(test_request_close_from_visible,
                                          setup_close, teardown_close),
        cmocka_unit_test_setup_teardown(test_request_close_from_idle,
                                          setup_close, teardown_close),
        cmocka_unit_test_setup_teardown(test_request_close_null_args,
                                          setup_close, teardown_close),
        cmocka_unit_test_setup_teardown(test_request_close_full_lifecycle,
                                          setup_close, teardown_close),
        cmocka_unit_test_setup_teardown(test_request_close_with_conflict,
                                          setup_close, teardown_close),
        cmocka_unit_test_setup_teardown(test_request_close_intercept_mode_fail,
                                          setup_close, teardown_close),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}