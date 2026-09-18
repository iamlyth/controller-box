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

    /* Init manager (creates window, renderer, text cache, panels, and
     * now initialises all three tab modules).  Shut down the manager-owned
     * settings tab so these tests can re-initialise it. */
    int rc = cbx_manager_init(&f->mgr, NULL);
    assert_int_equal(rc, 0);
    cbx_settings_tab_shutdown(cbx_manager_settings_tab(&f->mgr));

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

/* Move down wraps around at the dynamic row count for the minimum (1-slot),
 * default (4-slot), and maximum (16-slot / 23-row) layouts.  Exercising the
 * non-default layouts proves the bound is not the fixed enum count. */
static void test_move_down_wrap(void **state)
{
    st_fixture *f = FIX(state);
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_SETTINGS];
    cbx_settings_tab_init(&f->tab, panel, &f->mgr.text_cache,
                            &f->mgr.theme, f->mgr.font_id);

    const int slot_counts[] = { 1, 4, CBX_MAX_CONTROLLERS };
    for (size_t c = 0; c < sizeof(slot_counts) / sizeof(slot_counts[0]); c++) {
        f->tab.settings.virtual_controllers.count = slot_counts[c];
        assert_int_equal(cbx_settings_tab_refresh(&f->tab), 0);
        int count = cbx_settings_tab_setting_count(&f->tab);

        f->tab.selected = 0;
        for (int i = 1; i < count; i++) {
            cbx_settings_tab_move_down(&f->tab);
            assert_int_equal(cbx_settings_tab_selected(&f->tab), i);
            assert_int_equal(cbx_list_get_selected(&f->tab.settings_list), i);
        }
        /* One more wraps to 0. */
        cbx_settings_tab_move_down(&f->tab);
        assert_int_equal(cbx_settings_tab_selected(&f->tab), 0);
        assert_int_equal(cbx_list_get_selected(&f->tab.settings_list), 0);
    }
}

/* Move up wraps around at the dynamic row count for the minimum (1-slot),
 * default (4-slot), and maximum (16-slot / 23-row) layouts. */
static void test_move_up_wrap(void **state)
{
    st_fixture *f = FIX(state);
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_SETTINGS];
    cbx_settings_tab_init(&f->tab, panel, &f->mgr.text_cache,
                            &f->mgr.theme, f->mgr.font_id);

    const int slot_counts[] = { 1, 4, CBX_MAX_CONTROLLERS };
    for (size_t c = 0; c < sizeof(slot_counts) / sizeof(slot_counts[0]); c++) {
        f->tab.settings.virtual_controllers.count = slot_counts[c];
        assert_int_equal(cbx_settings_tab_refresh(&f->tab), 0);
        int count = cbx_settings_tab_setting_count(&f->tab);

        f->tab.selected = 0;
        cbx_settings_tab_move_up(&f->tab);
        assert_int_equal(cbx_settings_tab_selected(&f->tab), count - 1);
        assert_int_equal(cbx_list_get_selected(&f->tab.settings_list),
                         count - 1);
    }
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

/* Theme is fixed to the one implemented palette: the row must not enter an
 * inert edit mode and every advertised theme must actually be implemented
 * (SPEC §5.5, §13). */
static void test_theme_fixed_to_default(void **state)
{
    st_fixture *f = FIX(state);
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_SETTINGS];
    cbx_settings_tab_init(&f->tab, panel, &f->mgr.text_cache,
                            &f->mgr.theme, f->mgr.font_id);

    for (int i = 0; cbx_st_themes[i]; i++)
        assert_true(cbx_theme_is_known(cbx_st_themes[i]));

    assert_false(cbx_settings_tab_row_editable(&f->tab, CBX_ST_SET_THEME));
    f->tab.selected = CBX_ST_SET_THEME;
    assert_int_equal(cbx_settings_tab_activate(&f->tab), 0);
    assert_int_equal(cbx_settings_tab_mode(&f->tab), CBX_ST_MODE_LIST);
    assert_string_equal(f->tab.settings.theme, "default");
    assert_non_null(cbx_settings_tab_status(&f->tab));
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

