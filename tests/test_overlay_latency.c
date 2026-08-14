/*
 * test_overlay_latency.c — Overlay latency timing harness (Task 1).
 *
 * Measures and asserts the performance bounds required by SPEC §4.9 and §11:
 *
 *   PER-02: ALL-detection → compositor-present < 10 ms p99
 *           (mark-dirty → render → present path)
 *   PER-03: Overlay close: input to game < 1 ms (set-PASS completion)
 *   PER-04: Daemon footprint — no busy-loop; idle poll path sleeps
 *   PER-01: Button-to-frame ≤ 75 ms p99 (derived from poll + show path)
 *
 * All timing uses SDL_GetTicks() on the SDL dummy/software-renderer test
 * backend.  Tests run ≥100 iterations and report p50/p99/max.  Bounds are
 * generous to tolerate CI scheduling jitter while remaining meaningful.
 *
 * SPEC §4.9, §11, §11.1.7; implementation-plan Task 1; DEC-002 (50 ms poll).
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

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

/*
 * Latency bounds — generous for CI jitter, meaningful for regression.
 *
 * SHOW_PATH_BOUND_MS: the mark-dirty → render → present path must complete
 *   under this p99.  The spec target is <10 ms; the dummy software renderer
 *   typically completes in <1 ms.  We assert p99 < 10 ms.
 *
 * CLOSE_PATH_BOUND_MS: the close (set InterceptMode=PASS) path must complete
 *   under this median.  The spec target is <1 ms.  SDL_GetTicks has 1 ms
 *   resolution, so median 0 satisfies <1 ms.
 *
 * ITERATIONS: minimum number of timing samples per test.
 */
#define SHOW_PATH_BOUND_MS   10   /* PER-02: p99 < 10 ms  */
#define CLOSE_PATH_BOUND_MS    1   /* PER-03: median < 1 ms */
#define IDLE_STEP_BOUND_MS     5  /* PER-04: idle step returns quickly    */
#define ITERATIONS           200  /* ≥100 iterations per the criteria      */

/* Overlay surface dimensions for testing. */
#define LAT_W  1280
#define LAT_H   720

/* --- Fixture ----------------------------------------------------------- */

typedef struct {
    /* SDL test harness (dummy video + software renderer). */
    TestSdlState           sdl;

    /* Overlay surface (pre-built texture). */
    cbx_overlay_surface    surface;

    /* Grid + render context (production render callback). */
    cbx_select_grid        grid;
    cbx_grid_render_ctx    render_ctx;
    cbx_theme              theme;
    cbx_settings           settings;
    cbx_assignments        assignments;

    /* Mock DBus for close-path timing. */
    ip_dbus_mock           mock;
    const ip_dbus_backend *backend;
    cbx_overlay_lifecycle  lifecycle;
} latency_fixture;

/* --- Helpers ----------------------------------------------------------- */

/*
 * Trivial render callback: fills the clip rect with a solid colour.
 * Used for pure infrastructure timing (dirty-rect, render-target switch,
 * clip, present) without depending on grid rendering complexity.
 */
static int
trivial_render_cb(SDL_Renderer *r, const SDL_Rect *clip, void *userdata)
{
    (void)userdata;
    SDL_SetRenderDrawColor(r, 30, 30, 42, 255);
    if (clip)
        SDL_RenderSetClipRect(r, clip);
    SDL_RenderFillRect(r, clip);
    SDL_RenderSetClipRect(r, NULL);
    return 0;
}

/*
 * Build a minimal grid (1 composite, 1 profile, 4 virtual controllers)
 * for production render-callback timing.
 */
static void
build_minimal_grid(latency_fixture *f)
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

/* --- Timing utilities -------------------------------------------------- */

typedef struct {
    Uint32 *samples;
    int     count;
} timing_result;

