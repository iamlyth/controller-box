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
#include "overlay/close.h"            /* cbx_close_sync_assignments */
#include "dbus/ip_hotplug.h"
#include "dbus/ip_properties.h"
#include "identify/assign.h"            /* CBX_DEFAULT_PROFILE */
#include "identify/composite_identity.h"
#include "identify/gamepad_order_restore.h"
#include "dbus/ip_gamepad_order.h"

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
    fprintf(stderr, "controller-box: trigger detected on %s — activating overlay\n",
            act->composite_path[0] ? act->composite_path : "(unknown)");
    /* Update lifecycle's composite_path to the activating composite. */
    if (act->composite_path[0]) {
        size_t len = strlen(act->composite_path);
        if (len >= sizeof(act->lifecycle->composite_path))
            len = sizeof(act->lifecycle->composite_path) - 1;
        memcpy(act->lifecycle->composite_path, act->composite_path, len);
        act->lifecycle->composite_path[len] = '\0';
    }
    cbx_overlay_lifecycle_activate(act->lifecycle);
    fprintf(stderr, "controller-box: overlay lifecycle activated\n");
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
/*  Reactive PropertiesChanged handling (Task 5)                        */
/* ================================================================== */

/*
 * Map a profile filesystem path back to the enumerated profile filename
 * used by the grid.  Falls back to the basename without a .yaml suffix when
 * the path is not in the loaded profile list.
 */
static void
overlay_profile_name_from_path(const cbx_overlay_service_ctx *svc,
                               const char *path, char *out, size_t out_len)
{
    if (!out || out_len == 0)
        return;
    out[0] = '\0';
    if (!path || !path[0])
        return;

    const cbx_profile_list *list = svc->profile_cycle.profiles;
    if (list) {
        for (int i = 0; i < list->count; i++) {
            if (strcmp(list->entries[i].path, path) == 0) {
                snprintf(out, out_len, "%s", list->entries[i].filename);
                return;
            }
        }
    }

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(out, out_len, "%.*s", (int)(out_len - 1), base);
    char *dot = strrchr(out, '.');
    if (dot && strcmp(dot, ".yaml") == 0)
        *dot = '\0';
}

/*
 * Update the displayed profile of the grid row that names `object_path`
 * from the composite's validated reactive ProfilePath/ProfileName.  This is
 * the rendered-UI half of per-device property application: only the row for
 * the reported composite changes, so one device's signal cannot overwrite
 * another's displayed profile.
 */
static void
overlay_apply_grid_profile(cbx_overlay_service_ctx *svc,
                           const char *object_path)
{
    const cbx_composite_entry *ce =
        cbx_device_model_find_composite(&svc->model, object_path);
    if (!ce)
        return;

    char name[CBX_GRID_PROFILE_LEN] = "";
    if (ce->has_profile_path && ce->profile_path[0])
        overlay_profile_name_from_path(svc, ce->profile_path,
                                       name, sizeof(name));

    /* Fall back to ProfileName (display name) matched against the loaded
     * profile list; if nothing matches, leave the current displayed value. */
    if (name[0] == '\0' && ce->has_profile_name && ce->profile_name[0]) {
        const cbx_profile_list *list = svc->profile_cycle.profiles;
        if (list) {
            for (int i = 0; i < list->count; i++) {
                if (strcmp(list->entries[i].display_name, ce->profile_name) == 0 ||
                    strcmp(list->entries[i].filename, ce->profile_name) == 0) {
                    snprintf(name, sizeof(name), "%s",
                             list->entries[i].filename);
                    break;
                }
            }
        }
    }

    if (name[0] == '\0')
        return;

    for (int i = 0; i < svc->grid.row_count; i++) {
        if (strcmp(svc->grid.rows[i].composite_path, object_path) == 0) {
            snprintf(svc->grid.rows[i].profile,
                     sizeof(svc->grid.rows[i].profile), "%s", name);
            break;
        }
    }
}

/*
 * Production PropertiesChanged callback.  Applies each validated change to
 * the per-device overlay model (cbx_device_model_apply_property), so a
 * change for one composite updates only that composite and a Manager
 * GamepadOrder updates only the model's Manager ordering.  A ProfilePath or
 * ProfileName change also refreshes the matching grid row's displayed
 * profile.  Unknown object paths, wrong interfaces, and spoofed senders
 * were already rejected upstream (ip_properties_handle_changed); this
 * callback additionally rejects paths absent from the live model.  Finally
 * the surface is marked dirty so the presented frame reflects the change.
 */
void
cbx_overlay_on_prop_change(const char *object_path, const char *iface_name,
                           const char *prop_name, ip_prop_type type,
                           const char *value, int count, void *userdata)
{
    (void)count;
    cbx_overlay_service_ctx *svc = (cbx_overlay_service_ctx *)userdata;
    if (!svc || !object_path || !iface_name || !prop_name)
        return;

    bool invalidated = (type == IP_PROP_TYPE_INVALIDATED);
    if (!cbx_device_model_apply_property(&svc->model, object_path,
                                          iface_name, prop_name, value,
                                          invalidated))
        return;  /* unknown device/property — do not dirty the surface */

    if (!invalidated && strcmp(iface_name, IP_IFACE_COMPOSITE) == 0 &&
        (strcmp(prop_name, "ProfileName") == 0 ||
         strcmp(prop_name, "ProfilePath") == 0)) {
        overlay_apply_grid_profile(svc, object_path);
    }

    /* SourceDevicePaths is part of the physical identity contract.  The
     * signal callback only records the need for reconciliation; doing
     * property reads here would re-enter DBus dispatch and could race the
     * hotplug model update.  The service loop performs the bounded pass. */
    if (strcmp(iface_name, IP_IFACE_COMPOSITE) == 0 &&
        strcmp(prop_name, "SourceDevicePaths") == 0)
        svc->identity_reconcile_pending = true;

    /* A validated change to a tracked property is a dirty trigger: re-render
     * so the presented frame reflects InputPlumber's live property state. */
    if (svc->initialized)
        cbx_overlay_surface_mark_dirty_all(&svc->surface);
}

int
cbx_overlay_props_wire(cbx_overlay_service_ctx *svc)
{
    if (!svc || !svc->conn.backend || !svc->conn.bus ||
        !svc->expected_sender[0])
        return -EINVAL;

    ip_properties_init(&svc->props, svc->conn.backend, svc->conn.bus,
                       svc->expected_sender, cbx_overlay_on_prop_change, svc);
    return ip_properties_subscribe(&svc->props);
}

/* ================================================================== */
/*  Serialized assignment persistence helpers                          */
/* ================================================================== */

/* The overlay service is a resident, single-threaded input/render loop.  It
 * must never block that loop indefinitely on the cross-process config lock:
 * a Manager transaction can hold it while polling InputPlumber.  Interactive
 * overlay writes therefore use a bounded wait and report a persistence
 * failure (close still returns input to the game) instead of stalling. */
#define CBX_OVERLAY_CONFIG_LOCK_TIMEOUT_MS 200

/*
 * Merge the overlay grid's assignment/order state onto `a` (a freshly loaded
 * on-disk table).  The assignment half is the shared close-path sync so the
 * two overlay save paths cannot drift; only rows present in the grid are
 * touched, so entries for controllers that are not currently displayed are
 * preserved and a concurrent Manager update is not erased by an overlay save.
 * gamepad_order is then rebuilt from the grid's slot ordering (the close-path
 * sync is deliberately order-agnostic).
 */
static bool
order_contains(char order[][CBX_MAX_ID_LEN], int count, const char *id)
{
    for (int i = 0; i < count; i++)
        if (strcmp(order[i], id) == 0)
            return true;
    return false;
}

/* Bounded id copy: GCC's format-truncation analysis cannot bound a `%s` read
 * from another fixed array, so copy explicitly instead. */
static void
copy_id(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0)
        return;
    size_t n = src ? strlen(src) : 0;
    if (n >= dst_size)
        n = dst_size - 1;
    if (n > 0)
        memcpy(dst, src, n);
    dst[n] = '\0';
}

static int
overlay_merge_grid(cbx_assignments *a, const cbx_select_grid *grid)
{
    int rc = cbx_close_sync_assignments(grid, a);
    if (rc < 0)
        return rc;

    /* Snapshot the saved order before rewriting it.  Entries whose controller
     * is not currently connected must survive a topology shrink: rebuilding
     * the order purely from the live grid would silently drop the preferred
     * position of a disconnected controller (task 6 acceptance). */
    char saved[CBX_MAX_GAMEPAD_ORDER][CBX_MAX_ID_LEN];
    int saved_count = a->gamepad_order_count;
    if (saved_count > CBX_MAX_GAMEPAD_ORDER)
        saved_count = CBX_MAX_GAMEPAD_ORDER;
    for (int i = 0; i < saved_count; i++) {
        copy_id(saved[i], sizeof(saved[i]), a->gamepad_order[i]);
    }

    char merged[CBX_MAX_GAMEPAD_ORDER][CBX_MAX_ID_LEN];
    int merged_count = 0;

    /* Currently connected rows, in player-slot order. */
    for (int slot = 0; slot < CBX_MAX_CONTROLLERS; slot++) {
        for (int i = 0; i < grid->row_count; i++) {
            const cbx_grid_row *row = &grid->rows[i];
            if (row->id[0] == '\0')
                continue;
            if (cbx_select_grid_col_to_slot(row->cur_col) != slot)
                continue;
            if (merged_count < CBX_MAX_GAMEPAD_ORDER &&
                !order_contains(merged, merged_count, row->id)) {
                copy_id(merged[merged_count++], CBX_MAX_ID_LEN, row->id);
            }
        }
    }

    /* Then any saved entry for a disconnected controller, preserving its
     * relative position after the connected players. */
    for (int i = 0; i < saved_count; i++) {
        if (saved[i][0] == '\0')
            continue;
        if (merged_count < CBX_MAX_GAMEPAD_ORDER &&
            !order_contains(merged, merged_count, saved[i])) {
            copy_id(merged[merged_count++], CBX_MAX_ID_LEN, saved[i]);
        }
    }

    a->gamepad_order_count = merged_count;
    for (int i = 0; i < merged_count; i++)
        copy_id(a->gamepad_order[i], CBX_MAX_ID_LEN, merged[i]);

    return 0;
}

#ifdef CBX_TESTING
/* Test seam: prove the grid→order merge preserves disconnected preferences
 * (task 6 topology-shrink acceptance) without exposing the static helper. */
int
cbx_overlay_merge_grid_for_test(cbx_assignments *a, const cbx_select_grid *grid)
{
    return overlay_merge_grid(a, grid);
}
#endif

