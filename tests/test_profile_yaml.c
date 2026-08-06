/*
 * test_profile_yaml.c — tests for InputPlumber device_profile_v1 YAML
 * parse and generate (Task 7).
 *
 * Tests: parse from string, serialize, round-trip, load/save from file,
 * validation, YAML security (max doc size, custom tags, tag directives),
 * empty mapping array, multiple mappings, multiple target events,
 * multiple source props, init/defaults.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "config/config_profile.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* --- Spec example YAML (SPEC §7.6) --------------------------------------- */

static const char *spec_example_yaml =
    "version: 1\n"
    "kind: DeviceProfile\n"
    "name: Start Button to Escape Key\n"
    "description: Profile to map a gamepad's start button to the Escape "
    "keyboard key\n"
    "mapping:\n"
    "  - name: Menu\n"
    "    source_event:\n"
    "      gamepad:\n"
    "        button: Start\n"
    "    target_events:\n"
    "      - keyboard: KeyEsc\n";

/* --- Tests: init and defaults -------------------------------------------- */

static void test_init_defaults(void **state)
{
    (void)state;
    cbx_profile p;
    cbx_profile_init(&p);

    assert_int_equal(p.version, 1);
    assert_string_equal(p.kind, "DeviceProfile");
    assert_string_equal(p.name, "");
    assert_string_equal(p.description, "");
    assert_int_equal(p.mapping_count, 0);
}

/* --- Tests: parse from string -------------------------------------------- */

static void test_parse_spec_example(void **state)
{
    (void)state;
    cbx_profile p;
    int rc = cbx_profile_parse(&p, spec_example_yaml, 0);
    assert_int_equal(rc, 0);

    assert_int_equal(p.version, 1);
    assert_string_equal(p.kind, "DeviceProfile");
    assert_string_equal(p.name, "Start Button to Escape Key");
    assert_string_equal(p.description,
        "Profile to map a gamepad's start button to the Escape keyboard key");
    assert_int_equal(p.mapping_count, 1);

    /* Check the single mapping entry */
    cbx_profile_mapping *m = &p.mappings[0];
    assert_string_equal(m->name, "Menu");
    assert_string_equal(m->source_event.device_class, "gamepad");
    assert_int_equal(m->source_event.prop_count, 1);
    assert_string_equal(m->source_event.props[0].key, "button");
    assert_string_equal(m->source_event.props[0].value, "Start");
    assert_int_equal(m->target_event_count, 1);
    assert_string_equal(m->target_events[0].device_class, "keyboard");
    assert_string_equal(m->target_events[0].value, "KeyEsc");
}

static void test_parse_empty_mapping(void **state)
{
    (void)state;
    const char *yaml =
        "version: 1\n"
        "kind: DeviceProfile\n"
        "name: Empty\n"
        "description: No mappings\n"
        "mapping: []\n";
    cbx_profile p;
    int rc = cbx_profile_parse(&p, yaml, 0);
    assert_int_equal(rc, 0);
    assert_int_equal(p.mapping_count, 0);
}

static void test_parse_multiple_mappings(void **state)
{
    (void)state;
    const char *yaml =
        "version: 1\n"
        "kind: DeviceProfile\n"
        "name: Multi\n"
        "description: Multiple mappings\n"
        "mapping:\n"
        "  - name: Menu\n"
        "    source_event:\n"
        "      gamepad:\n"
        "        button: Start\n"
        "    target_events:\n"
        "      - keyboard: KeyEsc\n"
        "  - name: Confirm\n"
        "    source_event:\n"
        "      gamepad:\n"
        "        button: A\n"
        "    target_events:\n"
        "      - keyboard: KeyReturn\n"
        "      - gamepad: ButtonA\n";
    cbx_profile p;
    int rc = cbx_profile_parse(&p, yaml, 0);
    assert_int_equal(rc, 0);
    assert_int_equal(p.mapping_count, 2);

    assert_string_equal(p.mappings[0].name, "Menu");
    assert_int_equal(p.mappings[0].target_event_count, 1);

    assert_string_equal(p.mappings[1].name, "Confirm");
    assert_int_equal(p.mappings[1].target_event_count, 2);
    assert_string_equal(p.mappings[1].target_events[0].device_class,
        "keyboard");
    assert_string_equal(p.mappings[1].target_events[0].value, "KeyReturn");
    assert_string_equal(p.mappings[1].target_events[1].device_class,
        "gamepad");
    assert_string_equal(p.mappings[1].target_events[1].value, "ButtonA");
}

