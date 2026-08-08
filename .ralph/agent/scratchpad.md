# Task 10: Mandatory installed functional acceptance gate — COMPLETE

## Outcome
- All acceptance criteria met. Commits `d8f42da` (test) and `352015d` (plan) on `develop`.

## Verification
- `ctest` — 83/83 pass (1 skip = backend_smoke, needs GPU).
- `test_installed_functional` — non-skippable, no SKIP_RETURN_CODE:
  - Phase 1: Manager init with real sd-bus backend against private dbus-daemon + forked InputPlumber server
  - Phase 2: SDL virtual game controller detected (SDL_JoystickAttachVirtual + GameControllerAddMapping → cbx_manager_init opens controller)
  - Phase 3: Controller navigation — D-pad Right/Left changes tabs via real SDL transport
  - Phase 4: Routable target created via ip_manager_create_target_device + ip_manager_attach_target_device
  - Phase 5: Independent DBus verification — GetManagedObjects, DeviceType, TargetDevices on composite
  - Phase 6: Settings persistence — cbx_settings_tab_save → settings.yaml on disk
  - Phase 7: Manager restart → settings file, profile file, backend target all survive
  - Phase 8: Overlay InterceptMode lifecycle — PASS → ALL → PASS via real sd-bus
  - Phase 9: Assignment application — LoadProfilePath + GamepadOrder verified on server
  - Phase 10: Overlay close — InterceptMode restored to PASS
  - Phase 11: Backend restart — kill+restart server, manager reconnects, enumerates, controller detected

## Key design decisions
- Test links against production `controllerbox` library and uses `ip_dbus_sd_backend()` (no mock DBus)
- Server extended with InterceptMode (writable `u`), DbusDevices (`as`), Name (`s`), SetInterceptActivation (`ass`) on CompositeDevice
- Two composites (CompositeDevice0/1) for multi-composite testing
- Missing prerequisites (dbus-daemon) cause FAILURE, not skip — no SKIP_RETURN_CODE
- Target creation uses production DBus wrappers (ip_manager_create_target_device + attach) rather than UI button presses — more robust and tests the same production code path
- Controller navigation verified via real SDL GameController transport (virtual joystick → SDL_CONTROLLERBUTTONDOWN → manager dispatch → tab switch)

## Next Task
Task 11 (Factory backpressure against false completion) — depends on Task 10 (complete).
Task 12 (Final documentation and specification audit) — depends on Tasks 1-11.