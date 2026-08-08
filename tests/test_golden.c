/*
 * test_golden.c — Golden image baseline comparison tests (Task 8).
 *
 * Compares live deterministic framebuffer captures against reviewed
 * baseline PNG images in tests/golden/ for every visual state tested
 * in Tasks 5 (overlay) and 7 (manager).
 *
 * Per-pixel tolerance: ±3 per channel.
 * Per-image tolerance: <2% of pixels may differ.
 *
 * On mismatch, actual/expected/diff PNGs are written to
 * tests/golden-fail/ with the test name.
 *
 * To regenerate baselines:
 *   CBX_GENERATE_GOLDEN=1 SDL_VIDEODRIVER=dummy \
 *     ctest --test-dir build-check -R test_golden --output-on-failure
 * Or use:
 *   scripts/generate-golden.sh
 *
 * Baseline update is a manual, reviewed commit — the test does not
 * auto-update baselines.
 */
#include "overlay/conflict.h"
#include "overlay/grid_render.h"
#include "overlay/surface_build.h"
#include "identify/assign.h"
#include "config/config_settings.h"
#include "config/config_assignments.h"
#include "config/config_paths.h"
#include "config/config_profile.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widget.h"
#include "icons/icon_cache.h"
#include "icons/icon_map.h"
#include "manager/manager.h"
#include "manager/controllers_tab.h"
#include "manager/profile_editor_list.h"
#include "manager/profile_editor_seq.h"
#include "manager/profiles_tab.h"
#include "manager/profile_validate.h"
#include "dbus/ip_input_signal.h"
#include "dbus_mock.h"
#include "fb_assert.h"
#include "test_harness.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <cmocka.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>

#ifndef CBX_SOURCE_DIR
#define CBX_SOURCE_DIR "."
#endif

#define SVG_DIR  CBX_SOURCE_DIR "/data/icons/svg/"
#define YAML_DIR CBX_SOURCE_DIR "/data/"
#define GOLDEN_DIR CBX_SOURCE_DIR "/tests/golden/"
#define FAIL_DIR   CBX_SOURCE_DIR "/tests/golden-fail/"

/* ── Golden check helper ─────────────────────────────────────────── */

/*
 * Check a captured framebuffer against a golden baseline, or save it
 * as a new baseline if CBX_GENERATE_GOLDEN is set.
 *
 * Returns true on success (match or saved), false on mismatch.
 */
static bool
golden_check(const uint8_t *buf, int w, int h, const char *name)
{
    char golden_path[PATH_MAX];
    snprintf(golden_path, sizeof(golden_path), "%s%s.png", GOLDEN_DIR, name);

    if (getenv("CBX_GENERATE_GOLDEN")) {
        /* Generate mode: save the baseline. */
        /* Ensure directory exists. */
        mkdir(GOLDEN_DIR, 0755);  /* Ignore if exists. */

        if (fb_save_png(buf, w, h, golden_path) != 0) {
            fprintf(stderr, "  [FAIL] Cannot save golden: %s\n", golden_path);
            return false;
        }
        printf("  [SAVED] %s\n", golden_path);
        return true;
    }

    /* Compare mode. */
    if (access(golden_path, R_OK) != 0) {
        fprintf(stderr, "  [FAIL] Golden baseline not found: %s\n"
                "         Run scripts/generate-golden.sh to create baselines.\n",
                golden_path);
        return false;
    }

    if (fb_golden_compare(buf, w, h, golden_path, 3, 2)) {
        printf("  [ OK ] %s\n", name);
        return true;
    }

    /* Mismatch — save failure artifacts. */
    mkdir(FAIL_DIR, 0755);

    char actual_path[PATH_MAX], expected_path[PATH_MAX], diff_path[PATH_MAX];
    snprintf(actual_path, sizeof(actual_path), "%s%s.actual.png", FAIL_DIR, name);
    snprintf(expected_path, sizeof(expected_path), "%s%s.expected.png", FAIL_DIR, name);
    snprintf(diff_path, sizeof(diff_path), "%s%s.diff.png", FAIL_DIR, name);

    fb_save_png(buf, w, h, actual_path);

    /* Load golden to buffer for expected + diff. */
    SDL_Surface *golden = IMG_Load(golden_path);
    if (golden) {
        SDL_Surface *conv = SDL_ConvertSurfaceFormat(golden,
                                SDL_PIXELFORMAT_ABGR8888, 0);
        SDL_FreeSurface(golden);
        if (conv) {
            fb_save_png((const uint8_t *)conv->pixels, w, h, expected_path);
            fb_save_diff(buf, (const uint8_t *)conv->pixels, w, h, diff_path);
            SDL_FreeSurface(conv);
        }
    }

    fprintf(stderr, "  [FAIL] %s — see %s\n", name, FAIL_DIR);
    return false;
}

