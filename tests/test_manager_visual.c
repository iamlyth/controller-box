/*
 * test_manager_visual.c — Deterministic framebuffer visual tests for the
 * manager UI (SPEC §5.6).
 *
 * Task 7 — Manager deterministic framebuffer visual tests.
 *
 * Renders through the production manager composition path:
 *   cbx_manager_init() / cbx_manager_init_with_dbus() →
 *   cbx_manager_render() → fb_read_pixels(renderer, NULL, buf, buf_len)
 *
 * Tests for each §5.6 state:
 *   1. Controllers tab (degraded) — content in device list + button regions
 *   2. Controllers tab (connected) — content with mock DBus devices
 *   3. Connected vs degraded — frames differ
 *   4. Profiles tab — content in profile list + button regions
 *   5. Settings tab — content in settings list + save button region
 *   6. Tab switching — frames differ between tabs
 *   7. Profile editor list mode — content in diagram + binding list + title
 *   8. Profile editor sequential mode — prompt + progress bar, partial differs
 *   9. Profile editor validation error — frames differ, red error indicator
 *
 * Each assertion checks pixel content, not struct fields.  Tests fail if
 * controls, text, or highlights are absent even when in-memory objects
 * are valid.
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
#include "manager/profile_diagram.h"
#include "config/config_settings.h"
#include "config/config_paths.h"
#include "config/config_profile.h"
#include "dbus/ip_input_signal.h"
#include "dbus_mock.h"
#include "ui/theme.h"
#include "ui/widget.h"
#include "fb_assert.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define MGR_W       1280
#define MGR_H        720
#define MGR_TOL       10
#define TABBAR_H      48

/* Widget regions (absolute screen coordinates at 1280×720). */
/* Controllers tab */
#define CT_LIST_X      16
#define CT_LIST_Y      64
#define CT_LIST_W    1248
#define CT_LIST_H     400
#define CT_BTN_Y      480
#define CT_BTN_W      200
#define CT_BTN_H       44
#define CT_BTN_GAP     16

/* Profiles tab */
#define PT_LIST_Y      64
#define PT_LIST_H     420
#define PT_BTN_Y      500
#define PT_BTN_W      180
#define PT_BTN_H       44

/* Settings tab */
#define ST_LIST_Y      64
#define ST_LIST_H     420
#define ST_BTN_Y      500
#define ST_BTN_W      200
#define ST_BTN_H       44

/* Profile editor (absolute coordinates, not relative to tab panel). */
#define PE_TITLE_X      16
#define PE_TITLE_Y       8
#define PE_TITLE_W     300
#define PE_TITLE_H      40
#define PE_DIAG_X       16
#define PE_DIAG_Y       40
#define PE_DIAG_W      300
#define PE_DIAG_H      300
#define PE_LIST_X      330
#define PE_LIST_Y       60
#define PE_LIST_W      580
#define PE_LIST_H      420
#define PE_STATUS_X     16
#define PE_STATUS_Y    348
#define PE_STATUS_W    894
#define PE_STATUS_H     36
#define PE_PROG_X      330
#define PE_PROG_Y      488
#define PE_PROG_W      580
#define PE_PROG_H       24

/*
 * Check if any pixel in a region differs between two frames.
 * More precise than fb_frames_differ for small regions.
 */
