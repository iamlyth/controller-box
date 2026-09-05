/*
 * overlay_service.c — overlay service startup and poll loop (Tasks 3–4).
 *
 * Implements run_overlay_service(): the production init path for the
 * overlay service mode.  The function initialises SDL, connects to
 * InputPlumber via the system DBus, enumerates composite devices,
 * pre-builds the overlay surface, registers intercept triggers, and
 * enters the poll loop with full InterceptMode polling, signal handling,
 * and input processing.
 *
 * SPEC §2.3–§2.5, §4.3–§4.5, §10.2–§10.3.
 */
#include "config.h"
#include "app/overlay_service.h"

#include "config/config_paths.h"
#include "config/config_settings.h"
#include "config/config_assignments.h"
#include "ui/renderer.h"
#include "ui/theme.h"
#include "ui/text.h"
#include "icons/icon_cache.h"
#include "icons/icon_map.h"
#include "dbus/ip_connection.h"
#include "dbus/dbus_interface.h"          /* ip_dbus_backend, ip_bus_handle, ip_dbus_sd_backend */
#include "dbus/ip_objectmanager.h"
#include "dbus/ip_device_model.h"
#include "dbus/ip_composite.h"
#include "dbus/ip_manager.h"
#include "dbus/ip_target.h"
#include "dbus/ip_input_signal.h"
#include "dbus/ip_intercept_poll.h"
#include "overlay/surface_build.h"
#include "overlay/lifecycle.h"
#include "overlay/grid_render.h"
#include "overlay/trigger.h"
#include "overlay/player_mode.h"
#include "overlay/host_mode.h"
#include "overlay/conflict.h"
#include "overlay/profile_cycle.h"
#include "overlay/dynamic_columns.h"
#include "dbus/ip_hotplug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

/* ================================================================== */
/*  Signal handling (SIGTERM / SIGINT)                                 */
/* ================================================================== */

static volatile sig_atomic_t g_running = 1;

static void
signal_handler(int signo)
{
    (void)signo;
    g_running = 0;
}

int
cbx_overlay_service_install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;            /* no SA_RESTART — interrupt SDL_PollEvent */

    if (sigaction(SIGTERM, &sa, NULL) != 0)
        return -1;
    if (sigaction(SIGINT, &sa, NULL) != 0)
        return -1;
    return 0;
}

bool
cbx_overlay_service_shutdown_requested(void)
{
    return g_running == 0;
}

void
cbx_overlay_service_reset_shutdown(void)
{
    g_running = 1;
}

/* ================================================================== */
/*  InterceptMode poll callbacks                                       */
/* ================================================================== */

/*
 * Per-composite activating callback: updates the lifecycle's composite_path
 * to the composite that triggered the activation, then activates the
 * overlay.  This ensures that close sets InterceptMode=PASS on the
 * activating composite, not just the primary one (SPEC §2.5).
 *
 * Under CBX_TESTING the storage class is dropped so native tests can wire
 * these exact production callbacks (instead of re-implementing local
 * copies) — see overlay_service.h.  In release builds they remain static.
 */
#ifdef CBX_TESTING
void
on_intercept_activating(void *userdata)
#else
static void
on_intercept_activating(void *userdata)
#endif
{
    cbx_poll_activation_ctx *act = (cbx_poll_activation_ctx *)userdata;
    if (!act || !act->lifecycle)
        return;
    /* Update lifecycle's composite_path to the activating composite. */
    if (act->composite_path[0]) {
        size_t len = strlen(act->composite_path);
        if (len >= sizeof(act->lifecycle->composite_path))
            len = sizeof(act->lifecycle->composite_path) - 1;
        memcpy(act->lifecycle->composite_path, act->composite_path, len);
        act->lifecycle->composite_path[len] = '\0';
    }
    cbx_overlay_lifecycle_activate(act->lifecycle);
}

#ifdef CBX_TESTING
void
on_intercept_deactivating(void *userdata)
#else
static void
on_intercept_deactivating(void *userdata)
#endif
{
    cbx_overlay_lifecycle *lc = (cbx_overlay_lifecycle *)userdata;
    /* If already closing/closed, close() returns -EPERM — that's fine. */
    cbx_overlay_lifecycle_close(lc);
}

#ifdef CBX_TESTING
void
on_intercept_error(int error_code, void *userdata)
#else
static void
on_intercept_error(int error_code, void *userdata)
#endif
{
    (void)userdata;
    fprintf(stderr,
            "controller-box: intercept poll error: %d\n", error_code);
}

/* Exact attachment confirmation is shared with startup reconciliation. */
static int wait_for_attachment(cbx_overlay_service_ctx *svc,
                               const char *composite, const char *target);

/* ================================================================== */
/*  Lifecycle on_save callback: conflict resolution + assignment save */
/* ================================================================== */

int
cbx_overlay_on_save(void *userdata)
{
    cbx_overlay_service_ctx *svc = (cbx_overlay_service_ctx *)userdata;

    /* Detect and resolve conflicts (SPEC §4.5). */
    cbx_conflict_list_init(&svc->conflicts);
    cbx_conflict_detect(&svc->grid, &svc->conflicts);
    cbx_conflict_resolve(&svc->grid, &svc->conflicts);

    /* Build GamepadOrder from the grid state (needed for engine apply). */
    char order[CBX_MAX_COMPOSITES * (CBX_MAX_PATH_LEN + 1)];
    order[0] = '\0';
    size_t order_len = 0;
    for (int slot = 0; slot < CBX_MAX_CONTROLLERS; slot++) {
        for (int i = 0; i < svc->grid.row_count; i++) {
            const cbx_grid_row *row = &svc->grid.rows[i];
            if (cbx_select_grid_col_to_slot(row->cur_col) != slot)
                continue;
            size_t path_len = strlen(row->composite_path);
            size_t need = path_len + (order_len > 0 ? 1 : 0);
            if (order_len + need >= sizeof(order) - 1) {
                /* Buffer would overflow — abort save to avoid truncation */
                return -ENAMETOOLONG;
            }
            if (order_len > 0)
                order[order_len++] = ',';
            memcpy(order + order_len, row->composite_path, path_len);
            order_len += path_len;
            order[order_len] = '\0';
        }
    }

    /* --- Phase 1: Apply to engine and verify (SPEC §§4.1-4.7) ---
     * For each assigned row: LoadProfilePath + verify ProfilePath +
     * AttachTargetDevice.  Engine state is verified before in-memory
     * state or disk persistence is touched.  If any step fails, the
     * in-memory assignments and disk retain the last confirmed state. */
    for (int i = 0; i < svc->grid.row_count; i++) {
        const cbx_grid_row *row = &svc->grid.rows[i];
        int slot = cbx_select_grid_col_to_slot(row->cur_col);
        if (slot < 0)
            continue;
        if (slot >= svc->model.target_count)
            return -ENODEV;

        /* Load profile and verify engine state.  Skip if no profile list
         * is available (e.g. degraded/test mode) — slot assignment and
         * GamepadOrder are still applied. */
        if (row->profile[0] && svc->profile_cycle.profiles) {
            int profile_rc = cbx_profile_cycle_apply(&svc->profile_cycle,
                                                       &svc->grid, i,
                                                       row->profile,
                                                       row->composite_path);
            if (profile_rc != 0)
                return profile_rc;
        }

        /* Attach target to composite for routability. */
        int attach_rc = ip_manager_attach_target_device(
            svc->conn.backend, svc->conn.bus,
            svc->model.targets[slot].path, row->composite_path);
        if (attach_rc != 0)
            return attach_rc;
        attach_rc = wait_for_attachment(svc, row->composite_path,
                                         svc->model.targets[slot].path);
        if (attach_rc != 0) {
            fprintf(stderr,
                "controller-box: assignment not exact after AttachTargetDevice; InputPlumber detach/transfer semantics may not support safe reassignment (slot=%d composite=%s target=%s)\n",
                slot, row->composite_path, svc->model.targets[slot].path);
            return attach_rc;
        }
    }

    /* --- Phase 2: Set GamepadOrder on the engine --- */
    int rc = ip_manager_set_gamepad_order(svc->conn.backend, svc->conn.bus,
                                           order, &svc->model);
    if (rc != 0)
        return rc;

    /* --- Phase 3: All engine state verified — sync in-memory and persist ---
     * Only after every LoadProfilePath, AttachTargetDevice, and
     * SetGamepadOrder succeeded do we update the in-memory assignments
     * and save to disk.  This guarantees that LoadProfile failures do
     * not appear saved (Task 8 acceptance criterion). */
    for (int i = 0; i < svc->grid.row_count; i++) {
        const cbx_grid_row *row = &svc->grid.rows[i];
        int slot = cbx_select_grid_col_to_slot(row->cur_col);

        /* Find existing assignment by id. */
        int found = -1;
        for (int j = 0; j < svc->assignments.assignment_count; j++) {
            if (strcmp(svc->assignments.assignments[j].id,
                       row->id) == 0) {
                found = j;
                break;
            }
        }

        if (slot >= 0) {
            /* Assigned: update or add. */
            if (found >= 0) {
                svc->assignments.assignments[found].slot = slot;
                snprintf(svc->assignments.assignments[found].profile,
                         sizeof(svc->assignments.assignments[found].profile),
                         "%s", row->profile);
            } else if (svc->assignments.assignment_count <
                       CBX_MAX_ASSIGNMENTS) {
                cbx_assignment *a =
                    &svc->assignments.assignments[
                        svc->assignments.assignment_count++];
                snprintf(a->id, sizeof(a->id), "%s", row->id);
                a->slot = slot;
                snprintf(a->profile, sizeof(a->profile),
                         "%s", row->profile);
            }
        } else {
            /* Unassigned: remove entry if present. */
            if (found >= 0) {
                svc->assignments.assignments[found] =
                    svc->assignments.assignments[
                        --svc->assignments.assignment_count];
            }
        }
    }

    /* Save gamepad_order identity IDs for restart restoration. */
    svc->assignments.gamepad_order_count = 0;
    for (int slot = 0; slot < CBX_MAX_CONTROLLERS; slot++) {
        for (int i = 0; i < svc->grid.row_count; i++) {
            const cbx_grid_row *row = &svc->grid.rows[i];
            if (cbx_select_grid_col_to_slot(row->cur_col) != slot)
                continue;
            if (svc->assignments.gamepad_order_count < CBX_MAX_GAMEPAD_ORDER) {
                snprintf(svc->assignments.gamepad_order[
                             svc->assignments.gamepad_order_count++],
                         CBX_MAX_ID_LEN, "%s", row->id);
            }
        }
    }

    rc = cbx_assignments_save(&svc->assignments);
    if (rc != 0)
        return rc;

    cbx_overlay_surface_mark_dirty_all(&svc->surface);
    return 0;
}

