/*
 * test_profiles_tab.c — Tests for the Profiles tab (Task 36).
 *
 * Tests:
 *   - Init populates the panel with widgets
 *   - Refresh enumerates profiles from filesystem
 *   - Create: default copy, empty, clone
 *   - Create validates name (^[a-zA-Z0-9_-]+$)
 *   - Create rejects duplicates (-EEXIST)
 *   - Delete: removes user-created profile + sidecar
 *   - Delete validates: rejects default, rejects system (-EINVAL)
 *   - Name input mode: char, backspace, confirm, cancel
 *   - Delete confirmation mode: begin, confirm, cancel
 *   - NULL safety
 *   - Accessors
 *   - Full workflow (create → delete)
 *
 * Task 36 — Profiles tab.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <setjmp.h>
#include <cmocka.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "manager/profiles_tab.h"
#include "manager/manager.h"
#include "config/config_profile.h"
#include "config/config_profile_meta.h"
#include "config/config_paths.h"
#include "ui/widget.h"

#ifndef CBX_FONT_PATH
#define CBX_FONT_PATH ""
#endif

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static void
ensure_dummy_driver(void)
{
    if (getenv("SDL_VIDEODRIVER") == NULL)
        SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "dummy",
                                 SDL_HINT_OVERRIDE);
}

/*
 * Create a temp directory tree for testing:
 *   <tmp>/user_profiles/   — user profiles dir
 *   <tmp>/system_profiles/ — system profiles dir
 *   <tmp>/config/           — config dir (for sidecars)
 *   <tmp>/config/profile-metadata/ — sidecar dir
 *
 * Uses XDG env vars to redirect the app to these directories.
 */
typedef struct {
    char tmp[256];               /* always short: /tmp/cbx_pt_test_PID */
    char user_dir[PATH_MAX];
    char system_dir[PATH_MAX];
    char config_dir[PATH_MAX];
    char meta_dir[PATH_MAX];
} pt_env;

static int
env_setup(pt_env *e)
{
    /* Create a unique temp directory. */
    snprintf(e->tmp, sizeof(e->tmp), "/tmp/cbx_pt_test_%d", (int)getpid());
    /* Remove if stale. */
    rmdir(e->tmp);  /* ignore errors */

    mkdir(e->tmp, 0700);

    snprintf(e->user_dir, sizeof(e->user_dir), "%s/user_profiles", e->tmp);
    snprintf(e->system_dir, sizeof(e->system_dir), "%s/system_profiles", e->tmp);
    snprintf(e->config_dir, sizeof(e->config_dir), "%s/config", e->tmp);
    snprintf(e->meta_dir, sizeof(e->meta_dir), "%s/config/controller-box/profile-metadata",
             e->tmp);

    mkdir(e->user_dir, 0700);
    mkdir(e->system_dir, 0700);
    mkdir(e->config_dir, 0700);
    mkdir(e->meta_dir, 0700);

    /* Set XDG env vars to redirect the app. */
    setenv("XDG_DATA_HOME", e->tmp, 1);
    setenv("XDG_CONFIG_HOME", e->config_dir, 1);

    return 0;
}

static void
env_teardown(pt_env *e)
{
    /* Best-effort cleanup. */
    char cmd[PATH_MAX + 128];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", e->tmp);
    (void)!system(cmd);

    unsetenv("XDG_DATA_HOME");
    unsetenv("XDG_CONFIG_HOME");
}

/* Write a minimal valid profile YAML to a path. */
static void
write_profile_yaml(const char *path, const char *name, int mappings)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "version: 1\n");
    fprintf(f, "kind: DeviceProfile\n");
    fprintf(f, "name: %s\n", name);
    fprintf(f, "description: Test profile\n");
    if (mappings > 0) {
        fprintf(f, "mapping:\n");
        for (int i = 0; i < mappings; i++)
            fprintf(f,
                "  - name: btn%d\n"
                "    source_event:\n"
                "      gamepad: {}\n"
                "    target_events:\n"
                "      - keyboard: Key%d\n",
                i, i);
    }
    fclose(f);
}

/* Write a profile YAML with all 6 NES minimum button bindings (A, B,
 * Up, Down, Left, Right).  Used for the default profile in tests that
 * exercise the production save path (which enforces NES minimum). */
static void
write_nes_profile_yaml(const char *path, const char *name)
{
    static const char *btns[] = {"A", "B", "Up", "Down", "Left", "Right"};
    static const char *keys[] = {"KeyA", "KeyB", "KeyUp", "KeyDown",
                                  "KeyLeft", "KeyRight"};
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "version: 1\n");
    fprintf(f, "kind: DeviceProfile\n");
    fprintf(f, "name: %s\n", name);
    fprintf(f, "description: NES test profile\n");
    fprintf(f, "mapping:\n");
    for (int i = 0; i < 6; i++)
        fprintf(f,
            "  - name: btn_%s\n"
            "    source_event:\n"
            "      gamepad:\n"
            "        button: %s\n"
            "    target_events:\n"
            "      - keyboard: %s\n",
            btns[i], btns[i], keys[i]);
    fclose(f);
}

/* Check if a file exists. */
static bool
file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* ------------------------------------------------------------------ */
/*  Test fixture                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    pt_env    env;
    cbx_manager mgr;
    cbx_profiles_tab tab;
} pt_fixture;

static int
pt_setup(void **state)
{
    pt_fixture *f = malloc(sizeof(*f));
    if (!f)
        return -1;
    memset(f, 0, sizeof(*f));
    ensure_dummy_driver();

    env_setup(&f->env);

    /* Create system default profile with NES minimum bindings so that
     * the production save path (which validates NES minimum) accepts it. */
    char def_path[PATH_MAX + 128];
    snprintf(def_path, sizeof(def_path), "%s/default.yaml", f->env.system_dir);
    write_nes_profile_yaml(def_path, "Default");

    /* Create system profile "fps". */
    char fps_path[PATH_MAX + 128];
    snprintf(fps_path, sizeof(fps_path), "%s/fps.yaml", f->env.system_dir);
    write_profile_yaml(fps_path, "FPS", 2);

    /* Init manager (creates SDL2 window, panels, tabbar, and now
     * initialises all three tab modules).  Shut down the manager-owned
     * profiles tab so these tests can re-initialise it with test dirs. */
    int rc = cbx_manager_init(&f->mgr, NULL);
    if (rc != 0) {
        env_teardown(&f->env);
        free(f);
        return -1;
    }
    cbx_profiles_tab_shutdown(cbx_manager_profiles_tab(&f->mgr));

    *state = f;
    return 0;
}

