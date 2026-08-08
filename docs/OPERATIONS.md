# Operations

Controller-Box runs as a **systemd user service** that communicates with
InputPlumber over the **system DBus**. This document covers service
architecture, management commands, configuration, performance expectations,
and troubleshooting.

## Service architecture

```
InputPlumber (system DBus service — input engine)
    ↕ native DBus readiness and owner-change recovery
controller-box.service (graphical-session user service)
```

- **InputPlumber** (`inputplumber.service`, system): owns evdev grab, virtual
  devices, event translation, profiles, intercept mode, and player ordering.
  Controller-Box never touches input routing directly — every state change goes
  through InputPlumber's DBus API.
- **Controller-Box** (`controller-box.service`, user): graphical-session
  overlay service with bounded `Restart=on-failure` backoff. A user unit must
  not order against or require a differently managed system unit. Instead,
  Controller-Box validates InputPlumber's Manager Version and native DBus
  contract, enters degraded mode when unavailable, and recovers on owner
  acquisition without restarting.

**Install order:** (1) InputPlumber, (2) Controller-Box, (3) enable the overlay
service.

If InputPlumber is not running, both applications retain the system-bus
connection in **degraded mode**. Backend-changing Manager controls are
unavailable, and the overlay remains hidden. `NameOwnerChanged` for the
InputPlumber name triggers Version validation, re-enumeration, target/profile
reconciliation, signal subscription, and poll re-arming without process
restart.

## Manager input methods

The manager supports two input paths:

- **Controller (primary):** Left/Right switches tabs, Up/Down navigates
  within a panel, A activates the focused control, B cancels.
- **Mouse (secondary):** Move the mouse over any visible control to hover it,
  left-click to activate. Click on a tab to switch tabs, on a list item to
  select it, or on a button to press it. Focus follows the pointer, so
  keyboard navigation resumes from the last-clicked widget.

Both paths invoke the same widget handlers and validation. Invisible widgets
(e.g., hidden type pickers) do not intercept pointer events.

## Profile editor

The profile editor (§5.4) is opened from the Profiles tab:

- **Edit existing:** Select a profile in the list and click Edit (or navigate
  to the Edit button with Up/Down and press A).
- **Create new:** Click Create, choose a source (Default copy / Empty /
  Clone), type a name, and press A — the editor opens with the new
  in-memory profile. No file is written until you save from the editor.

In the editor (list mode):

- **Up/Down** scrolls the binding list; the diagram highlights the
  corresponding button.
- On an Empty profile, **A** starts sequential capture so the first binding
  is reachable without an existing list row. Otherwise, **A** opens a binding
  edit sub-menu with three options:
  1. **Pick Target** — choose a target event from the device's capabilities.
  2. **Capture** — wait for a physical button press (via DBus InputEvent).
  3. **Sequential (All Buttons)** — prompt for each button in order.
- Activate the visible **Save** button (or press **B** in list mode) to
  validate, persist, and close. Validation and filesystem failures stay
  visible and recoverable in the editor.
- Activate **Discard** (or press **Start**) to close without writing changes.

In sequential mode:

- The diagram lights up the current button; press a physical button to
  capture it and auto-advance.
- **B** skips the current button.
- **Start** cancels sequential mode.

Capture and sequential mode subscribe to DBus InputEvent signals. The
subscription verifies the signal sender against InputPlumber's unique bus
name (e.g. `:1.42`), not the well-known name — signals from other processes
are rejected.

## Systemd management

The overlay service runs as a **user service** (not system-wide). The manager
installs it on first run; you can also manage it manually:

```bash
# Check status
systemctl --user status controller-box

# Start / stop / restart
systemctl --user start controller-box
systemctl --user stop controller-box
systemctl --user restart controller-box

# Enable at boot (done by manager on first run)
systemctl --user enable --now controller-box

# Disable
systemctl --user disable controller-box

# View logs
journalctl --user -u controller-box -f
```

### Service unit file

The service unit is written to `~/.config/systemd/user/controller-box.service`
by the manager. It contains:

```ini
[Unit]
Description=Controller-Box Overlay Service
After=graphical-session.target
PartOf=graphical-session.target

[Service]
ExecStart=/usr/bin/controller-box --overlay-service
Restart=on-failure
RestartSec=2s

[Install]
WantedBy=graphical-session.target
```

Under Flatpak, `ExecStart` uses `flatpak run org.shadowblip.ControllerBox
--overlay-service` and systemctl calls go through `flatpak-spawn --host`.

### Group membership

If your system uses polkit for DBus authorization, add your user to the
`inputplumber` group:

