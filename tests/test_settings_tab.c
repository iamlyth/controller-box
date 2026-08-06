/*
 * test_settings_tab.c — cmocka tests for the Settings tab (Task 39).
 *
 * Tests cover:
 *   - Init: panel populated, settings loaded, list has correct rows
 *   - Shutdown: null-safe, removes children
 *   - Navigation: move up/down/wrap
 *   - Toggle launch at boot
 *   - Edit theme (cycle), opacity (adjust), VC count (adjust)
 *   - Edit VC type (cycle)
 *   - Edit trigger combo (cycle)
 *   - Save settings (writes to disk)
 *   - Cancel edit (reverts changes)
 *   - Confirm edit (returns to list mode)
 *   - Accessors: null-safe, correct values
 *   - Icon overrides: set, remove, lookup, validate
 *   - Rendering: no crash
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include "manager/settings_tab.h"
#include "manager/manager.h"
#include "config/config_settings.h"
#include "config/config_paths.h"
#include "ui/widget.h"

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

/* --- Test helpers -------------------------------------------------------- */

static void ensure_dummy_driver(void)
{
    if (getenv("SDL_VIDEODRIVER") == NULL)
        SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "dummy",
                                 SDL_HINT_OVERRIDE);
}

typedef struct {
    char tmp[256];
    cbx_manager mgr;
    cbx_settings_tab tab;
} st_fixture;

static int st_setup(void **state)
{
    ensure_dummy_driver();

    st_fixture *f = malloc(sizeof(*f));
    assert_non_null(f);
    memset(f, 0, sizeof(*f));

    /* Set up isolated HOME. */
    snprintf(f->tmp, sizeof(f->tmp), "/tmp/cbx_st_test_%d", (int)getpid());
    char cmd[PATH_MAX * 2 + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", f->tmp);
    int r0 = system(cmd);
    (void)r0;
    mkdir(f->tmp, 0700);
    setenv("HOME", f->tmp, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");

    /* Init manager (creates window, renderer, text cache, panels). */
    int rc = cbx_manager_init(&f->mgr, NULL);
    assert_int_equal(rc, 0);

    *state = f;
    return 0;
}

static int st_teardown(void **state)
{
    st_fixture *f = *state;
    cbx_settings_tab_shutdown(&f->tab);
    cbx_manager_shutdown(&f->mgr);

    char cmd[PATH_MAX * 2 + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", f->tmp);
    int r = system(cmd);
    (void)r;
    unsetenv("HOME");
    free(f);
    return 0;
}

#define FIX(s) ((st_fixture *)*(s))

/* --- Init tests --------------------------------------------------------- */

/* Init populates panel with 3 children (list, button, label). */
static void test_init_populates_panel(void **state)
{
    st_fixture *f = FIX(state);
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_SETTINGS];
    int rc = cbx_settings_tab_init(&f->tab, panel, &f->mgr.text_cache,
                                     &f->mgr.theme, f->mgr.font_id);
    assert_int_equal(rc, 0);
    assert_int_equal(panel->child_count, 3);
}

/* Init loads settings (defaults if no file). */
static void test_init_loads_defaults(void **state)
{
    st_fixture *f = FIX(state);
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_SETTINGS];
    cbx_settings_tab_init(&f->tab, panel, &f->mgr.text_cache,
                            &f->mgr.theme, f->mgr.font_id);

    const cbx_settings *s = cbx_settings_tab_settings(&f->tab);
    assert_non_null(s);
    assert_string_equal(s->overlay_trigger, "Select+A");
    assert_true(s->launch_at_boot);
    assert_string_equal(s->theme, "default");
    assert_float_equal(s->overlay_opacity, 0.85, 0.001);
    assert_int_equal(s->virtual_controllers.count, 4);
}

/* Init null args. */
static void test_init_null_args(void **state)
{
    (void)state;
    cbx_settings_tab tab;
    assert_int_equal(cbx_settings_tab_init(&tab, NULL, NULL, NULL, -1), -EINVAL);
    assert_int_equal(cbx_settings_tab_init(NULL, (cbx_panel *)1, NULL, NULL, -1), -EINVAL);
}

/* Shutdown null-safe. */
static void test_shutdown_null_safe(void **state)
{
    (void)state;
    cbx_settings_tab_shutdown(NULL);
    cbx_settings_tab tab;
    memset(&tab, 0, sizeof(tab));
    cbx_settings_tab_shutdown(&tab);
}

/* Shutdown removes children from panel. */
static void test_shutdown_removes_children(void **state)
{
    st_fixture *f = FIX(state);
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_SETTINGS];
    cbx_settings_tab_init(&f->tab, panel, &f->mgr.text_cache,
                            &f->mgr.theme, f->mgr.font_id);
    assert_int_equal(panel->child_count, 3);

    cbx_settings_tab_shutdown(&f->tab);
    assert_int_equal(panel->child_count, 0);
}

