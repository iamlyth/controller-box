# Task 5 Complete — Profile editor wired into manager production path

## What was done

### Source changes

1. **`src/manager/profile_editor_list.h`** — Added `CBX_EDITOR_MODE_BINDING_EDIT` enum and `char expected_sender[128]` field to `cbx_profile_editor` struct.

2. **`src/manager/profile_editor_list.c`** — 
   - `cbx_profile_editor_set_dbus()`: Resolves unique bus name via `backend->get_unique_name(bus, IP_DBUS_NAME, &unique)` and stores in `ed->expected_sender`. Fixes the expected_sender security bug (was using well-known name `IP_DBUS_NAME` instead of unique name like `:1.42`).
   - `cbx_profile_editor_begin_capture()`: Uses `ed->expected_sender` instead of `IP_DBUS_NAME`.
   - `cbx_profile_editor_activate()`: In LIST mode, opens a binding edit sub-menu (`CBX_EDITOR_MODE_BINDING_EDIT`) with "Pick Target", "Capture", "Sequential (All Buttons)" options. In BINDING_EDIT mode, dispatches to the selected action.
   - `cbx_profile_editor_cancel()`: Handles BINDING_EDIT mode (returns to LIST).
   - `cbx_profile_editor_move_up/down()`: Handle BINDING_EDIT mode (scroll target_list).
   - `cbx_profile_editor_init()`: Widget positions now offset by panel rect origin (`px + x`, `py + y`) so editor works correctly within the manager's panel (which starts below the tabbar).

3. **`src/manager/profile_editor_seq.c`** — `cbx_profile_editor_begin_sequential()` uses `ed->expected_sender` instead of `IP_DBUS_NAME`.

4. **`src/manager/profiles_tab.h`** — Added `CBX_PT_MODE_EDITOR` mode, `cbx_profile_editor editor` field, `editor_initialized`, `editor_is_new`, `editor_profile_name`, `renderer`, `dbus_backend`, `dbus_bus` fields. Added `cbx_profiles_tab_set_context()` declaration.

5. **`src/manager/profiles_tab.c`** —
   - `cbx_profiles_tab_set_context()`: Stores renderer and DBus backend/bus for editor init.
   - `on_edit_pressed()`: Loads selected profile from disk, opens editor via `cbx_profiles_tab_open_editor()`.
   - `cbx_profiles_tab_name_input_confirm()`: Now builds in-memory profile and opens editor instead of writing a file. File is written on save from the editor.
   - `cbx_profiles_tab_open_editor()`: Lazy-inits editor, loads profile, sets DBus info, hides tab widgets, sets mode to EDITOR.
   - `cbx_profiles_tab_close_editor()`: Hides editor widgets, shows tab widgets, sets mode to LIST.
   - `cbx_profiles_tab_save_editor()`: Saves via `cbx_profile_save_to_dir()` with NES validation; on success closes editor + refreshes list; on failure shows error in editor status label.
   - `cbx_profiles_tab_handle_key()`: Handles EDITOR mode — Up/Down → editor navigation, A/B → swallow KEYDOWN, Start (TAB) → cancel editor/sequential.
   - `cbx_profiles_tab_activate()`: Handles EDITOR mode → `cbx_profile_editor_activate()`.
   - `cbx_profiles_tab_cancel()`: Handles EDITOR mode — B in LIST → save+close, B in SEQUENTIAL → skip, B in sub-modes → cancel sub-mode.
   - `cbx_profiles_tab_shutdown()`: Shuts down editor if initialized.

6. **`src/manager/manager.c`** — Calls `cbx_profiles_tab_set_context()` after init. Mode tracking updated to `pt.mode * 100 + editor.mode` so manager detects editor internal mode changes and rebuilds focus chain.

### Test changes

- **test_profiles_tab.c**: `init_tab` now calls `set_context` with renderer. `test_name_input_confirm` verifies editor opens then saves via cancel. `test_name_input_via_dispatch` updated for create-to-editor flow.
- **test_editor_list_mode.c**: Updated 6 tests for binding edit sub-menu (two `activate` calls to reach target pick). Added 3 expected_sender security tests: `test_expected_sender_resolved`, `test_expected_sender_accepts_match`, `test_expected_sender_rejects_mismatch`.
- **test_manager_production.c**: Added `test_editor_opens_via_dispatch` — writes NES profile, switches to Profiles tab, clicks Edit button, verifies editor is open with 6 bindings.
- **test_manager_visual.c**: Tests 7-9 use production Edit-button path via `vis_open_editor()` helper; render via `cbx_manager_render`; dynamic rect lookup for region checks. Removed unused `render_editor_panel`.
- **test_golden.c**: Tests 9-11 use production Edit-button path via `g_open_editor()` helper. Golden images regenerated.
- **Golden images**: `manager_editor_list.png`, `manager_editor_sequential.png`, `manager_editor_validation_error.png` regenerated.

### Docs
- `docs/OPERATIONS.md`: Added "Profile editor" section documenting access flow (Edit button, create-to-editor, list mode, sequential mode, capture, validation, save, expected_sender).

## Test results
74/74 pass (1 skip: backend_smoke). No regressions.

## Commit
`163f33e` on `develop`

## Next task
Task 6: Wire overlay DBus InputEvent signal handling for multi-controller input. No dependencies. Ready to start.