/* Edit icon override: cycle through presets. */
static void test_edit_icon_override(void **state)
{
    st_fixture *f = FIX(state);
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_SETTINGS];
    cbx_settings_tab_init(&f->tab, panel, &f->mgr.text_cache,
                            &f->mgr.theme, f->mgr.font_id);

    /* Initially no overrides. */
    assert_int_equal(f->tab.settings.icon_override_count, 0);

    f->tab.selected = CBX_ST_SET_ICON_OVERRIDE;
    cbx_settings_tab_activate(&f->tab);
    assert_int_equal(cbx_settings_tab_mode(&f->tab), CBX_ST_MODE_EDIT);

    /* Cycle up: None -> ds5 -> cc-xbox-360. */
    cbx_settings_tab_edit_up(&f->tab);
    assert_int_equal(f->tab.settings.icon_override_count, 1);
    assert_string_equal(cbx_settings_icon_override(&f->tab.settings, "ds5"),
                         "cc-xbox-360");

    /* Cycle up: ds5 -> xb360 -> cc-ps5. */
    cbx_settings_tab_edit_up(&f->tab);
    assert_int_equal(f->tab.settings.icon_override_count, 1);
    assert_string_equal(cbx_settings_icon_override(&f->tab.settings, "xb360"),
                         "cc-ps5");

    /* Cycle down: back to ds5 -> cc-xbox-360. */
    cbx_settings_tab_edit_down(&f->tab);
    assert_string_equal(cbx_settings_icon_override(&f->tab.settings, "ds5"),
                         "cc-xbox-360");

    /* Cycle down to None: clears overrides. */
    cbx_settings_tab_edit_down(&f->tab);
    assert_int_equal(f->tab.settings.icon_override_count, 0);

    /* Confirm edit. */
    cbx_settings_tab_confirm_edit(&f->tab);
    assert_int_equal(cbx_settings_tab_mode(&f->tab), CBX_ST_MODE_LIST);
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
    double orig_opacity = f->tab.settings.overlay_opacity;

    /* Edit opacity. */
    f->tab.selected = CBX_ST_SET_OPACITY;
    cbx_settings_tab_activate(&f->tab);
    cbx_settings_tab_edit_up(&f->tab);
    assert_float_equal(f->tab.settings.overlay_opacity,
                       orig_opacity + 0.05, 0.001);

    /* Cancel reverts. */
    cbx_settings_tab_cancel_edit(&f->tab);
    assert_int_equal(cbx_settings_tab_mode(&f->tab), CBX_ST_MODE_LIST);
    assert_float_equal(f->tab.settings.overlay_opacity, orig_opacity, 0.001);
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
    assert_int_equal(cbx_settings_tab_setting_count(NULL), 0);
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

/* ------------------------------------------------------------------ */
/*  Task 9 — every startup slot, theme honesty, persistence/restart    */
/* ------------------------------------------------------------------ */

/* Growing/shrinking the count changes the number of type rows and the
 * trailing rows move with it; every configured slot is reachable. */
static void test_type_rows_track_count_growth_and_shrink(void **state)
{
    st_fixture *f = FIX(state);
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_SETTINGS];
    cbx_settings_tab_init(&f->tab, panel, &f->mgr.text_cache,
                            &f->mgr.theme, f->mgr.font_id);

    /* Default count = 4 ⇒ 4 type rows, trigger/save immediately after. */
    assert_int_equal(cbx_settings_tab_type_row_count(&f->tab), 4);
    assert_int_equal(cbx_settings_tab_trigger_row(&f->tab), 8);
    assert_int_equal(cbx_settings_tab_icon_override_row(&f->tab), 9);
    assert_int_equal(cbx_settings_tab_save_row(&f->tab), 10);

    /* Grow to slot 5 through the production edit path. */
    f->tab.selected = CBX_ST_SET_VC_COUNT;
    assert_int_equal(cbx_settings_tab_activate(&f->tab), 0);
    cbx_settings_tab_edit_up(&f->tab);
    cbx_settings_tab_confirm_edit(&f->tab);
    assert_int_equal(f->tab.settings.virtual_controllers.count, 5);
    assert_int_equal(cbx_settings_tab_type_row_count(&f->tab), 5);
    assert_int_equal(cbx_settings_tab_row_for_type(&f->tab, 4), 8);
    assert_int_equal(cbx_settings_tab_trigger_row(&f->tab), 9);
    assert_int_equal(cbx_settings_tab_save_row(&f->tab), 11);
    assert_int_equal(cbx_list_item_count(&f->tab.settings_list), 12);

    /* Shrink back to 4 through the production edit path. */
    f->tab.selected = CBX_ST_SET_VC_COUNT;
    cbx_settings_tab_activate(&f->tab);
    cbx_settings_tab_edit_down(&f->tab);
    cbx_settings_tab_confirm_edit(&f->tab);
    assert_int_equal(f->tab.settings.virtual_controllers.count, 4);
    assert_int_equal(cbx_settings_tab_type_row_count(&f->tab), 4);
    assert_int_equal(cbx_settings_tab_save_row(&f->tab), 10);
}

