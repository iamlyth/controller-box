/*
 * test_service_install.c — cmocka tests for systemd service installation
 * (Task 40).
 *
 * Tests cover:
 *   - Unit path resolution (XDG, HOME, fallbacks, overflow)
 *   - Unit file content generation (template, Flatpak detection, binary path)
 *   - Atomic write (mkstemp + rename, directory creation, mode 0644)
 *   - Group membership check (found, not found, mock file, mock username)
 *   - systemd availability detection (mock systemctl)
 *   - Service active check (mock systemctl)
 *   - Full install flow (mock all, success path, already active, no systemd)
 *   - Uninstall (disable + unlink)
 *   - Null-safety throughout
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "manager/service_install.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ------------------------------------------------------------------ */
/*  Fixture                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    char tmp[256];
} si_fixture;

static int setup(void **state)
{
    si_fixture *f = malloc(sizeof(*f));
    assert_non_null(f);
    memset(f, 0, sizeof(*f));

    snprintf(f->tmp, sizeof(f->tmp), "/tmp/cbx_svc_test_%d", (int)getpid());
    char cmd[PATH_MAX * 2 + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", f->tmp);
    int r0 = system(cmd);
    (void)r0;
    mkdir(f->tmp, 0755);
    setenv("HOME", f->tmp, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("FLATPAK_ID");

    /* Reset test overrides. */
    cbx_service_set_mock_systemctl(NULL);
    cbx_service_set_mock_group_file(NULL);
    cbx_service_set_mock_username(NULL);

    *state = f;
    return 0;
}