/* ================================================================== */
/*  Player Mode callbacks (slot/profile change side effects)          */
/* ================================================================== */

int
cbx_overlay_on_slot_change(int row_idx, int new_slot, void *userdata)
{
    cbx_overlay_service_ctx *svc = (cbx_overlay_service_ctx *)userdata;
    (void)row_idx;
    (void)new_slot;
    /* Grid is already updated by player_mode_handle.
     * Re-detect conflicts so the re-render shows red highlights (SPEC §4.5). */
    cbx_conflict_list_init(&svc->conflicts);
    cbx_conflict_detect(&svc->grid, &svc->conflicts);
    /* Mark surface dirty for re-render. */
    cbx_overlay_surface_mark_dirty_all(&svc->surface);
    return 0;
}

int
cbx_overlay_on_profile_change(int row_idx, const char *profile,
                   const char *composite_path, void *userdata)
{
    cbx_overlay_service_ctx *svc = (cbx_overlay_service_ctx *)userdata;
    (void)row_idx;

    if (!svc->conn.backend || !composite_path || !profile || !profile[0])
        return -EINVAL;

    int rc = cbx_profile_cycle_apply(&svc->profile_cycle, &svc->grid,
                                      row_idx, profile, composite_path);
    if (rc != 0)
        return rc;
    rc = cbx_assignments_save(&svc->assignments);
    if (rc != 0)
        return rc;

    cbx_overlay_surface_mark_dirty_all(&svc->surface);
    return 0;
}

/* ================================================================== */
/*  Host Mode slot change callback                                     */
/* ================================================================== */

static int
on_host_slot_change(int row_idx, int new_slot, void *userdata)
{
    return cbx_overlay_on_slot_change(row_idx, new_slot, userdata);
}

/* ================================================================== */
/*  Helper: fill cbx_grid_composite_info from the device model + DBus  */
/* ================================================================== */
static void fill_composite_info(cbx_grid_composite_info *info,
                                 const cbx_composite_entry *entry,
                                 const ip_dbus_backend *backend,
                                 ip_bus_handle bus)
{
    /* Path */
    snprintf(info->composite_path, sizeof(info->composite_path),
             "%s", entry->path);

    /* Persistent ID (best-effort) */
    char *id = NULL;
    if (ip_composite_get_persistent_id(backend, bus, entry->path, &id) == 0
        && id) {
        snprintf(info->id, sizeof(info->id), "%s", id);
        free(id);
    } else {
        snprintf(info->id, sizeof(info->id), "composite-%d", entry->index);
    }

    /* Model name (best-effort) */
    char *name = NULL;
    if (ip_composite_get_name(backend, bus, entry->path, &name) == 0
        && name) {
        snprintf(info->model_name, sizeof(info->model_name), "%s", name);
        free(name);
    } else {
        snprintf(info->model_name, sizeof(info->model_name),
                 "Controller %d", entry->index);
    }
}

/* ================================================================== */
/*  Helper: set InterceptMode = PASS on all composites                 */
/* ================================================================== */
static void set_all_pass(const ip_dbus_backend *backend, ip_bus_handle bus,
                          const cbx_composite_entry *composites, int count)
{
    for (int i = 0; i < count; i++)
        ip_composite_set_intercept_mode(backend, bus,
                                         composites[i].path, "1");
}

static uint64_t
reconcile_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static const char *
reconcile_error_category(int rc)
{
    switch (rc) {
    case IP_ERR_SERVICE_UNKNOWN: return "service-unknown";
    case IP_ERR_ACCESS_DENIED: return "access-denied";
    case IP_ERR_NO_REPLY: return "no-reply/deadline-expired";
    case IP_ERR_INVALID_ARGS: return "invalid-arguments";
    case IP_ERR_NOT_CONNECTED: return "not-connected";
    case -ENODEV: return "missing-composite";
    case -EPROTOTYPE: return "wrong-device-type";
    default: return "dbus/internal";
    }
}

static void
reconcile_diag(cbx_overlay_service_ctx *svc, const char *phase,
               const char *operation, const char *kind, const char *path,
               int old_count, int new_count, int rc, uint64_t started,
               bool expired)
{
    snprintf(svc->reconcile_status.phase, sizeof(svc->reconcile_status.phase),
             "%s", phase ? phase : "unknown");
    snprintf(svc->reconcile_status.operation,
             sizeof(svc->reconcile_status.operation), "%s",
             operation ? operation : "unknown");
    snprintf(svc->reconcile_status.target_kind,
             sizeof(svc->reconcile_status.target_kind), "%s", kind ? kind : "");
    snprintf(svc->reconcile_status.target_path,
             sizeof(svc->reconcile_status.target_path), "%s", path ? path : "");
    svc->reconcile_status.old_count = old_count;
    svc->reconcile_status.new_count = new_count;
    svc->reconcile_status.rc = rc;
    svc->reconcile_status.elapsed_ms = (uint32_t)(reconcile_now_ms() - started);
    svc->reconcile_status.deadline_ms = svc->reconcile_timeout_ms ?
        svc->reconcile_timeout_ms : CBX_RECONCILE_TIMEOUT_MS;
    svc->reconcile_status.deadline_expired = expired;
    snprintf(svc->reconcile_status.detail,
             sizeof(svc->reconcile_status.detail),
             "phase=%.16s op=%.24s kind=%.16s path=%.48s count=%d->%d elapsed=%ums deadline=%ums expired=%.3s failure=%.25s rc=%d",
             svc->reconcile_status.phase, svc->reconcile_status.operation,
             svc->reconcile_status.target_kind,
             svc->reconcile_status.target_path, old_count, new_count,
             svc->reconcile_status.elapsed_ms,
             svc->reconcile_status.deadline_ms, expired ? "yes" : "no",
             reconcile_error_category(rc), rc);
}

