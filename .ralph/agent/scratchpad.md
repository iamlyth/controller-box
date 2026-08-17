# Task 9: Kernel-backed controller runner provisioning — BLOCKED

## Outcome
- Task 9 marked **blocked** in implementation plan with full evidence
- `/dev/uinput` does not exist on current runner; `modprobe` and `sudo` unavailable
- `~/.ssh/factory-ssh` launcher symlink missing; SSH to `dev-runner-vm` fails (hostname unresolvable)
- `check-factory-runner-evidence.py` reports stale aggregate binding (commit 6465013 vs HEAD 23ef8da)
- Runner setup requirements documented in `docs/OPERATIONS.md` §Kernel-backed controller runner setup
- Conformance matrix updated: VRF-05, DOD-03 note blocked status; SYS-01 task ref fixed (was Task 9, now Task 11)

## Verification
- Code inspection confirms test code is ready (Task 8): `test_installed_functional.c` detects `/dev/uinput` and uses kernel-backed evdev gamepad when present
- `test_kernel_controller.c` will exit 0 when `/dev/uinput` is provisioned
- Runner setup steps documented: `modprobe uinput`, `chmod 0660 /dev/uinput`, `usermod -aG input`, udev rules, `run-factory-runners.py` for evidence

## Commit
- `247ee36`: Task 9: Mark blocked — no /dev/uinput or SSH runner access

## Next Task
- Task 10: GPU backend smoke acceptance (gpu-compositor) — requires accelerated GPU backend, likely also blocked
- Task 11: aarch64 build target — can create toolchain file as partial deliverable, cross-compiler unavailable in nix-shell
- Task 12: Target hardware latency + human release acceptance — template can be created in software
- Task 13: Flatpak build + real InputPlumber — requires flatpak-builder + InputPlumber, likely blocked
- Task 14: Final audit — depends on all above

<!-- factory-stale-recovery:start -->
## Supervisor recovery feedback

- The previous `implementation` Ralph attempt terminated as a stale loop.
- Run `./scripts/final-gate.sh --implementation` yourself and fix every reported failure.
- Do not repeat a completion summary until that command passes. Replace this section in the next scratchpad handoff before requesting completion.
<!-- factory-stale-recovery:end -->
