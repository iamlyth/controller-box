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

static bool
send_key_up(cbx_manager *mgr, SDL_Keycode sym)
{
    SDL_Event ev = {0};
    ev.type = SDL_KEYUP;
    ev.key.keysym.sym = sym;
    return cbx_manager_handle_event(mgr, &ev);
}

static void
send_key_press(cbx_manager *mgr, SDL_Keycode sym)
{
    send_key(mgr, sym);
    send_key_up(mgr, sym);
}

static void
render_and_read(cbx_manager *mgr, uint8_t *buf)
{
    cbx_manager_render(mgr);
    assert_int_equal(
        fb_read_pixels(mgr->rend.renderer, NULL, buf, MGR_W * MGR_H * 4),
        0);
}

static bool
send_mouse_motion(cbx_manager *mgr, int x, int y)
{
    SDL_Event ev = {0};
    ev.type = SDL_MOUSEMOTION;
    ev.motion.x = x;
    ev.motion.y = y;
    return cbx_manager_handle_event(mgr, &ev);
}

static bool
send_mouse_btn_down(cbx_manager *mgr, int x, int y)
{
    SDL_Event ev = {0};
    ev.type = SDL_MOUSEBUTTONDOWN;
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.x = x;
    ev.button.y = y;
    return cbx_manager_handle_event(mgr, &ev);
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

    /* Icon directory is resolved via the production cbx_icon_dir() path
     * (SOURCE_ICON_DIR fallback in config_paths.c) — no env-var injection. */

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

    /* Backend-changing controls are absent and an actionable degraded
     * status is rendered instead. */
    SDL_Rect actions_rect = { CT_LIST_X, CT_BTN_Y,
                              3 * CT_BTN_W + 2 * CT_BTN_GAP, CT_BTN_H };
    assert_false(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                        &actions_rect, bg, MGR_TOL));
    SDL_Rect status_rect = { CT_LIST_X, CT_BTN_Y + CT_BTN_H + CT_BTN_GAP,
                             CT_LIST_W, CT_BTN_H };
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &status_rect, bg, MGR_TOL));
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
/*  Test 5b: Per-setting visual regions (MV-04)                      */
/* ------------------------------------------------------------------ */

static void
test_settings_per_setting_visual(void **state)
{
    struct mgr_vis_fixture *f = FIX(state);
    cbx_manager *mgr = &f->mgr;
    cbx_settings_tab *st = cbx_manager_settings_tab(mgr);

    /* Switch to Settings tab. */
    while (cbx_manager_active_tab(mgr) != CBX_MGR_TAB_SETTINGS)
        send_key(mgr, SDLK_RIGHT);

    uint8_t bg[3] = { mgr->theme.bg.r, mgr->theme.bg.g, mgr->theme.bg.b };

    /* Render initial frame. */
    render_and_read(mgr, f->buf_a);

    /* Each setting row should have meaningful non-background content. */
    int item_h = st->settings_list.item_h;
    assert_true(item_h > 0);
    int list_x = st->settings_list.base.rect.x;
    int list_w = st->settings_list.base.rect.w;
    int list_y = st->settings_list.base.rect.y;

    for (int i = 0; i < CBX_ST_SET_COUNT; i++) {
        SDL_Rect row_rect = { list_x, list_y + i * item_h,
                               list_w, item_h };
        assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                           &row_rect, bg, MGR_TOL));
    }

    /* Save button region should also have content. */
    SDL_Rect btn_rect = st->save_btn.base.rect;
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &btn_rect, bg, MGR_TOL));
}

