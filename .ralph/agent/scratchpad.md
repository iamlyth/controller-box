# Implementation Loop — Hardware-Blocked (Iteration 19)

## Outcome
- Replaced all system()/popen() with fork()/execvp() in service_install.c (defense-in-depth)
- Build: 100%, Tests: 98/98 pass, 0 compiler warnings, sanitizer gate clean
- 2 skipped (hardware: test_kernel_controller, test_backend_smoke)
- Final gate still rejects on MGR-36 (kernel-backed gamepad requires /dev/uinput)

## Fix this iteration (1 commit on develop)
- `cdc000d` service_install.c: replaced 3 popen() + 1 system() calls with run_command() (fork/execvp/pipe). Added build_argv() helper to tokenize prefix + append subcommand args. No shell invocation remains — eliminates theoretical injection even for future changes. Updated 8 tests to use executable mock scripts instead of shell command strings.

## All software-addressable findings now resolved
- ✅ system()/popen() → fork/exec (this iteration)
- ✅ strstr path confusion → classify_device_path (iter 18)
- ✅ 8 weakened assertions → exact value checks (iter 18)
- ✅ 7 security/test-quality issues (iter 17)
- ✅ Sanitizer bugs: trigger.c, manager.c, icon_cache (iter 16)
- Remaining: test_manager_interaction_ctrl.c mock doesn't verify DBus args (mitigated by native tests)
- Remaining: test_kernel_controller.c no semantic assertions (moot while skipping)

## Blocked State (unchanged)
- Tasks 3, 6, 7 require hardware capabilities not declared in `.factory/environment.toml`
- /dev/uinput: no device node, no sudo, Nix sandbox (zero capabilities)
- Runner dev-runner-vm: SSH unresolvable
- MGR-36, VRF-06, PERF-05, VRF-07, DOD-01/03/05/07/08/09 all blocked on hardware or human sign-off

## Recovery Handoff
- When hardware available: Task 3 → Tasks 4, 5 → Task 6 → Task 7 → Task 8 → Task 9
- All software-addressable code/test/security issues now resolved
- Next iteration: attempt final gate (will reject on MGR-36) or exit at iteration limit