static int
csv_exact_path_count(const char *csv, const char *path)
{
    if (!csv || !path)
        return 0;
    int count = 0;
    size_t plen = strlen(path);
    for (const char *p = csv; *p;) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        const char *end = strchr(p, ',');
        if (!end) end = p + strlen(p);
        while (end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;
        if ((size_t)(end - p) == plen && memcmp(p, path, plen) == 0)
            count++;
        p = *end ? end + 1 : end;
    }
    return count;
}

static bool
csv_is_exact_singleton(const char *csv, const char *path)
{
    int tokens = 0;
    if (!csv) return false;
    for (const char *p = csv; *p;) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        tokens++;
        const char *end = strchr(p, ',');
        p = end ? end + 1 : p + strlen(p);
    }
    return tokens == 1 && csv_exact_path_count(csv, path) == 1;
}

/* Resolve a persisted physical assignment by stable composite PersistentId.
 * Virtual slots do not imply physical composites: an absent assignment is a
 * valid ready-but-unassigned slot. */
static const char *
assigned_composite_for_slot(cbx_overlay_service_ctx *svc, int slot)
{
    for (int ai = 0; ai < svc->assignments.assignment_count; ai++) {
        const cbx_assignment *a = &svc->assignments.assignments[ai];
        if (a->slot != slot) continue;
        for (int ci = 0; ci < svc->model.composite_count; ci++) {
            char *id = NULL;
            int rc = ip_composite_get_persistent_id(svc->conn.backend,
                svc->conn.bus, svc->model.composites[ci].path, &id);
            bool match = rc == 0 && id && strcmp(id, a->id) == 0;
            free(id);
            if (match) return svc->model.composites[ci].path;
        }
    }
    return NULL;
}

static int
reconcile_enumerate(cbx_overlay_service_ctx *svc, cbx_device_model *out)
{
    cbx_device_model next;
    int rc = cbx_objectmanager_enumerate(svc->conn.backend, svc->conn.bus,
                                          &next);
    if (rc == 0) {
        svc->model = next;
        if (out) *out = next;
    }
    return rc;
}

static int
reconcile_order_targets(cbx_device_model *model,
                        char slots[][CBX_MAX_PATH_LEN], int count)
{
    cbx_device_entry ordered[CBX_MAX_DEVICES];
    bool used[CBX_MAX_DEVICES] = {false};
    int out = 0;
    for (int slot = 0; slot < count; slot++) {
        int found = -1;
        for (int i = 0; i < model->target_count; i++)
            if (!used[i] && strcmp(model->targets[i].path, slots[slot]) == 0) {
                found = i; break;
            }
        if (found < 0)
            return -ENOENT;
        ordered[out++] = model->targets[found];
        used[found] = true;
    }
    for (int i = 0; i < model->target_count; i++)
        if (!used[i]) ordered[out++] = model->targets[i];
    memcpy(model->targets, ordered,
           (size_t)model->target_count * sizeof(model->targets[0]));
    return 0;
}

static int
wait_for_exact_target(cbx_overlay_service_ctx *svc, const char *path,
                      const char *kind, bool present)
{
    uint32_t timeout = svc->reconcile_timeout_ms ? svc->reconcile_timeout_ms :
        CBX_RECONCILE_TIMEOUT_MS;
    uint32_t poll = svc->reconcile_poll_ms ? svc->reconcile_poll_ms :
        CBX_RECONCILE_POLL_MS;
    uint64_t deadline = reconcile_now_ms() + timeout;
    int last_rc = 0;

    for (;;) {
        cbx_device_model model;
        last_rc = reconcile_enumerate(svc, &model);
        if (last_rc == 0) {
            bool found = cbx_device_model_find_target(&model, path) != NULL;
            if (found == present) {
                if (!present)
                    return 0;
                char *actual = NULL;
                last_rc = ip_target_get_device_type(svc->conn.backend,
                    svc->conn.bus, path, &actual);
                if (last_rc == 0 && actual && strcmp(actual, kind) == 0) {
                    free(actual);
                    return 0;
                }
                if (last_rc == 0)
                    last_rc = -EPROTOTYPE;
                free(actual);
                if (last_rc == -EPROTOTYPE)
                    return last_rc;
            }
        }
        if (reconcile_now_ms() >= deadline)
            return -ETIMEDOUT;
        /* Dispatch ObjectManager traffic between bounded polls; sleeping is
         * only a short backoff, never the sole progress mechanism. */
        if (svc->conn.backend->process)
            svc->conn.backend->process(svc->conn.bus);
        SDL_Delay(poll);
    }
}

static int
wait_for_attachment(cbx_overlay_service_ctx *svc, const char *composite,
                    const char *target)
{
    uint32_t timeout = svc->reconcile_timeout_ms ? svc->reconcile_timeout_ms :
        CBX_RECONCILE_TIMEOUT_MS;
    uint32_t poll = svc->reconcile_poll_ms ? svc->reconcile_poll_ms :
        CBX_RECONCILE_POLL_MS;
    uint64_t deadline = reconcile_now_ms() + timeout;
    int last_rc = 0;
    for (;;) {
        char *paths = NULL;
        last_rc = ip_composite_get_target_devices(svc->conn.backend,
                                                    svc->conn.bus,
                                                    composite, &paths);
        /* Assignment is accepted only when the selected target is the exact
         * singleton set.  This detects stale/cross-slot targets after a
         * reassignment instead of treating a substring match as routing. */
        bool found = last_rc == 0 && csv_is_exact_singleton(paths, target);
        free(paths);
        if (found)
            return 0;
        if (reconcile_now_ms() >= deadline)
            return -ETIMEDOUT;
        if (svc->conn.backend->process)
            svc->conn.backend->process(svc->conn.bus);
        SDL_Delay(poll);
    }
}