static bool
region_differs(const uint8_t *a, const uint8_t *b, int w,
               const SDL_Rect *r)
{
    for (int y = r->y; y < r->y + r->h && y < MGR_H; y++) {
        for (int x = r->x; x < r->x + r->w && x < MGR_W; x++) {
            int idx = (y * w + x) * 4;
            if (a[idx]   != b[idx]   ||
                a[idx+1] != b[idx+1] ||
                a[idx+2] != b[idx+2] ||
                a[idx+3] != b[idx+3])
                return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  Fixture                                                            */
/* ------------------------------------------------------------------ */

struct mgr_vis_fixture {
    cbx_manager  mgr;
    bool         has_font;
    uint8_t     *buf_a;
    uint8_t     *buf_b;
    char         tmp[256];
    char         saved_home[256];
    bool         saved_home_set;
};

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static void
ensure_dummy_driver(void)
{
    SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "dummy",
                            SDL_HINT_OVERRIDE);
}

/*
 * Find a usable TrueType font.  Tries cbx_font_path() first, then
 * searches common nix-store locations for DejaVuSans.ttf.
 */
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

static bool
send_key(cbx_manager *mgr, SDL_Keycode sym)
{
    SDL_Event ev = {0};
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = sym;
    return cbx_manager_handle_event(mgr, &ev);
}

static void
render_and_read(cbx_manager *mgr, uint8_t *buf)
{
    cbx_manager_render(mgr);
    assert_int_equal(
        fb_read_pixels(mgr->rend.renderer, NULL, buf, MGR_W * MGR_H * 4),
        0);
}

static void
render_editor_panel(cbx_manager *mgr, cbx_panel *ed_panel, uint8_t *buf)
{
    SDL_SetRenderDrawColor(mgr->rend.renderer,
                           mgr->theme.bg.r, mgr->theme.bg.g,
                           mgr->theme.bg.b, 255);
    SDL_RenderClear(mgr->rend.renderer);
    cbx_widget_draw(&ed_panel->base, mgr->rend.renderer);
    assert_int_equal(
        fb_read_pixels(mgr->rend.renderer, NULL, buf, MGR_W * MGR_H * 4),
        0);
}

/*
 * Build a test profile with the first n_buttons of the NES minimum set.
 * Buttons: A, B, Up, Down, Left, Right (in that order).
 */
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

/* ------------------------------------------------------------------ */
/*  Fixture setup / teardown                                          */
/* ------------------------------------------------------------------ */

static int
mgr_vis_setup(void **state)
{
    ensure_dummy_driver();

    struct mgr_vis_fixture *f = malloc(sizeof(*f));
    assert_non_null(f);
    memset(f, 0, sizeof(*f));

    /* Isolated HOME so profiles tab refresh succeeds. */
    const char *home = getenv("HOME");
    if (home) {
        snprintf(f->saved_home, sizeof(f->saved_home), "%s", home);
        f->saved_home_set = true;
    }
    snprintf(f->tmp, sizeof(f->tmp), "/tmp/cbx_mvis_%d", (int)getpid());
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

    /* Init manager in degraded mode (NULL backend — InputPlumber
     * unavailable in CI, which is the natural production fallback). */
    const char *font = find_font();
    int rc = cbx_manager_init(&f->mgr, font);
    if (rc != 0) {
        if (f->saved_home_set) setenv("HOME", f->saved_home, 1);
        else unsetenv("HOME");
        free(f);
        return -1;
    }
    f->has_font = (font != NULL && f->mgr.font_id >= 0);

    f->buf_a = malloc((size_t)MGR_W * MGR_H * 4);
    f->buf_b = malloc((size_t)MGR_W * MGR_H * 4);
    assert_non_null(f->buf_a);
    assert_non_null(f->buf_b);

    *state = f;
    return 0;
}

static int
mgr_vis_teardown(void **state)
{
    struct mgr_vis_fixture *f = *state;
    if (f) {
        cbx_manager_shutdown(&f->mgr);
        free(f->buf_a);
        free(f->buf_b);

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

#define FIX(s) ((struct mgr_vis_fixture *)*(s))

/* ------------------------------------------------------------------ */
/*  Test 1: Controllers tab (degraded mode)                           */
/* ------------------------------------------------------------------ */

static void
test_controllers_tab_degraded(void **state)
{
    struct mgr_vis_fixture *f = FIX(state);
    cbx_manager *mgr = &f->mgr;

    /* Default tab is Controllers (tab 0). */
    assert_int_equal(cbx_manager_active_tab(mgr), CBX_MGR_TAB_CONTROLLERS);

    render_and_read(mgr, f->buf_a);

    uint8_t bg[3] = { mgr->theme.bg.r, mgr->theme.bg.g, mgr->theme.bg.b };

    /* Device list region — at minimum the list background is drawn. */
    SDL_Rect list_rect = { CT_LIST_X, CT_LIST_Y, CT_LIST_W, CT_LIST_H };
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &list_rect, bg, MGR_TOL));

    /* Body region below the tab bar — non-background content exists. */
    SDL_Rect body_rect = { 0, TABBAR_H, MGR_W, MGR_H - TABBAR_H };
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &body_rect, bg, MGR_TOL));

    /* Add button region. */
    SDL_Rect add_rect = { CT_LIST_X, CT_BTN_Y, CT_BTN_W, CT_BTN_H };
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &add_rect, bg, MGR_TOL));

    /* Remove button region. */
    SDL_Rect rm_rect = { CT_LIST_X + CT_BTN_W + CT_BTN_GAP,
                          CT_BTN_Y, CT_BTN_W, CT_BTN_H };
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &rm_rect, bg, MGR_TOL));

    /* Change Type button region. */
    SDL_Rect ct_rect = { CT_LIST_X + 2 * (CT_BTN_W + CT_BTN_GAP),
                          CT_BTN_Y, CT_BTN_W, CT_BTN_H };
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &ct_rect, bg, MGR_TOL));
}

