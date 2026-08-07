# Planning Iteration — Framebuffer Visual Acceptance

## Context
- Spec updated (commit 2f2903d) added §4.10, §5.6, §11.1 requiring framebuffer-backed visual acceptance
- Prior implementation completed 44 tasks (all functional components done)
- Key gaps: no SDL_RenderReadPixels anywhere, no golden images, overlay service run path is stub, no conflict red rendering

## Analysis (3 parallel planner-scout subagents)
1. **Overlay tests**: All 11 overlay test files are structural/state-machine level. Zero pixel readback. grid_render.c has no red conflict rendering. Production render path exercised only for crash-safety (NULL caches, return-code-only).
2. **Manager tests**: 12 test files, all structural. 6/12 manually attach modules (violates §5.6). test_manager_production.c checks child counts only. No pixel readback.
3. **Rendering infra**: test_harness uses SDL software renderer + dummy driver (good foundation). No SDL_RenderReadPixels, no golden images, no failure artifacts, no installed smoke test, no backend smoke.

## Plan decisions
- 11 tasks total, ordered by dependencies
- Task 1: fb_assert infrastructure (foundational)
- Task 2: conflict red rendering (deps: 1, uses fb_region_has_color)
- Task 3-4: overlay service init + poll loop (split for sizing)
- Task 5: overlay visual tests (deps: 1, 2)
- Task 6: manager DBus backend injection (enables connected-mode production-path testing)
- Task 7: manager visual tests (deps: 1, 6)
- Task 8: golden images (deps: 5, 7)
- Task 9: installed smoke test (deps: 4)
- Task 10: backend smoke (deps: 1)
- Task 11: final docs + spec audit (deps: all)

## Review fixes applied
- Fixed cbx_overlay_surface_build → cbx_overlay_surface_init
- Split overlay service into init + poll loop tasks
- Added Task 6 (manager DBus injection) for connected-mode testing
- Task 2 now depends on Task 1 (uses fb_region_has_color for red pixel assertion)
- Added spec text correction (500ms → 50ms) to Task 11
- Clarified composition path as callback-based (cbx_overlay_surface_render with cbx_select_grid_render_cb)
- Added partial progress state testing to Task 7
- Added build-check configuration note
- Clarified installed smoke test region checks with ImageMagick commands

## Review iteration (post-commit)
- Launched 3 parallel read-only subagents (reviewer, researcher, Explore)
- Reviewer found 3 issues: Task 3 verification command broken ($? captures grep not binary), Task 4 verification incomplete (only dry-run), Task 3 borderline sizing (left as-is)
- Researcher confirmed all spec sections (§4.10, §5.6, §11.1) exist and match plan; poll interval correction correctly deferred to Task 11
- Explorer found 1 hard mismatch: ip_objectmanager_get_managed_objects → cbx_objectmanager_enumerate (src/dbus/ip_objectmanager.h:33)
- All other 14 function names verified against codebase
- Applied 3 fixes, committed as 8d4ec15
- Plan is complete, internally consistent, executable one task at a time, tied to committed spec
