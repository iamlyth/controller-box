/*
 * interaction_inventory.c — Machine-readable interaction acceptance inventory
 * (Task 7, SPEC §5.7).
 *
 * Static array enumerating every interactive manager control (M01–M38),
 * overlay action (O01–O12), and disabled/degraded scenario (D01–D08).
 *
 * Each entry records:
 *   - control ID (M/O/D prefix + 2-digit number)
 *   - category (manager tabbar, controllers, profiles, settings, editor,
 *     overlay, or disabled/degraded)
 *   - context (tab name, editor, overlay, or scenario name)
 *   - widget type (tab, list, button, picker, name_input, etc.)
 *   - controller-path description (focus chain → A activation)
 *   - pointer-path description (mouse click) or n/a
 *   - expected semantic outcome (observable state transition, DBus call,
 *     file mutation, dialog navigation, etc.)
 *   - production dispatch path (function chain from SDL event to outcome)
 *   - verification status (unverified until the evidence task lands)
 *   - evidence task number (which implementation task provides the test)
 *
 * Tests iterate this array to drive automated traversal through normal
 * SDL events and production dispatch (SPEC §5.7).
 */
#include "interaction_inventory.h"

#include <string.h>

/* Helper macros for readability */
#define NA  CBX_PATH_NA
#define AVAIL CBX_PATH_AVAILABLE

