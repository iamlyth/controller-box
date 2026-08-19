# Planning cycle — conformance matrix and gap reconciliation

## Current state

- Codebase substantially complete: 66 source files, 92 test files, 98 CTest targets
- Runner receipt at commit 26df6c0 proves `kernel-uinput` + `installed-package` (test_kernel_controller passes, Flatpak build passes)
- Campaign audit round 1 found 5 findings; all addressed in this plan

## Plan tasks (4 total)

1. **Fix stale docs + add M39 to inventory** — README/OPERATIONS/CMakeLists stale capability claims, wrong inventory counts (52/6/1 → 51/7/1 pre-M39), M39 tested but not enumerated
2. **Controller-transport evidence for profile editor** — M28–M38 tested via keyboard labeled `_controller` (§5.7 violation); need ctrl_press tests through production gamepad transport
3. **aarch64 cross-compile attempt + hardware deferrals** — GPU smoke (gpu-compositor undeclared), Pi 4 latency (target-consumer undeclared), human release acceptance (§11.1.7)
4. **Final audit** — depends on 1–3; §11.2 definition of done

## Key decisions

- VRF-05, PKG-01, DBUS-02, DOD-03, DOD-05 marked verified where runner evidence at 26df6c0 proves them (reconciling campaign audit Finding 1)
- Hardware-blocked items (GPU, aarch64, Pi 4, human) classified partial/missing with Task 3 documenting deferrals per §11.2.6
- `ip_dbus_backend` vtable string conversion is NOT a §10.1 violation — wire-level sd-bus reads use native types (u, b, as); vtable is internal abstraction

## Next action

Run `./scripts/final-gate.sh --planning`; if it passes, emit the completion token.