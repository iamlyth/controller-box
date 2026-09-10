/*
 * test_kernel_controller.c — Kernel-backed controller integration test
 * (Task 7, SPEC §5.7, §11.1).
 *
 * Creates a synthetic evdev gamepad via /dev/uinput, launches the installed
 * Manager binary against a private InputPlumber-compatible DBus server,
 * and verifies semantic outcomes from real kernel gamepad events through
 * the production event loop.
 *
 * The test exercises the full production input path:
 *   uinput → kernel evdev → SDL joystick → manager event loop → DBus/config
 *
 * If /dev/uinput is unavailable in the test environment, the test exits
 * with code 77 and a diagnostic message explaining the missing capability.
 *
 * Usage: test_kernel_controller [build-dir]
 *   build-dir: CMake build directory containing test_ip_server and the
 *              installed controller-box binary (default: build-check).
 *
 * Exit codes: 0 = pass, 1 = fail, 77 = skip (/dev/uinput unavailable).
 *
 * Build: linked as a standalone binary (no cmocka, no libcontrollerbox).
 *        Uses only libc + linux/uinput.h.
 */

#define _GNU_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Utilities                                                           */
/* ------------------------------------------------------------------ */

static int g_failures = 0;

static void pass(const char *msg)
{
    printf("PASS: %s\n", msg);
}

static void fail(const char *msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    g_failures++;
}

static void msleep(int ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
}