/* ════════════════════════════════════════════════════════════════ */
/*  Overlay golden tests (800×600)                                   */
/* ════════════════════════════════════════════════════════════════ */

#define VIS_W 800
#define VIS_H 600

/* Layout constants — must match grid_render.c. */
#define HEADER_H    32
#define LABEL_W     200
#define PROFILE_W   160
#define CELL_MARGIN 4

struct ov_fixture {
    TestSdlState    sdl;
    cbx_text_cache  text_cache;
    int             font_id;
    bool            has_text;
    cbx_icon_cache  icon_cache;
    cbx_icon_map    icon_map;
    cbx_theme       theme;
    bool            has_icons;
};

static const char *
find_font(void)
{
    const char *p = cbx_font_path();
    if (p)
        return p;

    static char found[PATH_MAX];
    const char *candidates[] = {
        "/nix/store/zzs2q7lk5mn6y2rywd3snhak7098zs66-system-path"
            "/share/X11/fonts/DejaVuSans.ttf",
        NULL,
    };
    for (int i = 0; candidates[i]; i++) {
        if (access(candidates[i], R_OK) == 0) {
            snprintf(found, sizeof(found), "%s", candidates[i]);
            return found;
        }
    }

    FILE *fp = popen(
        "find /nix/store -maxdepth 4 -name DejaVuSans.ttf "
        "-path '*/share/X11/fonts/*' 2>/dev/null | head -1",
        "r");
    if (fp) {
        if (fgets(found, sizeof(found), fp) && found[0] != '\0') {
            size_t len = strlen(found);
            if (len > 0 && found[len - 1] == '\n')
                found[len - 1] = '\0';
            pclose(fp);
            if (found[0] != '\0' && access(found, R_OK) == 0)
                return found;
        }
        pclose(fp);
    }

    return NULL;
}

static int
ov_setup(void **state)
{
    struct ov_fixture *f = malloc(sizeof(*f));
    if (!f)
        return -1;
    memset(f, 0, sizeof(*f));
    f->font_id = -1;

    if (test_harness_sdl_init(&f->sdl) != 0) {
        free(f);
        return -1;
    }

    cbx_theme_default(&f->theme);

    if (cbx_text_cache_init(&f->text_cache, f->sdl.renderer) == 0) {
        const char *font = find_font();
        if (font) {
            f->font_id = cbx_text_load_font(&f->text_cache, font, 18);
            if (f->font_id >= 0)
                f->has_text = true;
        }
    }

    cbx_icon_map_init(&f->icon_map);
    char yaml_path[PATH_MAX + 64];
    snprintf(yaml_path, sizeof(yaml_path), "%s/controller-icons.yaml",
             YAML_DIR);
    if (cbx_icon_map_load(&f->icon_map, yaml_path) == 0) {
        if (cbx_icon_cache_init(&f->icon_cache, f->sdl.renderer,
                                SVG_DIR, 64) == 0) {
            cbx_icon_cache_load(&f->icon_cache, &f->icon_map);
            f->has_icons = true;
        }
    }

    *state = f;
    return 0;
}

static int
ov_teardown(void **state)
{
    struct ov_fixture *f = *state;
    if (f) {
        if (f->has_icons)
            cbx_icon_cache_cleanup(&f->icon_cache);
        if (f->text_cache.renderer)
            cbx_text_cache_cleanup(&f->text_cache);
        test_harness_sdl_shutdown(&f->sdl);
        free(f);
    }
    return 0;
}