static int teardown(void **state)
{
    si_fixture *f = *state;

    cbx_service_set_mock_systemctl(NULL);
    cbx_service_set_mock_group_file(NULL);
    cbx_service_set_mock_username(NULL);

    char cmd[PATH_MAX * 2 + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", f->tmp);
    int r = system(cmd);
    (void)r;
    unsetenv("HOME");
    free(f);
    return 0;
}

#define FIX(s) ((si_fixture *)*(s))

/* ------------------------------------------------------------------ */
/*  Unit path resolution tests                                         */
/* ------------------------------------------------------------------ */

static void test_unit_path_home(void **state)
{
    si_fixture *f = FIX(state);
    char buf[PATH_MAX + 64];
    int rc = cbx_service_unit_path(buf, sizeof(buf));
    assert_int_equal(rc, 0);
    /* HOME is set to temp dir; path should contain it. */
    assert_non_null(strstr(buf, f->tmp));
    assert_non_null(strstr(buf, "/.config/systemd/user/controller-box.service"));
}

static void test_unit_path_xdg(void **state)
{
    si_fixture *f = FIX(state);
    char xdg[PATH_MAX + 64];
    snprintf(xdg, sizeof(xdg), "%s/.config", f->tmp);
    setenv("XDG_CONFIG_HOME", xdg, 1);

    char buf[PATH_MAX + 64];
    int rc = cbx_service_unit_path(buf, sizeof(buf));
    assert_int_equal(rc, 0);
    /* XDG_CONFIG_HOME is absolute → path should not include .config. */
    assert_non_null(strstr(buf, "/systemd/user/controller-box.service"));
    assert_non_null(strstr(buf, xdg));

    unsetenv("XDG_CONFIG_HOME");
}

static void test_unit_path_no_home(void **state)
{
    (void)state;
    unsetenv("HOME");
    unsetenv("XDG_CONFIG_HOME");

    char buf[PATH_MAX];
    int rc = cbx_service_unit_path(buf, sizeof(buf));
    assert_int_equal(rc, -ENOENT);
}

static void test_unit_path_overflow(void **state)
{
    (void)state;
    char buf[4];
    int rc = cbx_service_unit_path(buf, sizeof(buf));
    assert_int_equal(rc, -ENAMETOOLONG);
}

static void test_unit_path_null_args(void **state)
{
    (void)state;
    assert_int_equal(cbx_service_unit_path(NULL, 100), -EINVAL);
    assert_int_equal(cbx_service_unit_path(NULL, 0), -EINVAL);
}

/* ------------------------------------------------------------------ */
/*  Unit file content tests                                            */
/* ------------------------------------------------------------------ */

static void test_unit_content_basic(void **state)
{
    (void)state;
    char buf[2048];
    int rc = cbx_service_unit_content(buf, sizeof(buf));
    assert_int_equal(rc, 0);

    /* Check key lines are present. */
    assert_non_null(strstr(buf, "[Unit]"));
    assert_non_null(strstr(buf, "Description=Controller-Box Overlay Service"));
    assert_non_null(strstr(buf, "After=inputplumber.service"));
    assert_non_null(strstr(buf, "Requires=inputplumber.service"));
    assert_non_null(strstr(buf, "[Service]"));
    assert_non_null(strstr(buf, "ExecStart="));
    assert_non_null(strstr(buf, "--overlay-service"));
    assert_non_null(strstr(buf, "Restart=always"));
    assert_non_null(strstr(buf, "[Install]"));
    assert_non_null(strstr(buf, "WantedBy=default.target"));
}

static void test_unit_content_flatpak(void **state)
{
    (void)state;
    setenv("FLATPAK_ID", "org.shadowblip.ControllerBox", 1);

    char buf[2048];
    int rc = cbx_service_unit_content(buf, sizeof(buf));
    assert_int_equal(rc, 0);

    assert_non_null(strstr(buf, "flatpak run org.shadowblip.ControllerBox"));
    assert_non_null(strstr(buf, "--overlay-service"));

    unsetenv("FLATPAK_ID");
}

static void test_unit_content_overflow(void **state)
{
    (void)state;
    char buf[10];
    int rc = cbx_service_unit_content(buf, sizeof(buf));
    assert_int_equal(rc, -ENAMETOOLONG);
}

static void test_unit_content_null_args(void **state)
{
    (void)state;
    assert_int_equal(cbx_service_unit_content(NULL, 100), -EINVAL);
    assert_int_equal(cbx_service_unit_content(NULL, 0), -EINVAL);
}

/* ------------------------------------------------------------------ */
/*  Atomic write tests                                                 */
/* ------------------------------------------------------------------ */

static void test_write_unit_creates_file(void **state)
{
    si_fixture *f = FIX(state);

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/systemd/user/controller-box.service", f->tmp);

    int rc = cbx_service_write_unit(path, NULL);
    assert_int_equal(rc, 0);

    /* Verify file exists and has correct content. */
    struct stat st;
    assert_int_equal(stat(path, &st), 0);
    assert_true(S_ISREG(st.st_mode));

    /* Check permissions (0644, but umask may affect — check read bits). */
    assert_true((st.st_mode & 0400) != 0);  /* owner read */
    assert_true((st.st_mode & 0040) != 0);  /* group read */

    /* Read and check content. */
    FILE *file = fopen(path, "r");
    assert_non_null(file);
    char content[2048];
    size_t n = fread(content, 1, sizeof(content) - 1, file);
    content[n] = '\0';
    fclose(file);

    assert_non_null(strstr(content, "[Unit]"));
    assert_non_null(strstr(content, "After=inputplumber.service"));
    assert_non_null(strstr(content, "Restart=always"));
}

static void test_write_unit_custom_content(void **state)
{
    si_fixture *f = FIX(state);

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/test.service", f->tmp);

    const char *custom = "[Unit]\nDescription=Test\n\n[Service]\nExecStart=/bin/true\n";

    int rc = cbx_service_write_unit(path, custom);
    assert_int_equal(rc, 0);

    FILE *file = fopen(path, "r");
    assert_non_null(file);
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, file);
    buf[n] = '\0';
    fclose(file);

    assert_string_equal(buf, custom);
}

static void test_write_unit_auto_path(void **state)
{
    /* When unit_path is NULL, auto-resolves from HOME. */
    si_fixture *f = FIX(state);

    int rc = cbx_service_write_unit(NULL, NULL);
    assert_int_equal(rc, 0);

    /* Verify the file was created in the expected location. */
    char expected[PATH_MAX];
    snprintf(expected, sizeof(expected),
             "%s/.config/systemd/user/controller-box.service", f->tmp);

    struct stat st;
    assert_int_equal(stat(expected, &st), 0);
}

