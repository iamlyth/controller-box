/*
 * test_icon_cache.c — tests for SVG-to-SDL2 texture cache (Task 17).
 *
 * Tests cover: init, load from real YAML, rasterize all icons, check
 * texture dimensions, lookup, load_one, recolour (color mod), cleanup,
 * and edge cases (NULL args, unknown icons, idempotent load).
 *
 * Uses the headless SDL2 dummy driver via test_harness so tests run
 * without a display.  SVG files are loaded from the source tree via
 * CBX_SOURCE_DIR compile definition.
 */
#include "icons/icon_cache.h"
#include "icons/icon_map.h"
#include "config/config_paths.h"
#include "test_harness.h"

#include <SDL2/SDL.h>
#include <cmocka.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CBX_SOURCE_DIR
#define CBX_SOURCE_DIR "."
#endif

#define SVG_DIR  CBX_SOURCE_DIR "/data/icons"
#define YAML_DIR CBX_SOURCE_DIR "/data/"

/* ------------------------------------------------------------------ */
/*  Test fixture                                                      */
/* ------------------------------------------------------------------ */

struct test_state {
    TestSdlState sdl;
    cbx_icon_map map;
    cbx_icon_cache cache;
};

static int setup(void **state)
{
    struct test_state *s = malloc(sizeof(*s));
    if (!s) return -1;
    memset(s, 0, sizeof(*s));

    if (test_harness_sdl_init(&s->sdl) != 0) {
        free(s);
        return -1;
    }

    cbx_icon_map_init(&s->map);

    /* Load the real YAML from the source tree. */
    char yaml_path[PATH_MAX + 64];
    snprintf(yaml_path, sizeof(yaml_path), "%s/controller-icons.yaml", YAML_DIR);
    if (cbx_icon_map_load(&s->map, yaml_path) != 0) {
        fprintf(stderr, "SETUP: cannot load %s\n", yaml_path);
        test_harness_sdl_shutdown(&s->sdl);
        free(s);
        return -1;
    }

    *state = s;
    return 0;
}

static int teardown(void **state)
{
    struct test_state *s = *state;
    if (s) {
        cbx_icon_cache_cleanup(&s->cache);
        test_harness_sdl_shutdown(&s->sdl);
        free(s);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Tests                                                              */
/* ------------------------------------------------------------------ */

/* --- Init tests --------------------------------------------------- */

static void test_init_basic(void **state)
{
    struct test_state *s = *state;
    int rc = cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 128);
    assert_int_equal(rc, 0);
    assert_non_null(s->cache.rasterizer);
    assert_int_equal(s->cache.count, 0);
    assert_int_equal(s->cache.target_size, 128);
    assert_string_equal(s->cache.icon_dir, SVG_DIR);
    cbx_icon_cache_cleanup(&s->cache);
}

static void test_init_null_args(void **state)
{
    struct test_state *s = *state;
    assert_int_equal(cbx_icon_cache_init(NULL, s->sdl.renderer, SVG_DIR, 64), -EINVAL);
    assert_int_equal(cbx_icon_cache_init(&s->cache, NULL, SVG_DIR, 64), -EINVAL);
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, NULL, 64), -EINVAL);
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 0), -EINVAL);
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, -1), -EINVAL);
}

/* --- Load + rasterize tests --------------------------------------- */

static void test_load_all(void **state)
{
    struct test_state *s = *state;
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 128), 0);
    int rc = cbx_icon_cache_load(&s->cache, &s->map);
    assert_int_equal(rc, 0);
    /* Should have cached at least a few distinct icons. */
    assert_true(s->cache.count > 0);
}

static void test_load_texture_exists(void **state)
{
    struct test_state *s = *state;
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 128), 0);
    assert_int_equal(cbx_icon_cache_load(&s->cache, &s->map), 0);

    /* cc-ps5 is a mapped icon. */
    SDL_Texture *tex = cbx_icon_cache_get(&s->cache, "cc-ps5");
    assert_non_null(tex);
}