/* Test 5c: Editing a setting changes its row region (MV-04). */
static void
test_settings_edit_changes_region(void **state)
{
    struct mgr_vis_fixture *f = FIX(state);
    cbx_manager *mgr = &f->mgr;
    cbx_settings_tab *st = cbx_manager_settings_tab(mgr);

    /* Switch to Settings tab. */
    while (cbx_manager_active_tab(mgr) != CBX_MGR_TAB_SETTINGS)
        send_key(mgr, SDLK_RIGHT);

    /* Navigate to theme row (index 1) and enter edit mode. */
    send_key(mgr, SDLK_DOWN);  /* tabbar -> list */
    send_key(mgr, SDLK_DOWN);  /* item 1 = theme */
    render_and_read(mgr, f->buf_a);

    /* Record the theme row region before editing. */
    int item_h = st->settings_list.item_h;
    int list_x = st->settings_list.base.rect.x;
    int list_w = st->settings_list.base.rect.w;
    int list_y = st->settings_list.base.rect.y;
    SDL_Rect theme_rect = { list_x, list_y + 1 * item_h, list_w, item_h };

    /* Enter edit mode (KEYDOWN + KEYUP to fire on_select -> activate). */
    send_key_press(mgr, SDLK_a);
    assert_int_equal(cbx_settings_tab_mode(st), CBX_ST_MODE_EDIT);

    /* Cycle the theme value. */
    send_key(mgr, SDLK_UP);  /* default -> dark */

    /* Render after edit and compare. */
    render_and_read(mgr, f->buf_b);

    /* The theme row region should differ (value label changed). */
    assert_true(region_differs(f->buf_a, f->buf_b, MGR_W,
                                    &theme_rect));
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


/* Helpers for editor tests (production dispatch path) */

static bool
vis_send_key(cbx_manager *mgr, SDL_Keycode sym)
{
    SDL_Event ev = {0};
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = sym;
    return cbx_manager_handle_event(mgr, &ev);
}

/* Select the profile entry with the given filename (base, no extension) in
 * the profiles list.  The default selection (index 0) is environment-
 * dependent because enumeration sorts the built-in Default alongside any
 * host/system InputPlumber profiles; resolve the entry we wrote instead
 * (BUG-0017). */
static bool
vis_select_profile(cbx_profiles_tab *pt, const char *filename)
{
    if (!pt)
        return false;
    for (int i = 0; i < pt->profiles.count; i++) {
        if (strcmp(pt->profiles.entries[i].filename, filename) == 0) {
            pt->selected_profile = i;
            cbx_list_set_selected(&pt->profile_list_w, i);
            return true;
        }
    }
    return false;
}

static void
vis_write_nes_profile(const char *home_dir)
{
    char prof_path[PATH_MAX + 64];
    snprintf(prof_path, sizeof(prof_path),
             "%s/.local/share/inputplumber/profiles/testprof.yaml",
             home_dir);
    FILE *fp = fopen(prof_path, "w");
    if (!fp)
        return;
    static const char *btns[] = {"A", "B", "Up", "Down", "Left", "Right"};
    static const char *keys[] = {"KeyA", "KeyB", "KeyUp", "KeyDown",
                                 "KeyLeft", "KeyRight"};
    fprintf(fp, "version: 1\nkind: DeviceProfile\nname: TestProfile\n");
    fprintf(fp, "description: NES test profile\nmapping:\n");
    for (int i = 0; i < 6; i++)
        fprintf(fp,
            "  - name: btn_%s\n"
            "    source_event:\n"
            "      gamepad:\n"
            "        button: %s\n"
            "    target_events:\n"
            "      - keyboard: %s\n",
            btns[i], btns[i], keys[i]);
    fclose(fp);
}

static cbx_profile_editor *
vis_open_editor(cbx_manager *mgr, const char *home_dir)
{
    vis_write_nes_profile(home_dir);
    while (cbx_manager_active_tab(mgr) != CBX_MGR_TAB_PROFILES)
        vis_send_key(mgr, SDLK_RIGHT);
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);
    if (!pt || cbx_profiles_tab_profile_count(pt) == 0)
        return NULL;
    if (!vis_select_profile(pt, "testprof"))
        return NULL;
    SDL_Rect btn_rect;
    cbx_widget_get_rect(&pt->edit_btn.base, &btn_rect);
    int cx = btn_rect.x + btn_rect.w / 2;
    int cy = btn_rect.y + btn_rect.h / 2;
    SDL_Event mev = {0};
    mev.type = SDL_MOUSEBUTTONDOWN;
    mev.button.button = SDL_BUTTON_LEFT;
    mev.button.x = cx; mev.button.y = cy;
    cbx_manager_handle_event(mgr, &mev);
    mev.type = SDL_MOUSEBUTTONUP;
    cbx_manager_handle_event(mgr, &mev);
    if (pt->mode != CBX_PT_MODE_EDITOR || !pt->editor_initialized)
        return NULL;
    return &pt->editor;
}
/* ------------------------------------------------------------------ */
/*  Test 7: Profile editor — binding list mode                        */
/* ------------------------------------------------------------------ */

