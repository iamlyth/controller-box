/*
 * controllers_tab.c — Controllers tab for the Manager UI (SPEC §5.2).
 *
 * Lists current target devices (virtual controllers) with their type,
 * and provides Add / Remove / Change-type actions backed by
 * InputPlumber's DBus API.
 *
 * Task 35 — Controllers tab.
 */
#include "manager/controllers_tab.h"

#include <SDL2/SDL.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dbus/ip_manager.h"
#include "dbus/ip_target.h"
#include "dbus/ip_composite.h"
#include "dbus/ip_objectmanager.h"
#include "dbus/ip_connection.h"   /* ip_connection_reason_for_error */
#include "config/config_assignments.h"  /* cbx_assignments_load/save for auto-Unassign */
#include "config/config_io.h"          /* cross-process config transaction lock */
#include "identify/assign.h"           /* cbx_assign_find_index */
#include "identify/composite_identity.h" /* cbx_composite_identity_extract */

/* ------------------------------------------------------------------ */
/*  Layout constants                                                  */
/* ------------------------------------------------------------------ */

#define CBX_CT_LIST_H    400
#define CBX_CT_BTN_W    200
#define CBX_CT_BTN_H     44
#define CBX_CT_BTN_GAP   16
#define CBX_CT_LIST_Y    16
#define CBX_CT_OPERATION_TIMEOUT_MS 2000u
#define CBX_CT_OPERATION_POLL_MS      10u

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Parse a comma-separated string into an array of trimmed tokens. */
static int
parse_csv(const char *csv, char out[][CBX_MAX_TYPE_LEN], int max)
{
    if (!csv || !out)
        return 0;

    int count = 0;
    const char *p = csv;

    while (*p && count < max) {
        /* skip leading whitespace */
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;

        /* find end of token */
        const char *start = p;
        while (*p && *p != ',')
            p++;
        const char *end = p;

        /* trim trailing whitespace */
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            end--;

        int len = (int)(end - start);
        if (len > 0 && len < CBX_MAX_TYPE_LEN) {
            memcpy(out[count], start, (size_t)len);
            out[count][len] = '\0';
            count++;
        }

        if (*p == ',')
            p++;
    }

    return count;
}

/* Build a label string "name (type)" for a device list item. */
static void
format_device_label(char *buf, size_t buflen, const char *name,
                     const char *type)
{
    if (!buf || buflen == 0)
        return;
    if (type && type[0])
        snprintf(buf, buflen, "%s (%s)", name ? name : "?", type);
    else
        snprintf(buf, buflen, "%s", name ? name : "?");
}

/* ------------------------------------------------------------------ */
/*  Forward declarations for on_select callbacks                       */
/* ------------------------------------------------------------------ */

static void on_device_selected(cbx_widget *w, int index,
                                void *user_data);
static void on_type_pick_selected(cbx_widget *w, int index,
                                    void *user_data);

/* ------------------------------------------------------------------ */
/*  Error display helpers (SPEC §5.2: show the failed DBus operation)   */
/* ------------------------------------------------------------------ */

/* Show an operation error in the status label without disabling buttons. */
static void
show_action_error(cbx_controllers_tab *tab, const char *action, int rc)
{
    if (!tab || !action)
        return;
    const char *reason;
    if (rc == -EIO)
        reason = "InputPlumber did not confirm the operation";
    else if (rc == -EINVAL)
        reason = "invalid request";
    else
        reason = ip_connection_reason_for_error(rc);

    char msg[CBX_LABEL_TEXT_LEN];
    snprintf(msg, sizeof(msg), "%s failed: %s", action, reason);
    cbx_label_set_text(&tab->status_lbl, msg);
    cbx_widget_set_visible(&tab->status_lbl.base, true);
}

/* Clear any operation error message (safe when backend is available). */
static void
clear_action_error(cbx_controllers_tab *tab)
{
    if (!tab || !tab->backend)
        return;
    cbx_label_set_text(&tab->status_lbl, "");
    cbx_widget_set_visible(&tab->status_lbl.base, false);
}

/* ------------------------------------------------------------------ */
/*  Button callbacks                                                   */
/* ------------------------------------------------------------------ */

static void
on_add_pressed(cbx_widget *w, void *user_data)
{
    (void)w;
    cbx_controllers_tab *tab = (cbx_controllers_tab *)user_data;
    if (!tab)
        return;
    cbx_controllers_tab_begin_type_pick(tab, CBX_CT_ACTION_ADD);
}

static void
on_remove_pressed(cbx_widget *w, void *user_data)
{
    (void)w;
    cbx_controllers_tab *tab = (cbx_controllers_tab *)user_data;
    if (!tab)
        return;
    cbx_controllers_tab_sync_selection(tab);
    int idx = tab->selected_device;
    if (idx < 0 || idx >= cbx_controllers_tab_device_count(tab))
        return;
    int rc = cbx_controllers_tab_remove(tab, idx);
    if (rc != 0)
        show_action_error(tab, "Remove", rc);
    else
        clear_action_error(tab);
}

static void
on_change_type_pressed(cbx_widget *w, void *user_data)
{
    (void)w;
    cbx_controllers_tab *tab = (cbx_controllers_tab *)user_data;
    if (!tab)
        return;
    cbx_controllers_tab_sync_selection(tab);
    int idx = tab->selected_device;
    if (idx >= 0 && idx < cbx_controllers_tab_device_count(tab))
        cbx_controllers_tab_begin_type_pick(tab, CBX_CT_ACTION_CHANGE);
}

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                         */
/* ------------------------------------------------------------------ */

