# Spec Compliance Audit — Task 16 (round 2): Native physical capture producing valid portable mappings

**Scope:** SPEC §§5.3–5.4, 5.7, 7.1, 7.6, 10.1–10.2, 11.1–11.2 vs. the Task 16
acceptance criteria (BUG-0017 resolution, interception ownership, composite
resolution/authentication, sequential capture semantics, B-skip/Start-cancel
affordances, native service evidence).

**Verification reproduced by this audit (fresh clean rebuild):**

- Clean rebuild (`rm -rf build && cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
  && cmake --build build --parallel`) — OK.
- Focused gate `ctest -R 'test_editor_list_mode|test_editor_seq_mode|test_manager_native|test_profile_save|test_installed_functional'` — 6/6 passed.
- `./scripts/verify.sh` — exit 0, 91/91 ctest passed (2 declared exit-77
  hardware skips: `test_kernel_controller`, `test_backend_smoke`). One rerun
  of a full-suite invocation failed `test_installed_diagram`
  ("custom-prefix cmake build failed", build output suppressed) — see INFO-1.
- `./scripts/verify-sanitizers.sh` — exit 0, ASan+UBSan, 91/91 passed, no
  sanitizer defects.

## Findings

### BLOCKER-1 — `cbx_profile_editor_set_dbus` wipes the composite device filter even when nothing changed, leaving an armed capture accepting events from any composite (fail-open)

**Files:** `src/manager/profile_editor_list.c` (`cbx_profile_editor_set_dbus`),
`src/manager/profiles_tab.c` (`cbx_profiles_tab_set_composite`),
`src/manager/manager.c` (`cbx_manager_on_tab_change`, Profiles case),
`src/manager/profile_editor_list.c` (`event_device_accepted` — now-stale
comment).

`cbx_profile_editor_set_dbus` clears `ed->target_count` and
`ed->dbus_device_count` **unconditionally**, but only re-arms capture
(`acquire_interception`) when `changed` is true:

```c
bool changed = (ed->backend != backend || ed->bus != bus ||
                strcmp(ed->composite_path, new_path) != 0);
if (changed)
    cbx_profile_editor_release_interception(ed);
...
ed->target_count = 0;
ed->dbus_device_count = 0;          /* always, even when !changed */
...
if (changed && (ed->capture_active || ed->seq_active)) { ... re-arm ... }
```

Production reachability: `cbx_manager_on_tab_change` (Profiles case) calls
`cbx_profiles_tab_set_composite` → `cbx_profile_editor_set_dbus` with the
**same** backend/bus/composite path whenever the Profiles tab is re-entered
(clicking another tab with the mouse and back; the tab bar is visible and
mouse-interactive while the editor is open — §§5.1/5.7 make the pointer a
mandatory path). If a capture/sequential session is active at that moment,
`changed == false`, so:

- the interception and subscription stay armed (`intercept_active`,
  `subscription_active` remain true),
- the composite's `DbusDevices` filter is wiped to zero entries, and
- `event_device_accepted()` returns **true for every device path** when
  `dbus_device_count == 0` — InputEvents from *other* composites are now
  captured into the binding.

