# Profiles

Controller-Box profiles are standard InputPlumber `device_profile_v1` YAML
files. There is no duplicate profile format — Controller-Box generates
conformant YAML that InputPlumber loads natively via `LoadProfilePath` /
`LoadProfileFromYaml`. The same YAML works with
`inputplumber device <id> profile load`.

## Ownership model

InputPlumber owns profile **content**; Controller-Box owns profile
**presentation**. Profiles are written directly to InputPlumber's user profile
directory. Controller-Box-specific metadata (display name, icon override,
display order) lives in optional sidecar files, never inside the profile YAML.

**Design principle:** *Same profile + same controller = same result, every
time.* Profiles map against the virtual device's capabilities, not the
physical controller — InputPlumber's capability maps normalize physical
differences. Profiles are deterministic and portable.

## File layout

```
~/.local/share/inputplumber/profiles/        ← InputPlumber + Controller-Box (read/write)
    default.yaml                              ← built-in default (read-only)
    fighting.yaml                             ← user-created profile
    ...
/usr/share/inputplumber/profiles/             ← system profiles (read-only)
/usr/share/inputplumber/devices/              ← device configs (read-only)
/usr/share/inputplumber/capability_maps/      ← capability maps (read-only)

~/.config/controller-box/profile-metadata/    ← Controller-Box sidecar metadata
    fighting.meta.yaml                        ← optional per-profile metadata
```

Controller-Box reads both the user and system profile directories. System
profiles are read-only; user profiles are read/write. The default profile is
always present, read-only, and always the fallback.

### Profile metadata sidecar

Each profile can have an optional sidecar in
`~/.config/controller-box/profile-metadata/<name>.meta.yaml`:

```yaml
display_name: "Fighting Profile"
icon: "cc-ps5"                    # built-in icon name
# icon: "/path/to/custom.png"    # or absolute path to custom PNG
display_order: 1
description: "Tournament fighting setup"
```

The icon override supports either a built-in icon name (looked up in the icon
cache) or an absolute path to a custom PNG (validated: no `..` traversal,
`realpath()` must resolve within a safe directory).

## DeviceProfile YAML schema

```yaml
version: 1
kind: DeviceProfile
name: "Start Button to Escape Key"
description: "Maps a gamepad's start button to the Escape keyboard key"
mapping:
  - name: "Menu"
    source_event:
      gamepad:
        button: Start
    target_events:
      - keyboard: KeyEsc
```

### Top-level fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `version` | int | Yes | Must be `1` |
| `kind` | string | Yes | Must be `"DeviceProfile"` |
| `name` | string | Yes | Profile name (double-quoted on emit) |
| `description` | string | No | Profile description (double-quoted on emit) |
| `mapping` | sequence | Yes | List of mapping entries (max 128) |

### Mapping entry

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Binding name (e.g., "Menu", "Jump") |
| `source_event` | mapping | Device class → properties (e.g., `gamepad: { button: Start }`) |
| `target_events` | sequence | List of device class → value mappings (max 16 per entry) |

### Device classes

Profiles can map between gamepad, keyboard, mouse, and touch events:

| Device class | Example source | Example target |
|-------------|---------------|---------------|
| `gamepad` | `button: Start`, `button: A`, `axis: LeftStickX` | — |
| `keyboard` | — | `KeyEsc`, `KeyA`, `KeyUp` |
| `mouse` | — | `ButtonLeft`, `MotionX` |
| `touchscreen` | — | — |

Source event properties are key-value pairs (max 8 per event). Target events
are device class → scalar value.

### Advanced mappings (not exposed in v1 editor)

The InputPlumber `device_profile_v1` schema supports advanced mapping types:
`chord`, `delayed_chord`, and gamepad→mouse. These are **valid YAML** and
profiles using them remain **loadable** by Controller-Box — the parser accepts
them (storing the device class with an empty value via a skip-depth mode).
However, the v1 profile editor does not expose these mapping types. They are
deferred to post-v1 (see SPEC §13).

## Profile editor

The profile editor has two modes sharing one always-visible, always-synchronized
controller diagram (left panel).

### Mode 1: Binding list

- Up/Down scrolls through the binding list.
- The highlighted row lights up the corresponding button on the diagram.
- Press **A** to edit a binding — choose from a list or capture a physical press.

### Mode 2: Sequential binding

- The editor prompts for each button in order.
- The diagram lights up the current button being prompted.
- Press the physical button to capture it → auto-advance to the next.
- Press **B** to skip the current binding.
- Press **Start** to cancel.
- A progress bar shows completion.

## NES minimum validation

A valid profile must bind at least:

| Required binding |
|-----------------|
| A |
| B |
| D-Pad Up |
| D-Pad Down |
| D-Pad Left |
| D-Pad Right |

All other bindings are **optional**: triggers, sticks, shoulders, Start, Select.

If any required binding is missing, the manager shows an error listing which
bindings are missing, and the profile cannot be saved. This makes "broken
profile" an impossible state — the minimum set ensures the profile can
navigate any menu and play any NES-class game.

## Default profile

The **Default** profile is:

- **Built-in**: shipped with Controller-Box, not user-created
- **Read-only**: cannot be edited or deleted
- **Always present**: guaranteed to exist
- **Always the fallback**: if a controller has no assigned profile, it uses
  Default

## New profile flow

1. From the Profiles tab, choose **Create**.
2. Pick a starting point:
   - **Default copy**: clone the built-in Default profile
   - **Empty**: start from a blank template
   - **Clone existing**: copy an existing user profile
3. The editor opens with the chosen starting point.
4. Edit bindings using either mode.
5. Validation runs on save — NES minimum must pass.
6. The profile is written to `~/.local/share/inputplumber/profiles/<name>.yaml`.
7. A sidecar metadata file is optionally created in
   `~/.config/controller-box/profile-metadata/<name>.meta.yaml`.

Profile names must match `^[a-zA-Z0-9_-]+$` (alphanumeric, underscore, hyphen).

## Per-controller scope

Profiles are **per-controller, not per-slot**. A controller's assigned profile
follows it as it moves between columns in the overlay. Only Up/Down changes
the profile; Left/Right changes only the slot position.

The assignment is stored in `assignments.yaml` keyed by the controller's
identity ID (Bluetooth MAC, USB serial, USB port path, or connection order —
strongest available identity wins).

When a controller connects, Controller-Box auto-assigns it to its preferred
slot and loads its preferred profile based on the saved assignment. If no
assignment exists, the controller goes to Unassigned with the Default profile.

## Profile save/load

Controller-Box profiles are saved atomically (mkstemp + rename, mode 0644 —
profiles are InputPlumber-format and readable by other tools). The YAML is
generated via `open_memstream` and conforms to the `device_profile_v1` schema.

Loading: `cbx_profile_load(path)` or `cbx_profile_parse(yaml, len)`. The
parser validates `version == 1` and `kind == "DeviceProfile"`.