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
 *   - Host edits any row's profile (L1/R1 profile cycle)
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

/* --- Profile-change callback tracking -------------------------------- */

typedef struct {
    int  count;
    int  row;
    char profile[CBX_GRID_PROFILE_LEN];
    char composite_path[CBX_MAX_PATH_LEN];
} hm_profile_callbacks;

static int
on_profile_change(int row_idx, const char *profile,
                  const char *composite_path, void *userdata)
{
    hm_profile_callbacks *cb = (hm_profile_callbacks *)userdata;
    if (cb) {
        cb->count++;
        cb->row = row_idx;
        if (profile)
            snprintf(cb->profile, sizeof(cb->profile), "%s", profile);
        if (composite_path)
            snprintf(cb->composite_path, sizeof(cb->composite_path),
                     "%s", composite_path);
    }
    return 0;
}

/* --- Dirty-surface trigger (W1) -------------------------------------- */

typedef struct {
    int transitions;
    bool last_active;
} hm_state_cb;

/*
 * Mirrors cbx_overlay_on_host_mode_change: a consumer marks the overlay
 * surface dirty on every host-mode state transition.  Recording the args
 * lets us assert that entering and exiting both fire the trigger.
 */
static int
on_state_change(bool active, void *userdata)
{
    hm_state_cb *cb = (hm_state_cb *)userdata;
    if (cb) {
        cb->transitions++;
        cb->last_active = active;
    }
    return 0;
}

/* --- Helpers ---------------------------------------------------------- */

static cbx_grid_composite_info
make_comp(const char *id, const char *name, const char *path)
{
    cbx_grid_composite_info c;
    memset(&c, 0, sizeof(c));
    if (id) {
        strncpy(c.id, id, CBX_MAX_ID_LEN - 1);
        c.id_stable = true;  /* test ids model real PersistentIds */
    }
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

/* Build a grid with caller-supplied persistent ids and composite paths.
 * Used by the hotplug-reconcile tests: the id models InputPlumber's
 * PersistentId (stable across a rebuild) while the path can be reused by a
 * different controller, so identity re-resolution can be distinguished
 * from stale-index lookup.  `stable[i]` selects whether row i's id is a
 * real PersistentId or the degraded (path-only) fallback. */
static void
build_grid_ids_paths_stable(cbx_select_grid *g,
                            const char *const *ids,
                            const char *const *paths,
                            const bool *stable, int n)
{
    cbx_grid_composite_info comps[CBX_GRID_MAX_ROWS];
    memset(comps, 0, sizeof(comps));
    for (int i = 0; i < n; i++) {
        snprintf(comps[i].id, sizeof(comps[i].id), "%s", ids[i]);
        snprintf(comps[i].model_name, sizeof(comps[i].model_name),
                 "Ctrl %d", i);
        snprintf(comps[i].composite_path, sizeof(comps[i].composite_path),
                 "/org/shadowblip/InputPlumber/%s",
                 paths ? paths[i] : ids[i]);
        comps[i].id_stable = stable ? stable[i] : true;
    }

    cbx_settings s;
    memset(&s, 0, sizeof(s));
    s.virtual_controllers.count = 4;
    for (int i = 0; i < 4; i++)
        strncpy(s.virtual_controllers.types[i], "xb360",
                CBX_MAX_TYPE_LEN - 1);

    cbx_assignments a;
    cbx_assignments_init(&a);

    cbx_select_grid_build(g, comps, n, &s, &a);
    cbx_select_grid_add_profile(g, "default");
}

static void
build_grid_ids_paths(cbx_select_grid *g,
                     const char *const *ids,
                     const char *const *paths, int n)
{
    build_grid_ids_paths_stable(g, ids, paths, NULL, n);
}

/* Build a grid whose rows have no stable PersistentId (degraded identity). */
static void
build_grid_degraded(cbx_select_grid *g,
                    const char *const *ids,
                    const char *const *paths, int n)
{
    bool stable[CBX_GRID_MAX_ROWS];
    for (int i = 0; i < n && i < CBX_GRID_MAX_ROWS; i++)
        stable[i] = false;
    build_grid_ids_paths_stable(g, ids, paths, stable, n);
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

/* --- Handle: host edits any row's profile (L1/R1) ------------------- */

static void
test_handle_profile_prev(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 2);
    cbx_select_grid_add_profile(&g, "alt");

    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    hm_profile_callbacks cb;
    memset(&cb, 0, sizeof(cb));
    hm.on_profile_change   = on_profile_change;
    hm.profile_change_data = &cb;
    cbx_host_mode_enter(&hm, 0);

    /* Row 0 starts on "default" (list index 0); PREV moves to the
     * previous list entry ("alt"). */
    assert_string_equal(cbx_select_grid_get_profile(&g, 0), "default");
    int rc = cbx_host_mode_handle(&hm, 0, CBX_HM_PROFILE_PREV, &g);
    assert_int_equal(rc, CBX_HM_RESULT_PROFILE);
    assert_int_equal(cb.count, 1);
    assert_int_equal(cb.row, 0);
    assert_string_equal(cbx_select_grid_get_profile(&g, 0), "alt");
    /* Callback carried the new profile name and the row's composite path. */
    assert_string_equal(cb.profile, "alt");
    assert_string_equal(cb.composite_path, g.rows[0].composite_path);
}

static void
test_handle_profile_next(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 2);
    cbx_select_grid_add_profile(&g, "alt");
    /* Start row 0 on "alt" so NEXT wraps back to "default". */
    snprintf(g.rows[0].profile, CBX_GRID_PROFILE_LEN, "alt");

    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    hm_profile_callbacks cb;
    memset(&cb, 0, sizeof(cb));
    hm.on_profile_change   = on_profile_change;
    hm.profile_change_data = &cb;
    cbx_host_mode_enter(&hm, 0);

    int rc = cbx_host_mode_handle(&hm, 0, CBX_HM_PROFILE_NEXT, &g);
    assert_int_equal(rc, CBX_HM_RESULT_PROFILE);
    assert_int_equal(cb.count, 1);
    assert_int_equal(cb.row, 0);
    assert_string_equal(cbx_select_grid_get_profile(&g, 0), "default");
    assert_string_equal(cb.profile, "default");
}

