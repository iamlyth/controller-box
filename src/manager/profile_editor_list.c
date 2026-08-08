/*
 * profile_editor_list.c — Binding list mode editor (Task 37, right panel).
 *
 * Implements the profile editor's list mode with a controller diagram
 * (left) and binding list (right).  The user navigates bindings with
 * Up/Down, presses A to edit (target-pick or capture mode), and the
 * diagram stays synchronised with the list selection.
 *
 * Task 37 — Profile editor — controller diagram and binding list mode.
 */
#include "manager/profile_editor_list.h"

#include <SDL2/SDL.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dbus/ip_composite.h"

/* ------------------------------------------------------------------ */
/*  Layout constants                                                  */
/* ------------------------------------------------------------------ */

#define CBX_PE_DIAGRAM_W  300
#define CBX_PE_DIAGRAM_H  300
#define CBX_PE_LIST_X     330
#define CBX_PE_LIST_W     580
#define CBX_PE_LIST_H     420
#define CBX_PE_LIST_Y     60
#define CBX_PE_TITLE_H     40
#define CBX_PE_STATUS_H   36
#define CBX_PE_TARGET_LIST_H 420

/* Progress bar (sequential mode, Task 38) */
#define CBX_PE_PROGRESS_W  580
#define CBX_PE_PROGRESS_H  24
#define CBX_PE_PROGRESS_Y  (CBX_PE_LIST_Y + CBX_PE_LIST_H + 8)

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/*
 * Find the "button" property in a source event's props.
 * Returns the value string, or NULL if not found.
 */
static const char *
source_event_button(const cbx_source_event *se)
{
    if (!se)
        return NULL;
    for (int i = 0; i < se->prop_count; i++) {
        if (strcmp(se->props[i].key, "button") == 0)
            return se->props[i].value;
    }
    /* Also check "axis" for stick axes */
    for (int i = 0; i < se->prop_count; i++) {
        if (strcmp(se->props[i].key, "axis") == 0)
            return se->props[i].value;
    }
    return NULL;
}

/*
 * Build a display label for a binding: "source → target1, target2"
 */
static void
format_binding_label(char *buf, size_t buflen,
                       const cbx_profile_mapping *m)
{
    if (!buf || buflen == 0 || !m)
        return;

    const char *btn = source_event_button(&m->source_event);
    if (!btn || !btn[0])
        btn = m->source_event.device_class;

    /* Build targets string */
    char targets_str[128] = "";
    for (int i = 0; i < m->target_event_count && i < 8; i++) {
        char tmp[64];
        if (i > 0)
            snprintf(tmp, sizeof(tmp), ", %s", m->target_events[i].value);
        else
            snprintf(tmp, sizeof(tmp), "%s", m->target_events[i].value);
        strncat(targets_str, tmp, sizeof(targets_str) - strlen(targets_str) - 1);
    }
    if (targets_str[0] == '\0')
        snprintf(targets_str, sizeof(targets_str), "(none)");

    snprintf(buf, buflen, "%s → %s", btn, targets_str);
}

/*
 * Map a source event's button name to a diagram button.
 * Uses cbx_profile_diagram_button_from_name which matches canonical names.
 */
static cbx_diag_button
source_to_diag_button(const cbx_source_event *se)
{
    const char *btn = source_event_button(se);
    if (!btn)
        return CBX_DIAG_BTN_NONE;
    return cbx_profile_diagram_button_from_name(btn);
}

/*
 * Update the diagram highlight to match the current list selection.
 */
static void
sync_diagram_highlight(cbx_profile_editor *ed)
{
    if (!ed)
        return;

    if (ed->selected_index < 0 || ed->selected_index >= ed->profile.mapping_count) {
        cbx_profile_diagram_clear_highlight(&ed->diagram);
        return;
    }

    cbx_diag_button btn = source_to_diag_button(
        &ed->profile.mappings[ed->selected_index].source_event);
    cbx_profile_diagram_highlight(&ed->diagram, btn);
}

/*
 * Parse a comma-separated capabilities string into target entries.
 * Each token becomes a target with device_class = cap (the capability
 * name as-is) and value = the token.  The label is "cap:token".
 */
