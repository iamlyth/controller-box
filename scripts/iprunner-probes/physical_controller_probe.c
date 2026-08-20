/*
 * physical_controller_probe.c — fresh human-generated event probe for the
 * `physical-controller` capability (Controller-Box infrastructure).
 *
 * Live mode exercises the same SDL2 controller stack the production
 * Controller-Box binaries use: SDL_Init(JOYSTICK|GAMECONTROLLER), device
 * enumeration by vendor/product (045e:028e), SDL_GameController open on the
 * physical device path, and SDL_CONTROLLERBUTTONDOWN/UP /
 * SDL_CONTROLLERAXISMOTION dispatch. Presence alone is not evidence: the
 * probe PASSes only when a fresh (post-window-start) event arrives from that
 * exact physical device instance. The evdev capability bits (EV_KEY/EV_ABS)
 * of the device are also dumped as evidence.
 *
 * The probe rejects substituted/injected devices: the event must come from
 * the specific SDL instance whose device path matches the required physical
 * path. A uinput-injected device is a different instance and path, so it can
 * never satisfy the probe.
 *
 * Fixture mode (--fixture FILE) replays a recorded event stream for the
 * adversarial test suite. The committed contract probe_argv never passes
 * --fixture, so production runs always use the live SDL path.
 *
 * Exit codes: 0 = fresh event observed, 1 = fail, 77 = SDL unavailable.
 *
 * Build (within the project nix-shell):
 *   cc -O2 -o physical_controller_probe physical_controller_probe.c \
 *      $(pkg-config --cflags --libs sdl2)
 */
#define _GNU_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <SDL.h>

#define VENDOR_XB360 0x045e
#define PRODUCT_XB360 0x028e

static const char *g_required_path = "/dev/input/event5";
static int g_required_vendor = VENDOR_XB360;
static int g_required_product = PRODUCT_XB360;
static int g_window_ms = 90000;
static const char *g_fixture = NULL;

static void fail_probe(const char *message)
{
    fprintf(stderr, "physical-controller-probe: FAIL: %s\n", message);
    exit(1);
}

static void print_evdev_caps(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "physical-controller-probe: cannot open evdev %s: %s\n",
                path, strerror(errno));
        return;
    }
    unsigned char key_bits[(EV_MAX / 8) + 1] = {0};
    unsigned char abs_bits[(ABS_MAX / 8) + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) == 0) {
        printf("physical-controller-probe: EV_KEY bits:");
        for (int i = 0; i < EV_MAX; i++)
            if (key_bits[i / 8] & (1 << (i % 8)))
                printf(" %d", i);
        printf("\n");
    }
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) == 0) {
        printf("physical-controller-probe: EV_ABS bits:");
        for (int i = 0; i < ABS_MAX; i++)
            if (abs_bits[i / 8] & (1 << (i % 8)))
                printf(" %d", i);
        printf("\n");
    }
    close(fd);
}

/* ------------------------------------------------------------------ */
/* Fixture replay (adversarial tests only)                             */
/* ------------------------------------------------------------------ */