```bash
sudo usermod -aG inputplumber $USER
# Log out and back in for group changes to take effect.
```

The manager checks group membership during service installation and shows a
warning with this guidance if the user is not in the group. Installation
proceeds regardless — the group check is advisory.

## InputPlumber dependency

InputPlumber is a separate package and a hard prerequisite. Install it first
from its own Flatpak or system package. Controller-Box's service unit will not
start until InputPlumber is available.

Controller-Box communicates with InputPlumber via:

- **Bus:** system DBus (`sd_bus_open_system()`)
- **Well-known name:** `org.shadowblip.InputPlumber`
- **Root path:** `/org/shadowblip/InputPlumber`
- **Manager interface:** `org.shadowblip.InputManager` at
  `/org/shadowblip/InputPlumber/Manager`

See [DBus-API.md](DBus-API.md) for the full API reference.

### Overlay service startup

The overlay service (`controller-box --overlay-service`) follows this
initialization sequence on startup:

1. **SDL video init** — creates a hidden SDL2 window and renderer
   (1280×720). If SDL cannot initialize (e.g., no display driver
   available), the service logs an error to stderr and exits non-zero.
2. **DBus connection** — connects to the system bus and verifies that
   InputPlumber is running. If InputPlumber is not found, the service
   logs `InputPlumber not found on system DBus` to stderr and exits
   non-zero.
3. **Device enumeration** — calls `GetManagedObjects` to discover all
   composite devices, source devices, and target devices.
4. **Settings + assignments** — loads `settings.yaml` and
   `assignments.yaml` from `~/.config/controller-box/` (best-effort;
   defaults are used if files are absent).
5. **Surface pre-build** — creates a target-texture overlay surface at
   the configured opacity, builds the selection grid from composites +
   settings + assignments, and pre-renders it. This ensures the overlay
   appears in <10 ms when activated.
6. **Trigger registration** — registers the overlay trigger combo
   (default `Select+A`) on every composite device via
   `SetInterceptActivation`, then sets `InterceptMode = PASS`.
7. **Lifecycle init** — initializes the overlay state machine
   (`IDLE → ACTIVATING → VISIBLE → CLOSING → IDLE`).
8. **Poll loop** — enters the main event loop (50 ms interval, DEC-002).
   The loop polls `InterceptMode` via `ip_intercept_poll` (one per
   composite device), processes SDL events for grid navigation, and
   handles `SIGTERM`/`SIGINT` for clean shutdown.

**Poll loop behavior (Task 4):**

- **InterceptMode polling:** Each composite device has an
  `ip_intercept_poll` state machine with a 50 ms SDL timer. When
  `InterceptMode` transitions to `ALL` (activation), the poll fires
  `cbx_overlay_lifecycle_activate()`, which shows the pre-built surface
  via `SDL_RenderCopy` + `SDL_RenderPresent` (<10 ms target).
- **Input processing:** While the overlay is visible, SDL keyboard events
  drive grid navigation: Left/Right moves slot, Up/Down cycles profile,
  R3 toggles Host Mode, B closes. In production, DBus `InputEvent`
  signals from InputPlumber carry per-controller input.
- **Close sequence:** On B press or deactivation, `cbx_overlay_lifecycle_close()`
  fires the `on_save` callback (conflict detection → conflict resolution
  → assignment save), sets `InterceptMode = PASS`, and hides the surface
  (not destroyed) for instant re-activation.
- **Signal handling:** `SIGTERM` and `SIGINT` set a shutdown flag that
  causes the poll loop to exit cleanly: stops all poll timers,
  force-closes the overlay if visible, destroys the surface, cleans up
  caches, disconnects DBus, and shuts down the renderer. Exit code 0.

**Prerequisites:**
- InputPlumber must be running and accessible on the system DBus.
- A display or dummy video driver must be available for SDL.
- A system TTF font (e.g., DejaVuSans) is recommended for text rendering
  (best-effort — the service runs without a font but shows no text).

**Dry-run mode:** `controller-box --overlay-service --dry-run` prints a
banner and exits 0 without performing any initialization. This is
headless-safe for acceptance checks.

**Failure modes:**
- SDL init failure → exit 1, stderr message (no crash).
- InputPlumber not found → exit 1, stderr message.
- Device enumeration failure → exit 1, stderr message.
- Missing settings/assignments → defaults used, service continues.

## Configuration

### Config directory

`~/.config/controller-box/` holds Controller-Box's own configuration. It is
created automatically with mode 0700 on first use.

### settings.yaml

App-level settings, written atomically (mode 0600):

