/*
 * test_host_mode.c — Unit tests for Host Mode state machine.
 *
 * Task 30 — Host Mode and conflict detection/resolution.
 *
 * Tests:
 *   - Init (defaults: inactive, host_row=-1, selected_row=-1)
 *   - Enter (active, host_row set, selected_row=host_row)
 *   - Exit (resets to inactive)
 *   - Toggle (enter, exit, frozen controller ignored)
 *   - Handle: Up/Down navigates rows (clamped, boundary)
 *   - Handle: Left/Right moves within selected row (fires slot change)
 *   - Handle: R3 exits host mode
 *   - Handle: B returns CLOSE
 *   - Handle: frozen controller (non-host) returns FROZEN
 *   - Handle: NULL safety
 *   - Visual state (NORMAL, HOST, SELECTED, FROZEN)
 *   - is_frozen
 *   - Host edits any row (not just its own)
 *   - Full lifecycle: enter → navigate → edit → exit
 */
#include "overlay/host_mode.h"
#include "overlay/grid_render.h"
#include "identify/assign.h"
#include "config/config_settings.h"

#include <errno.h>
#include <string.h>

#include <cmocka.h>

/* --- Callback tracking ------------------------------------------------ */

typedef struct {
    int count;
    int row;
    int slot;
} hm_callbacks;

static int
on_slot_change(int row_idx, int new_slot, void *userdata)
{
    hm_callbacks *cb = (hm_callbacks *)userdata;
    if (cb) {
        cb->count++;
        cb->row  = row_idx;
        cb->slot = new_slot;
    }
    return 0;
}

/* --- Helpers ---------------------------------------------------------- */

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
        strncpy(s.virtual_controllers.types[i], "xb360", CBX_MAX_TYPE_LEN - 1);

    cbx_assignments a;
    cbx_assignments_init(&a);

    cbx_select_grid_build(g, comps, rows, &s, &a);
    cbx_select_grid_add_profile(g, "default");
}

/* --- Init tests ------------------------------------------------------ */

static void
test_init(void **state)
{
    (void)state;
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    assert_false(hm.active);
    assert_int_equal(hm.host_row, -1);
    assert_int_equal(hm.selected_row, -1);
}

static void
test_init_null(void **state)
{
    (void)state;
    cbx_host_mode_init(NULL); /* should not crash */
}

/* --- Enter/Exit tests ------------------------------------------------ */

static void
test_enter(void **state)
{
    (void)state;
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    int rc = cbx_host_mode_enter(&hm, 2);
    assert_int_equal(rc, 0);
    assert_true(hm.active);
    assert_int_equal(hm.host_row, 2);
    assert_int_equal(hm.selected_row, 2);
}

static void
test_enter_null(void **state)
{
    (void)state;
    int rc = cbx_host_mode_enter(NULL, 0);
    assert_int_equal(rc, -EINVAL);
}

static void
test_enter_bad_row(void **state)
{
    (void)state;
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    int rc = cbx_host_mode_enter(&hm, -1);
    assert_int_equal(rc, -EINVAL);
    assert_false(hm.active);
}

static void
test_exit(void **state)
{
    (void)state;
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    cbx_host_mode_enter(&hm, 1);
    int rc = cbx_host_mode_exit(&hm);
    assert_int_equal(rc, 0);
    assert_false(hm.active);
    assert_int_equal(hm.host_row, -1);
    assert_int_equal(hm.selected_row, -1);
}

static void
test_exit_null(void **state)
{
    (void)state;
    int rc = cbx_host_mode_exit(NULL);
    assert_int_equal(rc, -EINVAL);
}

/* --- Toggle tests ---------------------------------------------------- */

static void
test_toggle_enter(void **state)
{
    (void)state;
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    int rc = cbx_host_mode_toggle(&hm, 1);
    assert_int_equal(rc, 1); /* entered */
    assert_true(hm.active);
    assert_int_equal(hm.host_row, 1);
}

static void
test_toggle_exit(void **state)
{
    (void)state;
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    cbx_host_mode_toggle(&hm, 0);
    int rc = cbx_host_mode_toggle(&hm, 0); /* same controller → exit */
    assert_int_equal(rc, 0); /* exited */
    assert_false(hm.active);
}