/* Slot 5 and slot 16 (the maximum) are exposed as editable type rows and
 * the large list scrolls rather than clipping them. */
static void test_all_slots_exposed_and_scrollable(void **state)
{
    st_fixture *f = FIX(state);
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_SETTINGS];
    cbx_settings_tab_init(&f->tab, panel, &f->mgr.text_cache,
                            &f->mgr.theme, f->mgr.font_id);

    f->tab.settings.virtual_controllers.count = CBX_MAX_CONTROLLERS;
    for (int i = 0; i < CBX_MAX_CONTROLLERS; i++)
        snprintf(f->tab.settings.virtual_controllers.types[i],
                 CBX_MAX_TYPE_LEN, "xb360");
    assert_int_equal(cbx_settings_tab_refresh(&f->tab), 0);

    assert_int_equal(cbx_settings_tab_type_row_count(&f->tab),
                     CBX_MAX_CONTROLLERS);
    assert_int_equal(cbx_settings_tab_setting_count(&f->tab),
                     CBX_ST_BASE_ROWS + CBX_MAX_CONTROLLERS +
                     CBX_ST_TRAILING_ROWS);
    assert_int_equal(cbx_list_item_count(&f->tab.settings_list),
                     cbx_settings_tab_setting_count(&f->tab));

    for (int slot = 0; slot < CBX_MAX_CONTROLLERS; slot++) {
        int row = cbx_settings_tab_row_for_type(&f->tab, slot);
        assert_int_equal(row, CBX_ST_BASE_ROWS + slot);
        assert_int_equal(cbx_settings_tab_type_slot(&f->tab, row), slot);
        assert_true(cbx_settings_tab_row_editable(&f->tab, row));
    }

    /* Slot 5 = row 9, slot 16 = row 19. */
    assert_int_equal(cbx_settings_tab_row_for_type(&f->tab, 4), 8);
    assert_int_equal(cbx_settings_tab_row_for_type(&f->tab, 15), 19);

    /* The final save row must be reachable by scrolling the list, and the
     * selected row must land inside the visible window. */
    int save_row = cbx_settings_tab_save_row(&f->tab);
    assert_int_equal(save_row, 22);
    cbx_list_set_selected(&f->tab.settings_list, save_row);
    assert_int_equal(cbx_list_get_selected(&f->tab.settings_list), save_row);
    int vis = f->tab.settings_list.visible_count;
    int off = f->tab.settings_list.scroll_offset;
    assert_true(vis >= 1);
    assert_true(off <= save_row && save_row < off + vis);
}

/* Edit slot 5 and slot 16 through the production activate/edit path, save,
 * restart, and verify both values persisted. */
