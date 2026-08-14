# Planning Scratchpad

## Status
Plan written to `.factory/artifacts/implementation-plan.md`, reviewed by read-only reviewer, and updated with findings.

## Plan summary
12 tasks + final audit covering all spec gaps identified by 5 parallel subagent audits:
1. BUG-0004 (POSIX declarations, Nix gate, close bug)
2. sd_set_property boolean variant + ManageAllDevices wrapper
3. Operational readiness property validation
4. Settings tab icon overrides
5. Unsaved-changes prompt on window close
6. Host Mode visual state rendering
7. Fix interaction tests to use SDL_CONTROLLERBUTTONDOWN
8. Performance/latency tests
9. Installed functional test: run overlay service
10. Font/icon visual tests + weakened assertion fixes
11. Interaction inventory verification status
12. Final documentation and specification audit

## Review findings addressed
- Added §3, §11 gameplay/footprint/reorder, §5.4 profile-scope, §9.1 Flathub rows to matrix
- Downgraded R-4.3/R-4.5/R-4.6/R-5.1/R-5.2 to `partial` (keyboard not controller events)
- Fixed M06/M07 swapped outcomes
- Added Task 5→7, Task 11→5,6 dependencies
- Strengthened BUG-0004 remote-runner acceptance
- Added pointer-path NA review to Task 11

## Next action
Publish `factory.plan` summary and emit `PLAN_COMPLETE`.