int
cbx_controllers_tab_init(cbx_controllers_tab *tab,
                           cbx_panel *panel,
                           const ip_dbus_backend *backend,
                           ip_bus_handle bus,
                           cbx_text_cache *cache,
                           const cbx_theme *theme,
                           int font_id)
{
    if (!tab || !panel)
        return -EINVAL;

    memset(tab, 0, sizeof(*tab));
    tab->backend = backend;
    tab->bus     = bus;
    tab->panel   = panel;
    tab->text_cache = cache;
    tab->theme   = theme;
    tab->font_id = font_id;
    tab->mode    = CBX_CT_MODE_LIST;
    tab->pending_action = CBX_CT_ACTION_NONE;
    tab->selected_device = -1;
    tab->selected_type   = -1;

    cbx_device_model_init(&tab->model);

    /* --- Device list ---------------------------------------------- */
    int rc = cbx_list_init(&tab->device_list, font_id, cache, theme);
    if (rc != 0)
        return rc;
    cbx_list_set_select_cb(&tab->device_list, on_device_selected);

    /* --- Type picker (hidden initially) --------------------------- */
    rc = cbx_list_init(&tab->type_picker, font_id, cache, theme);
    if (rc != 0) {
        cbx_widget_destroy(&tab->device_list.base);
        return rc;
    }
    cbx_widget_set_visible(&tab->type_picker.base, false);
    cbx_list_set_select_cb(&tab->type_picker, on_type_pick_selected);

    /* --- Buttons --------------------------------------------------- */
    rc = cbx_button_init(&tab->add_btn, "Add Controller", font_id,
                          cache, theme, on_add_pressed, tab);
    if (rc != 0) {
        cbx_widget_destroy(&tab->device_list.base);
        cbx_widget_destroy(&tab->type_picker.base);
        return rc;
    }

    rc = cbx_button_init(&tab->remove_btn, "Remove", font_id,
                          cache, theme, on_remove_pressed, tab);
    if (rc != 0) {
        cbx_widget_destroy(&tab->device_list.base);
        cbx_widget_destroy(&tab->type_picker.base);
        cbx_widget_destroy(&tab->add_btn.base);
        return rc;
    }

    rc = cbx_button_init(&tab->change_type_btn, "Change Type", font_id,
                          cache, theme, on_change_type_pressed, tab);
    if (rc != 0) {
        cbx_widget_destroy(&tab->device_list.base);
        cbx_widget_destroy(&tab->type_picker.base);
        cbx_widget_destroy(&tab->add_btn.base);
        cbx_widget_destroy(&tab->remove_btn.base);
        return rc;
    }

    rc = cbx_label_init(&tab->status_lbl, "", font_id, cache, theme);
    if (rc != 0) {
        cbx_widget_destroy(&tab->device_list.base);
        cbx_widget_destroy(&tab->type_picker.base);
        cbx_widget_destroy(&tab->add_btn.base);
        cbx_widget_destroy(&tab->remove_btn.base);
        cbx_widget_destroy(&tab->change_type_btn.base);
        return rc;
    }
    cbx_widget_set_visible(&tab->status_lbl.base, false);

    /* --- Add widgets to panel ------------------------------------- */
    cbx_panel_add_child(panel, &tab->device_list.base);
    cbx_panel_add_child(panel, &tab->add_btn.base);
    cbx_panel_add_child(panel, &tab->remove_btn.base);
    cbx_panel_add_child(panel, &tab->change_type_btn.base);
    cbx_panel_add_child(panel, &tab->type_picker.base);
    cbx_panel_add_child(panel, &tab->status_lbl.base);

    /* --- Layout --------------------------------------------------- */
    cbx_controllers_tab_layout(tab);

    /* --- Load supported types + refresh --------------------------- */
    if (backend && bus) {
        int load_rc = cbx_controllers_tab_load_supported_types(tab);
        int refresh_rc = load_rc == 0 ? cbx_controllers_tab_refresh(tab) : load_rc;
        if (refresh_rc == 0)
            cbx_controllers_tab_set_available(tab, true, NULL);
        else
            cbx_controllers_tab_set_available(tab, false,
                "InputPlumber enumeration/type query failed");
    } else {
        cbx_controllers_tab_set_available(tab, false,
                                           "InputPlumber unavailable — waiting for recovery");
    }

    return 0;
}

void
cbx_controllers_tab_shutdown(cbx_controllers_tab *tab)
{
    if (!tab)
        return;

    /* Remove children from panel. */
    if (tab->panel) {
        cbx_panel_remove_child(tab->panel, &tab->device_list.base);
        cbx_panel_remove_child(tab->panel, &tab->add_btn.base);
        cbx_panel_remove_child(tab->panel, &tab->remove_btn.base);
        cbx_panel_remove_child(tab->panel, &tab->change_type_btn.base);
        cbx_panel_remove_child(tab->panel, &tab->type_picker.base);
        cbx_panel_remove_child(tab->panel, &tab->status_lbl.base);
    }

    cbx_widget_destroy(&tab->device_list.base);
    cbx_widget_destroy(&tab->type_picker.base);
    cbx_widget_destroy(&tab->add_btn.base);
    cbx_widget_destroy(&tab->remove_btn.base);
    cbx_widget_destroy(&tab->change_type_btn.base);
    cbx_widget_destroy(&tab->status_lbl.base);

    memset(tab, 0, sizeof(*tab));
}

static int
csv_path_token_count(const char *csv)
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
csv_is_exact_singleton_path(const char *csv, const char *path)
{
    if (!csv || !path) return false;
    int tokens = 0, matches = 0;
    size_t plen = strlen(path);
    for (const char *p = csv; *p;) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        const char *end = strchr(p, ',');
        if (!end) end = p + strlen(p);
        while (end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;
        tokens++;
        if ((size_t)(end - p) == plen && memcmp(p, path, plen) == 0)
            matches++;
        p = *end ? end + 1 : end;
    }
    return tokens == 1 && matches == 1;
}

/* True when `path` is an exact CSV element of `csv` (not a substring of a
 * longer path). */
static bool
csv_contains_path(const char *csv, const char *path)
{
    if (!csv || !path || !path[0]) return false;
    size_t plen = strlen(path);
    for (const char *p = csv; *p;) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        const char *end = strchr(p, ',');
        if (!end) end = p + strlen(p);
        while (end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;
        if ((size_t)(end - p) == plen && memcmp(p, path, plen) == 0)
            return true;
        p = *end ? end + 1 : end;
    }
    return false;
}

/* Preserve already-established slot identities across unordered/reordered
 * ObjectManager dictionaries.  Newly returned CreateTargetDevice paths are
 * inserted explicitly by the operation that created them.  Across a fresh
 * process, ObjectManager parsing's lexical path order is the deterministic
 * fail-closed fallback because InputPlumber exposes no intrinsic slot ID. */
static void
preserve_target_order(cbx_device_model *next, const cbx_device_model *prior)
{
    cbx_device_entry ordered[CBX_MAX_DEVICES];
    bool used[CBX_MAX_DEVICES] = {false};
    int n = 0;
    for (int p = 0; p < prior->target_count; p++)
        for (int i = 0; i < next->target_count; i++)
            if (!used[i] && strcmp(prior->targets[p].path,
                                    next->targets[i].path) == 0) {
                ordered[n++] = next->targets[i]; used[i] = true; break;
            }
    for (int i = 0; i < next->target_count; i++)
        if (!used[i]) ordered[n++] = next->targets[i];
    memcpy(next->targets, ordered,
           (size_t)next->target_count * sizeof(next->targets[0]));
}

static int
refresh_until_path(cbx_controllers_tab *tab, const char *path, bool present,
                   const char *type)
{
    uint32_t started = SDL_GetTicks();
    for (;;) {
        int rc = cbx_controllers_tab_refresh(tab);
        if (rc == 0) {
            int found = -1;
            for (int i = 0; i < tab->model.target_count; i++)
                if (strcmp(tab->model.targets[i].path, path) == 0) {
                    found = i; break;
                }
            if ((found >= 0) == present) {
                if (!present || (!type || (found < tab->device_type_count &&
                    strcmp(tab->device_types[found], type) == 0)))
                    return 0;
            }
        }
        if ((uint32_t)(SDL_GetTicks() - started) >=
            CBX_CT_OPERATION_TIMEOUT_MS)
            return -EIO;
        if (tab->backend->process) {
            int prc = tab->backend->process(tab->bus);
            if (prc < 0)
                return prc;
        }
        SDL_Delay(CBX_CT_OPERATION_POLL_MS);
    }
}

