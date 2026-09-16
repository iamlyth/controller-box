/*
 * overlay_service.h — overlay service entry point (Tasks 3–4).
 *
 * Declares run_overlay_service(), the production startup path for the
 * overlay service mode of controller-box (SPEC §2.3, §2.4).
 *
 * The function is kept in a separate translation unit (not main.c) so
 * that cmocka tests can link against it without pulling in main().
 */
#ifndef CBX_OVERLAY_SERVICE_H
#define CBX_OVERLAY_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "dbus/dbus_interface.h"          /* ip_dbus_backend, ip_bus_handle */
#include "dbus/ip_connection.h"
#include "dbus/ip_device_model.h"
#include "dbus/ip_input_signal.h" /* ip_input_id, ip_input_category, ip_input_events */
#include "dbus/ip_intercept_poll.h"
#include "config/config_settings.h"
#include "config/config_assignments.h"
#include "config/config_profile_list.h"
#include "ui/renderer.h"
#include "ui/theme.h"
#include "ui/text.h"
#include "icons/icon_cache.h"
#include "icons/icon_map.h"
#include "overlay/player_mode.h"
#include "overlay/host_mode.h"
#include "overlay/lifecycle.h"
#include "overlay/grid_render.h"
#include "overlay/surface_build.h"
#include "overlay/conflict.h"
#include "overlay/profile_cycle.h"
#include "overlay/dynamic_columns.h"
#include "dbus/ip_hotplug.h"
#include "dbus/ip_properties.h"

/* --- Per-composite activation context (Task 9) ----------------------- */

/*
 * Per-poll activation context: tracks which composite triggered the
 * activation so close sets InterceptMode=PASS on the correct device
 * (SPEC §2.5: close restores PASS on the activating composite).
 */
typedef struct {
    cbx_overlay_lifecycle *lifecycle;
    char composite_path[CBX_MAX_PATH_LEN];
} cbx_poll_activation_ctx;

/* --- Reactive InputPlumber property state (Task 5) -------------------- */

/*
 * Last-known validated values for the InputPlumber properties the overlay
 * observes reactively through org.freedesktop.DBus.Properties.
 * PropertiesChanged (SPEC §10.1) are stored on the per-device model
 * (cbx_device_model.composites[] for CompositeDevice properties and
 * cbx_device_model.gamepad_order/gamepad_order for the Manager property),
 * not in a process-global cache.  See cbx_overlay_on_prop_change().
 */

/* --- Overlay input event handling (Task 6) ---------------------------- */

/*
 * Maximum number of DBusDevice paths tracked in the device_path→row mapping.
 */
#define CBX_MAX_DBUS_DEVICES 64

/*
 * Context for overlay input event handling.  Maps DBusDevice object paths
 * to grid row indices so that InputEvent signals from different controllers
 * are dispatched to the correct row in player/host mode.
 */
typedef struct {
    cbx_player_mode       *pm;
    cbx_host_mode         *hm;
    cbx_select_grid       *grid;
    cbx_overlay_lifecycle *lifecycle;

    /* Device path→row index mapping. */
    char  device_paths[CBX_MAX_DBUS_DEVICES][256];
    int   row_indices[CBX_MAX_DBUS_DEVICES];
    int   path_count;
} cbx_overlay_input_ctx;

/*
 * Add a single device_path→row_idx mapping entry.
 */
void cbx_overlay_input_add_mapping(cbx_overlay_input_ctx *ctx,
                                     const char *device_path,
                                     int row_idx);

/*
 * Build the full device_path→row mapping by querying each composite device's
 * DbusDevices property via ip_composite_get_dbus_devices.
 * Returns 0 on success, negative errno on error (partial mappings may still
 * be populated).
 */
int cbx_overlay_input_build_map(const ip_dbus_backend *backend,
                                  ip_bus_handle bus,
                                  cbx_grid_composite_info *composites,
                                  int comp_count,
                                  cbx_overlay_input_ctx *ctx);

/*
 * Find the row index for a given DBusDevice path.
 * Returns the row index, or -1 if not found.
 */
int cbx_overlay_input_find_row(const char *device_path,
                                 char paths[][256],
                                 const int *rows, int count);

/*
 * Map an ip_input_id to a cbx_pm_input value.
 * Returns 1 if mappable, 0 if not.
 */