static void test_slot5_and_slot16_edit_persist_restart(void **state)
{
    st_fixture *f = FIX(state);
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_SETTINGS];
    cbx_settings_tab_init(&f->tab, panel, &f->mgr.text_cache,
                            &f->mgr.theme, f->mgr.font_id);

    /* Grow to the maximum through the production edit path. */
    f->tab.selected = CBX_ST_SET_VC_COUNT;
    cbx_settings_tab_activate(&f->tab);
    while (f->tab.settings.virtual_controllers.count < CBX_MAX_CONTROLLERS)
        cbx_settings_tab_edit_up(&f->tab);
    cbx_settings_tab_confirm_edit(&f->tab);
    assert_int_equal(f->tab.settings.virtual_controllers.count,
                     CBX_MAX_CONTROLLERS);

    char slot5_before[CBX_MAX_TYPE_LEN];
    char slot16_before[CBX_MAX_TYPE_LEN];
    snprintf(slot5_before, sizeof(slot5_before), "%s",
             f->tab.settings.virtual_controllers.types[4]);
    snprintf(slot16_before, sizeof(slot16_before), "%s",
             f->tab.settings.virtual_controllers.types[15]);

    /* Slot 5 (list row 8). */
    f->tab.selected = cbx_settings_tab_row_for_type(&f->tab, 4);
    assert_int_equal(cbx_settings_tab_activate(&f->tab), 0);
    assert_int_equal(cbx_settings_tab_mode(&f->tab), CBX_ST_MODE_EDIT);
    cbx_settings_tab_edit_up(&f->tab);
    assert_string_not_equal(f->tab.settings.virtual_controllers.types[4],
                            slot5_before);
    cbx_settings_tab_confirm_edit(&f->tab);

    /* Slot 16 (list row 19). */
    f->tab.selected = cbx_settings_tab_row_for_type(&f->tab, 15);
    assert_int_equal(cbx_settings_tab_activate(&f->tab), 0);
    assert_int_equal(cbx_settings_tab_mode(&f->tab), CBX_ST_MODE_EDIT);
    cbx_settings_tab_edit_up(&f->tab);
    assert_string_not_equal(f->tab.settings.virtual_controllers.types[15],
                            slot16_before);
    cbx_settings_tab_confirm_edit(&f->tab);

    char slot5_after[CBX_MAX_TYPE_LEN];
    char slot16_after[CBX_MAX_TYPE_LEN];
    snprintf(slot5_after, sizeof(slot5_after), "%s",
             f->tab.settings.virtual_controllers.types[4]);
    snprintf(slot16_after, sizeof(slot16_after), "%s",
             f->tab.settings.virtual_controllers.types[15]);

    assert_int_equal(cbx_settings_tab_save(&f->tab), 0);

    /* Restart: discard the working copy and reload from disk. */
    cbx_settings_tab_shutdown(&f->tab);
    assert_int_equal(cbx_settings_tab_init(&f->tab, panel, &f->mgr.text_cache,
                                            &f->mgr.theme, f->mgr.font_id), 0);
    assert_int_equal(f->tab.settings.virtual_controllers.count,
                     CBX_MAX_CONTROLLERS);
    assert_string_equal(f->tab.settings.virtual_controllers.types[4],
                        slot5_after);
    assert_string_equal(f->tab.settings.virtual_controllers.types[15],
                        slot16_after);
}

/* All remaining settings and an icon override round-trip through save and a
 * simulated process restart. */
