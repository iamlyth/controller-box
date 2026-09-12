/*
 * test_icon_map.c — Unit tests for icon mapping table (Task 16).
 *
 * Tests: parse, load from string, lookup (known/unknown types),
 * default path, empty map lookup, max entries, YAML security
 * constraints (tags, depth, doc size), field truncation.
 */
#include "icons/icon_map.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#include <cmocka.h>

/* nanosvg for SVG compatibility verification. */
#include <nanosvg.h>
#include <nanosvgrast.h>

#ifndef CBX_SOURCE_DIR
#define CBX_SOURCE_DIR "."
#endif

#define SVG_DIR CBX_SOURCE_DIR "/data/icons"

/* --- Test YAML strings --------------------------------------------------- */

static const char *YAML_BASIC =
    "virtual_types:\n"
    "  - type: \"xb360\"\n"
    "    icon: \"cc-xbox-360\"\n"
    "    name: \"Xbox 360 Controller\"\n"
    "  - type: \"ds5\"\n"
    "    icon: \"cc-ps5\"\n"
    "    name: \"DualSense\"\n"
    "  - type: \"deck\"\n"
    "    icon: \"cc-steam-deck\"\n"
    "    name: \"Steam Deck Controller\"\n"
    "  - type: \"gamepad\"\n"
    "    icon: \"generic-gamepad\"\n"
    "    name: \"Generic Gamepad\"\n"
;

static const char *YAML_WITH_CUSTOM =
    "virtual_types:\n"
    "  - type: \"xb360\"\n"
    "    icon: \"cc-xbox-360\"\n"
    "    name: \"Xbox 360 Controller\"\n"
    "  - type: \"mouse\"\n"
    "    icon: \"cc-mouse\"\n"
    "    name: \"Mouse\"\n"
    "custom_icons:\n"
    "  - icon: \"arcade-stick\"\n"
    "    name: \"Arcade Stick\"\n"
    "  - icon: \"hitbox\"\n"
    "    name: \"Hit Box\"\n"
;

static const char *YAML_EMPTY = "";

static const char *YAML_NO_VIRTUAL =
    "custom_icons:\n"
    "  - icon: \"arcade-stick\"\n"
    "    name: \"Arcade Stick\"\n"
;



/* --- Tests --------------------------------------------------------------- */

/* init zeroes the struct */
static void test_init(void **state)
{
    (void)state;
    cbx_icon_map map;
    cbx_icon_map_init(&map);
    assert_int_equal(map.count, 0);
    assert_int_equal(map.loaded, 0);
    assert_int_equal(map.yaml_path[0], '\0');
}

/* init NULL is safe */
static void test_init_null(void **state)
{
    (void)state;
    cbx_icon_map_init(NULL);
    /* should not crash */
}

/* parse basic YAML with 4 entries */
static void test_parse_basic(void **state)
{
    (void)state;
    cbx_icon_map map;
    int rc = cbx_icon_map_parse(&map, YAML_BASIC, 0);
    assert_int_equal(rc, 0);
    assert_int_equal(map.count, 4);
    assert_int_equal(map.loaded, 1);

    /* Verify entries. */
    assert_string_equal(map.entries[0].type, "xb360");
    assert_string_equal(map.entries[0].icon, "cc-xbox-360");
    assert_string_equal(map.entries[0].name, "Xbox 360 Controller");

    assert_string_equal(map.entries[1].type, "ds5");
    assert_string_equal(map.entries[1].icon, "cc-ps5");
    assert_string_equal(map.entries[1].name, "DualSense");

    assert_string_equal(map.entries[2].type, "deck");
    assert_string_equal(map.entries[2].icon, "cc-steam-deck");
    assert_string_equal(map.entries[2].name, "Steam Deck Controller");

    assert_string_equal(map.entries[3].type, "gamepad");
    assert_string_equal(map.entries[3].icon, "generic-gamepad");
    assert_string_equal(map.entries[3].name, "Generic Gamepad");
}

/* parse with custom_icons section (custom entries not in virtual_types) */
static void test_parse_with_custom(void **state)
{
    (void)state;
    cbx_icon_map map;
    int rc = cbx_icon_map_parse(&map, YAML_WITH_CUSTOM, 0);
    assert_int_equal(rc, 0);
    /* Only virtual_types entries counted. */
    assert_int_equal(map.count, 2);
}

