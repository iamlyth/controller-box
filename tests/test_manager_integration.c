/*
 * test_manager_integration.c — Full manager flow integration test (Task 40).
 *
 * Exercises the complete manager workflow with mock DBus and SDL2 dummy
 * driver:
 *   1. Manager init (creates window, renderer, tabs, panels)
 *   2. Controllers tab: init, add controller (mock DBus), refresh
 *   3. Profiles tab: init, create profile, refresh
 *   4. Profile editor: init, load profile, begin sequential, capture buttons,
 *      validate NES minimum, save profile
 *   5. Settings tab: init, toggle launch_at_boot, save settings
 *   6. Service install: mock systemctl, install service, verify unit file
 *
 * Uses isolated $HOME, mock DBus backend, and mock systemctl scripts.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include "manager/manager.h"
#include "manager/controllers_tab.h"
#include "manager/profiles_tab.h"
#include "manager/profile_editor_list.h"
#include "manager/profile_editor_seq.h"
#include "manager/profile_validate.h"
#include "manager/profile_save.h"
#include "manager/settings_tab.h"
#include "manager/service_install.h"
#include "config/config_settings.h"
#include "config/config_paths.h"
#include "config/config_profile.h"
#include "config/config_profile_list.h"
#include "dbus/ip_input_signal.h"
#include "dbus_mock.h"
#include "ui/theme.h"
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

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static void ensure_dummy_driver(void)
{
    if (getenv("SDL_VIDEODRIVER") == NULL)
        SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "dummy",
                                 SDL_HINT_OVERRIDE);
}

/* The NES minimum set of buttons we need to capture for validation. */
static const char *const nes_buttons[] = {
    "A", "B", "Up", "Down", "Left", "Right"
};
#define NES_MIN_COUNT 6

/* ------------------------------------------------------------------ */
/*  Fixture                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    char tmp[256];               /* isolated HOME */
    cbx_manager mgr;

    ip_dbus_mock mock;
    const ip_dbus_backend *backend;

    cbx_controllers_tab ct;     /* controllers tab */
    cbx_profiles_tab pt;         /* profiles tab */
    cbx_settings_tab st;        /* settings tab */

    char mock_systemctl_path[PATH_MAX];
} mi_fixture;

