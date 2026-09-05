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
    cbx_list_set_select_cb(&tab->device_list, NULL);

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
        if (tab->backend->process) tab->backend->process(tab->bus);
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
        bool exact = rc == 0 && csv_is_exact_singleton_path(csv, target);
        free(csv);
        if (exact) return 0;
        if ((uint32_t)(SDL_GetTicks() - started) >=
            CBX_CT_OPERATION_TIMEOUT_MS)
            return -EIO;
        if (tab->backend->process) tab->backend->process(tab->bus);
        SDL_Delay(CBX_CT_OPERATION_POLL_MS);
    }
}

static int
assigned_composite_for_slot(cbx_controllers_tab *tab, int slot,
                            char out[CBX_MAX_PATH_LEN])
{
    cbx_assignments asgn;
    cbx_assignments_init(&asgn);
    if (cbx_assignments_load(&asgn) != 0) return 0;
    for (int ai = 0; ai < asgn.assignment_count; ai++) {
        if (asgn.assignments[ai].slot != slot) continue;
        for (int ci = 0; ci < tab->model.composite_count; ci++) {
            char *id = NULL;
            int rc = ip_composite_get_persistent_id(tab->backend, tab->bus,
                tab->model.composites[ci].path, &id);
            bool match = rc == 0 && id &&
                strcmp(id, asgn.assignments[ai].id) == 0;
            free(id);
            if (match) {
                snprintf(out, CBX_MAX_PATH_LEN, "%s",
                         tab->model.composites[ci].path);
                return 1;
            }
        }
    }
    return 0;
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

    /* Re-enumerate devices without letting dictionary order redefine slots. */
    cbx_device_model prior = tab->model;
    cbx_device_model next;
    int rc = cbx_objectmanager_enumerate(tab->backend, tab->bus, &next);
    if (rc != 0)
        return rc;
    preserve_target_order(&next, &prior);
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

    /* Rebuild the list widget. */
    cbx_list_clear(&tab->device_list);
    for (int i = 0; i < tab->model.target_count; i++) {
        char label[CBX_CT_LABEL_LEN];
        const char *type = (i < tab->device_type_count)
                            ? tab->device_types[i] : NULL;
        format_device_label(label, sizeof(label),
                              tab->model.targets[i].name, type);
        /* user_data stores the index (cast through intptr_t). */
        cbx_list_add_item(&tab->device_list, label, NULL,
                           (void *)(intptr_t)(long)i);
    }

    /* Clamp selection. */
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
        int cleanup = ip_manager_stop_target_device(tab->backend, tab->bus,
                                                     out_path);
        if (cleanup == 0)
            cleanup = refresh_until_path(tab, out_path, false, NULL);
        if (cleanup != 0)
            fprintf(stderr,
                "controller-box: Add cleanup failed for delayed target %s: rc=%d\n",
                out_path, cleanup);
    }
    if (rc == 0 && tab->settings) {
        int n = tab->settings->virtual_controllers.count;
        if (n >= CBX_MAX_CONTROLLERS) rc = -ENOSPC;
        else {
            snprintf(tab->settings->virtual_controllers.types[n],
                     CBX_MAX_TYPE_LEN, "%s", type);
            tab->settings->virtual_controllers.count = n + 1;
            rc = cbx_settings_save(tab->settings);
            if (rc == 0) {
                tab->expected_target_count = n + 1;
                check_orphan_columns(tab);
            }
        }
    }
    free(out_path);
    return rc;
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
    int rc = ip_manager_stop_target_device(tab->backend, tab->bus, path);
    if (rc != 0)
        return rc;

    rc = refresh_until_path(tab, path, false, NULL);
    if (rc != 0)
        return rc;

    /* SPEC §5.2 / CT-02: physical controller auto-Unassigned.
     * Load the persisted assignments, remove any assignment whose slot
     * matches the removed device (the physical controller in that slot
     * becomes Unassigned), and shift higher slots down by one to match
     * the new target indexing.  Save the updated assignments. */
    cbx_assignments asgn;
    cbx_assignments_init(&asgn);
    if (cbx_assignments_load(&asgn) == 0) {
        for (int i = asgn.assignment_count - 1; i >= 0; i--) {
            if (asgn.assignments[i].slot == device_index) {
                /* Remove: physical controller is now Unassigned. */
                asgn.assignments[i] =
                    asgn.assignments[--asgn.assignment_count];
            } else if (asgn.assignments[i].slot > device_index) {
                /* Shift down to match new target indexing. */
                asgn.assignments[i].slot--;
            }
        }
        rc = cbx_assignments_save(&asgn);
        if (rc != 0) return rc;
    }
    if (tab->settings) {
        int n = tab->settings->virtual_controllers.count;
        if (device_index < n) {
            if (device_index < n - 1)
                memmove(&tab->settings->virtual_controllers.types[device_index],
                        &tab->settings->virtual_controllers.types[device_index + 1],
                        (size_t)(n - device_index - 1) * CBX_MAX_TYPE_LEN);
            tab->settings->virtual_controllers.types[n - 1][0] = '\0';
            tab->settings->virtual_controllers.count = n - 1;
            rc = cbx_settings_save(tab->settings);
            if (rc != 0) return rc;
            tab->expected_target_count = n - 1;
        }
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
    char *replacement = NULL;
    int rc = ip_manager_create_target_device(tab->backend, tab->bus,
                                               new_type, &replacement);
    if (rc != 0) return rc;
    if (!replacement || !replacement[0]) { free(replacement); return -EIO; }

    rc = refresh_until_path(tab, replacement, true, new_type);
    char composite[CBX_MAX_PATH_LEN] = "";
    if (rc == 0 && assigned_composite_for_slot(tab, device_index, composite)) {
        rc = ip_manager_attach_target_device(tab->backend, tab->bus,
                                               replacement, composite);
        if (rc == 0)
            rc = wait_exact_attachment(tab, composite, replacement);
    }
    if (rc == 0) {
        /* Establish replacement as this slot before the old path vanishes,
         * so later unordered refreshes cannot move unrelated indices. */
        snprintf(tab->model.targets[device_index].path,
                 sizeof(tab->model.targets[device_index].path), "%s",
                 replacement);
        const char *slash = strrchr(replacement, '/');
        snprintf(tab->model.targets[device_index].name,
                 sizeof(tab->model.targets[device_index].name), "%s",
                 slash ? slash + 1 : replacement);
        rc = ip_manager_stop_target_device(tab->backend, tab->bus, old_path);
        if (rc == 0)
            rc = refresh_until_path(tab, old_path, false, NULL);
    }
    if (rc != 0) {
        int cleanup = ip_manager_stop_target_device(tab->backend, tab->bus,
                                                     replacement);
        if (cleanup == 0)
            cleanup = refresh_until_path(tab, replacement, false, NULL);
        if (cleanup != 0)
            fprintf(stderr,
                "controller-box: Change type cleanup failed for %s: rc=%d\n",
                replacement, cleanup);
    }
    if (rc == 0 && tab->settings &&
        device_index < tab->settings->virtual_controllers.count) {
        char prior[CBX_MAX_TYPE_LEN];
        snprintf(prior, sizeof(prior), "%s",
                 tab->settings->virtual_controllers.types[device_index]);
        snprintf(tab->settings->virtual_controllers.types[device_index],
                 CBX_MAX_TYPE_LEN, "%s", new_type);
        rc = cbx_settings_save(tab->settings);
        if (rc != 0)
            snprintf(tab->settings->virtual_controllers.types[device_index],
                     CBX_MAX_TYPE_LEN, "%s", prior);
    }
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

cbx_ct_mode
cbx_controllers_tab_mode(const cbx_controllers_tab *tab)
{
    return tab ? tab->mode : CBX_CT_MODE_LIST;
}