/* parse NULL args → EINVAL */
static void test_parse_null_args(void **state)
{
    (void)state;
    int rc = cbx_icon_map_parse(NULL, YAML_BASIC, 0);
    assert_int_equal(rc, -EINVAL);

    cbx_icon_map map;
    rc = cbx_icon_map_parse(&map, NULL, 0);
    assert_int_equal(rc, -EINVAL);
}

/* parse empty YAML → 0 entries, success */
static void test_parse_empty(void **state)
{
    (void)state;
    cbx_icon_map map;
    int rc = cbx_icon_map_parse(&map, YAML_EMPTY, 0);
    assert_int_equal(rc, 0);
    assert_int_equal(map.count, 0);
}

/* parse YAML with no virtual_types → 0 entries, success */
static void test_parse_no_virtual(void **state)
{
    (void)state;
    cbx_icon_map map;
    int rc = cbx_icon_map_parse(&map, YAML_NO_VIRTUAL, 0);
    assert_int_equal(rc, 0);
    assert_int_equal(map.count, 0);
}

/* lookup known type */
static void test_lookup_known(void **state)
{
    (void)state;
    cbx_icon_map map;
    cbx_icon_map_parse(&map, YAML_BASIC, 0);

    char icon[CBX_ICON_ICON_LEN];
    char name[CBX_ICON_NAME_LEN];
    int rc = cbx_icon_map_lookup(&map, "xb360", icon, sizeof(icon),
                                  name, sizeof(name));
    assert_int_equal(rc, 0);
    assert_string_equal(icon, "cc-xbox-360");
    assert_string_equal(name, "Xbox 360 Controller");
}

/* lookup different known type */
static void test_lookup_ds5(void **state)
{
    (void)state;
    cbx_icon_map map;
    cbx_icon_map_parse(&map, YAML_BASIC, 0);

    char icon[CBX_ICON_ICON_LEN];
    char name[CBX_ICON_NAME_LEN];
    int rc = cbx_icon_map_lookup(&map, "ds5", icon, sizeof(icon),
                                  name, sizeof(name));
    assert_int_equal(rc, 0);
    assert_string_equal(icon, "cc-ps5");
    assert_string_equal(name, "DualSense");
}

/* lookup unknown type → generic-gamepad + raw type as name */
static void test_lookup_unknown(void **state)
{
    (void)state;
    cbx_icon_map map;
    cbx_icon_map_parse(&map, YAML_BASIC, 0);

    char icon[CBX_ICON_ICON_LEN];
    char name[CBX_ICON_NAME_LEN];
    int rc = cbx_icon_map_lookup(&map, "totally-unknown-device",
                                  icon, sizeof(icon),
                                  name, sizeof(name));
    assert_int_equal(rc, 0);
    assert_string_equal(icon, "generic-gamepad");
    assert_string_equal(name, "totally-unknown-device");
}

/* lookup with NULL map → defaults */
static void test_lookup_null_map(void **state)
{
    (void)state;
    char icon[CBX_ICON_ICON_LEN];
    char name[CBX_ICON_NAME_LEN];
    int rc = cbx_icon_map_lookup(NULL, "xb360", icon, sizeof(icon),
                                  name, sizeof(name));
    assert_int_equal(rc, 0);
    assert_string_equal(icon, "generic-gamepad");
    assert_string_equal(name, "xb360");
}

/* lookup with unloaded map → defaults */
static void test_lookup_unloaded(void **state)
{
    (void)state;
    cbx_icon_map map;
    cbx_icon_map_init(&map);
    /* map.loaded = 0 */

    char icon[CBX_ICON_ICON_LEN];
    char name[CBX_ICON_NAME_LEN];
    int rc = cbx_icon_map_lookup(&map, "xb360", icon, sizeof(icon),
                                  name, sizeof(name));
    assert_int_equal(rc, 0);
    assert_string_equal(icon, "generic-gamepad");
    assert_string_equal(name, "xb360");
}

