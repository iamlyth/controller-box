/*
 * routing_observer.c — independent read-only observer for the
 * `controller-production-routing` candidate probe (Controller-Box infra).
 *
 * Opens TWO kernel event devices on separate read-only file descriptors:
 *  1. the physical USB Xbox 360 pad (VID 045e / PID 028e) that the human
 *     presses, and
 *  2. one of the virtual xb360 target event nodes that InputPlumber created
 *     (the node the physical event must be routed to through a composite).
 *
 * It NEVER writes to either device, never uses uinput, never injects
 * events, and never touches InputEvent / a private DBus. It only observes.
 *
 * PASS requires, in order, within the window:
 *  - a FRESH (post-baseline) physical event with the expected type/code/value,
 *    then
 *  - a FRESH target event with the SAME expected type/code/value.
 * If a matching target event arrives with NO preceding fresh physical event,
 * that is treated as direct injection/synthetic routing and the observer
 * fails. A physical event before the baseline is stale and ignored. A target
 * event that never arrives after a fresh physical event means the physical
 * event was not routed and the observer fails.
 *
 * Fixture mode (--fixture FILE) replays a recorded event stream for the
 * deterministic adversarial test suite. The committed candidate contract
 * probe_argv never passes --fixture, so live runs always observe the real
 * devices.
 *
 * Exit codes: 0 = routed fresh physical->target event observed,
 *             1 = fail, 2 = usage.
 *
 * Build (within the project nix-shell):
 *   cc -O2 -o routing_observer routing_observer.c
 */
#define _GNU_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static const char *g_physical_device = NULL;
static const char *g_target_device = NULL;
static int g_window_ms = 60000;
static int g_expected_type = EV_KEY;
static int g_expected_code = BTN_SOUTH; /* 304 */
static int g_expected_value = 1;
static const char *g_fixture = NULL;
static const char *g_physical_name = NULL;
static const char *g_target_name = NULL;

static void fail_observer(const char *message)
{
    fprintf(stderr, "routing-observer: FAIL: %s\n", message);
    exit(1);
}

static int code_matches(int type, int code, int value)
{
    return type == g_expected_type && code == g_expected_code
        && value == g_expected_value;
}

/* Convert a struct timeval into microseconds since the epoch (realtime),
 * the same clock family evdev reports. */
static long long to_usec(const struct timeval *tv)
{
    return (long long)tv->tv_sec * 1000000LL + (long long)tv->tv_usec;
}

static long long now_usec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000000LL + (long long)ts.tv_nsec / 1000LL;
}

/* Read all pending events off a fd.  Returns 1 if a matching fresh event
 * was seen (timestamp >= baseline_usec), else 0. */
static int drain_events(int fd, long long baseline_usec, const char *label,
                        int *saw_fresh)
{
    struct input_event events[16];
    ssize_t count;
    int matched = 0;
    while ((count = read(fd, events, sizeof(events))) > 0) {
        size_t n = (size_t)count / sizeof(struct input_event);
        for (size_t i = 0; i < n; i++) {
            const struct input_event *ev = &events[i];
            if (ev->type == EV_SYN)
                continue;
            long long ts = to_usec(&ev->time);
            printf("routing-observer: %s event type=%d code=%d value=%d ts=%lld\n",
                   label, ev->type, ev->code, ev->value, ts);
            if (ts < baseline_usec) {
                printf("routing-observer: %s event ts=%lld before baseline %lld (stale)\n",
                       label, ts, baseline_usec);
                continue;
            }
            *saw_fresh = 1;
            if (code_matches(ev->type, ev->code, ev->value)) {
                printf("routing-observer: %s FRESH MATCH type=%d code=%d value=%d\n",
                       label, ev->type, ev->code, ev->value);
                matched = 1;
            }
        }
    }
    if (count < 0 && errno != EAGAIN && errno != EINTR) {
        fprintf(stderr, "routing-observer: read failed on %s: %s\n", label,
                strerror(errno));
    }
    return matched;
}

/* Verify an evdev node's identity name matches the expectation. */
static int check_identity(int fd, const char *expected, const char *label)
{
    if (!expected)
        return 1;
    char name[256] = {0};
    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
        fprintf(stderr, "routing-observer: cannot read %s evdev identity: %s\n",
                label, strerror(errno));
        return 0;
    }
    if (strcmp(name, expected) != 0) {
        fprintf(stderr, "routing-observer: %s evdev identity '%s' does not match "
                        "expected '%s' (substituted/injected device rejected)\n",
                label, name, expected);
        return 0;
    }
    printf("routing-observer: %s identity name=%s\n", label, name);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Fixture replay (deterministic adversarial tests only)               */
/* ------------------------------------------------------------------ */