int
cbx_reconcile_startup_targets(cbx_overlay_service_ctx *svc)
{
    if (!svc || !svc->conn.backend || !svc->conn.bus)
        return -EINVAL;
    int desired = svc->settings.virtual_controllers.count;
    if (desired < 0 || desired > CBX_MAX_CONTROLLERS)
        return -EINVAL;

    memset(&svc->reconcile_status, 0, sizeof(svc->reconcile_status));
    uint64_t started = reconcile_now_ms();
    int orig_count = svc->model.target_count;
    char slots[CBX_MAX_DEVICES][CBX_MAX_PATH_LEN] = {{0}};
    char created[CBX_MAX_CONTROLLERS][CBX_MAX_PATH_LEN] = {{0}};
    int created_count = 0;
    for (int i = 0; i < orig_count; i++)
        snprintf(slots[i], sizeof(slots[i]), "%s", svc->model.targets[i].path);

    int rc = 0;
    const char *phase = "enumeration";
    const char *operation = "validate-topology";
    const char *kind = "";
    char path[CBX_MAX_PATH_LEN] = "";

    /* Targets are persistent virtual player slots, independent of physical
     * composite cardinality.  Zero composites is a normal ready topology;
     * only persisted, identity-matched assignments are attached below. */

    /* Grow using retained return paths, never count deltas or reply order. */
    for (int slot = orig_count; slot < desired; slot++) {
        kind = svc->settings.virtual_controllers.types[slot][0] ?
            svc->settings.virtual_controllers.types[slot] : "xb360";
        phase = "create"; operation = "CreateTargetDevice";
        char *returned = NULL;
        rc = ip_manager_create_target_device(svc->conn.backend, svc->conn.bus,
                                               kind, &returned);
        if (rc != 0 || !returned || !returned[0]) {
            if (rc == 0) rc = -EIO;
            free(returned);
            goto fail;
        }
        snprintf(created[created_count], sizeof(created[created_count]), "%s",
                 returned);
        snprintf(slots[slot], sizeof(slots[slot]), "%s", returned);
        snprintf(path, sizeof(path), "%s", created[created_count++]);
        free(returned);
        phase = "publication-timeout"; operation = "confirm-created-path";
        rc = wait_for_exact_target(svc, path, kind, true);
        if (rc != 0)
            goto fail;
    }

    /* Correct types without destroying the old slot until its replacement
     * is published and attached.  A later failure truthfully records any
     * original that has already been stopped. */
    int existing_slots = orig_count < desired ? orig_count : desired;
    for (int slot = 0; slot < existing_slots; slot++) {
        kind = svc->settings.virtual_controllers.types[slot][0] ?
            svc->settings.virtual_controllers.types[slot] : "xb360";
        memcpy(path, slots[slot], sizeof(path));
        path[sizeof(path) - 1] = '\0';
        char *actual = NULL;
        phase = "type-correction"; operation = "read-DeviceType";
        rc = ip_target_get_device_type(svc->conn.backend, svc->conn.bus,
                                        path, &actual);
        if (rc != 0) { free(actual); goto fail; }
        bool matches = actual && strcmp(actual, kind) == 0;
        free(actual);
        if (matches) continue;

        char old_path[CBX_MAX_PATH_LEN];
        snprintf(old_path, sizeof(old_path), "%s", path);
        char *returned = NULL;
        operation = "CreateTargetDevice";
        rc = ip_manager_create_target_device(svc->conn.backend, svc->conn.bus,
                                               kind, &returned);
        if (rc != 0 || !returned || !returned[0]) {
            if (rc == 0) rc = -EIO;
            free(returned); goto fail;
        }
        snprintf(created[created_count], sizeof(created[created_count]), "%s",
                 returned);
        snprintf(slots[slot], sizeof(slots[slot]), "%s", returned);
        snprintf(path, sizeof(path), "%s", created[created_count++]);
        free(returned);
        operation = "confirm-replacement-publication";
        rc = wait_for_exact_target(svc, path, kind, true);
        if (rc != 0) goto fail;
        const char *assigned = assigned_composite_for_slot(svc, slot);
        if (assigned) {
            phase = "attachment"; operation = "AttachTargetDevice";
            rc = ip_manager_attach_target_device(svc->conn.backend, svc->conn.bus,
                path, assigned);
            if (rc != 0) goto fail;
            operation = "verify-exact-TargetDevices";
            rc = wait_for_attachment(svc, assigned, path);
            if (rc != 0) goto fail;
        }
        phase = "type-correction"; operation = "StopTargetDevice";
        snprintf(path, sizeof(path), "%s", old_path);
        rc = ip_manager_stop_target_device(svc->conn.backend, svc->conn.bus,
                                             old_path);
        if (rc != 0) goto fail;
        svc->reconcile_status.originals_stopped = true;
        operation = "confirm-old-path-removal";
        rc = wait_for_exact_target(svc, old_path, kind, false);
        if (rc != 0) goto fail;
    }

    /* Attach only explicitly persisted physical assignments.  Unassigned
     * virtual slots remain created and ready without a composite. */
    for (int slot = 0; slot < desired; slot++) {
        const char *assigned = assigned_composite_for_slot(svc, slot);
        if (!assigned) continue;
        memcpy(path, slots[slot], sizeof(path));
        path[sizeof(path) - 1] = '\0';
        phase = "attachment"; operation = "AttachTargetDevice";
        rc = ip_manager_attach_target_device(svc->conn.backend, svc->conn.bus,
            path, assigned);
        if (rc != 0) goto fail;
        operation = "verify-exact-TargetDevices";
        rc = wait_for_attachment(svc, assigned, path);
        if (rc != 0) goto fail;
    }

    /* Destructive shrink is last; stopped originals are not called restored. */
    for (int slot = orig_count - 1; slot >= desired; slot--) {
        memcpy(path, slots[slot], sizeof(path));
        path[sizeof(path) - 1] = '\0';
        phase = "stop-removal-timeout";
        operation = "StopTargetDevice";
        rc = ip_manager_stop_target_device(svc->conn.backend, svc->conn.bus,
                                             path);
        if (rc != 0) goto fail;
        svc->reconcile_status.originals_stopped = true;
        operation = "confirm-stopped-path";
        rc = wait_for_exact_target(svc, path, "", false);
        if (rc != 0) goto fail;
    }

    rc = reconcile_enumerate(svc, NULL);
    if (rc != 0) { phase = "enumeration"; operation = "final-enumeration"; goto fail; }
    if (svc->model.target_count != desired) {
        rc = -EIO; phase = "enumeration"; operation = "final-cardinality";
        goto fail;
    }
    rc = reconcile_order_targets(&svc->model, slots, desired);
    if (rc != 0) {
        phase = "enumeration"; operation = "final-slot-identity";
        goto fail;
    }
    reconcile_diag(svc, "complete", "reconcile", "", "", orig_count,
                   svc->model.target_count, 0, started, false);
    return 0;

fail: {
        int primary_rc = rc;
        bool expired = rc == -ETIMEDOUT || rc == IP_ERR_NO_REPLY;
        reconcile_diag(svc, phase, operation, kind, path, orig_count,
                       svc->model.target_count, primary_rc, started, expired);
        char primary_detail[sizeof(svc->reconcile_status.detail)];
        snprintf(primary_detail, sizeof(primary_detail), "%s",
                 svc->reconcile_status.detail);

        /* Created paths are authoritative even if publication was delayed or
         * enumeration failed.  Attempt every cleanup independently. */
        for (int i = created_count - 1; i >= 0; i--) {
            int cleanup_rc = ip_manager_stop_target_device(svc->conn.backend,
                svc->conn.bus, created[i]);
            if (cleanup_rc == 0)
                cleanup_rc = wait_for_exact_target(svc, created[i], "", false);
            if (cleanup_rc != 0) {
                svc->reconcile_status.cleanup_failures++;
                fprintf(stderr,
                    "controller-box: reconcile rollback cleanup failed path=%s failure=%s rc=%d\n",
                    created[i], reconcile_error_category(cleanup_rc), cleanup_rc);
            }
        }
        reconcile_enumerate(svc, NULL);
        snprintf(svc->reconcile_status.detail,
                 sizeof(svc->reconcile_status.detail),
                 "%.*s cleanup_failures=%d originals_stopped=%s",
                 190, primary_detail, svc->reconcile_status.cleanup_failures,
                 svc->reconcile_status.originals_stopped ? "yes" : "no");
        fprintf(stderr, "controller-box: virtual-controller reconcile failed: %s\n",
                svc->reconcile_status.detail);
        return primary_rc;
    }
}

/* ================================================================== */
/*  Hotplug reconciliation (Task 9)                                     */
/* ================================================================== */

/*
 * Helper: (re)initialize all intercept polls for the current composites.
 * Stops any existing polls first, then creates one per composite with
 * per-composite activation context so close sets PASS on the correct
 * composite (SPEC §2.5).
 */
void
cbx_overlay_rearm_polls(cbx_overlay_service_ctx *svc)
{
    for (int i = 0; i < svc->poll_count; i++)
        ip_intercept_poll_stop(&svc->polls[i]);
    svc->poll_count = 0;

    if (svc->poll_event_type == (uint32_t)-1)
        return;

    for (int i = 0; i < svc->comp_count && i < CBX_MAX_COMPOSITES; i++) {
        svc->poll_acts[i].lifecycle = &svc->lifecycle;
        snprintf(svc->poll_acts[i].composite_path,
                 sizeof(svc->poll_acts[i].composite_path),
                 "%s", svc->composites[i].composite_path);
        ip_intercept_poll_init(&svc->polls[i],
                                svc->conn.backend, svc->conn.bus,
                                svc->composites[i].composite_path,
                                on_intercept_activating, &svc->poll_acts[i],
                                on_intercept_deactivating, &svc->lifecycle,
                                on_intercept_error, NULL);
        if (ip_intercept_poll_start(&svc->polls[i],
                                     IP_INTERCEPT_POLL_INTERVAL_MS,
                                     svc->poll_event_type) == 0)
            svc->poll_count++;
    }
}

/*
 * Reconcile the overlay state after a hotplug event modifies the device
 * model (SPEC §10.1: InterfacesAdded/InterfacesRemoved).  Re-enumerates,
 * rebuilds grid rows/columns, input mappings, triggers, and polls.
 *
 * If the overlay is visible, the grid is rebuilt with dynamic columns
 * preserving profiles.  If idle, a full rebuild from assignments is safe.
 */