static void
test_host_edits_other_row_profile(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    cbx_select_grid_add_profile(&g, "alt");
    /* Give row 1 a known starting profile distinct from row 0. */
    snprintf(g.rows[1].profile, CBX_GRID_PROFILE_LEN, "alt");

    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    hm_profile_callbacks cb;
    memset(&cb, 0, sizeof(cb));
    hm.on_profile_change   = on_profile_change;
    hm.profile_change_data = &cb;
    cbx_host_mode_enter(&hm, 0);

    /* Host (row 0) navigates to row 1 and cycles its profile. */
    cbx_host_mode_handle(&hm, 0, CBX_HM_DOWN, &g);
    assert_int_equal(hm.selected_row, 1);
    int rc = cbx_host_mode_handle(&hm, 0, CBX_HM_PROFILE_NEXT, &g);
    assert_int_equal(rc, CBX_HM_RESULT_PROFILE);
    assert_int_equal(cb.row, 1);
    /* Row 1's profile changed; row 0's did not. */
    assert_string_equal(cbx_select_grid_get_profile(&g, 1), "default");
    assert_string_equal(cbx_select_grid_get_profile(&g, 0), "default");
    assert_string_equal(cb.composite_path, g.rows[1].composite_path);
}

static void
test_handle_profile_frozen(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 3);
    cbx_select_grid_add_profile(&g, "alt");

    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    hm_profile_callbacks cb;
    memset(&cb, 0, sizeof(cb));
    hm.on_profile_change   = on_profile_change;
    hm.profile_change_data = &cb;
    cbx_host_mode_enter(&hm, 0); /* controller 0 is host */

    /* Frozen controller 1 sends a profile-cycle input: rejected, and the
     * frozen row's profile is untouched. */
    int rc = cbx_host_mode_handle(&hm, 1, CBX_HM_PROFILE_NEXT, &g);
    assert_int_equal(rc, CBX_HM_RESULT_FROZEN);
    assert_int_equal(cb.count, 0);
    assert_string_equal(cbx_select_grid_get_profile(&g, 1), "default");
    /* The host's own row is untouched too. */
    assert_string_equal(cbx_select_grid_get_profile(&g, 0), "default");
}

