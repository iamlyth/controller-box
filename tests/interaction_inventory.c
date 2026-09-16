/*
 * interaction_inventory.c — Machine-readable interaction acceptance inventory
 * (Task 1, SPEC §5.7).
 *
 * Static array enumerating every interactive manager control (M01–M45),
 * overlay action (O01–O13), and disabled/degraded scenario (D01–D08).
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
 * Verification status legend:
 *   VERIFIED       — has native-DBus production-path evidence (controller
 *                    path, or both paths). evidence_task is "—" when fully
 *                    verified, or "Task N" when the pointer path is still
 *                    pending.
 *   UNVERIFIED     — mock-only or no evidence; awaits the referenced task
 *                    for native-DBus production-path verification.
 *   NOT_APPLICABLE — pointer path is not applicable (controller-only action
 *                    such as physical-button capture or name-input typing).
 *   DEFERRED       — explicitly deferred per §13.
 *
 * Tests iterate this array to drive automated traversal through normal
 * SDL events and production dispatch (SPEC §5.7).
 */
#include "interaction_inventory.h"

#include <errno.h>
#include <stdbool.h>
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
      CBX_VERIFY_VERIFIED, "—" },

    { "M02", CBX_CAT_MANAGER_TABBAR, "Tab bar",
      CBX_WIDGET_TAB,
      "Left/Right → A",
      AVAIL, "Mouse click on tab rect",
      "Active tab switches to Profiles",
      "cbx_manager_handle_event → tabbar handle_event",
      CBX_VERIFY_VERIFIED, "—" },

    { "M03", CBX_CAT_MANAGER_TABBAR, "Tab bar",
      CBX_WIDGET_TAB,
      "Left/Right → A",
      AVAIL, "Mouse click on tab rect",
      "Active tab switches to Settings",
      "cbx_manager_handle_event → tabbar handle_event",
      CBX_VERIFY_VERIFIED, "—" },

    /* ---- Manager — Controllers tab (M04–M09) ---- */
    { "M04", CBX_CAT_MANAGER_CTRL, "Controllers tab",
      CBX_WIDGET_LIST,
      "Down from tabbar → Up/Down to scroll",
      AVAIL, "Mouse click on list item",
      "Item selected (visual focus)",
      "cbx_manager_handle_event → panel → list handle_event",
      CBX_VERIFY_VERIFIED, "Task 4" },

    { "M05", CBX_CAT_MANAGER_CTRL, "Controllers tab",
      CBX_WIDGET_BUTTON,
      "Down from list (boundary) → A",
      AVAIL, "Mouse click on button rect",
      "Type picker opens (mode change)",
      "cbx_manager_handle_event → panel → button handle_event → cbx_controllers_tab_begin_type_pick",
      CBX_VERIFY_VERIFIED, "—" },

    { "M06", CBX_CAT_MANAGER_CTRL, "Controllers tab",
      CBX_WIDGET_BUTTON,
      "Down from Add → A",
      AVAIL, "Mouse click on button rect",
      "StopTargetDevice DBus call; device count decreases",
      "cbx_manager_handle_event → panel → button → cbx_controllers_tab_remove",
      CBX_VERIFY_VERIFIED, "—" },

    { "M07", CBX_CAT_MANAGER_CTRL, "Controllers tab",
      CBX_WIDGET_BUTTON,
      "Down from Remove → A",
      AVAIL, "Mouse click on button rect",
      "Type picker opens (change mode)",
      "cbx_manager_handle_event → panel → button → cbx_controllers_tab_begin_type_pick",
      CBX_VERIFY_VERIFIED, "—" },

    { "M08", CBX_CAT_MANAGER_CTRL, "Controllers tab",
      CBX_WIDGET_PICKER,
      "Down to picker → A on type item",
      AVAIL, "Mouse click on type item",
      "Exact CreateTargetDevice(new type) request issued once; owning composite TargetDevices route rewritten to the replacement; old target stopped; device type changes; picker closes",
      "cbx_manager_handle_event → panel → list on_select → cbx_controllers_tab_confirm_type_pick",
      CBX_VERIFY_VERIFIED, "test_manager_interaction_ctrl.c (mock: exact request + route, ctrl/pointer); test_manager_native.c::test_m08_change_type_confirm_{controller,pointer}_native (native readback)" },

    { "M09", CBX_CAT_MANAGER_CTRL, "Controllers tab",
      CBX_WIDGET_PICKER,
      "B while picker open",
      NA, "n/a (cancel is B/ESC; the picker exposes no cancel widget)",
      "Picker closes, no DBus call",
      "cbx_manager_handle_event (B/ESC KEYDOWN) → cbx_controllers_tab_cancel_type_pick",
      CBX_VERIFY_VERIFIED, "Task 4 (controller); pointer n/a" },

    /* ---- Manager — Profiles tab (M10–M20) ---- */
    { "M10", CBX_CAT_MANAGER_PROF, "Profiles tab",
      CBX_WIDGET_LIST,
      "Down from tabbar → Up/Down",
      AVAIL, "Mouse click on item",
      "Profile selected (visual focus)",
      "cbx_manager_handle_event → panel → list",
      CBX_VERIFY_VERIFIED, "Task 5" },

    { "M11", CBX_CAT_MANAGER_PROF, "Profiles tab",
      CBX_WIDGET_BUTTON,
      "Down from list → A",
      AVAIL, "Mouse click on button rect",
      "Create source picker opens (Default copy / Empty / Clone)",
      "cbx_manager_handle_event → panel → button → cbx_profiles_tab_begin_create",
      CBX_VERIFY_VERIFIED, "—" },

    { "M12", CBX_CAT_MANAGER_PROF, "Profiles tab",
      CBX_WIDGET_PICKER,
      "Up/Down to select source → A",
      AVAIL, "Mouse click on source item",
      "Source selected; name-input mode opens",
      "cbx_manager_handle_event → panel → list on_select → cbx_profiles_tab_begin_name_input",
      CBX_VERIFY_VERIFIED, "Task 5" },

    { "M13", CBX_CAT_MANAGER_PROF, "Profiles tab",
      CBX_WIDGET_NAME_INPUT,
      "Controller D-pad Up/Down cycles the character at the cursor; Left/Right moves the cursor (no keyboard required)",
      NA, "n/a (character entry is controller/keyboard-only; dialog confirm/cancel are M15/M16)",
      "Characters appended/changed in the profile name buffer",
      "SDL_CONTROLLERBUTTONDOWN → cbx_manager_controller_to_key → SDLK_UP/DOWN → cbx_profiles_tab_name_input_cycle; keyboard letters supplemental",
      CBX_VERIFY_VERIFIED, "test_manager_native_prof.c (ctrl); test_manager_interaction_prof.c (keyboard supplemental)" },

    { "M14", CBX_CAT_MANAGER_PROF, "Profiles tab",
      CBX_WIDGET_NAME_INPUT,
      "Controller Back/Select (mapped to SDLK_BACKSPACE) deletes the character before the cursor",
      NA, "n/a (character deletion is controller/keyboard-only)",
      "Character deleted from the profile name buffer",
      "SDL_CONTROLLERBUTTONDOWN (BACK) → SDLK_BACKSPACE → cbx_profiles_tab_name_input_backspace; keyboard Backspace supplemental",
      CBX_VERIFY_VERIFIED, "test_manager_native_prof.c (ctrl); test_manager_interaction_prof.c (keyboard supplemental)" },

    { "M15", CBX_CAT_MANAGER_PROF, "Profiles tab",
      CBX_WIDGET_NAME_INPUT,
      "A/Return while in name-input mode",
      AVAIL, "Mouse click on confirm button in name-input dialog",
      "Editor opens with new in-memory profile (Default copy or Clone bindings, or Empty); no file written yet",
      "cbx_manager_handle_event → mouse → dialog_confirm_btn → cbx_profiles_tab_name_input_confirm → editor init",
      CBX_VERIFY_VERIFIED, "Task 5" },

    { "M16", CBX_CAT_MANAGER_PROF, "Profiles tab",
      CBX_WIDGET_NAME_INPUT,
      "B while in name-input mode",
      AVAIL, "Mouse click on cancel button in name-input dialog",
      "Returns to profile list; no file created",
      "cbx_manager_handle_event → mouse → dialog_cancel_btn → cbx_profiles_tab_name_input_cancel",
      CBX_VERIFY_VERIFIED, "Task 3" },

    { "M17", CBX_CAT_MANAGER_PROF, "Profiles tab",
      CBX_WIDGET_BUTTON,
      "Down from Create → A",
      AVAIL, "Mouse click on button rect",
      "Profile editor opens with selected profile",
      "cbx_manager_handle_event → panel → button → profile editor init",
      CBX_VERIFY_VERIFIED, "—" },

    { "M18", CBX_CAT_MANAGER_PROF, "Profiles tab",
      CBX_WIDGET_BUTTON,
      "Down from Edit → A",
      AVAIL, "Mouse click on button rect",
      "Confirm-delete mode opens",
      "cbx_manager_handle_event → panel → button → cbx_profiles_tab_begin_delete",
      CBX_VERIFY_VERIFIED, "Task 5" },

    { "M19", CBX_CAT_MANAGER_PROF, "Profiles tab",
      CBX_WIDGET_CONFIRM_DELETE,
      "A while in confirm-delete mode",
      AVAIL, "Mouse click on confirm (delete) button",
      "Profile file unlinked; sidecar deleted; list refreshes",
      "cbx_manager_handle_event → mouse → dialog_confirm_btn → cbx_profiles_tab_confirm_delete",
      CBX_VERIFY_VERIFIED, "Task 5" },

    { "M20", CBX_CAT_MANAGER_PROF, "Profiles tab",
      CBX_WIDGET_CONFIRM_DELETE,
      "B while in confirm-delete mode",
      AVAIL, "Mouse click on cancel (don't delete) button",
      "Returns to normal mode, no deletion",
      "cbx_manager_handle_event → mouse → dialog_cancel_btn → cbx_profiles_tab_cancel_delete",
      CBX_VERIFY_VERIFIED, "Task 5" },

    /* ---- Manager — Settings tab (M21–M27) ---- */
    { "M21", CBX_CAT_MANAGER_SETTINGS, "Settings tab",
      CBX_WIDGET_LIST,
      "Down from tabbar → Up/Down",
      AVAIL, "Mouse click on item",
      "Setting selected (visual focus)",
      "cbx_manager_handle_event → panel → list",
      CBX_VERIFY_VERIFIED, "Task 4" },

    { "M22", CBX_CAT_MANAGER_SETTINGS, "Settings tab",
      CBX_WIDGET_EDIT_MODE,
      "A on launch_at_boot item",
      AVAIL, "Mouse click on item",
      "Value toggles (e.g. launch_at_boot)",
      "cbx_manager_handle_event → cbx_settings_tab_activate",
      CBX_VERIFY_VERIFIED, "—" },

    { "M23", CBX_CAT_MANAGER_SETTINGS, "Settings tab",
      CBX_WIDGET_EDIT_MODE,
      "A on opacity/count/type/trigger/icon-override (theme is display-only)",
      AVAIL, "Mouse click on item",
      "Edit mode entered for that setting",
      "cbx_manager_handle_event → cbx_settings_tab_activate",
      CBX_VERIFY_VERIFIED, "Task 4" },

    { "M24", CBX_CAT_MANAGER_SETTINGS, "Settings tab",
      CBX_WIDGET_EDIT_MODE,
      "Controller D-pad Up/Down while in edit mode adjusts the value",
      NA, "n/a (no adjust widget exists; a pointer click on the setting row confirms the edit, M25)",
      "Value cycles/adjusts",
      "cbx_manager_handle_event → cbx_settings_tab_edit_up/edit_down",
      CBX_VERIFY_VERIFIED, "Task 4 (controller); pointer n/a" },

    { "M25", CBX_CAT_MANAGER_SETTINGS, "Settings tab",
      CBX_WIDGET_EDIT_MODE,
      "A while in edit mode",
      AVAIL, "Mouse click on the selected setting row confirms the edit",
      "Edit mode exits, value applied",
      "cbx_manager_handle_event → settings list on_select → cbx_settings_tab_confirm_edit",
      CBX_VERIFY_VERIFIED, "Task 4" },

    { "M26", CBX_CAT_MANAGER_SETTINGS, "Settings tab",
      CBX_WIDGET_EDIT_MODE,
      "B while in edit mode",
      NA, "n/a (cancel is B/ESC; there is no cancel widget)",
      "Edit mode exits, value reverts from disk",
      "cbx_manager_handle_event (B/ESC KEYDOWN) → cbx_settings_tab_cancel_edit",
      CBX_VERIFY_VERIFIED, "Task 4 (controller); pointer n/a" },

    { "M27", CBX_CAT_MANAGER_SETTINGS, "Settings tab",
      CBX_WIDGET_BUTTON,
      "Down from list → A",
      AVAIL, "Mouse click on button rect",
      "settings.yaml written to disk",
      "cbx_manager_handle_event → panel → button → cbx_settings_tab_save → cbx_settings_save",
      CBX_VERIFY_VERIFIED, "—" },

    /* ---- Manager — Profile editor (M28–M38) ---- */
    { "M28", CBX_CAT_MANAGER_EDITOR, "Profile editor",
      CBX_WIDGET_BINDING,
      "Up/Down to scroll",
      AVAIL, "Mouse click on item",
      "Binding highlighted; diagram lights corresponding button",
      "cbx_manager_handle_event → editor panel → list → cbx_profile_editor_move_down/up; controller: ctrl_press test_manager_native_prof.c::test_m28_binding_nav_ctrl; pointer: test_manager_native_prof.c::test_m29c_unbound_row_activation_pointer; keyboard: test_manager_interaction_prof.c::test_editor_list_nav",
      CBX_VERIFY_VERIFIED, "test_manager_native_prof.c (ctrl_press + pointer); test_manager_interaction_prof.c (keyboard supplemental)" },

    { "M29", CBX_CAT_MANAGER_EDITOR, "Profile editor",
      CBX_WIDGET_BINDING,
      "A on binding item",
      AVAIL, "Mouse click on item",
      "Binding edit sub-menu opens (target-pick or capture)",
      "cbx_manager_handle_event → cbx_profile_editor_activate; controller: ctrl_press test_manager_native_prof.c::test_m29_binding_edit_ctrl; keyboard: test_manager_interaction_prof.c::test_editor_activate_binding_keyboard",
      CBX_VERIFY_VERIFIED, "test_manager_native_prof.c (ctrl_press); test_manager_interaction_prof.c (keyboard supplemental)" },

    { "M30", CBX_CAT_MANAGER_EDITOR, "Profile editor",
      CBX_WIDGET_PICKER,
      "A on target item",
      AVAIL, "Mouse click on target item",
      "Binding target updated; picker closes",
      "cbx_manager_handle_event → list on_select → cbx_profile_editor_confirm_target_pick; controller: ctrl_press test_manager_native_prof.c::test_m30_target_pick_controller; keyboard: test_manager_interaction_prof.c::test_editor_target_pick_keyboard",
      CBX_VERIFY_VERIFIED, "test_manager_native_prof.c (ctrl_press); test_manager_interaction_prof.c (keyboard supplemental)" },

    { "M31", CBX_CAT_MANAGER_EDITOR, "Profile editor",
      CBX_WIDGET_CAPTURE,
      "A on 'capture' option in binding edit sub-menu",
      AVAIL, "Mouse click on 'capture' option",
      "Capture mode begins; waiting for physical button press",
      "cbx_manager_handle_event → cbx_profile_editor_begin_capture; controller: ctrl_press test_manager_native_prof.c::test_m31_capture_begin_controller; keyboard: test_manager_interaction_prof.c::test_editor_capture_begin_keyboard",
      CBX_VERIFY_VERIFIED, "test_manager_native_prof.c (ctrl_press); test_manager_interaction_prof.c (keyboard supplemental)" },

    { "M32", CBX_CAT_MANAGER_EDITOR, "Profile editor",
      CBX_WIDGET_CAPTURE,
      "Input event via DBus InputEvent",
      NA, "n/a",
      "Binding source captured; binding updated; capture ends",
      "direct callback: cbx_profile_editor_on_input_event (supplemental; DBus signal path tested in test_manager_native_prof)",
      CBX_VERIFY_NOT_APPLICABLE, "Task 5" },

    { "M33", CBX_CAT_MANAGER_EDITOR, "Profile editor",
      CBX_WIDGET_SEQUENTIAL,
      "A on 'sequential' action in editor",
      AVAIL, "Mouse click on 'sequential' option",
      "Sequential mode begins; first button prompted",
      "cbx_manager_handle_event → cbx_profile_editor_begin_sequential; controller: ctrl_press test_manager_native_prof.c::test_m33_seq_begin_controller; keyboard: test_manager_interaction_prof.c::test_editor_seq_begin_keyboard",
      CBX_VERIFY_VERIFIED, "test_manager_native_prof.c (ctrl_press); test_manager_interaction_prof.c (keyboard supplemental)" },

    { "M34", CBX_CAT_MANAGER_EDITOR, "Profile editor",
      CBX_WIDGET_SEQUENTIAL,
      "Physical button press (DBus InputEvent)",
      NA, "n/a",
      "Button captured; auto-advance; progress bar updates",
      "direct callback: cbx_profile_editor_on_input_event → cbx_profile_editor_seq_on_input; DBus signal path: EmitInputEvent → sd_bus_process → input_event_signal_cb → ip_input_events_handle → on_input_event (test_m34_seq_capture_dbus_signal)",
      CBX_VERIFY_NOT_APPLICABLE, "Task 5" },

    { "M35", CBX_CAT_MANAGER_EDITOR, "Profile editor",
      CBX_WIDGET_SEQUENTIAL,
      "B during sequential",
      NA, "n/a",
      "Current binding skipped; advance",
      "cbx_manager_handle_event → cbx_profile_editor_seq_skip",
      CBX_VERIFY_NOT_APPLICABLE, "Task 5" },

    { "M36", CBX_CAT_MANAGER_EDITOR, "Profile editor",
      CBX_WIDGET_SEQUENTIAL,
      "Start during sequential",
      NA, "n/a",
      "Sequential mode cancelled; changes discarded",
      "SDL_CONTROLLERBUTTON START → cbx_manager_controller_to_key → SDLK_TAB → cbx_profile_editor_cancel_sequential; controller: ctrl_press test_manager_native_prof.c::test_m36_seq_cancel",
      CBX_VERIFY_NOT_APPLICABLE, "Task 5" },

    { "M37", CBX_CAT_MANAGER_EDITOR, "Profile editor",
      CBX_WIDGET_EDITOR,
      "B (or A on close) from list mode",
      AVAIL, "Mouse click on Save button in editor",
      "Profile written to disk via cbx_profile_save_to_dir with NES validation; editor closes; profile list refreshes",
      "cbx_manager_handle_event → cbx_profile_save_to_dir → editor close; controller: ctrl_press test_manager_native_prof.c::test_m37_save_ctrl; keyboard: test_manager_interaction_prof.c::test_editor_save_close; pointer: click save_btn → cbx_profiles_tab_save_editor",
      CBX_VERIFY_VERIFIED, "test_manager_native_prof.c (ctrl_press); test_manager_interaction_prof.c (keyboard supplemental)" },

    { "M38", CBX_CAT_MANAGER_EDITOR, "Profile editor",
      CBX_WIDGET_EDITOR,
      "Start from list mode",
      AVAIL, "Mouse click on Discard button in editor",
      "Editor closes; changes discarded; no file written",
      "cbx_manager_handle_event → cbx_profile_editor_cancel; controller: ctrl_press test_manager_native_prof.c::test_m38_discard_ctrl; keyboard: test_manager_interaction_prof.c::test_editor_cancel_discard; pointer: click discard_btn → cbx_profiles_tab_discard_editor",
      CBX_VERIFY_VERIFIED, "test_manager_native_prof.c (ctrl_press); test_manager_interaction_prof.c (keyboard supplemental)" },

    /* ---- Manager — First-run service install (M39) ---- */
    { "M39", CBX_CAT_MANAGER_SETTINGS, "First-run service install",
      CBX_WIDGET_BUTTON,
      "A confirms / B cancels from first-run dialog",
      AVAIL, "Mouse click on Yes/No button",
      "A or Yes: systemd service unit written and enabled; B or No: dialog dismissed, no install; dialog closes in both cases",
      "cbx_manager_check_first_run → cbx_manager_handle_event → dialog → cbx_service_install; controller: test_manager_interaction_ctrl.c::test_first_run_confirm_virtual_controller (SDL virtual gamepad); pointer: click first_run_yes/first_run_no",
      CBX_VERIFY_VERIFIED, "test_manager_interaction_ctrl.c (SDL virtual controller + pointer)" },

    /* ---- Manager — Confirm-quit, create-picker cancel, read-only,
     *      editor sub-mode cancel, empty-profile add action (M40–M45) ---- */
    { "M40", CBX_CAT_MANAGER_PROF, "Profiles tab — confirm quit",
      CBX_WIDGET_EDITOR,
      "SDL_QUIT with a dirty editor → A (Save & Quit)",
      AVAIL, "Mouse click on the Confirm (Save & Quit) dialog button",
      "Profile saved; editor closes; manager exits; a failed save leaves the editor open with data intact and does NOT exit",
      "SDL_QUIT → cbx_profiles_tab_begin_confirm_quit → cbx_profiles_tab_handle_key (A) / on_dialog_confirm_pressed → cbx_profiles_tab_save_editor",
      CBX_VERIFY_VERIFIED, "test_manager_native_prof.c (ctrl+pointer)" },

    { "M41", CBX_CAT_MANAGER_PROF, "Profiles tab — confirm quit",
      CBX_WIDGET_EDITOR,
      "SDL_QUIT with a dirty editor → B (Discard & Quit)",
      AVAIL, "Mouse click on the Cancel (Discard & Quit) dialog button",
      "Editor closes; no file written; manager exits",
      "SDL_QUIT → cbx_profiles_tab_begin_confirm_quit → cbx_profiles_tab_handle_key (B) / on_dialog_cancel_pressed → cbx_profiles_tab_close_editor",
      CBX_VERIFY_VERIFIED, "test_manager_native_prof.c (ctrl+pointer)" },

    { "M42", CBX_CAT_MANAGER_PROF, "Profiles tab — create source picker",
      CBX_WIDGET_PICKER,
      "B while the create source picker is open",
      AVAIL, "Mouse click on the Cancel button in the create picker",
      "Picker closes; profile list restored; no file created",
      "cbx_manager_handle_event (B/ESC) / on_dialog_cancel_pressed → cbx_profiles_tab_cancel_create_pick",
      CBX_VERIFY_VERIFIED, "test_manager_native_prof.c (ctrl+pointer)" },

    { "M43", CBX_CAT_MANAGER_PROF, "Profiles tab — read-only Default",
      CBX_WIDGET_BUTTON,
      "Navigate to Edit with the read-only Default selected → A",
      AVAIL, "Mouse click on the Edit button with the read-only Default selected",
      "Read-only rejection: status shows 'Read-only profile'; mode stays LIST; no user default.yaml is written",
      "cbx_manager_handle_event → on_edit_pressed read_only guard (no editor open, no write)",
      CBX_VERIFY_VERIFIED, "test_manager_native_prof.c (ctrl); test_profiles_tab.c (pointer)" },

    { "M44", CBX_CAT_MANAGER_EDITOR, "Profile editor",
      CBX_WIDGET_EDITOR,
      "B in TARGET_PICK / CAPTURE / BINDING_EDIT sub-mode",
      NA, "n/a (the editor sub-mode cancel is B/ESC; visible Save/Discard controls are M37/M38)",
      "Binding sub-mode cancelled; returns to editor LIST; no profile mutation",
      "cbx_manager_handle_event → cbx_profiles_tab_cancel → cbx_profile_editor_cancel",
      CBX_VERIFY_VERIFIED, "test_manager_native_prof.c::test_m44_editor_submode_cancel_ctrl (ctrl); test_editor_list_mode.c (direct-callback supplemental)" },

    { "M45", CBX_CAT_MANAGER_EDITOR, "Profile editor — empty profile",
      CBX_WIDGET_EDITOR,
      "A on the binding list when the profile has zero mappings (add-first-binding)",
      AVAIL, "Mouse click on the first binding row with zero mappings",
      "Sequential mode begins; the first NES button is prompted",
      "cbx_manager_handle_event → cbx_profile_editor_activate LIST zero-mapping branch → cbx_profile_editor_begin_sequential",
      CBX_VERIFY_VERIFIED, "test_m45_empty_profile_sequential_ctrl (ctrl dispatch); test_m45_empty_profile_sequential_pointer (pointer dispatch); test_t16_empty_profile_sequential_save_native_reload (persistence supplement)" },

    /* ---- Overlay actions (O01–O13) ---- */
    { "O01", CBX_CAT_OVERLAY, "Overlay",
      CBX_WIDGET_OVERLAY_ACTION,
      "InterceptMode PASS→ALL detected by poll",
      NA, "n/a",
      "Surface shown; lifecycle ACTIVATING→VISIBLE",
      "run_overlay_service poll loop → ip_intercept_poll_tick → cbx_overlay_lifecycle_activate",
      CBX_VERIFY_VERIFIED, "Task 3" },

    { "O02", CBX_CAT_OVERLAY, "Overlay",
      CBX_WIDGET_OVERLAY_ACTION,
      "Left key / DBus InputEvent",
      NA, "n/a",
      "Controller's grid column decreases; on_slot_change callback fires",
      "poll loop → sdl_key_to_pm_input or ip_input_events → cbx_player_mode_handle",
      CBX_VERIFY_VERIFIED, "Task 3" },

    { "O03", CBX_CAT_OVERLAY, "Overlay",
      CBX_WIDGET_OVERLAY_ACTION,
      "Right key / DBus InputEvent",
      NA, "n/a",
      "Column increases; callback fires",
      "poll loop → sdl_key_to_pm_input or ip_input_events → cbx_player_mode_handle",
      CBX_VERIFY_VERIFIED, "Task 3" },

    { "O04", CBX_CAT_OVERLAY, "Overlay",
      CBX_WIDGET_OVERLAY_ACTION,
      "Up key / DBus InputEvent",
      NA, "n/a",
      "Profile name changes; LoadProfilePath DBus call; assignment updated",
      "poll loop → cbx_player_mode_handle → cbx_profile_cycle_apply",
      CBX_VERIFY_VERIFIED, "Task 3" },

    { "O05", CBX_CAT_OVERLAY, "Overlay",
      CBX_WIDGET_OVERLAY_ACTION,
      "Down key / DBus InputEvent",
      NA, "n/a",
      "Profile name changes (reverse)",
      "poll loop → cbx_player_mode_handle → cbx_profile_cycle_apply",
      CBX_VERIFY_VERIFIED, "Task 3" },

    { "O06", CBX_CAT_OVERLAY, "Overlay",
      CBX_WIDGET_OVERLAY_ACTION,
      "R3 key / DBus InputEvent",
      NA, "n/a",
      "Host mode entered; other controllers freeze",
      "poll loop → cbx_player_mode_handle → cbx_host_mode_toggle",
      CBX_VERIFY_VERIFIED, "Task 3" },

    { "O07", CBX_CAT_OVERLAY, "Overlay",
      CBX_WIDGET_OVERLAY_ACTION,
      "Up/Down keys / DBus InputEvent",
      NA, "n/a",
      "Host selected row changes",
      "poll loop → cbx_host_mode_handle",
      CBX_VERIFY_VERIFIED, "Task 3" },

    { "O08", CBX_CAT_OVERLAY, "Overlay",
      CBX_WIDGET_OVERLAY_ACTION,
      "Left/Right keys / DBus InputEvent",
      NA, "n/a",
      "Host changes a row's slot; conflict may arise",
      "poll loop → cbx_host_mode_handle",
      CBX_VERIFY_VERIFIED, "Task 3" },

    { "O09", CBX_CAT_OVERLAY, "Overlay",
      CBX_WIDGET_OVERLAY_ACTION,
      "R3 key",
      NA, "n/a",
      "Host mode exits; controllers unfreeze",
      "poll loop → cbx_host_mode_handle → cbx_host_mode_exit",
      CBX_VERIFY_VERIFIED, "Task 3" },

    { "O10", CBX_CAT_OVERLAY, "Overlay",
      CBX_WIDGET_OVERLAY_ACTION,
      "B key",
      NA, "n/a",
      "Assignments saved; conflicts auto-resolved; InterceptMode set to PASS; surface hidden",
      "DBus InputEvent 'B' (value 1.0) → ip_input_signal → cbx_player_mode_handle/cbx_host_mode_handle → cbx_overlay_lifecycle_close → cbx_close_on_save; controller close via emit_input_event test_overlay_native.c::test_o10_close_saves_and_sets_pass / test_o10b_close_conflict_resolution",
      CBX_VERIFY_VERIFIED, "Task 3" },

    { "O11", CBX_CAT_OVERLAY, "Overlay",
      CBX_WIDGET_OVERLAY_ACTION,
      "DBus InputEvent from different device paths",
      NA, "n/a",
      "Each controller moves its own row independently",
      "poll loop → ip_input_events → device_path→row mapping → cbx_player_mode_handle",
      CBX_VERIFY_VERIFIED, "Task 3" },

    { "O12", CBX_CAT_OVERLAY, "Overlay",
      CBX_WIDGET_OVERLAY_ACTION,
      "L1/R1 bumpers / DBus InputEvent",
      NA, "n/a",
      "Host cycles the selected row's profile (previous/next); profile follows the controller and LoadProfilePath is applied",
      "poll loop → ip_input_events → device_path→row → cbx_host_mode_handle → cbx_overlay_on_profile_change → cbx_profile_cycle_apply",
      CBX_VERIFY_VERIFIED, "Task 4" },

    { "O13", CBX_CAT_OVERLAY, "Overlay",
      CBX_WIDGET_OVERLAY_ACTION,
      "Two controllers independently navigate to same column (Player Mode)",
      NA, "n/a",
      "Conflict detected; red highlight appears on overlapping column; auto-move to lowest free column on save",
      "poll loop → cbx_player_mode_handle → cbx_conflict_detect → cbx_conflict_resolve",
      CBX_VERIFY_VERIFIED, "Task 3" },

    /* ---- Disabled/degraded/operation-failure scenarios (D01–D08) ---- */
    { "D01", CBX_CAT_DISABLED, "Controllers tab — InputPlumber unavailable",
      CBX_WIDGET_SCENARIO,
      "Navigate to Controllers tab; attempt Add/Remove/ChangeType",
      AVAIL, "Mouse click on disabled control (no effect)",
      "Degraded content shown; Add/Remove/ChangeType disabled; activation rejected, no DBus call",
      "cbx_manager_handle_event → disabled widget rejection",
      CBX_VERIFY_VERIFIED, "Task 6" },

    { "D02", CBX_CAT_DISABLED, "Controllers tab — no device selected",
      CBX_WIDGET_SCENARIO,
      "Activate Remove button with no device selected",
      AVAIL, "Mouse click on Remove with no selection (enabled no-op)",
      "No selection guard: no DBus side effect, no state change",
      "cbx_manager_handle_event → on_remove/on_change pressed selection guard",
      CBX_VERIFY_VERIFIED, "Task 6" },

    { "D03", CBX_CAT_DISABLED, "Profiles tab — no profile selected",
      CBX_WIDGET_SCENARIO,
      "Activate Delete button with no profile selected",
      AVAIL, "Mouse click on Delete with no selection (enabled no-op)",
      "No selection guard: no file deletion, no state change",
      "cbx_manager_handle_event → on_delete_pressed selection guard",
      CBX_VERIFY_VERIFIED, "Task 6" },

    { "D04", CBX_CAT_DISABLED, "Profile editor — save with missing NES bindings",
      CBX_WIDGET_SCENARIO,
      "Attempt to save profile with missing NES bindings",
      AVAIL, "Mouse click on save button (validation fails)",
      "Error shown; profile not saved; cbx_profile_validate_nes_minimum returns error; editor stays open",
      "cbx_profile_save_to_dir → cbx_profile_validate_nes_minimum → error",
      CBX_VERIFY_VERIFIED, "Task 6" },

    { "D05", CBX_CAT_DISABLED, "Settings tab — edit cancel",
      CBX_WIDGET_SCENARIO,
      "Enter edit mode → B to cancel",
      NA, "n/a (cancel is B/ESC only; M24/M26 document the controller-only edit controls)",
      "Value reverts from disk; no settings.yaml write",
      "cbx_manager_handle_event (B/ESC) → cbx_settings_tab_cancel_edit",
      CBX_VERIFY_VERIFIED, "Task 6 (controller); pointer n/a" },

    { "D06", CBX_CAT_DISABLED, "Controllers tab — DBus operation failure",
      CBX_WIDGET_SCENARIO,
      "Trigger CreateTargetDevice that returns error",
      AVAIL, "Mouse click on type item (DBus fails)",
      "Error shown to user; UI remains responsive; no state corruption",
      "cbx_controllers_tab_confirm_type_pick → DBus error → error display",
      CBX_VERIFY_VERIFIED, "Task 6" },

    { "D07", CBX_CAT_DISABLED, "Profile editor — filesystem failure",
      CBX_WIDGET_SCENARIO,
      "Attempt to save profile when disk is full",
      AVAIL, "Mouse click on save button (write fails)",
      "Error shown; profile not written; editor stays open",
      "cbx_profile_save_to_dir → write failure → error display",
      CBX_VERIFY_VERIFIED, "Task 6" },

    { "D08", CBX_CAT_DISABLED, "Profile editor — empty profile creation",
      CBX_WIDGET_SCENARIO,
      "Create empty profile → attempt save",
      AVAIL, "Mouse click on save button (NES validation fails)",
      "Editor opens with no bindings; save is blocked by NES validation until minimum bindings added",
      "cbx_profile_save_to_dir → cbx_profile_validate_nes_minimum → error",
      CBX_VERIFY_VERIFIED, "Task 6" },

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

