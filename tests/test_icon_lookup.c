/*
 * test_icon_lookup.c — tests for runtime icon lookup API (Task 18).
 *
 * Tests cover: basic device type lookup, profile icon override (built-in
 * name), absolute path PNG load via SDL2_image, path validation (..,
 * traversal, outside safe dirs), unknown device type fallback, NULL args,
 * cache miss + on-demand load, PNG load failure, override precedence,
 * and label correctness.
 *
 * Uses the headless SDL2 dummy driver via test_harness.  SVG files and
 * the YAML mapping are loaded from the source tree via CBX_SOURCE_DIR.
 */
#include "icons/icon_lookup.h"
#include "icons/icon_cache.h"
#include "icons/icon_map.h"
#include "config/config_paths.h"
#include "test_harness.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <cmocka.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef CBX_SOURCE_DIR
#define CBX_SOURCE_DIR "."
#endif

#define SVG_DIR  CBX_SOURCE_DIR "/data/icons/svg/"
#define YAML_DIR CBX_SOURCE_DIR "/data/"
#define PNG_FIXTURE CBX_SOURCE_DIR "/tests/fixtures/test_icon.png"

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

    char yaml_path[PATH_MAX + 64];
    snprintf(yaml_path, sizeof(yaml_path), "%s/controller-icons.yaml", YAML_DIR);
    if (cbx_icon_map_load(&s->map, yaml_path) != 0) {
        fprintf(stderr, "SETUP: cannot load %s\n", yaml_path);
        test_harness_sdl_shutdown(&s->sdl);
        free(s);
        return -1;
    }

    if (cbx_icon_cache_init(&s->cache, s->sdl.renderer, SVG_DIR, 128) != 0) {
        fprintf(stderr, "SETUP: cannot init icon cache\n");
        test_harness_sdl_shutdown(&s->sdl);
        free(s);
        return -1;
    }

    /* Pre-load all mapped icons into the cache. */
    cbx_icon_cache_load(&s->cache, &s->map);

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

/* --- Basic lookup (no override) ----------------------------------- */

static void test_lookup_known_type(void **state)
{
    struct test_state *s = *state;
    cbx_icon_result res;

    int rc = cbx_icon_lookup(&s->cache, &s->map, "xb360", NULL, &res);
    assert_int_equal(rc, 0);
    assert_non_null(res.texture);
    assert_int_not_equal(res.width, 0);
    assert_int_not_equal(res.height, 0);
    /* The mapping should return a non-empty label. */
    assert_string_not_equal(res.label, "");
}

static void test_lookup_ds5(void **state)
{
    struct test_state *s = *state;
    cbx_icon_result res;

    int rc = cbx_icon_lookup(&s->cache, &s->map, "ds5", NULL, &res);
    assert_int_equal(rc, 0);
    assert_non_null(res.texture);
    assert_string_not_equal(res.label, "");
}

static void test_lookup_deck(void **state)
{
    struct test_state *s = *state;
    cbx_icon_result res;

    int rc = cbx_icon_lookup(&s->cache, &s->map, "deck", NULL, &res);
    assert_int_equal(rc, 0);
    assert_non_null(res.texture);
    assert_string_not_equal(res.label, "");
}

static void test_lookup_unknown_type(void **state)
{
    struct test_state *s = *state;
    cbx_icon_result res;

    int rc = cbx_icon_lookup(&s->cache, &s->map, "nonexistent-controller", NULL, &res);
    assert_int_equal(rc, 0);
    /* Should fall back to generic-gamepad. */
    assert_non_null(res.texture);
    /* Label should be the raw type string for unknown types. */
    assert_string_equal(res.label, "nonexistent-controller");
}

static void test_lookup_null_device_type(void **state)
{
    struct test_state *s = *state;
    cbx_icon_result res;

    int rc = cbx_icon_lookup(&s->cache, &s->map, NULL, NULL, &res);
    assert_int_equal(rc, 0);
    /* Should fall back to generic-gamepad + "unknown" label. */
    assert_non_null(res.texture);
    assert_string_equal(res.label, "unknown");
}

