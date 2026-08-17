# Task 8: Prepare kernel-backed controller test code (software)

## Outcome
- `test_installed_functional.c` now detects `/dev/uinput` and creates a kernel-backed evdev gamepad when available, falling back to `SDL_JoystickAttachVirtual` when absent
- Added uinput helpers: `uinput_create_gamepad()`, `uinput_destroy()`, `uinput_press()`, `uinput_axis()`, `vbtn_to_evdev()`, `vaxis_to_evdev()`, `find_uinput_joystick_index()`, `add_uinput_gamecontroller_mapping()`
- `f_setup` tries uinput before SDL_Init (so evdev backend detects it), falls back to virtual joystick if `/dev/uinput` unavailable or SDL fails to detect the device
- `ctrl_press`/`ctrl_axis` branch on `g_use_uinput` global — uinput writes EV_KEY/EV_ABS through kernel, virtual uses SDL_JoystickSetVirtualButton/Axis
- Button mapping: virtual idx → evdev code (0→BTN_SOUTH, 1→BTN_EAST, 6→BTN_START, 11-14→DPAD). Runtime gamecontroller mapping normalizes to same SDL_CONTROLLER_BUTTON_* indices
- Axis mapping: virtual idx → evdev code (0→ABS_X, 1→ABS_Y, 2→ABS_RX, 3→ABS_RY, 4→ABS_Z, 5→ABS_RZ). Mapping string handles SDL sorted-axis-index normalization
- Phase 7/13: uinput device persists across manager restarts — no joy2/joy3 re-creation needed
- Phase 10b: macro `send_axis_va` branches between uinput and virtual for direct axis event calls
- Fixed misleading comment at line 9 (was "kernel-backed SDL virtual game controller", now "kernel-backed /dev/uinput when available, SDL virtual joystick fallback otherwise")
- Declared `kernel-uinput` capability in `.factory/environment.toml` (evidence pending Task 9)
- Updated conformance matrix: VRF-05 evidence updated, DOD-03 progress noted

## Verification
- `nix-shell --run 'ctest --test-dir build-check -R "installed_functional" --output-on-failure'` → passes (9.35s, SDL fallback path)
- `nix-shell --run 'ctest --test-dir build-check --output-on-failure'` → 98/98 pass (2 HW skips: test_kernel_controller, test_backend_smoke)
- Code inspection confirms `/dev/uinput` detection logic: `uinput_create_gamepad()` opens `/dev/uinput`, registers 15 buttons + 6 axes via ioctl, creates device via `UI_DEV_CREATE`

## Commit
- `f043e05`: Task 8: Prepare kernel-backed controller test code (software)

## Next Task
- Task 9: Kernel-backed controller runner provisioning (kernel-uinput, physical-controller) — requires external runner with /dev/uinput access