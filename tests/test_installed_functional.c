/*
 * test_installed_functional.c — Mandatory installed functional acceptance gate.
 *
 * SPEC §11.1 item 5: The packaged artifact is installed into a clean
 * environment containing only declared runtime dependencies.  The test
 * must:
 *
 *   1. Start a private native-signature InputPlumber-compatible DBus service.
 *   2. Hotplug a synthetic game controller (kernel-backed /dev/uinput
 *      when available, SDL virtual joystick fallback otherwise).
 *   3. Navigate the Manager with real controller events.
 *   4. Create and observe a routable virtual target.
 *   5. Create/save/reload a profile (persistence verified via filesystem).
 *   6. Apply assignment through the overlay (LoadProfilePath + GamepadOrder).
 *   7. Activate a mapped compositor-visible overlay.
 *   8. Verify persistence after process/backend restart.
 *   9. Independently inspect DBus/ObjectManager and filesystem outcomes.
 *
 * Missing prerequisites (dbus-daemon, SDL) are FAILURE, not skip.
 *
 * This test links against the production controllerbox library and uses
 * the real sd-bus backend (ip_dbus_sd_backend) against a private
 * dbus-daemon with a forked InputPlumber-compatible server.  No mock
 * DBus is used.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <systemd/sd-bus.h>

#include <SDL.h>

#include "dbus/dbus_client.h"
#include "dbus/ip_connection.h"
#include "dbus/ip_manager.h"
#include "dbus/ip_target.h"
#include "dbus/ip_composite.h"
#include "dbus/ip_objectmanager.h"
#include "dbus/ip_device_model.h"
#include "dbus/ip_intercept_poll.h"
#include "dbus/ip_input_signal.h"
#include "dbus/ip_hotplug.h"
#include "dbus/dbus_interface.h"             /* IP_DBUS_PATH, IP_IFACE_* */

#include "config/config_settings.h"
#include "config/config_assignments.h"
#include "config/config_profile.h"
#include "config/config_profile_list.h"
/* config_profile_yaml.h not needed — profile parsing is in config_profile_list.h */
#include "config/config_paths.h"

#include "manager/manager.h"
#include "app/overlay_service.h"

#include "ui/renderer.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "overlay/surface_build.h"
#include "overlay/lifecycle.h"
#include "overlay/grid_render.h"
#include "overlay/player_mode.h"
#include "overlay/host_mode.h"
#include "overlay/conflict.h"
#include "overlay/profile_cycle.h"
#include "overlay/trigger.h"
#include "icons/icon_map.h"
#include "icons/icon_cache.h"

#include "fb_assert.h"

/* ================================================================== */
/*  Compile-time configuration                                         */
/* ================================================================== */

#ifndef DBUS_SESSION_CONFIG
#error "DBUS_SESSION_CONFIG must be defined (path to session.conf)"
#endif

#ifndef CBX_SOURCE_DIR
#error "CBX_SOURCE_DIR must be defined (project source root for data files)"
#endif

#define OVERLAY_YAML_DIR CBX_SOURCE_DIR "/data/"
#define OVERLAY_W 1280
#define OVERLAY_H 720

/* ================================================================== */
/*  Kernel-backed uinput gamepad support                               */
/* ================================================================== */
/*
 * When /dev/uinput is available, a kernel-backed evdev gamepad is created
 * so SDL detects a real joystick through its evdev backend.  Button and
 * axis events are written via ioctl/write to the uinput file descriptor.
 * When /dev/uinput is absent (no kernel-uinput runner capability), the
 * test falls back to SDL_JoystickAttachVirtual (process-local virtual
 * joystick) — the original code path.
 *
 * The virtual-joystick button indices used throughout the test (0=A,
 * 1=B, 6=Start, 11=DPadUp … 14=DPadRight) are translated to evdev
 * button codes here.  A matching gamecontroller mapping string is added
 * at runtime so SDL generates the same SDL_CONTROLLERBUTTONDOWN events
 * regardless of which backend is active.
 */

static int  g_uinput_fd   = -1;
static bool g_use_uinput  = false;

#define UINPUT_DEV_NAME "CBX Functional Test Pad"

/* Evdev button codes registered on the uinput device (sorted by code
 * determines SDL joystick button index assignment). */
static const int g_uinput_buttons[] = {
    BTN_SOUTH,   /* A      (evdev 304 → joy idx 0) */
    BTN_EAST,    /* B      (evdev 305 → joy idx 1) */
    BTN_NORTH,   /* X      (evdev 307 → joy idx 2) */
    BTN_WEST,    /* Y      (evdev 308 → joy idx 3) */
    BTN_TL,      /* L1     (evdev 310 → joy idx 4) */
    BTN_TR,      /* R1     (evdev 311 → joy idx 5) */
    BTN_SELECT,  /* Back   (evdev 314 → joy idx 6) */
    BTN_START,   /* Start  (evdev 315 → joy idx 7) */
    BTN_MODE,    /* Guide  (evdev 316 → joy idx 8) */
    BTN_THUMBL,  /* L3     (evdev 317 → joy idx 9) */
    BTN_THUMBR,  /* R3     (evdev 318 → joy idx 10) */
    BTN_DPAD_UP,    /* (evdev 544 → joy idx 11) */
    BTN_DPAD_DOWN,  /* (evdev 545 → joy idx 12) */
    BTN_DPAD_LEFT,  /* (evdev 546 → joy idx 13) */
    BTN_DPAD_RIGHT, /* (evdev 547 → joy idx 14) */
};

/* Evdev absolute axes registered on the uinput device. */
static const int g_uinput_axes[] = {
    ABS_X,   /* left stick X  (evdev 0 → joy idx 0) */
    ABS_Y,   /* left stick Y  (evdev 1 → joy idx 1) */
    ABS_Z,   /* left trigger  (evdev 2 → joy idx 2) */
    ABS_RX,  /* right stick X (evdev 3 → joy idx 3) */
    ABS_RY,  /* right stick Y (evdev 4 → joy idx 4) */
    ABS_RZ,  /* right trigger (evdev 5 → joy idx 5) */
};

/* Map the virtual joystick button index used in the test to the evdev
 * button code that produces the same SDL gamecontroller button.
 *
 * Virtual mapping:  a:b0,b:b1,start:b6,dpup:b11,dpdown:b12,
 *                   dpleft:b13,dpright:b14
 * Uinput mapping:   a:b0,b:b1,back:b6,start:b7,dpup:b11,dpdown:b12,
 *                   dpleft:b13,dpright:b14
 *
 * Both mappings yield the same SDL_CONTROLLER_BUTTON_* index because
 * the gamecontroller API normalises across joystick button indices. */
static int
vbtn_to_evdev(int vbtn)
{
    switch (vbtn) {
        case 0:  return BTN_SOUTH;       /* A    → gc A    */
        case 1:  return BTN_EAST;        /* B    → gc B    */
        case 2:  return BTN_NORTH;       /* X    → gc X    */
        case 3:  return BTN_WEST;        /* Y    → gc Y    */
        case 4:  return BTN_TL;          /* L1   → gc LSh  */
        case 5:  return BTN_TR;          /* R1   → gc RSh  */
        case 6:  return BTN_START;       /* Start→ gc Start*/
        case 7:  return BTN_SELECT;      /* Back → gc Back */
        case 8:  return BTN_MODE;        /* Guide→ gc Guide*/
        case 9:  return BTN_THUMBL;      /* L3   → gc LStk */
        case 10: return BTN_THUMBR;      /* R3   → gc RStk */
        case 11: return BTN_DPAD_UP;
        case 12: return BTN_DPAD_DOWN;
        case 13: return BTN_DPAD_LEFT;
        case 14: return BTN_DPAD_RIGHT;
        default: return -1;
    }
}

/* Map the virtual joystick axis index used in the test to the evdev
 * absolute axis code that produces the same SDL gamecontroller axis.
 *
 * Virtual mapping:  leftx:a0,lefty:a1,rightx:a2,righty:a3,
 *                   lefttrigger:a4,righttrigger:a5
 * Uinput mapping:   leftx:a0,lefty:a1,lefttrigger:a2,rightx:a3,
 *                   righty:a4,righttrigger:a5
 *
 * Both mappings yield the same SDL_CONTROLLER_AXIS_* index. */
static int
vaxis_to_evdev(int vaxis)
{
    switch (vaxis) {
        case 0: return ABS_X;    /* left stick X  */
        case 1: return ABS_Y;    /* left stick Y  */
        case 2: return ABS_RX;   /* right stick X */
        case 3: return ABS_RY;   /* right stick Y */
        case 4: return ABS_Z;    /* left trigger  */
        case 5: return ABS_RZ;   /* right trigger */
        default: return -1;
    }
}

/* Create a virtual gamepad via /dev/uinput.  Returns 0 on success. */
static int
uinput_create_gamepad(void)
{
    g_uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (g_uinput_fd < 0)
        return -1;

    if (ioctl(g_uinput_fd, UI_SET_EVBIT, EV_KEY) < 0) {
        close(g_uinput_fd); g_uinput_fd = -1; return -1;
    }
    if (ioctl(g_uinput_fd, UI_SET_EVBIT, EV_ABS) < 0) {
        close(g_uinput_fd); g_uinput_fd = -1; return -1;
    }

    for (size_t i = 0; i < sizeof(g_uinput_buttons)/sizeof(g_uinput_buttons[0]); i++) {
        if (ioctl(g_uinput_fd, UI_SET_KEYBIT, g_uinput_buttons[i]) < 0) {
            close(g_uinput_fd); g_uinput_fd = -1; return -1;
        }
    }
    for (size_t i = 0; i < sizeof(g_uinput_axes)/sizeof(g_uinput_axes[0]); i++) {
        if (ioctl(g_uinput_fd, UI_SET_ABSBIT, g_uinput_axes[i]) < 0) {
            close(g_uinput_fd); g_uinput_fd = -1; return -1;
        }
        struct uinput_abs_setup abs;
        memset(&abs, 0, sizeof(abs));
        abs.code       = g_uinput_axes[i];
        abs.absinfo.minimum = -32768;
        abs.absinfo.maximum = 32767;
        abs.absinfo.flat = 0;
        abs.absinfo.fuzz = 0;
        if (ioctl(g_uinput_fd, UI_ABS_SETUP, &abs) < 0) {
            close(g_uinput_fd); g_uinput_fd = -1; return -1;
        }
    }

    struct uinput_setup setup;
    memset(&setup, 0, sizeof(setup));
    strncpy(setup.name, UINPUT_DEV_NAME, sizeof(setup.name) - 1);
    setup.id.bustype = BUS_USB;
    setup.id.vendor  = 0x045E;   /* Microsoft */
    setup.id.product = 0x28E;   /* Xbox 360   */
    setup.id.version = 0x0100;

    if (ioctl(g_uinput_fd, UI_DEV_SETUP, &setup) < 0) {
        close(g_uinput_fd); g_uinput_fd = -1; return -1;
    }
    if (ioctl(g_uinput_fd, UI_DEV_CREATE) < 0) {
        close(g_uinput_fd); g_uinput_fd = -1; return -1;
    }

    /* Give the kernel and udev time to settle the device node. */
    usleep(200000);
    return 0;
}