static void test_lookup_empty_device_type(void **state)
{
    struct test_state *s = *state;
    cbx_icon_result res;

    int rc = cbx_icon_lookup(&s->cache, &s->map, "", NULL, &res);
    assert_int_equal(rc, 0);
    assert_non_null(res.texture);
    assert_string_equal(res.label, "unknown");
}

static void test_lookup_null_map(void **state)
{
    struct test_state *s = *state;
    cbx_icon_result res;

    /* With NULL map, all device types are "unknown" → generic gamepad. */
    int rc = cbx_icon_lookup(&s->cache, NULL, "xb360", NULL, &res);
    assert_int_equal(rc, 0);
    assert_non_null(res.texture);
    assert_string_equal(res.label, "xb360");
}

/* --- Profile icon override: built-in name -------------------------- */

static void test_override_builtin_icon(void **state)
{
    struct test_state *s = *state;
    cbx_icon_result res;

    /* Override with a built-in icon name (not a path). */
    int rc = cbx_icon_lookup(&s->cache, &s->map, "xb360", "cc-ps5", &res);
    assert_int_equal(rc, 0);
    assert_non_null(res.texture);
    /* Label comes from the device_type lookup (xb360), not the override. */
    assert_string_not_equal(res.label, "");
}

static void test_override_builtin_unknown_type(void **state)
{
    struct test_state *s = *state;
    cbx_icon_result res;

    /* Override with a built-in icon for an unknown type. */
    int rc = cbx_icon_lookup(&s->cache, &s->map, "mystery-device",
                             "cc-xbox-360", &res);
    assert_int_equal(rc, 0);
    assert_non_null(res.texture);
    /* Label should be the raw type string since the type is unknown. */
    assert_string_equal(res.label, "mystery-device");
}

static void test_override_builtin_nonexistent(void **state)
{
    struct test_state *s = *state;
    cbx_icon_result res;

    /* Override with a nonexistent icon name → fall back to device type. */
    int rc = cbx_icon_lookup(&s->cache, &s->map, "xb360",
                             "nonexistent-icon", &res);
    assert_int_equal(rc, 0);
    assert_non_null(res.texture);
    /* Should fall back to the device type's icon. */
    assert_string_not_equal(res.label, "");
}

static void test_override_empty_string(void **state)
{
    struct test_state *s = *state;
    cbx_icon_result res;

    /* Empty override = no override. */
    int rc = cbx_icon_lookup(&s->cache, &s->map, "xb360", "", &res);
    assert_int_equal(rc, 0);
    assert_non_null(res.texture);
}

/* --- Profile icon override: absolute path PNG ---------------------- */

/* Helper: copy the fixture PNG to the user config dir and return the
 * destination path.  Returns 0 on success, -1 on failure. */
static int copy_png_to_safe_dir(char *out, size_t out_size)
{
    char config_dir[PATH_MAX];
    if (cbx_resolve_config_dir(config_dir, sizeof(config_dir)) != 0)
        return -1;
    cbx_ensure_dir(config_dir, 0700);

    snprintf(out, out_size, "%s/test_icon_lookup.png", config_dir);

    FILE *src_fp = fopen(PNG_FIXTURE, "rb");
    if (!src_fp) return -1;
    FILE *dst_fp = fopen(out, "wb");
    if (!dst_fp) { fclose(src_fp); return -1; }

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src_fp)) > 0)
        fwrite(buf, 1, n, dst_fp);
    fclose(src_fp);
    fclose(dst_fp);
    chmod(out, 0600);
    return 0;
}

static void test_override_png_path(void **state)
{
    struct test_state *s = *state;
    cbx_icon_result res;

    char safe_png[PATH_MAX + 64];
    if (copy_png_to_safe_dir(safe_png, sizeof(safe_png)) != 0) {
        skip();
        return;
    }

    int rc = cbx_icon_lookup(&s->cache, &s->map, "xb360",
                             safe_png, &res);
    assert_int_equal(rc, 0);
    assert_non_null(res.texture);
    assert_int_not_equal(res.width, 0);
    assert_int_not_equal(res.height, 0);

    unlink(safe_png);
}