static void
test_handle_profile_no_callback(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 2);
    cbx_select_grid_add_profile(&g, "alt");

    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    /* No on_profile_change set. */
    cbx_host_mode_enter(&hm, 0);

    int rc = cbx_host_mode_handle(&hm, 0, CBX_HM_PROFILE_PREV, &g);
    assert_int_equal(rc, CBX_HM_RESULT_PROFILE);
    assert_string_equal(cbx_select_grid_get_profile(&g, 0), "alt");
}

static void
test_handle_profile_no_profiles(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 2);
    cbx_select_grid_clear_profiles(&g);

    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    cbx_host_mode_enter(&hm, 0);

    /* No profiles to cycle: a no-op (NONE), never a bogus profile change. */
    int rc = cbx_host_mode_handle(&hm, 0, CBX_HM_PROFILE_NEXT, &g);
    assert_int_equal(rc, CBX_HM_RESULT_NONE);
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

/* --- State-change dirty trigger (W1) ---------------------------------- */

/*
 * Every host-mode state transition fires on_state_change — the dirty
 * trigger a consumer wires to mark the overlay surface dirty (SPEC §4.4/§4.9).
 * Entering and exiting via both the direct API and the toggle must fire it.
 */
static void
test_state_change_on_enter_exit(void **state)
{
    (void)state;
    cbx_select_grid g;
    build_test_grid(&g, 2);
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    hm_state_cb cb = {0, false};
    hm.on_state_change   = on_state_change;
    hm.state_change_data = &cb;

    /* Enter host mode → transition with active=true. */
    cbx_host_mode_enter(&hm, 0);
    assert_int_equal(cb.transitions, 1);
    assert_true(cb.last_active);

    /* Navigate (MOVED) must NOT fire a state-change transition. */
    cbx_host_mode_handle(&hm, 0, CBX_HM_DOWN, &g);
    assert_int_equal(cb.transitions, 1);
    assert_true(cb.last_active);

    /* Exit host mode (R3) → transition with active=false. */
    assert_int_equal(cbx_host_mode_handle(&hm, 0, CBX_HM_R3, &g),
                     CBX_HM_RESULT_EXIT);
    assert_int_equal(cb.transitions, 2);
    assert_false(cb.last_active);
}

static void
test_state_change_on_toggle(void **state)
{
    (void)state;
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    hm_state_cb cb = {0, false};
    hm.on_state_change   = on_state_change;
    hm.state_change_data = &cb;

    /* Toggle in → fires. */
    assert_int_equal(cbx_host_mode_toggle(&hm, 1), 1);
    assert_int_equal(cb.transitions, 1);
    assert_true(cb.last_active);

    /* Toggle out → fires. */
    assert_int_equal(cbx_host_mode_toggle(&hm, 1), 0);
    assert_int_equal(cb.transitions, 2);
    assert_false(cb.last_active);

    /* Frozen controller toggle → ignored, no transition. */
    assert_int_equal(cbx_host_mode_toggle(&hm, 1), 1); /* re-enter */
    assert_int_equal(cb.transitions, 3);
    assert_int_equal(cbx_host_mode_toggle(&hm, 2), -1); /* frozen */
    assert_int_equal(cb.transitions, 3);
}

/*
 * W2 no-op guard: exiting an already-idle host-mode object is not a real
 * state transition, so it must not fire the dirty trigger.  A redundant
 * exit on an inactive host-mode object emits no spurious state change.
 */
