/*
 * profiles_tab.c — Profiles tab for the Manager UI (SPEC §5.3).
 *
 * Lists profiles from filesystem enumeration (Task 8), sorted, with
 * icons.  The default profile is read-only.  Users can create new
 * profiles and delete user-created ones.
 *
 * Task 36 — Profiles tab.
 */
#include "manager/profiles_tab.h"
#include "manager/profile_save.h"
#include "manager/profile_editor_list.h"
#include "manager/profile_editor_seq.h"
#include "ui/input_map.h"

#include <SDL2/SDL.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config/config_profile.h"
#include "config/config_profile_meta.h"
#include "config/config_paths.h"

/* ------------------------------------------------------------------ */
/*  Layout constants                                                  */
/* ------------------------------------------------------------------ */

#define CBX_PT_LIST_H    420
#define CBX_PT_BTN_W    180
#define CBX_PT_BTN_H     44
#define CBX_PT_BTN_GAP   16
#define CBX_PT_LIST_Y    16

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Build a display label for a profile entry. */
static void
format_profile_label(char *buf, size_t buflen, const cbx_profile_entry *e)
{
    if (!buf || buflen == 0 || !e)
        return;

    const char *name = e->display_name[0] ? e->display_name : e->filename;

    if (e->read_only)
        snprintf(buf, buflen, "%s  [read-only]", name);
    else
        snprintf(buf, buflen, "%s", name);
}


/* Build the sidecar path for a profile name. */
static int
build_sidecar_path(cbx_profiles_tab *tab, char *buf, size_t buflen,
                     const char *name)
{
    if (tab && tab->test_meta_dir) {
        snprintf(buf, buflen, "%s/%s.meta.yaml", tab->test_meta_dir, name);
        return 0;
    }
    char dir[PATH_MAX];
    int rc = cbx_resolve_config_dir(dir, sizeof(dir));
    if (rc != 0)
        return rc;
    snprintf(buf, buflen, "%s/profile-metadata/%s.meta.yaml", dir, name);
    return 0;
}

/* Delete a profile's sidecar file (if it exists). */
static void
delete_sidecar(cbx_profiles_tab *tab, const char *profile_name)
{
    char path[PATH_MAX + 128];
    if (build_sidecar_path(tab, path, sizeof(path), profile_name) == 0)
        unlink(path);  /* ignore errors: file may not exist */
}

/* ------------------------------------------------------------------ */
/*  Forward declarations for on_select callbacks                       */
/* ------------------------------------------------------------------ */

static void on_create_source_selected(cbx_widget *w, int index,
                                         void *user_data);
static int  cbx_profiles_tab_open_editor(cbx_profiles_tab *tab,
                                            const cbx_profile *profile,
                                            const char *name,
                                            bool is_new);
static void cbx_profiles_tab_close_editor(cbx_profiles_tab *tab);
static int  cbx_profiles_tab_save_editor(cbx_profiles_tab *tab);

static void
on_save_editor_pressed(cbx_widget *w, void *user_data)
{
    (void)w;
    cbx_profiles_tab *tab = user_data;
    if (tab && tab->mode == CBX_PT_MODE_EDITOR)
        cbx_profiles_tab_save_editor(tab);
}

static void
on_discard_editor_pressed(cbx_widget *w, void *user_data)
{
    (void)w;
    cbx_profiles_tab *tab = user_data;
    if (tab && tab->mode == CBX_PT_MODE_EDITOR)
        cbx_profiles_tab_close_editor(tab);
}

/* ------------------------------------------------------------------ */
/*  Button callbacks                                                   */
/* ------------------------------------------------------------------ */

static void
on_create_pressed(cbx_widget *w, void *user_data)
{
    (void)w;
    cbx_profiles_tab *tab = (cbx_profiles_tab *)user_data;
    if (!tab)
        return;
    /* Open the create source picker (Default copy / Empty / Clone). */
    cbx_profiles_tab_begin_create_pick(tab);
}

static void
on_delete_pressed(cbx_widget *w, void *user_data)
{
    (void)w;
    cbx_profiles_tab *tab = (cbx_profiles_tab *)user_data;
    if (!tab)
        return;
    /* Sync the list selection to the tab's selected_profile. */
    tab->selected_profile = cbx_list_get_selected(&tab->profile_list_w);
    int idx = tab->selected_profile;
    if (idx >= 0 && idx < tab->profiles.count)
        cbx_profiles_tab_begin_delete(tab, idx);
}

static void
on_edit_pressed(cbx_widget *w, void *user_data)
{
    (void)w;
    cbx_profiles_tab *tab = (cbx_profiles_tab *)user_data;
    if (!tab || !tab->panel)
        return;

    /* Sync the list selection to the tab's selected_profile. */
    tab->selected_profile = cbx_list_get_selected(&tab->profile_list_w);
    int idx = tab->selected_profile;
    if (idx < 0 || idx >= tab->profiles.count)
        return;

    const cbx_profile_entry *e = &tab->profiles.entries[idx];

    /* Load the profile from disk. */
    cbx_profile prof;
    cbx_profile_init(&prof);
    int rc = cbx_profile_load(&prof, e->path);
    if (rc != 0)
        return;

    /* Open the editor with the loaded profile. */
    cbx_profiles_tab_open_editor(tab, &prof, e->filename, false);
}

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                         */
/* ------------------------------------------------------------------ */