static void test_override_png_cached(void **state)
{
    struct test_state *s = *state;
    cbx_icon_result res1, res2;

    char safe_png[PATH_MAX + 64];
    if (copy_png_to_safe_dir(safe_png, sizeof(safe_png)) != 0) {
        skip();
        return;
    }

    /* First lookup loads the PNG; second should be cached. */
    int rc1 = cbx_icon_lookup(&s->cache, &s->map, "xb360",
                              safe_png, &res1);
    assert_int_equal(rc1, 0);
    assert_non_null(res1.texture);

    int rc2 = cbx_icon_lookup(&s->cache, &s->map, "xb360",
                              safe_png, &res2);
    assert_int_equal(rc2, 0);
    assert_non_null(res2.texture);
    /* Same texture pointer (cached). */
    assert_ptr_equal(res1.texture, res2.texture);

    unlink(safe_png);
}

static void test_override_png_nonexistent_path(void **state)
{
    struct test_state *s = *state;
    cbx_icon_result res;

    /* Nonexistent PNG path → should fall back to device type icon. */
    int rc = cbx_icon_lookup(&s->cache, &s->map, "xb360",
                             "/nonexistent/path/icon.png", &res);
    assert_int_equal(rc, 0);
    /* Falls back to the xb360 device type icon. */
    assert_non_null(res.texture);
}

static void test_override_png_traversal(void **state)
{
    struct test_state *s = *state;
    cbx_icon_result res;

    /* Path with .. traversal → rejected → falls back to device type. */
    int rc = cbx_icon_lookup(&s->cache, &s->map, "xb360",
                             "/usr/share/../../etc/passwd", &res);
    assert_int_equal(rc, 0);
    /* Should fall back to the device type's icon. */
    assert_non_null(res.texture);
}

static void test_override_png_relative_path(void **state)
{
    struct test_state *s = *state;
    cbx_icon_result res;

    /* Relative path treated as a built-in icon name, not a path. */
    int rc = cbx_icon_lookup(&s->cache, &s->map, "xb360",
                             "relative/path.png", &res);
    assert_int_equal(rc, 0);
    /* Not a known icon name → falls back to device type. */
    assert_non_null(res.texture);
}

/* --- Path validation ----------------------------------------------- */

static void test_validate_path_absolute_safe(void **state)
{
    (void)state;
    /* A path within a safe dir (the user config dir).  Copy the fixture
     * there and validate. */
    char config_dir[PATH_MAX];
    if (cbx_resolve_config_dir(config_dir, sizeof(config_dir)) != 0) {
        skip();
        return;
    }
    cbx_ensure_dir(config_dir, 0700);

    char dest[PATH_MAX + 64];
    snprintf(dest, sizeof(dest), "%s/test_validate.png", config_dir);

    /* Copy fixture. */
    FILE *src_fp = fopen(PNG_FIXTURE, "rb");
    if (!src_fp) { skip(); return; }
    FILE *dst_fp = fopen(dest, "wb");
    if (!dst_fp) { fclose(src_fp); skip(); return; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src_fp)) > 0)
        fwrite(buf, 1, n, dst_fp);
    fclose(src_fp);
    fclose(dst_fp);
    chmod(dest, 0600);

    char resolved[PATH_MAX];
    int rc = cbx_icon_validate_path(dest, resolved, sizeof(resolved));
    assert_int_equal(rc, 0);
    assert_string_equal(resolved, dest);  /* realpath should match (no symlinks) */

    unlink(dest);
}

static void test_validate_path_not_absolute(void **state)
{
    (void)state;
    int rc = cbx_icon_validate_path("relative/path.png", NULL, 0);
    assert_int_equal(rc, -EINVAL);
}

static void test_validate_path_empty(void **state)
{
    (void)state;
    int rc = cbx_icon_validate_path("", NULL, 0);
    assert_int_equal(rc, -EINVAL);
}