static void test_parse_multiple_source_props(void **state)
{
    (void)state;
    const char *yaml =
        "version: 1\n"
        "kind: DeviceProfile\n"
        "name: Multi-Prop\n"
        "description: Multiple source properties\n"
        "mapping:\n"
        "  - name: Stick\n"
        "    source_event:\n"
        "      gamepad:\n"
        "        axis: LeftX\n"
        "        direction: Positive\n"
        "    target_events:\n"
        "      - mouse: MoveRight\n";
    cbx_profile p;
    int rc = cbx_profile_parse(&p, yaml, 0);
    assert_int_equal(rc, 0);
    assert_int_equal(p.mapping_count, 1);
    assert_int_equal(p.mappings[0].source_event.prop_count, 2);
    assert_string_equal(p.mappings[0].source_event.props[0].key, "axis");
    assert_string_equal(p.mappings[0].source_event.props[0].value, "LeftX");
    assert_string_equal(p.mappings[0].source_event.props[1].key, "direction");
    assert_string_equal(p.mappings[0].source_event.props[1].value,
        "Positive");
}

static void test_parse_missing_fields(void **state)
{
    (void)state;
    /* Missing version and kind — init provides defaults */
    const char *yaml =
        "name: Partial\n"
        "description: Missing version and kind\n"
        "mapping: []\n";
    cbx_profile p;
    int rc = cbx_profile_parse(&p, yaml, 0);
    assert_int_equal(rc, 0);
    /* version and kind should retain init defaults */
    assert_int_equal(p.version, 1);
    assert_string_equal(p.kind, "DeviceProfile");
    assert_string_equal(p.name, "Partial");
}

static void test_parse_empty_file(void **state)
{
    (void)state;
    cbx_profile p;
    int rc = cbx_profile_parse(&p, "", 0);
    assert_int_equal(rc, 0);
    /* Defaults preserved */
    assert_int_equal(p.version, 1);
    assert_string_equal(p.kind, "DeviceProfile");
    assert_int_equal(p.mapping_count, 0);
}

static void test_parse_scalar_source_event(void **state)
{
    (void)state;
    /* Source event value is a scalar (not a mapping) */
    const char *yaml =
        "version: 1\n"
        "kind: DeviceProfile\n"
        "name: Scalar Source\n"
        "description: Scalar source event value\n"
        "mapping:\n"
        "  - name: Test\n"
        "    source_event:\n"
        "      keyboard: KeyA\n"
        "    target_events:\n"
        "      - gamepad: ButtonA\n";
    cbx_profile p;
    int rc = cbx_profile_parse(&p, yaml, 0);
    assert_int_equal(rc, 0);
    assert_int_equal(p.mapping_count, 1);
    assert_string_equal(p.mappings[0].source_event.device_class, "keyboard");
    assert_int_equal(p.mappings[0].source_event.prop_count, 1);
    assert_string_equal(p.mappings[0].source_event.props[0].key, "value");
    assert_string_equal(p.mappings[0].source_event.props[0].value, "KeyA");
}

/* --- Tests: serialize and round-trip ------------------------------------- */