static void
test_toggle_frozen(void **state)
{
    (void)state;
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    cbx_host_mode_toggle(&hm, 0); /* controller 0 becomes host */
    int rc = cbx_host_mode_toggle(&hm, 1); /* controller 1 is frozen */
    assert_int_equal(rc, -1); /* ignored */
    assert_true(hm.active);
    assert_int_equal(hm.host_row, 0);
}

static void
test_toggle_null(void **state)
{
    (void)state;
    int rc = cbx_host_mode_toggle(NULL, 0);
    assert_int_equal(rc, -2);
}

static void
test_toggle_bad_row(void **state)
{
    (void)state;
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    int rc = cbx_host_mode_toggle(&hm, -1);
    assert_int_equal(rc, -2);
}

/* --- Handle: Up/Down (row navigation) ------------------------------- */

static void
test_handle_down(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    cbx_host_mode_enter(&hm, 0); /* host_row=0, selected=0 */

    int rc = cbx_host_mode_handle(&hm, 0, CBX_HM_DOWN, &g);
    assert_int_equal(rc, CBX_HM_RESULT_MOVED);
    assert_int_equal(hm.selected_row, 1);
}

static void
test_handle_up(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    cbx_host_mode_enter(&hm, 2); /* host_row=2, selected=2 */

    int rc = cbx_host_mode_handle(&hm, 2, CBX_HM_UP, &g);
    assert_int_equal(rc, CBX_HM_RESULT_MOVED);
    assert_int_equal(hm.selected_row, 1);
}

static void
test_handle_up_boundary(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    cbx_host_mode_enter(&hm, 0); /* selected=0, at top */

    int rc = cbx_host_mode_handle(&hm, 0, CBX_HM_UP, &g);
    assert_int_equal(rc, CBX_HM_RESULT_NONE);
    assert_int_equal(hm.selected_row, 0); /* unchanged */
}

static void
test_handle_down_boundary(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    cbx_host_mode_enter(&hm, 2); /* selected=2, at bottom */

    int rc = cbx_host_mode_handle(&hm, 2, CBX_HM_DOWN, &g);
    assert_int_equal(rc, CBX_HM_RESULT_NONE);
    assert_int_equal(hm.selected_row, 2); /* unchanged */
}

static void
test_handle_down_then_up(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 4);
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    cbx_host_mode_enter(&hm, 0);

    cbx_host_mode_handle(&hm, 0, CBX_HM_DOWN, &g);
    assert_int_equal(hm.selected_row, 1);
    cbx_host_mode_handle(&hm, 0, CBX_HM_DOWN, &g);
    assert_int_equal(hm.selected_row, 2);
    cbx_host_mode_handle(&hm, 0, CBX_HM_UP, &g);
    assert_int_equal(hm.selected_row, 1);
}

/* --- Handle: Left/Right (slot within selected row) ------------------- */

static void
test_handle_right(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    hm.on_slot_change    = on_slot_change;
    hm_callbacks cb       = {0};
    hm.slot_change_data   = &cb;
    cbx_host_mode_enter(&hm, 0);

    int rc = cbx_host_mode_handle(&hm, 0, CBX_HM_RIGHT, &g);
    assert_int_equal(rc, CBX_HM_RESULT_SLOT);
    assert_int_equal(cb.count, 1);
    assert_int_equal(cb.row, 0); /* selected_row = 0 (host's own) */
    assert_int_equal(cb.slot, 0); /* moved to col 1 = slot 0 (P1) */
}

static void
test_handle_left(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    /* Move row 0 to col 2 first (P2) */
    cbx_select_grid_move_right(&g, 0);
    cbx_select_grid_move_right(&g, 0);
    assert_int_equal(cbx_select_grid_get_cur_col(&g, 0), 2);

    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    hm.on_slot_change    = on_slot_change;
    hm_callbacks cb       = {0};
    hm.slot_change_data   = &cb;
    cbx_host_mode_enter(&hm, 0);

    int rc = cbx_host_mode_handle(&hm, 0, CBX_HM_LEFT, &g);
    assert_int_equal(rc, CBX_HM_RESULT_SLOT);
    assert_int_equal(cb.count, 1);
    assert_int_equal(cb.slot, 0); /* col 1 = slot 0 */
}

