# Campaign Round 4 Audit — Findings

## Status
Audit report written to `.factory/artifacts/campaign-audit.md` with `result: findings`.
Seven findings documented. The audit is complete pending final-gate validation.

## Key findings
1. 6 of 7 campaign-required capabilities undeclared/unevidenced (only `systemd-user` and `remote-project-gate` evidenced)
2. Controller acceptance uses process-local SDL virtual gamepads, not kernel-backed per SPEC §5.7/§11.1.5
3. GPU backend smoke test skips (exit 77) — no hardware renderer evidence
4. No human release acceptance artifact on target hardware
5. Pi 4 maximum overlay latency not measured on target hardware
6. Manager init doesn't load persisted settings before computing expected controller count
7. Production DBus interface header resides in test directory (`tests/dbus_mock.h`)

## Evidence
- Runner evidence: `check-factory-runner-evidence.py --print-capabilities` → only `remote-project-gate`, `systemd-user`
- Runner unreachable: `ssh dev-runner-vm` fails (hostname unresolvable, no factory-ssh symlink)
- `test_kernel_controller.c` skips (exit 77, no /dev/uinput)
- `test_backend_smoke.c` skips (exit 77, no GPU)
- 10 conformance matrix rows remain `partial` (MGR-36, VRF-05/06/07, PERF-05, DOD-01/03/05/08/09)
- Production source: `manager.c:233` calls `cbx_settings_defaults` but not `cbx_settings_load`
- 28+ production files include `tests/dbus_mock.h` for core interface definitions

## Subagent reviews completed
- Production correctness: traced init, dispatch, DBus, persistence, rendering, shutdown — confirmed production-ready paths, found settings-load gap and dbus_mock.h architectural issue
- Test quality: confirmed real SDL_RenderReadPixels, real private dbus-daemon, real sd-bus — found process-local gamepad gap and weakened kernel-controller assertions
- Security: no critical/high findings — strong defense-in-depth, minor fsync gap in service_install.c
- Documentation: PACKAGING.md accurate, minor OPERATIONS.md/README numbering and icon-naming issues (known/deferred)

## Next action
Emit the completion token.