static int
wait_exact_attachment(cbx_controllers_tab *tab, const char *composite,
                      const char *target)
{
    uint32_t started = SDL_GetTicks();
    for (;;) {
        char *csv = NULL;
        int rc = ip_composite_get_target_devices(tab->backend, tab->bus,
                                                   composite, &csv);
        bool exact = rc == 0 &&
            (target ? csv_is_exact_singleton_path(csv, target)
                    : csv_path_token_count(csv) == 0);
        free(csv);
        if (exact) return 0;
        if ((uint32_t)(SDL_GetTicks() - started) >=
            CBX_CT_OPERATION_TIMEOUT_MS)
            return -EIO;
        if (tab->backend->process) {
            int prc = tab->backend->process(tab->bus);
            if (prc < 0)
                return prc;
        }
        SDL_Delay(CBX_CT_OPERATION_POLL_MS);
    }
}

/* Return the id assigned to `slot` in `asgn` (first match), or 0 if none. */
static int
assigned_id_for_slot(const cbx_assignments *asgn, int slot,
                     char out[CBX_MAX_ID_LEN])
{
    for (int ai = 0; ai < asgn->assignment_count; ai++) {
        if (asgn->assignments[ai].slot != slot)
            continue;
        snprintf(out, CBX_MAX_ID_LEN, "%s", asgn->assignments[ai].id);
        return 1;
    }
    return 0;
}

/* Resolve a persisted identity id to a currently enumerated composite path
 * using the composite's source-derived physical identity (SPEC §6.2), not the
 * opaque PersistentId.  A transient identity query failure is never matched:
 * it must not route a slot onto the wrong physical controller. */
static bool
composite_path_for_id(cbx_controllers_tab *tab, const char *id,
                      char out[CBX_MAX_PATH_LEN])
{
    if (!tab || !id || !id[0])
        return false;
    if (tab->model.composite_count < 0 ||
        tab->model.composite_count > CBX_MAX_COMPOSITES)
        return false;

    /* Extract every composite's source-derived identity once, then apply the
     * shared match rule (composite_identity.h) so routing uses the same
     * absence-vs-failure and duplicate policy as overlay restoration.  A
     * transient identity query failure is never matched: it must not route a
     * slot onto the wrong physical controller. */
    cbx_composite_identity_entry entries[CBX_MAX_COMPOSITES];
    int count = 0;
    if (cbx_model_extract_identities(tab->backend, tab->bus, &tab->model,
                                     entries, &count) != 0)
        return false;

    return cbx_composite_identity_resolve_id(entries, count, id, out,
                                             CBX_MAX_PATH_LEN, NULL) == 1;
}

static int
assigned_composite_for_slot(cbx_controllers_tab *tab, int slot,
                            char out[CBX_MAX_PATH_LEN])
{
    cbx_assignments asgn;
    cbx_assignments_init(&asgn);
    if (cbx_assignments_load(&asgn) != 0)
        return 0;

    char id[CBX_MAX_ID_LEN];
    if (!assigned_id_for_slot(&asgn, slot, id))
        return 0;
    return composite_path_for_id(tab, id, out) ? 1 : 0;
}

/* True when `path` is present in the tab's current (refreshed) model. */
static bool
ct_model_has_path(const cbx_controllers_tab *tab, const char *path)
{
    if (!tab || !path || !path[0])
        return false;
    for (int i = 0; i < tab->model.target_count; i++)
        if (strcmp(tab->model.targets[i].path, path) == 0)
            return true;
    return false;
}

/* Stop a target created by this operation and confirm its exact removal.
 * Returns 0 when the target is confirmed gone, negative errno otherwise. */
static int
ct_stop_created(cbx_controllers_tab *tab, const char *path)
{
    int rc = ip_manager_stop_target_device(tab->backend, tab->bus, path);
    if (rc == 0)
        rc = refresh_until_path(tab, path, false, NULL);
    return rc;
}

/* Make `composite` route exactly to `path` (NULL/"" clears it) and verify the
 * readback.  A confirmed route must exist before a replacement is stopped so
 * the engine is never left pointing at a stopped target (SPEC §5.2). */
static int
ct_restore_route(cbx_controllers_tab *tab, const char *composite,
                 const char *path)
{
    int rc = ip_composite_set_target_device_paths(tab->backend, tab->bus,
                                                   composite,
                                                   path ? path : "");
    if (rc == 0)
        rc = wait_exact_attachment(tab, composite, path);
    return rc;
}

/* Disable topology mutation until a fresh enumeration confirms recovery.
 * Used after an irreversible/late failure so the UI never advertises a
 * fictitious preserved live target (SPEC §5.2). */
static void
ct_disable_until_reenumerated(cbx_controllers_tab *tab, const char *reason)
{
    if (tab)
        cbx_controllers_tab_set_available(tab, false, reason);
}

/* ------------------------------------------------------------------ */
/*  Refresh                                                            */
/* ------------------------------------------------------------------ */

void
cbx_controllers_tab_set_available(cbx_controllers_tab *tab,
                                   bool available, const char *reason)
{
    if (!tab)
        return;
    tab->add_btn.base.interactive = available;
    tab->remove_btn.base.interactive = available;
    tab->change_type_btn.base.interactive = available;
    cbx_widget_set_visible(&tab->add_btn.base, available);
    cbx_widget_set_visible(&tab->remove_btn.base, available);
    cbx_widget_set_visible(&tab->change_type_btn.base, available);
    cbx_widget_set_visible(&tab->status_lbl.base, !available ||
                            (reason && reason[0]));
    cbx_label_set_text(&tab->status_lbl,
                       reason && reason[0] ? reason : "");
}

/* Check for orphan columns: when expected_target_count is set and the
 * actual target count is lower, show an error in the status label. */
static void
check_orphan_columns(cbx_controllers_tab *tab)
{
    if (!tab || tab->expected_target_count <= 0)
        return;
    if (tab->model.target_count < tab->expected_target_count) {
        char msg[CBX_LABEL_TEXT_LEN];
        snprintf(msg, sizeof(msg),
                 "Topology incomplete: %d of %d virtual controllers active",
                 tab->model.target_count,
                 tab->expected_target_count);
        cbx_label_set_text(&tab->status_lbl, msg);
        cbx_widget_set_visible(&tab->status_lbl.base, true);
    } else if (strncmp(tab->status_lbl.text, "Topology incomplete:",
                       strlen("Topology incomplete:")) == 0) {
        cbx_label_set_text(&tab->status_lbl, "");
        cbx_widget_set_visible(&tab->status_lbl.base, false);
    }
}