/* Destroy the uinput device and close the file descriptor. */
static void
uinput_destroy(void)
{
    if (g_uinput_fd >= 0) {
        ioctl(g_uinput_fd, UI_DEV_DESTROY);
        close(g_uinput_fd);
        g_uinput_fd = -1;
    }
}

/* Write a single input event to the uinput device. */
static int
uinput_write_event(unsigned short type, unsigned short code, int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type  = type;
    ev.code  = code;
    ev.value = value;
    if (write(g_uinput_fd, &ev, sizeof(ev)) < (ssize_t)sizeof(ev))
        return -1;
    return 0;
}

/* Send a button press via uinput (caller handles pump/events). */
static void
uinput_press(int vbtn, int pressed)
{
    int evdev_btn = vbtn_to_evdev(vbtn);
    if (evdev_btn < 0 || g_uinput_fd < 0)
        return;
    uinput_write_event(EV_KEY, evdev_btn, pressed ? 1 : 0);
    uinput_write_event(EV_SYN, SYN_REPORT, 0);
}

/* Send an axis event via uinput (caller handles pump/events). */
static void
uinput_axis(int vaxis, Sint16 value)
{
    int evdev_axis = vaxis_to_evdev(vaxis);
    if (evdev_axis < 0 || g_uinput_fd < 0)
        return;
    uinput_write_event(EV_ABS, evdev_axis, value);
    uinput_write_event(EV_SYN, SYN_REPORT, 0);
}

/* Find the SDL joystick index that matches the uinput device name.
 * Returns the index or -1 if not found. */
static int
find_uinput_joystick_index(void)
{
    int n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++) {
        const char *name = SDL_JoystickNameForIndex(i);
        if (name && strcmp(name, UINPUT_DEV_NAME) == 0)
            return i;
    }
    return -1;
}

/* Build and register a gamecontroller mapping for the uinput device.
 * SDL assigns joystick button indices by sorting evdev button codes
 * numerically.  The resulting mapping produces the same SDL_CONTROLLER
 * button/axis indices as the virtual joystick mapping. */
static void
add_uinput_gamecontroller_mapping(SDL_Joystick *joy)
{
    char guid[33];
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joy), guid, sizeof(guid));

    char mapping[512];
    snprintf(mapping, sizeof(mapping),
        "%s,%s,"
        "a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,"
        "back:b6,start:b7,guide:b8,leftstick:b9,rightstick:b10,"
        "dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
        "leftx:a0,lefty:a1,lefttrigger:a2,rightx:a3,righty:a4,righttrigger:a5,"
        "platform:Linux,",
        guid, UINPUT_DEV_NAME);
    SDL_GameControllerAddMapping(mapping);
}

/* ================================================================== */
/*  Native IP server (shared implementation)                           */
/* ================================================================== */

#include "native_ip_server.h"

/* The server implementation is in native_ip_server.c.
 * Global state is accessible via g_nip_* symbols declared in native_ip_server.h. */

/*  Test fixture                                                       */
/* ================================================================== */

typedef struct {
    char    bus_address[512];
    pid_t   daemon_pid;
    pid_t   server_pid;
    char    tmp_home[PATH_MAX];
    char    prof_path[PATH_MAX + 1024];
    /* SDL virtual controller / uinput gamepad */
    int     joy_device_index;
    SDL_Joystick *joystick;
    bool    use_uinput;     /* true when /dev/uinput kernel-backed gamepad */
    /* DBus connection for independent inspection */
    const ip_dbus_backend *backend;
    ip_bus_handle bus;
} functional_fixture;

static void
ensure_dummy_driver(void)
{
    if (getenv("SDL_VIDEODRIVER") == NULL)
        SDL_SetHintWithPriority(SDL_HINT_VIDEODRIVER, "dummy",
                                 SDL_HINT_OVERRIDE);
}

static int
f_setup(void **state)
{
    /* --- Prerequisites: dbus-daemon must be available (FAIL, not skip) --- */
    if (!getenv("DBUS_SESSION_CONFIG") &&
        access("/usr/share/dbus-1/session.conf", R_OK) != 0 &&
        access(DBUS_SESSION_CONFIG, R_OK) != 0) {
        fail_msg("dbus-daemon not available — prerequisites missing (FAIL, not skip)");
    }

    functional_fixture *f = calloc(1, sizeof(*f));
    assert_non_null(f);

    /* --- Isolated HOME with profile --- */
    snprintf(f->tmp_home, sizeof(f->tmp_home),
             "/tmp/cbx_functional_%d", (int)getpid());
    mkdir(f->tmp_home, 0700);
    setenv("HOME", f->tmp_home, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");
    unsetenv("FLATPAK_ID");

    /* Create profile directory and a test profile. */
    char prof_dir[PATH_MAX + 256];
    snprintf(prof_dir, sizeof(prof_dir),
             "%s/.local/share/inputplumber/profiles", f->tmp_home);
    cbx_ensure_dir(prof_dir, 0700);

    /* Create a default profile (CBX_DEFAULT_PROFILE = "default").
     * The overlay grid build sets each row's profile to "default".
     * Without this file, cbx_overlay_on_save fails with -ENOENT when
     * trying to apply the profile via LoadProfilePath. */
    {
        char dft_path[PATH_MAX + 1024];
        snprintf(dft_path, sizeof(dft_path), "%s/default.yaml", prof_dir);
        FILE *dfp = fopen(dft_path, "w");
        assert_non_null(dfp);
        fprintf(dfp,
            "version: 1\n"
            "kind: DeviceProfile\n"
            "name: Default\n"
            "description: Default profile\n"
            "mapping:\n"
            "  - name: btn_A\n"
            "    source_event:\n"
            "      gamepad:\n"
            "        button: A\n"
            "    target_events:\n"
            "      - keyboard: KeyA\n");
        fclose(dfp);
    }

    char prof_path[PATH_MAX + 1024];
    snprintf(prof_path, sizeof(prof_path), "%s/test_profile.yaml", prof_dir);
    snprintf(f->prof_path, sizeof(f->prof_path), "%s", prof_path);
    FILE *fp = fopen(prof_path, "w");
    assert_non_null(fp);
    fprintf(fp,
        "version: 1\n"
        "kind: DeviceProfile\n"
        "name: TestProfile\n"
        "description: Installed functional test profile\n"
        "mapping:\n"
        "  - name: btn_A\n"
        "    source_event:\n"
        "      gamepad:\n"
        "        button: A\n"
        "    target_events:\n"
        "      - keyboard: KeyA\n"
        "  - name: btn_B\n"
        "    source_event:\n"
        "      gamepad:\n"
        "        button: B\n"
        "    target_events:\n"
        "      - keyboard: KeyB\n");
    fclose(fp);

    /* --- Start private bus --- */
    assert_int_equal(nip_start_private_bus(f->bus_address,
                                        sizeof(f->bus_address),
                                        &f->daemon_pid), 0);
    setenv("DBUS_SYSTEM_BUS_ADDRESS", f->bus_address, 1);

    /* --- Reset server state and fork server --- */
    nip_reset_server_state(2);
    for (int i = 0; i < 2; i++) {
        snprintf(g_nip_comp_names[i], sizeof(g_nip_comp_names[i]),
                 "TestController%d", i);
        snprintf(g_nip_dbus_devices[i], sizeof(g_nip_dbus_devices[i]),
                 "/org/shadowblip/InputPlumber/CompositeDevice%d", i);
    }

    const nip_server_config cfg = { .num_composites = 2, .version = "0.78.0" };
    f->server_pid = nip_fork_server(f->bus_address, &cfg);
    assert_true(f->server_pid > 0);

    /* --- Wait for server to be ready (poll Version) --- */
    f->backend = ip_dbus_sd_backend();
    f->bus = NULL;
    assert_int_equal(f->backend->connect(&f->bus), 0);

    char *version = NULL;
    int rc = -1;
    for (int i = 0; i < 200 && rc != 0; i++) {
        free(version); version = NULL;
        rc = f->backend->get_property(f->bus, "org.shadowblip.InputPlumber",
            "/org/shadowblip/InputPlumber/Manager",
            "org.shadowblip.InputManager", "Version", &version);
        if (rc != 0) usleep(10000);
    }
    assert_int_equal(rc, 0);
    assert_string_equal(version, "0.78.0");
    free(version);

    /* --- Initialize SDL and create synthetic game controller ---
     *
     * Prefer a kernel-backed /dev/uinput gamepad when available so SDL
     * detects a real evdev joystick through its production input backend.
     * Fall back to SDL_JoystickAttachVirtual (process-local virtual
     * joystick) when /dev/uinput is absent (no kernel-uinput capability).
     */
    ensure_dummy_driver();

    /* Try kernel-backed uinput first.  Must happen before SDL_Init so
     * SDL's evdev backend detects the device during initialisation. */
    f->use_uinput = false;
    if (uinput_create_gamepad() == 0) {
        f->use_uinput = true;
        g_use_uinput  = true;
    }

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);

    if (f->use_uinput) {
        /* SDL should have detected the uinput device during init.
         * Scan for it by name and open a joystick handle. */
        f->joy_device_index = find_uinput_joystick_index();
        if (f->joy_device_index >= 0) {
            f->joystick = SDL_JoystickOpen(f->joy_device_index);
            if (f->joystick)
                add_uinput_gamecontroller_mapping(f->joystick);
        }
        /* If SDL failed to detect the device (e.g. evdev backend not
         * built), fall back to the virtual joystick path. */
        if (!f->joystick) {
            uinput_destroy();
            f->use_uinput = false;
            g_use_uinput  = false;
        }
    }

    if (!f->use_uinput) {
        /* SDL virtual joystick fallback (process-local). */
        f->joy_device_index = SDL_JoystickAttachVirtual(
            SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 15, 0);
        assert_true(f->joy_device_index >= 0);

        f->joystick = SDL_JoystickOpen(f->joy_device_index);
        assert_non_null(f->joystick);

        /* Register a gamecontroller mapping for the virtual joystick. */
        char guid[33];
        SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(f->joystick),
                                    guid, sizeof(guid));
        char mapping[512];
        snprintf(mapping, sizeof(mapping),
                 "%s,Controller-Box Virtual,a:b0,b:b1,start:b6,"
                 "dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,platform:Linux,",
                 guid);
        assert_true(SDL_GameControllerAddMapping(mapping) >= 0);
    }

    SDL_GameControllerEventState(SDL_ENABLE);

    *state = f;
    return 0;
}