static void
cbx_overlay_reconcile_hotplug(cbx_overlay_service_ctx *svc)
{
    if (!svc || !svc->conn.backend || !svc->conn.bus)
        return;

    /* The device model was already updated incrementally by ip_hotplug
     * (SPEC §10.1: InterfacesAdded/InterfacesRemoved).  No full
     * re-enumeration needed — use the current model state directly. */

    /* Update composite info from the device model. */
    int new_comp_count = svc->model.composite_count;
    if (new_comp_count > CBX_MAX_COMPOSITES)
        new_comp_count = CBX_MAX_COMPOSITES;

    /* Save current profiles before grid rebuild. */
    char saved_profiles[CBX_GRID_MAX_PROFILES][CBX_GRID_PROFILE_LEN];
    int saved_profile_count = svc->grid.profile_count;
    if (saved_profile_count > CBX_GRID_MAX_PROFILES)
        saved_profile_count = CBX_GRID_MAX_PROFILES;
    for (int i = 0; i < saved_profile_count; i++)
        snprintf(saved_profiles[i], sizeof(saved_profiles[i]),
                 "%s", svc->grid.profiles[i]);

    /* Update composite info. */
    svc->comp_count = new_comp_count;
    for (int i = 0; i < new_comp_count; i++)
        fill_composite_info(&svc->composites[i], &svc->model.composites[i],
                             svc->conn.backend, svc->conn.bus);

    /* Check if target count changed → rebuild with dynamic columns
     * (SPEC §4.7).  Otherwise just rebuild the grid rows. */
    if (cbx_dynamic_columns_needs_rebuild(&svc->grid,
                                             svc->model.target_count)) {
        char target_types[CBX_MAX_CONTROLLERS][CBX_MAX_TYPE_LEN];
        int target_count = svc->model.target_count;
        if (target_count > CBX_MAX_CONTROLLERS)
            target_count = CBX_MAX_CONTROLLERS;
        for (int i = 0; i < target_count; i++) {
            char *dtype = NULL;
            if (ip_target_get_device_type(svc->conn.backend,
                                            svc->conn.bus,
                                            svc->model.targets[i].path,
                                            &dtype) == 0 && dtype) {
                snprintf(target_types[i], CBX_MAX_TYPE_LEN, "%s", dtype);
                free(dtype);
            } else {
                snprintf(target_types[i], CBX_MAX_TYPE_LEN, "xb360");
            }
        }
        cbx_dynamic_columns_rebuild(&svc->grid,
                                      (const char (*)[CBX_MAX_TYPE_LEN])target_types,
                                      target_count, svc->composites,
                                      new_comp_count, &svc->assignments);
    } else {
        cbx_select_grid_build(&svc->grid, svc->composites,
                               new_comp_count, &svc->settings,
                               &svc->assignments);
    }

    /* Restore profiles onto the rebuilt grid. */
    cbx_profile_cycle_load_profiles(&svc->grid, &svc->profiles);

    /* Re-detect conflicts after grid rebuild (SPEC §4.5). */
    cbx_conflict_list_init(&svc->conflicts);
    cbx_conflict_detect(&svc->grid, &svc->conflicts);

    /* Rebuild input map. */
    cbx_overlay_input_build_map(svc->conn.backend, svc->conn.bus,
                                 svc->composites, svc->comp_count,
                                 &svc->input_ctx);

    /* Re-register triggers on all composites and set PASS (SPEC §2.5). */
    set_all_pass(svc->conn.backend, svc->conn.bus,
                  svc->model.composites, svc->comp_count);
    for (int i = 0; i < svc->comp_count; i++)
        cbx_trigger_register(svc->conn.backend, svc->conn.bus,
                              svc->composites[i].composite_path,
                              svc->settings.overlay_trigger);

    /* Re-arm polls for current composites (SPEC §2.5, §10.1). */
    cbx_overlay_rearm_polls(svc);

    /* Mark surface dirty for re-render. */
    cbx_overlay_surface_mark_dirty_all(&svc->surface);
}

static void
overlay_backend_degraded(const char *reason, void *userdata)
{
    cbx_overlay_service_ctx *svc = userdata;
    if (!svc)
        return;
    fprintf(stderr, "controller-box: %s; waiting for recovery\n",
            reason ? reason : "InputPlumber unavailable");
    svc->backend_ready = false;
    if (svc->initialized)
        cbx_overlay_lifecycle_force_close(&svc->lifecycle);
    SDL_HideWindow(svc->rend.window);
}

static void
overlay_backend_ready(void *userdata)
{
    cbx_overlay_service_ctx *svc = userdata;
    if (!svc || !svc->initialized)
        return;

    for (int i = 0; i < svc->poll_count; i++)
        ip_intercept_poll_stop(&svc->polls[i]);
    svc->poll_count = 0;

    if (cbx_objectmanager_enumerate(svc->conn.backend, svc->conn.bus,
                                     &svc->model) != 0) {
        overlay_backend_degraded("InputPlumber enumeration failed", svc);
        return;
    }
    if (cbx_reconcile_startup_targets(svc) != 0) {
        overlay_backend_degraded(svc->reconcile_status.detail[0] ?
            svc->reconcile_status.detail :
            "InputPlumber virtual-controller reconciliation failed", svc);
        return;
    }

    svc->comp_count = svc->model.composite_count;
    if (svc->comp_count > CBX_MAX_COMPOSITES)
        svc->comp_count = CBX_MAX_COMPOSITES;
    for (int i = 0; i < svc->comp_count; i++)
        fill_composite_info(&svc->composites[i], &svc->model.composites[i],
                             svc->conn.backend, svc->conn.bus);
    cbx_select_grid_build(&svc->grid, svc->composites, svc->comp_count,
                           &svc->settings, &svc->assignments);
    cbx_profile_cycle_load_profiles(&svc->grid, &svc->profiles);

    /* Re-detect conflicts after grid rebuild (SPEC §4.5). */
    cbx_conflict_list_init(&svc->conflicts);
    cbx_conflict_detect(&svc->grid, &svc->conflicts);

    /* Re-initialise profile_cycle with current backend/bus pointers
     * so LoadProfilePath uses the live connection. */
    cbx_profile_cycle_init(&svc->profile_cycle, svc->conn.backend,
                            svc->conn.bus, &svc->assignments, &svc->profiles);

    /* Restore persisted slot/profile topology to the live engine before
     * advertising the overlay as ready (SPEC §§4.1-4.7: startup/restart
     * restores order/profile).  This re-applies LoadProfilePath,
     * AttachTargetDevice, and SetGamepadOrder from the saved state. */
    if (cbx_overlay_on_save(svc) != 0)
        fprintf(stderr, "controller-box: failed to restore assignments after recovery\n");

    cbx_overlay_input_build_map(svc->conn.backend, svc->conn.bus,
                                 svc->composites, svc->comp_count,
                                 &svc->input_ctx);

    const char *uniq = ip_connection_get_unique_name(&svc->conn);
    snprintf(svc->expected_sender, sizeof(svc->expected_sender), "%s",
             uniq ? uniq : "");
    ip_input_events_init(&svc->input_events, svc->conn.backend, svc->conn.bus,
                          svc->expected_sender, cbx_overlay_input_cb,
                          &svc->input_ctx);
    svc->input_events_ready = ip_input_events_subscribe(&svc->input_events) == 0;

    set_all_pass(svc->conn.backend, svc->conn.bus,
                  svc->model.composites, svc->comp_count);
    for (int i = 0; i < svc->comp_count; i++)
        cbx_trigger_register(svc->conn.backend, svc->conn.bus,
                              svc->composites[i].composite_path,
                              svc->settings.overlay_trigger);

    cbx_overlay_rearm_polls(svc);

    /* Re-subscribe to hotplug signals for incremental device updates
     * (SPEC §10.1: InterfacesAdded/InterfacesRemoved). */
    ip_hotplug_init(&svc->hp, svc->conn.backend, svc->conn.bus,
                     svc->expected_sender, &svc->model);
    ip_hotplug_subscribe(&svc->hp);

    svc->backend_ready = true;
    cbx_overlay_surface_mark_dirty_all(&svc->surface);
}