int
cbx_profiles_tab_init(cbx_profiles_tab *tab,
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
    tab->mode       = CBX_PT_MODE_LIST;
    tab->selected_profile = -1;
    tab->delete_target     = -1;

    /* --- Profile list --------------------------------------------- */
    int rc = cbx_list_init(&tab->profile_list_w, font_id, cache, theme);
    if (rc != 0)
        return rc;

    /* --- Create picker (hidden initially) ------------------------- */
    rc = cbx_list_init(&tab->create_picker, font_id, cache, theme);
    if (rc != 0) {
        cbx_widget_destroy(&tab->profile_list_w.base);
        return rc;
    }
    cbx_widget_set_visible(&tab->create_picker.base, false);
    cbx_list_set_select_cb(&tab->create_picker, on_create_source_selected);

    /* --- Buttons --------------------------------------------------- */
    rc = cbx_button_init(&tab->create_btn, "Create Profile", font_id,
                          cache, theme, on_create_pressed, tab);
    if (rc != 0) {
        cbx_widget_destroy(&tab->profile_list_w.base);
        cbx_widget_destroy(&tab->create_picker.base);
        return rc;
    }

    rc = cbx_button_init(&tab->edit_btn, "Edit Profile", font_id,
                          cache, theme, on_edit_pressed, tab);
    if (rc != 0) {
        cbx_widget_destroy(&tab->profile_list_w.base);
        cbx_widget_destroy(&tab->create_picker.base);
        cbx_widget_destroy(&tab->create_btn.base);
        return rc;
    }

    rc = cbx_button_init(&tab->delete_btn, "Delete Profile", font_id,
                          cache, theme, on_delete_pressed, tab);
    if (rc != 0) {
        cbx_widget_destroy(&tab->profile_list_w.base);
        cbx_widget_destroy(&tab->create_picker.base);
        cbx_widget_destroy(&tab->create_btn.base);
        cbx_widget_destroy(&tab->edit_btn.base);
        return rc;
    }

    /* --- Status label --------------------------------------------- */
    rc = cbx_label_init(&tab->status_lbl, "", font_id, cache, theme);
    if (rc != 0) {
        cbx_widget_destroy(&tab->profile_list_w.base);
        cbx_widget_destroy(&tab->create_picker.base);
        cbx_widget_destroy(&tab->create_btn.base);
        cbx_widget_destroy(&tab->edit_btn.base);
        cbx_widget_destroy(&tab->delete_btn.base);
        return rc;
    }
    cbx_widget_set_visible(&tab->status_lbl.base, false);

    rc = cbx_button_init(&tab->save_btn, "Save", font_id, cache, theme,
                          on_save_editor_pressed, tab);
    if (rc != 0)
        goto editor_button_fail;
    rc = cbx_button_init(&tab->discard_btn, "Discard", font_id, cache, theme,
                          on_discard_editor_pressed, tab);
    if (rc != 0) {
        cbx_widget_destroy(&tab->save_btn.base);
        goto editor_button_fail;
    }
    cbx_widget_set_visible(&tab->save_btn.base, false);
    cbx_widget_set_visible(&tab->discard_btn.base, false);

    /* --- Add widgets to panel ------------------------------------- */
    cbx_panel_add_child(panel, &tab->profile_list_w.base);
    cbx_panel_add_child(panel, &tab->create_btn.base);
    cbx_panel_add_child(panel, &tab->edit_btn.base);
    cbx_panel_add_child(panel, &tab->delete_btn.base);
    cbx_panel_add_child(panel, &tab->status_lbl.base);
    cbx_panel_add_child(panel, &tab->create_picker.base);
    cbx_panel_add_child(panel, &tab->save_btn.base);
    cbx_panel_add_child(panel, &tab->discard_btn.base);

/* --- Layout --------------------------------------------------- */    cbx_profiles_tab_layout(tab);
    /* NOTE: caller must call cbx_profiles_tab_refresh() after init.
     * For testing, call cbx_profiles_tab_set_test_dirs() first. */

    return 0;

editor_button_fail:
    cbx_widget_destroy(&tab->profile_list_w.base);
    cbx_widget_destroy(&tab->create_picker.base);
    cbx_widget_destroy(&tab->create_btn.base);
    cbx_widget_destroy(&tab->edit_btn.base);
    cbx_widget_destroy(&tab->delete_btn.base);
    cbx_widget_destroy(&tab->status_lbl.base);
    return rc;
}

void
cbx_profiles_tab_shutdown(cbx_profiles_tab *tab)
{
    if (!tab)
        return;

    /* Shut down the editor if it was initialised. */
    if (tab->editor_initialized) {
        cbx_profile_editor_shutdown(&tab->editor);
        tab->editor_initialized = false;
    }

    if (tab->panel) {
        cbx_panel_remove_child(tab->panel, &tab->profile_list_w.base);
        cbx_panel_remove_child(tab->panel, &tab->create_btn.base);
        cbx_panel_remove_child(tab->panel, &tab->edit_btn.base);
        cbx_panel_remove_child(tab->panel, &tab->delete_btn.base);
        cbx_panel_remove_child(tab->panel, &tab->status_lbl.base);
        cbx_panel_remove_child(tab->panel, &tab->create_picker.base);
        cbx_panel_remove_child(tab->panel, &tab->save_btn.base);
        cbx_panel_remove_child(tab->panel, &tab->discard_btn.base);
    }

    cbx_widget_destroy(&tab->profile_list_w.base);
    cbx_widget_destroy(&tab->create_picker.base);
    cbx_widget_destroy(&tab->create_btn.base);
    cbx_widget_destroy(&tab->edit_btn.base);
    cbx_widget_destroy(&tab->delete_btn.base);
    cbx_widget_destroy(&tab->status_lbl.base);
    cbx_widget_destroy(&tab->save_btn.base);
    cbx_widget_destroy(&tab->discard_btn.base);

    memset(tab, 0, sizeof(*tab));
}

