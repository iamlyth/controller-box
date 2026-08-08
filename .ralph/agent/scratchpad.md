# Task 9 Complete — Manager interaction tests: Profiles tab and profile editor

## What was done

### Production fixes

Three production fixes in `src/manager/profile_editor_list.c` to enable
pointer-path activation and fix controller navigation in editor sub-modes:

1. **on_select callbacks on editor lists**: `binding_list` and `target_list`
   now have `on_select` callbacks wired to `cbx_profile_editor_activate`.
   Before, clicking on editor list items only selected them visually but
   did not trigger activation — M29, M30, M31, M33 pointer paths were broken.

2. **move_up/move_down selection fix**: In BINDING_EDIT and TARGET_PICK
   modes, `move_up`/`move_down` now change the selected index via
   `cbx_list_set_selected` instead of calling `cbx_list_scroll_up/down`
   (which only changed scroll_offset, not selection). Without this fix,
   the binding edit sub-menu and target picker were unnavigable via
   controller (UP/DOWN did nothing visible).

3. **binding_list item user_data**: Changed from `(void *)(intptr_t)(i+1)`
   to `ed` so the on_select callback can safely call
   `cbx_profile_editor_activate(ed)`.

### Test file: `tests/test_manager_interaction_prof.c` — 35 sub-tests

- **M10–M20 (Profiles tab)**: list select, create button, create source
  picker, name input (chars/backspace/confirm/cancel), edit button, delete
  (open/confirm/cancel) — both controller and pointer paths
- **M28–M38 (Profile editor)**: binding list nav, activate binding,
  target pick confirm, capture begin/event, sequential begin/capture/
  skip/cancel, save and close, cancel/discard — both paths where applicable
- **D03**: Delete with no profile — no file deletion (both paths)
- **D04**: Save with missing NES bindings — error, no file, editor stays open
- **D07**: Filesystem failure — chmod user_dir 0555, save fails, editor stays
- **D08**: Empty profile creation — editor opens, save blocked by NES validation

### Key implementation insights

- In NAME_INPUT mode, `SDLK_a` triggers confirm and `SDLK_b` triggers
  cancel (intercepted by `cbx_profiles_tab_handle_key` on KEYDOWN before
  the letter range check). Use letters c–z for name input tests.
- Name input confirm happens on KEYDOWN, not KEYUP. Use `send_key_dn`
  (not `send_key_press`) to avoid the KEYUP activating the editor binding.
- Editor save (B in LIST mode) happens on KEYUP via `cbx_profiles_tab_cancel`
  → `cbx_profiles_tab_save_editor`. Use `send_key_press` (not `send_key_dn`).
- Sequential skip (B) also happens on KEYUP via tab_cancel → seq_skip.
- Editor cancel (Tab/Start) happens on KEYDOWN via `cbx_profiles_tab_handle_key`.
- `cbx_profile_save_to_dir` uses atomic write (mkstemp + rename), so making
  the file read-only doesn't prevent save. Make the directory read-only
  (chmod 0555) to prevent temp file creation.
- `open_editor_pointer` must select the user profile via pointer click
  before clicking Edit, so `on_edit_pressed` loads the correct profile.

### Test results
76/76 pass (1 skip: backend_smoke). No regressions. 35 new sub-tests.

### Commits
- `56dcf13` on `develop` — Test file + production fixes
- `5dd62b8` on `develop` — Plan conformance matrix update

## Next task
Task 10: Extract overlay service step function for testability.
Dependencies: Task 6 (complete). Ready to start.