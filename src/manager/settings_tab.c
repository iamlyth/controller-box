/*
 * settings_tab.c — Settings tab for the Manager UI (SPEC §5.5).
 *
 * Displays and edits app-level settings: launch at boot, theme, overlay
 * opacity, virtual controller startup config (count + types), overlay
 * trigger combo, and controller icon overrides (§8.4).
 *
 * Task 39 — Profile save and Settings tab.
 */
#include "manager/settings_tab.h"

#include <SDL2/SDL.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config/config_paths.h"

/* ------------------------------------------------------------------ */
/*  Layout constants                                                  */
/* ------------------------------------------------------------------ */

#define CBX_ST_LIST_H    420
#define CBX_ST_BTN_W    200
#define CBX_ST_BTN_H     44
#define CBX_ST_LIST_Y    16

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Known controller types for cycling (same as known_types in config_settings). */
static const char *const st_known_types[] = {
    "xb360", "ds5", "deck", "gamepad", "mouse", "keyboard", "touchscreen", NULL,
};

/* Icon override presets for the settings tab cycle UI (SPEC §8.4/§5.5).
 * Each preset is either "None" (clear overrides) or a single type-to-icon
 * override.  The data model supports multiple overrides; this UI provides
 * a simple cycle to set one at a time. */
static const struct {
    const char *type;  /* NULL = no override (clear all) */
    const char *icon;  /* icon name (built-in or absolute path) */
    const char *label; /* display label for the cycle entry */
} st_icon_presets[] = {
    { NULL,       NULL,           "None" },
    { "ds5",      "cc-xbox-360", "ds5 -> cc-xbox-360" },
    { "xb360",    "cc-ps5",      "xb360 -> cc-ps5" },
    { "deck",     "cc-xbox-360", "deck -> cc-xbox-360" },
    { "gamepad",  "cc-ps5",      "gamepad -> cc-ps5" },
};
#define ST_ICON_PRESET_COUNT ((int)(sizeof(st_icon_presets) / sizeof(st_icon_presets[0])))

/* Find the index of a string in a NULL-terminated array.
 * Returns the index, or -1 if not found. */
static int str_index(const char *const *arr, const char *val)
{
    if (!arr || !val)
        return -1;
    for (int i = 0; arr[i]; i++) {
        if (strcmp(arr[i], val) == 0)
            return i;
    }
    return -1;
}

/* Cycle to the next string in a NULL-terminated array.
 * If current is not found, returns the first entry. */
static const char *str_next(const char *const *arr, const char *current)
{
    int idx = str_index(arr, current);
    if (idx < 0)
        return arr[0];
    idx++;
    if (!arr[idx])
        idx = 0;
    return arr[idx];
}

/* Cycle to the previous string in a NULL-terminated array. */
static const char *str_prev(const char *const *arr, const char *current)
{
    int idx = str_index(arr, current);
    if (idx < 0)
        return arr[0];
    idx--;
    if (idx < 0) {
        /* count items */
        int n = 0;
        while (arr[n]) n++;
        idx = n - 1;
    }
    return arr[idx];
}

/* ------------------------------------------------------------------ */
/*  Dynamic row mapping (see settings_tab.h)                          */
/* ------------------------------------------------------------------ */

int cbx_settings_tab_type_row_count(const cbx_settings_tab *tab)
{
    if (!tab)
        return 0;
    int n = tab->settings.virtual_controllers.count;
    if (n < 1)
        n = 1;
    if (n > CBX_MAX_CONTROLLERS)
        n = CBX_MAX_CONTROLLERS;
    return n;
}

int cbx_settings_tab_row_for_type(const cbx_settings_tab *tab, int slot)
{
    if (slot < 0 || slot >= cbx_settings_tab_type_row_count(tab))
        return -1;
    return CBX_ST_BASE_ROWS + slot;
}

int cbx_settings_tab_trigger_row(const cbx_settings_tab *tab)
{
    return CBX_ST_BASE_ROWS + cbx_settings_tab_type_row_count(tab);
}

