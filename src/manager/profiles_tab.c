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

/* Build the full path for a new profile in the user profiles dir. */
static int
build_profile_path(cbx_profiles_tab *tab, char *buf, size_t buflen,
                     const char *name)
{
    if (tab && tab->test_user_dir) {
        snprintf(buf, buflen, "%s/%s.yaml", tab->test_user_dir, name);
        return 0;
    }
    char dir[PATH_MAX];
    int rc = cbx_user_profiles_dir(dir, sizeof(dir));
    if (rc != 0)
        return rc;
    snprintf(buf, buflen, "%s/%s.yaml", dir, name);
    return 0;
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
/*  Button callbacks                                                   */
/* ------------------------------------------------------------------ */

static void
on_create_pressed(cbx_widget *w, void *user_data)
{
    (void)w;
    cbx_profiles_tab *tab = (cbx_profiles_tab *)user_data;
    if (!tab)
        return;
    /* Default to copying the default profile. */
    cbx_profiles_tab_begin_create(tab, CBX_PT_CREATE_DEFAULT_COPY);
}

static void
on_delete_pressed(cbx_widget *w, void *user_data)
{
    (void)w;
    cbx_profiles_tab *tab = (cbx_profiles_tab *)user_data;
    if (!tab)
        return;
    int idx = tab->selected_profile;
    if (idx >= 0 && idx < tab->profiles.count)
        cbx_profiles_tab_begin_delete(tab, idx);
}

static void
on_edit_pressed(cbx_widget *w, void *user_data)
{
    (void)w;
    /* Editing is handled by Task 37/38 — this is a placeholder. */
    cbx_profiles_tab *tab = (cbx_profiles_tab *)user_data;
    if (!tab || !tab->panel)
        return;
    /* No-op for now; the editor is a separate task. */
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

    /* --- Add widgets to panel ------------------------------------- */
    cbx_panel_add_child(panel, &tab->profile_list_w.base);
    cbx_panel_add_child(panel, &tab->create_btn.base);
    cbx_panel_add_child(panel, &tab->edit_btn.base);
    cbx_panel_add_child(panel, &tab->delete_btn.base);
    cbx_panel_add_child(panel, &tab->status_lbl.base);
    cbx_panel_add_child(panel, &tab->create_picker.base);

    /* --- Layout --------------------------------------------------- */
    SDL_Rect pr;
    cbx_widget_get_rect(&panel->base, &pr);

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

    /* NOTE: caller must call cbx_profiles_tab_refresh() after init.
     * For testing, call cbx_profiles_tab_set_test_dirs() first. */

    return 0;
}

void
cbx_profiles_tab_shutdown(cbx_profiles_tab *tab)
{
    if (!tab)
        return;

    if (tab->panel) {
        cbx_panel_remove_child(tab->panel, &tab->profile_list_w.base);
        cbx_panel_remove_child(tab->panel, &tab->create_btn.base);
        cbx_panel_remove_child(tab->panel, &tab->edit_btn.base);
        cbx_panel_remove_child(tab->panel, &tab->delete_btn.base);
        cbx_panel_remove_child(tab->panel, &tab->status_lbl.base);
        cbx_panel_remove_child(tab->panel, &tab->create_picker.base);
    }

    cbx_widget_destroy(&tab->profile_list_w.base);
    cbx_widget_destroy(&tab->create_picker.base);
    cbx_widget_destroy(&tab->create_btn.base);
    cbx_widget_destroy(&tab->edit_btn.base);
    cbx_widget_destroy(&tab->delete_btn.base);
    cbx_widget_destroy(&tab->status_lbl.base);

    memset(tab, 0, sizeof(*tab));
}

/* ------------------------------------------------------------------ */
/*  Refresh                                                            */
/* ------------------------------------------------------------------ */

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

    /* Build the output path. */
    char path[PATH_MAX + 128];
    int rc = build_profile_path(tab, path, sizeof(path), name);
    if (rc != 0)
        return rc;

    /* Build the profile content based on the source. */
    cbx_profile prof;
    cbx_profile_init(&prof);
    snprintf(prof.name, sizeof(prof.name), "%s", name);

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

    /* Save the profile. */
    rc = cbx_profile_save(&prof, path);
    if (rc != 0)
        return rc;

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

    /* Return to list mode first. */
    cbx_pt_create_source src = tab->create_source;
    char name[CBX_PT_NAME_LEN];
    snprintf(name, sizeof(name), "%s", tab->name_buf);

    cbx_profiles_tab_name_input_cancel(tab);

    /* Create the profile. */
    return cbx_profiles_tab_create(tab, name, src);
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

/* ------------------------------------------------------------------ */
/*  Test directory overrides                                           */
/* ------------------------------------------------------------------ */

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