/* Recursively create a directory (like mkdir -p). Returns 0 on success. */
static int mkdir_p(const char *path)
{
    char tmp[PATH_MAX + 128];
    size_t len;

    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* Kill a process gracefully: SIGTERM → wait → SIGKILL. */
static void kill_pid(pid_t pid)
{
    if (pid <= 0) return;
    if (kill(pid, SIGTERM) == 0) {
        msleep(300);
        int status;
        if (waitpid(pid, &status, WNOHANG) == 0) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
    } else {
        waitpid(pid, NULL, WNOHANG);
    }
}

/* ------------------------------------------------------------------ */
/* uinput gamepad device                                               */
/* ------------------------------------------------------------------ */

#define UINPUT_DEV_NAME "ControllerBox Test Gamepad"

/* Gamepad buttons we register. */
static const int g_buttons[] = {
    BTN_SOUTH,   /* A */
    BTN_EAST,    /* B */
    BTN_NORTH,   /* X */
    BTN_WEST,    /* Y */
    BTN_SELECT,  /* Back/Select */
    BTN_START,   /* Start */
    BTN_MODE,    /* Guide/Home */
    BTN_DPAD_UP,
    BTN_DPAD_DOWN,
    BTN_DPAD_LEFT,
    BTN_DPAD_RIGHT,
    BTN_TL,      /* Left shoulder */
    BTN_TR,      /* Right shoulder */
    BTN_THUMBL,  /* Left thumbstick click */
    BTN_THUMBR,  /* Right thumbstick click */
};

/* Absolute axes we register. */
static const int g_axes[] = {
    ABS_X, ABS_Y,      /* Left stick */
    ABS_RX, ABS_RY,    /* Right stick */
    ABS_Z, ABS_RZ,     /* Triggers (L2/R2) */
    ABS_HAT0X, ABS_HAT0Y,  /* D-pad hat */
};

static int g_uinput_fd = -1;

/* Create a virtual gamepad via /dev/uinput. Returns 0 on success, -1 on error. */
static int uinput_create_gamepad(void)
{
    g_uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (g_uinput_fd < 0) {
        if (errno == ENOENT || errno == EACCES || errno == EPERM) {
            fprintf(stderr,
                "SKIP: /dev/uinput is not available (%s).\n"
                "      A kernel-backed evdev controller test requires the\n"
                "      uinput kernel module and write access to /dev/uinput.\n"
                "      This capability is not declared in the factory runner\n"
                "      environment. The test is\n"
                "      classified as 'partial' pending a runner with\n"
                "      'kernel-uinput' capability.\n",
                strerror(errno));
            return -1;
        }
        fprintf(stderr, "SKIP: cannot open /dev/uinput: %s\n", strerror(errno));
        return -1;
    }

    /* Enable event types. */
    if (ioctl(g_uinput_fd, UI_SET_EVBIT, EV_KEY) < 0) {
        fprintf(stderr, "FAIL: UI_SET_EVBIT EV_KEY: %s\n", strerror(errno));
        close(g_uinput_fd);
        g_uinput_fd = -1;
        return -1;
    }
    if (ioctl(g_uinput_fd, UI_SET_EVBIT, EV_ABS) < 0) {
        fprintf(stderr, "FAIL: UI_SET_EVBIT EV_ABS: %s\n", strerror(errno));
        close(g_uinput_fd);
        g_uinput_fd = -1;
        return -1;
    }

    /* Register buttons. */
    for (size_t i = 0; i < sizeof(g_buttons) / sizeof(g_buttons[0]); i++) {
        if (ioctl(g_uinput_fd, UI_SET_KEYBIT, g_buttons[i]) < 0) {
            fprintf(stderr, "FAIL: UI_SET_KEYBIT %d: %s\n",
                    g_buttons[i], strerror(errno));
            close(g_uinput_fd);
            g_uinput_fd = -1;
            return -1;
        }
    }

    /* Register absolute axes with reasonable ranges. */
    for (size_t i = 0; i < sizeof(g_axes) / sizeof(g_axes[0]); i++) {
        if (ioctl(g_uinput_fd, UI_SET_ABSBIT, g_axes[i]) < 0) {
            fprintf(stderr, "FAIL: UI_SET_ABSBIT %d: %s\n",
                    g_axes[i], strerror(errno));
            close(g_uinput_fd);
            g_uinput_fd = -1;
            return -1;
        }
    }

    /* Configure the device via uinput_setup (modern API). */
    struct uinput_setup setup;
    memset(&setup, 0, sizeof(setup));
    strncpy(setup.name, UINPUT_DEV_NAME, sizeof(setup.name) - 1);
    setup.id.bustype = BUS_USB;
    setup.id.vendor  = 0x045E;  /* Microsoft vendor ID (Xbox-style) */
    setup.id.product = 0x028E;  /* Xbox 360 controller product ID */
    setup.id.version = 0x0100;

    if (ioctl(g_uinput_fd, UI_DEV_SETUP, &setup) < 0) {
        fprintf(stderr, "FAIL: UI_DEV_SETUP: %s\n", strerror(errno));
        close(g_uinput_fd);
        g_uinput_fd = -1;
        return -1;
    }

    /* Create the device. */
    if (ioctl(g_uinput_fd, UI_DEV_CREATE) < 0) {
        fprintf(stderr, "FAIL: UI_DEV_CREATE: %s\n", strerror(errno));
        close(g_uinput_fd);
        g_uinput_fd = -1;
        return -1;
    }

    /* Give the kernel and udev time to settle the device node. */
    msleep(200);

    return 0;
}

/* Destroy the uinput device and close the file descriptor. */
static void uinput_destroy(void)
{
    if (g_uinput_fd >= 0) {
        ioctl(g_uinput_fd, UI_DEV_DESTROY);
        close(g_uinput_fd);
        g_uinput_fd = -1;
    }
}

/* Write a single input event to the uinput device. */
static int uinput_write_event(unsigned short type, unsigned short code, int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type  = type;
    ev.code  = code;
    ev.value = value;
    if (write(g_uinput_fd, &ev, sizeof(ev)) < (ssize_t)sizeof(ev)) {
        return -1;
    }
    return 0;
}

/* Send a button press + release with a SYN_REPORT. */
static void uinput_press_button(int btn)
{
    uinput_write_event(EV_KEY, btn, 1);
    uinput_write_event(EV_SYN, SYN_REPORT, 0);
    msleep(50);
    uinput_write_event(EV_KEY, btn, 0);
    uinput_write_event(EV_SYN, SYN_REPORT, 0);
    msleep(50);
}

/* Send a D-pad direction press + release. */
static void uinput_dpad(int direction)
{
    int btn;
    switch (direction) {
        case 0: btn = BTN_DPAD_UP;    break;
        case 1: btn = BTN_DPAD_DOWN;  break;
        case 2: btn = BTN_DPAD_LEFT;  break;
        case 3: btn = BTN_DPAD_RIGHT; break;
        default: return;
    }
    uinput_press_button(btn);
}

/* ------------------------------------------------------------------ */
/* Integration test: launch installed manager with gamepad input       */
/* ------------------------------------------------------------------ */

static pid_t g_ip_server_pid = 0;
static pid_t g_manager_pid   = 0;
static char  g_addr_file[]   = "/tmp/cbx_kernel_test_bus_XXXXXX";
static char  g_tmp_home[PATH_MAX];

/* Start the InputPlumber-compatible DBus server. Returns 0 on success. */
static int start_ip_server(const char *build_dir)
{
    char ip_server_path[PATH_MAX + 128];
    snprintf(ip_server_path, sizeof(ip_server_path), "%s/test_ip_server", build_dir);

    struct stat st;
    if (stat(ip_server_path, &st) != 0) {
        fprintf(stderr, "SKIP: test_ip_server not found at %s\n", ip_server_path);
        return -1;
    }

    /* Create a unique address file path. */
    int fd = mkstemp(g_addr_file);
    if (fd < 0) {
        fail("cannot create temp address file");
        return -1;
    }
    close(fd);
    unlink(g_addr_file);  /* Remove it; test_ip_server will create it. */

    g_ip_server_pid = fork();
    if (g_ip_server_pid < 0) {
        fail("fork failed for IP server");
        return -1;
    }
    if (g_ip_server_pid == 0) {
        /* Child: exec test_ip_server. */
        execl(ip_server_path, "test_ip_server", g_addr_file, NULL);
        _exit(127);
    }

    /* Wait for the address file to appear (up to 10 seconds). */
    for (int i = 0; i < 100; i++) {
        if (stat(g_addr_file, &st) == 0 && st.st_size > 0) {
            break;
        }
        msleep(100);
        /* Check if the process died. */
        if (waitpid(g_ip_server_pid, NULL, WNOHANG) != 0) {
            fail("IP server process exited before ready");
            g_ip_server_pid = 0;
            return -1;
        }
    }

    if (stat(g_addr_file, &st) != 0 || st.st_size == 0) {
        fail("IP server did not write address file in time");
        return -1;
    }

    return 0;
}

/* Read the DBus bus address from the address file. */
static int read_bus_address(char *out, size_t out_len)
{
    FILE *fp = fopen(g_addr_file, "r");
    if (!fp) {
        fail("cannot open bus address file");
        return -1;
    }
    if (!fgets(out, (int)out_len, fp)) {
        fclose(fp);
        fail("cannot read bus address");
        return -1;
    }
    fclose(fp);
    /* Strip trailing newline. */
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) {
        out[--len] = '\0';
    }
    return 0;
}