/* Return the validated profile currently routed through `target_path` by
 * any composite's reactive TargetDevices set, else "".  Matching is by
 * exact CSV element so one target cannot match a substring of another. */
static const char *
composite_profile_for_target(const cbx_controllers_tab *tab,
                             const char *target_path)
{
    if (!tab || !target_path || !target_path[0])
        return "";
    for (int i = 0; i < tab->model.composite_count; i++) {
        const cbx_composite_entry *e = &tab->model.composites[i];
        if (!e->has_target_devices || e->target_devices[0] == '\0')
            continue;
        const char *p = e->target_devices;
        while (*p) {
            const char *comma = strchr(p, ',');
            size_t len = comma ? (size_t)(comma - p) : strlen(p);
            if (len == strlen(target_path) &&
                strncmp(p, target_path, len) == 0) {
                if (e->has_profile_name && e->profile_name[0])
                    return e->profile_name;
                if (e->has_profile_path && e->profile_path[0]) {
                    const char *base = strrchr(e->profile_path, '/');
                    return base ? base + 1 : e->profile_path;
                }
                return "";
            }
            if (!comma)
                break;
            p = comma + 1;
        }
    }
    return "";
}

void
cbx_controllers_tab_refresh_labels(cbx_controllers_tab *tab)
{
    if (!tab)
        return;

    /* A live PropertiesChanged rebuilds the row labels but must not move
     * the user's highlight.  Remember the selected target by exact path
     * (the model order does not change on a property update) and restore it
     * after the rebuild.  Without this the widget silently drops to row 0
     * while tab->selected_device keeps its stale index, and the next
     * Remove/Change-Type sync from the widget would act on the wrong
     * device.  Prefer the widget's own highlight (what the user sees) and
     * fall back to the tab's synced index. */
    int keep = cbx_list_get_selected(&tab->device_list);
    if (keep < 0 || keep >= tab->model.target_count)
        keep = tab->selected_device;
    char selected_path[CBX_MAX_PATH_LEN] = "";
    if (keep >= 0 && keep < tab->model.target_count)
        snprintf(selected_path, sizeof(selected_path), "%s",
                 tab->model.targets[keep].path);

    cbx_list_clear(&tab->device_list);
    for (int i = 0; i < tab->model.target_count; i++) {
        char label[CBX_CT_LABEL_LEN];
        const char *type = (i < tab->device_type_count)
                            ? tab->device_types[i] : NULL;
        format_device_label(label, sizeof(label),
                              tab->model.targets[i].name, type);

        /* Annotate the routed physical controller's profile when the
         * reactive per-device model knows it, so a PropertiesChanged for
         * ProfileName/ProfilePath/TargetDevices updates the displayed row. */
        const char *profile =
            composite_profile_for_target(tab, tab->model.targets[i].path);
        if (profile && profile[0]) {
            size_t used = strlen(label);
            if (used < sizeof(label))
                snprintf(label + used, sizeof(label) - used,
                         " [%s]", profile);
        }

        /* Selection callback receives its owning tab through item user_data. */
        cbx_list_add_item(&tab->device_list, label, NULL, tab);
    }

    int restored = -1;
    if (selected_path[0]) {
        for (int i = 0; i < tab->model.target_count; i++)
            if (strcmp(tab->model.targets[i].path, selected_path) == 0) {
                restored = i;
                break;
            }
    }
    if (restored < 0 && tab->model.target_count > 0)
        restored = 0;
    cbx_list_set_selected(&tab->device_list, restored);
    tab->selected_device = restored;
}

void
cbx_controllers_tab_set_expected_count(cbx_controllers_tab *tab,
                                         int count)
{
    if (!tab)
        return;
    tab->expected_target_count = count;
    /* Re-check topology immediately in case the tab was already
     * refreshed during init (before expected count was set). */
    check_orphan_columns(tab);
}

void
cbx_controllers_tab_layout(cbx_controllers_tab *tab)
{
    if (!tab || !tab->panel)
        return;

    SDL_Rect panel_rect;
    cbx_widget_get_rect(&tab->panel->base, &panel_rect);

    /* Device list: top-left, fills most of the panel. */
    SDL_Rect list_rect = {
        .x = panel_rect.x + CBX_CT_LIST_Y,
        .y = panel_rect.y + CBX_CT_LIST_Y,
        .w = panel_rect.w - CBX_CT_LIST_Y * 2,
        .h = CBX_CT_LIST_H,
    };
    cbx_widget_set_rect(&tab->device_list.base, &list_rect);
    cbx_widget_set_rect(&tab->type_picker.base, &list_rect);

    /* Buttons: below the list, left to right. */
    int btn_y = panel_rect.y + CBX_CT_LIST_Y + CBX_CT_LIST_H + CBX_CT_BTN_GAP;
    int btn_x = panel_rect.x + CBX_CT_LIST_Y;

    SDL_Rect add_rect = { .x = btn_x, .y = btn_y,
                          .w = CBX_CT_BTN_W, .h = CBX_CT_BTN_H };
    cbx_widget_set_rect(&tab->add_btn.base, &add_rect);

    btn_x += CBX_CT_BTN_W + CBX_CT_BTN_GAP;
    SDL_Rect rm_rect = { .x = btn_x, .y = btn_y,
                          .w = CBX_CT_BTN_W, .h = CBX_CT_BTN_H };
    cbx_widget_set_rect(&tab->remove_btn.base, &rm_rect);

    btn_x += CBX_CT_BTN_W + CBX_CT_BTN_GAP;
    SDL_Rect ct_rect = { .x = btn_x, .y = btn_y,
                          .w = CBX_CT_BTN_W, .h = CBX_CT_BTN_H };
    cbx_widget_set_rect(&tab->change_type_btn.base, &ct_rect);

    SDL_Rect status_rect = {
        .x = panel_rect.x + CBX_CT_LIST_Y,
        .y = btn_y + CBX_CT_BTN_H + CBX_CT_BTN_GAP,
        .w = panel_rect.w - CBX_CT_LIST_Y * 2,
        .h = CBX_CT_BTN_H,
    };
    cbx_widget_set_rect(&tab->status_lbl.base, &status_rect);
}

