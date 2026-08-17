# Task 4: Complete missing interaction test paths (§5.7)

## Outcome
- Added `test_ctrl_type_pick_cancel_pointer_path` to `tests/test_manager_interaction_ctrl.c`
  - Opens type picker via mouse click on Change Type button (pointer path)
  - Cancels via ESC key (production dismiss: `cbx_manager_handle_event` → `cbx_controllers_tab_handle_key` → `cbx_controllers_tab_cancel_type_pick`)
  - No cancel button widget exists in type picker mode; ESC is the production cancel mechanism
- M16 name input cancel pointer: marked NOT_APPLICABLE in `tests/interaction_inventory.c`
  - Name input cancel is keyboard-only (B/ESC); no cancel button widget in name input mode
  - Updated `tests/test_interaction_inventory.c` expectations (M16 moved from verified_ids to na_ids, removed from avail_ids)
- VC types slots 1-3: 6 new tests via macro-generated functions
  - `test_settings_vc_type_slot_{1,2,3}_{controller,pointer}_path`
  - Each: navigate to slot → enter edit → cycle type → confirm → save → verify persisted YAML value changed
  - Uses separate test functions (not loop) for fresh fixture isolation (stale list selection issue)
- Updated conformance matrix: MGR-07 partial → verified, DOD-03 gaps reduced

## Verification
- `nix-shell --run 'ctest --test-dir build-check -R "interaction" --output-on-failure'` → 4/4 pass
- `nix-shell --run 'ctest --test-dir build-check --output-on-failure'` → 98/98 pass (2 hardware skips)

## Commit
- `5fcd1ca`: Task 4: Complete missing interaction test paths (M09, M16, VC slots 1-3)

## Next Task
- Task 5: Add overlay DBus InputEvent navigation tests (O02–O09) in `tests/test_overlay_native.c`