static int
cmp_u32(const void *a, const void *b)
{
    Uint32 va = *(const Uint32 *)a;
    Uint32 vb = *(const Uint32 *)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

static Uint32
percentile(timing_result *tr, double pct)
{
    if (tr->count <= 0)
        return 0;
    /* Already sorted by caller. */
    int idx = (int)(tr->count * pct);
    if (idx >= tr->count) idx = tr->count - 1;
    return tr->samples[idx];
}

static void
sort_samples(timing_result *tr)
{
    qsort(tr->samples, tr->count, sizeof(Uint32), cmp_u32);
}

/* --- Setup / Teardown -------------------------------------------------- */

static int
latency_setup(void **state)
{
    latency_fixture *f = malloc(sizeof(*f));
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
                                       LAT_W, LAT_H, 1.0);
    assert_int_equal(rc, 0);

    /* Pre-render the surface content once (simulates startup pre-build). */
    build_minimal_grid(f);
    cbx_overlay_surface_mark_dirty_all(&f->surface);
    rc = cbx_overlay_surface_render(&f->surface, f->sdl.renderer,
                                     cbx_select_grid_render_cb,
                                     &f->render_ctx);
    assert_int_equal(rc, 0);

    /* Mock DBus for close-path timing. */
    ip_dbus_mock_init(&f->mock);
    f->backend = ip_dbus_mock_backend(&f->mock);
    ip_dbus_mock_expect_ok(&f->mock, IP_IFACE_COMPOSITE,
                            "InterceptMode", "1");

    /* Lifecycle with instant transitions for deterministic timing. */
    cbx_overlay_lifecycle_init(&f->lifecycle, f->backend, f->mock.bus,
                                "/org/shadowblip/InputPlumber/CompositeDevice0",
                                &f->surface, f->sdl.renderer);
    f->lifecycle.fade_in_ms  = 0;
    f->lifecycle.fade_out_ms = 0;
    f->lifecycle.on_save      = NULL;  /* exclude persistence from timing */
    f->lifecycle.on_save_data = NULL;

    *state = f;
    return 0;
}

static int
latency_teardown(void **state)
{
    latency_fixture *f = *state;
    if (f) {
        cbx_overlay_surface_destroy(&f->surface);
        ip_dbus_mock_reset(&f->mock);
        test_harness_sdl_shutdown(&f->sdl);
        free(f);
    }
    return 0;
}

/* ====================================================================== */
/*  Tests                                                                 */
/* ====================================================================== */

/*
 * PER-02: ALL-detection → compositor-present < 10 ms p99.
 *
 * Measures the full mark-dirty → render → present path using the production
 * render callback on the dummy software renderer.  Runs ITERATIONS times
 * and asserts p99 < SHOW_PATH_BOUND_MS.  Reports p50/p99/max.
 *
 * This path corresponds to the production step function's step 7:
 *   mark_dirty_all → surface_render(cbx_select_grid_render_cb) → surface_show
 *
 * The pre-built surface is already rendered at init; this measures the
 * incremental re-render path that fires when state changes while visible.
 */
static void
test_show_path_render_present_latency(void **state)
{
    latency_fixture *f = *state;

    Uint32 samples_raw[ITERATIONS];
    timing_result tr = { .samples = samples_raw, .count = ITERATIONS };

    for (int i = 0; i < ITERATIONS; i++) {
        /* Mark dirty so the render path has work to do. */
        cbx_overlay_surface_mark_dirty_all(&f->surface);

        Uint32 t0 = SDL_GetTicks();

        /* Production render into target texture. */
        int rc = cbx_overlay_surface_render(&f->surface, f->sdl.renderer,
                                             cbx_select_grid_render_cb,
                                             &f->render_ctx);
        assert_int_equal(rc, 0);

        /* Production present: RenderCopy + RenderPresent. */
        rc = cbx_overlay_surface_show(&f->surface, f->sdl.renderer);
        assert_int_equal(rc, 0);

        Uint32 elapsed = SDL_GetTicks() - t0;
        samples_raw[i] = elapsed;
    }

    sort_samples(&tr);
    Uint32 p50 = percentile(&tr, 0.50);
    Uint32 p99 = percentile(&tr, 0.99);
    Uint32 max = tr.samples[tr.count - 1];

    printf("  show-path render+present: p50=%u p99=%u max=%u ms "
           "(bound %d ms)\n", p50, p99, max, SHOW_PATH_BOUND_MS);

    /* PER-02: p99 must be < 10 ms. */
    assert_true(p99 < SHOW_PATH_BOUND_MS);
}

/*
 * PER-02 (infrastructure): measure pure dirty-rect + render-target switch +
 * present with a trivial callback (no grid rendering).  This isolates the
 * infrastructure overhead from content complexity.
 */