This is exactly the fail-open class the task's own repair cycle fixed for the
probe-failure case ("a failed/empty probe releases the subscription and aborts
… instead of … accepting every device"): here the filter is emptied *after*
acquisition succeeded, with no abort and no re-probe. The acceptance
invariant "load its virtual capabilities and reject other devices' events" is
silently disabled by a production-reachable input path, and the capabilities
list is wiped mid-session as well. The comment on `event_device_accepted`
("that state is only reachable when no composite is selected") is no longer
true. No test covers `set_dbus` with unchanged arguments during an active
capture.

**Recommendation:** Either (a) clear `dbus_device_count`/`target_count` only
when `changed` is true, or (b) treat any `set_dbus` call during an active
capture/sequential as requiring re-acquisition (re-probe `DbusDevices`,
abort via `editor_abort_capture` on failure) regardless of `changed`. Add a
regression test: arm capture on composite A with two composites on the
backend, invoke `set_dbus` with identical backend/bus/path, and assert the
filter is intact (or the capture aborted) and a foreign composite's event is
still rejected.

### WARN-1 — NES-minimum validation checks the *source* side of mappings while capture semantics define the required set on the *target* side

**Files:** `src/manager/profile_validate.c` (`cbx_profile_has_binding` —
compares the mapping's **source_event** button name against the required
button), `src/manager/profile_editor_seq.c` (`seq_on_input` — records pressed
physical button as **source**, prompted virtual button as **target**),
`src/manager/profile_save.c` (save gate).

SPEC §5.4: "A valid profile must bind at least: A, B, D-Pad Up, D-Pad Down,
D-Pad Left, D-Pad Right … this is the minimum set that can navigate any menu
and play any NES-class game", with the rationale that "requiring it makes
'broken profile' an impossible state." The game navigates with the **virtual
device** — the target side of the mapping. With task 16's (correct)
InputPlumber semantics (source = pressed physical, target = prompted
virtual), the source-keyed check produces both failure modes:

- **False pass:** prompted "Guide", user presses physical **A** → mapping
  `source A → target Guide`. `cbx_profile_has_binding(A)` finds source "A" and
  the profile saves — while nothing maps to *virtual* A. The game cannot
  navigate menus; "broken profile" is possible, contradicting §5.4's core
  guarantee.
- **False fail:** prompted "A", user presses physical X → `source X →
  target A` (a legitimate remap — the entire purpose of a profile editor).
  Unless some other mapping uses source A, NES validation reports
  "Missing: A" and save is refused although the game-visible NES set is
  complete.

The empty-profile native test passes because it captures identity bindings
(pressed == prompted), the only case where the two sides coincide.

**Recommendation:** Validate the required set against mapping **targets**
(`target_events` with `device_class == "gamepad"` and value equal to the
required button), not sources. Add tests for both directions: a profile
with virtual A/B/dpad bound via non-identity sources must pass; a profile
with all six sources bound but to non-required virtual targets must fail.

### WARN-2 — List-mode capture can produce a mapping with zero target events, which saves as `target_events: []` — not a valid portable mapping

**Files:** `src/manager/profile_editor_list.c`
(`cbx_profile_editor_activate` unbound-row path via
`cbx_profile_editor_find_or_create_mapping` — creates a mapping with
`target_event_count == 0`; `cbx_profile_editor_on_input_event` — capture
writes only the source), `src/config/config_profile.c`
(`cbx_profile_validate` permits `target_event_count == 0`; `emit_mapping`
emits an empty `target_events` sequence), `src/manager/profile_save.c`.

Sequential capture gives newly created mappings an identity gamepad target
(`seq_on_input` sets `target_events[0] = gamepad:<prompted>`). List capture
does not: activating an unbound catalog row creates a source-only mapping,
and choosing "Capture" only rewrites the source to the pressed button. The
resulting mapping — source `<pressed>`, target `[]` — passes structural
validation (0 is in range) and, if the pressed source covers the NES source
names (WARN-1), passes NES validation too, so the production save path
writes `target_events: []` into the profile YAML. That is a semantically
void mapping, and the task's own title/acceptance is "native physical capture
produce **valid portable** mappings". (Contrast: the native t16 test asserts
`target_event_count >= 1` only for the sequential-captured mappings.)

**Recommendation:** Mirror the sequential path: when a capture completes on a
mapping created by the unbound-row flow with `target_event_count == 0`, set
an identity gamepad target from the row's catalog button (or require
"Pick Target" first and refuse capture on a target-less mapping). Optionally
reject `target_event_count == 0` in `cbx_profile_validate` so no write path
can persist a void mapping.

### WARN-3 — BUG-0017 (and BUG-0016) still open in the defect ledger despite being resolved by this task (carry-over from round 1)

**Files:** `.factory/bugs/open.md` (BUG-0017, BUG-0016 entries); the
`closed.md` ledger referenced by `AGENTS.md` does not exist.

Task 16's acceptance is "Resolve BUG-0017" and the resolution is implemented
and natively verified, but `open.md` still lists BUG-0017 — and BUG-0016 —
as open defects with no resolution note. SPEC §11.2 DOD-6 requires that the
open ledger contain no unresolved defect contradicting completed work. This
was WARN-1 of the round-1 audit (`docs/audit-spec-task16-compliance.md`) and
was not addressed by the repair cycle.

**Recommendation:** Mark both resolved with a short resolution/evidence note
(fix commits, test names, verification log), or create `closed.md` and move
them.

### WARN-4 — No test evidence for interception/subscription restore on editor close and manager shutdown during active capture (narrowed carry-over)

**Files:** `src/manager/profiles_tab.c` (`cbx_profiles_tab_close_editor` →
`cbx_profile_editor_reset_mode` → `cbx_profile_editor_release_interception`),
`src/manager/manager.c` (`cbx_manager_shutdown` tears tabs down before
`ip_connection_disconnect` — correct order), `tests/`.

The acceptance enumerates restore on "completion, cancel, editor close,
disconnect, shutdown and failed initialization". Completion, cancel, failed
init, exact prior mode, backend replacement, and backend loss/disconnect
(now covered by `test_backend_loss_aborts_capture` and the set_context
degraded rewire) all have direct evidence. The editor-close and
manager-shutdown paths exist in production code and are correctly ordered,
but no test arms capture and then asserts the restore write (mock:
`last_set_value` returns to the prior mode; native: server-side
`InterceptMode`) or the subscription release across those two paths.
SPEC §11.2.1/§11.2.3 require executable evidence for each
acceptance-enumerated behavior; this was round-1 WARN-2 and remains
unaddressed (the disconnect leg has since been covered).

**Recommendation:** Add tests: (a) arm capture, call
`cbx_profiles_tab_close_editor` (or the discard path), assert prior mode
restored and subscription released; (b) arm capture, call
`cbx_manager_shutdown`, assert the same against the native/mock backend.