static void
test_profile_editor_list_mode(void **state)
{
    struct mgr_vis_fixture *f = FIX(state);
    cbx_manager *mgr = &f->mgr;

    /* Open the editor via the production Edit-button path. */
    cbx_profile_editor *ed = vis_open_editor(mgr, f->tmp);
    assert_non_null(ed);
    assert_int_equal(cbx_profile_editor_binding_count(ed), 6);

    /* Render through the production manager render path. */
    cbx_manager_render(mgr);
    assert_int_equal(
        fb_read_pixels(mgr->rend.renderer, NULL, f->buf_a, MGR_W * MGR_H * 4),
        0);

    uint8_t bg[3] = { mgr->theme.bg.r, mgr->theme.bg.g, mgr->theme.bg.b };

    /* Use dynamic rect lookup for region checks. */
    SDL_Rect diag_rect;
    cbx_widget_get_rect(&ed->diagram.base, &diag_rect);
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &diag_rect, bg, MGR_TOL));

    /* BUG-0007: verify the controller outline SVG was loaded (base_texture
     * non-NULL) and that the diagram region has substantial content
     * (not just sparse label pixels from the broken state). */
    assert_non_null(ed->diagram.base_texture);
    /* With the SVG loaded, the diagram region should have content in
     * multiple sub-regions (not just one sparse row).  Check the top
     * half and bottom half separately. */
    {
        SDL_Rect diag_top = diag_rect;
        diag_top.h /= 2;
        assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                           &diag_top, bg, MGR_TOL));
        SDL_Rect diag_bot = diag_rect;
        diag_bot.y += diag_bot.h / 2;
        diag_bot.h -= diag_bot.h / 2;
        assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                           &diag_bot, bg, MGR_TOL));
    }

    SDL_Rect list_rect;
    cbx_widget_get_rect(&ed->binding_list.base, &list_rect);
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &list_rect, bg, MGR_TOL));

    SDL_Rect title_rect;
    cbx_widget_get_rect(&ed->title_lbl.base, &title_rect);
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &title_rect, bg, MGR_TOL));

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
}

/* ------------------------------------------------------------------ */
/*  Test 8: Profile editor — sequential binding mode                   */
/* ------------------------------------------------------------------ */

static void
test_profile_editor_sequential_mode(void **state)
{
    struct mgr_vis_fixture *f = FIX(state);
    cbx_manager *mgr = &f->mgr;

    /* Open the editor via the production Edit-button path. */
    cbx_profile_editor *ed = vis_open_editor(mgr, f->tmp);
    assert_non_null(ed);

    /* Start sequential binding mode (via editor API after production open). */
    int rc = cbx_profile_editor_begin_sequential(ed);
    assert_int_equal(rc, 0);
    assert_true(cbx_profile_editor_seq_is_active(ed));
    assert_int_equal(cbx_profile_editor_seq_get_step(ed), 0);

    /* Render empty-progress state. */
    cbx_manager_render(mgr);
    assert_int_equal(
        fb_read_pixels(mgr->rend.renderer, NULL, f->buf_a, MGR_W * MGR_H * 4),
        0);

    uint8_t bg[3] = { mgr->theme.bg.r, mgr->theme.bg.g, mgr->theme.bg.b };

    SDL_Rect status_rect;
    cbx_widget_get_rect(&ed->status_lbl.base, &status_rect);
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &status_rect, bg, MGR_TOL));

    SDL_Rect prog_rect;
    cbx_widget_get_rect(&ed->progress_bar.base, &prog_rect);
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &prog_rect, bg, MGR_TOL));

    SDL_Rect diag_rect;
    cbx_widget_get_rect(&ed->diagram.base, &diag_rect);
    assert_true(fb_region_has_content(f->buf_a, MGR_W, MGR_H,
                                       &diag_rect, bg, MGR_TOL));
    /* BUG-0007: verify SVG loaded in sequential mode too. */
    assert_non_null(ed->diagram.base_texture);

    /* Capture 3 buttons for partial completion. */
    const char *capture_events[] = { "A", "X", "Y" };
    for (int i = 0; i < 3; i++) {
        cbx_profile_editor_seq_on_input(IP_INPUT_A, IP_INPUT_CAT_BUTTON,
                                          1.0, capture_events[i], NULL, ed);
    }
    assert_int_equal(cbx_profile_editor_seq_get_step(ed), 3);

    /* Render partial-progress state. */
    cbx_manager_render(mgr);
    assert_int_equal(
        fb_read_pixels(mgr->rend.renderer, NULL, f->buf_b, MGR_W * MGR_H * 4),
        0);

    assert_true(region_differs(f->buf_a, f->buf_b, MGR_W, &prog_rect));

    uint8_t fill_color[3] = {
        mgr->theme.text_accent.r,
        mgr->theme.text_accent.g,
        mgr->theme.text_accent.b
    };
    assert_true(fb_region_has_color(f->buf_b, MGR_W, MGR_H,
                                     &prog_rect, fill_color, MGR_TOL));

    /* Set progress to complete and render. */
    cbx_progress_set_fraction(&ed->progress_bar, 1.0);
    cbx_manager_render(mgr);
    assert_int_equal(
        fb_read_pixels(mgr->rend.renderer, NULL, f->buf_a, MGR_W * MGR_H * 4),
        0);

    assert_true(region_differs(f->buf_a, f->buf_b, MGR_W, &prog_rect));
    assert_true(fb_region_has_color(f->buf_a, MGR_W, MGR_H,
                                     &prog_rect, fill_color, MGR_TOL));
}