static int run_fixture(void)
{
    FILE *stream = fopen(g_fixture, "r");
    if (!stream)
        fail_observer("cannot open fixture event stream");
    char line[512];
    long long baseline = 1000;
    int physical_seen = 0;
    int physical_fresh = 0;
    long long physical_ts = -1;
    while (fgets(line, sizeof(line), stream)) {
        long long ts = 0;
        int type = 0, code = 0, value = 0;
        char device[64] = {0};
        if (sscanf(line, "baseline %lld", &baseline) == 1) {
            continue;
        }
        if (sscanf(line, "%63s event %d %d %d ts %lld", device, &type, &code,
                   &value, &ts) == 5) {
            printf("routing-observer: %s event type=%d code=%d value=%d ts=%lld\n",
                   device, type, code, value, ts);
            if (strcmp(device, "physical") == 0) {
                if (ts < baseline) {
                    printf("routing-observer: physical event ts=%lld before baseline %lld (stale)\n",
                           ts, baseline);
                    continue;
                }
                physical_seen = 1;
                physical_fresh = 1;
                physical_ts = ts;
                if (!code_matches(type, code, value)) {
                    printf("routing-observer: physical event does not match expected type/code/value\n");
                    continue;
                }
            } else if (strcmp(device, "target") == 0) {
                if (!physical_seen) {
                    fprintf(stderr,
                            "routing-observer: target event without a preceding fresh "
                            "physical event (direct injection/synthetic routing rejected)\n");
                    fclose(stream);
                    return 1;
                }
                if (ts < baseline || ts < physical_ts) {
                    printf("routing-observer: target event ts=%lld before physical ts=%lld (stale)\n",
                           ts, physical_ts);
                    continue;
                }
                if (code_matches(type, code, value)) {
                    printf("routing-observer: target FRESH MATCH type=%d code=%d value=%d\n",
                           type, code, value);
                    fclose(stream);
                    printf("routing-observer: PASS (physical->target routed event observed)\n");
                    return 0;
                }
            }
        }
    }
    fclose(stream);
    if (!physical_seen) {
        fail_observer("no fresh physical event observed (presence alone cannot pass)");
    }
    if (!physical_fresh) {
        fail_observer("physical event was stale (before baseline)");
    }
    fail_observer("no routed target event observed after the fresh physical event");
    return 1;
}

/* ------------------------------------------------------------------ */
/* Live two-fd poll path                                               */
/* ------------------------------------------------------------------ */

static int open_device(const char *path, const char *label,
                       const char *expected_name)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "routing-observer: cannot open %s device %s: %s\n",
                label, path, strerror(errno));
        fail_observer("cannot open an event device");
    }
    unsigned char bit[1] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(bit)), bit) < 0) {
        close(fd);
        fail_observer("device is not an evdev node");
    }
    if (!check_identity(fd, expected_name, label)) {
        close(fd);
        fail_observer("evdev identity mismatch");
    }
    printf("routing-observer: observing %s on %s\n", label, path);
    return fd;
}

static int run_live(void)
{
    int physical_fd = open_device(g_physical_device, "physical", g_physical_name);
    int target_fd = open_device(g_target_device, "target", g_target_name);

    long long baseline = now_usec();
    printf("routing-observer: waiting for a routed fresh physical->target event "
           "within %d ms (human must press a button on the physical pad now)\n",
           g_window_ms);

    struct pollfd pollfds[2] = {
        {.fd = physical_fd, .events = POLLIN},
        {.fd = target_fd, .events = POLLIN},
    };

    int deadline = g_window_ms;
    int physical_fresh = 0;
    int physical_matched = 0;
    while (deadline > 0) {
        int ready = poll(pollfds, 2, 200);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            close(physical_fd);
            close(target_fd);
            fail_observer("poll failed");
        }
        if (pollfds[0].revents & POLLIN) {
            int saw_fresh = 0;
            if (drain_events(physical_fd, baseline, "physical", &saw_fresh)) {
                physical_matched = 1;
            }
            if (saw_fresh)
                physical_fresh = 1;
        }
        if (pollfds[1].revents & POLLIN) {
            int saw_fresh = 0;
            if (drain_events(target_fd, baseline, "target", &saw_fresh)) {
                if (!physical_fresh) {
                    close(physical_fd);
                    close(target_fd);
                    fail_observer("target event without a preceding fresh physical event "
                                  "(direct injection/synthetic routing rejected)");
                }
                close(physical_fd);
                close(target_fd);
                printf("routing-observer: PASS (physical->target routed event observed)\n");
                return 0;
            }
        }
        deadline -= 200;
    }
    close(physical_fd);
    close(target_fd);
    if (!physical_fresh)
        fail_observer("no fresh physical event observed within the window");
    if (!physical_matched)
        fail_observer("fresh physical event present but did not match expected type/code/value");
    fail_observer("fresh physical event observed but no matching routed target event");
    return 1;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fixture") == 0 && i + 1 < argc) {
            g_fixture = argv[++i];
        } else if (strcmp(argv[i], "--physical-device") == 0 && i + 1 < argc) {
            g_physical_device = argv[++i];
        } else if (strcmp(argv[i], "--target-device") == 0 && i + 1 < argc) {
            g_target_device = argv[++i];
        } else if (strcmp(argv[i], "--physical-name") == 0 && i + 1 < argc) {
            g_physical_name = argv[++i];
        } else if (strcmp(argv[i], "--target-name") == 0 && i + 1 < argc) {
            g_target_name = argv[++i];
        } else if (strcmp(argv[i], "--type") == 0 && i + 1 < argc) {
            g_expected_type = (int)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--code") == 0 && i + 1 < argc) {
            g_expected_code = (int)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--value") == 0 && i + 1 < argc) {
            g_expected_value = (int)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--window") == 0 && i + 1 < argc) {
            g_window_ms = (int)strtol(argv[++i], NULL, 0) * 1000;
        } else {
            fprintf(stderr, "routing-observer: unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (g_fixture)
        return run_fixture();
    if (!g_physical_device || !g_target_device) {
        fprintf(stderr, "routing-observer: --physical-device and --target-device "
                        "required (or --fixture)\n");
        return 2;
    }
    return run_live();
}