static cbx_grid_composite_info
make_comp(const char *id, const char *name, const char *path)
{
    cbx_grid_composite_info c;
    memset(&c, 0, sizeof(c));
    if (id)   strncpy(c.id, id, CBX_MAX_ID_LEN - 1);
    if (name) strncpy(c.model_name, name, CBX_MAX_NAME_LEN - 1);
    if (path) strncpy(c.composite_path, path, CBX_MAX_PATH_LEN - 1);
    return c;
}

static void
build_grid(cbx_select_grid *g, int rows)
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
        strncpy(s.virtual_controllers.types[i], "xb360",
                CBX_MAX_TYPE_LEN - 1);

    cbx_assignments a;
    cbx_assignments_init(&a);

    cbx_select_grid_build(g, comps, rows, &s, &a);

    cbx_select_grid_add_profile(g, "default");
    cbx_select_grid_add_profile(g, "fps");
    cbx_select_grid_add_profile(g, "retro");
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

static void
setup_ctx(struct ov_fixture *f, cbx_grid_render_ctx *ctx,
          cbx_select_grid *g, cbx_conflict_list *conflicts)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->grid = g;
    ctx->conflicts = conflicts;
    ctx->theme = &f->theme;
    if (f->has_text) {
        ctx->text_cache = &f->text_cache;
        ctx->font_id = f->font_id;
    }
    if (f->has_icons) {
        ctx->icon_cache = &f->icon_cache;
        ctx->icon_map = &f->icon_map;
    }
}