/* ------------------------------------------------------------------ */
/*  Test 9: Profile editor — validation error                          */
/* ------------------------------------------------------------------ */

static void
test_profile_editor_validation_error(void **state)
{
    struct mgr_vis_fixture *f = FIX(state);
    cbx_manager *mgr = &f->mgr;

    if (!f->has_font) {
        skip();
        return;
    }

    /* Open the editor via the production Edit-button path. */
    cbx_profile_editor *ed = vis_open_editor(mgr, f->tmp);
    assert_non_null(ed);

    /* Render clean state. */
    cbx_manager_render(mgr);
    assert_int_equal(
        fb_read_pixels(mgr->rend.renderer, NULL, f->buf_a, MGR_W * MGR_H * 4),
        0);

    /* Load an incomplete profile into the in-editor profile so the
     * NES-minimum validation fails on save (not a detached struct). */
    build_test_profile(&ed->profile, "Incomplete", 3);

    /* Click the real Save button through the production pointer path so
     * on_save → cbx_profiles_tab_save_editor → cbx_profile_save_to_dir
     * validates the in-editor profile and surfaces the error via the
     * status label — the error visual is not injected manually. */
    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);
    SDL_Rect save_rect;
    cbx_widget_get_rect(&pt->save_btn.base, &save_rect);
    SDL_Event mev = {0};
    mev.type = SDL_MOUSEBUTTONDOWN;
    mev.button.button = SDL_BUTTON_LEFT;
    mev.button.x = save_rect.x + save_rect.w / 2;
    mev.button.y = save_rect.y + save_rect.h / 2;
    cbx_manager_handle_event(mgr, &mev);
    mev.type = SDL_MOUSEBUTTONUP;
    cbx_manager_handle_event(mgr, &mev);

    /* Render error state. */
    cbx_manager_render(mgr);
    assert_int_equal(
        fb_read_pixels(mgr->rend.renderer, NULL, f->buf_b, MGR_W * MGR_H * 4),
        0);

    SDL_Rect status_rect;
    cbx_widget_get_rect(&ed->status_lbl.base, &status_rect);
    assert_true(region_differs(f->buf_a, f->buf_b, MGR_W, &status_rect));

    uint8_t red_target[3] = {
        mgr->theme.conflict.r,
        mgr->theme.conflict.g,
        mgr->theme.conflict.b
    };
    assert_true(fb_region_has_color(f->buf_b, MGR_W, MGR_H,
                                     &status_rect, red_target, MGR_TOL));
}

/* ------------------------------------------------------------------
 *  Task 5: Hover/press visual indication (framebuffer readback)
 * ------------------------------------------------------------------ */

/* Assert that focusing a control produces a visible change in the
 * framebuffer — the focus highlight (brighter background + focus
 * border) must be rendered, not just stored in the widget struct
 * (SPEC §5.6).  Uses mouse click to focus (focus-follows-pointer). */