static int
pt_teardown(void **state)
{
    pt_fixture *f = *state;
    if (f) {
        cbx_profiles_tab_shutdown(&f->tab);
        cbx_manager_shutdown(&f->mgr);
        env_teardown(&f->env);
        free(f);
    }
    return 0;
}

#define FIX(state) (*(pt_fixture **)(state))

/* Helper: init the profiles tab with test dirs and refresh. */
static void
init_tab(pt_fixture *f)
{
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_PROFILES];
    cbx_profiles_tab_init(&f->tab, panel,
                            &f->mgr.text_cache, &f->mgr.theme,
                            f->mgr.font_id);
    cbx_profiles_tab_set_test_dirs(&f->tab, f->env.user_dir,
                                     f->env.system_dir, f->env.meta_dir);
    cbx_profiles_tab_set_context(&f->tab, f->mgr.rend.renderer,
                                    NULL, NULL);
    cbx_profiles_tab_refresh(&f->tab);
}

/* Helper: init the profiles tab with real (non-test) dirs so that
 * cbx_profile_list_enumerate() scans the builtin profiles dir. */
static void
init_tab_real(pt_fixture *f)
{
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_PROFILES];
    cbx_profiles_tab_init(&f->tab, panel,
                            &f->mgr.text_cache, &f->mgr.theme,
                            f->mgr.font_id);
    /* Do NOT call set_test_dirs — let the tab use the real enumerate path
     * which scans cbx_builtin_profiles_dir() first. */
    cbx_profiles_tab_set_context(&f->tab, f->mgr.rend.renderer,
                                    NULL, NULL);
    cbx_profiles_tab_refresh(&f->tab);
}

/* ================================================================== */
/*  Tests                                                              */
/* ================================================================== */

/* --- Clean-install: Default copy uses shipped default (PR-04) --- */

/* Verify that on a clean install (empty user dirs, using the real
 * enumerate path), the immutable built-in Default is found and a
 * "Default copy" produces a profile with the same 6 NES bindings as
 * the shipped data/profiles/default.yaml. */
static void
test_clean_install_default_copy_uses_shipped(void **state)
{
    pt_fixture *f = FIX(state);

    /* Use the real enumerate path (scans builtin profiles dir). */
    init_tab_real(f);

    /* The builtin Default must be present. */
    int count = cbx_profiles_tab_profile_count(&f->tab);
    assert_int_in_range(count, 1, 100);

    bool found_default = false;
    for (int i = 0; i < count; i++) {
        const cbx_profile_entry *e = cbx_profiles_tab_entry(&f->tab, i);
        if (e && e->is_default) {
            assert_true(e->read_only);
            found_default = true;
        }
    }
    assert_true(found_default);

    /* "Default copy" must succeed and use the shipped default. */
    int rc = cbx_profiles_tab_create(&f->tab, "cleaninstall",
                                        CBX_PT_CREATE_DEFAULT_COPY);
    assert_int_equal(rc, 0);

    /* The copy should exist in the real user dir (XDG_DATA_HOME/inputplumber/
     * profiles). env_setup sets XDG_DATA_HOME to e->tmp. */
    char user_profiles_dir[PATH_MAX + 256];
    snprintf(user_profiles_dir, sizeof(user_profiles_dir),
             "%s/inputplumber/profiles", f->env.tmp);
    char copy_path[PATH_MAX + 1024];
    snprintf(copy_path, sizeof(copy_path), "%s/cleaninstall.yaml",
             user_profiles_dir);
    assert_true(file_exists(copy_path));

    /* Load the copy and verify it has 6 NES bindings (shipped default). */
    cbx_profile prof;
    rc = cbx_profile_load(&prof, copy_path);
    assert_int_equal(rc, 0);
    assert_int_equal(prof.mapping_count, 6);

    /* Clean up the copy. */
    unlink(copy_path);
}

/* --- Init ---------------------------------------------------------- */

static void
test_init_populates_panel(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);
    /* List controls plus explicit editor Save/Discard actions. */
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_PROFILES];
    assert_int_equal(cbx_panel_child_count(panel), 8);
}

static void
test_init_enumerates_profiles(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);
    /* Should find default + fps. */
    assert_int_equal(cbx_profiles_tab_profile_count(&f->tab), 2);
}

static void
test_init_null_args(void **state)
{
    (void)state;
    cbx_profiles_tab tab;
    int rc = cbx_profiles_tab_init(&tab, NULL, NULL, NULL, -1);
    assert_int_equal(rc, -EINVAL);
}

/* --- Refresh ------------------------------------------------------- */

static void
test_refresh_updates_list(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    /* Add a user profile. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/custom.yaml", f->env.user_dir);
    write_profile_yaml(path, "Custom", 1);

    /* Before refresh: 2 profiles. */
    assert_int_equal(cbx_profiles_tab_profile_count(&f->tab), 2);

    /* After refresh: 3 profiles. */
    int rc = cbx_profiles_tab_refresh(&f->tab);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_profiles_tab_profile_count(&f->tab), 3);
}

static void
test_refresh_null(void **state)
{
    (void)state;
    int rc = cbx_profiles_tab_refresh(NULL);
    assert_int_equal(rc, -EINVAL);
}

static void
test_refresh_empty_dirs(void **state)
{
    pt_fixture *f = FIX(state);

    /* Remove system profiles to get empty enumeration. */
    char cmd[PATH_MAX + 128];
    snprintf(cmd, sizeof(cmd), "rm -f %s/*.yaml", f->env.system_dir);
    (void)!system(cmd);

    init_tab(f);
    /* Empty dirs → 0 profiles (enumerate returns 0 with empty list). */
    assert_int_equal(cbx_profiles_tab_profile_count(&f->tab), 0);
}

/* --- Create: default copy ----------------------------------------- */