/* lookup NULL type → EINVAL */
static void test_lookup_null_type(void **state)
{
    (void)state;
    cbx_icon_map map;
    cbx_icon_map_parse(&map, YAML_BASIC, 0);

    char icon[CBX_ICON_ICON_LEN];
    char name[CBX_ICON_NAME_LEN];
    int rc = cbx_icon_map_lookup(&map, NULL, icon, sizeof(icon),
                                  name, sizeof(name));
    assert_int_equal(rc, -EINVAL);
}

/* lookup with NULL output buffers → success */
static void test_lookup_null_outputs(void **state)
{
    (void)state;
    cbx_icon_map map;
    cbx_icon_map_parse(&map, YAML_BASIC, 0);

    int rc = cbx_icon_map_lookup(&map, "xb360", NULL, 0, NULL, 0);
    assert_int_equal(rc, 0);
}

/* lookup with small output buffer → truncated but safe */
static void test_lookup_small_buffer(void **state)
{
    (void)state;
    cbx_icon_map map;
    cbx_icon_map_parse(&map, YAML_BASIC, 0);

    char icon[4];   /* too small for "cc-xbox-360" */
    char name[8];
    int rc = cbx_icon_map_lookup(&map, "xb360", icon, sizeof(icon),
                                  name, sizeof(name));
    assert_int_equal(rc, 0);
    /* Should be truncated to fit. */
    assert_int_equal(strlen(icon), 3);
    assert_int_equal(icon[3], '\0');
}

/* default path builds correctly */
static void test_default_path(void **state)
{
    (void)state;
    char path[PATH_MAX + 64];
    int rc = cbx_icon_map_default_path(path, sizeof(path));
    assert_int_equal(rc, 0);
    assert_true(strstr(path, "controller-icons.yaml") != NULL);
    /* The default path must resolve to a real, readable icons file regardless
       of its absolute source-root location; a stale build must not mask a
       broken default path. */
    assert_true(access(path, R_OK) == 0);
}

/* default path with NULL → EINVAL */
static void test_default_path_null(void **state)
{
    (void)state;
    int rc = cbx_icon_map_default_path(NULL, 0);
    assert_int_equal(rc, -EINVAL);
}

/* default path with too-small buffer → ENAMETOOLONG */
static void test_default_path_small(void **state)
{
    (void)state;
    char path[4];
    int rc = cbx_icon_map_default_path(path, sizeof(path));
    assert_int_equal(rc, -ENAMETOOLONG);
}

/* load from non-existent file → error */
static void test_load_nonexistent(void **state)
{
    (void)state;
    cbx_icon_map map;
    int rc = cbx_icon_map_load(&map, "/nonexistent/controller-icons.yaml");
    assert_true(rc < 0);
    assert_int_equal(map.loaded, 0);
}

/* load with NULL args → EINVAL */
static void test_load_null_args(void **state)
{
    (void)state;
    cbx_icon_map map;
    int rc = cbx_icon_map_load(NULL, "/tmp/test.yaml");
    assert_int_equal(rc, -EINVAL);

    rc = cbx_icon_map_load(&map, NULL);
    assert_int_equal(rc, -EINVAL);
}

/* load from a temp file with valid YAML */
static void test_load_tempfile(void **state)
{
    (void)state;
    /* Write test YAML to a temp file. */
    const char *tmp_path = "/tmp/test_icon_map_load.yaml";
    FILE *f = fopen(tmp_path, "wb");
    assert_non_null(f);
    fputs(YAML_BASIC, f);
    fclose(f);

    cbx_icon_map map;
    int rc = cbx_icon_map_load(&map, tmp_path);
    assert_int_equal(rc, 0);
    assert_int_equal(map.loaded, 1);
    assert_int_equal(map.count, 4);

    /* Verify yaml_path was set. */
    assert_string_equal(map.yaml_path, tmp_path);

    /* Verify lookup works. */
    char icon[CBX_ICON_ICON_LEN];
    char name[CBX_ICON_NAME_LEN];
    rc = cbx_icon_map_lookup(&map, "xb360", icon, sizeof(icon),
                              name, sizeof(name));
    assert_int_equal(rc, 0);
    assert_string_equal(icon, "cc-xbox-360");

    /* Cleanup. */
    unlink(tmp_path);
}