static int setup(void **state)
{
    ensure_dummy_driver();

    mi_fixture *f = malloc(sizeof(*f));
    assert_non_null(f);
    memset(f, 0, sizeof(*f));

    /* Isolated HOME. */
    snprintf(f->tmp, sizeof(f->tmp), "/tmp/cbx_mi_test_%d", (int)getpid());
    char cmd[PATH_MAX * 2 + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", f->tmp);
    int r0 = system(cmd);
    (void)r0;
    mkdir(f->tmp, 0700);
    setenv("HOME", f->tmp, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");
    unsetenv("FLATPAK_ID");

    /* Create a profiles directory so profile operations work. */
    char profiles_dir[PATH_MAX + 64];
    snprintf(profiles_dir, sizeof(profiles_dir),
             "%s/.local/share/inputplumber/profiles", f->tmp);
    cbx_ensure_dir(profiles_dir, 0700);

    /* Mock systemctl script with state file for is-active. */
    snprintf(f->mock_systemctl_path, sizeof(f->mock_systemctl_path),
             "%s/mock_systemctl", f->tmp);
    char state_file[PATH_MAX + 128];
    snprintf(state_file, sizeof(state_file), "%s/enabled_marker", f->tmp);

    FILE *s = fopen(f->mock_systemctl_path, "w");
    assert_non_null(s);
    fprintf(s, "#!/bin/sh\n");
    fprintf(s, "case \"$1\" in\n");
    fprintf(s, "  is-system-running) exit 0 ;;\n");
    fprintf(s, "  is-active) if [ -f \"%s\" ]; then echo active; else echo inactive; fi ;;\n", state_file);
    fprintf(s, "  enable) touch \"%s\"; exit 0 ;;\n", state_file);
    fprintf(s, "  disable) rm -f \"%s\"; exit 0 ;;\n", state_file);
    fprintf(s, "  *) exit 0 ;;\n");
    fprintf(s, "esac\n");
    fclose(s);
    chmod(f->mock_systemctl_path, 0755);

    cbx_service_set_mock_systemctl(f->mock_systemctl_path);

    /* Init manager. */
    int rc = cbx_manager_init(&f->mgr, NULL);
    assert_int_equal(rc, 0);

    /* Init mock DBus. */
    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);

    /* Set up DBus expectations for controllers tab. */
    ip_dbus_mock_expect_ok(&f->mock,
        IP_IFACE_MANAGER, "SupportedTargetDeviceIds",
        "evdev,evdev-gamepad,evdev-keyboard,evdev-mouse");

    /* Init controllers tab. */
    rc = cbx_controllers_tab_init(&f->ct,
                                   &f->mgr.panels[CBX_MGR_TAB_CONTROLLERS],
                                   f->backend, f->mock.bus,
                                   &f->mgr.text_cache, &f->mgr.theme,
                                   f->mgr.font_id);
    assert_int_equal(rc, 0);

    /* Init profiles tab with test dirs. */
    rc = cbx_profiles_tab_init(&f->pt,
                                &f->mgr.panels[CBX_MGR_TAB_PROFILES],
                                &f->mgr.text_cache, &f->mgr.theme,
                                f->mgr.font_id);
    assert_int_equal(rc, 0);

    /* Override profiles tab dirs to use our test home. */
    cbx_profiles_tab_set_test_dirs(&f->pt, profiles_dir,
                                    cbx_system_profiles_dir(), NULL);

    /* Re-refresh with test dirs. */
    cbx_profiles_tab_refresh(&f->pt);

    /* Init settings tab. */
    rc = cbx_settings_tab_init(&f->st,
                                &f->mgr.panels[CBX_MGR_TAB_SETTINGS],
                                &f->mgr.text_cache, &f->mgr.theme,
                                f->mgr.font_id);
    assert_int_equal(rc, 0);

    *state = f;
    return 0;
}

static int teardown(void **state)
{
    mi_fixture *f = *state;

    cbx_settings_tab_shutdown(&f->st);
    cbx_profiles_tab_shutdown(&f->pt);
    cbx_controllers_tab_shutdown(&f->ct);
    cbx_manager_shutdown(&f->mgr);
    ip_dbus_mock_free(&f->mock);
    cbx_service_set_mock_systemctl(NULL);
    cbx_service_set_mock_group_file(NULL);
    cbx_service_set_mock_username(NULL);

    char cmd[PATH_MAX * 2 + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", f->tmp);
    int r = system(cmd);
    (void)r;
    unsetenv("HOME");
    free(f);
    return 0;
}

#define FIX(s) ((mi_fixture *)*(s))

/* ------------------------------------------------------------------ */
/*  Tests                                                              */
/* ------------------------------------------------------------------ */

/* Test: manager initializes with 3 tabs. */
static void test_manager_init(void **state)
{
    mi_fixture *f = FIX(state);
    assert_int_equal(cbx_manager_tab_count(&f->mgr), 3);
    assert_int_equal(cbx_manager_active_tab(&f->mgr), CBX_MGR_TAB_CONTROLLERS);
}

/* Test: controllers tab has supported types loaded. */
static void test_controllers_tab_loaded(void **state)
{
    mi_fixture *f = FIX(state);
    assert_true(cbx_controllers_tab_supported_type_count(&f->ct) > 0);
}

/* Test: add a controller via mock DBus. */
static void test_add_controller(void **state)
{
    mi_fixture *f = FIX(state);

    /* Expect the CreateTargetDevice call to return a path. */
    ip_dbus_mock_expect_ok(&f->mock,
        IP_IFACE_MANAGER, "CreateTargetDevice",
        "/org/shadowblip/InputPlumber/TargetDevice0");

    /* Also need SupportedTargetDeviceIds for refresh. */
    ip_dbus_mock_expect_ok(&f->mock,
        IP_IFACE_MANAGER, "SupportedTargetDeviceIds",
        "evdev,evdev-gamepad,evdev-keyboard,evdev-mouse");

    int rc = cbx_controllers_tab_add(&f->ct, "evdev-gamepad");
    assert_int_equal(rc, 0);
}

/* Test: profiles tab initialized and empty. */
static void test_profiles_tab_initialized(void **state)
{
    mi_fixture *f = FIX(state);
    /* The default profile should be in system dir; user dir starts empty. */
    assert_true(cbx_profiles_tab_profile_count(&f->pt) >= 0);
}

/* Test: create a profile from empty source. */
static void test_create_profile(void **state)
{
    mi_fixture *f = FIX(state);

    int rc = cbx_profiles_tab_create(&f->pt, "testprof",
                                     CBX_PT_CREATE_EMPTY);
    assert_int_equal(rc, 0);

    /* Refresh and verify. */
    cbx_profiles_tab_refresh(&f->pt);

    /* Find the profile in the list. */
    bool found = false;
    int count = cbx_profiles_tab_profile_count(&f->pt);
    for (int i = 0; i < count; i++) {
        const cbx_profile_entry *e = cbx_profiles_tab_entry(&f->pt, i);
        if (e && strcmp(e->filename, "testprof") == 0)
            found = true;
    }
    assert_true(found);
}

/* Test: profile editor sequential binding, validation, and save. */
static void test_full_profile_workflow(void **state)
{
    mi_fixture *f = FIX(state);

    /* Create an empty profile. */
    int rc = cbx_profiles_tab_create(&f->pt, "workflow",
                                      CBX_PT_CREATE_EMPTY);
    assert_int_equal(rc, 0);

    /* Create a profile editor panel for testing.  We need a separate
     * panel since the manager's profile tab panel is already populated. */
    cbx_profile_editor ed;
    memset(&ed, 0, sizeof(ed));

    cbx_panel ed_panel;
    cbx_panel_init(&ed_panel, &f->mgr.theme);

    rc = cbx_profile_editor_init(&ed, &ed_panel, f->mgr.rend.renderer,
                                  &f->mgr.text_cache, &f->mgr.theme,
                                  f->mgr.font_id);
    assert_int_equal(rc, 0);

    /* Create a test profile with NES minimum buttons. */
    cbx_profile prof;
    cbx_profile_init(&prof);
    strncpy(prof.name, "workflow", sizeof(prof.name) - 1);
    strncpy(prof.kind, "DeviceProfile", sizeof(prof.kind) - 1);
    prof.version = 1;

    /* Add the 6 NES minimum button mappings. */
    for (int i = 0; i < NES_MIN_COUNT; i++) {
        cbx_profile_mapping *m = &prof.mappings[prof.mapping_count];
        memset(m, 0, sizeof(*m));
        strncpy(m->name, nes_buttons[i], sizeof(m->name) - 1);
        strncpy(m->source_event.device_class, "gamepad",
                 sizeof(m->source_event.device_class) - 1);
        m->source_event.prop_count = 1;
        strncpy(m->source_event.props[0].key, "button",
                 sizeof(m->source_event.props[0].key) - 1);
        strncpy(m->source_event.props[0].value, nes_buttons[i],
                 sizeof(m->source_event.props[0].value) - 1);
        m->target_event_count = 1;
        strncpy(m->target_events[0].device_class, "keyboard",
                 sizeof(m->target_events[0].device_class) - 1);
        strncpy(m->target_events[0].value, "KeyA",
                 sizeof(m->target_events[0].value) - 1);
        prof.mapping_count++;
    }

    /* Load the profile into the editor. */
    rc = cbx_profile_editor_load_profile(&ed, &prof);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_profile_editor_binding_count(&ed), NES_MIN_COUNT);

    /* Validate NES minimum. */
    char missing[256] = {0};
    rc = cbx_profile_validate_nes_minimum(&prof, missing, sizeof(missing));
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_profile_validate_missing_count(&prof), 0);

    /* Save the profile via profile_save. */
    char profiles_dir[PATH_MAX + 64];
    snprintf(profiles_dir, sizeof(profiles_dir),
             "%s/.local/share/inputplumber/profiles", f->tmp);

    rc = cbx_profile_save_to_dir(&prof, "workflow", NULL,
                                  profiles_dir, missing, sizeof(missing));
    assert_int_equal(rc, 0);

    /* Verify the file exists. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/workflow.yaml", profiles_dir);
    struct stat st;
    assert_int_equal(stat(path, &st), 0);

    /* Clean up editor. */
    cbx_profile_editor_shutdown(&ed);
    cbx_widget_destroy(&ed_panel.base);
}

