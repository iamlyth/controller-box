# Open Bugs

## BUG-0016: Profile editor only shows A, B, and D-pad directions

**Severity:** Major
**Component:** manager / profile editor
**Date discovered:** 2026-09-13

### Description

The profile editor's list mode only displays the 6 mappings that exist in the
shipped default profile (`data/profiles/default.yaml`): A, B, D-Pad Up,
D-Pad Down, D-Pad Left, D-Pad Right. All 17 buttons defined in
`cbx_diag_button` (X, Y, Start, Select, Guide, L1, R1, L2, R2, L3, R3) are
missing from the list.

### Root cause

`cbx_profile_editor_refresh` (`src/manager/profile_editor_list.c:717-733`)
iterates only `ed->profile.mapping_count` — it shows whatever mappings the
loaded profile contains, not all 17 possible buttons. The default profile
(`data/profiles/default.yaml`) was written to the NES-minimum validation set
(6 buttons per `profile_validate.c:20-27`), so only 6 rows appear.

### Fix options

1. Expand `data/profiles/default.yaml` to include all 17 buttons with
   identity mappings, or
2. Change `cbx_profile_editor_refresh` to show all `CBX_DIAG_BTN_COUNT`
   buttons as rows, with empty/placeholder targets for unbound ones.

Option 2 is preferable — it works regardless of which profile is loaded.

---

## BUG-0017: Profile editor capture mode doesn't register input

**Severity:** Major
**Component:** manager / profile editor
**Date discovered:** 2026-09-13

### Description

When the user selects "Configure" on a binding and enters capture mode
("Press a button to capture..."), pressing any button on the controller
does nothing. The capture never completes and the status label never
updates.

### Root cause

Three compounding defects prevent InputEvent signals from reaching the
capture callback:

1. **InterceptMode never enabled** — `cbx_profile_editor_begin_capture`
   (`src/manager/profile_editor_list.c:968-992`) subscribes to
   `InputEvent` signals via `ip_input_events_subscribe` but never calls
   `ip_composite_set_intercept_mode` to switch InputPlumber into
   ALL or GAMEPAD_ONLY intercept mode. InputPlumber only emits
   `InputEvent` signals during intercept mode. The overlay trigger code
   (`src/overlay/trigger.c:127-134`) does this correctly; the editor
   does not. Same defect in `profile_editor_seq.c:100-113`.

2. **Composite device path is NULL** — `profiles_tab.c:1317` passes
   `NULL` as `composite_path` to `cbx_profile_editor_set_dbus`, so
   `ed->composite_path` is empty. Even if the intercept call were added,
   there's no device path to target. This also breaks
   `cbx_profile_editor_load_capabilities` (which needs the path for
   `ip_composite_get_capabilities`).

3. **Sender verification silently drops signals** — if
   `get_unique_name` fails (`profile_editor_list.c:585-594`),
   `expected_sender` stays empty and `ip_input_events_handle`
   (`ip_input_signal.c:259-263`) drops every signal because
   `strcmp(payload->sender, ie->expected_sender)` fails.

### Fix

- Wire the composite device path through `cbx_profiles_tab_set_context`
  / `open_editor` into `cbx_profile_editor_set_dbus`.
- In `begin_capture` and `begin_sequential`, call
  `ip_composite_set_intercept_mode(backend, bus, composite_path, "3")`
  (GAMEPAD_ONLY) before subscribing, and restore to `"1"` (PASS) on
  cancel/exit/commit.
- Fall back to accepting all senders when `expected_sender` is empty,
  or fail loudly instead of silently dropping.

### Note

Unit tests pass because they call `cbx_profile_editor_on_input_event`
directly, bypassing the DBus/InterceptMode path entirely.

---

## BUG-0018: Virtual controllers not visible in Steam (RESOLVED — not a bug)

**Severity:** N/A
**Component:** external / Steam configuration
**Date discovered:** 2026-09-13
**Date resolved:** 2026-09-13

### Description

Controller-box manager reports "2 virtual slots ready" and InputPlumber
is running, but the virtual controllers did not appear in Steam's
Controller Settings.

### Resolution

Not a controller-box bug. `evtest` confirms InputPlumber creates the
virtual devices at the kernel level as "Microsoft X-Box 360 pad" evdev
devices (`/dev/input/event23-26`). Steam needs to be restarted to
rescan for new controllers, or the user must enable generic controller
support in Steam settings.
## BUG-0019: Audit parser records "no BLOCKER" findings as BLOCKERs

**Severity:** Major
**Component:** factory harness / parallel.py audit parser
**Date discovered:** 2026-09-13

### Description

When auditors return a report that explicitly says "No BLOCKER issues" or
"No findings at BLOCKER severity," the audit parser in `parallel.py`
still records these as BLOCKER findings in the issue tracker. This causes
false-positive BLOCKERs that trigger unnecessary repair cycles.

Observed during astra-run-001 campaign:
- issue-004 (efficiency): "No BLOCKER issues" → recorded as BLOCKER
- issue-005 (functional): "No findings at BLOCKER severity" → recorded as BLOCKER
- issue-008 (efficiency): "INFO" severity → recorded as BLOCKER
- issue-010 (security): "Codebase is defensively written" → recorded as BLOCKER

### Fix

The audit parser needs to distinguish between:
1. An auditor reporting BLOCKER findings (should be recorded as BLOCKER)
2. An auditor reporting no findings or INFO/WARN only (should NOT be recorded as BLOCKER)

The parser likely matches too broadly on the audit output text. It should
only record BLOCKERs when the auditor explicitly flags findings as BLOCKER
severity, not when the auditor says "No BLOCKER" or "INFO".

### Note

Do not fix while a campaign is running — the parser is live infrastructure.
Fix after campaign completion and test with a targeted unit test.