```yaml
overlay_trigger: "Select+A"
launch_at_boot: true
theme: "default"
overlay_opacity: 0.85
virtual_controllers:
  count: 4
  types: [xb360, xb360, xb360, xb360]
icon_overrides: []
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `overlay_trigger` | string | `"Select+A"` | The single hotkey to open the overlay |
| `launch_at_boot` | bool | `true` | Start overlay service at login |
| `theme` | string | `"default"` | Theme name (format TBD, see deferred items) |
| `overlay_opacity` | float | `0.85` | Overlay transparency (0.0–1.0) |
| `virtual_controllers.count` | int | `4` | Number of virtual controllers on startup (1–16) |
| `virtual_controllers.types` | list[string] | `[xb360, xb360, xb360, xb360]` | Per-slot virtual types |
| `icon_overrides` | list | `[]` | Per-type icon overrides (max 16 entries) |

Known controller types: `xb360`, `ds5`, `deck`, `gamepad`, `mouse`, `keyboard`,
`touchscreen`. The full list is dynamic — the manager populates the type picker
from InputPlumber's `SupportedTargetDeviceIds` DBus property.

### assignments.yaml

Auto-assignment table and gamepad order persistence (mode 0600):

```yaml
assignments:
  - id: "BT:AB:CD:01:EF:23"
    slot: 0
    profile: "fighting"
  - id: "USB:SN12345"
    slot: 1
    profile: ""
gamepad_order:
  - "BT:AB:CD:01:EF:23"
  - "USB:SN12345"
```

The `gamepad_order` field is a workaround for DBus gap #2: InputPlumber's
`Manager.GamepadOrder` property is in-memory only and resets on daemon restart.
Controller-Box saves the order keyed by `PersistentId` and re-applies it after
restart.

**Identity ID prefixes:**

| Prefix | Identity layer | Stability |
|--------|---------------|-----------|
| `BT:` | Bluetooth MAC (evdev `uniq`) | Stable, unique |
| `USB:SN` | USB serial | Stable when present |
| `USB:phys:` | USB port path | Stable only if same port |
| `ORDER:` | Connection order (fallback) | Session-level |

See [PROFILES.md](PROFILES.md) for the profile format and the controller
identification model.

### profile-metadata/

Optional sidecar files per profile (e.g., `fighting.meta.yaml`):

```yaml
display_name: "Fighting Profile"
icon: "cc-ps5"                    # built-in icon name
# icon: "/path/to/custom.png"    # or absolute path to custom PNG
display_order: 1
description: "Tournament fighting setup"
```

## Icon mapping

Controller-Box maps InputPlumber `DeviceType` strings to SVG icons using
`/usr/share/controller-box/controller-icons.yaml`. The mapping file is a YAML
list of entries with `type`, `icon`, and `name` fields.

```yaml
virtual_types:
  - type: "xb360"
    icon: "cc-xbox-360"
    name: "Xbox 360 Controller"
  - type: "ds5"
    icon: "cc-ps5"
    name: "DualSense"
  - type: "deck"
    icon: "cc-steam-deck"
    name: "Steam Deck Controller"
  - type: "gamepad"
    icon: "generic-gamepad"
    name: "Generic Gamepad"
custom_icons:
  - icon: "arcade-stick"
    name: "Arcade Stick"
  - icon: "hitbox"
    name: "Hit Box"