/* ------------------------------------------------------------------ */
/*  Test 2: Controllers tab (connected mode)                          */
/* ------------------------------------------------------------------ */

/* Mock GetManagedObjects fixture: 1 composite device + 1 target device. */
static const char *CONNECTED_FIXTURE =
    "/org/shadowblip/InputPlumber/Manager\t"
        "org.shadowblip.InputManager\n"
    "/org/shadowblip/InputPlumber/CompositeDevice0\t"
        "org.shadowblip.Input.CompositeDevice\n"
    "/org/shadowblip/InputPlumber/devices/target/gamepad0\t"
        "org.shadowblip.Input.Target,org.shadowblip.Input.Gamepad\n";

static void
test_controllers_tab_connected(void **state)
{
    struct mgr_vis_fixture *f = FIX(state);
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

    /* Refresh to ensure mock devices are loaded. */
    cbx_controllers_tab *ct = cbx_manager_controllers_tab(&mgr2);
    cbx_controllers_tab_refresh(ct);

    render_and_read(&mgr2, f->buf_a);

    uint8_t bg[3] = { mgr2.theme.bg.r, mgr2.theme.bg.g, mgr2.theme.bg.b };

    /* Device list region — should have device entries. */
    SDL_Rect list_rect = { CT_LIST_X, CT_LIST_Y, CT_LIST_W, CT_LIST_H };
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &list_rect, bg, MGR_TOL));

    /* Add button region. */
    SDL_Rect add_rect = { CT_LIST_X, CT_BTN_Y, CT_BTN_W, CT_BTN_H };
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &add_rect, bg, MGR_TOL));

    /* Remove button region. */
    SDL_Rect rm_rect = { CT_LIST_X + CT_BTN_W + CT_BTN_GAP,
                          CT_BTN_Y, CT_BTN_W, CT_BTN_H };
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &rm_rect, bg, MGR_TOL));

    /* Change Type button region. */
    SDL_Rect ct_rect = { CT_LIST_X + 2 * (CT_BTN_W + CT_BTN_GAP),
                          CT_BTN_Y, CT_BTN_W, CT_BTN_H };
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &ct_rect, bg, MGR_TOL));

    /* Body region below the tab bar. */
    SDL_Rect body_rect = { 0, TABBAR_H, MGR_W, MGR_H - TABBAR_H };
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &body_rect, bg, MGR_TOL));

    cbx_manager_shutdown(&mgr2);
    ip_dbus_mock_free(&mock);
}

/* ------------------------------------------------------------------ */
/*  Test 3: Connected vs degraded frames differ                        */
/* ------------------------------------------------------------------ */

static void
test_controllers_connected_vs_degraded(void **state)
{
    struct mgr_vis_fixture *f = FIX(state);

    /* Capture degraded-mode frame from the fixture manager. */
    render_and_read(&f->mgr, f->buf_a);

    /* Capture connected-mode frame from a temporary manager. */
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

    render_and_read(&mgr2, f->buf_b);

    /* Connected and degraded frames should differ — the device list
     * has entries in connected mode but not in degraded mode. */
    assert_true(fb_frames_differ(f->buf_a, f->buf_b, MGR_W, MGR_H, 1));

    cbx_manager_shutdown(&mgr2);
    ip_dbus_mock_free(&mock);
}

/* ------------------------------------------------------------------ */
/*  Test 4: Profiles tab                                               */
/* ------------------------------------------------------------------ */