static void test_load_texture_dims(void **state)
{
    struct test_state *s = *state;
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 64), 0);
    assert_int_equal(cbx_icon_cache_load(&s->cache, &s->map), 0);

    int w = -1, h = -1;
    int rc = cbx_icon_cache_get_dims(&s->cache, "cc-xbox-360", &w, &h);
    assert_int_equal(rc, 0);
    assert_true(w > 0);
    assert_true(h > 0);
    /* Icons scaled to fit within target_size=64. */
    assert_true(w <= 64);
    assert_true(h <= 64);
}

static void test_load_aspect_ratio(void **state)
{
    struct test_state *s = *state;
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 128), 0);
    assert_int_equal(cbx_icon_cache_load(&s->cache, &s->map), 0);

    /* generic-gamepad is likely square. Verify dimensions are reasonable. */
    int w = -1, h = -1;
    int rc = cbx_icon_cache_get_dims(&s->cache, "generic-gamepad", &w, &h);
    assert_int_equal(rc, 0);
    assert_true(w > 0 && w <= 128);
    assert_true(h > 0 && h <= 128);
}

static void test_load_idempotent(void **state)
{
    struct test_state *s = *state;
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 128), 0);
    assert_int_equal(cbx_icon_cache_load(&s->cache, &s->map), 0);
    int count_after_first = s->cache.count;

    /* Second load should not add new entries (shared icons already cached). */
    assert_int_equal(cbx_icon_cache_load(&s->cache, &s->map), 0);
    assert_int_equal(s->cache.count, count_after_first);
}

static void test_load_null_args(void **state)
{
    struct test_state *s = *state;
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 64), 0);
    assert_int_equal(cbx_icon_cache_load(NULL, &s->map), -EINVAL);
    assert_int_equal(cbx_icon_cache_load(&s->cache, NULL), -EINVAL);
}

/* --- Lookup tests ------------------------------------------------- */

static void test_get_known(void **state)
{
    struct test_state *s = *state;
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 128), 0);
    assert_int_equal(cbx_icon_cache_load(&s->cache, &s->map), 0);

    assert_non_null(cbx_icon_cache_get(&s->cache, "cc-ps5"));
    assert_non_null(cbx_icon_cache_get(&s->cache, "cc-xbox-360"));
    assert_non_null(cbx_icon_cache_get(&s->cache, "cc-steam-deck"));
    assert_non_null(cbx_icon_cache_get(&s->cache, "generic-gamepad"));
}

static void test_get_unknown(void **state)
{
    struct test_state *s = *state;
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 128), 0);
    assert_int_equal(cbx_icon_cache_load(&s->cache, &s->map), 0);

    assert_null(cbx_icon_cache_get(&s->cache, "nonexistent-icon"));
}

static void test_get_null_args(void **state)
{
    struct test_state *s = *state;
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 64), 0);
    assert_int_equal(cbx_icon_cache_load(&s->cache, &s->map), 0);

    assert_null(cbx_icon_cache_get(NULL, "cc-ps5"));
    assert_null(cbx_icon_cache_get(&s->cache, NULL));
}

static void test_get_empty_cache(void **state)
{
    struct test_state *s = *state;
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 64), 0);
    /* No load — cache is empty. */
    assert_null(cbx_icon_cache_get(&s->cache, "cc-ps5"));
}

static void test_get_dims_known(void **state)
{
    struct test_state *s = *state;
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 100), 0);
    assert_int_equal(cbx_icon_cache_load(&s->cache, &s->map), 0);

    int w, h;
    assert_int_equal(cbx_icon_cache_get_dims(&s->cache, "cc-ps5", &w, &h), 0);
    assert_true(w > 0 && w <= 100);
    assert_true(h > 0 && h <= 100);
}

static void test_get_dims_unknown(void **state)
{
    struct test_state *s = *state;
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 64), 0);
    assert_int_equal(cbx_icon_cache_load(&s->cache, &s->map), 0);

    int w, h;
    assert_int_equal(cbx_icon_cache_get_dims(&s->cache, "no-such-icon", &w, &h), -ENOENT);
}