static int
parse_capabilities_csv(const char *csv, cbx_pe_target *targets,
                         int *count, int max, const char *device_class)
{
    if (!csv || !targets || !count)
        return 0;

    int added = 0;
    const char *p = csv;

    while (*p && added < max) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;

        const char *start = p;
        while (*p && *p != ',')
            p++;
        const char *end = p;

        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            end--;

        int len = (int)(end - start);
        if (len > 0 && len < (int)sizeof(targets[*count].value)) {
            cbx_pe_target *t = &targets[*count];
            strncpy(t->device_class, device_class, sizeof(t->device_class) - 1);
            t->device_class[sizeof(t->device_class) - 1] = '\0';
            memcpy(t->value, start, (size_t)len);
            t->value[len] = '\0';
            snprintf(t->label, sizeof(t->label), "%s:%s",
                      device_class, t->value);
            (*count)++;
            added++;
        }

        if (*p == ',')
            p++;
    }

    return added;
}

/* ------------------------------------------------------------------ */
/*  Pointer-path callbacks for editor lists                          */
/* ------------------------------------------------------------------ */

/* Fires when the binding list is activated via mouse click or A-KEYUP.
 * Calls the same activate function the controller path reaches via
 * cbx_profiles_tab_activate, so both paths produce the same outcome. */
static void
on_binding_selected(cbx_widget *w, int index, void *user_data)
{
    (void)w;
    (void)index;
    cbx_profile_editor *ed = (cbx_profile_editor *)user_data;
    if (ed)
        cbx_profile_editor_activate(ed);
}

/* Fires when the target list is activated via mouse click or A-KEYUP.
 * Works in both BINDING_EDIT mode (selecting Pick Target / Capture /
 * Sequential) and TARGET_PICK mode (confirming a target event). */
static void
on_target_selected(cbx_widget *w, int index, void *user_data)
{
    (void)w;
    (void)index;
    cbx_profile_editor *ed = (cbx_profile_editor *)user_data;
    if (ed)
        cbx_profile_editor_activate(ed);
}

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                         */
/* ------------------------------------------------------------------ */