static void
test_handle_left_boundary(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 2);
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    hm.on_slot_change    = on_slot_change;
    hm_callbacks cb       = {0};
    hm.slot_change_data   = &cb;
    cbx_host_mode_enter(&hm, 0);

    /* Row 0 is on col 0 (Unassigned) — can't move left */
    int rc = cbx_host_mode_handle(&hm, 0, CBX_HM_LEFT, &g);
    assert_int_equal(rc, CBX_HM_RESULT_NONE);
    assert_int_equal(cb.count, 0);
}

static void
test_handle_right_no_callback(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 2);
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    /* No callback set */
    cbx_host_mode_enter(&hm, 0);

    int rc = cbx_host_mode_handle(&hm, 0, CBX_HM_RIGHT, &g);
    assert_int_equal(rc, CBX_HM_RESULT_SLOT);
}

/* --- Handle: host edits any row ------------------------------------- */

static void
test_host_edits_other_row(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    hm.on_slot_change    = on_slot_change;
    hm_callbacks cb       = {0};
    hm.slot_change_data   = &cb;
    cbx_host_mode_enter(&hm, 0);

    /* Navigate down to row 2 */
    cbx_host_mode_handle(&hm, 0, CBX_HM_DOWN, &g);
    cbx_host_mode_handle(&hm, 0, CBX_HM_DOWN, &g);
    assert_int_equal(hm.selected_row, 2);

    /* Move right in row 2 (not host's own row 0) */
    int rc = cbx_host_mode_handle(&hm, 0, CBX_HM_RIGHT, &g);
    assert_int_equal(rc, CBX_HM_RESULT_SLOT);
    assert_int_equal(cb.row, 2); /* callback fired for row 2 */
    assert_int_equal(cb.slot, 0);
    assert_int_equal(cbx_select_grid_get_cur_col(&g, 2), 1);
    /* Row 0 unchanged */
    assert_int_equal(cbx_select_grid_get_cur_col(&g, 0), 0);
}

/* --- Handle: R3 (exit) ---------------------------------------------- */

static void
test_handle_r3_exit(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 2);
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    cbx_host_mode_enter(&hm, 0);

    int rc = cbx_host_mode_handle(&hm, 0, CBX_HM_R3, &g);
    assert_int_equal(rc, CBX_HM_RESULT_EXIT);
    assert_false(hm.active);
}

/* --- Handle: B (close) ---------------------------------------------- */

static void
test_handle_b(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 2);
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    cbx_host_mode_enter(&hm, 0);

    int rc = cbx_host_mode_handle(&hm, 0, CBX_HM_B, &g);
    assert_int_equal(rc, CBX_HM_RESULT_CLOSE);
}

/* --- Handle: frozen controller -------------------------------------- */

static void
test_handle_frozen(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    cbx_host_mode_enter(&hm, 0); /* controller 0 is host */

    int rc = cbx_host_mode_handle(&hm, 1, CBX_HM_DOWN, &g);
    assert_int_equal(rc, CBX_HM_RESULT_FROZEN);
    /* selected_row unchanged */
    assert_int_equal(hm.selected_row, 0);
}

static void
test_handle_frozen_right(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    cbx_host_mode_enter(&hm, 0);

    int rc = cbx_host_mode_handle(&hm, 2, CBX_HM_RIGHT, &g);
    assert_int_equal(rc, CBX_HM_RESULT_FROZEN);
    /* Row 2 unchanged */
    assert_int_equal(cbx_select_grid_get_cur_col(&g, 2), 0);
}

/* --- Handle: NULL safety -------------------------------------------- */

static void
test_handle_null_hm(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 2);
    int rc = cbx_host_mode_handle(NULL, 0, CBX_HM_DOWN, &g);
    assert_int_equal(rc, CBX_HM_RESULT_ERROR);
}

