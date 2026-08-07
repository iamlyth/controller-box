# Scratchpad — Interaction Acceptance and Production Dispatch

## Planning analysis (fresh cycle)

### Prior cycle state
The prior planning cycle (11 tasks) completed framebuffer visual acceptance:
- Tasks 1–11: fb_assert infrastructure, conflict red rendering, overlay service
  init + poll loop, overlay/manager visual tests, golden images, backend smoke,
  installed smoke, final audit
- 74/74 tests pass, verify-project.sh passes, plan status: complete

### This cycle's gap analysis

Three parallel subagents mapped the codebase for interaction acceptance gaps:

**Manager interaction tests (Explore agent):**
- No test sends both controller-path AND pointer-path SDL events through
  `cbx_manager_handle_event` for the same control
- All tab-specific tests bypass production dispatch (direct function calls)
- Only tab switching is tested through production dispatch
- No interaction inventory exists
- Manager `handle_event` only handles SDL_KEYDOWN, no mouse events

**Overlay interaction (Explore agent):**
- All overlay tests use direct function calls, not production poll loop
- Multi-controller input hardcoded to row 0 (keyboard proxy)
- DBus InputEvent infrastructure exists (ip_input_signal.c) but not wired
- No test exercises the full overlay lifecycle through production dispatch

**Manager DBus and pointer wiring (Explore agent):**
- SDL_MOUSEMOTION not processed anywhere in src/
- Mouse events only reach focused widget, no global hit-testing
- List widget traps UP/DOWN (always returns true) — buttons unreachable
- Type picker on_select is NULL — confirm never called from event path
- Profiles tab edit button is a no-op placeholder
- Settings tab activate/edit functions never called from event path
- Security-hardened save (realpath + NES validation) never called from production

### Review findings (3 review subagents)

**Blocking:**
- SDLK_a not handled by button/list widgets — only SDLK_RETURN/SDLK_SPACE

**High:**
- Task 2 & 3 both modify cbx_manager_handle_event — need dependency
- Focus chain includes hidden widgets, no rebuild on mode change
- Create source picker missing (spec §5.3 requires Default copy / Empty / Clone)
- Capture mode entry point missing
- Sequential mode entry point missing
- Save control in editor missing

**Medium:**
- Task 8 too large — split by tab
- Task 9 combines refactoring + testing — split
- Task 4/5 undeclared dependency
- Profile create → open editor not implemented (spec requires it)
- Prior editor visual tests violate §5.6 (manually initialize editor)
- No operation-failure recovery scenario
- Host-mode profile cycling not implemented (spec §4.4 says host can edit profile)
- §3 System Requirements missing from matrix
- REQ-028 evidence contradictory
- REQ-029 doesn't list item 2 (production-path behavior)

### Plan structure (14 tasks)

1. Fix list focus trapping (no deps)
2. Manager pointer routing + visibility (deps: 1)
3. Wire tab activation + widget A-key + focus rebuild (deps: 1, 2)
4. Security-hardened profile save (deps: 3)
5. Wire profile editor + create-to-editor + entry points (deps: 3, 4)
6. Overlay DBus InputEvent handling (no deps)
7. Interaction inventory (deps: 1, 2, 3)
8. Manager interaction tests — Controllers + Settings (deps: 1, 2, 3, 7)
9. Manager interaction tests — Profiles + Editor (deps: 1, 2, 3, 4, 5, 7)
10. Overlay step function extraction (deps: 6)
11. Overlay interaction tests (deps: 6, 7, 10)
12. Enhanced installed smoke (deps: 2, 3)
13. Fix BUG-0002 (no deps)
14. Final audit (deps: all)

### Key decisions
- Split Task 8 into two (Controllers+Settings vs Profiles+Editor) for context limits
- Split Task 9 into two (step function extraction vs interaction tests) for
  production refactoring safety
- Made Task 3 depend on Task 2 (both modify manager handle_event)
- Made Task 5 depend on Task 4 (editor save uses security-hardened path)
- Added create-to-editor flow to Task 5 (spec requires create → open editor)
- Added widget SDLK_a handling to Task 3 (blocking fix for controller activation)
- Added focus-chain rebuild on mode change to Task 3
- Added visibility filtering to Task 2 (hidden widgets shouldn't consume events)
- Added create source picker to Task 3 (spec §5.3)
- Added capture/sequential entry points + save control to Task 5
- Updated prior visual tests in Task 5 (fixes §5.6 violation)
- Added operation-failure scenarios D06, D07, D08
- Added §3 to conformance matrix as REQ-006
- Fixed REQ-029 evidence and REQ-030 classification