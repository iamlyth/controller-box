# Operations

Controller-Box runs as a **systemd user service** that communicates with
InputPlumber over the **system DBus**. This document covers service
architecture, management commands, configuration, performance expectations,
and troubleshooting.

## Service architecture

```
inputplumber.service  (system service — input engine)
    ↑ Requires= / After=
controller-box.service  (user service — overlay + control surface)
```

- **InputPlumber** (`inputplumber.service`, system): owns evdev grab, virtual
  devices, event translation, profiles, intercept mode, and player ordering.
  Controller-Box never touches input routing directly — every state change goes
  through InputPlumber's DBus API.
- **Controller-Box** (`controller-box.service`, user): always-resident overlay
  service with `Restart=always`. The service unit hard-depends on InputPlumber
  via `After=inputplumber.service` and `Requires=inputplumber.service`. If
  InputPlumber is not running, the overlay service will not start until it is.

**Install order:** (1) InputPlumber, (2) Controller-Box, (3) enable the overlay
service.

If InputPlumber is not installed, Controller-Box enters **degraded mode** —
the DBus connection stays open, and `NameOwnerChanged` signals notify when
InputPlumber starts.

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
After=inputplumber.service
Requires=inputplumber.service

[Service]
ExecStart=/usr/bin/controller-box --overlay-service
Restart=always

[Install]
WantedBy=default.target
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
detection. The spec's ~500 ms figure was reduced to meet the <10 ms overlay
appearance target for the render path. Detection latency is bounded by the poll
interval; the render path itself is <1 ms.

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

Controller-Box enters degraded mode if InputPlumber is not running. Check:

```bash
systemctl status inputplumber    # system service
```

If InputPlumber is not installed, install it first. Controller-Box will
auto-connect when InputPlumber starts (via `NameOwnerChanged` signal).

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
   and Arrow key presses via `xdotool`, captures the root window via
   `import -window root` (ImageMagick), and verifies pixel variance
   (mean > 5.0 on 0–255 scale) in both the tab-bar region (top 48px)
   and the body region (below 48px).
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
PASS: manager terminated cleanly
PASS: overlay service exited cleanly (code 1: InputPlumber not found)
PASS: all installed smoke test checks passed
```

If Xvfb or ImageMagick is not installed:
```
SKIP: required tool 'Xvfb' is not installed
```
(ctest reports the test as Skipped, not Failed.)