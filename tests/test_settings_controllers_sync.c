/*
 * test_settings_controllers_sync.c — Regression test for Task 6.
 *
 * Eliminates the stale dual-settings-copy problem between the Settings tab
 * and the Controllers tab.  Scenario (SPEC §5.2 / §5.5 / §7.3):
 *
 *   1. Default settings are loaded; the Controllers tab reflects the
 *      default virtual-controller count and per-slot types.
 *   2. The Settings tab edits VC_COUNT and a VC_TYPE slot, then saves.
 *   3. Without a manager restart, switching back to the Controllers tab
 *      shows the updated count/types.
 *
 * The fix makes the Settings-tab save path reload the manager's
 * authoritative settings object (mgr->settings, borrowed by the
 * Controllers tab as mgr->ct.settings) so both tabs agree for the
 * remainder of the session.  This test drives the same public tab action
 * functions and the real save path the manager dispatches.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include "manager/manager.h"
#include "manager/settings_tab.h"
#include "manager/controllers_tab.h"
#include "config/config_settings.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef CBX_FONT_PATH
#define CBX_FONT_PATH ""
#endif

static void ensure_dummy_driver(void)
{
    if (getenv("SDL_VIDEODRIVER") == NULL)
        SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "dummy",
                                 SDL_HINT_OVERRIDE);
}

/* Build an isolated HOME so the test never touches the user's real config.
 * Creates a fresh settings.yaml and stores the path in out (caller frees). */
static void setup_isolated_home(char *out, size_t outsz)
{
    snprintf(out, outsz, "/tmp/cbx_settings_sync_%d", (int)getpid());
    char cmd[PATH_MAX * 2 + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", out);
    int r0 = system(cmd);
    (void)r0;
    mkdir(out, 0700);
    setenv("HOME", out, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");
    unsetenv("FLATPAK_ID");
}

static void teardown_isolated_home(const char *home)
{
    char cmd[PATH_MAX * 2 + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", home);
    int r1 = system(cmd);
    (void)r1;
    unsetenv("HOME");
}

/* Helper: select the tab row at the given setting index (0-based). */
static void select_row(cbx_settings_tab *st, int index)
{
    while (cbx_settings_tab_selected(st) != index)
        cbx_settings_tab_move_down(st);
}

static void
test_defaults_then_edited_then_visible_after_save(void **state)
{
    (void)state;
    ensure_dummy_driver();

    char home[PATH_MAX];
    setup_isolated_home(home, sizeof(home));

    /* Write canonical default settings via the production save path so the
     * manager's "default settings loaded" phase is explicit and isolated. */
    cbx_settings defaults;
    cbx_settings_defaults(&defaults);
    assert_int_equal(cbx_settings_save(&defaults), 0);

    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    /* --- Step 1: default count/types visible in the Controllers tab --- */
    assert_int_equal(mgr.settings.virtual_controllers.count, 4);
    assert_string_equal(mgr.settings.virtual_controllers.types[0], "xb360");
    cbx_controllers_tab *ct = cbx_manager_controllers_tab(&mgr);
    assert_non_null(ct);
    /* The Controllers tab borrows the authoritative object. */
    assert_ptr_equal(ct->settings, &mgr.settings);
    assert_int_equal(ct->settings->virtual_controllers.count, 4);
    assert_string_equal(ct->settings->virtual_controllers.types[0], "xb360");
    assert_int_equal(ct->expected_target_count, 4);

    /* --- Step 2: edit VC_COUNT (4 -> 2) via the Settings tab ---------- */
    cbx_settings_tab *st = cbx_manager_settings_tab(&mgr);
    assert_non_null(st);

    /* VC_COUNT row. */
    select_row(st, CBX_ST_SET_VC_COUNT);
    assert_int_equal(cbx_settings_tab_activate(st), 0);     /* enter edit */
    assert_int_equal(cbx_settings_tab_edit_down(st), 0);    /* 4 -> 3     */
    assert_int_equal(cbx_settings_tab_edit_down(st), 0);    /* 3 -> 2     */
    assert_int_equal(cbx_settings_tab_confirm_edit(st), 0); /* leave edit */

    /* VC_TYPE_0 row: cycle xb360 -> ds5. */
    select_row(st, CBX_ST_SET_VC_TYPE_0);
    assert_int_equal(cbx_settings_tab_activate(st), 0);     /* enter edit */
    assert_int_equal(cbx_settings_tab_edit_up(st), 0);      /* xb360->ds5 */
    assert_int_equal(cbx_settings_tab_confirm_edit(st), 0); /* leave edit */

    /* Save row.  cbx_settings_tab_activate on SAVE calls
     * cbx_settings_tab_save -> the manager's post-save hook re-loads
     * mgr->settings from disk. */
    select_row(st, CBX_ST_SET_SAVE);
    assert_int_equal(cbx_settings_tab_activate(st), 0);
    assert_string_equal(cbx_settings_tab_status(st), "Settings saved.");

    /* --- Step 3: switch to Controllers tab, no restart -------------- */
    mgr.active_tab = CBX_MGR_TAB_CONTROLLERS;
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_CONTROLLERS);

    /* The Controllers tab's authoritative settings reflect the edits. */
    assert_int_equal(mgr.settings.virtual_controllers.count, 2);
    assert_string_equal(mgr.settings.virtual_controllers.types[0], "ds5");
    assert_int_equal(ct->settings->virtual_controllers.count, 2);
    assert_string_equal(ct->settings->virtual_controllers.types[0], "ds5");
    assert_int_equal(ct->expected_target_count, 2);

    cbx_manager_shutdown(&mgr);
    teardown_isolated_home(home);
}

int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_defaults_then_edited_then_visible_after_save),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
