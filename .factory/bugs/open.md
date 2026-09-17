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
---

## BUG-0024: System tray icon for overlay service

**Severity:** Enhancement
**Component:** overlay service / UX
**Date discovered:** 2026-09-16

### Description

The overlay service runs as a background process with no visible
indication that it's running. A system tray icon would:

1. Reassure the user the service is active
2. Provide menu access to settings, manager, or quit
3. Show status (connected to InputPlumber, number of controllers)

### Implementation notes

SDL2 does not natively support system tray icons. Options:
- Freedesktop StatusNotifierItem via DBus (works on KDE, GNOME with extension)
- AppIndicator via libappindicator
- Simple GTK tray icon

This is a UX enhancement, not a functional bug, but it significantly
improves usability for end users who need confirmation the service is running.

---

## BUG-0025: Overlay service finds no composite devices (comp_count=0)

**Severity:** Critical
**Component:** overlay service / InputPlumber integration
**Date discovered:** 2026-09-16

### Description

The overlay service connects to InputPlumber (dbus connected=1,
backend_ready=1) and reports "triggers registered, overlay ready", but
no poll ticks ever fire. Pressing Select+A does nothing. The service
runs silently until killed.

Debug logging shows:
- DBus connection succeeds
- Wire steps succeed (returns 0)
- But no poll events are generated (poll_count stays 0)

The most likely cause is comp_count=0: the service's reconcile_enumerate
found no composite devices from InputPlumber's ObjectManager, so
cbx_overlay_rearm_polls arms 0 polls, and the trigger registration loop
does nothing. The service reports "ready" because
overlay_wire_required_steps returns 0 even when comp_count is 0.

### Root cause hypothesis

InputPlumber v0.78.0 (nixpkgs) may not expose virtual controllers as
composite devices in the ObjectManager interface the way the code
expects. The service enumerates composites via
`org.freedesktop.DBus.ObjectManager.GetManagedObjects` at
`/org/shadowblip/InputPlumber`, filtering for
`org.shadowblip.Input.CompositeDevice` interfaces. If InputPlumber
doesn't create composite devices until a source device is assigned to
a target, or if the D-Bus interface names changed in v0.78, the
enumeration returns 0 composites.

### Why tests pass but real usage fails

All 91 tests pass because they use mock DBus (tests/dbus_mock.c) which
always returns the expected composite devices. Real InputPlumber may:
1. Not expose composites via ObjectManager at all in v0.78
2. Use different interface names or object paths
3. Require explicit composite device creation before enumeration
4. Need the manager to create/assign composites first

This is a fundamental testing gap: mock DBus tests don't validate real
InputPlumber integration.

### Fix approach (for the campaign)

1. **Diagnose**: Add a diagnostic mode that dumps what InputPlumber's
   ObjectManager.GetManagedObjects actually returns at runtime. Or
   use `dbus-send`/`gdbus` to inspect the real ObjectManager tree.

2. **Fix enumeration**: Compare what the code expects vs what InputPlumber
   v0.78 actually exposes. The interface name
   `org.shadowblip.Input.CompositeDevice` and object path prefix
   `/org/shadowblip/InputPlumber/CompositeDevice` may have changed.

3. **Add integration tests**: Create a test that connects to a real
   (or more realistic mock) InputPlumber instance and verifies the
   enumeration path works. The mock should simulate v0.78's actual
   ObjectManager behavior, not just return hardcoded test data.

4. **Fail loudly when comp_count=0**: The service should not report
   "overlay ready" when there are 0 composite devices. It should
   either wait for devices or log a clear error.

5. **Handle degraded mode gracefully**: If no composites are found,
   the service should enter a waiting state and re-enumerate when
   InputPlumber's NameOwnerChanged fires, rather than silently
   running with 0 polls.

---

## BUG-0026: Tests use mock DBus that doesn't match real InputPlumber behavior

**Severity:** Major
**Component:** testing infrastructure
**Date discovered:** 2026-09-16

### Description

The entire test suite (91 tests) uses mock DBus (tests/dbus_mock.c) that
returns hardcoded responses. This means tests pass even when the code
doesn't work with real InputPlumber. Multiple bugs (BUG-0020 overlay
not appearing, BUG-0021 D-pad navigation, BUG-0022 B button conflict,
BUG-0025 no composite devices) were not caught by tests because the
mock doesn't simulate real InputPlumber behavior.

### Root cause

The mock DBus (tests/dbus_mock.c) is a test harness that returns
predefined responses for DBus method calls. It doesn't:
- Simulate ObjectManager.GetManagedObjects with realistic data
- Simulate intercept mode state transitions
- Simulate signal emission (InputEvent, PropertiesChanged)
- Simulate the trigger activation flow
- Match InputPlumber v0.78's actual interface/path structure

### Fix approach (for the campaign)

1. Create an integration test that runs against a real InputPlumber
   instance (or a more realistic mock that reads actual interface
   definitions from InputPlumber's D-Bus XML)

2. Add a test mode that connects to the system bus and verifies
   the enumeration, trigger registration, and intercept mode flow
   against a running InputPlumber (skip with exit 77 if unavailable)

3. The native DBus tests (test_native_dbus.c, test_overlay_native.c)
   use a private DBus server, but it still uses the mock — make it
   use a more realistic InputPlumber simulation

4. Consider adding a "smoke test" that actually launches InputPlumber
   (from nixpkgs) in a test fixture and runs the overlay service
   against it