static void
test_create_default_copy(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    int rc = cbx_profiles_tab_create(&f->tab, "mycopy",
                                        CBX_PT_CREATE_DEFAULT_COPY);
    assert_int_equal(rc, 0);

    /* Profile file should exist in user dir. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/mycopy.yaml", f->env.user_dir);
    assert_true(file_exists(path));

    /* Profile count should increase. */
    assert_int_equal(cbx_profiles_tab_profile_count(&f->tab), 3);

    /* New profile should not be read-only. */
    const cbx_profile_entry *e = cbx_profiles_tab_entry(&f->tab, 2);
    assert_non_null(e);
    assert_false(e->read_only);
}

static void
test_create_default_copy_no_default(void **state)
{
    pt_fixture *f = FIX(state);

    /* Remove system profiles so there's no default. */
    char cmd[PATH_MAX + 128];
    snprintf(cmd, sizeof(cmd), "rm -f %s/*.yaml", f->env.system_dir);
    (void)!system(cmd);

    init_tab(f);

    int rc = cbx_profiles_tab_create(&f->tab, "mycopy",
                                        CBX_PT_CREATE_DEFAULT_COPY);
    assert_int_equal(rc, -ENOENT);
}

/* --- Create: empty ------------------------------------------------- */

static void
test_create_empty(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    /* An empty profile has no NES minimum bindings, so the production
     * save path (cbx_profile_save_to_dir) must reject it. */
    int rc = cbx_profiles_tab_create(&f->tab, "empty1",
                                        CBX_PT_CREATE_EMPTY);
    assert_int_equal(rc, -EINVAL);

    /* No file should be written. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/empty1.yaml", f->env.user_dir);
    assert_false(file_exists(path));
}

/* A profile missing NES minimum bindings must be rejected when saved
 * through the production path (cbx_profile_save_to_dir).  Verify that
 * cloning a non-NES source (fps profile with 2 generic mappings) fails. */
static void
test_create_reject_missing_nes(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    /* The fps profile has 2 generic mappings (no NES button names). */
    /* Find fps and select it for cloning. */
    int fps_idx = -1;
    for (int i = 0; i < f->tab.profiles.count; i++) {
        if (strcmp(f->tab.profiles.entries[i].filename, "fps") == 0) {
            fps_idx = i;
            break;
        }
    }
    assert_int_not_equal(fps_idx, -1);
    f->tab.selected_profile = fps_idx;

    int rc = cbx_profiles_tab_create(&f->tab, "noNes",
                                        CBX_PT_CREATE_CLONE);
    assert_int_equal(rc, -EINVAL);

    /* No file should be written. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/noNes.yaml", f->env.user_dir);
    assert_false(file_exists(path));
}

/* --- Create: clone ------------------------------------------------- */

static void
test_create_clone(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    /* Select the first profile (default) to clone. */
    f->tab.selected_profile = 0;

    int rc = cbx_profiles_tab_create(&f->tab, "cloned",
                                        CBX_PT_CREATE_CLONE);
    assert_int_equal(rc, 0);

    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/cloned.yaml", f->env.user_dir);
    assert_true(file_exists(path));

    /* Cloned profile should have same mappings as default (3). */
    cbx_profile p;
    cbx_profile_init(&p);
    cbx_profile_load(&p, path);
    assert_int_equal(p.mapping_count, 6);
}

static void
test_create_clone_no_selection(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    f->tab.selected_profile = -1;
    int rc = cbx_profiles_tab_create(&f->tab, "cloned",
                                        CBX_PT_CREATE_CLONE);
    assert_int_equal(rc, -EINVAL);
}

/* --- Create: name validation -------------------------------------- */

static void
test_create_invalid_name(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    /* Names with spaces, slashes, dots should be rejected. */
    assert_int_equal(cbx_profiles_tab_create(&f->tab, "bad name",
                                                CBX_PT_CREATE_EMPTY), -EINVAL);
    assert_int_equal(cbx_profiles_tab_create(&f->tab, "bad/name",
                                                CBX_PT_CREATE_EMPTY), -EINVAL);
    assert_int_equal(cbx_profiles_tab_create(&f->tab, "bad.name",
                                                CBX_PT_CREATE_EMPTY), -EINVAL);
    assert_int_equal(cbx_profiles_tab_create(&f->tab, "",
                                                CBX_PT_CREATE_EMPTY), -EINVAL);
}

static void
test_create_duplicate(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    /* "default" already exists (system). */
    int rc = cbx_profiles_tab_create(&f->tab, "default",
                                        CBX_PT_CREATE_EMPTY);
    assert_int_equal(rc, -EEXIST);
}

static void
test_create_null(void **state)
{
    (void)state;
    int rc = cbx_profiles_tab_create(NULL, "test", CBX_PT_CREATE_EMPTY);
    assert_int_equal(rc, -EINVAL);
}

/* --- Delete -------------------------------------------------------- */

static void
test_delete_user_profile(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    /* Create a user profile first. */
    cbx_profiles_tab_create(&f->tab, "todelete", CBX_PT_CREATE_DEFAULT_COPY);
    assert_int_equal(cbx_profiles_tab_profile_count(&f->tab), 3);

    /* Find and delete it. */
    int idx = -1;
    for (int i = 0; i < f->tab.profiles.count; i++) {
        if (strcmp(f->tab.profiles.entries[i].filename, "todelete") == 0) {
            idx = i;
            break;
        }
    }
    assert_int_not_equal(idx, -1);

    int rc = cbx_profiles_tab_delete(&f->tab, idx);
    assert_int_equal(rc, 0);

    /* File should be gone. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/todelete.yaml", f->env.user_dir);
    assert_false(file_exists(path));

    /* Profile count should decrease. */
    assert_int_equal(cbx_profiles_tab_profile_count(&f->tab), 2);
}

static void
test_delete_with_sidecar(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    /* Create a user profile + sidecar. */
    cbx_profiles_tab_create(&f->tab, "withmeta", CBX_PT_CREATE_DEFAULT_COPY);

    cbx_profile_meta meta;
    cbx_profile_meta_init(&meta);
    snprintf(meta.display_name, sizeof(meta.display_name), "With Meta");
    meta.has_display_name = true;
    cbx_profile_meta_save_for(&meta, "withmeta");

    /* Verify sidecar exists. */
    char sidecar[PATH_MAX + 128];
    snprintf(sidecar, sizeof(sidecar), "%s/withmeta.meta.yaml", f->env.meta_dir);
    assert_true(file_exists(sidecar));

    /* Find and delete. */
    int idx = -1;
    for (int i = 0; i < f->tab.profiles.count; i++) {
        if (strcmp(f->tab.profiles.entries[i].filename, "withmeta") == 0) {
            idx = i;
            break;
        }
    }
    assert_int_not_equal(idx, -1);

    int rc = cbx_profiles_tab_delete(&f->tab, idx);
    assert_int_equal(rc, 0);

    /* Both files should be gone. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/withmeta.yaml", f->env.user_dir);
    assert_false(file_exists(path));
    assert_false(file_exists(sidecar));
}

static void
test_delete_default_rejected(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    /* Find the default profile. */
    int idx = -1;
    for (int i = 0; i < f->tab.profiles.count; i++) {
        if (f->tab.profiles.entries[i].is_default) {
            idx = i;
            break;
        }
    }
    assert_int_not_equal(idx, -1);

    int rc = cbx_profiles_tab_delete(&f->tab, idx);
    assert_int_equal(rc, -EINVAL);
}

static void
test_delete_system_rejected(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    /* Find the fps (system, non-default) profile. */
    int idx = -1;
    for (int i = 0; i < f->tab.profiles.count; i++) {
        if (strcmp(f->tab.profiles.entries[i].filename, "fps") == 0) {
            idx = i;
            break;
        }
    }
    assert_int_not_equal(idx, -1);

    int rc = cbx_profiles_tab_delete(&f->tab, idx);
    assert_int_equal(rc, -EINVAL);
}

static void
test_delete_bad_index(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    assert_int_equal(cbx_profiles_tab_delete(&f->tab, -1), -EINVAL);
    assert_int_equal(cbx_profiles_tab_delete(&f->tab, 999), -EINVAL);
}

static void
test_delete_null(void **state)
{
    (void)state;
    int rc = cbx_profiles_tab_delete(NULL, 0);
    assert_int_equal(rc, -EINVAL);
}

/* --- Name input mode ----------------------------------------------- */

static void
test_name_input_basic(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    int rc = cbx_profiles_tab_begin_create(&f->tab, CBX_PT_CREATE_EMPTY);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_profiles_tab_mode(&f->tab), CBX_PT_MODE_NAME_INPUT);

    /* Type "myname". */
    const char *name = "myname";
    for (int i = 0; name[i]; i++) {
        rc = cbx_profiles_tab_name_input_char(&f->tab, name[i]);
        assert_int_equal(rc, 0);
    }
    assert_string_equal(cbx_profiles_tab_name_buffer(&f->tab), "myname");
}

static void
test_name_input_backspace(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    cbx_profiles_tab_begin_create(&f->tab, CBX_PT_CREATE_EMPTY);
    cbx_profiles_tab_name_input_char(&f->tab, 'a');
    cbx_profiles_tab_name_input_char(&f->tab, 'b');
    cbx_profiles_tab_name_input_char(&f->tab, 'c');
    assert_string_equal(cbx_profiles_tab_name_buffer(&f->tab), "abc");

    cbx_profiles_tab_name_input_backspace(&f->tab);
    assert_string_equal(cbx_profiles_tab_name_buffer(&f->tab), "ab");

    /* Backspace on empty is a no-op. */
    cbx_profiles_tab_name_input_backspace(&f->tab);
    cbx_profiles_tab_name_input_backspace(&f->tab);
    assert_string_equal(cbx_profiles_tab_name_buffer(&f->tab), "");
}

static void
test_name_input_invalid_chars(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    cbx_profiles_tab_begin_create(&f->tab, CBX_PT_CREATE_EMPTY);

    /* Space, slash, dot should be rejected. */
    assert_int_equal(cbx_profiles_tab_name_input_char(&f->tab, ' '), -EINVAL);
    assert_int_equal(cbx_profiles_tab_name_input_char(&f->tab, '/'), -EINVAL);
    assert_int_equal(cbx_profiles_tab_name_input_char(&f->tab, '.'), -EINVAL);

    /* Valid chars should work. */
    assert_int_equal(cbx_profiles_tab_name_input_char(&f->tab, 'x'), 0);
    assert_int_equal(cbx_profiles_tab_name_input_char(&f->tab, '_'), 0);
    assert_int_equal(cbx_profiles_tab_name_input_char(&f->tab, '-'), 0);
    assert_int_equal(cbx_profiles_tab_name_input_char(&f->tab, '1'), 0);
}

static void
test_name_input_confirm(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    cbx_profiles_tab_begin_create(&f->tab, CBX_PT_CREATE_DEFAULT_COPY);
    cbx_profiles_tab_name_input_char(&f->tab, 'n');
    cbx_profiles_tab_name_input_char(&f->tab, 'e');
    cbx_profiles_tab_name_input_char(&f->tab, 'w');

    int rc = cbx_profiles_tab_name_input_confirm(&f->tab);
    assert_int_equal(rc, 0);

    /* Create-to-editor flow: editor should now be open, no file yet. */
    assert_int_equal(cbx_profiles_tab_mode(&f->tab), CBX_PT_MODE_EDITOR);
    assert_true(f->tab.editor_initialized);
    assert_true(f->tab.editor_is_new);
    assert_string_equal(f->tab.editor_profile_name, "new");
    assert_true(cbx_widget_is_visible(&f->tab.save_btn.base));
    assert_true(cbx_widget_is_visible(&f->tab.discard_btn.base));

    /* Save from editor via cancel (B in LIST = save+close). */
    bool handled = cbx_profiles_tab_cancel(&f->tab);
    assert_true(handled);

    /* Now back in list mode and profile should exist. */
    assert_int_equal(cbx_profiles_tab_mode(&f->tab), CBX_PT_MODE_LIST);

    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/new.yaml", f->env.user_dir);
    assert_true(file_exists(path));
}

static void
test_normal_target_model_without_sidecar(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    /* Normal workflow context comes from the selected virtual target, not a
     * test-authored profile metadata sidecar. */
    cbx_profiles_tab_set_device_type(&f->tab, "xb360");
    cbx_profiles_tab_begin_create(&f->tab, CBX_PT_CREATE_DEFAULT_COPY);
    for (const char *p = "target-model"; *p; p++)
        assert_int_equal(cbx_profiles_tab_name_input_char(&f->tab, *p), 0);
    assert_int_equal(cbx_profiles_tab_name_input_confirm(&f->tab), 0);
    assert_string_equal(cbx_profile_editor_resolved_icon(&f->tab.editor),
                        "cc-xbox-360");
    assert_int_equal(cbx_profile_editor_diagram_provenance(&f->tab.editor),
                     CBX_DIAG_PROVENANCE_SUPPORTED_MODEL);
}

static void
test_name_input_confirm_empty(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    cbx_profiles_tab_begin_create(&f->tab, CBX_PT_CREATE_EMPTY);
    /* No chars typed. */
    int rc = cbx_profiles_tab_name_input_confirm(&f->tab);
    assert_int_equal(rc, -EINVAL);
}

static void
test_name_input_cancel(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    cbx_profiles_tab_begin_create(&f->tab, CBX_PT_CREATE_EMPTY);
    cbx_profiles_tab_name_input_char(&f->tab, 'x');

    cbx_profiles_tab_name_input_cancel(&f->tab);
    assert_int_equal(cbx_profiles_tab_mode(&f->tab), CBX_PT_MODE_LIST);
    assert_string_equal(cbx_profiles_tab_name_buffer(&f->tab), "");
}

static void
test_name_input_not_in_mode(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    /* Not in name input mode. */
    assert_int_equal(cbx_profiles_tab_name_input_char(&f->tab, 'x'), -EINVAL);
    assert_int_equal(cbx_profiles_tab_name_input_backspace(&f->tab), -EINVAL);
    assert_int_equal(cbx_profiles_tab_name_input_confirm(&f->tab), -EINVAL);
}

/* --- Delete confirmation mode -------------------------------------- */

static void
test_delete_confirm_basic(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    /* Create a user profile. */
    cbx_profiles_tab_create(&f->tab, "willdel", CBX_PT_CREATE_DEFAULT_COPY);

    /* Find it. */
    int idx = -1;
    for (int i = 0; i < f->tab.profiles.count; i++) {
        if (strcmp(f->tab.profiles.entries[i].filename, "willdel") == 0) {
            idx = i;
            break;
        }
    }
    assert_int_not_equal(idx, -1);

    int rc = cbx_profiles_tab_begin_delete(&f->tab, idx);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_profiles_tab_mode(&f->tab), CBX_PT_MODE_CONFIRM_DELETE);

    rc = cbx_profiles_tab_confirm_delete(&f->tab);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_profiles_tab_mode(&f->tab), CBX_PT_MODE_LIST);

    /* Profile should be gone. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/willdel.yaml", f->env.user_dir);
    assert_false(file_exists(path));
}

static void
test_delete_confirm_readonly_rejected(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    /* Find default. */
    int idx = -1;
    for (int i = 0; i < f->tab.profiles.count; i++) {
        if (f->tab.profiles.entries[i].is_default) {
            idx = i;
            break;
        }
    }
    assert_int_not_equal(idx, -1);

    int rc = cbx_profiles_tab_begin_delete(&f->tab, idx);
    assert_int_equal(rc, -EINVAL);
}