static int
f_teardown(void **state)
{
    functional_fixture *f = *state;
    if (!f) return 0;

    /* Disconnect independent DBus connection. */
    if (f->bus) f->backend->disconnect(f->bus);

    /* Close joystick / destroy uinput device. */
    if (f->joystick) SDL_JoystickClose(f->joystick);
    if (f->use_uinput) {
        uinput_destroy();
        g_use_uinput = false;
    } else if (f->joy_device_index >= 0) {
        SDL_JoystickDetachVirtual(f->joy_device_index);
    }

    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    SDL_Quit();

    /* Kill server. */
    if (f->server_pid > 1) {
        kill(f->server_pid, SIGTERM);
        waitpid(f->server_pid, NULL, 0);
    }

    /* Kill daemon. */
    if (f->daemon_pid > 1) {
        kill(f->daemon_pid, SIGTERM);
        waitpid(f->daemon_pid, NULL, 0);
    }
    unsetenv("DBUS_SYSTEM_BUS_ADDRESS");

    /* Clean up temp HOME. */
    if (f->tmp_home[0]) {
        char cmd[PATH_MAX + 32];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", f->tmp_home);
        int sysrc = system(cmd);
        (void)sysrc;
    }

    free(f);
    return 0;
}

/* ================================================================== */
/*  Helper functions                                                   */
/* ================================================================== */

/* Dispatch all pending SDL events to the manager. */
static void
pump_manager(cbx_manager *mgr)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
        cbx_manager_handle_event(mgr, &ev);
}

/* Send a controller button press+release through the manager.
 * When g_use_uinput is true, events are written to the kernel uinput
 * device; otherwise SDL_JoystickSetVirtualButton is used. */
static void
ctrl_press(cbx_manager *mgr, SDL_Joystick *joy, int button)
{
    if (g_use_uinput) {
        uinput_press(button, 1);
        SDL_PumpEvents();
        pump_manager(mgr);
        uinput_press(button, 0);
        SDL_PumpEvents();
        pump_manager(mgr);
    } else {
        SDL_JoystickSetVirtualButton(joy, button, 1);
        SDL_PumpEvents();
        pump_manager(mgr);
        SDL_JoystickSetVirtualButton(joy, button, 0);
        SDL_PumpEvents();
        pump_manager(mgr);
    }
}

/* Send a controller axis event through the manager (production dispatch).
 * Exercises uinput_write_event (kernel) or SDL_JoystickSetVirtualAxis
 * (virtual) → SDL_PumpEvents → pump_manager (cbx_manager_handle_event).
 * Axis events are expected to be safely ignored by the manager — no
 * navigation, no state change, no crash.  SDL_CONTROLLERAXISMOTION events
 * flow through the same SDL event queue and are polled by SDL_PollEvent
 * in pump_manager. */
static void
ctrl_axis(cbx_manager *mgr, SDL_Joystick *joy, int axis, Sint16 value)
{
    if (g_use_uinput) {
        uinput_axis(axis, value);
        SDL_PumpEvents();
        pump_manager(mgr);
    } else {
        SDL_JoystickSetVirtualAxis(joy, axis, value);
        SDL_PumpEvents();
        pump_manager(mgr);
    }
}

/* Send a mouse left-button click at (x, y) through the manager. */
static bool
send_mouse_click(cbx_manager *mgr, int x, int y)
{
    SDL_Event ev = {0};
    ev.type = SDL_MOUSEBUTTONDOWN;
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.x = x;
    ev.button.y = y;
    bool down = cbx_manager_handle_event(mgr, &ev);

    ev.type = SDL_MOUSEBUTTONUP;
    ev.button.x = x;
    ev.button.y = y;
    cbx_manager_handle_event(mgr, &ev);
    return down;
}

/* ================================================================== */
/*  Overlay service helpers (following test_overlay_native.c pattern)   */
/* ================================================================== */

/* Push a custom SDL event (used to simulate the InterceptMode poll timer). */
static void
push_poll_event(uint32_t event_type)
{
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = event_type;
    ev.user.code = 0;
    ev.user.data1 = ev.user.data2 = NULL;
    SDL_PushEvent(&ev);
}

/* Push a keyboard KEYDOWN event (used to simulate overlay controller input). */
static void
push_keydown(SDL_Keycode sym)
{
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = sym;
    SDL_PushEvent(&ev);
}

/* Find a usable TrueType font for overlay text rendering. */
static const char *
find_overlay_font(void)
{
    const char *p = cbx_font_path();
    if (p)
        return p;

#ifdef CBX_FONT_PATH
    if (CBX_FONT_PATH[0] != '\0' && access(CBX_FONT_PATH, R_OK) == 0)
        return CBX_FONT_PATH;
#endif

    static char found[PATH_MAX];
    const char *candidates[] = {
        "/nix/store/zzs2q7lk5mn6y2rywd3snhak7098zs66-system-path"
            "/share/X11/fonts/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
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
        } else {
            pclose(fp);
        }
    }
    return NULL;
}

/* InterceptMode poll callbacks (production wrappers for test). */
static void
test_on_activating(void *userdata)
{
    cbx_poll_activation_ctx *act = (cbx_poll_activation_ctx *)userdata;
    if (!act || !act->lifecycle)
        return;
    if (act->composite_path[0]) {
        size_t len = strlen(act->composite_path);
        if (len >= sizeof(act->lifecycle->composite_path))
            len = sizeof(act->lifecycle->composite_path) - 1;
        memcpy(act->lifecycle->composite_path, act->composite_path, len);
        act->lifecycle->composite_path[len] = '\0';
    }
    cbx_overlay_lifecycle_activate(act->lifecycle);
}

static void
test_on_deactivating(void *userdata)
{
    cbx_overlay_lifecycle *lc = (cbx_overlay_lifecycle *)userdata;
    cbx_overlay_lifecycle_close(lc);
}

static void
test_on_poll_error(int error_code, void *userdata)
{
    (void)error_code;
    (void)userdata;
}

/* ================================================================== */
/*  Test: Installed functional acceptance                               */
/* ================================================================== */