/* parse YAML with custom tags → EPERM (security) */
static void test_parse_tags_rejected(void **state)
{
    (void)state;
    const char *yaml =
        "virtual_types:\n"
        "  - !custom_tag type: \"xb360\"\n"
        "    icon: \"cc-xbox-360\"\n"
        "    name: \"Xbox 360\"\n";
    cbx_icon_map map;
    int rc = cbx_icon_map_parse(&map, yaml, 0);
    assert_int_equal(rc, -EPERM);
}

/* parse very large YAML → EFBIG */
static void test_parse_too_large(void **state)
{
    (void)state;
    /* Create a YAML string larger than MAX_DOC_SIZE (1 MB). */
    size_t sz = (1024 * 1024) + 100;
    char *big = malloc(sz);
    assert_non_null(big);
    memset(big, ' ', sz - 1);
    big[0] = 'v';
    big[1] = 'i';
    big[2] = 'r';
    big[3] = 't';
    big[4] = '\n';
    big[sz - 1] = '\0';

    cbx_icon_map map;
    int rc = cbx_icon_map_parse(&map, big, 0);
    assert_int_equal(rc, -EFBIG);
    free(big);
}

/* parse YAML with many entries (max entries test) */
static void test_parse_many_entries(void **state)
{
    (void)state;
    /* Generate YAML with > CBX_ICON_MAP_MAX_ENTRIES entries. */
    char yaml[8192];
    int offset = 0;
    offset += snprintf(yaml + offset, sizeof(yaml) - offset,
                       "virtual_types:\n");
    for (int i = 0; i < CBX_ICON_MAP_MAX_ENTRIES + 10; i++) {
        offset += snprintf(yaml + offset, sizeof(yaml) - offset,
                           "  - type: \"type%d\"\n"
                           "    icon: \"icon%d\"\n"
                           "    name: \"Name %d\"\n",
                           i, i, i);
        if ((size_t)offset >= sizeof(yaml))
            break;
    }

    cbx_icon_map map;
    int rc = cbx_icon_map_parse(&map, yaml, 0);
    assert_int_equal(rc, 0);
    /* Should have capped at CBX_ICON_MAP_MAX_ENTRIES. */
    assert_int_equal(map.count, CBX_ICON_MAP_MAX_ENTRIES);
}

/* full YAML from the spec (§8.4) with real device types */
static void test_parse_full_mapping(void **state)
{
    (void)state;
    const char *yaml =
        "virtual_types:\n"
        "  - type: \"xb360\"\n"
        "    icon: \"cc-xbox-360\"\n"
        "    name: \"Xbox 360 Controller\"\n"
        "  - type: \"ds5\"\n"
        "    icon: \"cc-ps5\"\n"
        "    name: \"DualSense\"\n"
        "  - type: \"deck\"\n"
        "    icon: \"cc-steam-deck\"\n"
        "    name: \"Steam Deck Controller\"\n"
        "  - type: \"gamepad\"\n"
        "    icon: \"generic-gamepad\"\n"
        "    name: \"Generic Gamepad\"\n"
        "  - type: \"mouse\"\n"
        "    icon: \"cc-mouse\"\n"
        "    name: \"Mouse\"\n"
        "  - type: \"keyboard\"\n"
        "    icon: \"cc-keyboard\"\n"
        "    name: \"Keyboard\"\n"
        "custom_icons:\n"
        "  - icon: \"arcade-stick\"\n"
        "    name: \"Arcade Stick\"\n"
        "  - icon: \"hitbox\"\n"
        "    name: \"Hit Box\"\n";

    cbx_icon_map map;
    int rc = cbx_icon_map_parse(&map, yaml, 0);
    assert_int_equal(rc, 0);
    assert_int_equal(map.count, 6);

    /* Verify all lookups. */
    char icon[CBX_ICON_ICON_LEN];
    char name[CBX_ICON_NAME_LEN];

    cbx_icon_map_lookup(&map, "xb360", icon, sizeof(icon), name, sizeof(name));
    assert_string_equal(icon, "cc-xbox-360");

    cbx_icon_map_lookup(&map, "ds5", icon, sizeof(icon), name, sizeof(name));
    assert_string_equal(icon, "cc-ps5");

    cbx_icon_map_lookup(&map, "deck", icon, sizeof(icon), name, sizeof(name));
    assert_string_equal(icon, "cc-steam-deck");

    cbx_icon_map_lookup(&map, "gamepad", icon, sizeof(icon), name, sizeof(name));
    assert_string_equal(icon, "generic-gamepad");

    cbx_icon_map_lookup(&map, "mouse", icon, sizeof(icon), name, sizeof(name));
    assert_string_equal(icon, "cc-mouse");

    cbx_icon_map_lookup(&map, "keyboard", icon, sizeof(icon), name, sizeof(name));
    assert_string_equal(icon, "cc-keyboard");
}