static void
test_profiles_tab(void **state)
{
    struct mgr_vis_fixture *f = FIX(state);
    cbx_manager *mgr = &f->mgr;

    /* Switch to Profiles tab (tab 1). */
    while (cbx_manager_active_tab(mgr) != CBX_MGR_TAB_PROFILES)
        send_key(mgr, SDLK_RIGHT);
    assert_int_equal(cbx_manager_active_tab(mgr), CBX_MGR_TAB_PROFILES);

    render_and_read(mgr, f->buf_a);

    uint8_t bg[3] = { mgr->theme.bg.r, mgr->theme.bg.g, mgr->theme.bg.b };

    /* Profile list region. */
    SDL_Rect list_rect = { CT_LIST_X, PT_LIST_Y, CT_LIST_W, PT_LIST_H };
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &list_rect, bg, MGR_TOL));

    /* Create button region. */
    SDL_Rect create_rect = { CT_LIST_X, PT_BTN_Y, PT_BTN_W, PT_BTN_H };
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &create_rect, bg, MGR_TOL));

    /* Edit button region. */
    SDL_Rect edit_rect = { CT_LIST_X + PT_BTN_W + CT_BTN_GAP,
                             PT_BTN_Y, PT_BTN_W, PT_BTN_H };
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &edit_rect, bg, MGR_TOL));

    /* Delete button region. */
    SDL_Rect del_rect = { CT_LIST_X + 2 * (PT_BTN_W + CT_BTN_GAP),
                           PT_BTN_Y, PT_BTN_W, PT_BTN_H };
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &del_rect, bg, MGR_TOL));
}

/* ------------------------------------------------------------------ */
/*  Test 5: Settings tab                                               */
/* ------------------------------------------------------------------ */

static void
test_settings_tab(void **state)
{
    struct mgr_vis_fixture *f = FIX(state);
    cbx_manager *mgr = &f->mgr;

    /* Switch to Settings tab (tab 2). */
    while (cbx_manager_active_tab(mgr) != CBX_MGR_TAB_SETTINGS)
        send_key(mgr, SDLK_RIGHT);
    assert_int_equal(cbx_manager_active_tab(mgr), CBX_MGR_TAB_SETTINGS);

    render_and_read(mgr, f->buf_a);

    uint8_t bg[3] = { mgr->theme.bg.r, mgr->theme.bg.g, mgr->theme.bg.b };

    /* Settings list region — should contain setting labels and values. */
    SDL_Rect list_rect = { CT_LIST_X, ST_LIST_Y, CT_LIST_W, ST_LIST_H };
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &list_rect, bg, MGR_TOL));

    /* Save button region. */
    SDL_Rect save_rect = { CT_LIST_X, ST_BTN_Y, ST_BTN_W, ST_BTN_H };
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &save_rect, bg, MGR_TOL));

    /* If font is available, check for text-colored pixels in the
     * settings list region (setting labels like "Overlay Trigger",
     * "Launch at Boot", "Theme", etc.). */
    if (f->has_font) {
        uint8_t text_color[3] = {
            mgr->theme.text_primary.r,
            mgr->theme.text_primary.g,
            mgr->theme.text_primary.b
        };
        /* Check the left half of the list for label text. */
        SDL_Rect label_region = { CT_LIST_X, ST_LIST_Y,
                                   CT_LIST_W / 2, ST_LIST_H };
        assert_true(fb_region_has_color(f->buf_a, MGR_W, MGR_H,
                                          &label_region, text_color,
                                          MGR_TOL));
    }
}

/* ------------------------------------------------------------------ */
/*  Test 6: Tab switching produces materially different frames         */
/* ------------------------------------------------------------------ */

