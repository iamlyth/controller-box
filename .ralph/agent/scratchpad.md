# Task 12 — Enhance installed smoke test with coordinate-based mouse clicks

## Status
- Task 11 complete (commit `0a2f6e9` on develop)
- Task 12 started: `task-1785997610-263b` (key `spec:task-12`)
- Emitted `factory.implement` event to delegate to Factory Worker

## Task 12 scope
- File: `tests/test_installed_smoke.sh`
- Add coordinate-based mouse clicks on manager body controls (settings list item, Save button, profiles tab control)
- Verify visible state change or file mutation from clicks
- Preserve existing keyboard tab switching and controller-proxy navigation
- Skip with exit 77 when Xvfb/xdotool/ImageMagick unavailable
- Do not bypass production initialization — launch real installed binary
- Update `docs/OPERATIONS.md` installed smoke test description
- Verify: `nix-shell --run "ctest --test-dir build-check -R test_installed_smoke --output-on-failure"` and `nix-shell --run "./scripts/verify-project.sh"`

## Next
- Factory Worker implements Task 12
- Remaining: Task 13 (BUG-0002 fix), Task 14 (final audit)