/* --- Navigation tests --------------------------------------------------- */

/* Move down wraps around. */
static void test_move_down_wrap(void **state)
{
    st_fixture *f = FIX(state);
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_SETTINGS];
    cbx_settings_tab_init(&f->tab, panel, &f->mgr.text_cache,
                            &f->mgr.theme, f->mgr.font_id);

    int count = cbx_settings_tab_setting_count(&f->tab);
    f->tab.selected = 0;

    for (int i = 1; i < count; i++) {
        cbx_settings_tab_move_down(&f->tab);
        assert_int_equal(cbx_settings_tab_selected(&f->tab), i);
    }
    /* One more wraps to 0. */
    cbx_settings_tab_move_down(&f->tab);
    assert_int_equal(cbx_settings_tab_selected(&f->tab), 0);
}

/* Move up wraps around. */
static void test_move_up_wrap(void **state)
{
    st_fixture *f = FIX(state);
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_SETTINGS];
    cbx_settings_tab_init(&f->tab, panel, &f->mgr.text_cache,
                            &f->mgr.theme, f->mgr.font_id);

    f->tab.selected = 0;
    cbx_settings_tab_move_up(&f->tab);
    int count = cbx_settings_tab_setting_count(&f->tab);
    assert_int_equal(cbx_settings_tab_selected(&f->tab), count - 1);
}

/* --- Toggle test -------------------------------------------------------- */

/* Activate toggles launch at boot. */
static void test_toggle_launch_boot(void **state)
{
    st_fixture *f = FIX(state);
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_SETTINGS];
    cbx_settings_tab_init(&f->tab, panel, &f->mgr.text_cache,
                            &f->mgr.theme, f->mgr.font_id);

    f->tab.selected = CBX_ST_SET_LAUNCH_BOOT;
    bool initial = f->tab.settings.launch_at_boot;

    cbx_settings_tab_activate(&f->tab);
    assert_int_equal(f->tab.settings.launch_at_boot, !initial);

    cbx_settings_tab_activate(&f->tab);
    assert_int_equal(f->tab.settings.launch_at_boot, initial);
}

/* --- Edit tests --------------------------------------------------------- */

/* Edit theme: enter edit mode, cycle up. */
static void test_edit_theme(void **state)
{
    st_fixture *f = FIX(state);
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_SETTINGS];
    cbx_settings_tab_init(&f->tab, panel, &f->mgr.text_cache,
                            &f->mgr.theme, f->mgr.font_id);

    f->tab.selected = CBX_ST_SET_THEME;
    assert_int_equal(cbx_settings_tab_mode(&f->tab), CBX_ST_MODE_LIST);

    /* Enter edit mode. */
    cbx_settings_tab_activate(&f->tab);
    assert_int_equal(cbx_settings_tab_mode(&f->tab), CBX_ST_MODE_EDIT);

    /* Cycle up: default → dark. */
    cbx_settings_tab_edit_up(&f->tab);
    assert_string_equal(f->tab.settings.theme, "dark");

    /* Cycle up: dark → light. */
    cbx_settings_tab_edit_up(&f->tab);
    assert_string_equal(f->tab.settings.theme, "light");

    /* Cycle up: light → default (wrap). */
    cbx_settings_tab_edit_up(&f->tab);
    assert_string_equal(f->tab.settings.theme, "default");

    /* Cycle down: default → light. */
    cbx_settings_tab_edit_down(&f->tab);
    assert_string_equal(f->tab.settings.theme, "light");

    /* Confirm edit. */
    cbx_settings_tab_confirm_edit(&f->tab);
    assert_int_equal(cbx_settings_tab_mode(&f->tab), CBX_ST_MODE_LIST);
}