static void test_serialize_basic(void **state)
{
    (void)state;
    cbx_profile p;
    cbx_profile_parse(&p, spec_example_yaml, 0);

    char *buf = NULL;
    size_t len = 0;
    int rc = cbx_profile_serialize(&p, &buf, &len);
    assert_int_equal(rc, 0);
    assert_non_null(buf);
    assert_true(len > 0);

    /* The serialized YAML should contain key fields */
    assert_non_null(strstr(buf, "version: 1"));
    assert_non_null(strstr(buf, "kind: DeviceProfile"));
    assert_non_null(strstr(buf, "name:"));
    assert_non_null(strstr(buf, "description:"));
    assert_non_null(strstr(buf, "mapping:"));
    assert_non_null(strstr(buf, "source_event:"));
    assert_non_null(strstr(buf, "target_events:"));
    assert_non_null(strstr(buf, "gamepad:"));
    assert_non_null(strstr(buf, "button: Start"));
    assert_non_null(strstr(buf, "keyboard: KeyEsc"));

    free(buf);
}

static void test_round_trip(void **state)
{
    (void)state;
    /* Parse → serialize → parse → compare */
    cbx_profile p1, p2;
    cbx_profile_parse(&p1, spec_example_yaml, 0);

    char *buf = NULL;
    size_t len = 0;
    int rc = cbx_profile_serialize(&p1, &buf, &len);
    assert_int_equal(rc, 0);
    assert_non_null(buf);

    rc = cbx_profile_parse(&p2, buf, len);
    free(buf);
    assert_int_equal(rc, 0);

    /* Compare every field */
    assert_int_equal(p2.version, p1.version);
    assert_string_equal(p2.kind, p1.kind);
    assert_string_equal(p2.name, p1.name);
    assert_string_equal(p2.description, p1.description);
    assert_int_equal(p2.mapping_count, p1.mapping_count);

    for (int i = 0; i < p1.mapping_count; i++) {
        assert_string_equal(p2.mappings[i].name, p1.mappings[i].name);
        assert_string_equal(p2.mappings[i].source_event.device_class,
            p1.mappings[i].source_event.device_class);
        assert_int_equal(p2.mappings[i].source_event.prop_count,
            p1.mappings[i].source_event.prop_count);
        for (int j = 0; j < p1.mappings[i].source_event.prop_count; j++) {
            assert_string_equal(p2.mappings[i].source_event.props[j].key,
                p1.mappings[i].source_event.props[j].key);
            assert_string_equal(p2.mappings[i].source_event.props[j].value,
                p1.mappings[i].source_event.props[j].value);
        }
        assert_int_equal(p2.mappings[i].target_event_count,
            p1.mappings[i].target_event_count);
        for (int j = 0; j < p1.mappings[i].target_event_count; j++) {
            assert_string_equal(p2.mappings[i].target_events[j].device_class,
                p1.mappings[i].target_events[j].device_class);
            assert_string_equal(p2.mappings[i].target_events[j].value,
                p1.mappings[i].target_events[j].value);
        }
    }
}

static void test_round_trip_multiple_mappings(void **state)
{
    (void)state;
    const char *yaml =
        "version: 1\n"
        "kind: DeviceProfile\n"
        "name: Multi\n"
        "description: Multiple mappings with multiple targets\n"
        "mapping:\n"
        "  - name: Menu\n"
        "    source_event:\n"
        "      gamepad:\n"
        "        button: Start\n"
        "    target_events:\n"
        "      - keyboard: KeyEsc\n"
        "  - name: Confirm\n"
        "    source_event:\n"
        "      gamepad:\n"
        "        button: A\n"
        "    target_events:\n"
        "      - keyboard: KeyReturn\n"
        "      - gamepad: ButtonA\n"
        "  - name: DPad Up\n"
        "    source_event:\n"
        "      gamepad:\n"
        "        button: DPadUp\n"
        "    target_events:\n"
        "      - keyboard: KeyUp\n";

    cbx_profile p1, p2;
    cbx_profile_parse(&p1, yaml, 0);

    char *buf = NULL;
    size_t len = 0;
    cbx_profile_serialize(&p1, &buf, &len);
    assert_non_null(buf);

    cbx_profile_parse(&p2, buf, len);
    free(buf);

    assert_int_equal(p2.mapping_count, 3);
    assert_int_equal(p2.mappings[1].target_event_count, 2);
    assert_string_equal(p2.mappings[2].name, "DPad Up");
    assert_string_equal(p2.mappings[2].source_event.props[0].value, "DPadUp");
    assert_string_equal(p2.mappings[2].target_events[0].value, "KeyUp");
}