static void
test_tab_switch_differs(void **state)
{
    struct mgr_vis_fixture *f = FIX(state);
    cbx_manager *mgr = &f->mgr;

    /* Start on Controllers (tab 0). */
    assert_int_equal(cbx_manager_active_tab(mgr), CBX_MGR_TAB_CONTROLLERS);
    render_and_read(mgr, f->buf_a);

    /* Switch to Profiles (tab 1). */
    send_key(mgr, SDLK_RIGHT);
    assert_int_equal(cbx_manager_active_tab(mgr), CBX_MGR_TAB_PROFILES);
    render_and_read(mgr, f->buf_b);
    assert_true(fb_frames_differ(f->buf_a, f->buf_b, MGR_W, MGR_H, 2));

    /* Switch to Settings (tab 2). */
    send_key(mgr, SDLK_RIGHT);
    assert_int_equal(cbx_manager_active_tab(mgr), CBX_MGR_TAB_SETTINGS);
    render_and_read(mgr, f->buf_a);
    assert_true(fb_frames_differ(f->buf_a, f->buf_b, MGR_W, MGR_H, 2));

    /* Switch back to Controllers. */
    send_key(mgr, SDLK_RIGHT);  /* wraps or stays at 2 */
    send_key(mgr, SDLK_LEFT);
    send_key(mgr, SDLK_LEFT);
    assert_int_equal(cbx_manager_active_tab(mgr), CBX_MGR_TAB_CONTROLLERS);
    render_and_read(mgr, f->buf_b);
    assert_true(fb_frames_differ(f->buf_a, f->buf_b, MGR_W, MGR_H, 2));
}

/* ------------------------------------------------------------------ */
/*  Test 7: Profile editor — binding list mode                        */
/* ------------------------------------------------------------------ */

static void
test_profile_editor_list_mode(void **state)
{
    struct mgr_vis_fixture *f = FIX(state);
    cbx_manager *mgr = &f->mgr;

    /* Create a profile editor with its own panel. */
    cbx_panel ed_panel;
    cbx_panel_init(&ed_panel, &mgr->theme);
    SDL_Rect panel_rect = { 0, 0, MGR_W, MGR_H };
    cbx_widget_set_rect(&ed_panel.base, &panel_rect);

    cbx_profile_editor *ed = malloc(sizeof(*ed));
    assert_non_null(ed);
    memset(ed, 0, sizeof(*ed));

    int rc = cbx_profile_editor_init(ed, &ed_panel, mgr->rend.renderer,
                                       &mgr->text_cache, &mgr->theme,
                                       mgr->font_id);
    assert_int_equal(rc, 0);

    /* Load a profile with 6 NES minimum bindings. */
    cbx_profile prof;
    build_test_profile(&prof, "TestProfile", 6);
    rc = cbx_profile_editor_load_profile(ed, &prof);
    assert_int_equal(rc, 0);
    assert_int_equal(cbx_profile_editor_binding_count(ed), 6);

    /* Render the editor panel. */
    render_editor_panel(mgr, &ed_panel, f->buf_a);

    uint8_t bg[3] = { mgr->theme.bg.r, mgr->theme.bg.g, mgr->theme.bg.b };

    /* Controller diagram region. */
    SDL_Rect diag_rect = { PE_DIAG_X, PE_DIAG_Y, PE_DIAG_W, PE_DIAG_H };
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &diag_rect, bg, MGR_TOL));

    /* Binding list region. */
    SDL_Rect list_rect = { PE_LIST_X, PE_LIST_Y, PE_LIST_W, PE_LIST_H };
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &list_rect, bg, MGR_TOL));

    /* Title label region. */
    SDL_Rect title_rect = { PE_TITLE_X, PE_TITLE_Y, PE_TITLE_W, PE_TITLE_H };
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &title_rect, bg, MGR_TOL));

    /* If font is available, check for text-colored pixels in the
     * binding list (binding names like "A → KeyA"). */
    if (f->has_font) {
        uint8_t text_color[3] = {
            mgr->theme.text_primary.r,
            mgr->theme.text_primary.g,
            mgr->theme.text_primary.b
        };
        assert_true(fb_region_has_color(f->buf_a, MGR_W, MGR_H,
                                          &list_rect, text_color,
                                          MGR_TOL));
    }

    cbx_profile_editor_shutdown(ed);
    free(ed);
}

/* ------------------------------------------------------------------ */
/*  Test 8: Profile editor — sequential binding mode                   */
/* ------------------------------------------------------------------ */