/* ================================================================== */
/*  Helper: map SDL key to player_mode / host_mode input               */
/* ================================================================== */
static int
sdl_key_to_pm_input(SDL_Keycode key, cbx_pm_input *out)
{
    switch (key) {
    case SDLK_LEFT:   *out = CBX_PM_LEFT;  return 1;
    case SDLK_RIGHT:  *out = CBX_PM_RIGHT; return 1;
    case SDLK_UP:     *out = CBX_PM_UP;    return 1;
    case SDLK_DOWN:   *out = CBX_PM_DOWN;  return 1;
    case SDLK_b:      *out = CBX_PM_B;     return 1;
    case SDLK_r:      *out = CBX_PM_R3;    return 1;
    default:          return 0;
    }
}

static int
sdl_key_to_hm_input(SDL_Keycode key, cbx_hm_input *out)
{
    switch (key) {
    case SDLK_LEFT:   *out = CBX_HM_LEFT;  return 1;
    case SDLK_RIGHT:  *out = CBX_HM_RIGHT; return 1;
    case SDLK_UP:     *out = CBX_HM_UP;    return 1;
    case SDLK_DOWN:   *out = CBX_HM_DOWN;  return 1;
    case SDLK_b:      *out = CBX_HM_B;     return 1;
    case SDLK_r:      *out = CBX_HM_R3;    return 1;
    default:          return 0;
    }
}

/* ================================================================== */
/*  Overlay input event handling (Task 6)                              */
/*  Maps DBus InputEvent signals to grid rows and dispatches to        */
/*  player_mode or host_mode with the correct row index.               */
/* ================================================================== */

void
cbx_overlay_input_add_mapping(cbx_overlay_input_ctx *ctx,
                                const char *device_path,
                                int row_idx)
{
    if (!ctx || !device_path || ctx->path_count >= CBX_MAX_DBUS_DEVICES)
        return;
    snprintf(ctx->device_paths[ctx->path_count],
             sizeof(ctx->device_paths[ctx->path_count]),
             "%s", device_path);
    ctx->row_indices[ctx->path_count] = row_idx;
    ctx->path_count++;
}

int
cbx_overlay_input_find_row(const char *device_path,
                              char paths[][256],
                              const int *rows, int count)
{
    if (!device_path || !paths || !rows)
        return -1;
    for (int i = 0; i < count; i++) {
        if (strcmp(paths[i], device_path) == 0)
            return rows[i];
    }
    return -1;
}

int
cbx_overlay_input_build_map(const ip_dbus_backend *backend,
                              ip_bus_handle bus,
                              cbx_grid_composite_info *composites,
                              int comp_count,
                              cbx_overlay_input_ctx *ctx)
{
    if (!backend || !composites || !ctx)
        return -EINVAL;

    ctx->path_count = 0;

    for (int i = 0; i < comp_count; i++) {
        char *dbus_devices = NULL;
        int rc = ip_composite_get_dbus_devices(backend, bus,
                                                composites[i].composite_path,
                                                &dbus_devices);
        if (rc != 0 || !dbus_devices)
            continue;  /* best-effort */

        /* Parse comma-separated DBusDevice paths. */
        char *saveptr = NULL;
        char *token = strtok_r(dbus_devices, ",", &saveptr);
        while (token) {
            /* Trim leading whitespace. */
            while (*token == ' ')
                token++;
            if (*token)
                cbx_overlay_input_add_mapping(ctx, token, i);
            token = strtok_r(NULL, ",", &saveptr);
        }

        free(dbus_devices);
    }

    return 0;
}

int
cbx_ip_input_to_pm(ip_input_id input, cbx_pm_input *out)
{
    switch (input) {
    case IP_INPUT_LEFT:  *out = CBX_PM_LEFT;  return 1;
    case IP_INPUT_RIGHT: *out = CBX_PM_RIGHT; return 1;
    case IP_INPUT_UP:    *out = CBX_PM_UP;    return 1;
    case IP_INPUT_DOWN:  *out = CBX_PM_DOWN;  return 1;
    case IP_INPUT_B:     *out = CBX_PM_B;     return 1;
    case IP_INPUT_R3:    *out = CBX_PM_R3;    return 1;
    default:             return 0;
    }
}

int
cbx_ip_input_to_hm(ip_input_id input, cbx_hm_input *out)
{
    switch (input) {
    case IP_INPUT_LEFT:  *out = CBX_HM_LEFT;  return 1;
    case IP_INPUT_RIGHT: *out = CBX_HM_RIGHT; return 1;
    case IP_INPUT_UP:    *out = CBX_HM_UP;    return 1;
    case IP_INPUT_DOWN:  *out = CBX_HM_DOWN;  return 1;
    case IP_INPUT_B:     *out = CBX_HM_B;     return 1;
    case IP_INPUT_R3:    *out = CBX_HM_R3;    return 1;
    default:             return 0;
    }
}

void
cbx_overlay_input_cb(ip_input_id input,
                       ip_input_category category,
                       double value,
                       const char *raw_event,
                       const char *device_path,
                       void *userdata)
{
    (void)raw_event;
    cbx_overlay_input_ctx *ctx = (cbx_overlay_input_ctx *)userdata;
    if (!ctx || !ctx->pm || !ctx->hm || !ctx->grid || !ctx->lifecycle)
        return;

    /* Only process button press events (value == 1.0).
     * Button releases (0.0) and axis events are ignored — the overlay
     * only needs directional navigation. */
    if (category != IP_INPUT_CAT_BUTTON || value != 1.0)
        return;

    /* Map device_path→row.  Unknown device paths are dropped. */
    int row_idx = cbx_overlay_input_find_row(device_path,
                                               ctx->device_paths,
                                               ctx->row_indices,
                                               ctx->path_count);
    if (row_idx < 0 || row_idx >= ctx->grid->row_count)
        return;  /* unknown device path or out of bounds */

    if (cbx_host_mode_is_active(ctx->hm)) {
        /* Host Mode: host navigates rows + slots. */
        cbx_hm_input hm_in;
        if (!cbx_ip_input_to_hm(input, &hm_in))
            return;

        int host_row = cbx_host_mode_get_host_row(ctx->hm);
        int result = cbx_host_mode_handle(ctx->hm, host_row, hm_in,
                                            ctx->grid);
        if (result == CBX_HM_RESULT_EXIT) {
            cbx_host_mode_exit(ctx->hm);
        } else if (result == CBX_HM_RESULT_CLOSE) {
            cbx_overlay_lifecycle_close(ctx->lifecycle);
        } else if (result == CBX_HM_RESULT_MOVED ||
                   result == CBX_HM_RESULT_SLOT) {
            cbx_overlay_surface_mark_dirty_all(ctx->lifecycle->surface);
        }
    } else {
        /* Player Mode: each controller edits its own row. */
        cbx_pm_input pm_in;
        if (!cbx_ip_input_to_pm(input, &pm_in))
            return;

        int result = cbx_player_mode_handle(ctx->pm, row_idx, pm_in);
        if (result == CBX_PM_RESULT_CLOSE) {
            cbx_overlay_lifecycle_close(ctx->lifecycle);
        } else if (result == CBX_PM_RESULT_HOST) {
            cbx_host_mode_toggle(ctx->hm, row_idx);
        }
        /* Surface dirty flag is set by callbacks. */
    }
}

/* ================================================================== */
/*  Step function: process one iteration of the poll loop (Task 10)   */
/* ================================================================== */