typedef struct {
    const cbx_select_grid *grid;
} overlay_grid_txn_args;

static int
overlay_txn_merge_grid(cbx_assignments *a, void *userdata)
{
    overlay_grid_txn_args *args = userdata;
    return overlay_merge_grid(a, args->grid);
}

typedef struct {
    const char *id;
    const char *profile;
} overlay_profile_txn_args;

static int
overlay_txn_set_profile(cbx_assignments *a, void *userdata)
{
    overlay_profile_txn_args *args = userdata;
    /* Reuse the production assignment-update helper so a new controller gets
     * a real free slot instead of an invalid -1 slot. */
    return cbx_profile_cycle_update_assignment(a, args->id, args->profile);
}

/* ================================================================== */
/*  Grid → engine application (shared by save and hotplug reconcile)   */
/* ================================================================== */

/*
 * Build the bus-wide comma-separated GamepadOrder from the grid's
 * player-slot ordering.  Only rows holding an assigned slot contribute
 * (Unassigned rows are omitted) and rows are emitted in slot order, so the
 * engine order is exactly the displayed topology.  Returns 0, or
 * -ENAMETOOLONG when the composite paths cannot fit in `order`.
 */
static int
overlay_build_gamepad_order(const cbx_select_grid *grid,
                            char *order, size_t order_size)
{
    if (!grid || !order || order_size == 0)
        return -EINVAL;

    order[0] = '\0';
    size_t order_len = 0;
    for (int slot = 0; slot < CBX_MAX_CONTROLLERS; slot++) {
        for (int i = 0; i < grid->row_count; i++) {
            const cbx_grid_row *row = &grid->rows[i];
            if (cbx_select_grid_col_to_slot(row->cur_col) != slot)
                continue;
            size_t path_len = strlen(row->composite_path);
            size_t need = path_len + (order_len > 0 ? 1 : 0);
            if (order_len + need >= order_size - 1)
                return -ENAMETOOLONG;
            if (order_len > 0)
                order[order_len++] = ',';
            memcpy(order + order_len, row->composite_path, path_len);
            order_len += path_len;
            order[order_len] = '\0';
        }
    }
    return 0;
}

/*
 * Choose the profile to load for `row` from the enumerated profile list.
 *
 * The saved preference is used when it still exists.  When it does not (the
 * profile YAML was deleted or renamed) the built-in default — or, failing
 * that, the first enumerated profile — is a valid alternative, so a stale
 * preference degrades to a working profile instead of failing restoration
 * forever and leaving operations disabled.  `out_name` is empty only when no
 * profile can be applied (no list or an empty list).
 */
static void
overlay_pick_profile(const cbx_overlay_service_ctx *svc,
                     const cbx_grid_row *row,
                     char *out_name, size_t out_name_len)
{
    if (!out_name || out_name_len == 0)
        return;
    out_name[0] = '\0';
    if (!svc || !row || !svc->profile_cycle.profiles)
        return;

    const cbx_profile_list *list = svc->profile_cycle.profiles;
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->entries[i].filename, row->profile) == 0) {
            copy_id(out_name, out_name_len, row->profile);
            return;
        }
    }
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->entries[i].filename, CBX_DEFAULT_PROFILE) == 0) {
            copy_id(out_name, out_name_len, CBX_DEFAULT_PROFILE);
            return;
        }
    }
    if (list->count > 0)
        copy_id(out_name, out_name_len, list->entries[0].filename);
}

/*
 * Apply the grid's assignment topology to the live InputPlumber engine:
 * verified profile load plus exact-singleton TargetDevices replacement for
 * every assigned row, then the grid-derived GamepadOrder.
 *
 * This is the single engine-apply definition shared by the startup/close
 * save path (cbx_overlay_on_save, which then persists) and the hotplug
 * reconcile, which must make a reconnected controller's saved slot/profile
 * effective immediately rather than only displaying it (SPEC §6.2/§10.1).
 *
 * `clear_all` preserves the save path's authoritative transfer/Unassigned
 * semantics: every grid row is detached before reassignment so P1→P2
 * transfers cannot leave an additive target.  The hotplug path passes
 * false: adding or removing one controller must not tear down the routing
 * of the controllers that are already attached.
 */
static int
overlay_apply_grid_engine(cbx_overlay_service_ctx *svc, bool clear_all,
                           bool restore_order)
{
    if (!svc || !svc->conn.backend || !svc->conn.bus)
        return -EINVAL;

    char order[CBX_MAX_COMPOSITES * (CBX_MAX_PATH_LEN + 1)];
    int rc = overlay_build_gamepad_order(&svc->grid, order, sizeof(order));
    if (rc != 0)
        return rc;

    /* Resolve durable order before any engine mutation, from the same
     * checked physical snapshot used by the assignment grid. */
    if (restore_order) {
        char *saved = NULL;
        rc = ip_gamepad_order_load(&saved);
        if (rc != 0)
            return rc;
        if (saved && saved[0]) {
            bool uncertain = false;
            rc = cbx_gamepad_order_map_snapshot(svc->identities,
                svc->identity_count, saved, order, sizeof(order),
                NULL, NULL, &uncertain);
            if (rc == 0 && uncertain)
                rc = -EAGAIN;
        }
        free(saved);
        if (rc != 0)
            return rc;
    }

    /* Phase 1: exact replacement routing (SPEC §§4.1-4.7). */
    {
        for (int i = 0; i < svc->grid.row_count; i++) {
            /* A changed physical identity may no longer authorize the old
             * route.  Unassigned must mean detached on hotplug too, while
             * unaffected assigned controllers retain their routing. */
            if (!clear_all &&
                cbx_select_grid_col_to_slot(svc->grid.rows[i].cur_col) >= 0)
                continue;
            const char *composite = svc->grid.rows[i].composite_path;
            int clear_rc = ip_composite_set_target_device_paths(
                svc->conn.backend, svc->conn.bus, composite, "");
            if (clear_rc != 0)
                return clear_rc;
            clear_rc = wait_for_attachment(svc, composite, NULL);
            if (clear_rc != 0)
                return clear_rc;
        }
    }

    for (int i = 0; i < svc->grid.row_count; i++) {
        const cbx_grid_row *row = &svc->grid.rows[i];
        int slot = cbx_select_grid_col_to_slot(row->cur_col);
        if (slot < 0)
            continue;
        if (slot >= svc->model.target_count)
            return -ENODEV;

        /* Load profile and verify engine state.  Skip if no profile list
         * is available (e.g. degraded/test mode) — slot assignment and
         * GamepadOrder are still applied.  A saved profile that no longer
         * exists falls back to a valid alternative (built-in default or the
         * first enumerated profile) instead of failing restoration forever;
         * the row is then updated to the profile actually loaded so the UI
         * and persisted table never claim an engine state that is not held
         * (task 6 acceptance: invalid preferred properties with valid
         * alternatives). */
        if (row->profile[0] && svc->profile_cycle.profiles) {
            char apply_profile[CBX_GRID_PROFILE_LEN];
            overlay_pick_profile(svc, row, apply_profile,
                                 sizeof(apply_profile));
            if (apply_profile[0] != '\0') {
                if (strcmp(apply_profile, row->profile) != 0) {
                    fprintf(stderr,
                            "controller-box: saved profile '%s' for %s is "
                            "not available; applying '%s'\n",
                            row->profile, row->composite_path,
                            apply_profile);
                    snprintf(svc->grid.rows[i].profile,
                             CBX_GRID_PROFILE_LEN, "%s", apply_profile);
                }
                int profile_rc = cbx_profile_cycle_apply(
                    &svc->profile_cycle, &svc->grid, i,
                    apply_profile, row->composite_path);
                if (profile_rc != 0)
                    return profile_rc;
            }
        }

        int attach_rc = ip_composite_set_target_device_paths(
            svc->conn.backend, svc->conn.bus, row->composite_path,
            svc->model.targets[slot].path);
        if (attach_rc != 0)
            return attach_rc;
        attach_rc = wait_for_attachment(svc, row->composite_path,
                                         svc->model.targets[slot].path);
        if (attach_rc != 0)
            return attach_rc;
    }

    /* Phase 2: Set GamepadOrder on the engine. */
    return ip_manager_set_gamepad_order(svc->conn.backend, svc->conn.bus,
                                        order, &svc->model);
}

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

    /* Apply the conflict-resolved topology to the engine (exact
     * TargetDevices replacement, verified profile load, GamepadOrder). */
    int rc = overlay_apply_grid_engine(svc, true, false);
    if (rc != 0)
        return rc;

    /* --- Phase 3: All engine state verified — persist the merged table ---
     * Only after every LoadProfilePath, exact TargetDevices replacement, and
     * SetGamepadOrder succeeded do we publish the assignments, and we merge
     * onto the current on-disk table under the shared config lock so a
     * concurrent Manager write is not erased. */
    overlay_grid_txn_args persist_args = { &svc->grid };
    rc = cbx_assignments_transaction_timeout(overlay_txn_merge_grid,
                                             &persist_args,
                                             &svc->assignments,
                                             CBX_OVERLAY_CONFIG_LOCK_TIMEOUT_MS);
    if (rc != 0)
        return rc;

    /* Save/close path: the presented frame reflects the persisted and
     * conflict-resolved topology, so it is deliberately kept in the
     * dirty-trigger set as part of the W2 reconciliation of the §4.9
     * pre-build policy — re-render rather than present a stale pre-built
     * frame. */
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
    /* Slot change is a dirty trigger (SPEC §4.9): re-render so the
     * presented frame reflects the new column assignment. */
    cbx_overlay_surface_mark_dirty_all(&svc->surface);
    return 0;
}

/*
 * Restore the displayed profile of `row_idx` to the profile the engine
 * currently holds.  Used only when an engine apply fails: the mode handler
 * (Player or Host) cycled the grid's profile string before firing the
 * profile-change callback, so on failure the grid would otherwise display a
 * profile the engine rejected and diverge from real engine state.  The
 * engine's ProfilePath is authoritative; it is reverse-mapped through the
 * loaded profile list to a filename for the grid.
 */
static void
revert_grid_profile_to_engine(cbx_overlay_service_ctx *svc, int row_idx,
                              const char *composite_path)
{
    if (!svc || row_idx < 0 || row_idx >= svc->grid.row_count)
        return;
    if (!svc->conn.backend || !svc->conn.bus)
        return;

    char *engine_path = NULL;
    if (ip_composite_get_profile_path(svc->conn.backend, svc->conn.bus,
                                      composite_path, &engine_path) != 0 ||
        !engine_path) {
        free(engine_path);
        return;
    }

    const cbx_profile_list *list = svc->profile_cycle.profiles;
    if (list) {
        for (int i = 0; i < list->count; i++) {
            if (strcmp(list->entries[i].path, engine_path) == 0) {
                snprintf(svc->grid.rows[row_idx].profile,
                         CBX_GRID_PROFILE_LEN, "%s",
                         list->entries[i].filename);
                break;
            }
        }
    }
    free(engine_path);
}