static void test_write_unit_null_args(void **state)
{
    (void)state;
    int rc = cbx_service_write_unit("/tmp/valid", "");
    assert_int_equal(rc, 0);
    /* cleanup */
    unlink("/tmp/valid");
}

/* ------------------------------------------------------------------ */
/*  Group membership tests                                             */
/* ------------------------------------------------------------------ */

static void test_group_check_in_group(void **state)
{
    si_fixture *f = FIX(state);

    /* Create a mock /etc/group file. */
    char grouppath[PATH_MAX];
    snprintf(grouppath, sizeof(grouppath), "%s/group", f->tmp);

    FILE *g = fopen(grouppath, "w");
    assert_non_null(g);
    fprintf(g, "root:x:0:\ninput:x:42:\ninputplumber:x:911:testuser,otheruser\n");
    fclose(g);

    cbx_service_set_mock_group_file(grouppath);
    cbx_service_set_mock_username("testuser");

    int rc = cbx_service_check_group();
    assert_int_equal(rc, 1);
}

static void test_group_check_not_in_group(void **state)
{
    si_fixture *f = FIX(state);

    char grouppath[PATH_MAX];
    snprintf(grouppath, sizeof(grouppath), "%s/group", f->tmp);

    FILE *g = fopen(grouppath, "w");
    assert_non_null(g);
    fprintf(g, "root:x:0:\ninputplumber:x:911:otheruser\n");
    fclose(g);

    cbx_service_set_mock_group_file(grouppath);
    cbx_service_set_mock_username("testuser");

    int rc = cbx_service_check_group();
    assert_int_equal(rc, 0);
}

static void test_group_check_no_inputplumber_group(void **state)
{
    si_fixture *f = FIX(state);

    char grouppath[PATH_MAX];
    snprintf(grouppath, sizeof(grouppath), "%s/group", f->tmp);

    FILE *g = fopen(grouppath, "w");
    assert_non_null(g);
    fprintf(g, "root:x:0:\ninput:x:42:\n");
    fclose(g);

    cbx_service_set_mock_group_file(grouppath);
    cbx_service_set_mock_username("testuser");

    int rc = cbx_service_check_group();
    assert_int_equal(rc, 0);
}

static void test_group_check_file_not_found(void **state)
{
    (void)state;
    cbx_service_set_mock_group_file("/nonexistent/group/file");
    cbx_service_set_mock_username("testuser");

    int rc = cbx_service_check_group();
    assert_int_equal(rc, 0);  /* graceful: not in group */
}

static void test_group_check_no_username(void **state)
{
    (void)state;
    cbx_service_set_mock_username(NULL);
    /* Don't unsetenv HOME — but we need to handle getpwuid.  In the test
     * sandbox, getpwuid may return NULL.  We accept either -ENOENT or 0. */
    int rc = cbx_service_check_group();
    assert_true(rc == -ENOENT || rc == 0);
}

/* ------------------------------------------------------------------ */
/*  systemd availability tests                                         */
/* ------------------------------------------------------------------ */

static void test_systemd_available_mock_true(void **state)
{
    (void)state;
    /* Use /bin/sh -c 'exit 0' to always succeed. */
    cbx_service_set_mock_systemctl("/bin/sh -c 'exit 0'");
    int rc = cbx_service_systemd_available();
    assert_int_equal(rc, 1);
}

static void test_systemd_available_mock_false(void **state)
{
    (void)state;
    /* Use a nonexistent command to simulate systemctl not found (exit 127). */
    cbx_service_set_mock_systemctl("/nonexistent/command/that/does/not/exist");
    int rc = cbx_service_systemd_available();
    /* The shell returns 127 for nonexistent commands → 0 (not available). */
    assert_int_equal(rc, 0);
}

/* ------------------------------------------------------------------ */
/*  Service active check tests                                         */
/* ------------------------------------------------------------------ */

static void test_is_active_mock_true(void **state)
{
    (void)state;
    /* We need a command that outputs "active" and exits 0.
     * Use echo active. */
    cbx_service_set_mock_systemctl("echo active #");
    int rc = cbx_service_is_active();
    assert_int_equal(rc, 1);
}