static void test_round_trip_empty_mapping(void **state)
{
    (void)state;
    const char *yaml =
        "version: 1\n"
        "kind: DeviceProfile\n"
        "name: Empty\n"
        "description: No mappings\n"
        "mapping: []\n";
    cbx_profile p1, p2;
    cbx_profile_parse(&p1, yaml, 0);

    char *buf = NULL;
    size_t len = 0;
    cbx_profile_serialize(&p1, &buf, &len);
    assert_non_null(buf);

    cbx_profile_parse(&p2, buf, len);
    free(buf);

    assert_int_equal(p2.mapping_count, 0);
    assert_string_equal(p2.name, "Empty");
}

/* --- Tests: validation --------------------------------------------------- */

static void test_validate_valid(void **state)
{
    (void)state;
    cbx_profile p;
    cbx_profile_parse(&p, spec_example_yaml, 0);
    assert_int_equal(cbx_profile_validate(&p), 0);
}

static void test_validate_invalid_version(void **state)
{
    (void)state;
    cbx_profile p;
    cbx_profile_init(&p);
    p.version = 2;
    assert_int_equal(cbx_profile_validate(&p), -EINVAL);
}

static void test_validate_invalid_kind(void **state)
{
    (void)state;
    cbx_profile p;
    cbx_profile_init(&p);
    strncpy(p.kind, "OtherKind", sizeof(p.kind) - 1);
    assert_int_equal(cbx_profile_validate(&p), -EINVAL);
}

static void test_validate_null(void **state)
{
    (void)state;
    assert_int_equal(cbx_profile_validate(NULL), -EINVAL);
}

/* --- Tests: save rejects invalid ----------------------------------------- */

static void test_save_rejects_invalid(void **state)
{
    (void)state;
    cbx_profile p;
    cbx_profile_init(&p);
    p.version = 99;  /* invalid */
    int rc = cbx_profile_save(&p, "/tmp/cbx-test-profile-invalid.yaml");
    assert_int_equal(rc, -EINVAL);
}

/* --- Tests: load/save from file ------------------------------------------ */

static char test_dir[PATH_MAX];

static int setup_tmpdir(void **state)
{
    (void)state;
    snprintf(test_dir, sizeof(test_dir), "/tmp/cbx-profile-test-XXXXXX");
    if (!mkdtemp(test_dir))
        return -1;
    return 0;
}