static int run_fixture(void)
{
    FILE *stream = fopen(g_fixture, "r");
    if (!stream)
        fail_probe("cannot open fixture");
    char line[512];
    int device_present = 0;
    int required_instance = 0;
    long baseline = 1000;
    int fresh_seen = 0;
    while (fgets(line, sizeof(line), stream)) {
        char kind[64] = {0};
        long a = 0, b = 0;
        char name[128] = {0};
        double value = 0.0;
        if (sscanf(line, "device-present %ld", &a) == 1) {
            device_present = (int)a;
        } else if (sscanf(line, "instance %ld", &a) == 1) {
            required_instance = (int)a;
        } else if (sscanf(line, "baseline %ld", &baseline) == 1) {
            /* fixture-provided window start */
        } else if (sscanf(line, "event %63s %ld ts %ld name %127s value %lf",
                          kind, &a, &b, name, &value) == 5) {
            if ((strcmp(kind, "button") == 0 || strcmp(kind, "axis") == 0)) {
                if ((int)a != required_instance) {
                    fprintf(stderr,
                            "physical-controller-probe: event from instance %ld "
                            "but required instance is %d (injected/substituted device)\n",
                            a, required_instance);
                    fclose(stream);
                    return 1;
                }
                if (b >= baseline) {
                    printf("physical-controller-probe: FRESH EVENT %s=%s value=%.3f "
                           "timestamp=%ld (>= baseline %ld)\n",
                           kind, name, value, b, baseline);
                    fresh_seen = 1;
                } else {
                    printf("physical-controller-probe: stale event %s ts=%ld "
                           "(before baseline %ld) ignored\n", name, b, baseline);
                }
            }
        }
    }
    fclose(stream);
    if (!device_present)
        fail_probe("physical controller absent (presence alone cannot pass)");
    if (!fresh_seen)
        fail_probe("no fresh human-generated event within the window");
    printf("physical-controller-probe: PASS\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Live SDL path                                                       */
/* ------------------------------------------------------------------ */

static int run_live(void)
{
    if (SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0)
        fail_probe("SDL_Init failed");
    int count = SDL_NumJoysticks();
    int found = -1;
    const char *found_path = NULL;
    for (int i = 0; i < count; i++) {
        Uint16 vendor = SDL_JoystickGetDeviceVendor(i);
        Uint16 product = SDL_JoystickGetDeviceProduct(i);
        const char *path = SDL_JoystickPathForIndex(i);
        if (vendor == (Uint16)g_required_vendor
            && product == (Uint16)g_required_product) {
            found = i;
            found_path = path ? path : "";
            break;
        }
    }
    if (found < 0)
        fail_probe("physical controller absent (presence alone cannot pass)");
    printf("physical-controller-probe: device index=%d path=%s vendor=0x%04x product=0x%04x\n",
           found, found_path ? found_path : "(none)", g_required_vendor, g_required_product);
    if (!found_path || strcmp(found_path, g_required_path) != 0) {
        fprintf(stderr,
                "physical-controller-probe: device path %s does not match the "
                "required physical path %s (substituted/injected device rejected)\n",
                found_path ? found_path : "(none)", g_required_path);
        SDL_Quit();
        return 1;
    }
    print_evdev_caps(found_path);

    SDL_Joystick *joystick = SDL_JoystickOpen(found);
    if (!joystick)
        fail_probe("SDL_JoystickOpen failed");
    SDL_GameController *controller = SDL_GameControllerOpen(found);
    if (!controller) {
        SDL_JoystickClose(joystick);
        fail_probe("SDL_GameControllerOpen failed");
    }
    SDL_JoystickID instance = SDL_JoystickInstanceID(joystick);
    Uint32 baseline = SDL_GetTicks();
    printf("physical-controller-probe: waiting for a fresh event within %d ms "
           "(human must press a button or move an axis now)\n", g_window_ms);

    Uint32 deadline = baseline + (Uint32)g_window_ms;
    while (SDL_GetTicks() < deadline) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP:
                if (event.cbutton.which != instance)
                    continue; /* another device — ignored */
                if (event.cbutton.timestamp < baseline)
                    continue; /* queued before the window — stale, ignored */
                printf("physical-controller-probe: FRESH EVENT button=%s value=%.1f "
                       "timestamp=%u (>= baseline %u)\n",
                       SDL_GameControllerGetStringForButton(event.cbutton.button),
                       event.cbutton.state == SDL_PRESSED ? 1.0 : 0.0,
                       event.cbutton.timestamp, baseline);
                SDL_GameControllerClose(controller);
                SDL_JoystickClose(joystick);
                SDL_Quit();
                printf("physical-controller-probe: PASS\n");
                return 0;
            case SDL_CONTROLLERAXISMOTION:
                if (event.caxis.which != instance)
                    continue;
                if (event.caxis.timestamp < baseline)
                    continue;
                printf("physical-controller-probe: FRESH EVENT axis=%s value=%.3f "
                       "timestamp=%u (>= baseline %u)\n",
                       SDL_GameControllerGetStringForAxis(event.caxis.axis),
                       ((float)event.caxis.value) / 32767.0f,
                       event.caxis.timestamp, baseline);
                SDL_GameControllerClose(controller);
                SDL_JoystickClose(joystick);
                SDL_Quit();
                printf("physical-controller-probe: PASS\n");
                return 0;
            default:
                break;
            }
        }
        SDL_Delay(10);
    }
    SDL_GameControllerClose(controller);
    SDL_JoystickClose(joystick);
    SDL_Quit();
    fail_probe("no fresh human-generated event within the window (device present but no event)");
    return 1;
}

int main(int argc, char **argv)
{
    for (int i = 1; i + 1 < argc + 1 && i < argc; i++) {
        if (strcmp(argv[i], "--fixture") == 0 && i + 1 < argc) {
            g_fixture = argv[++i];
        } else if (strcmp(argv[i], "--event-path") == 0 && i + 1 < argc) {
            g_required_path = argv[++i];
        } else if (strcmp(argv[i], "--vendor") == 0 && i + 1 < argc) {
            g_required_vendor = (int)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--product") == 0 && i + 1 < argc) {
            g_required_product = (int)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--window") == 0 && i + 1 < argc) {
            g_window_ms = (int)strtol(argv[++i], NULL, 0) * 1000;
        } else {
            fprintf(stderr, "physical-controller-probe: unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (g_fixture)
        return run_fixture();
    return run_live();
}
