
## Maintenance Planning — BUG-0001 (2026-08-06)

**Selected bug:** BUG-0001 — Manager launches with an effectively blank interface (critical, triaged)

**Root cause:** `src/app/main.c:77` passes `NULL` as font_path to `cbx_manager_init()`. No font is loaded (`font_id = -1`). All `cbx_text_render()` calls silently return NULL. UI shows structural rectangles but no text.

**Key findings from read-only subagents:**
- No font files bundled in repo; no font install rule in CMakeLists.txt
- No font path constant in config.h.in (DATA_DIR and ICON_DIR exist, but no FONT_DIR)
- Tests have compile-time font discovery (CBX_FONT_PATH) but production binary has none
- manager.c treats missing font as "non-fatal" — silent degradation, blank UI
- Alpha-blending is NOT the bug — properly initialized and verified
- contract_change: false confirmed — spec mentions text rendering as concept but doesn't specify font paths

**Front matter values:**
- bug_fingerprint: f54e2df5cbb1e2fac06b74e3c607f458b19321a705ccbd9c7e0686136df4c191
- spec_commit: 12f82db38f999986de4216dfc50a6e13452db4c9
- spec_blob: 0522f7f1aa79d70b343ed6022956683a7c11695f
- base_commit: 2a8fc0ae27b49e557fcd06e198e2c1de3326918e

**Plan structure:** 4 tasks
1. Runtime font discovery (config_paths.c/h, config.h.in, CMakeLists.txt)
2. Wire font discovery into main.c + actionable failure handling
3. Automated regression test (test_font_init.c)
4. Maintenance verification and documentation audit

**Status:** MAINTENANCE_PLAN.md written. Parser validation passed. Ready for completion.

**Iteration 3 update:** Fixed parser error — removed stray `## Tasks` heading (line 75) that matched `## Task` prefix but failed the full task-header regex. Committed as 4d2ff57. `validate-maintenance-plan.py planning` now passes: "valid (4 tasks, status=active)". Emitted `factory.maintenance.plan` completion event.