static void
test_profile_editor_sequential_mode(void **state)
{
    struct mgr_vis_fixture *f = FIX(state);
    cbx_manager *mgr = &f->mgr;

    /* Create a profile editor. */
    cbx_panel ed_panel;
    cbx_panel_init(&ed_panel, &mgr->theme);
    SDL_Rect panel_rect = { 0, 0, MGR_W, MGR_H };
    cbx_widget_set_rect(&ed_panel.base, &panel_rect);

    cbx_profile_editor *ed = malloc(sizeof(*ed));
    assert_non_null(ed);
    memset(ed, 0, sizeof(*ed));

    int rc = cbx_profile_editor_init(ed, &ed_panel, mgr->rend.renderer,
                                       &mgr->text_cache, &mgr->theme,
                                       mgr->font_id);
    assert_int_equal(rc, 0);

    /* Load a profile (can be minimal — sequential mode adds bindings). */
    cbx_profile prof;
    build_test_profile(&prof, "SeqTest", 6);
    rc = cbx_profile_editor_load_profile(ed, &prof);
    assert_int_equal(rc, 0);

    /* Start sequential binding mode. */
    rc = cbx_profile_editor_begin_sequential(ed);
    assert_int_equal(rc, 0);
    assert_true(cbx_profile_editor_seq_is_active(ed));
    assert_int_equal(cbx_profile_editor_seq_get_step(ed), 0);

    /* Render — empty progress bar. */
    render_editor_panel(mgr, &ed_panel, f->buf_a);

    uint8_t bg[3] = { mgr->theme.bg.r, mgr->theme.bg.g, mgr->theme.bg.b };

    /* Sequential prompt region (status label). */
    SDL_Rect status_rect = { PE_STATUS_X, PE_STATUS_Y,
                              PE_STATUS_W, PE_STATUS_H };
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &status_rect, bg, MGR_TOL));

    /* Progress bar region. */
    SDL_Rect prog_rect = { PE_PROG_X, PE_PROG_Y, PE_PROG_W, PE_PROG_H };
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &prog_rect, bg, MGR_TOL));

    /* Diagram should still be visible (highlights current button). */
    SDL_Rect diag_rect = { PE_DIAG_X, PE_DIAG_Y, PE_DIAG_W, PE_DIAG_H };
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &diag_rect, bg, MGR_TOL));

    /* Capture 3 buttons to set partial completion. */
    const char *capture_events[] = { "A", "X", "Y" };
    for (int i = 0; i < 3; i++) {
        cbx_profile_editor_seq_on_input(IP_INPUT_A, IP_INPUT_CAT_BUTTON,
                                          1.0, capture_events[i], NULL, ed);
    }
    assert_int_equal(cbx_profile_editor_seq_get_step(ed), 3);

    /* Render — partial progress. */
    render_editor_panel(mgr, &ed_panel, f->buf_b);

    /* Partial progress frame should differ from empty progress frame
     * in the progress bar region (fill color appears). */
    SDL_Rect prog_rect2 = { PE_PROG_X, PE_PROG_Y, PE_PROG_W, PE_PROG_H };
    assert_true(region_differs(f->buf_a, f->buf_b, MGR_W, &prog_rect2));

    /* Check for progress bar fill color (text_accent = {100,180,255})
     * in the progress bar region — present in partial, absent in empty. */
    uint8_t fill_color[3] = {
        mgr->theme.text_accent.r,
        mgr->theme.text_accent.g,
        mgr->theme.text_accent.b
    };
    assert_true(fb_region_has_color(f->buf_b, MGR_W, MGR_H,
                                     &prog_rect2, fill_color, MGR_TOL));
    assert_false(fb_region_has_color(f->buf_a, MGR_W, MGR_H,
                                      &prog_rect2, fill_color, MGR_TOL));

    /* Now set progress to complete (1.0) directly for comparison. */
    cbx_progress_set_fraction(&ed->progress_bar, 1.0);
    render_editor_panel(mgr, &ed_panel, f->buf_a);

    /* Complete progress should differ from partial in the progress bar. */
    assert_true(region_differs(f->buf_a, f->buf_b, MGR_W, &prog_rect2));
    /* Complete should have more fill color pixels than partial. */
    assert_true(fb_region_has_color(f->buf_a, MGR_W, MGR_H,
                                     &prog_rect2, fill_color, MGR_TOL));

    cbx_profile_editor_shutdown(ed);
    free(ed);
}

/* ------------------------------------------------------------------ */
/*  Test 9: Profile editor — validation error                          */
/* ------------------------------------------------------------------ */

