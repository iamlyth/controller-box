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

/* ------------------------------------------------------------------ */
/*  Layout constants                                                  */
/* ------------------------------------------------------------------ */

#define CBX_CT_LIST_H    400
#define CBX_CT_BTN_W    200
#define CBX_CT_BTN_H     44
#define CBX_CT_BTN_GAP   16
#define CBX_CT_LIST_Y    16

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
    if (idx >= 0 && idx < cbx_controllers_tab_device_count(tab))
        cbx_controllers_tab_remove(tab, idx);
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

    /* --- Add widgets to panel ------------------------------------- */
    cbx_panel_add_child(panel, &tab->device_list.base);
    cbx_panel_add_child(panel, &tab->add_btn.base);
    cbx_panel_add_child(panel, &tab->remove_btn.base);
    cbx_panel_add_child(panel, &tab->change_type_btn.base);
    cbx_panel_add_child(panel, &tab->type_picker.base);

    /* --- Layout --------------------------------------------------- */
    SDL_Rect panel_rect;
    cbx_widget_get_rect(&panel->base, &panel_rect);

    /* Device list: top-left, fills most of the panel. */
    SDL_Rect list_rect = {
        .x = panel_rect.x + CBX_CT_LIST_Y,
        .y = panel_rect.y + CBX_CT_LIST_Y,
        .w = panel_rect.w - CBX_CT_LIST_Y * 2,
        .h = CBX_CT_LIST_H,
    };
    cbx_widget_set_rect(&tab->device_list.base, &list_rect);

    /* Type picker: same position, hidden. */
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

    /* --- Load supported types + refresh --------------------------- */
    if (backend && bus) {
        cbx_controllers_tab_load_supported_types(tab);
        cbx_controllers_tab_refresh(tab);
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
    }

    cbx_widget_destroy(&tab->device_list.base);
    cbx_widget_destroy(&tab->type_picker.base);
    cbx_widget_destroy(&tab->add_btn.base);
    cbx_widget_destroy(&tab->remove_btn.base);
    cbx_widget_destroy(&tab->change_type_btn.base);

    memset(tab, 0, sizeof(*tab));
}

/* ------------------------------------------------------------------ */
/*  Refresh                                                            */
/* ------------------------------------------------------------------ */

int
cbx_controllers_tab_refresh(cbx_controllers_tab *tab)
{
    if (!tab || !tab->backend)
        return -EINVAL;

    /* Re-enumerate devices. */
    int rc = cbx_objectmanager_enumerate(tab->backend, tab->bus, &tab->model);
    if (rc != 0)
        return rc;

    /* Query each target's DeviceType. */
    tab->device_type_count = 0;
    for (int i = 0; i < tab->model.target_count && i < CBX_CT_MAX_DEVICES; i++) {
        char *dtype = NULL;
        rc = ip_target_get_device_type(tab->backend, tab->bus,
                                         tab->model.targets[i].path,
                                         &dtype);
        if (rc == 0 && dtype) {
            snprintf(tab->device_types[i], CBX_MAX_TYPE_LEN, "%s", dtype);
            free(dtype);
        } else {
            tab->device_types[i][0] = '\0';
        }
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

    return 0;
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

    int previous_count = tab->model.target_count;
    char *out_path = NULL;
    int rc = ip_manager_create_target_device(tab->backend, tab->bus,
                                               type, &out_path);
    if (rc != 0)
        return rc;

    rc = cbx_controllers_tab_refresh(tab);
    if (rc == 0 && tab->model.target_count != previous_count + 1)
        rc = -EIO;
    bool found = false;
    if (rc == 0 && out_path) {
        for (int i = 0; i < tab->model.target_count; i++) {
            if (strcmp(tab->model.targets[i].path, out_path) == 0) {
                found = true;
                break;
            }
        }
        if (!found)
            rc = -EIO;
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

    const char *path = tab->model.targets[device_index].path;
    int rc = ip_manager_stop_target_device(tab->backend, tab->bus, path);
    if (rc != 0)
        return rc;

    /* SPEC §5.2: physical controller in that slot auto-moves to
     * Unassigned; InputPlumber stops the target device. */
    return cbx_controllers_tab_refresh(tab);
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

    /* Find the composite for this target.  In the common case,
     * composite index = target list index. */
    if (device_index >= tab->model.composite_count)
        return -EINVAL;

    const char *composite_path = tab->model.composites[device_index].path;

    /* Build the new types CSV: current types with the changed one
     * replaced.  In the simple case (one target per composite), the
     * CSV is just the new type.  For multiple targets, we replace
     * the one at device_index. */
    char csv[CBX_MAX_CONTROLLERS * (CBX_MAX_TYPE_LEN + 1)];
    csv[0] = '\0';

    for (int i = 0; i < tab->model.target_count; i++) {
        char type_buf[CBX_MAX_TYPE_LEN];
        const char *t;

        if (i == device_index)
            t = new_type;
        else if (i < tab->device_type_count)
            t = tab->device_types[i];
        else {
            /* Query the type if we don't have it cached. */
            char *dtype = NULL;
            int rc = ip_target_get_device_type(tab->backend, tab->bus,
                                                 tab->model.targets[i].path,
                                                 &dtype);
            if (rc == 0 && dtype) {
                snprintf(type_buf, CBX_MAX_TYPE_LEN, "%s", dtype);
                free(dtype);
                t = type_buf;
            } else {
                t = "gamepad"; /* fallback */
            }
        }

        if (csv[0] != '\0')
            strncat(csv, ",", sizeof(csv) - strlen(csv) - 1);
        strncat(csv, t, sizeof(csv) - strlen(csv) - 1);
    }

    int rc = ip_composite_set_target_devices(tab->backend, tab->bus,
                                                composite_path, csv);
    if (rc != 0)
        return rc;

    /* Refresh to show the updated type; stale UI is an operation error. */
    return cbx_controllers_tab_refresh(tab);
}

int
cbx_controllers_tab_begin_type_pick(cbx_controllers_tab *tab,
                                       cbx_ct_action action)
{
    if (!tab)
        return -EINVAL;
    if (tab->supported_type_count == 0)
        return -ENOENT;

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
    if (action == CBX_CT_ACTION_ADD)
        return cbx_controllers_tab_add(tab, type);
    if (action == CBX_CT_ACTION_CHANGE)
        return cbx_controllers_tab_change_type(tab, device_index, type);

    return -EINVAL;
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