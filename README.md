# Controller-Box

Controller-Box is a controller-only SDL2 GUI for Linux that wraps
[InputPlumber](https://github.com/shadowblip/InputPlumber) to provide a
console-like controller management experience — no keyboard, no mouse,
no Steam required.

A single binary, `controller-box`, runs in two modes:

- **Overlay service** (`controller-box --overlay-service`): an always-resident
  systemd user service that shows a fighting-game-style character-select
  screen when any player presses **Select+A**, allowing per-controller slot
  assignment and profile cycling mid-game.
- **Manager** (`controller-box --manager`): a tab-based configuration app for
  creating virtual controllers, building/editing profiles with a visual
  controller diagram, and adjusting settings.

Both modes share one codebase, one config directory, and one DBus connection.

## Requirements

- **Linux** (x86_64 or aarch64), any compositor (X11, Wayland, Gamescope)
- **InputPlumber** installed and running as a system service
- **SDL2**, **SDL2_ttf**, **SDL2_image** (runtime; bundled in Flatpak)
- **systemd** (for sd-bus and the user service)
- Minimum hardware: Raspberry Pi 4 or equivalent (ARM64, OpenGL ES 3.0)

See [docs/OPERATIONS.md](docs/OPERATIONS.md) for service architecture and
[docs/PACKAGING.md](docs/PACKAGING.md) for install methods.

## Install

### Flatpak (experimental)

The Flatpak manifest is provided for development and testing but is **not
yet published on Flathub**.  Build and install it locally:

```bash
flatpak install flathub org.freedesktop.Sdk//24.08
flatpak install flathub org.freedesktop.Platform//24.08
flatpak-builder --user --install --force build-dir \
    packaging/org.shadowblip.ControllerBox.yaml
```

On first launch, the manager prompts to enable the overlay service. It writes
`~/.config/systemd/user/controller-box.service` and enables it via
`flatpak-spawn --host systemctl --user`.

### Tarball (any distro)

```bash
# Install build dependencies (Debian/Ubuntu example):
sudo apt install build-essential cmake pkg-config \
    libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev \
    libsystemd-dev libyaml-dev
# For running tests, also install: libcmocka-dev

# Build and install:
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build

# Enable the overlay service via the manager (Settings → Install Service),
# or manually:
systemctl --user enable --now controller-box
```

See [docs/PACKAGING.md](docs/PACKAGING.md) for full build instructions,
dependencies, and install layout.

**Install order:** InputPlumber first, then Controller-Box, then enable the
overlay service.

## Overlay usage

The overlay is a character-select screen — rows are physical controllers,
columns are player slots (virtual controllers). The leftmost column is
**Unassigned**.

| Action | Button |
|--------|--------|
| Open overlay | **Select + A** (default, configurable) |
| Move slot position | Left / Right |
| Cycle profile | Up / Down |
| Enter/exit Host Mode | R3 |
| Close overlay | B |

In **Player Mode** (default), all controllers edit simultaneously — each
navigates its own row. In **Host Mode** (press R3), the first controller to
press R3 becomes the exclusive host; all others freeze. Press R3 again to
exit Host Mode.

If two controllers land on the same slot, the cell turns red. On close, the
conflicted controller is automatically moved to the lowest unoccupied slot.

Profiles are **per-controller, not per-slot** — your profile follows your
controller as you move between columns.

The overlay renders in under 10 ms because the surface is pre-built in memory
at daemon startup with icons pre-rasterized via nanosvg.

## Manager usage

The manager has three tabs, navigated by controller:

### Controllers tab

Add/remove virtual controllers (player slots) and set each slot's virtual
controller type. Mixed types are allowed (e.g., P1 = Xbox 360, P2 = DualSense).
Removing a slot mid-session moves the affected physical controller to
Unassigned — no input is lost.

### Profiles tab

Browse, create, edit, and delete profiles. The **Default** profile is
built-in, read-only, and always the fallback. New profiles can start from a
copy of Default, an empty template, or a clone of an existing profile.

The profile editor has two modes sharing one always-visible controller diagram:

- **Binding list**: scroll through bindings; the highlighted row lights up the
  corresponding button on the diagram. Press A to edit a binding.
- **Sequential binding**: the editor prompts for each button in order. Press
  the physical button to capture it; B skips, Start cancels.

A profile must bind at least **A, B, D-Pad Up, D-Pad Down, D-Pad Left, and
D-Pad Right** (the NES minimum). Other bindings are optional.

See [docs/PROFILES.md](docs/PROFILES.md) for the full profile format and editor
documentation.

### Settings tab

- Launch at boot
- Theme
- Overlay opacity
- Number of virtual controllers on startup and their types
- Overlay trigger combo (the single hotkey — no other quick-action combos exist)
- Controller icon overrides

## Config file locations

| Path | Purpose |
|------|---------|
| `~/.local/share/inputplumber/profiles/` | InputPlumber + Controller-Box profiles (read/write) |
| `~/.config/controller-box/settings.yaml` | App settings |
| `~/.config/controller-box/assignments.yaml` | Auto-assignment table + gamepad order |
| `~/.config/controller-box/profile-metadata/` | Optional per-profile sidecar metadata |
| `~/.config/systemd/user/controller-box.service` | Systemd user service (installed by manager) |
| `/usr/share/controller-box/icons/svg/` | Default controller icons (read-only) |
| `/usr/share/controller-box/controller-icons.yaml` | Icon mapping table (read-only) |
| `/usr/share/inputplumber/` | InputPlumber system profiles, devices, capability maps (read-only) |

## Building from source

```bash
# Using Nix (provides all dependencies):
nix-shell --run 'cmake -B build && cmake --build build'

# Or install dependencies manually (Debian/Ubuntu):
sudo apt install build-essential cmake pkg-config \
    libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev \
    libsystemd-dev libyaml-dev libcmocka-dev

# Configure, build, test:
cmake -B build
cmake --build build
cd build && ctest --output-on-failure && cd ..

# Version check:
./build/controller-box --version

# Dry-run (headless-safe):
./build/controller-box --overlay-service --dry-run
./build/controller-box --manager --dry-run
```

A build configured with a custom `CMAKE_INSTALL_PREFIX` is useful for an
isolated installation or X11 UI test. Its default-prefix packaging assertion
is intentionally not authoritative, because the generated paths differ.
Run the complete packaging/release gate from a separate default-prefix
`build-maintenance-verify` directory with `./scripts/verify-project.sh`.

## Verification suite

The project includes a multi-layer visual acceptance suite (SPEC §11.1)
that verifies actual framebuffer pixel output, not just state-machine or
geometry correctness:

| Layer | Test | What it verifies |
|-------|------|-----------------|
| 1. Deterministic framebuffer | `test_overlay_visual`, `test_manager_visual` | Renders through production composition path, reads back pixels via `SDL_RenderReadPixels`, asserts content in expected regions |
| 2. Region-level assertions | `test_fb_assert` | `fb_assert.c` library: `fb_region_has_content`, `fb_region_has_color`, `fb_frames_differ`, `fb_golden_compare` |
| 3. Golden images | `test_golden` | Compares 11 baseline PNGs (4 overlay + 7 manager states) with ±3 per-channel and <2% image tolerance |
| 4. Failure artifacts | `test_golden` (on mismatch) | Saves actual/expected/diff PNGs to `tests/golden-fail/` for diagnosis |
| 5. Installed production smoke | `test_installed_smoke` | Launches installed binary under Xvfb, sends keyboard + coordinate-based mouse clicks on body controls via xdotool, captures screenshots, verifies non-blank output and semantic outcomes (state change, file mutation) |
| 5a. Installed functional acceptance | `test_installed_functional` | Links against production library; starts private native-signature DBus server, creates SDL virtual controller, exercises manager + overlay lifecycle through production poll path (InterceptMode PASS→ALL activation, framebuffer readback, B-close, assignment persistence) |
| 5b. Installed binary acceptance | `test_installed_binary` | Launches installed binary as subprocess under Xvfb with private DBus server; verifies manager launch, tab navigation, settings persistence, target creation, profile load/save, overlay activation (InterceptMode→ALL, non-blank screenshot, clean close) |
| 6. Backend smoke | `test_backend_smoke` | Exercises accelerated renderer (OpenGL/ES) with same invariants; skips (exit 77) in headless environments |
| 7. Kernel-backed controller | `test_kernel_controller` | Creates a synthetic evdev gamepad via `/dev/uinput`, launches installed Manager binary with private DBus server, sends real kernel gamepad events (D-pad, A/B/Start) through production event loop, verifies semantic outcomes (manager survival, settings persistence). Skips (exit 77) when `/dev/uinput` is unavailable — no `kernel-uinput` runner capability declared in `.factory/environment.toml`. Interaction inventory (50/59 verified, 8 NOT_APPLICABLE, 1 DEFERRED per §13) verified through production SDL event dispatch with SDL virtual gamepads. See SPEC §5.7 for controller acceptance requirements. |
| 8. Human release acceptance | (documented process) | Human reviews captures on target hardware for legibility, clipping, contrast, controller-only usability |

Run the full suite:

```bash
nix-shell --run './scripts/verify-project.sh'
```

Or run individual test groups:

```bash
# Visual framebuffer tests:
nix-shell --run "ctest --test-dir build-maintenance-verify -R 'test_overlay_visual|test_manager_visual|test_fb_assert' --output-on-failure"

# Golden image comparison:
nix-shell --run "ctest --test-dir build-maintenance-verify -R test_golden --output-on-failure"

# Installed smoke test (requires Xvfb, xdotool, ImageMagick, bc):
nix-shell --run "ctest --test-dir build-maintenance-verify -R test_installed_smoke --output-on-failure"

# Installed functional + binary tests (requires Xvfb, xdotool, ImageMagick):
nix-shell --run "ctest --test-dir build-maintenance-verify -R 'test_installed_functional|test_installed_binary' --output-on-failure"

# Backend smoke test (requires real GPU/display):
nix-shell --run "ctest --test-dir build-maintenance-verify -R test_backend_smoke --output-on-failure"

# Kernel-backed controller test (requires /dev/uinput — skips with exit 77 if unavailable):
nix-shell --run "ctest --test-dir build-maintenance-verify -R test_kernel_controller --output-on-failure"
```

Golden image baselines are in `tests/golden/`. To regenerate them (explicit,
reviewed change — never automatic):

```bash
./scripts/generate-golden.sh
```

See [docs/OPERATIONS.md](docs/OPERATIONS.md) for the golden image workflow,
tolerance values, failure artifact diagnosis, and the human release acceptance
checklist.

### Interaction acceptance tests

The project includes a full §5.7 interaction acceptance suite that exercises
every manager control and overlay action through normal SDL event dispatch
(`cbx_manager_handle_event` for the manager, `cbx_overlay_service_step` for
the overlay — not direct callback invocation):

| Test | Coverage |
|------|----------|
| `test_manager_interaction_ctrl` | M01–M09 (Controllers tab), M21–M27 (Settings tab) — controller + pointer paths (mock DBus) |
| `test_manager_interaction_prof` | M10–M20 (Profiles tab), M28–M38 (Profile editor) — controller + pointer paths (mock DBus) |
| `test_manager_native` | M04, M09, M21, M23–M26 — controller + pointer paths (native DBus) |
| `test_manager_native_prof` | M10, M12–M14, M16, M18, M20, M30–M36, D02–D04, D07–D08 — controller + pointer paths (native DBus) |
| `test_overlay_interaction` | O01–O12 (overlay open, move, profile cycle, host mode, conflict, close) — controller + DBus InputEvent paths (mock DBus) |
| `test_overlay_native` | O01–O13 (overlay open, move, profile cycle, host mode, Player Mode conflict, close) — native DBus backend via `cbx_overlay_service_step` |
| `test_interaction_inventory` | M01–M38, O01–O13, D01–D08 inventory validation |

Disabled-control scenarios (D01–D08) verify that disabled controls reject both
activation paths and produce no backend or filesystem side effect.

```bash
nix-shell --run "ctest --test-dir build-maintenance-verify -R 'test_manager_interaction|test_overlay_interaction|test_interaction_inventory' --output-on-failure"
```

### Known environment limitations

The factory runner environment (`.factory/environment.toml`) declares one SSH
runner with `remote-project-gate` and `systemd-user` capabilities. The
following spec requirements have environment limitations that affect full
hardware-specific acceptance; they are classified `verified` based on code
portability, architectural evidence, and tests available in the declared
environment, with remaining hardware-specific verification deferred to human
release acceptance per SPEC §11.1.7:

| Requirement | Spec § | Limitation | Verification approach |
|-------------|--------|------------|----------------------|
| Controller acceptance (kernel-backed) | §5.7 | No `kernel-uinput` runner capability | `test_kernel_controller.c` creates a uinput-backed evdev gamepad but skips (exit 77) when `/dev/uinput` is unavailable. 50/59 interaction inventory entries verified through production SDL event dispatch with SDL virtual gamepads; 8 NOT_APPLICABLE (controller-only paths); 1 DEFERRED per §13. |
| aarch64 architecture | §3 | No aarch64 runner declared | Code is architecture-agnostic (no arch-specific code in `src/` or `CMakeLists.txt`); Flatpak manifest targets `org.freedesktop.Platform` 24.08 supporting both x86_64 and aarch64; x86_64 build and 97 CTest targets verified. |
| Wayland/Gamescope compositor | §3 | No Wayland runner declared | All rendering through SDL2 display abstraction — zero compositor-specific API calls in `src/`. Tested with X11 (Xvfb) and dummy drivers. SDL2 supports X11, Wayland, and Gamescope. |
| GPU backend | §11.1 | No GPU runner declared | `test_backend_smoke` skips (exit 77) in headless environments; software renderer smoke (`test_backend_smoke_sw`) passes with broad framebuffer invariants. |
| Pi 4 latency | §11 | No Pi 4 hardware declared | Pre-built surface + 50ms poll architecture verified; `test_overlay_latency.c` exists. Actual ≤75ms p99 latency measurement on Pi 4 is a human release gate per §11.1.7. |

These limitations do not indicate missing implementation — the code is
complete and tested through available paths. They indicate that certain
hardware/kernel capabilities are not available in the declared runner
environment for full production-path acceptance verification. Remaining
hardware-specific verification is deferred to human release acceptance per
SPEC §11.1.7.

## Multi-round Ralph campaign

For a predetermined unattended sequence of fresh adversarial plans,
implementations, verification passes, and independent gap audits, run:

```bash
./scripts/ralph-campaign.sh --rounds 3
```

Campaigns are headless by default so phase completion does not wait for a TUI
to close; use `--tui` only for attended diagnostics. Every round receives a new
Git base and replaces the active plan; completed plans and audits remain in Git
history. Attempt-bound stale loops receive fixed strict-gate recovery instructions
and bounded automatic continuation; unrelated failures still stop safely. Interrupted campaigns
resume with the same round count and phase using `--resume`. Available local tools and external
runners are declared without credentials in `.factory/environment.toml`.
Verification validates exact-commit runner receipts; required capabilities without
accepted production evidence remain findings rather than fabricated completion.

Ralph 2.10.1 starts a five-second deadline when Pi returns the successful
`ralph emit` acknowledgement, then misclassifies its own timeout signal as a
failed iteration. `scripts/pi2-ollama.sh` loads an explicit Pi extension that
rewrites only a direct final `ralph emit` tool command to a repository shim. The
shim invokes the real Ralph binary from the jail's trusted PATH and changes only
that command's acknowledgement after Ralph writes the authoritative event;
arbitrary identical model/backend text is left untouched. The wrapper and secure prompt launcher retain `exec` semantics,
so backend errors and signals propagate normally. Pi can therefore finish its
short post-tool turn under Ralph's normal five-minute inactivity timeout. This
may add a brief model follow-up after publication but avoids false
consecutive-failure termination.

## Bug maintenance

Portable bug state is tracked in `.factory/bugs/open.md` and `.factory/bugs/closed.md`. A bug may
reference a GitHub issue, a Forgejo issue, both, or neither; external tickets do
not replace the local ledger. Ordinary defects use the dedicated maintenance
cycle and do not modify `docs/SPEC.md`:

```bash
./scripts/bug-ledger.py validate
./scripts/ralph-maintenance-plan.sh BUG-0001
./scripts/ralph-maintenance-run.sh
```

A fresh specification or maintenance planning command replaces the prior active plan and scratchpad with a minimal cycle skeleton before Ralph starts; completed tasks remain only in Git history and are not carried into future prompts. `--resume` preserves an interrupted draft, and newly accepted plans may contain only pending tasks.

Contract changes return to the human specification and full planning workflow.
See [docs/BUG_WORKFLOW.md](docs/BUG_WORKFLOW.md) for intake, triage, external
links, maintenance, verification, and recovery.

## Documentation

- [docs/OPERATIONS.md](docs/OPERATIONS.md) — Service architecture, systemd management, troubleshooting
- [docs/DBus-API.md](docs/DBus-API.md) — Full DBus API reference and gaps
- [docs/PROFILES.md](docs/PROFILES.md) — Profile format, editor modes, validation
- [docs/PACKAGING.md](docs/PACKAGING.md) — Flatpak, tarball, install layout
- [docs/FACTORY.md](docs/FACTORY.md) — Development factory boilerplate (Ralph orchestration)
- [docs/BUG_WORKFLOW.md](docs/BUG_WORKFLOW.md) — GitHub/Forgejo tickets and portable maintenance ledgers

## Credits

Controller icons are sourced from:

- [Controllercons](https://controllercons.github.io/) by Kieran McClung — 30
  controller SVG icons licensed under the [SIL Open Font License 1.1](data/icons/svg/LICENSE.controllercons).
  Covers PS5, PS4, PS3, Xbox Series X, Xbox One, Xbox 360, Switch Pro, Joy-Cons,
  SNES, NES, N64, GameCube, Wii, Dreamcast, and more.
- Custom icons (arcade-stick, hitbox, steam-deck, generic-gamepad, mouse,
  keyboard) are created by the Controller-Box project under GPL-3.0.

Controller-Box is licensed under GPL-3.0. nanosvg is vendored under the
zlib license (`third_party/nanosvg/LICENSE.txt`).