static uint8_t *
ov_render(struct ov_fixture *f, cbx_grid_render_ctx *ctx)
{
    cbx_overlay_surface surface;
    memset(&surface, 0, sizeof(surface));
    int rc = cbx_overlay_surface_init(&surface, f->sdl.renderer,
                                       VIS_W, VIS_H, 1.0);
    if (rc != 0)
        return NULL;

    cbx_overlay_surface_mark_dirty_all(&surface);
    rc = cbx_overlay_surface_render(&surface, f->sdl.renderer,
                                     cbx_select_grid_render_cb, ctx);
    if (rc != 0) {
        cbx_overlay_surface_destroy(&surface);
        return NULL;
    }

    uint8_t *buf = malloc(VIS_W * VIS_H * 4);
    if (!buf) {
        cbx_overlay_surface_destroy(&surface);
        return NULL;
    }

    SDL_SetRenderTarget(f->sdl.renderer,
                        cbx_overlay_surface_get_texture(&surface));
    rc = fb_read_pixels(f->sdl.renderer, NULL, buf, VIS_W * VIS_H * 4);
    SDL_SetRenderTarget(f->sdl.renderer, NULL);

    cbx_overlay_surface_destroy(&surface);

    if (rc != 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* ── Overlay golden test functions ──────────────────────────────── */

/* 1. Player Mode — 3 controllers on P1, P2, P3 (no conflict). */
static void
test_golden_overlay_player_mode(void **state)
{
    struct ov_fixture *f = *state;

    cbx_select_grid g;
    build_grid(&g, 3);
    move_to_col(&g, 0, 1);
    move_to_col(&g, 1, 2);
    move_to_col(&g, 2, 3);

    cbx_conflict_list conflicts;
    cbx_conflict_detect(&g, &conflicts);

    cbx_grid_render_ctx ctx;
    setup_ctx(f, &ctx, &g, &conflicts);

    uint8_t *buf = ov_render(f, &ctx);
    assert_non_null(buf);

    assert_true(golden_check(buf, VIS_W, VIS_H, "overlay_player_mode"));
    free(buf);
}

/* 2. Host Mode — row 1 moved to P1 (conflict, different highlight). */
static void
test_golden_overlay_host_mode(void **state)
{
    struct ov_fixture *f = *state;

    cbx_select_grid g;
    build_grid(&g, 3);
    move_to_col(&g, 0, 1);
    move_to_col(&g, 1, 1);  /* Same as row 0 → conflict */
    move_to_col(&g, 2, 3);

    cbx_conflict_list conflicts;
    cbx_conflict_detect(&g, &conflicts);

    cbx_grid_render_ctx ctx;
    setup_ctx(f, &ctx, &g, &conflicts);

    uint8_t *buf = ov_render(f, &ctx);
    assert_non_null(buf);

    assert_true(golden_check(buf, VIS_W, VIS_H, "overlay_host_mode"));
    free(buf);
}

/* 3. Conflict — rows 0,1 both on P1, row 2 unassigned. */
static void
test_golden_overlay_conflict(void **state)
{
    struct ov_fixture *f = *state;

    cbx_select_grid g;
    build_grid(&g, 3);
    move_to_col(&g, 0, 1);
    move_to_col(&g, 1, 1);
    /* Row 2 stays on Unassigned. */

    cbx_conflict_list conflicts;
    cbx_conflict_detect(&g, &conflicts);

    cbx_grid_render_ctx ctx;
    setup_ctx(f, &ctx, &g, &conflicts);

    uint8_t *buf = ov_render(f, &ctx);
    assert_non_null(buf);

    assert_true(golden_check(buf, VIS_W, VIS_H, "overlay_conflict"));
    free(buf);
}

/* 4. Unassigned + columns — rows on P1, P2, Unassigned. */
static void
test_golden_overlay_unassigned(void **state)
{
    struct ov_fixture *f = *state;

    cbx_select_grid g;
    build_grid(&g, 3);
    move_to_col(&g, 0, 1);
    move_to_col(&g, 1, 2);
    /* Row 2 stays on Unassigned. */

    cbx_conflict_list conflicts;
    cbx_conflict_detect(&g, &conflicts);

    cbx_grid_render_ctx ctx;
    setup_ctx(f, &ctx, &g, &conflicts);

    uint8_t *buf = ov_render(f, &ctx);
    assert_non_null(buf);

    assert_true(golden_check(buf, VIS_W, VIS_H, "overlay_unassigned"));
    free(buf);
}

/* ════════════════════════════════════════════════════════════════ */
/*  Manager golden tests (1280×720)                                  */
/* ════════════════════════════════════════════════════════════════ */

#define MGR_W    1280
#define MGR_H     720
#define TABBAR_H    48

struct mgr_fixture {
    cbx_manager  mgr;
    bool         has_font;
    uint8_t     *buf;
    char         tmp[256];
    char         saved_home[256];
    bool         saved_home_set;
};

static void
ensure_dummy_driver(void)
{
    SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "dummy",
                            SDL_HINT_OVERRIDE);
}

static int
mgr_setup(void **state)
{
    ensure_dummy_driver();

    struct mgr_fixture *f = malloc(sizeof(*f));
    assert_non_null(f);
    memset(f, 0, sizeof(*f));

    /* Isolated HOME. */
    const char *home = getenv("HOME");
    if (home) {
        snprintf(f->saved_home, sizeof(f->saved_home), "%s", home);
        f->saved_home_set = true;
    }
    snprintf(f->tmp, sizeof(f->tmp), "/tmp/cbx_golden_%d", (int)getpid());
    char cmd[PATH_MAX * 2 + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", f->tmp);
    int r0 = system(cmd);
    (void)r0;
    mkdir(f->tmp, 0700);
    setenv("HOME", f->tmp, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");
    unsetenv("FLATPAK_ID");

    char profiles_dir[PATH_MAX + 64];
    snprintf(profiles_dir, sizeof(profiles_dir),
             "%s/.local/share/inputplumber/profiles", f->tmp);
    cbx_ensure_dir(profiles_dir, 0700);

    const char *font = find_font();
    int rc = cbx_manager_init(&f->mgr, font);
    if (rc != 0) {
        if (f->saved_home_set) setenv("HOME", f->saved_home, 1);
        else unsetenv("HOME");
        free(f);
        return -1;
    }
    f->has_font = (font != NULL && f->mgr.font_id >= 0);

    f->buf = malloc((size_t)MGR_W * MGR_H * 4);
    assert_non_null(f->buf);

    *state = f;
    return 0;
}

static int
mgr_teardown(void **state)
{
    struct mgr_fixture *f = *state;
    if (f) {
        cbx_manager_shutdown(&f->mgr);
        free(f->buf);

        if (f->saved_home_set) setenv("HOME", f->saved_home, 1);
        else unsetenv("HOME");

        char cmd[PATH_MAX * 2 + 32];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", f->tmp);
        int r = system(cmd);
        (void)r;
        free(f);
    }
    return 0;
}

static bool
send_key(cbx_manager *mgr, SDL_Keycode sym)
{
    SDL_Event ev = {0};
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = sym;
    return cbx_manager_handle_event(mgr, &ev);
}

static void
mgr_render_and_read(cbx_manager *mgr, uint8_t *buf)
{
    cbx_manager_render(mgr);
    assert_int_equal(
        fb_read_pixels(mgr->rend.renderer, NULL, buf, MGR_W * MGR_H * 4),
        0);
}

static void
build_test_profile(cbx_profile *p, const char *name, int n_buttons)
{
    cbx_profile_init(p);
    strncpy(p->name, name, sizeof(p->name) - 1);

    static const char *const names[] = {
        "A", "B", "Up", "Down", "Left", "Right"
    };
    if (n_buttons > 6)
        n_buttons = 6;

    for (int i = 0; i < n_buttons; i++) {
        cbx_profile_mapping *m = &p->mappings[p->mapping_count];
        memset(m, 0, sizeof(*m));
        strncpy(m->name, names[i], sizeof(m->name) - 1);
        strncpy(m->source_event.device_class, "gamepad",
                 sizeof(m->source_event.device_class) - 1);
        m->source_event.prop_count = 1;
        strncpy(m->source_event.props[0].key, "button",
                 sizeof(m->source_event.props[0].key) - 1);
        strncpy(m->source_event.props[0].value, names[i],
                 sizeof(m->source_event.props[0].value) - 1);
        m->target_event_count = 1;
        strncpy(m->target_events[0].device_class, "keyboard",
                 sizeof(m->target_events[0].device_class) - 1);
        strncpy(m->target_events[0].value, "KeyA",
                 sizeof(m->target_events[0].value) - 1);
        p->mapping_count++;
    }
}

static const char *CONNECTED_FIXTURE =
    "/org/shadowblip/InputPlumber/Manager\t"
        "org.shadowblip.InputManager\n"
    "/org/shadowblip/InputPlumber/CompositeDevice0\t"
        "org.shadowblip.Input.CompositeDevice\n"
    "/org/shadowblip/InputPlumber/devices/target/gamepad0\t"
        "org.shadowblip.Input.Target,org.shadowblip.Input.Gamepad\n";

/* ── Manager golden test functions ──────────────────────────────── */

/* 5. Controllers tab (degraded mode). */
static void
test_golden_manager_controllers_degraded(void **state)
{
    struct mgr_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;

    assert_int_equal(cbx_manager_active_tab(mgr), CBX_MGR_TAB_CONTROLLERS);
    mgr_render_and_read(mgr, f->buf);

    assert_true(golden_check(f->buf, MGR_W, MGR_H,
                              "manager_controllers_degraded"));
}

/* 6. Controllers tab (connected mode with mock DBus). */
static void
test_golden_manager_controllers_connected(void **state)
{
    struct mgr_fixture *f = *state;
    ensure_dummy_driver();

    ip_dbus_mock mock;
    ip_dbus_mock_init(&mock);
    ip_dbus_mock_expect_ok(&mock, IP_IFACE_OBJECT_MANAGER,
                            "GetManagedObjects", CONNECTED_FIXTURE);
    ip_dbus_mock_expect_ok(&mock, IP_IFACE_MANAGER,
                            "SupportedTargetDeviceIds",
                            "xb360,ds5,deck");
    ip_dbus_mock_expect_ok(&mock, IP_IFACE_TARGET,
                            "DeviceType", "xb360");

    const ip_dbus_backend *be = ip_dbus_mock_backend(&mock);

    cbx_manager mgr2;
    const char *font = f->has_font ? find_font() : NULL;
    int rc = cbx_manager_init_with_dbus(&mgr2, font, be, mock.bus);
    assert_int_equal(rc, 0);

    cbx_controllers_tab *ct = cbx_manager_controllers_tab(&mgr2);
    cbx_controllers_tab_refresh(ct);

    uint8_t *buf = malloc(MGR_W * MGR_H * 4);
    assert_non_null(buf);
    mgr_render_and_read(&mgr2, buf);

    assert_true(golden_check(buf, MGR_W, MGR_H,
                              "manager_controllers_connected"));

    free(buf);
    cbx_manager_shutdown(&mgr2);
    ip_dbus_mock_free(&mock);
}

/* 7. Profiles tab. */
static void
test_golden_manager_profiles(void **state)
{
    struct mgr_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;

    while (cbx_manager_active_tab(mgr) != CBX_MGR_TAB_PROFILES)
        send_key(mgr, SDLK_RIGHT);
    assert_int_equal(cbx_manager_active_tab(mgr), CBX_MGR_TAB_PROFILES);

    mgr_render_and_read(mgr, f->buf);

    assert_true(golden_check(f->buf, MGR_W, MGR_H,
                              "manager_profiles"));
}

/* 8. Settings tab. */
static void
test_golden_manager_settings(void **state)
{
    struct mgr_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;

    while (cbx_manager_active_tab(mgr) != CBX_MGR_TAB_SETTINGS)
        send_key(mgr, SDLK_RIGHT);
    assert_int_equal(cbx_manager_active_tab(mgr), CBX_MGR_TAB_SETTINGS);

    mgr_render_and_read(mgr, f->buf);

    assert_true(golden_check(f->buf, MGR_W, MGR_H,
                              "manager_settings"));
}

/* 9. Profile editor — binding list mode. */

/* Helpers for editor golden tests (production dispatch path) */
static bool
g_send_key(cbx_manager *mgr, SDL_Keycode sym)
{
    SDL_Event ev = {0};
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = sym;
    return cbx_manager_handle_event(mgr, &ev);
}

static void
g_write_nes_profile(const char *home_dir)
{
    char prof_path[PATH_MAX + 64];
    snprintf(prof_path, sizeof(prof_path),
             "%s/.local/share/inputplumber/profiles/testprof.yaml", home_dir);
    FILE *fp = fopen(prof_path, "w");
    if (!fp) return;
    static const char *btns[] = {"A","B","Up","Down","Left","Right"};
    static const char *keys[] = {"KeyA","KeyB","KeyUp","KeyDown","KeyLeft","KeyRight"};
    fprintf(fp, "version: 1\nkind: DeviceProfile\nname: TestProfile\n");
    fprintf(fp, "description: NES test profile\nmapping:\n");
    for (int i = 0; i < 6; i++)
        fprintf(fp, "  - name: btn_%s\n    source_event:\n      gamepad:\n        button: %s\n    target_events:\n      - keyboard: %s\n",
            btns[i], btns[i], keys[i]);
    fclose(fp);
}

static cbx_profile_editor *
g_open_editor(cbx_manager *mgr, const char *home_dir)
{
    g_write_nes_profile(home_dir);
    while (cbx_manager_active_tab(mgr) != CBX_MGR_TAB_PROFILES)
        g_send_key(mgr, SDLK_RIGHT);
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);
    if (!pt || cbx_profiles_tab_profile_count(pt) == 0)
        return NULL;
    SDL_Rect br;
    cbx_widget_get_rect(&pt->edit_btn.base, &br);
    SDL_Event mev = {0};
    mev.type = SDL_MOUSEBUTTONDOWN;
    mev.button.button = SDL_BUTTON_LEFT;
    mev.button.x = br.x + br.w/2; mev.button.y = br.y + br.h/2;
    cbx_manager_handle_event(mgr, &mev);
    mev.type = SDL_MOUSEBUTTONUP;
    cbx_manager_handle_event(mgr, &mev);
    if (pt->mode != CBX_PT_MODE_EDITOR || !pt->editor_initialized)
        return NULL;
    return &pt->editor;
}

static void
test_golden_manager_editor_list(void **state)
{
    struct mgr_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;

    cbx_profile_editor *ed = g_open_editor(mgr, f->tmp);
    assert_non_null(ed);

    cbx_manager_render(mgr);
    assert_int_equal(
        fb_read_pixels(mgr->rend.renderer, NULL, f->buf, MGR_W * MGR_H * 4), 0);

    assert_true(golden_check(f->buf, MGR_W, MGR_H, "manager_editor_list"));
}

/* 10. Profile editor — sequential mode (partial progress). */
static void
test_golden_manager_editor_sequential(void **state)
{
    struct mgr_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;

    cbx_profile_editor *ed = g_open_editor(mgr, f->tmp);
    assert_non_null(ed);

    int rc = cbx_profile_editor_begin_sequential(ed);
    assert_int_equal(rc, 0);

    const char *capture_events[] = { "A", "X", "Y" };
    for (int i = 0; i < 3; i++)
        cbx_profile_editor_seq_on_input(IP_INPUT_A, IP_INPUT_CAT_BUTTON,
                                          1.0, capture_events[i], NULL, ed);
    assert_int_equal(cbx_profile_editor_seq_get_step(ed), 3);

    cbx_manager_render(mgr);
    assert_int_equal(
        fb_read_pixels(mgr->rend.renderer, NULL, f->buf, MGR_W * MGR_H * 4), 0);

    assert_true(golden_check(f->buf, MGR_W, MGR_H,
                              "manager_editor_sequential"));
}

/* 11. Profile editor — validation error. */
static void
test_golden_manager_editor_validation_error(void **state)
{
    struct mgr_fixture *f = *state;
    cbx_manager *mgr = &f->mgr;

    if (!f->has_font) { skip(); return; }

    cbx_profile_editor *ed = g_open_editor(mgr, f->tmp);
    assert_non_null(ed);

    cbx_profile incomplete;
    build_test_profile(&incomplete, "Incomplete", 3);
    char missing[256] = {0};
    cbx_profile_validate_nes_minimum(&incomplete, missing, sizeof(missing));

    char error_msg[512];
    snprintf(error_msg, sizeof(error_msg), "Missing: %s", missing);
    cbx_label_set_text(&ed->status_lbl, error_msg);
    cbx_label_set_color(&ed->status_lbl, mgr->theme.conflict);

    cbx_manager_render(mgr);
    assert_int_equal(
        fb_read_pixels(mgr->rend.renderer, NULL, f->buf, MGR_W * MGR_H * 4), 0);

    assert_true(golden_check(f->buf, MGR_W, MGR_H,
                              "manager_editor_validation_error"));
}

/* ════════════════════════════════════════════════════════════════ */
/*  Main                                                             */
/* ════════════════════════════════════════════════════════════════ */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        /* Overlay golden tests */
        cmocka_unit_test_setup_teardown(
            test_golden_overlay_player_mode, ov_setup, ov_teardown),
        cmocka_unit_test_setup_teardown(
            test_golden_overlay_host_mode, ov_setup, ov_teardown),
        cmocka_unit_test_setup_teardown(
            test_golden_overlay_conflict, ov_setup, ov_teardown),
        cmocka_unit_test_setup_teardown(
            test_golden_overlay_unassigned, ov_setup, ov_teardown),
        /* Manager golden tests */
        cmocka_unit_test_setup_teardown(
            test_golden_manager_controllers_degraded,
            mgr_setup, mgr_teardown),
        cmocka_unit_test_setup_teardown(
            test_golden_manager_controllers_connected,
            mgr_setup, mgr_teardown),
        cmocka_unit_test_setup_teardown(
            test_golden_manager_profiles,
            mgr_setup, mgr_teardown),
        cmocka_unit_test_setup_teardown(
            test_golden_manager_settings,
            mgr_setup, mgr_teardown),
        cmocka_unit_test_setup_teardown(
            test_golden_manager_editor_list,
            mgr_setup, mgr_teardown),
        cmocka_unit_test_setup_teardown(
            test_golden_manager_editor_sequential,
            mgr_setup, mgr_teardown),
        cmocka_unit_test_setup_teardown(
            test_golden_manager_editor_validation_error,
            mgr_setup, mgr_teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}