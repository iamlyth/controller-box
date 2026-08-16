# Campaign Round 3 Audit — Current Handoff

## Status
Audit report written to `.factory/artifacts/campaign-audit.md` with `result: findings`.
Five findings identified, all rooted in missing environment capabilities that
the campaign `required_capabilities` list demands but the declared runner
cannot evidence.

## Key evidence
- Runner evidence digest: `46ccb1aa...` matches front matter.
- Evidenced capabilities: only `remote-project-gate` and `systemd-user`.
- Missing: `physical-controller`, `kernel-uinput`, `inputplumber-system-dbus`,
  `gpu-compositor`, `installed-package`, `target-consumer` (6 of 7).
- Production code is clean — no stubs/TODOs/placeholders. DBus native type
  fidelity, sender verification, atomic persistence, path traversal protection
  all confirmed by source audit.
- Installed tests use `xdotool key` (keyboard) — spec §5.7 says keyboard events
  are supplemental, "must never be labeled controller acceptance."
- `test_kernel_controller.c` and `test_backend_smoke.c` skip with exit 77.
- Conformance matrix PERF-01/MGR-07 marked "verified" despite skips; spec
  §11.1.5 says missing backend is "failure, not a skip."

## Findings summary
1. Installed functional smoke test uses keyboard events, not controller input.
2. Controller-path acceptance uses process-local SDL virtual gamepads.
3. GPU backend smoke skipped — no gpu-compositor.
4. Human release acceptance not possible — no target-consumer.
5. Conformance matrix PERF-01/MGR-07 incorrectly classified as "verified."

## Next action
Run `./scripts/final-gate.sh --campaign-audit` to validate the report.
If it passes, emit the completion token. If it fails, fix the deficiency.