static void test_remaining_settings_icon_override_persist_restart(void **state)
{
    st_fixture *f = FIX(state);
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_SETTINGS];
    cbx_settings_tab_init(&f->tab, panel, &f->mgr.text_cache,
                            &f->mgr.theme, f->mgr.font_id);

    bool boot = f->tab.settings.launch_at_boot;
    double opacity = f->tab.settings.overlay_opacity;
    char trigger[CBX_MAX_STR_LEN];
    snprintf(trigger, sizeof(trigger), "%s", f->tab.settings.overlay_trigger);

    /* Toggle launch at boot. */
    f->tab.selected = CBX_ST_SET_LAUNCH_BOOT;
    assert_int_equal(cbx_settings_tab_activate(&f->tab), 0);
    assert_int_equal(f->tab.settings.launch_at_boot, !boot);

    /* Opacity +0.05. */
    f->tab.selected = CBX_ST_SET_OPACITY;
    cbx_settings_tab_activate(&f->tab);
    cbx_settings_tab_edit_up(&f->tab);
    cbx_settings_tab_confirm_edit(&f->tab);

    /* Cycle the trigger to the next combo. */
    f->tab.selected = cbx_settings_tab_trigger_row(&f->tab);
    cbx_settings_tab_activate(&f->tab);
    cbx_settings_tab_edit_up(&f->tab);
    cbx_settings_tab_confirm_edit(&f->tab);
    char new_trigger[CBX_MAX_STR_LEN];
    snprintf(new_trigger, sizeof(new_trigger), "%s",
             f->tab.settings.overlay_trigger);
    assert_string_not_equal(new_trigger, trigger);

    /* Icon override: first preset. */
    f->tab.selected = cbx_settings_tab_icon_override_row(&f->tab);
    cbx_settings_tab_activate(&f->tab);
    cbx_settings_tab_edit_up(&f->tab);
    assert_int_equal(f->tab.settings.icon_override_count, 1);
    char icon_type[CBX_ICON_OVR_TYPE_LEN];
    char icon_name[CBX_ICON_OVR_ICON_LEN];
    snprintf(icon_type, sizeof(icon_type), "%s",
             f->tab.settings.icon_overrides[0].type);
    snprintf(icon_name, sizeof(icon_name), "%s",
             f->tab.settings.icon_overrides[0].icon);
    cbx_settings_tab_confirm_edit(&f->tab);

    assert_int_equal(cbx_settings_tab_save(&f->tab), 0);

    /* Restart. */
    cbx_settings_tab_shutdown(&f->tab);
    assert_int_equal(cbx_settings_tab_init(&f->tab, panel, &f->mgr.text_cache,
                                            &f->mgr.theme, f->mgr.font_id), 0);
    const cbx_settings *s = cbx_settings_tab_settings(&f->tab);
    assert_int_equal(s->launch_at_boot, !boot);
    assert_float_equal(s->overlay_opacity, opacity + 0.05, 0.001);
    assert_string_equal(s->overlay_trigger, new_trigger);
    assert_int_equal(s->icon_override_count, 1);
    assert_string_equal(s->icon_overrides[0].type, icon_type);
    assert_string_equal(s->icon_overrides[0].icon, icon_name);
}

/* ------------------------------------------------------------------ */
/*  Production-dispatch tests (through cbx_manager_handle_event)        */
/* ------------------------------------------------------------------ */

static bool send_key_dn(cbx_manager *mgr, SDL_Keycode sym)
{
    SDL_Event ev = {0};
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = sym;
    return cbx_manager_handle_event(mgr, &ev);
}

static bool send_key_up(cbx_manager *mgr, SDL_Keycode sym)
{
    SDL_Event ev = {0};
    ev.type = SDL_KEYUP;
    ev.key.keysym.sym = sym;
    return cbx_manager_handle_event(mgr, &ev);
}

static void
test_settings_activate_via_dispatch(void **state)
{
    (void)state;
    /* Use a temp HOME so settings load/write are isolated. */
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "/tmp/cbx_st_disp_%d", (int)getpid());
    setenv("HOME", tmp, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");

    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    /* Switch to Settings tab (tab 2). */
    send_key_dn(&mgr, SDLK_RIGHT);
    send_key_dn(&mgr, SDLK_RIGHT);
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_SETTINGS);

    cbx_settings_tab *st = cbx_manager_settings_tab(&mgr);
    assert_non_null(st);

    /* Navigate down from tabbar to the settings list. */
    send_key_dn(&mgr, SDLK_DOWN);
    assert_true(st->settings_list.base.focused);

    /* The first setting is launch_at_boot. Toggle it with A. */
    bool initial = st->settings.launch_at_boot;
    assert_true(send_key_dn(&mgr, SDLK_a));
    assert_true(send_key_up(&mgr, SDLK_a));
    assert_int_equal(st->settings.launch_at_boot, !initial);

    /* Toggle again to restore. */
    assert_true(send_key_dn(&mgr, SDLK_a));
    assert_true(send_key_up(&mgr, SDLK_a));
    assert_int_equal(st->settings.launch_at_boot, initial);

    cbx_manager_shutdown(&mgr);

    /* Clean up. */
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmp);
    (void)!system(cmd);
}

