# Task 3 Complete — Wire tab-specific activation, widget A-key handling, and focus-chain rebuild through manager event dispatch

## What was done

### Widget changes
1. **`widget_button.c`**: Added `SDLK_a` to KEYDOWN (sets pressed) and KEYUP (fires `on_press`) — same press/release semantics as `SDLK_RETURN`/`SDLK_SPACE`.
2. **`widget_list.c`**: Changed `on_select` to fire on **KEYUP** for `SDLK_RETURN`/`SDLK_SPACE`/`SDLK_a` (KEYDOWN sets `pressed` visual state, KEYUP fires callback). Mouse clicks fire `on_select` on **MOUSEUP** (matching KEYUP semantics). List without `on_select` returns `false` on KEYUP (lets manager forward A to tab-activate). Added `bool pressed` field. Blur clears pressed state.
3. **`widget.h`**: Added `bool pressed` to `cbx_list`, `bool interactive` to `cbx_widget`. Labels/images/progress set `interactive=false` (excluded from focus chain). All other widgets set `interactive=true`.

### Manager changes (`manager.c`)
1. Added `cbx_manager_tab_handle_key()` — intercepts modal-mode keys before the focused widget (B cancel, letter keys in name input, Up/Down in settings edit mode).
2. Added KEYUP handling for `SDLK_a`/`SDLK_RETURN`/`SDLK_SPACE` → forwards to active tab's activate function when focused widget doesn't consume the KEYUP.
3. Added B/Escape KEYUP forwarding to tab-cancel function.
4. Added `cbx_manager_check_mode_change()` — tracks tab mode before/after each event, rebuilds focus chain (skipping invisible and non-interactive widgets) and focuses the first interactive panel child when mode changes. Applied to both keyboard and mouse event paths.
5. Focus chain rebuild (`cbx_manager_rebuild_focus`) skips invisible AND non-interactive widgets.

### Tab changes
1. **Controllers tab**: Added `cbx_controllers_tab_activate()` (confirms type pick), `cbx_controllers_tab_cancel()` (cancels type pick), `cbx_controllers_tab_handle_key()` (B cancel). Wired type_picker `on_select` to `on_type_pick_selected` (syncs `selected_type` from list, calls `confirm_type_pick`).
2. **Profiles tab**: Added `cbx_profiles_tab_activate()` (confirms name input, delete, create source), `cbx_profiles_tab_cancel()`, `cbx_profiles_tab_handle_key()` (letter keys, backspace, A/B in name input; A/B in confirm delete; B in create pick). Changed Create button to open create source picker (3 options) instead of hardcoding `DEFAULT_COPY`. Wired create_picker `on_select` to `on_create_source_selected`. Synced `selected_profile` from list in `on_delete_pressed`.
3. **Settings tab**: Wired settings_list `on_select` to `on_setting_selected` (calls `cbx_settings_tab_activate`). Added `cbx_settings_tab_handle_key()` (Up/Down in edit mode, B cancel).

### Tests added
- **test_widget_list.c**: Updated select_callback test for KEYUP firing; added SDLK_a test; added mouse MOUSEUP test; added no-select-returns-false-on-KEYUP test; updated unrelated-event test.
- **test_widgets.c**: Added SDLK_a button key press test; updated unrelated-event test.
- **test_controllers_tab.c**: `test_type_pick_via_dispatch`, `test_type_pick_confirm_via_dispatch` (production dispatch via mouse click + keyboard through `cbx_manager_handle_event`).
- **test_profiles_tab.c**: `test_create_picker_via_dispatch`, `test_name_input_via_dispatch`, `test_confirm_delete_via_dispatch`.
- **test_settings_tab.c**: `test_settings_activate_via_dispatch`, `test_settings_edit_via_dispatch`, `test_settings_edit_cancel_via_dispatch`.

## Test results
84/84 pass (1 skip: backend_smoke). No regressions.

## Commit
`bc8c7b3` on `develop`

## Next task
Task 4: Security-hardened profile save in production path. Dependencies: Task 3 (complete). Ready to start.