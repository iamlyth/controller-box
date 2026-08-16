# Implementation Scratchpad — Controller-Box v1

## Task 7 complete: Add kernel-backed controller test or document environment limitation

- **Commit:** ed67914 on develop
- **What:**
  1. Created `tests/test_kernel_controller.c` — a standalone C binary (no cmocka/libcontrollerbox dependency) that:
     - Opens `/dev/uinput`, creates a virtual gamepad (15 buttons + 8 axes via uinput_setup)
     - If a build-dir arg is provided: forks `test_ip_server` (private DBus), sets up temp HOME with fonts/config, launches installed `controller-box --manager` with `SDL_VIDEODRIVER=dummy`, sends gamepad events (D-pad, A/B/Start) through uinput → kernel evdev → SDL joystick → production event loop, verifies manager survival + settings.yaml existence
     - Exits 77 with diagnostic when `/dev/uinput` is unavailable
  2. Registered in `tests/CMakeLists.txt` as standalone executable with `SKIP_RETURN_CODE 77`
  3. Updated conformance matrix: PERF-01 and MGR-07 reclassified to `partial` with explicit kernel-uinput rationale
  4. Updated README: new verification table row, Known environment limitations section, run command
- **Verification:** `ctest -R test_kernel_controller` → Skipped (exit 77, /dev/uinput not in sandbox). `verify-project.sh` → pass (97 tests, 1 skip). `verify-boilerplate.sh` → pass. `check-docs-sync.sh` → pass.
- **Plan:** Task 7 marked complete with evidence in implementation-plan.md.

## Next task

Task 8: Final documentation and specification audit. Dependencies: Tasks 1-7 (all complete). Update conformance matrix so every normative requirement is verified or has documented environment limitations. Run full §5.7 control inventory through production dispatch. Execute installed end-to-end workflows, visual/degraded-state acceptance, clean-build regression, packaging, and project verification. Launch parallel read-only reviews. Update README and docs. Run verify-boilerplate.sh + final-gate.sh --implementation.