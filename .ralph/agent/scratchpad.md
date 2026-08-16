# Implementation Loop — Hardware-Blocked (Iteration 31)

## Outcome
- 98/98 tests pass (0 failures, 2 hardware skips: test_kernel_controller #3, test_backend_smoke #88)
- Final gate rejects on MGR-36 (partial — needs kernel-backed gamepad via /dev/uinput)
- All software-addressable work complete: no open tasks
- Plan status: active; tasks 3-9 hardware-dependent
- Git tree clean (HEAD: 5d1d1d5 iteration 30)

## New Findings (Iteration 31)
- uinput kernel module IS loaded on host: `uinput 28672 0 - Live`
- evdev handler available (Minor=64), joydev handler available (Minor=0)
- Real input devices exist on host (Keychron keyboard, Logitech G305 mouse, etc.) — NO gamepad
- /dev is tmpfs (rw, uid=1000) but mknod fails: "Operation not permitted" (zero capabilities)
- User namespaces enabled (unprivileged_userns_clone=1) but mknod STILL fails inside userns
- systemd-tmpfiles available but cannot create device nodes without privileges
- PID 1 shares same mount namespace (mnt:[4026533487]) — no alternative /dev
- SSH runner dev-runner-vm: hostname still unresolvable

## Blocker Analysis
- Task 3 (declare kernel-uinput capability) → requires hardware + human environment.toml change
- Task 4 (kernel controller acceptance) → depends on Task 3
- Task 5 (installed functional with kernel controller) → depends on Task 3
- Task 6 (GPU backend smoke) → requires /dev/dri
- Task 7 (Pi-4 performance) → requires physical Pi-4 hardware
- Task 8 (human release acceptance) → requires human + target hardware
- Task 9 (final audit) → depends on all above

## Recovery Handoff (when hardware available)
1. Expose /dev/uinput (docker --device=/dev/uinput or mknod as root) → Task 3
2. Run test_kernel_controller.c (exit 0) → Task 4
3. Run test_installed_functional.c with kernel controller → Task 5
4. Expose /dev/dri/ for GPU → Task 6 (test_backend_smoke.c)
5. Provision Pi-4 hardware → Task 7 (perf) → Task 8 (human sign-off)
6. Run final audit gate → Task 9
7. Then: ./scripts/final-gate.sh --implementation