static void
test_delete_confirm_cancel(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    /* Create a user profile. */
    cbx_profiles_tab_create(&f->tab, "keep", CBX_PT_CREATE_DEFAULT_COPY);

    int idx = -1;
    for (int i = 0; i < f->tab.profiles.count; i++) {
        if (strcmp(f->tab.profiles.entries[i].filename, "keep") == 0) {
            idx = i;
            break;
        }
    }
    assert_int_not_equal(idx, -1);

    cbx_profiles_tab_begin_delete(&f->tab, idx);
    cbx_profiles_tab_cancel_delete(&f->tab);
    assert_int_equal(cbx_profiles_tab_mode(&f->tab), CBX_PT_MODE_LIST);

    /* Profile should still exist. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/keep.yaml", f->env.user_dir);
    assert_true(file_exists(path));
}

static void
test_delete_confirm_not_in_mode(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    int rc = cbx_profiles_tab_confirm_delete(&f->tab);
    assert_int_equal(rc, -EINVAL);
}

/* --- Accessors ----------------------------------------------------- */

static void
test_accessors_null_safe(void **state)
{
    (void)state;
    assert_int_equal(cbx_profiles_tab_profile_count(NULL), 0);
    assert_int_equal(cbx_profiles_tab_selected(NULL), -1);
    assert_int_equal(cbx_profiles_tab_mode(NULL), CBX_PT_MODE_LIST);
    assert_null(cbx_profiles_tab_name_buffer(NULL));
    assert_null(cbx_profiles_tab_list(NULL));
    assert_null(cbx_profiles_tab_entry(NULL, 0));
}

static void
test_entry_accessor(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    const cbx_profile_entry *e = cbx_profiles_tab_entry(&f->tab, 0);
    assert_non_null(e);
    /* First entry should be default. */
    assert_true(e->is_default || e->read_only);

    /* Out of bounds. */
    assert_null(cbx_profiles_tab_entry(&f->tab, 999));
    assert_null(cbx_profiles_tab_entry(&f->tab, -1));
}

/* --- Shutdown ------------------------------------------------------ */

static void
test_shutdown_null_safe(void **state)
{
    (void)state;
    cbx_profiles_tab_shutdown(NULL);
}

static void
test_shutdown_removes_children(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);
    cbx_panel *panel = &f->mgr.panels[CBX_MGR_TAB_PROFILES];
    assert_int_equal(cbx_panel_child_count(panel), 8);

    cbx_profiles_tab_shutdown(&f->tab);
    assert_int_equal(cbx_panel_child_count(panel), 0);
}