void
cbx_overlay_service_step(cbx_overlay_service_ctx *svc)
{
    SDL_Event ev;

    /* 1. Process all pending SDL events. */
    while (SDL_PollEvent(&ev)) {
        if (ev.type == svc->poll_event_type) {
            /* InterceptMode poll timer fired — tick all polls. */
            for (int i = 0; i < svc->poll_count; i++)
                ip_intercept_poll_tick(&svc->polls[i]);
        } else if (ev.type == SDL_QUIT) {
            g_running = 0;
        } else if (ev.type == SDL_KEYDOWN &&
                   cbx_overlay_lifecycle_is_active(&svc->lifecycle)) {
            /* Process input only when overlay is visible. */
            SDL_Keycode key = ev.key.keysym.sym;

            if (cbx_host_mode_is_active(&svc->hm)) {
                /* Host Mode: host navigates rows + slots. */
                cbx_hm_input hm_in;
                if (sdl_key_to_hm_input(key, &hm_in)) {
                    int host_row = cbx_host_mode_get_host_row(&svc->hm);
                    int result = cbx_host_mode_handle(
                        &svc->hm, host_row, hm_in, &svc->grid);

                    if (result == CBX_HM_RESULT_EXIT) {
                        cbx_host_mode_exit(&svc->hm);
                    } else if (result == CBX_HM_RESULT_CLOSE) {
                        cbx_overlay_lifecycle_close(&svc->lifecycle);
                    } else if (result == CBX_HM_RESULT_MOVED ||
                               result == CBX_HM_RESULT_SLOT) {
                        cbx_overlay_surface_mark_dirty_all(&svc->surface);
                    }
                }
            } else {
                /* Player Mode: each controller edits its own row. */
                cbx_pm_input pm_in;
                if (sdl_key_to_pm_input(key, &pm_in)) {
                    /* Use row 0 (primary controller) for keyboard
                     * input.  In production, DBus InputEvent signals
                     * carry the device path, which maps to a row. */
                    int result = cbx_player_mode_handle(
                        &svc->pm, 0, pm_in);

                    if (result == CBX_PM_RESULT_CLOSE) {
                        cbx_overlay_lifecycle_close(&svc->lifecycle);
                    } else if (result == CBX_PM_RESULT_HOST) {
                        cbx_host_mode_toggle(&svc->hm, 0);
                    }
                    /* Surface dirty flag is set by callbacks. */
                }
            }
        }
    }

    /* 2. Process pending DBus messages unconditionally so NameOwnerChanged
     *    signals are drained even in degraded mode (when input_events_ready
     *    is false).  This is the recovery path: without this drain,
     *    NameOwnerChanged for InputPlumber's bus name would never be
     *    dispatched and the service could not recover from degraded mode. */
    if (svc->conn.backend && svc->conn.bus &&
        svc->conn.backend->process) {
        for (int i = 0; i < 64; i++) {
            int processed = svc->conn.backend->process(svc->conn.bus);
            if (processed <= 0)
                break;
        }
    }

    /* 3. Process pending DBus InputEvent signals (only if subscribed). */
    if (svc->input_events_ready)
        ip_input_events_process(&svc->input_events);

    /* 4. Check if hotplug signals modified the device model and reconcile
     *    grid rows, columns, input mappings, triggers, and polls
     *    (SPEC §10.1: InterfacesAdded/InterfacesRemoved). */
    if (svc->hp.model_changed) {
        svc->hp.model_changed = false;
        cbx_overlay_reconcile_hotplug(svc);
    }

    /* 5. Advance lifecycle (fade animation, timeout). */
    cbx_overlay_lifecycle_tick(&svc->lifecycle);

    /* The production overlay window is created hidden.  Mapping is a
     * lifecycle outcome, not a framebuffer side effect: presenting to a
     * hidden window is invisible even though off-screen tests are green. */
    if (cbx_overlay_lifecycle_is_active(&svc->lifecycle))
        cbx_renderer_show(&svc->rend);
    else
        cbx_renderer_hide(&svc->rend);

    /* 6. Re-arm IDLE polls when the overlay is idle so the next
     *    activation can be detected (SPEC §2.5: close permits later
     *    activations).  After close, the activating composite's poll
     *    transitions to IDLE; without re-arming it the overlay can
     *    only be activated once. */
    if (svc->backend_ready && !cbx_overlay_lifecycle_is_active(&svc->lifecycle)) {
        for (int i = 0; i < svc->poll_count; i++) {
            if (svc->polls[i].state == IP_POLL_IDLE)
                ip_intercept_poll_start(&svc->polls[i],
                                         IP_INTERCEPT_POLL_INTERVAL_MS,
                                         svc->poll_event_type);
        }
    }

    /* 7. Re-render dirty surface (when overlay is active). */
    if (cbx_overlay_lifecycle_is_active(&svc->lifecycle) &&
        cbx_overlay_surface_is_dirty(&svc->surface)) {
        cbx_overlay_surface_render(&svc->surface, svc->rend.renderer,
                                    cbx_select_grid_render_cb,
                                    &svc->render_ctx);
        cbx_overlay_surface_show(&svc->surface, svc->rend.renderer);
    }
}

