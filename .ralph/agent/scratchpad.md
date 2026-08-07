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
### Final review cycle (iteration 3)

Two read-only subagents reviewed the plan and found three actionable gaps:

1. **§11 performance targets** — no dedicated REQ entry. Added REQ-032,
   classified `verified` (architectural guarantees enforced by design:
   pre-built surface, no inline DBus, single InterceptMode set, SDL2 footprint,
   GamepadOrder atomic setter).

2. **Host Mode profile cycling (§4.4)** — inventory had no O12 entry for
   the host cycling a profile on the selected row. Added O12 marked
   "not-yet-implemented" with remediation path via Task 11.

3. **Controller icon overrides (§5.5)** — spec lists "controller icon
   overrides (§8.4)" as an app-level setting; config_settings.c has the
   API (`cbx_settings_set_icon_override`/get/remove) and data model
   (`icon_overrides[]`), but settings_tab.h has no `CBX_ST_SET_ICON_OVERRIDE`
   enum and the settings tab UI doesn't expose it. Updated REQ-021 evidence
   to note this gap, updated M23 to include "icon-override", added Task 14
   resolution requirement (implement or defer per §13).

4. **Task 6 dependency issue** — Task 6 claimed production-poll-loop testing
   but the step function arrives in Task 10 (which depends on Task 6). Softened
   Task 6's acceptance criteria: direct `ip_input_events_handle()` testing is
   sufficient; full production-poll-loop verification deferred to Task 11.

Also updated base_commit from 4b96eb9 to c99e137 (current HEAD).

Plan is now complete: 14 tasks, all pending, 32 conformance requirements,
38 manager controls + 12 overlay actions, 8 scenarios, final audit depends
on all tasks, remediation rule present.

### Review iteration 4 — fixes applied

Three read-only subagents reviewed the plan (reviewer, security-reviewer, planner-scout).

**Blocking fixes:**
1. B1 (reviewer): List widget on_select/tab-activate double-action. Fixed by
   changing list to fire on_select on KEYUP (not KEYDOWN) for RETURN/SPACE/A,
   matching button press/release semantics. Manager tab-activate forwarding
   fires only when focused widget doesn't consume KEYUP. All activation lists
   (settings, type picker, create picker) have on_select wired. test_widget_list.c
   updated for KEYUP firing.
2. B-2 (security): Profile editor expected_sender uses well-known name not unique
   bus name → all InputEvent signals silently dropped in production. Added
   explicit acceptance criterion to Task 5: must use ip_connection_get_unique_name().
3. B-1 (security)/H2 (reviewer): Task 4 TOCTOU fix insufficient guidance. Added
   explicit sub-task: verify_path_within_dir returns canonicalized path, save
   uses it for write. Added grep verification for direct cbx_profile_save bypass.

**High fixes:**
4. REQ-021 icon override UI: Changed from "human-approved deferral" to
   "deferred per §13" (interface details for compound settings not in v1).
5. H-1 (security): Direct cbx_profile_save bypass in profiles_tab.c:403
   explicitly called out in Task 4 with grep verification.
6. Task 14 §11.2 coverage: Added explicit acceptance criteria for items 2
   (production-path audit), 4 (visual and degraded-state), 5 (sanitizer clause),
   and human visual acceptance gate acknowledgment.

**Medium fixes:**
7. M37 pointer path: Changed from "Mouse click on close area" to "n/a
   (controller-only — editor save/close via B key)".
8. Task 12: Added Task 1 as explicit dependency (was transitive only).
9. Task 6: Added device path validation acceptance criterion (InputEvent
   signals only from known composite device paths).

Plan committed as e18263f. 14 tasks, 32 REQs, 38 M-controls, 12 O-actions,
8 scenarios. All blocking issues resolved. Ready for PLAN_COMPLETE.

### Plan completion — iteration 5

Verified plan consistency before PLAN_COMPLETE:
- 14 tasks, all `pending` status
- 32 REQs, 38 M-controls, 12 O-actions, 8 scenarios
- Task 14 depends on Tasks 1-13 (all)
- Remediation rule present
- Spec committed and clean (no working-tree changes to docs/SPEC.md)
- Plan committed at e18263f, HEAD d944caa
- All 4 review iteration fixes applied

No further changes needed. Emitting PLAN_COMPLETE.