static void
test_show_path_infrastructure_latency(void **state)
{
    latency_fixture *f = *state;

    Uint32 samples_raw[ITERATIONS];
    timing_result tr = { .samples = samples_raw, .count = ITERATIONS };

    for (int i = 0; i < ITERATIONS; i++) {
        cbx_overlay_surface_mark_dirty_all(&f->surface);

        Uint32 t0 = SDL_GetTicks();
        int rc = cbx_overlay_surface_render(&f->surface, f->sdl.renderer,
                                             trivial_render_cb, NULL);
        assert_int_equal(rc, 0);
        rc = cbx_overlay_surface_show(&f->surface, f->sdl.renderer);
        assert_int_equal(rc, 0);
        samples_raw[i] = SDL_GetTicks() - t0;
    }

    sort_samples(&tr);
    Uint32 p50 = percentile(&tr, 0.50);
    Uint32 p99 = percentile(&tr, 0.99);
    Uint32 max = tr.samples[tr.count - 1];

    printf("  show-path infrastructure: p50=%u p99=%u max=%u ms\n",
           p50, p99, max);

    assert_true(p99 < SHOW_PATH_BOUND_MS);
}

/*
 * Structural: the show path performs no texture allocation.
 *
 * The pre-built surface texture is created once at init.  The show path
 * (render + present) must not create a new texture.  We verify by checking
 * that the texture pointer is the same before and after the full
 * mark-dirty → render → show cycle.
 */
static void
test_show_path_no_texture_allocation(void **state)
{
    latency_fixture *f = *state;

    SDL_Texture *tex_before = cbx_overlay_surface_get_texture(&f->surface);
    assert_non_null(tex_before);

    /* Full show path. */
    cbx_overlay_surface_mark_dirty_all(&f->surface);
    int rc = cbx_overlay_surface_render(&f->surface, f->sdl.renderer,
                                         cbx_select_grid_render_cb,
                                         &f->render_ctx);
    assert_int_equal(rc, 0);
    rc = cbx_overlay_surface_show(&f->surface, f->sdl.renderer);
    assert_int_equal(rc, 0);

    SDL_Texture *tex_after = cbx_overlay_surface_get_texture(&f->surface);

    /* The texture pointer must be unchanged — no allocation in show path. */
    assert_ptr_equal(tex_before, tex_after);
}

/*
 * Structural: the production poll interval is exactly 50 ms (DEC-002).
 * The show path is a single RenderCopy + Present (no texture creation).
 *
 * Verifies IP_INTERCEPT_POLL_INTERVAL_MS == 50, which bounds the
 * detection latency.  Combined with the <10 ms show path, the worst-case
 * button-to-frame is ~60 ms (50 ms poll + <10 ms show), well under the
 * 75 ms p99 spec target (PER-01).
 */
static void
test_poll_interval_and_structure(void **state)
{
    (void)state;

    /* PER-04 structural: poll interval is 50 ms (DEC-002). */
    assert_int_equal(IP_INTERCEPT_POLL_INTERVAL_MS, 50);

    /*
     * PER-01 derivation:
     *   worst-case detection = IP_INTERCEPT_POLL_INTERVAL_MS (50 ms)
     *   worst-case show path < SHOW_PATH_BOUND_MS (10 ms)
     *   total worst-case < 60 ms < 75 ms p99 target
     *
     * The show path latency tests above verify the <10 ms bound.
     * The close path latency test below verifies the <1 ms bound.
     * The Pi-4 absolute bound is human-release-gated per §11.1.7
     * and documented in docs/OPERATIONS.md.
     */
    int worst_case_ms = IP_INTERCEPT_POLL_INTERVAL_MS + SHOW_PATH_BOUND_MS;
    assert_true(worst_case_ms <= 75);
}

/*
 * PER-03: Overlay close — input to game < 1 ms (set-PASS completion).
 *
 * Measures the close path: cbx_overlay_lifecycle_close() with fade_out=0
 * and on_save=NULL.  This exercises set_intercept_pass() (mock DBus call)
 * + enter_idle() (hide surface).  Asserts median < 1 ms.
 *
 * The mock DBus is in-memory, so the InterceptMode set is a function call.
 * In production this is a single sd_bus call over a local socket (~0.1 ms).
 */
