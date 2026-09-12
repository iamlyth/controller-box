---
spec_path: docs/SPEC.md
spec_commit: 3a10f6b7
base_commit: b98dc29e
status: active
roles_override: {"skip_auditors": ["linting"], "skip_studies": ["manager"]}
---

## Task 1: Fix test_golden profile-editor golden image mismatches
Title: Fix test_golden profile-editor golden image mismatches
Status: completed
Dependencies: none
Acceptance: `test_golden` passes all 11 sub-tests. The three profile-editor
Verification: `ctest --test-dir build -R test_golden --output-on-failure`
Runner: none
Evidence: `test_golden` passes all 11 sub-tests.

## Task 2: Stabilize flaky acceptance tests
Title: Stabilize flaky acceptance tests
Status: completed
Dependencies: none
Acceptance: The full ctest suite passes reliably across repeated consecutive
Verification: `ctest --test-dir build -E '^test_icon_map$' --output-on-failure
Runner: none
Evidence: Implemented in tests/CMakeLists.txt (per-test TIMEOUTs calibrated to

## Task 3: Kernel-backed controller integration test on kernel-uinput runner
Title: Kernel-backed controller integration test on kernel-uinput runner
Status: completed
Dependencies: none
Acceptance: `test_kernel_controller` runs (not skipped) and passes on a runner
Verification: `ctest --test-dir build -R test_kernel_controller --output-on-failure`
Runner: kernel-uinput
Evidence: Verification passed on dev-runner-vm (kernel-uinput capability). ctest --test-dir build -R test_kernel_controller --output-on-failure → 100% passed. Fixes applied: (1) D-pad events sent as ABS_HAT0X/ABS_HAT0Y hat axes instead of BTN_DPAD_* buttons (SDL2 Xbox 360 mapping expects hat axes); (2) axis ranges configured via UI_ABS_SETUP; (3) systemd service unit file pre-created to skip first-run modal dialog; (4) navigation sequence corrected: 10 DOWN presses to reach Save entry (index 10) in the 11-item settings list.

## Task 4: Accelerated backend smoke test on gpu-compositor runner
Title: Accelerated backend smoke test on gpu-compositor runner
Status: completed
Dependencies: none
Acceptance: `test_backend_smoke` runs (not skipped) and passes on a runner with
Verification: `ctest --test-dir build -R test_backend_smoke --output-on-failure`
Runner: gpu-compositor
Evidence: Verification passed on gpurunner. 100% tests passed.

## Task 5: Fix test_icon_map default-path install-state dependency
Title: Fix test_icon_map default-path install-state dependency
Status: completed
Dependencies: none
Acceptance: `test_icon_map` passes all 42 sub-tests deterministically from a
Verification: `ctest --test-dir build -R test_icon_map --output-on-failure`
Runner: none
Evidence: test_icon_map passes all 42 sub-tests. Fix applied.

## Task 6: Resolve orphaned study-report files to keep git status clean
Title: Resolve orphaned study-report files to keep git status clean
Status: completed
Dependencies: none
Acceptance: The repository has no untracked files and stays clean across the
  per-round study/bookkeeping cycle. Specifically: (1) `.factory/rounds/` is
  added to `.gitignore` (transient round bookkeeping, analogous to the already-
  ignored `.factory-state/`); (2) the duplicate untracked artifact
  `.factory/artifacts/subsystem-study-manager.md` (content duplicates the tracked
  `subsystem-manager-study.md`) is removed; (3) current-cycle study reports under
  `.factory/artifacts/` follow a consistent naming convention so they are either
  tracked or ignored and never reappear as untracked orphans after the routine
  `git add -A` commit boundary. After the change `git status --porcelain` is
  empty, and it remains empty once the next normal study round runs.
Verification: `git status --porcelain` (expect empty);
  `grep -q '^\.factory/rounds/' .gitignore`;
  `test ! -e .factory/artifacts/subsystem-study-manager.md`
Runner: none
Evidence: (unassigned) tester records an empty `git status --porcelain`, the
  `.factory/rounds/` gitignore entry, and removal of the duplicate artifact.

## Task 7: Create missing docs/OPERATIONS.md and fix referenced documentation
Title: Create missing docs/OPERATIONS.md and fix referenced documentation
Status: pending
Dependencies: 6
Acceptance: `docs/OPERATIONS.md` exists and is referenced as the authoritative
  operations/runtime guide. Every documentation reference that is currently
  dangling in `README.md` is resolved (present-ified or de-linked). The
  `AGENTS.md` ordinary-defects pointer to `.factory/bugs/{open,closed}.md` is
  made consistent with reality: either the bug-ledger files are created or the
  reference is corrected to the actual location, so no doc points at a
  non-existent path.
Verification: `test -f docs/OPERATIONS.md && grep -q 'OPERATIONS.md' README.md &&
  grep -q '\.factory/bugs' AGENTS.md && (test -d .factory/bugs || grep -qE
  '\.factory/bugs/.{0,40}?does not exist|no longer' AGENTS.md)&& echo docs-ok`
Runner: none
Evidence: (unassigned) tester records the file present and the doc-link checks passing.

## Task 8: Fix config overlay_opacity serialization precision drift
Title: Fix config overlay_opacity serialization precision drift
Status: pending
Dependencies: 6
Acceptance: `overlay_opacity` round-trips without precision loss for any value
  the parser accepts. The serializer in `src/config/config_settings.c` no longer
  truncates legal fractional values (e.g. `0.3333`) via a fixed `%.2f` format;
  emitted YAML preserves the loaded value in full so save→load is lossless.
  `test_settings` gains a regression case for a non-2-decimal opacity value and
  passes all existing round-trip cases.
Verification: `ctest --test-dir build -R test_settings --output-on-failure`
  plus `./scripts/verify.sh`
Runner: none
Evidence: (unassigned) tester records `test_settings` pass plus the new precision case.

## Task 9: Wire identify connect-time orchestration into production connect path
Title: Wire identify connect-time orchestration into production connect path
Status: pending
Dependencies: 6
Acceptance: The controller-connect pipeline that SPEC §6.2–6.3 and §10.3 describe
  is actually exercised in production: on source-device add/connection the
  hotplug path in `src/app/overlay_service.c` calls `cbx_identity_extract` to
  derive a stable ID, applies `cbx_assign_persist_auto_assign` / `cbx_assign_resolve`
  for slot+profile preference lookup, re-applies persisted `GamepadOrder` via
  `cbx_gamepad_order_restore`, and records downgrade-safe preferences. These
  identify functions, currently only referenced by tests, gain production call
  sites without test-only bypasses.
Verification: `ctest --test-dir build -R 'test_identity|test_assign|test_order_restore|test_overlay_native|test_gamepad_order' --output-on-failure`
  plus `./scripts/verify.sh`
Runner: none
Evidence: (unassigned) tester records the targeted and full test results plus the
  production call-site evidence.

## Task 10: Fix widget_list out-of-bounds read from negative scroll_offset
Title: Fix widget_list out-of-bounds read from negative scroll_offset
Status: pending
Dependencies: 6
Acceptance: `src/ui/widget_list.c` no longer indexes `items[]` out of bounds.
  In the `SDL_MOUSEWHEEL` handler the clamp
  `scroll_offset = item_count - visible_count` can be negative when
  `visible_count > item_count` (the lower `scroll_offset < 0` guard runs before
  it), so `scroll_offset` becomes negative and `list_draw` reads `items[negative]`.
  The clamp is bounded to `>= 0` (and re-guarded), and a regression test drives a
  small list with a large visible count through a wheel event and asserts no
  negative offset / no OOB access.
Verification: `ctest --test-dir build -R 'test_widget_list' --output-on-failure`
  plus `./scripts/verify.sh`
Runner: none
Evidence: (unassigned) tester records the targeted list test and full-suite pass.

## Task 11: Fix input_map axis returning no KEYUP (stuck-key on stick nav)
Title: Fix input_map axis returning no KEYUP (stuck-key on stick nav)
Status: pending
Dependencies: 6
Acceptance: Controller-stick-driven navigation no longer leaves a widget with a
  stuck press/release state. `src/ui/input_map.c` currently emits only
  `SDL_KEYDOWN` for an axis deviation and returns nothing once the stick returns
  to the deadzone, contrary to its own header contract ("return-to-center →
  SDL_KEYUP for the last direction"). The axis path now emits the matching
  `SDL_KEYUP` on deadzone return (state tracked per direction), or the header and
  consumer semantics are aligned so press/release pairing is correct.
  `test_input_map` gains a case covering stick push + center-return and asserts
  paired KEYDOWN/KEYUP.
Verification: `ctest --test-dir build -R 'test_input_map' --output-on-failure`
  plus `./scripts/verify.sh`
Runner: none
Evidence: (unassigned) tester records the targeted input-map test and full-suite pass.

## Task 12: Eliminate uncached-text texture leak in cbx_text_render
Title: Eliminate uncached-text texture leak in cbx_text_render
Status: pending
Dependencies: 6
Acceptance: The resident overlay/manager no longer leaks one `SDL_Texture` per
  uncached render call. In `src/ui/text.c`, on the over-length
  (`>= CBX_TEXT_MAX_LEN`) and cache-full paths `cbx_text_render` returns a fresh
  texture that callers must destroy, but production render loops
  (`src/overlay/grid_render.c`, `src/ui/widget_label.c`) treat the returned
  handle as cache-owned and never free it, so each uncached render leaks a
  texture. These paths now cache-own, or every consumer frees, the texture, and a
  regression test repeatedly renders an over-length/uncached string and asserts
  texture/renderer resource growth stays bounded (zero per-render leak).
Verification: `ctest --test-dir build -R 'test_text|test_grid_render' --output-on-failure`
  plus `./scripts/verify.sh`
Runner: none
Evidence: (unassigned) tester records the targeted text test and full-suite pass.

## Task 13: Harden DBus sender verification against daemon loss/squatting
Title: Harden DBus sender verification against daemon loss/squatting
Status: pending
Dependencies: 6
Acceptance: The F3 anti-squatting defense survives InputPlumber name loss.
  `src/dbus/dbus_client.c` `NameOwnerChanged` handling clears
  `expected_sender` (and PID/EUID fingerprint) when the InputPlumber unique name
  is lost (`new_owner == ""`), so a name later reassigned to a squatter is not
  blindly trusted by signal handlers. `src/dbus/ip_connection.c` `verify_sender`
  performs its `if (!conn)` guard before writing `sender_verified`/
  `expected_pid`/`expected_uid` (avoiding UB on a NULL connection). Both changes
  are covered by `test_connection` / native DBus tests.
Verification: `ctest --test-dir build -R 'test_connection|test_native_dbus|test_overlay_native' --output-on-failure`
  plus `./scripts/verify.sh`
Runner: none
Evidence: (unassigned) tester records the targeted DBus tests and full-suite pass.

## Task 14: Final documentation and specification audit
Title: Final documentation and specification audit
Status: pending
Dependencies: 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13
Acceptance: The complete active-cycle task ledger is present and every task is
  resolved (`completed`, or genuinely `blocked` pending an explicitly recorded
  external/human requirement — never passing merely because a model cannot act).
  `git status` is clean, all doc references resolve, the conformance matrix in
  `docs/SPEC.md` §11 reflects verified acceptance with source evidence, and
  `./scripts/verify.sh` passes in full.
Verification: ./scripts/verify.sh
Runner: none
Evidence: Passing verification-gate output; clean `git status`; conformance
  matrix with every normative requirement verified.