/* --- Full workflow ------------------------------------------------- */

static void
test_full_workflow(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    /* Start: 2 profiles (default + fps). */
    assert_int_equal(cbx_profiles_tab_profile_count(&f->tab), 2);

    /* Create via name input — opens editor (create-to-editor flow). */
    cbx_profiles_tab_begin_create(&f->tab, CBX_PT_CREATE_DEFAULT_COPY);
    cbx_profiles_tab_name_input_char(&f->tab, 'w');
    cbx_profiles_tab_name_input_char(&f->tab, 'f');
    cbx_profiles_tab_name_input_char(&f->tab, '1');
    int rc = cbx_profiles_tab_name_input_confirm(&f->tab);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_profiles_tab_mode(&f->tab), CBX_PT_MODE_EDITOR);

    /* Save from editor. */
    cbx_profiles_tab_cancel(&f->tab);
    assert_int_equal(cbx_profiles_tab_mode(&f->tab), CBX_PT_MODE_LIST);

    /* Now 3 profiles. */
    assert_int_equal(cbx_profiles_tab_profile_count(&f->tab), 3);

    /* Create another via direct create. */
    rc = cbx_profiles_tab_create(&f->tab, "empty2", CBX_PT_CREATE_DEFAULT_COPY);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_profiles_tab_profile_count(&f->tab), 4);

    /* Delete wf1 via delete confirmation. */
    int idx = -1;
    for (int i = 0; i < f->tab.profiles.count; i++) {
        if (strcmp(f->tab.profiles.entries[i].filename, "wf1") == 0) {
            idx = i;
            break;
        }
    }
    assert_int_not_equal(idx, -1);

    cbx_profiles_tab_begin_delete(&f->tab, idx);
    rc = cbx_profiles_tab_confirm_delete(&f->tab);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_profiles_tab_profile_count(&f->tab), 3);

    /* Delete empty2. */
    idx = -1;
    for (int i = 0; i < f->tab.profiles.count; i++) {
        if (strcmp(f->tab.profiles.entries[i].filename, "empty2") == 0) {
            idx = i;
            break;
        }
    }
    rc = cbx_profiles_tab_delete(&f->tab, idx);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_profiles_tab_profile_count(&f->tab), 2);
}