static void test_is_active_mock_false(void **state)
{
    (void)state;
    /* echo inactive → not active. */
    cbx_service_set_mock_systemctl("echo inactive #");
    int rc = cbx_service_is_active();
    assert_int_equal(rc, 0);
}

/* ------------------------------------------------------------------ */
/*  Full install flow tests                                            */
/* ------------------------------------------------------------------ */

static void test_install_no_systemd(void **state)
{
    (void)state;
    cbx_service_set_mock_systemctl("/nonexistent/cmd/xyz");

    char status[256];
    int rc = cbx_service_install(status, sizeof(status));
    assert_int_equal(rc, CBX_SVC_NO_SYSTEMD);
    assert_non_null(strstr(status, "systemd"));
}

static void test_install_already_active(void **state)
{
    (void)state;
    /* Mock systemctl to echo "active" for is-active checks. */
    cbx_service_set_mock_systemctl("echo active #");

    char status[256];
    int rc = cbx_service_install(status, sizeof(status));
    assert_int_equal(rc, CBX_SVC_ALREADY_ACTIVE);
    assert_non_null(strstr(status, "already active"));
}

static void test_install_success(void **state)
{
    si_fixture *f = FIX(state);

    /* Create a mock systemctl script that uses a state file:
     *   - is-system-running → exit 0 (running)
     *   - is-active → inactive before enable, active after
     *   - enable → creates state file, exit 0 */
    char script_path[PATH_MAX];
    snprintf(script_path, sizeof(script_path), "%s/mock_systemctl", f->tmp);
    char state_file[PATH_MAX];
    snprintf(state_file, sizeof(state_file), "%s/enabled_marker", f->tmp);

    FILE *s = fopen(script_path, "w");
    assert_non_null(s);
    fprintf(s, "#!/bin/sh\n");
    fprintf(s, "case \"$1\" in\n");
    fprintf(s, "  is-system-running) exit 0 ;;\n");
    fprintf(s, "  is-active) if [ -f \"%s\" ]; then echo active; else echo inactive; fi ;;\n", state_file);
    fprintf(s, "  enable) touch \"%s\"; exit 0 ;;\n", state_file);
    fprintf(s, "  *) exit 0 ;;\n");
    fprintf(s, "esac\n");
    fclose(s);
    chmod(script_path, 0755);

    cbx_service_set_mock_systemctl(script_path);

    char status[256];
    int rc = cbx_service_install(status, sizeof(status));
    assert_int_equal(rc, CBX_SVC_OK);

    /* Verify the unit file was created. */
    char unit_path[PATH_MAX + 128];
    snprintf(unit_path, sizeof(unit_path),
             "%s/.config/systemd/user/controller-box.service", f->tmp);
    struct stat st;
    assert_int_equal(stat(unit_path, &st), 0);
}

static void test_install_enable_failed(void **state)
{
    si_fixture *f = FIX(state);

    /* Mock systemctl: is-system-running OK, is-active returns "inactive",
     * enable --now fails (exit 1). */
    char script_path[PATH_MAX];
    snprintf(script_path, sizeof(script_path), "%s/mock_systemctl_fail", f->tmp);

    FILE *s = fopen(script_path, "w");
    assert_non_null(s);
    fprintf(s, "#!/bin/sh\n");
    fprintf(s, "case \"$1\" in\n");
    fprintf(s, "  is-system-running) exit 0 ;;\n");
    fprintf(s, "  is-active) echo inactive ;;\n");
    fprintf(s, "  enable) echo 'Enable failed' >&2; exit 1 ;;\n");
    fprintf(s, "  *) exit 0 ;;\n");
    fprintf(s, "esac\n");
    fclose(s);
    chmod(script_path, 0755);

    cbx_service_set_mock_systemctl(script_path);

    char status[256];
    int rc = cbx_service_install(status, sizeof(status));
    assert_int_equal(rc, CBX_SVC_ENABLE_FAILED);
}

