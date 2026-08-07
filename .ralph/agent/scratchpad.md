# Task 1 Complete — List focus boundary fix

## What was done
`list_handle_event` in `src/ui/widget_list.c` returned `true` for `SDLK_UP`/`SDLK_DOWN` even at list boundaries, trapping focus and preventing `cbx_focus_chain_navigate` from moving to adjacent widgets. Fixed: returns `false` when `selected==0` (UP) or `selected==item_count-1` (DOWN), letting the manager fall through to focus-chain navigation. Mid-list navigation still returns `true`.

## Tests added
- `test_list_boundary_returns_false` — 3-item list: false at top/bottom, true mid-list
- `test_list_single_item_boundary` — single item: both UP/DOWN return false
- `test_list_empty_boundary` — empty list: both return false
- `test_manager_focus_traversal_no_trap` — full tabbar→list→buttons→tabbar traversal through `cbx_manager_handle_event` on Controllers and Settings tabs

## Test results
74/74 pass (1 skip: backend_smoke). No regressions.

## Commit
`dfcea93` on `develop`

## Next task
Task 2: Add manager pointer event routing, mouse hit-testing, and visibility filtering. Dependencies: Task 1 (complete). Ready to start.