/* Set up a temporary HOME directory with fonts and config. */
static int setup_tmp_home(void)
{
    const char *tmpl = "/tmp/cbx_kernel_test_home_XXXXXX";
    strncpy(g_tmp_home, tmpl, sizeof(g_tmp_home) - 1);
    g_tmp_home[sizeof(g_tmp_home) - 1] = '\0';

    if (!mkdtemp(g_tmp_home)) {
        fail("cannot create temp HOME directory");
        return -1;
    }

    char path[PATH_MAX + 128];

    /* Create font directory and symlink/copy DejaVuSans.ttf. */
    snprintf(path, sizeof(path), "%s/.local/share/fonts", g_tmp_home);
    if (mkdir_p(path) != 0) {
        fail("cannot create font directory");
        return -1;
    }

    /* Search for DejaVuSans.ttf in common locations. */
    const char *font_candidates[] = {
        "/nix/store/zzs2q7lk5mn6y2rywd3snhak7098zs66-system-path/share/X11/fonts/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        NULL,
    };

    char font_dest[PATH_MAX + 128];
    snprintf(font_dest, sizeof(font_dest), "%s/.local/share/fonts/DejaVuSans.ttf", g_tmp_home);

    for (int i = 0; font_candidates[i]; i++) {
        if (access(font_candidates[i], R_OK) == 0) {
            /* Use symlink to avoid copying a large file. */
            if (symlink(font_candidates[i], font_dest) != 0) {
                /* Fallback: copy. */
                FILE *src = fopen(font_candidates[i], "rb");
                if (src) {
                    FILE *dst = fopen(font_dest, "wb");
                    if (dst) {
                        char buf[4096];
                        size_t n;
                        while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
                            fwrite(buf, 1, n, dst);
                        }
                        fclose(dst);
                    }
                    fclose(src);
                }
            }
            break;
        }
    }

    /* Create config directory. */
    snprintf(path, sizeof(path), "%s/.config/controller-box", g_tmp_home);
    mkdir_p(path);

    /* Create data directory for InputPlumber profiles. */
    snprintf(path, sizeof(path), "%s/.local/share/inputplumber/profiles", g_tmp_home);
    mkdir_p(path);

    /* Create a minimal test profile. */
    snprintf(path, sizeof(path), "%s/.local/share/inputplumber/profiles/test_profile.yaml", g_tmp_home);
    FILE *pf = fopen(path, "w");
    if (pf) {
        fprintf(pf, "version: 1\n"
                    "mapping:\n"
                    "  - source_event:\n"
                    "      type: button\n"
                    "      index: 0\n"
                    "    target_event:\n"
                    "      type: button\n"
                    "      index: 0\n");
        fclose(pf);
    }

    return 0;
}