static void test_validate_path_null(void **state)
{
    (void)state;
    int rc = cbx_icon_validate_path(NULL, NULL, 0);
    assert_int_equal(rc, -EINVAL);
}

static void test_validate_path_traversal(void **state)
{
    (void)state;
    int rc = cbx_icon_validate_path("/usr/share/../../etc/passwd", NULL, 0);
    assert_int_equal(rc, -EINVAL);
}

static void test_validate_path_double_dot_mid(void **state)
{
    (void)state;
    int rc = cbx_icon_validate_path("/usr/share/safe/../unsafe.png", NULL, 0);
    assert_int_equal(rc, -EINVAL);
}

static void test_validate_path_dotdot_prefix(void **state)
{
    (void)state;
    int rc = cbx_icon_validate_path("../etc/passwd", NULL, 0);
    assert_int_equal(rc, -EINVAL);
}

static void test_validate_path_dotdot_only(void **state)
{
    (void)state;
    int rc = cbx_icon_validate_path("..", NULL, 0);
    assert_int_equal(rc, -EINVAL);
}

/* --- NULL args ---------------------------------------------------- */

static void test_lookup_null_cache(void **state)
{
    struct test_state *s = *state;
    cbx_icon_result res;
    int rc = cbx_icon_lookup(NULL, &s->map, "xb360", NULL, &res);
    assert_int_equal(rc, -EINVAL);
}

static void test_lookup_null_result(void **state)
{
    struct test_state *s = *state;
    int rc = cbx_icon_lookup(&s->cache, &s->map, "xb360", NULL, NULL);
    assert_int_equal(rc, -EINVAL);
}

/* --- Label correctness --------------------------------------------- */

static void test_label_from_map(void **state)
{
    struct test_state *s = *state;
    cbx_icon_result res;

    /* The label for a known type should come from the icon map. */
    (void)cbx_icon_lookup(&s->cache, &s->map, "xb360", NULL, &res);
    /* The map maps xb360 to "Xbox 360" display name. */
    assert_string_not_equal(res.label, "xb360");
    assert_string_not_equal(res.label, "");
}

static void test_label_raw_for_unknown(void **state)
{
    struct test_state *s = *state;
    cbx_icon_result res;

    (void)cbx_icon_lookup(&s->cache, &s->map, "totally-unknown-type-xyz",
                          NULL, &res);
    assert_string_equal(res.label, "totally-unknown-type-xyz");
}

/* --- On-demand load ------------------------------------------------ */

static void test_load_on_demand(void **state)
{
    struct test_state *s = *state;
    cbx_icon_result res;

    /* Use a fresh cache that hasn't been pre-loaded. */
    cbx_icon_cache fresh;
    assert_int_equal(cbx_icon_cache_init(&fresh, s->sdl.renderer, SVG_DIR, 128), 0);

    /* Lookup a known type — should load on demand. */
    int rc = cbx_icon_lookup(&fresh, &s->map, "xb360", NULL, &res);
    assert_int_equal(rc, 0);
    assert_non_null(res.texture);

    cbx_icon_cache_cleanup(&fresh);
}

static void test_override_load_on_demand(void **state)
{
    struct test_state *s = *state;
    cbx_icon_result res;

    /* Use a fresh cache. */
    cbx_icon_cache fresh;
    assert_int_equal(cbx_icon_cache_init(&fresh, s->sdl.renderer, SVG_DIR, 128), 0);

    /* Override with a known icon not yet in the cache. */
    int rc = cbx_icon_lookup(&fresh, &s->map, "xb360", "cc-ps5", &res);
    assert_int_equal(rc, 0);
    assert_non_null(res.texture);

    cbx_icon_cache_cleanup(&fresh);
}

/* --- Dimensions --------------------------------------------------- */

static void test_dims_nonzero(void **state)
{
    struct test_state *s = *state;
    cbx_icon_result res;

    (void)cbx_icon_lookup(&s->cache, &s->map, "xb360", NULL, &res);
    assert_int_not_equal(res.width, 0);
    assert_int_not_equal(res.height, 0);
}