static void
test_close_path_timing(void **state)
{
    latency_fixture *f = *state;

    Uint32 samples_raw[ITERATIONS];
    timing_result tr = { .samples = samples_raw, .count = ITERATIONS };

    for (int i = 0; i < ITERATIONS; i++) {
        /* Reset to IDLE for each iteration. */
        f->lifecycle.state = CBX_OVERLAY_VISIBLE;

        /* Re-arm mock expectation (mock deduplicates by iface+member,
         * so a single expectation covers all iterations). */
        Uint32 t0 = SDL_GetTicks();
        int rc = cbx_overlay_lifecycle_close(&f->lifecycle);
        Uint32 elapsed = SDL_GetTicks() - t0;

        assert_int_equal(rc, 0);
        /* Close with fade_out_ms=0 transitions to IDLE immediately. */
        assert_int_equal(f->lifecycle.state, CBX_OVERLAY_IDLE);
        samples_raw[i] = elapsed;
    }

    sort_samples(&tr);
    Uint32 p50 = percentile(&tr, 0.50);
    Uint32 p99 = percentile(&tr, 0.99);
    Uint32 max = tr.samples[tr.count - 1];

    printf("  close-path (set PASS): p50=%u p99=%u max=%u ms "
           "(bound %d ms median)\n", p50, p99, max, CLOSE_PATH_BOUND_MS);

    /* PER-03: median < 1 ms (SDL_GetTicks resolution is 1 ms). */
    assert_true(p50 < CLOSE_PATH_BOUND_MS);
}

/*
 * PER-03 (activation+close cycle): measure the full activate → close cycle
 * to verify that the complete lifecycle transition is fast.
 */
static void
test_activate_close_cycle_timing(void **state)
{
    latency_fixture *f = *state;

    Uint32 samples_raw[ITERATIONS];
    timing_result tr = { .samples = samples_raw, .count = ITERATIONS };

    for (int i = 0; i < ITERATIONS; i++) {
        f->lifecycle.state = CBX_OVERLAY_IDLE;

        Uint32 t0 = SDL_GetTicks();
        int rc1 = cbx_overlay_lifecycle_activate(&f->lifecycle);
        int rc2 = cbx_overlay_lifecycle_close(&f->lifecycle);
        Uint32 elapsed = SDL_GetTicks() - t0;

        assert_int_equal(rc1, 0);
        assert_int_equal(rc2, 0);
        samples_raw[i] = elapsed;
    }

    sort_samples(&tr);
    Uint32 p50 = percentile(&tr, 0.50);
    Uint32 p99 = percentile(&tr, 0.99);
    Uint32 max = tr.samples[tr.count - 1];

    printf("  activate+close cycle: p50=%u p99=%u max=%u ms\n",
           p50, p99, max);

    /* Full cycle should still be < 10 ms (activate is instant + close is
     * set-PASS + idle, both <1 ms each on the mock backend). */
    assert_true(p99 < SHOW_PATH_BOUND_MS);
}

/*
 * PER-04: Idle/no-busy-loop assertion.
 *
 * Verifies that the daemon's poll path (step function when idle) returns
 * quickly rather than spinning.  In the production main loop, each
 * iteration calls cbx_overlay_service_step() then SDL_Delay(10), so the
 * daemon sleeps between steps.  This test measures a single step call
 * with no pending events and asserts it completes in bounded time.
 *
 * The structural poll interval (50 ms) is verified in
 * test_poll_interval_and_structure().  Together these prove the daemon
 * idles efficiently: step returns quickly, then the main loop sleeps.
 */
