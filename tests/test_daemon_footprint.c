/*
 * test_daemon_footprint.c — Daemon memory footprint test (PERF-04, Task 2).
 *
 * Measures the RSS (resident set size) of the overlay service context
 * after initialisation and after 100 idle steps.  Asserts the footprint
 * is bounded (< 50 MB RSS) and that idle stepping does not cause
 * measurable growth (no memory leak in the hot path).
 *
 * RSS is read from /proc/self/statm (field 2 = resident pages,
 * multiplied by sysconf(_SC_PAGESIZE)).
 *
 * SPEC §4.9, §11; implementation-plan Task 2; PERF-04.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "test_harness.h"
#include "fb_assert.h"

#include "overlay/surface_build.h"
#include "overlay/lifecycle.h"
#include "overlay/grid_render.h"
#include "dbus_mock.h"
#include "dbus/ip_composite.h"
#include "dbus/ip_intercept_poll.h"
#include "ui/theme.h"
#include "config/config_settings.h"
#include "config/config_assignments.h"
#include "app/overlay_service.h"

/* --- Constants --------------------------------------------------------- */

#define FOOTPRINT_W   1280
#define FOOTPRINT_H    720
#define IDLE_STEPS     100
#define RSS_LIMIT_MB   50
/* Allow up to 1 MB growth across 100 idle steps (tolerance for allocator
 * fragmentation / page-level rounding, not a leak). */
#define RSS_GROWTH_LIMIT_BYTES  (1 * 1024 * 1024)

/* --- Memory measurement helper ---------------------------------------- */

/*
 * Read the process RSS in bytes from /proc/self/statm.
 * Returns RSS in bytes, or -1 if /proc is unavailable.
 *
 * /proc/self/statm fields (all in pages):
 *   size  resident  shared  text  lib  data  dt
 *
 * We use the "resident" field (column 2) multiplied by the page size.
 */
static long
get_rss_bytes(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f)
        return -1;

    long size_pages = 0, resident_pages = 0;
    int matched = fscanf(f, "%ld %ld", &size_pages, &resident_pages);
    fclose(f);

    if (matched < 2)
        return -1;

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        page_size = 4096;

    return resident_pages * page_size;
}

/* --- Fixture ----------------------------------------------------------- */

typedef struct {
    TestSdlState           sdl;
    cbx_overlay_surface    surface;
    cbx_select_grid        grid;
    cbx_grid_render_ctx    render_ctx;
    cbx_theme              theme;
    cbx_settings           settings;
    cbx_assignments        assignments;
    ip_dbus_mock           mock;
    const ip_dbus_backend *backend;
    cbx_overlay_lifecycle  lifecycle;
} footprint_fixture;

/*
 * Build a minimal grid (1 composite, 1 profile, 4 virtual controllers)
 * so the overlay surface and render context are fully initialised —
 * matching the production daemon's post-startup state.
 */
static void
build_minimal_grid(footprint_fixture *f)
{
    cbx_grid_composite_info comps[1];
    memset(comps, 0, sizeof(comps));
    snprintf(comps[0].id, sizeof(comps[0].id), "TEST:0");
    snprintf(comps[0].model_name, sizeof(comps[0].model_name), "TestPad");
    snprintf(comps[0].composite_path, sizeof(comps[0].composite_path),
             "/org/shadowblip/InputPlumber/CompositeDevice0");

    memset(&f->settings, 0, sizeof(f->settings));
    f->settings.virtual_controllers.count = 4;
    for (int i = 0; i < 4; i++)
        snprintf(f->settings.virtual_controllers.types[i],
                 CBX_MAX_TYPE_LEN, "xb360");

    cbx_assignments_init(&f->assignments);
    cbx_select_grid_init(&f->grid);
    cbx_select_grid_build(&f->grid, comps, 1, &f->settings,
                          &f->assignments);
    cbx_select_grid_add_profile(&f->grid, "Default");
    snprintf(f->grid.rows[0].profile, CBX_GRID_PROFILE_LEN, "Default");

    cbx_theme_default(&f->theme);

    f->render_ctx = (cbx_grid_render_ctx){
        .grid       = &f->grid,
        .icon_cache = NULL,
        .icon_map   = NULL,
        .theme      = &f->theme,
        .text_cache = NULL,
        .font_id    = -1,
        .conflicts  = NULL,
    };
}