int
cbx_controllers_tab_refresh(cbx_controllers_tab *tab)
{
    if (!tab || !tab->backend)
        return -EINVAL;

    /* Re-enumerate devices without letting dictionary order redefine slots.
     * Preserve the visibly selected object by exact path, not by stale index. */
    char selected_path[CBX_MAX_PATH_LEN] = "";
    if (tab->selected_device >= 0 &&
        tab->selected_device < tab->model.target_count)
        snprintf(selected_path, sizeof(selected_path), "%s",
                 tab->model.targets[tab->selected_device].path);
    cbx_device_model prior = tab->model;
    cbx_device_model next;
    int rc = cbx_objectmanager_enumerate(tab->backend, tab->bus, &next);
    if (rc != 0)
        return rc;
    preserve_target_order(&next, &prior);
    /* Carry already-observed reactive per-device properties (profile/routing)
     * across the re-enumeration so an authoritative refresh does not discard
     * validated PropertiesChanged state. */
    cbx_device_model_preserve_props(&next, &prior);
    tab->model = next;

    /* Query each target's DeviceType.  A connected bus is not available
     * state when required typed properties cannot be read (SPEC §2.4). */
    tab->device_type_count = 0;
    int type_rc = 0;
    for (int i = 0; i < tab->model.target_count && i < CBX_CT_MAX_DEVICES; i++) {
        char *dtype = NULL;
        rc = ip_target_get_device_type(tab->backend, tab->bus,
                                         tab->model.targets[i].path,
                                         &dtype);
        if (rc == 0 && dtype && dtype[0]) {
            snprintf(tab->device_types[i], CBX_MAX_TYPE_LEN, "%s", dtype);
        } else {
            tab->device_types[i][0] = '\0';
            if (type_rc == 0)
                type_rc = rc != 0 ? rc : -EIO;
        }
        free(dtype);
        tab->device_type_count++;
    }

    /* Rebuild the list widget from the refreshed model. */
    cbx_controllers_tab_refresh_labels(tab);

    /* Restore the same selected target after reorder.  If it disappeared,
     * clamp to the nearest surviving row; an empty topology selects none. */
    if (selected_path[0]) {
        int restored = -1;
        for (int i = 0; i < tab->model.target_count; i++)
            if (strcmp(tab->model.targets[i].path, selected_path) == 0) {
                restored = i;
                break;
            }
        if (restored >= 0)
            tab->selected_device = restored;
    }
    if (tab->selected_device >= tab->model.target_count)
        tab->selected_device = tab->model.target_count - 1;
    if (tab->selected_device < 0 && tab->model.target_count > 0)
        tab->selected_device = 0;
    cbx_list_set_selected(&tab->device_list, tab->selected_device);

    /* SPEC §5.2: missing targets are an error; zero physical composites is
     * instead a valid ready-but-unassigned virtual topology. */
    check_orphan_columns(tab);
    if (type_rc == 0 && tab->model.target_count > 0 &&
        tab->model.composite_count == 0 &&
        !cbx_widget_is_visible(&tab->status_lbl.base)) {
        char msg[CBX_LABEL_TEXT_LEN];
        snprintf(msg, sizeof(msg),
                 "%d virtual slots ready; no physical controllers assigned",
                 tab->model.target_count);
        cbx_label_set_text(&tab->status_lbl, msg);
        cbx_widget_set_visible(&tab->status_lbl.base, true);
    } else if (tab->model.composite_count > 0 &&
               strstr(tab->status_lbl.text, "virtual slots ready;") != NULL) {
        cbx_label_set_text(&tab->status_lbl, "");
        cbx_widget_set_visible(&tab->status_lbl.base, false);
    }

    return type_rc;
}

/* ------------------------------------------------------------------ */
/*  Actions                                                            */
/* ------------------------------------------------------------------ */

int
cbx_controllers_tab_load_supported_types(cbx_controllers_tab *tab)
{
    if (!tab || !tab->backend)
        return -EINVAL;

    char *csv = NULL;
    int rc = ip_manager_get_supported_target_device_ids(tab->backend,
                                                          tab->bus, &csv);
    if (rc != 0)
        return rc;

    tab->supported_type_count = 0;
    if (csv) {
        tab->supported_type_count =
            parse_csv(csv, tab->supported_types, CBX_CT_MAX_TYPES);
        free(csv);
    }

    return 0;
}

int
cbx_controllers_tab_add(cbx_controllers_tab *tab, const char *type)
{
    if (!tab || !tab->backend || !type)
        return -EINVAL;

    /* Enforce the configured product limit before touching InputPlumber. */
    if (tab->model.target_count >= CBX_MAX_CONTROLLERS ||
        (tab->settings &&
         tab->settings->virtual_controllers.count >= CBX_MAX_CONTROLLERS))
        return -ENOSPC;

    char *out_path = NULL;
    int rc = ip_manager_create_target_device(tab->backend, tab->bus,
                                               type, &out_path);
    if (rc != 0)
        return rc;
    if (!out_path || !out_path[0]) {
        free(out_path);
        return -EIO;
    }

    /* A virtual slot is valid without a physical controller.  Confirm the
     * exact returned path and type asynchronously; never infer success from
     * one immediate count refresh or attach it to an unrelated composite. */
    rc = refresh_until_path(tab, out_path, true, type);
    if (rc != 0) {
        int cleanup = ct_stop_created(tab, out_path);
        if (cleanup != 0) {
            ct_disable_until_reenumerated(tab,
                "Add cleanup failed; assignment disabled until re-enumeration");
            fprintf(stderr,
                "controller-box: Add cleanup failed for delayed target %s: rc=%d\n",
                out_path, cleanup);
        }
    }
    if (rc == 0 && tab->settings) {
        int n = tab->settings->virtual_controllers.count;
        cbx_settings proposed = *tab->settings;
        snprintf(proposed.virtual_controllers.types[n], CBX_MAX_TYPE_LEN,
                 "%s", type);
        proposed.virtual_controllers.count = n + 1;
        rc = cbx_settings_save(&proposed);
        if (rc == 0) {
            *tab->settings = proposed;
            tab->expected_target_count = n + 1;
            check_orphan_columns(tab);
        } else {
            /* Persistence is part of success.  Compensate the published
             * target and confirm its exact path disappeared. */
            int rollback = ct_stop_created(tab, out_path);
            if (rollback != 0) {
                ct_disable_until_reenumerated(tab,
                    "Add reconciliation failed; assignment disabled until re-enumeration");
                show_action_error(tab, "Add reconciliation", rollback);
            }
        }
    }
    free(out_path);
    return rc;
}

/* Snapshot of the state persisted before a destructive remove so the
 * backend-failure path can restore it without re-reading the pre-state. */
typedef struct {
    bool have_asgn;
    cbx_assignments old_asgn;
    cbx_assignments proposed_asgn;
    bool change_settings;
    cbx_settings old_settings;
    cbx_settings proposed_settings;
} ct_remove_snapshot;

/*
 * Persist the desired post-remove assignment/settings state.  Runs with the
 * cross-process config lock held, and only for the duration of the file
 * load-modify-save: the caller runs the DBus mutation (and its compensation)
 * outside the lock so an unresponsive InputPlumber cannot stall every other
 * config writer.  On failure the first file is restored before returning.
 */