/* ================================================================== */
/*  Main entry                                                         */
/* ================================================================== */
int run_overlay_service(int dry_run)
{
    printf("controller-box: overlay-service mode%s\n",
           dry_run ? " (dry-run)" : "");
    if (dry_run)
        return 0;

    /* --- 0. Install signal handlers ------------------------------- */
    g_running = 1;
    if (cbx_overlay_service_install_signal_handlers() != 0) {
        fprintf(stderr,
                "controller-box: warning: signal handler setup failed\n");
        /* Continue anyway — the service still works, just no clean
         * shutdown on SIGTERM. */
    }

    /* Allocate the service context on the heap — it contains large
     * arrays (device model, text cache, icon cache) that together
     * exceed 200 KB. */
    cbx_overlay_service_ctx *svc = calloc(1, sizeof(*svc));
    if (!svc) {
        fprintf(stderr, "controller-box: out of memory\n");
        return 1;
    }

    int rc;

    /* --- 1. SDL video + hidden renderer ----------------------------- */
    rc = cbx_renderer_init(&svc->rend, "Controller-Box Overlay",
                               CBX_RENDERER_DEFAULT_W,
                               CBX_RENDERER_DEFAULT_H, false);
    if (rc != 0) {
        fprintf(stderr,
                "controller-box: failed to initialise SDL renderer: %s\n",
                SDL_GetError());
        free(svc);
        return 1;
    }

    /* --- 2. Connect to InputPlumber via system DBus ------------------ */
    ip_connection_init(&svc->conn, ip_dbus_sd_backend());
    rc = ip_connection_connect(&svc->conn);
    int conn_rc = rc;  /* save for degraded reason at end of init */
    if (!svc->conn.bus) {
        fprintf(stderr, "controller-box: system DBus unavailable: %d\n", rc);
        ip_connection_disconnect(&svc->conn);
        cbx_renderer_shutdown(&svc->rend);
        free(svc);
        return 1;
    }
    svc->backend_ready = ip_connection_is_connected(&svc->conn);

    /* Keep the service alive in degraded mode so NameOwnerChanged can
     * recover it without depending on a system/user unit relationship. */
    cbx_device_model_init(&svc->model);
    if (svc->backend_ready) {
        rc = cbx_objectmanager_enumerate(svc->conn.backend, svc->conn.bus,
                                          &svc->model);
        if (rc != 0) {
            fprintf(stderr,
                    "controller-box: failed to enumerate devices: %d\n", rc);
            svc->backend_ready = false;
        }
    }

    /* --- 4. Load settings + assignments (best-effort) ---------------- */
    cbx_settings_defaults(&svc->settings);
    cbx_settings_load(&svc->settings);   /* best-effort — defaults are OK */

    cbx_assignments_init(&svc->assignments);
    cbx_assignments_load(&svc->assignments);  /* best-effort */

    rc = svc->backend_ready ? cbx_reconcile_startup_targets(svc) : 0;
    if (rc != 0) {
        fprintf(stderr,
                "controller-box: failed to reconcile virtual controllers: %d\n",
                rc);
        ip_connection_disconnect(&svc->conn);
        cbx_renderer_shutdown(&svc->rend);
        free(svc);
        return 1;
    }

    /* --- 5. Text cache, theme, icon cache ---------------------------- */
    cbx_text_cache_init(&svc->text_cache, svc->rend.renderer);
    svc->font_id = -1;
    const char *font_path = cbx_font_path();
    if (font_path)
        svc->font_id = cbx_text_load_font(&svc->text_cache, font_path, 16);

    cbx_theme_default(&svc->theme);

    cbx_icon_map_init(&svc->icon_map);
    char icon_map_path[512];
    if (cbx_icon_map_default_path(icon_map_path, sizeof(icon_map_path)) == 0)
        cbx_icon_map_load(&svc->icon_map, icon_map_path);  /* best-effort */

    cbx_icon_cache_init(&svc->icon_cache, svc->rend.renderer,
                         cbx_icon_dir(), 48);
    cbx_icon_cache_load(&svc->icon_cache, &svc->icon_map);  /* best-effort */

    /* --- 6. Build the selection grid --------------------------------- */
    svc->comp_count = svc->model.composite_count;
    if (svc->comp_count > CBX_MAX_COMPOSITES)
        svc->comp_count = CBX_MAX_COMPOSITES;
    for (int i = 0; i < svc->comp_count; i++)
        fill_composite_info(&svc->composites[i], &svc->model.composites[i],
                             svc->conn.backend, svc->conn.bus);

    cbx_select_grid_init(&svc->grid);
    cbx_select_grid_build(&svc->grid, svc->composites, svc->comp_count,
                           &svc->settings, &svc->assignments);
    /* Detect conflicts for initial render (SPEC §4.5). */
    cbx_conflict_list_init(&svc->conflicts);
    cbx_conflict_detect(&svc->grid, &svc->conflicts);
    rc = cbx_profile_list_enumerate(&svc->profiles);
    if (rc != 0 || svc->profiles.count == 0) {
        fprintf(stderr, "controller-box: no usable profiles available: %d\n", rc);
        ip_connection_disconnect(&svc->conn);
        cbx_renderer_shutdown(&svc->rend);
        free(svc);
        return 1;
    }
    cbx_profile_cycle_load_profiles(&svc->grid, &svc->profiles);
    cbx_profile_cycle_init(&svc->profile_cycle, svc->conn.backend,
                            svc->conn.bus, &svc->assignments, &svc->profiles);

    /* --- 7. Initialise and pre-render the overlay surface ------------ */
    rc = cbx_overlay_surface_init(&svc->surface, svc->rend.renderer,
                                   CBX_RENDERER_DEFAULT_W,
                                   CBX_RENDERER_DEFAULT_H,
                                   svc->settings.overlay_opacity);
    if (rc != 0) {
        fprintf(stderr,
                "controller-box: failed to create overlay surface: %d\n",
                rc);
        cbx_icon_cache_cleanup(&svc->icon_cache);
        cbx_text_cache_cleanup(&svc->text_cache);
        ip_connection_disconnect(&svc->conn);
        cbx_renderer_shutdown(&svc->rend);
        free(svc);
        return 1;
    }

    svc->render_ctx = (cbx_grid_render_ctx){
        .grid       = &svc->grid,
        .icon_cache = &svc->icon_cache,
        .icon_map   = &svc->icon_map,
        .theme      = &svc->theme,
        .text_cache = svc->font_id >= 0 ? &svc->text_cache : NULL,
        .font_id    = svc->font_id,
        .settings   = &svc->settings,
        .conflicts  = &svc->conflicts,
        .hm         = &svc->hm,
    };
    cbx_overlay_surface_mark_dirty_all(&svc->surface);
    cbx_overlay_surface_render(&svc->surface, svc->rend.renderer,
                                cbx_select_grid_render_cb, &svc->render_ctx);

    /* Restore persisted slot/profile topology to the live engine before
     * advertising the overlay as ready. */
    if (svc->backend_ready) {
        rc = cbx_overlay_on_save(svc);
        if (rc != 0) {
            fprintf(stderr, "controller-box: failed to restore assignments: %d\n",
                    rc);
            cbx_overlay_surface_destroy(&svc->surface);
            cbx_icon_cache_cleanup(&svc->icon_cache);
            cbx_text_cache_cleanup(&svc->text_cache);
            ip_connection_disconnect(&svc->conn);
            cbx_renderer_shutdown(&svc->rend);
            free(svc);
            return 1;
        }
    }

    /* --- 8. Register overlay triggers + set PASS --------------------- */
    if (svc->comp_count > 0) {
        const char *paths[CBX_MAX_COMPOSITES];
        for (int i = 0; i < svc->comp_count; i++)
            paths[i] = svc->composites[i].composite_path;
        cbx_trigger_register_all(svc->conn.backend, svc->conn.bus,
                                  paths, svc->comp_count,
                                  svc->settings.overlay_trigger);
    }
    set_all_pass(svc->conn.backend, svc->conn.bus,
                  svc->model.composites, svc->comp_count);

    /* --- 9. Initialise overlay lifecycle ----------------------------- */
    const char *primary_path =
        svc->comp_count > 0 ? svc->composites[0].composite_path : "";
    cbx_overlay_lifecycle_init(&svc->lifecycle, svc->conn.backend, svc->conn.bus,
                               primary_path, &svc->surface, svc->rend.renderer);

    /* --- 10. Set up mode state + callbacks ---------------------------- */
    /* Player Mode (SPEC §4.3). */
    cbx_player_mode_init(&svc->pm, &svc->grid);
    svc->pm.on_slot_change     = cbx_overlay_on_slot_change;
    svc->pm.slot_change_data   = svc;
    svc->pm.on_profile_change  = cbx_overlay_on_profile_change;
    svc->pm.profile_change_data = svc;

    /* Host Mode (SPEC §4.4). */
    cbx_host_mode_init(&svc->hm);
    svc->hm.on_slot_change    = on_host_slot_change;
    svc->hm.slot_change_data  = svc;

    /* Lifecycle on_save callback: conflict resolution + assignment save. */
    svc->lifecycle.on_save       = cbx_overlay_on_save;
    svc->lifecycle.on_save_data  = svc;

    /* --- 10b. Set up DBus InputEvent signal handling (Task 6) ------- */
    svc->input_ctx.pm        = &svc->pm;
    svc->input_ctx.hm        = &svc->hm;
    svc->input_ctx.grid      = &svc->grid;
    svc->input_ctx.lifecycle  = &svc->lifecycle;
    svc->input_ctx.path_count = 0;
    cbx_overlay_input_build_map(svc->conn.backend, svc->conn.bus,
                                  svc->composites, svc->comp_count,
                                  &svc->input_ctx);

    const char *uniq = ip_connection_get_unique_name(&svc->conn);
    snprintf(svc->expected_sender, sizeof(svc->expected_sender),
             "%s", uniq ? uniq : "");
    ip_input_events_init(&svc->input_events, svc->conn.backend, svc->conn.bus,
                          svc->expected_sender,
                          cbx_overlay_input_cb, &svc->input_ctx);
    svc->input_events_ready = false;
    if (ip_input_events_subscribe(&svc->input_events) == 0)
        svc->input_events_ready = true;

    /* --- 11. Set up InterceptMode polling --------------------------- */
    svc->poll_event_type = SDL_RegisterEvents(1);
    svc->poll_count = 0;

    cbx_overlay_rearm_polls(svc);

    /* Subscribe to ObjectManager hotplug signals for incremental
     * device updates (SPEC §10.1: InterfacesAdded/InterfacesRemoved). */
    ip_hotplug_init(&svc->hp, svc->conn.backend, svc->conn.bus,
                     svc->expected_sender, &svc->model);
    if (svc->backend_ready)
        ip_hotplug_subscribe(&svc->hp);

    svc->initialized = true;
    ip_connection_set_reenumerate_cb(&svc->conn, overlay_backend_ready, svc);
    ip_connection_set_degraded_cb(&svc->conn, overlay_backend_degraded, svc);
    if (!svc->backend_ready)
        overlay_backend_degraded(ip_connection_reason_for_error(conn_rc), svc);

    /* --- 12. Poll loop (Task 4) -------------------------------------- */
    while (g_running) {
        cbx_overlay_service_step(svc);
        SDL_Delay(10);
    }

    /* --- 13. Clean shutdown ------------------------------------------- */
    /* Stop all InterceptMode poll timers. */
    for (int i = 0; i < svc->poll_count; i++)
        ip_intercept_poll_stop(&svc->polls[i]);

    /* Force-close the overlay if still visible. */
    cbx_overlay_lifecycle_force_close(&svc->lifecycle);

    /* Destroy surface, cleanup caches, disconnect, shutdown renderer. */
    cbx_overlay_surface_destroy(&svc->surface);
    cbx_icon_cache_cleanup(&svc->icon_cache);
    cbx_text_cache_cleanup(&svc->text_cache);
    ip_connection_disconnect(&svc->conn);
    cbx_renderer_shutdown(&svc->rend);
    free(svc);
    return 0;
}