# Planning Scratchpad — Controller-Box

## Iteration 1: Initial Plan Generation

### Context
- Greenfield C/C++ project — no source code exists
- Spec: `docs/SPEC.md` (583 lines, committed at 12f82db)
- Repository has only factory boilerplate (scripts, tests, configs)
- `IMPLEMENTATION_PLAN.md` is a placeholder with "needs-planning" status

### Subagent Analysis (4 parallel)
1. **planner-scout**: Mapped 9 implementation areas with 72 sub-tasks, dependency DAG, source tree layout
2. **researcher**: Detailed SDL2 widget patterns, sd-bus API, nanosvg→SDL2 texture pipeline, testing strategy (cmocka + SDL2 dummy driver + CTest)
3. **security-reviewer**: 16 findings — HIGH: polkit auth gap, temp file TOCTOU, DBus signal spoofing, input injection; MEDIUM: path traversal, systemd unit injection, Flatpak permissions, YAML validation
4. **docs-reviewer**: README + OPERATIONS need full rewrite; 3 new docs needed (DBus-API.md, PROFILES.md, PACKAGING.md); 14 documentation sync points identified

### Key Decisions
- **Language: C (not C++)** — all deps (SDL2, sd-bus, nanosvg) are C; spec emphasizes minimal footprint for Pi 4; use libyaml for YAML, function-pointer vtables for widgets. Confidence: 85.
- **Test framework: cmocka** — lightweight, C-native, CTest-integrated, used by systemd ecosystem.
- **InterceptMode poll interval: 50ms** (not 500ms) — spec says <10ms from button press, but 500ms poll means worst case 500ms. Recommend 50ms poll to reduce detection latency to acceptable levels. Document as known tension with spec §11.
- **DBus sender verification mandatory** — all signal handlers must verify sender unique name matches InputPlumber's known unique bus name.

### Plan Structure
44 tasks across 10 phases, dependency-ordered, each sized for one fresh implementation context. Security mitigations embedded as acceptance criteria within relevant tasks. Documentation split across tasks with final audit gate.

### Review Iteration
Launched 2 review subagents (reviewer + docs-reviewer):
- docs-reviewer: Format passes all 7 checks (front matter, task shape, status values, final task, completion gates, numbering, internal references). No violations.
- reviewer: Found 17 issues (6 high, 5 medium, 6 low). All high and medium issues fixed:
  - Added device config/capability map enumeration to Task 8 (gap #4)
  - Added DbusDevices/OutputCapabilities/TargetCapabilities to Task 13
  - Added O_NOFOLLOW/realpath()/filename validation to Tasks 8, 36, 39
  - Added YAML parser hardening to Tasks 5, 6
  - Added InterceptMode poll interval spec tension note to architecture section
  - Fixed Task 24 scope (infrastructure only, content deferred to Task 29)
  - Added Task 11 dependency to Tasks 15, 27
  - Clarified Task 15 (persistence) vs Task 27 (restoration orchestration) split
  - Added virtual device capabilities loading to Task 37
  - Added sidecar 0600 permission to Task 8
  - Added AccessDenied/ServiceUnknown user-facing handling to Task 9
  - Clarified Settings tab icon overrides in Task 39
  - Tightened vague acceptance criteria in Tasks 16, 24, 33
  - Noted deferred items (ForceFeedback, ManageAllDevices, SendEvent) in constraints
  - Added 'no user names/colors' to Task 29 per §4.8

### Plan is complete and internally consistent.