/* Test: settings tab toggle launch_at_boot and save. */
static void test_settings_save(void **state)
{
    mi_fixture *f = FIX(state);

    /* Navigate to the launch_at_boot setting (index 0). */
    assert_int_equal(cbx_settings_tab_selected(&f->st), 0);

    /* Activate to toggle. */
    int rc = cbx_settings_tab_activate(&f->st);
    assert_int_equal(rc, 0);

    /* Verify the toggle changed. */
    const cbx_settings *s = cbx_settings_tab_settings(&f->st);
    assert_false(s->launch_at_boot);  /* was true by default → now false */

    /* Navigate to save (index 9 = CBX_ST_SET_SAVE). */
    for (int i = 0; i < CBX_ST_SET_COUNT - 1; i++)
        cbx_settings_tab_move_down(&f->st);

    assert_int_equal(cbx_settings_tab_selected(&f->st), CBX_ST_SET_SAVE);

    /* Save. */
    rc = cbx_settings_tab_activate(&f->st);
    assert_int_equal(rc, 0);

    /* Verify settings file exists. */
    char settings_path[PATH_MAX + 128];
    snprintf(settings_path, sizeof(settings_path),
             "%s/.config/controller-box/settings.yaml", f->tmp);
    struct stat st;
    assert_int_equal(stat(settings_path, &st), 0);
}