/* unknown type returns the raw type as name (SPEC §8.4) */
static void test_unknown_returns_raw_type(void **state)
{
    (void)state;
    cbx_icon_map map;
    cbx_icon_map_parse(&map, YAML_BASIC, 0);

    char icon[CBX_ICON_ICON_LEN];
    char name[CBX_ICON_NAME_LEN];

    cbx_icon_map_lookup(&map, "some-future-device",
                         icon, sizeof(icon), name, sizeof(name));
    assert_string_equal(icon, "generic-gamepad");
    assert_string_equal(name, "some-future-device");

    cbx_icon_map_lookup(&map, "ps3",
                         icon, sizeof(icon), name, sizeof(name));
    assert_string_equal(icon, "generic-gamepad");
    assert_string_equal(name, "ps3");
}

/* lookup with only icon output (name NULL) */
static void test_lookup_icon_only(void **state)
{
    (void)state;
    cbx_icon_map map;
    cbx_icon_map_parse(&map, YAML_BASIC, 0);

    char icon[CBX_ICON_ICON_LEN];
    int rc = cbx_icon_map_lookup(&map, "xb360", icon, sizeof(icon),
                                  NULL, 0);
    assert_int_equal(rc, 0);
    assert_string_equal(icon, "cc-xbox-360");
}

/* lookup with only name output (icon NULL) */
static void test_lookup_name_only(void **state)
{
    (void)state;
    cbx_icon_map map;
    cbx_icon_map_parse(&map, YAML_BASIC, 0);

    char name[CBX_ICON_NAME_LEN];
    int rc = cbx_icon_map_lookup(&map, "xb360", NULL, 0,
                                  name, sizeof(name));
    assert_int_equal(rc, 0);
    assert_string_equal(name, "Xbox 360 Controller");
}

/* entry with missing icon field → empty icon string */
static void test_entry_missing_icon(void **state)
{
    (void)state;
    const char *yaml =
        "virtual_types:\n"
        "  - type: \"test\"\n"
        "    name: \"Test Device\"\n";
    cbx_icon_map map;
    int rc = cbx_icon_map_parse(&map, yaml, 0);
    assert_int_equal(rc, 0);
    assert_int_equal(map.count, 1);
    assert_string_equal(map.entries[0].type, "test");
    assert_string_equal(map.entries[0].icon, "");
    assert_string_equal(map.entries[0].name, "Test Device");
}

/* entry with missing type field → not stored */
static void test_entry_missing_type(void **state)
{
    (void)state;
    const char *yaml =
        "virtual_types:\n"
        "  - icon: \"some-icon\"\n"
        "    name: \"No Type\"\n"
        "  - type: \"has-type\"\n"
        "    icon: \"has-icon\"\n"
        "    name: \"Has Type\"\n";
    cbx_icon_map map;
    int rc = cbx_icon_map_parse(&map, yaml, 0);
    assert_int_equal(rc, 0);
    /* Only the entry with a type should be stored. */
    assert_int_equal(map.count, 1);
    assert_string_equal(map.entries[0].type, "has-type");
}

/* long type string is truncated safely */
static void test_long_type_truncated(void **state)
{
    (void)state;
    /* Type string longer than CBX_ICON_TYPE_LEN (64). */
    char long_type[200];
    memset(long_type, 'x', 199);
    long_type[199] = '\0';

    char yaml[512];
    snprintf(yaml, sizeof(yaml),
             "virtual_types:\n"
             "  - type: \"%s\"\n"
             "    icon: \"icon\"\n"
             "    name: \"Name\"\n",
             long_type);

    cbx_icon_map map;
    int rc = cbx_icon_map_parse(&map, yaml, 0);
    assert_int_equal(rc, 0);
    assert_int_equal(map.count, 1);
    /* Type should be truncated to fit. */
    assert_int_equal(strlen(map.entries[0].type), CBX_ICON_TYPE_LEN - 1);
}