/* Edit opacity: adjust up and down. */
static void test_edit_opacity(void **state)
{
    st_fixture *f = FIX(state);
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_SETTINGS];
    cbx_settings_tab_init(&f->tab, panel, &f->mgr.text_cache,
                            &f->mgr.theme, f->mgr.font_id);

    f->tab.selected = CBX_ST_SET_OPACITY;
    cbx_settings_tab_activate(&f->tab);
    assert_int_equal(cbx_settings_tab_mode(&f->tab), CBX_ST_MODE_EDIT);

    double initial = f->tab.settings.overlay_opacity;

    cbx_settings_tab_edit_up(&f->tab);
    assert_float_equal(f->tab.settings.overlay_opacity, initial + 0.05, 0.001);

    cbx_settings_tab_edit_down(&f->tab);
    assert_float_equal(f->tab.settings.overlay_opacity, initial, 0.001);

    /* Clamp at 1.0. */
    f->tab.settings.overlay_opacity = 0.98;
    cbx_settings_tab_edit_up(&f->tab);
    assert_float_equal(f->tab.settings.overlay_opacity, 1.0, 0.001);
    cbx_settings_tab_edit_up(&f->tab);
    assert_float_equal(f->tab.settings.overlay_opacity, 1.0, 0.001);

    /* Clamp at 0.0. */
    f->tab.settings.overlay_opacity = 0.02;
    cbx_settings_tab_edit_down(&f->tab);
    assert_float_equal(f->tab.settings.overlay_opacity, 0.0, 0.001);
    cbx_settings_tab_edit_down(&f->tab);
    assert_float_equal(f->tab.settings.overlay_opacity, 0.0, 0.001);

    cbx_settings_tab_cancel_edit(&f->tab);
    assert_int_equal(cbx_settings_tab_mode(&f->tab), CBX_ST_MODE_LIST);
}

/* Edit VC count: adjust up and down. */
static void test_edit_vc_count(void **state)
{
    st_fixture *f = FIX(state);
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_SETTINGS];
    cbx_settings_tab_init(&f->tab, panel, &f->mgr.text_cache,
                            &f->mgr.theme, f->mgr.font_id);

    f->tab.selected = CBX_ST_SET_VC_COUNT;
    cbx_settings_tab_activate(&f->tab);

    int initial = f->tab.settings.virtual_controllers.count;
    assert_int_equal(initial, 4);

    cbx_settings_tab_edit_up(&f->tab);
    assert_int_equal(f->tab.settings.virtual_controllers.count, 5);

    cbx_settings_tab_edit_down(&f->tab);
    assert_int_equal(f->tab.settings.virtual_controllers.count, 4);

    /* Clamp at max. */
    f->tab.settings.virtual_controllers.count = CBX_MAX_CONTROLLERS;
    cbx_settings_tab_edit_up(&f->tab);
    assert_int_equal(f->tab.settings.virtual_controllers.count, CBX_MAX_CONTROLLERS);

    /* Clamp at min (1). */
    f->tab.settings.virtual_controllers.count = 1;
    cbx_settings_tab_edit_down(&f->tab);
    assert_int_equal(f->tab.settings.virtual_controllers.count, 1);

    cbx_settings_tab_confirm_edit(&f->tab);
}