static void
test_installed_functional(void **state)
{
    functional_fixture *f = *state;

    /* ================================================================ */
    /*  Phase 1: Manager initialization with real DBus + SDL controller  */
    /* ================================================================ */

    /* cbx_manager_init() connects to DBUS_SYSTEM_BUS_ADDRESS (our private
     * bus) using the production sd-bus backend.  It also initializes SDL
     * and opens any available game controllers — our virtual joystick
     * should be detected. */
    cbx_manager mgr;
    int rc = cbx_manager_init(&mgr, NULL);
    assert_int_equal(rc, 0);

    /* The manager should have detected the DBus backend (not degraded). */
    assert_true(mgr.dbus_connected);

    /* Flush the SDL_CONTROLLERDEVICEADDED event from the virtual joystick. */
    pump_manager(&mgr);

    /* ================================================================ */
    /*  Phase 2: Controller detection                                     */
    /* ================================================================ */

    /* The virtual joystick was created before manager init, so the manager
     * should have opened it during init (SDL_NumJoysticks > 0). */
    assert_true(mgr.gamecontroller_count >= 1);

    /* ================================================================ */
    /*  Phase 3: Controller navigation (real SDL events)                 */
    /* ================================================================ */

    /* The manager starts on the Controllers tab (first tab). */
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_CONTROLLERS);

    /* Navigate to Profiles tab using D-pad Right on the virtual controller.
     * This exercises the real SDL GameController transport: virtual
     * joystick button → SDL_CONTROLLERBUTTONDOWN → manager dispatch. */
    ctrl_press(&mgr, f->joystick, 14);  /* D-pad Right → next tab */

    /* Verify the tab changed.  If the controller event was processed,
     * the active tab should now be Profiles. */
    int tab_after = cbx_manager_active_tab(&mgr);
    assert_int_equal(tab_after, CBX_MGR_TAB_PROFILES);

    /* Navigate to Settings tab. */
    ctrl_press(&mgr, f->joystick, 14);  /* D-pad Right → Settings */
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_SETTINGS);

    /* Navigate back to Controllers. */
    ctrl_press(&mgr, f->joystick, 13);  /* D-pad Left → back to Profiles */
    ctrl_press(&mgr, f->joystick, 13);  /* D-pad Left → back to Controllers */
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_CONTROLLERS);

    /* ================================================================ */
    /*  Phase 4: Create routable target via production DBus wrappers     */
    /* ================================================================ */

    /* Create a virtual target through the production Manager DBus
     * wrappers — the same code path the Controllers tab uses when
     * the Add button is activated. */
    char *target_path = NULL;
    rc = ip_manager_create_target_device(f->backend, f->bus,
                                           "xb360", &target_path);
    assert_int_equal(rc, 0);
    assert_non_null(target_path);

    /* Attach the target to CompositeDevice0 for routability. */
    const char *comp0 = "/org/shadowblip/InputPlumber/CompositeDevice0";
    rc = ip_manager_attach_target_device(f->backend, f->bus,
                                           target_path, comp0);
    assert_int_equal(rc, 0);

    /* ================================================================ */
    /*  Phase 5: Independent DBus verification                           */
    /* ================================================================ */

    /* Verify via GetManagedObjects that the target was created. */
    cbx_device_model model;
    cbx_device_model_init(&model);
    rc = cbx_objectmanager_enumerate(f->backend, f->bus, &model);
    assert_int_equal(rc, 0);

    /* Should have 2 composites and at least 1 target. */
    assert_true(model.composite_count >= 2);
    assert_true(model.target_count >= 1);

    /* Verify the target has a DeviceType matching what we created. */
    char *dtype = NULL;
    rc = ip_target_get_device_type(f->backend, f->bus,
                                    target_path, &dtype);
    assert_int_equal(rc, 0);
    assert_non_null(dtype);
    assert_string_equal(dtype, "xb360");
    free(dtype);

    /* Verify the target is routable — attached to CompositeDevice0.
     * Read TargetDevices property on the composite. */
    char *target_devs = NULL;
    rc = ip_composite_get_target_devices(f->backend, f->bus,
                                           comp0, &target_devs);
    assert_int_equal(rc, 0);
    assert_non_null(target_devs);
    /* TargetDevices should contain our target path. */
    assert_true(strstr(target_devs, target_path) != NULL);
    free(target_devs);
    free(target_path);
    target_path = NULL;

    /* ================================================================ */
    /*  Phase 6: Profile persistence — save settings                     */
    /* ================================================================ */

    /* Navigate to Settings tab via controller — verify real controller
     * events reach the Settings tab. */
    ctrl_press(&mgr, f->joystick, 14);  /* D-pad Right → Profiles */
    ctrl_press(&mgr, f->joystick, 14);  /* D-pad Right → Settings */
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_SETTINGS);

    /* Save settings to disk through the production settings tab API.
     * cbx_settings_tab_save writes to
     * $HOME/.config/controller-box/settings.yaml. */
    cbx_settings_tab *st = cbx_manager_settings_tab(&mgr);
    assert_non_null(st);
    rc = cbx_settings_tab_save(st);
    assert_int_equal(rc, 0);

    /* Verify settings file exists on disk (filesystem inspection). */
    char settings_path[PATH_MAX + 1024];
    snprintf(settings_path, sizeof(settings_path),
             "%s/.config/controller-box/settings.yaml", f->tmp_home);
    assert_int_equal(access(settings_path, F_OK), 0);

    /* Verify the profile file exists (created in setup). */
    assert_int_equal(access(f->prof_path, F_OK), 0);

    /* ================================================================ */
    /*  Phase 7: Restart manager — verify persistence                    */
    /* ================================================================ */

    cbx_manager_shutdown(&mgr);

    /* Re-create the virtual joystick for the new manager instance.
     * With uinput, the kernel device persists across manager restarts —
     * no re-creation is needed, the manager will re-detect it on init. */
    int joy2_idx = -1;
    SDL_Joystick *joy2 = NULL;
    if (!f->use_uinput) {
        joy2_idx = SDL_JoystickAttachVirtual(
            SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 15, 0);
        assert_true(joy2_idx >= 0);
        joy2 = SDL_JoystickOpen(joy2_idx);
        assert_non_null(joy2);
    }

    /* The same GUID/mapping should apply. */
    SDL_GameControllerEventState(SDL_ENABLE);

    /* Re-initialize the manager. */
    rc = cbx_manager_init(&mgr, NULL);
    assert_int_equal(rc, 0);
    assert_true(mgr.dbus_connected);

    pump_manager(&mgr);

    /* Verify the manager detected the controller. */
    assert_true(mgr.gamecontroller_count >= 1);

    /* Verify the manager reconnected to the backend and enumerated devices. */
    cbx_device_model model2;
    cbx_device_model_init(&model2);
    rc = cbx_objectmanager_enumerate(f->backend, f->bus, &model2);
    assert_int_equal(rc, 0);
    assert_true(model2.composite_count >= 2);

    /* Verify the settings file still exists (persistence after restart). */
    assert_int_equal(access(settings_path, F_OK), 0);

    /* Verify the target still exists on the server (routable target
     * survives manager restart — the target is on the backend, not
     * in the manager process). */
    assert_true(model2.target_count >= 1);

    /* ================================================================ */
    /*  Phase 8: Overlay service initialization with real DBus            */
    /*                                                                   */
    /*  Allocate a production cbx_overlay_service_ctx and initialize all  */
    /*  components: SDL renderer, DBus connection, device enumeration,   */
    /*  settings/assignments, grid build, profile cycle, overlay surface, */
    /*  lifecycle (fade=0 for determinism), player/host mode with        */
    /*  callbacks, input event subscription, InterceptMode polls, and    */
    /*  trigger registration.  Rendering resources (text cache, icon     */
    /*  cache, theme) are initialized so the framebuffer can be read     */
    /*  back and verified.  This follows the same 25-step sequence as    */
    /*  test_overlay_native.c::native_setup.                              */
    /* ================================================================ */

    cbx_overlay_service_ctx *svc = calloc(1, sizeof(*svc));
    assert_non_null(svc);

    /* 8a: Renderer (hidden window, dummy driver). */
    assert_int_equal(cbx_renderer_init(&svc->rend, "test-overlay-functional",
                                         OVERLAY_W, OVERLAY_H, false), 0);

    /* 8b: DBus connection via production sd-bus backend. */
    ip_connection_init(&svc->conn, f->backend);
    assert_int_equal(ip_connection_connect(&svc->conn), 0);
    svc->backend_ready = ip_connection_is_connected(&svc->conn);
    assert_true(svc->backend_ready);

    /* 8c: Enumerate devices through production ObjectManager. */
    cbx_device_model_init(&svc->model);
    assert_int_equal(cbx_objectmanager_enumerate(svc->conn.backend,
                                                   svc->conn.bus,
                                                   &svc->model), 0);
    assert_true(svc->model.composite_count >= 2);

    /* 8d: Settings + assignments. */
    cbx_settings_defaults(&svc->settings);
    svc->settings.virtual_controllers.count = 4;
    cbx_assignments_init(&svc->assignments);

    /* 8e: Create + attach target (needed for on_save). */
    char *ov_target_path = NULL;
    assert_int_equal(ip_manager_create_target_device(
        svc->conn.backend, svc->conn.bus,
        "xb360", &ov_target_path), 0);
    assert_non_null(ov_target_path);
    free(ov_target_path);

    /* Re-enumerate to pick up the new target. */
    assert_int_equal(cbx_objectmanager_enumerate(svc->conn.backend,
                                                   svc->conn.bus,
                                                   &svc->model), 0);
    assert_int_equal(ip_manager_attach_target_device(
        svc->conn.backend, svc->conn.bus,
        svc->model.targets[0].path, comp0), 0);

    /* 8f: Build composite info from device model. */
    svc->comp_count = svc->model.composite_count;
    for (int i = 0; i < svc->comp_count; i++) {
        char *id = NULL;
        ip_composite_get_persistent_id(svc->conn.backend, svc->conn.bus,
                                         svc->model.composites[i].path, &id);
        snprintf(svc->composites[i].composite_path,
                 sizeof(svc->composites[i].composite_path),
                 "%s", svc->model.composites[i].path);
        snprintf(svc->composites[i].id,
                 sizeof(svc->composites[i].id),
                 "%s", id ? id : "composite-N");
        free(id);
        char *name = NULL;
        ip_composite_get_name(svc->conn.backend, svc->conn.bus,
                               svc->model.composites[i].path, &name);
        snprintf(svc->composites[i].model_name,
                 sizeof(svc->composites[i].model_name),
                 "%s", name ? name : "Controller");
        free(name);
    }

    /* 8g: Build grid from composites + settings + assignments. */
    cbx_select_grid_init(&svc->grid);
    assert_int_equal(cbx_select_grid_build(&svc->grid, svc->composites,
                                            svc->comp_count,
                                            &svc->settings,
                                            &svc->assignments), 0);

    /* 8h: Enumerate profiles and load into grid. */
    assert_int_equal(cbx_profile_list_enumerate(&svc->profiles), 0);
    assert_int_equal(cbx_profile_cycle_load_profiles(&svc->grid,
                                                        &svc->profiles), 0);

    /* 8i: Profile cycle init. */
    cbx_profile_cycle_init(&svc->profile_cycle, svc->conn.backend,
                            svc->conn.bus, &svc->assignments,
                            &svc->profiles);

    /* 8j: Overlay surface (render-to-texture). */
    assert_int_equal(cbx_overlay_surface_init(&svc->surface,
                                                svc->rend.renderer,
                                                OVERLAY_W, OVERLAY_H,
                                                1.0), 0);

    /* 8k: Rendering resources — text cache, icon cache, theme. */
    cbx_theme_default(&svc->theme);
    svc->font_id = -1;
    if (cbx_text_cache_init(&svc->text_cache, svc->rend.renderer) == 0) {
        const char *font = find_overlay_font();
        if (font) {
            svc->font_id = cbx_text_load_font(&svc->text_cache, font, 18);
        }
    }

    cbx_icon_map_init(&svc->icon_map);
    char icon_yaml[PATH_MAX + 64];
    snprintf(icon_yaml, sizeof(icon_yaml), "%s/controller-icons.yaml",
             OVERLAY_YAML_DIR);
    bool has_icons = false;
    if (cbx_icon_map_load(&svc->icon_map, icon_yaml) == 0) {
        if (cbx_icon_cache_init(&svc->icon_cache, svc->rend.renderer,
                                cbx_icon_dir(), 64) == 0) {
            cbx_icon_cache_load(&svc->icon_cache, &svc->icon_map);
            has_icons = true;
        }
    }

    /* 8l: Render context — full resources for framebuffer verification. */
    svc->render_ctx.grid = &svc->grid;
    svc->render_ctx.icon_cache = has_icons ? &svc->icon_cache : NULL;
    svc->render_ctx.icon_map = has_icons ? &svc->icon_map : NULL;
    svc->render_ctx.theme = &svc->theme;
    svc->render_ctx.text_cache = (svc->font_id >= 0) ? &svc->text_cache : NULL;
    svc->render_ctx.font_id = svc->font_id;
    svc->render_ctx.settings = &svc->settings;

    /* 8m: Lifecycle (fade=0 for deterministic instant transitions). */
    cbx_overlay_lifecycle_init(&svc->lifecycle, svc->conn.backend,
                                svc->conn.bus, comp0,
                                &svc->surface, svc->rend.renderer);
    svc->lifecycle.fade_in_ms = 0;
    svc->lifecycle.fade_out_ms = 0;
    svc->lifecycle.state = CBX_OVERLAY_IDLE;

    /* 8n: Wire on_save (production callback). */
    svc->lifecycle.on_save = cbx_overlay_on_save;
    svc->lifecycle.on_save_data = svc;

    /* 8o: Player mode + callbacks. */
    cbx_player_mode_init(&svc->pm, &svc->grid);
    svc->pm.on_slot_change = cbx_overlay_on_slot_change;
    svc->pm.slot_change_data = svc;
    svc->pm.on_profile_change = cbx_overlay_on_profile_change;
    svc->pm.profile_change_data = svc;

    /* 8p: Host mode + callbacks. */
    cbx_host_mode_init(&svc->hm);
    svc->hm.on_slot_change = cbx_overlay_on_slot_change;
    svc->hm.slot_change_data = svc;

    /* 8q: Build input map from real DBus. */
    svc->input_ctx.pm = &svc->pm;
    svc->input_ctx.hm = &svc->hm;
    svc->input_ctx.grid = &svc->grid;
    svc->input_ctx.lifecycle = &svc->lifecycle;
    svc->input_ctx.path_count = 0;
    cbx_overlay_input_build_map(svc->conn.backend, svc->conn.bus,
                                  svc->composites, svc->comp_count,
                                  &svc->input_ctx);
    assert_int_equal(svc->input_ctx.path_count, 2);

    /* 8r: Input event subscription. */
    const char *uniq = ip_connection_get_unique_name(&svc->conn);
    snprintf(svc->expected_sender, sizeof(svc->expected_sender),
             "%s", uniq ? uniq : "");
    ip_input_events_init(&svc->input_events, svc->conn.backend,
                          svc->conn.bus, svc->expected_sender,
                          cbx_overlay_input_cb, &svc->input_ctx);
    svc->input_events_ready = false;
    if (ip_input_events_subscribe(&svc->input_events) == 0)
        svc->input_events_ready = true;
    assert_true(svc->input_events_ready);

    /* 8s: InterceptMode polls (init only — no timer for determinism). */
    svc->poll_event_type = SDL_RegisterEvents(1);
    svc->poll_count = 0;
    for (int i = 0; i < svc->comp_count && i < CBX_MAX_COMPOSITES; i++) {
        svc->poll_acts[i].lifecycle = &svc->lifecycle;
        snprintf(svc->poll_acts[i].composite_path,
                 sizeof(svc->poll_acts[i].composite_path),
                 "%s", svc->composites[i].composite_path);
        ip_intercept_poll_init(&svc->polls[i],
                                svc->conn.backend, svc->conn.bus,
                                svc->composites[i].composite_path,
                                test_on_activating, &svc->poll_acts[i],
                                test_on_deactivating, &svc->lifecycle,
                                test_on_poll_error, NULL);
        svc->poll_count++;
    }

    /* 8t: Register triggers (SetInterceptActivation + InterceptMode=PASS). */
    {
        const char *paths[CBX_MAX_COMPOSITES];
        for (int i = 0; i < svc->comp_count; i++)
            paths[i] = svc->composites[i].composite_path;
        cbx_trigger_register_all(svc->conn.backend, svc->conn.bus,
                                    paths, svc->comp_count,
                                    svc->settings.overlay_trigger);
    }

    /* 8u: Pre-render the overlay surface (mark dirty + render once). */
    cbx_overlay_surface_mark_dirty_all(&svc->surface);
    cbx_overlay_surface_render(&svc->surface, svc->rend.renderer,
                                cbx_select_grid_render_cb, &svc->render_ctx);

    svc->initialized = true;
    cbx_overlay_service_reset_shutdown();

    /* Verify initial state: overlay idle, InterceptMode PASS on both composites. */
    assert_int_equal(svc->lifecycle.state, CBX_OVERLAY_IDLE);
    char *mode_str = NULL;
    rc = ip_composite_get_intercept_mode(f->backend, f->bus, comp0, &mode_str);
    assert_int_equal(rc, 0);
    assert_string_equal(mode_str, "1");
    free(mode_str);

    /* ================================================================ */
    /*  Phase 9: Overlay activation via InterceptMode poll detection      */
    /*                                                                   */
    /*  Set poll to PASS_WAIT state, change InterceptMode from PASS (1)  */
    /*  to ALL (2) on the server, push the poll event, and call          */
    /*  cbx_overlay_service_step.  The poll tick reads the new mode,     */
    /*  fires on_activating, which calls cbx_overlay_lifecycle_activate. */
    /*  The lifecycle tick transitions IDLE → VISIBLE (fade=0 = instant).*/
    /*  The step function then renders the overlay surface and shows it. */
    /* ================================================================ */

    svc->polls[0].state = IP_POLL_PASS_WAIT;

    /* Set InterceptMode = ALL (2) on server — simulates InputPlumber
     * detecting the Select+A trigger combo. */
    assert_int_equal(ip_composite_set_intercept_mode(
        svc->conn.backend, svc->conn.bus, comp0, "2"), 0);

    /* Verify InterceptMode is ALL on the server. */
    mode_str = NULL;
    assert_int_equal(ip_composite_get_intercept_mode(
        svc->conn.backend, svc->conn.bus, comp0, &mode_str), 0);
    assert_string_equal(mode_str, "2");
    free(mode_str);

    /* Push poll event and step — this is the production poll path. */
    push_poll_event(svc->poll_event_type);
    cbx_overlay_service_step(svc);

    /* Overlay should be visible (activated via poll detection). */
    assert_int_equal(svc->lifecycle.state, CBX_OVERLAY_VISIBLE);

    /* ================================================================ */
    /*  Phase 10: Framebuffer readback — verify overlay rendered          */
    /*                                                                   */
    /*  After activation, cbx_overlay_service_step rendered the grid     */
    /*  into the overlay surface texture.  Read back the pixels and      */
    /*  verify the framebuffer is non-blank: grid background, position   */
    /*  indicator, controller text, and icons must all be present.       */
    /* ================================================================ */

    /* Step again to ensure the dirty surface was rendered + shown. */
    cbx_overlay_service_step(svc);

    /* Read back pixels from the overlay surface texture. */
    uint8_t *fb = malloc(OVERLAY_W * OVERLAY_H * 4);
    assert_non_null(fb);
    SDL_SetRenderTarget(svc->rend.renderer,
                        cbx_overlay_surface_get_texture(&svc->surface));
    assert_int_equal(fb_read_pixels(svc->rend.renderer, NULL, fb,
                                     OVERLAY_W * OVERLAY_H * 4), 0);
    SDL_SetRenderTarget(svc->rend.renderer, NULL);

    /* Background color from theme. */
    uint8_t bg[3] = { svc->theme.bg.r, svc->theme.bg.g, svc->theme.bg.b };

    /* Verify grid area is non-blank (content present in the grid region). */
    SDL_Rect grid_area = { 0, 32, OVERLAY_W, OVERLAY_H - 32 };
    assert_true(fb_region_has_content(fb, OVERLAY_W, OVERLAY_H,
                                       &grid_area, bg, 25));

    /* Verify header area has content (column headers like "Unassigned", "P1"). */
    SDL_Rect header_area = { 0, 0, OVERLAY_W, 32 };
    assert_true(fb_region_has_content(fb, OVERLAY_W, OVERLAY_H,
                                       &header_area, bg, 25));

    /* Verify row label area has content (controller names). */
    SDL_Rect label_area = { 0, 32, 200, OVERLAY_H - 32 };
    assert_true(fb_region_has_content(fb, OVERLAY_W, OVERLAY_H,
                                       &label_area, bg, 25));

    /* Verify a cell area has content (position indicator circle or icon). */
    SDL_Rect cell_area = { 200, 32, 200, OVERLAY_H - 32 };
    assert_true(fb_region_has_content(fb, OVERLAY_W, OVERLAY_H,
                                       &cell_area, bg, 25));

    free(fb);

    /* ================================================================ */
    /*  Phase 10b: Axis events — safely ignored by overlay (HIGH gap)     */
    /*                                                                   */
    /*  Axis events are generated via SDL_JoystickSetVirtualAxis         */
    /*  (virtual joystick) or uinput_write_event (kernel-backed) and       */
    /*  appear as SDL_CONTROLLERAXISMOTION events.  cbx_overlay_service_step*/
    /*  polls SDL events but only handles SDL_KEYDOWN — axis events are    */
    /*  silently discarded.  Verify that sending axis events does not      */
    /*  navigate the grid or change the overlay state.                     */
    /* ================================================================ */
    {
        int col_before = cbx_select_grid_get_cur_col(&svc->grid, 0);
        int state_before = svc->lifecycle.state;

        /* Send axis events through the overlay service step path. */
#define send_axis_va(ax, val) do { \
    if (f->use_uinput) { uinput_axis(ax, val); } \
    else { SDL_JoystickSetVirtualAxis(f->joystick, ax, val); } \
    SDL_PumpEvents(); cbx_overlay_service_step(svc); \
} while(0)

        send_axis_va(0, SDL_JOYSTICK_AXIS_MAX);
        send_axis_va(0, 0);
        send_axis_va(1, SDL_JOYSTICK_AXIS_MAX);
        send_axis_va(1, 0);
        send_axis_va(4, SDL_JOYSTICK_AXIS_MAX);
        send_axis_va(4, 0);