static void test_png_dims(void **state)
{
    struct test_state *s = *state;
    cbx_icon_result res;

    char safe_png[PATH_MAX + 64];
    if (copy_png_to_safe_dir(safe_png, sizeof(safe_png)) != 0) {
        skip();
        return;
    }

    (void)cbx_icon_lookup(&s->cache, &s->map, "xb360", safe_png, &res);
    /* The test PNG is 8x8. */
    assert_int_equal(res.width, 8);
    assert_int_equal(res.height, 8);

    unlink(safe_png);
}

/* --- Safe directory PNG test --------------------------------------- */

static void test_png_in_user_config_dir(void **state)
{
    struct test_state *s = *state;
    cbx_icon_result res;

    char safe_png[PATH_MAX + 64];
    if (copy_png_to_safe_dir(safe_png, sizeof(safe_png)) != 0) {
        skip();
        return;
    }

    int rc = cbx_icon_lookup(&s->cache, &s->map, "xb360", safe_png, &res);
    assert_int_equal(rc, 0);
    assert_non_null(res.texture);
    assert_int_equal(res.width, 8);
    assert_int_equal(res.height, 8);

    unlink(safe_png);
}

/* --- Icon cache insert API ---------------------------------------- */

static void test_cache_insert(void **state)
{
    struct test_state *s = *state;

    /* Create a dummy texture and insert it. */
    SDL_Texture *tex = SDL_CreateTexture(s->sdl.renderer,
                                          SDL_PIXELFORMAT_RGBA8888,
                                          SDL_TEXTUREACCESS_STATIC,
                                          16, 16);
    assert_non_null(tex);

    int rc = cbx_icon_cache_insert(&s->cache, "test-custom-png", tex, 16, 16);
    assert_int_equal(rc, 0);

    /* Verify it's retrievable. */
    SDL_Texture *got = cbx_icon_cache_get(&s->cache, "test-custom-png");
    assert_ptr_equal(got, tex);

    int w, h;
    assert_int_equal(cbx_icon_cache_get_dims(&s->cache, "test-custom-png",
                                              &w, &h), 0);
    assert_int_equal(w, 16);
    assert_int_equal(h, 16);
}

static void test_cache_insert_null_args(void **state)
{
    struct test_state *s = *state;
    SDL_Texture *tex = SDL_CreateTexture(s->sdl.renderer,
                                          SDL_PIXELFORMAT_RGBA8888,
                                          SDL_TEXTUREACCESS_STATIC,
                                          16, 16);
    assert_non_null(tex);

    assert_int_equal(cbx_icon_cache_insert(NULL, "key", tex, 16, 16), -EINVAL);
    assert_int_equal(cbx_icon_cache_insert(&s->cache, NULL, tex, 16, 16), -EINVAL);
    assert_int_equal(cbx_icon_cache_insert(&s->cache, "", tex, 16, 16), -EINVAL);
    assert_int_equal(cbx_icon_cache_insert(&s->cache, "key", NULL, 16, 16), -EINVAL);
    assert_int_equal(cbx_icon_cache_insert(&s->cache, "key", tex, 0, 16), -EINVAL);
    assert_int_equal(cbx_icon_cache_insert(&s->cache, "key", tex, 16, 0), -EINVAL);

    SDL_DestroyTexture(tex);
}