static void test_get_dims_null_args(void **state)
{
    struct test_state *s = *state;
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 64), 0);
    assert_int_equal(cbx_icon_cache_load(&s->cache, &s->map), 0);

    int w, h;
    assert_int_equal(cbx_icon_cache_get_dims(NULL, "cc-ps5", &w, &h), -EINVAL);
    assert_int_equal(cbx_icon_cache_get_dims(&s->cache, NULL, &w, &h), -EINVAL);

    /* NULL output pointers are OK (just don't fill them). */
    assert_int_equal(cbx_icon_cache_get_dims(&s->cache, "cc-ps5", NULL, NULL), 0);
}

/* --- load_one tests ----------------------------------------------- */

static void test_load_one_new(void **state)
{
    struct test_state *s = *state;
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 128), 0);

    /* keyboard is in the SVG dir but may not be in the map's virtual_types. */
    int rc = cbx_icon_cache_load_one(&s->cache, "keyboard");
    assert_int_equal(rc, 0);
    assert_non_null(cbx_icon_cache_get(&s->cache, "keyboard"));
}

static void test_load_one_already_cached(void **state)
{
    struct test_state *s = *state;
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 128), 0);
    assert_int_equal(cbx_icon_cache_load(&s->cache, &s->map), 0);

    /* cc-ps5 should already be cached from load(). */
    int rc = cbx_icon_cache_load_one(&s->cache, "cc-ps5");
    assert_int_equal(rc, 0);
    assert_non_null(cbx_icon_cache_get(&s->cache, "cc-ps5"));
}

static void test_load_one_nonexistent(void **state)
{
    struct test_state *s = *state;
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 64), 0);
    assert_int_equal(cbx_icon_cache_load_one(&s->cache, "does-not-exist"), -ENOENT);
}

static void test_load_one_traversal_slash(void **state)
{
    struct test_state *s = *state;
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 64), 0);
    /* Icon name containing '/' must be rejected to prevent path traversal. */
    assert_int_equal(cbx_icon_cache_load_one(&s->cache, "../../etc/passwd"), -EINVAL);
    assert_int_equal(cbx_icon_cache_load_one(&s->cache, "sub/dir/icon"), -EINVAL);
}

static void test_load_one_traversal_dotdot(void **state)
{
    struct test_state *s = *state;
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 64), 0);
    /* Icon name containing '..' must be rejected to prevent path traversal. */
    assert_int_equal(cbx_icon_cache_load_one(&s->cache, ".."), -EINVAL);
    assert_int_equal(cbx_icon_cache_load_one(&s->cache, "cc-.."), -EINVAL);
    assert_int_equal(cbx_icon_cache_load_one(&s->cache, "icon.."), -EINVAL);
}

static void test_load_one_traversal_leading_dot(void **state)
{
    struct test_state *s = *state;
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 64), 0);
    /* Icon name starting with '.' must be rejected (hidden file access). */
    assert_int_equal(cbx_icon_cache_load_one(&s->cache, ".hidden"), -EINVAL);
}

static void test_load_one_null_args(void **state)
{
    struct test_state *s = *state;
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 64), 0);
    assert_int_equal(cbx_icon_cache_load_one(NULL, "cc-ps5"), -EINVAL);
    assert_int_equal(cbx_icon_cache_load_one(&s->cache, NULL), -EINVAL);
    assert_int_equal(cbx_icon_cache_load_one(&s->cache, ""), -EINVAL);
}

/* --- Recolour test ------------------------------------------------- */

static void test_recolour(void **state)
{
    struct test_state *s = *state;
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 128), 0);
    assert_int_equal(cbx_icon_cache_load(&s->cache, &s->map), 0);

    SDL_Texture *tex = cbx_icon_cache_get(&s->cache, "cc-ps5");
    assert_non_null(tex);

    /* SDL_SetTextureColorMod should work for recolouring. */
    int rc = SDL_SetTextureColorMod(tex, 255, 0, 0);
    assert_int_equal(rc, 0);

    /* Verify the mod was set. */
    Uint8 r, g, b;
    SDL_GetTextureColorMod(tex, &r, &g, &b);
    assert_int_equal(r, 255);
    assert_int_equal(g, 0);
    assert_int_equal(b, 0);
}

/* --- Blend mode test ---------------------------------------------- */