int
cbx_profile_editor_init(cbx_profile_editor *ed,
                          cbx_panel *panel,
                          SDL_Renderer *renderer,
                          cbx_text_cache *cache,
                          const cbx_theme *theme,
                          int font_id)
{
    if (!ed || !panel)
        return -EINVAL;

    memset(ed, 0, sizeof(*ed));
    ed->panel = panel;
    ed->text_cache = cache;
    ed->theme = theme;
    ed->font_id = font_id;
    ed->renderer = renderer;
    ed->mode = CBX_EDITOR_MODE_LIST;
    ed->selected_index = -1;
    ed->editing_index = -1;
    ed->capture_active = false;
    ed->captured_button = CBX_DIAG_BTN_NONE;

    cbx_profile_init(&ed->profile);

    /* Get panel origin for relative widget positioning. */
    SDL_Rect pr = {0, 0, 0, 0};
    cbx_widget_get_rect(&panel->base, &pr);
    const int px = pr.x;
    const int py = pr.y;

    /* --- Diagram (left panel) ------------------------------------- */
    int rc = cbx_profile_diagram_init(&ed->diagram, renderer, NULL, theme);
    if (rc != 0)
        return rc;
    SDL_Rect diag_rect = { px + 16, py + CBX_PE_TITLE_H, CBX_PE_DIAGRAM_W,
                            CBX_PE_DIAGRAM_H };
    cbx_widget_set_rect(&ed->diagram.base, &diag_rect);

    /* --- Title label ---------------------------------------------- */
    rc = cbx_label_init(&ed->title_lbl, "Profile Editor", font_id,
                          cache, theme);
    if (rc != 0) {
        cbx_widget_destroy(&ed->diagram.base);
        return rc;
    }
    SDL_Rect title_rect = { px + 16, py + 8, CBX_PE_DIAGRAM_W, CBX_PE_TITLE_H };
    cbx_widget_set_rect(&ed->title_lbl.base, &title_rect);

    /* --- Binding list (right panel) ------------------------------- */
    rc = cbx_list_init(&ed->binding_list, font_id, cache, theme);
    if (rc != 0) {
        cbx_widget_destroy(&ed->diagram.base);
        cbx_widget_destroy(&ed->title_lbl.base);
        return rc;
    }
    SDL_Rect list_rect = { px + CBX_PE_LIST_X, py + CBX_PE_LIST_Y,
                            CBX_PE_LIST_W, CBX_PE_LIST_H };
    cbx_widget_set_rect(&ed->binding_list.base, &list_rect);

    /* --- Target picker list (hidden initially) -------------------- */
    rc = cbx_list_init(&ed->target_list, font_id, cache, theme);
    if (rc != 0) {
        cbx_widget_destroy(&ed->diagram.base);
        cbx_widget_destroy(&ed->title_lbl.base);
        cbx_widget_destroy(&ed->binding_list.base);
        return rc;
    }
    cbx_widget_set_visible(&ed->target_list.base, false);
    cbx_widget_set_rect(&ed->target_list.base, &list_rect);

    /* Wire on_select callbacks so pointer (mouse) activation works.
     * For the controller path, A-KEYUP falls through to
     * cbx_profiles_tab_activate -> cbx_profile_editor_activate.
     * For the pointer path, MOUSEUP fires on_select which calls
     * the same activate function. */
    cbx_list_set_select_cb(&ed->binding_list, on_binding_selected);
    cbx_list_set_select_cb(&ed->target_list, on_target_selected);

    /* --- Status label --------------------------------------------- */
    rc = cbx_label_init(&ed->status_lbl, "", font_id, cache, theme);
    if (rc != 0) {
        cbx_widget_destroy(&ed->diagram.base);
        cbx_widget_destroy(&ed->title_lbl.base);
        cbx_widget_destroy(&ed->binding_list.base);
        cbx_widget_destroy(&ed->target_list.base);
        return rc;
    }
    SDL_Rect status_rect = { px + 16,
        py + CBX_PE_TITLE_H + CBX_PE_DIAGRAM_H + 8,
        CBX_PE_LIST_X + CBX_PE_LIST_W - 16,
        CBX_PE_STATUS_H };
    cbx_widget_set_rect(&ed->status_lbl.base, &status_rect);

    /* --- Progress bar (Task 38 — sequential mode) ------------------- */
    rc = cbx_progress_init(&ed->progress_bar, theme);
    if (rc != 0) {
        cbx_widget_destroy(&ed->diagram.base);
        cbx_widget_destroy(&ed->title_lbl.base);
        cbx_widget_destroy(&ed->binding_list.base);
        cbx_widget_destroy(&ed->target_list.base);
        cbx_widget_destroy(&ed->status_lbl.base);
        return rc;
    }
    SDL_Rect prog_rect = { px + CBX_PE_LIST_X, py + CBX_PE_PROGRESS_Y,
                             CBX_PE_PROGRESS_W, CBX_PE_PROGRESS_H };
    cbx_widget_set_rect(&ed->progress_bar.base, &prog_rect);
    cbx_widget_set_visible(&ed->progress_bar.base, false);

    /* --- Add widgets to panel ------------------------------------- */
    cbx_panel_add_child(panel, &ed->title_lbl.base);
    cbx_panel_add_child(panel, &ed->diagram.base);
    cbx_panel_add_child(panel, &ed->binding_list.base);
    cbx_panel_add_child(panel, &ed->target_list.base);
    cbx_panel_add_child(panel, &ed->status_lbl.base);
    cbx_panel_add_child(panel, &ed->progress_bar.base);

    return 0;
}

void
cbx_profile_editor_shutdown(cbx_profile_editor *ed)
{
    if (!ed)
        return;

    /* Remove children from panel */
    if (ed->panel) {
        cbx_panel_remove_child(ed->panel, &ed->title_lbl.base);
        cbx_panel_remove_child(ed->panel, &ed->diagram.base);
        cbx_panel_remove_child(ed->panel, &ed->binding_list.base);
        cbx_panel_remove_child(ed->panel, &ed->target_list.base);
        cbx_panel_remove_child(ed->panel, &ed->status_lbl.base);
        cbx_panel_remove_child(ed->panel, &ed->progress_bar.base);
    }

    cbx_widget_destroy(&ed->title_lbl.base);
    cbx_widget_destroy(&ed->diagram.base);
    cbx_widget_destroy(&ed->binding_list.base);
    cbx_widget_destroy(&ed->target_list.base);
    cbx_widget_destroy(&ed->status_lbl.base);
    cbx_widget_destroy(&ed->progress_bar.base);

    memset(ed, 0, sizeof(*ed));
}

