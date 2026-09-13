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

## BUG-0018: Virtual controllers not visible in Steam

**Severity:** Minor
**Component:** external / InputPlumber integration
**Date discovered:** 2026-09-13

### Description

Controller-box manager reports "2 virtual slots ready" and InputPlumber
is running, but the virtual controllers do not appear in Steam's
Controller Settings.

### Likely cause

This is most likely an InputPlumber configuration issue, not a
controller-box bug. InputPlumber may be creating virtual evdev devices
that Steam doesn't recognize, or the devices may not have the right
uinput/evdev attributes for Steam's controller detection. Could also be
a udev permissions issue on the virtual device nodes.

### Investigation needed

- Check if InputPlumber actually creates `/dev/input/event*` devices
  when controller-box requests virtual controllers
- Verify `evtest` sees the virtual devices
- Check if Steam needs the devices to have specific evdev key/abs bits
- May need InputPlumber target device type set to "xbox" (Steam's
  preferred controller layout)