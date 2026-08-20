/*
 * target_consumer_observer.c — independent evdev consumer for the
 * `target-consumer` candidate probe (Controller-Box infrastructure).
 *
 * Opens a kernel event device on a separate consumer file descriptor and
 * reports whether an exact EV_KEY/EV_ABS event (expected code and value)
 * arrives within the window. It never writes to the device and never uses
 * uinput; it only observes.
 *
 * Fixture mode (--fixture FILE) replays a recorded event stream for the
 * adversarial test suite. The committed candidate contract probe_argv never
 * passes --fixture, so candidate runs always use the live evdev path.
 *
 * Exit codes: 0 = expected event observed, 1 = no match, 2 = usage.
 *
 * Build (within the project nix-shell):
 *   cc -O2 -o target_consumer_observer target_consumer_observer.c
 */
#define _GNU_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *g_device = NULL;
static int g_expected_type = EV_KEY;
static int g_expected_code = BTN_A;
static int g_expected_value = 1;
static int g_window_ms = 15000;
static const char *g_fixture = NULL;

static void fail_observer(const char *message)
{
    fprintf(stderr, "target-consumer-observer: FAIL: %s\n", message);
    exit(1);
}

static int code_matches(int type, int code, int value)
{
    return type == g_expected_type && code == g_expected_code && value == g_expected_value;
}

static int run_fixture(void)
{
    FILE *stream = fopen(g_fixture, "r");
    if (!stream)
        fail_observer("cannot open fixture event stream");
    char line[512];
    int matched = 0;
    while (fgets(line, sizeof(line), stream)) {
        int type = 0, code = 0, value = 0;
        if (sscanf(line, "event %d %d %d", &type, &code, &value) == 3) {
            printf("target-consumer-observer: event type=%d code=%d value=%d\n",
                   type, code, value);
            if (code_matches(type, code, value))
                matched = 1;
        }
    }
    fclose(stream);
    if (!matched)
        fail_observer("expected event not observed on the consumer device");
    printf("target-consumer-observer: MATCH type=%d code=%d value=%d\n",
           g_expected_type, g_expected_code, g_expected_value);
    return 0;
}

static int run_live(void)
{
    int fd = open(g_device, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        fail_observer("cannot open the target kernel event device");
    /* The device must be readable without writing; confirm it is a real
     * evdev node before observing. */
    unsigned char bit[1] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(bit)), bit) < 0) {
        close(fd);
        fail_observer("device is not an evdev node");
    }
    printf("target-consumer-observer: observing %s for type=%d code=%d value=%d "
           "within %d ms\n", g_device, g_expected_type, g_expected_code,
           g_expected_value, g_window_ms);
    struct pollfd pollfd = {.fd = fd, .events = POLLIN};
    int deadline = g_window_ms;
    while (deadline > 0) {
        int ready = poll(&pollfd, 1, 200);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            fail_observer("poll failed on the consumer device");
        }
        if (ready > 0) {
            struct input_event events[16];
            ssize_t count = read(fd, events, sizeof(events));
            if (count < 0) {
                if (errno == EAGAIN || errno == EINTR)
                    continue;
                close(fd);
                fail_observer("read failed on the consumer device");
            }
            size_t n = (size_t)count / sizeof(struct input_event);
            for (size_t i = 0; i < n; i++) {
                const struct input_event *event = &events[i];
                if (event->type == EV_SYN)
                    continue;
                printf("target-consumer-observer: event type=%d code=%d value=%d\n",
                       event->type, event->code, event->value);
                if (code_matches(event->type, event->code, event->value)) {
                    close(fd);
                    printf("target-consumer-observer: MATCH type=%d code=%d value=%d\n",
                           g_expected_type, g_expected_code, g_expected_value);
                    return 0;
                }
            }
            deadline -= 200;
        }
    }
    close(fd);
    fail_observer("expected event not observed on the consumer device");
    return 1;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            g_device = argv[++i];
        } else if (strcmp(argv[i], "--type") == 0 && i + 1 < argc) {
            g_expected_type = (int)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--code") == 0 && i + 1 < argc) {
            g_expected_code = (int)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--value") == 0 && i + 1 < argc) {
            g_expected_value = (int)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--window") == 0 && i + 1 < argc) {
            g_window_ms = (int)strtol(argv[++i], NULL, 0) * 1000;
        } else if (strcmp(argv[i], "--fixture") == 0 && i + 1 < argc) {
            g_fixture = argv[++i];
        } else {
            fprintf(stderr, "target-consumer-observer: unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!g_device && !g_fixture) {
        fprintf(stderr, "target-consumer-observer: --device or --fixture required\n");
        return 2;
    }
    if (g_fixture)
        return run_fixture();
    return run_live();
}
