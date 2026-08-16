# Campaign Audit Round 5 — Handoff

## Status
Independent audit complete. Result: `findings`.

## Key discovery
Production icon cache path mismatch: `overlay_service.c:1164` passes `cbx_icon_dir()`
(= `/usr/share/controller-box/icons`) to `cbx_icon_cache_init()`, but
`icon_cache.c:99` builds paths as `icon_dir/{name}.svg` without a `/svg/` subdirectory.
CMake installs SVGs to `icons/svg/`. Tests bypass by passing `data/icons/svg/` directly.
Overlay will render without virtual-device icons in production — SPEC §4.10 violation.

## Environment gaps (confirmed)
Only 2/7 required capabilities declared: `systemd-user`, `remote-project-gate`.
Missing: physical-controller, kernel-uinput, inputplumber-system-dbus, gpu-compositor,
installed-package, target-consumer. Tasks 3-7 remain pending (hardware-blocked).

## Subagent reviews (5 parallel, all completed)
- Production paths: clean (init, DBus, settings, security, lifecycle all verified)
- Test quality: mocks identified but native DBus tests are genuine; process-local
  virtual gamepad in installed functional test (SPEC §5.7 violation)
- Documentation: README/OPERATIONS.md accurate (52/6/1, x86_64 qualifier, Flatpak experimental)
- Visual rendering: icon cache path mismatch found; content density checks adequate
- Security: no issues (fork/exec, O_NOFOLLOW, uint32 types, path classification)

## Next action
Emit the completion token.