/* Edit VC type: cycle through known types. */
static void test_edit_vc_type(void **state)
{
    st_fixture *f = FIX(state);
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_SETTINGS];
    cbx_settings_tab_init(&f->tab, panel, &f->mgr.text_cache,
                            &f->mgr.theme, f->mgr.font_id);

    f->tab.selected = CBX_ST_SET_VC_TYPE_0;
    cbx_settings_tab_activate(&f->tab);

    assert_string_equal(f->tab.settings.virtual_controllers.types[0], "xb360");

    cbx_settings_tab_edit_up(&f->tab);
    assert_string_equal(f->tab.settings.virtual_controllers.types[0], "ds5");

    cbx_settings_tab_edit_up(&f->tab);
    assert_string_equal(f->tab.settings.virtual_controllers.types[0], "deck");

    cbx_settings_tab_edit_down(&f->tab);
    assert_string_equal(f->tab.settings.virtual_controllers.types[0], "ds5");

    cbx_settings_tab_confirm_edit(&f->tab);
}

/* Edit trigger combo: cycle. */
static void test_edit_trigger(void **state)
{
    st_fixture *f = FIX(state);
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_SETTINGS];
    cbx_settings_tab_init(&f->tab, panel, &f->mgr.text_cache,
                            &f->mgr.theme, f->mgr.font_id);

    f->tab.selected = CBX_ST_SET_TRIGGER;
    cbx_settings_tab_activate(&f->tab);

    assert_string_equal(f->tab.settings.overlay_trigger, "Select+A");

    cbx_settings_tab_edit_up(&f->tab);
    assert_string_equal(f->tab.settings.overlay_trigger, "Start+B");

    cbx_settings_tab_edit_up(&f->tab);
    assert_string_equal(f->tab.settings.overlay_trigger, "L3+R3");

    cbx_settings_tab_edit_up(&f->tab);
    assert_string_equal(f->tab.settings.overlay_trigger, "Select+A");

    cbx_settings_tab_confirm_edit(&f->tab);
}

/* --- Cancel edit test --------------------------------------------------- */

/* Cancel edit reverts changes. */
static void test_cancel_edit_reverts(void **state)
{
    st_fixture *f = FIX(state);
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_SETTINGS];
    cbx_settings_tab_init(&f->tab, panel, &f->mgr.text_cache,
                            &f->mgr.theme, f->mgr.font_id);

    /* Save initial settings to disk. */
    cbx_settings_tab_save(&f->tab);
    char orig_theme[64];
    strncpy(orig_theme, f->tab.settings.theme, sizeof(orig_theme) - 1);
    orig_theme[sizeof(orig_theme) - 1] = '\0';

    /* Edit theme. */
    f->tab.selected = CBX_ST_SET_THEME;
    cbx_settings_tab_activate(&f->tab);
    cbx_settings_tab_edit_up(&f->tab);
    assert_string_not_equal(f->tab.settings.theme, orig_theme);

    /* Cancel reverts. */
    cbx_settings_tab_cancel_edit(&f->tab);
    assert_int_equal(cbx_settings_tab_mode(&f->tab), CBX_ST_MODE_LIST);
    assert_string_equal(f->tab.settings.theme, orig_theme);
}

/* --- Save test ---------------------------------------------------------- */

/* Save writes settings to disk. */
static void test_save_writes_settings(void **state)
{
    st_fixture *f = FIX(state);
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_SETTINGS];
    cbx_settings_tab_init(&f->tab, panel, &f->mgr.text_cache,
                            &f->mgr.theme, f->mgr.font_id);

    /* Modify a setting. */
    f->tab.settings.launch_at_boot = false;
    strncpy(f->tab.settings.theme, "dark", sizeof(f->tab.settings.theme) - 1);

    /* Select save and activate. */
    f->tab.selected = CBX_ST_SET_SAVE;
    int rc = cbx_settings_tab_activate(&f->tab);
    assert_int_equal(rc, 0);

    /* Verify file was written. */
    char path[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/.config/controller-box/settings.yaml", f->tmp);
    struct stat st;
    assert_int_equal(stat(path, &st), 0);

    /* Reload and verify. */
    cbx_settings loaded;
    int rc2 = cbx_settings_load(&loaded);
    assert_int_equal(rc2, 0);
    assert_false(loaded.launch_at_boot);
    assert_string_equal(loaded.theme, "dark");
}