int cbx_ip_input_to_pm(ip_input_id input, cbx_pm_input *out);

/*
 * Map an ip_input_id to a cbx_hm_input value.
 * Returns 1 if mappable, 0 if not.
 */
int cbx_ip_input_to_hm(ip_input_id input, cbx_hm_input *out);

/*
 * Callback for ip_input_events: maps device_path→row, dispatches to
 * player_mode or host_mode with the correct row index.
 */
void cbx_overlay_input_cb(ip_input_id input,
                            ip_input_category category,
                            double value,
                            const char *raw_event,
                            const char *device_path,
                            void *userdata);

/* --- Overlay service context (Task 10) ------------------------------- */

/*
 * Comprehensive context struct holding all overlay-service loop state.
 * Enables cbx_overlay_service_step() to be self-contained and testable
 * without running the infinite poll loop.
 */
typedef struct cbx_overlay_service_ctx {
    /* --- Core handles --- */
    cbx_renderer           rend;          /* SDL window + renderer            */
    ip_connection          conn;          /* DBus connection                   */
    cbx_device_model       model;         /* enumerated devices                */
    cbx_settings           settings;      /* loaded config                     */
    cbx_assignments        assignments;   /* slot assignments                  */
    cbx_profile_list       profiles;      /* built-in, user, and system profiles */
    cbx_profile_cycle      profile_cycle; /* live backend profile application    */

    /* --- Rendering resources --- */
    cbx_text_cache         text_cache;
    int                    font_id;
    cbx_theme              theme;
    cbx_icon_map           icon_map;
    cbx_icon_cache         icon_cache;

    /* --- Grid + composites --- */
    cbx_grid_composite_info composites[CBX_MAX_COMPOSITES];
    int                    comp_count;
    cbx_select_grid        grid;

    /* --- Overlay surface --- */
    cbx_overlay_surface    surface;
    cbx_grid_render_ctx    render_ctx;

    /* --- Lifecycle --- */
    cbx_overlay_lifecycle  lifecycle;

    /* --- Mode state --- */
    cbx_player_mode        pm;
    cbx_host_mode          hm;
    cbx_conflict_list      conflicts;

    /* --- Input event handling --- */
    cbx_overlay_input_ctx  input_ctx;
    ip_input_events        input_events;
    char                   expected_sender[128];
    bool                   input_events_ready;

    /* --- InterceptMode polling --- */
    ip_intercept_poll      polls[CBX_MAX_COMPOSITES];
    cbx_poll_activation_ctx poll_acts[CBX_MAX_COMPOSITES];
    int                    poll_count;
    uint32_t               poll_event_type;

    /* --- Hotplug --- */
    ip_hotplug             hp;             /* ObjectManager signal handler       */

    /* --- Reactive PropertiesChanged handling (Task 5) --- */
    ip_properties         props;          /* PropertiesChanged subscription    */

    /* --- Reconciliation status ------------------------------------ */
    struct {
        char phase[32];
        char operation[48];
        char target_kind[CBX_MAX_TYPE_LEN];
        char target_path[CBX_MAX_PATH_LEN];
        int old_count;
        int new_count;
        int rc;
        uint32_t elapsed_ms;
        uint32_t deadline_ms;
        bool deadline_expired;
        int cleanup_failures;
        bool originals_stopped;
        char detail[256];
    } reconcile_status;
    uint32_t reconcile_timeout_ms; /* 0 = CBX_RECONCILE_TIMEOUT_MS */
    uint32_t reconcile_poll_ms;    /* 0 = CBX_RECONCILE_POLL_MS    */

    /* --- Status --- */
    bool                   initialized;   /* true after full init              */
    bool                   backend_ready; /* Version + enumeration succeeded   */

    /* --- Bounded recovery retry (Task 7) --------------------------------- */
    /* When a startup/recovery attempt fails in a required step (owner,
     * enumeration, subscriptions, trigger registration, mapping, assignment
     * restoration, type probes), operations stay disabled and the service
     * retries within the two-second readiness window before waiting for the
     * next NameOwnerChanged. */
    bool                   recovery_pending;
    int                    recovery_attempts;
    uint64_t               recovery_deadline_ms;
    /* Monotonic time of the next paced retry attempt; 0 means "attempt
     * immediately" so a freshly armed or manually seeded budget retries
     * without waiting for the first interval. */
    uint64_t               recovery_next_attempt_ms;
    char                   readiness_detail[256];
} cbx_overlay_service_ctx;