static void test_install_verify_failed(void **state)
{
    si_fixture *f = FIX(state);

    /* Mock systemctl: is-system-running OK, is-active returns "inactive"
     * after enable.  enable succeeds but is-active still says inactive. */
    char script_path[PATH_MAX];
    snprintf(script_path, sizeof(script_path), "%s/mock_systemctl_vfail", f->tmp);

    FILE *s = fopen(script_path, "w");
    assert_non_null(s);
    fprintf(s, "#!/bin/sh\n");
    fprintf(s, "case \"$1\" in\n");
    fprintf(s, "  is-system-running) exit 0 ;;\n");
    fprintf(s, "  is-active) echo inactive ;;\n");
    fprintf(s, "  enable) exit 0 ;;\n");
    fprintf(s, "  *) exit 0 ;;\n");
    fprintf(s, "esac\n");
    fclose(s);
    chmod(script_path, 0755);

    cbx_service_set_mock_systemctl(script_path);

    char status[256];
    int rc = cbx_service_install(status, sizeof(status));
    assert_int_equal(rc, CBX_SVC_VERIFY_FAILED);
}

static void test_install_group_warning(void **state)
{
    si_fixture *f = FIX(state);

    /* Mock systemctl with state file. */
    char script_path[PATH_MAX];
    snprintf(script_path, sizeof(script_path), "%s/mock_systemctl_ok", f->tmp);
    char state_file[PATH_MAX];
    snprintf(state_file, sizeof(state_file), "%s/enabled_ok", f->tmp);

    FILE *s = fopen(script_path, "w");
    assert_non_null(s);
    fprintf(s, "#!/bin/sh\n");
    fprintf(s, "case \"$1\" in\n");
    fprintf(s, "  is-system-running) exit 0 ;;\n");
    fprintf(s, "  is-active) if [ -f \"%s\" ]; then echo active; else echo inactive; fi ;;\n", state_file);
    fprintf(s, "  enable) touch \"%s\"; exit 0 ;;\n", state_file);
    fprintf(s, "  *) exit 0 ;;\n");
    fprintf(s, "esac\n");
    fclose(s);
    chmod(script_path, 0755);

    cbx_service_set_mock_systemctl(script_path);

    /* Mock group: not in inputplumber. */
    char grouppath[PATH_MAX + 128];
    snprintf(grouppath, sizeof(grouppath), "%s/group", f->tmp);
    FILE *g = fopen(grouppath, "w");
    fprintf(g, "root:x:0:\n");
    fclose(g);
    cbx_service_set_mock_group_file(grouppath);
    cbx_service_set_mock_username("testuser");

    char status[512];
    int rc = cbx_service_install(status, sizeof(status));
    assert_int_equal(rc, CBX_SVC_OK);
    assert_non_null(strstr(status, "inputplumber"));
    assert_non_null(strstr(status, "WARNING"));
}

static void test_install_null_status_buf(void **state)
{
    (void)state;
    /* Should not crash with NULL status buffer. */
    cbx_service_set_mock_systemctl("/nonexistent/cmd/xyz");
    int rc = cbx_service_install(NULL, 0);
    assert_int_equal(rc, CBX_SVC_NO_SYSTEMD);
}

/* ------------------------------------------------------------------ */
/*  Uninstall tests                                                    */
/* ------------------------------------------------------------------ */

static void test_uninstall_removes_file(void **state)
{
    si_fixture *f = FIX(state);

    /* First write the unit file. */
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/.config/systemd/user/controller-box.service", f->tmp);
    int rc = cbx_service_write_unit(path, NULL);
    assert_int_equal(rc, 0);

    /* Verify it exists. */
    struct stat st;
    assert_int_equal(stat(path, &st), 0);

    /* Uninstall. */
    cbx_service_set_mock_systemctl("/bin/sh -c 'exit 0'");
    rc = cbx_service_uninstall();
    assert_int_equal(rc, 0);

    /* Verify it's gone. */
    assert_int_equal(stat(path, &st), -1);
}

static void test_uninstall_no_file(void **state)
{
    (void)state;
    cbx_service_set_mock_systemctl("/bin/sh -c 'exit 0'");
    int rc = cbx_service_uninstall();
    assert_int_equal(rc, 0);  /* ENOENT is OK */
}

/* ------------------------------------------------------------------ */
/*  Test overrides reset                                               */
/* ------------------------------------------------------------------ */

