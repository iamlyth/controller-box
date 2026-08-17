# Task 6: Fix golden comparison silent skip in backend smoke SW

## Outcome
- Replaced `printf("[SKIP]")` with `fprintf(stderr, "[FAIL]...") + goto cleanup` at both golden comparison sites in `tests/test_backend_smoke_sw.c`:
  - Line ~423: overlay_player_mode.png golden comparison
  - Line ~640: manager_controllers_degraded.png golden comparison
- When a golden baseline file is missing, the test now fails with a clear diagnostic identifying the missing path
- Updated "Best-effort" comments to reflect mandatory comparison (file header + both inline comments)
- Updated conformance matrix: VRF-03 partial→verified, DOD-05 gap reduced (Task 6 resolved, remaining: Tasks 9, 10 hardware)

## Verification
- `nix-shell --run 'ctest --test-dir build-check -R "backend_smoke_sw" --output-on-failure'` → 1/1 pass (0.04s) with existing baselines
- Temporarily removed `tests/golden/overlay_player_mode.png` → test FAILED with diagnostic: `[FAIL] overlay: golden baseline not found at /workspace/project/tests/golden/overlay_player_mode.png`
- `nix-shell --run 'ctest --test-dir build-check --output-on-failure'` → 98/98 pass (2 hardware skips)

## Commit
- `ea19af3`: Task 6: Fix golden comparison silent skip in backend smoke SW

## Next Task
- Task 7: Wire first-run service installation into manager UI (§9.1, §9.3)