int cbx_settings_tab_icon_override_row(const cbx_settings_tab *tab)
{
    return cbx_settings_tab_trigger_row(tab) + 1;
}

int cbx_settings_tab_save_row(const cbx_settings_tab *tab)
{
    return cbx_settings_tab_trigger_row(tab) + 2;
}

int cbx_settings_tab_type_slot(const cbx_settings_tab *tab, int row)
{
    int slot = row - CBX_ST_BASE_ROWS;
    if (slot < 0 || slot >= cbx_settings_tab_type_row_count(tab))
        return -1;
    return slot;
}

bool cbx_settings_tab_row_editable(const cbx_settings_tab *tab, int row)
{
    if (!tab || row < 0)
        return false;
    /* Only "default" has an implemented palette (SPEC §13); presenting an
     * inert theme cycle would be dishonest. */
    if (row == CBX_ST_SET_THEME)
        return false;
    if (row < CBX_ST_BASE_ROWS)
        return true;
    if (cbx_settings_tab_type_slot(tab, row) >= 0)
        return true;
    if (row == cbx_settings_tab_trigger_row(tab))
        return true;
    if (row == cbx_settings_tab_icon_override_row(tab))
        return true;
    if (row == cbx_settings_tab_save_row(tab))
        return true;
    return false;
}

/* Map a list row to its logical setting kind.  Type rows all report
 * CBX_ST_SET_VC_TYPE_0 as a generic marker; callers must resolve the slot
 * with cbx_settings_tab_type_slot(). */
static cbx_st_setting st_row_kind(const cbx_settings_tab *tab, int row)
{
    if (!tab)
        return CBX_ST_SET_COUNT;
    switch (row) {
    case 0: return CBX_ST_SET_LAUNCH_BOOT;
    case 1: return CBX_ST_SET_THEME;
    case 2: return CBX_ST_SET_OPACITY;
    case 3: return CBX_ST_SET_VC_COUNT;
    default: break;
    }
    if (cbx_settings_tab_type_slot(tab, row) >= 0)
        return CBX_ST_SET_VC_TYPE_0;
    if (row == cbx_settings_tab_trigger_row(tab))
        return CBX_ST_SET_TRIGGER;
    if (row == cbx_settings_tab_icon_override_row(tab))
        return CBX_ST_SET_ICON_OVERRIDE;
    if (row == cbx_settings_tab_save_row(tab))
        return CBX_ST_SET_SAVE;
    return CBX_ST_SET_COUNT;
}