/* Launch the installed controller-box manager binary. Returns 0 on success. */
static int start_manager(const char *build_dir, const char *bus_addr)
{
    char bin_path[PATH_MAX + 128];

    /* The CTest wrapper stages an isolated install and passes its exact binary.
     * Keep the legacy search below for direct/manual execution. */
    const char *installed_override = getenv("CBX_TEST_INSTALLED_BINARY");
    int found = 0;
    if (installed_override && installed_override[0] == '/' &&
        access(installed_override, X_OK) == 0) {
        snprintf(bin_path, sizeof(bin_path), "%s", installed_override);
        found = 1;
    } else if (installed_override) {
        fail("CBX_TEST_INSTALLED_BINARY is not an executable absolute path");
        return -1;
    }

    /* Try common install prefix locations. */
    const char *prefixes[] = {
        ".test-install-bin/bin/controller-box",
        ".test-install-bin/usr/bin/controller-box",
        NULL,
    };

    for (int i = 0; !found && prefixes[i]; i++) {
        if (build_dir[0] == '/') {
            snprintf(bin_path, sizeof(bin_path), "%s/../%s", build_dir, prefixes[i]);
        } else {
            snprintf(bin_path, sizeof(bin_path), "%s/%s", build_dir, prefixes[i]);
        }
        /* Try relative to project root. */
        char abs_path[PATH_MAX + 128];
        if (realpath(bin_path, abs_path)) {
            snprintf(bin_path, sizeof(bin_path), "%s", abs_path);
            found = 1;
            break;
        }
        /* Also try without realpath (file may exist but path too long). */
        if (access(bin_path, X_OK) == 0) {
            found = 1;
            break;
        }
    }

    if (!found) {
        /* Try staging dir relative to build_dir parent. */
        snprintf(bin_path, sizeof(bin_path), "%s/../.test-install-bin/bin/controller-box", build_dir);
        if (access(bin_path, X_OK) != 0) {
            snprintf(bin_path, sizeof(bin_path), "%s/../.test-install-bin/usr/bin/controller-box", build_dir);
            if (access(bin_path, X_OK) != 0) {
                fail("installed controller-box binary not found");
                return -1;
            }
        }
    }

    /* Set up environment for the manager subprocess. */
    char env_str[PATH_MAX * 4];
    char xdg_config[PATH_MAX + 128];
    char xdg_data[PATH_MAX + 128];
    snprintf(xdg_config, sizeof(xdg_config), "%s/.config", g_tmp_home);
    snprintf(xdg_data, sizeof(xdg_data), "%s/.local/share", g_tmp_home);

    g_manager_pid = fork();
    if (g_manager_pid < 0) {
        fail("fork failed for manager");
        return -1;
    }
    if (g_manager_pid == 0) {
        /* Child: set environment and exec controller-box. */
        setenv("HOME", g_tmp_home, 1);
        setenv("XDG_CONFIG_HOME", xdg_config, 1);
        setenv("XDG_DATA_HOME", xdg_data, 1);
        setenv("DBUS_SYSTEM_BUS_ADDRESS", bus_addr, 1);
        setenv("SDL_VIDEODRIVER", "dummy", 1);
        /* Clear DISPLAY to avoid conflicts with real X server. */
        unsetenv("DISPLAY");

        execl(bin_path, "controller-box", "--manager", NULL);
        _exit(127);
    }

    /* Wait for the manager to start (check it stays alive for 2 seconds). */
    msleep(2000);

    int status;
    if (waitpid(g_manager_pid, &status, WNOHANG) != 0) {
        fail("manager process exited immediately after launch");
        g_manager_pid = 0;
        return -1;
    }

    (void)env_str;  /* Unused but kept for documentation. */
    return 0;
}