static int
footprint_setup(void **state)
{
    footprint_fixture *f = malloc(sizeof(*f));
    assert_non_null(f);
    memset(f, 0, sizeof(*f));

    /* SDL dummy video + software renderer. */
    if (test_harness_sdl_init(&f->sdl) != 0) {
        free(f);
        fail_msg("SDL init failed");
        return -1;
    }

    /* Build the overlay surface at production resolution. */
    int rc = cbx_overlay_surface_init(&f->surface, f->sdl.renderer,
                                       FOOTPRINT_W, FOOTPRINT_H, 1.0);
    assert_int_equal(rc, 0);

    /* Pre-render the surface content once (simulates daemon startup). */
    build_minimal_grid(f);
    cbx_overlay_surface_mark_dirty_all(&f->surface);
    rc = cbx_overlay_surface_render(&f->surface, f->sdl.renderer,
                                     cbx_select_grid_render_cb,
                                     &f->render_ctx);
    assert_int_equal(rc, 0);

    /* Mock DBus. */
    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "InterceptMode", "1");

    /* Lifecycle with instant transitions. */
    cbx_overlay_lifecycle_init(&f->lifecycle, f->backend, f->mock.bus,
                                "/org/shadowblip/InputPlumber/CompositeDevice0",
                                &f->surface, f->sdl.renderer);
    f->lifecycle.fade_in_ms  = 0;
    f->lifecycle.fade_out_ms = 0;
    f->lifecycle.on_save      = NULL;
    f->lifecycle.on_save_data = NULL;

    *state = f;
    return 0;
}

static int
footprint_teardown(void **state)
{
    footprint_fixture *f = *state;
    if (f) {
        cbx_overlay_surface_destroy(&f->surface);
        ip_dbus_mock_reset(&f->mock);
        test_harness_sdl_shutdown(&f->sdl);
        free(f);
    }
    return 0;
}

/* --- Tests ------------------------------------------------------------- */

/*
 * PERF-04: Measure the overlay service context's memory footprint (RSS)
 * after init and after 100 idle steps.  Assert:
 *   1. RSS after init < 50 MB
 *   2. RSS after 100 idle steps < 50 MB
 *   3. Growth across 100 idle steps < 1 MB (no leak in the hot path)
 *
 * The test exercises the production cbx_overlay_service_step() path in
 * idle state (degraded: backend_ready=false, no InputPlumber), matching
 * the daemon's idle footprint.
 */
static void
test_daemon_footprint_bounded(void **state)
{
    footprint_fixture *f = *state;

    /* Allocate a service context — same wiring as the production idle
     * step test in test_overlay_latency.c. */
    cbx_overlay_service_ctx *svc = calloc(1, sizeof(*svc));
    assert_non_null(svc);

    memcpy(&svc->surface, &f->surface, sizeof(f->surface));
    svc->rend.renderer = f->sdl.renderer;
    svc->rend.window   = f->sdl.window;

    ip_connection_init(&svc->conn, f->backend);
    ip_connection_set_bus(&svc->conn, f->mock.bus);

    cbx_settings defaults;
    memset(&defaults, 0, sizeof(defaults));
    defaults.virtual_controllers.count = 1;
    snprintf(defaults.virtual_controllers.types[0],
             CBX_MAX_TYPE_LEN, "xb360");
    memcpy(&svc->settings, &defaults, sizeof(defaults));

    cbx_assignments_init(&svc->assignments);
    cbx_select_grid_init(&svc->grid);
    svc->comp_count = 0;
    svc->poll_count = 0;
    svc->poll_event_type = (uint32_t)-1;
    svc->initialized = true;
    svc->backend_ready = false;

    memcpy(&svc->lifecycle, &f->lifecycle, sizeof(f->lifecycle));
    svc->lifecycle.state = CBX_OVERLAY_IDLE;

    /* --- Measure RSS after init --- */
    long rss_after_init = get_rss_bytes();
    assert_true(rss_after_init > 0);  /* /proc must be available on Linux */

    long rss_limit = (long)RSS_LIMIT_MB * 1024 * 1024;

    printf("  RSS after init: %ld bytes (%.2f MB), limit %d MB\n",
           rss_after_init, (double)rss_after_init / (1024.0 * 1024.0),
           RSS_LIMIT_MB);

    assert_true(rss_after_init < rss_limit);

    /* --- Run 100 idle steps --- */
    for (int i = 0; i < IDLE_STEPS; i++) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
            ;
        cbx_overlay_service_step(svc);
    }

    /* --- Measure RSS after 100 idle steps --- */
    long rss_after_idle = get_rss_bytes();
    assert_true(rss_after_idle > 0);

    long growth = rss_after_idle - rss_after_init;

    printf("  RSS after %d idle steps: %ld bytes (%.2f MB)\n",
           IDLE_STEPS, rss_after_idle,
           (double)rss_after_idle / (1024.0 * 1024.0));
    printf("  Growth: %ld bytes (%.2f KB), limit %d KB\n",
           growth, (double)growth / 1024.0,
           (int)(RSS_GROWTH_LIMIT_BYTES / 1024));

    assert_true(rss_after_idle < rss_limit);
    assert_true(growth < RSS_GROWTH_LIMIT_BYTES);

    /* Verify the overlay remained IDLE throughout. */
    assert_int_equal(svc->lifecycle.state, CBX_OVERLAY_IDLE);

    free(svc);
}

/* --- Main ------------------------------------------------------------- */

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(
            test_daemon_footprint_bounded,
            footprint_setup, footprint_teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}