static void
test_exit_noop_no_transition(void **state)
{
    (void)state;
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    hm_state_cb cb = {0, false};
    hm.on_state_change   = on_state_change;
    hm.state_change_data = &cb;

    /* Exit on an already-idle host-mode object (no prior enter): no fire. */
    assert_int_equal(cbx_host_mode_exit(&hm), 0);
    assert_int_equal(cb.transitions, 0);

    /* Enter once then exit twice: only the real exit fires (the redundant
     * second exit is a no-op). */
    assert_int_equal(cbx_host_mode_enter(&hm, 0), 0);
    assert_int_equal(cb.transitions, 1);
    assert_int_equal(cbx_host_mode_exit(&hm), 0);
    assert_int_equal(cb.transitions, 2);
    assert_int_equal(cbx_host_mode_exit(&hm), 0); /* no-op */
    assert_int_equal(cb.transitions, 2);
}

/* =================================================================== */
/*  Hotplug reconcile (SPEC §4.4/§10.1)                                 */
/* =================================================================== */
/*
 * A hotplug rebuild re-derives grid rows from composite order, so stored
 * row indices can shift or disappear.  cbx_host_mode_reconcile must
 * re-resolve the host (and selected) row by persistent id, preserving
 * exclusivity, and must exit host mode when the host controller is gone
 * rather than freezing everyone or handing host privileges to whatever
 * controller lands on the old index.
 */

/* Host's row shifts because an earlier controller was removed. */
static void
test_reconcile_host_row_shifted(void **state)
{
    (void)state;
    static const char *ids3[]  = { "A", "B", "C" };
    static const char *paths3[] = { "pa", "pb", "pc" };
    static const char *ids2[]  = { "B", "C" };
    static const char *paths2[] = { "pb", "pc" };
    cbx_select_grid g;
    build_grid_ids_paths(&g, ids3, paths3, 3);

    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    /* B (row 1) is host and has navigated to edit C (row 2). */
    assert_int_equal(cbx_host_mode_enter_with_grid(&hm, &g, 1), 0);
    assert_int_equal(cbx_host_mode_handle(&hm, 1, CBX_HM_DOWN, &g),
                     CBX_HM_RESULT_MOVED);
    assert_int_equal(hm.selected_row, 2);

    /* Remove A: B shifts to row 0, C to row 1. */
    build_grid_ids_paths(&g, ids2, paths2, 2);
    assert_int_equal(cbx_host_mode_reconcile(&hm, &g), 1);

    assert_true(cbx_host_mode_is_active(&hm));
    assert_int_equal(cbx_host_mode_get_host_row(&hm), 0);
    assert_int_equal(cbx_host_mode_get_selected_row(&hm), 1);
    assert_string_equal(g.rows[0].id, "B");
    assert_string_equal(g.rows[1].id, "C");

    /* Host still acts; the other row stays frozen. */
    assert_int_equal(cbx_host_mode_handle(&hm, 0, CBX_HM_RIGHT, &g),
                     CBX_HM_RESULT_SLOT);
    assert_int_equal(cbx_host_mode_handle(&hm, 1, CBX_HM_RIGHT, &g),
                     CBX_HM_RESULT_FROZEN);
}

/* Production entry path (toggle_with_grid) also records the identity. */
static void
test_reconcile_after_toggle_with_grid(void **state)
{
    (void)state;
    static const char *ids3[]  = { "A", "B", "C" };
    static const char *paths3[] = { "pa", "pb", "pc" };
    static const char *ids2[]  = { "B", "C" };
    static const char *paths2[] = { "pb", "pc" };
    cbx_select_grid g;
    build_grid_ids_paths(&g, ids3, paths3, 3);

    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    assert_int_equal(cbx_host_mode_toggle_with_grid(&hm, &g, 1), 1);

    build_grid_ids_paths(&g, ids2, paths2, 2);
    assert_int_equal(cbx_host_mode_reconcile(&hm, &g), 1);
    assert_true(cbx_host_mode_is_active(&hm));
    assert_int_equal(cbx_host_mode_get_host_row(&hm), 0);
    assert_string_equal(g.rows[0].id, "B");
}

