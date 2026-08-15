# Packaging

Controller-Box can be installed two ways: Flatpak (primary) or tarball
(fallback for any distro). Both produce the same install layout.

## Install layout

```
/usr/bin/controller-box                                        ← one binary, two modes
/usr/share/controller-box/controller-box.service               ← systemd user service (reference template)
/usr/share/controller-box/icons/svg/                            ← default controller icons
/usr/share/controller-box/controller-icons.yaml                 ← icon mapping table
/usr/share/applications/controller-box-manager.desktop          ← desktop entry (launches manager)
```

The systemd user service is also installed at runtime by the manager to
`~/.config/systemd/user/controller-box.service` on first run. The installed
template at `/usr/share/controller-box/controller-box.service` is a reference
copy; the manager generates the unit content dynamically (adjusting the
ExecStart path for Flatpak if needed).

### Desktop entry

```ini
[Desktop Entry]
Type=Application
Name=Controller-Box
Comment=Configure virtual controllers and profiles for InputPlumber
Exec=/usr/bin/controller-box --manager
Icon=controller-box
Categories=Game;Settings;
Terminal=false
```

Under Flatpak, the manifest renames this to
`org.shadowblip.ControllerBox.desktop` and fixes the `Exec` path to
`controller-box` (the Flatpak runtime resolves the binary via the `command`
key in the manifest).

## Tarball install (v1 fallback)

### Build from source

```bash
# Install build dependencies (example for Debian/Ubuntu):
sudo apt install build-essential cmake pkg-config \
    libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev \
    libsystemd-dev libyaml-dev

# Configure and build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run tests (optional)
cd build && ctest --output-on-failure && cd ..

# Install to a staging directory for testing
cmake --install build --prefix /tmp/test-install

# Verify install layout
tests/test_packaging_install.sh /tmp/test-install

# Install system-wide (default prefix: /usr)
sudo cmake --install build
```

### Dependencies

| Dependency | Debian/Ubuntu package | Fedora |
|---|---|---|
| SDL2 | libsdl2-dev | SDL2-devel |
| SDL2_ttf | libsdl2-ttf-dev | SDL2_ttf-devel |
| SDL2_image | libsdl2-image-dev | SDL2_image-devel |
| systemd (sd-bus) | libsystemd-dev | systemd-devel |
| libyaml | libyaml-dev | libyaml-devel |
| cmocka (tests only) | libcmocka-dev | libcmocka-devel |
| nanosvg | vendored in source tree | vendored in source tree |

### Post-install steps

1. **Install InputPlumber** — Controller-Box requires InputPlumber running as a
   system service. Install it first from its own package.
2. **Enable the overlay service** — Open the manager
   (`controller-box --manager`), go to Settings, and install the systemd user
   service. Alternatively, manually copy the service template and enable:
   ```bash
   cp /usr/share/controller-box/controller-box.service ~/.config/systemd/user/
   systemctl --user daemon-reload
   systemctl --user enable --now controller-box
   ```
3. **Add your user to the inputplumber group** (if using polkit):
   ```bash
   sudo usermod -aG inputplumber $USER
   # Log out and back in for group changes to take effect.
   ```

### Service unit

The systemd user service unit declares (per SPEC §2.4):

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

The user unit follows the graphical user session and deliberately has no
cross-manager dependency on InputPlumber's system service. Runtime DBus
readiness and owner-change recovery handle either startup order.

## Flatpak

Flatpak is the primary install method (SPEC §9.1), targeting Steam Deck,
desktop Linux, Bazzite, Nobara, ChimeraOS, and any Flatpak-capable distro.

**Experimental status:** The Flatpak manifest is provided for development and
testing but is **not yet published on Flathub**. It has not undergone a
verified clean build against a published Flathub runtime, nor has it been
submitted for Flathub review. Do not advertise a `flatpak install flathub`
command until the publication marker in the manifest is set to true.