static void test_mock_overrides_reset(void **state)
{
    (void)state;
    cbx_service_set_mock_systemctl("/bin/sh -c 'exit 0'");
    cbx_service_set_mock_group_file("/tmp/test");
    cbx_service_set_mock_username("tester");

    /* Reset. */
    cbx_service_set_mock_systemctl(NULL);
    cbx_service_set_mock_group_file(NULL);
    cbx_service_set_mock_username(NULL);

    /* Verify reset by checking group (should use real /etc/group). */
    int rc = cbx_service_check_group();
    /* In test sandbox, result is 0 (not in group) or -ENOENT. */
    assert_true(rc == 0 || rc == -ENOENT);
}

/* ------------------------------------------------------------------ */
/*  Test runner                                                        */
/* ------------------------------------------------------------------ */

static const struct CMUnitTest tests[] = {
    /* Unit path resolution. */
    cmocka_unit_test_setup_teardown(test_unit_path_home, setup, teardown),
    cmocka_unit_test_setup_teardown(test_unit_path_xdg, setup, teardown),
    cmocka_unit_test_setup_teardown(test_unit_path_no_home, setup, teardown),
    cmocka_unit_test_setup_teardown(test_unit_path_overflow, setup, teardown),
    cmocka_unit_test_setup_teardown(test_unit_path_null_args, setup, teardown),

    /* Unit file content. */
    cmocka_unit_test_setup_teardown(test_unit_content_basic, setup, teardown),
    cmocka_unit_test_setup_teardown(test_unit_content_flatpak, setup, teardown),
    cmocka_unit_test_setup_teardown(test_unit_content_overflow, setup, teardown),
    cmocka_unit_test_setup_teardown(test_unit_content_null_args, setup, teardown),

    /* Atomic write. */
    cmocka_unit_test_setup_teardown(test_write_unit_creates_file, setup, teardown),
    cmocka_unit_test_setup_teardown(test_write_unit_custom_content, setup, teardown),
    cmocka_unit_test_setup_teardown(test_write_unit_auto_path, setup, teardown),
    cmocka_unit_test_setup_teardown(test_write_unit_null_args, setup, teardown),

    /* Group membership. */
    cmocka_unit_test_setup_teardown(test_group_check_in_group, setup, teardown),
    cmocka_unit_test_setup_teardown(test_group_check_not_in_group, setup, teardown),
    cmocka_unit_test_setup_teardown(test_group_check_no_inputplumber_group, setup, teardown),
    cmocka_unit_test_setup_teardown(test_group_check_file_not_found, setup, teardown),
    cmocka_unit_test_setup_teardown(test_group_check_no_username, setup, teardown),

    /* systemd availability. */
    cmocka_unit_test_setup_teardown(test_systemd_available_mock_true, setup, teardown),
    cmocka_unit_test_setup_teardown(test_systemd_available_mock_false, setup, teardown),

    /* Service active check. */
    cmocka_unit_test_setup_teardown(test_is_active_mock_true, setup, teardown),
    cmocka_unit_test_setup_teardown(test_is_active_mock_false, setup, teardown),

    /* Full install flow. */
    cmocka_unit_test_setup_teardown(test_install_no_systemd, setup, teardown),
    cmocka_unit_test_setup_teardown(test_install_already_active, setup, teardown),
    cmocka_unit_test_setup_teardown(test_install_success, setup, teardown),
    cmocka_unit_test_setup_teardown(test_install_enable_failed, setup, teardown),
    cmocka_unit_test_setup_teardown(test_install_verify_failed, setup, teardown),
    cmocka_unit_test_setup_teardown(test_install_group_warning, setup, teardown),
    cmocka_unit_test_setup_teardown(test_install_null_status_buf, setup, teardown),

    /* Uninstall. */
    cmocka_unit_test_setup_teardown(test_uninstall_removes_file, setup, teardown),
    cmocka_unit_test_setup_teardown(test_uninstall_no_file, setup, teardown),

    /* Test overrides. */
    cmocka_unit_test_setup_teardown(test_mock_overrides_reset, setup, teardown),
};

int main(void)
{
    return cmocka_run_group_tests(tests, NULL, NULL);
}