static const cbx_interaction_entry inventory[] = {
    /* ---- Manager — tab bar (M01–M03) ---- */
    { "M01", CBX_CAT_MANAGER_TABBAR, "Tab bar",
      CBX_WIDGET_TAB,
      "Left/Right from any tab → focus tabbar → A",
      AVAIL, "Mouse move + click on tab rect",
      "Active tab switches to Controllers; panel children visible",
      "cbx_manager_handle_event → tabbar handle_event",
      CBX_VERIFY_UNVERIFIED, "Task 8" },

    { "M02", CBX_CAT_MANAGER_TABBAR, "Tab bar",
      CBX_WIDGET_TAB,
      "Left/Right → A",
      AVAIL, "Mouse click on tab rect",
      "Active tab switches to Profiles",
      "cbx_manager_handle_event → tabbar handle_event",
      CBX_VERIFY_UNVERIFIED, "Task 8" },

    { "M03", CBX_CAT_MANAGER_TABBAR, "Tab bar",
      CBX_WIDGET_TAB,
      "Left/Right → A",
      AVAIL, "Mouse click on tab rect",
      "Active tab switches to Settings",
      "cbx_manager_handle_event → tabbar handle_event",
      CBX_VERIFY_UNVERIFIED, "Task 8" },

    /* ---- Manager — Controllers tab (M04–M09) ---- */
    { "M04", CBX_CAT_MANAGER_CTRL, "Controllers tab",
      CBX_WIDGET_LIST,
      "Down from tabbar → Up/Down to scroll",
      AVAIL, "Mouse click on list item",
      "Item selected (visual focus)",
      "cbx_manager_handle_event → panel → list handle_event",
      CBX_VERIFY_UNVERIFIED, "Task 8" },

    { "M05", CBX_CAT_MANAGER_CTRL, "Controllers tab",
      CBX_WIDGET_BUTTON,
      "Down from list (boundary) → A",
      AVAIL, "Mouse click on button rect",
      "Type picker opens (mode change)",
      "cbx_manager_handle_event → panel → button handle_event → cbx_controllers_tab_begin_type_pick",
      CBX_VERIFY_UNVERIFIED, "Task 8" },

    { "M06", CBX_CAT_MANAGER_CTRL, "Controllers tab",
      CBX_WIDGET_BUTTON,
      "Down from Add → A",
      AVAIL, "Mouse click on button rect",
      "StopTargetDevice DBus call; device count decreases",
      "cbx_manager_handle_event → panel → button → cbx_controllers_tab_remove",
      CBX_VERIFY_UNVERIFIED, "Task 8" },

    { "M07", CBX_CAT_MANAGER_CTRL, "Controllers tab",
      CBX_WIDGET_BUTTON,
      "Down from Remove → A",
      AVAIL, "Mouse click on button rect",
      "Type picker opens (change mode)",
      "cbx_manager_handle_event → panel → button → cbx_controllers_tab_begin_type_pick",
      CBX_VERIFY_UNVERIFIED, "Task 8" },

    { "M08", CBX_CAT_MANAGER_CTRL, "Controllers tab",
      CBX_WIDGET_PICKER,
      "Down to picker → A on type item",
      AVAIL, "Mouse click on type item",
      "CreateTargetDevice or SetTargetDevices DBus call; device type changes; picker closes",
      "cbx_manager_handle_event → panel → list on_select → cbx_controllers_tab_confirm_type_pick",
      CBX_VERIFY_UNVERIFIED, "Task 8" },

    { "M09", CBX_CAT_MANAGER_CTRL, "Controllers tab",
      CBX_WIDGET_PICKER,
      "B while picker open",
      NA, "n/a (controller-only sub-mode)",
      "Picker closes, no DBus call",
      "cbx_manager_handle_event → tab cancel handling",
      CBX_VERIFY_NOT_APPLICABLE, "Task 8" },

    /* ---- Manager — Profiles tab (M10–M20) ---- */
    { "M10", CBX_CAT_MANAGER_PROF, "Profiles tab",
      CBX_WIDGET_LIST,
      "Down from tabbar → Up/Down",
      AVAIL, "Mouse click on item",
      "Profile selected (visual focus)",
      "cbx_manager_handle_event → panel → list",
      CBX_VERIFY_UNVERIFIED, "Task 9" },

    { "M11", CBX_CAT_MANAGER_PROF, "Profiles tab",
      CBX_WIDGET_BUTTON,
      "Down from list → A",
      AVAIL, "Mouse click on button rect",
      "Create source picker opens (Default copy / Empty / Clone)",
      "cbx_manager_handle_event → panel → button → cbx_profiles_tab_begin_create",
      CBX_VERIFY_UNVERIFIED, "Task 9" },

    { "M12", CBX_CAT_MANAGER_PROF, "Profiles tab",
      CBX_WIDGET_PICKER,
      "Up/Down to select source → A",
      AVAIL, "Mouse click on source item",
      "Source selected; name-input mode opens",
      "cbx_manager_handle_event → panel → list on_select → cbx_profiles_tab_begin_name_input",
      CBX_VERIFY_UNVERIFIED, "Task 9" },

    { "M13", CBX_CAT_MANAGER_PROF, "Profiles tab",
      CBX_WIDGET_NAME_INPUT,
      "Letter keys while in name-input mode",
      NA, "n/a",
      "Characters appended to profile name",
      "cbx_manager_handle_event → cbx_profiles_tab_name_input_char",
      CBX_VERIFY_NOT_APPLICABLE, "Task 9" },

    { "M14", CBX_CAT_MANAGER_PROF, "Profiles tab",
      CBX_WIDGET_NAME_INPUT,
      "Backspace while in name-input mode",
      NA, "n/a",
      "Last char deleted",
      "cbx_manager_handle_event → cbx_profiles_tab_name_input_backspace",
      CBX_VERIFY_NOT_APPLICABLE, "Task 9" },

    { "M15", CBX_CAT_MANAGER_PROF, "Profiles tab",
      CBX_WIDGET_NAME_INPUT,
      "A/Return while in name-input mode",
      NA, "n/a",
      "Editor opens with new in-memory profile (Default copy or Clone bindings, or Empty); no file written yet",
      "cbx_manager_handle_event → cbx_profiles_tab_name_input_confirm → editor init",
      CBX_VERIFY_NOT_APPLICABLE, "Task 9" },

    { "M16", CBX_CAT_MANAGER_PROF, "Profiles tab",
      CBX_WIDGET_NAME_INPUT,
      "B while in name-input mode",
      NA, "n/a",
      "Returns to profile list; no file created",
      "cbx_manager_handle_event → cbx_profiles_tab_name_input_cancel",
      CBX_VERIFY_NOT_APPLICABLE, "Task 9" },

    { "M17", CBX_CAT_MANAGER_PROF, "Profiles tab",
      CBX_WIDGET_BUTTON,
      "Down from Create → A",
      AVAIL, "Mouse click on button rect",
      "Profile editor opens with selected profile",
      "cbx_manager_handle_event → panel → button → profile editor init",
      CBX_VERIFY_UNVERIFIED, "Task 9" },

    { "M18", CBX_CAT_MANAGER_PROF, "Profiles tab",
      CBX_WIDGET_BUTTON,
      "Down from Edit → A",
      AVAIL, "Mouse click on button rect",
      "Confirm-delete mode opens",
      "cbx_manager_handle_event → panel → button → cbx_profiles_tab_begin_delete",
      CBX_VERIFY_UNVERIFIED, "Task 9" },

    { "M19", CBX_CAT_MANAGER_PROF, "Profiles tab",
      CBX_WIDGET_CONFIRM_DELETE,
      "A while in confirm-delete mode",
      NA, "n/a",
      "Profile file unlinked; sidecar deleted; list refreshes",
      "cbx_manager_handle_event → cbx_profiles_tab_confirm_delete",
      CBX_VERIFY_NOT_APPLICABLE, "Task 9" },

    { "M20", CBX_CAT_MANAGER_PROF, "Profiles tab",
      CBX_WIDGET_CONFIRM_DELETE,
      "B while in confirm-delete mode",
      NA, "n/a",
      "Returns to normal mode, no deletion",
      "cbx_manager_handle_event → cbx_profiles_tab_cancel_delete",
      CBX_VERIFY_NOT_APPLICABLE, "Task 9" },

    /* ---- Manager — Settings tab (M21–M27) ---- */
    { "M21", CBX_CAT_MANAGER_SETTINGS, "Settings tab",
      CBX_WIDGET_LIST,
      "Down from tabbar → Up/Down",
      AVAIL, "Mouse click on item",
      "Setting selected (visual focus)",
      "cbx_manager_handle_event → panel → list",
      CBX_VERIFY_UNVERIFIED, "Task 8" },

    { "M22", CBX_CAT_MANAGER_SETTINGS, "Settings tab",
      CBX_WIDGET_EDIT_MODE,
      "A on launch_at_boot item",
      AVAIL, "Mouse click on item",
      "Value toggles (e.g. launch_at_boot)",
      "cbx_manager_handle_event → cbx_settings_tab_activate",
      CBX_VERIFY_UNVERIFIED, "Task 8" },

    { "M23", CBX_CAT_MANAGER_SETTINGS, "Settings tab",
      CBX_WIDGET_EDIT_MODE,
      "A on theme/opacity/count/type/trigger/icon-override",
      AVAIL, "Mouse click on item",
      "Edit mode entered for that setting",
      "cbx_manager_handle_event → cbx_settings_tab_activate",
      CBX_VERIFY_UNVERIFIED, "Task 8" },

    { "M24", CBX_CAT_MANAGER_SETTINGS, "Settings tab",
      CBX_WIDGET_EDIT_MODE,
      "Up/Down while in edit mode",
      NA, "n/a",
      "Value cycles/adjusts",
      "cbx_manager_handle_event → cbx_settings_tab_edit_up/edit_down",
      CBX_VERIFY_NOT_APPLICABLE, "Task 8" },

    { "M25", CBX_CAT_MANAGER_SETTINGS, "Settings tab",
      CBX_WIDGET_EDIT_MODE,
      "A while in edit mode",
      NA, "n/a",
      "Edit mode exits, value applied",
      "cbx_manager_handle_event → cbx_settings_tab_confirm_edit",
      CBX_VERIFY_NOT_APPLICABLE, "Task 8" },

    { "M26", CBX_CAT_MANAGER_SETTINGS, "Settings tab",
      CBX_WIDGET_EDIT_MODE,
      "B while in edit mode",
      NA, "n/a",
      "Edit mode exits, value reverts from disk",
      "cbx_manager_handle_event → cbx_settings_tab_cancel_edit",
      CBX_VERIFY_NOT_APPLICABLE, "Task 8" },

    { "M27", CBX_CAT_MANAGER_SETTINGS, "Settings tab",
      CBX_WIDGET_BUTTON,
      "Down from list → A",
      AVAIL, "Mouse click on button rect",
      "settings.yaml written to disk",
      "cbx_manager_handle_event → panel → button → cbx_settings_tab_save → cbx_settings_save",
      CBX_VERIFY_UNVERIFIED, "Task 8" },

    /* ---- Manager — Profile editor (M28–M38) ---- */
    { "M28", CBX_CAT_MANAGER_EDITOR, "Profile editor",
      CBX_WIDGET_BINDING,
      "Up/Down to scroll",
      AVAIL, "Mouse click on item",
      "Binding highlighted; diagram lights corresponding button",
      "cbx_manager_handle_event → editor panel → list → cbx_profile_editor_move_down/activate",
      CBX_VERIFY_UNVERIFIED, "Task 9" },

    { "M29", CBX_CAT_MANAGER_EDITOR, "Profile editor",
      CBX_WIDGET_BINDING,
      "A on binding item",
      AVAIL, "Mouse click on item",
      "Binding edit sub-menu opens (target-pick or capture)",
      "cbx_manager_handle_event → cbx_profile_editor_activate",
      CBX_VERIFY_UNVERIFIED, "Task 9" },

    { "M30", CBX_CAT_MANAGER_EDITOR, "Profile editor",
      CBX_WIDGET_PICKER,
      "A on target item",
      AVAIL, "Mouse click on target item",
      "Binding target updated; picker closes",
      "cbx_manager_handle_event → list on_select → cbx_profile_editor_confirm_target_pick",
      CBX_VERIFY_UNVERIFIED, "Task 9" },

    { "M31", CBX_CAT_MANAGER_EDITOR, "Profile editor",
      CBX_WIDGET_CAPTURE,
      "A on 'capture' option in binding edit sub-menu",
      AVAIL, "Mouse click on 'capture' option",
      "Capture mode begins; waiting for physical button press",
      "cbx_manager_handle_event → cbx_profile_editor_begin_capture",
      CBX_VERIFY_UNVERIFIED, "Task 9" },

    { "M32", CBX_CAT_MANAGER_EDITOR, "Profile editor",
      CBX_WIDGET_CAPTURE,
      "Input event via DBus InputEvent",
      NA, "n/a",
      "Binding source captured; binding updated; capture ends",
      "ip_input_events → cbx_profile_editor_on_input_event",
      CBX_VERIFY_NOT_APPLICABLE, "Task 9" },

    { "M33", CBX_CAT_MANAGER_EDITOR, "Profile editor",
      CBX_WIDGET_SEQUENTIAL,
      "A on 'sequential' action in editor",
      AVAIL, "Mouse click on 'sequential' option",
      "Sequential mode begins; first button prompted",
      "cbx_manager_handle_event → cbx_profile_editor_begin_sequential",
      CBX_VERIFY_UNVERIFIED, "Task 9" },

    { "M34", CBX_CAT_MANAGER_EDITOR, "Profile editor",
      CBX_WIDGET_SEQUENTIAL,
      "Physical button press (DBus InputEvent)",
      NA, "n/a",
      "Button captured; auto-advance; progress bar updates",
      "ip_input_events → cbx_profile_editor_seq_on_input",
      CBX_VERIFY_NOT_APPLICABLE, "Task 9" },

    { "M35", CBX_CAT_MANAGER_EDITOR, "Profile editor",
      CBX_WIDGET_SEQUENTIAL,
      "B during sequential",
      NA, "n/a",
      "Current binding skipped; advance",
      "cbx_manager_handle_event → cbx_profile_editor_seq_skip",
      CBX_VERIFY_NOT_APPLICABLE, "Task 9" },

    { "M36", CBX_CAT_MANAGER_EDITOR, "Profile editor",
      CBX_WIDGET_SEQUENTIAL,
      "Start during sequential",
      NA, "n/a",
      "Sequential mode cancelled; changes discarded",
      "ip_input_events → cbx_profile_editor_cancel_sequential",
      CBX_VERIFY_NOT_APPLICABLE, "Task 9" },

    { "M37", CBX_CAT_MANAGER_EDITOR, "Profile editor",
      CBX_WIDGET_EDITOR,
      "B (or A on close) from list mode",
      NA, "n/a (controller-only — editor save/close via B key)",
      "Profile written to disk via cbx_profile_save_to_dir with NES validation; editor closes; profile list refreshes",
      "cbx_manager_handle_event → cbx_profile_save_to_dir → editor close",
      CBX_VERIFY_NOT_APPLICABLE, "Task 9" },

    { "M38", CBX_CAT_MANAGER_EDITOR, "Profile editor",
      CBX_WIDGET_EDITOR,
      "Start from list mode",
      NA, "n/a",
      "Editor closes; changes discarded; no file written",
      "cbx_manager_handle_event → cbx_profile_editor_cancel",
      CBX_VERIFY_NOT_APPLICABLE, "Task 9" },

    /* ---- Overlay actions (O01–O12) ---- */
    { "O01", CBX_CAT_OVERLAY, "Overlay",
      CBX_WIDGET_OVERLAY_ACTION,
      "InterceptMode PASS→ALL detected by poll",
      NA, "n/a",
      "Surface shown; lifecycle ACTIVATING→VISIBLE",
      "run_overlay_service poll loop → ip_intercept_poll_tick → cbx_overlay_lifecycle_activate",
      CBX_VERIFY_UNVERIFIED, "Task 11" },

    { "O02", CBX_CAT_OVERLAY, "Overlay",
      CBX_WIDGET_OVERLAY_ACTION,
      "Left key / DBus InputEvent",
      NA, "n/a",
      "Controller's grid column decreases; on_slot_change callback fires",
      "poll loop → sdl_key_to_pm_input or ip_input_events → cbx_player_mode_handle",
      CBX_VERIFY_UNVERIFIED, "Task 11" },

    { "O03", CBX_CAT_OVERLAY, "Overlay",
      CBX_WIDGET_OVERLAY_ACTION,
      "Right key / DBus InputEvent",
      NA, "n/a",
      "Column increases; callback fires",
      "poll loop → sdl_key_to_pm_input or ip_input_events → cbx_player_mode_handle",
      CBX_VERIFY_UNVERIFIED, "Task 11" },

    { "O04", CBX_CAT_OVERLAY, "Overlay",
      CBX_WIDGET_OVERLAY_ACTION,
      "Up key / DBus InputEvent",
      NA, "n/a",
      "Profile name changes; LoadProfilePath DBus call; assignment updated",
      "poll loop → cbx_player_mode_handle → cbx_profile_cycle_apply",
      CBX_VERIFY_UNVERIFIED, "Task 11" },

    { "O05", CBX_CAT_OVERLAY, "Overlay",
      CBX_WIDGET_OVERLAY_ACTION,
      "Down key / DBus InputEvent",
      NA, "n/a",
      "Profile name changes (reverse)",
      "poll loop → cbx_player_mode_handle → cbx_profile_cycle_apply",
      CBX_VERIFY_UNVERIFIED, "Task 11" },

    { "O06", CBX_CAT_OVERLAY, "Overlay",
      CBX_WIDGET_OVERLAY_ACTION,
      "R3 key / DBus InputEvent",
      NA, "n/a",
      "Host mode entered; other controllers freeze",
      "poll loop → cbx_player_mode_handle → cbx_host_mode_toggle",
      CBX_VERIFY_UNVERIFIED, "Task 11" },

    { "O07", CBX_CAT_OVERLAY, "Overlay",
      CBX_WIDGET_OVERLAY_ACTION,
      "Up/Down keys / DBus InputEvent",
      NA, "n/a",
      "Host selected row changes",
      "poll loop → cbx_host_mode_handle",
      CBX_VERIFY_UNVERIFIED, "Task 11" },

    { "O08", CBX_CAT_OVERLAY, "Overlay",
      CBX_WIDGET_OVERLAY_ACTION,
      "Left/Right keys / DBus InputEvent",
      NA, "n/a",
      "Host changes a row's slot; conflict may arise",
      "poll loop → cbx_host_mode_handle",
      CBX_VERIFY_UNVERIFIED, "Task 11" },

    { "O09", CBX_CAT_OVERLAY, "Overlay",
      CBX_WIDGET_OVERLAY_ACTION,
      "R3 key",
      NA, "n/a",
      "Host mode exits; controllers unfreeze",
      "poll loop → cbx_host_mode_handle → cbx_host_mode_exit",
      CBX_VERIFY_UNVERIFIED, "Task 11" },

    { "O10", CBX_CAT_OVERLAY, "Overlay",
      CBX_WIDGET_OVERLAY_ACTION,
      "B key",
      NA, "n/a",
      "Assignments saved; conflicts auto-resolved; InterceptMode set to PASS; surface hidden",
      "poll loop → cbx_player_mode_handle/cbx_host_mode_handle → cbx_overlay_lifecycle_close → cbx_close_on_save",
      CBX_VERIFY_UNVERIFIED, "Task 11" },

    { "O11", CBX_CAT_OVERLAY, "Overlay",
      CBX_WIDGET_OVERLAY_ACTION,
      "DBus InputEvent from different device paths",
      NA, "n/a",
      "Each controller moves its own row independently",
      "poll loop → ip_input_events → device_path→row mapping → cbx_player_mode_handle",
      CBX_VERIFY_UNVERIFIED, "Task 11" },

    { "O12", CBX_CAT_OVERLAY, "Overlay",
      CBX_WIDGET_OVERLAY_ACTION,
      "TBD (Up/Down repurposed for row nav in Host Mode; spec §4.4 says host can 'edit slot/profile' but cbx_hm_input has no profile-cycle input)",
      NA, "n/a",
      "Host changes the profile of the selected row",
      "poll loop → cbx_host_mode_handle (not yet implemented)",
      CBX_VERIFY_DEFERRED, "Task 11 (remediation if blocking)" },

    /* ---- Disabled/degraded/operation-failure scenarios (D01–D08) ---- */
    { "D01", CBX_CAT_DISABLED, "Controllers tab — InputPlumber unavailable",
      CBX_WIDGET_SCENARIO,
      "Navigate to Controllers tab; attempt Add/Remove/ChangeType",
      NA, "n/a (degraded state; controls disabled)",
      "Degraded content shown; Add/Remove/ChangeType disabled; activation rejected, no DBus call",
      "cbx_manager_handle_event → disabled widget rejection",
      CBX_VERIFY_UNVERIFIED, "Task 8" },

    { "D02", CBX_CAT_DISABLED, "Controllers tab — no device selected",
      CBX_WIDGET_SCENARIO,
      "Activate Remove button with no device selected",
      NA, "n/a (disabled state)",
      "Disabled; activation produces no DBus side effect",
      "cbx_manager_handle_event → disabled widget rejection",
      CBX_VERIFY_UNVERIFIED, "Task 8" },

    { "D03", CBX_CAT_DISABLED, "Profiles tab — no profile selected",
      CBX_WIDGET_SCENARIO,
      "Activate Delete button with no profile selected",
      NA, "n/a (disabled state)",
      "Disabled; activation produces no file deletion",
      "cbx_manager_handle_event → disabled widget rejection",
      CBX_VERIFY_UNVERIFIED, "Task 9" },

    { "D04", CBX_CAT_DISABLED, "Profile editor — save with missing NES bindings",
      CBX_WIDGET_SCENARIO,
      "Attempt to save profile with missing NES bindings",
      NA, "n/a (validation failure)",
      "Error shown; profile not saved; cbx_profile_validate_nes_minimum returns error; editor stays open",
      "cbx_profile_save_to_dir → cbx_profile_validate_nes_minimum → error",
      CBX_VERIFY_UNVERIFIED, "Task 9" },

    { "D05", CBX_CAT_DISABLED, "Settings tab — edit cancel",
      CBX_WIDGET_SCENARIO,
      "Enter edit mode → B to cancel",
      NA, "n/a (controller-only cancel)",
      "Value reverts from disk; no settings.yaml write",
      "cbx_manager_handle_event → cbx_settings_tab_cancel_edit",
      CBX_VERIFY_UNVERIFIED, "Task 8" },

    { "D06", CBX_CAT_DISABLED, "Controllers tab — DBus operation failure",
      CBX_WIDGET_SCENARIO,
      "Trigger CreateTargetDevice that returns error",
      NA, "n/a (backend failure)",
      "Error shown to user; UI remains responsive; no state corruption",
      "cbx_controllers_tab_confirm_type_pick → DBus error → error display",
      CBX_VERIFY_UNVERIFIED, "Task 8" },

    { "D07", CBX_CAT_DISABLED, "Profile editor — filesystem failure",
      CBX_WIDGET_SCENARIO,
      "Attempt to save profile when disk is full",
      NA, "n/a (filesystem failure)",
      "Error shown; profile not written; editor stays open",
      "cbx_profile_save_to_dir → write failure → error display",
      CBX_VERIFY_UNVERIFIED, "Task 9" },

    { "D08", CBX_CAT_DISABLED, "Profile editor — empty profile creation",
      CBX_WIDGET_SCENARIO,
      "Create empty profile → attempt save",
      NA, "n/a (validation failure)",
      "Editor opens with no bindings; save is blocked by NES validation until minimum bindings added",
      "cbx_profile_save_to_dir → cbx_profile_validate_nes_minimum → error",
      CBX_VERIFY_UNVERIFIED, "Task 9" },

    /* Terminator */
    { NULL, (cbx_inv_category)0, NULL, (cbx_inv_widget_type)0,
      NULL, (cbx_inv_path_availability)0, NULL, NULL, NULL,
      (cbx_inv_verify_status)0, NULL }
};

const cbx_interaction_entry *cbx_interaction_inventory_get(void)
{
    return inventory;
}

size_t cbx_interaction_inventory_count(void)
{
    return sizeof(inventory) / sizeof(inventory[0]) - 1; /* exclude terminator */
}

const cbx_interaction_entry *cbx_interaction_inventory_find(const char *id)
{
    if (!id)
        return NULL;
    for (size_t i = 0; inventory[i].id != NULL; i++) {
        if (strcmp(inventory[i].id, id) == 0)
            return &inventory[i];
    }
    return NULL;
}