# Task 10 Complete — Extract overlay service step function for testability

## What was done

### Production refactoring

1. **Created `cbx_overlay_service_ctx` struct** in `overlay_service.h` — a
   comprehensive context holding all overlay-service loop state: renderer,
   connection, device model, settings, assignments, text cache, font_id,
   theme, icon map, icon cache, composites array, comp_count, grid,
   surface, render_ctx, lifecycle, player_mode, host_mode, conflicts,
   input_ctx, input_events, expected_sender, input_events_ready, polls
   array, poll_count, poll_event_type, and initialized flag. The struct
   is ~200 KB+ so it's heap-allocated (`calloc`) in production.

2. **Extracted `cbx_overlay_service_step()`** — the single-iteration poll
   loop body. Processes SDL events (poll-timer ticks, SDL_QUIT, keyboard
   navigation via player/host mode), DBus InputEvent signals, lifecycle
   ticks, and dirty-surface re-renders. Takes `cbx_overlay_service_ctx *`
   and is fully self-contained.

3. **Refactored `run_overlay_service()`** — initializes the context struct,
   then loops `while (g_running) { cbx_overlay_service_step(svc); SDL_Delay(10); }`,
   then cleans up. All stack-local variables replaced by struct members.

4. **Updated all callbacks** (`on_overlay_save`, `on_slot_change`,
   `on_profile_change`, `on_host_slot_change`) to use
   `cbx_overlay_service_ctx *` instead of the old `overlay_ctx *`.
   The `overlay_ctx` typedef was removed.

5. **Restructured `overlay_service.h`** — moved `cbx_overlay_input_ctx`
   definition before `cbx_overlay_service_ctx` (dependency order).

### Test file: `tests/test_overlay_service.c` — 3 new tests (11 total)

- **test_step_quit_sets_shutdown**: Queue SDL_QUIT, run one step, verify
  `cbx_overlay_service_shutdown_requested()` returns true.
- **test_step_keydown_updates_grid**: Queue SDLK_RIGHT, run one step, verify
  row 0's cur_col changes from 0 (Unassigned) to 1 (P1).
- **test_step_empty_queue_no_crash**: Flush events, run one step, verify no
  crash, grid unchanged, no shutdown.

Step fixture (`step_fixture`) allocates a `cbx_overlay_service_ctx` on the
heap, initializes SDL with dummy driver, creates a 1-composite grid with
player/host mode, sets lifecycle to VISIBLE so keydown events are processed,
and tears down cleanly.

### Key implementation insights

- The `cbx_overlay_service_ctx` struct must be heap-allocated (not stack)
  because it contains large arrays (device_model ~53KB, text_cache ~145KB,
  icon_cache ~37KB) totaling ~200KB+.
- The step function uses the file-scope `g_running` flag for SDL_QUIT.
  Tests use `cbx_overlay_service_reset_shutdown()` /
  `cbx_overlay_service_shutdown_requested()` to control and check it.
- The `cbx_overlay_input_ctx` typedef must be defined BEFORE
  `cbx_overlay_service_ctx` in the header since the latter contains it.
- The step function does NOT call `SDL_Delay()` — that stays in the
  `run_overlay_service()` loop wrapper so tests can run the step without
  delays.

### Test results
77/77 pass (1 skip: backend_smoke). No regressions. 3 new sub-tests.

### Commits
- `e172c01` on `develop`

## Next task
Task 11: Overlay production-dispatch interaction tests.
Dependencies: Task 6, Task 7, Task 10. Ready to start.