```

At runtime, `cbx_icon_map_load()` parses the YAML, and
`cbx_icon_map_lookup(type, ...)` returns the icon name and display name for a
given `DeviceType`. Unknown types fall back to `generic-gamepad` with the raw
type string as the label.

SVG files live in `/usr/share/controller-box/icons/svg/`. Controllercons
icons are prefixed `cc-` (e.g. `cc-xbox-360`, `cc-ps5`). Custom icons use
plain names (`steam-deck`, `generic-gamepad`, `arcade-stick`, `hitbox`,
`mouse`, `keyboard`).

To add a new device type mapping, append an entry to `controller-icons.yaml`
under `virtual_types:`. To add a new icon, place the SVG in the icons directory
and reference it by filename (without `.svg`).

### Icon override (profile sidecar)

Each profile can override its icon via the metadata sidecar:

```yaml
icon: "cc-ps5"                 # built-in icon name
icon: "/path/to/custom.png"    # absolute path to custom image
```

Resolution precedence:

1. **Profile icon override** (from sidecar metadata):
   - Absolute path (`/...`) → loads custom PNG via SDL2_image. The path is
     validated: no `..` traversal, `realpath()` must resolve within a safe
     directory (user config dir, user data dir, or system data dir). If
     validation fails, falls back to step 2.
   - Built-in icon name (e.g. `cc-ps5`) → looked up in the icon cache.
2. **Icon map lookup** by `DeviceType` string → icon name + display name.
3. **Unknown DeviceType** → `generic-gamepad` silhouette + raw type string.

Icons show the **virtual controller type** (what the game sees), not the
physical controller — no VID:PID lookup needed.

## DBus gaps and workarounds

Five InputPlumber DBus API gaps were identified during implementation. All have
workarounds; no upstream changes are required for v1.

| # | Gap | Workaround |
|---|-----|------------|
| 1 | No `PropertiesChanged` signal for `InterceptMode` | Poll at 50 ms interval (DEC-002); track state via state machine with timeout handling |
| 2 | `GamepadOrder` not persisted (resets on daemon restart) | Save to `assignments.yaml` keyed by `PersistentId`; re-apply after restart |
| 3 | `CreateCompositeDevice` requires a YAML file path (no string variant) | Write temp YAML via `mkstemp` (mode 0600), pass path, unlink after call |
| 4 | No DBus method to enumerate profiles/configs on disk | Read filesystem directly: `~/.local/share/inputplumber/profiles/`, `/usr/share/inputplumber/profiles/`, `/usr/share/inputplumber/devices/`, `/usr/share/inputplumber/capability_maps/` |
| 5 | No DBus method to add/remove source devices on running composites | Not needed for v1; InputPlumber auto-manages composites from device configs |

See [DBus-API.md](DBus-API.md) for the full DBus API reference.

## Performance expectations

| Metric | Target | Mechanism |
|--------|--------|-----------|
| Overlay appearance | **<10 ms** (button press → visible) | Pre-built surface in memory; icons pre-rasterized via nanosvg at startup; incremental dirty-rect rendering |
| Gameplay input latency | **~1–2 ms** | InputPlumber intercept overhead only; DBus is a side branch, never inline during gameplay |
| Overlay close → game input | **<1 ms** | Single `InterceptMode` → PASS; overlay hidden, not destroyed |
| Daemon footprint | Always resident, no measurable impact | SDL2 minimal memory; idles on DBus signals + 50 ms poll |
| Player reorder | Atomic, InputPlumber-managed | `GamepadOrder` setter suspends all, resumes in new order with 100 ms stagger |

The `InterceptMode` poll interval is 50 ms (DEC-002), yielding ~51 ms worst-case
detection. Detection latency is bounded by the poll interval; the render path
itself is <1 ms.

## Bug maintenance

GitHub and Forgejo issues are optional external references. The portable,
canonical workflow state is `open-bugs.md` and `closed-bugs.md`; never put PATs
or credential-bearing URLs in either ledger. Use `scripts/bug-ledger.py` for
validated intake, links, transitions, closure evidence, and interrupted-close
recovery.

An ordinary defect is triaged and handled with:

```bash
./scripts/ralph-maintenance-plan.sh BUG-0001
./scripts/ralph-maintenance-run.sh
```

One cycle handles one bug and runs the configured project verifier before
closure. Fresh specification and maintenance planning atomically seed minimal
plan/scratchpad state, leaving completed plans only in Git history; `--resume`
preserves the active draft, and planning gates reject carried-over non-pending
tasks. If expected behavior requires a product decision or specification
change, block maintenance and return to the human specification workflow.
Recovery modes are `maintenance-planning` and `maintenance`. See
[BUG_WORKFLOW.md](BUG_WORKFLOW.md) for the complete process.

## Troubleshooting

### InputPlumber not detected

The **manager** enters degraded mode if InputPlumber is not running (empty
device list, functional buttons). The **overlay service** exits with code 1.
Check:

```bash
systemctl status inputplumber    # system service
```

If InputPlumber is not installed, install it first. The manager will
auto-connect when InputPlumber starts (via `NameOwnerChanged` signal).
Restart the overlay service after InputPlumber is running:

### Overlay service won't start

```bash
systemctl --user status controller-box
journalctl --user -u controller-box --no-pager -n 50
```

Common causes: InputPlumber not running (the `Requires=` directive blocks
start), or the user is not in the `inputplumber` group (DBus access denied).

### DBus access denied

If the DBus connection returns `AccessDenied`, add your user to the
`inputplumber` group:

```bash
sudo usermod -aG inputplumber $USER
# Log out and back in.
```

### GamepadOrder not persisting after restart

This is expected behavior (DBus gap #2). Controller-Box saves the order to
`assignments.yaml` and re-applies it automatically after InputPlumber restarts.
If the order is not restored, check that `assignments.yaml` contains
`gamepad_order` entries with valid `PersistentId` values.

### InterceptMode stuck

If the overlay appears but input doesn't route to the game on close, the
`InterceptMode` property may be stuck at `ALL`. The poll state machine has a
timeout that fires after consecutive ticks with no mode change. Restart the
overlay service:

```bash
systemctl --user restart controller-box
```

### Icons not showing

Check that `/usr/share/controller-box/controller-icons.yaml` exists and is
valid YAML. Unknown device types show the `generic-gamepad` silhouette — this
is expected, not an error. To add a mapping, see the icon mapping section
above.

### Overlay trigger not working

Ensure the trigger combo is registered on each composite device. The default
is `Select+A` (configurable in Settings). The combo is set via
`SetInterceptActivation` on each composite device. If you add a new virtual
controller, restart the overlay service to re-register the trigger.

### Flatpak: systemctl not found

Under Flatpak, systemctl calls go through `flatpak-spawn --host systemctl
--user`. If this fails, ensure the `org.freedesktop.Flatpak` talk-name
permission is present (it is in the manifest). If the host system doesn't have
systemd user services, the overlay service cannot be auto-installed — use the
tarball install instead.

### Profile validation fails

Profiles must bind at least A, B, D-Pad Up, D-Pad Down, D-Pad Left, and D-Pad
Right (the NES minimum). The manager shows which bindings are missing. See
[PROFILES.md](PROFILES.md) for the full profile format.

## Golden image workflow

The `test_golden` ctest compares live deterministic framebuffer captures
against reviewed baseline PNG images in `tests/golden/`.  This catches
unintended visual regressions across all overlay and manager UI states.

### Tolerance values

- **Per-pixel tolerance**: ±3 per RGB channel (accounts for minor renderer
  rounding differences)
- **Per-image tolerance**: <2% of total pixels may differ (allows small
  anti-aliasing or font hinting variations)

### Baseline images

Baselines live in `tests/golden/` and cover every visual state from the
overlay (§4.10) and manager (§5.6) visual tests:

| Image | Dimensions | State |
|-------|------------|-------|
| `overlay_player_mode.png` | 800×600 | 3 controllers on P1, P2, P3 |
| `overlay_host_mode.png` | 800×600 | Row 1 moved to P1 (conflict) |
| `overlay_conflict.png` | 800×600 | Rows 0+1 on P1, row 2 unassigned |
| `overlay_unassigned.png` | 800×600 | Rows on P1, P2, Unassigned |
| `manager_controllers_degraded.png` | 1280×720 | Controllers tab, no DBus |
| `manager_controllers_connected.png` | 1280×720 | Controllers tab, mock devices |
| `manager_profiles.png` | 1280×720 | Profiles tab |
| `manager_settings.png` | 1280×720 | Settings tab |
| `manager_editor_list.png` | 1280×720 | Profile editor, binding list |
| `manager_editor_sequential.png` | 1280×720 | Profile editor, 3/6 captured |
| `manager_editor_validation_error.png` | 1280×720 | Profile editor, red error text |

### Regenerating baselines

Baseline update is a **manual, reviewed commit** — the test never auto-updates
baselines.  To regenerate:

```bash
scripts/generate-golden.sh
```

This builds the `test_golden` target and runs it with `CBX_GENERATE_GOLDEN=1`,
writing PNGs to `tests/golden/`.  Review the images, then commit them if
correct.

Alternatively, run manually:

```bash
nix-shell --run "cmake --build build-check --target test_golden"
CBX_GENERATE_GOLDEN=1 SDL_VIDEODRIVER=dummy \
  ctest --test-dir build-check -R test_golden --output-on-failure