/* Host removed → host mode exits (no privilege shift, no input freeze). */
static void
test_reconcile_host_removed_exits(void **state)
{
    (void)state;
    static const char *ids3[]  = { "A", "B", "C" };
    static const char *paths3[] = { "pa", "pb", "pc" };
    static const char *ids2[]  = { "A", "C" };
    static const char *paths2[] = { "pa", "pc" };
    cbx_select_grid g;
    build_grid_ids_paths(&g, ids3, paths3, 3);

    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    hm_state_cb cb = {0, false};
    hm.on_state_change   = on_state_change;
    hm.state_change_data = &cb;
    assert_int_equal(cbx_host_mode_enter_with_grid(&hm, &g, 1), 0); /* B */
    assert_int_equal(cb.transitions, 1);

    /* Remove B (the host). */
    build_grid_ids_paths(&g, ids2, paths2, 2);
    assert_int_equal(cbx_host_mode_reconcile(&hm, &g), 0);

    assert_false(cbx_host_mode_is_active(&hm));
    assert_int_equal(hm.host_row, -1);
    /* Exiting fired the dirty trigger exactly once. */
    assert_int_equal(cb.transitions, 2);
    assert_false(cb.last_active);
}

/* The selected (edited) row is removed → selection falls back to the host. */
static void
test_reconcile_selected_removed_falls_back(void **state)
{
    (void)state;
    static const char *ids3[]  = { "A", "B", "C" };
    static const char *paths3[] = { "pa", "pb", "pc" };
    static const char *ids2[]  = { "A", "B" };
    static const char *paths2[] = { "pa", "pb" };
    cbx_select_grid g;
    build_grid_ids_paths(&g, ids3, paths3, 3);

    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    cbx_host_mode_enter_with_grid(&hm, &g, 0); /* host A */
    cbx_host_mode_handle(&hm, 0, CBX_HM_DOWN, &g);
    cbx_host_mode_handle(&hm, 0, CBX_HM_DOWN, &g);
    assert_int_equal(hm.selected_row, 2); /* editing C */

    /* Remove C (the edited row), keep the host. */
    build_grid_ids_paths(&g, ids2, paths2, 2);
    assert_int_equal(cbx_host_mode_reconcile(&hm, &g), 1);

    assert_true(cbx_host_mode_is_active(&hm));
    assert_int_equal(cbx_host_mode_get_host_row(&hm), 0);
    assert_int_equal(cbx_host_mode_get_selected_row(&hm), 0);
}

/* Empty grid (all controllers gone) → exit rather than freeze. */
static void
test_reconcile_empty_grid_exits(void **state)
{
    (void)state;
    static const char *ids1[]  = { "A" };
    static const char *paths1[] = { "pa" };
    cbx_select_grid g;
    build_grid_ids_paths(&g, ids1, paths1, 1);

    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    cbx_host_mode_enter_with_grid(&hm, &g, 0);

    cbx_settings s;
    memset(&s, 0, sizeof(s));
    s.virtual_controllers.count = 1;
    snprintf(s.virtual_controllers.types[0], CBX_MAX_TYPE_LEN, "xb360");
    cbx_assignments a;
    cbx_assignments_init(&a);
    cbx_select_grid_build(&g, NULL, 0, &s, &a);
    assert_int_equal(g.row_count, 0);

    assert_int_equal(cbx_host_mode_reconcile(&hm, &g), 0);
    assert_false(cbx_host_mode_is_active(&hm));
}

/*
 * Persistent id wins over the composite path: a different controller that
 * reuses the old host's path must not inherit host privileges.
 */
static void
test_reconcile_prefers_persistent_id_over_path(void **state)
{
    (void)state;
    static const char *ids3[]  = { "A", "B", "C" };
    static const char *paths3[] = { "pa", "pb", "pc" };
    static const char *ids2[]  = { "X", "B" };
    static const char *paths2[] = { "pb", "pb2" }; /* pb reused by X */
    cbx_select_grid g;
    build_grid_ids_paths(&g, ids3, paths3, 3);

    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    assert_int_equal(cbx_host_mode_enter_with_grid(&hm, &g, 1), 0); /* B */

    build_grid_ids_paths(&g, ids2, paths2, 2);
    assert_int_equal(cbx_host_mode_reconcile(&hm, &g), 1);

    /* Followed B's persistent id (row 1), not X on the reused path. */
    assert_int_equal(cbx_host_mode_get_host_row(&hm), 1);
    assert_string_equal(g.rows[1].id, "B");
    assert_true(cbx_host_mode_is_frozen(&hm, 0));
    assert_false(cbx_host_mode_is_frozen(&hm, 1));
}