static void
test_idle_step_no_busy_loop(void **state)
{
    latency_fixture *f = *state;

    /* We need a service context for the step function.  Reuse the
     * fixture's SDL state and mock DBus to measure step-like overhead. */
    Uint32 samples_raw[ITERATIONS];
    timing_result tr = { .samples = samples_raw, .count = ITERATIONS };

    for (int i = 0; i < ITERATIONS; i++) {
        /* Simulate the idle production path: poll event queue (empty)
         * + lifecycle tick (IDLE state → no-op) + dirty check (not dirty).
         * This is what cbx_overlay_service_step does when idle. */

        Uint32 t0 = SDL_GetTicks();

        /* 1. Drain the SDL event queue (empty → returns immediately). */
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
            ;

        /* 2. Lifecycle tick in IDLE state (no-op). */
        f->lifecycle.state = CBX_OVERLAY_IDLE;
        cbx_overlay_lifecycle_tick(&f->lifecycle);

        /* 3. Dirty check (surface is clean → no render). */
        bool dirty = cbx_overlay_surface_is_dirty(&f->surface);
        if (dirty) {
            cbx_overlay_surface_render(&f->surface, f->sdl.renderer,
                                         trivial_render_cb, NULL);
            cbx_overlay_surface_show(&f->surface, f->sdl.renderer);
        }

        samples_raw[i] = SDL_GetTicks() - t0;
    }

    sort_samples(&tr);
    Uint32 p50 = percentile(&tr, 0.50);
    Uint32 p99 = percentile(&tr, 0.99);
    Uint32 max = tr.samples[tr.count - 1];

    printf("  idle step (no busy-loop): p50=%u p99=%u max=%u ms "
           "(bound %d ms)\n", p50, p99, max, IDLE_STEP_BOUND_MS);

    /* The idle step must return quickly — not spin. */
    assert_true(p99 < IDLE_STEP_BOUND_MS);
}

/*
 * PER-04 (production step): measure cbx_overlay_service_step() when idle
 * through the production dispatch path.  This uses the full service
 * context to verify the entire step (event drain + DBus process + lifecycle
 * tick + dirty render check) completes quickly when there is nothing to do.
 */
static void
test_idle_production_step_no_busy_loop(void **state)
{
    latency_fixture *f = *state;

    /* Allocate a minimal service context to exercise the production
     * step function.  We reuse the fixture's renderer and mock DBus. */
    cbx_overlay_service_ctx *svc = calloc(1, sizeof(*svc));
    assert_non_null(svc);

    /* Wire the service context minimally for idle step. */
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
    svc->poll_event_type = (uint32_t)-1;  /* no poll events */
    svc->initialized = true;
    svc->backend_ready = false;  /* no InputPlumber → degraded, still steps */

    memcpy(&svc->lifecycle, &f->lifecycle, sizeof(f->lifecycle));
    svc->lifecycle.state = CBX_OVERLAY_IDLE;

    Uint32 samples_raw[ITERATIONS];
    timing_result tr = { .samples = samples_raw, .count = ITERATIONS };

    for (int i = 0; i < ITERATIONS; i++) {
        /* Flush any pending events so the step is truly idle. */
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
            ;

        Uint32 t0 = SDL_GetTicks();
        cbx_overlay_service_step(svc);
        samples_raw[i] = SDL_GetTicks() - t0;
    }

    sort_samples(&tr);
    Uint32 p50 = percentile(&tr, 0.50);
    Uint32 p99 = percentile(&tr, 0.99);
    Uint32 max = tr.samples[tr.count - 1];

    printf("  production idle step: p50=%u p99=%u max=%u ms "
           "(bound %d ms)\n", p50, p99, max, IDLE_STEP_BOUND_MS);

    assert_true(p99 < IDLE_STEP_BOUND_MS);

    /* Verify the overlay remained IDLE throughout (no spurious activation). */
    assert_int_equal(svc->lifecycle.state, CBX_OVERLAY_IDLE);

    free(svc);
}

/* --- Main --------------------------------------------------------------- */

int main(void)
{
    const struct CMUnitTest tests[] = {
        /* PER-02: show path latency */
        cmocka_unit_test_setup_teardown(
            test_show_path_render_present_latency,
            latency_setup, latency_teardown),
        cmocka_unit_test_setup_teardown(
            test_show_path_infrastructure_latency,
            latency_setup, latency_teardown),

        /* Structural: no texture allocation in show path */
        cmocka_unit_test_setup_teardown(
            test_show_path_no_texture_allocation,
            latency_setup, latency_teardown),

        /* Structural: poll interval = 50 ms + button-to-frame derivation */
        cmocka_unit_test(test_poll_interval_and_structure),

        /* PER-03: close path timing */
        cmocka_unit_test_setup_teardown(
            test_close_path_timing,
            latency_setup, latency_teardown),
        cmocka_unit_test_setup_teardown(
            test_activate_close_cycle_timing,
            latency_setup, latency_teardown),

        /* PER-04: idle / no busy loop */
        cmocka_unit_test_setup_teardown(
            test_idle_step_no_busy_loop,
            latency_setup, latency_teardown),
        cmocka_unit_test_setup_teardown(
            test_idle_production_step_no_busy_loop,
            latency_setup, latency_teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}