/* ------------------------------------------------------------------ */
/*  Runtime verification ledger (SPEC §5.7)                            */
/* ------------------------------------------------------------------ */
/*
 * Mutable side table holding runtime "verified" marks.  Unlike the static
 * `inventory[]` array (whose verify_status is a declaration of intent),
 * these flags are only set by cbx_interaction_inventory_mark_verified()
 * which dispatch tests call after their assertions all pass.  This is
 * what ties the ledger's verified flags to actual test pass status.
 */
#define CBX_INV_MAX_ENTRIES 96
static bool g_runtime_verified[CBX_INV_MAX_ENTRIES];

int
cbx_interaction_inventory_mark_verified(const char *id)
{
    if (!id)
        return -EINVAL;
    for (size_t i = 0; inventory[i].id != NULL && i < CBX_INV_MAX_ENTRIES; i++) {
        if (strcmp(inventory[i].id, id) == 0) {
            g_runtime_verified[i] = true;
            return 0;
        }
    }
    return -EINVAL;
}

int
cbx_interaction_inventory_is_verified(const char *id)
{
    if (!id)
        return -1;
    for (size_t i = 0; inventory[i].id != NULL && i < CBX_INV_MAX_ENTRIES; i++) {
        if (strcmp(inventory[i].id, id) == 0)
            return g_runtime_verified[i] ? 1 : 0;
    }
    return -1;
}

void
cbx_interaction_inventory_reset(void)
{
    for (size_t i = 0; i < CBX_INV_MAX_ENTRIES; i++)
        g_runtime_verified[i] = false;
}