/*
 * A stable PersistentId is authoritative.  When the host device is gone, a
 * different controller that reused its composite path must NOT inherit host
 * privileges: the stored stable id no longer matches any row, so host mode
 * exits instead of falling back to the reused path (SPEC §4.4).
 */
static void
test_reconcile_stable_id_reused_path_exits(void **state)
{
    (void)state;
    static const char *ids2[]  = { "A", "B" };
    static const char *paths2[] = { "pa", "pb" };
    static const char *ids_after[]  = { "A", "X" };
    static const char *paths_after[] = { "pa", "pb" }; /* X reuses pb */
    cbx_select_grid g;
    build_grid_ids_paths(&g, ids2, paths2, 2);

    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    hm_state_cb cb = {0, false};
    hm.on_state_change   = on_state_change;
    hm.state_change_data = &cb;
    /* B (row 1, stable id) is the host. */
    assert_int_equal(cbx_host_mode_enter_with_grid(&hm, &g, 1), 0);
    assert_string_equal(hm.host_id, "B");
    assert_string_equal(hm.host_composite_path, "");
    assert_int_equal(cb.transitions, 1);

    /* B unplugged; X (a different controller) takes the pb path. */
    build_grid_ids_paths(&g, ids_after, paths_after, 2);
    assert_int_equal(cbx_host_mode_reconcile(&hm, &g), 0);

    assert_false(cbx_host_mode_is_active(&hm));
    assert_int_equal(cb.transitions, 2);
    /* X on the reused path must NOT be frozen-as-host or granted input. */
    assert_false(cbx_host_mode_is_frozen(&hm, 1));
}

/*
 * A degraded (no stable PersistentId) host is re-resolved by composite path
 * against another degraded row: a re-enumerated degraded device at the same
 * path keeps host mode and the path fallback still works.
 */
static void
test_reconcile_degraded_id_matches_same_path(void **state)
{
    (void)state;
    static const char *ids1[]  = { "" };
    static const char *paths1[] = { "pa" };
    cbx_select_grid g;
    build_grid_degraded(&g, ids1, paths1, 1);

    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    assert_int_equal(cbx_host_mode_enter_with_grid(&hm, &g, 0), 0);
    assert_string_equal(hm.host_id, "");            /* degraded */
    assert_string_equal(hm.host_composite_path,
                        g.rows[0].composite_path);

    /* Rebuild with the same degraded path. */
    build_grid_degraded(&g, ids1, paths1, 1);
    assert_int_equal(cbx_host_mode_reconcile(&hm, &g), 1);
    assert_true(cbx_host_mode_is_active(&hm));
    assert_int_equal(cbx_host_mode_get_host_row(&hm), 0);
}

/*
 * A degraded host must not hand privileges to a controller that now reports
 * a stable PersistentId at the same path: the path belongs to a different
 * physical device, so host mode exits.
 */
static void
test_reconcile_degraded_id_stable_row_exits(void **state)
{
    (void)state;
    static const char *ids1[]  = { "" };
    static const char *paths1[] = { "pa" };
    static const char *ids_after[]  = { "X" };
    static const char *paths_after[] = { "pa" }; /* X now reports an id */
    cbx_select_grid g;
    build_grid_degraded(&g, ids1, paths1, 1);

    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    assert_int_equal(cbx_host_mode_enter_with_grid(&hm, &g, 0), 0);
    assert_string_equal(hm.host_id, "");

    build_grid_ids_paths(&g, ids_after, paths_after, 1);
    assert_int_equal(cbx_host_mode_reconcile(&hm, &g), 0);
    assert_false(cbx_host_mode_is_active(&hm));
}

