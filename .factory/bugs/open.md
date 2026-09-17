# Open Bugs

## BUG-0016: Profile editor only shows A, B, and D-pad directions

**Severity:** Major
**Component:** manager / profile editor
**Date discovered:** 2026-09-13
**Status:** RESOLVED by campaign task 15 — verify after rebuild

### Description

The profile editor's list mode only displayed the 6 mappings that exist in the
shipped default profile. Task 15 was completed by the campaign — the editor
now enumerates all 17 supported buttons including unbound entries.

### Fix verification

After pulling and rebuilding, open the manager, go to profiles tab, edit a
profile. All 17 buttons should appear (A, B, X, Y, Start, Select, Guide, L1,
R1, L2, R2, L3, R3, D-Pad Up/Down/Left/Right).

---

## BUG-0017: Profile editor capture mode doesn't register input

**Severity:** Major
**Component:** manager / profile editor
**Date discovered:** 2026-09-13
**Status:** RESOLVED by campaign task 16 — verify after rebuild

### Description

When the user selects "Configure" on a binding and enters capture mode,
pressing any button on the controller did nothing. Three compounding defects:
InterceptMode never enabled, composite device path was NULL, and sender
verification silently dropped all signals.

### Fix verification

After pulling and rebuilding, enter capture mode on a binding and press a
button on the controller. The capture should register. B should skip
(without being captured as a binding) and Start should cancel.

---

## BUG-0018: Virtual controllers not visible in Steam (RESOLVED — not a bug)

**Severity:** N/A
**Component:** external / Steam configuration
**Date discovered:** 2026-09-13
**Date resolved:** 2026-09-13

### Resolution

Not a controller-box bug. `evtest` confirms InputPlumber creates the virtual
devices at the kernel level as "Microsoft X-Box 360 pad" evdev devices.
Steam needs to be restarted to rescan for new controllers.

---

## BUG-0019: Audit parser records "no BLOCKER" findings as BLOCKERs

**Severity:** Major
**Component:** factory harness / parallel.py audit parser
**Date discovered:** 2026-09-13
**Status:** RESOLVED — parser now requires explicit positive BLOCKER markers

---

## BUG-0020: Overlay does not appear when pressing Select+A

**Severity:** Major
**Component:** overlay service
**Date discovered:** 2026-09-16

### Description

The overlay service does not appear when pressing Select+A on a connected
controller. The user has InputPlumber running and virtual controllers
visible in evtest, but the overlay never shows.

### Possible causes

1. The overlay service may not be running (`controller-box --overlay-service`)
2. InterceptMode activation may be failing — the overlay needs to set
   intercept mode to ALL/GAMEPAD_ONLY on the composite device to receive
   InputEvent signals
3. The trigger combo (Select+A) may not be reaching the overlay service
4. SDL2 may not be able to create a window/overlay in the user's display
   environment (X11/Wayland/Gamescope)

### Investigation needed

- Check if the overlay service is running and producing any log output
- Check if InputPlumber's intercept mode is being activated
- Check SDL2 video driver — the overlay needs a display to render to
- Try running with `SDL_VIDEODRIVER=x11` or check Wayland support

---

## BUG-0021: D-pad left/right navigates tabs instead of being captured

**Severity:** Major
**Component:** manager / profile editor
**Date discovered:** 2026-09-16

### Description

When in the profile editor (capture or list mode), pressing left or right
on the D-pad changes the active tab/menu instead of being captured or
navigating the binding list. This interrupts the editing process entirely.

### Root cause hypothesis

The manager's tab-level input handler is intercepting D-pad left/right for
tab navigation before the profile editor can handle them. The editor's input
handler needs to consume these events when it's active, preventing them
from bubbling to the tab navigation.

### Fix

When the profile editor is active (list mode, capture mode, or sequential
mode), the manager should route D-pad events to the editor first. Only if
the editor doesn't consume them should they bubble to tab navigation.

---

## BUG-0022: B button can't be mapped because it also means cancel

**Severity:** Major
**Component:** manager / profile editor
**Date discovered:** 2026-09-16

### Description

The B button serves dual purpose: it's used for "cancel/back" navigation
throughout the manager, but it's also a mappable button in profiles. When
trying to map B in capture mode, pressing B cancels the capture instead of
registering the binding.

### Status

Task 16 (completed by campaign) was supposed to address this: "Preserve
B-skip/Start-cancel affordances without confusing navigation streams with
captured binding input or making required B impossible to bind." Needs
verification after rebuild — if the campaign's fix doesn't work, this bug
is still open.

### Fix (if still broken)

In capture mode, B should be captured as a binding only when B is the
button being mapped. When B is NOT the target button, B should cancel.
The sequential mode already handles this ("B skips and Start cancels —
but only while they are not the button being captured").

---

## BUG-0023: Active profile not visible in the manager

**Severity:** Minor
**Component:** manager / UI
**Date discovered:** 2026-09-16

### Description

The manager does not clearly show which profile is currently active for
each controller. The user can't tell at a glance which profile is routed
to which controller slot.

### Fix

The controllers tab should display the active profile name for each
controller slot. The code has `controllers_tab.c:610` which can retrieve
the profile routed through a target path, but it may not be displayed
prominently in the UI.