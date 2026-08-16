# Implementation Loop — Security Fixes Done, Test/Doc Remediation Pending

## Outcome
- Task 11 (security fixes) complete: path traversal in icon_cache.c, NaN SVG
  dimensions, sd_sender_ok defense-in-depth in dbus_client.c
- Tasks 12 (test quality) and 13 (docs) created from independent review findings
- Tasks 3, 6, 7 remain blocked on hardware; Tasks 4, 5, 8 transitively blocked
- Task 9 (final audit) now depends on Tasks 1-8, 10-13

## Verification
- 98/98 tests pass (2 expected skips: kernel_controller, backend_smoke)
- `verify-sanitizers.sh` — clean, no ASan/UBSan defects
- `verify-project.sh` — pass
- `verify-boilerplate.sh` — pass
- `check-plan-freshness.sh` — pass

## Commits
- `d942453`: fix(security): path traversal, NaN dimensions, sd_sender_ok
- `d9b9fdb`: plan: add Tasks 11-13, mark Task 11 complete

## Review Findings (Tasks 12-13)
- **Task 12 (test quality)**: D06 tests lack error message text assertions;
  M37/M38 marked NOT_APPLICABLE but have production-path evidence; M34
  sequential capture lacks DBus signal path variant
- **Task 13 (docs)**: README says build-check but verify-project.sh uses
  build-maintenance-verify; PACKAGING.md missing fonts/profiles/static-libs
  in install layout; OPERATIONS.md visual test regex missing
  test_backend_smoke_sw; CMakeLists.txt comment says O01-O12 vs O01-O13

## Blocked State
- Tasks 3, 6, 7: no /dev/uinput, GPU, or Pi 4 hardware; runner unreachable
- Runner dev-runner-vm: SSH hostname unresolvable, no ~/.ssh/factory-ssh
- Runner evidence aggregate stale (bound to old commit 04d3334)

## Next Task
- Task 12 (test quality fixes) — highest priority pending, no hardware needed
- Task 13 (docs fixes) — also pending, no hardware needed
- After 12+13: only hardware-blocked tasks remain; loop blocked on runner