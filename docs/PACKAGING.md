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
After=inputplumber.service
Requires=inputplumber.service

[Service]
ExecStart=/usr/bin/controller-box --overlay-service
Restart=always

[Install]
WantedBy=default.target
```

The `After=` / `Requires=` directives create a hard dependency on
InputPlumber's system service. If InputPlumber is not running, the overlay
service will not start until it is.

## Flatpak

Flatpak is the primary install method (SPEC §9.1), targeting Steam Deck,
desktop Linux, Bazzite, Nobara, ChimeraOS, and any Flatpak-capable distro.
Published on Flathub.

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

The unit has `Restart=always` and survives reboots.

### InputPlumber dependency

InputPlumber is a documented prerequisite. Install it first from its own
Flatpak or system package. The overlay service unit declares
`After=inputplumber.service` / `Requires=inputplumber.service` so it will
not start until InputPlumber is available.

## Version

The version is embedded at build time from CMake's `project(VERSION ...)`.
Run `controller-box --version` to check the installed version. See Task 43
for version embedding details.

## Post-v1 roadmap

- .deb (amd64 + arm64)
- .rpm
- Nix derivation
- AUR
- Batocera package (.BATOEXEC)
- Eventual upstream Batocera integration