static void test_cache_insert_replace(void **state)
{
    struct test_state *s = *state;

    SDL_Texture *tex1 = SDL_CreateTexture(s->sdl.renderer,
                                            SDL_PIXELFORMAT_RGBA8888,
                                            SDL_TEXTUREACCESS_STATIC,
                                            16, 16);
    assert_non_null(tex1);

    int rc = cbx_icon_cache_insert(&s->cache, "replace-test", tex1, 16, 16);
    assert_int_equal(rc, 0);

    SDL_Texture *tex2 = SDL_CreateTexture(s->sdl.renderer,
                                            SDL_PIXELFORMAT_RGBA8888,
                                            SDL_TEXTUREACCESS_STATIC,
                                            32, 32);
    assert_non_null(tex2);

    /* Insert with same key → should replace. */
    rc = cbx_icon_cache_insert(&s->cache, "replace-test", tex2, 32, 32);
    assert_int_equal(rc, 0);

    SDL_Texture *got = cbx_icon_cache_get(&s->cache, "replace-test");
    assert_ptr_equal(got, tex2);

    int w, h;
    cbx_icon_cache_get_dims(&s->cache, "replace-test", &w, &h);
    assert_int_equal(w, 32);
    assert_int_equal(h, 32);
    /* tex1 was destroyed by insert_entry's replace logic. */
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */

int main(void)
{
    const struct CMUnitTest tests[] = {
        /* Basic lookup (no override) */
        cmocka_unit_test_setup_teardown(test_lookup_known_type, setup, teardown),
        cmocka_unit_test_setup_teardown(test_lookup_ds5, setup, teardown),
        cmocka_unit_test_setup_teardown(test_lookup_deck, setup, teardown),
        cmocka_unit_test_setup_teardown(test_lookup_unknown_type, setup, teardown),
        cmocka_unit_test_setup_teardown(test_lookup_null_device_type, setup, teardown),
        cmocka_unit_test_setup_teardown(test_lookup_empty_device_type, setup, teardown),
        cmocka_unit_test_setup_teardown(test_lookup_null_map, setup, teardown),

        /* Override: built-in name */
        cmocka_unit_test_setup_teardown(test_override_builtin_icon, setup, teardown),
        cmocka_unit_test_setup_teardown(test_override_builtin_unknown_type, setup, teardown),
        cmocka_unit_test_setup_teardown(test_override_builtin_nonexistent, setup, teardown),
        cmocka_unit_test_setup_teardown(test_override_empty_string, setup, teardown),

        /* Override: absolute path PNG */
        cmocka_unit_test_setup_teardown(test_override_png_path, setup, teardown),
        cmocka_unit_test_setup_teardown(test_override_png_cached, setup, teardown),
        cmocka_unit_test_setup_teardown(test_override_png_nonexistent_path, setup, teardown),
        cmocka_unit_test_setup_teardown(test_override_png_traversal, setup, teardown),
        cmocka_unit_test_setup_teardown(test_override_png_relative_path, setup, teardown),

        /* Path validation */
        cmocka_unit_test(test_validate_path_absolute_safe),
        cmocka_unit_test(test_validate_path_not_absolute),
        cmocka_unit_test(test_validate_path_empty),
        cmocka_unit_test(test_validate_path_null),
        cmocka_unit_test(test_validate_path_traversal),
        cmocka_unit_test(test_validate_path_double_dot_mid),
        cmocka_unit_test(test_validate_path_dotdot_prefix),
        cmocka_unit_test(test_validate_path_dotdot_only),

        /* NULL args */
        cmocka_unit_test_setup_teardown(test_lookup_null_cache, setup, teardown),
        cmocka_unit_test_setup_teardown(test_lookup_null_result, setup, teardown),

        /* Label correctness */
        cmocka_unit_test_setup_teardown(test_label_from_map, setup, teardown),
        cmocka_unit_test_setup_teardown(test_label_raw_for_unknown, setup, teardown),

        /* On-demand load */
        cmocka_unit_test_setup_teardown(test_load_on_demand, setup, teardown),
        cmocka_unit_test_setup_teardown(test_override_load_on_demand, setup, teardown),

        /* Dimensions */
        cmocka_unit_test_setup_teardown(test_dims_nonzero, setup, teardown),
        cmocka_unit_test_setup_teardown(test_png_dims, setup, teardown),

        /* Safe directory PNG */
        cmocka_unit_test_setup_teardown(test_png_in_user_config_dir, setup, teardown),

        /* Icon cache insert API */
        cmocka_unit_test_setup_teardown(test_cache_insert, setup, teardown),
        cmocka_unit_test_setup_teardown(test_cache_insert_null_args, setup, teardown),
        cmocka_unit_test_setup_teardown(test_cache_insert_replace, setup, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}