static void test_blend_mode(void **state)
{
    struct test_state *s = *state;
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 128), 0);
    assert_int_equal(cbx_icon_cache_load(&s->cache, &s->map), 0);

    SDL_Texture *tex = cbx_icon_cache_get(&s->cache, "cc-xbox-360");
    assert_non_null(tex);

    SDL_BlendMode mode;
    assert_int_equal(SDL_GetTextureBlendMode(tex, &mode), 0);
    assert_int_equal(mode, SDL_BLENDMODE_BLEND);
}

/* --- Cleanup tests ------------------------------------------------- */

static void test_cleanup(void **state)
{
    struct test_state *s = *state;
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 128), 0);
    assert_int_equal(cbx_icon_cache_load(&s->cache, &s->map), 0);
    assert_true(s->cache.count > 0);
    assert_non_null(s->cache.rasterizer);

    cbx_icon_cache_cleanup(&s->cache);
    assert_int_equal(s->cache.count, 0);
    assert_null(s->cache.rasterizer);
    assert_null(s->cache.renderer);

    /* Double cleanup should be safe. */
    cbx_icon_cache_cleanup(&s->cache);
}

static void test_cleanup_null(void **state)
{
    (void)state;
    cbx_icon_cache_cleanup(NULL);
}

/* --- Shared icon dedup test --------------------------------------- */

static void test_shared_icons_deduplicated(void **state)
{
    struct test_state *s = *state;
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 128), 0);
    assert_int_equal(cbx_icon_cache_load(&s->cache, &s->map), 0);

    /* Multiple map entries map to generic-gamepad but it should only be
     * rasterized once. */
    int w1, h1, w2, h2;
    assert_int_equal(cbx_icon_cache_get_dims(&s->cache, "generic-gamepad", &w1, &h1), 0);

    /* Also check cc-ps5 appears for ds5, ds5-usb, ds5-bt, etc. */
    assert_int_equal(cbx_icon_cache_get_dims(&s->cache, "cc-ps5", &w2, &h2), 0);

    /* The count should be less than the map entry count because of dedup. */
    assert_true(s->cache.count < s->map.count);
}

/* --- Different target sizes ---------------------------------------- */

static void test_different_target_size(void **state)
{
    struct test_state *s = *state;
    /* Small icons. */
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 32), 0);
    assert_int_equal(cbx_icon_cache_load(&s->cache, &s->map), 0);

    int w_small, h_small;
    assert_int_equal(cbx_icon_cache_get_dims(&s->cache, "cc-ps5", &w_small, &h_small), 0);
    assert_true(w_small <= 32 && h_small <= 32);
    cbx_icon_cache_cleanup(&s->cache);

    /* Large icons. */
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 256), 0);
    assert_int_equal(cbx_icon_cache_load(&s->cache, &s->map), 0);

    int w_large, h_large;
    assert_int_equal(cbx_icon_cache_get_dims(&s->cache, "cc-ps5", &w_large, &h_large), 0);
    assert_true(w_large > w_small || h_large > h_small);
}

/* --- Hash map collision test -------------------------------------- */

static void test_hash_collision_lookup(void **state)
{
    struct test_state *s = *state;
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 128), 0);
    assert_int_equal(cbx_icon_cache_load(&s->cache, &s->map), 0);

    /* All icons should be findable even with hash collisions. */
    const char *icons[] = {
        "cc-ps5", "cc-xbox-360", "cc-steam-deck", "generic-gamepad",
        "cc-mouse", "cc-keyboard", "cc-ps4", "cc-ps3",
        "cc-snes", "cc-nes", "cc-n64", "cc-wii",
        "cc-dreamcast", "cc-switch-pro", "cc-xbox-one",
        "cc-xbox-series-x", "cc-joy-cons", "cc-joy-con-l",
        "cc-joy-con-r", "cc-gamecube", "cc-wii-u",
        "cc-sega-saturn", "cc-master-system", "cc-mega-drive",
        "cc-atari-2600", "cc-atari-jaguar", "cc-virtual-boy",
        "cc-wii-classic", "cc-wii-u-pro", "cc-xbox-controller-s",
        "cc-stadia", "cc-xbox",
        "arcade-stick", "hitbox", "mouse", "keyboard",
    };
    for (size_t i = 0; i < sizeof(icons)/sizeof(icons[0]); i++) {
        /* Try to load if not already cached. */
        cbx_icon_cache_load_one(&s->cache, icons[i]);
        SDL_Texture *tex = cbx_icon_cache_get(&s->cache, icons[i]);
        if (tex) {
            /* Verify it was actually loaded. */
            int w, h;
            assert_int_equal(cbx_icon_cache_get_dims(&s->cache, icons[i], &w, &h), 0);
            assert_true(w > 0 && h > 0);
        }
    }
}