/* ------------------------------------------------------------------ */
/*  Profile loading                                                    */
/* ------------------------------------------------------------------ */

int
cbx_profile_editor_load_profile(cbx_profile_editor *ed,
                                   const cbx_profile *profile)
{
    if (!ed || !profile)
        return -EINVAL;

    ed->profile = *profile;
    ed->profile_loaded = true;

    /* Update title */
    if (ed->profile.name[0])
        cbx_label_set_text(&ed->title_lbl, ed->profile.name);
    else
        cbx_label_set_text(&ed->title_lbl, "Profile Editor");

    return cbx_profile_editor_refresh(ed);
}

const cbx_profile *
cbx_profile_editor_get_profile(const cbx_profile_editor *ed)
{
    if (!ed || !ed->profile_loaded)
        return NULL;
    return &ed->profile;
}

/* ------------------------------------------------------------------ */
/*  DBus / capabilities                                                */
/* ------------------------------------------------------------------ */

void
cbx_profile_editor_set_dbus(cbx_profile_editor *ed,
                              const ip_dbus_backend *backend,
                              ip_bus_handle bus,
                              const char *composite_path)
{
    if (!ed)
        return;
    ed->backend = backend;
    ed->bus = bus;
    if (composite_path) {
        strncpy(ed->composite_path, composite_path,
                 sizeof(ed->composite_path) - 1);
        ed->composite_path[sizeof(ed->composite_path) - 1] = '\0';
    } else {
        ed->composite_path[0] = '\0';
    }

    /* Resolve InputPlumber's unique bus name for InputEvent sender
     * verification.  DBus message sender fields contain unique
     * connection names (e.g. ":1.42"), not well-known names — using
     * the well-known name (IP_DBUS_NAME) means strcmp always fails
     * and all legitimate InputEvent signals are silently dropped. */
    ed->expected_sender[0] = '\0';
    if (backend && bus && backend->get_unique_name) {
        char *unique = NULL;
        if (backend->get_unique_name(bus, IP_DBUS_NAME, &unique) == 0
            && unique) {
            strncpy(ed->expected_sender, unique,
                     sizeof(ed->expected_sender) - 1);
            ed->expected_sender[sizeof(ed->expected_sender) - 1] = '\0';
            free(unique);
        }
    }
}