### Manifest

The Flatpak manifest is at `packaging/org.shadowblip.ControllerBox.yaml`.
It builds SDL2, SDL2_ttf, SDL2_image, and libyaml from source as Flatpak
build modules, then builds Controller-Box with CMake. nanosvg is vendored
in the source tree (`third_party/nanosvg/`) and compiled by CMake directly.
libsystemd (sd-bus) is provided by the freedesktop SDK.

### Building

```bash
# Install flatpak-builder and the freedesktop SDK
flatpak install flathub org.freedesktop.Sdk//24.08
flatpak install flathub org.freedesktop.Platform//24.08

# Build and install locally
flatpak-builder --user --install --force build-dir \
    packaging/org.shadowblip.ControllerBox.yaml

# Or just verify dependencies (no build)
flatpak-builder --show-deps packaging/org.shadowblip.ControllerBox.yaml
```

### Permissions

The Flatpak manifest requests only the minimum permissions needed:

| Permission | Rationale |
|---|---|
| `--socket=wayland` | SDL2 Wayland backend (primary on Steam Deck) |
| `--socket=fallback-x11` | SDL2 X11 fallback for non-Wayland desktops |
| `--device=dri` | GPU acceleration for SDL2 rendering |
| `--system-talk-name=org.shadowblip.InputPlumber` | Talk to InputPlumber on system DBus (§10.1) |
| `--talk-name=org.freedesktop.Flatpak` | `flatpak-spawn --host systemctl --user` for service install |
| `--filesystem=~/.local/share/inputplumber/profiles` | Read/write InputPlumber user profiles (gap #4) |
| `--filesystem=/usr/share/inputplumber:ro` | Read system profiles/devices/capability maps (gap #4) |
| `--filesystem=~/.config/controller-box` | Read/write Controller-Box settings and assignments (§3.1) |
| `--filesystem=~/.config/systemd/user` | Install systemd user service on first run (§9.1) |

No `--filesystem=host`, `--filesystem=home`, or `--device=all` permissions
are requested. Each permission is documented with a rationale comment in
the manifest.

### Systemd service under Flatpak

Flatpak cannot ship systemd units to the host. The manager installs the
service on first run:

1. User installs the Flatpak and opens the manager.
2. Manager prompts: "Enable overlay service?"
3. Manager writes `~/.config/systemd/user/controller-box.service` with
   `ExecStart=flatpak run org.shadowblip.ControllerBox --overlay-service`.
4. Manager calls `flatpak-spawn --host systemctl --user enable --now
   controller-box`.

The unit uses `Restart=on-failure` with a two-second backoff and follows the graphical session.

### InputPlumber dependency

InputPlumber is a documented runtime prerequisite. Install it from its own
Flatpak or system package. Controller-Box can start first in degraded mode
and reconciles state when the InputPlumber DBus owner becomes ready.

## Version

The version is embedded at build time from CMake's `project(VERSION ...)`
and injected into `config.h` via `@PROJECT_VERSION@`. The version is
available at compile time as `CONTROLLER_BOX_VERSION` and at runtime
via `controller-box --version`.

```bash
$ controller-box --version
controller-box 0.1.0
```

The `--version` flag works in both modes (`--overlay-service` and
`--manager`) — it is processed before mode dispatch and exits immediately.

### Release tarball

To produce a distributable tarball:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
DESTDIR=/tmp/staging cmake --install build
# The staging area contains the full install layout under /tmp/staging/usr/
tar -czf controller-box-0.1.0.tar.gz -C /tmp/staging usr/
```

The packaging integration test (`tests/test_packaging.sh`) verifies the
full pipeline: build → install → file layout → binary execution → version
match.

## Post-v1 roadmap

- .deb (amd64 + arm64)
- .rpm
- Nix derivation
- AUR
- Batocera package (.BATOEXEC)
- Eventual upstream Batocera integration