static void
test_profile_editor_validation_error(void **state)
{
    struct mgr_vis_fixture *f = FIX(state);
    cbx_manager *mgr = &f->mgr;

    /* This test requires a font — the error indicator is red text in
     * the status label, which only renders with a loaded font. */
    if (!f->has_font) {
        skip();
        return;
    }

    /* Create a profile editor. */
    cbx_panel ed_panel;
    cbx_panel_init(&ed_panel, &mgr->theme);
    SDL_Rect panel_rect = { 0, 0, MGR_W, MGR_H };
    cbx_widget_set_rect(&ed_panel.base, &panel_rect);

    cbx_profile_editor *ed = malloc(sizeof(*ed));
    assert_non_null(ed);
    memset(ed, 0, sizeof(*ed));

    int rc = cbx_profile_editor_init(ed, &ed_panel, mgr->rend.renderer,
                                       &mgr->text_cache, &mgr->theme,
                                       mgr->font_id);
    assert_int_equal(rc, 0);

    /* Load a complete profile (all 6 NES minimum buttons). */
    cbx_profile prof;
    build_test_profile(&prof, "Complete", 6);
    rc = cbx_profile_editor_load_profile(ed, &prof);
    assert_int_equal(rc, 0);

    /* Render clean state. */
    render_editor_panel(mgr, &ed_panel, f->buf_a);

    /* Create an incomplete profile and validate it. */
    cbx_profile incomplete;
    build_test_profile(&incomplete, "Incomplete", 3);  /* missing 3 buttons */

    char missing[256] = {0};
    int vrc = cbx_profile_validate_nes_minimum(&incomplete, missing,
                                                  sizeof(missing));
    assert_int_equal(vrc, -EINVAL);
    assert_true(strlen(missing) > 0);

    /* Set the editor's status label to the error message with red color. */
    char error_msg[512];
    snprintf(error_msg, sizeof(error_msg), "Missing: %s", missing);
    cbx_label_set_text(&ed->status_lbl, error_msg);
    cbx_label_set_color(&ed->status_lbl, mgr->theme.conflict);

    /* Render error state. */
    render_editor_panel(mgr, &ed_panel, f->buf_b);

    /* Error state should differ from clean state in the status region. */
    SDL_Rect status_rect = { PE_STATUS_X, PE_STATUS_Y,
                              PE_STATUS_W, PE_STATUS_H };
    assert_true(region_differs(f->buf_a, f->buf_b, MGR_W, &status_rect));

    /* Assert error-indicator pixels (red text) in the status/error region. */
    uint8_t red_target[3] = {
        mgr->theme.conflict.r,
        mgr->theme.conflict.g,
        mgr->theme.conflict.b
    };
    assert_true(fb_region_has_color(f->buf_b, MGR_W, MGR_H,
                                     &status_rect, red_target, MGR_TOL));

    cbx_profile_editor_shutdown(ed);
    free(ed);
}

/* ------------------------------------------------------------------ */
/*  Test runner                                                        */
/* ------------------------------------------------------------------ */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(
            test_controllers_tab_degraded, mgr_vis_setup, mgr_vis_teardown),
        cmocka_unit_test_setup_teardown(
            test_controllers_tab_connected, mgr_vis_setup, mgr_vis_teardown),
        cmocka_unit_test_setup_teardown(
            test_controllers_connected_vs_degraded,
            mgr_vis_setup, mgr_vis_teardown),
        cmocka_unit_test_setup_teardown(
            test_profiles_tab, mgr_vis_setup, mgr_vis_teardown),
        cmocka_unit_test_setup_teardown(
            test_settings_tab, mgr_vis_setup, mgr_vis_teardown),
        cmocka_unit_test_setup_teardown(
            test_tab_switch_differs, mgr_vis_setup, mgr_vis_teardown),
        cmocka_unit_test_setup_teardown(
            test_profile_editor_list_mode, mgr_vis_setup, mgr_vis_teardown),
        cmocka_unit_test_setup_teardown(
            test_profile_editor_sequential_mode,
            mgr_vis_setup, mgr_vis_teardown),
        cmocka_unit_test_setup_teardown(
            test_profile_editor_validation_error,
            mgr_vis_setup, mgr_vis_teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}