int
cbx_overlay_on_profile_change(int row_idx, const char *profile,
                   const char *composite_path, void *userdata)
{
    cbx_overlay_service_ctx *svc = (cbx_overlay_service_ctx *)userdata;
    if (!svc)
        return -EINVAL;

    /* The mode handler has already cycled the grid row's displayed profile
     * before firing this callback, so the profile-change event has occurred
     * from the user's perspective.  SPEC §4.9 requires the pre-built surface
     * to be dirtied so the next presentation reflects it; mark it here,
     * before any early return, so a failed apply can never leave a stale
     * presented frame. */
    cbx_overlay_surface_mark_dirty_all(&svc->surface);

    if (!svc->conn.backend || !composite_path || !profile || !profile[0])
        return -EINVAL;

    int rc = cbx_profile_cycle_apply(&svc->profile_cycle, &svc->grid,
                                      row_idx, profile, composite_path);
    if (rc != 0) {
        /* Engine apply failed: roll the displayed profile back to the
         * engine's actual profile so the overlay never misrepresents live
         * state (SPEC §4.4/§4.9).  The dirty mark above already covers the
         * re-render.  When no profile list is loaded the apply never reached
         * the engine, so there is no authoritative profile to map back. */
        if (svc->profile_cycle.profiles)
            revert_grid_profile_to_engine(svc, row_idx, composite_path);
        return rc;
    }

    /* Persist the profile on the current on-disk table under the shared
     * config lock; the profile-cycle apply already updated svc->assignments
     * in memory, and the transaction merges the single changed row so a
     * concurrent write to another controller is preserved.  The wait is
     * bounded so a slow Manager transaction cannot stall this event loop. */
    if (row_idx >= 0 && row_idx < svc->grid.row_count &&
        svc->grid.rows[row_idx].id[0] != '\0') {
        const cbx_grid_row *row = &svc->grid.rows[row_idx];
        overlay_profile_txn_args args = {
            row->id,
            row->profile,
        };
        rc = cbx_assignments_transaction_timeout(overlay_txn_set_profile, &args,
                                                 &svc->assignments,
                                                 CBX_OVERLAY_CONFIG_LOCK_TIMEOUT_MS);
    } else {
        /* A row without a stable id cannot be keyed in assignments.yaml, so
         * there is nothing meaningful to persist.  An unlocked full-table
         * save of the service's stale in-memory snapshot would erase
         * concurrent Manager/order writes (task 21 acceptance); the engine
         * already holds the applied profile, so skip persistence. */
        rc = 0;
    }
    /* The dirty trigger already fired above.  A persistence failure does
     * not undo the engine-applied profile, so the grid remains truthful. */
    return rc;
}

/* ================================================================== */
/*  Host Mode slot change callback                                     */
/* ================================================================== */

static int
on_host_slot_change(int row_idx, int new_slot, void *userdata)
{
    return cbx_overlay_on_slot_change(row_idx, new_slot, userdata);
}

/*
 * Host-mode state-transition callback: fired on host-mode enter and exit
 * (SPEC §4.4).  Entering/exiting host mode materially changes the
 * rendered row visuals — HOST/SELECTED/FROZEN rows appear on entry and
 * revert to normal Player Mode on exit — so the pre-built surface must be
 * marked dirty so the next presentation reflects the transition.  Host-mode
 * transitions are a deliberate W2 extension of the §4.9 pre-build dirty
 * policy (which enumerates device/slot/profile change events), not a
 * device/slot/profile change themselves.
 */
int
cbx_overlay_on_host_mode_change(bool active, void *userdata)
{
    (void)active;
    cbx_overlay_service_ctx *svc = (cbx_overlay_service_ctx *)userdata;
    if (!svc)
        return -EINVAL;
    cbx_overlay_surface_mark_dirty_all(&svc->surface);
    return 0;
}

/*
 * Lifecycle on_closed callback: end Host Mode when the overlay reaches
 * IDLE (SPEC §4.4).  Host Mode is scoped to one overlay session — the
 * first controller to press R3 becomes the exclusive host — so a close
 * (B-press, deactivation, timeout, backend loss, or shutdown) must end it.
 * Without this, reopening the overlay would present every non-host
 * controller frozen even though none of them pressed R3 in the new session,
 * and only the stale host could exit.  cbx_host_mode_exit is a no-op on an
 * already-idle object (W2), so this never double-fires the dirty trigger;
 * when it does exit, the state-change callback marks the surface dirty so
 * the next activation renders normal rows.  on_save has already persisted
 * the session's edits by the time IDLE is reached.
 */
void
cbx_overlay_on_lifecycle_closed(void *userdata)
{
    cbx_overlay_service_ctx *svc = (cbx_overlay_service_ctx *)userdata;
    if (!svc)
        return;
    cbx_host_mode_exit(&svc->hm);
}