static int
controllers_tab_remove_persist(cbx_controllers_tab *tab, int device_index,
                               ct_remove_snapshot *snap)
{
    memset(snap, 0, sizeof(*snap));
    cbx_assignments_init(&snap->old_asgn);

    snap->have_asgn = cbx_assignments_load(&snap->old_asgn) == 0;
    snap->proposed_asgn = snap->old_asgn;
    for (int i = snap->proposed_asgn.assignment_count - 1; i >= 0; i--) {
        if (snap->proposed_asgn.assignments[i].slot == device_index)
            snap->proposed_asgn.assignments[i] =
                snap->proposed_asgn.assignments[
                    --snap->proposed_asgn.assignment_count];
        else if (snap->proposed_asgn.assignments[i].slot > device_index)
            snap->proposed_asgn.assignments[i].slot--;
    }

    snap->change_settings = tab->settings &&
        device_index < tab->settings->virtual_controllers.count;
    if (snap->change_settings) {
        snap->old_settings = *tab->settings;
        snap->proposed_settings = *tab->settings;
        int n = snap->proposed_settings.virtual_controllers.count;
        if (device_index < n - 1)
            memmove(&snap->proposed_settings.virtual_controllers.types[device_index],
                    &snap->proposed_settings.virtual_controllers.types[device_index + 1],
                    (size_t)(n - device_index - 1) * CBX_MAX_TYPE_LEN);
        snap->proposed_settings.virtual_controllers.types[n - 1][0] = '\0';
        snap->proposed_settings.virtual_controllers.count = n - 1;
    }

    /* If either write fails, restore the first file and leave InputPlumber
     * untouched rather than reporting a half-success. */
    int rc = 0;
    bool assignments_written = false;
    if (snap->have_asgn) {
        rc = cbx_assignments_save(&snap->proposed_asgn);
        assignments_written = rc == 0;
    }
    if (rc == 0 && snap->change_settings)
        rc = cbx_settings_save(&snap->proposed_settings);
    if (rc != 0 && assignments_written)
        (void)cbx_assignments_save(&snap->old_asgn);
    return rc;
}

/*
 * Restore the pre-remove assignment state by inverting exactly this
 * operation's delta on the freshly loaded on-disk table: re-add entries the
 * remove dropped and un-shift the slots it decremented, while leaving any
 * entries a concurrent writer added or changed in the meantime untouched.
 */
typedef struct {
    const cbx_assignments *old_asgn;
    const cbx_assignments *proposed_asgn;
} ct_remove_restore_args;

static int
ct_remove_restore_assignments(cbx_assignments *a, void *userdata)
{
    ct_remove_restore_args *args = userdata;

    for (int i = 0; i < args->old_asgn->assignment_count; i++) {
        const cbx_assignment *o = &args->old_asgn->assignments[i];

        int p = -1;
        for (int j = 0; j < args->proposed_asgn->assignment_count; j++) {
            if (strcmp(args->proposed_asgn->assignments[j].id, o->id) == 0) {
                p = j;
                break;
            }
        }

        if (p < 0) {
            /* Removed by the forward op: re-add unless a concurrent writer
             * already (re)created it. */
            if (cbx_assign_find_index(a, o->id) < 0) {
                if (a->assignment_count >= CBX_MAX_ASSIGNMENTS)
                    return -ENOSPC;
                a->assignments[a->assignment_count++] = *o;
            }
        } else if (o->slot != args->proposed_asgn->assignments[p].slot) {
            /* Shifted by the forward op: revert only if a concurrent writer
             * has not since changed this entry's slot. */
            int idx = cbx_assign_find_index(a, o->id);
            if (idx >= 0 &&
                a->assignments[idx].slot ==
                    args->proposed_asgn->assignments[p].slot)
                a->assignments[idx].slot = o->slot;
        }
    }
    return 0;
}