static void
test_focus_visual_indication(void **state)
{
    struct mgr_vis_fixture *f = FIX(state);
    cbx_manager *mgr = &f->mgr;

    /* Switch to Profiles tab (buttons are always interactive there). */
    send_key(mgr, SDLK_RIGHT); /* Controllers -> Profiles */
    assert_int_equal(cbx_manager_active_tab(mgr), CBX_MGR_TAB_PROFILES);

    /* Baseline: tabbar focused. */
    render_and_read(mgr, f->buf_a);

    /* Switch to Settings tab and use the Save button (clicking it
     * saves settings, which is harmless in an empty test HOME). */
    send_key(mgr, SDLK_RIGHT); /* Profiles -> Settings */
    assert_int_equal(cbx_manager_active_tab(mgr), CBX_MGR_TAB_SETTINGS);
    cbx_settings_tab *st = cbx_manager_settings_tab(mgr);

    /* Baseline: Save button not focused. */
    render_and_read(mgr, f->buf_a);

    /* Mouse motion + click on Save button to focus it. */
    SDL_Rect btn_rect;
    cbx_widget_get_rect(&st->save_btn.base, &btn_rect);
    int cx = btn_rect.x + btn_rect.w / 2;
    int cy = btn_rect.y + btn_rect.h / 2;

    /* Just send MOUSEMOTION to hover, then MOUSEBUTTONDOWN to focus.
     * Don't send MOUSEBUTTONUP to avoid triggering the save action. */
    send_mouse_motion(mgr, cx, cy);
    send_mouse_btn_down(mgr, cx, cy);
    assert_true(st->save_btn.base.focused);
    assert_true(st->save_btn.pressed);

    /* Render with button focused + pressed. */
    render_and_read(mgr, f->buf_b);

    /* The Save button region should differ — focused+pressed button
     * has text_accent background instead of panel_bg. */
    assert_true(region_differs(f->buf_a, f->buf_b, MGR_W, &btn_rect));
}

/* Assert that pressing a button (mouse button down) produces a visible
 * change in the framebuffer — the pressed state (accent-tinted
 * background) must be rendered (SPEC §5.6). */
static void
test_press_visual_indication(void **state)
{
    struct mgr_vis_fixture *f = FIX(state);
    cbx_manager *mgr = &f->mgr;

    /* Switch to Profiles tab. */
    send_key(mgr, SDLK_RIGHT); /* Controllers -> Profiles */
    assert_int_equal(cbx_manager_active_tab(mgr), CBX_MGR_TAB_PROFILES);

    cbx_profiles_tab *pt = cbx_manager_profiles_tab(mgr);

    /* Baseline: render the tab normally. */
    render_and_read(mgr, f->buf_a);

    /* Send mouse motion + button down on the Create button (without
     * releasing) — this sets focused=true and pressed=true. */
    SDL_Rect btn_rect;
    cbx_widget_get_rect(&pt->create_btn.base, &btn_rect);
    int cx = btn_rect.x + btn_rect.w / 2;
    int cy = btn_rect.y + btn_rect.h / 2;

    /* Hover (mouse motion) over the button. */
    send_mouse_motion(mgr, cx, cy);
    assert_true(pt->create_btn.base.hover);

    /* Press (mouse button down) without release. */
    send_mouse_btn_down(mgr, cx, cy);
    assert_true(pt->create_btn.pressed);

    /* Render with button pressed. */
    render_and_read(mgr, f->buf_b);

    /* The button region should differ — pressed button uses
     * text_accent background instead of panel_bg. */
    assert_true(region_differs(f->buf_a, f->buf_b, MGR_W, &btn_rect));
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
            test_settings_per_setting_visual, mgr_vis_setup, mgr_vis_teardown),
        cmocka_unit_test_setup_teardown(
            test_settings_edit_changes_region, mgr_vis_setup, mgr_vis_teardown),
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

        /* Task 5: Hover/press visual indication */
        cmocka_unit_test_setup_teardown(
            test_focus_visual_indication, mgr_vis_setup, mgr_vis_teardown),
        cmocka_unit_test_setup_teardown(
            test_press_visual_indication, mgr_vis_setup, mgr_vis_teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}