/* ================================================================== */
/*  Helper: fill cbx_grid_composite_info from the device model + DBus  */
/* ================================================================== */
static void fill_composite_info(cbx_grid_composite_info *info,
                                 const cbx_composite_entry *entry,
                                 const cbx_composite_identity_entry *identity,
                                 const ip_dbus_backend *backend,
                                 ip_bus_handle bus)
{
    /* Path */
    snprintf(info->composite_path, sizeof(info->composite_path),
             "%s", entry->path);

    /* Identity from the composite's physical source devices (SPEC §6.2–6.3),
     * never the opaque PersistentId.  `cbx_composite_identity_extract`
     * chooses BT MAC → USB serial → USB port path → connection order and
     * falls back to ORDER:<index> when the source list is legitimately
     * absent, so the row keeps a valid, persistable identity instead of the
     * invalid `composite-<index>` synthetic id (cbx_validate_id rejects it).
     * A *transient* identity query failure leaves the id empty: the row must
     * not be matched against a saved assignment or persisted, so a DBus
     * hiccup can never reroute or erase another controller's preference
     * (task 6 acceptance).  Only real physical layers are stable for Host
     * Mode. */
    const cbx_identity *ident = &identity->ident;
    if (cbx_composite_identity_is_matchable(ident, identity->status)) {
        snprintf(info->id, sizeof(info->id), "%s", ident->id);
        info->id_stable = (ident->layer == CBX_IDENTITY_LAYER_BT_MAC ||
                           ident->layer == CBX_IDENTITY_LAYER_USB_SERIAL ||
                           ident->layer == CBX_IDENTITY_LAYER_USB_PORT);
    } else {
        info->id[0] = '\0';
        info->id_stable = false;
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

/* Probe every composite before changing the grid or routing.  A failed
 * source-property read is uncertainty, not confirmed device absence; doing
 * this pass first keeps the previous assignment/order state intact while a
 * recovery retry obtains a complete snapshot. */
static int
overlay_validate_identities(cbx_overlay_service_ctx *svc)
{
    if (!svc || !svc->conn.backend || !svc->conn.bus)
        return -EINVAL;

    svc->identities_valid = false;
    /* Drop the previous pass's snapshot: a failed identity pass must never
     * leave a stale entry readable at an index the grid still fills. */
    svc->identity_count = 0;
    cbx_composite_identity_entry entries[CBX_MAX_COMPOSITES];
    int count = 0;
    int rc = cbx_model_extract_identities(svc->conn.backend, svc->conn.bus,
                                          &svc->model, entries, &count);
    if (rc != 0)
        return rc;

    for (int i = 0; i < count; i++) {
        if (entries[i].status == CBX_COMPOSITE_IDENTITY_QUERY_FAILED)
            return -EAGAIN;
        for (int j = 0; j < i; j++) {
            if (entries[i].ident.id[0] &&
                strcmp(entries[i].ident.id, entries[j].ident.id) == 0) {
                fprintf(stderr, "controller-box: ambiguous physical identity %s\n",
                        entries[i].ident.id);
                return -EAGAIN;
            }
        }
    }
    memcpy(svc->identities, entries, sizeof(entries[0]) * (size_t)count);
    svc->identity_count = count;
    svc->identities_valid = true;
    return 0;
}

/* Identity entry for composite `i`, or an all-zero entry when the current
 * reconciliation pass did not validate one.  On a failed identity pass
 * `identity_count` is 0 while `svc->composites` still has model rows to
 * render, so callers must never index the retained snapshot directly. */
static const cbx_composite_identity_entry *
overlay_identity_for(const cbx_overlay_service_ctx *svc, int i)
{
    static const cbx_composite_identity_entry none;
    if (svc && i >= 0 && i < svc->identity_count)
        return &svc->identities[i];
    return &none;
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

/*
 * Apply (or clear, with 0) the bus-wide synchronous-call deadline that bounds
 * a readiness/recovery/hotplug pass.  The production sd-bus backend scales
 * every method call down to the remaining budget and fails an already-expired
 * call with -ETIMEDOUT, so the pass cannot blow its advertised window inside
 * a single slow/hung reply.  Backends that do not implement set_deadline are
 * left alone.
 */
static void
overlay_set_call_deadline(cbx_overlay_service_ctx *svc, uint64_t deadline_ms)
{
    if (svc && svc->conn.backend && svc->conn.backend->set_deadline)
        (void)svc->conn.backend->set_deadline(svc->conn.bus, deadline_ms);
}

static uint64_t
overlay_pass_deadline_ms(const cbx_overlay_service_ctx *svc)
{
    uint32_t timeout = svc->reconcile_timeout_ms ? svc->reconcile_timeout_ms :
        CBX_RECONCILE_TIMEOUT_MS;
    return reconcile_now_ms() + timeout;
}

/*
 * Bounded close-path on_save.  cbx_overlay_on_save() performs a sequence of
 * synchronous engine calls (clear-all + per-row LoadProfilePath/verify +
 * TargetDevices + attachment waits + GamepadOrder) and a persistence
 * transaction.  The lifecycle writes InterceptMode=PASS before this runs
 * (SPEC §11: input to the game in <1 ms), but an unbounded save would still
 * hold the single UI thread (and the overlay hide/fade) for as long as a
 * wedged InputPlumber takes — and each internal wait honours only its own
 * per-call budget.  Arm the same wall-clock deadline used by
 * readiness/recovery/hotplug so the whole save is bounded and every internal
 * call fails fast once the budget expires.  The deadline is always cleared
 * on exit so the close path cannot leak it into the next operation.
 */
int
cbx_overlay_on_save_bounded(void *userdata)
{
    cbx_overlay_service_ctx *svc = (cbx_overlay_service_ctx *)userdata;
    if (!svc)
        return -EINVAL;
    overlay_set_call_deadline(svc, overlay_pass_deadline_ms(svc));
    int rc = cbx_overlay_on_save(svc);
    overlay_set_call_deadline(svc, 0);
    return rc;
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

static int
csv_token_count(const char *csv)
{
    int tokens = 0;
    if (!csv) return 0;
    for (const char *p = csv; *p;) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        tokens++;
        const char *end = strchr(p, ',');
        p = end ? end + 1 : p + strlen(p);
    }
    return tokens;
}

static bool
csv_is_exact_singleton(const char *csv, const char *path)
{
    return csv_token_count(csv) == 1 && csv_exact_path_count(csv, path) == 1;
}

/* Resolve a persisted physical assignment by the composite's source-derived
 * physical identity (SPEC §6.2), not the opaque PersistentId.  `entries` are
 * the identities extracted once for the current model.  Composites whose
 * identity query failed transiently are never matched: a DBus hiccup must
 * not route one controller's target onto another controller's slot.  Virtual
 * slots do not imply physical composites: an absent assignment is a valid
 * ready-but-unassigned slot. */
static const char *
assigned_composite_for_slot(const cbx_composite_identity_entry *entries,
                            int entry_count,
                            const cbx_assignments *assignments, int slot)
{
    for (int ci = 0; ci < entry_count; ci++) {
        if (entries[ci].status == CBX_COMPOSITE_IDENTITY_QUERY_FAILED)
            return NULL;
    }

    for (int ai = 0; ai < assignments->assignment_count; ai++) {
        const cbx_assignment *a = &assignments->assignments[ai];
        if (a->slot != slot) continue;

        const char *match = NULL;
        for (int ci = 0; ci < entry_count; ci++) {
            if (!cbx_composite_identity_is_matchable(&entries[ci].ident,
                                                     entries[ci].status))
                continue;
            if (strcmp(entries[ci].ident.id, a->id) != 0)
                continue;
            /* A duplicated serial/port/order identity is not a physical
             * match.  Refuse the attachment rather than silently routing the
             * slot to whichever DBus object happened to be enumerated first. */
            if (match)
                return NULL;
            match = entries[ci].path;
        }
        if (match)
            return match;
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
        /* A re-enumeration must not discard reactive per-device property
         * state already applied for composites that survived the rebuild. */
        cbx_device_model_preserve_props(&next, &svc->model);
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
        /* The bus-wide pass deadline expired inside a call: abort instead of
         * spinning against a backend that will keep failing fast. */
        if (last_rc == -ETIMEDOUT)
            return -ETIMEDOUT;
        if (reconcile_now_ms() >= deadline)
            return -ETIMEDOUT;
        /* Dispatch ObjectManager traffic between bounded polls; sleeping is
         * only a short backoff, never the sole progress mechanism.  A DBus
         * processing error aborts the wait instead of spinning to timeout. */
        if (svc->conn.backend->process) {
            int prc = svc->conn.backend->process(svc->conn.bus);
            if (prc < 0)
                return prc;
        }
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
        bool found = last_rc == 0 &&
            (target ? csv_is_exact_singleton(paths, target)
                    : csv_token_count(paths) == 0);
        free(paths);
        if (found)
            return 0;
        /* See wait_for_exact_target: honour the bus-wide pass deadline. */
        if (last_rc == -ETIMEDOUT)
            return -ETIMEDOUT;
        if (reconcile_now_ms() >= deadline)
            return -ETIMEDOUT;
        if (svc->conn.backend->process) {
            int prc = svc->conn.backend->process(svc->conn.bus);
            if (prc < 0)
                return prc;
        }
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
    /* Per-created-target rollback context: the original target a replacement
     * superseded ("" for a new slot), the composite it was attached to (""
     * when none), and whether that original's StopTargetDevice was already
     * issued.  A replacement whose original stop was issued must never be
     * stopped while the composite still routes to it (SPEC §5.2). */
    char created_old[CBX_MAX_CONTROLLERS][CBX_MAX_PATH_LEN] = {{0}};
    char created_composite[CBX_MAX_CONTROLLERS][CBX_MAX_PATH_LEN] = {{0}};
    bool created_old_stopped[CBX_MAX_CONTROLLERS] = {false};
    int created_for_slot[CBX_MAX_DEVICES];
    for (int i = 0; i < CBX_MAX_DEVICES; i++)
        created_for_slot[i] = -1;
    int created_count = 0;
    for (int i = 0; i < orig_count; i++)
        snprintf(slots[i], sizeof(slots[i]), "%s", svc->model.targets[i].path);

    int rc = 0;
    const char *phase = "enumeration";
    const char *operation = "validate-topology";
    const char *kind = "";
    char path[CBX_MAX_PATH_LEN] = "";

    /* Extract each composite's source-derived identity once for this pass.
     * Assignment→composite attachment must resolve by physical identity
     * (SPEC §6.2), and a transient identity query failure must not match. */
    int identity_rc = overlay_validate_identities(svc);
    if (identity_rc != 0) {
        reconcile_diag(svc, "identity enumeration", "resolve-physical-identity",
                       "", "", orig_count, svc->model.target_count,
                       identity_rc, started, false);
        return identity_rc;
    }
    const cbx_composite_identity_entry *idents = svc->identities;
    int ident_count = svc->identity_count;

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
        created_for_slot[slot] = created_count;
        snprintf(path, sizeof(path), "%s", created[created_count++]);
        free(returned);
        phase = "publication-timeout"; operation = "confirm-created-path";
        rc = wait_for_exact_target(svc, path, kind, true);
        if (rc != 0)
            goto fail;
        fprintf(stderr,
                "controller-box: target-created slot=%d path=%s device-type=%s\n",
                slot, path, kind);
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
        int cidx = created_count;
        snprintf(created[cidx], sizeof(created[cidx]), "%s", returned);
        snprintf(created_old[cidx], sizeof(created_old[cidx]), "%s", old_path);
        snprintf(slots[slot], sizeof(slots[slot]), "%s", returned);
        created_for_slot[slot] = cidx;
        snprintf(path, sizeof(path), "%s", created[cidx]);
        created_count++;
        free(returned);
        operation = "confirm-replacement-publication";
        rc = wait_for_exact_target(svc, path, kind, true);
        if (rc != 0) goto fail;
        const char *assigned = assigned_composite_for_slot(idents,
                                                           ident_count,
                                                           &svc->assignments,
                                                           slot);
        if (assigned) {
            phase = "attachment"; operation = "Set TargetDevices property";
            rc = ip_composite_set_target_device_paths(svc->conn.backend,
                svc->conn.bus, assigned, path);
            if (rc != 0) goto fail;
            operation = "verify-exact-TargetDevices";
            rc = wait_for_attachment(svc, assigned, path);
            if (rc != 0) goto fail;
            snprintf(created_composite[cidx],
                     sizeof(created_composite[cidx]), "%s", assigned);
        }
        phase = "type-correction"; operation = "StopTargetDevice";
        snprintf(path, sizeof(path), "%s", old_path);
        rc = ip_manager_stop_target_device(svc->conn.backend, svc->conn.bus,
                                             old_path);
        if (rc != 0) goto fail;
        svc->reconcile_status.originals_stopped = true;
        created_old_stopped[cidx] = true;
        operation = "confirm-old-path-removal";
        rc = wait_for_exact_target(svc, old_path, kind, false);
        if (rc != 0) goto fail;
    }

    /* Attach only explicitly persisted physical assignments.  Unassigned
     * virtual slots remain created and ready without a composite. */
    for (int slot = 0; slot < desired; slot++) {
        const char *assigned = assigned_composite_for_slot(idents,
                                                           ident_count,
                                                           &svc->assignments,
                                                           slot);
        if (!assigned) continue;
        memcpy(path, slots[slot], sizeof(path));
        path[sizeof(path) - 1] = '\0';
        phase = "attachment"; operation = "Set TargetDevices property";
        rc = ip_composite_set_target_device_paths(svc->conn.backend,
            svc->conn.bus, assigned, path);
        if (rc != 0) goto fail;
        operation = "verify-exact-TargetDevices";
        rc = wait_for_attachment(svc, assigned, path);
        if (rc != 0) goto fail;
        int cidx = created_for_slot[slot];
        if (cidx >= 0 && cidx < created_count)
            snprintf(created_composite[cidx],
                     sizeof(created_composite[cidx]), "%s", assigned);
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
    /* A composite topology change during target publication invalidates the
     * physical snapshot.  Retry rather than index identities from the old
     * enumeration into a different set/order of composite paths. */
    bool same_composites = svc->model.composite_count == svc->identity_count;
    for (int i = 0; same_composites && i < svc->identity_count; i++)
        same_composites = strcmp(svc->model.composites[i].path,
                                 svc->identities[i].path) == 0;
    if (!same_composites) {
        svc->identities_valid = false;
        rc = -EAGAIN; phase = "identity enumeration";
        operation = "confirm-composite-snapshot";
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
         * enumeration failed.  Roll back each one independently.  A target
         * the composite still routes to is first un-routed, so the engine is
         * never left pointing at a target this pass then stops:
         *   - a replacement whose original stop was already issued is kept
         *     (it is the slot's only live target and matches the desired
         *     type) instead of being stopped under a live route;
         *   - a replacement whose original is still live has the composite
         *     restored to that original before the replacement is stopped;
         *   - a new slot's target is detached to Unassigned before stopping.
         * A routing restore that cannot be verified is surfaced with the
         * exact cleanup operation and leaves the target live. */
        for (int i = created_count - 1; i >= 0; i--) {
            const char *created_path = created[i];
            const char *old_path = created_old[i];
            const char *composite = created_composite[i];

            if (old_path[0] && created_old_stopped[i]) {
                /* Irreversible type correction: the replaced original's stop
                 * was accepted.  Keep the replacement routed and live. */
                svc->reconcile_status.originals_stopped = true;
                continue;
            }

            if (composite[0]) {
                char *td = NULL;
                int qrc = ip_composite_get_target_devices(svc->conn.backend,
                    svc->conn.bus, composite, &td);
                if (qrc != 0) {
                    /* Cannot confirm routing: do not stop a possibly-routed
                     * target.  Surface the exact failed lookup. */
                    svc->reconcile_status.cleanup_failures++;
                    if (svc->reconcile_status.cleanup_operation[0] == '\0') {
                        svc->reconcile_status.cleanup_rc = qrc;
                        snprintf(svc->reconcile_status.cleanup_operation,
                                 sizeof(svc->reconcile_status.cleanup_operation),
                                 "%s", "query-TargetDevices");
                        snprintf(svc->reconcile_status.cleanup_path,
                                 sizeof(svc->reconcile_status.cleanup_path),
                                 "%s", composite);
                    }
                    fprintf(stderr,
                        "controller-box: reconcile rollback routing query failed composite=%s failure=%s rc=%d\n",
                        composite, reconcile_error_category(qrc), qrc);
                    continue;
                }
                bool routed = csv_is_exact_singleton(td, created_path);
                free(td);
                if (routed) {
                    const char *restore = old_path[0] ? old_path : "";
                    int rrc = ip_composite_set_target_device_paths(
                        svc->conn.backend, svc->conn.bus, composite, restore);
                    if (rrc == 0)
                        rrc = wait_for_attachment(svc, composite,
                                                  old_path[0] ? old_path : NULL);
                    if (rrc != 0) {
                        /* Cannot compensate routing: keep the replacement
                         * live so the composite is not left pointing at a
                         * stopped target. */
                        svc->reconcile_status.cleanup_failures++;
                        if (svc->reconcile_status.cleanup_operation[0] == '\0') {
                            svc->reconcile_status.cleanup_rc = rrc;
                            snprintf(svc->reconcile_status.cleanup_operation,
                                     sizeof(svc->reconcile_status.cleanup_operation),
                                     "%s", "restore-routing");
                            snprintf(svc->reconcile_status.cleanup_path,
                                     sizeof(svc->reconcile_status.cleanup_path),
                                     "%s", composite);
                        }
                        fprintf(stderr,
                            "controller-box: reconcile rollback routing restore failed composite=%s target=%s failure=%s rc=%d\n",
                            composite, created_path,
                            reconcile_error_category(rrc), rrc);
                        continue;
                    }
                }
            }

            int cleanup_rc = ip_manager_stop_target_device(svc->conn.backend,
                svc->conn.bus, created_path);
            if (cleanup_rc == 0)
                cleanup_rc = wait_for_exact_target(svc, created_path, "", false);
            if (cleanup_rc != 0) {
                svc->reconcile_status.cleanup_failures++;
                if (svc->reconcile_status.cleanup_operation[0] == '\0') {
                    svc->reconcile_status.cleanup_rc = cleanup_rc;
                    snprintf(svc->reconcile_status.cleanup_operation,
                             sizeof(svc->reconcile_status.cleanup_operation),
                             "%s", "StopTargetDevice");
                    snprintf(svc->reconcile_status.cleanup_path,
                             sizeof(svc->reconcile_status.cleanup_path),
                             "%s", created_path);
                }
                fprintf(stderr,
                    "controller-box: reconcile rollback cleanup failed path=%s failure=%s rc=%d\n",
                    created_path, reconcile_error_category(cleanup_rc), cleanup_rc);
            }
        }
        (void)reconcile_enumerate(svc, NULL);
        snprintf(svc->reconcile_status.detail,
                 sizeof(svc->reconcile_status.detail),
                 "%.*s cleanup_failures=%d originals_stopped=%s cleanup_op=%.24s cleanup_rc=%d",
                 150, primary_detail, svc->reconcile_status.cleanup_failures,
                 svc->reconcile_status.originals_stopped ? "yes" : "no",
                 svc->reconcile_status.cleanup_operation[0]
                     ? svc->reconcile_status.cleanup_operation : "none",
                 svc->reconcile_status.cleanup_rc);
        fprintf(stderr, "controller-box: virtual-controller reconcile failed: %s\n",
                svc->reconcile_status.detail);
        return primary_rc;
    }
}

/* ================================================================== */
/*  Hotplug reconciliation (Task 9)                                     */
/* ================================================================== */

/*
 * Stop every intercept poll and clear the managed count.  Iterating the full
 * fixed poll array (not poll_count) guarantees that no armed timer survives a
 * partial/failed rearm, a rebuild, or shutdown even if an earlier sparse
 * start left a poll armed at a non-contiguous index.  stop() is a no-op for
 * a slot that was never armed.
 */
static void
overlay_stop_all_polls(cbx_overlay_service_ctx *svc)
{
    for (int i = 0; i < CBX_MAX_COMPOSITES; i++)
        ip_intercept_poll_stop(&svc->polls[i]);
    svc->poll_count = 0;
}

/*
 * Helper: (re)initialize all intercept polls for the current composites.
 * Stops any existing polls first, then creates one per composite with
 * per-composite activation context so close sets PASS on the correct
 * composite (SPEC §2.5).
 */
int
cbx_overlay_rearm_polls(cbx_overlay_service_ctx *svc)
{
    if (!svc)
        return -EINVAL;

    overlay_stop_all_polls(svc);

    if (svc->poll_event_type == (uint32_t)-1)
        return -EIO;

    int polls_to_arm = svc->comp_count;
    if (polls_to_arm > CBX_MAX_COMPOSITES)
        return -EIO;

    for (int i = 0; i < polls_to_arm; i++) {
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
                                     svc->poll_event_type) != 0) {
            /* A sparse partial arming is not readiness: a successfully
             * started poll at an earlier index must not stay armed outside
             * the managed poll_count/cleanup bounds.  Tear every timer down
             * so the caller sees a clean IDLE state to retry from. */
            overlay_stop_all_polls(svc);
            return -EIO;
        }
    }
    svc->poll_count = polls_to_arm;

    return 0;
}

/* Forward declaration: the readiness diagnostic helper is defined below,
 * after the hotplug reconciler that also records required-step failures. */
static void overlay_set_readiness_detail(cbx_overlay_service_ctx *svc,
                                         const char *phase, int rc);

/*
 * Reconcile the overlay state after a hotplug event modifies the device
 * model (SPEC §10.1: InterfacesAdded/InterfacesRemoved).  Re-enumerates,
 * rebuilds grid rows/columns, input mappings, triggers, and polls.
 *
 * If the overlay is visible, the grid is rebuilt with dynamic columns
 * preserving profiles.  If idle, a full rebuild from assignments is safe.
 *
 * Returns 0 when every required step succeeded.  A DbusDevices probe
 * failure fails closed: the stale input map is abandoned, the failing phase
 * is recorded in svc->readiness_detail, and the negative errno lets the
 * caller keep operations disabled and schedule a bounded retry.
 */
static int
cbx_overlay_reconcile_hotplug(cbx_overlay_service_ctx *svc)
{
    if (!svc || !svc->conn.backend || !svc->conn.bus)
        return -EINVAL;

    /* Bound the whole hotplug pass: identity extraction, grid rebuild, input
     * map, PASS writes and trigger registration all issue synchronous DBus
     * calls on the UI thread. */
    overlay_set_call_deadline(svc, overlay_pass_deadline_ms(svc));

    /* The device model was already updated incrementally by ip_hotplug
     * (SPEC §10.1: InterfacesAdded/InterfacesRemoved).  No full
     * re-enumeration needed — use the current model state directly. */
    bool identity_changed = svc->identity_reconcile_pending ||
                             svc->hp.identity_changed || !svc->identities_valid ||
                             svc->identity_count != svc->model.composite_count;
    for (int i = 0; !identity_changed && i < svc->identity_count; i++)
        identity_changed = strcmp(svc->identities[i].path,
                                   svc->model.composites[i].path) != 0;
    /* Consume only this pass's signals.  Synchronous attachment waits can
     * dispatch new changes; those must remain pending for the next pass. */
    svc->identity_reconcile_pending = false;
    svc->hp.model_changed = false;
    svc->hp.identity_changed = false;
    if (identity_changed) {
        int identity_rc = overlay_validate_identities(svc);
        if (identity_rc != 0) {
            overlay_set_readiness_detail(svc, "identity enumeration",
                                         identity_rc);
            overlay_set_call_deadline(svc, 0);
            return identity_rc;
        }
    }

    /* Update composite info from the device model.  Target-only hotplug does
     * not invalidate physical identities; retain the already resolved rows
     * so an unrelated source-property read cannot erase assignments. */
    int old_comp_count = svc->comp_count;
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
    if (identity_changed || old_comp_count != new_comp_count) {
        for (int i = 0; i < new_comp_count; i++)
            fill_composite_info(&svc->composites[i],
                                 &svc->model.composites[i],
                                 overlay_identity_for(svc, i),
                                 svc->conn.backend, svc->conn.bus);
    }

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

    /* Re-resolve host-mode row indices against the rebuilt grid.  A
     * hotplug add/remove can shift or delete rows, so the stored host and
     * selected indices may no longer name the same physical controller.
     * Reconcile by persistent identity, exiting host mode when the host is
     * gone so no other controller inherits host privileges and input never
     * freezes (SPEC §4.4/§10.1). */
    cbx_host_mode_reconcile(&svc->hm, &svc->grid);

    /* Re-detect conflicts after grid rebuild (SPEC §4.5). */
    cbx_conflict_list_init(&svc->conflicts);
    cbx_conflict_detect(&svc->grid, &svc->conflicts);

    /* Make the reconnected controller's persisted slot and profile effective
     * on the live engine immediately (SPEC §6.2/§10.1).  The rebuilt grid
     * already shows the saved column, but without this engine apply a
     * controller added mid-session would not control its virtual gamepad
     * until the overlay is next saved.  Pass clear_all=false so adding or
     * removing one controller never tears down the routing of controllers
     * that are already attached.  Apply even when the grid has no assigned
     * rows: a confirmed topology shrink must publish an empty GamepadOrder
     * rather than leaving a removed path live in InputPlumber. */
    int apply_rc = overlay_apply_grid_engine(svc, false, true);
    if (apply_rc != 0) {
        overlay_set_readiness_detail(svc, "assignment restoration", apply_rc);
        overlay_set_call_deadline(svc, 0);
        return apply_rc;
    }

    /* Rebuild input map.  A DbusDevices probe failure is a required-step
     * failure (SPEC §10.1): without the path→row map this controller's
     * InputEvent navigation would be silently dropped, so fail closed
     * instead of presenting a ready overlay that ignores input. */
    int rc = cbx_overlay_input_build_map(svc->conn.backend, svc->conn.bus,
                                          svc->composites, svc->comp_count,
                                          &svc->input_ctx);
    if (rc != 0) {
        overlay_set_readiness_detail(svc, "input mapping", rc);
        overlay_set_call_deadline(svc, 0);
        return rc;
    }

    /* Re-register triggers on all composites and set PASS (SPEC §2.5). */
    set_all_pass(svc->conn.backend, svc->conn.bus,
                  svc->model.composites, svc->comp_count);
    for (int i = 0; i < svc->comp_count; i++)
        cbx_trigger_register(svc->conn.backend, svc->conn.bus,
                              svc->composites[i].composite_path,
                              svc->settings.overlay_trigger);

    /* Re-arm polls for current composites (SPEC §2.5, §10.1). */
    cbx_overlay_rearm_polls(svc);

    /* Hotplug reconcile rebuilt grid rows, columns and input mappings, so
     * the surface is stale: dirty it to re-render from the new device model
     * (SPEC §4.9 device-change trigger). */
    cbx_overlay_surface_mark_dirty_all(&svc->surface);
    overlay_set_call_deadline(svc, 0);
    return 0;
}

static void
overlay_backend_degraded(const char *reason, void *userdata)
{
    cbx_overlay_service_ctx *svc = userdata;
    if (!svc)
        return;
    const char *msg = reason ? reason : "InputPlumber unavailable";
    fprintf(stderr, "controller-box: %s; waiting for recovery\n", msg);
    /* Callers may pass svc->readiness_detail itself as `reason`; avoid an
     * overlapping snprintf (source == destination is undefined). */
    if (msg != svc->readiness_detail)
        snprintf(svc->readiness_detail, sizeof(svc->readiness_detail),
                 "%s", msg);
    svc->backend_ready = false;
    if (svc->initialized)
        cbx_overlay_lifecycle_force_close(&svc->lifecycle);
    SDL_HideWindow(svc->rend.window);
}

/* Record an actionable readiness/recovery failure diagnostic. */
static void
overlay_set_readiness_detail(cbx_overlay_service_ctx *svc, const char *phase,
                             int rc)
{
    if (!svc)
        return;
    snprintf(svc->readiness_detail, sizeof(svc->readiness_detail),
             "%s failed: %s (%d)", phase ? phase : "recovery",
             ip_connection_reason_for_error(rc), rc);
}

/* Apply saved slots/profiles and physical order without rewriting durable
 * preferences, including preferences for disconnected controllers. */
static int
overlay_restore_assignments(cbx_overlay_service_ctx *svc)
{
    if (!svc)
        return -EINVAL;

    cbx_conflict_list_init(&svc->conflicts);
    cbx_conflict_detect(&svc->grid, &svc->conflicts);
    cbx_conflict_resolve(&svc->grid, &svc->conflicts);
    return overlay_apply_grid_engine(svc, true, true);
}

/*
 * Run every required post-rebuild step once the owner is verified and the
 * device model/grid are current.  This is the single definition of the
 * fail-closed readiness sequence shared by startup and recovery: assignment
 * and gamepad-order restoration, input mapping, both required signal
 * subscriptions, trigger registration, intercept-poll arming and the
 * PropertiesChanged wiring.
 *
 * Stops at the first failure, records the failing phase in
 * svc->readiness_detail, and returns the negative errno so the caller keeps
 * operations disabled and retries within the bounded readiness window.
 * Returns 0 when the service is fully wired.
 */
static int
overlay_wire_required_steps(cbx_overlay_service_ctx *svc)
{
    if (!svc || !svc->conn.backend || !svc->conn.bus)
        return -EINVAL;

    /* Apply persisted slot/profile assignments first.  This establishes a
     * complete, verified current topology but does not persist any new
     * preference.  In particular, it must not replace a saved physical
     * GamepadOrder with the current grid order before restoration. */
    int rc = overlay_restore_assignments(svc);
    if (rc != 0) {
        overlay_set_readiness_detail(svc, "assignment restoration", rc);
        return rc;
    }

    /* Required input mapping.  A DbusDevices probe failure is distinct
     * from a legitimately empty device list: without the path→row map the
     * controller's navigation events are dropped, so record the failure and
     * keep the service disabled rather than advertising readiness. */
    rc = cbx_overlay_input_build_map(svc->conn.backend, svc->conn.bus,
                                      svc->composites, svc->comp_count,
                                      &svc->input_ctx);
    if (rc != 0) {
        overlay_set_readiness_detail(svc, "input mapping", rc);
        return rc;
    }

    const char *uniq = ip_connection_get_unique_name(&svc->conn);
    snprintf(svc->expected_sender, sizeof(svc->expected_sender), "%s",
             uniq ? uniq : "");

    /* Required input subscription: without it the overlay cannot receive
     * navigation events over the intercept channel. */
    ip_input_events_init(&svc->input_events, svc->conn.backend, svc->conn.bus,
                          svc->expected_sender, cbx_overlay_input_cb,
                          &svc->input_ctx);
    rc = ip_input_events_subscribe(&svc->input_events);
    if (rc != 0) {
        svc->input_events_ready = false;
        overlay_set_readiness_detail(svc, "InputEvent subscription", rc);
        return rc;
    }
    svc->input_events_ready = true;

    /* Required hotplug subscription: device add/remove must be observed. */
    ip_hotplug_init(&svc->hp, svc->conn.backend, svc->conn.bus,
                     svc->expected_sender, &svc->model);
    rc = ip_hotplug_subscribe(&svc->hp);
    if (rc != 0) {
        overlay_set_readiness_detail(svc, "hotplug subscription", rc);
        return rc;
    }

    /* Required trigger registration + PASS on every composite. */
    set_all_pass(svc->conn.backend, svc->conn.bus,
                  svc->model.composites, svc->comp_count);
    for (int i = 0; i < svc->comp_count; i++) {
        rc = cbx_trigger_register(svc->conn.backend, svc->conn.bus,
                                   svc->composites[i].composite_path,
                                   svc->settings.overlay_trigger);
        if (rc != 0) {
            overlay_set_readiness_detail(svc, "trigger registration", rc);
            return rc;
        }
    }

    /* Required intercept-poll arming. */
    rc = cbx_overlay_rearm_polls(svc);
    if (rc != 0) {
        overlay_set_readiness_detail(svc, "intercept poll arming", rc);
        return rc;
    }

    /* Reactive PropertiesChanged subscription: observe GamepadOrder,
     * ProfileName, ProfilePath, TargetDevices, SourceDevicePaths changes on
     * the live backend (SPEC §10.1). */
    rc = cbx_overlay_props_wire(svc);
    if (rc != 0) {
        overlay_set_readiness_detail(svc, "PropertiesChanged subscription", rc);
        return rc;
    }

    return 0;
}

/*
 * Perform one full backend recovery/readiness pass.  Owner identity, complete
 * enumeration, target reconciliation and the grid rebuild are required before
 * overlay_wire_required_steps() owns the post-rebuild readiness sequence.  A
 * failure stops at the first failing step, leaves svc->readiness_detail
 * describing the exact phase, and returns a negative errno so the caller can
 * keep operations disabled and retry within a bounded window.
 *
 * Returns 0 when the service is fully ready.
 */
static int
overlay_recover(cbx_overlay_service_ctx *svc)
{
    if (!svc || !svc->conn.backend || !svc->conn.bus)
        return -EINVAL;

    overlay_stop_all_polls(svc);

    /* One wall-clock deadline for the whole recovery pass: enumeration,
     * virtual-controller reconciliation, identity extraction, gamepad-order
     * restore and the required wiring steps are all synchronous DBus work on
     * the UI thread (SPEC §2.4: operational within two seconds). */
    overlay_set_call_deadline(svc, overlay_pass_deadline_ms(svc));
    int rc;

    /* The owner must be current and credential-verified; without a trusted
     * sender there is no safe endpoint to subscribe or route against. */
    if (!ip_connection_is_sender_verified(&svc->conn) ||
        !ip_connection_get_unique_name(&svc->conn)) {
        overlay_set_readiness_detail(svc, "owner verification",
                                     IP_ERR_UNVERIFIED);
        rc = IP_ERR_UNVERIFIED;
        goto out;
    }

    if (reconcile_enumerate(svc, NULL) != 0) {
        overlay_set_readiness_detail(svc, "enumeration", -EIO);
        rc = -EIO;
        goto out;
    }

    rc = cbx_reconcile_startup_targets(svc);
    if (rc != 0) {
        snprintf(svc->readiness_detail, sizeof(svc->readiness_detail), "%s",
                 svc->reconcile_status.detail[0]
                     ? svc->reconcile_status.detail
                     : "virtual-controller reconciliation failed");
        goto out;
    }

    svc->comp_count = svc->model.composite_count;
    if (svc->comp_count > CBX_MAX_COMPOSITES)
        svc->comp_count = CBX_MAX_COMPOSITES;
    for (int i = 0; i < svc->comp_count; i++)
        fill_composite_info(&svc->composites[i], &svc->model.composites[i],
                             overlay_identity_for(svc, i),
                             svc->conn.backend, svc->conn.bus);
    cbx_select_grid_build(&svc->grid, svc->composites, svc->comp_count,
                           &svc->settings, &svc->assignments);
    cbx_profile_cycle_load_profiles(&svc->grid, &svc->profiles);

    /* Backend recovery rebuilt the grid from a fresh enumeration.  Re-resolve
     * host mode by persistent identity for the same reason as a hotplug
     * rebuild (SPEC §4.4/§10.1). */
    cbx_host_mode_reconcile(&svc->hm, &svc->grid);

    /* Re-detect conflicts after grid rebuild (SPEC §4.5). */
    cbx_conflict_list_init(&svc->conflicts);
    cbx_conflict_detect(&svc->grid, &svc->conflicts);

    /* Re-initialise profile_cycle with current backend/bus pointers
     * so LoadProfilePath uses the live connection. */
    cbx_profile_cycle_init(&svc->profile_cycle, svc->conn.backend,
                            svc->conn.bus, &svc->assignments, &svc->profiles);

    rc = overlay_wire_required_steps(svc);

out:
    overlay_set_call_deadline(svc, 0);
    return rc;
}

/* Arm the bounded retry window after any failed readiness or DBus
 * processing step.  Re-arming is a no-op while a retry is already pending;
 * otherwise a failure that recurs every loop iteration would reset the
 * attempt budget each frame and the retry would never be bounded.  Attempts
 * are spaced across the two-second window so a transient failure that clears
 * in a few hundred ms is still retried. */
static void
overlay_schedule_recovery(cbx_overlay_service_ctx *svc)
{
    if (!svc || svc->recovery_pending)
        return;
    uint64_t now = reconcile_now_ms();
    svc->recovery_pending   = true;
    svc->recovery_attempts  = 0;
    svc->recovery_deadline_ms = now + CBX_RECONCILE_TIMEOUT_MS;
    svc->recovery_next_attempt_ms = now + CBX_RECOVERY_ATTEMPT_INTERVAL_MS;
}

static void
overlay_backend_ready(void *userdata)
{
    cbx_overlay_service_ctx *svc = userdata;
    if (!svc || !svc->initialized)
        return;

    int rc = overlay_recover(svc);
    if (rc != 0) {
        overlay_backend_degraded(
            svc->readiness_detail[0] ? svc->readiness_detail
                                     : "InputPlumber recovery failed", svc);
        overlay_schedule_recovery(svc);
        return;
    }

    svc->backend_ready = true;
    svc->recovery_pending  = false;
    svc->recovery_attempts = 0;
    svc->readiness_detail[0] = '\0';
    /* Backend recovery re-enumerated the device model: dirty the surface so
     * the next step re-renders from the recovered state (SPEC §4.9
     * device-change trigger). */
    cbx_overlay_surface_mark_dirty_all(&svc->surface);
}

/*
 * Bounded recovery retry: called once per overlay-service step.  While the
 * owner is still present and the retry deadline/attempt budget is not
 * exhausted, re-run the full recovery pass so a transient startup/recovery
 * failure becomes operational within the two-second readiness window.
 * Returns true if the service became ready on this tick.
 */
static bool
overlay_recovery_tick(cbx_overlay_service_ctx *svc)
{
    if (!svc || !svc->initialized || svc->backend_ready ||
        !svc->recovery_pending)
        return false;

    uint64_t now = reconcile_now_ms();
    if (svc->recovery_attempts >= CBX_RECOVERY_MAX_ATTEMPTS ||
        now >= svc->recovery_deadline_ms) {
        svc->recovery_pending = false;
        fprintf(stderr, "controller-box: recovery retries exhausted: %s\n",
                svc->readiness_detail[0] ? svc->readiness_detail
                                         : "unknown");
        return false;
    }

    /* Pace attempts across the readiness window so a transient failure that
     * clears in a few hundred ms is still retried instead of burning the
     * whole budget in consecutive UI frames.  A zero next-attempt time (a
     * manually seeded budget) attempts immediately. */
    if (svc->recovery_next_attempt_ms != 0 &&
        now < svc->recovery_next_attempt_ms)
        return false;

    /* If the owner vanished again, stop retrying until NameOwnerChanged
     * signals a new acquisition. */
    if (!ip_connection_is_connected(&svc->conn)) {
        svc->recovery_pending = false;
        return false;
    }

    svc->recovery_attempts++;
    svc->recovery_next_attempt_ms =
        now + CBX_RECOVERY_ATTEMPT_INTERVAL_MS;
    if (overlay_recover(svc) != 0)
        return false;

    svc->backend_ready = true;
    svc->recovery_pending  = false;
    svc->recovery_attempts = 0;
    svc->readiness_detail[0] = '\0';
    cbx_overlay_surface_mark_dirty_all(&svc->surface);
    return true;
}

#ifdef CBX_TESTING
void
cbx_overlay_install_recovery_callbacks(cbx_overlay_service_ctx *svc)
{
    if (!svc)
        return;
    ip_connection_set_reenumerate_cb(&svc->conn, overlay_backend_ready, svc);
    ip_connection_set_degraded_cb(&svc->conn, overlay_backend_degraded, svc);
}
#endif /* CBX_TESTING */

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
    case SDLK_LEFT:   *out = CBX_HM_LEFT;         return 1;
    case SDLK_RIGHT:  *out = CBX_HM_RIGHT;        return 1;
    case SDLK_UP:     *out = CBX_HM_UP;           return 1;
    case SDLK_DOWN:   *out = CBX_HM_DOWN;         return 1;
    /* Keyboard equivalents of the Host Mode L1/R1 profile-cycle affordance
     * (documented in host_mode.h): Q = previous, E = next. */
    case SDLK_q:      *out = CBX_HM_PROFILE_PREV; return 1;
    case SDLK_e:      *out = CBX_HM_PROFILE_NEXT; return 1;
    case SDLK_b:      *out = CBX_HM_B;            return 1;
    case SDLK_r:      *out = CBX_HM_R3;           return 1;
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
    int first_error = 0;

    for (int i = 0; i < comp_count; i++) {
        char *dbus_devices = NULL;
        int rc = ip_composite_get_dbus_devices(backend, bus,
                                                composites[i].composite_path,
                                                &dbus_devices);
        if (rc != 0) {
            /* Probe failure (e.g. transient AccessDenied/NoReply), not an
             * empty device list: surface the error so the caller fails
             * closed and retries.  Mappings already resolved for earlier
             * composites are retained (the header documents partial
             * population), but readiness never uses a partial map. */
            if (first_error == 0)
                first_error = rc;
            continue;
        }
        if (!dbus_devices)
            continue;  /* legitimate empty DbusDevices */

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

    return first_error;
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
    case IP_INPUT_LEFT:  *out = CBX_HM_LEFT;         return 1;
    case IP_INPUT_RIGHT: *out = CBX_HM_RIGHT;        return 1;
    case IP_INPUT_UP:    *out = CBX_HM_UP;           return 1;
    case IP_INPUT_DOWN:  *out = CBX_HM_DOWN;         return 1;
    /* Host Mode profile-cycle affordance (SPEC §4.4): L1 cycles to the
     * previous profile, R1 to the next, for the selected row. */
    case IP_INPUT_L1:    *out = CBX_HM_PROFILE_PREV; return 1;
    case IP_INPUT_R1:    *out = CBX_HM_PROFILE_NEXT; return 1;
    case IP_INPUT_B:     *out = CBX_HM_B;            return 1;
    case IP_INPUT_R3:    *out = CBX_HM_R3;           return 1;
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

        /* Pass the actual sending row (row_idx from the device path map),
         * not the host row.  This lets cbx_host_mode_handle apply its
         * freeze guard (row_idx != host_row -> FROZEN) so a frozen
         * (non-host) controller cannot move the host's selected row across
         * columns, navigate the host's profile, or exit host mode via R3. */
        int result = cbx_host_mode_handle(ctx->hm, row_idx, hm_in,
                                            ctx->grid);
        /* CBX_HM_RESULT_EXIT: cbx_host_mode_handle already exited host mode
         * (R3 case) and fired on_state_change exactly once so the surface is
         * dirty.  Do not exit again here — a second cbx_host_mode_exit would
         * double-fire the dirty trigger (W2: deliberate/consistent triggers). */
        if (result == CBX_HM_RESULT_CLOSE) {
            cbx_overlay_lifecycle_close(ctx->lifecycle);
        } else if (result == CBX_HM_RESULT_MOVED) {
            /* Row navigation fires no callback, so the dispatch dirties the
             * surface to reflect the moved SELECTED-row highlight (§4.10
             * transition).  SLOT and PROFILE are dirtied by their own
             * callbacks (on_slot_change / on_profile_change) and are
             * deliberately not double-marked here (W2 reconciliation). */
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
            cbx_host_mode_toggle_with_grid(ctx->hm, ctx->grid, row_idx);
        }
        /* Surface dirty flag is set by callbacks. */
    }
}

/* ================================================================== */
/*  Step function: process one iteration of the poll loop (Task 10)   */
/* ================================================================== */

/*
 * Decide whether a queued poll timer event may tick its poll.  The event is
 * accepted only when event.user.data1 names one of the service's fixed poll
 * slots (ownership — the array lives for the service lifetime, so the
 * pointer cannot dangle) and event.user.code still matches that slot's
 * current arm generation (a stop/rearm/rebuild advances it).  Naming the
 * policy makes the step loop read as intent and gives the generation load a
 * single race-free site.
 */
static bool
poll_event_targets_live_poll(cbx_overlay_service_ctx *svc,
                             const SDL_Event *ev)
{
    ip_intercept_poll *owner = (ip_intercept_poll *)ev->user.data1;
    if (!owner)
        return false;
    for (int i = 0; i < CBX_MAX_COMPOSITES; i++) {
        if (owner == &svc->polls[i])
            return (uint32_t)ev->user.code ==
                   atomic_load(&owner->generation);
    }
    return false;
}

/*
 * Recreate every GPU-owned asset after SDL_RENDER_DEVICE_RESET.  The SDL2
 * event contract says the device has been reset and all textures must be
 * recreated: the pre-built overlay target texture is rebuilt in place, and
 * the text/icon caches drop their textures so the next render re-uploads
 * them (fonts and the SVG rasterizer are CPU-side and stay valid).  Without
 * this, the next activation would present stale or blank text/icon regions
 * indefinitely, because a successful surface render clears the dirty list
 * and nothing marks it again.  The surface rebuild schedules a full repaint
 * so the very next step redraws the grid into the fresh target.
 */
static void
overlay_recover_device_reset(cbx_overlay_service_ctx *svc)
{
    int rc = cbx_overlay_surface_rebuild(&svc->surface, svc->rend.renderer);
    if (rc != 0)
        fprintf(stderr,
                "controller-box: overlay surface rebuild after device reset "
                "failed: %d\n", rc);

    cbx_text_cache_reset(&svc->text_cache);

    /* Keep the rasterizer and reload the mapped icons so grid icons are not
     * blank.  Custom on-demand PNGs are not in the map and reload lazily
     * through cbx_icon_lookup() on the next draw. */
    cbx_icon_cache_reset(&svc->icon_cache);
    if (svc->icon_cache.rasterizer)
        cbx_icon_cache_load(&svc->icon_cache, &svc->icon_map);
}

void
cbx_overlay_service_step(cbx_overlay_service_ctx *svc)
{
    SDL_Event ev;

    /* 1. Process all pending SDL events. */
    while (SDL_PollEvent(&ev)) {
        if (ev.type == svc->poll_event_type) {
            /* Tick only the poll that armed this timer event, so one 50 ms
             * event performs exactly one DBus read per device (linear reads).
             * The ownership+generation policy is in
             * poll_event_targets_live_poll. */
            if (poll_event_targets_live_poll(svc, &ev)) {
                ip_intercept_poll_tick((ip_intercept_poll *)ev.user.data1);
                /* poll_count is the number of armed slots and is used as
                 * the bound for every rearm/cleanup loop.  Keep the event
                 * diagnostic separate; incrementing poll_count per timer
                 * event eventually indexed beyond CBX_MAX_COMPOSITES. */
                svc->poll_ticks++;
                if (svc->poll_ticks % 100 == 0)
                    fprintf(stderr, "controller-box: poll tick #%llu\n",
                            (unsigned long long)svc->poll_ticks);
            }
        } else if (ev.type == SDL_QUIT) {
            g_running = 0;
        } else if (ev.type == SDL_RENDER_TARGETS_RESET) {
            /* Render targets were reset; their contents are undefined and
             * must be repainted.  The overlay surface is a render target,
             * so mark it fully dirty and let the step's render pass below
             * repaint it before the next present. */
            cbx_overlay_surface_mark_dirty_all(&svc->surface);
        } else if (ev.type == SDL_RENDER_DEVICE_RESET) {
            /* The whole graphics device was reset: every texture is invalid
             * and must be recreated (see helper). */
            overlay_recover_device_reset(svc);
        } else if (ev.type == SDL_KEYDOWN &&
                   cbx_overlay_lifecycle_is_active(&svc->lifecycle)) {
            /* Process input only when overlay is visible. */
            SDL_Keycode key = ev.key.keysym.sym;

            if (cbx_host_mode_is_active(&svc->hm)) {
                /* Host Mode: host navigates rows + slots. */
                cbx_hm_input hm_in;
                if (sdl_key_to_hm_input(key, &hm_in)) {
                    /* The SDL keyboard always represents the primary
                     * controller (row 0), matching the Player Mode keyboard
                     * path.  Pass that actual sending row rather than the
                     * host row so the freeze guard in cbx_host_mode_handle
                     * is exercised: a frozen (non-host) controller can no
                     * longer drive the host's selection or exit host mode. */
                    int result = cbx_host_mode_handle(
                        &svc->hm, 0, hm_in, &svc->grid);

                    /* CBX_HM_RESULT_EXIT is handled inside handle (R3 case):
                     * it already exited host mode and fired on_state_change to
                     * dirty the surface once.  Re-exiting here would double-
                     * fire the trigger (W2: deliberate/consistent). */
                    if (result == CBX_HM_RESULT_CLOSE) {
                        cbx_overlay_lifecycle_close(&svc->lifecycle);
                    } else if (result == CBX_HM_RESULT_MOVED) {
                        /* Row navigation fires no callback (see DBus path
                         * above); SLOT/PROFILE are covered by their
                         * callbacks and deliberately not double-marked. */
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
                        cbx_host_mode_toggle_with_grid(&svc->hm, &svc->grid, 0);
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
     *    dispatched and the service could not recover from degraded mode.
     *    The drain is bounded so a signal flood cannot starve UI work, and a
     *    DBus processing error is propagated into the degraded state rather
     *    than being swallowed. */
    int drain_rc = 0;
    if (svc->conn.backend && svc->conn.bus &&
        svc->conn.backend->process) {
        for (int i = 0; i < 64; i++) {
            int processed = svc->conn.backend->process(svc->conn.bus);
            if (processed < 0) {
                drain_rc = processed;
                break;
            }
            if (processed == 0)
                break;
        }
    }

    /* 3. Process pending DBus InputEvent signals (only if subscribed). */
    if (svc->input_events_ready) {
        int prc = ip_input_events_process(&svc->input_events);
        if (prc < 0 && drain_rc == 0)
            drain_rc = prc;
    }
    if (drain_rc < 0) {
        /* Degrade/notify only on the transition: a persistent processing
         * error must not re-run the degraded work and flood stderr every
         * frame.  overlay_schedule_recovery() also refuses to reset an
         * in-flight retry budget, so the retry stays bounded. */
        if (svc->backend_ready)
            overlay_backend_degraded(
                ip_connection_reason_for_error(drain_rc), svc);
        overlay_schedule_recovery(svc);
    }

    /* 3b. Bounded recovery retry: a failed startup/recovery attempt is
     * retried while the owner is present and the two-second budget allows,
     * so a transient failure becomes operational without a restart. */
    overlay_recovery_tick(svc);

    /* 4. Check if hotplug signals modified the device model and reconcile
     *    grid rows, columns, input mappings, triggers, and polls
     *    (SPEC §10.1: InterfacesAdded/InterfacesRemoved). */
    if (svc->hp.model_changed || svc->identity_reconcile_pending) {
        int hotplug_rc = cbx_overlay_reconcile_hotplug(svc);
        if (hotplug_rc != 0) {
            overlay_backend_degraded(
                svc->readiness_detail[0] ? svc->readiness_detail
                                         : "hotplug reconciliation failed",
                svc);
            overlay_schedule_recovery(svc);
        }
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
    fprintf(stderr, "controller-box: dbus connected=%d backend_ready=%d\n",
            svc->conn.bus ? 1 : 0, svc->backend_ready);

    /* Keep the service alive in degraded mode so NameOwnerChanged can
     * recover it without depending on a system/user unit relationship. */
    cbx_device_model_init(&svc->model);
    if (svc->backend_ready) {
        /* Use the same enumeration path as recovery so startup and recovery
         * cannot diverge (reconcile_enumerate preserves reactive property
         * state on a non-empty model; the model is empty here). */
        rc = reconcile_enumerate(svc, NULL);
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

    if (svc->backend_ready) {
        /* Bound the synchronous topology reconciliation. */
        overlay_set_call_deadline(svc, overlay_pass_deadline_ms(svc));
        rc = cbx_reconcile_startup_targets(svc);
        overlay_set_call_deadline(svc, 0);
    } else {
        rc = 0;
    }
    if (rc != 0) {
        fprintf(stderr,
                "controller-box: failed to reconcile virtual controllers: %d\n",
                rc);
        svc->backend_ready = false;
        if (svc->reconcile_status.detail[0]) {
            snprintf(svc->readiness_detail, sizeof(svc->readiness_detail),
                     "%s", svc->reconcile_status.detail);
        } else {
            overlay_set_readiness_detail(svc, "virtual-controller reconcile", rc);
        }
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
    fprintf(stderr, "controller-box: comp_count=%d composite_count=%d\n",
            svc->comp_count, svc->model.composite_count);
    if (svc->comp_count > CBX_MAX_COMPOSITES)
        svc->comp_count = CBX_MAX_COMPOSITES;
    if (svc->comp_count > 0)
        overlay_set_call_deadline(svc, overlay_pass_deadline_ms(svc));
    for (int i = 0; i < svc->comp_count; i++)
        fill_composite_info(&svc->composites[i], &svc->model.composites[i],
                             overlay_identity_for(svc, i),
                             svc->conn.backend, svc->conn.bus);
    overlay_set_call_deadline(svc, 0);

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

    /* Assignment restoration, trigger registration and PASS are required
     * readiness steps; they run once after the mode/lifecycle setup below so
     * startup and recovery share one fail-closed sequence
     * (overlay_wire_required_steps). */

    /* --- 9. Initialise overlay lifecycle ----------------------------- */
    const char *primary_path =
        svc->comp_count > 0 ? svc->composites[0].composite_path : "";
    cbx_overlay_lifecycle_init(&svc->lifecycle, svc->conn.backend, svc->conn.bus,
                               primary_path, &svc->surface, svc->rend.renderer);

    /* Activation must present the pre-built surface immediately.  SPEC
     * §4.9/§11 require ≤ 75 ms p99 / ≤ 100 ms max from button press and
     * < 10 ms p99 from ALL detection to the first compositor-visible frame;
     * a fade-in that ramps from alpha 0 would hide the overlay for its
     * whole duration.  Instant activation matches the pre-built design and
     * every other production call site (tests use fade_in_ms = 0). */
    svc->lifecycle.fade_in_ms = 0;

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
    /* Host Mode can edit the selected row's profile too (SPEC §4.4): reuse
     * the production profile-change callback (LoadProfilePath + assignment
     * persist + dirty trigger) exactly as Player Mode does. */
    svc->hm.on_profile_change   = cbx_overlay_on_profile_change;
    svc->hm.profile_change_data = svc;
    /* Host-mode enter/exit marks the pre-built surface dirty so the
     * presented frame reflects the HOST/SELECTED/FROZEN row visuals on
     * entry and reverts to Player Mode on exit (SPEC §4.4).  This is a
     * deliberate W2 extension of the §4.9 pre-build dirty policy. */
    svc->hm.on_state_change   = cbx_overlay_on_host_mode_change;
    svc->hm.state_change_data = svc;

    /* Lifecycle on_save callback: conflict resolution + assignment save.
     * The close path uses the bounded variant so the engine-apply chain is
     * deadline-limited after InterceptMode=PASS has already released input. */
    svc->lifecycle.on_save       = cbx_overlay_on_save_bounded;
    svc->lifecycle.on_save_data  = svc;

    /* Lifecycle on_closed callback: end Host Mode with the overlay so its
     * state cannot leak into the next activation (SPEC §4.4). */
    svc->lifecycle.on_closed      = cbx_overlay_on_lifecycle_closed;
    svc->lifecycle.on_closed_data = svc;

    /* --- 10b. Point the input map at the live mode/lifecycle objects -- */
    svc->input_ctx.pm         = &svc->pm;
    svc->input_ctx.hm         = &svc->hm;
    svc->input_ctx.grid       = &svc->grid;
    svc->input_ctx.lifecycle  = &svc->lifecycle;
    svc->input_ctx.path_count = 0;

    /* --- 11. Set up InterceptMode polling --------------------------- */
    svc->poll_event_type = SDL_RegisterEvents(1);
    svc->poll_count = 0;

    /* Install the production recovery callbacks before the shared readiness
     * sequence runs, so a required-step failure is observable and retryable. */
    svc->initialized = true;
    ip_connection_set_reenumerate_cb(&svc->conn, overlay_backend_ready, svc);
    ip_connection_set_degraded_cb(&svc->conn, overlay_backend_degraded, svc);

    /* The required readiness sequence (assignment restoration, input map,
     * required subscriptions, trigger registration, poll arming and
     * PropertiesChanged wiring) is defined once in
     * overlay_wire_required_steps() and shared with overlay_recover().  Bound
     * it with the same wall-clock budget as recovery so a hung reply cannot
     * stall startup past the advertised window. */
    if (svc->backend_ready) {
        overlay_set_call_deadline(svc, overlay_pass_deadline_ms(svc));
        int wire_rc = overlay_wire_required_steps(svc);
        overlay_set_call_deadline(svc, 0);
        if (wire_rc != 0) {
            fprintf(stderr, "controller-box: readiness steps failed: %d\n", wire_rc);
            svc->backend_ready = false;
        } else {
            fprintf(stderr, "controller-box: triggers registered, overlay ready\n");
        }
    }

    if (!svc->backend_ready) {
        const char *reason = svc->readiness_detail[0]
            ? svc->readiness_detail
            : ip_connection_reason_for_error(conn_rc);
        overlay_backend_degraded(reason, svc);
        overlay_schedule_recovery(svc);
    }

    /* --- 12. Poll loop (Task 4) -------------------------------------- */
    fprintf(stderr, "controller-box: entering main loop (backend_ready=%d, comp_count=%d)\n",
            svc->backend_ready, svc->comp_count);

    while (g_running) {
        cbx_overlay_service_step(svc);
        SDL_Delay(10);
    }

    /* --- 13. Clean shutdown ------------------------------------------- */
    /* Stop all InterceptMode poll timers (full fixed array, so a poll left
     * armed by a partial rearm cannot leak past shutdown). */
    overlay_stop_all_polls(svc);

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