/* --- Sidecar persistence on create --------------------------------- */

static void
test_create_clone_copies_mappings(void **state)
{
    pt_fixture *f = FIX(state);
    init_tab(f);

    /* Clone the default (which has 3 mappings). */
    f->tab.selected_profile = 0;
    int rc = cbx_profiles_tab_create(&f->tab, "copy3",
                                        CBX_PT_CREATE_CLONE);
    assert_int_equal(rc, 0);

    /* Verify mappings were copied. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/copy3.yaml", f->env.user_dir);
    cbx_profile p;
    cbx_profile_init(&p);
    cbx_profile_load(&p, path);
    assert_int_equal(p.mapping_count, 6);
    /* Name should be the new name, not the source. */
    assert_string_equal(p.name, "copy3");
}

/* ================================================================== */
/*  Production-dispatch tests (through cbx_manager_handle_event)        */
/* ================================================================== */

static bool pt_send_key_dn(cbx_manager *mgr, SDL_Keycode sym)
{
    SDL_Event ev = {0};
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = sym;
    return cbx_manager_handle_event(mgr, &ev);
}

static bool pt_send_key_up(cbx_manager *mgr, SDL_Keycode sym)
{
    SDL_Event ev = {0};
    ev.type = SDL_KEYUP;
    ev.key.keysym.sym = sym;
    return cbx_manager_handle_event(mgr, &ev);
}

static void
test_create_picker_via_dispatch(void **state)
{
    (void)state;
    pt_env env;
    env_setup(&env);
    char def_path[PATH_MAX + 128];
    snprintf(def_path, sizeof(def_path), "%s/default.yaml", env.system_dir);
    write_nes_profile_yaml(def_path, "Default");

    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);
    assert_non_null(pt);
    cbx_profiles_tab_set_test_dirs(pt, env.user_dir, env.system_dir, env.meta_dir);
    cbx_profiles_tab_refresh(pt);

    /* Switch to Profiles tab. */
    pt_send_key_dn(&mgr, SDLK_RIGHT);
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_PROFILES);

    /* Click on the Create button to open the create source picker. */
    SDL_Rect btn_rect;
    cbx_widget_get_rect(&pt->create_btn.base, &btn_rect);
    int cx = btn_rect.x + btn_rect.w / 2;
    int cy = btn_rect.y + btn_rect.h / 2;
    SDL_Event mev = {0};
    mev.type = SDL_MOUSEBUTTONDOWN;
    mev.button.button = SDL_BUTTON_LEFT;
    mev.button.x = cx; mev.button.y = cy;
    cbx_manager_handle_event(&mgr, &mev);
    mev.type = SDL_MOUSEBUTTONUP;
    cbx_manager_handle_event(&mgr, &mev);
    assert_int_equal(pt->mode, CBX_PT_MODE_CREATE_PICK);
    assert_true(pt->create_picker.base.visible);

    /* Navigate to second option (Empty) and confirm with A. */
    pt_send_key_dn(&mgr, SDLK_DOWN);
    assert_int_equal(cbx_list_get_selected(&pt->create_picker), 1);
    pt_send_key_dn(&mgr, SDLK_a);
    assert_true(pt_send_key_up(&mgr, SDLK_a));
    assert_int_equal(pt->mode, CBX_PT_MODE_NAME_INPUT);

    /* Keyboard text entry uses Escape to cancel; letter B remains typeable. */
    pt_send_key_dn(&mgr, SDLK_ESCAPE);
    assert_int_equal(pt->mode, CBX_PT_MODE_LIST);

    cbx_manager_shutdown(&mgr);
    env_teardown(&env);
}

