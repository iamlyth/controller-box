# Controller-Box — Technical Specification

**Status:** Ready for implementation (to-tickets → implement)
**Source:** Wayfinder map, Forgejo `lyth/project-planning` issue #1 (16 decisions), tickets #2–#10 (resolved), and research artifacts in this directory (`inputplumber-research-summary.md`, `wayfinder-ticket6-dbus-api-audit.md`, `controller-identification-research.md`).

---

## 1. Executive Summary

**Controller-Box** is a controller management GUI for Linux: an SDL2-based in-game overlay plus a full manager application that wrap [InputPlumber](https://github.com/ShadowBlip/InputPlumber) — the open-source Rust input-routing daemon — and give it a console-like, controller-only user experience.

**The problem.** InputPlumber is a powerful engine — it grabs physical controllers, creates virtual target devices, remaps inputs via profiles, reorders players, and intercepts input for overlays — but it ships with only a CLI/TUI. There is no graphical, controller-navigable way to assign players, switch profiles, or manage virtual controllers. On a controller-only "console PC" (no keyboard, no mouse, no Steam), this makes InputPlumber unusable for its best use case.

**The solution.** A single binary, `controller-box`, with two modes:

- **Overlay service** (always resident, systemd user service) — a fighting-game-style "character select" screen that appears in under 10 ms when any player presses **Select + A**. Players move their own controller between player slots and cycle their own profile, mid-game, without touching a keyboard.
- **Manager** (launched on demand) — a tab-based configuration app for creating virtual controllers, building/editing profiles with a visual controller diagram, and adjusting settings.

**Who it's for.** The primary user is running a controller-only Linux gaming box — a living-room PC, a Pi 4 retro console, a Steam Deck in desktop/GameScope session, or an FGC tournament setup — with no Steam and no keyboard attached. Friends bring their own controllers; profiles follow the controller, not the seat.

**What it explicitly is not.** It does not replace InputPlumber's input routing engine, does not integrate with Steam, and does not support keyboard/mouse-first navigation (see §12, Out of Scope).

### User stories (summary)

1. As a console-PC owner, I want to press one button combo on any controller to open an overlay, so that I never need a keyboard mid-game.
2. As a player, I want to move my controller between player slots with left/right, so that seat order doesn't dictate player order.
3. As a player, I want my profile to follow my controller when I change slots, so that my bindings are always mine.
4. As a tournament organizer, I want a friend to plug in their own controller and have their saved profile and preferred slot auto-load, so that setup takes zero adjustment.
5. As a host, I want to press R3 to take exclusive control of the overlay, so that I can fix other players' assignments without them interfering.
6. As a player, I want two controllers landing on the same slot to be visibly flagged and auto-resolved on exit, so that conflicts never silently break a game.
7. As a user, I want to add/remove virtual controllers (player slots) in a manager app, so that the game sees exactly the controllers I choose.
8. As a user, I want mixed virtual controller types per slot (P1 = xb360, P2 = ds5), so that each game gets the device type it works best with.
9. As a user, I want to build a profile by pressing physical buttons in sequence while a controller diagram lights up, so that mapping is fast and error-free.
10. As a user, I want profiles validated against a minimum binding set, so that I can never save a profile that can't navigate a menu.
11. As a user, I want profiles saved as standard InputPlumber YAML, so that they work with InputPlumber's own CLI and survive GUI updates.
12. As a user, I want icons in the overlay to show what the *game* sees (virtual device type), so that the display matches in-game behavior.
13. As a Steam Deck / Bazzite / ChimeraOS user, I want a Flatpak install that sets up its own background service, so that I don't hand-edit systemd units.
14. As a Pi 4 user on an unusual distro, I want a tarball fallback with `make install`, so that I'm not blocked on Flatpak support.

---

## 2. Architecture Overview

### 2.1 Engine vs. control surface (Decision 5)

The system is split strictly in two:

- **InputPlumber is the engine.** It owns: evdev grab, virtual (target) devices, event translation, profile loading, intercept mode, and player ordering. It is a separate project, a separate package, and a separate systemd *system* service. It is **never bundled** with the GUI (Decision 13 rationale: different languages and build systems — Rust vs. C/C++ — independent release cycles, and users who want InputPlumber alone should get it alone; bundling would mean vendoring/forking).
- **The GUI is the control surface.** It owns: the overlay, the manager UI, the profile browser/editor, the player-order UI, and the virtual connect/disconnect UI. It **never touches input routing directly** — every state change goes through InputPlumber's DBus API.

### 2.2 Communication: direct DBus via sd-bus (Decision 4)

The GUI talks to InputPlumber over the **system DBus** using **sd-bus** (part of systemd — zero additional dependencies). No CLI wrapping, no proxy daemon. InputPlumber exposes a complete DBus API (`org.shadowblip.InputPlumber`) with ObjectManager enumeration, intercept signals, and every operation the GUI needs (see §10; full audit in `wayfinder-ticket6-dbus-api-audit.md`).

### 2.3 Binary architecture: one binary, two modes (Decisions 7, 13)

A single executable, `controller-box`:

| Invocation | Mode | Purpose |
|---|---|---|
| `controller-box` or `controller-box --overlay-service` | Overlay service | Runs as a systemd **user** service, always resident. Holds a pre-built overlay surface in memory. Waits for the intercept-activation signal, shows the overlay, hides it on B. |
| `controller-box --manager` | Manager | Launched on demand. Full tab-based configuration UI (Controllers / Profiles / Settings). |

Both modes share one codebase, one config directory, and one DBus connection pattern. Rationale for one binary: shared widget toolkit, shared DBus client code, shared config parsing — two binaries would duplicate all of it; a DBus-client manager (Option C from ticket #7) would couple the manager to a running daemon for pure config editing.

### 2.4 Service model

```
inputplumber.service        (system service — separate package, prerequisite)
controller-box.service    (user service — installed by the manager on first run)
```

The overlay is a systemd user service, while InputPlumber is a system service. A user unit **must not** declare `After=` or `Requires=` for `inputplumber.service`: system and user managers have separate dependency graphs, so such directives incorrectly search for a user unit and can prevent startup. The overlay instead orders with the graphical user session, starts with bounded restart backoff, checks ownership of `org.shadowblip.InputPlumber`, and remains alive in a degraded state while the system service is absent.

If InputPlumber is unavailable, unauthorized, incompatible, or fails enumeration, Manager and overlay show/report a specific actionable error rather than treating a system-bus connection as readiness. Backend-dependent controls are visibly disabled. Both modes watch `NameOwnerChanged`; after the service appears or restarts they re-enumerate and become operational within two seconds without restarting Controller-Box. Install order remains: (1) InputPlumber, (2) GUI, (3) enable GUI service.

### 2.5 Hotkey architecture (Decisions 6, 4-resolution)

Exactly **one hotkey exists in the entire system: the overlay trigger** (default Select + A, user-configurable). There are no quick-action combos — all interaction happens inside the overlay. This deliberately eliminates the PASS-mode listener from the original hybrid design (ticket #4 resolution): the only DBus interaction during gameplay is waiting for the intercept activation.

Mechanism:

1. GUI registers the trigger combo via `SetInterceptActivation` on each composite device, and sets `InterceptMode = 1` (PASS). InputPlumber watches for the combo at kernel level (~1 ms overhead).
2. On activation, InputPlumber auto-switches to `InterceptMode = 2` (ALL) and routes input over DBus signals instead of to the game.
3. The GUI daemon detects the mode change (poll, ~50 ms (DEC-002) — no signal exists; gap workaround #1, §10.3) and displays the pre-built overlay.
4. The user interacts; **B** closes.
5. On close, the GUI sets `InterceptMode = 1` (PASS) again. Input flows back to the game in <1 ms. The overlay is hidden, not destroyed.

---

## 3. System Requirements

| Requirement | Specification | Rationale |
|---|---|---|
| **Minimum hardware** | Raspberry Pi 4 (ARM64, OpenGL ES 3.0) | Decision 2. Pi Zero / Pi 3 class hardware is out of scope — insufficient GPU headroom for a composited overlay. |
| **Architectures** | x86_64 and aarch64 | Both build targets required for tarball; Flatpak covers both. |
| **Display server** | Any compositor: X11, Wayland, Gamescope (Steam Deck) | Decision 3. Bare DRM/framebuffer (no compositor) is fully out of scope — RetroPie-style users are CLI-comfortable and unlikely to need this tool. |
| **Runtime dependencies** | SDL2, SDL2_ttf, SDL2_image; systemd (sd-bus); nanosvg (vendored, ~500-line C library) | SDL2 chosen (Decision 1) for best memory usage, speed, and portability across all targets — the accepted tradeoff is building the UI widget system from scratch. sd-bus is part of systemd, present on every target platform. nanosvg rasterizes controller icons (§8). |
| **Engine dependency** | InputPlumber (system service), installed separately | Not bundled (§2.1). |
| **Authorization** | Polkit rules sufficient to call InputPlumber DBus methods | All InputPlumber DBus methods/properties have polkit checks (research §6). |

**Accepted consequence of SDL2:** the GUI must implement its own widget system — buttons, lists, focus chains, text rendering — fully navigable by controller. This is a deliberate trade: the smallest possible runtime footprint on Pi 4 in exchange for building widgets once.

---

## 4. Overlay Specification

### 4.1 Model: fighting-game select screen (Decision 7)

The overlay is a **selector**, modeled on fighting-game / sports-game character select screens. It is not a menu system.

**Layout:** rows = physical controllers; columns = player slots (virtual controllers). The leftmost column is **Unassigned**. Each cell shows the icon of the *virtual* controller type for that slot (what the game sees — not the physical controller; see §8).

```
              Unassigned    P1       P2       P3       P4
                          ┌──────┬──────┬──────┬──────┬──────┐
8BitDo Ultimate 2C   ─►  │  ○   │  ●   │  ○   │  ○   │  ○   │  Profile: Default
8BitDo Ultimate 2C   ─►  │  ○   │  ○   │  ●   │  ○   │  ○   │  Profile: Fighting
Xbox Series          ─►  │  ○   │  ○   │  ○   │  ●   │  ○   │  Profile: Default
                          └──────┴──────┴──────┴──────┴──────┘
● = your position    ○ = available
```

**Controls:**

| Input | Action |
|---|---|
| Left / Right | Move your position across columns (slot position) |
| Up / Down | Cycle your profile (per-controller, follows you) |
| R3 | Enter/exit Host Mode |
| B | Close overlay |

### 4.2 Trigger (Decisions 6, 7; ticket #4)

- **Default: Select + A.** Deliberately *not* Select+Start — that conflicts with ES-DE's exit-game combo.
- User-configurable in Manager → Settings (a single setting, not a tab).
- Implemented via InputPlumber's `SetInterceptActivation` (kernel-level, ~1 ms) — see §2.5.

### 4.3 Player Mode (default)

All controllers edit simultaneously, like a fighting-game character select. Each controller moves its own row independently: left/right across columns, up/down to cycle its own profile. No controller can affect another's row in Player Mode.

### 4.4 Host Mode (R3)

The first controller to press **R3** becomes the exclusive host. All other controllers freeze. The host can navigate to any row and edit slot/profile. R3 again exits back to Player Mode. *Interface details (row navigation, edit affordances) are deferred to implementation — see §13.*

### 4.5 Conflict resolution

If two controllers land on the same column, the **second arrival is shown in red**. On overlay exit, the conflicted controller is moved to the **lowest unoccupied P slot**. Example: P4's controller moves to P2, P2 is occupied, P1 is free → controller becomes P1. The resolution is deterministic and automatic; the red highlight ensures the user is never surprised by it.

### 4.6 Profiles in the overlay

Profiles are **per-controller, not per-slot**. A controller's profile follows it across columns. This matches the FGC tournament scenario: plug in, your profile loads with you, wherever you sit.

### 4.7 Dynamic columns

The column count scales with the number of virtual (target) controllers InputPlumber has instantiated: a MAME cabinet with 6 virtual controllers shows 6 columns; a 2-player setup shows 2 (+ Unassigned). When the manager adds/removes a virtual controller, the overlay reflects the new column count the next time it opens — no overlay changes needed (ticket #8).

### 4.8 No nicknames (ticket #5)

Controllers are shown by **model name + slot position** (plus the virtual-type icon). There are no user-assigned labels or colors. The identification machinery (§6) exists to make auto-assignment work — the *user interaction* stays purely positional. Rationale: naming prompts on connect are disruptive; a devices panel adds a whole management surface; positional interaction is console-native and always sufficient when auto-assignment does its job.

### 4.9 Rendering & performance

The overlay surface is **pre-built in memory** at daemon startup — icons rasterized, textures cached, layout computed from current state and dirtied only on device/slot/profile change events. Nothing is constructed on demand. With the required ~50 ms polling workaround, button-to-first-visible-frame latency is ≤75 ms at p99 and ≤100 ms maximum on minimum supported hardware. Time from detecting `InterceptMode = ALL` to mapping/presenting the first compositor-visible frame remains <10 ms at p99 (see §11).

### 4.10 Visual acceptance

The overlay is not considered implemented merely because its state machine, widget tree, or texture objects exist. Verification must demonstrate actual framebuffer output through the same composition path used by `controller-box --overlay-service`.

At minimum, deterministic visual tests must render and read back pixels for: the normal Player Mode grid, Host Mode, conflict highlighting, Unassigned plus at least two player columns, controller model/profile text, and virtual-device icons. Each state must contain meaningful non-background output in its expected regions, and transitions must produce a materially different frame. The conflict state must contain the specified red indication. Tests must fail if text, icons, rows, columns, or highlights are absent even when their in-memory objects and dimensions are valid.

---

## 5. Manager Specification

### 5.1 Structure (Decisions 10, 11; ticket #2)

The manager is a separate mode from the overlay (§2.3), navigated by controller with a **tab bar at the top** (console settings-menu style): Left/Right switches tabs, Up/Down navigates within a panel, and A activates the focused control. Three tabs: **Controllers**, **Profiles**, **Settings**. (Ticket #2 originally sketched a Hotkeys tab; ticket #4's single-hotkey decision collapsed it into one Settings entry.)

Controller navigation is the mandatory primary path, but the manager also supports a mouse pointer as a secondary path. Every visible enabled tab, button, list row, selector, editor control, and dialog action must respond to pointer hover and a left-button click inside its rendered bounds. Controller activation and pointer activation must invoke the same behavior and validation. No required operation may be available only by mouse, and decorative labels, diagrams, and progress indicators must not masquerade as interactive controls.

### 5.2 Controllers tab

Configures the virtual controllers the games see:

- **Add / remove virtual controllers** — this is where player slots (overlay columns) come from. Virtual connect/disconnect is **manager-only** (Decision 14, ticket #8). The overlay never creates or destroys slots; it only assigns physical controllers among existing ones.
- **Set each virtual controller's type** — `xb360`, `ds5`, `deck`, `gamepad` (generic), `mouse`, `keyboard`, etc. (full type list in §10.2).
- **Mixed types allowed** — P1 = xb360, P2 = ds5, no warnings. Each virtual device exposes its full capability set to the game; the mix is the user's choice.
- **Removing a slot** mid-session: the physical controller in that slot auto-moves to Unassigned; the overlay's column count adjusts dynamically; InputPlumber stops the target device so the game sees one fewer controller; no input is lost — the physical controller still works in the overlay.

The configured startup count/type list is authoritative desired topology. Overlay/service startup reconciles InputPlumber to exactly that ordered topology before assignment is enabled; displaying columns without corresponding InputPlumber targets is an error, not success. Add succeeds only after ObjectManager exposes one additional target of the selected type and it is attached/routable under the slot model. Remove succeeds only after the target disappears and affected physical controllers are confirmed Unassigned. Type change replaces only the selected slot and preserves all other topology. Failures retain the last confirmed topology and show the failed DBus operation.

Action matrix (ticket #8):

| Action | Where | Mechanism |
|---|---|---|
| Create virtual controller (add slot) | Manager → Controllers | Add button, pick type → `CreateTargetDevice` / `SetTargetDevices` |
| Remove virtual controller (remove slot) | Manager → Controllers | Remove → controller auto-Unassigned → `StopTargetDevice` / `SetTargetDevices` |
| Assign physical controller to slot | Overlay | Move left/right across columns |
| Unassign physical controller | Overlay | Move to Unassigned column |
| Change virtual controller type | Manager → Controllers | Select slot, change type |

### 5.3 Profiles tab

Browse, create, edit, delete profiles.

- The **Default profile is built-in, always present, read-only, always the fallback.** Controllers work out of the box; profiles exist only for modifications from default.
- **New profile flow:** pick a starting point — Default copy / Empty / Clone existing → opens the editor.
- User-created profiles are stored as new InputPlumber YAML files in the same user profile directory (§7).
- Controller-Box ships an immutable, InputPlumber-compatible Default profile so a clean installation works even when host profile directories are empty. "Default copy" must not depend on an unverified external file.
- Empty profile creation includes a reachable sequential/add-first-binding action when the mapping list has zero rows. A clean-home user can capture the NES minimum, save through normal production events, restart Manager, and see the profile. Save and discard are explicit visible controls; window close with unsaved changes prompts instead of silently discarding.

### 5.4 Profile editor (Decision 11)

**Two editing modes sharing one always-visible, always-synchronized controller diagram** (left panel):

- **Mode 1 — Binding list.** Up/Down scrolls the binding list (right panel). The highlighted row lights up the corresponding button on the diagram. **A** edits the binding — choose the target from a list or capture a physical button press.
- **Mode 2 — Sequential binding.** The editor prompts for each button in order; the diagram lights up the button currently being mapped. Press a physical button → captured → auto-advance. **B** skips, **Start** cancels. A progress bar shows completion.

**Validation — NES minimum.** A valid profile must bind at least: **A, B, D-Pad Up, D-Pad Down, D-Pad Left, D-Pad Right.** An error is shown if any required binding is missing. All other bindings (triggers, sticks, shoulders, Start, Select) are optional. Rationale: this is the minimum set that can navigate any menu and play any NES-class game; requiring it makes "broken profile" an impossible state.

**Profile scope.** Profiles map against the **virtual device's capabilities, not the physical controller**. InputPlumber's capability maps normalize physical-controller differences, so the same profile works regardless of connection method (Bluetooth vs. USB) or physical model.

**Design principle:** *Same profile + same controller = same result, every time.* Profiles are deterministic and portable — no dynamic behavior.

**Covered use cases (ticket #2):** (1) specialized controllers — arcade sticks, fighting pads; (2) friends bringing personal controllers with saved profiles; (3) per-game profile variations (manual switching in v1; auto-switching is out of scope, §12).

### 5.5 Settings tab

App-level settings: launch at boot, theme, overlay opacity, number of virtual controllers on startup (and their types), overlay trigger combo (the single hotkey), controller icon overrides (§8.4).

### 5.6 Visual acceptance

The installed manager must render usable body content, not only a window, tab labels, widget metadata, or non-zero rectangles. Production-path verification must render and read back pixels for all three tabs using the same initialization and composition path as `controller-box --manager`; tests must not manually attach modules that production startup omits.

Required deterministic states are: Controllers with controls visible in both connected and InputPlumber-unavailable degraded modes; Profiles with the built-in Default profile and create/edit/delete controls; Settings with every configurable setting and its current/default value; and the profile editor with its controller diagram, binding list, sequential-binding prompt, validation error, and progress state. Every expected control and text region must contain meaningful non-background framebuffer output. Switching tabs or editor modes must change the captured frame. A test that checks only child counts, visibility flags, geometry, focus membership, or "render did not crash" does not satisfy this requirement.

### 5.7 Interaction acceptance

The manager must maintain a machine-readable or test-enumerated inventory of every interactive control and its expected semantic outcome. Automated acceptance must traverse that inventory through normal SDL events and production dispatch—not by calling a control callback or tab-specific activation function directly.

For every visible enabled control, tests must prove both paths:

- **Controller path:** reach the control from the tab bar using the normal focus chain, visibly indicate focus, activate it with the controller A event, and verify the intended outcome.
- **Pointer path:** derive a click point from the control's final rendered bounds, send normal mouse motion plus left-button down/up events, visibly indicate hover/press state, and verify the same outcome.

An event-handler return value is not outcome evidence. Depending on the control, evidence must include an observable state transition, dialog/editor navigation, changed framebuffer region, exact mock DBus request, validated file/configuration mutation, or persisted value after restart. Disabled controls must reject both activation paths and produce no backend or filesystem side effect. Hit testing must follow final layout after resize and must not use stale pre-layout rectangles.

End-to-end scenarios must cover at least: Controllers add/remove/type-change; Profiles create from each starting point, select, edit, validate, save, and delete; Settings change and persistence; profile-editor list and sequential modes including cancel/error paths; tab switching; and recovery from InputPlumber-unavailable and operation-failure states. Overlay interaction remains controller-driven and must similarly be exercised through its production event path for open, movement, profile cycling, Host Mode, conflict resolution, and close.

The installed-production smoke test must perform representative coordinate-based manager clicks in body controls as well as tab clicks. Controller acceptance uses the production controller transport with a physical or kernel-backed synthetic gamepad while InputPlumber is running; keyboard-generated SDL events are supplemental accessibility evidence and must never be labeled controller acceptance. Backend acceptance uses a real or private DBus service exporting InputPlumber's native signatures and ObjectManager behavior; the string-only mock is supplemental. A visual change without the specified semantic outcome, or a semantic unit test that bypasses production event routing, does not satisfy interaction acceptance.

---

## 6. Controller Identification

### 6.1 The problem (research evidence)

No production Linux software solves the identical-controller problem well. Dolphin has a known bug (#12126) where identical USB controllers with empty/duplicate serials are deduplicated into one device. RetroArch and PCSX2 use model-level identifiers (SDL2 GUID encodes bustype+VID+PID+version+name — identical for identical units). SDL2's instance ID is per-session only. Steam alone sidesteps the problem — by creating numbered virtual gamepads, exactly the InputPlumber model. Bluetooth controllers never have this problem: the MAC is unique and stable. See `controller-identification-research.md` for the full six-product comparison.

### 6.2 Multi-layered auto-assignment (Decision 8)

On connect, the GUI identifies each controller by the strongest available identity, in descending order:

| Layer | Identity source | evdev/DBus source | Stability |
|---|---|---|---|
| 1 | **Bluetooth MAC** | evdev `uniq` → `UniqueId` property | Stable, unique — zero adjustment across sessions |
| 2 | **USB serial** | evdev `uniq` → `UniqueId`; HIDRaw `SerialNumber` | Stable when present (Sony, Microsoft, Nintendo provide real serials) |
| 3 | **USB port path** | evdev `phys` → `PhysPath` property | Stable only if the same port is used; breaks on port change |
| 4 | **Connection order** | Enumeration order | Fallback only; session-level |

The GUI saves per-controller preferences (**preferred slot, preferred profile**) keyed to the best available stable ID. Controllers with layer 1–2 identities get zero-adjustment across sessions. Controllers without stable IDs fall back to connection order, with the overlay as the manual override.

### 6.3 ID format and identity-strength tracking

Assignment keys carry a **type prefix** so the GUI knows identity strength at runtime:

```
BT:AB:CD:01:EF:23        Bluetooth MAC (strong)
USB:SN12345              USB serial (strong)
USB:phys:usb-3-2         USB port path (semi-stable — warn if it changes)
```

Stored in `assignments.yaml` (§7.3). Rationale for the prefix scheme: when a controller reconnects with a *weaker* identity than before (e.g., moved USB ports, phys path changed), the GUI can detect the downgrade and fall back gracefully instead of mismatching.

---

## 7. Config Layer

### 7.1 Ownership model: hybrid (Decision 12)

**InputPlumber owns profile content; the GUI owns presentation.** Profiles are standard InputPlumber DeviceProfile YAML — the GUI's editor generates `version: 1, kind: DeviceProfile, name, description, mapping` documents written directly to InputPlumber's user profile directory and loaded natively via `LoadProfilePath` / `LoadProfileFromYaml`. **There is no duplicate profile format.** GUI metadata lives in optional sidecar files.

Rationale (ticket #3): extending InputPlumber's format (Option B) would fork the schema and break CLI compatibility; storing everything GUI-side (Option A) would duplicate the mapping format and lose native InputPlumber loading. Hybrid keeps profiles portable — the same YAML works with `inputplumber device <id> profile load`.

### 7.2 File layout

```
~/.local/share/inputplumber/profiles/        ← InputPlumber + GUI both read/write
├── fighting.yaml                             ← DeviceProfile (button mappings)
├── mario-kart.yaml                           ← another profile
└── default.yaml                              ← system default (read-only)

~/.config/controller-box/                   ← GUI-only config
├── settings.yaml                             # app settings
├── assignments.yaml                          # auto-assignment table
└── profile-metadata/                         # optional sidecar per profile
    ├── fighting.meta.yaml                    # icon, display order, description override
    └── mario-kart.meta.yaml
```

### 7.3 settings.yaml

```yaml
overlay_trigger: "Select+A"
launch_at_boot: true
theme: "default"
overlay_opacity: 0.85
virtual_controllers:
  count: 4
  types: [xb360, xb360, xb360, xb360]
```

### 7.4 assignments.yaml

```yaml
assignments:
  - id: "BT:AB:CD:01:EF:23"    # Bluetooth MAC (stable)
    slot: 0                     # P1
    profile: "fighting"
  - id: "USB:SN12345"           # USB serial (stable)
    slot: 1                     # P2
    profile: "default"
  - id: "USB:phys:usb-3-2"     # USB port path (semi-stable)
    slot: 0
    profile: "default"
```

The GUI also persists **gamepad order** here (workaround for DBus gap #2, §10.3) and restores it after daemon restart.

### 7.5 profile-metadata sidecar (optional)

```yaml
display_name: "Fighting — No Triggers"
icon: "gamepad"                 # built-in icon name or absolute path to custom image
display_order: 2
description: "Triggers disabled, L/R bumpers only"
```

If no meta file exists, the GUI reads `name` and `description` from the profile YAML itself and uses the default icon. Meta files are strictly optional — profiles are fully functional without them.

### 7.6 InputPlumber profile format compatibility

Generated profiles must conform to InputPlumber's `device_profile_v1` schema:

```yaml
version: 1
kind: DeviceProfile
name: Start Button to Escape Key
description: Profile to map a gamepad's start button to the Escape keyboard key
mapping:
  - name: Menu
    source_event:
      gamepad:
        button: Start
    target_events:
      - keyboard: KeyEsc
```

Profiles can map between gamepad, keyboard, mouse, and touch events. Advanced mapping types (`chord`, `delayed_chord`, gamepad→mouse) are valid InputPlumber constructs but the v1 editor does not expose them (§13).

---

## 8. Controller Icons

### 8.1 What icons represent (Decision 16)

Icons show the **virtual controller type — what the game sees** (`xb360`, `ds5`, `deck`, …), **not** the physical controller. This is consistent with the architecture: InputPlumber creates virtual devices, games see those, profiles map physical→virtual. InputPlumber exposes `DeviceType` as a string property on target devices; the GUI maps that string directly to an icon. **No VID:PID lookup table is needed.**

### 8.2 Source: Controllercons

[Controllercons](https://controllercons.github.io/) — 30 controller icons in SVG, SIL Open Font License 1.1. Covers PS5, PS4, PS3, Xbox Series X, Xbox One, Xbox 360, Switch Pro, Joy-Cons, SNES, NES, N64, GameCube, Wii, Dreamcast, and more.

**Gaps the project must fill with custom icons in matching style:** arcade stick, hitbox (both required for the FGC audience), Steam Deck, and a generic gamepad silhouette for unknown types.

### 8.3 Format and rendering

SDL2 does not render SVG natively. **nanosvg** (~500-line C library, vendored) rasterizes SVGs to SDL textures **at startup** at the display's native resolution. Textures are cached in memory as part of the pre-built overlay (§4.9) — zero loading delay when the overlay appears. SVG sources are recolorable for theming.

### 8.4 Mapping table

`/usr/share/controller-box/controller-icons.yaml` maps InputPlumber `DeviceType` strings to icons:

```yaml
virtual_types:
  - type: "xb360"
    icon: "cc-xbox-360"
    name: "Xbox 360 Controller"
  - type: "ds5"
    icon: "cc-ps5"
    name: "DualSense"
  - type: "deck"
    icon: "cc-steam-deck"      # custom, we create
    name: "Steam Deck Controller"
  - type: "gamepad"
    icon: "generic-gamepad"    # custom, we create
    name: "Generic Gamepad"
```

**Unknown types:** generic gamepad silhouette + the raw `DeviceType` string as label.

### 8.5 Profile override

The profile metadata sidecar (§7.5) overrides the icon:

```yaml
icon: "cc-ps5"                 # built-in icon name
icon: "/path/to/custom.png"    # absolute path to custom image
```

---

## 9. Packaging

### 9.1 Flatpak — primary (Decision 15, ticket #9)

Targets Steam Deck, desktop Linux, Bazzite, Nobara, ChimeraOS, and other Flatpak-capable distributions. The manifest is **experimental until** a clean Flatpak build passes the installed functional gate, host profile paths are proven visible, host InputPlumber DBus access is verified, and the application is actually published. Documentation must not advertise a Flathub install command before publication.

**Manifest permissions:**

```
--system-talk-name=org.shadowblip.InputPlumber   # system DBus access
--filesystem=~/.local/share/inputplumber         # read/write user profiles
--filesystem=/usr/share/inputplumber:ro          # read system profiles/devices/capability maps
```

**Systemd service problem and solution.** Flatpak cannot ship systemd units to the host. The manager installs the service on first run:

```
User installs Flatpak → opens manager (first launch) →
"Enable overlay service? This will install a systemd user service." → Yes →
GUI writes ~/.config/systemd/user/controller-box.service →
systemctl --user enable --now controller-box → done
```

The unit runs `flatpak run <app-id> --overlay-service`, uses bounded `Restart=on-failure` backoff, and follows the graphical user session.

### 9.2 Tarball — fallback (v1)

Covers any distro, x86_64 + aarch64 (Pi 4). CMake build against system SDL2 dev packages. `make install` places the binary, the systemd user service file, the desktop entry, and the default controller icons.

### 9.3 Install layout (both formats)

```
/usr/bin/controller-box                                        ← one binary, two modes
~/.config/systemd/user/controller-box.service                  ← user service (installed by manager on first run)
~/.local/share/applications/controller-box-manager.desktop     ← desktop entry for manager
/usr/share/controller-box/icons/                               ← default controller icons
/usr/share/controller-box/controller-icons.yaml                ← icon mapping table
```

### 9.4 InputPlumber as a dependency

Flatpak: documented host-system prerequisite. Tarball: user installs InputPlumber first from its own package. The GUI user service performs runtime bus-name/readiness checks and recovery as specified in §2.4; it does not declare an invalid cross-manager systemd dependency.

### 9.5 Later (post-v1)

.deb (amd64 + arm64), .rpm, Nix derivation, AUR, Batocera package (.BATOEXEC), eventual upstream Batocera integration.

---

## 10. DBus Integration

### 10.1 Connection model

- **Bus:** system bus. **Bus name:** `org.shadowblip.InputPlumber`. **Root path:** `/org/shadowblip/InputPlumber`.
- **Enumeration:** `org.freedesktop.DBus.ObjectManager.GetManagedObjects()` at the root path returns all composite devices, source devices, and target devices in one call.
- **Hotplug:** subscribe to ObjectManager `InterfacesAdded` / `InterfacesRemoved` — source, composite, and target devices all register/unregister through the object server. **No polling for device presence.**
- **Property changes:** `org.freedesktop.DBus.Properties.PropertiesChanged` is emitted for `GamepadOrder`, `ProfileName`, `ProfilePath`, `TargetDevices`, `SourceDevicePaths` — but **not** for `InterceptMode` (gap #1).
- **Native type fidelity:** production reads and writes each member using its declared DBus signature, including `u` for `InterceptMode`, `b` for booleans, and `as` for string arrays. Converting every property through a string getter is prohibited. Release tests run against a real/private sd-bus service with these signatures.
- **Operational readiness:** a raw system-bus connection is not readiness. Controller-Box verifies the InputPlumber owner, reads `Version` from `/org/shadowblip/InputPlumber/Manager`, completes ObjectManager enumeration, and validates required typed properties. It processes DBus traffic continuously, subscribes hotplug and owner changes, and reconciles current composites, targets, triggers, polls, and input mappings after startup and service/device changes.

Object tree:

```
/org/shadowblip/InputPlumber
├── Manager                          (org.shadowblip.InputManager)
├── CompositeDevice0                 (org.shadowblip.Input.CompositeDevice)
├── CompositeDevice1, ...
└── devices/
    ├── source/  event0, hidraw0, iio:device0, led0, ...
    └── target/  gamepad0, keyboard0, mouse0, dbus0, ...
```

### 10.2 API surface used by the GUI

**Manager interface** — `/org/shadowblip/InputPlumber/Manager` (`org.shadowblip.InputManager`):

| Member | Type | GUI use |
|---|---|---|
| `CreateCompositeDevice(config_path: s) → s` | method | Create composite from YAML file path (see gap #3) |
| `CreateTargetDevice(kind: s) → s` | method | Add a virtual controller (player slot) — returns object path |
| `StopTargetDevice(path: s)` | method | Remove a virtual controller |
| `AttachTargetDevice(target_path: s, composite_path: s)` | method | Attach standalone target to a composite |
| `GamepadOrder: as` | property, rw | Player ordering. Setting it suspends all devices, resumes them one-by-one in the new order (100 ms stagger). Emits `PropertiesChanged`. **Not persisted** (gap #2). |
| `SupportedTargetDeviceIds: as` | property, r | Populate the manager's type picker (`xb360`, `ds5`, `deck`, `gamepad`, …) |
| `SupportedTargetDevices: as` | property, r | Human-readable names for the same |
| `Version: s` | property, r | Compatibility check at startup |
| `ManageAllDevices: b` | property, rw | Expose in Settings if needed |

**CompositeDevice interface** — `/org/shadowblip/InputPlumber/CompositeDevice{N}`:

| Member | Type | GUI use |
|---|---|---|
| `SetInterceptActivation(activation_events: as, target_event: s)` | method | Register the overlay trigger combo (Select + A) |
| `InterceptMode: u` | property, rw | 0=NONE, 1=PASS, 2=ALL, 3=GAMEPAD_ONLY. Overlay lifecycle: PASS → (activation) → ALL → (close) → PASS. **No change signal** (gap #1 — poll). |
| `LoadProfilePath(path: s)` | method | Load a profile from file |
| `LoadProfileFromYaml(profile: s)` | method | Load a profile from a YAML/JSON string (used to preview/test edited profiles without writing files) |
| `GetProfileYaml() → s` | method | Dump current profile |
| `ProfileName: s`, `ProfilePath: s` | properties, r | Current profile display; `PropertiesChanged` emitted |
| `SetTargetDevices(target_device_types: as)` | method | Replace all target devices (stops old, creates new) — manager's Controllers tab |
| `TargetDevices: as` | property, rw | Current target device paths |
| `SourceDevicePaths: as` | property, r | Which physical devices compose this device |
| `PersistentId: s` | property, r | Persistent identifier computed from source devices — used in the identification layer (§6) |
| `Name: s` | property, r | Display name |
| `Capabilities / OutputCapabilities / TargetCapabilities: as` | properties, r | Capability display; editor scope (§5.4) |
| `Stop()` | method | Stop the composite device |
| `SendEvent(event: s, value: v)`, `SendButtonChord(events: as)` | methods | Editor "test binding" affordance (optional in v1) |
| `DbusDevices: as` | property, r | Locate the intercept-mode DBus target |
| `FilteredEvents: a{ss:as}` (rw), `FilterableEvents: a{ss:as}` (r) | properties | Event filtering (not required for v1 core flows) |

**Target device interfaces:**

- `org.shadowblip.Input.Target` — `Name: s`, `DeviceType: s` (the icon-mapping key, §8.1).
- `org.shadowblip.Input.Gamepad` / `.Keyboard` / `.Mouse` / `.Touchscreen` — `Name: s`; `SendKey(key: s, value: b)`, `MoveCursor(x: i, y: i)` on the respective interfaces.
- `org.shadowblip.Input.DBusDevice` — signals `InputEvent(event: s, value: d)` and `TouchEvent(...)`: overlay input while in intercept mode.
- `org.shadowblip.Output.ForceFeedback` — `Rumble(value: d)`, `Stop()`, `Enabled: b` (rw).

**Source device interfaces (identification layer, §6):**

- `org.shadowblip.Input.Source.EventDevice` / `.UdevDevice` (all read): `Name`, `PhysPath`, `IdVendor`, `IdProduct`, `UniqueId` (evdev `uniq` — BT MAC or USB serial), `SysfsPath`, `DevicePath`, `IdBustype`, `IdVersion`, plus capability arrays.
- `org.shadowblip.Input.Source.HIDRawDevice`: `SerialNumber`, `Manufacturer`, `Product`, `IdVendor`, `IdProduct`, `InterfaceNumber`. **Note:** serial is `UniqueId` on evdev/udev but `SerialNumber` on HIDRaw — check the right interface per device type.

### 10.3 The five gaps and their workarounds (ticket #6 audit — all confirmed against InputPlumber source)

| # | Gap | Workaround |
|---|---|---|
| 1 | **No `PropertiesChanged` signal for `InterceptMode`** — internal `set_intercept_mode()` never calls the signal emitter, for both external sets and the PASS→ALL auto-switch | GUI polls `InterceptMode` (~50 ms interval (DEC-002)). Since the GUI initiates the overlay trigger and the close, it tracks state locally and treats the poll as confirmation. |
| 2 | **`GamepadOrder` not persisted** — in-memory only, resets to empty on daemon restart | GUI saves the order in its own config (§7.4) and re-applies it via the property setter after daemon restart / device changes. |
| 3 | **`CreateCompositeDevice` requires a YAML file path** — no string-based or source-path-based variant on DBus | GUI writes a temp composite-device YAML (e.g. `/tmp/controller-box-XXXX.yaml`) and passes the path. |
| 4 | **No DBus method to enumerate profiles / device configs / capability maps on disk** | GUI reads the filesystem directly: `~/.local/share/inputplumber/profiles/`, `/usr/share/inputplumber/profiles/`, `/usr/share/inputplumber/devices/`, `/usr/share/inputplumber/capability_maps/`. (Covered by the Flatpak filesystem permissions, §9.1.) |
| 5 | **Add/remove individual source devices on a running composite is not exposed on DBus** (internal commands exist but are not on the interface) | **Not needed for v1.** InputPlumber's auto-management builds composites from device configs; the GUI manages slots via target devices only. |

**Verdict (carried from ticket #6):** the InputPlumber DBus API is sufficient for v1. All gaps have workarounds. No upstream changes required.

---

## 11. Performance Requirements

| Requirement | Target | Mechanism |
|---|---|---|
| Overlay appearance | **≤75 ms p99, ≤100 ms max** from button press; **<10 ms p99** from `ALL` detection to compositor-visible present | The ~50 ms poll dominates button-to-detection latency; the pre-built surface bounds detection-to-present work. Both intervals are measured separately. |
| Gameplay input latency | **~1–2 ms** (InputPlumber's own intercept overhead only) | InputPlumber does **not** route gameplay input over DBus — the DBus channel is a side branch, never inline. During gameplay, intercept mode is PASS (kernel-level watch only). During overlay use, input goes over DBus — but the game is not receiving input then anyway. |
| Overlay close | Input flowing to game in **<1 ms** | Single DBus property set: `InterceptMode` back to PASS. Overlay hidden, not destroyed. |
| Daemon footprint | Always resident without measurable impact | SDL2's minimal memory profile was the deciding factor in the toolkit choice (Decision 1); the daemon idles waiting on DBus signals and a ~50 ms property poll (DEC-002). |
| Player reorder | Atomic, InputPlumber-managed | `GamepadOrder` setter suspends all devices and resumes them in the new order (100 ms stagger between devices). The GUI calls the setter; it does not manage suspend/resume itself. |

### 11.1 Rendering verification and release evidence

Visual requirements in §§4–5 are release gates. The automated suite must include all of the following layers:

1. **Deterministic framebuffer tests.** Run a fixed-size SDL software renderer with deterministic fixture data, theme, and readable test font. Render through production composition functions and read the current render target or backbuffer with `SDL_RenderReadPixels` (or an equivalent API that proves final pixel output). Target textures must not be treated as readable by `SDL_LockTexture` unless they were explicitly created with a lockable access mode.
2. **Region-level assertions.** Assert meaningful non-background and foreground/text-colored pixels inside required controls, labels, icons, lists, diagrams, and status regions. Assert important state changes alter the appropriate regions. These invariants are mandatory and must tolerate harmless rasterization differences.
3. **Golden images.** Keep reviewed baseline images for each major state listed in §§4.10 and 5.6. Compare deterministic software-renderer captures with a documented per-pixel/per-image tolerance rather than an unrestricted exact hash. A baseline update is an explicit reviewed change, never an automatic test side effect.
4. **Failure artifacts.** On mismatch, save actual, expected, and visual-diff images with the test name and renderer metadata so a human can diagnose the frame without rerunning interactively.
5. **Installed functional smoke test.** Install the packaged artifact into a clean environment containing only declared runtime dependencies. Start a real or private native-signature InputPlumber-compatible DBus service, hotplug a physical or kernel-backed synthetic controller, navigate Manager with real controller events, create and observe a routable virtual target, create/save/reload a profile, activate a mapped compositor-visible overlay, and verify persistence after process/backend restart. Capture required windows and independently inspect DBus/ObjectManager and filesystem outcomes. Missing backend, skipped package build, expected early exit, keyboard-only interaction, or a merely nonblank window is failure, not a skip.
6. **Backend smoke coverage.** Exercise the deployment renderer backend (OpenGL/OpenGL ES where available) with broad framebuffer invariants. Deterministic golden comparison may remain on the software renderer, but successful object creation or draw calls alone are insufficient for hardware-backend acceptance.
7. **Human release acceptance.** Before promotion to `main`, a human reviews representative manager and overlay captures on target hardware for legibility, clipping, focus indication, contrast, and controller-only usability. Automation catches missing or divergent output; it does not approve aesthetics.

The verification suite must explicitly fail when a required screen is blank or incomplete even if unit, state-machine, geometry, and no-crash tests pass. Screenshot/framebuffer artifacts and the exact commands that produced them are part of final verification evidence.

### 11.2 Autonomous implementation definition of done

Iteration count, task count, compilation, and a green unit-test subset are not definitions of done. The autonomous implementation loop may claim completion only when all of the following are objectively true:

1. **Complete conformance matrix.** Every normative requirement in this specification is classified `verified` with specific source evidence and an executable test or acceptance command. No requirement remains `partial`, `missing`, `ambiguous`, assumed, or verified only by prose.
2. **Production-path behavior.** All v1 workflows run through the same initialization, event dispatch, rendering, backend, persistence, and shutdown paths as the installed binaries. Test-only assembly or direct callback invocation may supplement but never replace production-path acceptance. Test doubles must preserve external type/signature semantics; a mock that accepts behavior rejected by the real dependency cannot verify conformance.
3. **Complete interaction traversal.** Every enabled control in the §5.7 inventory has passing controller and pointer activation evidence, and every overlay action has passing controller-event evidence. Tests verify semantic outcomes, not merely event consumption, focus movement, pixels, or lack of a crash.
4. **Visual and degraded-state acceptance.** §§4.10, 5.6, and 11.1 pass for normal, empty, loading, unavailable, validation-error, backend-error, and recovery states required by the affected workflow. No required screen or region is blank, clipped, unreachable, or misleadingly enabled.
5. **Regression and quality gates.** The full clean-build, unit, integration, end-to-end, installed-package, and project verification suites pass. There are no unexplained skips, flaky rerun dependencies, weakened assertions, leaked processes/files, compiler warnings introduced by the cycle, or sanitizer/static-analysis defects in changed code where those checks are supported.
6. **Known-defect accounting.** Open bug ledgers and review findings contain no unresolved defect that contradicts a v1 requirement. A defect may be deferred only by an explicit human-approved specification or release decision; silently treating it as out of scope is prohibited.
7. **Independent review.** Read-only correctness, test-quality, security, and documentation reviews find no unresolved blocking issue. Review must challenge whether tests can pass while production behavior remains broken.
8. **Documentation and reproducibility.** README and operational documentation match observed behavior; build, install, acceptance, artifact, and recovery commands work from a clean checkout; final evidence records exact commands and results.
9. **Repository integrity.** The complete active-cycle task ledger remains present, every task is complete with evidence, the canonical specification binding is fresh, and the Git tree is clean on `develop`.

If final verification discovers any gap, the loop must not emit its completion promise. It must preserve existing task history, append a uniquely numbered pending remediation task, add that task as a dependency of the final audit, implement and verify it in a later fresh iteration, and rerun the entire definition of done. If an iteration, runtime, quota, or external session ceiling is reached first, the cycle remains explicitly `active` or `blocked` with a recovery handoff; reaching a ceiling is never success. Human visual acceptance on target hardware remains required before promotion from `develop` to `main` (§11.1).

---

## 12. Out of Scope (v1)

- **Bare DRM/framebuffer support** (no compositor) — RetroPie-class users are CLI-comfortable (Decision 3).
- **Pi Zero / Pi 3 class hardware** (Decision 2).
- **Touch-first or mouse-only product mode** — controller-first, always. Secondary manager pointer activation is nevertheless required by §§5.1 and 5.7 and must cover every manager operation.
- **Replacing InputPlumber's input routing engine** — the GUI is a control surface only (Decision 5).
- **Steam integration or any Steam API dependency.**
- **Quick-action hotkey combos** — only the overlay trigger exists (ticket #4).
- **Bundling InputPlumber** — separate package, separate service (Decision 13).
- **Network input routing** — listed in InputPlumber's README but not implemented upstream.
- **Per-game profile auto-switching** — InputPlumber has no concept of it; deferred.
- **Adding/removing source devices on running composites** — not exposed on DBus and not needed (gap #5).
- **User-assigned controller names/colors** — superseded by positional interaction + auto-assignment (ticket #5).

## 13. Deferred Items (post-v1)

- **Host Mode interface details** — how the host navigates between rows, what editing looks like. The mode and its trigger (R3) are specified; the interior UX is specified during implementation.
- **Theme/skinning details** — default theme appearance and theme format. The `theme` setting key exists in `settings.yaml`; the format is TBD.
- **Launching the manager without a desktop environment** — desktop entry + overlay "open manager" action covers most cases; console-only launch path TBD.
- **Advanced profile mappings** — `chord`, `delayed_chord`, gamepad→mouse mappings. v1 ships simple button remapping; the YAML format already supports these, so profiles using them remain loadable.
- **Per-game profiles** — manual profile switching in v1; auto-switching awaits upstream support.
- **Distro packages** — .deb, .rpm, Nix, AUR, Batocera (§9.5).

---

## Appendix A — Source Traceability

| Spec section | Decisions / tickets | Research artifact |
|---|---|---|
| §2 Architecture | Decisions 4, 5, 6, 13; tickets #4, #7 | `inputplumber-research-summary.md` §6 |
| §3 Requirements | Decisions 1, 2, 3 | — |
| §4 Overlay | Decisions 6, 7, 14; tickets #4, #5, #8 | — |
| §5 Manager | Decisions 10, 11, 14; tickets #2, #8 | — |
| §6 Identification | Decision 8; ticket #5 | `controller-identification-research.md` |
| §7 Config | Decision 12; ticket #3 | `inputplumber-research-summary.md` §4 |
| §8 Icons | Decision 16; ticket #10 | — |
| §9 Packaging | Decision 15; ticket #9 | — |
| §10 DBus | Decisions 4, 9; ticket #6 | `inputplumber-research-summary.md` §2, `wayfinder-ticket6-dbus-api-audit.md` |
| §11 Performance | Decision 6 | `inputplumber-research-summary.md` §5 |