int
cbx_profile_editor_load_capabilities(cbx_profile_editor *ed)
{
    if (!ed)
        return -EINVAL;

    ed->target_count = 0;

    /* Read capabilities from DBus if backend is set */
    if (ed->backend && ed->bus && ed->composite_path[0]) {
        char *caps = NULL;
        char *out_caps = NULL;
        char *tgt_caps = NULL;

        if (ip_composite_get_capabilities(ed->backend, ed->bus,
                                            ed->composite_path, &caps) == 0
            && caps) {
            parse_capabilities_csv(caps, ed->targets, &ed->target_count,
                                     CBX_PE_MAX_TARGETS, "gamepad");
            free(caps);
        }

        if (ip_composite_get_output_capabilities(ed->backend, ed->bus,
                                                   ed->composite_path,
                                                   &out_caps) == 0
            && out_caps) {
            parse_capabilities_csv(out_caps, ed->targets, &ed->target_count,
                                     CBX_PE_MAX_TARGETS, "output");
            free(out_caps);
        }

        if (ip_composite_get_target_capabilities(ed->backend, ed->bus,
                                                    ed->composite_path,
                                                    &tgt_caps) == 0
            && tgt_caps) {
            parse_capabilities_csv(tgt_caps, ed->targets, &ed->target_count,
                                     CBX_PE_MAX_TARGETS, "target");
            free(tgt_caps);
        }
    }

    /* If no targets loaded from DBus, add some defaults */
    if (ed->target_count == 0) {
        static const char *default_targets[] = {
            "keyboard:KeyA", "keyboard:KeyB", "keyboard:KeyC",
            "keyboard:KeyD", "keyboard:KeyE", "keyboard:KeyF",
            "keyboard:KeyEsc", "keyboard:KeyReturn",
            "mouse:ButtonLeft", "mouse:ButtonRight",
        };
        int n = (int)(sizeof(default_targets) / sizeof(default_targets[0]));
        for (int i = 0; i < n && ed->target_count < CBX_PE_MAX_TARGETS; i++) {
            cbx_pe_target *t = &ed->targets[ed->target_count];
            const char *colon = strchr(default_targets[i], ':');
            if (colon) {
                int dlen = (int)(colon - default_targets[i]);
                if (dlen >= (int)sizeof(t->device_class))
                    dlen = (int)sizeof(t->device_class) - 1;
                memcpy(t->device_class, default_targets[i], (size_t)dlen);
                t->device_class[dlen] = '\0';
                strncpy(t->value, colon + 1, sizeof(t->value) - 1);
                t->value[sizeof(t->value) - 1] = '\0';
            } else {
                strncpy(t->device_class, "keyboard",
                         sizeof(t->device_class) - 1);
                t->device_class[sizeof(t->device_class) - 1] = '\0';
                strncpy(t->value, default_targets[i], sizeof(t->value) - 1);
                t->value[sizeof(t->value) - 1] = '\0';
            }
            snprintf(t->label, sizeof(t->label), "%s", default_targets[i]);
            ed->target_count++;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Refresh: rebuild binding list from profile                        */
/* ------------------------------------------------------------------ */

int
cbx_profile_editor_refresh(cbx_profile_editor *ed)
{
    if (!ed)
        return -EINVAL;

    cbx_list_clear(&ed->binding_list);

    for (int i = 0; i < ed->profile.mapping_count; i++) {
        char label[CBX_PE_LABEL_LEN];
        format_binding_label(label, sizeof(label),
                               &ed->profile.mappings[i]);
        cbx_list_add_item(&ed->binding_list, label, NULL, ed);
    }

    /* Set selection */
    if (ed->profile.mapping_count > 0) {
        if (ed->selected_index < 0)
            ed->selected_index = 0;
        if (ed->selected_index >= ed->profile.mapping_count)
            ed->selected_index = ed->profile.mapping_count - 1;
    } else {
        ed->selected_index = -1;
    }
    cbx_list_set_selected(&ed->binding_list, ed->selected_index);

    sync_diagram_highlight(ed);

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Navigation                                                         */
/* ------------------------------------------------------------------ */

int
cbx_profile_editor_move_up(cbx_profile_editor *ed)
{
    if (!ed || ed->profile.mapping_count <= 0)
        return -1;

    if (ed->mode == CBX_EDITOR_MODE_TARGET_PICK ||
        ed->mode == CBX_EDITOR_MODE_BINDING_EDIT) {
        int sel = cbx_list_get_selected(&ed->target_list);
        if (sel > 0)
            cbx_list_set_selected(&ed->target_list, sel - 1);
        return cbx_list_get_selected(&ed->target_list);
    }

    if (ed->selected_index > 0)
        ed->selected_index--;
    else
        ed->selected_index = ed->profile.mapping_count - 1;  /* wrap */

    cbx_list_set_selected(&ed->binding_list, ed->selected_index);
    sync_diagram_highlight(ed);
    return ed->selected_index;
}

int
cbx_profile_editor_move_down(cbx_profile_editor *ed)
{
    if (!ed || ed->profile.mapping_count <= 0)
        return -1;

    if (ed->mode == CBX_EDITOR_MODE_TARGET_PICK ||
        ed->mode == CBX_EDITOR_MODE_BINDING_EDIT) {
        int sel = cbx_list_get_selected(&ed->target_list);
        int cnt = cbx_list_item_count(&ed->target_list);
        if (sel < cnt - 1)
            cbx_list_set_selected(&ed->target_list, sel + 1);
        return cbx_list_get_selected(&ed->target_list);
    }

    if (ed->selected_index < ed->profile.mapping_count - 1)
        ed->selected_index++;
    else
        ed->selected_index = 0;  /* wrap */

    cbx_list_set_selected(&ed->binding_list, ed->selected_index);
    sync_diagram_highlight(ed);
    return ed->selected_index;
}

int
cbx_profile_editor_activate(cbx_profile_editor *ed)
{
    if (!ed)
        return -EINVAL;

    if (ed->mode == CBX_EDITOR_MODE_TARGET_PICK)
        return cbx_profile_editor_confirm_target_pick(ed);

    if (ed->mode == CBX_EDITOR_MODE_CAPTURE) {
        /* In capture mode, A does nothing (wait for physical press) */
        return 0;
    }

    if (ed->mode == CBX_EDITOR_MODE_BINDING_EDIT) {
        /* Confirm the selected action in the binding edit sub-menu. */
        int idx = cbx_list_get_selected(&ed->target_list);
        if (idx < 0)
            return -EINVAL;

        /* Restore binding list visibility before dispatching. */
        cbx_widget_set_visible(&ed->target_list.base, false);
        cbx_widget_set_visible(&ed->binding_list.base, true);

        switch (idx) {
        case 0:  /* Pick Target */
            return cbx_profile_editor_begin_target_pick(ed);
        case 1:  /* Capture */
            return cbx_profile_editor_begin_capture(ed);
        case 2:  /* Sequential (All Buttons) */
            return cbx_profile_editor_begin_sequential(ed);
        default:
            ed->mode = CBX_EDITOR_MODE_LIST;
            ed->editing_index = -1;
            cbx_label_set_text(&ed->status_lbl, "");
            return -EINVAL;
        }
    }

    if (ed->mode == CBX_EDITOR_MODE_SEQUENTIAL) {
        /* A in sequential mode does nothing (wait for physical press) */
        return 0;
    }

    /* LIST mode: open the binding edit sub-menu. */
    if (ed->selected_index < 0 || ed->selected_index >= ed->profile.mapping_count)
        return -EINVAL;

    ed->editing_index = ed->selected_index;
    ed->mode = CBX_EDITOR_MODE_BINDING_EDIT;

    /* Populate target list with the three edit options. */
    cbx_list_clear(&ed->target_list);
    cbx_list_add_item(&ed->target_list, "Pick Target", NULL, ed);
    cbx_list_add_item(&ed->target_list, "Capture", NULL, ed);
    cbx_list_add_item(&ed->target_list, "Sequential (All Buttons)", NULL, ed);
    cbx_list_set_selected(&ed->target_list, 0);

    /* Show target list, hide binding list. */
    cbx_widget_set_visible(&ed->binding_list.base, false);
    cbx_widget_set_visible(&ed->target_list.base, true);
    cbx_label_set_text(&ed->status_lbl,
                         "Edit binding: A=select  B=back");

    return 0;
}

int
cbx_profile_editor_cancel(cbx_profile_editor *ed)
{
    if (!ed)
        return -EINVAL;

    if (ed->mode == CBX_EDITOR_MODE_TARGET_PICK) {
        cbx_profile_editor_cancel_target_pick(ed);
        return 0;
    }

    if (ed->mode == CBX_EDITOR_MODE_CAPTURE) {
        cbx_profile_editor_cancel_capture(ed);
        return 0;
    }

    if (ed->mode == CBX_EDITOR_MODE_SEQUENTIAL) {
        cbx_profile_editor_cancel_sequential(ed);
        return 0;
    }

    if (ed->mode == CBX_EDITOR_MODE_BINDING_EDIT) {
        /* Cancel binding edit sub-menu, return to list mode. */
        cbx_widget_set_visible(&ed->target_list.base, false);
        cbx_widget_set_visible(&ed->binding_list.base, true);
        ed->mode = CBX_EDITOR_MODE_LIST;
        ed->editing_index = -1;
        cbx_label_set_text(&ed->status_lbl, "");
        return 0;
    }

    return -ENOENT;  /* in list mode, nothing to cancel */
}

/* ------------------------------------------------------------------ */
/*  Target pick mode                                                   */
/* ------------------------------------------------------------------ */

int
cbx_profile_editor_begin_target_pick(cbx_profile_editor *ed)
{
    if (!ed || ed->selected_index < 0
        || ed->selected_index >= ed->profile.mapping_count)
        return -EINVAL;

    /* Ensure we have targets */
    if (ed->target_count == 0) {
        int rc = cbx_profile_editor_load_capabilities(ed);
        if (rc != 0)
            return rc;
    }

    if (ed->target_count == 0)
        return -ENODATA;

    /* Populate target list */
    cbx_list_clear(&ed->target_list);
    for (int i = 0; i < ed->target_count; i++) {
        cbx_list_add_item(&ed->target_list, ed->targets[i].label,
                           NULL, ed);
    }
    cbx_list_set_selected(&ed->target_list, 0);

    /* Switch UI: hide binding list, show target list */
    cbx_widget_set_visible(&ed->binding_list.base, false);
    cbx_widget_set_visible(&ed->target_list.base, true);

    ed->editing_index = ed->selected_index;
    ed->mode = CBX_EDITOR_MODE_TARGET_PICK;
    cbx_label_set_text(&ed->status_lbl,
                         "Select target event.  A=Confirm  B=Cancel");

    return 0;
}

int
cbx_profile_editor_confirm_target_pick(cbx_profile_editor *ed)
{
    if (!ed || ed->mode != CBX_EDITOR_MODE_TARGET_PICK)
        return -EINVAL;

    int tgt_idx = cbx_list_get_selected(&ed->target_list);
    if (tgt_idx < 0 || tgt_idx >= ed->target_count)
        return -EINVAL;

    int map_idx = ed->editing_index;
    if (map_idx < 0 || map_idx >= ed->profile.mapping_count)
        return -EINVAL;

    /* Apply the selected target to the binding */
    cbx_profile_mapping *m = &ed->profile.mappings[map_idx];
    cbx_pe_target *tgt = &ed->targets[tgt_idx];

    /* Set the first target event to the selected target */
    if (m->target_event_count == 0)
        m->target_event_count = 1;

    strncpy(m->target_events[0].device_class, tgt->device_class,
             sizeof(m->target_events[0].device_class) - 1);
    m->target_events[0].device_class[sizeof(m->target_events[0].device_class) - 1] = '\0';
    strncpy(m->target_events[0].value, tgt->value,
             sizeof(m->target_events[0].value) - 1);
    m->target_events[0].value[sizeof(m->target_events[0].value) - 1] = '\0';

    /* Return to list mode */
    cbx_profile_editor_cancel_target_pick(ed);

    /* Refresh binding list to show updated label */
    cbx_profile_editor_refresh(ed);

    return 0;
}

void
cbx_profile_editor_cancel_target_pick(cbx_profile_editor *ed)
{
    if (!ed)
        return;

    cbx_widget_set_visible(&ed->target_list.base, false);
    cbx_widget_set_visible(&ed->binding_list.base, true);

    ed->editing_index = -1;
    ed->mode = CBX_EDITOR_MODE_LIST;
    cbx_label_set_text(&ed->status_lbl, "");
}

/* ------------------------------------------------------------------ */
/*  Capture mode                                                       */
/* ------------------------------------------------------------------ */

int
cbx_profile_editor_begin_capture(cbx_profile_editor *ed)
{
    if (!ed || ed->selected_index < 0
        || ed->selected_index >= ed->profile.mapping_count)
        return -EINVAL;

    ed->editing_index = ed->selected_index;
    ed->capture_active = true;
    ed->captured_button = CBX_DIAG_BTN_NONE;
    ed->mode = CBX_EDITOR_MODE_CAPTURE;
    cbx_label_set_text(&ed->status_lbl,
                         "Press a button to capture...  B=Cancel");

    /* Initialize input event handler — use the unique bus name
     * (expected_sender) resolved in set_dbus(), NOT the well-known
     * name (IP_DBUS_NAME).  DBus message sender fields contain unique
     * connection names, so using the well-known name means strcmp
     * always fails and legitimate InputEvent signals are dropped. */
    if (ed->backend && ed->bus) {
        ip_input_events_init(&ed->input_events, ed->backend, ed->bus,
                              ed->expected_sender[0] ? ed->expected_sender : NULL,
                              cbx_profile_editor_on_input_event, ed);
        ip_input_events_subscribe(&ed->input_events);
    }

    return 0;
}

void
cbx_profile_editor_cancel_capture(cbx_profile_editor *ed)
{
    if (!ed)
        return;

    ed->capture_active = false;
    ed->editing_index = -1;
    ed->captured_button = CBX_DIAG_BTN_NONE;
    ed->mode = CBX_EDITOR_MODE_LIST;
    cbx_label_set_text(&ed->status_lbl, "");
}

void
cbx_profile_editor_on_input_event(ip_input_id input,
                                    ip_input_category category,
                                    double value,
                                    const char *raw_event,
                                    const char *device_path,
                                    void *userdata)
{
    cbx_profile_editor *ed = (cbx_profile_editor *)userdata;
    if (!ed)
        return;

    /* Dispatch to sequential mode handler if active */
    if (ed->mode == CBX_EDITOR_MODE_SEQUENTIAL) {
        cbx_profile_editor_seq_on_input(input, category, value,
                                           raw_event, device_path, userdata);
        return;
    }

    /* Otherwise, handle capture mode */
    if (!ed->capture_active)
        return;

    /* Only capture button presses (value == 1.0), not releases */
    if (value != 1.0)
        return;

    if (!raw_event)
        return;

    int map_idx = ed->editing_index;
    if (map_idx < 0 || map_idx >= ed->profile.mapping_count)
        return;

    /* Set the source event's button to the captured event */
    cbx_profile_mapping *m = &ed->profile.mappings[map_idx];

    /* Find or create the "button" prop */
    int prop_idx = -1;
    for (int i = 0; i < m->source_event.prop_count; i++) {
        if (strcmp(m->source_event.props[i].key, "button") == 0
            || strcmp(m->source_event.props[i].key, "axis") == 0) {
            prop_idx = i;
            break;
        }
    }

    if (prop_idx < 0) {
        if (m->source_event.prop_count < CBX_MAX_EVENT_PROPS) {
            prop_idx = m->source_event.prop_count++;
            strncpy(m->source_event.props[prop_idx].key, "button",
                     sizeof(m->source_event.props[prop_idx].key) - 1);
            m->source_event.props[prop_idx].key
                [sizeof(m->source_event.props[prop_idx].key) - 1] = '\0';
        } else {
            return;
        }
    }

    strncpy(m->source_event.props[prop_idx].value, raw_event,
             sizeof(m->source_event.props[prop_idx].value) - 1);
    m->source_event.props[prop_idx].value
        [sizeof(m->source_event.props[prop_idx].value) - 1] = '\0';

    /* Store the captured button for diagram highlight */
    ed->captured_button = cbx_profile_diagram_button_from_name(raw_event);

    /* Exit capture mode */
    ed->capture_active = false;
    ed->mode = CBX_EDITOR_MODE_LIST;
    ed->editing_index = -1;
    cbx_label_set_text(&ed->status_lbl, "Captured!");

    /* Refresh binding list */
    cbx_profile_editor_refresh(ed);
}

/* ------------------------------------------------------------------ */
/*  Accessors (for testing)                                             */
/* ------------------------------------------------------------------ */

cbx_editor_mode
cbx_profile_editor_get_mode(const cbx_profile_editor *ed)
{
    if (!ed)
        return CBX_EDITOR_MODE_LIST;
    return ed->mode;
}

int
cbx_profile_editor_binding_count(const cbx_profile_editor *ed)
{
    if (!ed)
        return 0;
    return ed->profile.mapping_count;
}

int
cbx_profile_editor_get_selected(const cbx_profile_editor *ed)
{
    if (!ed)
        return -1;
    return ed->selected_index;
}

cbx_diag_button
cbx_profile_editor_get_diagram_highlight(const cbx_profile_editor *ed)
{
    if (!ed)
        return CBX_DIAG_BTN_NONE;
    return cbx_profile_diagram_get_highlight(&ed->diagram);
}

int
cbx_profile_editor_get_target_count(const cbx_profile_editor *ed)
{
    if (!ed)
        return 0;
    return ed->target_count;
}

const char *
cbx_profile_editor_get_status(const cbx_profile_editor *ed)
{
    if (!ed)
        return NULL;
    return ed->status_lbl.text;
}

int
cbx_profile_editor_get_editing_index(const cbx_profile_editor *ed)
{
    if (!ed)
        return -1;
    return ed->editing_index;
}

bool
cbx_profile_editor_is_capture_active(const cbx_profile_editor *ed)
{
    if (!ed)
        return false;
    return ed->capture_active;
}