### WARN-5 — Capture arms interception with no dispatchable input path when sender authentication is unresolved (fail-silent corner, carry-over)

**Files:** `src/manager/profile_editor_list.c`
(`cbx_profile_editor_acquire_interception` — passes
`ed->expected_sender[0] ? ed->expected_sender : NULL`; no failure when a
composite is selected but the unique-name resolution failed),
`src/dbus/ip_input_signal.c` (`ip_input_events_handle` drops every event
silently when `expected_sender` is NULL).

When `get_unique_name` fails while the composite queries succeed
(e.g. a service-restart race), the editor still subscribes and sets
`InterceptMode = GAMEPAD_ONLY`; `ip_input_events_handle` then silently drops
every InputEvent, so InputPlumber swallows the controller's input while the
status label still says "Press a button to capture..." — the exact
fail-silent class BUG-0017 root cause #3 called out ("fail loudly instead of
silently dropping"). This was round-1 WARN-3 and was not addressed by the
repair cycle (the repair closed the DbusDevices probe corner, not this one).

**Recommendation:** In `cbx_profile_editor_acquire_interception`, treat an
empty `expected_sender` together with a non-empty `composite_path` as an
acquisition failure (fail closed before setting `InterceptMode`), or accept
all senders only via an explicit logged downgrade. Add a regression test
with a backend whose `get_unique_name` fails.

### INFO-1 — Installed-path acceptance tests are flaky under host load, and their custom-prefix build failures are undiagnosable (output discarded)

**Files:** `tests/test_installed_diagram.sh` (custom-prefix
configure/build/install with all output sent to `/dev/null`), related
installed-path tests (`test_installed_binary`, `test_installed_smoke`).

During this audit's reproduction, one full-suite run failed
`test_installed_binary` and one `verify.sh` rerun failed
`test_installed_diagram` with "custom-prefix cmake build failed" after 2.8 s;
both passed in isolation and on immediate rerun. The host was under heavy
external memory pressure (~53/62 GB used) during the failures, which is the
plausible cause — the failures were not reproducible and the full gates
otherwise exit 0. However, the failing step discards the compiler/build
output (`>/dev/null 2>&1`), so a real build break in this path would be
reported only as "custom-prefix cmake build failed", with no artifact to
diagnose from — SPEC §11.1.4 requires failure artifacts for exactly this
reason, and §11.2.5 treats flaky rerun dependencies as a gate concern.

**Recommendation:** Capture the custom-prefix build output to a log file
under the test's temp tree and print it (or save it as a named artifact) on
failure, mirroring the golden-image failure-artifact convention.

### INFO-2 — Sequential capture on a pre-existing non-identity mapping keeps the old target, not the prompted one

**Files:** `src/manager/profile_editor_seq.c` (`seq_on_input` — "an existing
mapping keeps the output the user already chose").

The acceptance says "Sequential capture records the pressed source and
prompted virtual target". For a newly created mapping that holds (target =
prompted button). For an existing mapping whose target differs from the
prompted virtual button, re-running sequential records the pressed source
but keeps the *existing* target — the prompt then mislabels the binding the
press will produce. Deliberate per the code comment, and harmless for the
empty-profile and identity flows the acceptance tests; noting for the
record. If round-1's WARN-1 direction is ever revisited (target-keyed
lookup), this asymmetry disappears.

### INFO-3 — Round-1 cosmetic/observational items remain unaddressed

**Files:** `src/manager/profile_editor_list.c` (`begin_capture` status text
"…B=Cancel" describes an affordance the armed DBus stream does not have — B
presses are captured as bindings), `src/manager/profile_editor_seq.c`
(completion path blanks the "Sequential binding complete!" label and hides
the progress bar in the same frame; dual-stream B/Start exclusivity
assumption undocumented), `docs/` (the upstream InputPlumber research
artifacts cited by SPEC §1 are not in-repo, so the InputEvent string
vocabulary's live-service conformance still rests on the `iprunner`
hardware runner).

All three were round-1 INFO-1..4; none block task 16 and none regressed.

## Verdict

Task 16's acceptance criteria are implemented through the production paths
and all three declared verification commands reproduce exit 0: explicit
composite resolution (`manager.c` → `cbx_controllers_tab_selected_composite`
→ `profiles_tab_set_composite`), fail-closed acquisition with exact
prior-mode restore and subscription release across completion/cancel/failed
init/backend loss, sender + `DbusDevices` device authentication with native
foreign-event rejection, prompt-aware B/Start guards in both input streams,
and the clean-empty-profile sequential NES capture with native
load/save/restart round trip, all natively observed on the sd-bus service.

One BLOCKER remains: the unconditional device-filter wipe in
`cbx_profile_editor_set_dbus` re-opens the fail-open hole this task's repair
cycle closed, reachable in production via the pointer path during an armed
capture (BLOCKER-1). It should be fixed — and regression-tested — before the
task is considered complete. WARN-1/WARN-2 undercut the task's headline goal
("valid portable mappings") in remap and unbound-row-capture scenarios and
should be scheduled for the next planning round.