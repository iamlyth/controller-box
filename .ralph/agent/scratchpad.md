# Implementation Handoff

## Outcome
Task 3 (Editor unsaved-close prompt, sequential production capture, clone, determinism) complete. Tasks 1-3 closed; 7 plan tasks remain.

## What was done
- **PR-07 (unsaved-close prompt):** Added `dirty` flag to `cbx_profile_editor` (set on target_pick/capture/seq_on_input, cleared on load_profile). SDL_QUIT moved from `cbx_manager_run` to `cbx_manager_handle_event` — checks dirty flag, enters `CBX_PT_MODE_CONFIRM_QUIT` if dirty (A=save&quit, B=discard&quit via `quit_after_action` flag). Tests: `test_quit_unsaved_prompt_appears`, `test_quit_unsaved_save_and_quit`, `test_quit_unsaved_discard_and_quit`, `test_quit_no_changes_immediate`.
- **PE-04 (sequential capture via DBus signal):** `test_editor_seq_capture_dbus_signal` uses `backend->inject_signal` → `input_event_signal_cb` → `ip_input_events_handle` → editor callback → `seq_on_input` → auto-advance (production DBus signal path, not direct callback).
- **PR-02/M15/IA-10 (clone existing):** `test_prof_create_clone_controller` + `_pointer` — navigate create picker to "Clone current", type name, confirm, editor opens with 6 cloned bindings (prod dispatch, both paths).
- **PE-06 (capability-scoped binding):** `test_capability_scoped_binding` — target-pick mode via prod dispatch, target list populated from keyboard/mouse capabilities (virtual device, not physical).
- **PE-07 (determinism/portability):** `test_profile_determinism_load_twice` (load twice→identical mapping state) + `test_profile_portability_same_result` (NES+Start round-trip→same state).
- Updated conformance matrix: PR-02, PR-07, PE-04, PE-06, PE-07, IA-10, M15, M22, M25, M31, M32 all `verified`.
- Updated `docs/OPERATIONS.md` with unsaved prompt, clone, sequential capture, determinism docs.

## Verification
- `ctest -R 'test_manager_interaction_prof|test_profile_save'` → all passed (45 + 16 tests).
- Full suite: 90/90 passed, 1 pre-existing skip (test_backend_smoke).
- No regressions.

## Commit
- af2de81 on `develop`.

## Next task
Task 4: Settings icon override and interaction/visual coverage (pending, no deps).
Tasks 7, 8 also pending with no deps. Task 5 depends on 4. Task 6 depends on 2,3,4,5. Task 9 depends on 7. Task 10 (final audit) depends on all.