int
cbx_controllers_tab_remove(cbx_controllers_tab *tab, int device_index)
{
    if (!tab || !tab->backend)
        return -EINVAL;
    if (device_index < 0 || device_index >= tab->model.target_count)
        return -EINVAL;

    char path[CBX_MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s", tab->model.targets[device_index].path);

    /* Phase 1: persist the desired state under the config lock.  The lock is
     * held only for the load-modify-save of the config files; the DBus
     * mutation below and its compensation run outside it, so a slow
     * InputPlumber cannot stall every other config writer (e.g. the resident
     * overlay's input-critical save path). */
    ct_remove_snapshot snap;
    int lock = cbx_io_lock();
    if (lock < 0)
        return lock;
    int rc = controllers_tab_remove_persist(tab, device_index, &snap);
    cbx_io_unlock(lock);
    if (rc != 0)
        return rc;

    /* Resolve the routed composite from the pre-remove snapshot outside the
     * lock (DBus latency must not serialize config writers), then mutate the
     * backend. */
    char composite[CBX_MAX_PATH_LEN] = "";
    char assigned_id[CBX_MAX_ID_LEN];
    bool assigned = snap.have_asgn &&
        assigned_id_for_slot(&snap.old_asgn, device_index, assigned_id) &&
        composite_path_for_id(tab, assigned_id, composite);

    if (assigned) {
        rc = ip_composite_set_target_device_paths(tab->backend, tab->bus,
                                                    composite, "");
        if (rc == 0)
            rc = wait_exact_attachment(tab, composite, NULL);
    }
    bool stop_issued = false;
    if (rc == 0) {
        rc = ip_manager_stop_target_device(tab->backend, tab->bus, path);
        if (rc == 0)
            stop_issued = true;
    }
    if (rc == 0)
        rc = refresh_until_path(tab, path, false, NULL);

    if (rc != 0) {
        /* A stop whose late removal readback failed is irreversible: the
         * target cannot be revived, so keep the post-remove desired topology
         * instead of resurrecting an assignment/slot for a target that is
         * already gone, and disable assignment until a fresh enumeration
         * confirms recovery (SPEC §5.2).  Otherwise roll the pre-remove
         * state back. */
        if (stop_issued || !ct_model_has_path(tab, path)) {
            if (snap.change_settings) {
                *tab->settings = snap.proposed_settings;
                tab->expected_target_count =
                    snap.proposed_settings.virtual_controllers.count;
            }
            ct_disable_until_reenumerated(tab,
                "Remove not confirmed; assignment disabled until re-enumeration");
            return rc;
        }

        /* Phase 3: restore persisted desired state.  The assignments restore
         * runs through the shared transaction (which takes the config lock
         * itself); taking the lock here and then calling the transaction
         * would self-deadlock on the nested flock.  settings.yaml has a
         * single writer (the Manager), so it is restored directly. */
        if (snap.have_asgn) {
            ct_remove_restore_args restore = {
                &snap.old_asgn, &snap.proposed_asgn,
            };
            (void)cbx_assignments_transaction(
                ct_remove_restore_assignments, &restore, NULL);
        }
        if (snap.change_settings)
            (void)cbx_settings_save(&snap.old_settings);
        /* If the target still exists, restore its exact singleton route;
         * otherwise expose reconciliation failure explicitly for startup
         * compensation. */
        if (assigned) {
            int restore = ip_composite_set_target_device_paths(tab->backend,
                tab->bus, composite, path);
            if (restore == 0)
                restore = wait_exact_attachment(tab, composite, path);
            if (restore != 0)
                show_action_error(tab, "Remove reconciliation", restore);
        }
        return rc;
    }

    if (snap.change_settings) {
        *tab->settings = snap.proposed_settings;
        tab->expected_target_count =
            snap.proposed_settings.virtual_controllers.count;
    }
    return 0;
}

int
cbx_controllers_tab_change_type(cbx_controllers_tab *tab,
                                  int device_index,
                                  const char *new_type)
{
    if (!tab || !tab->backend || !new_type)
        return -EINVAL;
    if (device_index < 0 || device_index >= tab->model.target_count)
        return -EINVAL;

    char old_path[CBX_MAX_PATH_LEN];
    snprintf(old_path, sizeof(old_path), "%s",
             tab->model.targets[device_index].path);
    cbx_settings proposed_settings;
    bool settings_prepared = tab->settings &&
        device_index < tab->settings->virtual_controllers.count;
    if (settings_prepared) {
        proposed_settings = *tab->settings;
        snprintf(proposed_settings.virtual_controllers.types[device_index],
                 CBX_MAX_TYPE_LEN, "%s", new_type);
        int save_rc = cbx_settings_save(&proposed_settings);
        if (save_rc != 0)
            return save_rc;
    }

    char *replacement = NULL;
    int rc = ip_manager_create_target_device(tab->backend, tab->bus,
                                               new_type, &replacement);
    if (rc != 0 || !replacement || !replacement[0]) {
        if (rc == 0) rc = -EIO;
        free(replacement);
        if (settings_prepared) (void)cbx_settings_save(tab->settings);
        return rc;
    }

    /* Confirm the exact replacement path and type before touching routing or
     * stopping the old target (SPEC §5.2: stage until readback succeeds). */
    rc = refresh_until_path(tab, replacement, true, new_type);
    if (rc != 0) {
        int cleanup = ct_stop_created(tab, replacement);
        if (cleanup != 0)
            ct_disable_until_reenumerated(tab,
                "Change type cleanup failed; assignment disabled until re-enumeration");
        if (settings_prepared) (void)cbx_settings_save(tab->settings);
        free(replacement);
        return rc;
    }

    /* Attach the replacement to the composite that owns this slot before the
     * old target is stopped, and verify the exact route. */
    char composite[CBX_MAX_PATH_LEN] = "";
    bool routed = false;
    if (assigned_composite_for_slot(tab, device_index, composite)) {
        rc = ct_restore_route(tab, composite, replacement);
        if (rc != 0) {
            /* The property write may have applied.  Restore the still-live
             * old route before stopping the replacement; if compensation
             * cannot be verified, keep the replacement routed rather than
             * stopping a target the composite still points at. */
            int cleanup = ct_restore_route(tab, composite, old_path);
            if (cleanup == 0)
                cleanup = ct_stop_created(tab, replacement);
            if (cleanup != 0)
                ct_disable_until_reenumerated(tab,
                    "Change type routing compensation failed; assignment disabled until re-enumeration");
            if (settings_prepared) (void)cbx_settings_save(tab->settings);
            free(replacement);
            return rc;
        }
        routed = true;
    }

    /* Establish replacement as this slot before the old path vanishes, so
     * later unordered refreshes cannot move unrelated indices. */
    snprintf(tab->model.targets[device_index].path,
             sizeof(tab->model.targets[device_index].path), "%s",
             replacement);
    const char *slash = strrchr(replacement, '/');
    snprintf(tab->model.targets[device_index].name,
             sizeof(tab->model.targets[device_index].name), "%s",
             slash ? slash + 1 : replacement);

    rc = ip_manager_stop_target_device(tab->backend, tab->bus, old_path);
    if (rc != 0) {
        /* The old target is still live: restore its route before stopping the
         * replacement, then put the old target back as this slot. */
        if (routed) {
            int restore = ct_restore_route(tab, composite, old_path);
            if (restore != 0) {
                /* Cannot restore routing: keep the replacement routed and
                 * live, and do not advertise the old topology as restored. */
                if (settings_prepared) *tab->settings = proposed_settings;
                ct_disable_until_reenumerated(tab,
                    "Change type routing rollback failed; assignment disabled until re-enumeration");
                free(replacement);
                return rc;
            }
        }
        int cleanup = ct_stop_created(tab, replacement);
        if (ct_model_has_path(tab, old_path)) {
            snprintf(tab->model.targets[device_index].path,
                     sizeof(tab->model.targets[device_index].path), "%s",
                     old_path);
            const char *old_slash = strrchr(old_path, '/');
            snprintf(tab->model.targets[device_index].name,
                     sizeof(tab->model.targets[device_index].name),
                     "%.*s",
                     (int)sizeof(tab->model.targets[device_index].name) - 1,
                     old_slash ? old_slash + 1 : old_path);
        }
        (void)cbx_controllers_tab_refresh(tab);
        if (settings_prepared) (void)cbx_settings_save(tab->settings);
        if (cleanup != 0)
            ct_disable_until_reenumerated(tab,
                "Change type cleanup failed; assignment disabled until re-enumeration");
        free(replacement);
        return rc;
    }

    /* The old stop was accepted, so the old target cannot be revived.  Confirm
     * its exact disappearance.  If that readback is late, keep the replacement
     * routed and the desired type, and disable assignment until a fresh
     * enumeration confirms the topology instead of advertising the old target
     * as restored (SPEC §5.2). */
    rc = refresh_until_path(tab, old_path, false, NULL);
    if (rc != 0) {
        if (settings_prepared) *tab->settings = proposed_settings;
        ct_disable_until_reenumerated(tab,
            "Change type removal unconfirmed; assignment disabled until re-enumeration");
        free(replacement);
        return rc;
    }

    if (settings_prepared)
        *tab->settings = proposed_settings;
    free(replacement);
    return rc;
}

int
cbx_controllers_tab_begin_type_pick(cbx_controllers_tab *tab,
                                       cbx_ct_action action)
{
    if (!tab)
        return -EINVAL;
    if (tab->supported_type_count == 0)
        return -ENOENT;

    /* Clear any previous operation error. */
    clear_action_error(tab);

    /* Populate the type picker list. */
    cbx_list_clear(&tab->type_picker);
    for (int i = 0; i < tab->supported_type_count; i++)
        cbx_list_add_item(&tab->type_picker, tab->supported_types[i],
                           NULL, tab);  /* user_data = tab pointer for on_select */

    tab->selected_type = 0;
    cbx_list_set_selected(&tab->type_picker, 0);

    /* Show the type picker, hide the device list. */
    cbx_widget_set_visible(&tab->device_list.base, false);
    cbx_widget_set_visible(&tab->type_picker.base, true);
    cbx_widget_set_visible(&tab->add_btn.base, false);
    cbx_widget_set_visible(&tab->remove_btn.base, false);
    cbx_widget_set_visible(&tab->change_type_btn.base, false);

    tab->mode = CBX_CT_MODE_TYPE_PICK;
    tab->pending_action = action;

    return 0;
}

int
cbx_controllers_tab_confirm_type_pick(cbx_controllers_tab *tab)
{
    if (!tab || tab->mode != CBX_CT_MODE_TYPE_PICK)
        return -EINVAL;
    if (tab->selected_type < 0 ||
        tab->selected_type >= tab->supported_type_count)
        return -EINVAL;

    const char *type = tab->supported_types[tab->selected_type];
    cbx_ct_action action = tab->pending_action;
    int device_index = tab->selected_device;

    /* Return to list mode first. */
    cbx_controllers_tab_cancel_type_pick(tab);

    /* Execute the pending action. */
    int rc;
    if (action == CBX_CT_ACTION_ADD)
        rc = cbx_controllers_tab_add(tab, type);
    else if (action == CBX_CT_ACTION_CHANGE)
        rc = cbx_controllers_tab_change_type(tab, device_index, type);
    else
        return -EINVAL;

    /* Show error on failure, clear on success (SPEC §5.2). */
    if (rc != 0)
        show_action_error(tab,
                          action == CBX_CT_ACTION_ADD ? "Add" : "Change type",
                          rc);
    else
        clear_action_error(tab);
    return rc;
}

void
cbx_controllers_tab_cancel_type_pick(cbx_controllers_tab *tab)
{
    if (!tab)
        return;

    /* Restore list view. */
    cbx_widget_set_visible(&tab->device_list.base, true);
    cbx_widget_set_visible(&tab->type_picker.base, false);
    cbx_widget_set_visible(&tab->add_btn.base, true);
    cbx_widget_set_visible(&tab->remove_btn.base, true);
    cbx_widget_set_visible(&tab->change_type_btn.base, true);

    tab->mode = CBX_CT_MODE_LIST;
    tab->pending_action = CBX_CT_ACTION_NONE;
    tab->selected_type = -1;
}

/* ------------------------------------------------------------------ */
/*  Tab-level activation / cancel (for manager event forwarding)        */
/* ------------------------------------------------------------------ */

/* on_select callback for the type picker list.
 * Called when the user presses A (KEYUP) or clicks (MOUSEUP) on a type item.
 * Syncs the selected type index and confirms the pick. */
static void
on_device_selected(cbx_widget *w, int index, void *user_data)
{
    (void)w;
    cbx_controllers_tab *tab = user_data;
    if (!tab || index < 0 || index >= tab->model.target_count)
        return;
    tab->selected_device = index;
}

void
cbx_controllers_tab_sync_selection(cbx_controllers_tab *tab)
{
    if (!tab || tab->mode != CBX_CT_MODE_LIST)
        return;
    int selected = cbx_list_get_selected(&tab->device_list);
    tab->selected_device = selected >= 0 && selected < tab->model.target_count
                         ? selected : -1;
}

static void
on_type_pick_selected(cbx_widget *w, int index, void *user_data)
{
    (void)w;
    cbx_controllers_tab *tab = (cbx_controllers_tab *)user_data;
    if (!tab || tab->mode != CBX_CT_MODE_TYPE_PICK)
        return;
    tab->selected_type = index;
    cbx_controllers_tab_confirm_type_pick(tab);
}

int
cbx_controllers_tab_activate(cbx_controllers_tab *tab)
{
    if (!tab)
        return -EINVAL;

    switch (tab->mode) {
    case CBX_CT_MODE_TYPE_PICK:
        /* Sync selected_type from the picker list, then confirm. */
        tab->selected_type = cbx_list_get_selected(&tab->type_picker);
        return cbx_controllers_tab_confirm_type_pick(tab);
    case CBX_CT_MODE_LIST:
    default:
        /* No tab-level activation in list mode. */
        return 0;
    }
}

bool
cbx_controllers_tab_cancel(cbx_controllers_tab *tab)
{
    if (!tab)
        return false;

    if (tab->mode == CBX_CT_MODE_TYPE_PICK) {
        cbx_controllers_tab_cancel_type_pick(tab);
        return true;
    }
    return false;
}

bool
cbx_controllers_tab_handle_key(cbx_controllers_tab *tab, const SDL_Event *ev)
{
    if (!tab || !ev || ev->type != SDL_KEYDOWN)
        return false;

    SDL_Keycode key = ev->key.keysym.sym;

    if (tab->mode == CBX_CT_MODE_TYPE_PICK) {
        if (key == SDLK_b || key == SDLK_ESCAPE) {
            cbx_controllers_tab_cancel_type_pick(tab);
            return true;
        }
    }

    return false;
}

/* ------------------------------------------------------------------ */
/*  Accessors                                                          */
/* ------------------------------------------------------------------ */

int
cbx_controllers_tab_device_count(const cbx_controllers_tab *tab)
{
    return tab ? tab->model.target_count : 0;
}

int
cbx_controllers_tab_supported_type_count(const cbx_controllers_tab *tab)
{
    return tab ? tab->supported_type_count : 0;
}

const char *
cbx_controllers_tab_device_type(const cbx_controllers_tab *tab, int index)
{
    if (!tab || index < 0 || index >= tab->device_type_count)
        return NULL;
    return tab->device_types[index];
}

const char *
cbx_controllers_tab_device_path(const cbx_controllers_tab *tab, int index)
{
    if (!tab || index < 0 || index >= tab->model.target_count)
        return NULL;
    return tab->model.targets[index].path;
}

const char *
cbx_controllers_tab_supported_type(const cbx_controllers_tab *tab,
                                      int index)
{
    if (!tab || index < 0 || index >= tab->supported_type_count)
        return NULL;
    return tab->supported_types[index];
}

int
cbx_controllers_tab_selected_device(const cbx_controllers_tab *tab)
{
    return tab ? tab->selected_device : -1;
}

int
cbx_controllers_tab_selected_composite(const cbx_controllers_tab *tab,
                                        char *out, size_t outsz)
{
    if (!tab || !out || outsz == 0)
        return 0;
    out[0] = '\0';
    if (tab->selected_device < 0 ||
        tab->selected_device >= tab->model.target_count)
        return 0;
    const char *target = tab->model.targets[tab->selected_device].path;
    if (!target[0])
        return 0;

    for (int ci = 0; ci < tab->model.composite_count; ci++) {
        const cbx_composite_entry *e = &tab->model.composites[ci];
        bool match = false;
        char *csv = NULL;
        if (tab->backend && tab->bus &&
            ip_composite_get_target_devices(tab->backend, tab->bus,
                                             e->path, &csv) == 0 && csv) {
            match = csv_contains_path(csv, target);
        } else if (e->has_target_devices) {
            /* Authoritative read unavailable: fall back to the last
             * validated reactive value rather than guessing. */
            match = csv_contains_path(e->target_devices, target);
        }
        free(csv);
        if (match) {
            snprintf(out, outsz, "%s", e->path);
            return 1;
        }
    }
    return 0;
}

cbx_ct_mode
cbx_controllers_tab_mode(const cbx_controllers_tab *tab)
{
    return tab ? tab->mode : CBX_CT_MODE_LIST;
}