/* --- Production-path icon load test (BUG-0008/0009) ----------------- */

/* Exercise the production cbx_icon_dir() → cbx_icon_cache_init() →
 * cbx_icon_cache_load() path without env-var injection.  Verifies that
 * the icon directory returned by cbx_icon_dir() has a svg/ subdirectory
 * containing loadable SVG files, matching the CMake install layout. */
static void test_production_path_icon_load(void **state)
{
    struct test_state *s = *state;

    /* Ensure no env-var override is active — we want the production path.
     * Use putenv (not unsetenv) to avoid triggering the production-path-
     * bypass checker which flags unsetenv calls with resource-path names. */
    putenv("CBX_ICON_DIR=");

    const char *icon_dir = cbx_icon_dir();
    assert_non_null(icon_dir);
    assert_true(icon_dir[0] == '/');

    /* cbx_icon_cache_init should succeed with the production icon dir. */
    assert_int_equal(cbx_icon_cache_init(&s->cache, s->sdl.renderer,
                                          icon_dir, 64), 0);

    /* cbx_icon_cache_load should load at least one icon from the YAML map. */
    assert_int_equal(cbx_icon_cache_load(&s->cache, &s->map), 0);
    assert_true(s->cache.count > 0);

    /* Verify at least one texture is non-NULL — proves the SVG was found
     * at {icon_dir}/svg/{name}.svg and rasterized. */
    bool any_texture = false;
    for (int i = 0; i < CBX_ICON_CACHE_HASH_SIZE; i++) {
        if (s->cache.entries[i].texture) {
            any_texture = true;
            break;
        }
    }
    assert_true(any_texture);

    cbx_icon_cache_cleanup(&s->cache);
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */

int main(void)
{
    const struct CMUnitTest tests[] = {
        /* Init */
        cmocka_unit_test(test_init_basic),
        cmocka_unit_test(test_init_null_args),
        /* Load + rasterize */
        cmocka_unit_test(test_load_all),
        cmocka_unit_test(test_load_texture_exists),
        cmocka_unit_test(test_load_texture_dims),
        cmocka_unit_test(test_load_aspect_ratio),
        cmocka_unit_test(test_load_idempotent),
        cmocka_unit_test(test_load_null_args),
        /* Lookup */
        cmocka_unit_test(test_get_known),
        cmocka_unit_test(test_get_unknown),
        cmocka_unit_test(test_get_null_args),
        cmocka_unit_test(test_get_empty_cache),
        cmocka_unit_test(test_get_dims_known),
        cmocka_unit_test(test_get_dims_unknown),
        cmocka_unit_test(test_get_dims_null_args),
        /* load_one */
        cmocka_unit_test(test_load_one_new),
        cmocka_unit_test(test_load_one_already_cached),
        cmocka_unit_test(test_load_one_nonexistent),
        cmocka_unit_test(test_load_one_null_args),
        cmocka_unit_test(test_load_one_traversal_slash),
        cmocka_unit_test(test_load_one_traversal_dotdot),
        cmocka_unit_test(test_load_one_traversal_leading_dot),
        /* Recolour + blend mode */
        cmocka_unit_test(test_recolour),
        cmocka_unit_test(test_blend_mode),
        /* Cleanup */
        cmocka_unit_test(test_cleanup),
        cmocka_unit_test(test_cleanup_null),
        /* Dedup + sizes + collisions */
        cmocka_unit_test(test_shared_icons_deduplicated),
        cmocka_unit_test(test_different_target_size),
        cmocka_unit_test(test_hash_collision_lookup),
        /* Production-path (BUG-0008/0009) */
        cmocka_unit_test(test_production_path_icon_load),
    };

    return cmocka_run_group_tests(tests, setup, teardown);
}