static void
test_settings_edit_via_dispatch(void **state)
{
    (void)state;
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "/tmp/cbx_st_edit_%d", (int)getpid());
    setenv("HOME", tmp, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");

    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    send_key_dn(&mgr, SDLK_RIGHT);
    send_key_dn(&mgr, SDLK_RIGHT);
    cbx_settings_tab *st = cbx_manager_settings_tab(&mgr);
    assert_non_null(st);

    /* Down to settings list. */
    send_key_dn(&mgr, SDLK_DOWN);

    /* Navigate to Overlay Opacity (index 2). */
    send_key_dn(&mgr, SDLK_DOWN);
    send_key_dn(&mgr, SDLK_DOWN);
    assert_int_equal(cbx_list_get_selected(&st->settings_list), CBX_ST_SET_OPACITY);

    /* A to enter edit mode. */
    send_key_dn(&mgr, SDLK_a);
    assert_true(send_key_up(&mgr, SDLK_a));
    assert_int_equal(st->mode, CBX_ST_MODE_EDIT);

    /* Up adjusts opacity forward. */
    double orig_opacity = st->settings.overlay_opacity;
    send_key_dn(&mgr, SDLK_UP);
    assert_float_equal(st->settings.overlay_opacity, orig_opacity + 0.05, 0.001);

    /* Down adjusts back. */
    send_key_dn(&mgr, SDLK_DOWN);
    assert_float_equal(st->settings.overlay_opacity, orig_opacity, 0.001);

    /* A confirms edit. */
    send_key_dn(&mgr, SDLK_a);
    assert_true(send_key_up(&mgr, SDLK_a));
    assert_int_equal(st->mode, CBX_ST_MODE_LIST);

    cbx_manager_shutdown(&mgr);

    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmp);
    (void)!system(cmd);
}

static void
test_settings_edit_cancel_via_dispatch(void **state)
{
    (void)state;
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "/tmp/cbx_st_cancel_%d", (int)getpid());
    setenv("HOME", tmp, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");

    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    send_key_dn(&mgr, SDLK_RIGHT);
    send_key_dn(&mgr, SDLK_RIGHT);
    cbx_settings_tab *st = cbx_manager_settings_tab(&mgr);

    send_key_dn(&mgr, SDLK_DOWN);
    send_key_dn(&mgr, SDLK_DOWN);  /* item 1 */
    send_key_dn(&mgr, SDLK_DOWN);  /* item 2 = opacity */

    /* Enter edit mode. */
    send_key_dn(&mgr, SDLK_a);
    send_key_up(&mgr, SDLK_a);
    assert_int_equal(st->mode, CBX_ST_MODE_EDIT);

    double orig_opacity = st->settings.overlay_opacity;

    /* Adjust opacity. */
    send_key_dn(&mgr, SDLK_UP);
    assert_float_equal(st->settings.overlay_opacity,
                       orig_opacity + 0.05, 0.001);

    /* B cancels edit — reverts from disk. */
    send_key_dn(&mgr, SDLK_b);
    assert_int_equal(st->mode, CBX_ST_MODE_LIST);
    assert_float_equal(st->settings.overlay_opacity, orig_opacity, 0.001);

    cbx_manager_shutdown(&mgr);

    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmp);
    (void)!system(cmd);
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
        cmocka_unit_test_setup_teardown(test_theme_fixed_to_default,
            st_setup, st_teardown),
        cmocka_unit_test_setup_teardown(test_edit_opacity,
            st_setup, st_teardown),
        cmocka_unit_test_setup_teardown(test_edit_vc_count,
            st_setup, st_teardown),
        cmocka_unit_test_setup_teardown(test_edit_vc_type,
            st_setup, st_teardown),
        cmocka_unit_test_setup_teardown(test_edit_trigger,
            st_setup, st_teardown),
        cmocka_unit_test_setup_teardown(test_edit_icon_override,
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
        cmocka_unit_test_setup_teardown(
            test_type_rows_track_count_growth_and_shrink,
            st_setup, st_teardown),
        cmocka_unit_test_setup_teardown(test_all_slots_exposed_and_scrollable,
            st_setup, st_teardown),
        cmocka_unit_test_setup_teardown(
            test_slot5_and_slot16_edit_persist_restart,
            st_setup, st_teardown),
        cmocka_unit_test_setup_teardown(
            test_remaining_settings_icon_override_persist_restart,
            st_setup, st_teardown),
        cmocka_unit_test(test_settings_activate_via_dispatch),
        cmocka_unit_test(test_settings_edit_via_dispatch),
        cmocka_unit_test(test_settings_edit_cancel_via_dispatch),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}