static void
test_name_input_via_dispatch(void **state)
{
    (void)state;
    pt_env env;
    env_setup(&env);
    char def_path[PATH_MAX + 128];
    snprintf(def_path, sizeof(def_path), "%s/default.yaml", env.system_dir);
    write_nes_profile_yaml(def_path, "Default");

    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);
    cbx_profiles_tab_set_test_dirs(pt, env.user_dir, env.system_dir, env.meta_dir);
    cbx_profiles_tab_refresh(pt);

    pt_send_key_dn(&mgr, SDLK_RIGHT);

    /* Click on the Create button to open the create source picker. */
    SDL_Rect btn_rect;
    cbx_widget_get_rect(&pt->create_btn.base, &btn_rect);
    int cx = btn_rect.x + btn_rect.w / 2;
    int cy = btn_rect.y + btn_rect.h / 2;
    SDL_Event mev = {0};
    mev.type = SDL_MOUSEBUTTONDOWN;
    mev.button.button = SDL_BUTTON_LEFT;
    mev.button.x = cx; mev.button.y = cy;
    cbx_manager_handle_event(&mgr, &mev);
    mev.type = SDL_MOUSEBUTTONUP;
    cbx_manager_handle_event(&mgr, &mev);
    assert_int_equal(pt->mode, CBX_PT_MODE_CREATE_PICK);
    /* Select Default Copy (index 0 — already selected). */
    pt_send_key_dn(&mgr, SDLK_a);
    pt_send_key_up(&mgr, SDLK_a);
    assert_int_equal(pt->mode, CBX_PT_MODE_NAME_INPUT);

    /* Type a name. */
    pt_send_key_dn(&mgr, SDLK_n);
    pt_send_key_dn(&mgr, SDLK_e);
    pt_send_key_dn(&mgr, SDLK_w);
    assert_string_equal(cbx_profiles_tab_name_buffer(pt), "new");

    /* Backspace. */
    pt_send_key_dn(&mgr, SDLK_BACKSPACE);
    assert_string_equal(cbx_profiles_tab_name_buffer(pt), "ne");
    pt_send_key_dn(&mgr, SDLK_w);
    assert_string_equal(cbx_profiles_tab_name_buffer(pt), "new");

    /* Keyboard text entry uses Return to confirm; letter A remains typeable. */
    pt_send_key_dn(&mgr, SDLK_RETURN);
    assert_int_equal(pt->mode, CBX_PT_MODE_EDITOR);

    /* Save from editor with B (KEYDOWN swallowed, KEYUP saves). */
    pt_send_key_dn(&mgr, SDLK_b);
    pt_send_key_up(&mgr, SDLK_b);
    assert_int_equal(pt->mode, CBX_PT_MODE_LIST);

    /* Verify the profile was created. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/new.yaml", env.user_dir);
    struct stat stbuf;
    assert_int_equal(stat(path, &stbuf), 0);

    cbx_manager_shutdown(&mgr);
    env_teardown(&env);
}

static void
test_confirm_delete_via_dispatch(void **state)
{
    (void)state;
    pt_env env;
    env_setup(&env);
    char def_path[PATH_MAX + 128];
    snprintf(def_path, sizeof(def_path), "%s/default.yaml", env.system_dir);
    write_nes_profile_yaml(def_path, "Default");
    char usr_path[PATH_MAX + 128];
    snprintf(usr_path, sizeof(usr_path), "%s/todelete.yaml", env.user_dir);
    write_profile_yaml(usr_path, "ToDelete", 1);

    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);
    cbx_profiles_tab_set_test_dirs(pt, env.user_dir, env.system_dir, env.meta_dir);
    cbx_profiles_tab_refresh(pt);

    pt_send_key_dn(&mgr, SDLK_RIGHT);
    pt_send_key_dn(&mgr, SDLK_DOWN);  /* tabbar → profile list (selected=0=default) */
    pt_send_key_dn(&mgr, SDLK_DOWN);  /* list 0→1 (todelete) */

    /* Click on the Delete button to begin delete. */
    SDL_Rect btn_rect;
    cbx_widget_get_rect(&pt->delete_btn.base, &btn_rect);
    int cx = btn_rect.x + btn_rect.w / 2;
    int cy = btn_rect.y + btn_rect.h / 2;
    SDL_Event mev = {0};
    mev.type = SDL_MOUSEBUTTONDOWN;
    mev.button.button = SDL_BUTTON_LEFT;
    mev.button.x = cx; mev.button.y = cy;
    cbx_manager_handle_event(&mgr, &mev);
    mev.type = SDL_MOUSEBUTTONUP;
    cbx_manager_handle_event(&mgr, &mev);
    assert_int_equal(pt->mode, CBX_PT_MODE_CONFIRM_DELETE);

    /* A to confirm delete. */
    pt_send_key_dn(&mgr, SDLK_a);
    assert_int_equal(pt->mode, CBX_PT_MODE_LIST);

    /* Verify the file is gone. */
    struct stat stbuf;
    assert_true(stat(usr_path, &stbuf) != 0);

    cbx_manager_shutdown(&mgr);
    env_teardown(&env);
}

static void
send_pointer_click(cbx_manager *mgr, int x, int y)
{
    SDL_Event ev = {0};
    ev.type = SDL_MOUSEBUTTONDOWN;
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.x = x; ev.button.y = y;
    cbx_manager_handle_event(mgr, &ev);
    ev.type = SDL_MOUSEBUTTONUP;
    cbx_manager_handle_event(mgr, &ev);
}

static void
test_profile_sidecar_diagram_identity_via_dispatch(void **state)
{
    (void)state;
    pt_env env;
    env_setup(&env);
    char meta_parent[PATH_MAX];
    snprintf(meta_parent, sizeof(meta_parent), "%s/config/controller-box", env.tmp);
    mkdir(meta_parent, 0700);
    mkdir(env.meta_dir, 0700);
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/default.yaml", env.system_dir);
    write_nes_profile_yaml(path, "Default");
    const char *names[] = {"a_xbox", "b_ps5"};
    const char *icons[] = {"cc-xbox-360", "cc-ps5"};
    for (int i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "%s/%s.yaml", env.user_dir, names[i]);
        write_nes_profile_yaml(path, names[i]);
        snprintf(path, sizeof(path), "%s/%s.meta.yaml", env.meta_dir, names[i]);
        FILE *meta = fopen(path, "w");
        assert_non_null(meta);
        fprintf(meta, "display_order: %d\nicon: %s\n", -2 + i, icons[i]);
        fclose(meta);
    }

    ensure_dummy_driver();
    cbx_manager mgr;
    assert_int_equal(cbx_manager_init(&mgr, NULL), 0);
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(&mgr);
    cbx_profiles_tab_set_test_dirs(pt, env.user_dir, env.system_dir, env.meta_dir);
    assert_int_equal(cbx_profiles_tab_refresh(pt), 0);
    pt_send_key_dn(&mgr, SDLK_RIGHT);

    for (int i = 0; i < 2; i++) {
        SDL_Rect list_rect, edit_rect;
        cbx_widget_get_rect(&pt->profile_list_w.base, &list_rect);
        cbx_widget_get_rect(&pt->edit_btn.base, &edit_rect);
        send_pointer_click(&mgr, list_rect.x + 30,
                           list_rect.y + i * pt->profile_list_w.item_h + 8);
        send_pointer_click(&mgr, edit_rect.x + edit_rect.w / 2,
                           edit_rect.y + edit_rect.h / 2);
        assert_int_equal(pt->mode, CBX_PT_MODE_EDITOR);
        assert_string_equal(cbx_profile_editor_resolved_icon(&pt->editor), icons[i]);
        assert_int_equal(cbx_profile_editor_diagram_provenance(&pt->editor),
                         CBX_DIAG_PROVENANCE_PROFILE_OVERRIDE);
        assert_non_null(pt->editor.diagram.base_texture);
        assert_true(cbx_widget_is_visible(&pt->editor.diagram.base));
        assert_true(cbx_profile_editor_get_diagram_highlight(&pt->editor) >= 0);
        cbx_manager_render(&mgr);
        pt_send_key_dn(&mgr, SDLK_TAB); /* discard/close through dispatch */
        assert_int_equal(pt->mode, CBX_PT_MODE_LIST);
    }
    cbx_manager_shutdown(&mgr);
    env_teardown(&env);
}