static void
test_handle_null_grid(void **state)
{
    (void)state;
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    cbx_host_mode_enter(&hm, 0);
    int rc = cbx_host_mode_handle(&hm, 0, CBX_HM_DOWN, NULL);
    assert_int_equal(rc, CBX_HM_RESULT_ERROR);
}

static void
test_handle_not_active(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 2);
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    /* Not active */
    int rc = cbx_host_mode_handle(&hm, 0, CBX_HM_DOWN, &g);
    assert_int_equal(rc, CBX_HM_RESULT_ERROR);
}

/* --- Accessors ------------------------------------------------------- */

static void
test_is_active(void **state)
{
    (void)state;
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    assert_false(cbx_host_mode_is_active(&hm));
    cbx_host_mode_enter(&hm, 0);
    assert_true(cbx_host_mode_is_active(&hm));
}

static void
test_get_host_row(void **state)
{
    (void)state;
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    assert_int_equal(cbx_host_mode_get_host_row(&hm), -1);
    cbx_host_mode_enter(&hm, 3);
    assert_int_equal(cbx_host_mode_get_host_row(&hm), 3);
}

static void
test_get_selected_row(void **state)
{
    (void)state;
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    assert_int_equal(cbx_host_mode_get_selected_row(&hm), -1);
    cbx_host_mode_enter(&hm, 1);
    assert_int_equal(cbx_host_mode_get_selected_row(&hm), 1);
}

static void
test_accessors_null(void **state)
{
    (void)state;
    assert_false(cbx_host_mode_is_active(NULL));
    assert_int_equal(cbx_host_mode_get_host_row(NULL), -1);
    assert_int_equal(cbx_host_mode_get_selected_row(NULL), -1);
}

/* --- Visual state ---------------------------------------------------- */

static void
test_row_state_inactive(void **state)
{
    (void)state;
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    /* Not active → all rows are NORMAL */
    assert_int_equal(cbx_host_mode_row_state(&hm, 0), CBX_ROW_NORMAL);
    assert_int_equal(cbx_host_mode_row_state(&hm, 1), CBX_ROW_NORMAL);
}

static void
test_row_state_active(void **state)
{
    (void)state;
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    cbx_host_mode_enter(&hm, 0); /* host=0, selected=0 */

    assert_int_equal(cbx_host_mode_row_state(&hm, 0), CBX_ROW_SELECTED);
    assert_int_equal(cbx_host_mode_row_state(&hm, 1), CBX_ROW_FROZEN);
    assert_int_equal(cbx_host_mode_row_state(&hm, 2), CBX_ROW_FROZEN);
}

static void
test_row_state_selected_diff_host(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    cbx_host_mode_enter(&hm, 0); /* host=0, selected=0 */
    cbx_host_mode_handle(&hm, 0, CBX_HM_DOWN, &g); /* selected=1 */

    assert_int_equal(cbx_host_mode_row_state(&hm, 0), CBX_ROW_HOST);
    assert_int_equal(cbx_host_mode_row_state(&hm, 1), CBX_ROW_SELECTED);
    assert_int_equal(cbx_host_mode_row_state(&hm, 2), CBX_ROW_FROZEN);
}

static void
test_row_state_null(void **state)
{
    (void)state;
    assert_int_equal(cbx_host_mode_row_state(NULL, 0), CBX_ROW_NORMAL);
}

/* --- is_frozen ------------------------------------------------------- */

static void
test_is_frozen(void **state)
{
    (void)state;
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    cbx_host_mode_enter(&hm, 0);

    assert_false(cbx_host_mode_is_frozen(&hm, 0)); /* host */
    assert_true(cbx_host_mode_is_frozen(&hm, 1));  /* not host */
    assert_true(cbx_host_mode_is_frozen(&hm, 2));
}

static void
test_is_frozen_inactive(void **state)
{
    (void)state;
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    /* Not active → nothing is frozen */
    assert_false(cbx_host_mode_is_frozen(&hm, 0));
    assert_false(cbx_host_mode_is_frozen(&hm, 1));
}

