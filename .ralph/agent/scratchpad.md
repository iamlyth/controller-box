# Task 5: Wire profile editor into manager production path

## Understanding

Task 5 wires the profile editor (list mode + sequential mode) into the production manager path. Currently:
- The Edit button in profiles_tab.c is a **no-op placeholder**
- The create flow writes a file to disk instead of opening the editor
- The editor is only tested in isolation (test_editor_list_mode.c, test_editor_seq_mode.c)
- Visual/golden tests manually initialize the editor (bypassing production path)
- `expected_sender` in capture/sequential mode uses `IP_DBUS_NAME` (well-known name) instead of unique bus name — making capture completely broken in production

## Implementation Plan

### A. Profile editor changes

1. **profile_editor_list.h**: Add `CBX_EDITOR_MODE_BINDING_EDIT` enum value. Add `char expected_sender[128]` field to struct.

2. **profile_editor_list.c**: 
   - `cbx_profile_editor_set_dbus`: Resolve unique bus name via `backend->get_unique_name(bus, IP_DBUS_NAME, &unique)` and store in `ed->expected_sender`
   - `cbx_profile_editor_begin_capture`: Replace `IP_DBUS_NAME` with `ed->expected_sender`
   - `cbx_profile_editor_activate`: In LIST mode, open a binding edit sub-menu (mode=BINDING_EDIT) with "Pick Target", "Capture", "Sequential (All Buttons)" options using target_list widget
   - Handle BINDING_EDIT mode in `activate` (dispatch to target-pick/capture/sequential), `cancel` (back to LIST), `move_up/down` (scroll target_list)

3. **profile_editor_seq.c**: Replace `IP_DBUS_NAME` with `ed->expected_sender` in `begin_sequential`

### B. Profiles tab changes

4. **profiles_tab.h**: Add `CBX_PT_MODE_EDITOR` mode. Add fields: `cbx_profile_editor editor`, `bool editor_initialized`, `bool editor_active`, `char editor_profile_name[CBX_PT_NAME_LEN]`, `bool editor_is_new`, `SDL_Renderer *renderer`, `const ip_dbus_backend *dbus_backend`, `ip_bus_handle dbus_bus`. Add `cbx_profiles_tab_set_context()` declaration.

5. **profiles_tab.c**:
   - `cbx_profiles_tab_set_context()`: Store renderer and DBus backend/bus
   - `on_edit_pressed`: Load selected profile, lazy-init editor, set DBus info, show editor widgets, hide tab widgets, set mode=EDITOR
   - `cbx_profiles_tab_name_input_confirm`: Instead of `cbx_profiles_tab_create` (writes file), build in-memory profile and open editor with it
   - New `cbx_profiles_tab_open_editor()`: Common code for opening editor (both edit and create paths)
   - New `cbx_profiles_tab_close_editor()`: Hide editor widgets, show tab widgets, set mode=LIST, refresh
   - New `cbx_profiles_tab_save_editor()`: Get profile from editor, save via `cbx_profile_save_to_dir`, on success close+refresh, on failure show error in editor status
   - `cbx_profiles_tab_handle_key`: Handle CBX_PT_MODE_EDITOR — Up/Down → editor move, A/B KEYDOWN → swallow, Start (TAB) → cancel editor
   - `cbx_profiles_tab_activate`: Handle CBX_PT_MODE_EDITOR → editor activate
   - `cbx_profiles_tab_cancel`: Handle CBX_PT_MODE_EDITOR — B in LIST → save, B in sub-modes → editor cancel, B in SEQUENTIAL → skip
   - `cbx_profiles_tab_shutdown`: Shutdown editor if initialized

6. **manager.c**: Update mode tracking to include editor mode: `prev_mode = pt.mode * 100 + editor.mode`

### C. Test changes

7. **test_profiles_tab.c**: Add editor dispatch tests — open editor via Edit button, verify editor widgets visible, test save/close, test cancel
8. **test_manager_production.c**: Add test that opens editor via production dispatch
9. **test_manager_visual.c**: Update tests 7-9 to use production Edit-button path
10. **test_golden.c**: Update tests 9-11 to use production Edit-button path, regenerate goldens
11. **test_editor_list_mode.c**: Add expected_sender security test

## Key design decisions

- **Lazy editor init**: Editor initialized on first open (avoids needing renderer at tab init time)
- **Binding edit sub-menu**: New `CBX_EDITOR_MODE_BINDING_EDIT` mode shows a 3-item picker (Pick Target / Capture / Sequential) using existing target_list widget
- **Start key = SDLK_TAB**: Used for cancel-editor and cancel-sequential (not used elsewhere in manager)
- **B in editor LIST mode = save+close**: Per spec M37. B in sub-modes = cancel sub-mode. B in SEQUENTIAL = skip.
- **Mode tracking**: Combined `pt.mode * 100 + editor.mode` so manager detects editor internal mode changes for focus chain rebuilds
- **Editor widgets added to profiles panel**: They're hidden when editor inactive, shown when active. Focus chain automatically skips invisible widgets.