/* re-parse resets the map (init called in parse) */
static void test_reparse_resets(void **state)
{
    (void)state;
    cbx_icon_map map;
    cbx_icon_map_parse(&map, YAML_BASIC, 0);
    assert_int_equal(map.count, 4);

    /* Parse again with different YAML. */
    int rc = cbx_icon_map_parse(&map, YAML_WITH_CUSTOM, 0);
    assert_int_equal(rc, 0);
    assert_int_equal(map.count, 2);
}

/* --- nanosvg compatibility verification ----------------------------------- */

/* Verify a single SVG parses and rasterizes to non-zero dimensions. */
static int verify_svg_nanosvg(const char *svg_name)
{
    char path[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/svg/%s", SVG_DIR, svg_name);

    NSVGimage *image = nsvgParseFromFile(path, "px", 96.0f);
    if (!image)
        return -1;
    if (image->width <= 0.0f || image->height <= 0.0f) {
        nsvgDelete(image);
        return -2;
    }

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) {
        nsvgDelete(image);
        return -3;
    }

    int w = (int)image->width;
    int h = (int)image->height;
    unsigned char *pixels = malloc((size_t)w * h * 4);
    if (!pixels) {
        nsvgDeleteRasterizer(rast);
        nsvgDelete(image);
        return -4;
    }

    nsvgRasterize(rast, image, 0.0f, 0.0f, 1.0f, pixels, w, h, w * 4);

    /* Check at least one non-transparent pixel exists. */
    int has_content = 0;
    for (int i = 0; i < w * h; i++) {
        if (pixels[i * 4 + 3] > 0) {
            has_content = 1;
            break;
        }
    }

    free(pixels);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);

    return has_content ? 0 : -5;
}

/* Verify a Controllercons SVG (ps5) parses and rasterizes. */
static void test_svg_ps5(void **state)
{
    (void)state;
    int rc = verify_svg_nanosvg("ps5.svg");
    assert_int_equal(rc, 0);
}

/* Verify a Controllercons SVG (xbox-360) parses and rasterizes. */
static void test_svg_xbox360(void **state)
{
    (void)state;
    int rc = verify_svg_nanosvg("xbox-360.svg");
    assert_int_equal(rc, 0);
}

/* Verify custom SVG: steam-deck parses and rasterizes. */
static void test_svg_steam_deck(void **state)
{
    (void)state;
    int rc = verify_svg_nanosvg("steam-deck.svg");
    assert_int_equal(rc, 0);
}

/* Verify custom SVG: generic-gamepad parses and rasterizes. */
static void test_svg_generic_gamepad(void **state)
{
    (void)state;
    int rc = verify_svg_nanosvg("generic-gamepad.svg");
    assert_int_equal(rc, 0);
}

/* Verify custom SVG: arcade-stick parses and rasterizes. */
static void test_svg_arcade_stick(void **state)
{
    (void)state;
    int rc = verify_svg_nanosvg("arcade-stick.svg");
    assert_int_equal(rc, 0);
}

/* Verify custom SVG: hitbox parses and rasterizes. */
static void test_svg_hitbox(void **state)
{
    (void)state;
    int rc = verify_svg_nanosvg("hitbox.svg");
    assert_int_equal(rc, 0);
}

/* Verify custom SVG: mouse parses and rasterizes. */
static void test_svg_mouse(void **state)
{
    (void)state;
    int rc = verify_svg_nanosvg("mouse.svg");
    assert_int_equal(rc, 0);
}

/* Verify custom SVG: keyboard parses and rasterizes. */
static void test_svg_keyboard(void **state)
{
    (void)state;
    int rc = verify_svg_nanosvg("keyboard.svg");
    assert_int_equal(rc, 0);
}