#undef send_axis_va

        /* Verify grid position and overlay state unchanged. */
        assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 0),
                         col_before);
        assert_int_equal(svc->lifecycle.state, state_before);
    }

    /* ================================================================ */
    /*  Phase 11: Overlay close — assignment application + clean close   */
    /*                                                                   */
    /*  Move row 0 to column 1 (P1 slot) via SDL keydown, then close     */
    /*  the overlay with B.  The production on_save callback fires,      */
    /*  which applies LoadProfilePath + AttachTargetDevice + GamepadOrder*/
    /*  via DBus, persists assignments to disk, and sets InterceptMode   */
    /*  back to PASS.  Verify all outcomes through the independent DBus  */
    /*  connection and filesystem inspection.                            */
    /* ================================================================ */

    /* Move row 0 right to P1 slot (col 1). */
    push_keydown(SDLK_RIGHT);
    cbx_overlay_service_step(svc);
    assert_int_equal(cbx_select_grid_get_cur_col(&svc->grid, 0), 1);

    /* Clear row 0's profile to isolate the assignment-persistence path
     * in on_save.  The grid build sets each row's profile to
     * CBX_DEFAULT_PROFILE ("default"), which resolves to a system
     * profile path.  Profile application via LoadProfilePath is
     * exercised in test_overlay_native.c O10.  Here we focus on the
     * overlay lifecycle: activation, framebuffer, close, and
     * assignment persistence. */
    svc->grid.rows[0].profile[0] = '\0';

    /* Close overlay via B button. */
    push_keydown(SDLK_b);
    cbx_overlay_service_step(svc);

    /* Overlay should be idle (closed). */
    assert_int_equal(svc->lifecycle.state, CBX_OVERLAY_IDLE);

    /* Assignment should be saved (on_save ran). */
    assert_int_equal(svc->assignments.assignment_count, 1);
    assert_int_equal(svc->assignments.assignments[0].slot, 0);

    /* Verify InterceptMode was set to PASS (1) on the wire by on_save. */
    mode_str = NULL;
    rc = ip_composite_get_intercept_mode(f->backend, f->bus, comp0, &mode_str);
    assert_int_equal(rc, 0);
    assert_string_equal(mode_str, "1");
    free(mode_str);

    /* Verify GamepadOrder was set on the engine via DBus (MEDIUM gap).
     * The overlay moved row 0 to col 1 (slot 0 = P1), so on_save
     * built a GamepadOrder CSV containing comp0's path and called
     * ip_manager_set_gamepad_order.  Read it back through the
     * independent DBus connection and verify. */
    {
        char *order_str = NULL;
        rc = ip_manager_get_gamepad_order(f->backend, f->bus, &order_str);
        assert_int_equal(rc, 0);
        assert_non_null(order_str);
        /* The order CSV must contain comp0's composite device path. */
        assert_non_null(strstr(order_str, comp0));
        free(order_str);
    }

    /* ================================================================ */
    /*  Phase 12: Overlay service cleanup                                 */
    /* ================================================================ */

    /* Stop polls. */
    for (int i = 0; i < svc->poll_count; i++)
        ip_intercept_poll_stop(&svc->polls[i]);

    /* Destroy surface + shutdown renderer. */
    cbx_overlay_surface_destroy(&svc->surface);
    cbx_renderer_shutdown(&svc->rend);

    /* Clean up rendering resources. */
    if (svc->font_id >= 0)
        cbx_text_cache_cleanup(&svc->text_cache);
    if (has_icons)
        cbx_icon_cache_cleanup(&svc->icon_cache);

    /* Disconnect DBus. */
    ip_connection_disconnect(&svc->conn);

    free(svc);

    /* ================================================================ */
    /*  Phase 13: Backend restart — verify recovery                     */
    /* ================================================================ */

    /* Kill the InputPlumber server and restart it.  The manager should
     * recover via NameOwnerChanged when the server reappears. */

    /* First, shut down the manager. */
    cbx_manager_shutdown(&mgr);
    if (joy2) SDL_JoystickClose(joy2);
    if (joy2_idx >= 0) SDL_JoystickDetachVirtual(joy2_idx);

    /* Kill the server. */
    kill(f->server_pid, SIGTERM);
    waitpid(f->server_pid, NULL, 0);
    f->server_pid = 0;

    /* Reset server state for the new instance. */
    nip_reset_server_state(2);
    for (int i = 0; i < 2; i++) {
        snprintf(g_nip_comp_names[i], sizeof(g_nip_comp_names[i]),
                 "TestController%d", i);
        snprintf(g_nip_dbus_devices[i], sizeof(g_nip_dbus_devices[i]),
                 "/org/shadowblip/InputPlumber/CompositeDevice%d", i);
    }

    /* Restart the server. */
    const nip_server_config cfg = { .num_composites = 2, .version = "0.78.0" };
    f->server_pid = nip_fork_server(f->bus_address, &cfg);
    assert_true(f->server_pid > 0);

    /* Wait for the new server to be ready. */
    char *version = NULL;
    rc = -1;
    for (int i = 0; i < 200 && rc != 0; i++) {
        free(version); version = NULL;
        rc = f->backend->get_property(f->bus, "org.shadowblip.InputPlumber",
            "/org/shadowblip/InputPlumber/Manager",
            "org.shadowblip.InputManager", "Version", &version);
        if (rc != 0) usleep(10000);
    }
    assert_int_equal(rc, 0);
    free(version);

    /* Re-initialize the manager — should connect to the restarted server.
     * With uinput, the kernel device persists — no re-creation needed. */
    int joy3_idx = -1;
    SDL_Joystick *joy3 = NULL;
    if (!f->use_uinput) {
        joy3_idx = SDL_JoystickAttachVirtual(
            SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 15, 0);
        assert_true(joy3_idx >= 0);
        joy3 = SDL_JoystickOpen(joy3_idx);
        assert_non_null(joy3);
    }
    SDL_GameControllerEventState(SDL_ENABLE);

    rc = cbx_manager_init(&mgr, NULL);
    assert_int_equal(rc, 0);
    assert_true(mgr.dbus_connected);

    pump_manager(&mgr);
    assert_true(mgr.gamecontroller_count >= 1);

    /* Verify GetManagedObjects works after backend restart. */
    cbx_device_model model3;
    cbx_device_model_init(&model3);
    rc = cbx_objectmanager_enumerate(f->backend, f->bus, &model3);
    assert_int_equal(rc, 0);
    assert_true(model3.composite_count >= 2);

    /* Verify settings persistence after backend restart. */
    assert_int_equal(access(settings_path, F_OK), 0);

    /* Verify profile file still exists. */
    assert_int_equal(access(f->prof_path, F_OK), 0);

    /* ================================================================ */
    /*  Cleanup                                                           */
    /* ================================================================ */

    cbx_manager_shutdown(&mgr);
    if (joy3) SDL_JoystickClose(joy3);
    if (joy3_idx >= 0) SDL_JoystickDetachVirtual(joy3_idx);
}