/* Save via function directly. */
static void test_save_direct(void **state)
{
    st_fixture *f = FIX(state);
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_SETTINGS];
    cbx_settings_tab_init(&f->tab, panel, &f->mgr.text_cache,
                            &f->mgr.theme, f->mgr.font_id);

    f->tab.settings.overlay_opacity = 0.50;
    int rc = cbx_settings_tab_save(&f->tab);
    assert_int_equal(rc, 0);

    cbx_settings loaded;
    cbx_settings_load(&loaded);
    assert_float_equal(loaded.overlay_opacity, 0.50, 0.001);
}

/* --- Accessor tests ----------------------------------------------------- */

/* Accessors null-safe. */
static void test_accessors_null_safe(void **state)
{
    (void)state;
    assert_null(cbx_settings_tab_settings(NULL));
    assert_int_equal(cbx_settings_tab_mode(NULL), CBX_ST_MODE_LIST);
    assert_int_equal(cbx_settings_tab_selected(NULL), 0);
    assert_null(cbx_settings_tab_status(NULL));
    assert_int_equal(cbx_settings_tab_setting_count(NULL), CBX_ST_SET_COUNT);
}

/* --- Icon overrides test ------------------------------------------------ */

/* Icon overrides: set, lookup, remove. */
static void test_icon_overrides(void **state)
{
    (void)state;
    cbx_settings s;
    cbx_settings_defaults(&s);

    /* Initially no overrides. */
    assert_int_equal(s.icon_override_count, 0);
    assert_null(cbx_settings_icon_override(&s, "xb360"));

    /* Set an override. */
    int rc = cbx_settings_set_icon_override(&s, "xb360", "my-xbox-icon");
    assert_int_equal(rc, 0);
    assert_int_equal(s.icon_override_count, 1);
    assert_string_equal(cbx_settings_icon_override(&s, "xb360"), "my-xbox-icon");

    /* Update existing override. */
    rc = cbx_settings_set_icon_override(&s, "xb360", "custom-xbox");
    assert_int_equal(rc, 0);
    assert_int_equal(s.icon_override_count, 1);  /* still 1, not 2 */
    assert_string_equal(cbx_settings_icon_override(&s, "xb360"), "custom-xbox");

    /* Add another. */
    rc = cbx_settings_set_icon_override(&s, "ds5", "custom-ds5");
    assert_int_equal(rc, 0);
    assert_int_equal(s.icon_override_count, 2);

    /* Remove an override. */
    rc = cbx_settings_remove_icon_override(&s, "xb360");
    assert_int_equal(rc, 0);
    assert_int_equal(s.icon_override_count, 1);
    assert_null(cbx_settings_icon_override(&s, "xb360"));
    assert_string_equal(cbx_settings_icon_override(&s, "ds5"), "custom-ds5");

    /* Remove non-existent. */
    rc = cbx_settings_remove_icon_override(&s, "xb360");
    assert_int_equal(rc, -ENOENT);

    /* NULL args. */
    assert_int_equal(cbx_settings_set_icon_override(NULL, "x", "y"), -EINVAL);
    assert_int_equal(cbx_settings_set_icon_override(&s, NULL, "y"), -EINVAL);
    assert_int_equal(cbx_settings_remove_icon_override(NULL, "x"), -EINVAL);
}

/* Icon overrides round-trip save/load. */
static void test_icon_overrides_round_trip(void **state)
{
    (void)state;
    /* setup_home is called via cmocka fixture, but we can test directly. */
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "/tmp/cbx_st_ovr_%d", (int)getpid());
    char cmd[PATH_MAX * 2 + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp);
    int r0 = system(cmd);
    (void)r0;
    mkdir(tmp, 0700);
    setenv("HOME", tmp, 1);
    unsetenv("XDG_CONFIG_HOME");

    cbx_settings s;
    cbx_settings_defaults(&s);
    cbx_settings_set_icon_override(&s, "xb360", "my-xbox");
    cbx_settings_set_icon_override(&s, "ds5", "my-ds5");

    int rc = cbx_settings_save(&s);
    assert_int_equal(rc, 0);

    cbx_settings loaded;
    rc = cbx_settings_load(&loaded);
    assert_int_equal(rc, 0);
    assert_int_equal(loaded.icon_override_count, 2);
    assert_string_equal(cbx_settings_icon_override(&loaded, "xb360"), "my-xbox");
    assert_string_equal(cbx_settings_icon_override(&loaded, "ds5"), "my-ds5");

    /* Validate passes with overrides. */
    assert_int_equal(cbx_settings_validate(&loaded), 0);

    /* Cleanup. */
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp);
    int r = system(cmd);
    (void)r;
    unsetenv("HOME");
}

