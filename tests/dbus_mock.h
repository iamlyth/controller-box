/*
 * dbus_mock.h — DBus mock backend for unit tests.
 *
 * This header defines the mock backend that implements the production
 * `ip_dbus_backend` vtable from dbus_interface.h.  Tests register canned
 * return values keyed by (interface, member, path); the mock backend
 * replays them when the production code calls the corresponding vtable
 * method.
 *
 * Production definitions (constants, vtable struct, signal payload types)
 * live in src/dbus/dbus_interface.h and are included here for shared
 * access.  No src/ file should include this header — it is test-only.
 */
#ifndef CBX_DBUS_MOCK_H
#define CBX_DBUS_MOCK_H

#include "dbus/dbus_interface.h"

#include <stdbool.h>

/*
 * Mock backend — a simple canned-response store.
 *
 * Each expectation maps (iface, member) → a return code and an optional
 * string value.  When the production code calls a vtable method, the
 * mock looks up the expectation, returns the canned rc, and copies out
 * the canned value (if non-NULL).  A fixed-size table keeps the mock
 * dependency-free and simple; later tasks can extend it.
 */
#define IP_MOCK_MAX_EXPECTATIONS 32

typedef struct {
    const char *iface;
    const char *member;   /* method name, property name, or signal name */
    int         rc;       /* canned return code */
    char       *value;   /* canned string value (heap-owned by mock, or NULL) */
} ip_mock_expectation;

/*
 * Signal subscription table (Task 9 extension).
 * Stores registered callbacks for (iface, member) pairs so that
 * inject_signal can dispatch to the correct callback.
 */
#define IP_MOCK_MAX_SUBSCRIPTIONS 8

/* --- Signal queue (for process()-based dispatch) -------------------- */
/* When a test needs to verify that a run-loop drains DBus signals via
 * process() rather than inject_signal, it can queue a signal here.
 * mock_process() dispatches queued signals to registered callbacks. */
#define IP_MOCK_MAX_QUEUED_SIGNALS 8

/* --- Last method-call argument capture --------------------------------- */
/* The mock records the string input arguments of the most recent
 * call_method so tests can assert the exact values a production caller
 * sends (e.g. the SetTargetDevices CSV, CreateTargetDevice kind,
 * AttachTargetDevice paths) — not merely that a call returned OK.  This
 * is what makes a topology-preservation semantic assertion possible. */
#define IP_MOCK_LAST_ARGS_LEN 512

typedef struct {
    bool   has_call;   /* true after a call_method recorded args */
    char   iface[64];
    char   member[64];
    char   args[IP_MOCK_LAST_ARGS_LEN]; /* string args joined by ',' */
} ip_mock_last_call;

typedef struct {
    char iface[64];
    char member[64];
    /* NameOwnerChanged payload (deep-copied strings). */
    char noc_name[128];
    char noc_old[128];
    char noc_new[128];
    ip_owner_changed_payload noc_payload;
} ip_mock_queued_signal;

typedef struct {
    const char   *iface;
    const char   *member;
    ip_signal_cb  cb;
    void         *userdata;
} ip_mock_subscription;

typedef struct {
    ip_mock_expectation  expectations[IP_MOCK_MAX_EXPECTATIONS];
    int                  count;
    ip_bus_handle        bus;   /* opaque, points back to this struct */
    ip_mock_subscription subscriptions[IP_MOCK_MAX_SUBSCRIPTIONS];
    int                  sub_count;
    int                  subscribe_fail_rc;  /* 0 = normal, <0 = fail subscribe */
    ip_mock_queued_signal queued_signals[IP_MOCK_MAX_QUEUED_SIGNALS];
    int                  queued_signal_count;
    uint32_t             creds_pid;   /* GetConnectionCredentials pid (default 4242) */
    uint32_t             creds_uid;   /* GetConnectionCredentials uid (default 0 = root/trusted) */
    int                  creds_rc;    /* 0 = success; <0 to simulate creds lookup failure */
    int                  unique_name_rc; /* 0 = success; <0 to simulate get_unique_name failure */
    ip_mock_last_call    last_call;   /* most recent method call's string args */
    char target_devices_value[IP_MOCK_LAST_ARGS_LEN]; /* writable as property state */
    bool target_devices_written;
} ip_dbus_mock;

/* --- Mock lifecycle -------------------------------------------------------- */

/* Initialise an empty mock state. */
void ip_dbus_mock_init(ip_dbus_mock *mock);

/* Free any heap-owned canned values. */
void ip_dbus_mock_free(ip_dbus_mock *mock);

/* Return a backend vtable backed by `mock`.  The vtable pointers remain
 * valid as long as `mock` is alive. */
const ip_dbus_backend *ip_dbus_mock_backend(ip_dbus_mock *mock);

/* --- Expectation helpers (test-side) --------------------------------------- */

/* Register a canned return value for (iface, member).  `value` is
 * strdup'd internally (may be NULL).  Returns 0 on success, -1 if the
 * table is full. */
int ip_dbus_mock_expect(ip_dbus_mock *mock, const char *iface,
                        const char *member, int rc, const char *value);

/* Convenience: expect a successful call returning `value`. */
int ip_dbus_mock_expect_ok(ip_dbus_mock *mock, const char *iface,
                            const char *member, const char *value);

/* Convenience: expect a failing call with error code `rc`. */
int ip_dbus_mock_expect_error(ip_dbus_mock *mock, const char *iface,
                              const char *member, int rc);

/* Look up a canned expectation.  Returns NULL if not found. */
const ip_mock_expectation *ip_dbus_mock_find(ip_dbus_mock *mock,
                                             const char *iface,
                                             const char *member);

/* Reset the mock to its initial (empty) state, freeing canned values. */
void ip_dbus_mock_reset(ip_dbus_mock *mock);

/* Configure the credential fingerprint returned by get_connection_creds.
 * Defaults to pid=4242, uid=0 (root — trusted by the anti-squatting
 * policy).  Pass an untrusted uid (e.g. 12345) to exercise sender
 * verification rejection. */
void ip_dbus_mock_set_creds(ip_dbus_mock *mock, uint32_t pid, uint32_t uid);

/* Force get_connection_creds to fail with `rc` (<0), simulating a creds
 * lookup failure (e.g. the owner vanished between name resolution and
 * verification). */
void ip_dbus_mock_set_creds_fail(ip_dbus_mock *mock, int rc);

/* Force get_unique_name to fail with `rc` (<0), simulating a name-to-
 * unique-connection resolution failure during connect.  This is a
 * non-fatal condition in ip_connection (still connected, sender unverified). */
void ip_dbus_mock_set_unique_name_fail(ip_dbus_mock *mock, int rc);

/* Queue a NameOwnerChanged signal for dispatch by mock_process().
 * This allows tests to verify that a run-loop's process() call drains
 * NameOwnerChanged and fires connection callbacks (as opposed to
 * inject_signal which dispatches synchronously). */
int ip_dbus_mock_queue_noc(ip_dbus_mock *mock,
                            const char *name,
                            const char *old_owner,
                            const char *new_owner);

/*
 * Return the string input arguments of the most recent call_method for
 * (iface, member), joined by ','.  If the most recent call_method was not
 * for (iface, member), returns -ENOENT.  On success returns 0 and copies
 * the captured args into `out` (NUL-terminated).  This lets tests assert
 * the exact production request payload, not just the return code.
 */
int ip_dbus_mock_last_call(ip_dbus_mock *mock,
                            const char *iface,
                            const char *member,
                            char *out,
                            size_t outsz);

#endif /* CBX_DBUS_MOCK_H */