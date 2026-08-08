# Task 8 Complete — Manager interaction tests: Controllers and Settings tabs

## What was done

### Production fix: Focus chain button reachability

Side-by-side buttons (Add/Remove/ChangeType, Create/Edit/Delete) were
unreachable via the controller focus chain because spatial DOWN from
the list went to the horizontally-closest button, and there was no way
to navigate between same-y-level buttons (LEFT/RIGHT always switched
tabs). Fixed in 3 files:

1. **`src/manager/manager.c` — `cbx_manager_rebuild_focus`**: Panel
   children are now grouped by y proximity (within 10px center-to-center)
   into the same row, instead of each child getting a unique sequential
   row. Side-by-side buttons at the same y share a row.

2. **`src/ui/focus.c` — `find_neighbor`**: In HOST mode, LEFT/RIGHT
   navigation is restricted to same-row widgets. UP/DOWN remains
   unrestricted (can cross rows). PLAYER mode unchanged.

3. **`src/manager/manager.c` — `cbx_manager_handle_event`**: For
   SDLK_LEFT/RIGHT, if the focused widget is NOT the tabbar, try focus
   chain horizontal navigation first. If no same-row neighbor exists,
   fall back to tab switching. On the tabbar, LEFT/RIGHT always switches
   tabs (existing behavior preserved).

### Source changes

- **`tests/test_manager_interaction_ctrl.c`** — 28 sub-tests:
  - M01–M03: Tab switching (controller LEFT/RIGHT + pointer click)
  - M04: Device list selection (controller DOWN/UP + pointer click)
  - M05+M08: Add flow — open type picker, confirm → CreateTargetDevice (both paths)
  - M06: Remove → StopTargetDevice, device count decreases (both paths)
  - M07+M08: Change Type — open picker, confirm → SetTargetDevices (both paths)
  - M09: Type picker cancel via B (controller only, pointer NA)
  - M21: Settings list selection (both paths)
  - M22: Toggle launch_at_boot (both paths)
  - M23+M24+M25: Edit flow — enter, cycle, confirm (controller); enter via click (pointer)
  - M26/D05: Cancel edit — value reverts (controller + pointer entry)
  - M27: Save — settings.yaml written (both paths)
  - D01: InputPlumber unavailable — Add rejected (both paths)
  - D02: Remove no device — no side effect (both paths)
  - D06: DBus failure — mode returns to LIST, no corruption (both paths)

- **`tests/CMakeLists.txt`** — Added `test_manager_interaction_ctrl` executable
  and ctest registration with `SDL_VIDEODRIVER=dummy` env.

- **`IMPLEMENTATION_PLAN.md`** — Task 8 status → complete. REQ-017, REQ-018,
  REQ-021 → verified. REQ-023 → partial (Controllers+Settings done, Profiles+Editor
  pending Task 9).

### Test results
76/76 pass (1 skip: backend_smoke). No regressions. 28 new sub-tests in
test_manager_interaction_ctrl.

### Key implementation insights

- `cbx_settings_tab_selected(st)` (st->selected) is only updated by
  `on_setting_selected` callback (fires on A key/click). For UP/DOWN
  list navigation, use `cbx_list_get_selected(&st->settings_list)`.
- Settings list pointer clicks must use `rect.y + index * item_h + item_h/2`
  for y-coordinate, not the list center y.
- After `nav_to_buttons` (2 DOWNs), focus lands on the spatially closest
  button (change_type_btn, rightmost). Use LEFT to navigate to the
  desired button (2-index LEFTs).
- `cbx_settings_tab_settings(st)->theme` returns a pointer to the internal
  buffer — copy it before modifying to avoid aliasing.
- Mock DBus: `ip_dbus_mock_reset` clears expectations but preserves the
  bus handle. Reset between init and action to set fresh expectations
  for post-action refresh calls.

### Commits
- `649b59c` on `develop` — Test file + production fix
- `7a18bea` on `develop` — Plan conformance matrix update

## Next task
Task 9: Manager interaction tests — Profiles tab and profile editor through
production dispatch. Dependencies: Task 1, 2, 3, 4, 5, 7 (all complete).
Ready to start.