/* Icon overrides full (ENOSPC). */
static void test_icon_overrides_full(void **state)
{
    (void)state;
    cbx_settings s;
    cbx_settings_defaults(&s);

    char type[32];
    for (int i = 0; i < CBX_MAX_ICON_OVERRIDES; i++) {
        snprintf(type, sizeof(type), "type%d", i);
        int rc = cbx_settings_set_icon_override(&s, type, "icon");
        assert_int_equal(rc, 0);
    }
    assert_int_equal(s.icon_override_count, CBX_MAX_ICON_OVERRIDES);

    /* Adding one more should fail. */
    int rc = cbx_settings_set_icon_override(&s, "overflow", "icon");
    assert_int_equal(rc, -ENOSPC);
}

/* Validate rejects empty icon override entries. */
static void test_validate_rejects_empty_icon_override(void **state)
{
    (void)state;
    cbx_settings s;
    cbx_settings_defaults(&s);

    /* Manually set an invalid override (empty icon). */
    s.icon_override_count = 1;
    s.icon_overrides[0].type[0] = 'x';
    s.icon_overrides[0].type[1] = '\0';
    s.icon_overrides[0].icon[0] = '\0';  /* empty icon */
    assert_int_equal(cbx_settings_validate(&s), -EINVAL);

    /* Fix it. */
    strncpy(s.icon_overrides[0].icon, "valid", sizeof(s.icon_overrides[0].icon) - 1);
    assert_int_equal(cbx_settings_validate(&s), 0);

    /* Empty type. */
    s.icon_overrides[0].type[0] = '\0';
    assert_int_equal(cbx_settings_validate(&s), -EINVAL);
}

/* --- main ---------------------------------------------------------------- */

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_init_populates_panel,
            st_setup, st_teardown),
        cmocka_unit_test_setup_teardown(test_init_loads_defaults,
            st_setup, st_teardown),
        cmocka_unit_test(test_init_null_args),
        cmocka_unit_test(test_shutdown_null_safe),
        cmocka_unit_test_setup_teardown(test_shutdown_removes_children,
            st_setup, st_teardown),
        cmocka_unit_test_setup_teardown(test_move_down_wrap,
            st_setup, st_teardown),
        cmocka_unit_test_setup_teardown(test_move_up_wrap,
            st_setup, st_teardown),
        cmocka_unit_test_setup_teardown(test_toggle_launch_boot,
            st_setup, st_teardown),
        cmocka_unit_test_setup_teardown(test_edit_theme,
            st_setup, st_teardown),
        cmocka_unit_test_setup_teardown(test_edit_opacity,
            st_setup, st_teardown),
        cmocka_unit_test_setup_teardown(test_edit_vc_count,
            st_setup, st_teardown),
        cmocka_unit_test_setup_teardown(test_edit_vc_type,
            st_setup, st_teardown),
        cmocka_unit_test_setup_teardown(test_edit_trigger,
            st_setup, st_teardown),
        cmocka_unit_test_setup_teardown(test_cancel_edit_reverts,
            st_setup, st_teardown),
        cmocka_unit_test_setup_teardown(test_save_writes_settings,
            st_setup, st_teardown),
        cmocka_unit_test_setup_teardown(test_save_direct,
            st_setup, st_teardown),
        cmocka_unit_test(test_accessors_null_safe),
        cmocka_unit_test(test_icon_overrides),
        cmocka_unit_test(test_icon_overrides_round_trip),
        cmocka_unit_test(test_icon_overrides_full),
        cmocka_unit_test(test_validate_rejects_empty_icon_override),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}