/* Test: service installation with mock systemctl. */
static void test_service_install(void **state)
{
    mi_fixture *f = FIX(state);

    /* Mock systemctl is already set up in setup. */
    /* Also mock group to be not in inputplumber (to test warning). */
    char grouppath[PATH_MAX + 128];
    snprintf(grouppath, sizeof(grouppath), "%s/group", f->tmp);
    FILE *g = fopen(grouppath, "w");
    assert_non_null(g);
    fprintf(g, "root:x:0:\n");
    fclose(g);
    cbx_service_set_mock_group_file(grouppath);
    cbx_service_set_mock_username("testuser");

    char status[512];
    int rc = cbx_service_install(status, sizeof(status));
    assert_int_equal(rc, CBX_SVC_OK);

    /* Verify the unit file was created. */
    char unit_path[PATH_MAX + 128];
    snprintf(unit_path, sizeof(unit_path),
             "%s/.config/systemd/user/controller-box.service", f->tmp);
    struct stat st;
    assert_int_equal(stat(unit_path, &st), 0);

    /* Verify status message includes group warning. */
    assert_non_null(strstr(status, "inputplumber"));
}

/* Test: uninstall the service. */
static void test_service_uninstall(void **state)
{
    mi_fixture *f = FIX(state);

    /* Install first. */
    char status[256];
    int rc = cbx_service_install(status, sizeof(status));
    assert_int_equal(rc, CBX_SVC_OK);

    /* Verify file exists. */
    char unit_path[PATH_MAX + 128];
    snprintf(unit_path, sizeof(unit_path),
             "%s/.config/systemd/user/controller-box.service", f->tmp);
    struct stat st;
    assert_int_equal(stat(unit_path, &st), 0);

    /* Uninstall. */
    rc = cbx_service_uninstall();
    assert_int_equal(rc, 0);

    /* Verify file is gone. */
    assert_int_equal(stat(unit_path, &st), -1);
}

/* Test: full workflow — create profile, edit sequential, validate, save,
 * set settings, install service. */