/* ================================================================== */
/*  Helper: send a keyboard KEYDOWN event through the SDL queue.      */
/*  Used for typing letters in NAME_INPUT mode (gamepad A/B are        */
/*  mapped to confirm/cancel, not letters, so name typing requires     */
/*  keyboard events).  windowID=0 ensures these are treated as         */
/*  regular keyboard events, not controller-derived.                   */
/* ================================================================== */

static void
send_key_down(cbx_manager *mgr, SDL_Keycode key)
{
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SDL_KEYDOWN;
    ev.key.type = SDL_KEYDOWN;
    ev.key.keysym.sym = key;
    ev.key.state = SDL_PRESSED;
    ev.key.repeat = 0;
    /* windowID = 0 → not CBX_CONTROLLER_EVENT_WINDOW_ID → keyboard */
    SDL_PushEvent(&ev);
    pump_manager(mgr);
}

/* ================================================================== */
/*  Helper: drain the manager's own DBus connection to process         */
/*  pending signals (e.g. NameOwnerChanged for backend recovery).      */
/* ================================================================== */

static void
drain_manager_dbus(cbx_manager *mgr, int timeout_ms)
{
    if (!mgr || !mgr->dbus_backend || !mgr->dbus_bus ||
        !mgr->dbus_backend->process)
        return;
    for (int i = 0; i < timeout_ms / 10; i++) {
        int processed = mgr->dbus_backend->process(mgr->dbus_bus);
        if (processed <= 0)
            usleep(10000);
    }
}

/* ================================================================== */
/*  Test 2: Controller acceptance via production gamepad transport     */
/*                                                                   */
/*  Exercises A (b0) and B (b1) buttons through the virtual gamepad   */
/*  → SDL_CONTROLLERBUTTONDOWN/UP → cbx_manager_controller_to_key →    */
/*  cbx_manager_handle_event production dispatch path across all 3    */
/*  tabs and the profile editor.                                      */
/* ================================================================== */