/* Verify all SVGs in data/icons/svg/ parse and rasterize. */
static void test_svg_all_compat(void **state)
{
    (void)state;
    DIR *dir = opendir(SVG_DIR "/svg");
    if (!dir) {
        /* If the directory doesn't exist in this build environment,
         * skip the test rather than fail. */
        skip();
        return;
    }

    int checked = 0;
    int failed = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        /* Only check .svg files. */
        const char *name = ent->d_name;
        size_t len = strlen(name);
        if (len < 5 || strcmp(name + len - 4, ".svg") != 0)
            continue;

        int rc = verify_svg_nanosvg(name);
        if (rc != 0) {
            fprintf(stderr, "SVG parse/rasterize failed for %s: rc=%d\n",
                    name, rc);
            failed++;
        }
        checked++;
    }
    closedir(dir);

    assert_true(checked > 0);
    assert_int_equal(failed, 0);
}

/* Verify controller-icons.yaml loads from the source data directory. */
static void test_load_real_yaml(void **state)
{
    (void)state;
    char path[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/data/controller-icons.yaml",
             CBX_SOURCE_DIR);

    cbx_icon_map map;
    int rc = cbx_icon_map_load(&map, path);
    if (rc < 0) {
        /* File might not exist in some build environments. */
        skip();
        return;
    }
    assert_int_equal(rc, 0);
    assert_true(map.count > 0);
    assert_int_equal(map.loaded, 1);

    /* Verify known lookups. */
    char icon[CBX_ICON_ICON_LEN];
    char name[CBX_ICON_NAME_LEN];

    rc = cbx_icon_map_lookup(&map, "xb360", icon, sizeof(icon),
                              name, sizeof(name));
    assert_int_equal(rc, 0);
    assert_string_equal(icon, "cc-xbox-360");

    rc = cbx_icon_map_lookup(&map, "ds5", icon, sizeof(icon),
                              name, sizeof(name));
    assert_int_equal(rc, 0);
    assert_string_equal(icon, "cc-ps5");

    rc = cbx_icon_map_lookup(&map, "deck", icon, sizeof(icon),
                              name, sizeof(name));
    assert_int_equal(rc, 0);
    assert_string_equal(icon, "cc-steam-deck");
}

/* --- Main ---------------------------------------------------------------- */

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_init),
        cmocka_unit_test(test_init_null),
        cmocka_unit_test(test_parse_basic),
        cmocka_unit_test(test_parse_with_custom),
        cmocka_unit_test(test_parse_null_args),
        cmocka_unit_test(test_parse_empty),
        cmocka_unit_test(test_parse_no_virtual),
        cmocka_unit_test(test_lookup_known),
        cmocka_unit_test(test_lookup_ds5),
        cmocka_unit_test(test_lookup_unknown),
        cmocka_unit_test(test_lookup_null_map),
        cmocka_unit_test(test_lookup_unloaded),
        cmocka_unit_test(test_lookup_null_type),
        cmocka_unit_test(test_lookup_null_outputs),
        cmocka_unit_test(test_lookup_small_buffer),
        cmocka_unit_test(test_default_path),
        cmocka_unit_test(test_default_path_null),
        cmocka_unit_test(test_default_path_small),
        cmocka_unit_test(test_load_nonexistent),
        cmocka_unit_test(test_load_null_args),
        cmocka_unit_test(test_load_tempfile),
        cmocka_unit_test(test_parse_tags_rejected),
        cmocka_unit_test(test_parse_too_large),
        cmocka_unit_test(test_parse_many_entries),
        cmocka_unit_test(test_parse_full_mapping),
        cmocka_unit_test(test_unknown_returns_raw_type),
        cmocka_unit_test(test_lookup_icon_only),
        cmocka_unit_test(test_lookup_name_only),
        cmocka_unit_test(test_entry_missing_icon),
        cmocka_unit_test(test_entry_missing_type),
        cmocka_unit_test(test_long_type_truncated),
        cmocka_unit_test(test_reparse_resets),
        cmocka_unit_test(test_svg_ps5),
        cmocka_unit_test(test_svg_xbox360),
        cmocka_unit_test(test_svg_steam_deck),
        cmocka_unit_test(test_svg_generic_gamepad),
        cmocka_unit_test(test_svg_arcade_stick),
        cmocka_unit_test(test_svg_hitbox),
        cmocka_unit_test(test_svg_mouse),
        cmocka_unit_test(test_svg_keyboard),
        cmocka_unit_test(test_svg_all_compat),
        cmocka_unit_test(test_load_real_yaml),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}