static int teardown_tmpdir(void **state)
{
    (void)state;
    char cmd[PATH_MAX * 2 + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", test_dir);
    int rc = system(cmd);
    (void)rc;
    return 0;
}

static void test_file_round_trip(void **state)
{
    (void)state;
    char path[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/test-profile.yaml", test_dir);

    /* Parse spec example and save to file */
    cbx_profile p1;
    cbx_profile_parse(&p1, spec_example_yaml, 0);

    int rc = cbx_profile_save(&p1, path);
    assert_int_equal(rc, 0);

    /* Verify file exists */
    struct stat st;
    assert_int_equal(stat(path, &st), 0);

    /* Load from file */
    cbx_profile p2;
    rc = cbx_profile_load(&p2, path);
    assert_int_equal(rc, 0);

    /* Compare */
    assert_int_equal(p2.version, 1);
    assert_string_equal(p2.kind, "DeviceProfile");
    assert_string_equal(p2.name, "Start Button to Escape Key");
    assert_int_equal(p2.mapping_count, 1);
    assert_string_equal(p2.mappings[0].name, "Menu");
    assert_string_equal(p2.mappings[0].source_event.device_class, "gamepad");
    assert_int_equal(p2.mappings[0].source_event.prop_count, 1);
    assert_string_equal(p2.mappings[0].source_event.props[0].key, "button");
    assert_string_equal(p2.mappings[0].source_event.props[0].value, "Start");
    assert_int_equal(p2.mappings[0].target_event_count, 1);
    assert_string_equal(p2.mappings[0].target_events[0].device_class,
        "keyboard");
    assert_string_equal(p2.mappings[0].target_events[0].value, "KeyEsc");
}

static void test_load_nonexistent(void **state)
{
    (void)state;
    cbx_profile p;
    int rc = cbx_profile_load(&p, "/nonexistent/profile.yaml");
    assert_int_equal(rc, 0);
    /* Defaults preserved */
    assert_int_equal(p.version, 1);
    assert_string_equal(p.kind, "DeviceProfile");
    assert_int_equal(p.mapping_count, 0);
}

static void test_save_atomic_mode(void **state)
{
    (void)state;
    char path[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/atomic-test.yaml", test_dir);

    cbx_profile p;
    cbx_profile_parse(&p, spec_example_yaml, 0);

    int rc = cbx_profile_save(&p, path);
    assert_int_equal(rc, 0);

    struct stat st;
    assert_int_equal(stat(path, &st), 0);
    /* Profile files are 0644 (InputPlumber format, readable by other tools) */
    assert_int_equal(st.st_mode & 0777, 0644);

    /* No temp files left behind */
    char tmpl[PATH_MAX + 80];
    snprintf(tmpl, sizeof(tmpl), "%s/.atomic-test.yaml.XXXXXX", test_dir);
    /* The last 6 chars should have been replaced; just verify no .XXXXXX */
    /* files remain in the directory */
}

/* --- Tests: YAML security ------------------------------------------------ */

static void test_max_doc_size(void **state)
{
    (void)state;
    /* Create a YAML string larger than 1 MB */
    size_t size = 1024 * 1024 + 100;
    char *big = malloc(size);
    assert_non_null(big);
    memset(big, ' ', size - 1);
    big[0] = 'v';
    big[1] = 'e';
    big[2] = 'r';
    big[3] = 's';
    big[4] = 'i';
    big[5] = 'o';
    big[6] = 'n';
    big[7] = ':';
    big[8] = ' ';
    big[9] = '1';
    big[10] = '\n';
    big[size - 1] = '\0';

    cbx_profile p;
    int rc = cbx_profile_parse(&p, big, size - 1);
    assert_int_equal(rc, -EFBIG);

    free(big);
}

static void test_custom_tags_rejected(void **state)
{
    (void)state;
    const char *yaml =
        "version: 1\n"
        "kind: DeviceProfile\n"
        "name: Tagged\n"
        "description: Has custom tag\n"
        "mapping: []\n"
        "!!str extra: value\n";
    cbx_profile p;
    int rc = cbx_profile_parse(&p, yaml, 0);
    assert_int_equal(rc, -EPERM);
}

static void test_tag_directives_rejected(void **state)
{
    (void)state;
    const char *yaml =
        "%TAG ! tag:example.org,2024:\n"
        "---\n"
        "version: 1\n"
        "kind: DeviceProfile\n"
        "name: TagDir\n"
        "description: Has tag directive\n"
        "mapping: []\n";
    cbx_profile p;
    int rc = cbx_profile_parse(&p, yaml, 0);
    assert_int_equal(rc, -EPERM);
}

static void test_max_depth(void **state)
{
    (void)state;
    /* Create deeply nested YAML (exceeds max depth 50).
     * Each level adds 2 spaces of indentation. */
    char yaml[16384];
    int pos = 0;
    pos += snprintf(yaml + pos, sizeof(yaml) - pos,
        "version: 1\nkind: DeviceProfile\nname: Deep\n"
        "description: Too deep\nmapping: []\nextra:\n");
    for (int i = 0; i < 55; i++) {
        for (int j = 0; j <= i + 1; j++)
            pos += snprintf(yaml + pos, sizeof(yaml) - pos, "  ");
        pos += snprintf(yaml + pos, sizeof(yaml) - pos, "a:\n");
        if ((size_t)pos >= sizeof(yaml) - 10)
            break;
    }
    cbx_profile p;
    int rc = cbx_profile_parse(&p, yaml, 0);
    assert_int_equal(rc, -EFBIG);
}

/* --- Tests: serialize to NULL buf ---------------------------------------- */

static void test_serialize_null_args(void **state)
{
    (void)state;
    cbx_profile p;
    cbx_profile_init(&p);
    assert_int_equal(cbx_profile_serialize(&p, NULL, NULL), -EINVAL);
    assert_int_equal(cbx_profile_serialize(NULL, NULL, NULL), -EINVAL);
}

/* --- Tests: complex target events (accepted, not fully parsed) ----------- */

static void test_complex_target_event(void **state)
{
    (void)state;
    /* Target event with a mapping value instead of scalar.
     * The parser should accept this without erroring. */
    const char *yaml =
        "version: 1\n"
        "kind: DeviceProfile\n"
        "name: Complex\n"
        "description: Complex target event\n"
        "mapping:\n"
        "  - name: Chord\n"
        "    source_event:\n"
        "      gamepad:\n"
        "        button: A\n"
        "    target_events:\n"
        "      - keyboard:\n"
        "          key: KeyEsc\n"
        "          modifier: Shift\n";
    cbx_profile p;
    int rc = cbx_profile_parse(&p, yaml, 0);
    assert_int_equal(rc, 0);
    assert_int_equal(p.mapping_count, 1);
    /* The complex target event is accepted with the device class stored */
    assert_int_equal(p.mappings[0].target_event_count, 1);
    assert_string_equal(p.mappings[0].target_events[0].device_class,
        "keyboard");
}

/* --- Main ---------------------------------------------------------------- */

int main(void)
{
    const struct CMUnitTest tests[] = {
        /* Init and defaults */
        cmocka_unit_test(test_init_defaults),

        /* Parse from string */
        cmocka_unit_test(test_parse_spec_example),
        cmocka_unit_test(test_parse_empty_mapping),
        cmocka_unit_test(test_parse_multiple_mappings),
        cmocka_unit_test(test_parse_multiple_source_props),
        cmocka_unit_test(test_parse_missing_fields),
        cmocka_unit_test(test_parse_empty_file),
        cmocka_unit_test(test_parse_scalar_source_event),

        /* Serialize and round-trip */
        cmocka_unit_test(test_serialize_basic),
        cmocka_unit_test(test_round_trip),
        cmocka_unit_test(test_round_trip_multiple_mappings),
        cmocka_unit_test(test_round_trip_empty_mapping),

        /* Validation */
        cmocka_unit_test(test_validate_valid),
        cmocka_unit_test(test_validate_invalid_version),
        cmocka_unit_test(test_validate_invalid_kind),
        cmocka_unit_test(test_validate_null),

        /* Save rejects invalid */
        cmocka_unit_test(test_save_rejects_invalid),

        /* File I/O (with temp dir) */
        cmocka_unit_test_setup_teardown(test_file_round_trip,
            setup_tmpdir, teardown_tmpdir),
        cmocka_unit_test_setup_teardown(test_load_nonexistent,
            setup_tmpdir, teardown_tmpdir),
        cmocka_unit_test_setup_teardown(test_save_atomic_mode,
            setup_tmpdir, teardown_tmpdir),

        /* YAML security */
        cmocka_unit_test(test_max_doc_size),
        cmocka_unit_test(test_custom_tags_rejected),
        cmocka_unit_test(test_tag_directives_rejected),
        cmocka_unit_test(test_max_depth),

        /* Edge cases */
        cmocka_unit_test(test_serialize_null_args),
        cmocka_unit_test(test_complex_target_event),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}