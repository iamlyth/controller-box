# Task 2 Complete — Manager pointer event routing, mouse hit-testing, and visibility filtering

## What was done
1. **`cbx_widget_handle_event`** (`src/ui/widget.c`): Added `!w->visible` check — invisible widgets no longer consume events.
2. **`cbx_widget` struct** (`src/ui/widget.h`): Added `bool hover` field for mouse hover state tracking.
3. **`cbx_manager_handle_event`** (`src/manager/manager.c`): Added routing for `SDL_MOUSEMOTION`, `SDL_MOUSEBUTTONDOWN`, `SDL_MOUSEBUTTONUP` via three new static functions:
   - `cbx_manager_hit_test()` — iterates tabbar + active panel's visible children, returns widget under cursor using `SDL_PointInRect`.
   - `cbx_manager_update_hover()` — clears hover on all widgets, sets hover on the widget under cursor (called on MOUSEMOTION).
   - `cbx_manager_handle_mouse_event()` — coordinates hit-test, hover update, focus-on-click, and event dispatch.
4. Updated `test_manager_unrelated_event` to send MOUSEMOTION at empty-space coordinates (640,600) instead of (0,0) which now hits the tabbar.
5. Added "Manager input methods" section to `docs/OPERATIONS.md`.

## Tests added (in `tests/test_manager_tabs.c`)
- `test_mouse_click_unfocused_button` — click on unfocused add button → on_press fires, button becomes focused
- `test_mouse_click_tab_switches` — click on tabbar tabs → tab switches (all three tabs tested)
- `test_mouse_click_list_item` — click on settings list items → correct item selected
- `test_invisible_widget_no_click` — make add button invisible, click on its rect → no callback, no focus
- `test_mouse_motion_updates_hover` — MOUSEMOTION over tabbar/button → hover=true; move away → hover=false
- `test_mouse_click_empty_space` — click at (640,600) → not consumed, no tab/focus change

## Test results
74/74 pass (1 skip: backend_smoke). No regressions.

## Commit
`5b19b05` on `develop`

## Next task
Task 3: Wire tab-specific activation, widget A-key handling, and focus-chain rebuild through manager event dispatch. Dependencies: Task 1 (complete), Task 2 (complete). Ready to start.