```

### Failure artifacts

On mismatch, the test writes three PNGs to `tests/golden-fail/`:

- `<name>.actual.png` — the live capture
- `<name>.expected.png` — the golden baseline
- `<name>.diff.png` — red for differing pixels, dimmed for matches

This directory is gitignored and should not be committed.

## Backend smoke test

The `test_backend_smoke` ctest (SPEC §11.1.6) verifies that rendering
through an **accelerated** SDL2 backend (OpenGL or OpenGL ES) produces
correct pixel output — not just successful draw calls, but actual
framebuffer content verified via `fb_read_pixels`.

### What it does

1. Creates an SDL2 renderer with `SDL_RENDERER_ACCELERATED |
   SDL_RENDERER_TARGETTEXTURE`.
2. Detects the backend name via `SDL_GetRendererInfo`.
3. If no accelerated backend is available, exits with code 77
   (ctest `SKIP_RETURN_CODE`) — the test is skipped, not failed.
4. If an accelerated backend is available:
   - Renders a representative overlay grid frame via
     `cbx_overlay_surface_render` + `cbx_select_grid_render_cb`.
   - Renders a manager tab frame via `cbx_manager_render()`.
   - Reads back pixels via `fb_read_pixels`.
   - Asserts `fb_region_has_content` in expected regions (grid cells,
     tab bar, body, buttons).
   - Asserts no all-black or all-background frames.
   - Compares the accelerated output against the software-renderer
     golden baselines (`fb_golden_compare` with ±3 per-channel,
     <2% image tolerance).

### Hardware requirements

This test requires a real display with GPU acceleration.  It does
**not** set `SDL_VIDEODRIVER=dummy`.  In headless CI environments
(no display), the test skips (exit 77).

### Running

```sh
nix-shell --run "cmake --build build-check --target test_backend_smoke"
nix-shell --run "ctest --test-dir build-check -R test_backend_smoke --output-on-failure"
```

On a headless machine, the output will show:
```
test_backend_smoke: SDL_Init failed: No available video device
```
and ctest reports the test as Skipped.

On a machine with a GPU, the test renders both overlay and manager
frames through the accelerated backend and verifies pixel content
against the golden baselines.

## Installed production smoke test

The `test_installed_smoke` ctest (SPEC §11.1.5) exercises the real
`main()` entry point of the **installed** binary (not `--dry-run`)
under a headless X11 server (Xvfb).  It verifies that the binary
builds, installs, launches, renders a visible window, responds to
keyboard input, and produces a non-blank framebuffer capture.

### What it does

1. Builds the binary and installs it to a staging prefix (`.test-install`).
2. Starts Xvfb on display `:99` (1280×720×24).
3. Sets `DISPLAY=:99` and `SDL_VIDEODRIVER=x11`.
4. Sets up a temporary HOME with DejaVuSans.ttf so the manager can
   render text.
5. **Manager mode**: launches `controller-box --manager`, sends Tab
   and Arrow key presses via `xdotool` (keyboard tab switching and
   controller-proxy navigation), captures the root window via
   `import -window root` (ImageMagick), and verifies pixel variance
   (mean > 5.0 on 0–255 scale) in both the tab-bar region (top 48px)
   and the body region (below 48px).
   Then exercises **coordinate-based mouse clicks** on manager body
   controls at known pixel positions: the Profiles tab control
   (639, 24), the Settings tab control (1065, 24), the first settings
   list item (100, 90), and the Save button (116, 522).  Each click is
   verified by comparing body-region screenshots before and after
   (ImageMagick difference mean) for visible state change, or by
   checking that `$HOME/.config/controller-box/settings.yaml` was
   created or mutated (file mutation from the Save button).
6. **Overlay service mode**: launches `controller-box --overlay-service`.
   If InputPlumber is available on the system DBus, the service runs
   and is terminated via SIGTERM.  If InputPlumber is unavailable
   (expected in test environments), the service exits cleanly with
   code 1 (not a crash).
7. Cleans up Xvfb and temporary files.

### Prerequisites

- **Xvfb** (`xorg.xorgserver` in nix-shell)
- **xdotool** (keyboard input injection)
- **ImageMagick** (`import` for screenshots, `convert` for pixel
  variance analysis)
- **bc** (floating-point arithmetic for threshold checks)

If any of these tools is unavailable, the test exits with code 77
(ctest `SKIP_RETURN_CODE`) and is reported as Skipped.

### Running

```sh
nix-shell --run "ctest --test-dir build-check -R test_installed_smoke --output-on-failure"
```

### Output

A successful run shows:
```
PASS: all required tools available (Xvfb, xdotool, import, convert)
PASS: installed binary: .test-install/bin/controller-box
PASS: installed --version: controller-box 0.1.0
PASS: Xvfb running (PID ...)
PASS: font available: ...
PASS: manager launched and running (PID ...)
PASS: keyboard input sent (Tab, Arrow keys)
PASS: screenshot captured: ...
PASS: tab bar region is non-blank (mean=...)
PASS: body region is non-blank (mean=...)
PASS: profiles tab click produced visible state change (diff=...)
PASS: settings tab click produced visible state change (diff=...)
PASS: settings list item click produced visible state change (diff=...)
PASS: Save button click mutated settings file (mtime increased, size=...)
PASS: coordinate-based mouse clicks completed
PASS: manager terminated cleanly
PASS: overlay service exited cleanly (code 1: InputPlumber not found)
PASS: all installed smoke test checks passed
```

If Xvfb or ImageMagick is not installed:
```
SKIP: required tool 'Xvfb' is not installed
```
(ctest reports the test as Skipped, not Failed.)

## Visual framebuffer tests

The `test_overlay_visual` and `test_manager_visual` ctests (SPEC §11.1.1–2)
are the foundation of the visual acceptance suite.  They render through the
**same production composition path** used by the real binary and assert on
actual pixel content — not struct fields, geometry, or visibility flags.

### test_overlay_visual (SPEC §4.10)

Renders 7 overlay states through `cbx_select_grid_build()` →
`cbx_overlay_surface_init()` → `cbx_overlay_surface_render()` →
`fb_read_pixels()`, then asserts:

1. **Player Mode grid** — content in grid cells, text regions, icon regions
2. **Host Mode differs** — `fb_frames_differ` between Player and Host Mode
3. **Conflict highlighting** — red `{220,40,40}` in conflicted cell, not in
   non-conflicted cell
4. **Unassigned + ≥2 columns** — content in all column headers + ≥2 player
   slot regions
5. **Controller model/profile text** — text-colored pixels in label regions
6. **Virtual-device icons** — content in icon regions for each occupied slot
7. **State transitions differ** — no-conflict→conflict frames differ,
   same-state frames don't differ (deterministic rendering)

### test_manager_visual (SPEC §5.6)

Renders 9 manager states through `cbx_manager_init()` →
`cbx_manager_render()` → `fb_read_pixels()`, then asserts:

1. **Controllers tab (degraded)** — content in device list + 3 buttons + body
2. **Controllers tab (connected)** — mock DBus devices, content in all regions
3. **Connected vs degraded differ** — `fb_frames_differ`
4. **Profiles tab** — content in profile list + create/edit/delete buttons
5. **Settings tab** — content in settings list + save button + text pixels
6. **Tab switch differs** — `fb_frames_differ` between all 3 tabs
7. **Profile editor (list mode)** — content in diagram + binding list + title
8. **Profile editor (sequential mode)** — prompt + progress bar content
9. **Profile editor (validation error)** — red text in status region

### test_fb_assert

Self-test for the `fb_assert.c` framebuffer assertion library.  Renders a
known colored rectangle via SDL software renderer, reads back pixels, and
verifies `fb_region_has_content`, `fb_region_has_color`, `fb_golden_compare`,
and `fb_frames_differ` all behave correctly.

### Running

```sh
nix-shell --run "ctest --test-dir build-check -R 'test_overlay_visual|test_manager_visual|test_fb_assert' --output-on-failure"
```

All three tests use `SDL_VIDEODRIVER=dummy` (software renderer) and run in
headless environments.

## Running all visual acceptance tests

The complete visual acceptance suite (layers 1–6) runs as part of the full
ctest suite:

```sh
nix-shell --run './scripts/verify-project.sh'
```

Or run just the visual layers:

```sh
nix-shell --run "ctest --test-dir build-check -R 'test_fb_assert|test_overlay_visual|test_manager_visual|test_golden|test_backend_smoke|test_installed_smoke' --output-on-failure"
```

Expected results in a headless environment (no GPU, no Xvfb):

- `test_fb_assert`: PASS (9 sub-tests)
- `test_overlay_visual`: PASS (7 sub-tests)
- `test_manager_visual`: PASS (9 sub-tests)
- `test_golden`: PASS (11 sub-tests)
- `test_backend_smoke`: Skipped (exit 77 — no GPU)
- `test_installed_smoke`: PASS (requires Xvfb/xdotool/ImageMagick in nix-shell)

## Human release acceptance checklist (SPEC §11.1.7)

Before promotion to `main`, a human reviews representative manager and overlay
captures on **target hardware** for qualities that automation cannot assess.
Automation catches missing or divergent output; it does not approve aesthetics.

### Procedure

1. **Build and install** on target hardware:
   ```sh
   cmake -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build
   sudo cmake --install build
   systemctl --user start controller-box
   ```

2. **Capture representative frames** — one for each major state:
   - Overlay: Player Mode, Host Mode, conflict, unassigned
   - Manager: Controllers (connected), Profiles, Settings, Profile editor
     (list mode), Profile editor (sequential mode)
   - Use `import -window root` (ImageMagick) or a screenshot tool to capture
     each state.

3. **Review each capture** against the following checklist:

   | Criterion | What to check |
   |-----------|---------------|
   | **Legibility** | Is all text readable at the target display resolution and viewing distance? Are fonts rendered correctly (no missing glyphs, no overflow)? |
   | **Clipping** | Does any text, icon, or widget extend beyond its container? Are grid cells fully visible without truncation? |
   | **Focus indication** | Is the currently focused element clearly distinguishable? Is the highlight color visible against the background? |
   | **Contrast** | Is there sufficient contrast between text and background, between focused and unfocused elements, between conflict-red and normal cells? |
   | **Controller-only usability** | Can every action be performed with only a controller (no keyboard/mouse fallback needed)? Navigate all tabs, edit a profile, resolve a conflict, and adjust settings using only D-pad and face buttons. |

4. **Document the review**: record the reviewer name, date, hardware,
   display resolution, and any issues found. File issues for any failing
   criterion before promotion.

5. **Store evidence**: the captured screenshots and the exact commands that
   produced them are part of final verification evidence (SPEC §11.1 closing
   mandate). Keep them in the release artifact or issue tracker.

### When to perform

- Before every promotion to `main`
- After any change to theme, font, layout, or rendering code
- After any change to the golden image baselines

## Specification coverage audit

Every requirement in SPEC §4.10, §5.6, and §11.1 is mapped to an automated
test or documented process:

### §4.10 — Overlay Visual Acceptance

| Requirement | Test/Process |
|-------------|-------------|
| Framebuffer output through production composition path | `test_overlay_visual` (renders via `cbx_overlay_surface_render` + `cbx_select_grid_render_cb`) |
| Player Mode grid with content in cells, text, icons | `test_overlay_visual::test_player_mode_grid` |
| Host Mode differs from Player Mode | `test_overlay_visual::test_host_mode_differs` |
| Conflict highlighting (red indicator) | `test_overlay_visual::test_conflict_highlighting` + `test_conflict::test_conflict_red_rendering` |
| Unassigned + ≥2 player columns | `test_overlay_visual::test_unassigned_with_columns` |
| Controller model/profile text | `test_overlay_visual::test_model_profile_text` |
| Virtual-device icons | `test_overlay_visual::test_virtual_device_icons` |
| State transitions produce different frames | `test_overlay_visual::test_state_transitions_differ` |
| Tests fail if text/icons/rows/columns absent | All `test_overlay_visual` sub-tests assert on pixel content, not struct fields |

### §5.6 — Manager Visual Acceptance

| Requirement | Test/Process |
|-------------|-------------|
| Render and read back pixels for all 3 tabs | `test_manager_visual` (Controllers, Profiles, Settings) |
| Same initialization as `controller-box --manager` | `test_manager_visual` uses `cbx_manager_init()` / `cbx_manager_render()` |
| No manually attached modules | `test_manager_visual` relies on production init path only |
| Controllers: connected + degraded modes | `test_manager_visual::test_controllers_tab_degraded` + `test_controllers_tab_connected` |
| Profiles: Default profile + create/edit/delete | `test_manager_visual::test_profiles_tab` |
| Settings: every setting + current/default value | `test_manager_visual::test_settings_tab` |
| Profile editor: diagram, binding list, sequential, validation, progress | `test_manager_visual::test_profile_editor_list_mode` + `test_profile_editor_sequential_mode` + `test_profile_editor_validation_error` |
| Meaningful non-background output in every region | All `test_manager_visual` sub-tests assert `fb_region_has_content` |
| Tab/mode switching changes captured frame | `test_manager_visual::test_tab_switch_differs` |
| No struct-field-only checks | All sub-tests assert on pixel content |

### §11.1 — Seven-Layer Rendering Verification

| Layer | Requirement | Test/Process |
|-------|-------------|-------------|
| 1. Deterministic framebuffer | Software renderer, production path, `SDL_RenderReadPixels` | `test_overlay_visual`, `test_manager_visual`, `test_fb_assert` |
| 2. Region-level assertions | Non-background + text-colored pixels, state changes alter regions | `fb_assert.c` library, used by all visual tests |
| 3. Golden images | Reviewed baselines, documented tolerance, explicit updates | `test_golden` (11 baselines, ±3/channel, <2% image) + `scripts/generate-golden.sh` |
| 4. Failure artifacts | Actual/expected/diff PNGs on mismatch | `test_golden` writes to `tests/golden-fail/` |
| 5. Installed production smoke | Installed binary under X11, input events, non-blank capture | `test_installed_smoke.sh` (Xvfb + xdotool + ImageMagick) |
| 6. Backend smoke | Accelerated renderer (OpenGL/ES), broad invariants | `test_backend_smoke.c` (skips exit 77 if no GPU) |
| 7. Human release acceptance | Human review on target hardware | Documented checklist above (§Human release acceptance checklist) |
| Closing mandate | Suite fails on blank/incomplete screens | All visual tests assert `fb_region_has_content`; golden test fails on >2% pixel diff |