/* ================================================================== */
/*  Test runner                                                        */
/* ================================================================== */

static const struct CMUnitTest tests[] = {
    /* Init */
    cmocka_unit_test_setup_teardown(test_init_populates_panel, pt_setup, pt_teardown),
    cmocka_unit_test_setup_teardown(test_init_enumerates_profiles, pt_setup, pt_teardown),
    cmocka_unit_test(test_init_null_args),

    /* Refresh */
    cmocka_unit_test_setup_teardown(test_refresh_updates_list, pt_setup, pt_teardown),
    cmocka_unit_test(test_refresh_null),
    cmocka_unit_test_setup_teardown(test_refresh_empty_dirs, pt_setup, pt_teardown),

    /* Clean-install: Default copy uses shipped default */
    cmocka_unit_test_setup_teardown(test_clean_install_default_copy_uses_shipped, pt_setup, pt_teardown),

    /* Create: default copy */
    cmocka_unit_test_setup_teardown(test_create_default_copy, pt_setup, pt_teardown),
    cmocka_unit_test_setup_teardown(test_create_default_copy_no_default, pt_setup, pt_teardown),

    /* Create: empty */
    cmocka_unit_test_setup_teardown(test_create_empty, pt_setup, pt_teardown),
    cmocka_unit_test_setup_teardown(test_create_reject_missing_nes, pt_setup, pt_teardown),

    /* Create: clone */
    cmocka_unit_test_setup_teardown(test_create_clone, pt_setup, pt_teardown),
    cmocka_unit_test_setup_teardown(test_create_clone_no_selection, pt_setup, pt_teardown),

    /* Create: name validation */
    cmocka_unit_test_setup_teardown(test_create_invalid_name, pt_setup, pt_teardown),
    cmocka_unit_test_setup_teardown(test_create_duplicate, pt_setup, pt_teardown),
    cmocka_unit_test(test_create_null),

    /* Delete */
    cmocka_unit_test_setup_teardown(test_delete_user_profile, pt_setup, pt_teardown),
    cmocka_unit_test_setup_teardown(test_delete_with_sidecar, pt_setup, pt_teardown),
    cmocka_unit_test_setup_teardown(test_delete_default_rejected, pt_setup, pt_teardown),
    cmocka_unit_test_setup_teardown(test_delete_system_rejected, pt_setup, pt_teardown),
    cmocka_unit_test_setup_teardown(test_delete_bad_index, pt_setup, pt_teardown),
    cmocka_unit_test(test_delete_null),

    /* Name input mode */
    cmocka_unit_test_setup_teardown(test_name_input_basic, pt_setup, pt_teardown),
    cmocka_unit_test_setup_teardown(test_name_input_backspace, pt_setup, pt_teardown),
    cmocka_unit_test_setup_teardown(test_name_input_invalid_chars, pt_setup, pt_teardown),
    cmocka_unit_test_setup_teardown(test_name_input_confirm, pt_setup, pt_teardown),
    cmocka_unit_test_setup_teardown(test_normal_target_model_without_sidecar, pt_setup, pt_teardown),
    cmocka_unit_test_setup_teardown(test_name_input_confirm_empty, pt_setup, pt_teardown),
    cmocka_unit_test_setup_teardown(test_name_input_cancel, pt_setup, pt_teardown),
    cmocka_unit_test_setup_teardown(test_name_input_not_in_mode, pt_setup, pt_teardown),

    /* Delete confirmation mode */
    cmocka_unit_test_setup_teardown(test_delete_confirm_basic, pt_setup, pt_teardown),
    cmocka_unit_test_setup_teardown(test_delete_confirm_readonly_rejected, pt_setup, pt_teardown),
    cmocka_unit_test_setup_teardown(test_delete_confirm_cancel, pt_setup, pt_teardown),
    cmocka_unit_test_setup_teardown(test_delete_confirm_not_in_mode, pt_setup, pt_teardown),

    /* Accessors */
    cmocka_unit_test(test_accessors_null_safe),
    cmocka_unit_test_setup_teardown(test_entry_accessor, pt_setup, pt_teardown),

    /* Shutdown */
    cmocka_unit_test(test_shutdown_null_safe),
    cmocka_unit_test_setup_teardown(test_shutdown_removes_children, pt_setup, pt_teardown),

    /* Full workflow */
    cmocka_unit_test_setup_teardown(test_full_workflow, pt_setup, pt_teardown),
    cmocka_unit_test_setup_teardown(test_create_clone_copies_mappings, pt_setup, pt_teardown),

    /* Production-dispatch tests */
    cmocka_unit_test(test_create_picker_via_dispatch),
    cmocka_unit_test(test_name_input_via_dispatch),
    cmocka_unit_test(test_confirm_delete_via_dispatch),
    cmocka_unit_test(test_profile_sidecar_diagram_identity_via_dispatch),
};

int
main(void)
{
    return cmocka_run_group_tests(tests, NULL, NULL);
}