/* ------------------------------------------------------------------ */
/*  Refresh                                                            */
/* ------------------------------------------------------------------ */

void
cbx_profiles_tab_layout(cbx_profiles_tab *tab)
{
    if (!tab || !tab->panel)
        return;

    SDL_Rect pr;
    cbx_widget_get_rect(&tab->panel->base, &pr);

    /* Profile list: fills most of the panel. */
    SDL_Rect list_rect = {
        .x = pr.x + CBX_PT_LIST_Y,
        .y = pr.y + CBX_PT_LIST_Y,
        .w = pr.w - CBX_PT_LIST_Y * 2,
        .h = CBX_PT_LIST_H,
    };
    cbx_widget_set_rect(&tab->profile_list_w.base, &list_rect);
    cbx_widget_set_rect(&tab->create_picker.base, &list_rect);

    /* Buttons: below the list, left to right. */
    int btn_y = pr.y + CBX_PT_LIST_Y + CBX_PT_LIST_H + CBX_PT_BTN_GAP;
    int btn_x = pr.x + CBX_PT_LIST_Y;

    SDL_Rect c_rect = { .x = btn_x, .y = btn_y,
                         .w = CBX_PT_BTN_W, .h = CBX_PT_BTN_H };
    cbx_widget_set_rect(&tab->create_btn.base, &c_rect);

    btn_x += CBX_PT_BTN_W + CBX_PT_BTN_GAP;
    SDL_Rect e_rect = { .x = btn_x, .y = btn_y,
                         .w = CBX_PT_BTN_W, .h = CBX_PT_BTN_H };
    cbx_widget_set_rect(&tab->edit_btn.base, &e_rect);

    btn_x += CBX_PT_BTN_W + CBX_PT_BTN_GAP;
    SDL_Rect d_rect = { .x = btn_x, .y = btn_y,
                         .w = CBX_PT_BTN_W, .h = CBX_PT_BTN_H };
    cbx_widget_set_rect(&tab->delete_btn.base, &d_rect);

    /* Status label: below the buttons. */
    SDL_Rect s_rect = {
        .x = pr.x + CBX_PT_LIST_Y,
        .y = btn_y + CBX_PT_BTN_H + CBX_PT_BTN_GAP,
        .w = pr.w - CBX_PT_LIST_Y * 2,
        .h = CBX_PT_BTN_H,
    };
    cbx_widget_set_rect(&tab->status_lbl.base, &s_rect);

    SDL_Rect save_rect = { .x = pr.x + pr.w - (2 * CBX_PT_BTN_W) -
                                  (2 * CBX_PT_BTN_GAP),
                           .y = btn_y, .w = CBX_PT_BTN_W,
                           .h = CBX_PT_BTN_H };
    SDL_Rect discard_rect = { .x = save_rect.x + CBX_PT_BTN_W + CBX_PT_BTN_GAP,
                              .y = btn_y, .w = CBX_PT_BTN_W,
                              .h = CBX_PT_BTN_H };
    cbx_widget_set_rect(&tab->save_btn.base, &save_rect);
    cbx_widget_set_rect(&tab->discard_btn.base, &discard_rect);

}

