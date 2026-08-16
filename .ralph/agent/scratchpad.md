# Implementation Loop — Hardware-Blocked (Iteration 16)

## Outcome
- All software tasks (1, 2, 10, 11, 12, 13) complete
- Tasks 3, 6, 7 blocked on hardware — no /dev/uinput device node, no GPU, no Pi 4
- Tasks 4, 5, 8 transitively blocked on Tasks 3, 6, 7
- Task 9 (final audit) blocked on all implementation tasks
- No software-only tasks remain; cycle is genuinely hardware-blocked per SPEC §11.2

## Verification (this iteration)
- Build: 100% — all targets built
- Tests: 98/98 passed, 2 skipped (test_kernel_controller, test_backend_smoke — both need hardware)
- `check-plan-freshness.sh` — pass
- `final-gate.sh --implementation` — rejects on MGR-36 (conformance row `partial`: kernel-backed gamepad required)
- Git tree: clean on develop

## New finding (iteration 16)
- **uinput kernel module IS loaded** (`/proc/modules`: `uinput 28672 0 - Live`)
- **misc device IS registered** (`/proc/misc`: `223 uinput`)
- **But /dev/uinput device node cannot be created**:
  - `mknod` for character devices requires `CAP_MKNOD` in the **initial** user namespace (kernel's `may_mknod()` uses `capable()`, not `ns_capable()`)
  - No sudo, no setuid binaries, no udevd/systemd-udevd running
  - User namespace (`unshare -r`) gives all caps in child namespace, but `capable()` checks init namespace → EPERM
  - `mknod` via ctypes raw syscall: succeeds with wrong mode (creates regular file), EPERM with correct `S_IFCHR` mode
  - FIFO mknod works in user namespace, but char device mknod does not
- **Implication**: If the environment provided udevd or pre-created `/dev/uinput`, the kernel-backed test would work. The blocker is device node creation, not kernel module availability.

## Blocked State
- Tasks 3, 6, 7 require hardware capabilities not declared in `.factory/environment.toml`
- Runner `dev-runner-vm` SSH: unresolvable (no ~/.ssh/factory-ssh, no ~/.ssh/config)

## Recovery Handoff
- When hardware becomes available: Task 3 → Tasks 4, 5 → Task 6 → Task 7 → Task 8 → Task 9
- Task 3: either (a) create /dev/uinput device node (requires udevd or root), or (b) declare `physical-controller`/`kernel-uinput` runner capability with evidence accepted by `check-factory-runner-evidence.py`
- Task 6: declare `gpu-compositor` capability and run GPU backend smoke
- Task 7: declare `target-consumer` capability and measure Pi-4 performance
- Runner requires SSH config entry for `dev-runner-vm` and `~/.ssh/factory-ssh` key provisioning