/* Inactive host mode is a no-op reconcile. */
static void
test_reconcile_inactive_noop(void **state)
{
    (void)state;
    static const char *ids1[]  = { "A" };
    static const char *paths1[] = { "pa" };
    cbx_select_grid g;
    build_grid_ids_paths(&g, ids1, paths1, 1);
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);

    assert_int_equal(cbx_host_mode_reconcile(&hm, &g), 0);
    assert_false(cbx_host_mode_is_active(&hm));
}

/* NULL args are rejected, not dereferenced. */
static void
test_reconcile_null(void **state)
{
    (void)state;
    static const char *ids1[]  = { "A" };
    static const char *paths1[] = { "pa" };
    cbx_select_grid g;
    build_grid_ids_paths(&g, ids1, paths1, 1);
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);

    assert_int_equal(cbx_host_mode_reconcile(NULL, &g), -EINVAL);
    assert_int_equal(cbx_host_mode_reconcile(&hm, NULL), -EINVAL);
}

/* Grid-aware entry bounds-checks the host row. */
static void
test_enter_with_grid_bounds(void **state)
{
    (void)state;
    static const char *ids1[]  = { "A" };
    static const char *paths1[] = { "pa" };
    cbx_select_grid g;
    build_grid_ids_paths(&g, ids1, paths1, 1);
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);

    assert_int_equal(cbx_host_mode_enter_with_grid(&hm, &g, 1), -EINVAL);
    assert_false(cbx_host_mode_is_active(&hm));
    assert_int_equal(cbx_host_mode_toggle_with_grid(&hm, &g, 5), -2);
    assert_false(cbx_host_mode_is_active(&hm));
}

/* An out-of-range sender cannot act (hardening). */
static void
test_handle_sender_out_of_range(void **state)
{
    (void)state;
    static const char *ids1[]  = { "A" };
    static const char *paths1[] = { "pa" };
    cbx_select_grid g;
    build_grid_ids_paths(&g, ids1, paths1, 1);
    cbx_host_mode hm;
    cbx_host_mode_init(&hm);
    cbx_host_mode_enter_with_grid(&hm, &g, 0);

    assert_int_equal(cbx_host_mode_handle(&hm, 9, CBX_HM_DOWN, &g),
                     CBX_HM_RESULT_ERROR);
    assert_true(cbx_host_mode_is_active(&hm));
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
        /* Host edits any row's profile (L1/R1) */
        cmocka_unit_test(test_handle_profile_prev),
        cmocka_unit_test(test_handle_profile_next),
        cmocka_unit_test(test_host_edits_other_row_profile),
        cmocka_unit_test(test_handle_profile_frozen),
        cmocka_unit_test(test_handle_profile_no_callback),
        cmocka_unit_test(test_handle_profile_no_profiles),
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
        /* Dirty-surface trigger (W1) */
        cmocka_unit_test(test_state_change_on_enter_exit),
        cmocka_unit_test(test_state_change_on_toggle),
        cmocka_unit_test(test_exit_noop_no_transition),
        /* Hotplug reconcile (SPEC §4.4/§10.1) */
        cmocka_unit_test(test_reconcile_host_row_shifted),
        cmocka_unit_test(test_reconcile_after_toggle_with_grid),
        cmocka_unit_test(test_reconcile_host_removed_exits),
        cmocka_unit_test(test_reconcile_selected_removed_falls_back),
        cmocka_unit_test(test_reconcile_empty_grid_exits),
        cmocka_unit_test(test_reconcile_prefers_persistent_id_over_path),
        cmocka_unit_test(test_reconcile_stable_id_reused_path_exits),
        cmocka_unit_test(test_reconcile_degraded_id_matches_same_path),
        cmocka_unit_test(test_reconcile_degraded_id_stable_row_exits),
        cmocka_unit_test(test_reconcile_inactive_noop),
        cmocka_unit_test(test_reconcile_null),
        cmocka_unit_test(test_enter_with_grid_bounds),
        cmocka_unit_test(test_handle_sender_out_of_range),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}