static void
test_is_frozen_null(void **state)
{
    (void)state;
    assert_false(cbx_host_mode_is_frozen(NULL, 0));
}

/* --- Full lifecycle -------------------------------------------------- */

static void
test_full_lifecycle(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 4);
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    hm.on_slot_change    = on_slot_change;
    hm_callbacks cb       = {0};
    hm.slot_change_data   = &cb;

    /* Enter host mode from controller 1 */
    int rc = cbx_host_mode_toggle(&hm, 1);
    assert_int_equal(rc, 1);
    assert_true(cbx_host_mode_is_active(&hm));
    assert_int_equal(hm.host_row, 1);
    assert_int_equal(hm.selected_row, 1);

    /* Navigate down to row 3 */
    cbx_host_mode_handle(&hm, 1, CBX_HM_DOWN, &g);
    cbx_host_mode_handle(&hm, 1, CBX_HM_DOWN, &g);
    assert_int_equal(hm.selected_row, 3);

    /* Move right in row 3 */
    rc = cbx_host_mode_handle(&hm, 1, CBX_HM_RIGHT, &g);
    assert_int_equal(rc, CBX_HM_RESULT_SLOT);
    assert_int_equal(cb.row, 3);
    assert_int_equal(cbx_select_grid_get_cur_col(&g, 3), 1);

    /* Exit host mode via R3 */
    rc = cbx_host_mode_handle(&hm, 1, CBX_HM_R3, &g);
    assert_int_equal(rc, CBX_HM_RESULT_EXIT);
    assert_false(cbx_host_mode_is_active(&hm));
}

/* --- Main ------------------------------------------------------------ */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* Init */
        cmocka_unit_test(test_init),
        cmocka_unit_test(test_init_null),
        /* Enter/Exit */
        cmocka_unit_test(test_enter),
        cmocka_unit_test(test_enter_null),
        cmocka_unit_test(test_enter_bad_row),
        cmocka_unit_test(test_exit),
        cmocka_unit_test(test_exit_null),
        /* Toggle */
        cmocka_unit_test(test_toggle_enter),
        cmocka_unit_test(test_toggle_exit),
        cmocka_unit_test(test_toggle_frozen),
        cmocka_unit_test(test_toggle_null),
        cmocka_unit_test(test_toggle_bad_row),
        /* Up/Down navigation */
        cmocka_unit_test(test_handle_down),
        cmocka_unit_test(test_handle_up),
        cmocka_unit_test(test_handle_up_boundary),
        cmocka_unit_test(test_handle_down_boundary),
        cmocka_unit_test(test_handle_down_then_up),
        /* Left/Right navigation */
        cmocka_unit_test(test_handle_right),
        cmocka_unit_test(test_handle_left),
        cmocka_unit_test(test_handle_left_boundary),
        cmocka_unit_test(test_handle_right_no_callback),
        /* Host edits any row */
        cmocka_unit_test(test_host_edits_other_row),
        /* R3 exit */
        cmocka_unit_test(test_handle_r3_exit),
        /* B close */
        cmocka_unit_test(test_handle_b),
        /* Frozen controller */
        cmocka_unit_test(test_handle_frozen),
        cmocka_unit_test(test_handle_frozen_right),
        /* NULL safety */
        cmocka_unit_test(test_handle_null_hm),
        cmocka_unit_test(test_handle_null_grid),
        cmocka_unit_test(test_handle_not_active),
        /* Accessors */
        cmocka_unit_test(test_is_active),
        cmocka_unit_test(test_get_host_row),
        cmocka_unit_test(test_get_selected_row),
        cmocka_unit_test(test_accessors_null),
        /* Visual state */
        cmocka_unit_test(test_row_state_inactive),
        cmocka_unit_test(test_row_state_active),
        cmocka_unit_test(test_row_state_selected_diff_host),
        cmocka_unit_test(test_row_state_null),
        /* is_frozen */
        cmocka_unit_test(test_is_frozen),
        cmocka_unit_test(test_is_frozen_inactive),
        cmocka_unit_test(test_is_frozen_null),
        /* Full lifecycle */
        cmocka_unit_test(test_full_lifecycle),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}