/*
 * Process one iteration of the overlay-service poll loop.
 *
 * Handles pending SDL events (poll-timer ticks, SDL_QUIT, keyboard
 * navigation), processes pending DBus InputEvent signals, advances
 * the lifecycle state machine, and re-renders dirty surfaces.
 *
 * This function is extracted from the poll loop in run_overlay_service()
 * so that tests can inject events and verify outcomes without running
 * the infinite loop.  In production, run_overlay_service() calls this
 * repeatedly inside a while (g_running) loop with SDL_Delay(10).
 */
void cbx_overlay_service_step(cbx_overlay_service_ctx *svc);

/* --- Topology reconciliation (exposed for testing — Task 5) ------------- */

/* Finite production defaults.  Tests may override the per-context values. */
#define CBX_RECONCILE_TIMEOUT_MS 2000u
#define CBX_RECONCILE_POLL_MS      10u
#define CBX_RECOVERY_MAX_ATTEMPTS     8
/* Space retries across the two-second readiness window (2000 / 8) so a
 * transient failure that clears in a few hundred ms is still retried
 * instead of burning the attempt budget in consecutive UI frames. */
#define CBX_RECOVERY_ATTEMPT_INTERVAL_MS 250u

/*
 * (Re)initialize all intercept polls for the current composites.
 * Stops any existing polls first, then creates one per composite with
 * per-composite activation context so close sets PASS on the correct
 * composite (SPEC §2.5).  Exposed for test setup.
 *
 * Returns 0 when every required poll was armed (or there are no composites);
 * negative errno when the poll event type is unavailable or an expected poll
 * could not be started, so a caller can keep operations disabled rather than
 * silently running without intercept detection.
 */
int cbx_overlay_rearm_polls(cbx_overlay_service_ctx *svc);

/*
 * Reconcile InputPlumber's live target topology to match the configured
 * desired topology from settings.yaml (SPEC §5.2).
 *
 * Every CreateTargetDevice return path is retained immediately and polled
 * with a monotonic finite deadline until that exact object is published with
 * the expected Target interface and DeviceType.  Every stop is similarly
 * confirmed by exact-path disappearance.  Retained create paths define slots;
 * ObjectManager reply order is never identity.  On a fresh process, where
 * InputPlumber exposes no intrinsic target-slot property, lexical object-path
 * order is the deterministic fallback and exact returned paths take over for
 * mutations.  Physical composites are optional and attached only by persisted
 * PersistentId assignment.  Exact singleton TargetDevices sets are required.
 *
 * On failure, all paths created by this call are stopped with bounded exact-
 * path confirmation.  Originals stopped by type correction/shrink are not
 * claimed as restored; reconcile_status records that destructive boundary.
 *
 * Returns 0 on success, negative errno on failure.
 */
int cbx_reconcile_startup_targets(cbx_overlay_service_ctx *svc);

/* --- Overlay service entry point ------------------------------------- */

/*
 * Run the overlay service.
 *
 * When dry_run is non-zero, prints a mode banner and returns 0 without
 * performing any initialization — headless-safe for acceptance checks.
 *
 * When dry_run is zero:
 *   1. Initialises SDL video and creates a hidden renderer.
 *   2. Connects to the system DBus; if InputPlumber is unavailable,
 *      logs an error to stderr and returns non-zero (SPEC §2.4).
 *   3. Enumerates composite devices via GetManagedObjects.
 *   4. Builds and pre-renders the overlay grid surface.
 *   5. Registers the overlay trigger on all composite devices.
 *   6. Initialises the overlay lifecycle state machine.
 *   7. Enters the poll loop: polls InterceptMode at 50 ms (DEC-002),
 *      processes SDL events for grid navigation, handles SIGTERM/SIGINT
 *      for clean shutdown.
 *
 * Returns 0 on clean shutdown, non-zero on init failure.
 */
int run_overlay_service(int dry_run);

/* --- Signal handling (exposed for testing — Task 4) ------------------- */

/*
 * Install SIGTERM/SIGINT handlers that request clean shutdown.
 * Returns 0 on success, -1 on sigaction failure.
 */
int cbx_overlay_service_install_signal_handlers(void);