int
cbx_profiles_tab_refresh(cbx_profiles_tab *tab)
{
    if (!tab)
        return -EINVAL;

    int rc;
    if (tab->test_user_dir || tab->test_system_dir || tab->test_meta_dir)
        rc = cbx_profile_list_enumerate_dirs(&tab->profiles,
                                              tab->test_user_dir,
                                              tab->test_system_dir,
                                              tab->test_meta_dir);
    else
        rc = cbx_profile_list_enumerate(&tab->profiles);
    if (rc != 0)
        return rc;

    /* Rebuild the list widget. */
    cbx_list_clear(&tab->profile_list_w);
    for (int i = 0; i < tab->profiles.count; i++) {
        char label[CBX_PT_LABEL_LEN];
        format_profile_label(label, sizeof(label),
                              &tab->profiles.entries[i]);
        cbx_list_add_item(&tab->profile_list_w, label, NULL,
                           (void *)(intptr_t)(long)i);
    }

    /* Clamp selection. */
    if (tab->selected_profile >= tab->profiles.count)
        tab->selected_profile = tab->profiles.count - 1;
    if (tab->selected_profile < 0 && tab->profiles.count > 0)
        tab->selected_profile = 0;
    cbx_list_set_selected(&tab->profile_list_w, tab->selected_profile);

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Create                                                             */
/* ------------------------------------------------------------------ */

int
cbx_profiles_tab_create(cbx_profiles_tab *tab,
                          const char *name,
                          cbx_pt_create_source source)
{
    if (!tab || !name)
        return -EINVAL;

    /* Validate the name. */
    if (!cbx_validate_filename(name))
        return -EINVAL;

    /* Check if a profile with this name already exists. */
    for (int i = 0; i < tab->profiles.count; i++) {
        if (strcmp(tab->profiles.entries[i].filename, name) == 0)
            return -EEXIST;
    }

    /* Build the profile content based on the source. */
    cbx_profile prof;
    cbx_profile_init(&prof);
    snprintf(prof.name, sizeof(prof.name), "%s", name);
    int rc = 0;

    if (source == CBX_PT_CREATE_DEFAULT_COPY || source == CBX_PT_CREATE_CLONE) {
        /* Find the source profile to clone from. */
        const cbx_profile_entry *src = NULL;

        if (source == CBX_PT_CREATE_DEFAULT_COPY) {
            /* Find the default profile. */
            for (int i = 0; i < tab->profiles.count; i++) {
                if (tab->profiles.entries[i].is_default) {
                    src = &tab->profiles.entries[i];
                    break;
                }
            }
            if (!src)
                return -ENOENT;
        } else {
            /* Clone the selected profile. */
            int idx = tab->selected_profile;
            if (idx < 0 || idx >= tab->profiles.count)
                return -EINVAL;
            src = &tab->profiles.entries[idx];
            /* Can't clone a read-only profile (shouldn't block, but
             * the result would be read-only content — allow it, it
             * becomes a new editable profile). */
        }

        /* Load the source profile to copy its mappings. */
        cbx_profile src_prof;
        cbx_profile_init(&src_prof);
        rc = cbx_profile_load(&src_prof, src->path);
        if (rc != 0)
            return rc;

        /* Copy mappings. */
        prof.mapping_count = src_prof.mapping_count;
        for (int i = 0; i < src_prof.mapping_count; i++)
            prof.mappings[i] = src_prof.mappings[i];
    }
    /* CBX_PT_CREATE_EMPTY: leave mappings empty. */

    /* Save the profile via the manager-level save path, which enforces
     * filename validation, NES minimum binding validation, and path
     * canonicalization (TOCTOU-safe) before writing. */
    char missing_buf[CBX_PT_LABEL_LEN];
    rc = cbx_profile_save_to_dir(&prof, name, NULL,
                                  tab->test_user_dir,
                                  missing_buf, sizeof(missing_buf));
    if (rc != 0) {
        if (rc == -EINVAL && missing_buf[0] != '\0') {
            /* NES minimum validation failed — show the missing buttons. */
            char msg[CBX_PT_LABEL_LEN + 16];
            snprintf(msg, sizeof(msg), "Missing: %s", missing_buf);
            cbx_label_set_text(&tab->status_lbl, msg);
            cbx_widget_set_visible(&tab->status_lbl.base, true);
        }
        return rc;
    }

    /* Refresh to show the new profile. */
    cbx_profiles_tab_refresh(tab);

    /* Select the newly created profile. */
    for (int i = 0; i < tab->profiles.count; i++) {
        if (strcmp(tab->profiles.entries[i].filename, name) == 0) {
            tab->selected_profile = i;
            cbx_list_set_selected(&tab->profile_list_w, i);
            break;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Delete                                                             */
/* ------------------------------------------------------------------ */

int
cbx_profiles_tab_delete(cbx_profiles_tab *tab, int profile_index)
{
    if (!tab)
        return -EINVAL;
    if (profile_index < 0 || profile_index >= tab->profiles.count)
        return -EINVAL;

    const cbx_profile_entry *e = &tab->profiles.entries[profile_index];

    /* Validate: not read-only (not default, not system). */
    if (e->read_only)
        return -EINVAL;

    /* Delete the profile YAML. */
    if (unlink(e->path) != 0)
        return -errno;

    /* Delete the sidecar if it exists. */
    delete_sidecar(tab, e->filename);

    /* Refresh to update the list. */
    cbx_profiles_tab_refresh(tab);

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Name input mode                                                    */
/* ------------------------------------------------------------------ */

int
cbx_profiles_tab_begin_create(cbx_profiles_tab *tab,
                                cbx_pt_create_source source)
{
    if (!tab)
        return -EINVAL;

    tab->mode = CBX_PT_MODE_NAME_INPUT;
    tab->create_source = source;
    tab->name_len = 0;
    tab->name_buf[0] = '\0';

    /* Show status label with prompt. */
    const char *prompt;
    switch (source) {
    case CBX_PT_CREATE_DEFAULT_COPY:
        prompt = "Enter name (copy default):";
        break;
    case CBX_PT_CREATE_EMPTY:
        prompt = "Enter name (empty profile):";
        break;
    case CBX_PT_CREATE_CLONE:
        prompt = "Enter name (clone current):";
        break;
    default:
        prompt = "Enter name:";
        break;
    }
    cbx_label_set_text(&tab->status_lbl, prompt);
    cbx_widget_set_visible(&tab->status_lbl.base, true);

    return 0;
}

int
cbx_profiles_tab_name_input_char(cbx_profiles_tab *tab, char ch)
{
    if (!tab || tab->mode != CBX_PT_MODE_NAME_INPUT)
        return -EINVAL;

    /* Only allow valid filename characters. */
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
        return -EINVAL;

    if (tab->name_len >= CBX_PT_NAME_LEN - 1)
        return -ENOSPC;

    tab->name_buf[tab->name_len++] = ch;
    tab->name_buf[tab->name_len] = '\0';

    /* Update status label to show current input. */
    char prompt[CBX_PT_LABEL_LEN];
    snprintf(prompt, sizeof(prompt), "Name: %s_", tab->name_buf);
    cbx_label_set_text(&tab->status_lbl, prompt);

    return 0;
}

int
cbx_profiles_tab_name_input_backspace(cbx_profiles_tab *tab)
{
    if (!tab || tab->mode != CBX_PT_MODE_NAME_INPUT)
        return -EINVAL;

    if (tab->name_len > 0) {
        tab->name_len--;
        tab->name_buf[tab->name_len] = '\0';

        char prompt[CBX_PT_LABEL_LEN];
        snprintf(prompt, sizeof(prompt), "Name: %s_", tab->name_buf);
        cbx_label_set_text(&tab->status_lbl, prompt);
    }

    return 0;
}

int
cbx_profiles_tab_name_input_confirm(cbx_profiles_tab *tab)
{
    if (!tab || tab->mode != CBX_PT_MODE_NAME_INPUT)
        return -EINVAL;

    if (tab->name_len == 0)
        return -EINVAL;

    cbx_pt_create_source src = tab->create_source;
    char name[CBX_PT_NAME_LEN];
    snprintf(name, sizeof(name), "%s", tab->name_buf);

    /* Cancel name input (resets mode to LIST, clears buffer). */
    cbx_profiles_tab_name_input_cancel(tab);

    /* Validate the name. */
    if (!cbx_validate_filename(name))
        return -EINVAL;

    /* Check if a profile with this name already exists. */
    for (int i = 0; i < tab->profiles.count; i++) {
        if (strcmp(tab->profiles.entries[i].filename, name) == 0)
            return -EEXIST;
    }

    /* Build the in-memory profile based on the source. */
    cbx_profile prof;
    cbx_profile_init(&prof);
    snprintf(prof.name, sizeof(prof.name), "%s", name);

    if (src == CBX_PT_CREATE_DEFAULT_COPY || src == CBX_PT_CREATE_CLONE) {
        const cbx_profile_entry *src_entry = NULL;

        if (src == CBX_PT_CREATE_DEFAULT_COPY) {
            for (int i = 0; i <tab->profiles.count; i++) {
                if (tab->profiles.entries[i].is_default) {
                    src_entry = &tab->profiles.entries[i];
                    break;
                }
            }
            if (!src_entry)
                return -ENOENT;
        } else {
            int idx = tab->selected_profile;
            if (idx < 0 || idx >= tab->profiles.count)
                return -EINVAL;
            src_entry = &tab->profiles.entries[idx];
        }

        cbx_profile src_prof;
        cbx_profile_init(&src_prof);
        int rc = cbx_profile_load(&src_prof, src_entry->path);
        if (rc != 0)
            return rc;

        prof.mapping_count = src_prof.mapping_count;
        for (int i = 0; i < src_prof.mapping_count; i++)
            prof.mappings[i] = src_prof.mappings[i];
    }
    /* CBX_PT_CREATE_EMPTY: leave mappings empty. */

    /* Open the editor with the in-memory profile.  No file is
     * written until the user saves from the editor (which
     * validates NES minimum bindings via cbx_profile_save_to_dir). */
    return cbx_profiles_tab_open_editor(tab, &prof, name, true);
}

void
cbx_profiles_tab_name_input_cancel(cbx_profiles_tab *tab)
{
    if (!tab)
        return;

    tab->mode = CBX_PT_MODE_LIST;
    tab->name_len = 0;
    tab->name_buf[0] = '\0';
    cbx_widget_set_visible(&tab->status_lbl.base, false);
    cbx_label_set_text(&tab->status_lbl, "");
}

/* ------------------------------------------------------------------ */
/*  Delete confirmation mode                                           */
/* ------------------------------------------------------------------ */

int
cbx_profiles_tab_begin_delete(cbx_profiles_tab *tab, int profile_index)
{
    if (!tab)
        return -EINVAL;
    if (profile_index < 0 || profile_index >= tab->profiles.count)
        return -EINVAL;

    const cbx_profile_entry *e = &tab->profiles.entries[profile_index];
    if (e->read_only)
        return -EINVAL;

    tab->mode = CBX_PT_MODE_CONFIRM_DELETE;
    tab->delete_target = profile_index;

    char prompt[CBX_PT_LABEL_LEN];
    snprintf(prompt, sizeof(prompt), "Delete \"%s\"?  A=Yes  B=No",
             e->display_name[0] ? e->display_name : e->filename);
    cbx_label_set_text(&tab->status_lbl, prompt);
    cbx_widget_set_visible(&tab->status_lbl.base, true);

    return 0;
}

int
cbx_profiles_tab_confirm_delete(cbx_profiles_tab *tab)
{
    if (!tab || tab->mode != CBX_PT_MODE_CONFIRM_DELETE)
        return -EINVAL;

    int target = tab->delete_target;

    /* Return to list mode first. */
    cbx_profiles_tab_cancel_delete(tab);

    /* Execute the delete. */
    return cbx_profiles_tab_delete(tab, target);
}

void
cbx_profiles_tab_cancel_delete(cbx_profiles_tab *tab)
{
    if (!tab)
        return;

    tab->mode = CBX_PT_MODE_LIST;
    tab->delete_target = -1;
    cbx_widget_set_visible(&tab->status_lbl.base, false);
    cbx_label_set_text(&tab->status_lbl, "");
}

/*
 * Enter confirm-quit mode: shows a prompt asking the user whether to
 * save unsaved editor changes before quitting or discard them.
 * The manager calls this when SDL_QUIT arrives while the editor has
 * unsaved edits (dirty flag set).
 */
void
cbx_profiles_tab_begin_confirm_quit(cbx_profiles_tab *tab)
{
    if (!tab)
        return;

    tab->mode = CBX_PT_MODE_CONFIRM_QUIT;
    tab->quit_after_action = false;

    cbx_label_set_text(&tab->status_lbl,
                         "Unsaved changes.  A=Save & Quit  B=Discard & Quit");
    cbx_widget_set_visible(&tab->status_lbl.base, true);
}

/* ------------------------------------------------------------------ */
/*  Create source picker mode                                         */
/* ------------------------------------------------------------------ */

int
cbx_profiles_tab_begin_create_pick(cbx_profiles_tab *tab)
{
    if (!tab)
        return -EINVAL;

    /* Populate the create picker with the three source options. */
    cbx_list_clear(&tab->create_picker);
    cbx_list_add_item(&tab->create_picker, "Default copy", NULL, tab);
    cbx_list_add_item(&tab->create_picker, "Empty", NULL, tab);
    cbx_list_add_item(&tab->create_picker, "Clone current", NULL, tab);
    cbx_list_set_selected(&tab->create_picker, 0);

    /* Show the picker, hide the profile list and buttons. */
    cbx_widget_set_visible(&tab->profile_list_w.base, false);
    cbx_widget_set_visible(&tab->create_btn.base, false);
    cbx_widget_set_visible(&tab->edit_btn.base, false);
    cbx_widget_set_visible(&tab->delete_btn.base, false);
    cbx_widget_set_visible(&tab->create_picker.base, true);

    tab->mode = CBX_PT_MODE_CREATE_PICK;

    cbx_label_set_text(&tab->status_lbl,
                       "Create from: Up/Down to select, A=confirm, B=cancel");
    cbx_widget_set_visible(&tab->status_lbl.base, true);

    return 0;
}

void
cbx_profiles_tab_cancel_create_pick(cbx_profiles_tab *tab)
{
    if (!tab)
        return;

    /* Restore list view. */
    cbx_widget_set_visible(&tab->profile_list_w.base, true);
    cbx_widget_set_visible(&tab->create_btn.base, true);
    cbx_widget_set_visible(&tab->edit_btn.base, true);
    cbx_widget_set_visible(&tab->delete_btn.base, true);
    cbx_widget_set_visible(&tab->create_picker.base, false);

    tab->mode = CBX_PT_MODE_LIST;
    cbx_widget_set_visible(&tab->status_lbl.base, false);
    cbx_label_set_text(&tab->status_lbl, "");
}

/* on_select callback for the create source picker.
 * Called when the user presses A (KEYUP) or clicks (MOUSEUP) on a source.
 * Maps the index to a create_source and enters name input mode. */
static void
on_create_source_selected(cbx_widget *w, int index, void *user_data)
{
    (void)w;
    cbx_profiles_tab *tab = (cbx_profiles_tab *)user_data;
    if (!tab || tab->mode != CBX_PT_MODE_CREATE_PICK)
        return;
    if (index < 0 || index > 2)
        return;

    cbx_pt_create_source source = (cbx_pt_create_source)index;

    /* Restore list view before entering name input. */
    cbx_widget_set_visible(&tab->profile_list_w.base, true);
    cbx_widget_set_visible(&tab->create_btn.base, true);
    cbx_widget_set_visible(&tab->edit_btn.base, true);
    cbx_widget_set_visible(&tab->delete_btn.base, true);
    cbx_widget_set_visible(&tab->create_picker.base, false);

    cbx_profiles_tab_begin_create(tab, source);
}

/* ------------------------------------------------------------------ */
/*  Tab-level activation / cancel / key handling                       */
/* ------------------------------------------------------------------ */

int
cbx_profiles_tab_activate(cbx_profiles_tab *tab)
{
    if (!tab)
        return -EINVAL;

    switch (tab->mode) {
    case CBX_PT_MODE_NAME_INPUT:
        return cbx_profiles_tab_name_input_confirm(tab);
    case CBX_PT_MODE_CONFIRM_DELETE:
        return cbx_profiles_tab_confirm_delete(tab);
    case CBX_PT_MODE_CREATE_PICK:
        /* Sync from picker selection and enter name input. */
        {
            int idx = cbx_list_get_selected(&tab->create_picker);
            if (idx < 0 || idx > 2)
                return -EINVAL;
            cbx_pt_create_source source = (cbx_pt_create_source)idx;
            /* Restore list view. */
            cbx_widget_set_visible(&tab->profile_list_w.base, true);
            cbx_widget_set_visible(&tab->create_btn.base, true);
            cbx_widget_set_visible(&tab->edit_btn.base, true);
            cbx_widget_set_visible(&tab->delete_btn.base, true);
            cbx_widget_set_visible(&tab->create_picker.base, false);
            return cbx_profiles_tab_begin_create(tab, source);
        }
    case CBX_PT_MODE_EDITOR:
        return cbx_profile_editor_activate(&tab->editor);
    case CBX_PT_MODE_LIST:
    default:
        /* No tab-level activation in list mode. */
        return 0;
    }
}

bool
cbx_profiles_tab_cancel(cbx_profiles_tab *tab)
{
    if (!tab)
        return false;

    switch (tab->mode) {
    case CBX_PT_MODE_NAME_INPUT:
        cbx_profiles_tab_name_input_cancel(tab);
        return true;
    case CBX_PT_MODE_CONFIRM_DELETE:
        cbx_profiles_tab_cancel_delete(tab);
        return true;
    case CBX_PT_MODE_CREATE_PICK:
        cbx_profiles_tab_cancel_create_pick(tab);
        return true;
    case CBX_PT_MODE_EDITOR:
        {
            cbx_editor_mode em = cbx_profile_editor_get_mode(&tab->editor);
            if (em == CBX_EDITOR_MODE_LIST) {
                /* B in LIST mode: save and close. */
                cbx_profiles_tab_save_editor(tab);
                return true;
            } else if (em == CBX_EDITOR_MODE_SEQUENTIAL) {
                /* B in SEQUENTIAL: skip current binding. */
                cbx_profile_editor_seq_skip(&tab->editor);
                return true;
            } else {
                /* B in TARGET_PICK / CAPTURE / BINDING_EDIT:
                 * cancel the sub-mode, return to editor LIST. */
                cbx_profile_editor_cancel(&tab->editor);
                return true;
            }
        }
    default:
        return false;
    }
}

bool
cbx_profiles_tab_handle_key(cbx_profiles_tab *tab, const SDL_Event *ev)
{
    if (!tab || !ev || ev->type != SDL_KEYDOWN)
        return false;

    SDL_Keycode key = ev->key.keysym.sym;

    switch (tab->mode) {
    case CBX_PT_MODE_NAME_INPUT:
        {
        bool controller_event =
            ev->key.windowID == CBX_CONTROLLER_EVENT_WINDOW_ID;
        if ((controller_event && key == SDLK_b) || key == SDLK_ESCAPE) {
            cbx_profiles_tab_name_input_cancel(tab);
            return true;
        }
        if (key == SDLK_BACKSPACE) {
            cbx_profiles_tab_name_input_backspace(tab);
            return true;
        }
        if ((controller_event && key == SDLK_a) || key == SDLK_RETURN) {
            cbx_profiles_tab_name_input_confirm(tab);
            return true;
        }
        /* Letter keys: add character to name buffer. */
        if (key >= SDLK_a && key <= SDLK_z) {
            cbx_profiles_tab_name_input_char(tab, (char)key);
            return true;
        }
        if (key >= SDLK_0 && key <= SDLK_9) {
            cbx_profiles_tab_name_input_char(tab, (char)key);
            return true;
        }
        if (key == SDLK_MINUS) {
            cbx_profiles_tab_name_input_char(tab, '-');
            return true;
        }
        if (key == SDLK_UNDERSCORE) {
            cbx_profiles_tab_name_input_char(tab, '_');
            return true;
        }
        return false;
        }

    case CBX_PT_MODE_CONFIRM_DELETE:
        if (key == SDLK_a || key == SDLK_RETURN) {
            cbx_profiles_tab_confirm_delete(tab);
            return true;
        }
        if (key == SDLK_b || key == SDLK_ESCAPE) {
            cbx_profiles_tab_cancel_delete(tab);
            return true;
        }
        return false;

    case CBX_PT_MODE_CREATE_PICK:
        if (key == SDLK_b || key == SDLK_ESCAPE) {
            cbx_profiles_tab_cancel_create_pick(tab);
            return true;
        }
        return false;

    case CBX_PT_MODE_CONFIRM_QUIT:
        /* A = save & quit; B = discard & quit */
        if (key == SDLK_a || key == SDLK_RETURN) {
            cbx_profiles_tab_save_editor(tab);
            tab->quit_after_action = true;
            return true;
        }
        if (key == SDLK_b || key == SDLK_ESCAPE) {
            cbx_profiles_tab_close_editor(tab);
            tab->quit_after_action = true;
            return true;
        }
        return false;

    case CBX_PT_MODE_EDITOR:
        /* Start (TAB) key: cancel editor (discard) or cancel sequential. */
        if (key == SDLK_TAB) {
            cbx_editor_mode em = cbx_profile_editor_get_mode(&tab->editor);
            if (em == CBX_EDITOR_MODE_SEQUENTIAL)
                cbx_profile_editor_cancel_sequential(&tab->editor);
            else if (em == CBX_EDITOR_MODE_LIST)
                cbx_profiles_tab_close_editor(tab);
            return true;
        }
        /* Up/Down: editor navigation (intercept before the list widget
         * so the editor can sync the diagram highlight). */
        if (key == SDLK_UP) {
            cbx_profile_editor_move_up(&tab->editor);
            return true;
        }
        if (key == SDLK_DOWN) {
            cbx_profile_editor_move_down(&tab->editor);
            return true;
        }
        /* A/B/Return/Escape KEYDOWN: swallow so the focused widget
         * gets the KEYUP for activation/cancel via the manager. */
        if (key == SDLK_a || key == SDLK_RETURN || key == SDLK_SPACE)
            return true;
        if (key == SDLK_b || key == SDLK_ESCAPE)
            return true;
        return false;

    default:
        return false;
    }
}

void
cbx_profiles_tab_set_test_dirs(cbx_profiles_tab *tab,
                                 const char *user_dir,
                                 const char *system_dir,
                                 const char *meta_dir)
{
    if (!tab)
        return;
    tab->test_user_dir   = user_dir;
    tab->test_system_dir = system_dir;
    tab->test_meta_dir   = meta_dir;
}

void
cbx_profiles_tab_set_context(cbx_profiles_tab *tab,
                                SDL_Renderer *renderer,
                                const ip_dbus_backend *backend,
                                ip_bus_handle bus)
{
    if (!tab)
        return;
    tab->renderer     = renderer;
    tab->dbus_backend = backend;
    tab->dbus_bus     = bus;
}

/* ------------------------------------------------------------------ */
/*  Profile editor open / close / save (Task 5)                       */
/* ------------------------------------------------------------------ */

static void
hide_tab_widgets(cbx_profiles_tab *tab)
{
    cbx_widget_set_visible(&tab->profile_list_w.base, false);
    cbx_widget_set_visible(&tab->create_btn.base, false);
    cbx_widget_set_visible(&tab->edit_btn.base, false);
    cbx_widget_set_visible(&tab->delete_btn.base, false);
    cbx_widget_set_visible(&tab->status_lbl.base, false);
    cbx_widget_set_visible(&tab->create_picker.base, false);
}

static void
show_tab_widgets(cbx_profiles_tab *tab)
{
    cbx_widget_set_visible(&tab->profile_list_w.base, true);
    cbx_widget_set_visible(&tab->create_btn.base, true);
    cbx_widget_set_visible(&tab->edit_btn.base, true);
    cbx_widget_set_visible(&tab->delete_btn.base, true);
    /* status_lbl and create_picker stay hidden in list mode */
}

/* Open the profile editor with the given profile.  If is_new is true,
 * no file exists yet — the profile is in-memory and will be saved
 * on editor close.  If is_new is false, the profile is an existing
 * file being edited. */
static int
cbx_profiles_tab_open_editor(cbx_profiles_tab *tab,
                              const cbx_profile *profile,
                              const char *name,
                              bool is_new)
{
    if (!tab || !tab->panel || !profile || !name)
        return -EINVAL;

    /* Lazy-initialise the editor on first use. */
    if (!tab->editor_initialized) {
        if (!tab->renderer)
            return -EINVAL;
        int rc = cbx_profile_editor_init(&tab->editor,
                                            tab->panel,
                                            tab->renderer,
                                            tab->text_cache,
                                            tab->theme,
                                            tab->font_id);
        if (rc != 0)
            return rc;
        tab->editor_initialized = true;
    }

    /* Load the profile into the editor. */
    int rc = cbx_profile_editor_load_profile(&tab->editor, profile);
    if (rc != 0)
        return rc;

    /* Set DBus info (for capture mode and capabilities). */
    if (tab->dbus_backend && tab->dbus_bus)
        cbx_profile_editor_set_dbus(&tab->editor,
                                      tab->dbus_backend,
                                      tab->dbus_bus, NULL);

    /* Refresh to populate the binding list. */
    cbx_profile_editor_refresh(&tab->editor);

    /* Store the profile name for saving. */
    snprintf(tab->editor_profile_name, sizeof(tab->editor_profile_name),
              "%s", name);
    tab->editor_is_new = is_new;

    /* Hide tab widgets, editor widgets are shown by editor init/refresh. */
    hide_tab_widgets(tab);
    cbx_widget_set_visible(&tab->save_btn.base, true);
    cbx_widget_set_visible(&tab->discard_btn.base, true);

    tab->mode = CBX_PT_MODE_EDITOR;

    return 0;
}

/* Close the editor and return to the profiles list. */
static void
cbx_profiles_tab_close_editor(cbx_profiles_tab *tab)
{
    if (!tab)
        return;

    /* Hide editor widgets. */
    if (tab->editor_initialized) {
        cbx_widget_set_visible(&tab->editor.title_lbl.base, false);
        cbx_widget_set_visible(&tab->editor.diagram.base, false);
        cbx_widget_set_visible(&tab->editor.binding_list.base, false);
        cbx_widget_set_visible(&tab->editor.target_list.base, false);
        cbx_widget_set_visible(&tab->editor.status_lbl.base, false);
        cbx_widget_set_visible(&tab->editor.progress_bar.base, false);
    }

    cbx_widget_set_visible(&tab->save_btn.base, false);
    cbx_widget_set_visible(&tab->discard_btn.base, false);

    /* Show tab widgets. */
    show_tab_widgets(tab);

    tab->mode = CBX_PT_MODE_LIST;
}

/* Save the editor's profile to disk via the security-hardened save
 * path (cbx_profile_save_to_dir).  Returns 0 on success, negative
 * errno on failure (e.g. NES validation error).  On failure, the
 * editor stays open with an error message. */
static int
cbx_profiles_tab_save_editor(cbx_profiles_tab *tab)
{
    if (!tab || !tab->editor_initialized)
        return -EINVAL;

    const cbx_profile *prof = cbx_profile_editor_get_profile(&tab->editor);
    if (!prof)
        return -EINVAL;

    char missing_buf[CBX_PT_LABEL_LEN];
    int rc = cbx_profile_save_to_dir(prof,
                                       tab->editor_profile_name,
                                       NULL,
                                       tab->test_user_dir,
                                       missing_buf, sizeof(missing_buf));
    if (rc != 0) {
        if (rc == -EINVAL && missing_buf[0] != '\0') {
            /* NES minimum validation failed — show missing buttons. */
            char msg[CBX_PT_LABEL_LEN + 16];
            snprintf(msg, sizeof(msg), "Missing: %s", missing_buf);
            cbx_label_set_text(&tab->editor.status_lbl, msg);
        } else {
            cbx_label_set_text(&tab->editor.status_lbl,
                                 "Save failed.");
        }
        return rc;
    }

    /* Save succeeded — close editor and refresh list. */
    cbx_profiles_tab_close_editor(tab);
    cbx_profiles_tab_refresh(tab);

    /* Select the saved profile. */
    for (int i = 0; i < tab->profiles.count; i++) {
        if (strcmp(tab->profiles.entries[i].filename,
                    tab->editor_profile_name) == 0) {
            tab->selected_profile = i;
            cbx_list_set_selected(&tab->profile_list_w, i);
            break;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Accessors                                                          */
/* ------------------------------------------------------------------ */

const cbx_profile_entry *
cbx_profiles_tab_entry(const cbx_profiles_tab *tab, int index)
{
    if (!tab || index < 0 || index >= tab->profiles.count)
        return NULL;
    return &tab->profiles.entries[index];
}

int
cbx_profiles_tab_profile_count(const cbx_profiles_tab *tab)
{
    return tab ? tab->profiles.count : 0;
}

int
cbx_profiles_tab_selected(const cbx_profiles_tab *tab)
{
    return tab ? tab->selected_profile : -1;
}

cbx_pt_mode
cbx_profiles_tab_mode(const cbx_profiles_tab *tab)
{
    return tab ? tab->mode : CBX_PT_MODE_LIST;
}

const char *
cbx_profiles_tab_name_buffer(const cbx_profiles_tab *tab)
{
    return tab ? tab->name_buf : NULL;
}

const cbx_profile_list *
cbx_profiles_tab_list(const cbx_profiles_tab *tab)
{
    return tab ? &tab->profiles : NULL;
}