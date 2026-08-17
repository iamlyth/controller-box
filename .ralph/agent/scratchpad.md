# Task 7: Wire first-run service installation into manager UI (§9.1, §9.3)

## Outcome
- Added `cbx_manager_check_first_run()` to `manager.c` — detects first-run (no `controller-box.service` in `~/.config/systemd/user/`) and shows modal "Enable overlay service?" dialog with Yes/No buttons
- Dialog is modal: intercepts all keyboard/mouse events via `cbx_manager_handle_first_run_event()` when `first_run_active` is true
- Controller A/Enter/Space → `on_first_run_yes` → `cbx_service_install()` → writes unit file + `systemctl --user enable --now`
- Controller B/Escape → `on_first_run_no` → dismiss dialog
- Mouse click on Yes/No buttons → same outcomes via button `on_press` callbacks
- Called from `cbx_manager_run()` (production event loop); tests call explicitly
- SDL_QUIT, SDL_WINDOWEVENT, controller hotplug pass through normally
- Dialog rendering: semi-transparent overlay + dialog box + label + buttons
- Updated `test_installed_smoke.sh` and `test_installed_binary.sh` to pre-create service file (suppress dialog in installed tests)
- Updated conformance matrix: PKG-03 partial→verified, M39 missing→verified
- Updated README.md first-launch description

## Verification
- `nix-shell --run 'ctest --test-dir build-check -R "service_install|interaction_ctrl" --output-on-failure'` → both pass (0.11s + 0.66s)
- 4 new tests: `test_first_run_{confirm,cancel}_{controller,pointer}_path` — all dispatch through `cbx_manager_handle_event` with mocked systemctl
- `nix-shell --run 'ctest --test-dir build-check --output-on-failure'` → 98/98 pass (2 hardware skips)

## Commit
- `7a16ff6`: Task 7: Wire first-run service installation into manager UI (§9.1, §9.3)

## Next Task
- Task 8: Prepare kernel-backed controller test code (software)