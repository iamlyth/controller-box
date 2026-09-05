/*
 * routing_observer.c — independent read-only observer for the
 * `controller-production-routing` candidate probe (Controller-Box infra).
 *
 * Opens TWO kernel event devices on separate read-only file descriptors:
 *  1. the physical USB Xbox 360 pad (VID 045e / PID 028e) that the human
 *     presses, and
 *  2. each of four virtual xb360 target event nodes that InputPlumber created.
 * Fixture replay requires four independent physicalN -> targetN correlations;
 * one event copied to several targets, or target0-only activity, cannot pass.
 *
 * It NEVER writes to either device, never uses uinput, never injects
 * events, and never touches InputEvent / a private DBus. It only observes.
 *
 * PASS requires, in order, within the window:
 *  - a FRESH (post-baseline) physical event matching the expected
 *    type/code/value, whose timestamp is recorded, then
 *  - a FRESH target event with the SAME expected type/code/value at or after
 *    that physical event's timestamp.
 * A matching target event with NO preceding fresh physical event is direct
 * injection/synthetic routing and fails. A matching target event whose
 * preceding physical event did not match (or which precedes the physical
 * event) is a correlation failure and fails. A physical event before the
 * baseline is stale and ignored.
 *
 * Live mode REQUIRES --physical-name and --target-name: both the physical and
 * the target EVIOCGNAME identities must match before any event is accepted,
 * so a substituted or injected device cannot be observed as evidence.
 *
 * --name-only PATH prints the EVIOCGNAME identity of one evdev node (used by
 * the probe to compare target DBus object names against kernel node names
 * for collective-cardinality binding) and exits 0/1.
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

/* Read all pending events off a fd.  Returns 1 if a fresh (>= baseline)
 * matching event was seen; if match_ts is non-NULL the timestamp of that
 * matching event is stored there. saw_fresh is set when any fresh event was
 * read. */
static int drain_events(int fd, long long baseline_usec, const char *label,
                        int *saw_fresh, long long *match_ts)
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
                if (match_ts)
                    *match_ts = ts;
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
    if (!expected) {
        fprintf(stderr, "routing-observer: no %s identity name provided (required)\n",
                label);
        return 0;
    }
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

/* Read the EVIOCGNAME of a single evdev node (identity-only mode). */
static int run_name_only(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "routing-observer: cannot open %s: %s\n", path,
                strerror(errno));
        return 1;
    }
    unsigned char bit[1] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(bit)), bit) < 0) {
        fprintf(stderr, "routing-observer: %s is not an evdev node: %s\n", path,
                strerror(errno));
        close(fd);
        return 1;
    }
    char name[256] = {0};
    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
        fprintf(stderr, "routing-observer: cannot read identity of %s: %s\n", path,
                strerror(errno));
        close(fd);
        return 1;
    }
    printf("%s\n", name);
    close(fd);
    return 0;
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
    long long baseline = 1000, physical_ts[4] = {-1,-1,-1,-1};
    long long previous_physical_ts = -1;
    int matched[4] = {0,0,0,0};
    while (fgets(line, sizeof(line), stream)) {
        long long ts = 0;
        int type = 0, code = 0, value = 0, slot = -1;
        char device[64] = {0};
        if (sscanf(line, "baseline %lld", &baseline) == 1)
            continue;
        if (sscanf(line, "%63s event %d %d %d ts %lld", device, &type, &code,
                   &value, &ts) != 5)
            continue;
        if (sscanf(device, "physical%d", &slot) == 1) {
            if (slot < 0 || slot >= 4 || ts < baseline)
                continue;
            if (!code_matches(type, code, value))
                fail_observer("physical event does not match expected type/code/value");
            if (ts <= previous_physical_ts)
                fail_observer("fresh physical event timestamp was reused or not increasing");
            physical_ts[slot] = ts;
            previous_physical_ts = ts;
            printf("routing-observer: slot=%d fresh physical ts=%lld\n", slot, ts);
        } else if (sscanf(device, "target%d", &slot) == 1) {
            if (slot < 0 || slot >= 4 || physical_ts[slot] < baseline)
                fail_observer("target event without its own preceding fresh physical event (direct injection/synthetic routing rejected)");
            if (!code_matches(type, code, value) || ts < physical_ts[slot])
                fail_observer("target event cannot be correlated to its slot's fresh physical event");
            matched[slot] = 1;
            printf("routing-observer: slot=%d target correlated physical_ts=%lld target_ts=%lld\n",
                   slot, physical_ts[slot], ts);
        }
    }
    fclose(stream);
    for (int i = 0; i < 4; i++)
        if (!matched[i])
            fail_observer("all four targets did not receive independently correlated events");
    printf("routing-observer: PASS (4/4 independently consumable targets)\n");
    return 0;
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
    if (!g_physical_name || !g_target_name) {
        fail_observer("live mode requires --physical-name and --target-name "
                      "(EVIOCGNAME identity verification)");
    }
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
    long long physical_matched_ts = -1;
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
            long long mts = -1;
            if (drain_events(physical_fd, baseline, "physical", &saw_fresh, &mts)) {
                physical_matched = 1;
                physical_matched_ts = mts;
            }
            if (saw_fresh)
                physical_fresh = 1;
        }
        if (pollfds[1].revents & POLLIN) {
            int saw_fresh = 0;
            long long mts = -1;
            if (drain_events(target_fd, baseline, "target", &saw_fresh, &mts)) {
                if (!physical_fresh) {
                    close(physical_fd);
                    close(target_fd);
                    fail_observer("target event without a preceding fresh physical event "
                                  "(direct injection/synthetic routing rejected)");
                }
                if (!physical_matched || mts < physical_matched_ts) {
                    close(physical_fd);
                    close(target_fd);
                    fail_observer("target event cannot be correlated to a fresh matching "
                                  "physical event (physical->target correlation rejected)");
                }
                close(physical_fd);
                close(target_fd);
                printf("routing-observer: RESULT source_event_us=%lld target_event_us=%lld read_only=true\n",
                       physical_matched_ts, mts);
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
    fail_observer("fresh matching physical event observed but no matching routed target event");
    return 1;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--name-only") == 0 && i + 1 < argc) {
            return run_name_only(argv[++i]);
        } else if (strcmp(argv[i], "--fixture") == 0 && i + 1 < argc) {
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