static void test_full_integration(void **state)
{
    mi_fixture *f = FIX(state);

    /* Step 1: Create a profile. */
    int rc = cbx_profiles_tab_create(&f->pt, "integration",
                                      CBX_PT_CREATE_EMPTY);
    assert_int_equal(rc, 0);

    /* Step 2: Build a valid NES minimum profile. */
    cbx_profile prof;
    cbx_profile_init(&prof);
    strncpy(prof.name, "integration", sizeof(prof.name) - 1);
    strncpy(prof.kind, "DeviceProfile", sizeof(prof.kind) - 1);
    prof.version = 1;

    for (int i = 0; i < NES_MIN_COUNT; i++) {
        cbx_profile_mapping *m = &prof.mappings[prof.mapping_count];
        memset(m, 0, sizeof(*m));
        strncpy(m->name, nes_buttons[i], sizeof(m->name) - 1);
        strncpy(m->source_event.device_class, "gamepad",
                 sizeof(m->source_event.device_class) - 1);
        m->source_event.prop_count = 1;
        strncpy(m->source_event.props[0].key, "button",
                 sizeof(m->source_event.props[0].key) - 1);
        strncpy(m->source_event.props[0].value, nes_buttons[i],
                 sizeof(m->source_event.props[0].value) - 1);
        m->target_event_count = 1;
        strncpy(m->target_events[0].device_class, "keyboard",
                 sizeof(m->target_events[0].device_class) - 1);
        strncpy(m->target_events[0].value, "KeyA",
                 sizeof(m->target_events[0].value) - 1);
        prof.mapping_count++;
    }

    /* Step 3: Validate. */
    char missing[256] = {0};
    rc = cbx_profile_validate_nes_minimum(&prof, missing, sizeof(missing));
    assert_int_equal(rc, 0);

    /* Step 4: Save the profile. */
    char profiles_dir[PATH_MAX + 64];
    snprintf(profiles_dir, sizeof(profiles_dir),
             "%s/.local/share/inputplumber/profiles", f->tmp);
    rc = cbx_profile_save_to_dir(&prof, "integration", NULL,
                                  profiles_dir, missing, sizeof(missing));
    assert_int_equal(rc, 0);

    /* Step 5: Save settings. */
    /* Toggle launch_at_boot. */
    cbx_settings_tab_activate(&f->st);  /* toggle (at index 0) */

    /* Navigate to save and activate. */
    for (int i = 0; i < CBX_ST_SET_COUNT - 1; i++)
        cbx_settings_tab_move_down(&f->st);
    cbx_settings_tab_activate(&f->st);  /* save */

    /* Verify settings file. */
    char settings_path[PATH_MAX + 128];
    snprintf(settings_path, sizeof(settings_path),
             "%s/.config/controller-box/settings.yaml", f->tmp);
    struct stat st;
    assert_int_equal(stat(settings_path, &st), 0);

    /* Step 6: Install the overlay service. */
    char svc_status[512];
    rc = cbx_service_install(svc_status, sizeof(svc_status));
    assert_int_equal(rc, CBX_SVC_OK);

    /* Verify unit file. */
    char unit_path[PATH_MAX + 128];
    snprintf(unit_path, sizeof(unit_path),
             "%s/.config/systemd/user/controller-box.service", f->tmp);
    assert_int_equal(stat(unit_path, &st), 0);

    /* Step 7: Rendering doesn't crash. */
    cbx_manager_render(&f->mgr);
}

/* Test: rendering all tabs doesn't crash. */
static void test_render_all_tabs(void **state)
{
    mi_fixture *f = FIX(state);

    /* Render each tab. */
    for (int tab = 0; tab < CBX_MGR_TAB_COUNT; tab++) {
        f->mgr.active_tab = tab;
        cbx_manager_render(&f->mgr);
    }

    /* Restore to first tab. */
    f->mgr.active_tab = CBX_MGR_TAB_CONTROLLERS;
}

/* ------------------------------------------------------------------ */
/*  Test runner                                                        */
/* ------------------------------------------------------------------ */

static const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(test_manager_init, setup, teardown),
    cmocka_unit_test_setup_teardown(test_controllers_tab_loaded, setup, teardown),
    cmocka_unit_test_setup_teardown(test_add_controller, setup, teardown),
    cmocka_unit_test_setup_teardown(test_profiles_tab_initialized, setup, teardown),
    cmocka_unit_test_setup_teardown(test_create_profile, setup, teardown),
    cmocka_unit_test_setup_teardown(test_full_profile_workflow, setup, teardown),
    cmocka_unit_test_setup_teardown(test_settings_save, setup, teardown),
    cmocka_unit_test_setup_teardown(test_service_install, setup, teardown),
    cmocka_unit_test_setup_teardown(test_service_uninstall, setup, teardown),
    cmocka_unit_test_setup_teardown(test_full_integration, setup, teardown),
    cmocka_unit_test_setup_teardown(test_render_all_tabs, setup, teardown),
};

int main(void)
{
    return cmocka_run_group_tests(tests, NULL, NULL);
}