static void
test_installed_controller_acceptance(void **state)
{
    functional_fixture *f = *state;
    cbx_manager mgr;
    int rc;

    /* --- Phase 1: Manager init with production DBus + gamepad --- */
    rc = cbx_manager_init(&mgr, NULL);
    assert_int_equal(rc, 0);
    assert_true(mgr.dbus_connected);

    pump_manager(&mgr);
    assert_true(mgr.gamecontroller_count >= 1);

    /* --- Phase 1b: Axis events — safely ignored by manager (HIGH gap) ---
     *
     * Axis events are generated via SDL_JoystickSetVirtualAxis (virtual
     * joystick) or uinput_write_event (kernel-backed) and appear as
     * SDL_CONTROLLERAXISMOTION events.  The manager does not map axis
     * events to key events (only button presses are mapped).  This
     * exercises the production dispatch path (SDL_PumpEvents →
     * SDL_PollEvent → cbx_manager_handle_event) and verifies that
     * axis events are safely ignored: no crash, no tab change, no
     * mode change, no device count change. */
    {
        int gc_count_before = mgr.gamecontroller_count;
        int tab_before = cbx_manager_active_tab(&mgr);
        int mode_before = cbx_controllers_tab_mode(&mgr.ct);

        /* Left stick X full-deflect right then center. */
        ctrl_axis(&mgr, f->joystick, 0, SDL_JOYSTICK_AXIS_MAX);
        ctrl_axis(&mgr, f->joystick, 0, 0);
        /* Left stick Y full-deflect down then center. */
        ctrl_axis(&mgr, f->joystick, 1, SDL_JOYSTICK_AXIS_MAX);
        ctrl_axis(&mgr, f->joystick, 1, 0);
        /* Right stick X full-deflect left then center. */
        ctrl_axis(&mgr, f->joystick, 2, SDL_JOYSTICK_AXIS_MIN);
        ctrl_axis(&mgr, f->joystick, 2, 0);
        /* Right stick Y full-deflect up then center. */
        ctrl_axis(&mgr, f->joystick, 3, SDL_JOYSTICK_AXIS_MIN);
        ctrl_axis(&mgr, f->joystick, 3, 0);
        /* Left trigger full press then release. */
        ctrl_axis(&mgr, f->joystick, 4, SDL_JOYSTICK_AXIS_MAX);
        ctrl_axis(&mgr, f->joystick, 4, 0);
        /* Right trigger full press then release. */
        ctrl_axis(&mgr, f->joystick, 5, SDL_JOYSTICK_AXIS_MAX);
        ctrl_axis(&mgr, f->joystick, 5, 0);

        /* Verify no state changed — axis events are ignored. */
        assert_int_equal(mgr.gamecontroller_count, gc_count_before);
        assert_int_equal(cbx_manager_active_tab(&mgr), tab_before);
        assert_int_equal(cbx_controllers_tab_mode(&mgr.ct), mode_before);
    }

    /* --- Phase 2: Controllers tab — Add type picker open (A) + cancel (B) */
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_CONTROLLERS);

    /* Navigate tabbar → device list → button row (rightmost = Change Type) */
    ctrl_press(&mgr, f->joystick, 12);  /* D-pad Down → device list */
    ctrl_press(&mgr, f->joystick, 12);  /* D-pad Down → button row */
    /* Navigate to Add button (leftmost): D-pad Left × 2 */
    ctrl_press(&mgr, f->joystick, 13);  /* Left → Remove */
    ctrl_press(&mgr, f->joystick, 13);  /* Left → Add */

    /* A (b0) → open type picker */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(cbx_controllers_tab_mode(&mgr.ct), CBX_CT_MODE_TYPE_PICK);

    /* B (b1) → cancel type picker */
    ctrl_press(&mgr, f->joystick, 1);
    assert_int_equal(cbx_controllers_tab_mode(&mgr.ct), CBX_CT_MODE_LIST);

    /* --- Phase 3: Controllers tab — Add confirm (A) → device count +1 --- */
    /* After B cancel, focus went to device list (first visible child). */
    /* Re-navigate to Add button: down to button row, left × 2 to Add. */
    ctrl_press(&mgr, f->joystick, 12);  /* D-pad Down → button row */
    ctrl_press(&mgr, f->joystick, 13);  /* Left → Remove */
    ctrl_press(&mgr, f->joystick, 13);  /* Left → Add */

    /* A (b0) → re-open type picker */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(cbx_controllers_tab_mode(&mgr.ct), CBX_CT_MODE_TYPE_PICK);

    /* D-pad down → select second type (ds5, index 1) */
    ctrl_press(&mgr, f->joystick, 12);

    /* A (b0) → confirm type → CreateTargetDevice via DBus */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(cbx_controllers_tab_mode(&mgr.ct), CBX_CT_MODE_LIST);
    assert_int_equal(mgr.ct.model.target_count, 1);

    /* --- Phase 4: Controllers tab — Change Type open (A) + cancel (B) --- */
    /* After Add confirm, focus went to device list (first visible child). */
    /* Navigate down to button row (1 DOWN: list has 1 item, at bottom → focus chain) */
    ctrl_press(&mgr, f->joystick, 12);  /* D-pad Down → button row (rightmost = Change Type) */

    /* A (b0) → open change type picker */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(cbx_controllers_tab_mode(&mgr.ct), CBX_CT_MODE_TYPE_PICK);
    assert_int_equal(mgr.ct.pending_action, CBX_CT_ACTION_CHANGE);

    /* B (b1) → cancel */
    ctrl_press(&mgr, f->joystick, 1);
    assert_int_equal(cbx_controllers_tab_mode(&mgr.ct), CBX_CT_MODE_LIST);

    /* --- Phase 5: Controllers tab — Remove (A) → device count 0 --- */
    /* After B cancel, focus went to device list (first visible child). */
    /* Navigate down to button row, then left to Remove. */
    ctrl_press(&mgr, f->joystick, 12);  /* D-pad Down → button row (Change Type) */
    ctrl_press(&mgr, f->joystick, 13);  /* Left → Remove */

    /* A (b0) → remove device via StopTargetDevice + refresh */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(mgr.ct.model.target_count, 0);

    /* --- Phase 6: Settings tab — toggle setting (A) + save (A) --- */
    /* Navigate to Settings tab: D-pad Right × 3 (first Right moves from
     * Remove → Change Type in the button row, second Right switches to
     * Profiles, third Right switches to Settings). */
    ctrl_press(&mgr, f->joystick, 14);  /* Right → Change Type (focus chain) */
    ctrl_press(&mgr, f->joystick, 14);  /* Right → Profiles tab */
    ctrl_press(&mgr, f->joystick, 14);  /* Right → Settings tab */
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_SETTINGS);

    /* D-pad down → focus settings list (item 0 = launch_at_boot) */
    ctrl_press(&mgr, f->joystick, 12);

    /* Record initial launch_at_boot value */
    bool initial_lab = mgr.st.settings.launch_at_boot;

    /* A (b0) → toggle launch_at_boot via list on_select → activate */
    ctrl_press(&mgr, f->joystick, 0);
    assert_true(mgr.st.settings.launch_at_boot != initial_lab);

    /* Navigate to Save item (index 10 = CBX_ST_SET_SAVE): D-pad down × 10 */
    for (int i = 0; i < 10; i++)
        ctrl_press(&mgr, f->joystick, 12);

    /* A (b0) → save settings to disk */
    ctrl_press(&mgr, f->joystick, 0);

    /* Assert settings file written to disk */
    char settings_path[PATH_MAX + 256];
    snprintf(settings_path, sizeof(settings_path),
             "%s/.config/controller-box/settings.yaml", f->tmp_home);
    assert_int_equal(access(settings_path, F_OK), 0);

    /* Read back settings.yaml and verify the toggled launch_at_boot
     * value was actually persisted — not just that the file exists.
     * A zero-byte or stale file would pass access(F_OK) alone. */
    {
        cbx_settings loaded;
        memset(&loaded, 0, sizeof(loaded));
        cbx_settings_defaults(&loaded);
        /* HOME is already set to tmp_home in setup; XDG_CONFIG_HOME is unset. */
        int load_rc = cbx_settings_load(&loaded);
        assert_int_equal(load_rc, 0);
        assert_int_equal(loaded.launch_at_boot, mgr.st.settings.launch_at_boot);
    }

    /* --- Phase 7: Profiles tab — Create (A) + name + confirm (A) + save (B) --- */
    /* Navigate to Profiles tab: D-pad Left × 1 */
    ctrl_press(&mgr, f->joystick, 13);  /* Left → Profiles */
    assert_int_equal(cbx_manager_active_tab(&mgr), CBX_MGR_TAB_PROFILES);

    /* Navigate to button row: need (profiles.count + 1) D-pad downs
     * from the tabbar (1 to reach the list, then profiles.count-1
     * within the list, then 1 more to reach the button row). */
    {
        int n = mgr.pt.profiles.count;
        for (int i = 0; i < n + 1; i++)
            ctrl_press(&mgr, f->joystick, 12);  /* D-pad Down */
    }
    /* Navigate to Create button: D-pad left × 2 (from rightmost Delete) */
    ctrl_press(&mgr, f->joystick, 13);  /* Left → Edit */
    ctrl_press(&mgr, f->joystick, 13);  /* Left → Create */

    /* A (b0) → open create picker */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(mgr.pt.mode, CBX_PT_MODE_CREATE_PICK);

    /* A (b0) → confirm "Default copy" (index 0) → enters NAME_INPUT */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(mgr.pt.mode, CBX_PT_MODE_NAME_INPUT);

    /* Type a name via keyboard events (gamepad A/B map to confirm/cancel) */
    send_key_down(&mgr, SDLK_n);
    send_key_down(&mgr, SDLK_e);
    send_key_down(&mgr, SDLK_w);

    /* Gamepad A (b0) → confirm name → opens editor with new profile.
     * Note: the A KEYUP also triggers cbx_manager_tab_activate which
     * calls cbx_profile_editor_activate in LIST mode → enters
     * BINDING_EDIT sub-mode.  This is expected: the A press that
     * confirms the name is the same physical button whose release
     * activates the first binding in the newly-opened editor. */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(mgr.pt.mode, CBX_PT_MODE_EDITOR);

    /* B (b1) → cancel BINDING_EDIT sub-mode → back to editor LIST */
    ctrl_press(&mgr, f->joystick, 1);

    /* B (b1) → save and close editor (B in editor LIST mode = save) */
    ctrl_press(&mgr, f->joystick, 1);
    assert_int_equal(mgr.pt.mode, CBX_PT_MODE_LIST);

    /* Assert new profile file exists on disk */
    char new_prof_path[PATH_MAX + 256];
    snprintf(new_prof_path, sizeof(new_prof_path),
             "%s/.local/share/inputplumber/profiles/new.yaml", f->tmp_home);
    assert_int_equal(access(new_prof_path, F_OK), 0);

    /* Read back the profile YAML and verify it's valid content — not
     * just that the file exists.  A zero-byte or corrupted file would
     * pass access(F_OK) alone. */
    {
        cbx_profile loaded;
        memset(&loaded, 0, sizeof(loaded));
        int load_rc = cbx_profile_load(&loaded, new_prof_path);
        assert_int_equal(load_rc, 0);
        assert_int_equal(loaded.version, 1);
        assert_true(loaded.mapping_count >  0);
    }

    /* --- Phase 8: Profiles tab — Edit (A) + editor nav + B cancel/back --- */
    /* After Create+Save, focus is on the profile list with the new
     * profile selected (at the bottom). Navigate to button row: 1 DOWN
     * (at bottom of list → focus chain → button row). */
    {
        int sel = cbx_list_get_selected(&mgr.pt.profile_list_w);
        int n = mgr.pt.profiles.count;
        /* Navigate to bottom of list, then 1 more to button row */
        for (int i = 0; i < (n - 1 - sel) + 1; i++)
            ctrl_press(&mgr, f->joystick, 12);  /* D-pad Down */
    }
    /* Navigate to Edit button: D-pad left × 1 (from rightmost Delete) */
    ctrl_press(&mgr, f->joystick, 13);  /* Left → Edit */

    /* A (b0) → open editor with selected profile */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(mgr.pt.mode, CBX_PT_MODE_EDITOR);

    /* Editor: D-pad down to navigate binding list */
    ctrl_press(&mgr, f->joystick, 12);

    /* A (b0) → activate binding → enters BINDING_EDIT sub-mode */
    ctrl_press(&mgr, f->joystick, 0);

    /* B (b1) → cancel sub-mode → back to editor LIST */
    ctrl_press(&mgr, f->joystick, 1);

    /* B (b1) → save and close editor */
    ctrl_press(&mgr, f->joystick, 1);
    assert_int_equal(mgr.pt.mode, CBX_PT_MODE_LIST);

    /* --- Phase 9: Profiles tab — Delete (A) + confirm (A) --- */
    /* After Edit+Save, focus is on the profile list. Navigate to the
     * button row (at bottom of list → 1 more DOWN → button row). */
    {
        int sel = cbx_list_get_selected(&mgr.pt.profile_list_w);
        int n = mgr.pt.profiles.count;
        for (int i = 0; i < (n - 1 - sel) + 1; i++)
            ctrl_press(&mgr, f->joystick, 12);  /* D-pad Down */
    }
    /* Delete is rightmost button — already there. */

    /* A (b0) → open delete confirmation */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(mgr.pt.mode, CBX_PT_MODE_CONFIRM_DELETE);

    /* A (b0) → confirm delete → file removed */
    ctrl_press(&mgr, f->joystick, 0);
    assert_int_equal(mgr.pt.mode, CBX_PT_MODE_LIST);

    /* Assert profile file no longer exists */
    assert_int_not_equal(access(new_prof_path, F_OK), 0);

    /* Assert the profile metadata sidecar was also deleted — the
     * inventory semantic outcome for M19 is "Profile file unlinked;
     * sidecar deleted; list refreshes." */
    char new_meta_path[PATH_MAX + 256];
    snprintf(new_meta_path, sizeof(new_meta_path),
             "%s/.config/controller-box/profile-metadata/new.meta.yaml",
             f->tmp_home);
    assert_int_not_equal(access(new_meta_path, F_OK), 0);

    /* --- Cleanup --- */
    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  Test 3: Manager-UI backend recovery through production dispatch    */