/* Build a display label for a settings-list row. */
static void format_setting_label(cbx_settings_tab *tab, char *buf,
                                   size_t buflen, int row)
{
    if (!buf || buflen == 0 || !tab)
        return;

    const cbx_settings *s = &tab->settings;
    cbx_st_setting kind = st_row_kind(tab, row);

    switch (kind) {
    case CBX_ST_SET_LAUNCH_BOOT:
        snprintf(buf, buflen, "Launch at Boot: %s",
                 s->launch_at_boot ? "On" : "Off");
        break;
    case CBX_ST_SET_THEME:
        snprintf(buf, buflen, "Theme: %s", s->theme);
        break;
    case CBX_ST_SET_OPACITY:
        snprintf(buf, buflen, "Overlay Opacity: %.2f", s->overlay_opacity);
        break;
    case CBX_ST_SET_VC_COUNT:
        snprintf(buf, buflen, "Virtual Controllers: %d",
                 s->virtual_controllers.count);
        break;
    case CBX_ST_SET_VC_TYPE_0: {
        int slot = cbx_settings_tab_type_slot(tab, row);
        if (slot >= 0)
            snprintf(buf, buflen, "  Controller %d Type: %s",
                     slot + 1, s->virtual_controllers.types[slot]);
        else
            buf[0] = '\0';
        break;
    }
    case CBX_ST_SET_TRIGGER:
        snprintf(buf, buflen, "Overlay Trigger: %s", s->overlay_trigger);
        break;
    case CBX_ST_SET_ICON_OVERRIDE: {
        if (s->icon_override_count > 0) {
            snprintf(buf, buflen, "Icon Override: %s -> %s",
                     s->icon_overrides[0].type, s->icon_overrides[0].icon);
        } else {
            snprintf(buf, buflen, "Icon Override: None");
        }
        break;
    }
    case CBX_ST_SET_SAVE:
        snprintf(buf, buflen, "Save Settings");
        break;
    default:
        if (buflen > 0)
            buf[0] = '\0';
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Forward declarations for on_select callbacks                       */
/* ------------------------------------------------------------------ */

static void on_setting_selected(cbx_widget *w, int index, void *user_data);

/* ------------------------------------------------------------------ */
/*  Button callbacks                                                   */
/* ------------------------------------------------------------------ */

static void on_save_pressed(cbx_widget *w, void *user_data)
{
    (void)w;
    cbx_settings_tab *tab = (cbx_settings_tab *)user_data;
    if (!tab)
        return;
    cbx_settings_tab_save(tab);
}

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                         */
/* ------------------------------------------------------------------ */

int cbx_settings_tab_init(cbx_settings_tab *tab,
                           cbx_panel *panel,
                           cbx_text_cache *cache,
                           const cbx_theme *theme,
                           int font_id)
{
    if (!tab || !panel)
        return -EINVAL;

    memset(tab, 0, sizeof(*tab));
    tab->panel      = panel;
    tab->text_cache = cache;
    tab->theme      = theme;
    tab->font_id    = font_id;
    tab->mode       = CBX_ST_MODE_LIST;
    tab->selected   = 0;
    tab->icon_preset_idx = 0;

    /* Load settings from disk (or defaults if no file). */
    int rc = cbx_settings_load(&tab->settings);
    if (rc < 0) {
        /* Fall back to defaults on error. */
        cbx_settings_defaults(&tab->settings);
    }
    tab->loaded = true;

    /* --- Settings list -------------------------------------------- */
    rc = cbx_list_init(&tab->settings_list, font_id, cache, theme);
    if (rc != 0)
        return rc;
    cbx_list_set_select_cb(&tab->settings_list, on_setting_selected);

    /* --- Save button ---------------------------------------------- */
    rc = cbx_button_init(&tab->save_btn, "Save Settings", font_id,
                          cache, theme, on_save_pressed, tab);
    if (rc != 0) {
        cbx_widget_destroy(&tab->settings_list.base);
        return rc;
    }

    /* --- Status label --------------------------------------------- */
    rc = cbx_label_init(&tab->status_lbl, "", font_id, cache, theme);
    if (rc != 0) {
        cbx_widget_destroy(&tab->settings_list.base);
        cbx_widget_destroy(&tab->save_btn.base);
        return rc;
    }

    /* --- Add widgets to panel ------------------------------------- */
    cbx_panel_add_child(panel, &tab->settings_list.base);
    cbx_panel_add_child(panel, &tab->save_btn.base);
    cbx_panel_add_child(panel, &tab->status_lbl.base);

    /* --- Layout --------------------------------------------------- */
    cbx_settings_tab_layout(tab);

    /* Populate the settings list. */
    cbx_settings_tab_refresh(tab);

    return 0;
}

void
cbx_settings_tab_layout(cbx_settings_tab *tab)
{
    if (!tab || !tab->panel)
        return;

    SDL_Rect rect;
    cbx_widget_get_rect(&tab->panel->base, &rect);

    /* Settings list: top area. */
    SDL_Rect list_rect = { rect.x + 16, rect.y + CBX_ST_LIST_Y,
                           rect.w - 32, CBX_ST_LIST_H };
    cbx_widget_set_rect(&tab->settings_list.base, &list_rect);

    /* Save button: below list. */
    int btn_y = rect.y + CBX_ST_LIST_Y + CBX_ST_LIST_H + 16;
    SDL_Rect btn_rect = { rect.x + 16, btn_y, CBX_ST_BTN_W, CBX_ST_BTN_H };
    cbx_widget_set_rect(&tab->save_btn.base, &btn_rect);

    /* Status label: below button. */
    SDL_Rect status_rect = { rect.x + 16, btn_y + CBX_ST_BTN_H + 16,
                              rect.w - 32, 32 };
    cbx_widget_set_rect(&tab->status_lbl.base, &status_rect);
}


int cbx_settings_tab_refresh(cbx_settings_tab *tab)
{
    if (!tab)
        return -EINVAL;

    cbx_list_clear(&tab->settings_list);

    int rows = cbx_settings_tab_setting_count(tab);
    char label[CBX_ST_LABEL_LEN];
    for (int i = 0; i < rows; i++) {
        format_setting_label(tab, label, sizeof(label), i);
        cbx_list_add_item(&tab->settings_list, label, NULL, tab);
    }

    /* Clamp selection. */
    if (tab->selected >= rows)
        tab->selected = rows - 1;
    if (tab->selected < 0)
        tab->selected = 0;
    cbx_list_set_selected(&tab->settings_list, tab->selected);

    return 0;
}

void cbx_settings_tab_shutdown(cbx_settings_tab *tab)
{
    if (!tab)
        return;

    if (tab->panel) {
        cbx_panel_remove_child(tab->panel, &tab->settings_list.base);
        cbx_panel_remove_child(tab->panel, &tab->save_btn.base);
        cbx_panel_remove_child(tab->panel, &tab->status_lbl.base);
    }

    cbx_widget_destroy(&tab->settings_list.base);
    cbx_widget_destroy(&tab->save_btn.base);
    cbx_widget_destroy(&tab->status_lbl.base);

    memset(tab, 0, sizeof(*tab));
}

/* ------------------------------------------------------------------ */
/*  Actions                                                            */
/* ------------------------------------------------------------------ */

int cbx_settings_tab_save(cbx_settings_tab *tab)
{
    if (!tab)
        return -EINVAL;

    /* Publish the working copy into the Manager's authoritative struct so
     * the Controllers tab and process-restart paths observe the same values.
     * The working copy is synchronised from the authoritative struct
     * whenever the tab becomes active, so it can never resurrect a stale
     * topology over a newer Controllers-tab change. */
    if (tab->external && tab->external != &tab->settings)
        *tab->external = tab->settings;

    const cbx_settings *to_save = tab->external ? tab->external
                                                : &tab->settings;
    int rc = cbx_settings_save(to_save);
    if (rc < 0) {
        cbx_label_set_text(&tab->status_lbl, "Save failed!");
        return rc;
    }

    cbx_label_set_text(&tab->status_lbl, "Settings saved.");
    return 0;
}

int cbx_settings_tab_set_external(cbx_settings_tab *tab,
                                   cbx_settings *external)
{
    if (!tab)
        return -EINVAL;
    tab->external = external;
    if (external) {
        tab->settings = *external;
        tab->loaded = true;
    }
    return cbx_settings_tab_refresh(tab);
}

int cbx_settings_tab_sync(cbx_settings_tab *tab)
{
    if (!tab)
        return -EINVAL;
    /* Never discard a setting being edited; the user must confirm or cancel
     * before the tab refreshes from the authoritative state. */
    if (tab->mode == CBX_ST_MODE_EDIT)
        return 0;

    if (tab->external) {
        tab->settings = *tab->external;
        tab->loaded = true;
    } else {
        int rc = cbx_settings_load(&tab->settings);
        if (rc < 0)
            cbx_settings_defaults(&tab->settings);
        tab->loaded = true;
    }
    return cbx_settings_tab_refresh(tab);
}

int cbx_settings_tab_move_up(cbx_settings_tab *tab)
{
    if (!tab)
        return -EINVAL;

    if (tab->mode == CBX_ST_MODE_EDIT)
        return cbx_settings_tab_edit_up(tab);

    tab->selected--;
    if (tab->selected < 0)
        tab->selected = CBX_ST_SET_COUNT - 1;
    cbx_list_set_selected(&tab->settings_list, tab->selected);
    return 0;
}

int cbx_settings_tab_move_down(cbx_settings_tab *tab)
{
    if (!tab)
        return -EINVAL;

    if (tab->mode == CBX_ST_MODE_EDIT)
        return cbx_settings_tab_edit_down(tab);

    tab->selected++;
    if (tab->selected >= CBX_ST_SET_COUNT)
        tab->selected = 0;
    cbx_list_set_selected(&tab->settings_list, tab->selected);
    return 0;
}

int cbx_settings_tab_activate(cbx_settings_tab *tab)
{
    if (!tab)
        return -EINVAL;

    if (tab->mode == CBX_ST_MODE_EDIT)
        return cbx_settings_tab_confirm_edit(tab);

    int row = tab->selected;
    cbx_st_setting kind = st_row_kind(tab, row);

    /* A row that is not adjustable must not silently enter an inert edit
     * mode (SPEC §5.5, §13). */
    if (!cbx_settings_tab_row_editable(tab, row)) {
        if (kind == CBX_ST_SET_THEME)
            cbx_label_set_text(&tab->status_lbl,
                "Only the default theme is implemented");
        return 0;
    }

    switch (kind) {
    case CBX_ST_SET_LAUNCH_BOOT:
        /* Toggle directly. */
        tab->settings.launch_at_boot = !tab->settings.launch_at_boot;
        cbx_settings_tab_refresh(tab);
        break;

    case CBX_ST_SET_SAVE:
        return cbx_settings_tab_save(tab);

    case CBX_ST_SET_OPACITY:
    case CBX_ST_SET_VC_COUNT:
    case CBX_ST_SET_VC_TYPE_0:
    case CBX_ST_SET_TRIGGER:
    case CBX_ST_SET_ICON_OVERRIDE:
        /* Enter edit mode for adjustable settings. */
        tab->mode = CBX_ST_MODE_EDIT;
        /* For icon override, initialize preset index from current state. */
        if (kind == CBX_ST_SET_ICON_OVERRIDE) {
            tab->icon_preset_idx = 0;  /* default to "None" */
            if (tab->settings.icon_override_count > 0) {
                /* Find matching preset for the first override. */
                for (int i = 1; i < ST_ICON_PRESET_COUNT; i++) {
                    if (strcmp(tab->settings.icon_overrides[0].type,
                               st_icon_presets[i].type) == 0 &&
                        strcmp(tab->settings.icon_overrides[0].icon,
                               st_icon_presets[i].icon) == 0) {
                        tab->icon_preset_idx = i;
                        break;
                    }
                }
            }
        }
        cbx_label_set_text(&tab->status_lbl,
                           "Editing: Up/Down to adjust, A=confirm, B=cancel");
        break;

    default:
        break;
    }

    return 0;
}

/* Apply the currently selected icon override preset to the settings struct.
 * Called during edit_up/edit_down cycling so the label preview is live. */
static void apply_icon_preset(cbx_settings_tab *tab)
{
    if (!tab || tab->icon_preset_idx < 0 ||
        tab->icon_preset_idx >= ST_ICON_PRESET_COUNT)
        return;

    /* Clear all existing overrides. */
    tab->settings.icon_override_count = 0;

    const char *type = st_icon_presets[tab->icon_preset_idx].type;
    const char *icon = st_icon_presets[tab->icon_preset_idx].icon;
    if (type && icon) {
        cbx_settings_set_icon_override(&tab->settings, type, icon);
    }

    cbx_settings_tab_refresh(tab);
}

int cbx_settings_tab_edit_up(cbx_settings_tab *tab)
{
    if (!tab || tab->mode != CBX_ST_MODE_EDIT)
        return -EINVAL;

    int row = tab->selected;
    cbx_st_setting kind = st_row_kind(tab, row);

    switch (kind) {
    case CBX_ST_SET_OPACITY:
        tab->settings.overlay_opacity += 0.05;
        if (tab->settings.overlay_opacity > 1.0)
            tab->settings.overlay_opacity = 1.0;
        break;
    case CBX_ST_SET_VC_COUNT:
        if (tab->settings.virtual_controllers.count < CBX_MAX_CONTROLLERS)
            tab->settings.virtual_controllers.count++;
        /* Pad types if needed. */
        for (int i = 0; i < tab->settings.virtual_controllers.count; i++) {
            if (tab->settings.virtual_controllers.types[i][0] == '\0')
                strncpy(tab->settings.virtual_controllers.types[i], "xb360",
                        sizeof(tab->settings.virtual_controllers.types[i]) - 1);
        }
        break;
    case CBX_ST_SET_VC_TYPE_0: {
        int slot = cbx_settings_tab_type_slot(tab, row);
        if (slot >= 0) {
            const char *next = str_next(st_known_types,
                                        tab->settings.virtual_controllers.types[slot]);
            strncpy(tab->settings.virtual_controllers.types[slot], next,
                    sizeof(tab->settings.virtual_controllers.types[slot]) - 1);
            tab->settings.virtual_controllers.types[slot]
                [sizeof(tab->settings.virtual_controllers.types[slot]) - 1] = '\0';
        }
        break;
    }
    case CBX_ST_SET_TRIGGER: {
        const char *next = str_next(cbx_st_triggers, tab->settings.overlay_trigger);
        strncpy(tab->settings.overlay_trigger, next,
                sizeof(tab->settings.overlay_trigger) - 1);
        tab->settings.overlay_trigger[sizeof(tab->settings.overlay_trigger) - 1] = '\0';
        break;
    }
    case CBX_ST_SET_ICON_OVERRIDE:
        tab->icon_preset_idx++;
        if (tab->icon_preset_idx >= ST_ICON_PRESET_COUNT)
            tab->icon_preset_idx = 0;
        /* Apply preset preview to settings struct. */
        apply_icon_preset(tab);
        break;
    default:
        break;
    }

    cbx_settings_tab_refresh(tab);
    return 0;
}

int cbx_settings_tab_edit_down(cbx_settings_tab *tab)
{
    if (!tab || tab->mode != CBX_ST_MODE_EDIT)
        return -EINVAL;

    int row = tab->selected;
    cbx_st_setting kind = st_row_kind(tab, row);

    switch (kind) {
    case CBX_ST_SET_OPACITY:
        tab->settings.overlay_opacity -= 0.05;
        if (tab->settings.overlay_opacity < 0.0)
            tab->settings.overlay_opacity = 0.0;
        break;
    case CBX_ST_SET_VC_COUNT:
        if (tab->settings.virtual_controllers.count > 1)
            tab->settings.virtual_controllers.count--;
        break;
    case CBX_ST_SET_VC_TYPE_0: {
        int slot = cbx_settings_tab_type_slot(tab, row);
        if (slot >= 0) {
            const char *prev = str_prev(st_known_types,
                                          tab->settings.virtual_controllers.types[slot]);
            strncpy(tab->settings.virtual_controllers.types[slot], prev,
                    sizeof(tab->settings.virtual_controllers.types[slot]) - 1);
            tab->settings.virtual_controllers.types[slot]
                [sizeof(tab->settings.virtual_controllers.types[slot]) - 1] = '\0';
        }
        break;
    }
    case CBX_ST_SET_TRIGGER: {
        const char *prev = str_prev(cbx_st_triggers, tab->settings.overlay_trigger);
        strncpy(tab->settings.overlay_trigger, prev,
                sizeof(tab->settings.overlay_trigger) - 1);
        tab->settings.overlay_trigger[sizeof(tab->settings.overlay_trigger) - 1] = '\0';
        break;
    }
    case CBX_ST_SET_ICON_OVERRIDE:
        tab->icon_preset_idx--;
        if (tab->icon_preset_idx < 0)
            tab->icon_preset_idx = ST_ICON_PRESET_COUNT - 1;
        apply_icon_preset(tab);
        break;
    default:
        break;
    }

    cbx_settings_tab_refresh(tab);
    return 0;
}

int cbx_settings_tab_confirm_edit(cbx_settings_tab *tab)
{
    if (!tab || tab->mode != CBX_ST_MODE_EDIT)
        return -EINVAL;

    tab->mode = CBX_ST_MODE_LIST;
    cbx_label_set_text(&tab->status_lbl, "");
    cbx_settings_tab_refresh(tab);
    return 0;
}

void cbx_settings_tab_cancel_edit(cbx_settings_tab *tab)
{
    if (!tab)
        return;

    if (tab->mode != CBX_ST_MODE_EDIT)
        return;

    /* Revert the working copy to the last authoritative state.  When bound
     * to the Manager this is the shared struct, so cancelling one tab's edit
     * never reverts a newer topology written by the other tab. */
    if (tab->external) {
        tab->settings = *tab->external;
    } else if (tab->loaded) {
        int rc = cbx_settings_load(&tab->settings);
        if (rc < 0)
            cbx_settings_defaults(&tab->settings);
    } else {
        cbx_settings_defaults(&tab->settings);
    }

    tab->mode = CBX_ST_MODE_LIST;
    cbx_label_set_text(&tab->status_lbl, "");
    cbx_settings_tab_refresh(tab);
}

/* on_select wrapper for the settings list: calls cbx_settings_tab_activate.
 * Used so both keyboard A (KEYUP) and mouse click (MOUSEUP) activate
 * the selected setting through the list's on_select callback. */
static void
on_setting_selected(cbx_widget *w, int index, void *user_data)
{
    (void)w;
    cbx_settings_tab *tab = (cbx_settings_tab *)user_data;
    if (!tab)
        return;
    /* Sync the list selection to the tab's selected index. */
    tab->selected = index;
    cbx_settings_tab_activate(tab);
}

/* ------------------------------------------------------------------ */
/*  Tab-level key handling                                             */
/* ------------------------------------------------------------------ */

bool
cbx_settings_tab_handle_key(cbx_settings_tab *tab, const SDL_Event *ev)
{
    if (!tab || !ev || ev->type != SDL_KEYDOWN)
        return false;

    SDL_Keycode key = ev->key.keysym.sym;

    if (tab->mode == CBX_ST_MODE_EDIT) {
        /* In edit mode, Up/Down adjust the value (intercept before the
         * list can consume them for navigation). */
        if (key == SDLK_UP) {
            cbx_settings_tab_edit_up(tab);
            return true;
        }
        if (key == SDLK_DOWN) {
            cbx_settings_tab_edit_down(tab);
            return true;
        }
        if (key == SDLK_b || key == SDLK_ESCAPE) {
            cbx_settings_tab_cancel_edit(tab);
            return true;
        }
    }

    return false;
}

/* ------------------------------------------------------------------ */
/*  Accessors                                                          */
/* ------------------------------------------------------------------ */

const cbx_settings *cbx_settings_tab_settings(const cbx_settings_tab *tab)
{
    return tab ? &tab->settings : NULL;
}

cbx_st_mode cbx_settings_tab_mode(const cbx_settings_tab *tab)
{
    return tab ? tab->mode : CBX_ST_MODE_LIST;
}

int cbx_settings_tab_selected(const cbx_settings_tab *tab)
{
    return tab ? tab->selected : 0;
}

const char *cbx_settings_tab_status(const cbx_settings_tab *tab)
{
    if (!tab)
        return NULL;
    return tab->status_lbl.text;
}

int cbx_settings_tab_setting_count(const cbx_settings_tab *tab)
{
    if (!tab)
        return 0;
    return CBX_ST_BASE_ROWS + cbx_settings_tab_type_row_count(tab) +
           CBX_ST_TRAILING_ROWS;
}