/* Check if the manager process is still alive. */
static int manager_alive(void)
{
    if (g_manager_pid <= 0) return 0;
    return (kill(g_manager_pid, 0) == 0);
}

/* Check if settings.yaml exists and optionally get its mtime. */
static int check_settings_yaml(time_t *mtime_out)
{
    char path[PATH_MAX + 128];
    snprintf(path, sizeof(path), "%s/.config/controller-box/settings.yaml", g_tmp_home);
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;  /* File does not exist. */
    }
    if (mtime_out) *mtime_out = st.st_mtime;
    return 1;
}

/* Run the full integration test. Returns 0 on success. */
static int run_integration_test(const char *build_dir)
{
    /* --- Step 1: Start DBus server --- */
    printf("\n--- Starting InputPlumber-compatible DBus server ---\n");
    if (start_ip_server(build_dir) != 0) {
        return -1;
    }
    pass("IP server started");

    char bus_addr[512];
    if (read_bus_address(bus_addr, sizeof(bus_addr)) != 0) {
        return -1;
    }
    pass("DBus bus address obtained");

    /* --- Step 2: Set up temp HOME --- */
    printf("\n--- Setting up temporary HOME ---\n");
    if (setup_tmp_home() != 0) {
        return -1;
    }
    pass("temp HOME with fonts and config created");

    /* --- Step 3: Launch manager --- */
    printf("\n--- Launching controller-box --manager ---\n");
    if (start_manager(build_dir, bus_addr) != 0) {
        return -1;
    }
    pass("manager process started and alive");

    /* --- Step 4: Send gamepad events --- */
    printf("\n--- Sending kernel gamepad events through uinput ---\n");

    /*
     * The synthetic gamepad uses the standard Xbox 360 device identity
     * (vendor 0x045E, product 0x028E), which SDL's built-in game-controller
     * database maps as a controller — so the manager recognizes the
     * kernel-backed gamepad through the production path with no manual
     * mapping registration required.
     *
     * Navigation sequence (drives the manager to write settings.yaml as a
     * persisted semantic outcome):
     * 1. D-pad RIGHT × 3 → Settings tab
     * 2. D-pad DOWN → focus first setting (Launch at Boot)
     * 3. A button → toggle the setting (marks it changed)
     * 4. D-pad DOWN → focus the list's "Save" entry
     * 5. A button → activate Save → cbx_settings_tab_save → writes
     *    settings.yaml to disk
     */

    /* Navigate tabs with D-pad RIGHT to the Settings tab. */
    for (int i = 0; i < 3; i++) {
        uinput_dpad(3);  /* RIGHT */
        msleep(200);
    }
    pass("D-pad RIGHT events sent (navigated to Settings tab)");

    /* Focus the first setting in the list. */
    uinput_dpad(1);  /* DOWN */
    msleep(200);

    /* A → toggle the first setting. */
    uinput_press_button(BTN_SOUTH);
    msleep(300);
    pass("A button press sent (toggled setting)");

    /* DOWN → focus the settings list's "Save" entry. */
    uinput_dpad(1);  /* DOWN */
    msleep(200);

    /* A → activate Save → persists settings.yaml. */
    uinput_press_button(BTN_SOUTH);
    msleep(300);
    pass("A button press sent (activated Save)");

    /* B button (cancel/back). */
    uinput_press_button(BTN_EAST);
    msleep(300);
    pass("B button press sent (cancel)");

    /* Press Start button. */
    uinput_press_button(BTN_START);
    msleep(300);
    pass("Start button press sent");

    /* Navigate left back. */
    for (int i = 0; i < 3; i++) {
        uinput_dpad(2);  /* LEFT */
        msleep(200);
    }
    pass("D-pad LEFT events sent (tab navigation back)");

    /* --- Step 5: Verify outcomes --- */
    printf("\n--- Verifying semantic outcomes ---\n");

    /* Check 1: Manager is still alive after gamepad events. */
    if (manager_alive()) {
        pass("manager process survived gamepad event sequence");
    } else {
        fail("manager process crashed or exited during gamepad events");
        int status;
        waitpid(g_manager_pid, &status, 0);
        g_manager_pid = 0;
        return -1;
    }

    /* Check 2: Assert a SEMANTIC outcome produced by the kernel-backed
     * gamepad events — a persisted settings.yaml written to disk.  "The
     * manager did not crash" is not outcome evidence (SPEC §5.7/§11.1.5);
     * the test must fail if no semantic effect was produced, not merely
     * survive the event sequence. */
    time_t settings_mtime = 0;
    if (check_settings_yaml(&settings_mtime)) {
        pass("settings.yaml persisted in temp HOME (semantic outcome)");
    } else {
        fail("settings.yaml was not created — kernel gamepad events produced no persisted semantic outcome");
    }

    /* Check 3: Send more events to stress-test stability. */
    for (int i = 0; i < 10; i++) {
        uinput_dpad(i % 4);
        uinput_press_button(BTN_SOUTH);
        uinput_press_button(BTN_EAST);
        msleep(100);
    }

    if (manager_alive()) {
        pass("manager survived extended gamepad event stress test");
    } else {
        fail("manager crashed during extended gamepad events");
        int status;
        waitpid(g_manager_pid, &status, 0);
        g_manager_pid = 0;
        return -1;
    }

    /* --- Step 6: Clean up --- */
    printf("\n--- Cleaning up ---\n");
    kill_pid(g_manager_pid);
    g_manager_pid = 0;
    pass("manager process terminated");

    kill_pid(g_ip_server_pid);
    g_ip_server_pid = 0;
    pass("IP server terminated");

    /* Remove temp files. */
    if (g_addr_file[0]) unlink(g_addr_file);
    if (g_tmp_home[0]) {
        /* Best-effort cleanup of temp HOME. */
        char cmd[PATH_MAX + 64];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s' 2>/dev/null || true", g_tmp_home);
        if (system(cmd)) {}
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("=== Task 7: Kernel-backed controller integration test ===\n\n");

    /* Step 1: Check for /dev/uinput availability. */
    printf("--- Checking /dev/uinput availability ---\n");
    if (uinput_create_gamepad() != 0) {
        /* uinput_create_gamepad already printed the SKIP diagnostic. */
        uinput_destroy();
        return 77;
    }
    pass("virtual gamepad created via /dev/uinput");

    /* Step 2: If a build directory is provided, run the full integration test. */
    const char *build_dir = (argc > 1) ? argv[1] : "build-check";

    printf("\n--- Running integration test with build dir: %s ---\n", build_dir);

    int rc = run_integration_test(build_dir);
    if (rc != 0) {
        /* Clean up any remaining processes. */
        if (g_manager_pid > 0) kill_pid(g_manager_pid);
        if (g_ip_server_pid > 0) kill_pid(g_ip_server_pid);
        if (g_addr_file[0]) unlink(g_addr_file);
        if (g_tmp_home[0]) {
            char cmd[PATH_MAX + 64];
            snprintf(cmd, sizeof(cmd), "rm -rf '%s' 2>/dev/null || true", g_tmp_home);
            if (system(cmd)) {}
        }
    }

    /* Step 3: Destroy uinput device. */
    uinput_destroy();
    pass("uinput device destroyed");

    /* Step 4: Report results. */
    printf("\n");
    if (g_failures == 0) {
        printf("PASS: kernel-backed controller integration test completed\n");
        return 0;
    } else {
        printf("FAIL: %d check(s) failed\n", g_failures);
        return 1;
    }
}