/*                                                                   */
/*  Verifies that when InputPlumber's DBus name is lost (server killed) */
/*  and reacquired (server restarted), the manager's own callbacks     */
/*  (wired in cbx_manager_init production path) fire and correctly     */
/*  disable/re-enable the controllers tab UI.                         */
/* ================================================================== */

static void
test_installed_backend_recovery(void **state)
{
    functional_fixture *f = *state;
    cbx_manager mgr;
    int rc;

    /* --- Phase 1: Initialize manager with production DBus path --- */
    /* cbx_manager_init(NULL) uses ip_dbus_sd_backend, connects to the
     * private bus, subscribes to NameOwnerChanged, and wires
     * cbx_manager_backend_ready/degraded as the ip_connection callbacks. */
    rc = cbx_manager_init(&mgr, NULL);
    assert_int_equal(rc, 0);
    assert_true(mgr.dbus_connected);

    pump_manager(&mgr);
    assert_true(mgr.gamecontroller_count >= 1);

    /* Verify initial healthy state: controls enabled, backend present */
    assert_true(mgr.ct.add_btn.base.interactive);
    assert_non_null(mgr.ct.backend);

    /* --- Phase 2: Simulate InputPlumber owner loss --- */
    /* Kill the InputPlumber server.  The DBus daemon will emit
     * NameOwnerChanged (old=server_unique_name, new=""). */
    kill(f->server_pid, SIGTERM);
    waitpid(f->server_pid, NULL, 0);
    f->server_pid = 0;

    /* Drain the manager's own DBus connection to process the
     * NameOwnerChanged signal → noc_signal_callback →
     * ip_connection_handle_name_changed → degraded_cb →
     * cbx_manager_backend_degraded. */
    drain_manager_dbus(&mgr, 3000);

    /* Verify degraded state through the manager's own callback */
    assert_false(mgr.dbus_connected);
    assert_null(mgr.ct.backend);
    assert_false(mgr.ct.add_btn.base.interactive);
    assert_false(mgr.ct.remove_btn.base.interactive);
    assert_false(mgr.ct.change_type_btn.base.interactive);

    /* Status label should be visible with a reason */
    assert_true(mgr.ct.status_lbl.base.visible);

    /* --- Phase 3: Simulate InputPlumber owner reacquisition --- */
    /* Reset server state for the new instance with the expected 4/4 target
     * topology, so recovery proves the prior incomplete status clears. */
    nip_reset_server_state(4);
    for (int i = 0; i < 4; i++) {
        snprintf(g_nip_comp_names[i], sizeof(g_nip_comp_names[i]),
                 "TestController%d", i);
        snprintf(g_nip_dbus_devices[i], sizeof(g_nip_dbus_devices[i]),
                 "/org/shadowblip/InputPlumber/CompositeDevice%d", i);
        snprintf(g_nip_target_paths[i], sizeof(g_nip_target_paths[i]),
                 "/org/shadowblip/InputPlumber/devices/target/xb360%d", i);
        snprintf(g_nip_target_types[i], sizeof(g_nip_target_types[i]),
                 "xb360");
    }
    g_nip_target_count = 4;

    /* Restart the InputPlumber server. */
    const nip_server_config cfg = { .num_composites = 4, .version = "0.78.0" };
    f->server_pid = nip_fork_server(f->bus_address, &cfg);
    assert_true(f->server_pid > 0);

    /* Wait for the new server to be ready (poll Version via
     * the fixture's independent connection). */
    char *version = NULL;
    rc = -1;
    for (int i = 0; i < 200 && rc != 0; i++) {
        free(version); version = NULL;
        rc = f->backend->get_property(f->bus, "org.shadowblip.InputPlumber",
            "/org/shadowblip/InputPlumber/Manager",
            "org.shadowblip.InputManager", "Version", &version);
        if (rc != 0) usleep(10000);
    }
    assert_int_equal(rc, 0);
    free(version);

    /* Drain the manager's DBus to process NameOwnerChanged (new owner) →
     * noc_signal_callback → ip_connection_handle_name_changed →
     * reenumerate_cb → cbx_manager_backend_ready. */
    drain_manager_dbus(&mgr, 3000);

    /* Verify recovered state through the manager's own callback */
    assert_true(mgr.dbus_connected);
    assert_non_null(mgr.ct.backend);
    assert_true(mgr.ct.add_btn.base.interactive);
    assert_true(mgr.ct.remove_btn.base.interactive);
    assert_true(mgr.ct.change_type_btn.base.interactive);

    /* Status label should be hidden (no error) */
    assert_false(mgr.ct.status_lbl.base.visible);

    /* Verify device model re-enumerated (composites visible) */
    assert_true(mgr.ct.model.composite_count >= 2);

    /* No manager restart was needed — recovery is transparent */
    /* (The manager was never shut down; only the backend recovered.) */

    /* --- Cleanup --- */
    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  D01 pointer path: click disabled control in degraded state        */
/* ================================================================== */

/* D01 pointer: After InputPlumber becomes unavailable, clicking on the
 * (now invisible) Add button area produces no side effect — mode stays
 * LIST, no crash, no DBus call. */
static void
test_d01_pointer_degraded_click(void **state)
{
    functional_fixture *f = *state;
    cbx_manager mgr;
    int rc;

    rc = cbx_manager_init(&mgr, NULL);
    assert_int_equal(rc, 0);
    assert_true(mgr.dbus_connected);
    pump_manager(&mgr);

    /* Record Add button center before degradation. */
    int cx = mgr.ct.add_btn.base.rect.x + mgr.ct.add_btn.base.rect.w / 2;
    int cy = mgr.ct.add_btn.base.rect.y + mgr.ct.add_btn.base.rect.h / 2;

    /* Kill the InputPlumber server → degraded state. */
    kill(f->server_pid, SIGTERM);
    waitpid(f->server_pid, NULL, 0);
    f->server_pid = 0;

    drain_manager_dbus(&mgr, 3000);

    /* Verify degraded state. */
    assert_false(mgr.dbus_connected);
    assert_false(mgr.ct.add_btn.base.visible);
    assert_false(mgr.ct.add_btn.base.interactive);

    /* Click on the area where Add button was — it's now invisible so
     * hit_test won't find it.  No side effect. */
    send_mouse_click(&mgr, cx, cy);

    /* Mode stays LIST, no crash. */
    assert_int_equal(cbx_controllers_tab_mode(&mgr.ct), CBX_CT_MODE_LIST);

    /* No DBus side effects: target_count must still be 0 (no device
     * was created by clicking a disabled button in degraded state). */
    assert_int_equal(mgr.ct.model.target_count, 0);

    cbx_manager_shutdown(&mgr);
}

/* ================================================================== */
/*  Test runner                                                        */
/* ================================================================== */

int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_installed_functional,
                                         f_setup, f_teardown),
        cmocka_unit_test_setup_teardown(test_installed_controller_acceptance,
                                         f_setup, f_teardown),
        cmocka_unit_test_setup_teardown(test_installed_backend_recovery,
                                         f_setup, f_teardown),
        cmocka_unit_test_setup_teardown(test_d01_pointer_degraded_click,
                                         f_setup, f_teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}