/*
 * Returns true if a shutdown signal (SIGTERM/SIGINT) has been received.
 */
bool cbx_overlay_service_shutdown_requested(void);

/*
 * Reset the shutdown flag to false (for testing).
 */
void cbx_overlay_service_reset_shutdown(void);

/* --- Production callbacks (exposed for testing — Task 11) ------------- */

/*
 * Lifecycle on_save callback: conflict detection → resolution →
 * assignment sync → cbx_assignments_save.  Fired by cbx_overlay_lifecycle_close().
 */
int cbx_overlay_on_save(void *userdata);

/*
 * Player-mode slot-change callback: marks the overlay surface dirty
 * so the next step re-renders.  Fired by cbx_player_mode_handle().
 */
int cbx_overlay_on_slot_change(int row_idx, int new_slot, void *userdata);

/*
 * Player-mode profile-change callback: loads the new profile on
 * InputPlumber via LoadProfilePath DBus call and marks the surface dirty.
 * Fired by cbx_player_mode_handle().
 */
int cbx_overlay_on_profile_change(int row_idx, const char *profile,
                                     const char *composite_path,
                                     void *userdata);

/*
 * Host-mode state-transition callback: marks the overlay surface dirty on
 * host-mode enter and exit so the pre-built surface is re-rendered to
 * reflect the HOST/SELECTED/FROZEN row visuals (SPEC §4.4/§4.9).  Fired by
 * cbx_host_mode_enter()/cbx_host_mode_exit().
 */
int cbx_overlay_on_host_mode_change(bool active, void *userdata);

/*
 * Lifecycle on_closed callback: ends Host Mode when the overlay reaches
 * IDLE so host state cannot leak across overlay sessions (SPEC §4.4).
 * Wired as lifecycle.on_closed in run_overlay_service(); exposed so tests
 * can mirror the exact production wiring.
 */
void cbx_overlay_on_lifecycle_closed(void *userdata);

/*
 * Production PropertiesChanged callback: applies the validated change to
 * the per-device overlay model (the matching composite entry for
 * ProfileName/ProfilePath/TargetDevices/SourceDevicePaths, or the model's
 * Manager GamepadOrder) and, for a profile change, updates the matching
 * grid row's displayed profile before marking the surface dirty.  A change
 * for one device can never overwrite another, and an unknown/foreign path
 * is rejected (SPEC §10.1).  Wired by cbx_overlay_props_wire().
 */
void cbx_overlay_on_prop_change(const char *object_path,
                                const char *iface_name,
                                const char *prop_name,
                                ip_prop_type type,
                                const char *value, int count,
                                void *userdata);

/*
 * Wire the PropertiesChanged subscription into the live connection using
 * the exact production init/subscribe path (ip_properties_init +
 * ip_properties_subscribe).  Must be called after conn.backend/conn.bus and
 * expected_sender are set; safe to call again after a backend reacquisition.
 * Returns 0 on success, negative errno on failure.
 */
int cbx_overlay_props_wire(cbx_overlay_service_ctx *svc);

/* --- Production InterceptMode poll callbacks (Task 13) ---------------- */

/*
 * The production per-composite activating/deactivating/error callbacks that
 * the overlay service wires into its intercept polls.  They are static in
 * release builds; under CBX_TESTING they are exported so native tests can
 * register the exact production callbacks instead of re-implementing local
 * copies (which can drift from the real lifecycle behaviour).
 */
#ifdef CBX_TESTING
void on_intercept_activating(void *userdata);
void on_intercept_deactivating(void *userdata);
void on_intercept_error(int error_code, void *userdata);

/* Install the exact production recovery callbacks (overlay_backend_ready /
 * overlay_backend_degraded) on the context's connection so native tests can
 * exercise owner loss/reacquisition through the production callback path. */
void cbx_overlay_install_recovery_callbacks(cbx_overlay_service_ctx *svc);

/* Test seam for the production grid→assignments/order merge used by the
 * startup/close save path.  Exposed only under CBX_TESTING so a test can
 * prove a disconnected controller's saved gamepad-order position survives a
 * topology shrink (task 6). */
int cbx_overlay_merge_grid_for_test(cbx_assignments *a,
                                    const cbx_select_grid *grid);
#endif /* CBX_TESTING */

#endif /* CBX_OVERLAY_SERVICE_H */